#pragma once

// **A small block of ground, and every knob that shapes it.**
//
// The solar demo is where the terrain has to work, and it is a poor place to
// find out why it does not: a change to the generator costs a planet-wide
// rebuild, the interesting ground is wherever the camera happens to be
// pointing, and half the effects only appear at a scale you have to fly to.
// This is the same generator over nine chunks of ground you can walk across in
// a minute, with the shape of it on sliders.
//
// **Nine chunks a side is the whole point of the number.** At 16 voxels a
// chunk and a metre a voxel that is 144 m across -- big enough to hold a hill
// and a hollow and to walk out of, small enough that regenerating the entire
// field takes a fraction of a second, so a slider can rebuild the world on
// release rather than asking for a reload.
//
// What it is for, in order of how often it has been wanted:
//
//   * Watching what a generation parameter actually does, at the scale a
//     person stands at rather than from orbit.
//   * Trying vegetation against ground you can put anywhere you like -- the
//     biome here is two sliders rather than a hydrology pass, so a desert is
//     one drag away instead of a landing site search.
//   * Somewhere to build developer tools before they go into the demo that
//     matters. The spawn points and the clipping toggle below are both meant
//     to be ported.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <map>

#include "Demo.h"
#include "Grass.h"
#include "Vegetation.h"

class TerrainLab : public DemoLayer
{
public:
	TerrainLab()
		: DemoLayer("TerrainLab"), m_Camera(60.0f, 16.0f / 9.0f, 0.1f, 900.0f)
	{
		RegisterParam("Walk speed", &m_WalkSpeed);
		RegisterParam("Dig radius", &m_DigRadius);

		// **Opens on a gradient, not on one biome nine times.** The demo is
		// for looking at boundaries, so the default grid runs wet to dry
		// across it and cold to warm down it -- every neighbouring pair is a
		// transition, which is the thing to look at.
		static const int opening[s_Grid][s_Grid] =
		{
			{ 2, 1, 0 },   // wetland, forest, meadow
			{ 1, 0, 3 },   // forest,  meadow, steppe
			{ 5, 3, 4 },   // tundra,  steppe, desert
		};

		for (int j = 0; j < s_Grid; j++)
		for (int i = 0; i < s_Grid; i++)
		{
			m_On[i][j] = true;
			m_Cell[i][j] = opening[j][i];
		}
	}

	void OnDemoAttach() override
	{
		BuildShaders();
		Generate();

		// A rim cell rather than the middle one: the pit is centred, so cell 4
		// is the bottom of the lake and opens you underwater. From the rim you
		// are looking across it.
		GoTo(1);
	}

private:
	// --- The block ----------------------------------------------------------

	// **Three chunks a side, and the three is the same three as the grid.**
	//
	// Sixteen voxels a chunk plus the one lattice plane that closes the last
	// cell. At a metre a voxel that is a 144 m cube.
	//
	// It is a *cube*, not a sheet. The vertical extent is 48 m with the
	// terrain sitting in the middle of it, so there is real rock underneath to
	// dig into and real air above to dig out into, rather than a surface with
	// nothing on either side of it.
	static constexpr int s_Chunks = 9;
	static constexpr int s_Side = s_Chunks * 16 + 1;

	// **The grid is three by three whatever the block is.** Nine chunks a side
	// at a metre a voxel is 144 m, so each of the nine cells owns a 3x3 group
	// of chunks and covers 48 m -- which is about as small as a biome can be
	// and still read as a place rather than as a patch. Decoupling the two
	// numbers is what lets the block grow without the panel growing with it:
	// eighty-one checkboxes would be a worse tool than nine.
	static constexpr int s_Grid = 3;

	static constexpr float s_WalkerRadius = 0.35f;
	static constexpr float s_WalkerHalfHeight = 0.55f;
	static constexpr float s_EyeHeight = 0.75f;

	float Extent() const { return (float)(s_Side - 1) * m_Voxel; }

	// --- Generation ---------------------------------------------------------
	//
	// Every field here is on a slider. The shapes are the same three the
	// planet uses, deliberately: a fractal base, a ridged layer that makes
	// ranges, and a plateau control that decides whether the ground is a
	// smooth swell or a tableland with an edge. If a change reads well here it
	// is worth trying on a planet; if it does not, nothing has been rebuilt.

	struct Shape
	{
		float FeatureSize = 60.0f;    // metres between the largest bumps
		int Octaves = 5;
		float Amplitude = 22.0f;      // metres, peak to trough
		float Ridged = 0.45f;         // 0 rolling, 1 ridgelines
		float Warp = 0.25f;           // domain warp, as a share of the feature

		// **The same plateau control the planet grew a continental shelf
		// from.** Zero leaves a smooth swell; small values squash the height
		// into two levels joined by a slope, which is what puts an edge on a
		// mesa. Worth having here because it is much easier to see what it
		// does from the ground than from orbit.
		float Plateau = 0.0f;

		// **A pit that holds water, which is a shape and not a texture.**
		//
		// Noise does not make lakes. A basin has to be a *bowl* -- ground that
		// falls away smoothly from a rim and comes back up on every side --
		// and nothing built from summed octaves reliably closes like that,
		// which is why the planet needs a whole drainage pass to find the few
		// places that happen to. Here it is put in on purpose: a smooth
		// depression subtracted from the height, deepest at the middle and
		// zero at the rim, so there is somewhere for water to sit.
		//
		// `smoothstep` on the radius rather than a cone: a conical pit has a
		// crease at the bottom that reads as a fold in the ground, and a flat
		// bottom is where a lake bed belongs anyway.
		float Basin = 24.0f;         // metres deep at the middle
		float BasinSize = 62.0f;     // radius of the rim

		// Caves, as rock removed after the fact -- which is what stops the
		// surface being a height function and lets it fold over.
		float CaveStrength = 0.0f;
		float CaveSize = 22.0f;

		unsigned int Seed = 1337u;
	};

	Shape m_Shape;

	// **A biome is two numbers and a name.**
	//
	// Everything downstream -- the colour of the soil, whether grass grows and
	// what colour it is, whether it has gone to straw -- reads moisture and
	// warmth and nothing else. That is deliberate and it is the same pair the
	// planet's Whittaker square uses, so anything tuned here transfers.
	struct Biome
	{
		const char* Name;
		float Moisture;
		float Warmth;
	};

	static const Biome* Biomes()
	{
		static const Biome table[] =
		{
			{ "Meadow",   0.72f, 0.55f },
			{ "Forest",   0.85f, 0.50f },
			{ "Wetland",  0.97f, 0.60f },
			{ "Steppe",   0.34f, 0.70f },
			{ "Desert",   0.06f, 0.94f },
			{ "Tundra",   0.45f, 0.10f },
			{ "Bare",     0.02f, 0.40f },
		};

		return table;
	}

	static constexpr int s_BiomeCount = 7;

	// ImGui wants a plain array of names. Built from the table rather than
	// written out again, so adding a biome is still one line in one place.
	static const char* const* BiomeNames()
	{
		static const char* names[s_BiomeCount] = {};
		static bool filled = false;

		if (!filled)
		{
			for (int i = 0; i < s_BiomeCount; i++)
				names[i] = Biomes()[i].Name;

			filled = true;
		}

		return names;
	}

	// **The grid, and what it means.**
	//
	// One cell per chunk column. Unticked is not "no biome" -- it is *no
	// ground*: the column is empty and you can walk into the hole and look at
	// the section, which is the cheapest way to see what the generator is
	// doing under the surface.
	bool m_On[s_Grid][s_Grid];
	int m_Cell[s_Grid][s_Grid];

	float m_Voxel = 1.0f;

	// **Wind is a field, not a vector.**
	//
	// A single direction and speed for the whole world is the thing that made
	// the grass read as a machine: every blade leaning the same way by the
	// same amount, for ever. Real wind over open ground has structure at
	// several scales at once -- a prevailing direction, gust fronts tens of
	// metres across that arrive and pass, and small eddies that swirl and die.
	//
	// Three layers, and each is a different *size* rather than a different
	// amplitude of the same thing:
	//
	//   * The prevailing wind, which is the slider and does not vary.
	//   * Gusts, ~70 m across, which multiply the speed between a lull and a
	//     squall. This is the layer you feel.
	//   * Eddies, ~18 m across, which turn the direction by up to a quarter
	//     turn and are what stop a gust front being a straight edge.
	//
	// **All of it is advected with the mean wind**, which is the part that
	// makes it look like weather rather than like noise: a gust is a structure
	// travelling downwind, so it is sampled at `position - mean * time`. Stand
	// still and the pattern comes past you; run downwind at wind speed and it
	// very nearly stops, which is exactly what a balloon does.
	glm::vec2 MeanWind() const
	{
		float angle = glm::radians(m_WindAngle);

		return glm::vec2(std::cos(angle), std::sin(angle)) * m_WindSpeed;
	}

	// Returns the wind at a point as a 2D vector in the ground plane.
	glm::vec2 WindAt(float x, float z, float time) const
	{
		glm::vec2 mean = MeanWind();

		// Downwind is the direction the pattern travels, so subtracting it is
		// what carries the gusts past a standing observer.
		glm::vec2 at = glm::vec2(x, z) - mean * time;

		float gust = Noise2D(at.x / 70.0f, at.y / 70.0f, m_Shape.Seed + 811u);
		float lull = Noise2D(at.x / 210.0f, at.y / 210.0f, m_Shape.Seed + 812u);

		// Two scales multiplied rather than added: a big slow lull that takes
		// the whole area quiet, with gusts riding inside it. Added, the two
		// would average out and the field would be flat again.
		// **The offsets are 1.0 so the slider means what it says.** The noise
		// is zero-mean, so a product of `(1 + a n)` terms averages one and the
		// field's mean speed is the mean wind. With 0.75 and 0.80 here the
		// product averaged 0.6 and every reading came out well under the
		// number on the slider, which is the sort of quiet lie that makes a
		// tuning session take twice as long.
		float strength = glm::clamp(
			(1.0f + 0.55f * gust) * (1.0f + 0.45f * lull), 0.15f, 2.1f);

		float swirl = Noise2D(at.x / 18.0f, at.y / 18.0f, m_Shape.Seed + 813u);

		float turn = swirl * 0.8f;   // radians, so about a quarter turn

		float c = std::cos(turn), sn = std::sin(turn);

		glm::vec2 turned(mean.x * c - mean.y * sn, mean.x * sn + mean.y * c);

		return turned * strength;
	}

	float CellSize() const { return Extent() / (float)s_Grid; }

	// Which cell a world position falls in, clamped to the grid.
	glm::ivec2 CellAt(float x, float z) const
	{
		float half = 0.5f * Extent();
		float cell = CellSize();

		return glm::ivec2(
			glm::clamp((int)std::floor((x + half) / cell), 0, s_Grid - 1),
			glm::clamp((int)std::floor((z + half) / cell), 0, s_Grid - 1));
	}

	bool CellOn(int i, int j) const
	{
		return i >= 0 && j >= 0 && i < s_Grid && j < s_Grid && m_On[i][j];
	}

	// **The climate at a point, blended across the grid.**
	//
	// A cell's biome is a property of a 16 m square, and a 16 m square of
	// desert against a 16 m square of meadow with a hard line between them
	// reads as a tiled floor, not as country. What makes a boundary look like
	// a boundary is that it is *wide* -- moisture changes over hundreds of
	// metres in life, and even compressed to a few here the eye accepts it.
	//
	// So the cells are treated as samples at their own centres and read back
	// bilinearly. That gives a smooth field with the stated value at each
	// centre, and a transition a full cell wide between any two neighbours,
	// for four multiplies.
	//
	// **An unticked cell contributes nothing rather than contributing zero.**
	// Those are different: zero moisture is a desert, and a hole in the ground
	// should not make its neighbours arid. The weights of the cells that do
	// exist are renormalised instead, so ground beside a hole keeps the
	// climate it would have had.
	glm::vec2 ClimateAt(float x, float z) const
	{
		float half = 0.5f * Extent();
		float cell = CellSize();

		// Cell-centre coordinates: centre of cell 0 is at 0, of cell 1 at 1.
		float u = (x + half) / cell - 0.5f;
		float v = (z + half) / cell - 0.5f;

		int i = (int)std::floor(u), j = (int)std::floor(v);

		float fu = u - (float)i, fv = v - (float)j;

		// Smoothstep rather than linear: a linear blend has a crease along
		// every cell line, which is exactly the tiling this is here to avoid.
		fu = fu * fu * (3.0f - 2.0f * fu);
		fv = fv * fv * (3.0f - 2.0f * fv);

		glm::vec2 sum(0.0f);
		float total = 0.0f;

		for (int dj = 0; dj <= 1; dj++)
		for (int di = 0; di <= 1; di++)
		{
			int ci = glm::clamp(i + di, 0, s_Grid - 1);
			int cj = glm::clamp(j + dj, 0, s_Grid - 1);

			if (!m_On[ci][cj])
				continue;

			float weight = (di ? fu : 1.0f - fu) * (dj ? fv : 1.0f - fv);

			const Biome& biome = Biomes()[m_Cell[ci][cj]];

			sum += glm::vec2(biome.Moisture, biome.Warmth) * weight;
			total += weight;
		}

		// Every cell around this point is off, which happens only over a hole.
		if (total < 1e-4f)
			return glm::vec2(0.5f, 0.5f);

		return sum / total;
	}

	// One octave of value noise, warped. `Veg::Hash2DUnit` is the same hash
	// the vegetation uses, so nothing here needs its own.
	static float Noise2D(float x, float y, unsigned int seed)
	{
		int ix = (int)std::floor(x), iy = (int)std::floor(y);
		float fx = x - (float)ix, fy = y - (float)iy;

		// Smoothstep on the fraction: linear interpolation of value noise has
		// a visible crease along every lattice line.
		fx = fx * fx * (3.0f - 2.0f * fx);
		fy = fy * fy * (3.0f - 2.0f * fy);

		float a = Veg::Hash2DUnit(ix, iy, seed);
		float b = Veg::Hash2DUnit(ix + 1, iy, seed);
		float c = Veg::Hash2DUnit(ix, iy + 1, seed);
		float d = Veg::Hash2DUnit(ix + 1, iy + 1, seed);

		return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy) * 2.0f - 1.0f;
	}

	float Height(float x, float z) const
	{
		float frequency = 1.0f / glm::max(m_Shape.FeatureSize, 1.0f);

		// **Domain warp before anything else.** Moving the sample point by a
		// slow noise is what turns concentric blobs into something that looks
		// eroded, and it costs one extra octave rather than a simulation.
		if (m_Shape.Warp > 0.0f)
		{
			float w = m_Shape.FeatureSize * m_Shape.Warp;

			x += Noise2D(x * frequency * 0.5f, z * frequency * 0.5f,
				m_Shape.Seed + 91u) * w;
			z += Noise2D(x * frequency * 0.5f, z * frequency * 0.5f,
				m_Shape.Seed + 92u) * w;
		}

		float sum = 0.0f, total = 0.0f, weight = 1.0f;

		for (int i = 0; i < glm::max(m_Shape.Octaves, 1); i++)
		{
			float n = Noise2D(x * frequency, z * frequency,
				m_Shape.Seed + (unsigned int)i);

			// Ridged is `1 - |n|` recentred; the mix is what lets one slider
			// run from rolling hills to ridgelines without two code paths.
			float ridge = (1.0f - std::abs(n)) * 2.0f - 1.0f;

			sum += glm::mix(n, ridge, m_Shape.Ridged) * weight;
			total += weight;

			weight *= 0.5f;
			frequency *= 2.0f;
		}

		float unit = total > 0.0f ? sum / total : 0.0f;

		// The plateau, as on the planet: squash toward two levels joined by a
		// slope of the given width. See `Shape::Plateau`.
		if (m_Shape.Plateau > 0.0f)
		{
			float edge = m_Shape.Plateau;

			unit = glm::smoothstep(-edge, edge,
				glm::clamp(unit, -1.0f, 1.0f)) * 2.0f - 1.0f;
		}

		float height = unit * m_Shape.Amplitude * 0.5f;

		// The bowl, subtracted after the noise so the rim follows the terrain
		// it is cut into rather than flattening it.
		if (m_Shape.Basin > 0.0f)
		{
			float away = std::sqrt(x * x + z * z);

			float bowl = 1.0f - glm::smoothstep(0.0f,
				glm::max(m_Shape.BasinSize, 1.0f), away);

			// Squared, so the sides are steep near the rim and the floor is
			// broad and flat -- the profile a lake bed actually has.
			height -= m_Shape.Basin * bowl * bowl;
		}

		return height;
	}

	// Where the water sits, and whether there is any. The level is measured
	// from the rim rather than from zero so that deepening the basin does not
	// also empty it -- which is the behaviour anyone dragging the slider
	// expects, and the opposite of what a fixed level gives.
	bool HasWater() const { return m_Shape.Basin > 0.0f && m_WaterFill > 0.0f; }

	float WaterLevel() const
	{
		return -m_Shape.Basin * (1.0f - m_WaterFill);
	}

	glm::vec2 Slope(float x, float z) const
	{
		// Central differences over a voxel. Wide enough not to be noise, and
		// the only reason it exists is the gradient correction below.
		float h = m_Voxel;

		return glm::vec2(
			(Height(x + h, z) - Height(x - h, z)) / (2.0f * h),
			(Height(x, z + h) - Height(x, z - h)) / (2.0f * h));
	}

	// The terrain, as a distance rather than a height.
	//
	// **The division is not decoration.** `p.y - h(x, z)` has a gradient of
	// length sqrt(1 + |grad h|^2), so it changes faster than a distance does,
	// and anything that marches it -- the walker's ground query, the dig
	// raycast -- would step straight past the surface. Dividing by that length
	// gives the first-order true distance. Same reasoning as `VoxelTerrain`.
	float Density(const glm::vec3& p) const
	{
		// **An unticked cell is a hole, not flat ground.** Returning a large
		// positive distance is "empty" to everything downstream -- the mesher
		// makes no triangles, the walker falls, the raycast passes through --
		// so the column simply is not there and you can walk into the gap and
		// look at the section of its neighbours.
		glm::ivec2 cell = CellAt(p.x, p.z);

		if (!m_On[cell.x][cell.y])
			return m_Voxel * 4.0f;

		float h = Height(p.x, p.z);
		glm::vec2 slope = Slope(p.x, p.z);

		float ground = (p.y - h) / std::sqrt(1.0f + glm::dot(slope, slope));

		if (m_Shape.CaveStrength <= 0.0f)
			return ground;

		// Caves are rock *subtracted*, which is `max(d, -s)`. A height field
		// can never fold over; the moment anything is removed from under the
		// surface it stops being a graph over the ground plane.
		float f = 1.0f / glm::max(m_Shape.CaveSize, 1.0f);

		float cave = Noise2D(p.x * f, p.z * f, m_Shape.Seed + 501u) * 0.6f
			+ Noise2D(p.x * f * 2.0f + p.y * f, p.z * f * 2.0f,
				m_Shape.Seed + 502u) * 0.4f;

		float carve = (m_Shape.CaveStrength * 6.0f) * (0.35f - std::abs(cave));

		return glm::max(ground, -carve);
	}

	void Generate()
	{
		float half = 0.5f * Extent();

		m_Field = std::make_shared<Egss::VoxelField3D>();
		m_Field->Create({ s_Side, s_Side, s_Side }, m_Voxel,
			{ -half, -half, -half });

		m_Field->Fill([this](const glm::dvec3& p)
			{ return Density(glm::vec3(p)); });

		m_Field->MarkAllDirty();

		BuildWorld();
		RebuildDirtyMeshes();
	}

	void BuildWorld()
	{
		m_World.Clear();
		m_World.Gravity = { 0.0f, -9.81f, 0.0f };

		Egss::RigidBody3D ground =
			Egss::RigidBody3D::MakeSdf({ 0.0f, 0.0f, 0.0f }, m_Field);

		m_Ground = m_World.AddBody(ground);

		SpawnWalker();
	}

	void SpawnWalker()
	{
		Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(
			{ 0.0f, 40.0f, 0.0f }, s_WalkerRadius, s_WalkerHalfHeight, 80.0f);

		// Upright and staying that way: a capsule that can tip over is a
		// ragdoll, not a player.
		body.InverseInertiaLocal = glm::mat3(0.0f);
		body.InverseInertiaWorld = glm::mat3(0.0f);
		body.Friction = 0.4f;
		body.Restitution = 0.0f;
		body.LinearDamping = 0.0f;

		m_Walker = m_World.AddBody(body);
	}

	// --- Meshing ------------------------------------------------------------

	void RebuildDirtyMeshes()
	{
		const std::vector<glm::ivec3>& dirty = m_Field->DirtyChunks();

		m_TriangleCount = 0;
		m_GrassTriangles = 0;

		for (const glm::ivec3& chunk : dirty)
		{
			glm::ivec3 min, max;
			m_Field->ChunkRange(chunk, min, max);

			Egss::MeshData data = Egss::MarchingCubes::Mesh(*m_Field, min, max);

			size_t key = ChunkKey(chunk);

			if (data.Indices.empty())
			{
				m_Chunks.erase(key);
				m_Grass.erase(key);
				m_Trees.erase(key);
				continue;
			}

			m_Chunks[key] = std::make_shared<Egss::Mesh>(data, "LabChunk");

			BuildChunkGrass(key, chunk, data);
			BuildChunkTrees(key, chunk, data);
		}

		m_Field->ClearDirtyChunks();

		for (const auto& entry : m_Chunks)
			m_TriangleCount += (int)entry.second->GetTriangleCount();

		for (const auto& entry : m_Grass)
			m_GrassTriangles += (int)entry.second->GetTriangleCount();
	}

	// **Two passes of blades, long and short.**
	//
	// One length of blade reads as a brush: every tip at the same height is a
	// flat plane of green with nothing behind it. Real grass is a canopy over
	// an understorey, and the cheapest way to say so is to scatter twice --
	// a tall sparse pass and a short dense one. The short pass is what hides
	// the ground between the tall blades, which is the job the density was
	// being asked to do on its own.
	void BuildChunkGrass(size_t key, const glm::ivec3& chunk,
		const Egss::MeshData& data)
	{
		m_Grass.erase(key);

		if (!m_ShowGrass || m_GrassDensity <= 0.0f)
			return;

		unsigned int seed = (unsigned int)(chunk.x * 73 + chunk.y * 19
			+ chunk.z * 131);

		auto up = [](const glm::vec3&) { return glm::vec3(0.0f, 1.0f, 0.0f); };

		// Grass where it is wet enough, asked of the **blended** field at the
		// blade's own position rather than of one number for the chunk. That
		// is what makes a meadow thin out into a steppe across a boundary
		// instead of stopping on the chunk line.
		auto allow = [this](const glm::vec3& at, const glm::vec3&)
		{
			glm::vec2 climate = ClimateAt(at.x, at.z);

			// Dry ground grows less, and hot dry ground grows none -- the same
			// desert rule the ground colour uses.
			float desert = glm::smoothstep(0.34f, 0.10f, climate.x)
				* glm::smoothstep(0.45f, 0.75f, climate.y);

			float wet = glm::smoothstep(0.20f, 0.55f, climate.x)
				* (1.0f - desert);

			// Nothing grows under a lake. A metre of margin so the shoreline
			// is a band rather than a line drawn on the water.
			if (HasWater() && at.y < WaterLevel() + 1.0f)
				wet *= glm::smoothstep(WaterLevel() - 0.5f,
					WaterLevel() + 1.0f, at.y);

			return wet;
		};

		Grass::Settings tall;
		tall.Density = m_GrassDensity * 0.45f;
		tall.Height = m_GrassHeight;
		tall.Width = 0.006f;
		tall.Seed = 977u;

		Grass::Settings low;
		low.Density = m_GrassDensity * 0.55f;
		low.Height = m_GrassHeight * 0.45f;
		low.Width = 0.005f;
		low.Lean = 0.5f;
		low.Seed = 4231u;

		Egss::MeshData blades = Grass::Build(data, tall, seed, up, allow);
		Egss::MeshData under = Grass::Build(data, low, seed, up, allow);

		// One mesh, one draw. Appending needs the second lot's indices shifted
		// past the first lot's vertices -- the classic off-by-a-whole-mesh.
		unsigned int base = (unsigned int)blades.Vertices.size();

		blades.Vertices.insert(blades.Vertices.end(),
			under.Vertices.begin(), under.Vertices.end());

		for (unsigned int index : under.Indices)
			blades.Indices.push_back(index + base);

		if (blades.Indices.empty())
			return;

		blades.Submeshes.clear();

		Egss::Submesh all;
		all.IndexCount = (unsigned int)blades.Indices.size();
		blades.Submeshes.push_back(all);
		blades.RecalculateBounds();

		m_Grass[key] = std::make_shared<Egss::Mesh>(blades, "LabGrass");
	}

	// --- Trees ---------------------------------------------------------------

	struct Tree
	{
		glm::vec3 At;
		float Scale;
		float Yaw;
		int Shape;
	};

	// **Trees are scattered over the chunk's own triangles, exactly as the
	// grass is.** A triangle of the mesh cannot disagree with the mesh, so
	// picking a point inside one puts the trunk on the surface that is drawn,
	// with no search and no tolerance. Grass has done this since it was
	// written and has never floated or sunk.
	//
	// **This was built to test a theory about the planet's buried trees, and
	// the measurement killed the theory.** The planet places trees by asking
	// the generator for the surface radius along a direction -- an analytic
	// answer -- rather than off the mesh, and the guess was that the two
	// disagree enough to sink a tree to its canopy.
	//
	// Measured here over 156 trees: the field's own distance at a trunk's foot
	// is at worst **0.1159 m**, and the gap between the mesh position and the
	// analytic height is at worst **0.1268 m**. They agree to about a tenth of
	// a metre, which is the marching-cubes interpolation error and nothing
	// more. Whatever buries the planet's trees, it is not this.
	//
	// Scattering off the mesh is still the better way round -- it is exact by
	// construction rather than by luck, and it needs no surface search -- but
	// the comment that used to sit here asserted a cause this file now
	// disproves, and it is worth leaving the disproof where the next person
	// will look for it.
	void BuildChunkTrees(size_t key, const glm::ivec3& chunk,
		const Egss::MeshData& data)
	{
		m_Trees.erase(key);

		if (!m_ShowTrees || m_TreeDensity <= 0.0f || data.Indices.size() < 3)
			return;

		unsigned int seed = 5501u + (unsigned int)(chunk.x * 73 + chunk.y * 19
			+ chunk.z * 131);

		std::vector<Tree> trees;

		size_t triangles = data.Indices.size() / 3;

		for (size_t t = 0; t < triangles; t++)
		{
			const glm::vec3& a = data.Vertices[data.Indices[t * 3 + 0]].Position;
			const glm::vec3& b = data.Vertices[data.Indices[t * 3 + 1]].Position;
			const glm::vec3& c = data.Vertices[data.Indices[t * 3 + 2]].Position;

			glm::vec3 face = glm::cross(b - a, c - a);
			float area2 = glm::length(face);

			if (area2 < 1e-8f)
				continue;

			glm::vec3 n = face / area2;

			if (n.y < 0.0f)
				n = -n;

			// A tree needs flatter ground than grass does -- a trunk on a
			// steep face leans out of the hill and reads as fallen.
			if (n.y < 0.80f)
				continue;

			glm::vec3 centre = (a + b + c) / 3.0f;

			// Nothing grows under the lake, and nothing right at its edge:
			// a trunk half in the water looks like a mistake even when the
			// waterline is exactly right.
			if (HasWater() && centre.y < WaterLevel() + 1.5f)
				continue;

			glm::vec2 climate = ClimateAt(centre.x, centre.z);

			// **Trees are the wettest thing on the map.** Grass will grow on a
			// steppe and a tree will not, so the threshold sits well above the
			// grass's -- which is what makes the boundary between wood and
			// open ground fall inside a biome transition rather than on it.
			float wet = glm::smoothstep(0.55f, 0.85f, climate.x);

			// And they stop where it is cold, the same way the planet's do.
			float warm = glm::smoothstep(0.15f, 0.35f, climate.y);

			float chance = wet * warm * m_TreeDensity * (0.5f * area2);

			if (chance <= 1e-4f)
				continue;

			int count = (int)chance;

			if (Veg::Hash2DUnit((int)t, 0, seed) < chance - (float)count)
				count++;

			for (int i = 0; i < count; i++)
			{
				float u = Veg::Hash2DUnit((int)t, i * 4 + 1, seed);
				float v = Veg::Hash2DUnit((int)t, i * 4 + 2, seed);
				float su = std::sqrt(u);

				Tree tree;
				tree.At = a + (b - a) * (su * (1.0f - v)) + (c - a) * (su * v);
				tree.Yaw = Veg::Hash2DUnit((int)t, i * 4 + 3, seed) * 6.2831853f;
				tree.Scale = 0.65f + Veg::Hash2DUnit((int)t, i * 4 + 4, seed) * 0.8f;
				tree.Shape = (int)(Veg::Hash2DUnit((int)t, i * 4 + 5, seed)
					* (float)s_TreeShapes) % s_TreeShapes;

				trees.push_back(tree);
			}
		}


		if (!trees.empty())
			m_Trees[key] = std::move(trees);
	}

	static size_t ChunkKey(const glm::ivec3& chunk)
	{
		return ((size_t)chunk.z * 1024 + chunk.y) * 1024 + chunk.x;
	}

	// --- Editing ------------------------------------------------------------

	void Dig(bool add)
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float distance = 0.0f;
		glm::vec3 point(0.0f), normal(0.0f);

		if (!m_Field->Raycast(origin, direction, m_Reach, distance, point, normal))
			return;

		// Adding puts the sphere's centre outside the surface, or half of it
		// lands inside the rock it is meant to sit on.
		glm::vec3 at = add ? point + normal * (m_DigRadius * 0.5f) : point;

		if (m_Field->EditSphere(at, m_DigRadius, add) == 0)
			return;

		RebuildDirtyMeshes();

		// **Rebuild the ground, not the world.**
		//
		// This used to call `BuildWorld`, which calls `SpawnWalker`, which
		// puts the player back at forty metres above the origin -- so every
		// dig teleported you into the sky and digging read as broken rather
		// than as working and moving you.
		//
		// The collider holds the field by pointer so the *shape* follows on
		// its own; what does not follow is the broadphase bound, and a body
		// whose bounds are stale stops colliding at the edges of what changed.
		// Replacing the ground body alone fixes that and leaves everything
		// else -- the player, their velocity, where they were looking --
		// exactly as it was.
		Egss::RigidBody3D& ground = m_World.GetBody(m_Ground);

		ground = Egss::RigidBody3D::MakeSdf({ 0.0f, 0.0f, 0.0f }, m_Field);
	}

	// --- Developer tools ----------------------------------------------------
	//
	// Both of these are meant for the solar demo and are being built here
	// first, where a mistake costs a rebuild of nine chunks rather than a
	// planet.

	// **The spawn points are the grid.** A named list of places was the right
	// idea while the climate was two sliders; now that every cell has its own
	// biome, the useful thing to stand in the middle of is a cell -- and there
	// are exactly nine of them.
	void GoTo(int which)
	{
		if (which < 0 || which >= s_Grid * s_Grid)
			return;

		m_Spawn = which;

		int i = which % s_Grid, j = which / s_Grid;

		float cell = CellSize();
		float half = 0.5f * Extent();

		glm::vec3 at(
			-half + ((float)i + 0.5f) * cell,
			0.0f,
			-half + ((float)j + 0.5f) * cell);

		// Drop in from above rather than at the ground: the terrain under a
		// spawn changes with every slider, so a fixed height is either buried
		// or floating, and falling a couple of metres is neither.
		at.y = Height(at.x, at.z) + 2.5f;

		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);
		body.Position = at;
		body.Velocity = glm::vec3(0.0f);
		body.Awake = true;

		// **Face the middle of the block.** A spawn tool that drops you facing
		// whichever way you happened to be looking makes you turn round before
		// you can see anything, and the interesting thing is nearly always
		// toward the centre -- the water pit is there, and so is every cell
		// boundary from a corner.
		//
		// Forward is `(cos yaw, ., sin yaw)`, so the yaw that points at the
		// origin from `at` is the angle of `-at`.
		if (glm::length(glm::vec2(at.x, at.z)) > 1e-3f)
		{
			m_Yaw = glm::degrees(std::atan2(-at.z, -at.x));
			m_Pitch = -12.0f;
			m_Camera.SetRotation(m_Yaw, m_Pitch);
		}

		m_Camera.SetPosition(at + glm::vec3(0.0f, s_EyeHeight, 0.0f));
	}

	void RebuildGrass()
	{
		m_Field->MarkAllDirty();
		RebuildDirtyMeshes();
	}

	// --- Walking ------------------------------------------------------------

	void MoveWalker(Egss::Timestep step)
	{
		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		body.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.UpdateInertiaWorld();

		glm::vec3 forward = m_Camera.GetForward();
		glm::vec3 flat = forward;
		flat.y = 0.0f;

		if (glm::length(flat) < 1e-4f)
			flat = { 0.0f, 0.0f, 1.0f };

		flat = glm::normalize(flat);

		glm::vec3 right = glm::normalize(
			glm::cross(flat, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 wish(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) wish += m_NoClip ? forward : flat;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) wish -= m_NoClip ? forward : flat;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) wish += right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) wish -= right;

		// **Clipping off: the body stops being a body.**
		//
		// The tempting version of this is to keep simulating and turn the
		// collider off, which leaves the solver pushing a shape that nothing
		// pushes back on -- so gravity still accumulates and letting go of the
		// keys drops you through the floor at whatever speed you had reached.
		// Making it kinematic and integrating the position by hand is both
		// simpler and the thing a person actually means by "let me through the
		// ground": while it is on, the world does not act on you at all.
		if (m_NoClip)
		{
			body.Type = Egss::BodyType::Kinematic;
			body.Velocity = glm::vec3(0.0f);

			float speed = m_WalkSpeed * (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT)
				? 6.0f : 2.5f);

			if (Egss::Input::IsKeyPressed(EGSS_KEY_SPACE))
				wish += glm::vec3(0.0f, 1.0f, 0.0f);
			if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_CONTROL))
				wish -= glm::vec3(0.0f, 1.0f, 0.0f);

			if (glm::length(wish) > 1e-4f)
				body.Position += glm::normalize(wish) * speed * (float)step;

			m_Camera.SetPosition(body.Position
				+ glm::vec3(0.0f, s_EyeHeight, 0.0f));

			return;
		}

		body.Type = Egss::BodyType::Dynamic;

		float feet = body.Position.y - (s_WalkerHalfHeight + s_WalkerRadius);

		float ground = 0.0f;
		glm::vec3 normal(0.0f, 1.0f, 0.0f);
		bool found = m_World.GroundBelow(body.Position, ground, normal, m_Walker);

		m_Grounded = found && (feet - ground) < 0.25f;

		glm::vec3 velocity = body.Velocity;

		if (glm::length(wish) > 1e-4f)
		{
			glm::vec3 move = glm::normalize(wish) * m_WalkSpeed;
			velocity.x = move.x;
			velocity.z = move.z;
		}
		else if (m_Grounded)
		{
			velocity.x = 0.0f;
			velocity.z = 0.0f;
		}

		if (m_Grounded && Egss::Input::IsKeyPressed(EGSS_KEY_SPACE))
			velocity.y = m_JumpSpeed;

		body.Velocity = velocity;
		body.Awake = true;

		m_Camera.SetPosition(body.Position + glm::vec3(0.0f, s_EyeHeight, 0.0f));
	}

	// --- Shaders ------------------------------------------------------------

	void BuildShaders();
	void BuildWater();
	void BuildTrees();

	// --- Hooks --------------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		MoveWalker(step);
		m_World.Step(step);

		m_Time += (float)step;
	}

	void OnDemoUpdate(Egss::Timestep ts) override;
	void OnDemoImGui() override;

	void OnDemoEvent(Egss::Event& event) override
	{
		Egss::EventDispatcher dispatcher(event);

		dispatcher.Dispatch<Egss::KeyPressedEvent>(
			[this](Egss::KeyPressedEvent& key)
			{
				if (key.GetKeyCode() == EGSS_KEY_TAB)
				{
					SetMouseLook(!m_MouseLook);
					return true;
				}

				if (key.GetKeyCode() == EGSS_KEY_V)
				{
					m_NoClip = !m_NoClip;
					return true;
				}

				// The number row walks the spawn points, which is the fastest
				// way to compare two biomes: one key each.
				int digit = key.GetKeyCode() - EGSS_KEY_1;

				if (digit >= 0 && digit < s_Grid * s_Grid)
				{
					GoTo(digit);
					return true;
				}

				return false;
			});

		dispatcher.Dispatch<Egss::MouseButtonPressedEvent>(
			[this](Egss::MouseButtonPressedEvent& mouse)
			{
				if (!m_MouseLook)
					return false;

				if (mouse.GetMouseButton() == EGSS_MOUSE_BUTTON_LEFT)
				{
					Dig(false);
					return true;
				}

				if (mouse.GetMouseButton() == EGSS_MOUSE_BUTTON_RIGHT)
				{
					Dig(true);
					return true;
				}

				return false;
			});
	}

	void SetMouseLook(bool on)
	{
		m_MouseLook = on;
		m_MouseSampled = false;

		// Never grab the pointer during a replay: nobody is watching, and a
		// run that steals the cursor while the machine is in use is the exact
		// problem the hidden window exists to avoid.
		if (!Egss::Input::IsPlayingBack())
			Egss::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	void Look(float dt);

	// --- State --------------------------------------------------------------

	Egss::PerspectiveCamera m_Camera;
	Egss::PhysicsWorld3D m_World;

	std::shared_ptr<Egss::VoxelField3D> m_Field;
	std::map<size_t, std::shared_ptr<Egss::Mesh>> m_Chunks;
	std::map<size_t, std::shared_ptr<Egss::Mesh>> m_Grass;
	std::map<size_t, std::vector<Tree>> m_Trees;

	static constexpr int s_TreeShapes = 3;

	std::shared_ptr<Egss::Mesh> m_TreeBark[s_TreeShapes];
	std::shared_ptr<Egss::Mesh> m_TreeLeaves[s_TreeShapes];

	std::shared_ptr<Egss::Shader> m_TreeShader;
	std::shared_ptr<Egss::Material> m_TreeMaterial;

	bool m_ShowTrees = true;
	float m_TreeDensity = 0.012f;   // trees per square metre where fully wooded
	int m_TreeCount = 0;
	float m_TreeReach = 130.0f;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;

	std::shared_ptr<Egss::Shader> m_GrassShader;
	std::shared_ptr<Egss::Material> m_GrassMaterial;

	std::shared_ptr<Egss::Mesh> m_Water;
	std::shared_ptr<Egss::Shader> m_WaterShader;
	std::shared_ptr<Egss::Material> m_WaterMaterial;

	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;
	Egss::PhysicsWorld3D::BodyHandle m_Ground = 0;

	bool m_Grounded = false;
	bool m_NoClip = false;
	bool m_ShowGrass = true;
	bool m_ShowWireframe = false;

	int m_Spawn = 0;

	float m_WalkSpeed = 6.0f;
	float m_JumpSpeed = 6.5f;
	float m_DigRadius = 3.0f;
	float m_Reach = 60.0f;

	float m_GrassDensity = 60.0f;
	float m_GrassHeight = 0.55f;

	// The wind, here only so the grass has something to lean in. One vector
	// and a speed, rather than the whole climate model -- this demo is about
	// the ground.
	float m_WindSpeed = 5.0f;
	float m_WindAngle = 40.0f;

	// How far a blade may lean, as a share of its own length. 0.85 lets it lie
	// almost flat and no further, which is what grass in a gale actually does.
	float m_MaxLean = 0.85f;

	// How full the basin is, from dry to level with the rim.
	float m_WaterFill = 0.55f;

	float m_Time = 0.0f;
	float m_FrameTime = 0.0f;

	int m_TriangleCount = 0;
	int m_GrassTriangles = 0;

	float m_Yaw = -90.0f;
	float m_Pitch = -10.0f;
	float m_Sensitivity = 0.12f;
	float m_LookRate = 90.0f;

	bool m_MouseLook = false;
	bool m_MouseSampled = false;
	float m_LastMouseX = 0.0f;
	float m_LastMouseY = 0.0f;
};

// --- Shaders ----------------------------------------------------------------

// **Ground that is dirt with grass on it, not a green surface.**
//
// The solar demo paints the ground the colour of what grows on it, which works
// from orbit and fails underfoot: real ground seen between blades is *soil*,
// and painting it green is the single thing that makes a grass field read as a
// carpet. So the ground here is brown by default, and the green is a tint laid
// over it where grass grows -- which means the blades and the earth between
// them are two different colours, and the eye reads depth rather than a plane.
//
// The texture is three octaves of the same value noise the terrain is built
// from, sampled on the world position. Not a lookup: this demo has no textures
// and does not want the loading path for one, and what is wanted is only enough
// variation to break the flatness. Two things are worth more than a bitmap
// here -- that the noise is in *world* space so it does not swim when you walk,
// and that it moves the colour rather than the brightness, because a lit
// surface with only value variation reads as dirty rather than as soil.
inline void TerrainLab::BuildShaders()
{
	std::string vertexSrc = R"(
		#version 330 core

		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;

		out vec3 v_World;
		out vec3 v_Normal;

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			v_World = world.xyz;
			v_Normal = mat3(u_Transform) * a_Normal;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string fragmentSrc = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_World;
		in vec3 v_Normal;

		uniform vec3 u_SunDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform float u_Ambient;

		// **The grid, and the blend done per pixel.**
		//
		// Nine cells of (moisture, warmth), read back bilinearly at the
		// fragment's own position -- the same expression `ClimateAt` uses on
		// the CPU, because the grass has to agree with the ground it grows
		// out of. Nine vec3s is cheaper than a texture and needs no upload
		// path; the third component is the cell's weight, which is zero for an
		// unticked cell so a hole does not dry out its neighbours.
		uniform vec3 u_Cells[9];
		uniform float u_CellSize;
		uniform float u_Half;

		float hash(vec2 p)
		{
			return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
		}

		float noise(vec2 p)
		{
			vec2 i = floor(p), f = fract(p);
			f = f * f * (3.0 - 2.0 * f);

			return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x),
				mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
		}

		float fbm(vec2 p)
		{
			return noise(p) * 0.55 + noise(p * 2.3) * 0.30
				+ noise(p * 5.1) * 0.15;
		}

		// Bilinear over the cell centres, skipping cells that are not there.
		vec2 climate(vec2 at)
		{
			vec2 c = (at + u_Half) / u_CellSize - 0.5;

			vec2 base = floor(c);
			vec2 f = c - base;

			f = f * f * (3.0 - 2.0 * f);

			vec2 sum = vec2(0.0);
			float total = 0.0;

			for (int dj = 0; dj <= 1; dj++)
			for (int di = 0; di <= 1; di++)
			{
				ivec2 g = ivec2(clamp(base + vec2(di, dj), vec2(0.0), vec2(2.0)));

				vec3 cell = u_Cells[g.y * 3 + g.x];

				float weight = (di == 1 ? f.x : 1.0 - f.x)
					* (dj == 1 ? f.y : 1.0 - f.y) * cell.z;

				sum += cell.xy * weight;
				total += weight;
			}

			return total > 1e-4 ? sum / total : vec2(0.5);
		}

		void main()
		{
			vec3 normal = normalize(v_Normal);

			vec2 here = climate(v_World.xz);

			float u_Moisture = here.x;
			float u_Warmth = here.y;

			// **Soil first.** Everything else is laid on top of this, which is
			// the point: what shows between blades has to be earth.
			vec3 dryDirt = vec3(0.34, 0.25, 0.16);
			vec3 wetDirt = vec3(0.21, 0.15, 0.10);

			vec3 dirt = mix(dryDirt, wetDirt, u_Moisture);

			vec3 sand = vec3(0.72, 0.62, 0.41);
			vec3 rock = vec3(0.42, 0.40, 0.38);

			// Grass is a *tint on the soil*, not a replacement for it, so the
			// ground under a sparse field still reads as ground.
			vec3 turf = mix(vec3(0.22, 0.34, 0.13), vec3(0.32, 0.44, 0.16),
				u_Warmth);

			// The texture, in world space so it does not swim as you walk.
			float grain = fbm(v_World.xz * 0.7);
			float patch = fbm(v_World.xz * 0.11);

			// Hue and saturation, not brightness -- a surface varied only in
			// value looks dirty rather than like soil.
			dirt *= mix(0.82, 1.18, grain);
			dirt = mix(dirt, dirt.bgr * 0.9 + 0.05, 0.12 * patch);

			vec3 base = dirt;

			// Desert: dry and warm. This is the whole biome rule here, and it
			// is two sliders rather than a hydrology pass on purpose.
			float desert = smoothstep(0.34, 0.10, u_Moisture)
				* smoothstep(0.45, 0.75, u_Warmth);

			base = mix(base, sand * mix(0.88, 1.12, grain), desert);

			// Grass cover, thinning as it dries out, and never total -- soil
			// shows through a real sward everywhere.
			float cover = smoothstep(0.25, 0.60, u_Moisture)
				* (1.0 - desert) * (0.55 + 0.45 * patch);

			base = mix(base, turf * mix(0.85, 1.15, grain), cover * 0.75);

			// Steep ground is rock whatever the climate: soil does not stay on
			// a cliff, which is why a cliff is a different colour in life.
			// `flat` is an interpolation qualifier in GLSL, not a name you
			// may use, and the compiler says so without mentioning that.
			float level = clamp(normal.y, 0.0, 1.0);

			base = mix(rock * mix(0.9, 1.1, grain), base,
				smoothstep(0.45, 0.80, level));

			float diffuse = max(dot(normal, -u_SunDirection), 0.0);
			float dome = 0.5 + 0.5 * normal.y;

			vec3 lit = base * (u_SkyColor * dome * u_Ambient
				+ u_SunColor * diffuse);

			color = vec4(lit, 1.0);
		}
	)";

	m_Shader.reset(Egss::Shader::Create("LabGround", vertexSrc, fragmentSrc));
	m_Material = Egss::Material::Create(m_Shader);

	// **The blades, with the variation in the mesh's own hash.**
	//
	// Every blade the same green is the other half of why a field reads as a
	// rug. Real grass varies blade to blade -- age, shade, species -- and the
	// cheapest honest source of that here is the per-blade number the scatterer
	// already writes into the texture coordinate.
	std::string grassVertex = R"(
		#version 330 core

		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;

		// The mean wind; the gusts and eddies are computed here, from the same
		// three layers `WindAt` uses on the CPU. Kept in the shader rather
		// than baked per chunk because a gust is metres across and a chunk is
		// sixteen -- per chunk it would be one number for a whole gust front.
		uniform vec3 u_Wind;
		uniform float u_Time;
		uniform float u_Keep;
		uniform float u_Seed;
		uniform float u_MaxLean;

		out vec3 v_Normal;
		out float v_Up;
		out float v_Tint;

		float hash(vec2 p)
		{
			return fract(sin(dot(p, vec2(127.1, 311.7)) + u_Seed) * 43758.5453);
		}

		float noise(vec2 p)
		{
			vec2 i = floor(p), f = fract(p);
			f = f * f * (3.0 - 2.0 * f);

			return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x),
				mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y)
				* 2.0 - 1.0;
		}

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			float along = a_TexCoord.y;
			float ticket = a_TexCoord.x;

			// Same level of detail as the planet: a per-blade ticket against a
			// threshold that is a uniform, so both sides are constant across a
			// blade and it can never tear.
			if (ticket > u_Keep)
			{
				gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
				v_Normal = vec3(0.0, 1.0, 0.0);
				v_Up = 0.0;
				v_Tint = 0.0;
				return;
			}

			// **The wind here, not the wind everywhere.** Three layers, all
			// advected with the mean so a gust is a thing that travels past
			// you rather than a pattern that flickers in place. Same
			// expression as `WindAt` on the CPU.
			vec2 mean = u_Wind.xz;
			vec2 at = world.xz - mean * u_Time;

			float gust = noise(at / 70.0);
			float lull = noise(at / 210.0 + 31.7);
			float swirl = noise(at / 18.0 + 67.3);

			float strength = clamp((1.0 + 0.55 * gust) * (1.0 + 0.45 * lull),
				0.15, 2.1);

			float turn = swirl * 0.8;
			float c = cos(turn), sn = sin(turn);

			vec2 here = vec2(mean.x * c - mean.y * sn,
				mean.x * sn + mean.y * c) * strength;

			// Pressure goes as the square of the *local* speed, which is what
			// makes a gust arrive as a wave across a field rather than as a
			// brightness change.
			float speed = length(here);
			float pressure = 0.5 * 1.2 * speed * speed;

			vec3 push = vec3(here.x, 0.0, here.y);

			vec3 lean = push * (0.0033 * pressure * along * along / max(speed, 1e-4));

			// **A blade cannot lean further than it is long.**
			//
			// Without this the displacement goes as the square of the wind, so
			// at the top of the slider a blade is thrown several times its own
			// height and the field turns into a smear of stretched triangles.
			// A real blade lies flat and stops. `u_MaxLean` is a share of the
			// blade's own length, so a short blade in the understorey is
			// capped shorter than a tall one and the two stay in proportion.
			// The blade's own height is not in the vertex, but `along` times
			// the nominal height is a bound on it, and a bound is all a cap
			// needs.
			float limit = u_MaxLean * along;

			float reach = length(lean);

			if (reach > limit && reach > 1e-6)
				lean *= limit / reach;

			world.xyz += lean;

			v_Normal = mat3(u_Transform) * a_Normal;
			v_Up = along;
			v_Tint = ticket;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string grassFragment = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_Normal;
		in float v_Up;
		in float v_Tint;

		uniform vec3 u_SunDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform float u_Ambient;
		uniform vec3 u_Root;
		uniform vec3 u_Tip;
		uniform vec3 u_Dry;
		uniform float u_Dryness;

		void main()
		{
			vec3 normal = normalize(v_Normal);

			if (normal.y < 0.0)
				normal = -normal;

			// Dark at the root, light at the tip: ambient occlusion, and a
			// real one -- a blade near the ground is surrounded by other
			// blades and sees almost none of the sky. It is the single thing
			// that makes a field read as depth rather than as a green plane.
			vec3 base = mix(u_Root, u_Tip, v_Up);

			// **Blade to blade variation, from the ticket.** Two independent
			// shifts rather than one: a brightness spread alone reads as
			// noise, while a hue spread as well reads as different plants.
			// Wide enough to read as different plants, not so wide that the
			// low end goes black -- which is what 0.72 of an already dark
			// root did.
			base *= mix(0.86, 1.22, v_Tint);
			base = mix(base, base.gbr, 0.10 * fract(v_Tint * 7.13));

			// Drying out: the same blades, gone to straw.
			base = mix(base, u_Dry * mix(0.8, 1.2, v_Tint), u_Dryness);

			float diffuse = max(dot(normal, -u_SunDirection), 0.0);
			float dome = 0.5 + 0.5 * normal.y;

			vec3 lit = base * (u_SkyColor * dome * u_Ambient
				+ u_SunColor * diffuse);

			color = vec4(lit, 1.0);
		}
	)";

	m_GrassShader.reset(
		Egss::Shader::Create("LabGrass", grassVertex, grassFragment));

	m_GrassMaterial = Egss::Material::Create(m_GrassShader);

	BuildWater();
	BuildTrees();
}

// **Three shapes, built once, drawn everywhere.**
//
// `Veg::MakeTreeMesh` grows a tree from the origin along +y, which is exactly
// what a flat world wants -- the planet has to rotate every trunk onto its own
// local vertical and this does not, so the transform is a translate, a yaw and
// a scale and nothing else.
//
// Three is enough to stop a wood reading as wallpaper and few enough that the
// meshes can be built at load. Each is a different *habit* rather than a
// different random seed: a tall narrow one, a broad low one, and a middling one
// between them, because three seeds of the same parameters give three trees
// that are recognisably the same tree.
inline void TerrainLab::BuildTrees()
{
	Veg::TreeParams shapes[s_TreeShapes];

	// Tall and narrow -- a conifer's habit.
	shapes[0].Depth = 5;
	shapes[0].Children = 3;
	shapes[0].Length = 3.2f;
	shapes[0].Radius = 0.16f;
	shapes[0].Spread = 26.0f;
	shapes[0].LengthRatio = 0.78f;
	shapes[0].LeafRadius = 0.50f;

	// Broad and low -- an open-grown hardwood.
	shapes[1].Depth = 4;
	shapes[1].Children = 4;
	shapes[1].Length = 2.2f;
	shapes[1].Radius = 0.24f;
	shapes[1].Spread = 48.0f;
	shapes[1].LengthRatio = 0.70f;
	shapes[1].LeafRadius = 0.75f;

	// Between the two.
	shapes[2].Depth = 4;
	shapes[2].Children = 3;
	shapes[2].Length = 2.7f;
	shapes[2].Radius = 0.20f;
	shapes[2].Spread = 36.0f;
	shapes[2].LeafRadius = 0.60f;

	for (int i = 0; i < s_TreeShapes; i++)
	{
		Egss::MeshData bark, leaves;

		Veg::MakeTreeMesh(1471u + (unsigned int)i * 97u, shapes[i], bark, leaves);

		m_TreeBark[i] = std::make_shared<Egss::Mesh>(bark, "LabBark");
		m_TreeLeaves[i] = std::make_shared<Egss::Mesh>(leaves, "LabLeaves");
	}

	// The tree shader is the grass shader's argument one size up: the same
	// wind field, the same gust structure, and a bend that goes as the square
	// of height because a trunk is a cantilever. What differs is that a tree
	// carries its own root in a uniform -- there is one transform per tree, so
	// the root is known exactly and needs no reconstructing.
	std::string vertexSrc = R"(
		#version 330 core

		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;

		uniform vec3 u_Wind;
		uniform float u_Time;
		uniform float u_Seed;
		uniform float u_Compliance;

		out vec3 v_Normal;
		out float v_Up;

		float hash(vec2 p)
		{
			return fract(sin(dot(p, vec2(127.1, 311.7)) + u_Seed) * 43758.5453);
		}

		float noise(vec2 p)
		{
			vec2 i = floor(p), f = fract(p);
			f = f * f * (3.0 - 2.0 * f);

			return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x),
				mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y)
				* 2.0 - 1.0;
		}

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			// The root, exactly: the transform's own translation. No
			// reconstruction and nothing to drift, which is the advantage of
			// one draw per tree over an instance buffer.
			vec3 root = u_Transform[3].xyz;

			float height = max(world.y - root.y, 0.0);

			// The same three layers the grass leans in, sampled at the tree's
			// root so the whole tree agrees with itself -- a trunk bending one
			// way while its own canopy bends another is the artefact this
			// avoids.
			vec2 mean = u_Wind.xz;
			vec2 at = root.xz - mean * u_Time;

			float gust = noise(at / 70.0);
			float lull = noise(at / 210.0 + 31.7);
			float swirl = noise(at / 18.0 + 67.3);

			float strength = clamp((1.0 + 0.55 * gust) * (1.0 + 0.45 * lull),
				0.15, 2.1);

			float turn = swirl * 0.8;
			float c = cos(turn), sn = sin(turn);

			vec2 here = vec2(mean.x * c - mean.y * sn,
				mean.x * sn + mean.y * c) * strength;

			float speed = length(here);
			float pressure = 0.5 * 1.2 * speed * speed;

			// Height squared: a trunk is a beam clamped at the ground, so the
			// root stays vertical and the crown does the moving.
			vec3 push = vec3(here.x, 0.0, here.y) / max(speed, 1e-4);

			world.xyz += push * (u_Compliance * pressure * height * height);

			v_Normal = mat3(u_Transform) * a_Normal;
			v_Up = height;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string fragmentSrc = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_Normal;
		in float v_Up;

		uniform vec3 u_SunDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform float u_Ambient;
		uniform vec3 u_Color;

		void main()
		{
			vec3 normal = normalize(v_Normal);

			float diffuse = max(dot(normal, -u_SunDirection), 0.0);
			float dome = 0.5 + 0.5 * normal.y;

			vec3 lit = u_Color * (u_SkyColor * dome * u_Ambient
				+ u_SunColor * diffuse);

			color = vec4(lit, 1.0);
		}
	)";

	m_TreeShader.reset(Egss::Shader::Create("LabTree", vertexSrc, fragmentSrc));
	m_TreeMaterial = Egss::Material::Create(m_TreeShader);
}

// **Water is one quad, and the depth buffer does the rest.**
//
// A lake surface only needs to exist where the ground is below it, and the
// depth buffer already knows where that is: draw a single horizontal plane
// across the whole block at the water level, and every part of it that is
// underground is hidden by the ground standing in front of it. What is left is
// exactly the water in the pit, with a shoreline that follows the terrain to
// the pixel and cost nothing to find.
//
// That is the real reason the pit is a *shape* and not a texture. Given a bowl,
// the water needs no mesh of its own at all -- which is worth remembering when
// this goes back to the planet, where the ocean is currently a whole sphere
// with a wet mask on it.
inline void TerrainLab::BuildWater()
{
	Egss::MeshData plane;

	// Unit square, stretched by the transform, so the voxel slider never needs
	// it rebuilt.
	for (int j = 0; j < 2; j++)
	for (int i = 0; i < 2; i++)
		plane.Vertices.push_back({
			{ (float)i - 0.5f, 0.0f, (float)j - 0.5f },
			{ 0.0f, 1.0f, 0.0f },
			{ (float)i, (float)j } });

	plane.Indices = { 0, 2, 1, 1, 2, 3 };

	Egss::Submesh all;
	all.IndexCount = 6;
	plane.Submeshes.push_back(all);
	plane.RecalculateBounds();

	m_Water = std::make_shared<Egss::Mesh>(plane, "LabWater");

	std::string waterVertex = R"(
		#version 330 core

		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;

		out vec3 v_World;

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			v_World = world.xyz;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string waterFragment = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_World;

		uniform vec3 u_Eye;
		uniform vec3 u_SunDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform vec3 u_Shallow;
		uniform vec3 u_Deep;
		uniform float u_Time;
		uniform vec3 u_Wind;

		void main()
		{
			vec3 view = normalize(u_Eye - v_World);

			// Two crossed ripples travelling with the wind. Not a wave model --
			// there is no displacement -- but enough that the highlight breaks
			// up instead of sitting on the plane as one disc.
			vec2 drift = u_Wind.xz * u_Time * 0.15;

			float a = (v_World.x - drift.x) * 1.7 + u_Time * 1.1;
			float b = (v_World.z - drift.y) * 2.3 - u_Time * 0.8;

			vec3 normal = normalize(vec3(cos(a) * 0.06, 1.0, cos(b) * 0.06));

			float facing = clamp(dot(normal, view), 0.0, 1.0);

			// Schlick, with water's 0.02 at normal incidence. It is why a lake
			// is a mirror at a grazing angle and clear straight down, and it
			// doubles as the alpha -- which is what lets the bottom show near
			// the shore without a second pass.
			float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

			// The plane knows nothing about the ground beneath it, so there is
			// no depth to colour by; the view angle stands in for it. That is
			// right at a grazing angle and merely plausible from above, and it
			// is the one thing here the planet's water does better.
			vec3 body = mix(u_Deep, u_Shallow, facing);

			float diffuse = max(dot(normal, -u_SunDirection), 0.0);

			vec3 midway = normalize(-u_SunDirection + view);
			float glint = pow(max(dot(normal, midway), 0.0), 180.0);

			vec3 lit = body * (0.35 * u_SkyColor + 0.65 * u_SunColor * diffuse)
				+ u_SunColor * glint * fresnel * 6.0;

			color = vec4(lit, clamp(0.50 + 0.45 * fresnel, 0.0, 1.0));
		}
	)";

	m_WaterShader.reset(
		Egss::Shader::Create("LabWater", waterVertex, waterFragment));

	m_WaterMaterial = Egss::Material::Create(m_WaterShader);
}

// --- Rendering ---------------------------------------------------------------

inline void TerrainLab::Look(float dt)
{
	auto [mouseX, mouseY] = Egss::Input::GetMousePosition();

	if (m_MouseLook)
	{
		// The step that turns it on has no previous position to subtract, and
		// capturing the cursor moves it -- so skip exactly one sample.
		if (m_MouseSampled)
		{
			m_Yaw += (mouseX - m_LastMouseX) * m_Sensitivity;
			m_Pitch -= (mouseY - m_LastMouseY) * m_Sensitivity;
		}

		m_MouseSampled = true;
	}

	m_LastMouseX = mouseX;
	m_LastMouseY = mouseY;

	if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  m_Yaw -= m_LookRate * dt;
	if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) m_Yaw += m_LookRate * dt;
	if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    m_Pitch += m_LookRate * dt;
	if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  m_Pitch -= m_LookRate * dt;

	m_Pitch = glm::clamp(m_Pitch, -85.0f, 85.0f);
	m_Camera.SetRotation(m_Yaw, m_Pitch);
}

inline void TerrainLab::OnDemoUpdate(Egss::Timestep ts)
{
	m_FrameTime = ts.GetMilliseconds();

	Look((float)ts);

	Egss::RenderCommand::SetClearColor({ 0.50f, 0.62f, 0.75f, 1.0f });
	Egss::RenderCommand::Clear();

	Egss::Renderer::BeginScene(m_Camera);

	glm::vec3 sun = glm::normalize(glm::vec3(-0.4f, -0.72f, -0.45f));

	m_Material->Set("u_SunDirection", sun);
	m_Material->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
	m_Material->Set("u_SkyColor", glm::vec3(0.50f, 0.62f, 0.75f));
	m_Material->Set("u_Ambient", 0.55f);
	// Nine elements, set one at a time: `Material` has no array setter, and
	// GLSL exposes `u_Cells[3]` as its own uniform name, so this needs no
	// engine change to work.
	for (int j = 0; j < s_Grid; j++)
	for (int i = 0; i < s_Grid; i++)
	{
		const Biome& biome = Biomes()[m_Cell[i][j]];

		m_Material->Set("u_Cells[" + std::to_string(j * s_Grid + i) + "]",
			glm::vec3(biome.Moisture, biome.Warmth,
				m_On[i][j] ? 1.0f : 0.0f));
	}

	m_Material->Set("u_CellSize", CellSize());
	m_Material->Set("u_Half", 0.5f * Extent());

	if (m_ShowWireframe)
	{
		Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Line);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);
	}

	for (const auto& entry : m_Chunks)
		Egss::Renderer::Submit(m_Material, entry.second, glm::mat4(1.0f));

	if (m_ShowWireframe)
	{
		Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Fill);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	if (m_ShowGrass && !m_Grass.empty())
	{
		glm::vec2 mean = MeanWind();

		glm::vec3 wind(mean.x, 0.0f, mean.y);

		m_GrassMaterial->Set("u_SunDirection", sun);
		m_GrassMaterial->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_GrassMaterial->Set("u_SkyColor", glm::vec3(0.50f, 0.62f, 0.75f));
		m_GrassMaterial->Set("u_Ambient", 0.55f);
		m_GrassMaterial->Set("u_Wind", wind);
		m_GrassMaterial->Set("u_Time", m_Time);
		m_GrassMaterial->Set("u_Seed", (float)(m_Shape.Seed % 997u));

		// Metres, as a share of how far up the blade a vertex is -- so the cap
		// scales with the blade rather than being one distance for all of them.
		m_GrassMaterial->Set("u_MaxLean", m_GrassHeight * m_MaxLean);

		// Darker at the root than the tip -- see the fragment shader.
		m_GrassMaterial->Set("u_Root", glm::vec3(0.14f, 0.22f, 0.09f));
		m_GrassMaterial->Set("u_Tip", glm::vec3(0.44f, 0.64f, 0.26f));
		m_GrassMaterial->Set("u_Dry", glm::vec3(0.62f, 0.55f, 0.28f));

		glm::vec3 eye = m_Camera.GetPosition();

		for (const auto& entry : m_Grass)
		{
			// The same per-chunk keep the planet uses: both sides of the test
			// constant across a blade, so it can never tear one in half.
			glm::vec3 centre = 0.5f * (entry.second->GetBoundsMin()
				+ entry.second->GetBoundsMax());

			float away = glm::length(centre - eye);

			m_GrassMaterial->Set("u_Keep", glm::mix(1.0f, 0.25f,
				glm::smoothstep(25.0f, 85.0f, away)));

			// Straw or green, from the climate at this chunk's own centre.
			// Per chunk rather than per blade because it is a colour and a
			// 16 m cell is already the resolution the biome has.
			glm::vec2 climate = ClimateAt(centre.x, centre.z);

			m_GrassMaterial->Set("u_Dryness",
				glm::smoothstep(0.58f, 0.30f, climate.x));

			Egss::Renderer::Submit(m_GrassMaterial, entry.second,
				glm::mat4(1.0f));
		}
	}

	// Trees before the water, because they are opaque and the water is not.
	if (m_ShowTrees && !m_Trees.empty())
	{
		glm::vec2 mean = MeanWind();

		m_TreeMaterial->Set("u_SunDirection", sun);
		m_TreeMaterial->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_TreeMaterial->Set("u_SkyColor", glm::vec3(0.50f, 0.62f, 0.75f));
		m_TreeMaterial->Set("u_Ambient", 0.55f);
		m_TreeMaterial->Set("u_Wind", glm::vec3(mean.x, 0.0f, mean.y));
		m_TreeMaterial->Set("u_Time", m_Time);
		m_TreeMaterial->Set("u_Seed", (float)(m_Shape.Seed % 997u));

		glm::vec3 eye = m_Camera.GetPosition();

		int drawn = 0;

		for (const auto& entry : m_Trees)
		for (const Tree& tree : entry.second)
		{
			// **A distance cull, which is all the level of detail a 144 m
			// block needs.** The far corner is 200 m away and a tree is a few
			// hundred triangles, so there is nothing here that a second mesh
			// would save; what there is to save is drawing the ones behind
			// you, and the frustum does that already.
			if (glm::length(tree.At - eye) > m_TreeReach)
				continue;

			glm::mat4 transform =
				glm::rotate(
					glm::translate(glm::mat4(1.0f), tree.At),
					tree.Yaw, glm::vec3(0.0f, 1.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(tree.Scale));

			// A trunk barely moves and a canopy moves a good deal, for the
			// r^4 reason set out in the solar demo's trees: compliance goes as
			// 1/(E I) and I as the fourth power of the section radius.
			m_TreeMaterial->Set("u_Color", glm::vec3(0.29f, 0.21f, 0.14f));
			m_TreeMaterial->Set("u_Compliance", 1.1e-5f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeBark[tree.Shape], transform);

			m_TreeMaterial->Set("u_Color", glm::vec3(0.17f, 0.33f, 0.13f));
			m_TreeMaterial->Set("u_Compliance", 20.0f * 1.1e-5f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeLeaves[tree.Shape], transform);

			drawn++;
		}

		m_TreeCount = drawn;
	}

	// Water last: it is blended, so anything it may sit in front of has to be
	// in the depth buffer already.
	if (HasWater() && m_Water)
	{
		glm::vec2 mean = MeanWind();

		m_WaterMaterial->Set("u_Eye", m_Camera.GetPosition());
		m_WaterMaterial->Set("u_SunDirection", sun);
		m_WaterMaterial->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_WaterMaterial->Set("u_SkyColor", glm::vec3(0.50f, 0.62f, 0.75f));
		m_WaterMaterial->Set("u_Shallow", glm::vec3(0.32f, 0.55f, 0.55f));
		m_WaterMaterial->Set("u_Deep", glm::vec3(0.05f, 0.16f, 0.27f));
		m_WaterMaterial->Set("u_Time", m_Time);
		m_WaterMaterial->Set("u_Wind", glm::vec3(mean.x, 0.0f, mean.y));

		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);

		// No culling: the camera can be under the surface, and a lake seen
		// from below is a thing you should be able to swim up through.
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		glm::mat4 transform = glm::scale(
			glm::translate(glm::mat4(1.0f),
				glm::vec3(0.0f, WaterLevel(), 0.0f)),
			glm::vec3(Extent(), 1.0f, Extent()));

		Egss::Renderer::Submit(m_WaterMaterial, m_Water, transform);

		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	Egss::Renderer::EndScene();
}

inline void TerrainLab::OnDemoImGui()
{
	ImGui::Begin("Terrain lab");

	ImGui::Text("%.2f ms  |  %d ground tris, %d grass tris, %d trees",
		m_FrameTime, m_TriangleCount, m_GrassTriangles, m_TreeCount);

	ImGui::Text("%d x %d x %d chunks, %.0f m across, %.2f m a voxel",
		s_Chunks, s_Chunks, s_Chunks, Extent(), m_Voxel);

	ImGui::TextDisabled("Tab mouse look, WASD walk, LMB dig, RMB add, V noclip");

	// --- Developer tools ---------------------------------------------------

	if (ImGui::CollapsingHeader("Dev tools", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("No clip (V)", &m_NoClip);
		ImGui::TextDisabled("  space/ctrl to rise and sink while it is on");
	}

	// --- The biome grid -----------------------------------------------------
	//
	// **Laid out the way it is on the ground.** The grid is drawn with +z
	// running *down* the panel and +x across it, which is the view from above
	// with north at the top -- so the cell you tick is the cell you can see
	// when you stand in the middle and face +z. Getting that backwards makes
	// every experiment take two tries.
	// --- Generation ---------------------------------------------------------
	//
	// Every one of these rebuilds the field. That is affordable because the
	// block is bounded: 144 m of ground regenerates in a fraction of a second,
	// so a slider can rebuild the world on release rather than asking for a
	// reload -- which is the difference between exploring a parameter and
	// guessing at one.
	if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool changed = false;

		changed |= ImGui::SliderFloat("Feature size", &m_Shape.FeatureSize,
			8.0f, 220.0f, "%.0f m");
		changed |= ImGui::SliderInt("Octaves", &m_Shape.Octaves, 1, 8);
		changed |= ImGui::SliderFloat("Amplitude", &m_Shape.Amplitude,
			1.0f, 90.0f, "%.1f m");
		changed |= ImGui::SliderFloat("Ridged", &m_Shape.Ridged, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Warp", &m_Shape.Warp, 0.0f, 1.0f);

		ImGui::TextDisabled("  0 rolling hills, 1 ridgelines; warp erodes them");

		changed |= ImGui::SliderFloat("Plateau", &m_Shape.Plateau, 0.0f, 0.9f);
		ImGui::TextDisabled("  the continental-shelf control, at walking scale");

		ImGui::Separator();

		changed |= ImGui::SliderFloat("Water pit", &m_Shape.Basin,
			0.0f, 40.0f, "%.1f m deep");
		changed |= ImGui::SliderFloat("Pit radius", &m_Shape.BasinSize,
			10.0f, 200.0f, "%.0f m");

		ImGui::SliderFloat("Water level", &m_WaterFill, 0.0f, 1.0f,
			"%.2f of the pit");

		ImGui::TextDisabled("  a bowl in the ground; the water is one plane");
		ImGui::TextDisabled("  the terrain occludes, so the shore is exact");

		ImGui::Separator();

		changed |= ImGui::SliderFloat("Caves", &m_Shape.CaveStrength,
			0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Cave size", &m_Shape.CaveSize,
			6.0f, 60.0f, "%.0f m");

		int seed = (int)m_Shape.Seed;
		if (ImGui::SliderInt("Seed", &seed, 1, 9999))
		{
			m_Shape.Seed = (unsigned int)seed;
			changed = true;
		}

		changed |= ImGui::SliderFloat("Voxel", &m_Voxel, 0.5f, 3.0f, "%.2f m");

		if (ImGui::Button("Regenerate") || (changed && !ImGui::IsAnyItemActive()))
			Generate();
	}

	if (ImGui::CollapsingHeader("Biomes", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool ground = false;   // needs the field rebuilt
		bool cover = false;    // needs only the grass rebuilt

		ImGui::TextDisabled("Tick to fill a column; untick to leave a hole.");
		ImGui::TextDisabled("Number keys 1-9 stand you in a cell.");

		for (int j = 0; j < s_Grid; j++)
		{
			for (int i = 0; i < s_Grid; i++)
			{
				if (i > 0)
					ImGui::SameLine();

				ImGui::PushID(j * s_Grid + i);

				ImGui::BeginGroup();

				// Ticking a cell adds or removes ground, which is the field.
				if (ImGui::Checkbox("##on", &m_On[i][j]))
					ground = true;

				ImGui::SetNextItemWidth(96.0f);

				// Changing a biome is only climate: the rock is the same rock,
				// so this rebuilds the cover and leaves the field alone.
				if (ImGui::Combo("##biome", &m_Cell[i][j],
					BiomeNames(), s_BiomeCount))
					cover = true;

				ImGui::EndGroup();
				ImGui::PopID();
			}
		}

		ImGui::Separator();

		cover |= ImGui::Checkbox("Trees", &m_ShowTrees);
		cover |= ImGui::SliderFloat("Trees per m^2", &m_TreeDensity,
			0.0f, 0.12f, "%.3f");
		ImGui::SliderFloat("Tree reach", &m_TreeReach, 20.0f, 220.0f, "%.0f m");
		ImGui::TextDisabled("  trees want it wetter than grass does, so a wood"
			" ends inside a transition");

		ImGui::Separator();

		cover |= ImGui::Checkbox("Grass", &m_ShowGrass);
		cover |= ImGui::SliderFloat("Blades per m^2", &m_GrassDensity,
			0.0f, 200.0f, "%.0f");
		cover |= ImGui::SliderFloat("Blade height", &m_GrassHeight,
			0.1f, 1.5f, "%.2f m");

		ImGui::SliderFloat("Wind", &m_WindSpeed, 0.0f, 30.0f, "%.1f m/s");
		ImGui::SliderFloat("Wind from", &m_WindAngle, 0.0f, 360.0f, "%.0f deg");
		ImGui::SliderFloat("Max lean", &m_MaxLean, 0.0f, 1.2f, "%.2f of length");
		ImGui::TextDisabled("  gusts and eddies are noise; the slider is the mean");

		if (ground && !ImGui::IsAnyItemActive())
			Generate();
		else if (cover && !ImGui::IsAnyItemActive())
			RebuildGrass();
	}

	ImGui::Checkbox("Wireframe", &m_ShowWireframe);

	ImGui::End();
}
