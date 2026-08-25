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

		BuildColourMap();
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
		return 0.5f * m_Settings.Amplitude + std::abs(m_ReliefBias)
			+ 0.5f * m_Settings.Roughness;
	}

	const Settings& Get() const { return m_Settings; }
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
	void BuildColourMap()
	{
		const int width = 1024;
		const int height = 512;

		std::vector<unsigned char> pixels((size_t)width * height * 4);

		const float pi = 3.14159265358979323846f;

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
				float texel = 2.0f * 3.14159265f * m_Settings.Radius / (float)width;

				int useful = 1;

				while (useful < m_Settings.Octaves
					&& m_Settings.FeatureSize / (float)(1 << useful) > 2.0f * texel)
					useful++;

				float unit = glm::clamp(
					Relief(direction, useful) / (2.0f * m_Settings.Amplitude) + 0.5f,
					0.0f, 1.0f);

				unsigned char level = (unsigned char)(unit * 255.0f);

				size_t at = ((size_t)y * width + x) * 4;

				pixels[at + 0] = level;
				pixels[at + 1] = level;
				pixels[at + 2] = level;
				pixels[at + 3] = 255;
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
		glm::vec3 Position;    // on the ground, in the planet's own frame
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

		glm::vec3 low = m_Field->Origin() + glm::vec3(chunk) * m_ChunkWorld;
		glm::vec3 high = low + glm::vec3(m_ChunkWorld);

		float sea = m_Settings.OceanRadius;
		float top = std::max(m_Settings.Amplitude * 0.5f
			- (sea - m_Settings.Radius), 1.0f);

		for (int i = 0; i < m_Settings.PlantsPerChunk; i++)
		{
			glm::vec3 jitter(
				Hash3D(chunk.x * 3 + i, chunk.y, chunk.z, m_Settings.Seed + 811u),
				Hash3D(chunk.x, chunk.y * 3 + i, chunk.z, m_Settings.Seed + 812u),
				Hash3D(chunk.x, chunk.y, chunk.z * 3 + i, m_Settings.Seed + 813u));

			glm::vec3 candidate = low + jitter * m_ChunkWorld;
			float length = glm::length(candidate);

			if (length < 1e-3f)
				continue;

			glm::vec3 direction = candidate / length;
			glm::vec3 at = direction * (m_Settings.Radius + Relief(direction));

			if (glm::any(glm::lessThan(at, low)) || glm::any(glm::greaterThanEqual(at, high)))
				continue;

			// Above the beach, below the rock line -- the same two numbers the
			// fragment shader colours the ground with, so trees stop where the
			// green does rather than marching up a snowfield.
			float height = glm::length(at) - sea;

			if (height < 0.8f || height / top > 0.62f)
				continue;

			// And off the ice caps, which is the same latitude the tundra
			// starts at.
			if (std::abs(direction.y) > 0.58f)
				continue;

			// Nothing grows on a cliff.
			glm::vec3 normal = SurfaceNormal(at);

			if (glm::dot(normal, direction) < 0.78f)
				continue;

			// **Forests clump.** A uniform scatter over every qualifying metre
			// of ground reads as an orchard; one broad noise field deciding
			// where woodland is at all gives clearings and treelines.
			if (Noise3D(direction * (m_Settings.Radius / (m_Settings.FeatureSize * 1.5f)),
				m_Settings.Seed + 909u) < -0.30f)
				continue;

			Plant plant;
			plant.Position = at;
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

	int StreamAround(const glm::vec3& focus, float radius, float budget)
	{
		float spent = 0.0f;
		glm::ivec3 count = m_Field->ChunkCount();
		glm::vec3 local = focus - m_Field->Origin();

		glm::ivec3 centre = glm::ivec3(glm::floor(local / m_ChunkWorld));
		int reach = (int)std::ceil(radius / m_ChunkWorld) + 1;

		auto sdf = [this](const glm::vec3& p) { return Density(p); };

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

			glm::ivec3 min, max;
			m_Field->ChunkRange(chunk, min, max);

			Egss::MeshData data = Egss::MarchingTetrahedra::Mesh(*m_Field, min, max, 1);

			spent += 1.0f;

			if (data.Indices.empty())
			{
				m_Chunks.erase(key);
				continue;
			}

			data.RecalculateBounds();

			Chunk entry;
			entry.MeshPtr = std::make_shared<Egss::Mesh>(data, "PlanetChunk");
			entry.Centre = ChunkCentre(chunk);

			PlantChunk(chunk, entry.Plants);
			entry.Triangles = data.Indices.size() / 3;

			m_Chunks[key] = entry;
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

			glm::vec3 chunkCentre = ChunkCentre(chunk);

			if (glm::length(chunkCentre - focus) > radius)
				continue;

			m_Filled.insert(key);

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
				float uniform = glm::length(chunkCentre) < m_Settings.Radius
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

			m_Field->FillChunk(chunk, sdf, 1);

			// **Filling a chunk stales its low neighbours' meshes.**
			// `ChunkRange` includes one plane past the chunk's own cells, so a
			// mesh reads the *first* plane of the chunks above it in x, y and
			// z. Mesh a chunk before those exist and it meshes against empty
			// space -- which is what put a black grid of cracks across the
			// first planet, one line per chunk boundary, looking for all the
			// world like a mesher bug rather than an ordering one.
			m_Dirty.insert(key);

			for (int axis = 0; axis < 3; axis++)
			{
				glm::ivec3 lower = chunk;
				lower[axis] -= 1;

				if (lower[axis] < 0)
					continue;

				size_t lowerKey = Key(lower);

				// Only ones already built: a chunk not yet filled will mesh
				// against this one when its own turn comes.
				if (m_Chunks.count(lowerKey))
					m_Dirty.insert(lowerKey);
			}

			spent += 1.0f;
		}

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
	bool HighNeighboursFilled(const glm::ivec3& chunk) const
	{
		glm::ivec3 count = m_Field->ChunkCount();

		for (int axis = 0; axis < 3; axis++)
		{
			glm::ivec3 higher = chunk;
			higher[axis] += 1;

			if (higher[axis] >= count[axis])
				continue;

			if (!m_Filled.count(Key(higher)))
				return false;
		}

		return true;
	}

	void EvictBeyond(const glm::vec3& focus, float radius)
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
	void ReleaseBeyond(const glm::vec3& focus, float radius)
	{
		for (auto it = m_Filled.begin(); it != m_Filled.end(); )
		{
			glm::ivec3 chunk = Unkey(*it);

			if (glm::length(ChunkCentre(chunk) - focus) > radius)
			{
				m_Field->ClearChunk(chunk);
				it = m_Filled.erase(it);
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
		glm::vec3 Centre = glm::vec3(0.0f);
		size_t Triangles = 0;

		// Placed with the mesh and thrown away with it, because they are a
		// function of the same chunk index and cost nothing to reproduce.
		std::vector<Plant> Plants;
	};

	const std::unordered_map<size_t, Chunk>& Chunks() const { return m_Chunks; }

	size_t TriangleCount() const
	{
		size_t total = 0;
		for (const auto& [key, chunk] : m_Chunks)
			total += chunk.Triangles;

		return total;
	}

	size_t FilledChunks() const { return m_Filled.size(); }
	size_t MeshedChunks() const { return m_Chunks.size(); }

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
	bool TouchesSurface(const glm::vec3& chunkCentre) const
	{
		float distance = glm::length(chunkCentre);

		// The centre of the planet has no direction to ask about, and is solid
		// rock in every case that matters.
		if (distance < m_ChunkWorld)
			return false;

		// Cheap reject first: nothing outside the whole planet's relief range
		// can be near the surface, whatever direction it is in.
		float chunkReach = m_ChunkWorld * 0.8660254f;

		if (std::abs(distance - m_Settings.Radius)
			> ReliefReach() + chunkReach + m_Settings.VoxelSize)
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
			glm::vec3 at = chunkCentre + glm::vec3(i - 1, j - 1, k - 1) * half;

			float length = glm::length(at);

			if (length < 1e-3f)
				continue;

			float relief = Relief(at / length);

			low = glm::min(low, relief);
			high = glm::max(high, relief);
		}

		// The radii the ground occupies over this chunk, against the radii the
		// chunk occupies. They have to overlap for the surface to be in it.
		float margin = chunkReach + m_Settings.VoxelSize;

		return distance - margin <= m_Settings.Radius + high
			&& distance + margin >= m_Settings.Radius + low;
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

	std::unordered_map<size_t, Chunk> m_Chunks;
	// How far the outward scan got while the focus chunk stayed put.
	glm::ivec3 m_ScanCentre = glm::ivec3(0x7fffffff);
	size_t m_ScanFrom = 0;

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
