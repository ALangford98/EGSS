#pragma once

// Voxel terrain you can dig, and what falls when you dig the wrong bit out.
//
// The heightfield demo next door draws a map that is a *function* of x and z, so
// it can have hills and cannot have caves. This one stores a signed distance in
// three dimensions, which can hold an overhang, an arch or a tunnel -- and, more
// to the point, can be changed while you are standing on it.
//
// Three things are worth watching for:
//
//   * **Digging is arithmetic, not surgery.** Carving a sphere is `max(d, -s)`
//     over the lattice points it reaches. No resampling, no remeshing of the
//     world, and the result is still a distance field for the collider to read.
//
//   * **Only the chunks that changed are rebuilt.** The field tracks which ones
//     an edit touched, including the neighbour below a seam, and the demo
//     rebuilds exactly those meshes. The counter in the panel is how many.
//
//   * **Cut something free and it falls.** After every edit, a flood fill from
//     bedrock finds solid voxels no longer connected to the world; each becomes
//     a dynamic body with its own mesh. That is the standard approach --
//     Teardown makes a new object per disconnected chunk.
//
// What it deliberately does *not* do yet is decide that a connection is too thin
// to hold. A pillar attached by one voxel is attached, so a mountain can hang
// from a thread; making that break needs a load and a strength per connection
// rather than a yes or no, and that is the next piece.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include <map>
#include <chrono>

#include "Demo.h"

class VoxelTerrain : public DemoLayer
{
public:
	VoxelTerrain()
		: DemoLayer("VoxelTerrain"), m_Camera(60.0f, 16.0f / 9.0f, 0.1f, 400.0f)
	{
		RegisterParam("Dig radius", &m_DigRadius);
		RegisterParam("Min island voxels", &m_MinIslandVoxels);
		RegisterParam("Tension", &m_Tension);
		RegisterParam("First person", &m_FirstPerson);
		RegisterParam("Walk speed", &m_WalkSpeed);
		// Registered because it scales the recorded mouse deltas, so a session
		// recorded after moving this slider only replays as itself if the
		// slider moves again on playback.
		RegisterParam("Sensitivity", &m_Sensitivity);
		RegisterParam("Rock strength", &m_StrengthKilo);
	}

	void OnDemoAttach() override
	{
		// Above the ground and looking down at it. The terrain sits around
		// y = 5, and a camera left at the origin starts *inside* the rock --
		// which renders as a sky full of dark backfaces and looks like a
		// meshing bug rather than a misplaced camera.
		m_Camera.SetPosition({ 0.0f, 30.0f, 46.0f });
		m_Camera.SetRotation(m_Yaw, m_Pitch);

		BuildShader();
		Generate();
	}

private:
	// --- The world ---------------------------------------------------------

	// Voxel size and extent. 0.5 m over 48 m is 97 lattice points a side
	// horizontally, which is 24 m of vertical range at the same resolution --
	// about a million samples, and the sparse chunks hold a fraction of them.
	static constexpr float s_Voxel = 0.5f;

	// The walker. A capsule 1.8 m tall overall -- 0.35 radius plus 0.55 of
	// segment either side -- with the eye a little below the top of it.
	static constexpr float s_WalkerRadius = 0.35f;
	static constexpr float s_WalkerHalfHeight = 0.55f;
	static constexpr float s_EyeHeight = 0.75f;
	static constexpr int s_SideX = 97;
	static constexpr int s_SideY = 49;
	static constexpr int s_SideZ = 97;

	void Generate()
	{
		m_Field = std::make_shared<Egss::VoxelField3D>();
		m_Field->Create({ s_SideX, s_SideY, s_SideZ }, s_Voxel,
			{ -0.5f * (s_SideX - 1) * s_Voxel, 0.0f, -0.5f * (s_SideZ - 1) * s_Voxel });

		// The double is for planet-sized fields (see `PositionOfFixed`);
		// this one is 256 m across and loses nothing by dropping it.
		m_Field->Fill([this](const glm::dvec3& p) { return Density(glm::vec3(p)); });
		m_Field->MarkAllDirty();

		BuildWorld();
		RebuildDirtyMeshes();
	}

	// The terrain, as a distance.
	//
	// A height function would be `p.y - h(x, z)` and could never fold over. This
	// starts from that and then *subtracts* rock with a second noise field, which
	// is what opens caves and leaves overhangs -- the surface stops being a graph
	// over the ground plane the moment anything is removed from under it.
	//
	// **The division is not decoration.** `p.y - h(x, z)` has a gradient of
	// length sqrt(1 + |grad h|^2), so it changes faster than a distance does,
	// and sphere tracing through it would step straight past the surface.
	// Dividing by that length gives the first-order true distance.
	float Density(const glm::vec3& p) const
	{
		float h = Height(p.x, p.z);
		glm::vec2 slope = Slope(p.x, p.z);

		float ground = (p.y - h) / std::sqrt(1.0f + glm::dot(slope, slope));

		// Caves, subtracted from the rock. `A \ B` is `max(A, -B)`, where B is
		// the cave's own distance field -- negative inside a cave.
		//
		// **Where there is no cave, B has to be a long way positive, not zero.**
		// Fading the cave out by multiplying its value towards zero is the
		// obvious way to keep caves away from the surface and is wrong: it makes
		// `max(ground, 0)` up there, which clamps every interior sample to
		// exactly zero and moves the surface down to wherever the fade begins.
		// It meshes as a terraced hillside, which reads as a marching-cubes
		// artefact and is not one. Fade towards +50 instead.
		float depth = glm::clamp((h - p.y - 1.2f) / 2.5f, 0.0f, 1.0f);
		float floor = glm::clamp((p.y - 0.8f) / 2.0f, 0.0f, 1.0f);

		float cave = (0.62f - Noise3D(p * 0.10f, 991u)) * 6.0f
			+ (1.0f - depth * floor) * 50.0f;

		return glm::max(ground, -cave);
	}

	float Height(float x, float z) const
	{
		return 9.0f
			+ 3.2f * std::sin(x * 0.075f) * std::cos(z * 0.062f)
			+ 1.4f * std::sin(x * 0.19f + 1.3f)
			+ 1.1f * std::cos(z * 0.17f - 0.7f);
	}

	glm::vec2 Slope(float x, float z) const
	{
		return {
			3.2f * 0.075f * std::cos(x * 0.075f) * std::cos(z * 0.062f)
				+ 1.4f * 0.19f * std::cos(x * 0.19f + 1.3f),
			-3.2f * 0.062f * std::sin(x * 0.075f) * std::sin(z * 0.062f)
				- 1.1f * 0.17f * std::sin(z * 0.17f - 0.7f)
		};
	}

	// Value noise on a 3D lattice, hashed from the coordinate rather than drawn
	// from a generator -- the same rule the heightfield terrain follows, and for
	// the same reason: a generator's state would make each sample depend on how
	// many were taken before it, and the map would stop being a function of its
	// seed.
	static float Noise3D(const glm::vec3& p, uint32_t seed)
	{
		glm::vec3 base = glm::floor(p);
		glm::vec3 f = p - base;
		glm::vec3 s = f * f * (3.0f - 2.0f * f);

		int xi = (int)base.x, yi = (int)base.y, zi = (int)base.z;

		auto corner = [&](int dx, int dy, int dz)
		{
			return Hash(xi + dx, yi + dy, zi + dz, seed);
		};

		float x00 = glm::mix(corner(0, 0, 0), corner(1, 0, 0), s.x);
		float x10 = glm::mix(corner(0, 1, 0), corner(1, 1, 0), s.x);
		float x01 = glm::mix(corner(0, 0, 1), corner(1, 0, 1), s.x);
		float x11 = glm::mix(corner(0, 1, 1), corner(1, 1, 1), s.x);

		return glm::mix(glm::mix(x00, x10, s.y), glm::mix(x01, x11, s.y), s.z);
	}

	// Multiply in `uint32_t`, not in `int` -- see the note on OpenWorld's
	// Hash2D. The cast has to be on the *operand*, not on the result: cast the
	// result and the signed overflow has already happened by the time it runs,
	// which is undefined behaviour that GCC 16 will reason from about the loops
	// calling this. Same bits either way.
	static float Hash(int x, int y, int z, uint32_t seed)
	{
		uint32_t h = seed;
		h ^= (uint32_t)x * 374761393u;
		h ^= (uint32_t)y * 668265263u;
		h ^= (uint32_t)z * 2147483647u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;

		return (float)(h & 0xFFFFFF) / (float)0xFFFFFF;
	}

	void BuildWorld()
	{
		m_World.Clear();
		m_Debris.clear();

		m_World.Gravity = { 0.0f, -9.81f, 0.0f };

		Egss::RigidBody3D ground = Egss::RigidBody3D::MakeSdf({ 0.0f, 0.0f, 0.0f }, m_Field);
		ground.Friction = 0.8f;
		ground.Restitution = 0.0f;
		m_World.AddBody(ground);

		SpawnWalker();
	}

	// --- On foot ------------------------------------------------------------
	//
	// A capsule, because that is what people are made of in a physics engine: no
	// corners to catch on a ledge, and it stands rather than rolling away.
	//
	// **Dynamic, with its rotation thrown away every step.** A dynamic body is
	// what makes it fall, slide down slopes and get shoved by falling rock. A
	// dynamic *capsule* also topples the moment it touches anything off-centre,
	// which is correct physics and useless for walking -- so the orientation is
	// reset and the angular velocity zeroed each step. That is the standard
	// trick and worth naming as one: the body is a real rigid body everywhere
	// except about its own axis.
	void SpawnWalker()
	{
		float ground = 0.0f;
		glm::vec3 normal(0.0f);
		m_World.GroundBelow({ 0.0f, 30.0f, 0.0f }, ground, normal);

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(
			{ 0.0f, ground + s_EyeHeight + 1.0f, 0.0f }, s_WalkerRadius,
			s_WalkerHalfHeight, 75.0f);

		body.Friction = 0.4f;
		body.Restitution = 0.0f;
		// A person does not coast to a halt over ten metres, and the horizontal
		// velocity is set outright every step anyway -- this only damps what the
		// world does to them.
		body.LinearDamping = 0.05f;

		m_Walker = m_World.AddBody(body);
	}

	void MoveWalker(Egss::Timestep step)
	{
		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		// Upright, every step. See SpawnWalker.
		body.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.UpdateInertiaWorld();

		glm::vec3 forward = m_Camera.GetForward();
		forward.y = 0.0f;

		if (glm::length(forward) < 1e-4f)
			forward = { 0.0f, 0.0f, 1.0f };

		forward = glm::normalize(forward);
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 wish(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) wish += forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) wish -= forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) wish += right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) wish -= right;

		// Feet on something? Asked of the world rather than of the contacts,
		// because `GroundBelow` already marches a distance field correctly --
		// including under an arch, where "the height at (x, z)" has no answer.
		float feet = body.Position.y - (s_WalkerHalfHeight + s_WalkerRadius);

		float ground = 0.0f;
		glm::vec3 normal(0.0f, 1.0f, 0.0f);
		// The last argument is the initial best, not a floor to search from,
		// and leaving it at its default of zero rejects every surface below
		// sea level. See the note in `TerrainLab::MoveWalker`.
		bool found = m_World.GroundBelow(body.Position, ground, normal,
			m_Walker, -1000.0f);

		m_Grounded = found && (feet - ground) < 0.25f;

		// The horizontal velocity is *set*, not pushed: a force would have the
		// player accelerating for a second after every key press, and sliding
		// for a second after letting go.
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

		// The eye rides the capsule. No smoothing: the capsule is already what
		// the solver produced, and easing it would put the camera inside the
		// ground on the frame it lands.
		m_Camera.SetPosition(body.Position + glm::vec3(0.0f, s_EyeHeight, 0.0f));

		(void)step;
	}

	// --- Meshing -----------------------------------------------------------

	void RebuildDirtyMeshes()
	{
		const std::vector<glm::ivec3>& dirty = m_Field->DirtyChunks();

		m_LastRemeshed = (int)dirty.size();
		m_TriangleCount = 0;

		for (const glm::ivec3& chunk : dirty)
		{
			glm::ivec3 min, max;
			m_Field->ChunkRange(chunk, min, max);

			// One cell of overlap: the last cell of a chunk reads the first
			// lattice plane of the next, and without it every boundary is a
			// crack.
			Egss::MeshData data = Egss::MarchingCubes::Mesh(*m_Field, min, max);

			size_t key = ChunkKey(chunk);

			if (data.Indices.empty())
				m_Chunks.erase(key);
			else
				m_Chunks[key] = std::make_shared<Egss::Mesh>(data, "VoxelChunk");
		}

		m_Field->ClearDirtyChunks();

		for (const auto& entry : m_Chunks)
			m_TriangleCount += (int)entry.second->GetTriangleCount();
	}

	static size_t ChunkKey(const glm::ivec3& chunk)
	{
		return ((size_t)chunk.z * 1024 + chunk.y) * 1024 + chunk.x;
	}

	// --- Editing -----------------------------------------------------------

	void Dig(bool add)
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float distance = 0.0f;
		glm::vec3 point(0.0f), normal(0.0f);

		if (!m_Field->Raycast(origin, direction, m_Reach, distance, point, normal))
			return;

		// Stepped along the normal when adding, so a new blob sits *on* the
		// surface rather than half inside it.
		glm::vec3 at = add ? point + normal * (m_DigRadius * 0.5f) : point;

		if (m_Field->EditSphere(at, m_DigRadius, add) == 0)
			return;

		m_Edits++;

		RebuildDirtyMeshes();
		Collapse();
		CollectIslands();
	}

	// What the edit left standing that should not be.
	//
	// Connectivity says a pillar attached by one voxel is attached. Statics says
	// that voxel is carrying a building. `VoxelStress` routes the load to the
	// anchors, groups the connections into cross-sections and compares each
	// section's stress against the rock's strength -- so undermining a ledge
	// brings it down without having to cut all the way through it.
	//
	// Iterated, because breaking the worst section changes what every other one
	// carries: a collapse is a cascade. Capped, because a cascade in a map this
	// size can run a long way and a frame has to end.
	void Collapse()
	{
		if (!m_Tension)
			return;

		Egss::VoxelStressSettings settings;
		settings.Density = m_Density;
		settings.Strength = m_Strength;
		settings.AnchorHeight = 0;

		auto before = std::chrono::steady_clock::now();

		// `Relieve` breaks several independent sections from one analysis, and
		// an analysis is a pass over every solid voxel -- so the loop is over
		// *rounds* rather than over breaks. Two rounds of six beats twelve
		// analyses by a factor of six.
		m_LastCollapses = 0;
		for (int round = 0; round < m_MaxRounds; round++)
		{
			int broke = Egss::VoxelStress::Relieve(*m_Field, settings, 6);
			if (broke == 0)
				break;

			m_LastCollapses += broke;
		}

		m_CollapseMilliseconds = (float)std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - before).count();

		m_Collapses += m_LastCollapses;

		if (m_LastCollapses > 0)
			RebuildDirtyMeshes();
	}

	// Whatever the edit cut free, handed to the solver.
	void CollectIslands()
	{
		std::vector<Egss::VoxelIsland> islands = Egss::VoxelIslands::Find(
			*m_Field, 0, (size_t)glm::max(m_MinIslandVoxels, 1));

		for (const Egss::VoxelIsland& island : islands)
		{
			Egss::VoxelField3D piece = Egss::VoxelIslands::Extract(*m_Field, island);

			Egss::MeshData data = Egss::MarchingCubes::Mesh(piece);
			if (data.Indices.empty())
				continue;

			Debris debris;
			debris.Mesh = std::make_shared<Egss::Mesh>(data, "Debris");

			// The mesh comes out in world coordinates and the body has both a
			// position and an orientation, so the mesh has to be carried into
			// the body's frame: undo the island's own rotation, then its centre.
			// The body's transform puts it back, wherever it has tumbled to.
			debris.ToLocal = glm::mat4_cast(glm::inverse(island.Orientation))
				* glm::translate(glm::mat4(1.0f), -island.Centre);

			// **One box per island, oriented to the piece's own axes.** `Sat3D`
			// is convex-only, so a severed lump has to be approximated by
			// something convex, and a box is what there is -- but an
			// axis-aligned one around a slab that broke off at an angle is
			// mostly air, which lands the rock on a corner. Measured on a slab
			// tilted 35 degrees: the oriented box is 79% rock where the
			// axis-aligned box is 27%.
			//
			// Still one box. An L-shaped piece is no better for being turned,
			// and that is what convex decomposition would fix.
			// **The piece as a set of boxes**, which is what makes a concave lump
			// behave like one: `Sat3D` needs convex, and greedy growing turns the
			// island's voxels into a handful of boxes that tile it exactly.
			//
			// The body sits at the centre of *mass*, not the middle of the
			// bounding box -- a rigid body's position has to be its centre of
			// mass or gravity applies a torque that should not exist.
			auto boxes = std::make_shared<std::vector<Egss::CompoundChild>>(
				Egss::VoxelIslands::Decompose(*m_Field, island));

			if (boxes->empty())
				continue;

			Egss::RigidBody3D body = Egss::RigidBody3D::MakeCompound(island.Centre,
				boxes, island.Volume * m_Density);
			body.Orientation = island.Orientation;
			body.UpdateInertiaWorld();

			m_DebrisBoxes += (int)boxes->size();
			body.Friction = 0.7f;
			body.Restitution = 0.0f;

			debris.Body = m_World.AddBody(body);
			m_Debris.push_back(debris);
		}

		if (!islands.empty())
		{
			m_IslandsFound += (int)islands.size();
			RebuildDirtyMeshes();
		}
	}

	// --- Layer -------------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		Look(step);

		if (m_FirstPerson)
			MoveWalker(step);
		else
			MoveCamera(step);

		// Edits happen on the fixed step, not in an event handler: the mouse and
		// the keyboard are in the replay stream and events are not, so a digging
		// session records and replays.
		// Space is jump on foot, so it only digs from the fly camera.
		bool dig = (!m_FirstPerson && Egss::Input::IsKeyPressed(EGSS_KEY_SPACE))
			|| Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
		bool add = (!m_FirstPerson && Egss::Input::IsKeyPressed(EGSS_KEY_F))
			|| Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_RIGHT);

		// One edit per press rather than per step, or holding the key hollows
		// out the map in a second.
		if (dig && !m_WasDigging)
			Dig(false);
		else if (add && !m_WasAdding)
			Dig(true);

		m_WasDigging = dig;
		m_WasAdding = add;

		m_World.Step(step);
	}

	// Shared by both modes: the arrows always turn, and Tab hands the mouse to
	// the camera instead of to the panels.
	//
	// All of this is in the fixed step, mouse included, for the usual reason:
	// the replay stream carries the cursor position per step, so a look done
	// with the mouse replays exactly like one done with the arrows. Deltas are
	// taken between two *sampled* positions rather than from an event, because
	// events are not in the stream.
	void Look(Egss::Timestep step)
	{
		float dt = step;

		// Tab captures, Escape releases -- Escape because a captured cursor is
		// a mode you can get stuck in, and the key everyone tries first should
		// be the way out.
		bool toggle = Egss::Input::IsKeyPressed(EGSS_KEY_TAB);
		if (toggle && !m_WasToggling)
			SetMouseLook(!m_MouseLook);
		else if (m_MouseLook && Egss::Input::IsKeyPressed(EGSS_KEY_ESCAPE))
			SetMouseLook(false);

		m_WasToggling = toggle;

		auto [mouseX, mouseY] = Egss::Input::GetMousePosition();

		if (m_MouseLook)
		{
			// The step that turns it on has no previous position to subtract --
			// and capturing the cursor moves it, so the first delta would be
			// wherever the pointer happened to be sitting. Skip exactly one.
			if (m_MouseSampled)
			{
				// Yaw grows to the *right*: forward is
				// (cos yaw, ., sin yaw) and GetRight is cross(forward, up),
				// which works out as +x at yaw 0. Worth stating because the
				// arrow keys had it backwards until a test compared the turn
				// against that basis vector instead of against itself.
				m_Yaw += (mouseX - m_LastMouseX) * m_Sensitivity;
				// Screen y grows downward, so pushing the mouse away is a
				// negative delta and has to raise the pitch.
				m_Pitch -= (mouseY - m_LastMouseY) * m_Sensitivity;
			}

			m_MouseSampled = true;
		}

		m_LastMouseX = mouseX;
		m_LastMouseY = mouseY;

		// These two were the wrong way round: left arrow turned the camera
		// right. Nobody noticed because the fix is to press the other key.
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  m_Yaw -= m_LookRate * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) m_Yaw += m_LookRate * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    m_Pitch += m_LookRate * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  m_Pitch -= m_LookRate * dt;

		m_Pitch = glm::clamp(m_Pitch, -85.0f, 85.0f);
		m_Camera.SetRotation(m_Yaw, m_Pitch);
	}

	// The logical mode and the hardware cursor are set together here and
	// nowhere else, so they cannot drift apart -- but they are still two
	// things. A replay drives the mode from the recorded Tab presses and must
	// *not* grab the cursor: nobody is watching a playback, and a run that
	// steals the pointer while the machine is being used is the exact problem
	// the hidden window was added to solve.
	void SetMouseLook(bool on)
	{
		m_MouseLook = on;
		m_MouseSampled = false;

		if (!Egss::Input::IsPlayingBack())
			Egss::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	// Said in both modes, because a captured cursor is a state you can get
	// stuck in and the way out is not guessable from the picture.
	void MouseLookHelp()
	{
		if (m_MouseLook)
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
				"Mouse look ON -- Esc or Tab releases the cursor");
		else
			ImGui::TextDisabled("Tab captures the mouse to look");
	}

	void MoveCamera(Egss::Timestep step)
	{
		float dt = step;

		glm::vec3 forward = m_Camera.GetForward();
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 move(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) move += forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) move -= forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) move += right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) move -= right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) move.y += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) move.y -= 1.0f;

		if (glm::length(move) > 1e-4f)
			m_Camera.SetPosition(m_Camera.GetPosition() + glm::normalize(move) * m_Speed * dt);
	}

	// Switching away with the cursor captured would leave the next demo with a
	// pointer it never asked for and no way to give it back.
	void OnDemoDeactivated() override
	{
		SetMouseLook(false);
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		Egss::RenderCommand::SetClearColor({ 0.44f, 0.55f, 0.68f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::BeginScene(m_Camera);

		m_Material->Set("u_SunDirection", glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f)));
		m_Material->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_Material->Set("u_SkyColor", glm::vec3(0.42f, 0.52f, 0.66f));
		m_Material->Set("u_Ambient", 0.25f);

		// The surface, unless it has been replaced by its own wireframe.
		if (!m_ShowWireframe)
		{
			m_Material->Set("u_Color", glm::vec4(0.44f, 0.46f, 0.42f, 1.0f));
			for (const auto& entry : m_Chunks)
				Egss::Renderer::Submit(m_Material, entry.second, glm::mat4(1.0f));
		}
		else
		{
			// **Which vertices marching cubes joined to which.** Backface
			// culling goes with it: half the edges of a closed surface belong
			// to triangles facing away, and culling them leaves a wireframe
			// with holes in exactly the places worth looking at.
			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Line);
			Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

			m_Material->Set("u_Color", glm::vec4(0.85f, 0.90f, 0.80f, 1.0f));
			for (const auto& entry : m_Chunks)
				Egss::Renderer::Submit(m_Material, entry.second, glm::mat4(1.0f));

			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Fill);
			Egss::RenderCommand::SetCullFace(Egss::CullFace::Back);
		}

		// **And where it put them.** Drawn after the surface and without depth
		// testing, so a vertex on the far side of a hill still shows -- which is
		// the point of looking at them at all.
		if (m_ShowPoints)
		{
			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Point);
			Egss::RenderCommand::SetPointSize(4.0f);
			Egss::RenderCommand::SetDepthTest(false);

			m_Material->Set("u_Color", glm::vec4(1.0f, 0.55f, 0.25f, 1.0f));
			for (const auto& entry : m_Chunks)
				Egss::Renderer::Submit(m_Material, entry.second, glm::mat4(1.0f));

			Egss::RenderCommand::SetDepthTest(true);
			Egss::RenderCommand::SetPolygonMode(Egss::PolygonMode::Fill);
		}

		// Debris is drawn from its body's transform, so what you see falling is
		// what the solver is moving.
		m_Material->Set("u_Color", glm::vec4(0.62f, 0.44f, 0.34f, 1.0f));
		for (const Debris& debris : m_Debris)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(debris.Body);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				* debris.ToLocal;

			Egss::Renderer::Submit(m_Material, debris.Mesh, transform);
		}

		Egss::Renderer::EndScene();
	}

	void OnDemoImGui() override
	{
		ImGui::Begin("Voxel terrain");

		ImGui::Checkbox("First person", &m_FirstPerson);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On foot: a capsule that falls, walks and gets shoved by\n"
				"whatever you bring down on it. Off, a fly camera, which is\n"
				"how you look at what you did.");

		if (m_FirstPerson)
		{
			ImGui::Text("WASD walk   Space jump   arrows look");
			ImGui::Text("Left click digs, right click adds");
			MouseLookHelp();
			ImGui::Text("%s, eye at %.2f m", m_Grounded ? "on the ground" : "in the air",
				m_Camera.GetPosition().y);
			ImGui::SliderFloat("Walk speed", &m_WalkSpeed, 1.0f, 12.0f);
			if (ImGui::Button("Put me back on the map"))
			{
				Egss::RigidBody3D& body = m_World.GetBody(m_Walker);
				float ground = 0.0f;
				glm::vec3 normal(0.0f);
				m_World.GroundBelow({ 0.0f, 30.0f, 0.0f }, ground, normal, m_Walker);
				body.Position = { 0.0f, ground + 2.0f, 0.0f };
				body.Velocity = glm::vec3(0.0f);
			}
		}
		else
		{
			ImGui::Text("WASD move   Q/E down/up   arrows look");
			ImGui::Text("Space or left click digs, F or right click adds");
			MouseLookHelp();
		}

		// Always shown, never hidden behind the mouse-look flag: while mouse
		// look is on the cursor is captured, so that is precisely when you
		// cannot reach a slider to change it.
		ImGui::SliderFloat("Sensitivity", &m_Sensitivity, 0.02f, 0.5f);

		ImGui::Separator();
		ImGui::Checkbox("Wireframe", &m_ShowWireframe);
		ImGui::SameLine();
		ImGui::Checkbox("Points", &m_ShowPoints);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("The triangles marching cubes built and the vertices it\n"
				"placed on the cell edges. The points sit *between* lattice\n"
				"samples -- that interpolation is the whole reason the field\n"
				"stores a distance rather than a solid flag, and it is what\n"
				"makes a half-metre grid look smooth instead of blocky.");
		ImGui::Separator();

		ImGui::Text("%.2f ms   %d triangles in %zu chunk meshes",
			m_FrameTime, m_TriangleCount, m_Chunks.size());
		ImGui::Text("field: %zu of %zu chunks allocated, %zu KB",
			m_Field->AllocatedChunks(), m_Field->TotalChunks(),
			m_Field->AllocatedBytes() / 1024);
		ImGui::Text("%d edits, %d chunks remeshed by the last one",
			m_Edits, m_LastRemeshed);
		ImGui::Text("%d islands cut free, %zu still falling, %d collider boxes",
			m_IslandsFound, m_Debris.size(), m_DebrisBoxes);
		ImGui::Text("%d sections failed (%d last edit, %.2f ms)",
			m_Collapses, m_LastCollapses, m_CollapseMilliseconds);

		ImGui::Separator();
		ImGui::Checkbox("Tension", &m_Tension);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off, a pillar held by one voxel holds a mountain --\n"
				"which is what connectivity alone can say, and what\n"
				"Teardown does. On, each cross-section carries a load and\n"
				"fails above a stress, so undermining a ledge brings it\n"
				"down without cutting all the way through.\n"
				"Checked against a cantilever's root stress 3 rho g L^2 / h\n"
				"and against a column's height limit sigma / (rho g).");

		if (m_Tension)
			ImGui::SliderFloat("Rock strength (kPa)", &m_StrengthKilo, 20.0f, 2000.0f);
		m_Strength = m_StrengthKilo * 1000.0f;

		ImGui::SliderFloat("Dig radius", &m_DigRadius, 0.5f, 4.0f);
		ImGui::SliderInt("Min island voxels", &m_MinIslandVoxels, 1, 200);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Pieces smaller than this are deleted rather than dropped.\n"
				"A single edit can shed hundreds of crumbs, and the broadphase\n"
				"measurements in this project say a few hundred loose bodies is\n"
				"where the cost starts to show.");

		if (ImGui::Button("Regenerate"))
		{
			m_Chunks.clear();
			m_Edits = 0;
			m_IslandsFound = 0;
			Generate();
		}

		ImGui::End();
	}

	void BuildShader()
	{
		// A sun rather than a point light: the map is sixty metres across, and
		// the Physics3D shader's `1 / (1 + 0.015 d^2)` falloff leaves anything
		// past a few metres lit by ambient alone.
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Normal;

			void main()
			{
				v_Normal = mat3(u_Transform) * a_Normal;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_Normal;

			uniform vec4 u_Color;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunColor;
			uniform vec3 u_SkyColor;
			uniform float u_Ambient;

			void main()
			{
				vec3 n = normalize(v_Normal);

				float sun = max(dot(n, -u_SunDirection), 0.0);
				// Fills the shadowed side from above, so slopes facing away are
				// still readable instead of black.
				float sky = 0.5 + 0.5 * n.y;

				vec3 lit = u_Color.rgb * (u_Ambient + sun * u_SunColor + sky * u_SkyColor * 0.35);
				color = vec4(lit, u_Color.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("VoxelSun", vertexSrc, fragmentSrc));
		m_Material = Egss::Material::Create(m_Shader);
	}

	struct Debris
	{
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		std::shared_ptr<Egss::Mesh> Mesh;
		glm::mat4 ToLocal = glm::mat4(1.0f);
	};

	Egss::PerspectiveCamera m_Camera;
	// Yaw 0 looks along +x in this camera, not -z: from z = +34 the map is at
	// -90 degrees. Worth stating, because "the terrain is off to one side"
	// looks exactly like a generation bug.
	float m_Yaw = -90.0f;
	float m_Pitch = -29.0f;
	float m_Speed = 14.0f;
	float m_LookRate = 70.0f;

	// Off at startup, and it has to be: capturing the cursor the moment the
	// demo appears takes the pointer off whatever else is on screen.
	bool m_MouseLook = false;
	bool m_WasToggling = false;
	bool m_MouseSampled = false;
	float m_LastMouseX = 0.0f;
	float m_LastMouseY = 0.0f;
	// Degrees per pixel. 0.1 is the usual starting point and puts a 180 turn
	// at about half a mousepad.
	float m_Sensitivity = 0.1f;

	std::shared_ptr<Egss::VoxelField3D> m_Field;
	std::map<size_t, std::shared_ptr<Egss::Mesh>> m_Chunks;
	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;

	Egss::PhysicsWorld3D m_World;
	std::vector<Debris> m_Debris;

	// First person by default: the map is meant to be walked on, and the fly
	// camera is for looking at what you did to it.
	bool m_FirstPerson = true;
	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;
	bool m_Grounded = false;
	float m_WalkSpeed = 5.0f;
	float m_JumpSpeed = 6.0f;

	bool m_ShowWireframe = false;
	bool m_ShowPoints = false;

	bool m_Tension = true;
	// Deliberately weak. Real rock is megapascals; this is what makes a map you
	// can undermine rather than one that is correct.
	// Above the map's own self-weight: the tallest ground is about 15 m over
	// bedrock, and a column of this rock crushes itself past sigma/(rho g), so
	// anything under ~210 kPa has the map collapsing before it is touched.
	float m_StrengthKilo = 500.0f;
	float m_Strength = 500000.0f;
	int m_MaxRounds = 3;
	int m_Collapses = 0;
	int m_LastCollapses = 0;
	float m_CollapseMilliseconds = 0.0f;

	float m_DigRadius = 1.6f;
	float m_Reach = 60.0f;
	float m_Density = 1400.0f;      // rock, near enough, in kg per cubic metre
	int m_MinIslandVoxels = 8;

	bool m_WasDigging = false;
	bool m_WasAdding = false;

	int m_Edits = 0;
	int m_LastRemeshed = 0;
	int m_IslandsFound = 0;
	int m_DebrisBoxes = 0;
	int m_TriangleCount = 0;
	float m_FrameTime = 0.0f;
};
