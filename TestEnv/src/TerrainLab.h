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

#include <cstring>
#include <map>

#include "Demo.h"
#include "Grass.h"
#include "Rocks.h"
#include "Vegetation.h"

class TerrainLab : public DemoLayer
{
public:
	TerrainLab()
		: DemoLayer("TerrainLab"), m_Camera(60.0f, 16.0f / 9.0f, 0.1f, 900.0f)
	{
		RegisterParam("Walk speed", &m_WalkSpeed);
		RegisterParam("Dig radius", &m_DigRadius);

		// The two designs to start from, and the knobs that shape them. These
		// are registered because the design decides what `C` consumes and what
		// `B` puts in the world -- an unregistered slider here would make a
		// recorded building session replay as a different building.
		FillPanel(m_Draft, 24, 1);

		Design wallPanel;
		std::strncpy(wallPanel.Name, "Wall panel", sizeof(wallPanel.Name) - 1);
		wallPanel.Upright = true;
		FillPanel(wallPanel, 20, 1);

		m_Designs.push_back(m_Draft);
		m_Designs.push_back(wallPanel);

		// **The design is registered piece by piece, and that is why there is a
		// cap on pieces.**
		//
		// What the editor draws reaches the simulation -- it decides what `C`
		// consumes and what `B` puts in the world -- so it has to be in the
		// recording, and `ReplayParams` takes fixed pointers to plain values.
		// One integer a piece is the whole design, because the packed code *is*
		// the stored form rather than a copy of one.
		//
		// Sixty-four slots written every step would be a lot of file; they are
		// not, because the recorder only writes parameters that *changed*. A
		// design sits still except in the moment a piece is put down.
		//
		// The draft is a member and the saved designs are a vector, and not the
		// other way round: a registered pointer into a vector that can grow is
		// a pointer that stops being valid the first time somebody saves a
		// seventh design.
		for (int i = 0; i < s_MaxParts; i++)
			RegisterParam("Panel part " + std::to_string(i), &m_Draft.Code[i]);

		RegisterParam("Panel upright", &m_Draft.Upright);

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

	// **The ground under the workshop is levelled, and the terrace lives in
	// `Height` so that nothing has to be told about it.**
	//
	// The first attempt looked for ground flat enough to build on and the
	// measurement killed it: the flattest 8 m square anywhere on a ring 22 m
	// out still varied **4.16 m**, which is a five-metre plinth and a doorway
	// two metres above the path. There is no flat ground on this terrain, so
	// the site is cut and filled instead -- which is what actually happens
	// when a building goes on a hill.
	//
	// Folding it into `Height` rather than into `Density` is the whole trick.
	// The slope, the distance field, the mesh, the grass, the trees, the
	// boulders and the walker's ground query all read the terrain through
	// this one function, so every one of them sees the terrace and not one of
	// them needed a line changed.
	static constexpr float s_PadEdge = 1.2f;   // pad beyond the walls
	static constexpr float s_PadApron = 5.0f;  // and the ramp back to the hill

	float Height(float x, float z) const
	{
		float h = RawHeight(x, z);

		if (!m_Terraced)
			return h;

		// A square pad with a smooth apron: `t` is 1 on the pad, 0 past it.
		float out = glm::max(glm::max(
			std::abs(x - m_ShedAt.x) - (s_ShedHalf + s_PadEdge),
			std::abs(z - m_ShedAt.z) - (s_ShedHalf + s_PadEdge)), 0.0f);

		float t = 1.0f - glm::smoothstep(0.0f, s_PadApron, out);

		return glm::mix(h, m_ShedAt.y - s_ShedFloor, t);
	}

	// True on the levelled pad, which is where nothing grows and no boulder
	// was ever bedded.
	bool OnShedPad(float x, float z, float margin = 0.0f) const
	{
		return m_Terraced
			&& std::abs(x - m_ShedAt.x) < s_ShedHalf + s_PadEdge + margin
			&& std::abs(z - m_ShedAt.z) < s_ShedHalf + s_PadEdge + margin;
	}

	float RawHeight(float x, float z) const
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
		// **Before the fill, because the fill reads `Height` and `Height` now
		// carries the terrace.** Siting it afterwards would cut the pad into
		// a field that had already been sampled without it.
		SiteShed();

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
		// **Every handle into the world dies here.** `Clear` empties the body
		// list and the next `AddBody` starts again at zero, so a pool kept
		// across a regenerate is not stale -- it is aimed at somebody else's
		// body. The loose timber has always been cleared; the felled tops and
		// the panels were not, and a panel that survived a slider drag came
		// back owning the walker.
		m_Fell.clear();
		m_Panels.clear();
		m_PanelPool.clear();
		m_Carry.clear();

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
				m_Stones.erase(key);
				continue;
			}

			m_Chunks[key] = std::make_shared<Egss::Mesh>(data, "LabChunk");

			BuildChunkGrass(key, chunk, data);
			BuildChunkTrees(key, chunk, data);
			BuildChunkStones(key, chunk, data);
		}

		m_Field->ClearDirtyChunks();

		// After every chunk, because the pool is filled in one pass over all
		// of them and a per-chunk fill would keep overwriting the front.
		SyncStoneBodies();

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
			// Nothing grows on the inside of a hole you dug, and nothing
			// grows on the workshop floor.
			if (!OnSurface(at) || OnShedPad(at.x, at.z))
				return 0.0f;

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
		int Size;
		glm::vec3 Leaf;
		glm::vec3 Bark;

		// The cut, in the tree's own frame: how far up its axis, which way the
		// axe is going in, and how far through. Zero depth is an untouched
		// tree. Cuts are lost when the chunk is remeshed -- digging under a
		// wood rebuilds its trees -- which is honest for a lab and would not
		// be for a game.
		float CutY = 0.0f;
		float CutDepth = 0.0f;
		glm::vec2 CutSide = glm::vec2(1.0f, 0.0f);
		bool Severed = false;
	};

	// **The crown, once it is no longer attached.**
	//
	// A felled top is a rigid body and the same mesh drawn from the other side
	// of the cut, so nothing is built and nothing is thrown away -- the stump
	// keeps the fragments below the cut height and this keeps the ones above.
	// A pool, for the same reason the boulders use one: bodies can be
	// rewritten but not removed.
	struct Felled
	{
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		bool Active = false;
		int Shape = 0;
		int Size = 0;
		float Scale = 1.0f;
		float CutY = 0.0f;

		// **How far the cut is below the capsule's centre.**
		//
		// The body is a capsule about the *middle* of the fallen stem and the
		// mesh is drawn from its *cut*, and for a long time nothing carried
		// the distance between them -- the drawing simply put the mesh origin
		// at the body's position. So the moment a tree came off its stump the
		// crown jumped half its own length straight up, which is what "it
		// disappears and comes back a metre in the air" is.
		float Lift = 0.0f;

		// **The hinge: the strip of uncut wood on the far side of the notch.**
		// Where the butt is pinned while the tree goes over, and which way it
		// turns. Cleared once it is most of the way down, after which it is an
		// ordinary body again.
		glm::vec3 Hinge = glm::vec3(0.0f);
		glm::vec3 HingeAxis = glm::vec3(1.0f, 0.0f, 0.0f);
		bool Hinged = false;

		glm::vec3 Leaf = glm::vec3(0.2f);
		glm::vec3 Bark = glm::vec3(0.2f);
	};

	// The transform the fallen crown is drawn with. One function, because the
	// only way to know the mesh lands where the standing tree left it is to
	// ask the same matrix the renderer uses.
	glm::mat4 FelledFrame(const Felled& fell) const
	{
		const Egss::RigidBody3D& body = m_World.GetBody(fell.Body);

		// Read right to left: lift the mesh so its own cut height is at the
		// origin, scale it, slide it down the stem to where the cut is, then
		// place and turn it with the body.
		return glm::translate(glm::mat4(1.0f), body.Position)
			* glm::mat4_cast(body.Orientation)
			* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -fell.Lift, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(fell.Scale))
			* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -fell.CutY, 0.0f));
	}

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
	// The six habits, in one place, because the tree meshes and anything that
	// wants to check them both need the same table.
	static void TreeShapes(Veg::TreeParams* shapes)
	{
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
	}

	// **Where each habit belongs.** Not a classification of real forests --
	// two numbers cannot carry one -- but the same two numbers the rest of the
	// demo runs on, used to say something rather than nothing.
	float HabitWeight(int habit, const glm::vec2& climate) const
	{
		float wet = climate.x;
		float warm = climate.y;

		// Mild: neither cold nor hot. A ridge rather than a ramp, because
		// broadleaves give out at both ends and a ramp only knows one.
		float mild = glm::smoothstep(0.20f, 0.45f, warm)
			* glm::smoothstep(0.95f, 0.60f, warm);

		switch (habit)
		{
			// Spire: a conifer, and conifers win where the season is short.
			case 0: return 0.15f + 1.5f * glm::smoothstep(0.55f, 0.15f, warm);

			// Vase: an open-grown broadleaf, wanting mild and wet.
			case 1: return 1.4f * mild * glm::smoothstep(0.55f, 0.85f, wet);

			// Mop: the same country, denser canopy.
			case 2: return 1.1f * mild * glm::smoothstep(0.60f, 0.90f, wet);

			// Sapling: everywhere, in small numbers. A wood with no young
			// trees in it is a plantation.
			case 3: return 0.35f;

			// Parasol: a flat wide crown over a bare trunk, which is what an
			// isolated tree on warm open ground grows into.
			case 4: return 1.2f * glm::smoothstep(0.50f, 0.85f, warm)
				* glm::smoothstep(0.85f, 0.45f, wet);

			// Scrub: the dry edge, where the others give out.
			case 5: return 1.6f * glm::smoothstep(0.80f, 0.45f, wet);
		}

		return 0.0f;
	}

	// **Leaves are not one green.**
	//
	// The differences are real and they follow the climate. A leaf in a hot
	// dry place is small, thick, waxy and pale -- that is sclerophylly, and
	// the wax is there to keep water in, which is also why it looks grey-green
	// rather than green. A conifer needle is dark, nearly blue-green, because
	// a leaf that has to work in a short cool season packs in chlorophyll and
	// keeps it all winter. A broadleaf in mild wet country is the yellow-green
	// everyone means by "green" because it can afford a thin cheap leaf and
	// throw it away each autumn.
	glm::vec3 LeafColour(const glm::vec2& climate, float jitter) const
	{
		glm::vec3 broadleaf(0.19f, 0.38f, 0.12f);
		glm::vec3 needle(0.11f, 0.24f, 0.17f);
		glm::vec3 sclerophyll(0.36f, 0.38f, 0.21f);

		float cold = glm::smoothstep(0.50f, 0.15f, climate.y);

		float arid = glm::smoothstep(0.60f, 0.22f, climate.x)
			* glm::smoothstep(0.35f, 0.70f, climate.y);

		glm::vec3 colour = glm::mix(broadleaf, needle, cold);

		colour = glm::mix(colour, sclerophyll, arid);

		// Crown to crown, so a wood is not one flat colour. Hue as well as
		// value, for the same reason the grass gets both.
		colour *= 0.84f + 0.32f * jitter;

		// Rotating the channels is a hue shift with no maths in it. GLM's
		// swizzles are off in this build, so it is written out.
		glm::vec3 shifted(colour.g, colour.b, colour.r);

		return glm::mix(colour, shifted, 0.10f * jitter);
	}

	// Bark goes the other way: pale and smooth where it is dry and bright,
	// dark and wet-looking under a closed canopy.
	glm::vec3 BarkColour(const glm::vec2& climate, float jitter) const
	{
		glm::vec3 dark(0.22f, 0.16f, 0.11f);
		glm::vec3 pale(0.46f, 0.40f, 0.31f);

		float open = glm::smoothstep(0.75f, 0.30f, climate.x);

		return glm::mix(dark, pale, open) * (0.86f + 0.28f * jitter);
	}

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

			// Keyed to the ground rather than to the triangle's place in the
			// list -- see `Veg::ScatterKey`.
			int key = Veg::ScatterKey(centre.x, centre.y, centre.z);

			float chance = wet * warm * m_TreeDensity * (0.5f * area2);

			if (chance <= 1e-4f)
				continue;

			int count = (int)chance;

			if (Veg::Hash2DUnit(key, 0, seed) < chance - (float)count)
				count++;

			for (int i = 0; i < count; i++)
			{
				float u = Veg::Hash2DUnit(key, i * 4 + 1, seed);
				float v = Veg::Hash2DUnit(key, i * 4 + 2, seed);
				float su = std::sqrt(u);

				glm::vec3 root = a + (b - a) * (su * (1.0f - v))
					+ (c - a) * (su * v);

				// Nothing was left standing where the pad was cut.
				if (!OnSurface(root) || OnShedPad(root.x, root.z, 1.0f))
					continue;

				Tree tree;
				tree.At = root;
				tree.Yaw = Veg::Hash2DUnit(key, i * 4 + 3, seed) * 6.2831853f;

				// Only a jitter now. The size classes carry the range, and
				// they carry it with the trunk thickened to match -- which a
				// uniform scale cannot do. See `BuildTrees`.
				tree.Scale = 0.88f
					+ Veg::Hash2DUnit(key, i * 4 + 4, seed) * 0.24f;

				// **Which habit grows here is a question about the climate.**
				//
				// A wood of six architectures mixed evenly is a botanical
				// garden. Weighting them by where they belong -- conifers
				// where it is cold, broadleaves where it is mild and wet,
				// a flat-crowned tree on warm open ground, scrub at the dry
				// edge -- is what makes the change from one biome to the next
				// read as a different *country* rather than as a colour
				// change. The panel's checkboxes still hold: a habit switched
				// off has no weight anywhere, so a wood of one habit is still
				// one checkbox away.
				float total = 0.0f;
				float weight[s_TreeShapes];

				for (int k = 0; k < s_TreeShapes; k++)
				{
					weight[k] = m_ShapeOn[k] ? HabitWeight(k, climate) : 0.0f;
					total += weight[k];
				}

				tree.Shape = 0;

				if (total > 1e-5f)
				{
					float pick = Veg::Hash2DUnit(key, i * 4 + 5, seed) * total;

					for (int k = 0; k < s_TreeShapes; k++)
					{
						pick -= weight[k];

						if (pick <= 0.0f)
						{
							tree.Shape = k;
							break;
						}
					}
				}
				else
				{
					continue;
				}

				float lot = Veg::Hash2DUnit(key, i * 4 + 6, seed);

				tree.Size = 0;

				for (int k = 0; k < s_TreeSizes; k++)
				{
					lot -= s_TreeShare[k];

					if (lot <= 0.0f)
					{
						tree.Size = k;
						break;
					}
				}

				tree.Leaf = LeafColour(climate,
					Veg::Hash2DUnit(key, i * 4 + 7, seed));

				tree.Bark = BarkColour(climate,
					Veg::Hash2DUnit(key, i * 4 + 8, seed));

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

	// --- Boulders, where boulders actually end up -----------------------------
	//
	// **Rock is not scattered evenly and never has been.** The first pass here
	// dropped grey spheres from the sky at uniformly random points and let the
	// solver settle them, which produces exactly what it sounds like: balls
	// resting on hilltops, balls rolling down slopes for ever, and none of it
	// saying anything about the ground it is on. Boulders are placed now, not
	// dropped, and where they are placed is a statement about the terrain.
	//
	// Three rules, all of them things you can go outside and check.
	//
	// **Scree collects at the foot of steep ground, not on it.** Loose rock
	// on a slope steeper than the angle of repose keeps moving; below it, it
	// stops. So a site needs two things at once -- its own slope at or under
	// repose so a block can rest, and steep ground *above* it to have supplied
	// the block in the first place. That second half is what makes a boulder
	// field mean something: it says there is a cliff up there.
	//
	// **Bedrock shows through where soil cannot stay.** Ground steeper than
	// about forty degrees keeps no soil, so what is on it is the rock itself:
	// a few large blocks, mostly buried, part of the face rather than lying on
	// it.
	//
	// **Soil buries stone and the lack of it does not.** A meadow and a wood
	// have a soil profile and a litter layer over the top, and a surface stone
	// is under both within a few centuries. Frost-shattered tundra, bare rock
	// and stony desert have neither -- in a desert the wind takes the fines
	// away and leaves the coarse behind, which is a lag deposit and is why a
	// stony desert is stony. So the biome does not decide whether rock exists;
	// it decides whether you can see it.
	//
	// The angle of repose for angular rock debris is about 34 degrees. That is
	// a measured property of loose material and not a number anyone here
	// chose, which is why it is the one written as an angle.
	static constexpr float s_Repose = 0.6745f;    // tan 34 degrees

	// And the threshold hillslope angle, which is the one that decides whether
	// ground *supplies* debris rather than whether it holds it. Near 30
	// degrees in soil-mantled country, and lower than repose on purpose.
	static constexpr float s_Threshold = 0.5774f; // tan 30 degrees

	// **Sizes follow a power law, because fragmentation does.** Broken rock
	// has a fractal size distribution -- N(>d) proportional to d^-b with b
	// near 2.5 -- so a boulder field is a great many cobbles with a handful of
	// blocks in it, and the handful is what you notice. Drawing sizes from a
	// uniform range instead gives a field of identical lumps, which is the
	// other half of why the old rocks read as props.
	static constexpr float s_Fragment = 2.5f;
	static constexpr float s_StoneMin = 0.22f;    // metres, radius
	static constexpr float s_StoneMax = 2.4f;

	// How many can have a collider. Bodies cannot be removed from the world,
	// only rewritten, so the pool is fixed and the unused ones are parked
	// where nothing else ever goes. See `SyncStoneBodies`.
	static constexpr int s_StoneBodies = 512;

	struct Stone
	{
		glm::vec3 At;        // centre, already sunk into the ground
		glm::vec3 Radii;     // three axes; a clast is not a ball

		// The frame it came to rest in, as a basis rather than a quaternion:
		// the only thing that reads it builds a matrix from it, and a basis
		// is one `glm::mat4` cast away while a quaternion needs a GTX header
		// for the shortest-arc constructor.
		glm::mat3 Lie;
		glm::vec3 Colour;
		int Mesh = 0;
	};

	// **How much steep ground stands above a point**, which is where scree
	// comes from. The gradient of the height field points uphill, so walk that
	// way and ask how steep it is; a source further off contributes less,
	// because a block has to get here.
	float Supply(float x, float z) const
	{
		const float h = 2.0f;

		glm::vec2 uphill(
			(Height(x + h, z) - Height(x - h, z)) / (2.0f * h),
			(Height(x, z + h) - Height(x, z - h)) / (2.0f * h));

		float len = glm::length(uphill);

		if (len < 1e-4f)
			return 0.0f;

		uphill /= len;

		float best = 0.0f;

		for (float away : { 5.0f, 11.0f, 19.0f, 29.0f })
		{
			float px = x + uphill.x * away;
			float pz = z + uphill.y * away;

			float sx = (Height(px + h, pz) - Height(px - h, pz)) / (2.0f * h);
			float sz = (Height(px, pz + h) - Height(px, pz - h)) / (2.0f * h);

			float slope = std::sqrt(sx * sx + sz * sz);

			// **The shedding threshold is not the angle of repose**, and
			// getting those two confused put two boulders on the whole map.
			// Repose is the angle at which loose material *stops*; what
			// decides whether a hillside delivers rock downhill is the
			// threshold hillslope angle, which in soil-mantled country stands
			// near 30 degrees -- lower, because a slope does not have to be
			// bare to move debris down it, and every real landscape has far
			// more ground above 30 degrees than above 34.
			float sheds = glm::smoothstep(s_Threshold * 0.75f,
				s_Threshold * 1.25f, slope);

			best = glm::max(best, sheds * std::exp(-away / 18.0f));
		}

		return best;
	}

	// **Scatter belongs on the surface, and the inside of a hole is not one.**
	//
	// Digging exposes new triangles, and every one of them is ground as far as
	// the scatterers are concerned -- so boulders appeared in the hole the
	// moment it was dug, on faces that are steep because they are the *wall of
	// a hole* rather than because they are a crag. Three of them turned up for
	// a two-metre dig.
	//
	// The generator's own height function says where the outside is. A metre
	// and a half of slack covers the mesher's interpolation and any ground the
	// player has *added*, which should still grow things; anything well below
	// the surface is an interior and gets nothing.
	static constexpr float s_SurfaceSlack = 1.5f;

	bool OnSurface(const glm::vec3& at) const
	{
		return at.y > Height(at.x, at.z) - s_SurfaceSlack;
	}

	void BuildChunkStones(size_t key, const glm::ivec3& chunk,
		const Egss::MeshData& data)
	{
		m_Stones.erase(key);

		if (!m_ShowStones || m_StoneDensity <= 0.0f || data.Indices.size() < 3)
			return;

		unsigned int seed = 9311u + (unsigned int)(chunk.x * 73 + chunk.y * 19
			+ chunk.z * 131);

		std::vector<Stone> stones;

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

			glm::vec3 centre = (a + b + c) / 3.0f;

			// Nothing on the lake bed: it would be under the water, where the
			// only thing you could tell about it is that it is there.
			if (HasWater() && centre.y < WaterLevel() - 0.3f)
				continue;

			// **The slope of the landscape, not the slope of the mesh.**
			//
			// Reading it off the triangle's normal makes the rules describe
			// the *mesh*, which the player can edit -- so the wall of a hole
			// two metres across counts as "too steep for soil" and sprouts an
			// outcrop, and three boulders appeared round the rim of every dig.
			// The height field says where the crags are, and no amount of
			// digging changes it.
			//
			// The mesh's normal is still what a stone is bedded against; it is
			// only the *rule* that comes from the landscape.
			const float step = 2.0f;

			float gx = (Height(centre.x + step, centre.z)
				- Height(centre.x - step, centre.z)) / (2.0f * step);
			float gz = (Height(centre.x, centre.z + step)
				- Height(centre.x, centre.z - step)) / (2.0f * step);

			float slope = std::sqrt(gx * gx + gz * gz);

			// **Talus** -- shallow enough to hold a block, with steep ground
			// above to have delivered one.
			float rests = 1.0f - glm::smoothstep(s_Repose * 0.85f,
				s_Repose * 1.15f, slope);

			float supply = Supply(centre.x, centre.z);

			float talus = rests * supply;

			// **Outcrop** -- too steep for soil, so what is there is rock.
			float outcrop = glm::smoothstep(s_Repose, s_Repose * 1.5f, slope);

			glm::vec2 climate = ClimateAt(centre.x, centre.z);

			// Soil and litter bury a surface stone; where neither forms, it
			// stays where the last cold winter or the last flood left it.
			float soil = glm::smoothstep(0.15f, 0.55f, climate.x)
				* glm::smoothstep(0.10f, 0.35f, climate.y);

			float bare = 1.0f - 0.80f * soil;

			// **Fields, not a sprinkle.** A boulder field has edges; rock
			// scattered at one density over a whole landscape is a texture.
			// One slow octave decides where the stony ground is.
			float patch = glm::smoothstep(-0.15f, 0.35f,
				Noise2D(centre.x / 47.0f, centre.z / 47.0f, m_Shape.Seed + 613u));

			float weight = (talus * patch + outcrop * 0.55f) * bare;

			int key = Veg::ScatterKey(centre.x, centre.y, centre.z);

			float chance = weight * m_StoneDensity * (0.5f * area2);

			if (chance <= 1e-4f)
				continue;

			int count = (int)chance;

			if (Veg::Hash2DUnit(key, 0, seed) < chance - (float)count)
				count++;

			for (int i = 0; i < count; i++)
			{
				float u = Veg::Hash2DUnit(key, i * 6 + 1, seed);
				float v = Veg::Hash2DUnit(key, i * 6 + 2, seed);
				float su = std::sqrt(u);

				glm::vec3 at = a + (b - a) * (su * (1.0f - v))
					+ (c - a) * (su * v);

				// The pad was levelled; anything that was lying on it was
				// cleared with the earth.
				if (OnShedPad(at.x, at.z, 1.0f))
					continue;

				// Asked at the stone's own place rather than at the triangle's
				// centre: a big triangle on the rim of a hole can have its
				// centre above the surface and its corners well under.
				if (!OnSurface(at))
					continue;

				// The power law, by inverse transform: N(>d) goes as d^-b, so
				// d = dmin * U^(-1/b). Most come out small; a few do not, and
				// the few are the ones that make the field.
				float lot = glm::max(Veg::Hash2DUnit(key, i * 6 + 3, seed),
					1e-4f);

				float size = s_StoneMin * std::pow(lot, -1.0f / s_Fragment)
					* m_StoneSize;

				// **Fall sorting.** A big block carries more momentum than a
				// cobble and rolls further from the cliff, so the base of a
				// talus slope is coarser than its head. `supply` is largest
				// close to the source, so leaning against it puts the large
				// blocks at the outside of the field.
				size *= 0.75f + 0.5f * (1.0f - supply);

				size = glm::clamp(size, s_StoneMin, s_StoneMax * m_StoneSize);

				// A clast is not a ball. Axial ratios near the middle of what
				// river and scree gravels actually measure: b/a about 0.7,
				// c/a about 0.5.
				float mid = 0.60f + Veg::Hash2DUnit(key, i * 6 + 4, seed) * 0.28f;
				float shortest = 0.38f + Veg::Hash2DUnit(key, i * 6 + 5, seed) * 0.28f;

				Stone stone;
				stone.Radii = glm::vec3(size, size * shortest, size * mid);

				// **Settled means lying on its flattest face.** Loose clasts
				// come to rest with the short axis upright -- that is the
				// lowest centre of mass, and it is why a shingle beach is flat
				// and not a heap of edges. Tilted a little off the ground's
				// normal so a field does not look combed.
				float spin = Veg::Hash2DUnit(key, i * 6 + 6, seed) * 6.2831853f;
				float tip = (Veg::Hash2DUnit(key, i * 6 + 7, seed) - 0.5f) * 0.5f;

				// Part way toward the face's normal rather than all the way:
				// a block wedged on a slope leans into it, but not as far as
				// the slope, or a talus field looks like a comb.
				glm::vec3 up = glm::normalize(glm::mix(
					glm::vec3(0.0f, 1.0f, 0.0f), n, 0.65f));

				// And tipped a little off that, so no two lie alike.
				up = glm::normalize(up + glm::vec3(std::cos(spin) * tip, 0.0f,
					std::sin(spin) * tip));

				glm::vec3 reference = std::abs(up.y) < 0.9f
					? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

				glm::vec3 east = glm::normalize(glm::cross(reference, up));
				glm::vec3 north = glm::cross(up, east);

				glm::vec3 across = east * std::cos(spin) + north * std::sin(spin);

				// **The third column is `cross(X, Y)` and it has to be.**
				//
				// This was `north * cos - east * sin`, which is the same
				// direction *negated* -- so the basis was left-handed, the
				// matrix had determinant -1, and every boulder was drawn
				// inside out. Back-face culling then showed the inside of the
				// mesh: from a distance a rock is a rock either way, and close
				// up you are standing inside it looking at the far wall.
				//
				// Measured -1.0000 at every spin angle, +1.0000 with this.
				//
				// `Grass.h` builds `east`/`north` the same way and is fine,
				// because it only ever uses them as offset directions. The
				// handedness matters the moment they become the columns of a
				// transform.
				stone.Lie = glm::mat3(across, up, glm::cross(across, up));

				// **Bedded, not balanced.** A stone that has been there any
				// time at all is part buried -- frost heave and washed-in
				// fines see to it -- and a stone sitting exactly on the
				// surface reads as scenery dropped on the ground, which is
				// what these looked like. Sinking by a third of the short
				// axis is enough for the eye and cheap enough to be free.
				float buried = 0.30f + 0.25f
					* Veg::Hash2DUnit(key, i * 6 + 8, seed);

				stone.At = at + up * stone.Radii.y * (1.0f - 2.0f * buried);

				stone.Mesh = (int)(Veg::Hash2DUnit(key, i * 6 + 9, seed)
					* (float)s_StoneMeshes) % s_StoneMeshes;

				stone.Colour = StoneColour(climate,
					Veg::Hash2DUnit(key, i * 6 + 10, seed));

				stones.push_back(stone);
			}
		}

		if (!stones.empty())
			m_Stones[key] = std::move(stones);
	}

	// **Rock is grey until the climate gets at it.**
	//
	// In a hot dry place a boulder acquires desert varnish -- a dark
	// manganese-and-iron film that takes millennia and turns an exposed face
	// nearly brown-black. In a cold wet one it acquires lichen, which is the
	// pale grey-green that makes an upland boulder field look the colour it
	// does. Both are coatings on the same granite, so this is a tint on one
	// base rather than three rock types.
	glm::vec3 StoneColour(const glm::vec2& climate, float jitter) const
	{
		glm::vec3 granite(0.44f, 0.42f, 0.40f);
		glm::vec3 varnish(0.34f, 0.23f, 0.16f);
		glm::vec3 lichen(0.46f, 0.50f, 0.42f);

		float arid = glm::smoothstep(0.35f, 0.08f, climate.x)
			* glm::smoothstep(0.45f, 0.75f, climate.y);

		float damp = glm::smoothstep(0.40f, 0.80f, climate.x)
			* glm::smoothstep(0.60f, 0.25f, climate.y);

		glm::vec3 colour = glm::mix(granite, varnish, arid * 0.62f);

		colour = glm::mix(colour, lichen, damp * 0.55f);

		// Grain to grain, so two boulders side by side are not the same stone.
		return colour * (0.82f + 0.36f * jitter);
	}

	// **The colliders, from a pool that is rewritten rather than rebuilt.**
	//
	// `PhysicsWorld3D` has no way to remove a body -- and it does not need one
	// for this, because a static body can be *moved*. The pool is allocated
	// once at the size of the cap; each rebuild writes the boulders that exist
	// into the front of it and parks the rest a kilometre underneath the
	// world, where nothing else ever goes. Exactly the trick `Dig` uses on the
	// ground body, for exactly the same reason.
	//
	// One sphere for a three-axis boulder is a deliberate under-approximation:
	// the sphere is the *short* axis, so you can walk right up to a big flat
	// slab and never be stopped by air. Being stopped by nothing is a bug and
	// being stopped early is a shape.
	void SyncStoneBodies()
	{
		while ((int)m_StoneBodies.size() < s_StoneBodies)
		{
			Egss::RigidBody3D body = Egss::RigidBody3D::MakeStaticSphere(
				glm::vec3(0.0f, -1000.0f, 0.0f), 0.1f);

			body.Friction = 0.8f;
			body.Restitution = 0.0f;

			m_StoneBodies.push_back(m_World.AddBody(body));
		}

		int at = 0;

		for (const auto& entry : m_Stones)
		for (const Stone& stone : entry.second)
		{
			if (at >= s_StoneBodies)
				break;

			Egss::RigidBody3D& body = m_World.GetBody(m_StoneBodies[at++]);

			body = Egss::RigidBody3D::MakeStaticSphere(stone.At,
				glm::min(glm::min(stone.Radii.x, stone.Radii.y),
					stone.Radii.z));

			body.Friction = 0.8f;
			body.Restitution = 0.0f;
		}

		m_StoneCount = at;

		for (; at < s_StoneBodies; at++)
		{
			Egss::RigidBody3D& body = m_World.GetBody(m_StoneBodies[at]);

			body.Position = glm::vec3(0.0f, -1000.0f, 0.0f);
		}
	}

	// --- Loose bodies and buoyancy --------------------------------------------

	// **Everything in the water is a box with a density.**
	//
	// A log, a cobble, a length of timber, a weight to put on it: the only
	// thing that separates them is how heavy they are for their size, which is
	// the whole point of doing buoyancy properly rather than tagging things as
	// floaty.
	enum class Flotsam { Driftwood, Cobble, Plank, Weight, Log };

	struct Loose
	{
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		glm::vec3 Half = glm::vec3(0.5f);
		glm::vec3 Colour = glm::vec3(0.5f);
		Flotsam Kind = Flotsam::Driftwood;

		// **How much of its own box the thing actually is.** A plank is all of
		// it; a log is a cylinder in a square box, which is pi/4. Mass and
		// displacement both scale by it, so a round log floats at the draft a
		// round log floats at rather than at a squared beam's.
		float Fill = 1.0f;

		// In your hands, so gravity and water are somebody else's problem.
		bool Carried = false;

		// **Which slot of which pile it is stacked in, or -1 for loose.**
		// A stacked board is a static body sitting exactly where the layout
		// puts it -- not a dynamic one that happens to have settled there.
		// Eighty boards off one log will not settle into a stack; they will
		// find a way out of each other and go down the hill.
		int Stack = -1;
		int Pile = 0;
	};

	// **A fixed pool, because bodies can be rewritten and not removed.**
	//
	// Same reason the boulders have one. It also fixes a leak nobody had
	// noticed: "Reset the water" called `ScatterLoose` again, which cleared the
	// list and added a fresh set of bodies while the old ones stayed in the
	// world for ever -- so every press doubled the physics.
	static constexpr int s_LoosePool = 200;

	std::vector<Egss::PhysicsWorld3D::BodyHandle> m_LoosePool;

	// **Archimedes, and now the shape of what is under water as well.**
	//
	// The upward force on a submerged body is the weight of the fluid it
	// displaces, `rho g V`, and whether a thing floats is therefore whether its
	// own density is under the water's. That was the whole model and it was
	// enough to make a log swim and a cobble sink.
	//
	// It cannot make a plank tip when you put a weight on one end. A force
	// applied through the centre of mass has no moment about the centre of
	// mass, whatever the shape is doing -- so the old version could get the
	// *draft* of anything right and the *attitude* of nothing.
	//
	// **What produces the moment is that the displaced volume is not centred on
	// the body.** Heel a plank and more of one side goes under; the centroid of
	// the submerged part -- the centre of buoyancy -- shifts that way, and the
	// upward force through the new centroid is a couple that rights it. That is
	// metacentric stability, and it is a property of the *shape* of the
	// submerged part rather than of its size.
	//
	// Clipping the box against the waterline analytically would give it
	// exactly. Summing over a lattice of sample points gives it to whatever
	// resolution is paid for, in about ten lines, and generalises to any shape
	// the moment one turns up -- so that is what this does. Each sample carries
	// its share of the volume, asks how much of its own cell is under the
	// surface, and pushes up *there*.
	//
	// **The share is a smooth fraction, not in-or-out.** A 38 mm plank three
	// samples thick would otherwise quantise its own buoyancy to a third of its
	// volume and float in visible steps. For a horizontal waterline the
	// fraction of a cell that is under is exactly a clamped linear ramp, so
	// this costs nothing and is not an approximation.
	static constexpr int s_FloatSamples = 4;   // per axis, so 64 a body

	void ApplyBuoyancy(float dt)
	{
		if (!HasWater())
			return;

		const float water = 1000.0f;   // kg/m^3

		float level = WaterLevel();

		for (const Loose& loose : m_Loose)
		{
			Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			if (body.InverseMass <= 0.0f || loose.Carried)
				continue;

			// Cheap reject: nothing within its own diagonal of the surface can
			// be touching it.
			float reach = glm::length(loose.Half);

			if (body.Position.y - reach > level)
				continue;

			// **A sleeping body ignores forces, and buoyancy is a force.**
			//
			// The solver puts a body that has stopped moving to sleep, which is
			// right -- a boulder resting on a hillside should not be integrated
			// for ever. But a log that fell in the lake, hit the bed and slept
			// there stays asleep however hard the water pushes on it. It read
			// as buoyancy being too weak: the log sat at 0.276 submerged with
			// its own weight measurably greater than the force lifting it,
			// which is not an equilibrium at all and was the tell.
			body.Awake = true;

			glm::mat3 frame = glm::mat3_cast(body.Orientation);

			glm::vec3 cell = loose.Half / (float)s_FloatSamples;

			// `Fill` is what makes a round log displace a cylinder's worth
			// rather than its bounding box's -- pi/4 of it, which is 27 per
			// cent, and the difference between a log floating half out and a
			// log floating a third out.
			float share = 8.0f * cell.x * cell.y * cell.z * loose.Fill;

			int under = 0;

			for (int i = 0; i < s_FloatSamples; i++)
			for (int j = 0; j < s_FloatSamples; j++)
			for (int k = 0; k < s_FloatSamples; k++)
			{
				glm::vec3 local(
					(2.0f * (float)i + 1.0f - (float)s_FloatSamples) * cell.x,
					(2.0f * (float)j + 1.0f - (float)s_FloatSamples) * cell.y,
					(2.0f * (float)k + 1.0f - (float)s_FloatSamples) * cell.z);

				glm::vec3 at = body.Position + frame * local;

				// How much of this cell's own height is below the surface. The
				// cell is turned with the body, so its vertical extent is the
				// projection of its half-diagonal on the vertical -- which for
				// a box is the sum of the three axes' contributions.
				glm::vec3 up = glm::transpose(frame) * glm::vec3(0.0f, 1.0f, 0.0f);

				float span = std::abs(up.x) * cell.x + std::abs(up.y) * cell.y
					+ std::abs(up.z) * cell.z;

				float wet = glm::clamp((level - (at.y - span))
					/ glm::max(2.0f * span, 1e-5f), 0.0f, 1.0f);

				if (wet <= 0.0f)
					continue;

				under++;

				// Up is the weight of the water pushed aside, applied where it
				// is pushed aside. `ApplyImpulseAt` turns that into a force and
				// a torque about the centre of mass, which is the whole reason
				// this is a lattice and not one number.
				glm::vec3 lift(0.0f, water * 9.81f * share * wet * dt, 0.0f);

				m_World.ApplyImpulseAt(loose.Body, lift, at);

				// **And water is thick.** Without drag a floating body is a
				// spring with no damper: it overshoots the surface, leaves the
				// water, falls back and bobs for ever. Quadratic, like the air
				// drag, but with a thousand times the density behind it --
				// which is why a log bobs twice and settles. Sampled too, so a
				// plank slapping down flat is damped harder than one slicing in
				// edgewise, which is the difference between drag on a shape and
				// drag on a number.
				glm::vec3 flow = body.Velocity
					+ glm::cross(body.AngularVelocity, frame * local);

				float speed = glm::length(flow);

				if (speed > 1e-4f)
				{
					// The cell's share of the body's *plan* area -- divided
					// by the number of layers, so summing over all of them
					// gives the plan area once rather than once per layer.
					// Plan area is the dominant term for something bobbing;
					// it is not the right one for a plank swinging end-over-
					// end, and this is damping rather than a measurement.
					float area = 4.0f * cell.x * cell.z
						/ (float)s_FloatSamples;

					m_World.ApplyImpulseAt(loose.Body, -flow
						* (0.5f * water * 0.9f * area * speed * wet * dt), at);
				}
			}

			(void)under;
		}
	}

	// **What is in the water, and why each of it is there.**
	//
	// Driftwood and a cobble are the buoyancy test as it was: one thing lighter
	// than water and one heavier, both settling under Archimedes rather than
	// under a flag. The planks and the weights are the test the sampled version
	// makes possible -- a 2x4 is thin enough that where you put a weight on it
	// changes what it does, which is exactly the thing a force through the
	// centre of mass cannot show.
	//
	// **Real timber sizes, because the numbers are the point.** A 2x4 is
	// 38 x 89 mm, and at 2.4 m long that is 8.12 litres. Pine at 500 kg/m^3
	// makes it 4.06 kg with 4.06 kg of reserve buoyancy, so it floats with
	// exactly half its 38 mm under -- its centre level with the water. Each
	// weight is a 70 mm block of concrete at 2400 kg/m^3, which is 823 g. Five
	// of them is 4.11 kg and sinks it; four is 3.29 kg and does not. All of
	// that follows from the sizes and none of it is written down in the code,
	// which is what makes it worth measuring.
	static constexpr float s_PlankHalf[3] = { 1.2f, 0.019f, 0.0445f };
	static constexpr float s_WeightHalf = 0.035f;

	static constexpr float s_Pine = 500.0f;
	static constexpr float s_Concrete = 2400.0f;
	static constexpr float s_Granite = 2650.0f;

	static constexpr int s_WeightCount = 12;

	// Everything unused is parked a kilometre below the world, where nothing
	// else ever goes -- the same trick `Dig` uses on the ground body.
	void ParkLoose(size_t from)
	{
		for (size_t i = from; i < m_LoosePool.size(); i++)
		{
			Egss::RigidBody3D& body = m_World.GetBody(m_LoosePool[i]);

			body = Egss::RigidBody3D::MakeStaticSphere(
				glm::vec3(0.0f, -1000.0f, 0.0f), 0.05f);
		}
	}

	int AddLoose(const glm::vec3& at, const glm::vec3& half, float density,
		Flotsam kind, const glm::vec3& colour, float yaw = 0.0f,
		float fill = 1.0f)
	{
		while ((int)m_LoosePool.size() < s_LoosePool)
			m_LoosePool.push_back(m_World.AddBody(
				Egss::RigidBody3D::MakeStaticSphere(
					glm::vec3(0.0f, -1000.0f, 0.0f), 0.05f)));

		if (m_Loose.size() >= m_LoosePool.size())
			return -1;

		Loose loose;
		loose.Half = half;
		loose.Colour = colour;
		loose.Kind = kind;
		loose.Fill = fill;
		loose.Body = m_LoosePool[m_Loose.size()];

		float mass = density * fill * 8.0f * half.x * half.y * half.z;

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(at, half, mass);

		body.Orientation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
		body.UpdateInertiaWorld();

		body.Friction = 0.7f;
		body.Restitution = 0.02f;

		// **Air drag stays off.** These are metres across and the demo has no
		// wind force on solids; the water supplies its own damping and leaving
		// the default on would slow a falling plank for no stated reason.
		body.LinearDamping = 0.0f;
		body.AngularDamping = 0.02f;

		m_World.GetBody(loose.Body) = body;

		m_Loose.push_back(loose);

		return (int)m_Loose.size() - 1;
	}

	void ScatterLoose()
	{
		m_Loose.clear();
		m_Stacked = 0;
		m_Carry.clear();

		unsigned int seed = 20261u;

		float level = HasWater() ? WaterLevel() : 0.0f;

		// The old flotsam: in and around the lake, because that is the only
		// place it says anything.
		for (int i = 0; i < m_LooseCount; i++)
		{
			float angle = Veg::Hash2DUnit(i, 1, seed) * 6.2831853f;
			float reach = std::sqrt(Veg::Hash2DUnit(i, 2, seed))
				* glm::max(m_Shape.BasinSize * 0.55f, 6.0f);

			glm::vec3 at(std::cos(angle) * reach, 0.0f, std::sin(angle) * reach);

			at.y = glm::max(Height(at.x, at.z), level) + 3.0f
				+ Veg::Hash2DUnit(i, 3, seed) * 4.0f;

			bool wood = Veg::Hash2DUnit(i, 4, seed) < 0.34f;

			float size = 0.5f + Veg::Hash2DUnit(i, 5, seed) * 0.9f;

			AddLoose(at, glm::vec3(size), wood ? s_Pine : s_Granite,
				wood ? Flotsam::Driftwood : Flotsam::Cobble,
				wood ? glm::vec3(0.42f, 0.30f, 0.18f)
					: glm::vec3(0.40f, 0.39f, 0.37f));
		}

		if (!HasWater())
			return;

		glm::vec3 plankHalf(s_PlankHalf[0], s_PlankHalf[1], s_PlankHalf[2]);

		// Five planks laid out where you can walk to them: a lone one to load
		// off centre, and a raft of four side by side to stand things on.
		AddLoose(glm::vec3(-6.0f, level + 0.6f, 4.0f), plankHalf, s_Pine,
			Flotsam::Plank, glm::vec3(0.62f, 0.47f, 0.29f));

		for (int i = 0; i < 4; i++)
			AddLoose(glm::vec3(4.0f, level + 0.6f,
				-1.0f + (float)i * (2.2f * s_PlankHalf[2] + 0.01f)),
				plankHalf, s_Pine, Flotsam::Plank,
				glm::vec3(0.58f + 0.03f * (float)i, 0.44f, 0.27f));

		// The weights start in a heap on the shore, so picking one up is
		// walking over and looking at it rather than opening a menu.
		for (int i = 0; i < s_WeightCount; i++)
		{
			float angle = (float)i * 0.9f;

			glm::vec3 at(-2.0f + std::cos(angle) * 0.6f, 0.0f,
				10.0f + std::sin(angle) * 0.6f);

			at.y = glm::max(Height(at.x, at.z), level) + 0.6f
				+ 0.2f * (float)i;

			AddLoose(at, glm::vec3(s_WeightHalf), s_Concrete, Flotsam::Weight,
				glm::vec3(0.46f, 0.46f, 0.48f));
		}

		ParkLoose(m_Loose.size());
	}

	// **Put a weight where you are looking.**
	//
	// The nearest weight not already placed is moved to the point the eye ray
	// meets a plank, the water or the ground, and dropped from rest. That is
	// the whole interaction: no inventory, no carry, no throw -- because the
	// question being asked is "what does the plank do with the load *here*",
	// and anything that adds a velocity to the answer is in the way.
	void PlaceWeight()
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float best = 6.0f;
		glm::vec3 landing(0.0f);
		bool found = false;

		// Against the planks first, as oriented boxes -- the slab test in each
		// one's own frame, which is three intervals intersected.
		for (const Loose& loose : m_Loose)
		{
			if (loose.Kind != Flotsam::Plank)
				continue;

			const Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			glm::mat3 frame = glm::mat3_cast(body.Orientation);
			glm::mat3 into = glm::transpose(frame);

			glm::vec3 from = into * (origin - body.Position);
			glm::vec3 along = into * direction;

			float near = 0.0f, far = best;
			bool miss = false;

			for (int axis = 0; axis < 3 && !miss; axis++)
			{
				if (std::abs(along[axis]) < 1e-6f)
				{
					miss = std::abs(from[axis]) > loose.Half[axis];
					continue;
				}

				float a = (-loose.Half[axis] - from[axis]) / along[axis];
				float b = (loose.Half[axis] - from[axis]) / along[axis];

				if (a > b)
					std::swap(a, b);

				near = glm::max(near, a);
				far = glm::min(far, b);

				miss = near > far;
			}

			if (miss || near >= best)
				continue;

			best = near;
			landing = origin + direction * near
				+ glm::vec3(0.0f, s_WeightHalf + 0.01f, 0.0f);
			found = true;
		}

		// Otherwise the water surface or the ground, whichever the ray reaches
		// first. Marched rather than solved, because the ground is a field.
		if (!found)
		{
			for (float t = 0.4f; t <= 6.0f; t += 0.06f)
			{
				glm::vec3 at = origin + direction * t;

				bool wet = HasWater() && at.y <= WaterLevel();

				if (wet || at.y <= Height(at.x, at.z))
				{
					landing = at + glm::vec3(0.0f, s_WeightHalf + 0.02f, 0.0f);
					found = true;
					break;
				}
			}
		}

		if (!found)
			return;

		// Whichever weight is furthest from where it is wanted, so repeated
		// presses lay them out rather than fighting over one.
		int pick = -1;
		float away = -1.0f;

		for (size_t i = 0; i < m_Loose.size(); i++)
		{
			if (m_Loose[i].Kind != Flotsam::Weight)
				continue;

			float d = glm::length(
				m_World.GetBody(m_Loose[i].Body).Position - landing);

			if (d > away)
			{
				away = d;
				pick = (int)i;
			}
		}

		if (pick < 0)
			return;

		Egss::RigidBody3D& body = m_World.GetBody(m_Loose[(size_t)pick].Body);

		body.Position = landing;
		body.PreviousPosition = landing;
		body.Velocity = glm::vec3(0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		body.UpdateInertiaWorld();
		body.Awake = true;

		m_Stacked++;
	}

	// --- The axe --------------------------------------------------------------

	// Where the axe hangs when it is not in your hands. On the wall opposite
	// the shed's door, so you can see it as you walk in.
	glm::vec3 AxeRack() const
	{
		return ShedCentre() + glm::vec3(0.0f, 1.55f, s_ShedHalf - 0.32f);
	}

	void ToggleAxe()
	{
		const Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		// Taken and returned at the rack, so the axe cannot be dropped in a
		// field and lost. There is one of it, the way there is one portal.
		if (glm::length(body.Position - AxeRack()) < 2.6f)
			m_HasAxe = !m_HasAxe;
	}

	// **A tree's frame, from world space.** The instance transform is
	// `translate(At) * rotate(Yaw about +Y) * scale(Scale)`, so undoing it is
	// the same three steps backwards. Written out rather than inverting a
	// matrix because the tree's own coordinates are what the cut is stored in
	// and it is worth being able to read the conversion.
	static glm::vec3 IntoTree(const Tree& tree, const glm::vec3& at)
	{
		glm::vec3 offset = at - tree.At;

		float c = std::cos(tree.Yaw), sn = std::sin(tree.Yaw);

		// `rotate(+Yaw)` sends (x, z) to (x c + z s, -x s + z c), so the
		// inverse sends it back the other way.
		return glm::vec3(offset.x * c - offset.z * sn, offset.y,
			offset.x * sn + offset.z * c) / glm::max(tree.Scale, 1e-4f);
	}

	// **Which tree the axe is lined up on, and where on it.**
	//
	// A ray against an upright cylinder round the trunk, which is what the
	// lower part of every one of these habits is. Twice the trunk's radius, so
	// aiming is a matter of looking at the tree rather than at a particular
	// column of pixels, and only the bottom half of the tree -- swinging an
	// axe at a canopy eight metres up should find nothing.
	void AimAxe()
	{
		m_AimTree = -1;

		if (!m_HasAxe)
			return;

		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float best = s_AxeReach;

		for (auto& entry : m_Trees)
		for (size_t i = 0; i < entry.second.size(); i++)
		{
			Tree& tree = entry.second[i];

			if (tree.Severed)
				continue;

			float radius = m_TreeTrunk[tree.Shape][tree.Size] * tree.Scale * 2.0f;
			float top = m_TreeTop[tree.Shape][tree.Size] * tree.Scale * 0.5f;

			// The circle, in the ground plane. `t` is along the ray.
			glm::vec2 to(origin.x - tree.At.x, origin.z - tree.At.z);
			glm::vec2 along(direction.x, direction.z);

			float a = glm::dot(along, along);

			if (a < 1e-6f)
				continue;

			float b = 2.0f * glm::dot(to, along);
			float c = glm::dot(to, to) - radius * radius;

			float discriminant = b * b - 4.0f * a * c;

			if (discriminant < 0.0f)
				continue;

			float t = (-b - std::sqrt(discriminant)) / (2.0f * a);

			// Inside the cylinder already: take the exit rather than a
			// negative entry, so standing against a trunk still aims at it.
			if (t < 0.0f)
				t = (-b + std::sqrt(discriminant)) / (2.0f * a);

			if (t < 0.0f || t > best)
				continue;

			glm::vec3 hit = origin + direction * t;

			float height = hit.y - tree.At.y;

			if (height < 0.05f || height > top)
				continue;

			best = t;

			m_AimKey = entry.first;
			m_AimTree = (int)i;
			m_AimY = height / glm::max(tree.Scale, 1e-4f);

			glm::vec3 local = IntoTree(tree, hit);

			glm::vec2 side(local.x, local.z);

			float span = glm::length(side);

			m_AimSide = span > 1e-4f ? side / span : glm::vec2(1.0f, 0.0f);
		}
	}

	// One stroke. The wedge deepens where you were aiming; when it is through,
	// the top comes off.
	void Swing()
	{
		m_Swing = 1.0f;

		// Nothing standing in front of you: maybe something lying down. A
		// felled top first, then a log -- one key, and which job it does is
		// whatever the axe is actually pointed at.
		if (m_AimTree < 0)
		{
			if (!BuckNearest())
				RiveNearest();

			return;
		}

		auto found = m_Trees.find(m_AimKey);

		if (found == m_Trees.end() || m_AimTree >= (int)found->second.size())
			return;

		Tree& tree = found->second[(size_t)m_AimTree];

		// A fresh cut, or the same one deepened. Moving the aim more than a
		// notch's width away starts again, which is what happens if you do it
		// with a real axe as well.
		float radius = m_TreeTrunk[tree.Shape][tree.Size] * tree.Scale;

		if (tree.CutDepth <= 0.0f
			|| std::abs(m_AimY - tree.CutY) * tree.Scale > radius * s_AxeKerf)
		{
			tree.CutY = m_AimY;
			tree.CutSide = m_AimSide;
			tree.CutDepth = 0.0f;
		}

		m_Chopped++;

		// **A stroke takes a fixed bite of wood, so a thick trunk takes more
		// of them.** Hit points would be a second rule that could disagree
		// with the one the notch is drawn from; this is the same number twice.
		tree.CutDepth += s_AxeBite / glm::max(2.0f * radius, 0.05f);

		if (tree.CutDepth >= 1.0f)
		{
			tree.CutDepth = 1.0f;
			Sever(tree);
		}
	}

	// The top comes off and becomes a body. The stump keeps standing, drawn
	// from the same mesh with everything above the cut discarded.
	void Sever(Tree& tree)
	{
		tree.Severed = true;
		m_Felled++;

		while ((int)m_Fell.size() < s_FelledMax)
		{
			Felled spare;

			Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(
				glm::vec3(0.0f, -1000.0f, 0.0f), glm::vec3(0.5f), 1.0f);

			body.Type = Egss::BodyType::Static;

			spare.Body = m_World.AddBody(body);

			m_Fell.push_back(spare);
		}

		// Oldest first, so felling a seventeenth tree retires the first.
		Felled& slot = m_Fell[(size_t)(m_Felled - 1) % (size_t)s_FelledMax];

		slot.Active = true;
		slot.Shape = tree.Shape;
		slot.Size = tree.Size;
		slot.Scale = tree.Scale;
		slot.CutY = tree.CutY;
		slot.Leaf = tree.Leaf;
		slot.Bark = tree.Bark;

		float scale = tree.Scale;

		float above = glm::max(m_TreeTop[tree.Shape][tree.Size] - tree.CutY,
			0.2f) * scale;

		// **A capsule along the trunk, not a box round the canopy.**
		//
		// The box was the obvious choice and it was wrong by four metres: its
		// half-extents have to cover the crown, so a wide top rests on a cube
		// four metres across and the whole tree floats above the ground on a
		// collider nothing can see. A felled tree is a long cylinder that
		// happens to have twigs on it, and the twigs are not what it lies on.
		//
		// The radius is a couple of trunk-widths, which is about what a fallen
		// trunk with its branch stubs actually rests on.
		float radius = glm::max(m_TreeTrunk[tree.Shape][tree.Size] * scale
			* 2.5f, 0.22f);

		float halfHeight = glm::max(above * 0.5f - radius, 0.05f);

		// **Mass from the wood, not from the collider.** A crown is mostly
		// air, so filling anything its size with timber gives a five-metre top
		// the mass of a small car and it lands like one. A trunk of this
		// radius plus a share for the branches is nearer the truth.
		float timber = 3.14159265f * radius * radius * (2.0f * halfHeight);

		float mass = 700.0f * 0.55f * timber;

		glm::vec3 root = tree.At + glm::vec3(0.0f, tree.CutY * scale, 0.0f);

		glm::vec3 centre = root + glm::vec3(0.0f, above * 0.5f, 0.0f);

		// The cut is half the stem's length below the capsule's centre, which
		// is the number the drawing needs and never had.
		slot.Lift = above * 0.5f;
		slot.Hinge = root;
		slot.Hinged = true;

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(centre, radius,
			halfHeight, glm::max(mass, 20.0f));

		body.Friction = 0.85f;
		body.Restitution = 0.0f;

		// A little viscous damping for the tumble through the air; what stops
		// it on the ground is `StepFelled`, which is a different mechanism and
		// has to be.
		body.AngularDamping = 0.15f;
		body.LinearDamping = 0.05f;

		// **It falls the way it was cut, and it falls about its butt.**
		//
		// A hinge on the uncut side is what sends a felled tree where the
		// cutter meant it to go; the wedge is on `CutSide`, so it tips that
		// way. Two things were wrong with the old nudge and both showed.
		//
		// The sign. `omega x r` for a point at `r = (0, h, 0)` under
		// `omega = (0, 0, w)` is `(-w h, 0, 0)`, so tipping the top toward
		// `+x` wants `w` negative -- and `cross(up, +x)` is already
		// `(0, 0, -1)`. Negating it as well sent every tree over backwards,
		// away from its own notch, while a separate linear nudge pushed the
		// centre forwards. The two disagreed, which is most of why it read as
		// a glitch rather than as a fall.
		//
		// And the linear part has to be the one the spin implies. A body given
		// a spin about its centre and a velocity picked separately has no
		// stationary point: the butt slides sideways off the stump. Setting
		// `v = omega x (centre - butt)` makes the butt instantaneously still,
		// which is what a hinge is, and the tree pivots instead of leaping.
		float c = std::cos(tree.Yaw), sn = std::sin(tree.Yaw);

		glm::vec3 fall(tree.CutSide.x * c + tree.CutSide.y * sn, 0.0f,
			-tree.CutSide.x * sn + tree.CutSide.y * c);

		slot.HingeAxis = glm::normalize(
			glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), fall));

		// **A nudge sized in metres a second at the tip, not in radians a
		// second at the middle.** A fixed spin gives a 27 m standard a crown
		// already doing nine metres a second at the instant of the cut, which
		// is about the speed it should be doing when it *lands*. Everything a
		// person sees is the tip, so the tip is what the push is quoted in.
		slot.Hinged = true;

		body.AngularVelocity = slot.HingeAxis
			* (s_TipPush / glm::max(above, 0.5f));

		body.Velocity = glm::cross(body.AngularVelocity,
			glm::vec3(0.0f, above * 0.5f, 0.0f));

		body.Awake = true;

		m_World.GetBody(slot.Body) = body;
	}

	// **Rolling resistance, which the solver has not got and a log needs.**
	//
	// A capsule is a cylinder and a cylinder rolls. The first felled top went
	// fourteen metres down the hill and was still doing six metres a second
	// ten seconds later -- the same fault the loose stones had as spheres, and
	// this time the shape cannot be blamed, because a felled trunk really is a
	// cylinder.
	//
	// Turning up the angular damping does not fix it either, and the reason is
	// worth stating: **viscous damping gives a terminal speed, not a stop.**
	// Set against gravity on a slope it settles at whatever speed the two
	// balance at and stays there. Raising it far enough to look stopped also
	// makes the tree fall as though through treacle.
	//
	// What stops a real log is rolling resistance, which is Coulomb: the
	// ground deforms under the load, the contact patch sits ahead of the axis,
	// and the couple that results is set by the **weight**, not by the speed.
	//
	//     torque = mu_r N R
	//
	// with mu_r near a quarter for timber on soil, against a hundredth for a
	// tyre on tarmac. That is why a log does not roll away and a wheel does.
	// It also gives a clean prediction: a cylinder rolls only where the slope
	// exceeds `atan(mu_r)`, about 14 degrees, and stops everywhere else.
	//
	// Capped so it can only bring the spin to zero and never reverse it --
	// friction does not drive things.
	static constexpr float s_RollResistance = 0.25f;

	// **A felled tree turns on its hinge, and gravity is a torque about the
	// stump rather than a pull on its middle.**
	//
	// Cut through and let go and the crown simply drops. The stump is not a
	// collider, so it free-falls whatever the cut height was, lands upright,
	// and topples from there -- which is most of what "it disappears and comes
	// back about a metre in the air" is describing. No amount of damping fixes
	// it, because the problem is that nothing is holding the butt.
	//
	// What holds it in a real felling is the hinge: the strip of uncut wood on
	// the far side of the notch, which holds until the tree is most of the way
	// over and is the whole reason a feller can aim one. With it, the crown is
	// a rod pivoted at one end, and that has an equation of motion:
	//
	//     I = m L^2 / 3,   torque = m g (L/2) sin(theta)
	//     =>  alpha = (3 g / 2 L) sin(theta)
	//
	// with theta from upright. This supplies that torque and then puts the
	// butt back on the stump, which is what a constraint does. It runs *after*
	// the solver has integrated, because a projection applied before the step
	// is a projection the step undoes.
	//
	// The hinge lets go at 75 degrees, and the rest is an ordinary falling
	// body landing on the ground.
	static constexpr float s_TipPush = 0.80f;      // m/s at the tip, at the cut
	static constexpr float s_HingeBreak = 75.0f;   // degrees over

	void HoldFalling(float dt)
	{
		for (Felled& fell : m_Fell)
		{
			if (!fell.Active || !fell.Hinged)
				continue;

			Egss::RigidBody3D& body = m_World.GetBody(fell.Body);

			glm::vec3 axis = glm::mat3_cast(body.Orientation)
				* glm::vec3(0.0f, 1.0f, 0.0f);

			float lean = std::acos(glm::clamp(axis.y, -1.0f, 1.0f));

			if (lean > glm::radians(s_HingeBreak))
			{
				fell.Hinged = false;
				continue;
			}

			float stem = glm::max(2.0f * fell.Lift, 0.2f);

			float alpha = 1.5f * 9.81f * std::sin(lean) / stem;

			float spin = glm::dot(body.AngularVelocity, fell.HingeAxis)
				+ alpha * dt;

			body.AngularVelocity = fell.HingeAxis * spin;

			// The butt goes back on the stump, and the linear velocity is the
			// one the spin implies -- anything else and the two describe
			// different motions.
			body.Position = fell.Hinge + axis * fell.Lift;

			body.Velocity = glm::cross(body.AngularVelocity,
				body.Position - fell.Hinge);

			body.Awake = true;
		}
	}

	void StepFelled(float dt)
	{
		for (Felled& fell : m_Fell)
		{
			if (!fell.Active)
				continue;

			Egss::RigidBody3D& body = m_World.GetBody(fell.Body);

			if (!body.Awake || body.InverseMass <= 0.0f)
				continue;

			// A tree still on its hinge is being turned, not rolled.
			if (fell.Hinged)
				continue;

			// Nothing to roll on in mid-air.
			if (body.Position.y - Height(body.Position.x, body.Position.z)
				> body.Radius * 1.8f)
				continue;

			float spin = glm::length(body.AngularVelocity);

			if (spin < 1e-4f)
				continue;

			float torque = s_RollResistance * body.GetMass() * 9.81f
				* body.Radius;

			glm::vec3 change = body.InverseInertiaWorld
				* (-body.AngularVelocity / spin * torque) * dt;

			body.AngularVelocity = glm::length(change) >= spin
				? glm::vec3(0.0f) : body.AngularVelocity + change;
		}
	}

	// --- Timber ---------------------------------------------------------------
	//
	// **A tree becomes logs, a log becomes planks, and each step is work.**
	//
	// Fell a tree and what lies on the ground is a whole top. Hit it with the
	// axe and it is bucked into logs of a standard length. Carry a log to the
	// bench in the shed and it is sawn into boards. Nothing is spawned from
	// nothing at any point: the number of planks that comes out of a log is
	// its volume, times what a mill actually recovers.
	//
	// **Real timber sizes throughout, because the numbers are the point.** A
	// "2x4" is nominal: rough-sawn off the mill it is a full 50 x 100 mm, and
	// only after planing is it the 38 x 89 mm the floating ones are. Boards
	// come off the saw rough, so these are 50 x 100 x 2.4 m -- which is also
	// why they read as timber rather than as lath.
	static constexpr float s_LogRadius = 0.17f;
	static constexpr float s_LogLength = 2.5f;

	static constexpr float s_BoardHalf[3] = { 1.2f, 0.025f, 0.05f };

	// **Sawmill recovery.** A round log cannot become square boards without
	// loss: the slabs off the outside, the edgings, and a 4 mm kerf on every
	// cut. Sixty per cent by volume is what a small mill gets out of a
	// straight log, and it is the only reason a quarter-cubic-metre log does
	// not turn into twenty-one planks.
	static constexpr float s_MillRecovery = 0.60f;

	glm::vec3 SawBench() const
	{
		return ShedCentre() + glm::vec3(s_ShedHalf - 0.9f, 0.45f, 1.6f);
	}

	glm::vec3 CraftTable() const
	{
		return ShedCentre() + glm::vec3(-(s_ShedHalf - 0.9f), 0.45f, 1.6f);
	}

	// --- The piles ------------------------------------------------------------
	//
	// **Boards are stacked, not dropped.**
	//
	// A board off the saw is a dynamic body, and eighty of them out of one
	// butt log will not settle into a pile: they find a way out of each other,
	// spread across the floor and go out of the door. Trying to make them
	// settle is the wrong fix -- a stack of sawn timber is *stacked*, by hand,
	// one board at a time, and a person putting boards down does not leave
	// them where they land either.
	//
	// So a board that reaches a pile is placed: it goes to the next slot of a
	// layout and becomes static. That is the same trick the bedded boulders
	// use -- a thing that is where it is put and never simulated -- and it is
	// what makes the pile readable. Take one off the top and it is a dynamic
	// body again.
	//
	// Two piles, because the timber comes off the saw at one bench and is
	// spent at the other: green off the mill beside the saw, seasoned stock
	// beside the crafting table where the editor is. Carrying between them is
	// the work.
	enum class Pile { Mill = 0, Stock = 1 };

	static constexpr int s_PileAcross = 8;      // boards to a course
	static constexpr float s_PileGap = 0.004f;  // sticker between boards

	glm::vec3 PileAt(Pile pile) const
	{
		// Clear of the bench and against the wall behind it, so the pile is
		// beside the work rather than under it.
		// Lifted by the bearers, which is where sawn timber sits: off the
		// floor, so air gets under the bottom course.
		return ShedCentre() + (pile == Pile::Mill
			? glm::vec3(s_ShedHalf - 1.1f, 0.06f, -1.3f)
			: glm::vec3(-(s_ShedHalf - 1.1f), 0.06f, -1.3f));
	}

	// Where the n-th board of a pile sits. Boards lie along the room's x axis,
	// courses run across it, and the layers go up.
	glm::vec3 PileSlot(Pile pile, int n) const
	{
		int across = n % s_PileAcross;
		int layer = n / s_PileAcross;

		float wide = BoardWide() + s_PileGap;

		return PileAt(pile) + glm::vec3(0.0f,
			BoardThick() * ((float)layer + 0.5f),
			((float)across - 0.5f * (float)(s_PileAcross - 1)) * wide);
	}

	// How far from a pile counts as reaching it.
	static constexpr float s_PileReach = 2.2f;

	int PileCount(Pile pile) const
	{
		int total = 0;

		for (const Loose& loose : m_Loose)
			if (loose.Stack >= 0 && loose.Pile == (int)pile)
				total++;

		return total;
	}

	// The lowest slot nobody is in, so a pile taken from the top and added to
	// again fills the hole rather than leaving a gap in the air.
	int NextPileSlot(Pile pile) const
	{
		for (int slot = 0; slot < s_LoosePool; slot++)
		{
			bool taken = false;

			for (const Loose& loose : m_Loose)
				if (loose.Stack == slot && loose.Pile == (int)pile)
				{
					taken = true;
					break;
				}

			if (!taken)
				return slot;
		}

		return -1;
	}

	// The topmost board of a pile, which is the one a hand reaches first.
	int TopOfPile(Pile pile) const
	{
		int best = -1, highest = -1;

		for (size_t i = 0; i < m_Loose.size(); i++)
			if (m_Loose[i].Stack > highest && m_Loose[i].Pile == (int)pile)
			{
				highest = m_Loose[i].Stack;
				best = (int)i;
			}

		return best;
	}

	void PutOnPile(int which, Pile pile)
	{
		int slot = NextPileSlot(pile);

		if (slot < 0)
			return;

		Loose& board = m_Loose[(size_t)which];

		board.Stack = slot;
		board.Pile = (int)pile;

		Egss::RigidBody3D& body = m_World.GetBody(board.Body);

		body.Position = PileSlot(pile, slot);
		body.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		body.Velocity = glm::vec3(0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.Type = Egss::BodyType::Static;
		body.Awake = false;

		body.UpdateInertiaWorld();
	}

	void TakeOffPile(int which)
	{
		Loose& board = m_Loose[(size_t)which];

		board.Stack = -1;

		Egss::RigidBody3D& body = m_World.GetBody(board.Body);

		body.Type = Egss::BodyType::Dynamic;
		body.Awake = true;
	}

	// Which pile, if any, you are standing at.
	int PileHere(Pile& pile) const
	{
		float best = s_PileReach;
		int found = -1;

		glm::vec3 at = m_Camera.GetPosition();

		for (int i = 0; i < 2; i++)
		{
			float range = glm::length(at - PileAt((Pile)i)
				- glm::vec3(0.0f, s_EyeHeight, 0.0f));

			if (range > best)
				continue;

			best = range;
			found = i;
			pile = (Pile)i;
		}

		return found;
	}

	float LogVolume() const
	{
		return glm::pi<float>() * s_LogRadius * s_LogRadius * s_LogLength;
	}

	float BoardVolume() const
	{
		return 8.0f * s_BoardHalf[0] * s_BoardHalf[1] * s_BoardHalf[2];
	}

	// The wood in one loose piece: its box, times how much of the box it is.
	float LooseVolume(const Loose& loose) const
	{
		return loose.Fill * 8.0f * loose.Half.x * loose.Half.y * loose.Half.z;
	}

	// **How many boards a log is worth is its own volume, not a constant.**
	//
	// `BoardsPerLog` returned eleven for every log, and it was right to,
	// because every log was the same 0.17 m by 2.5 m cylinder. A stem tapers
	// and trees come in sizes, so a butt log off a standard is worth dozens
	// and a top log off a pole is worth two -- and nothing has to say so.
	int BoardsFrom(const Loose& loose) const
	{
		return glm::max(1, (int)(s_MillRecovery * LooseVolume(loose)
			/ BoardVolume()));
	}

	// The nominal log the timber sizes were fitted against, kept only so the
	// panel can quote a yardstick.
	int BoardsPerLog() const
	{
		return glm::max(1, (int)(s_MillRecovery * LogVolume() / BoardVolume()));
	}

	// **The top diameter limit is why the tip of a felled tree is left in the
	// wood.** Below about 100 mm at the small end a length of stem will not
	// square up into a board: it is brash, not timber. Every forestry
	// measurement of a standing tree stops at exactly this rule, and it is
	// what stops a sapling from yielding anything at all.
	static constexpr float s_TopDiameter = 0.10f;

	int CountTimber(Flotsam kind) const
	{
		int total = 0;

		for (const Loose& loose : m_Loose)
			if (loose.Kind == kind)
				total++;

		return total;
	}

	// **Bucking.** A felled top is one long body; the axe cuts it into logs.
	// How many is how long it was, which is why a big tree is worth more than
	// a small one without anything having to say so.
	bool BuckNearest()
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		int best = -1;
		float nearest = s_AxeReach;

		for (size_t i = 0; i < m_Fell.size(); i++)
		{
			if (!m_Fell[i].Active)
				continue;

			const Egss::RigidBody3D& body = m_World.GetBody(m_Fell[i].Body);

			glm::vec3 to = body.Position - origin;

			float along = glm::dot(to, direction);

			if (along < 0.0f || along > nearest)
				continue;

			// Distance from the ray to the capsule's centre line, near enough
			// at this range: the perpendicular distance to its centre plus its
			// own half-length is a generous but honest reach.
			float off = glm::length(to - direction * along);

			if (off > body.Radius + body.HalfHeight)
				continue;

			nearest = along;
			best = (int)i;
		}

		if (best < 0)
			return false;

		return BuckFelled((size_t)best);
	}

	// The cutting itself, split from the aiming so it can be asked a question
	// without a camera pointed at anything.
	bool BuckFelled(size_t which)
	{
		Felled& fell = m_Fell[which];

		const Egss::RigidBody3D& body = m_World.GetBody(fell.Body);

		// **A log is the piece of stem it was cut from, and a stem tapers.**
		//
		// This used to make N identical 0.17 m logs, so a 24 m conifer and a
		// 5 m pole gave the same timber and only the count differed. A stem is
		// a cone near enough: it carries the trunk's own radius at the cut and
		// runs out toward the tip, so the butt log is fat, each one above it
		// is thinner, and a big tree is worth felling without anything having
		// to say so.
		float stem = glm::max(2.0f * fell.Lift, 0.2f);

		float butt = glm::max(
			m_TreeTrunk[fell.Shape][fell.Size] * fell.Scale, 0.01f);

		glm::mat3 frame = glm::mat3_cast(body.Orientation);

		glm::vec3 along = frame * glm::vec3(0.0f, 1.0f, 0.0f);

		// The butt end, half a stem back down the capsule's own axis.
		glm::vec3 root = body.Position - along * fell.Lift;

		int logs = 0;

		for (int i = 0; i < s_MaxLogs; i++)
		{
			float from = (float)i * s_LogLength;
			float to = from + s_LogLength;

			if (to > stem)
				break;

			// The small end has to make the grade or the rest is brash.
			if (2.0f * butt * (1.0f - to / stem) < s_TopDiameter)
				break;

			float radius = butt * (1.0f - 0.5f * (from + to) / stem);

			glm::vec3 at = root + along * (0.5f * (from + to));

			// Set down on the ground rather than left along the axis of a
			// capsule that may be lying at any angle: logs of different
			// thickness strung along one line half bury the thin ones.
			at.y = glm::max(at.y, Height(at.x, at.z) + radius + 0.05f);

			float yaw = std::atan2(along.x, along.z);

			// A log is a cylinder in a square box: the collider and the
			// displacement are the box, the `Fill` is what makes both of them
			// a cylinder's.
			AddLoose(at, glm::vec3(0.5f * s_LogLength, radius, radius),
				s_Pine, Flotsam::Log, glm::vec3(0.44f, 0.31f, 0.18f),
				yaw - glm::half_pi<float>(), glm::pi<float>() * 0.25f);

			logs++;
		}

		// The top is gone -- it is these logs now, or it was brash and there
		// is nothing to show for it, which is what felling a sapling is worth.
		fell.Active = false;

		m_World.GetBody(fell.Body) = Egss::RigidBody3D::MakeStaticSphere(
			glm::vec3(0.0f, -1000.0f, 0.0f), 0.05f);

		m_Bucked += logs;

		return true;
	}

	// **Riving: a log too heavy to shoulder is split down its length.**
	//
	// A butt log off a standard is 1.3 m^3, which is six hundred kilograms of
	// pine. Nothing anybody lifts -- and the alternative to splitting it is
	// felling only small trees, which is the opposite of the point of having
	// sizes. Riving is what actually happens to a log that has to be moved by
	// hand: halved down the grain, and halved again, until a piece is a piece
	// a person can carry.
	//
	// **Nothing is lost, which is the same rule the rest of this runs on.**
	// Each half keeps the full length and half the cross-section, so the
	// radius goes down by sqrt(2) and the volume -- and therefore the board
	// count -- is exactly conserved.
	bool RiveNearest()
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		int best = -1;
		float nearest = s_AxeReach;

		for (size_t i = 0; i < m_Loose.size(); i++)
		{
			if (m_Loose[i].Kind != Flotsam::Log || m_Loose[i].Carried)
				continue;

			const Egss::RigidBody3D& body = m_World.GetBody(m_Loose[i].Body);

			glm::vec3 to = body.Position - origin;

			float along = glm::dot(to, direction);

			if (along < 0.0f || along > nearest)
				continue;

			if (glm::length(to - direction * along)
				> glm::length(m_Loose[i].Half))
				continue;

			nearest = along;
			best = (int)i;
		}

		if (best < 0)
			return false;

		Loose& log = m_Loose[(size_t)best];

		// Under a board's width there is nothing left to split into.
		if (2.0f * log.Half.y < 2.0f * s_BoardHalf[2])
			return false;

		float radius = log.Half.y * 0.70710678f;

		Egss::RigidBody3D& body = m_World.GetBody(log.Body);

		glm::vec3 at = body.Position;
		glm::quat turn = body.Orientation;

		// Across the log rather than along it, so the two halves fall apart
		// instead of into each other.
		glm::vec3 aside = glm::mat3_cast(turn) * glm::vec3(0.0f, 0.0f, 1.0f);

		log.Half = glm::vec3(log.Half.x, radius, radius);

		float mass = s_Pine * log.Fill * 8.0f * log.Half.x * radius * radius;

		body = Egss::RigidBody3D::MakeBox(at - aside * (radius * 1.05f),
			log.Half, mass);

		body.Orientation = turn;
		body.Friction = 0.7f;
		body.Restitution = 0.02f;
		body.AngularDamping = 0.02f;
		body.Awake = true;
		body.UpdateInertiaWorld();

		int made = AddLoose(at + aside * (radius * 1.05f), log.Half, s_Pine,
			Flotsam::Log, glm::vec3(0.46f, 0.33f, 0.20f), 0.0f, log.Fill);

		if (made >= 0)
			m_World.GetBody(m_Loose[(size_t)made].Body).Orientation = turn;

		m_Riven++;

		return true;
	}

	// --- Carrying -------------------------------------------------------------
	//
	// **Capacity is a mass, not a number of items.**
	//
	// One board at a time was honest and unusable: a 26-board floor panel is 26
	// walks across the shed. An armful is what a person actually does, and the
	// question "how big an armful" has one right answer -- as much as you can
	// lift -- which is a mass.
	//
	// It is a mass rather than a count for a second reason. When characters
	// have attributes, strength has to land somewhere, and a kilogram budget is
	// somewhere it can land without anything else being told: scale
	// `s_Strength` and the armful, the pace penalty and the panel all follow.
	// A count would have had to be re-derived per item type by hand.
	//
	// 40 kg is six rough 2x4s. That is a heavy but real armful of timber, and
	// it is deliberately not enough for a 113 kg log -- see `LoadFactor`.
	static constexpr float s_Strength = 1.0f;
	static constexpr float s_CarryPerStrength = 40.0f;

	// How far from the first piece the rest of an armful can be gathered. A
	// pile is about a metre across; reaching further would be picking timber
	// off the floor of the whole room in one press.
	static constexpr float s_ArmSpread = 1.6f;

	// **And a ceiling on a single piece.** "The first piece is always allowed"
	// was right while the heaviest thing in the world was a 113 kg log: over
	// the budget, but a struggle rather than a refusal. Butt logs now run to
	// six hundred kilograms, and there is no reading of "struggle" that covers
	// shouldering half a tonne. Four times the budget is the limit; past it
	// the answer is the axe, not the arms.
	static constexpr float s_LiftCeiling = 4.0f;

	float CarryLimit() const { return s_Strength * s_CarryPerStrength; }

	float LiftLimit() const { return CarryLimit() * s_LiftCeiling; }

	bool Carrying() const { return !m_Carry.empty(); }

	float CarriedMass() const
	{
		float total = 0.0f;

		for (int slot : m_Carry)
			total += m_World.GetBody(m_Loose[(size_t)slot].Body).GetMass();

		return total;
	}

	// **One object is a lift you either make or you do not, and you make it.**
	// A 2.5 m log is 113 kg, which is over the budget and over what one person
	// should shoulder -- but refusing it would take away something that already
	// works, and a struggle reads better than a refusal. So the first piece is
	// always allowed and being over the budget costs pace instead. This is the
	// other thing the strength attribute will pull on.
	float LoadFactor() const
	{
		float over = CarriedMass() / CarryLimit() - 1.0f;

		return 1.0f - 0.45f * glm::clamp(over, 0.0f, 1.0f);
	}

	// Put one piece down without dropping the rest of the armful.
	void Release(int slot)
	{
		for (size_t i = 0; i < m_Carry.size(); i++)
			if (m_Carry[i] == slot)
			{
				m_Carry.erase(m_Carry.begin() + (long)i);
				break;
			}

		Loose& held = m_Loose[(size_t)slot];

		held.Carried = false;

		Egss::RigidBody3D& body = m_World.GetBody(held.Body);

		body.Type = Egss::BodyType::Dynamic;
		body.Velocity = glm::vec3(0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.Awake = true;

		// **Nothing is ever put down below the floor you are standing on.**
		//
		// Asked at the *eye* rather than under the load: you cannot be
		// standing inside rock, so the surface under your feet is the one a
		// thing put down in front of you lands on. Asked under the load it
		// finds whatever is below *that*, which for a board pushed under the
		// shed floor is the hillside beneath the plinth -- a correct answer to
		// the wrong question.
		float floor = m_World.GroundHeightBelow(m_Camera.GetPosition(),
			m_Walker, -1000.0f);

		float reach = glm::max(glm::max(held.Half.x, held.Half.y), held.Half.z);

		if (body.Position.y - reach < floor)
			body.Position.y = floor + reach + 0.02f;

		// **Put down at a pile means stacked, not dropped.** A person setting
		// boards down beside a stack sets them on it.
		Pile pile = Pile::Stock;

		if (held.Kind == Flotsam::Plank && PileHere(pile) >= 0)
			PutOnPile(slot, pile);
	}

	// The first carried piece of a kind, which is what the bench mills.
	int CarriedOf(Flotsam kind) const
	{
		for (int slot : m_Carry)
			if (m_Loose[(size_t)slot].Kind == kind)
				return slot;

		return -1;
	}

	// **One press takes an armful.** Whatever you are looking at, plus every
	// piece of the same kind within reach of it that still fits the budget,
	// nearest first. Carried pieces are kinematic and parked in front of the
	// eye, so they neither fall nor shove you about while you walk.
	void ToggleCarry()
	{
		if (Carrying())
		{
			while (!m_Carry.empty())
				Release(m_Carry.back());

			return;
		}

		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		int best = -1;
		float nearest = 4.0f;

		for (size_t i = 0; i < m_Loose.size(); i++)
		{
			const Loose& loose = m_Loose[i];

			if (loose.Kind != Flotsam::Log && loose.Kind != Flotsam::Plank)
				continue;

			const Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			glm::vec3 to = body.Position - origin;

			float along = glm::dot(to, direction);

			if (along < 0.0f || along > nearest)
				continue;

			if (glm::length(to - direction * along) > glm::length(loose.Half))
				continue;

			nearest = along;
			best = (int)i;
		}

		if (best < 0)
			return;

		// Too heavy to shoulder at all. Nothing happens, and what to do about
		// it is rive it.
		if (m_World.GetBody(m_Loose[(size_t)best].Body).GetMass() > LiftLimit())
		{
			m_TooHeavy = 1.2f;
			return;
		}

		// **Reaching into a pile takes off the top of it**, whichever board
		// you were pointing at. Pulling one out of the middle of a stack
		// leaves a hole in the air, and nobody does that.
		int from = m_Loose[(size_t)best].Stack >= 0
			? m_Loose[(size_t)best].Pile : -1;

		if (from >= 0)
		{
			int top = TopOfPile((Pile)from);

			if (top >= 0)
				best = top;
		}

		Take(best);

		Flotsam kind = m_Loose[(size_t)best].Kind;

		// Off a pile, an armful comes off the top course by course.
		if (from >= 0)
		{
			for (;;)
			{
				int top = TopOfPile((Pile)from);

				if (top < 0)
					break;

				if (CarriedMass()
					+ m_World.GetBody(m_Loose[(size_t)top].Body).GetMass()
					> CarryLimit())
					break;

				Take(top);
			}

			return;
		}

		// The rest of the armful, nearest to the first piece outwards, while
		// there is budget left. Same kind only: an armful of boards with a log
		// balanced on it is not a thing anybody does.
		glm::vec3 pile = m_World.GetBody(m_Loose[(size_t)best].Body).Position;

		for (;;)
		{
			int add = -1;
			float closest = s_ArmSpread;

			for (size_t i = 0; i < m_Loose.size(); i++)
			{
				const Loose& loose = m_Loose[i];

				if (loose.Carried || loose.Kind != kind)
					continue;

				float range = glm::length(
					m_World.GetBody(loose.Body).Position - pile);

				if (range > closest)
					continue;

				closest = range;
				add = (int)i;
			}

			if (add < 0)
				break;

			if (CarriedMass()
				+ m_World.GetBody(m_Loose[(size_t)add].Body).GetMass()
				> CarryLimit())
				break;

			Take(add);
		}
	}

	void Take(int slot)
	{
		if (m_Loose[(size_t)slot].Stack >= 0)
			TakeOffPile(slot);

		m_Carry.push_back(slot);

		m_Loose[(size_t)slot].Carried = true;

		Egss::RigidBody3D& body = m_World.GetBody(m_Loose[(size_t)slot].Body);

		body.Type = Egss::BodyType::Kinematic;
		body.Velocity = glm::vec3(0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
	}

	// **How far in front of the eye a load can be held.**
	//
	// Carried timber is kinematic and parked at a fixed distance in front of
	// the eye, and that is exactly why it went through walls and into the
	// ground: a kinematic body is a body the solver has been told not to move,
	// so nothing pushes back on it. Put it down there and it is released
	// *inside* the world, penetrating on every side, and the solver's way out
	// of that is whichever side is shallowest -- often downward, which is the
	// board falling through the floor.
	//
	// So the arm shortens, which is what anyone carrying a plank into a
	// doorway does. Terrain by its own raycast; static boxes by a slab test in
	// each box's own frame, so an oriented panel is tested as the box it is
	// rather than as the box that contains it.
	float CarryReach(float want) const
	{
		glm::vec3 eye = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float reach = want;

		float distance = 0.0f;
		glm::vec3 point, normal;

		if (m_Field && m_Field->Raycast(eye, direction, want, distance, point,
			normal))
			reach = glm::min(reach, distance);

		for (const Egss::RigidBody3D& body : m_World.GetBodies())
		{
			if (body.Type != Egss::BodyType::Static
				|| body.Shape != Egss::ColliderShape3D::Box)
				continue;

			glm::mat3 frame = glm::mat3_cast(glm::conjugate(body.Orientation));

			glm::vec3 origin = frame * (eye - body.Position);
			glm::vec3 along = frame * direction;

			float entry = 0.0f, leave = reach;
			bool hit = true;

			for (int a = 0; a < 3 && hit; a++)
			{
				if (std::abs(along[a]) < 1e-6f)
				{
					hit = std::abs(origin[a]) <= body.HalfExtents[a];
					continue;
				}

				float t0 = (-body.HalfExtents[a] - origin[a]) / along[a];
				float t1 = (body.HalfExtents[a] - origin[a]) / along[a];

				if (t0 > t1)
					std::swap(t0, t1);

				entry = glm::max(entry, t0);
				leave = glm::min(leave, t1);

				hit = entry <= leave;
			}

			if (hit)
				reach = glm::min(reach, entry);
		}

		return reach;
	}

	// Held out in front, turned to lie across the view the way anyone carries a
	// length of timber, and stacked upwards so an armful reads as a stack
	// rather than as one board with the rest hidden inside it.
	void CarryTimber()
	{
		glm::vec3 forward = m_Camera.GetForward();
		glm::vec3 right = m_Camera.GetRight();

		float yaw = std::atan2(right.x, right.z) - glm::half_pi<float>();

		float lift = 0.0f;

		for (int slot : m_Carry)
		{
			if (slot < 0 || slot >= (int)m_Loose.size())
				continue;

			const Loose& held = m_Loose[(size_t)slot];

			Egss::RigidBody3D& body = m_World.GetBody(held.Body);

			// Carried low enough that a full armful tops out below the sight
			// line: six boards stack 0.32 m, and starting at the old 0.35 m
			// put the top one across the middle of the screen.
			//
			// The arm gives way at a wall rather than pushing the load into
			// it, and never folds up shorter than the body itself.
			float arm = glm::max(CarryReach(1.6f) - 0.30f, 0.55f);

			body.Position = m_Camera.GetPosition() + forward * arm
				- glm::vec3(0.0f, 0.62f - lift, 0.0f);

			body.Orientation = glm::angleAxis(yaw,
				glm::vec3(0.0f, 1.0f, 0.0f));

			body.Velocity = glm::vec3(0.0f);
			body.AngularVelocity = glm::vec3(0.0f);

			body.UpdateInertiaWorld();

			lift += 2.1f * held.Half.y;
		}
	}

	// **The bench turns one log into boards, and the count is arithmetic.**
	void MillLog()
	{
		int slot = CarriedOf(Flotsam::Log);

		if (slot < 0)
			return;

		Loose& held = m_Loose[(size_t)slot];

		const Egss::RigidBody3D& body = m_World.GetBody(held.Body);

		if (glm::length(body.Position - SawBench()) > 3.0f)
			return;

		int boards = BoardsFrom(held);

		// Put down the log and nothing else: the rest of the armful stays in
		// your hands, and the slot it was in becomes the first board.
		Release(slot);

		held.Kind = Flotsam::Plank;
		held.Half = glm::vec3(s_BoardHalf[0], s_BoardHalf[1], s_BoardHalf[2]);
		held.Fill = 1.0f;
		held.Colour = glm::vec3(0.66f, 0.51f, 0.32f);

		float mass = s_Pine * 8.0f * s_BoardHalf[0] * s_BoardHalf[1]
			* s_BoardHalf[2];

		Egss::RigidBody3D& first = m_World.GetBody(held.Body);

		first = Egss::RigidBody3D::MakeBox(PileSlot(Pile::Mill, 0), held.Half,
			mass);

		first.Friction = 0.7f;

		PutOnPile(slot, Pile::Mill);

		// **Straight onto the pile, because eighty loose boards is not a
		// pile.** A butt log is worth dozens of them; dropped as dynamic
		// bodies at the bench they push each other apart and half of them end
		// up outside the shed. Timber comes off a saw and is stacked, and the
		// stacking is the same one board at a time either way.
		int made = 1;

		for (int i = 1; i < boards; i++)
		{
			int entry = AddLoose(PileSlot(Pile::Mill, i), held.Half, s_Pine,
				Flotsam::Plank, held.Colour);

			if (entry < 0)
				break;

			PutOnPile(entry, Pile::Mill);
			made++;
		}

		m_Milled += made;
	}

	// --- Prefabs --------------------------------------------------------------
	//
	// **A design is a list of pieces on a grid, and the bill of materials is a
	// cutting-stock problem.**
	//
	// The first version of this was two sliders -- boards across, boards along
	// -- which makes a rectangle and nothing else. That is a configurator, not
	// an editor: a floor and a wall were two settings of the same object and
	// there was no third thing you could describe. Now the table has a plan
	// view, the grid is one board wide (100 mm), and a piece is put down where
	// you click, cut to whatever length you asked for.
	//
	// Everything else follows from the piece list. The panel's size is the
	// bounding box of what you drew. Its mass is the wood in it. And the number
	// of boards it costs is **how few 2.4 m boards those pieces can be cut
	// from**, which is a bin-packing question and not a multiplication -- three
	// 0.8 m ledgers come out of one board, two 2.0 m ones do not.
	//
	// Laying the design flat and standing it up is still one flag, because that
	// part was right: a floor and a wall are the same assembly at different
	// angles.
	static constexpr int s_MaxDesigns = 6;
	static constexpr int s_PanelPool = 32;

	// Reach at the table. The two piles are 6.2 m apart, so timber has to be
	// carried across the room rather than counted where it fell.
	static constexpr float s_TableReach = 3.0f;

	// The grid is measured in board widths, so a piece is always a whole number
	// of cells across and the arithmetic stays in integers. 48 cells is 4.8 m,
	// which is two boards end to end -- a two-course panel has to fit, and at
	// 32 cells it did not.
	static constexpr int s_GridCells = 48;
	static constexpr int s_BoardCells = 24;   // 2.4 m / 100 mm

	// **The cap exists because the design has to be replayable.** See the note
	// on `m_Draft`: each piece is one registered integer, and a registered
	// parameter is a fixed slot in the recording's parameter table.
	static constexpr int s_MaxParts = 64;

	struct Part
	{
		int X = 0;
		int Z = 0;
		int Length = s_BoardCells;   // cells along its run
		bool AlongZ = false;
		bool Face = true;            // the course on top, rather than under
	};

	// **A piece packs into one integer, and that integer is the design.**
	//
	// Not a `Part` struct with a packed copy kept beside it -- the code *is*
	// the stored form, decoded on demand. Two representations of the same thing
	// is two things to keep in step, and the one that gets forgotten is always
	// the one the recorder reads.
	//
	// Zero is "no piece", which works because a piece of zero length is not a
	// piece. Twenty-one bits used of thirty-one.
	//
	// **Six bits for the length, not five.** Five holds 0 to 31, and a piece
	// spanning the 48-cell grid is 48 -- which masked to 16, or to 0 for a
	// 32-cell one, and the piece then decoded as an empty slot and vanished
	// without a word. A packed field one bit too narrow does not fail, it
	// forgets.
	static int PackPart(const Part& part)
	{
		return 1 | ((part.X & 63) << 1) | ((part.Z & 63) << 7)
			| ((part.Length & 63) << 13) | ((part.AlongZ ? 1 : 0) << 19)
			| ((part.Face ? 1 : 0) << 20);
	}

	static bool UnpackPart(int code, Part& out)
	{
		if ((code & 1) == 0)
			return false;

		out.X = (code >> 1) & 63;
		out.Z = (code >> 7) & 63;
		out.Length = (code >> 13) & 63;
		out.AlongZ = ((code >> 19) & 1) != 0;
		out.Face = ((code >> 20) & 1) != 0;

		return out.Length > 0;
	}

	struct Design
	{
		char Name[24] = "Floor panel";
		bool Upright = false;
		int Code[s_MaxParts] = { 0 };
	};

	struct Panel
	{
		Design Plan;
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		glm::vec3 At = glm::vec3(0.0f);
		float Yaw = 0.0f;
		bool Placed = false;
	};

	float BoardLength() const { return 2.0f * s_BoardHalf[0]; }
	float BoardThick() const { return 2.0f * s_BoardHalf[1]; }
	float BoardWide() const { return 2.0f * s_BoardHalf[2]; }

	// The grid's cell, which is one board's width by definition -- boards laid
	// edge to edge are what the grid is for.
	float Cell() const { return BoardWide(); }

	std::vector<Part> PartsOf(const Design& plan) const
	{
		std::vector<Part> parts;

		for (int i = 0; i < s_MaxParts; i++)
		{
			Part part;

			if (UnpackPart(plan.Code[i], part))
				parts.push_back(part);
		}

		return parts;
	}

	int PartCount(const Design& plan) const
	{
		int total = 0;

		for (int i = 0; i < s_MaxParts; i++)
			if ((plan.Code[i] & 1) != 0)
				total++;

		return total;
	}

	// The design's extent in cells: [low, high) on each axis. Empty designs
	// report a single cell so nothing downstream divides by zero.
	void DesignBounds(const Design& plan, glm::ivec2& low, glm::ivec2& high) const
	{
		low = glm::ivec2(s_GridCells);
		high = glm::ivec2(0);

		bool any = false;

		for (const Part& part : PartsOf(plan))
		{
			glm::ivec2 from(part.X, part.Z);

			glm::ivec2 to = from + (part.AlongZ
				? glm::ivec2(1, part.Length) : glm::ivec2(part.Length, 1));

			low = glm::min(low, from);
			high = glm::max(high, to);

			any = true;
		}

		if (!any)
		{
			low = glm::ivec2(0);
			high = glm::ivec2(1);
		}
	}

	glm::vec2 PanelSpan(const Design& plan) const
	{
		glm::ivec2 low, high;
		DesignBounds(plan, low, high);

		return glm::vec2(high - low) * Cell();
	}

	// In panel space, which is flat: x and z across the plan, y through the
	// thickness. `Upright` is a rotation applied afterwards, so the extents do
	// not depend on it. Two courses thick whether or not both are used -- the
	// collider is the assembly's envelope, and a design that is all face still
	// wants somewhere for the ledgers to go if one is added.
	glm::vec3 PanelHalf(const Design& plan) const
	{
		glm::vec2 span = PanelSpan(plan);

		return { 0.5f * span.x, BoardThick(), 0.5f * span.y };
	}

	// **The bill is a cutting-stock problem, solved first-fit-decreasing.**
	//
	// The pieces are cut from 2.4 m boards, so what a design costs is how few
	// boards its pieces can be got out of. Longest piece first into the first
	// board it fits: that is FFD, it is within 11/9 of optimal and it is what a
	// person at a saw bench does anyway -- cut the long ones while the boards
	// are whole.
	//
	// A multiplication cannot answer this. Three 0.8 m ledgers come out of one
	// board and two 2.0 m ones do not, and both cases occur in the two designs
	// the table opens with.
	int PanelCost(const Design& plan) const
	{
		std::vector<int> pieces;

		// **A piece longer than a board is cut from more than one.** Scarfing a
		// 3.2 m ledger out of 2.4 m stock takes a full board and a 0.8 m
		// offcut, so it is billed as both. Clamping the length instead -- which
		// is what this did first -- charged 2.4 m for 3.2 m of timber and made
		// wide designs quietly cheaper than they are.
		for (const Part& part : PartsOf(plan))
		{
			int left = part.Length;

			while (left > s_BoardCells)
			{
				pieces.push_back(s_BoardCells);
				left -= s_BoardCells;
			}

			pieces.push_back(left);
		}

		std::sort(pieces.begin(), pieces.end(), std::greater<int>());

		std::vector<int> boards;

		for (int piece : pieces)
		{
			size_t fit = boards.size();

			for (size_t i = 0; i < boards.size(); i++)
				if (boards[i] >= piece)
				{
					fit = i;
					break;
				}

			if (fit == boards.size())
				boards.push_back(s_BoardCells);

			boards[fit] -= piece;
		}

		return (int)boards.size();
	}

	// The wood actually in the design, which is less than the wood it cost --
	// the difference is the offcut left on the bench.
	float PanelMass(const Design& plan) const
	{
		int cells = 0;

		for (const Part& part : PartsOf(plan))
			cells += part.Length;

		return s_Pine * (float)cells * Cell() * Cell() * BoardThick();
	}

	// What is left over, in whole boards' worth of length.
	float PanelOffcut(const Design& plan) const
	{
		int cells = 0;

		for (const Part& part : PartsOf(plan))
			cells += part.Length;

		return (float)(PanelCost(plan) * s_BoardCells - cells) * Cell();
	}

	// --- Editing --------------------------------------------------------------

	bool PartFits(const Design& plan, const Part& want) const
	{
		// The packed code holds six bits of length and the grid is 48 cells,
		// so anything outside that could not be stored faithfully anyway.
		if (want.Length < 1 || want.Length > s_GridCells)
			return false;

		glm::ivec2 from(want.X, want.Z);

		glm::ivec2 to = from + (want.AlongZ
			? glm::ivec2(1, want.Length) : glm::ivec2(want.Length, 1));

		if (from.x < 0 || from.y < 0 || to.x > s_GridCells || to.y > s_GridCells)
			return false;

		// Only against the same course: a ledger is *under* the face, which is
		// the entire point of there being two.
		for (const Part& part : PartsOf(plan))
		{
			if (part.Face != want.Face)
				continue;

			glm::ivec2 low(part.X, part.Z);

			glm::ivec2 high = low + (part.AlongZ
				? glm::ivec2(1, part.Length) : glm::ivec2(part.Length, 1));

			if (from.x < high.x && to.x > low.x
				&& from.y < high.y && to.y > low.y)
				return false;
		}

		return true;
	}

	bool AddPart(Design& plan, const Part& want) const
	{
		if (!PartFits(plan, want))
			return false;

		for (int i = 0; i < s_MaxParts; i++)
			if ((plan.Code[i] & 1) == 0)
			{
				plan.Code[i] = PackPart(want);
				return true;
			}

		return false;
	}

	// The piece covering a cell in one course, or -1. Used by the right button.
	int PartAt(const Design& plan, int x, int z, bool face) const
	{
		for (int i = 0; i < s_MaxParts; i++)
		{
			Part part;

			if (!UnpackPart(plan.Code[i], part) || part.Face != face)
				continue;

			glm::ivec2 low(part.X, part.Z);

			glm::ivec2 high = low + (part.AlongZ
				? glm::ivec2(1, part.Length) : glm::ivec2(part.Length, 1));

			if (x >= low.x && x < high.x && z >= low.y && z < high.y)
				return i;
		}

		return -1;
	}

	void ClearDesign(Design& plan) const
	{
		for (int i = 0; i < s_MaxParts; i++)
			plan.Code[i] = 0;
	}

	// **The old two-slider panel, kept as a template rather than as the
	// editor.** It is still the fastest way to get a floor or a wall, and it is
	// now a starting point you can cut about instead of the only shape there
	// is. `across` boards edge to edge, `courses` end to end, on two ledgers a
	// course.
	void FillPanel(Design& plan, int across, int courses) const
	{
		ClearDesign(plan);

		for (int course = 0; course < courses; course++)
		for (int board = 0; board < across; board++)
		{
			Part face;
			face.X = course * s_BoardCells;
			face.Z = board;
			face.Length = s_BoardCells;
			face.AlongZ = false;
			face.Face = true;

			if (!AddPart(plan, face))
				return;
		}

		for (int course = 0; course < courses; course++)
		for (int end = 0; end < 2; end++)
		{
			Part ledger;
			ledger.X = course * s_BoardCells
				+ (end ? s_BoardCells - 1 : 0);
			ledger.Z = 0;
			ledger.Length = across;
			ledger.AlongZ = true;
			ledger.Face = false;

			if (!AddPart(plan, ledger))
				return;
		}
	}

	// **How many boards are on the pile beside the table.**
	//
	// It used to be "every loose plank within 2.5 m of the table", which
	// counted whatever had rolled under it and could not tell a stack from a
	// mess. Boards on a pile are in slots, so the pile knows its own size.
	int Stockpile() const { return PileCount(Pile::Stock); }

	// **Removing one keeps the pool indexed by position.** `AddLoose` hands
	// out `m_LoosePool[m_Loose.size()]`, so an entry cannot simply be erased --
	// the next one added would be given a body another entry is still using.
	// The last entry's *contents* are moved into this slot's body instead.
	void RemoveLoose(size_t at)
	{
		size_t last = m_Loose.size() - 1;

		// Whatever is being removed is no longer in your hands, and whatever
		// moves into its slot has to take the carry index with it.
		for (size_t i = 0; i < m_Carry.size(); i++)
			if (m_Carry[i] == (int)at)
			{
				m_Carry.erase(m_Carry.begin() + (long)i);
				break;
			}

		if (at != last)
		{
			m_World.GetBody(m_Loose[at].Body) =
				m_World.GetBody(m_Loose[last].Body);

			Egss::PhysicsWorld3D::BodyHandle keep = m_Loose[at].Body;

			m_Loose[at] = m_Loose[last];
			m_Loose[at].Body = keep;

			for (int& slot : m_Carry)
				if (slot == (int)last)
					slot = (int)at;
		}

		m_Loose.pop_back();

		ParkLoose(m_Loose.size());
	}

	// **The table turns boards into a panel.** `C` rather than a button in the
	// panel: a key press is recorded and replayed, and an ImGui click is not,
	// so a session that built something from a button could never play itself
	// back. The sliders are registered parameters for the same reason.
	void CraftPanel()
	{
		if (glm::length(m_Camera.GetPosition() - CraftTable()) > s_TableReach)
			return;

		const Design& plan = m_Draft;

		int cost = PanelCost(plan);

		if (Stockpile() < cost)
			return;

		// Off the top, so the pile goes down course by course rather than
		// leaving a hole somewhere in the middle of it.
		for (int taken = 0; taken < cost; taken++)
		{
			int top = TopOfPile(Pile::Stock);

			if (top < 0)
				return;

			RemoveLoose((size_t)top);
		}

		while ((int)m_PanelPool.size() < s_PanelPool)
			m_PanelPool.push_back(m_World.AddBody(
				Egss::RigidBody3D::MakeStaticSphere(
					glm::vec3(0.0f, -1000.0f, 0.0f), 0.05f)));

		if (m_Panels.size() >= m_PanelPool.size())
			return;

		Panel made;
		made.Plan = plan;
		made.Body = m_PanelPool[m_Panels.size()];

		m_Panels.push_back(made);

		m_Built++;
	}

	// The panel's frame: where it stands and which way up. `Upright` is a
	// quarter turn about the panel's own length, so the same rectangle is a
	// floor or a wall and the collider and the drawing share the transform.
	glm::mat4 PanelFrame(const Panel& panel) const
	{
		glm::mat4 frame = glm::rotate(
			glm::translate(glm::mat4(1.0f), panel.At),
			panel.Yaw, glm::vec3(0.0f, 1.0f, 0.0f));

		if (panel.Plan.Upright)
			frame = glm::rotate(frame, -glm::half_pi<float>(),
				glm::vec3(1.0f, 0.0f, 0.0f));

		return frame;
	}

	glm::quat PanelTurn(const Panel& panel) const
	{
		glm::quat yaw = glm::angleAxis(panel.Yaw, glm::vec3(0.0f, 1.0f, 0.0f));

		return panel.Plan.Upright
			? yaw * glm::angleAxis(-glm::half_pi<float>(),
				glm::vec3(1.0f, 0.0f, 0.0f))
			: yaw;
	}

	// **Placing one puts it on the ground in front of you, square to the view.**
	void PlacePanel()
	{
		int next = -1;

		for (size_t i = 0; i < m_Panels.size(); i++)
			if (!m_Panels[i].Placed)
			{
				next = (int)i;
				break;
			}

		if (next < 0)
			return;

		Panel& panel = m_Panels[(size_t)next];

		glm::vec3 forward = m_Camera.GetForward();

		forward.y = 0.0f;

		if (glm::length(forward) < 1e-4f)
			forward = glm::vec3(0.0f, 0.0f, 1.0f);

		forward = glm::normalize(forward);

		glm::vec3 at = m_Camera.GetPosition() + forward * 3.0f;

		// The panel's length runs across the view rather than away down it, so
		// a wall faces you and a floor lies where you were looking.
		panel.Yaw = std::atan2(forward.x, forward.z);

		glm::vec3 half = PanelHalf(panel.Plan);

		// After the quarter turn the breadth is what stands up, so which half
		// extent clears the ground depends on which way the panel is laid.
		float clear = panel.Plan.Upright ? half.z : half.y;

		float ground = m_World.GroundHeightBelow(
			glm::vec3(at.x, m_Camera.GetPosition().y, at.z), m_Walker,
			-1000.0f);

		panel.At = glm::vec3(at.x, ground + clear, at.z);
		panel.Placed = true;

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(panel.At, half,
			0.0f);

		body.Type = Egss::BodyType::Static;
		body.Orientation = PanelTurn(panel);
		body.Friction = 0.8f;

		body.UpdateInertiaWorld();

		m_World.GetBody(panel.Body) = body;

		m_Placed++;
	}

	void DrawPanels();
	void DrawPrefabEditor();

	// --- The portal and the toolshed ------------------------------------------
	//
	// **A door you can carry, and a room that is not where the door is.**
	//
	// The shed is a building on the map. Deploying the portal does not create
	// it -- it creates a *shortcut* to its doorway, which is what a door you
	// can carry has always looked like from the outside.
	//
	// **Crossing is a plane test, not a trigger volume.** A box you must be
	// inside for a frame can be stepped through at speed -- the player is at
	// six metres a second and a fixed step is a sixtieth of a second, so a
	// half-metre trigger is missed one time in five. Testing which *side* of
	// the doorway the player was on last step and is on now cannot be outrun,
	// because the two positions bracket the crossing however fast it happened.

	// **It used to be a pocket dimension, four hundred metres above the block,
	// and that was the wrong shape for it.**
	//
	// Above rather than below, because the ground is an SDF over the voxel
	// field and the field is only defined across the block: below it every
	// query reads as solid, so a room down there had the solver shoving the
	// player upward at ninety-five metres a step, doing exactly its job.
	// Above it the same query reads as air and a room is left alone.
	//
	// It worked, and it still made the only building in the demo a place that
	// did not exist until you deployed a door to it -- you could not see it,
	// walk to it, or put a panel down beside it. A workshop you cannot find is
	// not a workshop. So it stands on the terrain now, and **none of the
	// portal code changed**: the second camera and the plane test never cared
	// that the two doorways were in different worlds, only that they were two
	// doorways.
	static constexpr float s_ShedHalf = 4.0f;     // the room is 8 m square
	static constexpr float s_DoorHalf = 1.1f;     // half the doorway's width

	// **The two openings are the same size, and they have to be.** The panel
	// in the world frame is a window onto the shed's own doorway, so if one is
	// taller than the other the difference shows as a strip of the shed's
	// lintel hanging in mid-air above the frame. One constant for both.
	static constexpr float s_DoorTop = 2.45f;

	// The top of the floor, at the middle of the room. Chosen once by
	// `SiteShed` and then fixed: everything in the building is an offset from
	// it, so the whole workshop moves by writing one vector.
	glm::vec3 ShedCentre() const { return m_ShedAt; }

	// The doorway inside the shed, which is the way back out.
	glm::vec3 ShedDoor() const
	{
		return ShedCentre() + glm::vec3(0.0f, 0.0f, -s_ShedHalf);
	}

	// **Sited where the least earth has to move.**
	//
	// Once the ground is going to be levelled anyway, "flattest" stops being
	// the question. What a real siting minimises is cut and fill: how much
	// earth has to be shifted to make the pad. Balanced cut and fill puts the
	// pad at the **mean** ground height over the footprint, and the cost is
	// then the mean absolute deviation from it -- which is cubic metres of
	// earth per square metre of pad, a number with units rather than a score.
	//
	// The search is a ring band rather than the whole block: near enough to
	// find on foot from the spawn in the middle, far enough out to clear the
	// water pit that is there.
	static constexpr int s_ShedProbe = 9;       // samples across the footprint
	static constexpr float s_ShedNear = 14.0f;
	static constexpr float s_ShedFar = 30.0f;

	// How far the floor stands above the levelled ground: a threshold. Not
	// zero, because a slab whose top is exactly at ground level is coplanar
	// with the terrain across the whole room, and coplanar surfaces flicker.
	static constexpr float s_ShedFloor = 0.15f;
	static constexpr float s_ShedSlab = 0.45f;

	// The pad, and the earth moved to make it, at one spot. `level` comes back
	// as the height that balances cut against fill.
	float ShedCutFill(float x, float z, float& level) const
	{
		float sum = 0.0f;

		const float reach = s_ShedHalf + s_PadEdge;

		for (int a = 0; a < s_ShedProbe; a++)
		for (int b = 0; b < s_ShedProbe; b++)
		{
			float u = (float)a / (float)(s_ShedProbe - 1) * 2.0f - 1.0f;
			float v = (float)b / (float)(s_ShedProbe - 1) * 2.0f - 1.0f;

			sum += RawHeight(x + u * reach, z + v * reach);
		}

		level = sum / (float)(s_ShedProbe * s_ShedProbe);

		float moved = 0.0f;

		for (int a = 0; a < s_ShedProbe; a++)
		for (int b = 0; b < s_ShedProbe; b++)
		{
			float u = (float)a / (float)(s_ShedProbe - 1) * 2.0f - 1.0f;
			float v = (float)b / (float)(s_ShedProbe - 1) * 2.0f - 1.0f;

			moved += std::abs(
				RawHeight(x + u * reach, z + v * reach) - level);
		}

		return moved / (float)(s_ShedProbe * s_ShedProbe);
	}

	void SiteShed()
	{
		// Sited off the bare terrain, so the search cannot read a terrace it
		// is in the middle of choosing.
		m_Terraced = false;

		float best = 1.0e9f;
		float bestLevel = 0.0f;

		glm::vec3 at(0.0f, 0.0f, s_ShedNear);

		const int rings = 5, spokes = 32;

		for (int r = 0; r < rings; r++)
		for (int k = 0; k < spokes; k++)
		{
			float radius = glm::mix(s_ShedNear, s_ShedFar,
				(float)r / (float)(rings - 1));

			// Offset every ring by half a step so the spokes do not all lie
			// along the same lines and sample the same noise ridges.
			float angle = ((float)k + 0.5f * (float)r)
				* glm::two_pi<float>() / (float)spokes;

			float x = std::cos(angle) * radius;
			float z = std::sin(angle) * radius;

			float level = 0.0f;
			float moved = ShedCutFill(x, z, level);

			// A workshop in the lake is not a workshop.
			if (HasWater() && level < WaterLevel() + 1.0f)
				continue;

			if (moved >= best)
				continue;

			best = moved;
			bestLevel = level;
			at = glm::vec3(x, 0.0f, z);
		}

		m_ShedAt = glm::vec3(at.x, bestLevel + s_ShedFloor, at.z);
		m_ShedMoved = best;

		m_Terraced = true;
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

		// The floor slab: its top is the floor, and it is thick enough that
		// its underside is buried in the pad rather than showing daylight.
		wall(centre - glm::vec3(0.0f, 0.5f * s_ShedSlab, 0.0f),
			{ h, 0.5f * s_ShedSlab, h });

		// **The roof sits on the walls and overhangs them.** It used to span
		// the same 3.0 to 3.5 the wall tops run through, so its faces and
		// theirs were interpenetrating and the eaves came out as a row of
		// z-fighting stripes -- invisible from inside a dark room, and the
		// first thing you see walking up to a building.
		wall(centre + glm::vec3(0.0f, 3.425f, 0.0f),
			{ h + 0.35f, 0.175f, h + 0.35f });
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

		if (!m_ViaPortal)
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

				m_ViaPortal = true;
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

			m_ViaPortal = false;
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
			// **The load is applied here rather than to `m_WalkSpeed`.** That
			// is a registered parameter, so writing to it would put the weight
			// of whatever you happened to be holding into every recording.
			glm::vec3 move = glm::normalize(wish) * m_WalkSpeed * LoadFactor();

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

		// **F rather than E.** E is the portal, and a key that does one thing
		// beside a doorway and another beside a rack is a key nobody trusts.
		bool axe = Egss::Input::IsKeyPressed(EGSS_KEY_F);

		if (axe && !m_WasAxe)
			ToggleAxe();

		m_WasAxe = axe;

		bool weight = Egss::Input::IsKeyPressed(EGSS_KEY_G);

		if (weight && !m_WasWeight)
			PlaceWeight();

		m_WasWeight = weight;

		bool carry = Egss::Input::IsKeyPressed(EGSS_KEY_R);

		if (carry && !m_WasCarry)
			ToggleCarry();

		m_WasCarry = carry;

		bool mill = Egss::Input::IsKeyPressed(EGSS_KEY_T);

		if (mill && !m_WasMill)
			MillLog();

		m_WasMill = mill;

		bool craft = Egss::Input::IsKeyPressed(EGSS_KEY_C);

		if (craft && !m_WasCraft)
			CraftPanel();

		m_WasCraft = craft;

		bool place = Egss::Input::IsKeyPressed(EGSS_KEY_B);

		if (place && !m_WasPlace)
			PlacePanel();

		m_WasPlace = place;

		CarryTimber();

		AimAxe();

		// The stroke is a wind-up and a fall, so it wants to be visible for
		// about a third of a second whether or not it connected.
		m_Swing = glm::max(m_Swing - (float)step * 3.2f, 0.0f);

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

		// An axe in your hands is not a shovel. Swinging where the dig would
		// have gone means there is one button for "use what you are holding",
		// which is also why picking the axe up has to be deliberate.
		if (dig && !m_WasDigging)
		{
			if (m_HasAxe)
				Swing();
			else
				Dig(false);
		}
		else if (add && !m_WasAdding && !m_HasAxe)
		{
			Dig(true);
		}

		m_WasDigging = dig;
		m_WasAdding = add;


		MoveWalker(step);

		// **Four substeps, because a 2x4 is 38 mm thick.**
		//
		// A discrete solver moves a body `v dt` between collision tests, and a
		// concrete block settling onto a plank at a metre and a half a second
		// covers 25 mm in a sixtieth -- comparable with the plank it is
		// supposed to land on. So the weights went *through* the planks: four
		// of them sat correctly for a moment, the plank dipped, and by two
		// seconds later they were three metres below it and still falling.
		//
		// There is no continuous collision here and no speculative contact, so
		// the honest fix is a shorter step. A quarter of a sixtieth puts the
		// same block at 6 mm a step against 38 mm of timber, which is the
		// margin the test was missing. Buoyancy is applied inside the loop
		// because it is a force and forces belong to the step they act over.
		const int substeps = 4;

		float slice = (float)step / (float)substeps;

		for (int i = 0; i < substeps; i++)
		{
			ApplyBuoyancy(slice);
			StepFelled(slice);

			m_World.Step(slice);

			HoldFalling(slice);
		}

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
	void DrawHeldAxe(const Egss::PerspectiveCamera& camera);
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

	// **A pool of shapes, not one shape.** Every rock being the same jittered
	// sphere is visible the moment two of them are near each other, and the
	// fix costs six meshes of four hundred triangles.
	static constexpr int s_StoneMeshes = 6;

	std::map<size_t, std::vector<Stone>> m_Stones;
	std::vector<Egss::PhysicsWorld3D::BodyHandle> m_StoneBodies;
	std::shared_ptr<Egss::Mesh> m_Boulders[s_StoneMeshes];

	bool m_ShowStones = true;
	float m_StoneDensity = 1.2f;     // per square metre of open scree
	float m_StoneSize = 1.0f;
	int m_StoneCount = 0;
	int m_StoneDrawn = 0;
	float m_StoneReach = 110.0f;

	int CountStones() const
	{
		int total = 0;

		for (const auto& entry : m_Stones)
			total += (int)entry.second.size();

		return total;
	}

	std::shared_ptr<Egss::Mesh> m_Boulder;
	std::shared_ptr<Egss::Mesh> m_Log;
	std::shared_ptr<Egss::Mesh> m_Cube;
	int m_LooseCount = 12;
	int m_Stacked = 0;
	// **Which loose entries are in your hands.** A list rather than one index:
	// an armful is the unit now, and every place that used to ask "is this the
	// carried one" asks the list instead.
	std::vector<int> m_Carry;
	int m_Bucked = 0;
	int m_Riven = 0;
	int m_Milled = 0;

	// Seconds left on "that is too heavy", so the refusal says something.
	float m_TooHeavy = 0.0f;

	// Twenty-four is longer than any stem the tallest habit grows: a bound on
	// the loop, not a rule about trees.
	static constexpr int s_MaxLogs = 24;

	// The saved designs. Two to start with, because a floor and a wall are the
	// two things a panel can be and having both there is what makes the third
	// one obviously editable.
	std::vector<Design> m_Designs;

	// What the editor is editing, and what `C` builds.
	Design m_Draft;

	// The editor's own state: which course, which way round, and how long a
	// piece to cut. None of it reaches the simulation -- the pieces already on
	// the grid do, and those are registered.
	bool m_EditLedger = false;
	bool m_EditAlongZ = false;
	int m_EditLength = s_BoardCells;

	int m_FillAcross = 24;
	int m_FillCourses = 1;

	std::vector<Panel> m_Panels;
	std::vector<Egss::PhysicsWorld3D::BodyHandle> m_PanelPool;

	int m_Built = 0;
	int m_Placed = 0;

	bool m_WasCraft = false;
	bool m_WasPlace = false;

	static constexpr int s_TreeShapes = 6;

	// **Three size classes per habit, because a big tree is not a small tree
	// made bigger.** See `BuildTrees` for the allometry; the short version is
	// that a trunk has to thicken faster than the tree grows tall or the tree
	// buckles under its own weight, so the same habit at three sizes needs
	// three meshes and cannot be one mesh scaled.
	static constexpr int s_TreeSizes = 3;

	// **Grown, not thickened.** The first answer to "the trees are too thin"
	// was to fatten the trunks, and the measurement said not to: the biggest
	// standard here already stood at H/D 29 with an 0.80 m butt, which is a
	// *stout* tree -- a forest conifer runs H/D 40 to 60. Nothing was slender.
	// What was wrong was the stand. Nearly half of it was the smallest class,
	// and the smallest class of a scrub is a 2 cm stick.
	//
	// So the classes moved up and the mix moved with them. Radius follows
	// height as H^(3/2), so a scale of 1.4 on the smallest class is 1.66 on
	// its trunk -- which is why growing the tree is a better lever on
	// thickness than thickening it, and keeps the allometry honest.
	static constexpr float s_TreeScale[s_TreeSizes] = { 0.7f, 1.2f, 1.9f };

	// Small trees outnumber large ones in any stand that has been left alone:
	// a great many seedlings, fewer poles, a handful of standards. Not the
	// -3/2 self-thinning law, which needs a stand history this does not have,
	// but the same shape -- flattened, because a wood that is half saplings
	// has nothing in it worth felling.
	static constexpr float s_TreeShare[s_TreeSizes] = { 0.34f, 0.40f, 0.26f };

	// The trunk's radius at the base and the finished height, per habit and
	// size, measured off the mesh as it is built. The axe needs both: the
	// radius says how many strokes a trunk is worth and how wide the notch is,
	// the height says how big a body the crown becomes.
	float m_TreeTrunk[s_TreeShapes][s_TreeSizes] = {};
	float m_TreeTop[s_TreeShapes][s_TreeSizes] = {};
	float m_TreeSpan[s_TreeShapes][s_TreeSizes] = {};

	// --- The axe ------------------------------------------------------------
	//
	// **Not a felling animation.** A stroke lands where you were aiming, takes
	// a wedge out of the trunk at that exact height, and the tree comes down
	// when the wedge is through -- so a tree cut high leaves a tall stump and
	// a tree cut low leaves a low one, because the cut is a place and not an
	// event. The same idea as breaking a rock in the open-world demo: damage
	// where it landed rather than a state machine.
	static constexpr float s_AxeReach = 3.6f;

	// Metres of trunk a stroke takes. A stroke is a stroke whatever it hits,
	// so a thick trunk simply takes more of them -- which is the right way
	// round and needs no hit points.
	static constexpr float s_AxeBite = 0.055f;

	// How tall the notch is, as a share of the trunk's radius.
	static constexpr float s_AxeKerf = 1.1f;

	static constexpr int s_FelledMax = 16;

	bool m_HasAxe = false;
	float m_Swing = 0.0f;          // 1 at the top of the stroke, 0 at rest
	int m_Chopped = 0;
	int m_Felled = 0;

	// What the axe is lined up on this frame, and where a stroke would land.
	size_t m_AimKey = 0;
	int m_AimTree = -1;
	float m_AimY = 0.0f;
	glm::vec2 m_AimSide = glm::vec2(1.0f, 0.0f);

	std::vector<Felled> m_Fell;

	std::shared_ptr<Egss::Mesh> m_TreeBark[s_TreeShapes][s_TreeSizes];
	std::shared_ptr<Egss::Mesh> m_TreeLeaves[s_TreeShapes][s_TreeSizes];

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
	bool m_WasAxe = false;
	bool m_WasWeight = false;
	bool m_WasCarry = false;
	bool m_WasMill = false;

	// Where the workshop ended up, whether its terrace is in `Height` yet, and
	// the earth its pad cost -- the last only so the panel can say.
	glm::vec3 m_ShedAt = glm::vec3(0.0f, 0.0f, s_ShedNear);
	bool m_Terraced = false;
	float m_ShedMoved = 0.0f;

	bool m_PortalOn = false;

	// **Not "am I in the shed" -- "did I get here through the door I carry".**
	// The shed is a place on the map, so being inside it is a question about
	// position and the portal has no business answering it. What the link
	// needs to know is whether stepping out of the shed's doorway should put
	// you back at the frame or just outside the shed, and that is this.
	bool m_ViaPortal = false;
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

			// The same ground bounce the trees get: `0.5 + 0.5 * n.y` alone
			// makes anything facing down pure black, which is invisible on a
			// heightfield and very visible the moment you dig a roof over
			// yourself.
			float dome = mix(0.22, 1.0, 0.5 + 0.5 * normal.y);

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
	TreeShapes(shapes);

	// **Six habits, not six seeds.** Three seeds of the same parameters give
	// three trees that are recognisably the same tree; what makes a wood look
	// like a wood is that its trees have different *architecture*. The
	// parameters worth moving are the branching count, how far a child leans
	// off its parent, and how fast length falls off with depth -- those three
	// between them decide whether a tree is a spire, a vase or a mop.
	//
	// They are laid out here as a set to choose from rather than tuned to one
	// answer, because which of them reads best is a question for looking at
	// them, and the panel can turn each on and off for exactly that. The table
	// itself is `TreeShapes`, so anything that wants to check the trees reads
	// the same one the meshes are built from.

	// **A big tree is not a small tree made bigger, and the reason is
	// buckling.**
	//
	// A column of height H and radius r standing under its own weight buckles
	// at Greenhill's limit,
	//
	//     H_crit = (7.8373 E I / (rho g A))^(1/3) = (7.8373 E r^2 / (4 rho g))^(1/3)
	//
	// so the safe height goes as r^(2/3), and a trunk that keeps up with a
	// tree getting taller has to thicken as **H^(3/2)**. That is elastic
	// similarity, and it is why a sapling is a wand and an oak is a barrel:
	// double the height and the trunk is nearly three times as thick. Scaling
	// one mesh uniformly says the opposite -- that a forty-metre tree is a
	// four-metre tree seen closer -- and it is the reason the big ones here
	// looked like models.
	//
	// The constant comes out of the same formula. McMahon measured that trees
	// stand at about a quarter of their buckling height, so with green wood at
	// E = 10 GPa and rho = 800 kg/m^3,
	//
	//     r = H^(3/2) sqrt(256 rho g / (7.8373 E)) = 0.00506 H^(3/2)
	//
	// which puts a 6 m tree on a 15 cm trunk. The old parameters gave that
	// same tree a **52 cm** trunk, seven times the section it needs, which is
	// most of why they read as stubby.
	//
	// It is set from `Length` rather than from the finished height on purpose:
	// how tall a habit ends up for a given first segment depends on its whole
	// branching architecture, so leaving that out means the check afterwards
	// is measuring something the code did not assume.
	const float slenderness = 0.00506f;

	for (int i = 0; i < s_TreeShapes; i++)
	for (int j = 0; j < s_TreeSizes; j++)
	{
		Veg::TreeParams params = shapes[i];

		float scale = s_TreeScale[j];

		params.Length *= scale;
		params.LeafRadius *= scale;

		// The first segment is about a third of the finished height across
		// these habits, so the height this is aiming at is a few times the
		// length. The factor is folded into `slenderness` having been fitted
		// once; what matters is the exponent.
		params.Radius = slenderness
			* std::pow(params.Length * 3.0f, 1.5f);

		// A larger tree carries more orders of branching, which is not a
		// scaling law but is true of every tree anyone has counted.
		if (j == 2 && params.Depth < 5)
			params.Depth += 1;
		else if (j == 0 && params.Depth > 3)
			params.Depth -= 1;

		Egss::MeshData bark, leaves;

		Veg::MakeTreeMesh(1471u + (unsigned int)(i * s_TreeSizes + j) * 97u,
			params, bark, leaves);

		bark.RecalculateBounds();
		leaves.RecalculateBounds();

		// Measured off the finished mesh rather than taken from the
		// parameters, because what the axe has to cut is the trunk that is
		// actually there.
		m_TreeTrunk[i][j] = params.Radius;
		m_TreeTop[i][j] = glm::max(bark.BoundsMax.y, leaves.BoundsMax.y);
		m_TreeSpan[i][j] = glm::max(
			glm::max(leaves.BoundsMax.x, -leaves.BoundsMin.x),
			glm::max(leaves.BoundsMax.z, -leaves.BoundsMin.z));

		m_TreeBark[i][j] = std::make_shared<Egss::Mesh>(bark, "LabBark");
		m_TreeLeaves[i][j] = std::make_shared<Egss::Mesh>(leaves, "LabLeaves");
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

		// **The tree's own coordinates, untouched by the wind.**
		//
		// The cut is a fact about the tree, not about where the wind has bent
		// it to this frame -- a notch that swam about the trunk as it swayed
		// would be worse than no notch. So this is `a_Position` straight
		// through: y is height up the tree from its root and xz is across it,
		// both in metres before the instance scale.
		out vec3 v_Object;

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

			v_Object = a_Position;
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

		// **Nothing in the world is lit only from above.**
		//
		// `0.5 + 0.5 * n.y` is the share of the sky a surface can see, and it
		// is zero for anything facing down -- so the underside of a canopy, of
		// a lintel, of a boulder, came out pure black. That was invisible
		// while every tree was small enough to look down on, and the moment
		// the big size class arrived you could stand under one and the whole
		// screen went black.
		//
		// The missing term is the ground. A surface facing down sees the
		// ground, and the ground is not black -- it returns whatever its
		// albedo is, about a fifth for grass and soil. So the ambient is a
		// bright hemisphere above and a dimmer one below rather than a
		// hemisphere and a void.
		uniform float u_Bounce;

		// And a leaf is thin. It **transmits** -- a canopy lit from above
		// glows from underneath, which is why a wood in summer is green
		// twilight rather than a dark room. One extra term, the diffuse
		// computed against the *back* of the surface, scaled by how much gets
		// through. Zero for a boulder, which does not.
		uniform float u_Through;

		in vec3 v_Object;

		// **Cutting a tree is removing material, not swapping a model.**
		//
		// An axe takes a wedge out of one side of the trunk at the height it
		// landed, and the tree comes down when the wedge has gone through. So
		// the cut is described where it happens -- a height on the tree's own
		// axis, a direction it is cut from, and how far through it is -- and
		// the fragments inside that wedge are discarded. Nothing is remeshed
		// and nothing is pre-authored, so the notch is where the swing landed
		// rather than where somebody put it.
		//
		// `u_CutPart` says which half of a severed tree this draw is: -1 the
		// stump, +1 the crown on its way down, 0 a tree still standing.
		uniform float u_CutY;
		uniform float u_CutDepth;
		// A vec3 because `Material` has no vec2 setter; z is unused.
		uniform vec3 u_CutSide;
		uniform float u_CutRadius;
		uniform float u_CutKerf;
		uniform float u_CutPart;

		// Where the next swing would land, drawn as a line round the trunk so
		// the player is aiming at something rather than guessing. Set a long
		// way off the tree when this is not the tree being aimed at.
		uniform float u_AimY;

		void main()
		{
			if (u_CutPart < -0.5 && v_Object.y > u_CutY)
				discard;

			if (u_CutPart > 0.5 && v_Object.y < u_CutY)
				discard;

			// **Both the cut and the line are about the trunk, and only the
			// trunk.**
			//
			// Left out, the aim line ran along the *foliage* as well: this
			// habit's leaf clusters hang to a metre off the ground and six
			// metres out, so a line at chest height painted a bright band
			// across the canopy thirty metres away. It looked as though the
			// line was leaking onto other trees. It was not -- it was on the
			// right tree, on the parts of it nobody thinks of as being at
			// chest height. The notch had the same reach and was quietly
			// taking a disc out of the leaves at the same time.
			float axis = length(v_Object.xz);

			bool trunk = axis < u_CutRadius * 3.0;

			if (u_CutDepth > 0.0 && trunk)
			{
				// **A wedge, not a slot.** An axe cut is widest at the face
				// and closes to a point, because each stroke lands at an angle
				// and the chips come out of a V. Letting the depth fall off
				// with distance from the centre line costs one multiply and is
				// the difference between a cut tree and a tree with a slice
				// missing.
				float across = abs(v_Object.y - u_CutY) / max(u_CutKerf, 1e-4);

				float into = dot(v_Object.xz, u_CutSide.xy);

				// Full depth reaches `-radius`, which is right through.
				float reach = u_CutRadius * (1.0 - 2.0 * u_CutDepth)
					+ across * u_CutRadius;

				if (across < 1.0 && into > reach)
					discard;
			}

			vec3 normal = normalize(v_Normal);

			// **Two-sided, because a cut trunk is an open tube.**
			//
			// Take a wedge out of one side and the far wall's *inside* is what
			// faces you -- so with culling on there is nothing there and you
			// see straight through the tree, and without the flip the wall
			// that is there is lit as though it faced the other way. One line
			// fixes both. It is right for the leaves as well: a leaf is a
			// sheet, and the underside of one is not unlit.
			if (!gl_FrontFacing)
				normal = -normal;

			float front = max(dot(normal, -u_SunDirection), 0.0);
			float back = max(dot(-normal, -u_SunDirection), 0.0);

			float diffuse = front + u_Through * back;

			float dome = mix(u_Bounce, 1.0, 0.5 + 0.5 * normal.y);

			// The inside of a trunk is not bark. Sapwood is pale, and seeing
			// it is the whole point of having cut into the tree.
			vec3 tone = (!gl_FrontFacing && trunk)
				? vec3(0.74, 0.62, 0.44) : u_Color;

			// **Light off the ground, which is what stops a shaded wall from
			// being black.**
			//
			// The dome above only lights what faces up. Outdoors the other
			// half of the sphere is ground, and ground returns a good part of
			// what the sun puts on it -- which is why the north face of a
			// building on a bright day is dim and not dark. Leaving it out
			// did not show while the only things using this shader were trees
			// and a room nobody could see from outside; the moment the shed
			// stood in a field its door wall came out at **4%** of the sunlit
			// sand a metre in front of it, which is a night-time number in
			// full daylight.
			//
			// One constant albedo rather than the terrain's own colour under
			// each fragment: this is the term that stops a wall being black,
			// not a radiosity solve, and a wall does not see the ground it
			// stands on so much as the whole field around it.
			const vec3 albedo = vec3(0.42, 0.38, 0.30);

			vec3 bounce = albedo * u_SunColor
				* max(-u_SunDirection.y, 0.0) * (0.5 - 0.5 * normal.y);

			vec3 lit = tone * (u_SkyColor * dome * u_Ambient
				+ u_SunColor * diffuse + bounce);

			// The aim line, added rather than mixed so it reads on bark and on
			// leaves alike and cannot be mistaken for either.
			if (trunk)
				lit += vec3(0.9, 0.5, 0.15)
					* (1.0 - smoothstep(0.0, 0.035,
						abs(v_Object.y - u_AimY)));

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

	for (int i = 0; i < s_StoneMeshes; i++)
		m_Boulders[i] = std::make_shared<Egss::Mesh>(
			Boulder::Build(4177u + (unsigned int)i * 7919u), "LabStone");

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

	// **A log, as a cylinder lying along x.** Unit radius and unit half-length,
	// so the same scale that sizes its collider sizes it. Sixteen sides is
	// enough that a 34 cm log does not read as a hexagon and few enough that a
	// stack of them costs nothing.
	{
		Egss::MeshData log;

		const int sides = 16;

		auto ring = [&](float x, float nx)
		{
			for (int i = 0; i < sides; i++)
			{
				float a0 = (float)i / (float)sides * 6.2831853f;

				glm::vec3 at(x, std::cos(a0), std::sin(a0));

				log.Vertices.push_back({ at, glm::vec3(nx, 0.0f, 0.0f),
					{ (float)i / (float)sides, x * 0.5f + 0.5f } });
			}
		};

		// Two rings for the sides, with radial normals, and two more for the
		// end caps with axial ones -- a shared vertex cannot have both, and an
		// end cap lit as though it were the barrel is the tell.
		unsigned int side = (unsigned int)log.Vertices.size();

		for (int end = 0; end < 2; end++)
		for (int i = 0; i < sides; i++)
		{
			float a0 = (float)i / (float)sides * 6.2831853f;

			glm::vec3 at(end ? 1.0f : -1.0f, std::cos(a0), std::sin(a0));

			log.Vertices.push_back({ at,
				glm::vec3(0.0f, std::cos(a0), std::sin(a0)),
				{ (float)i / (float)sides, (float)end } });
		}

		for (int i = 0; i < sides; i++)
		{
			unsigned int a0 = side + (unsigned int)i;
			unsigned int a1 = side + (unsigned int)((i + 1) % sides);

			log.Indices.insert(log.Indices.end(), {
				a0, a1 + (unsigned int)sides, a0 + (unsigned int)sides,
				a0, a1, a1 + (unsigned int)sides });
		}

		for (int end = 0; end < 2; end++)
		{
			unsigned int centre = (unsigned int)log.Vertices.size();

			log.Vertices.push_back({ glm::vec3(end ? 1.0f : -1.0f, 0.0f, 0.0f),
				glm::vec3(end ? 1.0f : -1.0f, 0.0f, 0.0f), { 0.5f, 0.5f } });

			unsigned int rim = (unsigned int)log.Vertices.size();

			for (int i = 0; i < sides; i++)
			{
				float a0 = (float)i / (float)sides * 6.2831853f;

				log.Vertices.push_back({
					glm::vec3(end ? 1.0f : -1.0f, std::cos(a0), std::sin(a0)),
					glm::vec3(end ? 1.0f : -1.0f, 0.0f, 0.0f),
					{ 0.5f + 0.5f * std::cos(a0), 0.5f + 0.5f * std::sin(a0) } });
			}

			for (int i = 0; i < sides; i++)
			{
				unsigned int a0 = rim + (unsigned int)i;
				unsigned int a1 = rim + (unsigned int)((i + 1) % sides);

				if (end)
					log.Indices.insert(log.Indices.end(), { centre, a0, a1 });
				else
					log.Indices.insert(log.Indices.end(), { centre, a1, a0 });
			}
		}

		(void)ring;

		Egss::Submesh all;
		all.IndexCount = (unsigned int)log.Indices.size();

		log.Submeshes.push_back(all);
		log.RecalculateBounds();

		m_Log = std::make_shared<Egss::Mesh>(log, "LabLog");
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
	if (m_SeeThrough && (m_PortalOn || m_ViaPortal))
		DrawPortalView();

	DrawScene(m_Camera, Pass::Main);
}

// **Everything in the world, from whichever camera is asked for.**
//
// Split out of `OnDemoUpdate` so the portal can render the scene a second
// time from somewhere else. The pass says which side of the doorway is being
// drawn, and that is the only thing that differs between the three:
//
//   Main     -- what the eye sees.
//   ToShed   -- the same world from the shed's doorway, which is what is on
//               the far side of a portal planted in a field.
//   ToWorld  -- the same world from the planted frame, which is what is on
//               the far side of the shed's own door while the link is live.
//
// The three used to differ in *what* was drawn, because the shed was four
// hundred metres up and the terrain was not there. It is on the map now, so
// they differ only in where the camera stands -- which is what a portal
// between two doorways in one world ought to cost.
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

		// The albedo of grass and soil, near enough. Everything drawn through
		// this shader gets it; only leaves get the transmission below.
		m_TreeMaterial->Set("u_Bounce", 0.22f);
		m_TreeMaterial->Set("u_Through", 0.0f);
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
		for (size_t index = 0; index < entry.second.size(); index++)
		{
			const Tree& tree = entry.second[index];

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
			// The cut, in the tree's own coordinates. A tree nobody has
			// touched sets a depth of zero and the shader skips the whole
			// test; a severed one draws as its stump.
			bool aimed = pass == Pass::Main && m_AimTree >= 0
				&& entry.first == m_AimKey
				&& (size_t)m_AimTree == index;

			m_TreeMaterial->Set("u_CutY", tree.CutY);
			m_TreeMaterial->Set("u_CutDepth", tree.CutDepth);
			m_TreeMaterial->Set("u_CutSide",
				glm::vec3(tree.CutSide.x, tree.CutSide.y, 0.0f));
			m_TreeMaterial->Set("u_CutRadius",
				m_TreeTrunk[tree.Shape][tree.Size]);
			m_TreeMaterial->Set("u_CutKerf",
				m_TreeTrunk[tree.Shape][tree.Size] * s_AxeKerf);
			m_TreeMaterial->Set("u_CutPart", tree.Severed ? -1.0f : 0.0f);
			m_TreeMaterial->Set("u_AimY", aimed ? m_AimY : 1.0e9f);

			// A cut tree has an open face, so it needs its far wall drawn.
			// Only the cut ones, so this is a state change per felled tree
			// rather than per tree.
			bool open = tree.CutDepth > 0.0f || tree.Severed;

			if (open)
				Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

			m_TreeMaterial->Set("u_Color", tree.Bark);
			m_TreeMaterial->Set("u_Compliance", 1.1e-5f);
			m_TreeMaterial->Set("u_Through", 0.0f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeBark[tree.Shape][tree.Size], transform);

			m_TreeMaterial->Set("u_Color", tree.Leaf);
			m_TreeMaterial->Set("u_Compliance", 20.0f * 1.1e-5f);

			// A leaf passes a good deal of what falls on it; a trunk passes
			// none. Measured leaf transmittance is nearer a tenth, but a leaf
			// cluster here stands in for a few hundred leaves and light that
			// has been through two of them is still light.
			m_TreeMaterial->Set("u_Through", 0.30f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeLeaves[tree.Shape][tree.Size], transform);

			if (open)
				Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);

			drawn++;
		}

		m_TreeCount = drawn;
	}

	// **The tops that have come off**, drawn from the same mesh as the stump
	// they left, with the other half of the cut discarded. Nothing was built
	// when the tree came down and nothing is thrown away when it lands.
	if (m_ShowTrees && !m_Fell.empty())
	{
		m_TreeMaterial->Set("u_MaxLean", m_TreeMaxLean);
		m_TreeMaterial->Set("u_AimY", 1.0e9f);
		m_TreeMaterial->Set("u_CutDepth", 0.0f);
		m_TreeMaterial->Set("u_CutPart", 1.0f);

		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		for (const Felled& fell : m_Fell)
		{
			if (!fell.Active)
				continue;

			glm::mat4 transform = FelledFrame(fell);

			m_TreeMaterial->Set("u_CutY", fell.CutY);

			m_TreeMaterial->Set("u_Color", fell.Bark);
			m_TreeMaterial->Set("u_Compliance", 0.0f);
			m_TreeMaterial->Set("u_Through", 0.0f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeBark[fell.Shape][fell.Size], transform);

			m_TreeMaterial->Set("u_Color", fell.Leaf);
			m_TreeMaterial->Set("u_Through", 0.30f);

			Egss::Renderer::Submit(m_TreeMaterial,
				m_TreeLeaves[fell.Shape][fell.Size], transform);
		}

		Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
	}

	// Everything from here draws no cut.
	m_TreeMaterial->Set("u_CutDepth", 0.0f);
	m_TreeMaterial->Set("u_CutPart", 0.0f);
	m_TreeMaterial->Set("u_AimY", 1.0e9f);

	// **The bedded boulders**, which are terrain and not props: placed once
	// where the ground would actually hold them, and never simulated.
	if (m_ShowStones && !m_Stones.empty())
	{
		m_TreeMaterial->Set("u_Compliance", 0.0f);
		m_TreeMaterial->Set("u_MaxLean", 0.0f);

		// The trees left this at a leaf's value and a rock does not transmit.
		m_TreeMaterial->Set("u_Through", 0.0f);

		glm::vec3 eye = camera.GetPosition();

		int drawn = 0;

		for (const auto& entry : m_Stones)
		for (const Stone& stone : entry.second)
		{
			// A cobble at a hundred metres is a pixel; the same distance cull
			// the trees use, and for the same reason.
			if (glm::length(stone.At - eye) > m_StoneReach)
				continue;

			m_TreeMaterial->Set("u_Color", stone.Colour);

			glm::mat4 transform =
				glm::translate(glm::mat4(1.0f), stone.At)
				* glm::mat4(stone.Lie)
				* glm::scale(glm::mat4(1.0f), stone.Radii);

			Egss::Renderer::Submit(m_TreeMaterial, m_Boulders[stone.Mesh],
				transform);

			drawn++;
		}

		if (pass == Pass::Main)
			m_StoneDrawn = drawn;
	}

	// Driftwood and the odd cobble in the lake, which are the buoyancy test
	// and are the only rocks here that move.
	if (m_Boulder && !m_Loose.empty())
	{
		m_TreeMaterial->Set("u_Compliance", 0.0f);
		m_TreeMaterial->Set("u_MaxLean", 0.0f);
		m_TreeMaterial->Set("u_Through", 0.0f);

		// Same reason as the panels: a carried board would otherwise wear the
		// last tree's aim line and take its notch.
		m_TreeMaterial->Set("u_CutRadius", 0.0f);

		for (const Loose& loose : m_Loose)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(loose.Body);

			m_TreeMaterial->Set("u_Color", loose.Colour);

			// A rock for the rock, a cylinder for a log, a box for the rest.
			// All of them are box colliders: the boulder mesh fits inside the
			// unit box exactly, and the log fills its box in two axes and is
			// round in the third, which is what `Fill` accounts for.
			const std::shared_ptr<Egss::Mesh>& mesh =
				loose.Kind == Flotsam::Cobble ? m_Boulder
				: loose.Kind == Flotsam::Log ? m_Log : m_Cube;

			glm::mat4 transform =
				glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				* glm::scale(glm::mat4(1.0f), loose.Half);

			Egss::Renderer::Submit(m_TreeMaterial, mesh, transform);
		}
	}

	DrawPanels();

	// The workshop, which is a building on the map like anything else.
	DrawShed();

	// **The picture in the doorway.**
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

	if (m_PortalOn)
		DrawDoorFrame();

	if (pass == Pass::Main)
	{
		bool through = m_SeeThrough && m_PortalTexture
			&& (m_ViaPortal || m_PortalOn);

		glm::vec3 at = m_ViaPortal ? ShedDoor() : m_PortalAt;
		float yaw = m_ViaPortal ? 0.0f : m_PortalYaw;

		if (m_ViaPortal || m_PortalOn)
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
			else if (!m_ViaPortal)
			{
				DrawBox(at + glm::vec3(0.0f, 0.5f * s_DoorTop, 0.0f),
					{ s_DoorHalf, 0.5f * s_DoorTop, 0.02f }, yaw,
					glm::vec3(0.03f, 0.03f, 0.05f));
			}
		}
	}

	// The tool is in front of your face, so it is drawn before the water and
	// after everything solid -- and only in the main pass, because the second
	// camera is not the one holding it.
	if (pass == Pass::Main)
		DrawHeldAxe(camera);

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
// **Panels are drawn board by board, not as one box.**
//
// The collider is a single box, because that is what standing on a floor and
// walking into a wall want. The picture is the individual boards and ledgers,
// because the point of the whole pipeline is that this thing came out of the
// trees -- a smooth slab would say nothing about where it came from, and the
// board count would be invisible in exactly the object whose cost is a board
// count.
inline void TerrainLab::DrawPanels()
{
	if (!m_Cube || m_Panels.empty())
		return;

	m_TreeMaterial->Set("u_Compliance", 0.0f);
	m_TreeMaterial->Set("u_MaxLean", 0.0f);
	m_TreeMaterial->Set("u_Through", 0.0f);

	// **A panel is not a trunk.** The tree shader gates the axe wedge, the aim
	// line and the pale sapwood on `axis < u_CutRadius * 3.0`, and that uniform
	// is left set by whichever tree was drawn last. A zero makes the test fail
	// for every fragment, which switches off all three at once.
	m_TreeMaterial->Set("u_CutRadius", 0.0f);

	float thick = BoardThick();

	for (const Panel& panel : m_Panels)
	{
		if (!panel.Placed)
			continue;

		glm::ivec2 low, high;
		DesignBounds(panel.Plan, low, high);

		glm::vec2 span = PanelSpan(panel.Plan);
		glm::mat4 frame = PanelFrame(panel);

		int index = 0;

		for (const Part& part : PartsOf(panel.Plan))
		{
			// Cells to metres, with the design centred on its own bounding box
			// so a panel is put down where you are looking rather than offset
			// by wherever on the grid it happened to be drawn.
			glm::vec2 from = glm::vec2(glm::ivec2(part.X, part.Z) - low)
				* Cell() - 0.5f * span;

			glm::vec2 size = part.AlongZ
				? glm::vec2(Cell(), (float)part.Length * Cell())
				: glm::vec2((float)part.Length * Cell(), Cell());

			glm::vec3 at(from.x + 0.5f * size.x,
				part.Face ? 0.5f * thick : -0.5f * thick,
				from.y + 0.5f * size.y);

			// **Every piece a slightly different brown.** In one colour the
			// assembly is a single pale slab: the pieces are butted, there is
			// no gap to cast a shadow, and nothing tells one from the next --
			// so the object whose entire cost is a board count showed no
			// boards. Sawn timber does vary piece to piece, which makes the
			// variation the honest fix rather than a fake gap.
			float tone = 0.86f + 0.28f * Veg::Hash2DUnit(part.X,
				part.Z * 2 + (part.Face ? 1 : 0), 0x9E3779B9u);

			m_TreeMaterial->Set("u_Color", (part.Face
				? glm::vec3(0.66f, 0.51f, 0.32f)
				: glm::vec3(0.52f, 0.39f, 0.24f)) * tone);

			Egss::Renderer::Submit(m_TreeMaterial, m_Cube,
				frame * glm::translate(glm::mat4(1.0f), at)
				* glm::scale(glm::mat4(1.0f),
					glm::vec3(0.5f * size.x, 0.5f * thick, 0.5f * size.y)));

			index++;
		}
	}
}

// **The prefab editor: a plan view of the bench.**
//
// The grid is one board wide a cell, so a piece is a whole number of cells and
// the arithmetic never leaves integers. Left button puts a piece down, right
// button takes one up, and the two courses -- the face and the ledgers under
// it -- are edited one at a time, because there is no way to click on
// something that is underneath something else.
//
// The bill is the part worth having on screen. It is the only place the whole
// pipeline shows as one figure: twenty-six boards is two and a half logs is
// most of a tree.
inline void TerrainLab::DrawPrefabEditor()
{
	float reach = glm::length(m_Camera.GetPosition() - CraftTable());

	int stock = Stockpile();
	int cost = PanelCost(m_Draft);

	glm::vec2 span = PanelSpan(m_Draft);

	ImGui::Text("Prefabs: %d built, %d placed", m_Built, m_Placed);

	if (reach > s_TableReach)
		ImGui::TextDisabled("  draw anywhere; build at the table in the shed");

	ImGui::InputText("Name", m_Draft.Name, sizeof(m_Draft.Name));

	// --- The bill -------------------------------------------------------------
	//
	// **Above the canvas, because the pane is shorter than both.** The demo
	// panel is a docked column about 430 px high and the plan view wants most
	// of that; something had to be below the fold. The numbers are what you
	// check while drawing, so the drawing is what scrolls -- and anyone
	// working in here for real drags the panel taller, which is what docking
	// is for.

	ImGui::Checkbox("Stand it up (wall)", &m_Draft.Upright);

	ImGui::Text("%d of %d pieces, %.2f x %.2f m %s, %.0f kg",
		PartCount(m_Draft), s_MaxParts, span.x, span.y,
		m_Draft.Upright ? "wall" : "floor", PanelMass(m_Draft));

	// The cap is the registered-parameter budget, and hitting it silently
	// would look like the editor dropping pieces.
	if (PartCount(m_Draft) >= s_MaxParts)
		ImGui::TextDisabled("  full -- lift a piece before laying another");

	// Longest piece first into the first board it fits, so the count depends
	// on the shape and not on a multiplication.
	ImGui::Text("%d boards to cut them from, %.2f m offcut", cost,
		PanelOffcut(m_Draft));

	ImGui::Text("Stock at the table: %d of %d", stock, cost);

	if (reach <= s_TableReach && stock >= cost && cost > 0)
		ImGui::Text("C builds it, B puts it down where you look");
	else if (reach <= s_TableReach && cost > 0)
		ImGui::TextDisabled("  carry %d more boards to the table", cost - stock);

	// --- The canvas ---------------------------------------------------------

	ImGui::Checkbox("Ledger", &m_EditLedger);

	ImGui::SameLine();
	ImGui::Checkbox("Along Z", &m_EditAlongZ);

	ImGui::SliderInt("Cut to", &m_EditLength, 1, s_BoardCells);

	ImVec2 origin = ImGui::GetCursorScreenPos();

	// Capped, because the panel is a docked column and a canvas that takes all
	// of it pushes the bill -- the thing worth reading -- below the fold.
	float width = glm::clamp(ImGui::GetContentRegionAvail().x, 120.0f, 200.0f);

	float cell = width / (float)s_GridCells;

	ImVec2 size(width, width);

	ImGui::InvisibleButton("##plan", size);

	ImDrawList* draw = ImGui::GetWindowDrawList();

	draw->AddRectFilled(origin,
		ImVec2(origin.x + size.x, origin.y + size.y),
		IM_COL32(28, 26, 24, 255));

	// A line every four cells. Every cell is a thicket at this scale, and what
	// matters is reading the count, not the individual line.
	for (int i = 0; i <= s_GridCells; i += 4)
	{
		float at = (float)i * cell;

		ImU32 tint = (i % s_BoardCells) == 0
			? IM_COL32(90, 84, 74, 255) : IM_COL32(52, 48, 44, 255);

		draw->AddLine(ImVec2(origin.x + at, origin.y),
			ImVec2(origin.x + at, origin.y + size.y), tint);

		draw->AddLine(ImVec2(origin.x, origin.y + at),
			ImVec2(origin.x + size.x, origin.y + at), tint);
	}

	auto paint = [&](const Part& part, ImU32 tint)
	{
		float w = part.AlongZ ? cell : (float)part.Length * cell;
		float h = part.AlongZ ? (float)part.Length * cell : cell;

		ImVec2 from(origin.x + (float)part.X * cell,
			origin.y + (float)part.Z * cell);

		draw->AddRectFilled(from, ImVec2(from.x + w, from.y + h), tint);
		draw->AddRect(from, ImVec2(from.x + w, from.y + h),
			IM_COL32(20, 18, 16, 200));
	};

	// Ledgers first, so the face draws over them the way it lies over them.
	for (const Part& part : PartsOf(m_Draft))
		if (!part.Face)
			paint(part, IM_COL32(112, 84, 52, 255));

	for (const Part& part : PartsOf(m_Draft))
		if (part.Face)
			paint(part, IM_COL32(168, 130, 82, 255));

	// The piece about to go down, and red where it will not go.
	if (ImGui::IsItemHovered())
	{
		ImVec2 mouse = ImGui::GetIO().MousePos;

		Part want;
		want.X = (int)((mouse.x - origin.x) / cell);
		want.Z = (int)((mouse.y - origin.y) / cell);
		want.Length = m_EditLength;
		want.AlongZ = m_EditAlongZ;
		want.Face = !m_EditLedger;

		bool room = PartFits(m_Draft, want)
			&& PartCount(m_Draft) < s_MaxParts;

		paint(want, room ? IM_COL32(210, 190, 140, 130)
			: IM_COL32(190, 70, 60, 130));

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && room)
			AddPart(m_Draft, want);

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			int hit = PartAt(m_Draft, want.X, want.Z, want.Face);

			if (hit >= 0)
				m_Draft.Code[hit] = 0;
		}
	}

	ImGui::TextDisabled("left lays a piece, right lifts one");


	// --- Templates and saved designs ----------------------------------------
	//
	// **The old two-slider panel, demoted to a starting point.** It is still
	// the quickest way to a floor or a wall, and now it is something to cut
	// about rather than the only shape there is.
	ImGui::SliderInt("Template courses", &m_FillCourses, 1, 2);

	// **Bounded so a fill can never truncate.** A course is `across` face
	// boards and two ledgers, so the widest template that still fits the piece
	// cap depends on how many courses there are -- and a fill that quietly
	// stopped half way would look like a bug in the template rather than in
	// the arithmetic of the slider above it.
	// Two bounds, not one: the piece cap, and the grid the pieces sit on --
	// a single course of 62 boards fits the cap and runs 14 cells off the
	// edge, which truncates just as quietly.
	int widest = glm::min((s_MaxParts - 2 * m_FillCourses) / m_FillCourses,
		s_GridCells);

	m_FillAcross = glm::min(m_FillAcross, widest);

	ImGui::SliderInt("Template across", &m_FillAcross, 1, widest);

	if (ImGui::Button("Fill"))
		FillPanel(m_Draft, m_FillAcross, m_FillCourses);

	ImGui::SameLine();

	if (ImGui::Button("Clear"))
		ClearDesign(m_Draft);

	ImGui::SameLine();

	// **Saving and loading is editor state, not simulation state.** The pieces
	// themselves are registered parameters, so a replay writes the recorded
	// design back whatever these buttons did -- which is what lets them be
	// buttons at all, when `C` had to be a key.
	if (ImGui::Button("Save as new") && (int)m_Designs.size() < s_MaxDesigns)
		m_Designs.push_back(m_Draft);

	for (size_t i = 0; i < m_Designs.size(); i++)
	{
		ImGui::PushID((int)i);

		if (ImGui::Button("Load"))
			m_Draft = m_Designs[i];

		ImGui::SameLine();
		ImGui::Text("%s -- %d pieces%s, %d boards", m_Designs[i].Name,
			PartCount(m_Designs[i]),
			m_Designs[i].Upright ? ", upright" : "",
			PanelCost(m_Designs[i]));

		ImGui::PopID();
	}
}

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
	m_TreeMaterial->Set("u_Bounce", 0.22f);
	m_TreeMaterial->Set("u_Through", 0.0f);
	m_TreeMaterial->Set("u_Compliance", 0.0f);
	m_TreeMaterial->Set("u_MaxLean", 0.0f);
	m_TreeMaterial->Set("u_Time", m_Time);

	glm::vec3 centre = ShedCentre();
	const float h = s_ShedHalf;

	// **Sawn timber, not the near-black it was.** The old colour was picked
	// for a room lit by ambient alone, where anything lighter glared. Outdoors
	// beside sunlit ground it came out at 4% of the sand's brightness -- the
	// building read as a hole in the hill. Weathered boards are about this.
	glm::vec3 plank(0.58f, 0.47f, 0.35f);

	DrawBox(centre - glm::vec3(0.0f, 0.5f * s_ShedSlab, 0.0f),
		{ h, 0.5f * s_ShedSlab, h }, 0.0f, glm::vec3(0.42f, 0.33f, 0.23f));
	DrawBox(centre + glm::vec3(0.0f, 3.425f, 0.0f),
		{ h + 0.35f, 0.175f, h + 0.35f }, 0.0f, plank * 0.82f);
	DrawBox(centre + glm::vec3(-h, 1.5f, 0.0f), { 0.25f, 1.75f, h }, 0.0f, plank);
	DrawBox(centre + glm::vec3(h, 1.5f, 0.0f), { 0.25f, 1.75f, h }, 0.0f, plank);
	DrawBox(centre + glm::vec3(0.0f, 1.5f, h), { h, 1.75f, 0.25f }, 0.0f, plank);

	// The wall with the door in it: two posts and a lintel, and the lintel
	// sits at `s_DoorTop` so the opening matches the panel in the field.
	//
	// **The same numbers the collider uses.** The drawn lintel used to stop at
	// 3.0 m while the wall it sits in goes to 3.25, which left a quarter-metre
	// band above the door with nothing in it. Invisible while the ceiling was
	// also at 3.0; a slit of daylight over the doorway the moment the roof
	// went up to meet the walls.
	float side = 0.5f * (h - s_DoorHalf);

	DrawBox(centre + glm::vec3(-(s_DoorHalf + side), 1.5f, -h),
		{ side, 1.75f, 0.25f }, 0.0f, plank);
	DrawBox(centre + glm::vec3(s_DoorHalf + side, 1.5f, -h),
		{ side, 1.75f, 0.25f }, 0.0f, plank);
	DrawBox(centre + glm::vec3(0.0f, 2.85f, -h),
		{ s_DoorHalf, 0.4f, 0.25f }, 0.0f, plank);

	// **The rack, on the wall you face as you walk in.** Two pegs and, when it
	// is not in your hands, the axe on them -- so the wall tells you both that
	// there is a tool and where it goes back.
	glm::vec3 rack = AxeRack();

	glm::vec3 iron(0.30f, 0.31f, 0.33f);
	glm::vec3 haft(0.52f, 0.38f, 0.22f);

	DrawBox(rack + glm::vec3(-0.30f, -0.05f, 0.06f), { 0.035f, 0.035f, 0.09f },
		0.0f, glm::vec3(0.20f, 0.15f, 0.10f));
	DrawBox(rack + glm::vec3(0.30f, -0.05f, 0.06f), { 0.035f, 0.035f, 0.09f },
		0.0f, glm::vec3(0.20f, 0.15f, 0.10f));

	if (!m_HasAxe)
	{
		DrawBox(rack, { 0.36f, 0.028f, 0.028f }, 0.0f, haft);
		DrawBox(rack + glm::vec3(0.34f, 0.0f, 0.0f),
			{ 0.06f, 0.10f, 0.02f }, 0.0f, iron);
	}

	// **The saw bench and the crafting table**, two benches against the side
	// walls. A bench is a top and four legs and neither is worth a mesh; what
	// makes them benches is that `MillLog` and the prefab editor ask how far
	// you are from one.
	auto bench = [&](const glm::vec3& top, const glm::vec3& colour)
	{
		DrawBox(top, { 0.55f, 0.04f, 0.35f }, 0.0f, colour);

		for (int i = 0; i < 4; i++)
			DrawBox(top + glm::vec3((i & 1) ? 0.48f : -0.48f, -0.24f,
				(i & 2) ? 0.28f : -0.28f), { 0.04f, 0.24f, 0.04f }, 0.0f,
				colour * 0.8f);
	};

	bench(SawBench(), glm::vec3(0.40f, 0.31f, 0.20f));
	bench(CraftTable(), glm::vec3(0.36f, 0.28f, 0.19f));

	// **The bearers.** Two sticks on the floor under each pile, so an empty
	// pile is still a place to put boards rather than a patch of floor.
	for (int i = 0; i < 2; i++)
	{
		glm::vec3 pile = PileAt((Pile)i);

		for (int j = 0; j < 2; j++)
			DrawBox(pile + glm::vec3(j ? 0.9f : -0.9f, -0.03f, 0.0f),
				{ 0.07f, 0.03f, 0.55f }, 0.0f, glm::vec3(0.33f, 0.26f, 0.18f));
	}

	// The blade, so the saw bench reads as one rather than as a table.
	DrawBox(SawBench() + glm::vec3(0.0f, 0.16f, 0.0f),
		{ 0.02f, 0.16f, 0.16f }, 0.0f, glm::vec3(0.55f, 0.56f, 0.58f));
}

// **The axe in your hands, drawn in the camera's frame.**
//
// Not a world object with a transform chased every frame: it is fixed relative
// to the eye, so the only honest place to put it is the camera's own space and
// the only matrix needed is the inverse of the view. The stroke is one rotation
// about the axis across the view, eased so it goes over quickly and comes back
// slowly -- which is what a swing does and what a linear ramp never looks like.
inline void TerrainLab::DrawHeldAxe(const Egss::PerspectiveCamera& camera)
{
	if (!m_HasAxe)
		return;

	m_TreeMaterial->Set("u_SunDirection", -SunDirection());
	m_TreeMaterial->Set("u_SunColor", SunColour());
	m_TreeMaterial->Set("u_SkyColor", SkyColour());
	m_TreeMaterial->Set("u_Ambient", 0.75f);
	m_TreeMaterial->Set("u_Bounce", 0.35f);
	m_TreeMaterial->Set("u_Through", 0.0f);
	m_TreeMaterial->Set("u_Compliance", 0.0f);
	m_TreeMaterial->Set("u_MaxLean", 0.0f);
	m_TreeMaterial->Set("u_CutDepth", 0.0f);
	m_TreeMaterial->Set("u_CutPart", 0.0f);
	m_TreeMaterial->Set("u_AimY", 1.0e9f);

	// The stroke: up and back at the start, down and through at the end.
	float swing = m_Swing * m_Swing;

	float angle = glm::radians(-20.0f + 115.0f * (1.0f - swing));

	glm::mat4 eye = glm::inverse(camera.GetViewMatrix());

	glm::mat4 hand = glm::translate(eye, glm::vec3(0.38f, -0.34f, -0.75f));

	hand = glm::rotate(hand, angle, glm::vec3(1.0f, 0.0f, 0.0f));
	hand = glm::rotate(hand, glm::radians(-18.0f), glm::vec3(0.0f, 0.0f, 1.0f));

	m_TreeMaterial->Set("u_Color", glm::vec3(0.52f, 0.38f, 0.22f));

	Egss::Renderer::Submit(m_TreeMaterial, m_Cube,
		glm::scale(glm::translate(hand, glm::vec3(0.0f, 0.20f, 0.0f)),
			glm::vec3(0.022f, 0.24f, 0.022f)));

	m_TreeMaterial->Set("u_Color", glm::vec3(0.30f, 0.31f, 0.33f));

	Egss::Renderer::Submit(m_TreeMaterial, m_Cube,
		glm::scale(glm::translate(hand, glm::vec3(0.0f, 0.44f, 0.0f)),
			glm::vec3(0.05f, 0.075f, 0.016f)));
}

inline void TerrainLab::DrawDoorFrame()
{
	m_TreeMaterial->Set("u_SunDirection", -SunDirection());
	m_TreeMaterial->Set("u_SunColor", SunColour());
	m_TreeMaterial->Set("u_SkyColor", SkyColour());
	m_TreeMaterial->Set("u_Ambient", 0.55f);
	m_TreeMaterial->Set("u_Bounce", 0.22f);
	m_TreeMaterial->Set("u_Through", 0.0f);
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

// **The near plane has to be the doorway, and there is a known way to make it
// one.**
//
// Without this the second camera is an ordinary camera standing in the world a
// couple of metres behind the door, so it draws everything an ordinary camera
// there would -- including whatever happens to be *between* it and the doorway.
// Plant the portal in front of a boulder and the boulder appears in the
// doorway, blocking a view it could not possibly be in.
//
// The fix is not to sort or to cull. It is to move the near plane off the axis
// so that it lies exactly in the plane of the doorway, and let the hardware
// clip against it as it clips against any near plane. The projection matrix
// that does this is **Eric Lengyel's oblique frustum**, from *Oblique View
// Frustum Depth Projection and Clipping* (Journal of Game Development 1(2),
// 2005) -- the standard answer, and the one every portal renderer since Portal
// has used.
//
// The trick is that a projection matrix's third row *is* the near plane, up to
// a scale: a vertex survives clipping when `z_clip > -w_clip`, and both come
// from rows 2 and 3. So replacing row 2 with the wanted plane, scaled so the
// far corner of the frustum still lands at w, gives a frustum with that plane
// as its near plane and everything else -- the four sides and the far plane --
// left alone. `Q` below is that opposite corner, found by asking which corner
// of the unit clip cube is furthest from the plane.
//
// The cost is depth precision: the near plane is no longer at a constant
// distance, so the depth buffer's resolution varies across the frame. It has
// not been visible here, and it is the accepted price.
//
// The plane is given in world space as a point and a normal, with the normal
// pointing into the half you want to keep.
inline glm::mat4 ObliqueNearPlane(const glm::mat4& projection,
	const glm::mat4& view, const glm::vec3& at, const glm::vec3& normal)
{
	// A plane transforms by the inverse transpose, not by the matrix.
	glm::vec4 world(normal, -glm::dot(normal, at));

	glm::vec4 plane = glm::transpose(glm::inverse(view)) * world;

	float length = glm::length(glm::vec3(plane));

	if (length < 1e-6f)
		return projection;

	plane /= length;

	// Behind the camera, or so close to it that the maths is meaningless. An
	// oblique plane through the eye is a degenerate frustum.
	if (plane.w > -1e-3f)
		return projection;

	auto sign = [](float v) { return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f); };

	// The clip-space corner furthest from the plane. `projection` is GLM's
	// column-major, so `p[column][row]`.
	glm::vec4 corner(
		(sign(plane.x) + projection[2][0]) / projection[0][0],
		(sign(plane.y) + projection[2][1]) / projection[1][1],
		-1.0f,
		(1.0f + projection[2][2]) / projection[3][2]);

	glm::vec4 scaled = plane * (2.0f / glm::dot(plane, corner));

	glm::mat4 oblique = projection;

	oblique[0][2] = scaled.x;
	oblique[1][2] = scaled.y;
	oblique[2][2] = scaled.z + 1.0f;
	oblique[3][2] = scaled.w;

	return oblique;
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

	Doorway from = m_ViaPortal ? ShedSideDoor()
		: WorldSideDoor(m_Camera.GetPosition());

	Doorway to = m_ViaPortal ? ReturnDoor() : Doorway{ ShedDoor(), 0.0f };

	Pass pass = m_ViaPortal ? Pass::ToWorld : Pass::ToShed;

	glm::mat3 turn = DoorTurn(from, to);

	// Copied, so it keeps the field of view, the aspect and the clip planes
	// the main camera is using this frame -- which is what the screen-space
	// sampling depends on.
	Egss::PerspectiveCamera other = m_Camera;

	other.SetPosition(to.At + turn * (m_Camera.GetPosition() - from.At));
	other.SetOrientation(turn * m_Camera.GetForward(),
		glm::vec3(0.0f, 1.0f, 0.0f));

	// **Last, because setting the lens would overwrite it.** The kept half is
	// the far side of the destination doorway, which is where its heading
	// points -- and the heading is chosen so the camera is always on the other
	// one, so the plane is always in front of the camera.
	other.SetProjectionMatrix(ObliqueNearPlane(other.GetProjectionMatrix(),
		other.GetViewMatrix(), to.At, Facing(to.Yaw)));

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

	// **First in the panel, and its own section.** It lived at the bottom of
	// "Dev tools" behind four other subsystems, and the panel is a docked
	// column half the window high -- so it was below the fold and might as
	// well not have been there. An editor is a place you work, not a readout,
	// and it goes where the thing being worked on goes.
	if (ImGui::CollapsingHeader("Prefab editor", ImGuiTreeNodeFlags_DefaultOpen))
		DrawPrefabEditor();

	if (ImGui::CollapsingHeader("Dev tools", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("No clip (V)", &m_NoClip);
		ImGui::TextDisabled("  space/ctrl to rise and sink while it is on");

		ImGui::Separator();
		ImGui::Text("Portal: %s%s", m_PortalOn ? "deployed" : "stowed",
			m_ViaPortal ? " (you went through it)" : "");
		ImGui::TextDisabled("  E to plant it in front of you, E again nearby");
		ImGui::TextDisabled("  to pick it up; walk through to skip the walk");
		ImGui::Text("Workshop at (%.0f, %.0f), %.2f m3/m2 of earth moved",
			ShedCentre().x, ShedCentre().z, m_ShedMoved);

		ImGui::Separator();
		ImGui::Text("Axe: %s", m_HasAxe ? "in hand" : "on the shed wall");
		ImGui::TextDisabled("  F at the rack to take it or hang it back;");
		ImGui::TextDisabled("  left mouse swings, and the line on the trunk");
		ImGui::TextDisabled("  is where the stroke will land");
		ImGui::Text("%d strokes, %d trees down", m_Chopped, m_Felled);

		ImGui::Separator();
		ImGui::Text("Timber: %d logs, %d boards", CountTimber(Flotsam::Log),
			CountTimber(Flotsam::Plank));
		ImGui::TextDisabled("  swing at a felled top to buck it into logs;");
		ImGui::TextDisabled("  R takes an armful and puts it down, so a stack");
		ImGui::TextDisabled("  is wherever you carried them to; T at the saw");
		ImGui::TextDisabled("  bench in the shed rips a log into boards");

		if (Carrying())
		{
			ImGui::Text("Carrying %d, %.1f of %.0f kg%s",
				(int)m_Carry.size(), CarriedMass(), CarryLimit(),
				LoadFactor() < 0.999f ? " -- heavy" : "");

			if (LoadFactor() < 0.999f)
				ImGui::TextDisabled("  over the budget, so you walk at %.0f%%",
					100.0f * LoadFactor());
		}
		else
		{
			ImGui::TextDisabled("  an armful is %.0f kg, six rough 2x4s; a log",
				CarryLimit());
			ImGui::TextDisabled("  is 113 kg and goes on one shoulder, slowly");
		}
		ImGui::Text("%d logs bucked, %d riven, %d boards sawn", m_Bucked,
			m_Riven, m_Milled);
		ImGui::TextDisabled("  a nominal %.2f m log is %d boards;"
			" a real one is its own volume", s_LogLength, BoardsPerLog());
		ImGui::TextDisabled("  0.60 x pi r^2 L / (50 x 100 x 2400 mm): a mill");
		ImGui::TextDisabled("  loses the slabs, the edgings and the kerf");

		ImGui::Separator();
		ImGui::Text("Weights placed: %d of %d", m_Stacked, s_WeightCount);
		ImGui::TextDisabled("  G puts one where you are looking. A 2x4 is");
		ImGui::TextDisabled("  4.06 kg with 4.06 kg of reserve, and each");
		ImGui::TextDisabled("  weight is 823 g -- so four float and five do");
		ImGui::TextDisabled("  not, and where you put them decides the heel.");

		if (ImGui::Button("Reset the water"))
			ScatterLoose();

		ImGui::Separator();
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

		ImGui::Separator();

		cover |= ImGui::Checkbox("Boulders", &m_ShowStones);
		cover |= ImGui::SliderFloat("Stones per m^2", &m_StoneDensity,
			0.0f, 5.0f, "%.2f");
		cover |= ImGui::SliderFloat("Stone size", &m_StoneSize, 0.3f, 3.0f,
			"%.2fx");
		ImGui::SliderFloat("Stone reach", &m_StoneReach, 20.0f, 250.0f, "%.0f m");
		ImGui::Text("%d placed, %d drawn, %d with a collider",
			CountStones(), m_StoneDrawn, m_StoneCount);
		ImGui::TextDisabled("  at the foot of steep ground and on faces too");
		ImGui::TextDisabled("  steep for soil; buried where soil forms");

		ImGui::Separator();

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
