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
		// Wet to dry across, cold to warm down, so every neighbouring pair is
		// a transition -- which is the thing the demo is for looking at.
		static const int opening[s_Grid][s_Grid] =
		{
			{ 2, 1, 0, 3 },   // wetland, forest, meadow,  steppe
			{ 1, 0, 3, 4 },   // forest,  meadow, steppe,  desert
			{ 0, 3, 4, 4 },   // meadow,  steppe, desert,  desert
			{ 5, 5, 3, 6 },   // tundra,  tundra, steppe,  bare
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
		// `--time 0.5` puts the sun overhead for a capture. Unattended runs
		// have nobody to move a slider, and comparing a dawn against a noon is
		// the first thing anyone wants of a day cycle.
		const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

		// `--spawn N` stands at the centre of cell N. The spawn buttons are
		// the only way to be somewhere specific, and an unattended run has
		// nobody to press one -- which made "does this look the same from the
		// corner as from the middle" a question no capture could ask.
		int spawn = 1;

		for (size_t i = 1; i + 1 < arguments.size(); i++)
		{
			if (arguments[i] == "--time")
				m_TimeOfDay = (float)std::atof(arguments[i + 1].c_str());

			if (arguments[i] == "--spawn")
				spawn = std::atoi(arguments[i + 1].c_str());
		}

		// `--portal` plants the doorway on the first step, so a capture can
		// look through one. It is the only way to: deploying is a key press,
		// and an unattended run has nobody to press it.
		for (const std::string& argument : arguments)
			if (argument == "--portal")
				m_DeployOnStart = true;

		BuildShaders();
		Generate();

		// A rim cell rather than the middle one: the pit is centred, so the
		// middle cell is the bottom of the lake and opens you underwater.
		// From the rim you are looking across it.
		GoTo(spawn);
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

	// **The grid has nothing to do with the chunks.** `CellAt` is purely
	// positional, so the two numbers are free of each other entirely -- which
	// is what lets the grid be refined without touching the terrain, and what
	// made this change one constant.
	//
	// Four by four over the same 144 m block: sixteen cells of 36 m. That is
	// "nine more" read as *roughly double*, kept square because a rectangular
	// grid of eighteen would put the panel out of shape and make the layout
	// stop matching the ground. One line to change if a finer grid is wanted;
	// the only cost is that a cell smaller than about 20 m stops reading as a
	// place and starts reading as a patch.
	static constexpr int s_Grid = 4;

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
		ScatterLoose();
		BuildShed();
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

	// --- Sky ------------------------------------------------------------------
	//
	// **A day, as one angle.** Everything below reads `m_TimeOfDay` -- where
	// the sun is, what colour its light is, how bright the sky is, which way
	// the moon sits and whether it is up at all. One slider moves all of it,
	// and nothing has its own idea of what time it is.
	//
	// This is the solar demo's sky reduced to what a flat 144 m block needs. It
	// keeps the parts that carry the look -- a sun that reddens at the horizon,
	// a sky that darkens with it, haze that thickens with distance, a moon
	// opposite the sun -- and drops the parts that only mean anything on a
	// sphere, which is the raymarched shell and the curvature.

	// Where the sun is, as a unit vector. Noon is overhead; the day runs
	// east to west through it.
	glm::vec3 SunDirection() const
	{
		// 0 is midnight, 0.5 is noon. Rising in the east means the sun's
		// azimuth is fixed here and only its elevation moves, which for a
		// block this size is the whole of what anyone can see.
		float angle = (m_TimeOfDay - 0.25f) * 6.2831853f;

		return glm::normalize(glm::vec3(
			std::cos(angle) * 0.55f, std::sin(angle), 0.35f));
	}

	// The moon rides opposite, so it is up when the sun is not.
	glm::vec3 MoonDirection() const
	{
		return -SunDirection();
	}

	// **How high the sun is, which decides everything else.** Clamped at zero
	// rather than allowed negative: below the horizon it stops contributing
	// rather than contributing backwards.
	float SunHeight() const
	{
		return glm::clamp(SunDirection().y, 0.0f, 1.0f);
	}

	// **Sunlight reddens because the blue has been scattered out of it.**
	//
	// The path through the air is longest at the horizon, and scattering goes
	// as 1/lambda^4, so blue leaves the beam first. That is one expression
	// rather than a sunset colour anyone picked: attenuate each channel by its
	// own optical depth over a path that grows as the sun sets.
	glm::vec3 SunColour() const
	{
		float height = SunHeight();

		// Air mass, near enough: one at the zenith, rising sharply at the
		// horizon. The 0.06 keeps it finite when the sun is exactly level.
		float path = 1.0f / glm::max(height, 0.06f);

		glm::vec3 scatter(0.22f, 0.45f, 1.00f);   // the Rayleigh ratio

		glm::vec3 through = glm::exp(-scatter * (path - 1.0f) * m_Haze * 320.0f);

		return through * m_SunBrightness;
	}

	// The sky is what was scattered *out* of that beam, so it is the same
	// numbers the other way up -- bright blue overhead in the day, and near
	// black with a little starlight at night.
	glm::vec3 SkyColour() const
	{
		float height = SunHeight();

		glm::vec3 day(0.42f, 0.58f, 0.86f);
		glm::vec3 dusk(0.52f, 0.36f, 0.30f);
		glm::vec3 night(0.02f, 0.03f, 0.07f);

		// Dusk is a narrow band round the horizon, so it is a separate mix
		// rather than a point on one gradient -- a single ramp from day to
		// night passes through grey and never through orange.
		float lit = glm::smoothstep(0.0f, 0.35f, height);
		float low = glm::smoothstep(0.30f, 0.02f, height)
			* glm::smoothstep(-0.25f, 0.02f, SunDirection().y);

		glm::vec3 sky = glm::mix(night, day, lit);

		return glm::mix(sky, dusk, low * 0.75f);
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

		m_ShapesOn = 0;

		for (int k = 0; k < s_TreeShapes; k++)
			m_ShapesOn += m_ShapeOn[k] ? 1 : 0;

		if (!m_ShowTrees || m_TreeDensity <= 0.0f || m_ShapesOn == 0
			|| data.Indices.size() < 3)
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
				// Chosen from the shapes that are switched on, so the panel
				// can isolate one habit and look at a wood made only of it.
				int wanted = (int)(Veg::Hash2DUnit((int)t, i * 4 + 5, seed)
					* (float)m_ShapesOn) % glm::max(m_ShapesOn, 1);

				tree.Shape = 0;

				for (int k = 0, seen = 0; k < s_TreeShapes; k++)
					if (m_ShapeOn[k] && seen++ == wanted)
					{
						tree.Shape = k;
						break;
					}

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

	// --- Loose bodies and buoyancy --------------------------------------------

	struct Loose
	{
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		int Shape = 0;
		float Size = 1.0f;
		bool Floats = false;
	};

	// **Archimedes, and nothing else.** The upward force on a submerged body is
	// the weight of the fluid it displaces -- `rho g V` -- and whether a thing
	// floats is therefore whether its own density is under the water's. That is
	// the entire model, and it is worth having as the entire model because it
	// means a boat will float for the same reason a log does, and neither needs
	// a flag saying so.
	//
	// The displaced volume is the part of the body under the surface. Boxes and
	// spheres both get the same treatment: take how far the body's centre is
	// below the waterline, in units of its own half-height, and clamp -- so
	// nothing submerged is zero, half in is a half, and fully under is one.
	// That is exact for a box and within a few per cent for a sphere over most
	// of the range, and the error is smaller than the wave height.
	void ApplyBuoyancy()
	{
		if (!HasWater())
			return;

		const float water = 1000.0f;   // kg/m^3

		float level = WaterLevel();

		for (const Loose& loose : m_Loose)
		{
			Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			if (body.InverseMass <= 0.0f)
				continue;

			float half = loose.Size;

			float under = glm::clamp((level - (body.Position.y - half))
				/ (2.0f * half), 0.0f, 1.0f);

			if (under <= 0.0f)
				continue;

			// A cube of side 2r for a box, a sphere of radius r otherwise --
			// close enough that the density printed on the panel is the
			// density that decides whether it swims.
			float volume = loose.Shape == 0
				? (4.0f / 3.0f) * glm::pi<float>() * half * half * half
				: 8.0f * half * half * half;

			float mass = 1.0f / body.InverseMass;

			// **A sleeping body ignores forces, and buoyancy is a force.**
			//
			// The solver puts a body that has stopped moving to sleep, which
			// is right -- a boulder resting on a hillside should not be
			// integrated for ever. But a log that fell into the lake, hit the
			// bed and slept there stays asleep no matter how hard the water
			// pushes up on it, because the push is applied to a body that is
			// not being stepped. It read as buoyancy being too weak: the log
			// sat at 0.276 submerged with its own weight measurably greater
			// than the force lifting it, which is not an equilibrium at all
			// and was the tell.
			//
			// Anything with water on it is awake by definition.
			body.Awake = true;

			// Up is the weight of the water pushed aside.
			m_World.ApplyForce(loose.Body,
				glm::vec3(0.0f, water * 9.81f * volume * under, 0.0f));

			// **And water is thick.** Without drag a floating body is a
			// spring with no damper: it overshoots the surface, leaves the
			// water, falls back and bobs for ever. Quadratic, like the air
			// drag, but with a thousand times the density behind it -- which
			// is why a log bobs twice and settles.
			glm::vec3 relative = -body.Velocity;
			float speed = glm::length(relative);

			if (speed > 1e-4f)
			{
				float area = glm::pi<float>() * half * half;

				// **No fudge factor.** This had a `* 0.02` on it, which took a
				// force of eight thousand newtons down to a hundred and sixty
				// against a weight of ninety-six thousand -- so the log bobbed
				// for ever and was still moving at 1.78 m/s when it was
				// measured, which read as buoyancy being too weak. It was not:
				// the lift was right and the damping was crippled.
				//
				// `1/2 rho Cd A v^2` with water's own density behind it is
				// large, and it should be. That is why a log dropped in a lake
				// bobs twice and settles rather than oscillating like a spring.
				m_World.ApplyForce(loose.Body, relative
					* (0.5f * water * 0.9f * area * speed * under));
			}

			// Torque is not modelled: a real hull rights itself because its
			// centre of buoyancy moves as it heels, and that needs the
			// displaced *shape* rather than its volume. Worth knowing before
			// anyone puts a mast on one of these.
			(void)mass;
		}
	}

	// Boulders, and a few things light enough to swim. Both go through the same
	// list because the only difference between them is density -- which is the
	// point of doing buoyancy properly rather than tagging things as floaty.
	void ScatterLoose()
	{
		m_Loose.clear();

		unsigned int seed = 20261u;

		for (int i = 0; i < m_LooseCount; i++)
		{
			float u = Veg::Hash2DUnit(i, 1, seed);
			float v = Veg::Hash2DUnit(i, 2, seed);

			float half = 0.5f * Extent();

			glm::vec3 at((u - 0.5f) * Extent() * 0.8f, 0.0f,
				(v - 0.5f) * Extent() * 0.8f);

			at.y = Height(at.x, at.z) + 6.0f + Veg::Hash2DUnit(i, 3, seed) * 8.0f;

			(void)half;

			Loose loose;

			// A third of them are driftwood -- 500 kg/m^3, which is oak, and
			// which floats about half out of the water. The rest are granite
			// at 2650 and do not.
			loose.Floats = Veg::Hash2DUnit(i, 4, seed) < 0.34f;
			loose.Size = 0.5f + Veg::Hash2DUnit(i, 5, seed) * 0.9f;
			loose.Shape = loose.Floats ? 1 : 0;

			float density = loose.Floats ? 500.0f : 2650.0f;

			float volume = loose.Shape == 0
				? (4.0f / 3.0f) * glm::pi<float>() * loose.Size * loose.Size * loose.Size
				: 8.0f * loose.Size * loose.Size * loose.Size;

			float mass = density * volume;

			Egss::RigidBody3D body = loose.Shape == 0
				? Egss::RigidBody3D::MakeSphere(at, loose.Size, mass)
				: Egss::RigidBody3D::MakeBox(at, glm::vec3(loose.Size), mass);

			body.Friction = 0.6f;
			body.Restitution = 0.05f;

			// **Air drag stays off.** These are metres across and the demo has
			// no wind force on solids; leaving the default damping on would
			// slow a falling boulder for no stated reason.
			body.LinearDamping = 0.0f;
			body.AngularDamping = 0.02f;

			loose.Body = m_World.AddBody(body);

			m_Loose.push_back(loose);
		}
	}

	// --- The portal and the toolshed ------------------------------------------
	//
	// **A door you can carry, and a room that is not where the door is.**
	//
	// The shed is built once, four hundred metres below the block, and stays
	// there. Deploying the portal does not create it -- it creates a *way in*.
	// That is the whole idea of a pocket dimension and it is worth building the
	// cheap version first: the room is somewhere the terrain is not, so it
	// needs no hole cut in the ground, no clipping against the world, and its
	// floor is a box rather than a heightfield.
	//
	// **Crossing is a plane test, not a trigger volume.** A box you must be
	// inside for a frame can be stepped through at speed -- the player is at
	// six metres a second and a fixed step is a sixtieth of a second, so a
	// half-metre trigger is missed one time in five. Testing which *side* of
	// the doorway the player was on last step and is on now cannot be outrun,
	// because the two positions bracket the crossing however fast it happened.

	// **Above the block, not below it, and the reason is the ground collider.**
	//
	// The shed was first put 400 m *under* the terrain, which placed the player
	// correctly and then shoved them upward at ninety-five metres a step. The
	// ground is an SDF collider over the voxel field, and the field is only
	// defined across the block: below it every query reads as solid, so the
	// solver was doing exactly its job -- pushing a body out of rock that goes
	// down for ever.
	//
	// Above the field the same query reads as air, so a room up there is left
	// alone. The asymmetry is in the field, not in the collider, and putting
	// the pocket dimension in the sky costs nothing -- nobody can see it, which
	// is rather the point of a pocket dimension.
	static constexpr float s_ShedDrop = -400.0f;
	static constexpr float s_ShedHalf = 4.0f;     // the room is 8 m square
	static constexpr float s_DoorHalf = 1.1f;     // half the doorway's width

	// **The two openings are the same size, and they have to be.** The panel
	// in the world frame is a window onto the shed's own doorway, so if one is
	// taller than the other the difference shows as a strip of the shed's
	// lintel hanging in mid-air above the frame. One constant for both.
	static constexpr float s_DoorTop = 2.45f;

	glm::vec3 ShedCentre() const
	{
		return glm::vec3(0.0f, -s_ShedDrop, 0.0f);
	}

	// The doorway inside the shed, which is the way back out.
	glm::vec3 ShedDoor() const
	{
		return ShedCentre() + glm::vec3(0.0f, 0.0f, -s_ShedHalf);
	}

	void BuildShed()
	{
		glm::vec3 centre = ShedCentre();

		// Floor and four walls as static boxes. The doorway is a gap in one
		// wall rather than a hole in a mesh: two short walls either side of it,
		// so the collider and the thing you can see through are the same shape
		// and there is nothing to keep in step.
		auto wall = [&](const glm::vec3& at, const glm::vec3& half)
		{
			Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(at, half, 0.0f);
			body.Type = Egss::BodyType::Static;
			body.Friction = 0.7f;

			m_World.AddBody(body);
		};

		const float h = s_ShedHalf;

		wall(centre + glm::vec3(0.0f, -0.25f, 0.0f), { h, 0.25f, h });
		wall(centre + glm::vec3(0.0f, 3.25f, 0.0f), { h, 0.25f, h });
		wall(centre + glm::vec3(-h, 1.5f, 0.0f), { 0.25f, 1.75f, h });
		wall(centre + glm::vec3(h, 1.5f, 0.0f), { 0.25f, 1.75f, h });
		wall(centre + glm::vec3(0.0f, 1.5f, h), { h, 1.75f, 0.25f });

		// The wall with the door in it: two posts and a lintel.
		float side = 0.5f * (h - s_DoorHalf);

		wall(centre + glm::vec3(-(s_DoorHalf + side), 1.5f, -h),
			{ side, 1.75f, 0.25f });
		wall(centre + glm::vec3(s_DoorHalf + side, 1.5f, -h),
			{ side, 1.75f, 0.25f });
		wall(centre + glm::vec3(0.0f, 2.85f, -h), { s_DoorHalf, 0.4f, 0.25f });
	}

	// **A doorway is a place and a heading, and a portal is one rigid map
	// between two of them.**
	//
	// Take a point relative to the door you are leaving, turn it by the
	// difference between the two doors' headings, and set it down beside the
	// door you are arriving at. That is the whole of it, and writing it once
	// is the point: the player's teleport and the second camera that draws
	// what is on the other side are *the same transform*, so the picture in
	// the doorway cannot disagree with where you end up. A portal where those
	// two differ reads as a trick rather than as a place.
	struct Doorway
	{
		glm::vec3 At;
		float Yaw;
	};

	// The heading is the direction you are *travelling* when you go through,
	// not some fixed front. That matters, because the world door is a free-
	// standing frame you can walk round and the shed's door is a hole in a
	// wall: a pure rigid map from one to the other sends anyone who approaches
	// the frame from behind out into four hundred metres of empty air on the
	// wrong side of the shed. Choosing the heading to match the crossing means
	// you always arrive *inside the room*, whichever face of the frame you
	// walked at -- and the return trip inverts the same map, so you come back
	// out walking the way you came in, which is what a door does.
	static glm::vec3 Facing(float yaw)
	{
		return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw));
	}

	static glm::mat3 DoorTurn(const Doorway& from, const Doorway& to)
	{
		return glm::mat3(glm::rotate(glm::mat4(1.0f), to.Yaw - from.Yaw,
			glm::vec3(0.0f, 1.0f, 0.0f)));
	}

	// Which side of a doorway a point is on. The doorway faces +z in its own
	// frame; `yaw` turns it.
	static float DoorSide(const glm::vec3& at, const glm::vec3& door, float yaw)
	{
		return glm::dot(at - door, Facing(yaw));
	}

	// True if the point is within the doorway's opening, ignoring which side.
	static bool ThroughOpening(const glm::vec3& at, const glm::vec3& door,
		float yaw)
	{
		glm::vec3 facing = Facing(yaw);
		glm::vec3 across(facing.z, 0.0f, -facing.x);

		glm::vec3 offset = at - door;

		return std::abs(glm::dot(offset, across)) < s_DoorHalf
			&& offset.y > -0.4f && offset.y < s_DoorTop + 0.15f;
	}

	// The doorway you would leave by, given which side of the world door you
	// are on: its heading is the way you would be walking if you crossed.
	Doorway WorldSideDoor(const glm::vec3& at) const
	{
		float side = DoorSide(at, m_PortalAt, m_PortalYaw);

		return { m_PortalAt, side < 0.0f ? m_PortalYaw
			: m_PortalYaw + glm::pi<float>() };
	}

	// Inside the room there is one way out, so the heading is fixed: the door
	// is in the -z wall and you leave through it going -z.
	Doorway ShedSideDoor() const
	{
		return { ShedDoor(), glm::pi<float>() };
	}

	// Where the world door puts you back down, which is the reverse of the
	// crossing that brought you in.
	Doorway ReturnDoor() const
	{
		return { m_PortalAt, m_EntryYaw + glm::pi<float>() };
	}

	void TogglePortal()
	{
		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		if (m_PortalOn)
		{
			// Picked up only from close by, or E anywhere in the world
			// silently pockets a door you left on the far side of the block.
			if (glm::length(body.Position - m_PortalAt) < 6.0f)
				m_PortalOn = false;

			return;
		}

		// Deployed at your feet, facing the way you came from -- so the first
		// thing you do after planting it is walk forward through it.
		glm::vec3 forward = m_Camera.GetForward();
		forward.y = 0.0f;

		if (glm::length(forward) < 1e-4f)
			return;

		forward = glm::normalize(forward);

		m_PortalAt = body.Position - glm::vec3(0.0f,
			s_WalkerHalfHeight + s_WalkerRadius, 0.0f) + forward * 2.2f;

		m_PortalAt.y = Height(m_PortalAt.x, m_PortalAt.z);

		m_PortalYaw = std::atan2(forward.x, forward.z);
		m_PortalOn = true;

		m_PortalSide = DoorSide(body.Position, m_PortalAt, m_PortalYaw);
	}

	// Put the player through, by the same map the camera looks through. The
	// step past the plane is along the destination's own heading, so it is
	// always *onward* rather than a fixed offset that could land behind the
	// door and re-trigger on the next step.
	void StepThrough(const Doorway& from, const Doorway& to)
	{
		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		glm::mat3 turn = DoorTurn(from, to);

		body.Position = to.At + turn * (body.Position - from.At)
			+ Facing(to.Yaw) * 1.1f;

		body.Velocity = glm::vec3(0.0f);
		body.Awake = true;

		// **And the head turns with the body.** Leaving this out is what made
		// the shed hard to get out of: you were set down a metre in front of
		// the doorway still looking whichever way you had been looking inside,
		// which is usually straight back at the door -- so a step forward put
		// you back in the room, over and over. The view has to go through the
		// same rotation as the position or the two describe different portals.
		glm::vec3 forward = turn * m_Camera.GetForward();

		m_Yaw = glm::degrees(std::atan2(forward.z, forward.x));

		m_Camera.SetRotation(m_Yaw, m_Pitch);
		m_Camera.SetPosition(body.Position
			+ glm::vec3(0.0f, s_EyeHeight, 0.0f));
	}

	// Called every fixed step, after the walker has moved.
	void StepPortal()
	{
		if (!m_PortalOn)
			return;

		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		if (!m_InShed)
		{
			float side = DoorSide(body.Position, m_PortalAt, m_PortalYaw);

			// **A sign change, in either direction.**
			//
			// This tested `was > 0 && is <= 0` -- front to back only -- and
			// the portal is planted two metres *in front* of you, so you begin
			// behind it and walk the other way. It never fired once. A door is
			// a door from both sides anyway, and a sign change says "crossed"
			// without caring which way.
			if ((m_PortalSide > 0.0f) != (side > 0.0f)
				&& ThroughOpening(body.Position, m_PortalAt, m_PortalYaw))
			{
				// The heading is the way you were going: from behind the
				// frame that is `m_PortalYaw`, from in front of it the
				// opposite. Either way it maps to walking into the room.
				Doorway from{ m_PortalAt, m_PortalSide < 0.0f ? m_PortalYaw
					: m_PortalYaw + glm::pi<float>() };

				m_EntryYaw = from.Yaw;

				StepThrough(from, { ShedDoor(), 0.0f });

				m_InShed = true;
				m_ShedSide = DoorSide(body.Position, ShedDoor(), 0.0f);
			}

			m_PortalSide = side;

			return;
		}

		float side = DoorSide(body.Position, ShedDoor(), 0.0f);

		if ((m_ShedSide > 0.0f) != (side > 0.0f)
			&& ThroughOpening(body.Position, ShedDoor(), 0.0f))
		{
			StepThrough(ShedSideDoor(), ReturnDoor());

			m_InShed = false;
			m_PortalSide = DoorSide(body.Position, m_PortalAt, m_PortalYaw);
		}

		m_ShedSide = side;
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
	void BuildSky();

	// --- Hooks --------------------------------------------------------------

	// **Input is polled on the fixed step, not handled as events.**
	//
	// This demo had digging on a `MouseButtonPressedEvent` and it did not
	// work. Two reasons, and the second is the one that matters: an event can
	// be consumed before it reaches a demo layer, and -- much worse --
	// **events are not in the replay stream**. `Egss::Input` is, so a session
	// polled here records and plays back and one handled from events does not.
	// `VoxelTerrain` has done it this way since it was written and says so in
	// a comment; the lab simply did not follow it.
	//
	// Everything that edits or teleports goes through here for that reason.
	// Looking around stays in `Look`, which is also polled.
	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		bool toggle = Egss::Input::IsKeyPressed(EGSS_KEY_TAB);

		if (toggle && !m_WasToggling)
			SetMouseLook(!m_MouseLook);

		m_WasToggling = toggle;

		bool portal = Egss::Input::IsKeyPressed(EGSS_KEY_E);

		if (portal && !m_WasPortal)
			TogglePortal();

		m_WasPortal = portal;

		bool clip = Egss::Input::IsKeyPressed(EGSS_KEY_V);

		if (clip && !m_WasClipping)
			m_NoClip = !m_NoClip;

		m_WasClipping = clip;

		// The number row stands you in a cell. Edge-triggered, or holding a
		// key respawns you every step and you never fall.
		for (int i = 0; i < s_Grid * s_Grid; i++)
		{
			bool down = Egss::Input::IsKeyPressed(EGSS_KEY_1 + i);

			if (down && !m_WasSpawning[i])
				GoTo(i);

			m_WasSpawning[i] = down;
		}

		// **One edit per press, not per step.** Holding the button otherwise
		// hollows the block out in a second, which reads as the dig radius
		// being enormous rather than as the edit repeating.
		bool dig = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
		bool add = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_RIGHT);

		if (dig && !m_WasDigging)
			Dig(false);
		else if (add && !m_WasAdding)
			Dig(true);

		m_WasDigging = dig;
		m_WasAdding = add;

		ApplyBuoyancy();

		MoveWalker(step);
		m_World.Step(step);

		if (m_DeployOnStart)
		{
			m_DeployOnStart = false;
			TogglePortal();
		}

		StepPortal();

		m_Time += (float)step;
	}

	void OnDemoUpdate(Egss::Timestep ts) override;
	void OnDemoImGui() override;

	// Which side of the doorway is being drawn. See the note on `DrawScene`.
	enum class Pass { Main, ToShed, ToWorld };

	void DrawScene(const Egss::PerspectiveCamera& camera, Pass pass);
	void DrawPortalView();
	void DrawShed();
	void DrawDoorFrame();

	// The unit cube, placed and coloured. The doorway, its frame and the whole
	// shed are boxes, and none of them is worth a mesh of its own.
	void DrawBox(const glm::vec3& at, const glm::vec3& half, float yaw,
		const glm::vec3& colour);

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

	std::vector<Loose> m_Loose;
	std::shared_ptr<Egss::Mesh> m_Boulder;
	std::shared_ptr<Egss::Mesh> m_Cube;
	int m_LooseCount = 26;

	static constexpr int s_TreeShapes = 6;

	std::shared_ptr<Egss::Mesh> m_TreeBark[s_TreeShapes];
	std::shared_ptr<Egss::Mesh> m_TreeLeaves[s_TreeShapes];

	std::shared_ptr<Egss::Shader> m_TreeShader;
	std::shared_ptr<Egss::Material> m_TreeMaterial;

	bool m_ShowTrees = true;
	float m_TreeDensity = 0.012f;   // trees per square metre where fully wooded
	int m_TreeCount = 0;
	float m_TreeReach = 130.0f;
	float m_TreeMaxLean = 0.10f;

	// Which habits are in the mix. All on is a mixed wood; one on is a stand
	// of that shape, which is how you tell whether it is any good.
	bool m_ShapeOn[s_TreeShapes] = { true, true, true, true, true, true };
	int m_ShapesOn = s_TreeShapes;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;

	std::shared_ptr<Egss::Shader> m_GrassShader;
	std::shared_ptr<Egss::Material> m_GrassMaterial;

	std::shared_ptr<Egss::Mesh> m_Sky;
	std::shared_ptr<Egss::Shader> m_SkyShader;
	std::shared_ptr<Egss::Material> m_SkyMaterial;

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

	// Where the thinning begins and ends, and how soft the edge of it is.
	// `Band` is in ticket space: 0 is a hard cut and 0.5 means half the field
	// is part-grown at any distance.
	float m_LodNear = 20.0f;
	float m_LodFar = 75.0f;
	float m_LodBand = 0.30f;

	// The sky. One angle for the day, and two knobs for how thick the air is.
	float m_TimeOfDay = 0.34f;      // 0 midnight, 0.5 noon
	float m_DayLength = 0.0f;       // turns a second; 0 holds the time still
	float m_Haze = 0.0022f;         // per metre, before the air-mass term
	float m_SunBrightness = 1.25f;
	bool m_ShowSky = true;
	bool m_ShowClouds = true;
	float m_CloudCover = 0.45f;
	float m_CloudHeight = 90.0f;

	// How full the basin is, from dry to level with the rim.
	float m_WaterFill = 0.55f;

	// Swell in the geometry, ripples in the normal. Both switchable, because
	// whether either reads as water or as noise depends on the wind.
	bool m_Waves = true;
	bool m_Ripples = false;

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
	bool m_WasToggling = false;
	bool m_WasClipping = false;
	bool m_WasDigging = false;
	bool m_WasAdding = false;
	bool m_WasSpawning[s_Grid * s_Grid] = {};
	bool m_WasPortal = false;

	bool m_PortalOn = false;
	bool m_InShed = false;
	glm::vec3 m_PortalAt = glm::vec3(0.0f);
	float m_PortalYaw = 0.0f;
	float m_PortalSide = 0.0f;
	float m_ShedSide = 0.0f;

	// The heading of the crossing that took you in, kept so the way out is the
	// inverse of the way in rather than a second guess at it.
	float m_EntryYaw = 0.0f;

	// The second camera. Off is the old flat board, which is worth keeping
	// because it costs nothing and the portal pass draws the scene twice.
	bool m_SeeThrough = true;

	// Set by `--portal`; acted on at the first fixed step, because deploying
	// wants the camera to be pointing somewhere and it is not until then.
	bool m_DeployOnStart = false;

	std::shared_ptr<Egss::Framebuffer> m_PortalTarget;
	std::shared_ptr<Egss::Texture2D> m_PortalTexture;
	std::shared_ptr<Egss::Shader> m_PortalShader;
	std::shared_ptr<Egss::Material> m_PortalMaterial;
	glm::vec2 m_PortalSize = glm::vec2(0.0f);
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

	// **The grid's size lives in one place and reaches the shader from it.**
	//
	// It did not, and that cost an afternoon. `s_Grid` went from three to four
	// and this GLSL kept its hard-coded 3, so the ground read
	// `u_Cells[j * 3 + i]` out of a nine-element array the CPU had already
	// started filling as `j * 4 + i`. Seven of the sixteen writes landed past
	// the end and went nowhere; the nine that landed were shuffled into the
	// wrong squares.
	//
	// The symptom did not look like an indexing bug at all -- it looked like
	// grass growing on sand. The *grass* asks `ClimateAt` on the CPU and the
	// *ground* asked this scrambled copy of it, so the two stopped describing
	// the same world: a cell painted desert was a cell the scatterer thought
	// was meadow.
	//
	// A constant written out twice will eventually be wrong in one of the two
	// places. Injecting it as a `#define` is three lines and closes the whole
	// class of fault.
	std::string fragmentSrc = "#version 330 core\n#define GRID "
		+ std::to_string(s_Grid) + "\n" + R"(

		layout(location = 0) out vec4 color;

		in vec3 v_World;
		in vec3 v_Normal;

		uniform vec3 u_SunDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform float u_Ambient;
		uniform vec3 u_Eye;
		uniform float u_Haze;

		// **The grid, and the blend done per pixel.**
		//
		// One (moisture, warmth) per cell, read back bilinearly at the
		// fragment's own position -- the same expression `ClimateAt` uses on
		// the CPU, because the grass has to agree with the ground it grows
		// out of. A few vec3s are cheaper than a texture and need no upload
		// path; the third component is the cell's weight, which is zero for an
		// unticked cell so a hole does not dry out its neighbours. `GRID`
		// comes from `s_Grid` -- see the note where it is injected.
		uniform vec3 u_Cells[GRID * GRID];
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
				ivec2 g = ivec2(clamp(base + vec2(di, dj), vec2(0.0),
					vec2(float(GRID - 1))));

				vec3 cell = u_Cells[g.y * GRID + g.x];

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

			// **Haze, after the lighting and not before it.** This is air
			// between the eye and the ground, not a property of the ground --
			// so it sits outside the `base * (...)` term, or a shaded slope
			// and a lit one would haze by different amounts for no physical
			// reason. Same `1 - exp(-x)` extinction the sky is built from.
			float away = length(v_World - u_Eye);

			lit = mix(lit, u_SkyColor, 1.0 - exp(-away * u_Haze));

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
		uniform vec3 u_Eye;
		uniform float u_LodNear;
		uniform float u_LodFar;
		uniform float u_LodBand;
		uniform float u_GrassHeight;
		uniform float u_Seed;
		uniform float u_MaxLean;
		uniform float u_Fade;

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

			// **Level of detail that shrinks blades rather than dropping
			// them, so there is no line anywhere.**
			//
			// The old version was a hard test against a threshold held in a
			// uniform, set once per chunk. Both sides being constant across a
			// blade is what stopped it tearing blades in half -- but a value
			// that is constant per *chunk* changes in a step at every chunk
			// boundary, and sixteen metres is large enough to see: the field
			// thinned in visible straight lines and then stopped dead.
			//
			// Two changes fix it, and the second is what makes the first safe.
			// The distance is measured per vertex now, so it varies smoothly
			// across the whole field and there are no boundaries in it at all.
			// And a blade that loses the lottery is not discarded -- it is
			// *shrunk*, pulled down toward its own base over a band of
			// tickets, so a blade caught halfway is simply a shorter blade.
			//
			// That is the part that matters. A discard has to be all-or-
			// nothing across five vertices or the triangle between them is
			// stretched across the screen; a shrink does not, because a
			// slightly different shrink at each vertex is a slightly different
			// blade and not a defect. So the per-vertex distance that would
			// have torn the old scheme is harmless in this one.
			//
			// Pulling *down along the blade's own height* is what collapses it
			// to its base without knowing where its base is: each vertex moves
			// by its own share of the height, so the root (share zero) does not
			// move and the tip closes onto it.
			// **Distance from the eye, and it has to be said out loud
			// because it was not.** This read `length(world.xyz)` -- the
			// distance from the world *origin*. The block is centred there,
			// so the thinning was a fixed bullseye painted on the terrain:
			// full-height blades in the middle of the map and collapsed ones
			// at its corners, no matter where you were standing. Walking
			// around changed nothing, which is exactly what "the culling is
			// not in real time" looks like from inside it, and moving the
			// sliders only resized a ring you were probably not in.
			//
			// The per-chunk `u_Fade` beside this was measured from the camera
			// all along, so the shading converged with distance while the
			// geometry thinned by map position -- two level-of-detail schemes
			// disagreeing about where the viewer was.
			float range = length(world.xyz - u_Eye);

			float keep = 1.0 - smoothstep(u_LodNear, u_LodFar, range);

			// A soft band rather than a step: over `u_LodBand` of ticket space
			// the blade goes from full height to nothing, so at any distance
			// some blades are part-grown and the field has no edge in it.
			float shrink = clamp((ticket - keep) / max(u_LodBand, 0.001),
				0.0, 1.0);

			// **No discard, at any point.** The first version of this kept a
			// hard branch for `shrink >= 1` -- and that reintroduced exactly
			// the fault the shrink exists to avoid, because the branch is
			// taken per *vertex*: a blade straddling the line had some
			// vertices sent behind the near plane and the rest left where they
			// were, and the triangle between them was drawn across the screen.
			// The field filled with long straight streaks radiating from the
			// camera, which is what a stretched triangle looks like.
			//
			// Letting the shrink saturate instead costs nothing and cannot
			// tear: at full shrink every vertex has moved down by its own
			// share of the height, so the blade closes onto its own base and
			// becomes a six-millimetre sliver lying flat on ground of the same
			// colour, seventy-five metres away. The vertex work is still done
			// and the fragment work is not, which is the same trade the branch
			// was making without the defect.
			world.xyz -= vec3(0.0, 1.0, 0.0)
				* (u_GrassHeight * along * shrink);

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
		uniform vec3 u_Up;

		// 0 close, 1 far. See the note in `main` -- this is the whole of the
		// distance treatment and it is a shading change, not a geometry one.
		uniform float u_Fade;

		void main()
		{
			// **Distant grass stops being individual blades.**
			//
			// A blade is six millimetres across, so beyond a couple of metres
			// it is well under a pixel wide. Coverage is then a coin-flip per
			// pixel -- the blade either contains the sample point or it does
			// not -- and every blade carries its own normal, its own colour
			// jitter and its own root-to-tip gradient. Three high-frequency
			// signals, all aliasing, all moving in the wind: that is the
			// grainy sparkle, and no amount of smoothing the *geometry* will
			// touch it, because the geometry is not the thing being sampled
			// too coarsely -- the shading is.
			//
			// The fix is to make the two outcomes of that coin-flip look the
			// same. Converge every blade's normal toward the ground's, and its
			// colour toward the field's mean, as it recedes. Then a pixel that
			// lands on a blade and a pixel that lands on the gap beside it
			// shade almost identically, and the aliasing stops *showing* even
			// though it is still there.
			//
			// It is also the physically sensible thing. A field of grass seen
			// from a distance does not shade like a million independent
			// leaves; it shades like a surface. Close up you are looking at
			// blades and far away you are looking at a meadow, and this is the
			// one line that says so.
			vec3 normal = normalize(v_Normal);

			if (dot(normal, u_Up) < 0.0)
				normal = -normal;

			normal = normalize(mix(normal, u_Up, u_Fade));

			// Dark at the root, light at the tip: ambient occlusion, and a
			// real one -- a blade near the ground is surrounded by other
			// blades and sees almost none of the sky. It is the single thing
			// that makes a field read as depth rather than as a green plane.
			// The root-to-tip gradient is the third aliasing signal, so it
			// goes the same way: at distance every blade is its own average.
			vec3 base = mix(u_Root, u_Tip, mix(v_Up, 0.62, u_Fade));

			// **Blade to blade variation, from the ticket.** Two independent
			// shifts rather than one: a brightness spread alone reads as
			// noise, while a hue spread as well reads as different plants.
			// Wide enough to read as different plants, not so wide that the
			// low end goes black -- which is what 0.72 of an already dark
			// root did.
			// And the per-blade variation, faded out with the rest: it is what
			// makes a near field look like plants and what makes a far one
			// look like static.
			base *= mix(mix(0.86, 1.22, v_Tint), 1.02, u_Fade);
			base = mix(base, base.gbr, 0.10 * fract(v_Tint * 7.13) * (1.0 - u_Fade));

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
	BuildSky();
}

// **The sky is one inverted cube drawn behind everything.**
//
// The solar demo raymarches a shell because it has to work from orbit *and*
// from the ground in one continuous space. Standing on a 144 m block there is
// no orbit to serve, so the sky is a gradient with a sun, a moon and some cloud
// in it -- evaluated per pixel along the view ray, which is all the shell's
// raymarch would give at this scale anyway and costs one draw.
//
// Drawn first with depth writes off, so it fills the frame and everything else
// lands in front of it. That also means it needs no depth range trickery: it is
// simply the first thing drawn and the last thing anyone sees behind.
inline void TerrainLab::BuildSky()
{
	Egss::MeshData box;

	// A cube of side two centred on the origin, wound inward -- the camera is
	// inside it, so the faces that matter are the ones pointing at it.
	const glm::vec3 corners[8] = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
		{ -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 } };

	for (int i = 0; i < 8; i++)
		box.Vertices.push_back({ corners[i], glm::normalize(-corners[i]),
			{ 0.0f, 0.0f } });

	const unsigned int faces[36] = {
		0,2,1, 0,3,2,  4,5,6, 4,6,7,  0,1,5, 0,5,4,
		3,7,6, 3,6,2,  0,4,7, 0,7,3,  1,2,6, 1,6,5 };

	box.Indices.assign(faces, faces + 36);

	Egss::Submesh all;
	all.IndexCount = 36;
	box.Submeshes.push_back(all);
	box.RecalculateBounds();

	m_Sky = std::make_shared<Egss::Mesh>(box, "LabSky");

	std::string vertexSrc = R"(
		#version 330 core

		layout(location = 0) in vec3 a_Position;
		layout(location = 1) in vec3 a_Normal;
		layout(location = 2) in vec2 a_TexCoord;

		uniform mat4 u_ViewProjection;
		uniform mat4 u_Transform;

		out vec3 v_Ray;

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			// The direction from the eye, which is the transform's own
			// translation because the box is centred on the camera.
			v_Ray = world.xyz - u_Transform[3].xyz;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string fragmentSrc = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_Ray;

		uniform vec3 u_SunDirection;
		uniform vec3 u_MoonDirection;
		uniform vec3 u_SunColor;
		uniform vec3 u_SkyColor;
		uniform float u_SunHeight;
		uniform float u_Time;
		uniform float u_CloudCover;
		uniform float u_CloudHeight;
		uniform float u_ShowClouds;
		uniform vec3 u_Wind;

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
			return noise(p) * 0.52 + noise(p * 2.1) * 0.28
				+ noise(p * 4.3) * 0.14 + noise(p * 8.7) * 0.06;
		}

		void main()
		{
			vec3 ray = normalize(v_Ray);

			// **The gradient is the air, and it is thickest at the horizon.**
			// Looking up you see through the least of it and the sky is at its
			// deepest; looking level you see through the most and it pales.
			// One term, and it is the same reason a sunset is red.
			float up = clamp(ray.y, 0.0, 1.0);

			vec3 sky = mix(u_SkyColor * 1.35, u_SkyColor * 0.72,
				pow(1.0 - up, 2.2));

			// Below the horizon the dome is ground-coloured rather than sky,
			// so a camera that tips down does not see blue underneath itself.
			sky = mix(sky, u_SkyColor * 0.22, smoothstep(0.0, -0.12, ray.y));

			// **The sun, as a disc with a glow round it.** Half a degree is
			// 0.0087 radians, so the disc is `dot > cos(0.0044)` -- and the
			// glow is the same scattering that reddens it, spread over a few
			// degrees.
			float toSun = dot(ray, u_SunDirection);

			float disc = smoothstep(0.99993, 0.99997, toSun);
			float glow = pow(max(toSun, 0.0), 220.0) * 0.55
				+ pow(max(toSun, 0.0), 8.0) * 0.10;

			sky += u_SunColor * (disc * 14.0 + glow) * step(-0.15, u_SunDirection.y);

			// The moon: a smaller, cooler disc, only while it is up, and
			// fading as the sky brightens because a daytime moon is faint.
			float toMoon = dot(ray, u_MoonDirection);

			float moon = smoothstep(0.99990, 0.99995, toMoon);

			float moonlit = step(-0.05, u_MoonDirection.y)
				* (1.0 - smoothstep(0.0, 0.35, u_SunHeight));

			sky += vec3(0.82, 0.84, 0.78) * moon * 6.0 * moonlit;

			// **Cloud on a plane, read where the ray crosses it.** A ray going
			// up hits `u_CloudHeight` at a distance that grows without bound as
			// it approaches level -- which is exactly right, and is what makes
			// cloud bunch toward the horizon on its own without a second term.
			if (u_ShowClouds > 0.5 && ray.y > 0.02)
			{
				float reach = u_CloudHeight / ray.y;
				vec2 at = ray.xz * reach - u_Wind.xz * u_Time * 3.0;

				float cloud = fbm(at * 0.0032);

				// A coverage threshold rather than a brightness: cloud has
				// edges, and mixing a noise field straight in gives fog.
				float mask = smoothstep(0.62 - u_CloudCover * 0.45,
					0.86 - u_CloudCover * 0.45, cloud);

				// Thin out toward the horizon, where the plane model breaks
				// down and the ray would otherwise smear one cloud for ever.
				mask *= smoothstep(0.02, 0.22, ray.y);

				vec3 top = u_SunColor * 0.9 + u_SkyColor * 0.4;
				vec3 base = u_SkyColor * 0.55;

				sky = mix(sky, mix(base, top, 0.35 + 0.65 * u_SunHeight), mask);
			}

			color = vec4(sky, 1.0);
		}
	)";

	m_SkyShader.reset(Egss::Shader::Create("LabSky", vertexSrc, fragmentSrc));
	m_SkyMaterial = Egss::Material::Create(m_SkyShader);
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

	// **Six habits, not six seeds.** Three seeds of the same parameters give
	// three trees that are recognisably the same tree; what makes a wood look
	// like a wood is that its trees have different *architecture*. The
	// parameters worth moving are the branching count, how far a child leans
	// off its parent, and how fast length falls off with depth -- those three
	// between them decide whether a tree is a spire, a vase or a mop.
	//
	// They are laid out here as a set to choose from rather than tuned to one
	// answer, because which of them reads best is a question for looking at
	// them, and the panel can turn each on and off for exactly that.

	// 0: spire. Narrow, steep branching, long leader -- a conifer.
	shapes[0].Depth = 5;
	shapes[0].Children = 3;
	shapes[0].Length = 3.4f;
	shapes[0].Radius = 0.15f;
	shapes[0].Spread = 22.0f;
	shapes[0].LengthRatio = 0.80f;
	shapes[0].RadiusRatio = 0.58f;
	shapes[0].LeafRadius = 0.42f;

	// 1: vase. Few children, wide angle, short leader -- an open-grown oak.
	shapes[1].Depth = 4;
	shapes[1].Children = 3;
	shapes[1].Length = 2.4f;
	shapes[1].Radius = 0.26f;
	shapes[1].Spread = 52.0f;
	shapes[1].LengthRatio = 0.72f;
	shapes[1].RadiusRatio = 0.66f;
	shapes[1].LeafRadius = 0.80f;

	// 2: mop. Many short children off a stout trunk.
	shapes[2].Depth = 4;
	shapes[2].Children = 5;
	shapes[2].Length = 1.9f;
	shapes[2].Radius = 0.22f;
	shapes[2].Spread = 44.0f;
	shapes[2].LengthRatio = 0.64f;
	shapes[2].LeafRadius = 0.62f;

	// 3: sapling. Small, sparse, and the thing that makes a wood look grown
	// rather than planted -- a stand of one size reads as an orchard.
	shapes[3].Depth = 3;
	shapes[3].Children = 3;
	shapes[3].Length = 1.6f;
	shapes[3].Radius = 0.09f;
	shapes[3].Spread = 34.0f;
	shapes[3].LengthRatio = 0.70f;
	shapes[3].LeafRadius = 0.40f;

	// 4: parasol. A bare trunk carrying a flat wide crown, which is what an
	// isolated tree on open ground grows into.
	shapes[4].Depth = 4;
	shapes[4].Children = 4;
	shapes[4].Length = 2.9f;
	shapes[4].Radius = 0.20f;
	shapes[4].Spread = 68.0f;
	shapes[4].LengthRatio = 0.58f;
	shapes[4].LeafRadius = 0.72f;

	// 5: scrub. Low, many-stemmed, barely a tree -- the thing that belongs at
	// the dry edge of a wood where the others give out.
	shapes[5].Depth = 3;
	shapes[5].Children = 5;
	shapes[5].Length = 1.2f;
	shapes[5].Radius = 0.08f;
	shapes[5].Spread = 62.0f;
	shapes[5].LengthRatio = 0.66f;
	shapes[5].LeafRadius = 0.45f;

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
		uniform float u_MaxLean;

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

			vec3 lean = push * (u_Compliance * pressure * height * height);

			// **A tree bends; it does not fall over.**
			//
			// The displacement goes as the square of the height *and* the
			// square of the wind, so a crown six metres up in a strong gust
			// was being thrown many times its own height and the wood came out
			// as a tangle of stretched triangles. A real tree of this size
			// moves a fraction of its height even in a gale -- the trunk is
			// stiff and what is really happening is the branches flexing.
			//
			// The cap is a share of how high up the tree this vertex is, so a
			// low branch is held tighter than the crown and the shape stays a
			// tree rather than being sheared uniformly.
			float limit = u_MaxLean * height;

			float reach = length(lean);

			if (reach > limit && reach > 1e-6)
				lean *= limit / reach;

			world.xyz += lean;

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

	// One boulder mesh for every rock and every log. The shape is a jittered
	// sphere either way -- what tells a granite boulder from a floating log
	// here is its density and its colour, not its silhouette, and that is
	// honest enough for a buoyancy test.
	m_Boulder = std::make_shared<Egss::Mesh>(Boulder::Build(4177u), "LabRock");

	// **The doorway's panel: a window, sampled in screen space.**
	//
	// Nothing here is projected or unprojected. The portal pass rendered the
	// far side with this frame's own projection into a target the size of the
	// framebuffer, so the fragment at pixel (x, y) on the panel wants the
	// pixel at (x, y) of that target -- the same ray, seen from the other end
	// of the doorway. `gl_FragCoord` gives the pixel and the division gives
	// the coordinate, and that is the entire shader.
	//
	// Getting the angle right is therefore not something this does; it is
	// something it cannot fail to do. Walk sideways past the door and the view
	// slides the way a view through a window slides, because it is one.
	{
		std::string portalVertex = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			void main()
			{
				gl_Position = u_ViewProjection * u_Transform
					* vec4(a_Position, 1.0);
			}
		)";

		std::string portalFragment = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			uniform sampler2D u_View;

			// `Material` has no vec2 setter and this needed no engine change
			// to work; zw are unused.
			uniform vec4 u_Resolution;

			void main()
			{
				color = vec4(texture(u_View,
					gl_FragCoord.xy / u_Resolution.xy).rgb, 1.0);
			}
		)";

		m_PortalShader.reset(Egss::Shader::Create("LabPortal", portalVertex,
			portalFragment));

		m_PortalMaterial = Egss::Material::Create(m_PortalShader);
	}

	// A unit cube, scaled by whatever draws it. The doorway and the shed are
	// both made of boxes and neither is worth a mesh of its own.
	{
		Egss::MeshData cube;

		const glm::vec3 n[6] = { { 0,0,1 }, { 0,0,-1 }, { 1,0,0 },
			{ -1,0,0 }, { 0,1,0 }, { 0,-1,0 } };

		for (int f = 0; f < 6; f++)
		{
			glm::vec3 normal = n[f];
			glm::vec3 up = std::abs(normal.y) > 0.5f
				? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);

			glm::vec3 right = glm::cross(up, normal);
			up = glm::cross(normal, right);

			unsigned int at = (unsigned int)cube.Vertices.size();

			for (int j = 0; j < 2; j++)
			for (int i = 0; i < 2; i++)
				cube.Vertices.push_back({
					normal + right * ((float)i * 2.0f - 1.0f)
						+ up * ((float)j * 2.0f - 1.0f),
					normal, { (float)i, (float)j } });

			cube.Indices.insert(cube.Indices.end(),
				{ at, at + 1, at + 3, at, at + 3, at + 2 });
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)cube.Indices.size();
		cube.Submeshes.push_back(all);
		cube.RecalculateBounds();

		m_Cube = std::make_shared<Egss::Mesh>(cube, "LabCube");
	}
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

	// **A grid, not a quad, because waves need somewhere to happen.**
	//
	// One quad was enough while the surface was flat: the depth buffer found
	// the shoreline and the shader did the rest. Displacing it needs vertices
	// to displace, and 128 a side over a 144 m block is a vertex every 1.1 m
	// -- fine enough for a swell of ten metres and far too coarse for ripples,
	// which is why the ripples stay in the normal rather than the geometry.
	const int side = 128;

	for (int j = 0; j <= side; j++)
	for (int i = 0; i <= side; i++)
	{
		float u = (float)i / (float)side;
		float v = (float)j / (float)side;

		plane.Vertices.push_back({
			{ u - 0.5f, 0.0f, v - 0.5f },
			{ 0.0f, 1.0f, 0.0f },
			{ u, v } });
	}

	for (int j = 0; j < side; j++)
	for (int i = 0; i < side; i++)
	{
		unsigned int a = (unsigned int)(j * (side + 1) + i);
		unsigned int b = a + 1;
		unsigned int c = a + (unsigned int)(side + 1);
		unsigned int d = c + 1;

		plane.Indices.insert(plane.Indices.end(), { a, c, b, b, c, d });
	}

	Egss::Submesh all;
	all.IndexCount = (unsigned int)plane.Indices.size();
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
		out vec3 v_Normal;

		uniform vec3 u_Wind;
		uniform float u_Time;
		uniform float u_Waves;
		uniform float u_Ripples;

		// **Two crossed swells, travelling with the wind.**
		//
		// A gerstner wave would move the water in a circle and give the sharp
		// crests real waves have; this is a plain sine sum, which is enough for
		// a lake and costs two sines. What matters more than the shape is that
		// both components travel *downwind* -- a wave field that sits still, or
		// worse travels crosswind, reads as a texture rather than as water.
		//
		// The second is at 0.71 of the first's wavelength and 40 degrees off
		// it: an irrational-ish ratio, so the two never come back into phase
		// and the surface never repeats visibly.
		vec2 wave(vec2 at, vec2 wind, float time)
		{
			float speed = max(length(wind), 0.001);
			vec2 dir = wind / speed;

			vec2 cross = vec2(dir.y, -dir.x);
			vec2 second = normalize(dir * 0.77 + cross * 0.64);

			float k1 = 0.42, k2 = 0.59;

			// Deep-water waves travel at sqrt(g/k), which is why a long swell
			// outruns a short chop. Using it means the two components separate
			// over time instead of marching in lockstep.
			float c1 = sqrt(9.81 / k1), c2 = sqrt(9.81 / k2);

			float p1 = dot(at, dir) * k1 - time * k1 * c1;
			float p2 = dot(at, second) * k2 - time * k2 * c2;

			float amplitude = 0.055 * speed;

			// Height, and the slope along the dominant direction -- enough to
			// tilt the normal without a second pass.
			float h = sin(p1) * amplitude + sin(p2) * amplitude * 0.62;

			float slope = cos(p1) * amplitude * k1
				+ cos(p2) * amplitude * 0.62 * k2;

			return vec2(h, slope);
		}

		void main()
		{
			vec4 world = u_Transform * vec4(a_Position, 1.0);

			vec2 w = wave(world.xz, u_Wind.xz, u_Time);

			world.y += w.x * u_Waves;

			// The swell's own slope, as a normal. Ripples are added in the
			// fragment shader instead: they are centimetres across and this
			// grid has a vertex every 1.1 m, so putting them here would alias
			// them into nothing.
			vec2 dir = normalize(u_Wind.xz + vec2(1e-6));

			v_Normal = normalize(vec3(-dir.x * w.y * u_Waves, 1.0,
				-dir.y * w.y * u_Waves));

			v_World = world.xyz;

			gl_Position = u_ViewProjection * world;
		}
	)";

	std::string waterFragment = R"(
		#version 330 core

		layout(location = 0) out vec4 color;

		in vec3 v_World;
		in vec3 v_Normal;

		uniform vec3 u_Eye;
		uniform float u_Ripples;
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
			// The swell arrives as a normal from the vertex stage; the
			// ripples are a perturbation on top of it, and they are optional
			// because whether they read as wind on water or as noise depends
			// on the wind speed and is worth being able to switch off.
			vec3 normal = normalize(v_Normal);

			if (u_Ripples > 0.5)
			{
				vec2 drift = u_Wind.xz * u_Time * 0.15;

				float a = (v_World.x - drift.x) * 1.7 + u_Time * 1.1;
				float b = (v_World.z - drift.y) * 2.3 - u_Time * 0.8;

				// Scaled by the wind: a calm lake is a mirror, and ripples
				// that persist at zero wind are the giveaway that they are
				// decoration rather than weather.
				float chop = clamp(length(u_Wind.xz) / 12.0, 0.0, 1.0);

				normal = normalize(normal + vec3(cos(a) * 0.09 * chop, 0.0,
					cos(b) * 0.09 * chop));
			}

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

	// **The camera follows the pane, not the window.**
	//
	// The viewport is set to the editor's central node, which is a different
	// shape from the window -- 1.27 against 16:9 here. Leaving the projection
	// alone stretches the scene vertically by the ratio between them, and a
	// stretched scene is the kind of wrong that is easy to look at and hard to
	// name: nothing is obviously broken, the trees are simply too tall.
	//
	// Cheap to keep in step, since `SetAspectRatio` only rebuilds a matrix.
	if (g_Viewport.Valid())
		m_Camera.SetAspectRatio((float)g_Viewport.Width
			/ glm::max((float)g_Viewport.Height, 1.0f));
	else
		m_Camera.SetAspectRatio(16.0f / 9.0f);

	// The day advances on the frame clock rather than the fixed step, because
	// it changes nothing the simulation can see -- and holding it still at
	// zero is how you compare two shots of the same scene.
	m_TimeOfDay = glm::fract(m_TimeOfDay + m_DayLength * (float)ts);

	// **The other side of the doorway, drawn first.**
	//
	// A second camera, placed by the same rigid map that moves the player, and
	// rendered into an off-screen target. The doorway then samples that target
	// at its own screen position -- which is what makes the angle right for
	// free: both cameras share a projection, so the pixel behind the doorway
	// in the portal view *is* the pixel that belongs there.
	if (m_SeeThrough && (m_PortalOn || m_InShed))
		DrawPortalView();

	DrawScene(m_Camera, Pass::Main);
}

// **Everything in the world, from whichever camera is asked for.**
//
// Split out of `OnDemoUpdate` so the portal can render the scene a second
// time from somewhere else. The pass says which side of the doorway is being
// drawn, and that is the only thing that differs between the three:
//
//   Main     -- the world, plus the shed if you are standing in it.
//   ToShed   -- the room alone, which is all that is on the far side of a
//               doorway planted in a field. No sky, no ground: there is none
//               there, and drawing the terrain again to have it fall outside
//               the frustum would cost the whole scene for nothing.
//   ToWorld  -- the world without the room, which is what is on the far side
//               of the shed's own door.
//
// The doorway's panel is drawn only in `Main`, which is what stops a portal
// from recursing into itself.
inline void TerrainLab::DrawScene(const Egss::PerspectiveCamera& camera, Pass pass)
{
	glm::vec3 skyColour = SkyColour();

	// `u_SunDirection` is the direction light *travels*, which is the opposite
	// of the direction to the sun. Getting that backwards lights the world from
	// underneath and reads as a broken normal rather than a sign error.
	glm::vec3 sun = -SunDirection();

	glm::vec3 sunColour = SunColour();
	glm::vec2 windMean = MeanWind();

	// **The room, and nothing else.** A doorway planted in a field has a shed
	// on the other side of it and no sky, no ground and no weather -- so the
	// pass that draws what is through it draws six boxes and stops. Rendering
	// the terrain again to have all of it fall outside the frustum would cost
	// the whole scene for nothing, and the frustum is the only thing that
	// would have thrown it away.
	if (pass == Pass::ToShed)
	{
		Egss::RenderCommand::SetClearColor({ 0.015f, 0.014f, 0.02f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(camera);

		DrawShed();

		Egss::Renderer::EndScene();

		return;
	}

	Egss::RenderCommand::SetClearColor(
		{ skyColour.r, skyColour.g, skyColour.b, 1.0f });

	Egss::RenderCommand::Clear();

	Egss::Renderer::BeginScene(camera);

	if (m_ShowSky && m_Sky)
	{
		m_SkyMaterial->Set("u_SunDirection", SunDirection());
		m_SkyMaterial->Set("u_MoonDirection", MoonDirection());
		m_SkyMaterial->Set("u_SunColor", sunColour);
		m_SkyMaterial->Set("u_SkyColor", skyColour);
		m_SkyMaterial->Set("u_SunHeight", SunHeight());
		m_SkyMaterial->Set("u_Time", m_Time);
		m_SkyMaterial->Set("u_CloudCover", m_CloudCover);
		m_SkyMaterial->Set("u_CloudHeight", m_CloudHeight);
		m_SkyMaterial->Set("u_ShowClouds", m_ShowClouds ? 1.0f : 0.0f);
		m_SkyMaterial->Set("u_Wind", glm::vec3(windMean.x, 0.0f, windMean.y));

		// **Depth writes off and drawn first.** The box is centred on the
		// camera and scaled to sit inside the far plane; with no depth written
		// everything drawn afterwards lands in front of it whatever its own
		// distance, so the sky needs no special depth range.
		Egss::RenderCommand::SetDepthWrite(false);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		Egss::Renderer::Submit(m_SkyMaterial, m_Sky,
			glm::scale(glm::translate(glm::mat4(1.0f), camera.GetPosition()),
				glm::vec3(400.0f)));

		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	m_Material->Set("u_SunDirection", sun);
	m_Material->Set("u_SunColor", sunColour);
	m_Material->Set("u_SkyColor", skyColour);
	m_Material->Set("u_Ambient", 0.55f);
	m_Material->Set("u_Eye", camera.GetPosition());

	// Ambient falls with the sun: the ground at night is lit by the sky and
	// the sky at night is nearly black, so one number does both.
	m_Material->Set("u_Haze", m_Haze);
	// One element at a time: `Material` has no array setter, and
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
		glm::vec3 wind(windMean.x, 0.0f, windMean.y);
		glm::vec3 eye = camera.GetPosition();

		m_GrassMaterial->Set("u_SunDirection", sun);
		m_GrassMaterial->Set("u_SunColor", sunColour);
		m_GrassMaterial->Set("u_SkyColor", skyColour);
		m_GrassMaterial->Set("u_Ambient", 0.55f);
		m_GrassMaterial->Set("u_Wind", wind);
		m_GrassMaterial->Set("u_Time", m_Time);
		m_GrassMaterial->Set("u_Seed", (float)(m_Shape.Seed % 997u));

		// Metres, as a share of how far up the blade a vertex is -- so the cap
		// scales with the blade rather than being one distance for all of them.
		m_GrassMaterial->Set("u_MaxLean", m_GrassHeight * m_MaxLean);

		// Darker at the root than the tip -- see the fragment shader.
		m_GrassMaterial->Set("u_Up", glm::vec3(0.0f, 1.0f, 0.0f));
		m_GrassMaterial->Set("u_Eye", eye);
		m_GrassMaterial->Set("u_LodNear", m_LodNear);
		m_GrassMaterial->Set("u_LodFar", m_LodFar);
		m_GrassMaterial->Set("u_LodBand", m_LodBand);

		// The shrink is a share of the blade's height, so without this it
		// multiplies by zero and nothing ever thins -- which is how the
		// streaks above were reached with the level of detail apparently off.
		m_GrassMaterial->Set("u_GrassHeight", m_GrassHeight);
		m_GrassMaterial->Set("u_Root", glm::vec3(0.14f, 0.22f, 0.09f));
		m_GrassMaterial->Set("u_Tip", glm::vec3(0.44f, 0.64f, 0.26f));
		m_GrassMaterial->Set("u_Dry", glm::vec3(0.62f, 0.55f, 0.28f));

		for (const auto& entry : m_Grass)
		{
			// The same per-chunk keep the planet uses: both sides of the test
			// constant across a blade, so it can never tear one in half.
			glm::vec3 centre = 0.5f * (entry.second->GetBoundsMin()
				+ entry.second->GetBoundsMax());

			float away = glm::length(centre - eye);

			// **Blades stop entirely at the far end, and the ground carries
			// it.** Thinning to a fifth still leaves a fifth of the sparkle;
			// going to nothing leaves the terrain, which is already tinted
			// green by the same climate the grass grew from and already has a
			// noise texture on it. The only thing lost past sixty metres is
			// detail nobody can resolve.
			// The shading convergence is still per chunk -- it is a colour,
			// and a colour changing in steps of sixteen metres is invisible
			// where a *density* changing in the same steps was not.
			m_GrassMaterial->Set("u_Fade",
				glm::smoothstep(m_LodNear, m_LodFar, away));

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
		glm::vec2 mean = windMean;

		m_TreeMaterial->Set("u_SunDirection", sun);
		m_TreeMaterial->Set("u_SunColor", sunColour);
		m_TreeMaterial->Set("u_SkyColor", skyColour);
		m_TreeMaterial->Set("u_Ambient", 0.55f);
		m_TreeMaterial->Set("u_Wind", glm::vec3(mean.x, 0.0f, mean.y));
		m_TreeMaterial->Set("u_Time", m_Time);
		m_TreeMaterial->Set("u_Seed", (float)(m_Shape.Seed % 997u));

		// A share of the height. A tenth is already a lot of movement on a
		// six-metre crown; the grass gets 0.85 because grass really does lie
		// flat and a tree really does not.
		m_TreeMaterial->Set("u_MaxLean", m_TreeMaxLean);

		glm::vec3 eye = camera.GetPosition();

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

	// Boulders and driftwood, through the tree shader with the sway switched
	// off -- it is already lit by the same sun and sky, and a second shader
	// that differed only in having no wind term would be a second thing to
	// keep in step.
	if (m_Boulder && !m_Loose.empty())
	{
		m_TreeMaterial->Set("u_Compliance", 0.0f);
		m_TreeMaterial->Set("u_MaxLean", 0.0f);

		for (const Loose& loose : m_Loose)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			m_TreeMaterial->Set("u_Color", loose.Floats
				? glm::vec3(0.42f, 0.30f, 0.18f)
				: glm::vec3(0.40f, 0.39f, 0.37f));

			glm::mat4 transform =
				glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				* glm::scale(glm::mat4(1.0f), glm::vec3(loose.Size));

			Egss::Renderer::Submit(m_TreeMaterial, m_Boulder, transform);
		}
	}

	// **The doorway, the room if you are in it, and the picture in the
	// doorway.**
	//
	// The panel between the posts used to be a flat dark board, and it said so
	// in a comment: what made the door work was the plane test, not the
	// picture. It is a picture now -- the scene rendered a moment ago from the
	// far side, sampled at this fragment's own place on the screen.
	//
	// **Screen-space sampling is what makes the angle right, and it is right
	// for free.** The second camera shares this one's projection, so a
	// fragment of the panel at pixel (x, y) shows what the far camera drew at
	// pixel (x, y) -- which is exactly the ray that would have gone through
	// the doorway. There is nothing to line up by hand, no projected quad and
	// no matrix to get subtly wrong; walk sideways and the view slides the way
	// a view through a window slides, because it is one.
	if (pass != Pass::ToWorld && m_InShed)
		DrawShed();

	if (m_PortalOn)
		DrawDoorFrame();

	if (pass == Pass::Main)
	{
		bool through = m_SeeThrough && m_PortalTexture
			&& (m_InShed || m_PortalOn);

		glm::vec3 at = m_InShed ? ShedDoor() : m_PortalAt;
		float yaw = m_InShed ? 0.0f : m_PortalYaw;

		if (m_InShed || m_PortalOn)
		{
			if (through)
			{
				m_PortalMaterial->SetTexture("u_View", m_PortalTexture);
				m_PortalMaterial->Set("u_Resolution", glm::vec4(
					m_PortalSize.x, m_PortalSize.y, 0.0f, 0.0f));

				glm::mat4 transform = glm::scale(
					glm::rotate(glm::translate(glm::mat4(1.0f),
						at + glm::vec3(0.0f, 0.5f * s_DoorTop, 0.0f)),
						yaw, glm::vec3(0.0f, 1.0f, 0.0f)),
					glm::vec3(s_DoorHalf, 0.5f * s_DoorTop, 0.02f));

				Egss::Renderer::Submit(m_PortalMaterial, m_Cube, transform);
			}
			else if (!m_InShed)
			{
				DrawBox(at + glm::vec3(0.0f, 0.5f * s_DoorTop, 0.0f),
					{ s_DoorHalf, 0.5f * s_DoorTop, 0.02f }, yaw,
					glm::vec3(0.03f, 0.03f, 0.05f));
			}
		}
	}

	// Water last: it is blended, so anything it may sit in front of has to be
	// in the depth buffer already.
	if (HasWater() && m_Water)
	{
		glm::vec2 mean = windMean;

		m_WaterMaterial->Set("u_Eye", camera.GetPosition());
		m_WaterMaterial->Set("u_SunDirection", sun);
		m_WaterMaterial->Set("u_SunColor", sunColour);
		m_WaterMaterial->Set("u_SkyColor", skyColour);
		m_WaterMaterial->Set("u_Shallow", glm::vec3(0.32f, 0.55f, 0.55f));
		m_WaterMaterial->Set("u_Deep", glm::vec3(0.05f, 0.16f, 0.27f));
		m_WaterMaterial->Set("u_Time", m_Time);
		m_WaterMaterial->Set("u_Wind", glm::vec3(mean.x, 0.0f, mean.y));
		m_WaterMaterial->Set("u_Waves", m_Waves ? 1.0f : 0.0f);
		m_WaterMaterial->Set("u_Ripples", m_Ripples ? 1.0f : 0.0f);

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

// The unit cube, placed and coloured, through the tree shader with the sway
// switched off. It is already lit by the same sun and sky, and a second shader
// that differed only in having no wind term would be a second thing to keep in
// step.
inline void TerrainLab::DrawBox(const glm::vec3& at, const glm::vec3& half,
	float yaw, const glm::vec3& colour)
{
	m_TreeMaterial->Set("u_Color", colour);

	glm::mat4 transform = glm::scale(
		glm::rotate(glm::translate(glm::mat4(1.0f), at),
			yaw, glm::vec3(0.0f, 1.0f, 0.0f)),
		half);

	Egss::Renderer::Submit(m_TreeMaterial, m_Cube, transform);
}

// **The lighting has to be set here as well as in the main pass.** The pass
// that draws the room returns before the trees are reached, so on the first
// frame the tree material would carry whatever it was created with -- a black
// room, which reads as the portal failing rather than as an unset uniform.
inline void TerrainLab::DrawShed()
{
	m_TreeMaterial->Set("u_SunDirection", -SunDirection());
	m_TreeMaterial->Set("u_SunColor", SunColour());
	m_TreeMaterial->Set("u_SkyColor", SkyColour());
	m_TreeMaterial->Set("u_Ambient", 0.55f);
	m_TreeMaterial->Set("u_Compliance", 0.0f);
	m_TreeMaterial->Set("u_MaxLean", 0.0f);
	m_TreeMaterial->Set("u_Time", m_Time);

	glm::vec3 centre = ShedCentre();
	const float h = s_ShedHalf;

	glm::vec3 plank(0.30f, 0.22f, 0.15f);

	DrawBox(centre + glm::vec3(0.0f, -0.25f, 0.0f), { h, 0.25f, h }, 0.0f,
		glm::vec3(0.24f, 0.18f, 0.12f));
	DrawBox(centre + glm::vec3(0.0f, 3.25f, 0.0f), { h, 0.25f, h }, 0.0f, plank);
	DrawBox(centre + glm::vec3(-h, 1.5f, 0.0f), { 0.25f, 1.75f, h }, 0.0f, plank);
	DrawBox(centre + glm::vec3(h, 1.5f, 0.0f), { 0.25f, 1.75f, h }, 0.0f, plank);
	DrawBox(centre + glm::vec3(0.0f, 1.5f, h), { h, 1.75f, 0.25f }, 0.0f, plank);

	// The wall with the door in it: two posts and a lintel, and the lintel
	// sits at `s_DoorTop` so the opening matches the panel in the field.
	float side = 0.5f * (h - s_DoorHalf);
	float lintel = 0.5f * (3.0f - s_DoorTop);

	DrawBox(centre + glm::vec3(-(s_DoorHalf + side), 1.5f, -h),
		{ side, 1.75f, 0.25f }, 0.0f, plank);
	DrawBox(centre + glm::vec3(s_DoorHalf + side, 1.5f, -h),
		{ side, 1.75f, 0.25f }, 0.0f, plank);
	DrawBox(centre + glm::vec3(0.0f, s_DoorTop + lintel, -h),
		{ s_DoorHalf, lintel, 0.25f }, 0.0f, plank);
}

inline void TerrainLab::DrawDoorFrame()
{
	m_TreeMaterial->Set("u_SunDirection", -SunDirection());
	m_TreeMaterial->Set("u_SunColor", SunColour());
	m_TreeMaterial->Set("u_SkyColor", SkyColour());
	m_TreeMaterial->Set("u_Ambient", 0.55f);
	m_TreeMaterial->Set("u_Compliance", 0.0f);
	m_TreeMaterial->Set("u_MaxLean", 0.0f);
	m_TreeMaterial->Set("u_Time", m_Time);

	glm::vec3 across(std::cos(m_PortalYaw), 0.0f, -std::sin(m_PortalYaw));
	glm::vec3 timber(0.36f, 0.24f, 0.14f);

	float post = 0.5f * s_DoorTop;

	DrawBox(m_PortalAt + across * (s_DoorHalf + 0.12f)
		+ glm::vec3(0.0f, post, 0.0f), { 0.12f, post, 0.12f },
		m_PortalYaw, timber);

	DrawBox(m_PortalAt - across * (s_DoorHalf + 0.12f)
		+ glm::vec3(0.0f, post, 0.0f), { 0.12f, post, 0.12f },
		m_PortalYaw, timber);

	DrawBox(m_PortalAt + glm::vec3(0.0f, s_DoorTop + 0.12f, 0.0f),
		{ s_DoorHalf + 0.24f, 0.12f, 0.14f }, m_PortalYaw, timber);
}

// **The far side, rendered from a second camera placed by the same map that
// moves the player.**
//
// One pass, not two: only one doorway is ever in front of you, so standing in
// the field this draws the room and standing in the room it draws the field.
inline void TerrainLab::DrawPortalView()
{
	Egss::Window& window = Egss::Application::Get().GetWindow();

	unsigned int width = glm::max(window.GetWidth(), 1u);
	unsigned int height = glm::max(window.GetHeight(), 1u);

	// **Window-sized, not pane-sized, and rendered into the same sub-rect.**
	// The panel samples this at `gl_FragCoord.xy / resolution`, and
	// `gl_FragCoord` is measured from the corner of the *framebuffer*. Match
	// the framebuffer and the viewport to the main pass and the two agree
	// exactly, with no origin to carry about.
	if (!m_PortalTarget || (unsigned int)m_PortalSize.x != width
		|| (unsigned int)m_PortalSize.y != height)
	{
		Egss::FramebufferSpecification spec;
		spec.Width = width;
		spec.Height = height;
		spec.Attachments = { Egss::FramebufferTextureFormat::RGBA8,
			Egss::FramebufferTextureFormat::DEPTH24STENCIL8 };

		m_PortalTarget.reset(Egss::Framebuffer::Create(spec));

		// The wrapper does not own the handle, and the handle changes with
		// the framebuffer -- so it is rebuilt here and nowhere else.
		m_PortalTexture.reset(Egss::Texture2D::CreateFromHandle(
			m_PortalTarget->GetColorAttachmentRendererID(), width, height));

		// **Do not call `SetSmooth` on this.** It sets the minifying filter to
		// `GL_LINEAR_MIPMAP_LINEAR`, and a framebuffer attachment has no mip
		// chain -- which makes the texture *incomplete*, and an incomplete
		// texture samples black with no error anywhere. The doorway came out a
		// solid black board, which is exactly what it looked like before any
		// of this was written, so it read as the second camera never having
		// run. The framebuffer already gives its colour attachment
		// GL_LINEAR both ways; there was nothing to improve.

		m_PortalSize = glm::vec2((float)width, (float)height);
	}

	Doorway from = m_InShed ? ShedSideDoor()
		: WorldSideDoor(m_Camera.GetPosition());

	Doorway to = m_InShed ? ReturnDoor() : Doorway{ ShedDoor(), 0.0f };

	Pass pass = m_InShed ? Pass::ToWorld : Pass::ToShed;

	glm::mat3 turn = DoorTurn(from, to);

	// Copied, so it keeps the field of view, the aspect and the clip planes
	// the main camera is using this frame -- which is what the screen-space
	// sampling depends on.
	Egss::PerspectiveCamera other = m_Camera;

	other.SetPosition(to.At + turn * (m_Camera.GetPosition() - from.At));
	other.SetOrientation(turn * m_Camera.GetForward(),
		glm::vec3(0.0f, 1.0f, 0.0f));

	m_PortalTarget->Bind();

	// `Bind` sets the viewport to the whole target; the demo owns only the
	// editor's central pane, and the two passes must cover the same pixels.
	if (g_Viewport.Valid())
		Egss::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
			(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
			(unsigned int)g_Viewport.Height);

	DrawScene(other, pass);

	m_PortalTarget->Unbind();

	// `Unbind` restores the default target and not the viewport, so this has
	// to go back by hand or the main pass draws into the whole window.
	if (g_Viewport.Valid())
		Egss::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
			(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
			(unsigned int)g_Viewport.Height);
	else
		Egss::RenderCommand::SetViewport(0, 0, width, height);
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

		ImGui::Separator();
		ImGui::Text("Portal: %s%s", m_PortalOn ? "deployed" : "stowed",
			m_InShed ? " (you are in the shed)" : "");
		ImGui::TextDisabled("  E to plant it in front of you, E again nearby");
		ImGui::TextDisabled("  to pick it up; walk through to reach the shed");

		ImGui::Checkbox("See through it", &m_SeeThrough);
		ImGui::TextDisabled("  a second camera, so the doorway is a window;");
		ImGui::TextDisabled("  costs one extra pass while a door is up");
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

		ImGui::Checkbox("Swell", &m_Waves);
		ImGui::SameLine();
		ImGui::Checkbox("Wind ripples", &m_Ripples);
		ImGui::TextDisabled("  swell moves the surface; ripples only tilt it");

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
		ImGui::TextDisabled("Number keys walk the first nine cells.");

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

		// Turn all but one off to see a stand of a single habit, which is the
		// only way to judge whether that habit is any good.
		static const char* habits[s_TreeShapes] =
			{ "spire", "vase", "mop", "sapling", "parasol", "scrub" };

		for (int k = 0; k < s_TreeShapes; k++)
		{
			if (k % 3 != 0)
				ImGui::SameLine();

			cover |= ImGui::Checkbox(habits[k], &m_ShapeOn[k]);
		}
		ImGui::SliderFloat("Tree reach", &m_TreeReach, 20.0f, 220.0f, "%.0f m");
		ImGui::SliderFloat("Tree sway", &m_TreeMaxLean, 0.0f, 0.5f,
			"%.2f of height");
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

	// --- Sky ----------------------------------------------------------------

	if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::SliderFloat("Time of day", &m_TimeOfDay, 0.0f, 1.0f, "%.3f");

		// The clock as hours, because 0.34 means nothing and 08:10 does.
		int hour = (int)(m_TimeOfDay * 24.0f) % 24;
		int minute = (int)(m_TimeOfDay * 1440.0f) % 60;

		ImGui::SameLine();
		ImGui::Text("%02d:%02d", hour, minute);

		ImGui::SliderFloat("Day length", &m_DayLength, 0.0f, 0.05f,
			"%.4f turns a second");
		ImGui::TextDisabled("  zero holds the sun still, which is what two "
			"comparable captures need");

		ImGui::SliderFloat("Sun brightness", &m_SunBrightness, 0.0f, 3.0f);
		ImGui::SliderFloat("Haze", &m_Haze, 0.0f, 0.02f, "%.4f per m");

		ImGui::Checkbox("Sky", &m_ShowSky);
		ImGui::SameLine();
		ImGui::Checkbox("Clouds", &m_ShowClouds);

		ImGui::SliderFloat("Cloud cover", &m_CloudCover, 0.0f, 1.0f);
		ImGui::SliderFloat("Cloud height", &m_CloudHeight, 20.0f, 400.0f,
			"%.0f m");

		glm::vec3 sun = SunDirection();

		ImGui::Text("sun %+.0f deg above the horizon, light %.2f %.2f %.2f",
			glm::degrees(std::asin(glm::clamp(sun.y, -1.0f, 1.0f))),
			SunColour().r, SunColour().g, SunColour().b);
	}

	ImGui::Checkbox("Wireframe", &m_ShowWireframe);

	ImGui::End();
}
