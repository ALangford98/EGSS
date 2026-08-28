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
	}

	void OnDemoAttach() override
	{
		BuildShaders();
		Generate();

		GoTo(0);
	}

private:
	// --- The block ----------------------------------------------------------

	// Nine chunks of sixteen voxels, plus the one lattice plane that closes
	// the last cell. A metre a voxel by default, so the block is 144 m across
	// and 144 m tall -- room for a hill with air above it and rock below.
	static constexpr int s_Chunks = 9;
	static constexpr int s_Side = s_Chunks * 16 + 1;

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

		// Caves, as rock removed after the fact -- which is what stops the
		// surface being a height function and lets it fold over.
		float CaveStrength = 0.0f;
		float CaveSize = 22.0f;

		unsigned int Seed = 1337u;
	};

	Shape m_Shape;

	// The climate, as two numbers rather than a hydrology pass. That is the
	// whole reason a desert is reachable here: on a planet it takes a landing
	// site search, and here it is a slider.
	float m_Moisture = 0.62f;
	float m_Warmth = 0.55f;

	float m_Voxel = 1.0f;

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

		return unit * m_Shape.Amplitude * 0.5f;
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

		m_World.AddBody(ground);

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
				continue;
			}

			m_Chunks[key] = std::make_shared<Egss::Mesh>(data, "LabChunk");

			BuildChunkGrass(key, chunk, data);
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

		float moisture = m_Moisture;

		// Grass where it is wet enough and not on bare rock. The same test the
		// ground is coloured with, so the two cannot disagree.
		auto allow = [moisture](const glm::vec3&, const glm::vec3&)
		{
			return glm::smoothstep(0.25f, 0.55f, moisture);
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

		// The collider holds the field by pointer, so the shape follows -- but
		// the broadphase bounds do not, and a body whose bounds are stale
		// stops colliding at the edges of what changed.
		m_World.Clear();
		BuildWorld();
	}

	// --- Developer tools ----------------------------------------------------
	//
	// Both of these are meant for the solar demo and are being built here
	// first, where a mistake costs a rebuild of nine chunks rather than a
	// planet.

	struct Spawn
	{
		const char* Name;
		glm::vec3 Where;
		float Moisture;
		float Warmth;
	};

	// **Named places, each with the climate that makes it what it is.**
	// A spawn point that only moves the camera shows you the same ground from
	// somewhere else; what is wanted is to *see a biome*, and here that is two
	// numbers, so the spawn carries them.
	static const std::vector<Spawn>& Spawns()
	{
		static const std::vector<Spawn> places =
		{
			{ "Meadow",     {   0.0f, 40.0f,   0.0f }, 0.72f, 0.55f },
			{ "Desert",     {  48.0f, 40.0f,  48.0f }, 0.08f, 0.92f },
			{ "Steppe",     { -48.0f, 40.0f,  48.0f }, 0.34f, 0.70f },
			{ "Tundra",     {  48.0f, 40.0f, -48.0f }, 0.45f, 0.12f },
			{ "Wetland",    { -48.0f, 40.0f, -48.0f }, 0.95f, 0.60f },
		};

		return places;
	}

	void GoTo(int which)
	{
		if (which < 0 || which >= (int)Spawns().size())
			return;

		const Spawn& place = Spawns()[(size_t)which];

		m_Spawn = which;

		bool climateMoved = m_Moisture != place.Moisture
			|| m_Warmth != place.Warmth;

		m_Moisture = place.Moisture;
		m_Warmth = place.Warmth;

		// Drop in from above the ground rather than at it: the terrain under a
		// spawn point changes with every slider, so a fixed height is either
		// buried or floating, and falling a few metres is neither.
		glm::vec3 at = place.Where;
		at.y = Height(at.x, at.z) + 3.0f;

		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);
		body.Position = at;
		body.Velocity = glm::vec3(0.0f);
		body.Awake = true;

		m_Camera.SetPosition(at + glm::vec3(0.0f, s_EyeHeight, 0.0f));

		// The climate decides where grass grows, so moving between biomes has
		// to rebuild it. Only the grass -- the rock is the same rock.
		if (climateMoved)
			RebuildGrass();
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

				if (digit >= 0 && digit < (int)Spawns().size())
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

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;

	std::shared_ptr<Egss::Shader> m_GrassShader;
	std::shared_ptr<Egss::Material> m_GrassMaterial;

	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;

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

		uniform float u_Moisture;
		uniform float u_Warmth;
		uniform float u_Texture;

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

		void main()
		{
			vec3 normal = normalize(v_Normal);

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

		uniform vec3 u_Wind;
		uniform float u_Time;
		uniform float u_Keep;

		out vec3 v_Normal;
		out float v_Up;
		out float v_Tint;

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

			// A gust that travels with the air over a long swell -- see the
			// solar demo's grass for why the wavelength matters as much as the
			// phase.
			float speed = length(u_Wind);

			float travel = dot(world.xyz, normalize(u_Wind + vec3(1e-6)));
			float gust = 1.0 + 0.45 * sin((travel - speed * u_Time) / 55.0);

			float pressure = 0.5 * 1.2 * speed * speed;

			vec3 lean = u_Wind * (0.0033 * pressure * along * along * gust);
			lean.y = 0.0;

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
	m_Material->Set("u_Moisture", m_Moisture);
	m_Material->Set("u_Warmth", m_Warmth);
	m_Material->Set("u_Texture", 1.0f);

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
		float angle = glm::radians(m_WindAngle);

		glm::vec3 wind = glm::vec3(std::cos(angle), 0.0f, std::sin(angle))
			* m_WindSpeed;

		m_GrassMaterial->Set("u_SunDirection", sun);
		m_GrassMaterial->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_GrassMaterial->Set("u_SkyColor", glm::vec3(0.50f, 0.62f, 0.75f));
		m_GrassMaterial->Set("u_Ambient", 0.55f);
		m_GrassMaterial->Set("u_Wind", wind);
		m_GrassMaterial->Set("u_Time", m_Time);

		// Darker at the root than the tip -- see the fragment shader.
		m_GrassMaterial->Set("u_Root", glm::vec3(0.14f, 0.22f, 0.09f));
		m_GrassMaterial->Set("u_Tip", glm::vec3(0.44f, 0.64f, 0.26f));
		m_GrassMaterial->Set("u_Dry", glm::vec3(0.62f, 0.55f, 0.28f));

		// Grass goes to straw as the ground dries, before it stops growing.
		m_GrassMaterial->Set("u_Dryness",
			glm::smoothstep(0.55f, 0.28f, m_Moisture));

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

			Egss::Renderer::Submit(m_GrassMaterial, entry.second,
				glm::mat4(1.0f));
		}
	}

	Egss::Renderer::EndScene();
}

inline void TerrainLab::OnDemoImGui()
{
	ImGui::Begin("Terrain lab");

	ImGui::Text("%.2f ms  |  %d ground tris, %d grass tris",
		m_FrameTime, m_TriangleCount, m_GrassTriangles);

	ImGui::Text("%d x %d x %d chunks, %.0f m across, %.2f m a voxel",
		s_Chunks, s_Chunks, s_Chunks, Extent(), m_Voxel);

	ImGui::TextDisabled("Tab mouse look, WASD walk, LMB dig, RMB add, V noclip");

	// --- Developer tools ---------------------------------------------------

	if (ImGui::CollapsingHeader("Dev tools", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Checkbox("No clip (V)", &m_NoClip))
		{
			// Nothing to do but say so: `MoveWalker` switches the body between
			// kinematic and dynamic, which is where it has to happen.
		}

		ImGui::TextDisabled("  space/ctrl to rise and sink while it is on");

		ImGui::Separator();
		ImGui::TextDisabled("Spawn points -- number keys 1..%d",
			(int)Spawns().size());

		for (int i = 0; i < (int)Spawns().size(); i++)
		{
			if (i > 0)
				ImGui::SameLine();

			bool here = m_Spawn == i;

			if (here)
				ImGui::PushStyleColor(ImGuiCol_Button,
					ImVec4(0.24f, 0.46f, 0.28f, 1.0f));

			if (ImGui::Button(Spawns()[(size_t)i].Name))
				GoTo(i);

			if (here)
				ImGui::PopStyleColor();
		}
	}

	// --- Generation --------------------------------------------------------
	//
	// Every one of these rebuilds the field, which is why the block is nine
	// chunks: it costs a fraction of a second, so it can happen on release and
	// the slider can be dragged.

	if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool changed = false;

		changed |= ImGui::SliderFloat("Feature size", &m_Shape.FeatureSize,
			8.0f, 180.0f, "%.0f m");
		changed |= ImGui::SliderInt("Octaves", &m_Shape.Octaves, 1, 8);
		changed |= ImGui::SliderFloat("Amplitude", &m_Shape.Amplitude,
			1.0f, 90.0f, "%.1f m");
		changed |= ImGui::SliderFloat("Ridged", &m_Shape.Ridged, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Warp", &m_Shape.Warp, 0.0f, 1.0f);

		ImGui::TextDisabled("  0 rolling hills, 1 ridgelines; warp erodes them");

		changed |= ImGui::SliderFloat("Plateau", &m_Shape.Plateau, 0.0f, 0.9f);
		ImGui::TextDisabled("  the continental-shelf control, at walking scale");

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

		changed |= ImGui::SliderFloat("Voxel", &m_Voxel, 0.4f, 2.0f, "%.2f m");

		if (ImGui::Button("Regenerate") || (changed && !ImGui::IsAnyItemActive()))
			Generate();
	}

	// --- Climate and cover -------------------------------------------------

	if (ImGui::CollapsingHeader("Climate", ImGuiTreeNodeFlags_DefaultOpen))
	{
		bool cover = false;

		cover |= ImGui::SliderFloat("Moisture", &m_Moisture, 0.0f, 1.0f);
		cover |= ImGui::SliderFloat("Warmth", &m_Warmth, 0.0f, 1.0f);

		ImGui::TextDisabled("  dry and warm is desert; the ground follows both");

		cover |= ImGui::Checkbox("Grass", &m_ShowGrass);
		cover |= ImGui::SliderFloat("Blades per m^2", &m_GrassDensity,
			0.0f, 200.0f, "%.0f");
		cover |= ImGui::SliderFloat("Blade height", &m_GrassHeight,
			0.1f, 1.5f, "%.2f m");

		ImGui::SliderFloat("Wind", &m_WindSpeed, 0.0f, 30.0f, "%.1f m/s");
		ImGui::SliderFloat("Wind from", &m_WindAngle, 0.0f, 360.0f, "%.0f deg");

		if (cover && !ImGui::IsAnyItemActive())
			RebuildGrass();
	}

	ImGui::Checkbox("Wireframe", &m_ShowWireframe);

	ImGui::End();
}
