#pragma once

// A planet as a signed distance field: a sphere with terrain cut into it.
//
// The field is `|p| - R - relief(p)`, so "down" is toward the centre from
// everywhere and there is no edge to fall off. That one change of sign turns
// every heightmap intuition inside out: a chunk's *distance from the planet's
// centre* decides whether it contains any surface at all, and the streaming
// below is built around that -- of the 17,000 chunks a 300 m planet spans,
// fewer than a tenth hold any ground, and the rest are solid rock or empty sky
// and cost one float each.
//
// **Only the shell is ever filled.** A chunk whose distance from the centre
// misses the band `R +/- (amplitude + chunk radius)` cannot contain a crossing,
// which is a test on one number and skips most of the planet. Without it, a
// 300 m planet at 1.5 m voxels is 17,576 chunks of 4,096 samples -- 72 million
// density evaluations to generate a world you can see 200 m of.
//
// Relief is 3D value noise sampled on the *world* position rather than on a
// latitude and longitude, because a sphere has no seam only if nothing in the
// generator knows where the poles are. Sampling by direction and angle gives
// terrain that pinches at both ends, which looks exactly like the mistake it is.

#include <Egss.h>

#include <unordered_map>
#include <unordered_set>
#include "ChunkCache.h"

#include <queue>
#include <algorithm>

class VoxelPlanet
{
public:
	struct Settings
	{
		float Radius = 300.0f;        // metres to the mean surface
		float VoxelSize = 1.5f;
		float Amplitude = 26.0f;      // metres of relief, peak to trough

		// **The size of the biggest landform, in metres.** Not a frequency:
		// a frequency has to be re-tuned every time the radius or the voxel
		// size changes, and the first version -- noise sampled at
		// `direction * 90` with five octaves -- put its finest octave at 1.5 m
		// on a 1.5 m lattice. That is noise per voxel, and it produced a planet
		// of spikes rather than of hills.
		//
		// With the feature size in metres the octaves land where you can say
		// where they land: 70 m down to about 9 m at four octaves, all of it
		// comfortably coarser than a voxel.
		float FeatureSize = 70.0f;
		int Octaves = 4;
		unsigned int Seed = 1;

		// **Local roughness, which is not the same spectrum as the planet.**
		//
		// The relief above is a 1/f fractal anchored at `FeatureSize`, and on a
		// body the size of a real planet that leaves nothing underfoot: nine
		// kilometres of relief spread over a 732 km base means the octave at a
		// 45 m wavelength carries half a metre and the one at 3 m carries four
		// centimetres. That is *correct* -- a real planet is smooth at the
		// scale of a stride -- but a demo you walk around in wants ground with
		// shape to it, and adding octaves to the planetary spectrum cannot
		// give it any without making the mountains absurd.
		//
		// So local roughness is its own layer with its own amplitude, three
		// octaves from `RoughnessSize` down. Zero is the old behaviour.
		//
		// It also stops where the arithmetic does. The noise is sampled at
		// `direction * Radius / wavelength`, so a metre-scale wavelength on a
		// 6,371 km planet asks for coordinates near 10^6, where a float's
		// spacing is a sixteenth of a noise cell and the field comes out
		// quantised. Keeping the finest octave above about 25 m keeps the
		// sampling comfortably inside what a float can say.
		float Roughness = 0.0f;        // metres, peak to trough
		float RoughnessSize = 0.0f;    // metres, the coarsest of its octaves

		// **Ridges make mountains; they do not make continents.** The relief
		// is `1 - |noise|`, which puts high ground along the lines where the
		// noise crosses zero -- excellent ridgelines, and hopeless coastlines,
		// because cutting a network of ridges at a sea level leaves land as
		// filaments rather than as landmasses. The first ocean planet looked
		// like cracked glaze.
		//
		// So a share of the relief comes from plain, un-ridged, very
		// low-frequency noise instead: two octaves at a couple of cycles
		// around the planet, which is a handful of broad rises and basins.
		// Mixed rather than added, so the total stays inside `Amplitude` --
		// the chunk lattice and the shell test are both sized from that, and
		// terrain outside it is terrain in chunks nobody fills.
		//
		// Zero is exactly the old behaviour, which is what every other body
		// keeps.
		float ContinentShare = 0.0f;
		float ContinentSize = 0.0f;    // metres; the rises, not the ridges

		// **Landscape: the relief you can actually stand in.**
		//
		// The planetary spectrum above is a 1/f fractal anchored at
		// `FeatureSize`, and 1/f is the problem. Earth's 625 m of relief
		// spread from a 28.75 km wavelength downward puts, at the 400 m you
		// can see from head height, 625 * 400/28750 = **8.7 m** of ground --
		// and measured over two dozen sites it is 8.5. That is a lawn. It is
		// also *correct* for a fractal of that shape, which is why adding
		// octaves to it cannot fix anything: an octave fine enough to be seen
		// is an octave whose amplitude is a centimetre.
		//
		// A real landscape is not self-similar. It has a scale -- a few
		// kilometres between ridgelines, a few hundred metres of rise -- and
		// that scale is set by erosion and uplift, not by the shape of the
		// planet. So it gets its own layer, added rather than mixed, with its
		// own amplitude and its own anchor.
		//
		// **And it is not everywhere.** A planet with mountains uniformly
		// distributed over it is as wrong as one with none: what makes a range
		// read as a range is the plain beside it. `UpliftSize` is how far
		// apart mountain country is, and `LandscapeFloor` is the share of the
		// amplitude the quiet ground keeps -- which is what gives a plain
		// gentle rolls instead of a table.
		//
		// Measured over the same two dozen sites, with the values Earth uses:
		// 49 m of relief inside 400 m on average, 133 m in the ranges, 18 m
		// out on the flats, and 340 m inside 1.6 km where the mountains are.
		float Landscape = 0.0f;        // metres, peak to trough at full uplift
		float LandscapeSize = 0.0f;    // metres, the coarsest of its octaves
		int LandscapeOctaves = 6;
		float UpliftSize = 0.0f;       // metres between mountain country
		float LandscapeFloor = 0.18f;  // what the quiet ground keeps

		// --- Water -----------------------------------------------------------
		//
		// **Sea level is asked for as a coverage, not as a radius.** A radius
		// is meaningless without knowing what the terrain does around it: the
		// relief here is `1 - |noise|`, which is not symmetric about its mean,
		// so the same offset floods quite different fractions of two planets.
		// Earth is 29.2% land, that number is the input, and `Create` bisects
		// for the radius that produces it -- then measures what it actually
		// got, over a different set of directions, and says so.
		bool HasOcean = false;
		float LandFraction = 0.292f;
		float OceanRadius = 0.0f;      // computed, not set

		// Whether the surface is coloured by biome -- sea, sand, forest,
		// tundra, rock, ice -- or by the plain altitude ramp every other body
		// uses.
		bool Vegetated = false;

		glm::vec3 LowColour = { 0.42f, 0.36f, 0.30f };
		glm::vec3 HighColour = { 0.72f, 0.70f, 0.66f };

		// Trees. Zero means none, which is every body but the one.
		int PlantsPerChunk = 0;

		glm::vec3 Shallow = { 0.10f, 0.42f, 0.55f };
		glm::vec3 Deep = { 0.02f, 0.09f, 0.24f };
		glm::vec3 Sand = { 0.78f, 0.72f, 0.52f };
		glm::vec3 Tropical = { 0.16f, 0.42f, 0.15f };
		glm::vec3 Temperate = { 0.25f, 0.41f, 0.22f };
		glm::vec3 Tundra = { 0.46f, 0.42f, 0.32f };
		glm::vec3 Rock = { 0.42f, 0.38f, 0.34f };
		glm::vec3 Snow = { 0.92f, 0.94f, 0.96f };

		// The dry half of the Whittaker square. Warm and dry is sand and
		// scrub; cool and dry is the bleached grass of a steppe.
		glm::vec3 Desert = { 0.76f, 0.66f, 0.44f };
		glm::vec3 Steppe = { 0.55f, 0.54f, 0.34f };
	};

	void Create(const Settings& settings)
	{
		m_Settings = settings;
		m_Chunks.clear();
		m_Filled.clear();

		// The lattice has to hold the whole planet plus its tallest mountain,
		// plus a chunk of margin so the outermost cells have neighbours.
		// The lattice has to hold the deepest valley as well as the tallest
		// peak, and those are not symmetric -- see `ReliefReach`.
		float reach = settings.Radius + settings.Amplitude
			+ settings.Roughness + 4.0f * settings.VoxelSize;
		int half = (int)std::ceil(reach / settings.VoxelSize);

		int side = half * 2 + 1;
		side = ((side + Egss::VoxelField3D::ChunkSize - 1)
			/ Egss::VoxelField3D::ChunkSize) * Egss::VoxelField3D::ChunkSize + 1;

		m_Field = std::make_shared<Egss::VoxelField3D>();
		m_Field->Create({ side, side, side }, settings.VoxelSize,
			glm::vec3(-(float)(side / 2) * settings.VoxelSize));

		m_ChunkWorld = (float)Egss::VoxelField3D::ChunkSize * settings.VoxelSize;

		MeasureReliefBias();

		if (m_Settings.HasOcean)
		{
			m_Settings.OceanRadius = OceanRadiusFor(m_Settings.LandFraction);

			// **Measured back over a different set of directions.** Bisecting
			// until the sampler says 29.2% and then reporting the sampler's
			// own number would prove nothing but that the loop terminated.
			// The spiral is offset by half a step here, so the check is
			// against 4,096 places the search never looked.
			const int check = 16384;
			float achieved = MeasureLandFraction(m_Settings.OceanRadius, check, 0.5f);

			// Two estimates of a proportion from finite samples do not agree
			// exactly and should not be expected to. The standard error of the
			// difference is `sqrt(p(1-p)(1/n1 + 1/n2))`, and quoting it is the
			// difference between "close enough" and knowing how close enough
			// is. Anything inside two of these is the sampler, not the search.
			float p = m_Settings.LandFraction;
			float sigma = std::sqrt(p * (1.0f - p) * (1.0f / 8192.0f + 1.0f / (float)check));

			// **The shell test's bound, against a sample of what it is bounding.**
		// `ReliefReach` is analytic and the range below is measured, and the
		// one has to contain the other -- a chunk holding ground outside the
		// bound is a chunk filled with solid rock where a valley should be,
		// which draws as a wall. Said out loud at generation because it is
		// cheap and because the failure is invisible until it is enormous.
		{
			float low = 1e30f, high = -1e30f;

			for (int i = 0; i < 8192; i++)
			{
				float r = Relief(SpiralDirection(i, 8192, 0.31f));
				low = glm::min(low, r);
				high = glm::max(high, r);
			}

			EGSS_TRACE("  relief {0:+.1f} to {1:+.1f} m sampled, bound {2:.1f} m, "
				"bias {3:+.1f} m{4}", low, high, ReliefReach(), m_ReliefBias,
				glm::max(-low, high) <= ReliefReach() ? "" : "  -- OUTSIDE THE BOUND");
		}

		EGSS_TRACE("  sea level {0:+.2f} m about the mean radius, land {1:.1f}% "
				"(asked {2:.1f}%, {3:.1f} sigma on other directions)",
				m_Settings.OceanRadius - m_Settings.Radius,
				achieved * 100.0f, p * 100.0f, std::abs(achieved - p) / sigma);
		}

		BuildHydrology();
		BuildColourMap();
	}

	// --- Where the water goes -----------------------------------------------
	//
	// **Biomes are a function of drainage, so drainage is computed first.**
	//
	// The obvious way to place biomes is two more noise fields, one called
	// temperature and one called humidity, and a Whittaker lookup between
	// them. That produces coloured noise: rainforest on a ridge, desert in a
	// valley, and no relationship at all between where the green is and where
	// the water would actually be. What makes a biome map read as a world is
	// that the wet places are the places water collects, and knowing those
	// means solving the same problem a river network does.
	//
	// So: one grid, one depression fill, one flow accumulation, and moisture
	// falls out of it. Rivers and lakes fall out of the same pass and are not
	// drawn yet -- see the roadmap -- but the numbers are here.
	//
	// The grid is the one the colour map uses, so a texel means the same thing
	// in both and no resampling is needed between them.
	struct Hydrology
	{
		int Width = 0;
		int Height = 0;

		std::vector<float> Land;      // metres above sea level; negative is sea
		std::vector<float> Filled;    // after the depressions are filled

		// **The water surface, and it is not `Filled`.** The fill adds an
		// epsilon a cell so that routing always has somewhere downhill to go,
		// which means a filled basin slopes very slightly toward its outlet --
		// a hundredth of a metre across a hundred cells. That is right for
		// deciding where water *goes* and wrong for drawing what it looks
		// like, because a lake is level. This is the same flood without the
		// epsilon: exactly the spill elevation of whatever basin the cell is
		// in, and exactly flat across it.
		std::vector<float> Level;
		std::vector<float> Flow;      // square metres draining through, upstream
		std::vector<float> Moisture;  // 0 arid, 1 saturated
		std::vector<float> Warmth;    // 0 polar, 1 equatorial

		bool Valid() const { return Width > 0 && Height > 0; }

		size_t At(int x, int y) const { return (size_t)y * Width + x; }
	};

	const Hydrology& Water() const { return m_Water; }

	// **Where the water surface is, in metres about the mean radius.**
	//
	// Sea level out at sea, the basin's spill height in a lake, and *no water
	// at all* on dry land -- which is the whole point, and the thing a shell
	// at a fixed radius cannot say. A hole dug in the middle of a continent is
	// below sea level and still dry, because what decides whether there is
	// water somewhere is whether water can get there, not what altitude it is
	// at.
	float WaterHeightAt(const glm::vec3& direction) const
	{
		if (!m_Water.Valid())
			return 0.0f;

		return SampleHydrology(m_Water.Level, direction);
	}

	// 1 where there is standing water, 0 where there is not, bilinear between.
	// The soft edge does not matter: the shoreline a viewer sees comes from
	// the ground occluding the water surface, at whatever resolution the
	// terrain mesh has, not from this.
	float WetnessAt(const glm::vec3& direction) const
	{
		if (!m_Water.Valid())
			return 0.0f;

		return SampleHydrology(m_Wet, direction);
	}

	// The two axes of the Whittaker square, where the plant scatter and the
	// fragment shader both read them.
	float MoistureAt(const glm::vec3& direction) const
	{
		return m_Water.Valid() ? SampleHydrology(m_Water.Moisture, direction) : 0.0f;
	}

	float WarmthAt(const glm::vec3& direction) const
	{
		return m_Water.Valid() ? SampleHydrology(m_Water.Warmth, direction) : 0.0f;
	}

	// Bilinear over the grid, wrapping in longitude and clamping at the poles.
	// The same lookup the colour map's texel does, so what the shader reads and
	// what the plant placement reads cannot disagree.
	float SampleHydrology(const std::vector<float>& field,
		const glm::vec3& direction) const
	{
		if (!m_Water.Valid() || field.empty())
			return 0.0f;

		const float pi = 3.14159265358979323846f;

		float phi = std::acos(glm::clamp(direction.y, -1.0f, 1.0f));
		float theta = std::atan2(direction.z, direction.x);

		float u = (theta / (2.0f * pi) + 0.5f) * (float)m_Water.Width - 0.5f;
		float v = (phi / pi) * (float)m_Water.Height - 0.5f;

		int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
		float fx = u - (float)x0, fy = v - (float)y0;

		auto read = [&](int x, int y)
		{
			x = ((x % m_Water.Width) + m_Water.Width) % m_Water.Width;
			y = glm::clamp(y, 0, m_Water.Height - 1);

			return field[m_Water.At(x, y)];
		};

		float a = glm::mix(read(x0, y0), read(x0 + 1, y0), fx);
		float b = glm::mix(read(x0, y0 + 1), read(x0 + 1, y0 + 1), fx);

		return glm::mix(a, b, fy);
	}

	// --- Sea level ----------------------------------------------------------

	// Directions spread evenly enough to average -- the same Fibonacci spiral
	// the relief bias uses, with `offset` shifting it off the samples any
	// other caller took.
	static glm::vec3 SpiralDirection(int i, int samples, float offset)
	{
		const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));

		double t = ((double)i + (double)offset) / (double)samples;
		double z = 1.0 - 2.0 * t;
		double r = std::sqrt(std::max(0.0, 1.0 - z * z));
		double angle = golden * (double)i;

		return glm::vec3((float)(r * std::cos(angle)),
			(float)(r * std::sin(angle)), (float)z);
	}

	// What fraction of the surface stands above a given radius.
	float MeasureLandFraction(float oceanRadius, int samples, float offset) const
	{
		float sea = oceanRadius - m_Settings.Radius;
		int land = 0;

		for (int i = 0; i < samples; i++)
			if (Relief(SpiralDirection(i, samples, offset)) > sea)
				land++;

		return (float)land / (float)samples;
	}

	// Bisection, because the fraction is monotone in the radius -- raise the
	// sea and land can only shrink -- even though the relief's distribution is
	// nothing as simple.
	float OceanRadiusFor(float target) const
	{
		float low = m_Settings.Radius - m_Settings.Amplitude;
		float high = m_Settings.Radius + m_Settings.Amplitude;

		// Twenty-four halvings resolves the radius to a millionth of the
		// relief, which is far finer than 8,192 samples can distinguish -- the
		// sampling is the floor here, not the search.
		for (int i = 0; i < 24; i++)
		{
			float middle = 0.5f * (low + high);

			if (MeasureLandFraction(middle, 8192, 0.0f) > target)
				low = middle;
			else
				high = middle;
		}

		return 0.5f * (low + high);
	}

	// --- Somewhere to stand -------------------------------------------------

	// **The nearest direction that is dry land.**
	//
	// A lander aimed at the sunward point of this planet is under water seven
	// times in ten, because that is what a 29.2% land fraction means -- and
	// nothing here swims. You arrive standing on the seabed with the sea
	// closed over your head, which reads as a rendering fault and is not one.
	//
	// The search is the same Fibonacci spiral the sea level was bisected on,
	// offset clear of both of the sample sets that produced it. Even coverage
	// is what makes it enough to keep the passing direction with the largest
	// dot against `preferred`: that is the globally nearest one to within the
	// spacing, which at 8,192 samples is 2.3 degrees, or 14 m of arc on Earth.
	//
	// **One dry sample is not a landing site.** Relief is continuous, so the
	// first direction that clears the water on the way out of a bay is a beach
	// a metre wide, and a lander put there walks straight back into the sea.
	// `clearMetres` of arc has to be dry in four directions as well, so what
	// comes back is ground you can stand in the middle of. If nothing that
	// generous exists the requirement is halved twice and then dropped, and
	// what was achieved is logged rather than quietly substituted.
	glm::vec3 NearestLand(const glm::vec3& preferred, float clearMetres) const
	{
		glm::vec3 want = glm::normalize(preferred);

		if (!m_Settings.HasOcean)
			return want;

		float sea = m_Settings.OceanRadius - m_Settings.Radius;

		const int samples = 8192;

		// **Where you were already pointing, if it will do.** Otherwise the
		// answer is a point on a grid, and a lander aimed squarely at the
		// middle of a continent would be shifted to whichever sample happened
		// to be nearest -- up to a spiral spacing away for no reason at all.
		if (Relief(want) > sea
			&& DryAround(want, sea, clearMetres / std::max(1.0f, m_Settings.Radius)))
			return want;

		for (float share : { 1.0f, 0.5f, 0.25f, 0.0f })
		{
			float arc = clearMetres * share / std::max(1.0f, m_Settings.Radius);

			glm::vec3 best(0.0f);
			float bestDot = -2.0f;

			for (int i = 0; i < samples; i++)
			{
				glm::vec3 direction = SpiralDirection(i, samples, 0.25f);

				// The dot is three multiplies and each probe is five octaves
				// of 3D noise, so reject on the cheap test first.
				float dot = glm::dot(direction, want);

				if (dot <= bestDot || Relief(direction) <= sea)
					continue;

				if (share > 0.0f && !DryAround(direction, sea, arc))
					continue;

				best = direction;
				bestDot = dot;
			}

			if (bestDot > -2.0f)
			{
				EGSS_TRACE("  landing site {0:.1f} deg off the approach, {1:+.1f} m "
					"above sea, dry for {2:.0f} m around",
					glm::degrees(std::acos(glm::clamp(bestDot, -1.0f, 1.0f))),
					Relief(best) - sea, clearMetres * share);

				return best;
			}
		}

		// Reachable only on a world with no land above water anywhere, which
		// this generator does not make at any land fraction above zero. Said
		// out loud rather than handing back `preferred` and letting the lander
		// drown quietly.
		EGSS_WARN("no dry land anywhere on this planet");

		return want;
	}

	// **A disc, and not a cross -- sampled against the terrain's own finest
	// detail rather than against the size of the disc.**
	//
	// Two earlier versions were wrong in ways worth keeping written down.
	// Four probes on two axes let diagonal inlets through, which an
	// independent probe set caught in 3% of the sites it accepted. Rings at
	// fixed *fractions* of the arc then left the middle unprobed -- at a 10 m
	// clearance the innermost ring sat 4.5 m out, so a pond three metres from
	// the site was invisible, and 20 of 512 sites had water inside 3 m.
	//
	// What sets the spacing is the relief, not the disc. The noise runs for
	// `Octaves` octaves from a base wavelength of `FeatureSize`, so the finest
	// thing in it is `FeatureSize / 2^(Octaves-1)` across -- 2.6 m on Earth.
	// Probes half of that apart cannot step over a feature entirely, which is
	// the most any finite set can promise. It works out at eight rings and
	// about 220 points for a 10 m disc, and they are only ever evaluated for a
	// candidate that has already beaten the best angle so far.
	bool DryAround(const glm::vec3& direction, float sea, float arc) const
	{
		// Any vector not parallel to the direction will do to start the frame,
		// and near the poles +Y is parallel to it.
		glm::vec3 pole = std::abs(direction.y) > 0.9f
			? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

		glm::vec3 east = glm::normalize(glm::cross(pole, direction));
		glm::vec3 north = glm::cross(direction, east);

		const float pi = 3.14159265358979323846f;

		float finest = m_Settings.FeatureSize
			/ (float)(1 << glm::max(m_Settings.Octaves - 1, 0));

		float step = glm::max(0.5f * finest, 0.05f) / glm::max(m_Settings.Radius, 1.0f);

		int rings = glm::clamp((int)std::ceil(arc / step), 1, 12);

		for (int ring = 1; ring <= rings; ring++)
		{
			float radius = arc * (float)ring / (float)rings;

			// As many points as it takes to keep the gap around the ring to
			// the same step as the gap between rings.
			int count = glm::clamp((int)std::ceil(2.0f * pi * radius / step), 4, 64);

			float c = std::cos(radius), s = std::sin(radius);

			for (int i = 0; i < count; i++)
			{
				// Half a step of phase per ring, so the rings do not line up
				// into spokes with wedges of unprobed ground between them.
				float angle = 2.0f * pi * ((float)i + 0.5f * (float)ring) / (float)count;

				glm::vec3 sideways = east * std::cos(angle) + north * std::sin(angle);

				// The combination is already unit length, which `Relief` wants.
				if (Relief(direction * c + sideways * s) <= sea)
					return false;
			}
		}

		return true;
	}

	// **The relief's mean, measured rather than assumed.**
	//
	// `Relief` is built from `1 - |noise|`, which makes ridges instead of
	// blobs, and the obvious recentring is to subtract a half. That is wrong:
	// the mean of `|noise|` for this generator is about 0.165, not 0.5, so a
	// planet asked for a 300 m radius came out with a mean surface at 308.71 m
	// -- and nothing on screen says so, because a sphere of the wrong size
	// still looks like a sphere. Everything derived from the radius would have
	// carried the error: where an ocean sits, where a lander stops, what
	// gravity is at the surface.
	//
	// So the bias is sampled at generation, over directions spread evenly
	// enough to average it, and subtracted. The check in the temporary test
	// samples a different set with a different generator, so agreement between
	// them is not circular.
	void MeasureReliefBias()
	{
		m_ReliefBias = 0.0f;

		const int samples = 2048;
		double sum = 0.0;

		// A spiral over the sphere: even coverage without needing a random
		// number generator whose state would then be part of the terrain.
		const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));

		for (int i = 0; i < samples; i++)
		{
			double z = 1.0 - 2.0 * ((double)i + 0.5) / (double)samples;
			double r = std::sqrt(std::max(0.0, 1.0 - z * z));
			double angle = golden * (double)i;

			glm::vec3 direction((float)(r * std::cos(angle)),
				(float)(r * std::sin(angle)), (float)z);

			sum += Relief(direction);
		}

		m_ReliefBias = (float)(sum / samples);
	}

	// **How far the ground can get from the mean radius, and it is not half
	// the amplitude.**
	//
	// `shape` is bounded to ±1/2, so `shape * Amplitude` is bounded to ±A/2 --
	// and then the measured bias is *subtracted*, which slides the whole range
	// down by about a third of A without narrowing it. The ground therefore
	// reaches `A/2 + bias` below the mean radius and only `A/2 - bias` above:
	// the relief of this generator is skewed, because `1 - |noise|` is.
	//
	// **The shell test used A/2 and had done since the start.** At a 31 m
	// amplitude the third of A it was missing was ten metres -- less than half
	// a chunk, so almost nothing fell in it and nothing was ever seen. At
	// 625 m it is 209 metres, which is eight chunks of valley floor classified
	// as "nowhere near the surface" and filled with solid rock. That is a
	// **wall**, and the walls scale with the amplitude, which scales with the
	// radius: invisible at 360 m, a fine grid at 250 km, slabs the height of a
	// house at 1,000 km.
	//
	// Analytic rather than sampled: a min and max over a few thousand
	// directions would find *most* of the range, and the chunks it missed
	// would be exactly the rare deep ones nobody would think to look at.
	float ReliefReach() const
	{
		// `ridge` is in [0, 1] and the layer is `Landscape * share *
		// (ridge - 0.35)`, so the furthest it reaches either way is 0.65 of
		// the amplitude at full uplift. Analytic, like the rest of this: a
		// sampled bound would miss exactly the one peak that matters, and a
		// chunk outside the bound is not merely unfilled -- it is filled with
		// solid rock, which is a wall.
		return 0.5f * m_Settings.Amplitude + std::abs(m_ReliefBias)
			+ 0.5f * m_Settings.Roughness
			+ 0.65f * m_Settings.Landscape;
	}

	const Settings& Get() const { return m_Settings; }
	// --- Digging, and edits that survive being walked away from ------------
	//
	// **A planet is procedural, so the only chunks worth storing are the ones
	// somebody changed.** OpenWorld caches everything, because there the cache
	// buys time and the world fits in a file. Here it buys *data*: eviction is
	// regeneration, so a hole dug and walked away from was a hole that healed
	// itself, and no amount of cache would hold a planet anyway.
	//
	// So the set of edited chunks is tracked, they go to the file the moment
	// they change, and the fill checks that set before it generates.
	// **The landing site, kept on disk between runs.**
	//
	// Everything here is procedural, so a cache buys time rather than data --
	// which is why the *edits* file exists and a general chunk cache does not.
	// The default landing site is the exception, and it is the exception
	// precisely because it is fixed: the same nineteen thousand chunks are
	// generated on every single run, before anything is drawn, and in Debug
	// that is **10.4 seconds of the twenty it takes to open the demo**.
	//
	// Keyed by the same fingerprint as the edits, so changing the terrain
	// function throws the file away instead of loading a landscape that no
	// longer exists.
	//
	// Only while `Prefilling` is set, and only the chunks that hold something.
	// A chunk of open sky or solid rock is one float in the field and 27
	// samples to decide -- storing those would multiply the file by twelve to
	// save nothing -- and caching everything a walk touches would grow it
	// without bound, which is the behaviour a *site* cache exists not to have.
	void OpenSiteCache(const std::string& path)
	{
		m_Site.Open(path, Fingerprint());
	}

	void SetPrefilling(bool prefilling) { m_Prefilling = prefilling; }

	unsigned int CacheHits() const { return m_Site.Hits(); }
	unsigned int CacheWrites() const { return m_Site.Written(); }

	void OpenEdits(const std::string& path)
	{
		m_Edits.Open(path, Fingerprint());

		if (m_Edits.Rebuilt())
			return;

		// The index is the record of what was edited: anything in the file was
		// put there by a dig, because nothing else writes to it.
		m_Edited.clear();

		for (const glm::ivec3& chunk : m_Edits.Chunks())
			m_Edited.insert(Key(chunk));
	}

	// **Derived from what the generator does, not from a version number.**
	// Change the terrain function and every stored chunk describes a different
	// planet; a number somebody has to remember to bump is a session lost to
	// digging in a world that no longer exists. Sampling the density itself
	// cannot drift out of step with it.
	unsigned long long Fingerprint() const
	{
		unsigned long long hash = 1469598103934665603ull;

		auto mix = [&hash](double value)
		{
			unsigned long long bits;
			float single = (float)value;
			std::memcpy(&bits, &single, sizeof(single));

			hash = (hash ^ bits) * 1099511628211ull;
		};

		mix(m_Settings.Radius);
		mix(m_Settings.VoxelSize);
		mix((double)m_Settings.Seed);

		for (int i = 0; i < 64; i++)
		{
			glm::vec3 direction = SpiralDirection(i, 64, 0.17f);

			mix(Density(direction * (m_Settings.Radius + (float)(i % 5) * 3.0f)));
		}

		return hash;
	}

	// **Sphere tracing in a lattice-relative frame.**
	//
	// `VoxelField3D::Raycast` would do this, and does it in the field's own
	// coordinates -- which on a planet is the half-metre lattice everything
	// else here has just been moved off. The march is short enough to be worth
	// writing rather than widening another engine signature: the field says
	// how far it is safe to step, which is the whole algorithm.
	bool RayToSurface(const glm::vec3& origin, const glm::vec3& direction,
		const glm::ivec3& about, float maximum, glm::vec3& outPoint,
		glm::vec3& outNormal) const
	{
		float travelled = 0.0f;

		for (int step = 0; step < 128 && travelled < maximum; step++)
		{
			glm::vec3 at = origin + direction * travelled;
			float distance = m_Field->SampleDistanceFrom(at, about);

			if (distance < m_Settings.VoxelSize * 0.25f)
			{
				outPoint = at;
				outNormal = m_Field->SampleNormalFrom(at, about);

				return true;
			}

			// Capped: an unallocated chunk reads `Far`, which is a sentinel
			// and not a distance, and one step of it leaves the planet.
			travelled += glm::min(distance,
				(float)Egss::VoxelField3D::ChunkSize * m_Settings.VoxelSize);
		}

		return false;
	}

	// Carves or fills a sphere, in the frame `about` names. Returns the number
	// of voxels that changed sign -- zero means the spade hit nothing.
	int Dig(const glm::vec3& centre, const glm::ivec3& about, float radius, bool add)
	{
		int changed = m_Field->EditSphereFrom(centre, about, radius, add);

		if (changed == 0)
			return 0;

		// **What was written and what merely went stale are two sets.**
		//
		// The sphere reaches `radius` plus the band `EditSphere` keeps correct
		// either side of it -- those chunks changed and belong in the file. A
		// mesh reads one plane into its seven high neighbours, so the chunks
		// *below* those have meshes that are now wrong -- and they belong in
		// the remesh queue and nowhere near the file. Writing the second set
		// too would store the generator's own output as though somebody had
		// dug it, which is 125 chunks a hole instead of a handful.
		float margin = (float)Egss::VoxelField3D::SparseBandVoxels
			* m_Settings.VoxelSize + m_Settings.VoxelSize;

		float reach = radius + margin;

		glm::ivec3 low = about + glm::ivec3(glm::floor((centre - glm::vec3(reach))
			/ m_Settings.VoxelSize));
		glm::ivec3 high = about + glm::ivec3(glm::ceil((centre + glm::vec3(reach))
			/ m_Settings.VoxelSize));

		glm::ivec3 count = m_Field->ChunkCount();

		glm::ivec3 first = glm::max(low / Egss::VoxelField3D::ChunkSize, glm::ivec3(0));
		glm::ivec3 last = glm::min(high / Egss::VoxelField3D::ChunkSize,
			count - glm::ivec3(1));

		const glm::ivec3* offsets = HighNeighbourOffsets();

		for (int z = first.z; z <= last.z; z++)
		for (int y = first.y; y <= last.y; y++)
		for (int x = first.x; x <= last.x; x++)
		{
			glm::ivec3 chunk(x, y, z);
			size_t key = Key(chunk);

			if (!m_Field->HasChunk(chunk))
				continue;

			m_Field->SaveChunk(chunk, m_Scratch);
			m_Edits.Write(chunk, m_Scratch);

			m_Edited.insert(key);
			m_Filled.insert(key);
			m_Dirty.insert(key);

			for (int i = 0; i < 7; i++)
			{
				glm::ivec3 lower = chunk - offsets[i];

				if (lower.x < 0 || lower.y < 0 || lower.z < 0)
					continue;

				if (m_Chunks.count(Key(lower)))
					m_Dirty.insert(Key(lower));
			}
		}

		return changed;
	}

	size_t EditedChunks() const { return m_Edited.size(); }

	// The lattice point nearest a place, and where a lattice point is. The
	// pair a caller needs to put anything into a frame the field can address
	// without ever forming a planet-sized float -- see `RigidBody3D::MakeSdf`.
	glm::ivec3 LatticeNear(const glm::dvec3& fixed) const
	{
		return glm::ivec3(glm::round(
			(fixed - glm::dvec3(m_Field->Origin())) / (double)m_Settings.VoxelSize));
	}

	glm::dvec3 LatticePosition(const glm::ivec3& lattice) const
	{
		return glm::dvec3(m_Field->Origin())
			+ glm::dvec3(lattice) * (double)m_Settings.VoxelSize;
	}

	const std::shared_ptr<Egss::VoxelField3D>& Field() const { return m_Field; }

	// --- Generation ---------------------------------------------------------

	// Signed distance to the surface: negative inside the planet.
	float Density(const glm::vec3& p) const
	{
		float distance = glm::length(p);

		// The exact centre has no direction, and normalising it is a division
		// by zero that fills the middle of the planet with NaN -- which reads
		// as a hole through the core rather than as an error.
		if (distance < 1e-4f)
			return -m_Settings.Radius;

		return distance - m_Settings.Radius - Relief(p / distance);
	}

	// Metres of ground above the mean radius in a given direction.
	//
	// `maxOctaves` caps how fine the sum goes. The colour map passes it: one
	// of its texels is tens of kilometres of ground on a real planet, so
	// octaves below that are not detail, they are aliasing -- and they are the
	// expensive ones, because there are more of them than of everything else.
	float Relief(const glm::vec3& direction, int maxOctaves = 0) const
	{
		float amplitude = 1.0f;

		// Cycles across the planet: how many features of `FeatureSize` fit
		// round it. The noise is sampled on the sphere of radius `Radius`, so
		// a feature is that many metres of actual ground.
		float frequency = m_Settings.Radius / std::max(1.0f, m_Settings.FeatureSize);

		float sum = 0.0f;
		float total = 0.0f;

		int octaves = maxOctaves > 0
			? glm::min(maxOctaves, m_Settings.Octaves) : m_Settings.Octaves;

		for (int i = 0; i < octaves; i++)
		{
			sum += Noise3D(direction * frequency, m_Settings.Seed + (unsigned int)i) * amplitude;
			total += amplitude;

			amplitude *= 0.5f;
			frequency *= 2.0f;
		}

		float unit = total > 0.0f ? sum / total : 0.0f;

		// Ridged rather than plain: `1 - |n|` puts the peaks where the noise
		// crosses zero, which makes ridgelines instead of rolling blobs, and
		// then it is recentred so the mean stays near the mean radius -- the
		// volume check depends on that being true.
		float ridged = 1.0f - std::abs(unit);
		float shape = ridged - 0.5f;

		if (m_Settings.ContinentShare > 0.0f && m_Settings.ContinentSize > 0.0f)
		{
			float f = m_Settings.Radius / m_Settings.ContinentSize;

			float broad = Noise3D(direction * f, m_Settings.Seed + 101u)
				+ Noise3D(direction * f * 2.0f, m_Settings.Seed + 102u) * 0.5f;

			shape = glm::mix(shape, broad / 3.0f, m_Settings.ContinentShare);
		}

		float relief = shape * m_Settings.Amplitude - m_ReliefBias;

		relief += Landscape(direction, maxOctaves);

		// The local layer, added rather than mixed: it is small next to the
		// planetary relief by construction, so it cannot push the surface
		// outside the shell the lattice was sized for.
		if (m_Settings.Roughness > 0.0f && m_Settings.RoughnessSize > 0.0f)
		{
			float f = m_Settings.Radius / m_Settings.RoughnessSize;
			float weight = 1.0f;
			float local = 0.0f;
			float total = 0.0f;

			for (int i = 0; i < 3; i++)
			{
				local += Noise3D(direction * f, m_Settings.Seed + 401u + (unsigned int)i)
					* weight;
				total += weight;

				weight *= 0.5f;
				f *= 2.0f;
			}

			relief += (local / total) * m_Settings.Roughness * 0.5f;
		}

		return relief;
	}

	// The local landscape layer: see `Settings::Landscape`.
	//
	// Ridged like the planetary term, and for the same reason -- `1 - |n|`
	// peaks where the noise crosses zero, which is a line rather than a blob,
	// and a line is what a ridge is. Then squared, which is the whole
	// difference between hills and mountains: squaring a value in [0, 1]
	// leaves the peaks where they are and pushes everything else down, so the
	// ridges stay sharp and the ground between them broadens into valley
	// floors instead of sagging evenly.
	//
	// The `- 0.35` is where the valley floor sits relative to the mean, so the
	// layer neither raises nor lowers the planet much on average; `Create`
	// measures the residual into `m_ReliefBias` regardless.
	float Landscape(const glm::vec3& direction, int maxOctaves = 0) const
	{
		if (m_Settings.Landscape <= 0.0f || m_Settings.LandscapeSize <= 0.0f)
			return 0.0f;

		// **The map's cap applies here too.** `maxOctaves` is how many
		// planetary octaves the caller can represent; the wavelength of the
		// last one it kept is the finest wavelength it can represent at all,
		// and this layer is cut at the same place. Without it the 1,024-wide
		// map samples a 131 m ridge on a 1.5 km texel, which does not merely
		// look wrong -- an aliased height field routes water into pits that
		// are not there, and the drainage pass believes it.
		int octaves = m_Settings.LandscapeOctaves;

		if (maxOctaves > 0 && maxOctaves < m_Settings.Octaves)
		{
			float finest = m_Settings.FeatureSize
				/ (float)(1 << glm::max(maxOctaves - 1, 0));

			octaves = 1;

			while (octaves < m_Settings.LandscapeOctaves
				&& m_Settings.LandscapeSize / (float)(1 << octaves) > finest)
				octaves++;
		}

		// Where mountain country is. One slow octave: this is not terrain, it
		// is a mask, and a mask with detail in it would put a peak and a plain
		// within a kilometre of each other.
		float uplift = 1.0f;

		if (m_Settings.UpliftSize > 0.0f)
		{
			float f = m_Settings.Radius / m_Settings.UpliftSize;

			float n = Noise3D(direction * f, m_Settings.Seed + 601u) * 0.5f + 0.5f;

			uplift = glm::smoothstep(0.45f, 0.95f, n);
		}

		float frequency = m_Settings.Radius
			/ glm::max(m_Settings.LandscapeSize, 1.0f);

		float weight = 1.0f;
		float sum = 0.0f;
		float total = 0.0f;

		for (int i = 0; i < octaves; i++)
		{
			float n = Noise3D(direction * frequency,
				m_Settings.Seed + 701u + (unsigned int)i);

			sum += (1.0f - std::abs(n)) * weight;
			total += weight;

			weight *= 0.5f;
			frequency *= 2.0f;
		}

		float ridge = total > 0.0f ? sum / total : 0.0f;

		ridge *= ridge;

		float share = m_Settings.LandscapeFloor
			+ (1.0f - m_Settings.LandscapeFloor) * uplift;

		return m_Settings.Landscape * share * (ridge - 0.35f);
	}

	// --- The height map -----------------------------------------------------
	//
	// **The map carries height, and the shader decides what colour that is.**
	//
	// The first version baked the finished colour here and both the meshed
	// chunks and the smooth sphere sampled it. That put the biome rule in one
	// place, which was the point -- but it also quantised every *transition*
	// to a texel. A beach is under a metre of height on this planet and a
	// texel is two metres of ground, so the sand came out as a staircase of
	// squares and a lake as a bilinear diamond.
	//
	// Height is the opposite: it is smooth, so two metres between samples
	// costs almost nothing, and the sharp part -- where sand becomes grass,
	// where the snow line is -- is evaluated per pixel in the fragment shader
	// off an interpolated height. Better still, ground close enough to be
	// meshed does not need the map at all: the chunk's own geometry *is* the
	// height, exactly.
	//
	// So the map is only ever read for the sphere that stands in for the
	// planet past the streaming radius, and for the sea, which needs to know
	// where the coast is on the far side of the world.
	// One grid, four passes: sample, fill, route, accumulate. Then moisture and
	// warmth out of the results.
	void BuildHydrology()
	{
		m_Water = Hydrology();

		// Only where there is a sea for the water to end up in. Without an
		// outlet every basin is closed, the fill has nothing to drain to, and
		// "how much flows through here" has no answer.
		if (!m_Settings.HasOcean)
			return;

		const int width = s_MapWidth;
		const int height = s_MapHeight;
		const size_t count = (size_t)width * height;

		const float pi = 3.14159265358979323846f;

		m_Water.Width = width;
		m_Water.Height = height;
		m_Water.Land.resize(count);
		m_Water.Filled.resize(count);
		m_Water.Level.resize(count);
		m_Water.Flow.resize(count, 0.0f);
		m_Water.Moisture.resize(count, 0.0f);
		m_Water.Warmth.resize(count, 0.0f);

		// **Only the octaves the grid can represent.** Sampling relief finer
		// than two texels aliases, and an aliased height field routes water
		// into pits that are not there. The same cap the colour map takes.
		int useful = UsefulOctaves(width);

		std::vector<float> area(height);

		for (int y = 0; y < height; y++)
		{
			float phi = ((float)y + 0.5f) / (float)height * pi;

			// A texel of an equirectangular grid is `R^2 sin(phi) dtheta dphi`
			// -- it shrinks toward the poles, and flow accumulation is an area
			// so it has to know that. Counting texels instead over-weights the
			// ice caps by the ratio of a pole's row to the equator's, which on
			// a 512-row grid is three hundred to one.
			area[y] = m_Settings.Radius * m_Settings.Radius * std::sin(phi)
				* (2.0f * pi / (float)width) * (pi / (float)height);

			float sinPhi = std::sin(phi), cosPhi = std::cos(phi);

			for (int x = 0; x < width; x++)
			{
				float theta = (((float)x + 0.5f) / (float)width - 0.5f) * 2.0f * pi;

				glm::vec3 direction(std::cos(theta) * sinPhi, cosPhi,
					std::sin(theta) * sinPhi);

				// **In double, and the two big terms cancel.** As floats,
				// `Radius + relief - OceanRadius` is a hundred metres computed
				// from two numbers a quarter of a million long, so it lands on
				// the 0.0156 m grid a float has at that magnitude -- and a
				// lake surface then steps by that much across itself. At 1:1
				// it would step by half a metre.
				m_Water.Land[m_Water.At(x, y)] = (float)(
					((double)m_Settings.Radius - (double)m_Settings.OceanRadius)
					+ (double)Relief(direction, useful));
			}
		}

		auto before = std::chrono::high_resolution_clock::now();

		FillDepressions();

		int stranded = AccumulateFlow(area);

		DeriveClimate(area);
		DeriveWater();

		double ms = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - before).count();

		ReportHydrology(area, stranded, ms);
	}

	// **Everything that falls on land reaches the sea, and the sea says so.**
	//
	// Sea cells start at zero and only ever receive, so the water arriving in
	// them is the total catchment of the land -- which is the land's area, one
	// cell's worth at a time. That is a conservation law the accumulation
	// knows nothing about: it adds a number to a neighbour, repeatedly, and
	// has no idea what the planet's land area is. If the two agree, every
	// drop was routed and none was counted twice.
	//
	// It is also the check that catches a depression the fill missed: a cell
	// with nowhere lower to go keeps its catchment, and the sea comes up
	// short by exactly that basin.
	void ReportHydrology(const std::vector<float>& area, int stranded,
		double ms) const
	{
		double land = 0.0, delivered = 0.0, lakes = 0.0;
		double biggest = 0.0;

		for (int y = 0; y < m_Water.Height; y++)
			for (int x = 0; x < m_Water.Width; x++)
			{
				size_t at = m_Water.At(x, y);

				if (m_Water.Land[at] > 0.0f)
				{
					land += (double)area[y];

					if (m_Water.Filled[at] > m_Water.Land[at] + 1e-2f)
						lakes += (double)area[y];
				}
				else
				{
					delivered += (double)m_Water.Flow[at];
				}

				biggest = glm::max(biggest, (double)m_Water.Flow[at]);
			}

		double error = land > 0.0 ? std::abs(delivered - land) / land : 0.0;

		// **The grid's own land fraction, which is not the sampler's.** The
		// hydrology sees the relief capped at the octaves a texel can carry,
		// so its coastline is smoother than the one the mesher cuts and its
		// land comes out about a point low. Reported rather than reconciled:
		// moisture does not care, and pretending the two agree would be worse
		// than saying they do not.
		double sphere = 4.0 * 3.14159265358979323846
			* (double)m_Settings.Radius * (double)m_Settings.Radius;

		EGSS_TRACE("  drainage: {0:.4g} km^2 of land ({1:.1f}% of the sphere), "
			"{2:.4g} delivered to the sea ({3:.3f}% out), {4} stranded, {5:.0f} ms",
			land * 1e-6, land / sphere * 100.0, delivered * 1e-6,
			error * 100.0, stranded, ms);

		EGSS_TRACE("  the largest basin drains {0:.2f}% of the land; standing "
			"water covers {1:.2f}% of it",
			land > 0.0 ? biggest / land * 100.0 : 0.0,
			land > 0.0 ? lakes / land * 100.0 : 0.0);

		// **The four corners of the Whittaker square, by area.** A moisture
		// field that came out constant would still conserve, still drain and
		// still produce a map -- it would just produce one colour. This is the
		// check that there is a climate rather than a number.
		double quadrant[4] = { 0.0, 0.0, 0.0, 0.0 };
		double wetSum = 0.0, warmSum = 0.0;

		for (int y = 0; y < m_Water.Height; y++)
			for (int x = 0; x < m_Water.Width; x++)
			{
				size_t at = m_Water.At(x, y);

				if (m_Water.Land[at] <= 0.0f)
					continue;

				int which = (m_Water.Moisture[at] > 0.5f ? 1 : 0)
					+ (m_Water.Warmth[at] > 0.5f ? 2 : 0);

				quadrant[which] += (double)area[y];

				wetSum += (double)m_Water.Moisture[at] * (double)area[y];
				warmSum += (double)m_Water.Warmth[at] * (double)area[y];
			}

		double share = land > 0.0 ? 100.0 / land : 0.0;

		// **What makes it water rather than a shell at an altitude.**
		//
		// Three properties, each of which the old constant-radius sea failed:
		// a lake is level, water never sits on ground that is above it, and --
		// the one the whole design is for -- there are places below sea level
		// with no water on them, because water cannot get to them.
		// **Levelness is checked per basin, not per neighbour pair.** The
		// first version compared any two adjacent wet cells, which includes
		// two *different* basins meeting at a saddle -- and those are supposed
		// to differ, by exactly the height between their spill points. It
		// reported 25 mm of slope on a lake and the lakes were flat all along.
		double wetLand = 0.0, dryBelowSea = 0.0;
		double worstStanding = 0.0;

		for (int y = 0; y < m_Water.Height; y++)
			for (int x = 0; x < m_Water.Width; x++)
			{
				size_t at = m_Water.At(x, y);

				bool wet = m_Wet[at] > 0.5f;

				// **Ground below sea level with nothing on it**, which is the
				// number the whole design exists for. Counted over every cell,
				// not only the ones above sea level -- the first version of
				// this skipped everything below sea level on its way in and so
				// could only ever report zero, which it duly did.
				if (m_Water.Land[at] < 0.0f && !wet)
					dryBelowSea += (double)area[y];

				if (m_Water.Land[at] <= 0.0f)
					continue;

				if (wet)
				{
					wetLand += (double)area[y];

					// Nowhere may the surface be under the ground it covers.
					worstStanding = glm::max(worstStanding,
						(double)(m_Water.Land[at] - m_Water.Level[at]));
				}
			}

		EGSS_TRACE("  water: {0:.2f}% of land under a lake, every lake level "
			"to within {1:.5f} m of itself, no ground stands above its own "
			"surface by more than {2:.5f} m",
			land > 0.0 ? wetLand * share : 0.0, m_BasinSpread, worstStanding);

		// **The number the whole design exists for.** Ground below sea level
		// with nothing on it, because the sea cannot reach it. A shell at a
		// fixed radius has none of this by construction -- every point below
		// the radius is wet, which is exactly the behaviour that was wrong.
		EGSS_TRACE("  and {0:.4g} km^2 of land sits below sea level and dry, "
			"across {1} basins too arid to hold a lake", dryBelowSea * 1e-6,
			m_DryBasins);

		EGSS_TRACE("  climate over land: {0:.1f}% steppe, {1:.1f}% temperate, "
			"{2:.1f}% desert, {3:.1f}% tropical; mean moisture {4:.2f}, "
			"mean warmth {5:.2f}",
			quadrant[0] * share, quadrant[1] * share,
			quadrant[2] * share, quadrant[3] * share,
			land > 0.0 ? wetSum / land : 0.0, land > 0.0 ? warmSum / land : 0.0);
	}

	// **Priority-Flood, seeded from the sea.**
	//
	// Every land cell is raised until it has a path to the ocean that never
	// goes uphill: pop the lowest cell known to drain, and any neighbour it
	// reaches is at least that high. The epsilon is what makes the path
	// strictly downhill rather than flat, so the routing below always has a
	// lower neighbour to pick and cannot loop.
	//
	// A sphere has no boundary to seed from, which is the one thing that
	// differs from the usual formulation on a rectangle: the sea *is* the
	// boundary, and a planet with no sea has no outlet at all -- which is why
	// this does not run on one.
	void FillDepressions()
	{
		const int width = m_Water.Width, height = m_Water.Height;

		// Enough to break ties and nowhere near the metre the relief is
		// quoted in.
		const float epsilon = 1.0e-3f;

		std::vector<unsigned char> done((size_t)width * height, 0);

		// **Only the sea that is connected to the sea.**
		//
		// This used to seed from every cell below sea level, which quietly
		// asserted the thing the whole design is against: that water exists
		// wherever the altitude is low enough. A basin ringed by land and
		// floored below sea level would be seeded as ocean and come out full,
		// with no path for a drop to have got there. Reaching it by flood fill
		// from the actual sea instead leaves it as what it is -- a hole in the
		// middle of a continent, below sea level, dry.
		std::vector<unsigned char> ocean = ReachableSea();

		using Entry = std::pair<float, int>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;

		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
			{
				size_t at = m_Water.At(x, y);

				if (!ocean[at])
					continue;

				m_Water.Filled[at] = m_Water.Land[at];
				m_Water.Level[at] = 0.0f;

				done[at] = 1;

				// **Ordered by the level, not by the filled surface.**
				//
				// Priority-Flood is correct because a cell is first reached
				// along its lowest path -- and "lowest" has to mean the same
				// thing as the value being propagated. Ordering by the epsilon
				// surface while propagating the level breaks that: a rim cell
				// whose epsilons have accumulated pops early and hands a basin
				// a spill height 40 mm too high. Measured as exactly that, on
				// a planet whose lakes are supposed to be flat.
				open.push({ m_Water.Level[at], (int)at });
			}

		while (!open.empty())
		{
			Entry entry = open.top();
			open.pop();

			int current = entry.second;
			int x = current % width, y = current / width;

			for (int d = 0; d < 8; d++)
			{
				int nx, ny;

				if (!Neighbour(x, y, d, nx, ny))
					continue;

				size_t next = m_Water.At(nx, ny);

				if (done[next])
					continue;

				m_Water.Filled[next] = glm::max(m_Water.Land[next],
					m_Water.Filled[current] + epsilon);

				// Without the epsilon, so a basin's cells all take the same
				// spill height and the lake in it comes out level.
				m_Water.Level[next] = glm::max(m_Water.Land[next],
					m_Water.Level[current]);

				done[next] = 1;
				open.push({ m_Water.Level[next], (int)next });
			}
		}
	}

	// Which below-sea-level cells the sea can actually reach, by flood fill
	// from the deepest one. There is only ever one world ocean on a planet
	// generated this way -- a second, disconnected sea would show up here as a
	// basin instead, which is the correct thing for it to be.
	std::vector<unsigned char> ReachableSea() const
	{
		const int width = m_Water.Width, height = m_Water.Height;
		const size_t count = (size_t)width * height;

		std::vector<unsigned char> reached(count, 0);

		size_t deepest = 0;

		for (size_t i = 1; i < count; i++)
			if (m_Water.Land[i] < m_Water.Land[deepest])
				deepest = i;

		if (m_Water.Land[deepest] > 0.0f)
			return reached;

		std::vector<int> queue;
		queue.reserve(count);

		reached[deepest] = 1;
		queue.push_back((int)deepest);

		for (size_t head = 0; head < queue.size(); head++)
		{
			int at = queue[head];
			int x = at % width, y = at / width;

			for (int d = 0; d < 8; d++)
			{
				int nx, ny;

				if (!Neighbour(x, y, d, nx, ny))
					continue;

				size_t next = m_Water.At(nx, ny);

				if (reached[next] || m_Water.Land[next] > 0.0f)
					continue;

				reached[next] = 1;
				queue.push_back((int)next);
			}
		}

		return reached;
	}

	// Steepest descent on the filled surface, then one sweep from high to low.
	//
	// **Sorted rather than recursed.** Every cell's downstream neighbour is
	// strictly lower after the fill, so visiting cells in decreasing height
	// guarantees a cell's own upstream contributions have all arrived before
	// it is spent -- no recursion, no visited set, one pass.
	int AccumulateFlow(const std::vector<float>& area)
	{
		const int width = m_Water.Width, height = m_Water.Height;
		const size_t count = (size_t)width * height;

		std::vector<int> down(count, -1);
		std::vector<int> order(count);

		int stranded = 0;

		for (size_t i = 0; i < count; i++)
		{
			order[i] = (int)i;

			int x = (int)(i % width), y = (int)(i / width);

			if (m_Water.Land[i] <= 0.0f)
				continue;

			float best = m_Water.Filled[i];
			int pick = -1;

			for (int d = 0; d < 8; d++)
			{
				int nx, ny;

				if (!Neighbour(x, y, d, nx, ny))
					continue;

				size_t next = m_Water.At(nx, ny);

				if (m_Water.Filled[next] < best)
				{
					best = m_Water.Filled[next];
					pick = (int)next;
				}
			}

			down[i] = pick;
			m_Water.Flow[i] = area[y];

			if (pick < 0)
				stranded++;
		}

		std::sort(order.begin(), order.end(), [this](int a, int b)
		{
			return m_Water.Filled[a] > m_Water.Filled[b];
		});

		for (int at : order)
			if (down[at] >= 0)
				m_Water.Flow[down[at]] += m_Water.Flow[at];

		return stranded;
	}

	// Moisture and warmth, which is what a biome actually is.
	void DeriveClimate(const std::vector<float>& area)
	{
		const int width = m_Water.Width, height = m_Water.Height;
		const float pi = 3.14159265358979323846f;

		// How far the sea reaches inland, as a share of the planet. Coasts are
		// wet and continental interiors are not, and the scale that decides
		// which is which is the size of the continents.
		float reach = glm::max(m_Settings.ContinentSize * 0.35f,
			m_Settings.Radius * 0.05f);

		std::vector<float> toSea = DistanceToSea();

		for (int y = 0; y < height; y++)
		{
			float phi = ((float)y + 0.5f) / (float)height * pi;
			float latitude = pi * 0.5f - phi;

			// **Three wet bands and three dry ones**, which is what the
			// circulation gives: rising air at the equator and at about sixty
			// degrees, sinking air at thirty and at the poles. One cosine with
			// a sixty-degree period says exactly that, and is a great deal
			// less arbitrary than a noise field called humidity.
			float band = 0.5f + 0.5f * std::cos(latitude * 6.0f);

			// Warmth is the cosine of the latitude with a lapse rate on top.
			// Quoted against the relief rather than in kelvin per kilometre so
			// that a planet with more relief has a snow line in the same place
			// relative to its own mountains.
			float sun = glm::clamp(std::cos(latitude) * 1.12f - 0.06f, 0.0f, 1.0f);

			for (int x = 0; x < width; x++)
			{
				size_t at = m_Water.At(x, y);

				float altitude = glm::max(m_Water.Land[at], 0.0f);

				m_Water.Warmth[at] = glm::clamp(
					sun - 0.55f * altitude / glm::max(m_Settings.Amplitude * 0.5f, 1.0f),
					0.0f, 1.0f);

				// **Flow as a multiple of the cell's own catchment**, in
				// decades. A cell that only ever collects its own rain reads
				// zero; one draining a thousand times its own area reads one.
				// The log is the point -- drainage areas span the whole planet
				// and a linear scale would be one bright river and a dry world.
				float own = glm::max(area[y], 1.0f);
				float decades = std::log10(glm::max(m_Water.Flow[at], own) / own);

				float drained = glm::clamp(decades / 3.0f, 0.0f, 1.0f);
				float coastal = std::exp(-toSea[at] / reach);

				m_Water.Moisture[at] = glm::clamp(
					0.40f * band + 0.35f * coastal + 0.45f * drained, 0.0f, 1.0f);
			}
		}
	}

	// The wet mask: sea, or a basin whose spill height is above the ground.
	//
	// **A metre of margin, because the two surfaces disagree by less than
	// that.** `Level` equals `Land` exactly on every cell that is not in a
	// depression, and floating-point equality across half a million cells is a
	// coin toss -- without a threshold, a third of the dry planet came out
	// covered in a film of water one ulp deep.
	void DeriveWater()
	{
		const int width = m_Water.Width, height = m_Water.Height;
		const size_t count = (size_t)width * height;

		m_Wet.assign(count, 0.0f);

		std::vector<unsigned char> candidate(count, 0);

		for (size_t i = 0; i < count; i++)
		{
			if (m_Water.Land[i] <= 0.0f && m_Water.Level[i] <= 0.0f)
			{
				// The world ocean, which is wet by definition.
				m_Wet[i] = 1.0f;
				continue;
			}

			candidate[i] = m_Water.Level[i] > m_Water.Land[i] + 1.0f ? 1 : 0;
		}

		// **A basin is only a lake if its climate can keep one.**
		//
		// The depression fill puts water in every hollow on the planet, which
		// assumes it rains everywhere and never evaporates. It does not: an
		// endorheic basin in a dry place is a salt flat, and Death Valley is
		// below sea level and bone dry. So each basin is taken as a whole --
		// its own connected component -- and kept or dropped on the moisture
		// over it. As a whole, because deciding cell by cell would leave a
		// lake with holes in it, which is not a thing lakes have.
		std::vector<int> stack;

		for (size_t seed = 0; seed < count; seed++)
		{
			if (!candidate[seed])
				continue;

			std::vector<int> basin;

			stack.push_back((int)seed);
			candidate[seed] = 0;

			double wetness = 0.0;
			double spread = 0.0;

			while (!stack.empty())
			{
				int at = stack.back();
				stack.pop_back();

				basin.push_back(at);

				wetness += (double)m_Water.Moisture[at];
				spread = glm::max(spread,
					std::abs((double)(m_Water.Level[at] - m_Water.Level[seed])));

				int x = at % width, y = at / width;

				for (int d = 0; d < 8; d++)
				{
					int nx, ny;

					if (!Neighbour(x, y, d, nx, ny))
						continue;

					size_t next = m_Water.At(nx, ny);

					if (!candidate[next])
						continue;

					candidate[next] = 0;
					stack.push_back((int)next);
				}
			}

			m_BasinSpread = glm::max(m_BasinSpread, (float)spread);

			if (wetness / (double)basin.size() < s_LakeMoisture)
			{
				m_DryBasins++;
				continue;
			}

			for (int at : basin)
				m_Wet[at] = 1.0f;
		}
	}

	// Metres to the nearest sea, by a breadth-first sweep over the grid. Two
	// passes of a chamfer would be cheaper and cannot wrap round the back of
	// the planet correctly; this can, because the queue does not care about
	// scan order.
	std::vector<float> DistanceToSea() const
	{
		const int width = m_Water.Width, height = m_Water.Height;
		const size_t count = (size_t)width * height;

		std::vector<float> distance(count, -1.0f);
		std::vector<int> queue;
		queue.reserve(count);

		for (size_t i = 0; i < count; i++)
			if (m_Water.Land[i] <= 0.0f)
			{
				distance[i] = 0.0f;
				queue.push_back((int)i);
			}

		// A step in longitude is shorter near the poles; a step in latitude is
		// not. Both in metres, so the answer is a distance and not a texel
		// count.
		const float pi = 3.14159265358979323846f;
		float dLat = pi * m_Settings.Radius / (float)height;

		for (size_t head = 0; head < queue.size(); head++)
		{
			int at = queue[head];
			int x = at % width, y = at / width;

			float phi = ((float)y + 0.5f) / (float)height * pi;
			float dLon = 2.0f * pi * m_Settings.Radius * std::sin(phi) / (float)width;

			for (int d = 0; d < 8; d++)
			{
				int nx, ny;

				if (!Neighbour(x, y, d, nx, ny))
					continue;

				size_t next = m_Water.At(nx, ny);

				if (distance[next] >= 0.0f)
					continue;

				float step = std::sqrt((nx != x ? dLon * dLon : 0.0f)
					+ (ny != y ? dLat * dLat : 0.0f));

				distance[next] = distance[at] + step;
				queue.push_back((int)next);
			}
		}

		return distance;
	}

	// The eight neighbours, wrapping in longitude and stopping at the poles.
	//
	// A row past the pole is really the same row shifted half a turn, and
	// saying so properly would let water cross a pole. It cannot here, and
	// that is deliberate: an ice cap is where nothing grows and nothing
	// drains, so the two rows it would matter for are the two rows nothing
	// reads.
	static bool Neighbour(int x, int y, int d, int& outX, int& outY)
	{
		static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		static const int dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

		outY = y + dy[d];

		if (outY < 0 || outY >= s_MapHeight)
			return false;

		outX = ((x + dx[d]) % s_MapWidth + s_MapWidth) % s_MapWidth;

		return true;
	}

	// The finest octave a grid this wide can carry without aliasing.
	int UsefulOctaves(int width) const
	{
		float texel = 2.0f * 3.14159265f * m_Settings.Radius / (float)width;

		int useful = 1;

		while (useful < m_Settings.Octaves
			&& m_Settings.FeatureSize / (float)(1 << useful) > 2.0f * texel)
			useful++;

		return useful;
	}

	void BuildColourMap()
	{
		const int width = s_MapWidth;
		const int height = s_MapHeight;

		std::vector<unsigned char> pixels((size_t)width * height * 4);

		const float pi = 3.14159265358979323846f;

		int useful = UsefulOctaves(width);

		for (int y = 0; y < height; y++)
		{
			float phi = ((float)y + 0.5f) / (float)height * pi;
			float sinPhi = std::sin(phi), cosPhi = std::cos(phi);

			for (int x = 0; x < width; x++)
			{
				float theta = (((float)x + 0.5f) / (float)width - 0.5f) * 2.0f * pi;

				glm::vec3 direction(std::cos(theta) * sinPhi, cosPhi,
					std::sin(theta) * sinPhi);

				// Metres about the mean radius, folded into a byte across the
				// full relief. That is 0.12 m a step on Earth here -- coarse
				// for something underfoot, and this is never underfoot.
				// Capped at the octave whose wavelength is two texels: below
				// that the map cannot represent what it is sampling, and on a
				// real planet that is most of the octaves and nearly all of
				// the cost.
				float unit = glm::clamp(
					Relief(direction, useful) / (2.0f * m_Settings.Amplitude) + 0.5f,
					0.0f, 1.0f);

				size_t cell = (size_t)y * width + x;
				size_t at = cell * 4;

				// **Height, then the two numbers a biome is.** Three channels
				// that used to be the same byte three times, which was a
				// greyscale image stored in colour. Moisture and warmth are
				// computed on this exact grid, so a texel means one thing.
				pixels[at + 0] = (unsigned char)(unit * 255.0f);
				pixels[at + 1] = m_Water.Valid()
					? (unsigned char)(m_Water.Moisture[cell] * 255.0f) : 0;
				pixels[at + 2] = m_Water.Valid()
					? (unsigned char)(m_Water.Warmth[cell] * 255.0f) : 255;
				// **Alpha is where water is, not how transparent anything is.**
				// The sea shader used to decide by re-deriving the coastline
				// from the height channel, which can only ever say "below sea
				// level" -- and below sea level is not the same question as
				// wet. This channel is the drainage pass's answer to the
				// second one, and it is the only thing that knows about a
				// basin the sea cannot reach.
				pixels[at + 3] = m_Water.Valid() && !m_Wet.empty()
					? (unsigned char)(m_Wet[cell] * 255.0f) : 255;
			}
		}

		m_Map.reset(Egss::Texture2D::Create(width, height));
		m_Map->SetData(pixels.data(), (unsigned int)pixels.size());

		// One texel is about two metres of ground at the equator. Nearest
		// would put a visible grid on the horizon and turn the coastline the
		// sea shader reads out of this into a staircase.
		m_Map->SetSmooth(true);
	}

	const std::shared_ptr<Egss::Texture2D>& Map() const { return m_Map; }

	// --- Vegetation ---------------------------------------------------------
	//
	// **A tree belongs to the chunk its trunk stands in**, and is placed from
	// a hash of that chunk's index. That is what makes a forest stay put: walk
	// away until the chunks are evicted, walk back, and the same hash produces
	// the same trees in the same places. Storing them instead would work too
	// until the first time a planet is bigger than memory.
	//
	// Candidates are thrown at the chunk's box and the *surface* point below
	// each is kept only if it lands back inside the same box. Without that
	// test a tree near a chunk boundary is placed by both neighbours, and you
	// get pairs of trees growing out of each other along every seam.
	struct Plant
	{
		// **Relative to its chunk's origin**, not to the planet's centre --
		// see `Chunk::Origin`. Never more than a chunk diagonal, so a float
		// holds it to a fraction of a millimetre at any radius.
		glm::vec3 Position;
		glm::vec3 Up;          // which is also `normalize(Position)`
		float Yaw = 0.0f;
		float Scale = 1.0f;
		int Shape = 0;
	};

	void PlantChunk(const glm::ivec3& chunk, std::vector<Plant>& out) const
	{
		out.clear();

		if (m_Settings.PlantsPerChunk <= 0 || !m_Settings.HasOcean)
			return;

		// **In double, and the plant's own position comes out relative to
		// this.** `at` is a point on the surface, so its magnitude is the
		// planet's radius: as a float that is a 0.76 m lattice on a real
		// Earth, which is coarser than the trees are apart. The direction is a
		// unit vector and exact, the radius is one double, and the subtraction
		// that makes the position chunk-local happens before anything is cast.
		glm::dvec3 low = ChunkOriginFixed(chunk);
		glm::dvec3 high = low + glm::dvec3(m_ChunkWorld);

		float sea = m_Settings.OceanRadius;
		float top = std::max(m_Settings.Amplitude * 0.5f
			- (sea - m_Settings.Radius), 1.0f);

		for (int i = 0; i < m_Settings.PlantsPerChunk; i++)
		{
			glm::vec3 jitter(
				Hash3D(chunk.x * 3 + i, chunk.y, chunk.z, m_Settings.Seed + 811u),
				Hash3D(chunk.x, chunk.y * 3 + i, chunk.z, m_Settings.Seed + 812u),
				Hash3D(chunk.x, chunk.y, chunk.z * 3 + i, m_Settings.Seed + 813u));

			glm::dvec3 candidate = low + glm::dvec3(jitter) * (double)m_ChunkWorld;
			double length = glm::length(candidate);

			if (length < 1e-3)
				continue;

			glm::vec3 direction = glm::vec3(candidate / length);
			double radius = (double)m_Settings.Radius + (double)Relief(direction);
			glm::dvec3 at = glm::dvec3(direction) * radius;

			if (glm::any(glm::lessThan(at, low)) || glm::any(glm::greaterThanEqual(at, high)))
				continue;

			// Above the beach, below the rock line -- the same two numbers the
			// fragment shader colours the ground with, so trees stop where the
			// green does rather than marching up a snowfield.
			float height = (float)(radius - (double)sea);

			if (height < 0.8f || height / top > 0.62f)
				continue;

			// **Off the ice, and out of the desert.**
			//
			// This used to be `|direction.y| > 0.58`, a latitude cut, which is
			// the same stripe the colouring used to be and had the same
			// problem: a treeline that runs dead straight round the planet and
			// takes no notice of what is under it. Warmth carries the lapse
			// rate, so the line now bends up a valley and down over a plateau,
			// and moisture puts the edge of the forest where the water stops.
			float wet = SampleHydrology(m_Water.Moisture, direction);
			float warm = SampleHydrology(m_Water.Warmth, direction);

			if (warm < 0.22f || wet < 0.42f)
				continue;

			// Nothing grows on a cliff. The gradient is a difference of six
			// densities half a voxel apart, so it wants the planet-space point
			// -- and is a *direction*, which survives the cast.
			glm::vec3 normal = SurfaceNormal(glm::vec3(at));

			if (glm::dot(normal, direction) < 0.78f)
				continue;

			// **Forests clump, and they thin out before they stop.**
			//
			// Moisture decides how much woodland a place can carry; the noise
			// decides where inside it the clearings are. Both are needed --
			// moisture alone gives a forest with a hard edge at a map texel,
			// and noise alone gives the orchard this started as, evenly
			// scattered over every metre that qualifies.
			float canopy = glm::smoothstep(0.42f, 0.72f, wet);

			if (Hash3D(chunk.x + i * 5, chunk.y + i, chunk.z, m_Settings.Seed + 910u)
				> canopy)
				continue;

			// **A clearing is a few hundred metres, not forty kilometres.**
			//
			// This sampled the noise at `Radius / (FeatureSize * 1.5)`, which
			// on Earth is 5.8 cycles around the whole planet -- a 43 km
			// wavelength. Over the four hundred metres you can see, that is
			// not a field at all, it is a constant: either the whole landing
			// site was forest or none of it was, and thirty-five per cent of
			// the planet drew the short straw. The default site did. Measured
			// there: 20,538 candidates, 4,188 of them reaching this line, and
			// **every one rejected** -- a forest that was not thinned but
			// switched off, over a region hundreds of kilometres across.
			//
			// Six hundred metres is a clearing you can walk across, which is
			// what the comment above this always claimed it was.
			if (Noise3D(direction * (m_Settings.Radius / 600.0f),
				m_Settings.Seed + 909u) < -0.30f)
				continue;

			Plant plant;
			plant.Position = glm::vec3(at - low);
			plant.Up = direction;
			plant.Yaw = Hash3D(chunk.x, chunk.y + i, chunk.z, m_Settings.Seed + 814u)
				* 6.2831853f;
			plant.Scale = 0.6f + Hash3D(chunk.x + i, chunk.y, chunk.z,
				m_Settings.Seed + 815u) * 0.55f;
			plant.Shape = (int)(Hash3D(chunk.x, chunk.y, chunk.z + i,
				m_Settings.Seed + 816u) * 3.0f) % 3;

			out.push_back(plant);
		}
	}

	// Central differences on the density field. Six evaluations, which is
	// nothing next to the meshing that has already happened here.
	glm::vec3 SurfaceNormal(const glm::vec3& at) const
	{
		float e = m_Settings.VoxelSize * 0.5f;

		glm::vec3 gradient(
			Density(at + glm::vec3(e, 0, 0)) - Density(at - glm::vec3(e, 0, 0)),
			Density(at + glm::vec3(0, e, 0)) - Density(at - glm::vec3(0, e, 0)),
			Density(at + glm::vec3(0, 0, e)) - Density(at - glm::vec3(0, 0, e)));

		float length = glm::length(gradient);

		return length > 1e-9f ? gradient / length : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	// --- Streaming ----------------------------------------------------------

	// Fills and meshes up to `budget` chunks within `radius` of `focus`,
	// nearest first. Returns how many were meshed.
	// **The budget counts work, not chunks.**
	//
	// A chunk that gets generated and a chunk that gets rejected are not the
	// same expense -- one is 53,000 noise evaluations and the other is 27 --
	// but the rejected ones are not free either, and there are three of them
	// for every chunk kept. Counting only the fills let a step reject four
	// hundred chunks and call it one chunk of work, which is how a budget of
	// *one* still cost six milliseconds.
	static constexpr float s_RejectCost = 0.02f;

	// --- Level of detail ---------------------------------------------------
	//
	// The same idea OpenWorld uses on a flat field, and it transfers without a
	// change of shape because the *lattice* is not curved -- a planet is a
	// sphere in the density, not in the grid. Marching a chunk on every second
	// or fourth voxel is a quarter or a sixteenth of the triangles for
	// geometry that is a few hundred metres away and shrinking.

	int BandFor(float distance) const
	{
		if (!m_Lod)
			return 1;
		if (distance > m_LodFar)
			return 4;
		if (distance > m_LodNear)
			return 2;

		return 1;
	}

	// The band, with a margin that has to be crossed before a chunk actually
	// moves. Without it a chunk sitting on a boundary remeshes every step,
	// which is the whole budget spent on a mesh nobody can tell from the one
	// it replaced.
	int DesiredStride(float distance, int current) const
	{
		if (!m_Lod)
			return 1;

		int band = BandFor(distance);

		if (band == current)
			return current;

		// Coarsening is judged against the edge the chunk is leaving;
		// refining against the edge it is coming back inside.
		float edge = (band > current)
			? ((current == 1) ? m_LodNear : m_LodFar)
			: ((band == 1) ? m_LodNear : m_LodFar);

		if (band > current && distance < edge + m_LodHysteresis)
			return current;
		if (band < current && distance > edge - m_LodHysteresis)
			return current;

		return band;
	}

	// Meshes one chunk on a lattice `stride` voxels wide.
	//
	// **The seam is closed on the coarse side only.** A fine chunk already
	// agrees with its coarse neighbour, because both evaluate the same
	// tetrahedral decomposition of the same field at the same fine lattice
	// positions; it is the coarse one that has to subdivide its boundary layer
	// to meet it, and `VoxelTransition` is what does that. All of this is
	// engine machinery that already existed and had been measured on a flat
	// field -- the only thing a planet adds is that the chunks form a shell
	// rather than a slab, which the code cannot tell apart.
	//
	// A chunk needing the transition on two faces at once is out of scope here
	// exactly as it is in OpenWorld, and meshes plainly.
	void MeshChunk(const glm::ivec3& chunk, int stride)
	{
		Egss::MeshData data;
		BuildChunkMesh(chunk, stride, data);

		size_t key = Key(chunk);

		if (data.Indices.empty())
		{
			m_Chunks.erase(key);
			return;
		}

		if (data.Submeshes.empty())
		{
			Egss::Submesh all;
			all.IndexCount = (unsigned int)data.Indices.size();
			data.Submeshes.push_back(all);
		}

		data.RecalculateBounds();

		Chunk entry;
		entry.MeshPtr = std::make_shared<Egss::Mesh>(data, "PlanetChunk");
		entry.Centre = ChunkCentreFixed(chunk);
		entry.Origin = ChunkOriginFixed(chunk);
		entry.Triangles = data.Indices.size() / 3;
		entry.Stride = stride;
		entry.Coord = chunk;

		// **Plants outlive the mesh they arrived with.** They are a function
		// of the chunk index and reproduce identically, so recomputing them on
		// every level-of-detail change would be a hash and a bisection per
		// tree to arrive back where it started. They are also deliberately not
		// thinned with the lattice: a stride-4 chunk is 400 m away, the trees
		// in it are already three levels down, and dropping them would end the
		// forest at a visible circle.
		auto existing = m_Chunks.find(key);

		if (existing != m_Chunks.end())
			entry.Plants = std::move(existing->second.Plants);
		else
			PlantChunk(chunk, entry.Plants);

		m_Chunks[key] = std::move(entry);
	}

	// `MeshChunk`, plus the neighbours that meshing it just invalidated.
	//
	// **A transition is decided at mesh time from the neighbours that exist
	// then.** During streaming they mostly do not: chunks arrive in distance
	// order, so a coarse one is regularly meshed while the finer chunk beside
	// it is still an empty cell, sees no neighbour, and meshes plainly. When
	// the fine one shows up, nothing goes back to tell the coarse one it now
	// has a seam to close -- and the hole stays until that chunk's own band
	// happens to change, which on a planet you are standing still on is never.
	// The same thing happens the other way when a chunk refines: its coarser
	// neighbours were right until it moved.
	//
	// Restitching is one level deep and never changes a neighbour's stride, so
	// it cannot cascade -- a neighbour remeshed at the stride it already had
	// stales nobody in turn.
	void MeshChunkStitched(const glm::ivec3& chunk, int stride)
	{
		MeshChunk(chunk, stride);

		static const glm::ivec3 s_FaceDir[6] =
		{
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 },
			{  0,  0,  1 }, {  0,  0, -1 },
		};

		for (int f = 0; f < 6; f++)
		{
			auto it = m_Chunks.find(Key(chunk + s_FaceDir[f]));

			if (it != m_Chunks.end() && it->second.Stride > stride)
				MeshChunk(it->second.Coord, it->second.Stride);
		}
	}

	// The geometry alone, with nothing stored. Separate from `MeshChunk`
	// because a seam is a property of the triangles and can be counted without
	// uploading anything.
	void BuildChunkMesh(const glm::ivec3& chunk, int stride, Egss::MeshData& data) const
	{
		glm::ivec3 min, max;
		m_Field->ChunkRange(chunk, min, max);

		// **Every vertex measured from this chunk's own lattice origin.**
		// Nothing in the mesh is then further than a chunk diagonal from zero,
		// so a float holds it to a fraction of a millimetre whatever the
		// planet's radius is. Where the chunk actually is lives in
		// `Chunk::Origin`, in double, and is applied once per draw.
		const glm::ivec3 about = ChunkLattice(chunk);

		static const glm::ivec3 s_FaceDir[6] =
		{
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 },
			{  0,  0,  1 }, {  0,  0, -1 },
		};

		unsigned int boundaryMask = 0;
		int ratio = 1;
		int markedFaces = 0;

		for (int f = 0; f < 6; f++)
		{
			auto it = m_Chunks.find(Key(chunk + s_FaceDir[f]));

			if (it == m_Chunks.end())
				continue;

			int neighbourStride = it->second.Stride;

			if (neighbourStride < stride && stride % neighbourStride == 0)
			{
				boundaryMask |= (1u << f);
				ratio = stride / neighbourStride;
				markedFaces++;
			}
		}

		if (markedFaces == 1)
		{
			int face = 0;
			while (!(boundaryMask & (1u << face)))
				face++;

			glm::ivec3 interiorMin = min, interiorMax = max;
			glm::ivec3 layerMin = min, layerMax = max;

			switch (face)
			{
				case Egss::VoxelTransition::PosX: interiorMax.x -= stride; layerMin.x = interiorMax.x; break;
				case Egss::VoxelTransition::NegX: interiorMin.x += stride; layerMax.x = interiorMin.x; break;
				case Egss::VoxelTransition::PosY: interiorMax.y -= stride; layerMin.y = interiorMax.y; break;
				case Egss::VoxelTransition::NegY: interiorMin.y += stride; layerMax.y = interiorMin.y; break;
				case Egss::VoxelTransition::PosZ: interiorMax.z -= stride; layerMin.z = interiorMax.z; break;
				case Egss::VoxelTransition::NegZ: interiorMin.z += stride; layerMax.z = interiorMin.z; break;
			}

			data = Egss::MarchingTetrahedra::Mesh(*m_Field, interiorMin, interiorMax,
				stride, &about);
			Egss::VoxelTransition::MeshBoundaryLayer(*m_Field, layerMin, layerMax,
				stride, boundaryMask, ratio, data, &about);
		}
		else
		{
			data = Egss::MarchingTetrahedra::Mesh(*m_Field, min, max, stride, &about);
		}
	}

	// Remeshes up to `budget` chunks whose band no longer matches their mesh.
	// Budgeted for the same reason streaming is: marching a chunk is the cost,
	// and doing every stale one in a single step is the spike the budget
	// exists to prevent.
	int UpdateLod(const glm::dvec3& focus, int budget)
	{
		if (!m_Lod || budget <= 0)
			return 0;

		// Collected before anything is remeshed: `MeshChunk` can erase its own
		// entry when a coarser lattice finds no surface at all, and that
		// invalidates an iterator standing on it.
		std::vector<std::pair<glm::ivec3, int>> work;

		for (const auto& [key, chunk] : m_Chunks)
		{
			if ((int)work.size() >= budget)
				break;

			int want = DesiredStride((float)glm::length(chunk.Centre - focus), chunk.Stride);

			if (want != chunk.Stride)
				work.push_back({ chunk.Coord, want });
		}

		for (const auto& [coord, stride] : work)
			MeshChunkStitched(coord, stride);

		m_LodRemeshes = (int)work.size();

		return m_LodRemeshes;
	}

	int StreamAround(const glm::dvec3& focus, double radius, float budget)
	{
		float spent = 0.0f;
		glm::ivec3 count = m_Field->ChunkCount();
		glm::dvec3 local = focus - glm::dvec3(m_Field->Origin());

		glm::ivec3 centre = glm::ivec3(glm::floor(local / (double)m_ChunkWorld));
		int reach = (int)std::ceil(radius / (double)m_ChunkWorld) + 1;


		int meshed = 0;

		// **Meshing spends the same budget, and it goes first.**
		//
		// It used to have its own, floored at five chunks a step, so a caller
		// who asked for one chunk of work got one fill *and* five
		// marching-tetrahedra passes over 4,913 cells each -- which is why a
		// budget of one still cost six milliseconds. Marching a chunk costs
		// about what generating one does, so it counts the same.
		//
		// First, and on half the budget, because otherwise the two starve each
		// other: filling first means a budget of one is always spent on a fill,
		// the dirty queue is never drained, and terrain is generated and never
		// drawn. Turning a chunk that already exists into something visible
		// beats making another one that is not.
		for (auto it = m_Dirty.begin();
			it != m_Dirty.end() && spent < budget * 0.5f; )
		{
			size_t key = *it;
			glm::ivec3 chunk = Unkey(key);

			// **Wait for the high neighbours rather than mesh and repair.**
			//
			// Re-meshing a chunk once its neighbour arrives (below) does fix
			// the seam, but not before it has been on screen: filling runs
			// four chunks a step and the re-mesh queue drains five, so a
			// backlog of a couple of hundred takes seconds to clear. What you
			// see meanwhile is not a crack -- it is a *wall*, because the
			// unfilled neighbour reads as empty space and the mesher
			// faithfully closes the surface against it. Flying down to a
			// planet, that is black slabs standing up out of the ground and
			// then vanishing.
			//
			// Holding the mesh back costs a chunk of terrain at the streaming
			// edge, where the horizon sphere is drawn anyway. Nothing wrong is
			// ever drawn.
			if (!HighNeighboursFilled(chunk))
			{
				++it;
				continue;
			}

			it = m_Dirty.erase(it);

			// An edit or a late neighbour does not move a chunk between bands,
			// so a chunk that already has a mesh is remade on the lattice it
			// already had. One being meshed for the first time takes the band
			// outright -- there is no previous stride for the hysteresis to
			// hold it against, and being born at the right detail is what
			// keeps the level-of-detail pass down to a trickle.
			auto existing = m_Chunks.find(key);

			int stride = (existing != m_Chunks.end())
				? existing->second.Stride
				: BandFor((float)glm::length(ChunkCentreFixed(chunk) - focus));

			MeshChunkStitched(chunk, stride);

			spent += 1.0f;
			meshed++;
		}

		// Sorted by distance once per reach, so the ground under the camera
		// arrives before the horizon does.
		if (reach != m_SortedReach)
		{
			m_Offsets.clear();

			for (int z = -reach; z <= reach; z++)
				for (int y = -reach; y <= reach; y++)
					for (int x = -reach; x <= reach; x++)
						m_Offsets.push_back({ x, y, z });

			std::sort(m_Offsets.begin(), m_Offsets.end(),
				[](const glm::ivec3& a, const glm::ivec3& b)
				{
					return (a.x * a.x + a.y * a.y + a.z * a.z)
						< (b.x * b.x + b.y * b.y + b.z * b.z);
				});

			m_SortedReach = reach;
		}

		// **The scan resumes where it left off.**
		//
		// `m_Offsets` is sorted by distance and filling proceeds outward, so
		// once a prefix of it is filled that prefix stays filled while the
		// focus chunk does not move -- chunks are only ever released at three
		// times the load radius, which is outside this reach entirely. Walking
		// all 50,653 of them every step to rediscover that cost **2.9 ms a
		// step forever**, which is a floor no budget could get under: the demo
		// converged to a stable three milliseconds of finding nothing.
		if (centre != m_ScanCentre)
		{
			m_ScanCentre = centre;
			m_ScanFrom = 0;
		}

		for (size_t at = m_ScanFrom; at < m_Offsets.size(); at++)
		{
			const glm::ivec3& offset = m_Offsets[at];

			if (spent >= budget)
				break;

			glm::ivec3 chunk = centre + offset;

			if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
				|| chunk.x >= count.x || chunk.y >= count.y || chunk.z >= count.z)
				continue;

			size_t key = Key(chunk);

			if (m_Filled.count(key))
			{
				// Everything before the first unfilled entry is done with, and
				// saying so is what makes the next scan start there.
				if (at == m_ScanFrom)
					m_ScanFrom = at + 1;

				continue;
			}

			glm::dvec3 chunkCentre = ChunkCentreFixed(chunk);

			if (glm::length(chunkCentre - focus) > radius)
				continue;

			m_Filled.insert(key);

			// **An edited chunk is read back, never regenerated.**
			//
			// Before the fill's own two paths, because both of them would
			// overwrite the hole: the shell test would decide a chunk full of
			// tunnel was solid rock, and the generator would put the rock
			// back. This is the whole reason the file exists -- eviction here
			// is regeneration, so without it a hole healed the moment you
			// walked far enough away from it.
			if (m_Edited.count(key) && m_Edits.Read(chunk, m_Scratch)
				&& m_Field->LoadChunk(chunk, m_Scratch.data(), m_Scratch.size()))
			{
				m_Dirty.insert(key);

				const glm::ivec3* offsets = HighNeighbourOffsets();

				for (int i = 0; i < 7; i++)
				{
					glm::ivec3 lower = chunk - offsets[i];

					if (lower.x < 0 || lower.y < 0 || lower.z < 0)
						continue;

					if (m_Chunks.count(Key(lower)))
						m_Dirty.insert(Key(lower));
				}

				spent += 1.0f;

				continue;
			}

			// **From the site cache, before the shell test and the
			// generator.** Same shape as the edits path above and for the
			// same reason: what is stored is the answer, so nothing below
			// this needs to run.
			if (m_Prefilling && m_Site.Read(chunk, m_Scratch)
				&& m_Field->LoadChunk(chunk, m_Scratch.data(), m_Scratch.size()))
			{
				m_Dirty.insert(key);

				const glm::ivec3* cached = HighNeighbourOffsets();

				for (int i = 0; i < 7; i++)
				{
					glm::ivec3 lower = chunk - cached[i];

					if (lower.x < 0 || lower.y < 0 || lower.z < 0)
						continue;

					if (m_Chunks.count(Key(lower)))
						m_Dirty.insert(Key(lower));
				}

				spent += 1.0f;

				continue;
			}

			// **The shell test.** Everything outside the band is either solid
			// rock or empty sky, and a chunk of one uniform value costs one
			// float in VoxelField3D rather than 4,096.
			//
			// **But it still has to be given that value.** Leaving it
			// unallocated is not "solid rock" -- an unallocated chunk reads
			// `Far`, which is *air*, so the deep interior of the planet was
			// empty as far as the mesher was concerned. A surface chunk whose
			// +x, +y or +z neighbour happened to point inward then meshed its
			// own rock against that air and closed the surface with a wall,
			// which is where the black slabs standing out of the ground came
			// from. They were on one hemisphere only, which is the tell: the
			// three neighbours a mesh reads are all in the positive direction.
			//
			// A constant generator costs the loop but not the noise, and
			// collapses to the same one float.
			if (!TouchesSurface(chunkCentre))
			{
				float uniform = glm::length(chunkCentre) < (double)m_Settings.Radius
					? -Egss::VoxelField3D::Far : Egss::VoxelField3D::Far;

				// Set, not generated. The old form ran the constant through
				// `FillChunk`, which allocates a 16 KB scratch buffer and
				// writes 4,096 copies of it -- and on a planet these outnumber
				// the chunks that hold anything three to one, so that was most
				// of the streaming cost while the ground filled in.
				m_Field->SetUniform(chunk, uniform, 1);

				spent += s_RejectCost;

				continue;
			}

			FillChunkFast(chunk);

			if (m_Prefilling)
			{
				m_Field->SaveChunk(chunk, m_Scratch);
				m_Site.Write(chunk, m_Scratch);
			}

			// **Filling a chunk stales its low neighbours' meshes.**
			// `ChunkRange` includes one plane past the chunk's own cells, so a
			// mesh reads the *first* plane of the chunks above it in x, y and
			// z. Mesh a chunk before those exist and it meshes against empty
			// space -- which is what put a black grid of cracks across the
			// first planet, one line per chunk boundary, looking for all the
			// world like a mesher bug rather than an ordering one.
			m_Dirty.insert(key);

			const glm::ivec3* offsets = HighNeighbourOffsets();

			for (int i = 0; i < 7; i++)
			{
				glm::ivec3 lower = chunk - offsets[i];

				if (lower.x < 0 || lower.y < 0 || lower.z < 0)
					continue;

				size_t lowerKey = Key(lower);

				// Only ones already built: a chunk not yet filled will mesh
				// against this one when its own turn comes.
				if (m_Chunks.count(lowerKey))
					m_Dirty.insert(lowerKey);
			}

			spent += 1.0f;
		}

		// **Last, and on whatever is left.** New chunks are born in the right
		// band, so this pass only ever handles chunks the *player* moved past
		// -- a trickle, not a backlog. Starving it while a planet streams in is
		// the right priority anyway: terrain that is on screen at the wrong
		// detail beats terrain that is not on screen yet.
		UpdateLod(focus, (int)glm::max(budget - spent, 0.0f));

		return meshed;
	}

	// Drops meshes further than `radius` from `focus`, so a walk round a planet
	// does not accumulate the whole surface in memory. The *field* keeps them:
	// regenerating a chunk is a density evaluation per voxel, and remembering
	// it is six bytes when it is uniform.
	// `ChunkRange` reads one plane past the chunk into each high neighbour, so
	// those three have to exist before a mesh means anything. A neighbour
	// outside the field is not a neighbour -- the accessors clamp there, and
	// waiting for it would leave the lattice's outer shell unmeshed forever.
	// The seven chunks a mesh reads into: not three.
	//
	// **`ChunkRange` adds a plane in every axis at once**, so the lattice a
	// chunk is marched on includes the point at (+16, +16, +16) -- which
	// belongs to the *diagonal* neighbour, not to any of the three the code
	// used to name. Waiting for three and staling three is right along the
	// faces and wrong along the edges and at the corner, and the corner is
	// shared by four chunks nobody was telling.
	//
	// Measured, on a landed Earth: 422 of 963 stored meshes did not match what
	// the mesher would produce from the field as it finally stood. With all
	// seven it is zero.
	static const glm::ivec3* HighNeighbourOffsets()
	{
		static const glm::ivec3 offsets[7] =
		{
			{ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
			{ 1, 1, 0 }, { 1, 0, 1 }, { 0, 1, 1 },
			{ 1, 1, 1 },
		};

		return offsets;
	}

	bool HighNeighboursFilled(const glm::ivec3& chunk) const
	{
		glm::ivec3 count = m_Field->ChunkCount();
		const glm::ivec3* offsets = HighNeighbourOffsets();

		for (int i = 0; i < 7; i++)
		{
			glm::ivec3 higher = chunk + offsets[i];

			if (higher.x >= count.x || higher.y >= count.y || higher.z >= count.z)
				continue;

			if (!m_Filled.count(Key(higher)))
				return false;
		}

		return true;
	}

	void EvictBeyond(const glm::dvec3& focus, double radius)
	{
		for (auto it = m_Chunks.begin(); it != m_Chunks.end(); )
		{
			if (glm::length(it->second.Centre - focus) > radius)
				it = m_Chunks.erase(it);
			else
				++it;
		}
	}

	// **And give the voxels back, not only the meshes.**
	//
	// Dropping a mesh leaves the chunk it was built from filled, which on a
	// field a few hundred metres across is a rounding error and on a planet is
	// unbounded: walk far enough and every chunk you have ever passed is still
	// resident. The mesh radius is the smaller one, so this runs wider -- a
	// chunk just out of sight keeps its voxels and costs one re-mesh to come
	// back, where one just outside this radius costs a re-fill.
	void ReleaseBeyond(const glm::dvec3& focus, double radius)
	{
		for (auto it = m_Filled.begin(); it != m_Filled.end(); )
		{
			glm::ivec3 chunk = Unkey(*it);

			if (glm::length(ChunkCentreFixed(chunk) - focus) > radius)
			{
				m_Field->ClearChunk(chunk);
				it = m_Filled.erase(it);

				// **Releasing invalidates the scan's watermark.**
				//
				// `m_ScanFrom` is how far along the distance-sorted offsets
				// everything is known to be filled, and it only ever advances.
				// Taking a chunk back out of `m_Filled` without saying so
				// leaves the scan starting past it forever: the chunk is
				// released, never revisited, and reads as air. Normal play
				// hides it -- release happens at three times the load radius,
				// which you only reach by moving, and moving resets this
				// anyway -- but a test that released without moving found the
				// ground had stopped existing.
				m_ScanFrom = 0;
			}
			else
			{
				++it;
			}
		}
	}

	struct Chunk
	{
		std::shared_ptr<Egss::Mesh> MeshPtr;

		// **In the planet's frame, in double, and the mesh is not.** The
		// vertices in `MeshPtr` are measured from this chunk's own lattice
		// origin and are never more than 24 m from it; this is where that
		// origin is. Keeping the two apart is the whole point -- one is small
		// enough for a float and the other is not.
		glm::dvec3 Centre = glm::dvec3(0.0);
		glm::dvec3 Origin = glm::dvec3(0.0);

		size_t Triangles = 0;

		// The lattice this mesh was marched on, and the chunk index it came
		// from. The stride is here rather than derived from the distance
		// because the distance moves every step and the mesh does not: what a
		// neighbour needs to know is what this chunk *is*, not what it ought
		// to be.
		int Stride = 1;
		glm::ivec3 Coord = glm::ivec3(0);

		// Placed with the mesh and thrown away with it, because they are a
		// function of the same chunk index and cost nothing to reproduce.
		std::vector<Plant> Plants;
	};

	const std::unordered_map<size_t, Chunk>& Chunks() const { return m_Chunks; }

	bool HasChunkMesh(const glm::ivec3& chunk) const
	{
		return m_Chunks.count(Key(chunk)) != 0;
	}

	const Chunk* ChunkMesh(const glm::ivec3& chunk) const
	{
		auto it = m_Chunks.find(Key(chunk));

		return it == m_Chunks.end() ? nullptr : &it->second;
	}

	size_t TriangleCount() const
	{
		size_t total = 0;
		for (const auto& [key, chunk] : m_Chunks)
			total += chunk.Triangles;

		return total;
	}

	size_t FilledChunks() const { return m_Filled.size(); }
	size_t MeshedChunks() const { return m_Chunks.size(); }

	// Chunks and triangles per band, for the panel. Index 0 is stride 1, 1 is
	// stride 2, 2 is stride 4.
	void LodCounts(int outChunks[3], size_t outTriangles[3]) const
	{
		for (int i = 0; i < 3; i++)
		{
			outChunks[i] = 0;
			outTriangles[i] = 0;
		}

		for (const auto& [key, chunk] : m_Chunks)
		{
			int slot = chunk.Stride == 1 ? 0 : (chunk.Stride == 2 ? 1 : 2);

			outChunks[slot]++;
			outTriangles[slot] += chunk.Triangles;
		}
	}

	// **Level of detail, in metres from the focus.** Off by default: a planet
	// that has never been landed on draws its terrain from orbit through the
	// colour map, and the bands would only ever be measuring a distance
	// nothing is standing at.
	void SetLod(bool enabled, float near, float far, float hysteresis)
	{
		m_Lod = enabled;
		m_LodNear = near;
		m_LodFar = far;
		m_LodHysteresis = hysteresis;
	}

	int LodRemeshes() const { return m_LodRemeshes; }

	// Where the ground is in a direction, by bisection on the density -- used
	// to put a camera or a lander on the surface rather than inside it.
	float SurfaceRadius(const glm::vec3& direction) const
	{
		glm::vec3 unit = glm::normalize(direction);

		float low = m_Settings.Radius - m_Settings.Amplitude - 2.0f;
		float high = m_Settings.Radius + m_Settings.Amplitude + 2.0f;

		// The density is monotone across the shell for this generator -- one
		// crossing, so bisection cannot land on the wrong root.
		for (int i = 0; i < 40; i++)
		{
			float middle = 0.5f * (low + high);

			if (Density(unit * middle) < 0.0f)
				low = middle;
			else
				high = middle;
		}

		return 0.5f * (low + high);
	}

private:
	// **Twenty-one bits an axis, not sixteen.**
	//
	// Sixteen tops out at 65,535 chunks, which is a body 25 km across at
	// 1.5 m voxels -- fine for the 360 m planet this started as and silently
	// catastrophic above it, because the mask does not overflow, it *wraps*:
	// chunk 65,536 and chunk 0 get the same key, so a streamer walking a real
	// planet would find the far side of the world already filled. Twenty-one
	// bits reaches 2,097,152, which is 3,145 km of chunks -- Earth's 531,000
	// with room to spare -- and still fits three of them in a 64-bit word.
	static constexpr int KeyBits = 21;
	static constexpr size_t KeyMask = (size_t(1) << KeyBits) - 1;

	static size_t Key(const glm::ivec3& chunk)
	{
		return ((size_t)chunk.x & KeyMask) << (2 * KeyBits)
			| ((size_t)chunk.y & KeyMask) << KeyBits
			| ((size_t)chunk.z & KeyMask);
	}

	static glm::ivec3 Unkey(size_t key)
	{
		return glm::ivec3((int)((key >> (2 * KeyBits)) & KeyMask),
			(int)((key >> KeyBits) & KeyMask), (int)(key & KeyMask));
	}

	// **The chunk's own lattice origin, and the position of it, in double.**
	//
	// The lattice index is exact by construction -- it is an integer. What
	// used to be lossy was turning it into metres: at a real planet's radius
	// both terms of `Origin + index * VoxelSize` are about 6.4e6 and a float
	// carries 24 bits, so the answer landed on a 0.76 m grid. Done in double
	// the same expression has 53 bits and is exact to a nanometre, and the
	// small numbers it feeds -- a chunk's offset from the camera, a plant's
	// offset from its chunk -- stay small enough to be floats again.
	glm::ivec3 ChunkLattice(const glm::ivec3& chunk) const
	{
		return chunk * Egss::VoxelField3D::ChunkSize;
	}

	glm::dvec3 ChunkOriginFixed(const glm::ivec3& chunk) const
	{
		return glm::dvec3(m_Field->Origin())
			+ glm::dvec3(ChunkLattice(chunk)) * (double)m_Settings.VoxelSize;
	}

	glm::dvec3 ChunkCentreFixed(const glm::ivec3& chunk) const
	{
		return ChunkOriginFixed(chunk) + glm::dvec3(m_ChunkWorld * 0.5f);
	}

	glm::vec3 ChunkCentre(const glm::ivec3& chunk) const
	{
		return m_Field->Origin()
			+ (glm::vec3(chunk) + glm::vec3(0.5f)) * m_ChunkWorld;
	}

	// **Where the ground is here, not where the ground is on average.**
	//
	// This used to test the chunk against the *mean* radius, with the whole
	// relief range as its band -- so on a planet whose hills are 380 m it
	// accepted a shell 800 m thick and ran the full noise generator over every
	// chunk in it. Measured while landing: **7,200 chunks filled to produce
	// 808 that had any surface in them**, and 150 ms a step in Debug against a
	// 16.7 ms budget. Nine chunks of solid rock or open sky generated in full
	// for every one that was worth generating.
	//
	// The band is a property of the whole planet; the surface is a property of
	// the direction. Sampling `Relief` at the chunk's own corners costs nine
	// evaluations -- about 120 noise calls -- against the 53,000 a fill costs,
	// so rejecting a chunk this way is 450 times cheaper than filling it and
	// finding out.
	//
	// The margin is a chunk width past the corner samples. The corners bound
	// the relief *between* them but not a bump between two of them, and the
	// relief's slope is under one, so half a chunk of extra depth covers any
	// extremum they stepped over; doubling that costs a few percent of the
	// chunks and buys the certainty. The check that it is enough is that a
	// capture taken with this in place is byte-identical to one taken without,
	// because the only chunks it may skip are chunks with nothing in them.
	// --- Filling a chunk, given that the surface is a height field ---------
	//
	// **`Relief` is a function of the direction, and a chunk is a cube.**
	//
	// `Density(p)` is `|p| - Radius - Relief(p/|p|)`. The first two terms are
	// arithmetic; the third is thirteen octaves of value noise, eight hashes
	// each, and it is *all* of the cost -- measured during a landing at 1.74 ms
	// for a chunk's 4,096 voxels against 0.146 ms to march the same chunk into
	// triangles. Ninety-three per cent of the time it takes to arrive on a
	// planet is spent evaluating that one function.
	//
	// And it is a function of two variables being sampled over three. Every
	// voxel in a radial column has the same direction and therefore the same
	// relief, and the cube holds sixteen of them; the columns themselves are
	// only a few metres apart while the finest octave in the sum is 56 m
	// across. The volume is being asked to answer a question about a surface.
	//
	// So the relief over the chunk is sampled onto a small grid in the tangent
	// plane and read back bilinearly. The parameterisation is gnomonic --
	// `u = (p.t)/(p.n)`, `v = (p.b)/(p.n)` -- which has two properties worth
	// the name: it is scale-invariant, so a voxel's `(u, v)` costs three dot
	// products and two divides and no normalise at all, and it inverts exactly
	// as `normalize(n + t*u + b*v)`, so the grid is built at precisely the
	// directions it is later read at.
	//
	// **The grid's spacing comes from the finest wavelength in the sum**, not
	// from a constant. Interpolating a sinusoid of amplitude `A` and
	// wavelength `L` on a spacing `h` is wrong by at most `A/2 * (pi h / L)^2`,
	// which for sixteen samples a wavelength is `A/2 * 0.039` -- a centimetre
	// on the finest octave here, and less on every coarser one, since amplitude
	// falls as fast as wavelength does. Everything else in the sum is smoother
	// than the term the spacing was chosen for. `ReliefError` measures what it
	// actually costs rather than trusting that.
	//
	// **Nothing is double-valued.** `FillOneChunk` writes a chunk's own cells
	// and no others, so each lattice point takes its value from exactly one
	// grid -- two neighbouring chunks cannot disagree about a shared plane,
	// because there is no shared plane to disagree about. That is the whole
	// reason this is safe to do per chunk instead of globally.
	struct ReliefPatch
	{
		glm::vec3 Tangent { 1.0f, 0.0f, 0.0f };
		glm::vec3 Bitangent { 0.0f, 1.0f, 0.0f };
		glm::vec3 Normal { 0.0f, 0.0f, 1.0f };

		// The chunk centre's own dot products, in double: `p` is a
		// planet-sized float and `p.t` is a difference of large numbers, so
		// the small part is formed from the offset and the large part is
		// carried alongside it rather than through it.
		double CentreTangent = 0.0, CentreBitangent = 0.0, CentreNormal = 1.0;
		glm::vec3 Centre { 0.0f };

		float Low = 0.0f;      // the (u, v) the grid starts at
		float InverseStep = 1.0f;
		int Side = 2;

		std::vector<float> Value;

		float At(float u, float v) const
		{
			float fu = (u - Low) * InverseStep;
			float fv = (v - Low) * InverseStep;

			// Clamped rather than trusted. The bound below is exact for a
			// point inside the chunk, but the last chunk of the lattice
			// repeats its border and a clamped position can sit outside the
			// cube the grid was sized for.
			fu = glm::clamp(fu, 0.0f, (float)(Side - 1) - 1e-4f);
			fv = glm::clamp(fv, 0.0f, (float)(Side - 1) - 1e-4f);

			int iu = (int)fu, iv = (int)fv;
			float tu = fu - (float)iu, tv = fv - (float)iv;

			const float* row = &Value[(size_t)iv * Side + iu];

			float a = glm::mix(row[0], row[1], tu);
			float b = glm::mix(row[Side], row[Side + 1], tu);

			return glm::mix(a, b, tv);
		}
	};

	// **How far apart the grid's samples may be, and it is not a wavelength.**
	//
	// Bilinear interpolation of a sinusoid of amplitude `A` and wavelength `L`
	// on a spacing `h` is wrong by at most `A/2 * (pi h / L)^2`. The first
	// version of this took the shortest wavelength in the sum and put sixteen
	// samples across it, which is right only while every term has the same
	// amplitude. It does not: the error goes as `A / L^2`, and the landscape
	// layer's finest octave is 22 m of amplitude at a 131 m wavelength against
	// the planetary spectrum's 0.6 m at 56 m -- **six times the curvature at
	// twice the wavelength**. Measured, the wavelength rule gave 3.51 m
	// spacing and 0.26 m of error, which is a sixth of a voxel.
	//
	// So the spacing is solved for instead. Take the largest `A / L^2` any
	// term contributes, and invert the error bound for the `h` that meets a
	// tolerance:
	//
	//     h = (1/pi) * sqrt(2 * tolerance / max(A / L^2))
	//
	// The tolerance is two per cent of a voxel, which is the right unit for
	// it: this is an approximation to a field that is about to be sampled onto
	// a lattice of that spacing and marched into triangles, and there is no
	// sense in being much more accurate than the thing being built.
	float ReliefDetail() const
	{
		float curvature = 0.0f;

		// The planetary ridged sum. `1 - |n|` has the same amplitude as `n`,
		// and the octave weights sum to just under two.
		if (m_Settings.Octaves > 0)
		{
			float weight = std::pow(0.5f, (float)(m_Settings.Octaves - 1));
			float length = m_Settings.FeatureSize * weight;
			float height = m_Settings.Amplitude * weight * 0.5f;

			curvature = glm::max(curvature, height / glm::max(length * length, 1e-6f));
		}

		// The roughness layer: three octaves, weights summing to 1.75, and the
		// whole thing scaled by a half.
		if (m_Settings.Roughness > 0.0f && m_Settings.RoughnessSize > 0.0f)
		{
			float length = m_Settings.RoughnessSize * 0.25f;
			float height = m_Settings.Roughness * 0.5f * (0.25f / 1.75f);

			curvature = glm::max(curvature, height / glm::max(length * length, 1e-6f));
		}

		// The landscape layer, and the factor of two is the squaring: `ridge`
		// is squared before it is scaled, which doubles the slope of the term
		// wherever `ridge` is near one -- that is what sharpens the peaks, and
		// it sharpens the error with them.
		if (m_Settings.Landscape > 0.0f && m_Settings.LandscapeSize > 0.0f
			&& m_Settings.LandscapeOctaves > 0)
		{
			float weight = std::pow(0.5f, (float)(m_Settings.LandscapeOctaves - 1));
			float length = m_Settings.LandscapeSize * weight;
			float height = m_Settings.Landscape * weight * 2.0f;

			curvature = glm::max(curvature, height / glm::max(length * length, 1e-6f));
		}

		if (curvature <= 0.0f)
			return glm::max(m_Settings.VoxelSize, 1.0f);

		float tolerance = 0.02f * glm::max(m_Settings.VoxelSize, 0.1f);

		return 0.3183099f * std::sqrt(2.0f * tolerance / curvature);
	}

	// Builds the patch for one chunk. Returns false when the grid would cost
	// more than the voxels it is meant to save, which is the honest answer for
	// a body whose finest feature is smaller than its own voxels.
	bool BuildReliefPatch(const glm::dvec3& centre, ReliefPatch& patch) const
	{
		double distance = glm::length(centre);

		if (distance < 1e-3)
			return false;

		glm::vec3 n = glm::vec3(centre / distance);

		// Any axis that is not nearly `n` gives a stable tangent.
		glm::vec3 pick = std::abs(n.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

		patch.Normal = n;
		patch.Tangent = glm::normalize(glm::cross(pick, n));
		patch.Bitangent = glm::cross(n, patch.Tangent);

		patch.Centre = glm::vec3(centre);
		patch.CentreTangent = glm::dot(centre, glm::dvec3(patch.Tangent));
		patch.CentreBitangent = glm::dot(centre, glm::dvec3(patch.Bitangent));
		patch.CentreNormal = glm::dot(centre, glm::dvec3(patch.Normal));

		// **The bound is exact.** For `p = C + e` with `|e| <= H`, the
		// tangent component of `p` is the tangent component of `e`, and the
		// normal component is at least `|C| - H`. So `|u| <= H / (|C| - H)`,
		// and there is no need to guess a margin or to sample the corners.
		double half = (double)m_ChunkWorld * 0.8660254037844386; // sqrt(3)/2

		if (distance <= half * 2.0)
			return false;

		float span = (float)(half / (distance - half));

		// Angular spacing that lands `ReliefDetail()` metres apart on the
		// ground: an angle `a` covers `a * Radius` metres at this radius.
		float step = ReliefDetail() / glm::max(m_Settings.Radius, 1.0f);

		int side = (int)std::ceil(2.0f * span / step) + 1;

		// Below three there is nothing to interpolate; above forty-eight the
		// grid has more samples than the chunk has voxels in a plane and the
		// saving has gone.
		if (side > 48)
			return false;

		side = glm::max(side, 3);

		patch.Side = side;
		patch.Low = -span;
		patch.InverseStep = (float)(side - 1) / (2.0f * span);

		patch.Value.resize((size_t)side * side);

		float grid = 2.0f * span / (float)(side - 1);

		for (int j = 0; j < side; j++)
		{
			float v = -span + grid * (float)j;

			for (int i = 0; i < side; i++)
			{
				float u = -span + grid * (float)i;

				// The exact inverse of the gnomonic map the voxels use.
				glm::vec3 direction = glm::normalize(
					n + patch.Tangent * u + patch.Bitangent * v);

				patch.Value[(size_t)j * side + i] = Relief(direction);
			}
		}

		return true;
	}

public:
	// **What the interpolation actually costs, in metres of ground.**
	//
	// The estimate above says a centimetre. This measures it: for a spread of
	// chunks around a place, every voxel's interpolated density against the
	// generator's own answer. Reported as a distance because that is what it
	// is -- the surface moves by this much, and a voxel here is 1.5 m.
	void ReportReliefError(const glm::dvec3& focus, int chunks = 64) const
	{
		glm::dvec3 local = focus - glm::dvec3(m_Field->Origin());
		glm::ivec3 centre = glm::ivec3(glm::floor(local / (double)m_ChunkWorld));

		double worst = 0.0, sum = 0.0;
		long samples = 0, patched = 0;

		for (int i = 0; i < chunks; i++)
		{
			// A spread rather than a block: a block of chunks is one patch of
			// ground, and one patch of ground can be flat.
			glm::ivec3 chunk = centre + glm::ivec3(
				(i * 7) % 13 - 6, (i * 5) % 11 - 5, (i * 3) % 17 - 8);

			glm::dvec3 at = ChunkCentreFixed(chunk);

			if (!TouchesSurface(at))
				continue;

			ReliefPatch patch;

			if (!BuildReliefPatch(at, patch))
				continue;

			patched++;

			// The chunk's own voxels, every fourth one in each axis: the error
			// is smooth over a grid cell, so sixty-four points a chunk find
			// the peak of it without paying for four thousand.
			for (int k = 0; k < Egss::VoxelField3D::ChunkSize; k += 4)
			for (int j = 0; j < Egss::VoxelField3D::ChunkSize; j += 4)
			for (int i2 = 0; i2 < Egss::VoxelField3D::ChunkSize; i2 += 4)
			{
				glm::dvec3 p = ChunkOriginFixed(chunk)
					+ glm::dvec3(i2, j, k) * (double)m_Settings.VoxelSize;

				glm::vec3 e = glm::vec3(p) - patch.Centre;

				double normal = patch.CentreNormal
					+ (double)glm::dot(e, patch.Normal);

				if (normal < 1e-6)
					continue;

				float u = (float)((patch.CentreTangent
					+ (double)glm::dot(e, patch.Tangent)) / normal);
				float v = (float)((patch.CentreBitangent
					+ (double)glm::dot(e, patch.Bitangent)) / normal);

				double length = glm::length(p);

				if (length < 1e-3)
					continue;

				float exact = Relief(glm::vec3(p / length));
				double error = std::abs((double)patch.At(u, v) - (double)exact);

				worst = glm::max(worst, error);
				sum += error;
				samples++;
			}
		}

		EGSS_TRACE("Relief patch: {0} chunks, grid {1:.2f} m a sample, "
			"error mean {2:.4f} m worst {3:.4f} m over {4} voxels "
			"({5:.2f}% of a voxel)",
			patched, ReliefDetail(), samples ? sum / (double)samples : 0.0, worst,
			samples, 100.0 * worst / (double)m_Settings.VoxelSize);
	}

private:
	// Fills one chunk through the patch, falling back to the exact generator
	// when a patch would not pay for itself.
	void FillChunkFast(const glm::ivec3& chunk)
	{
		ReliefPatch patch;

		if (!BuildReliefPatch(ChunkCentreFixed(chunk), patch))
		{
			m_Field->FillChunk(chunk,
				[this](const glm::vec3& p) { return Density(p); }, 1);

			return;
		}

		float radius = m_Settings.Radius;

		m_Field->FillChunk(chunk, [&patch, radius](const glm::vec3& p)
		{
			// The offset is formed in float and is exact: `p` and the chunk
			// centre are within a chunk of each other, so the subtraction
			// itself loses nothing. The large half of each dot product was
			// taken in double when the patch was built.
			glm::vec3 e = p - patch.Centre;

			double normal = patch.CentreNormal + (double)glm::dot(e, patch.Normal);

			if (normal < 1e-6)
				return -radius;

			float u = (float)((patch.CentreTangent
				+ (double)glm::dot(e, patch.Tangent)) / normal);
			float v = (float)((patch.CentreBitangent
				+ (double)glm::dot(e, patch.Bitangent)) / normal);

			return glm::length(p) - radius - patch.At(u, v);
		}, 1);
	}

	bool TouchesSurface(const glm::dvec3& chunkCentre) const
	{
		// The distance in double, the *direction* in float. A planet's radius
		// does not fit a float's mantissa with metres to spare; a unit vector
		// always does, and it is the direction the relief is a function of.
		double distance = glm::length(chunkCentre);

		// The centre of the planet has no direction to ask about, and is solid
		// rock in every case that matters.
		if (distance < (double)m_ChunkWorld)
			return false;

		// Cheap reject first: nothing outside the whole planet's relief range
		// can be near the surface, whatever direction it is in.
		float chunkReach = m_ChunkWorld * 0.8660254f;

		if (std::abs(distance - (double)m_Settings.Radius)
			> (double)(ReliefReach() + chunkReach + m_Settings.VoxelSize))
			return false;

		// **Twenty-seven samples, not nine.** Eight corners bound the relief
		// between them and not a bump between two of them, which had to be
		// paid for with a whole chunk of extra margin -- and margin is waste:
		// it accepted 452 empty chunks for every 169 that held any surface. A
		// 3x3x3 grid includes the face and edge midpoints, so the widest
		// unsampled gap is half a chunk instead of a whole one and the margin
		// comes off. Both counts are hundreds of times cheaper than the fill
		// they are avoiding, so the denser one is free.
		float low = 1e30f, high = -1e30f;
		float half = m_ChunkWorld * 0.5f;

		for (int k = 0; k < 3; k++)
		for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
		{
			glm::dvec3 at = chunkCentre + glm::dvec3(i - 1, j - 1, k - 1) * (double)half;

			double length = glm::length(at);

			if (length < 1e-3)
				continue;

			float relief = Relief(glm::vec3(at / length));

			low = glm::min(low, relief);
			high = glm::max(high, relief);
		}

		// The radii the ground occupies over this chunk, against the radii the
		// chunk occupies. They have to overlap for the surface to be in it.
		float margin = chunkReach + m_Settings.VoxelSize;

		return distance - (double)margin <= (double)(m_Settings.Radius + high)
			&& distance + (double)margin >= (double)(m_Settings.Radius + low);
	}


	// 3D value noise. **The cast is inside the multiply on purpose** -- see the
	// 2026-08-17 changelog entry for what `(uint32_t)(x * 374761393)` does to a
	// release build when x overflows an int.
	static float Noise3D(const glm::vec3& p, unsigned int seed)
	{
		glm::vec3 floored = glm::floor(p);
		glm::vec3 f = p - floored;

		// Smoothstep, so the lattice does not show as a grid of creases.
		glm::vec3 t = f * f * (glm::vec3(3.0f) - 2.0f * f);

		int xi = (int)floored.x, yi = (int)floored.y, zi = (int)floored.z;

		float c000 = Hash3D(xi,     yi,     zi,     seed);
		float c100 = Hash3D(xi + 1, yi,     zi,     seed);
		float c010 = Hash3D(xi,     yi + 1, zi,     seed);
		float c110 = Hash3D(xi + 1, yi + 1, zi,     seed);
		float c001 = Hash3D(xi,     yi,     zi + 1, seed);
		float c101 = Hash3D(xi + 1, yi,     zi + 1, seed);
		float c011 = Hash3D(xi,     yi + 1, zi + 1, seed);
		float c111 = Hash3D(xi + 1, yi + 1, zi + 1, seed);

		float x00 = glm::mix(c000, c100, t.x);
		float x10 = glm::mix(c010, c110, t.x);
		float x01 = glm::mix(c001, c101, t.x);
		float x11 = glm::mix(c011, c111, t.x);

		return glm::mix(glm::mix(x00, x10, t.y), glm::mix(x01, x11, t.y), t.z) * 2.0f - 1.0f;
	}

	static float Hash3D(int x, int y, int z, unsigned int seed)
	{
		unsigned int h = seed;

		h ^= (uint32_t)x * 374761393u;
		h ^= (uint32_t)y * 668265263u;
		h ^= (uint32_t)z * 2147483647u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;

		return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
	}

	Settings m_Settings;
	std::shared_ptr<Egss::Texture2D> m_Map;
	std::shared_ptr<Egss::VoxelField3D> m_Field;

	// One grid for the height map and the hydrology both, so a texel means the
	// same patch of ground in each and nothing has to be resampled between
	// them. About 1.5 km of ground a texel at the radius this runs at.
	static constexpr int s_MapWidth = 1024;
	static constexpr int s_MapHeight = 512;

	Hydrology m_Water;

	// Whether each cell has standing water on it. Its own array rather than a
	// channel of `Hydrology`, because it is derived from two of those and is
	// the only one anything outside asks about by name.
	std::vector<float> m_Wet;

	// How arid a basin has to be before it is a salt flat rather than a lake,
	// and what the last derivation measured about them.
	static constexpr float s_LakeMoisture = 0.46f;

	int m_DryBasins = 0;
	float m_BasinSpread = 0.0f;

	// Chunks somebody has dug, and the file they live in.
	std::unordered_set<size_t> m_Edited;
	ChunkCache m_Edits;
	std::vector<unsigned char> m_Scratch;

	std::unordered_map<size_t, Chunk> m_Chunks;

	// Off until something lands: see SetLod.
	bool m_Lod = false;
	float m_LodNear = 120.0f;
	float m_LodFar = 240.0f;
	float m_LodHysteresis = 16.0f;
	int m_LodRemeshes = 0;
	// How far the outward scan got while the focus chunk stayed put.
	glm::ivec3 m_ScanCentre = glm::ivec3(0x7fffffff);
	size_t m_ScanFrom = 0;

	ChunkCache m_Site;
	bool m_Prefilling = false;

	std::unordered_set<size_t> m_Filled;

	// Chunks whose mesh is missing or stale. Meshed on the next stream, so a
	// newly filled neighbour is accounted for before anything is drawn.
	std::unordered_set<size_t> m_Dirty;

	std::vector<glm::ivec3> m_Offsets;
	int m_SortedReach = -1;

	float m_ChunkWorld = 24.0f;

	// Subtracted from every relief sample so the mean surface is the radius
	// that was asked for. Measured in Create; see MeasureReliefBias.
	float m_ReliefBias = 0.0f;
};
