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
		float reach = settings.Radius + settings.Amplitude + 4.0f * settings.VoxelSize;
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
	float Relief(const glm::vec3& direction) const
	{
		float amplitude = 1.0f;

		// Cycles across the planet: how many features of `FeatureSize` fit
		// round it. The noise is sampled on the sphere of radius `Radius`, so
		// a feature is that many metres of actual ground.
		float frequency = m_Settings.Radius / std::max(1.0f, m_Settings.FeatureSize);

		float sum = 0.0f;
		float total = 0.0f;

		for (int i = 0; i < m_Settings.Octaves; i++)
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

		return shape * m_Settings.Amplitude - m_ReliefBias;
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
				float unit = glm::clamp(
					Relief(direction) / (2.0f * m_Settings.Amplitude) + 0.5f, 0.0f, 1.0f);

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
	int StreamAround(const glm::vec3& focus, float radius, int budget)
	{
		glm::ivec3 count = m_Field->ChunkCount();
		glm::vec3 local = focus - m_Field->Origin();

		glm::ivec3 centre = glm::ivec3(glm::floor(local / m_ChunkWorld));
		int reach = (int)std::ceil(radius / m_ChunkWorld) + 1;

		auto sdf = [this](const glm::vec3& p) { return Density(p); };

		int meshed = 0;

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

		for (const glm::ivec3& offset : m_Offsets)
		{
			if (budget <= 0)
				break;

			glm::ivec3 chunk = centre + offset;

			if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
				|| chunk.x >= count.x || chunk.y >= count.y || chunk.z >= count.z)
				continue;

			size_t key = Key(chunk);
			if (m_Filled.count(key))
				continue;

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

				m_Field->FillChunk(chunk,
					[uniform](const glm::vec3&) { return uniform; }, 1);

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

			budget--;
		}

		// Meshing is the expensive half, and a re-mesh is as expensive as the
		// first one, so it gets the same budget rather than being unbounded.
		int meshBudget = std::max(1, budget) + 4;

		for (auto it = m_Dirty.begin(); it != m_Dirty.end() && meshBudget > 0; )
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

			meshBudget--;

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
	static size_t Key(const glm::ivec3& chunk)
	{
		return ((size_t)(chunk.x & 0xFFFF) << 32)
			| ((size_t)(chunk.y & 0xFFFF) << 16)
			| (size_t)(chunk.z & 0xFFFF);
	}

	static glm::ivec3 Unkey(size_t key)
	{
		return glm::ivec3((int)((key >> 32) & 0xFFFF),
			(int)((key >> 16) & 0xFFFF), (int)(key & 0xFFFF));
	}

	glm::vec3 ChunkCentre(const glm::ivec3& chunk) const
	{
		return m_Field->Origin()
			+ (glm::vec3(chunk) + glm::vec3(0.5f)) * m_ChunkWorld;
	}

	bool TouchesSurface(const glm::vec3& chunkCentre) const
	{
		// Half the diagonal, because a chunk is a cube and its far corner is
		// what decides whether the shell clips it.
		float chunkReach = m_ChunkWorld * 0.8660254f;
		float band = m_Settings.Amplitude * 0.5f + chunkReach + m_Settings.VoxelSize;

		return std::abs(glm::length(chunkCentre) - m_Settings.Radius) <= band;
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
