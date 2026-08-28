#pragma once

// A large play area: islands separated by water, streamed in around the
// player instead of baked upfront at load, and frustum-culled at
// submission. Built to answer several questions the roadmap needed a real
// scene for: does a big VoxelField3D world load in performantly with
// per-chunk generation, does culling actually reduce draw calls, and is a
// first-person controller built once and shared "up to scratch". A
// textured-vs-untextured compute comparison is deliberately not in this
// piece -- it needs this demo to exist first as the thing to measure.
//
// **Distant-chunk LOD is in**, on top of `MarchingTetrahedra::Mesh`'s stride:
// chunks past `m_LodNear` mesh on a stride-2 lattice and past `m_LodFar` on
// stride-4, with an 8 m hysteresis band so a chunk sitting on a boundary does
// not remesh every step. It buys little at a 64 m load radius, where almost
// everything is near -- the point of it is that a *bigger* radius becomes
// affordable: when the bands were 24 m and 48 m and the mesher was marching
// cubes, 128 m went from 745,644 triangles to 81,413, and seeing 128 m with LOD
// cost 0.82x what seeing 64 m without it did. The load radius defaults to 128 m
// on the strength of that.
//
// **Both of those numbers have since moved, and the comment claiming otherwise
// outlived them by two commits.** The bands are 56 m and 104 m now, and the
// terrain meshes with marching tetrahedra. Measured at a 128 m radius, once the
// load has converged (it has by step 2,500; step 500 is still 14% short):
//
//     bands       tets       cubes      tets/cubes
//     56 / 104    637,186    186,291    3.42x
//     24 / 48     240,734     67,955    3.54x
//     band factor   2.65x      2.74x
//
// So the mesher is worth ~3.4x and the bands ~2.7x, and the 8.4x recorded on
// 2026-08-18 as the swap's cost is those two multiplied -- the before and after
// were measured either side of a band change nobody was accounting for. The
// swap's own cost in time is **0.33 ms a step** (3.43 against 3.10, over 3,000
// lockstep steps in release), which is why it stays.
//
// Chunks are filled **nearest first** rather than in scan-line order, which
// only started to matter at the larger radius -- a 128 m disc is 10,455 chunks
// and takes minutes to populate at one a step, so the order in which it
// assembles is something you watch happen.
//
// **The field's fixed extent is not the world's extent.** VoxelField3D
// declares its whole lattice at Create() time (400 x 100 x 400 m here), but
// nothing is generated or meshed until a chunk falls within the load
// radius of the player -- see StreamChunks. Turning that into a genuinely
// unbounded world would mean reworking the field's storage from a flat
// indexed array to one keyed by coordinate, which would ripple into
// VoxelIslands/VoxelStress; a large fixed bound gets the practical result
// (nothing reachable in a session) without that risk.

#include <Egss.h>

#include "Vegetation.h"
#include <imgui.h>

#include "Demo.h"
#include "FirstPersonController.h"
#include "ChunkCache.h"
#include "Grass.h"

#include <unordered_set>
#include <unordered_map>
#include <climits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

class OpenWorld : public DemoLayer
{
public:
	OpenWorld()
		: DemoLayer("OpenWorld"), m_Camera(70.0f, 16.0f / 9.0f, 0.1f, 500.0f),
		  m_Controller(m_Camera, -90.0f, -14.0f)
	{
		RegisterParam("Walk speed", &m_Controller.Cfg.WalkSpeed);
		RegisterParam("Sensitivity", &m_Controller.Cfg.MouseSensitivity);
		RegisterParam("Load radius", &m_LoadRadius);
		RegisterParam("Culling", &m_Culling);
		RegisterParam("First person", &m_FirstPerson);
		RegisterParam("Chunks per step", &m_ChunksPerStep);
		RegisterParam("Textured", &m_Textured);
	}

	void OnDemoAttach() override
	{
		BuildShader();
		BuildIslands();

		m_Field = std::make_shared<Egss::VoxelField3D>();
		m_Field->Create({ s_SideX, s_SideY, s_SideZ }, s_Voxel,
			{ -0.5f * (s_SideX - 1) * s_Voxel, s_OriginY, -0.5f * (s_SideZ - 1) * s_Voxel });

		Egss::RigidBody3D ground = Egss::RigidBody3D::MakeSdf({ 0.0f, 0.0f, 0.0f }, m_Field);
		ground.Friction = 0.8f;
		ground.Restitution = 0.0f;
		m_World.Gravity = { 0.0f, -9.81f, 0.0f };
		m_World.AddBody(ground);

		// Built once and drawn with a transform each. A limb is a unit tube
		// from y=0 to y=1, so scaling by (radius, length, radius) puts it
		// between any two points.
		m_LimbMesh.reset(new Egss::Mesh(MakeTube({
			{ 0.00f, 1.00f, 1.00f },
			{ 0.35f, 0.94f, 0.94f },
			{ 1.00f, 0.72f, 0.72f } }, 10), "Limb"));

		// Wider across than deep at the chest, drawn in from the waist, out
		// again at the hips. Five rings is enough to read as a torso.
		// **0.46 m hip to shoulder, not 0.72.** The first version was a torso
		// of roughly the right shape and half a metre too tall, which put the
		// chest above the eyes -- in first person it filled the entire screen.
		// A person's eyes sit about 0.22 m above the shoulders and 0.67 above
		// the hips, and the mesh has to agree with that or the camera ends up
		// inside it.
		m_TorsoMesh.reset(new Egss::Mesh(MakeTube({
			{ 0.00f, 0.150f, 0.105f },
			{ 0.10f, 0.163f, 0.108f },
			{ 0.22f, 0.140f, 0.096f },
			{ 0.36f, 0.158f, 0.104f },
			{ 0.46f, 0.140f, 0.094f } }, 12), "Torso"));

		m_HeadMesh.reset(new Egss::Mesh(MakeOvoid({ 0.098f, 0.125f, 0.108f }, 12, 8), "Head"));
		m_HandMesh.reset(new Egss::Mesh(MakeOvoid({ 0.045f, 0.055f, 0.030f }, 8, 5), "Hand"));
		m_FootMesh.reset(new Egss::Mesh(MakeOvoid({ 0.048f, 0.045f, 0.100f }, 8, 5), "Foot"));
		m_JointMesh.reset(new Egss::Mesh(MakeOvoid({ 1.0f, 1.0f, 1.0f }, 8, 5), "Joint"));

		BuildWater();

		// The spawn island's chunks have to exist before GroundBelow has
		// anything to answer -- unlike the steady-state stream, this one
		// call is synchronous and unbudgeted, the same one-time cost
		// VoxelTerrain's whole-map Fill pays at attach, just over a much
		// smaller area.
		// Before any streaming, so the very first chunks can come from it.
		m_Cache.Open("openworld.chunks", FingerprintDensity());
		if (m_Cache.Rebuilt())
			EGSS_INFO("Chunk cache: starting fresh (absent, or a different world)");
		else
			EGSS_INFO("Chunk cache: {0} chunks already stored", m_Cache.Entries());

		m_Controller.Cfg.HasWater = true;
		m_Controller.Cfg.WaterLevel = s_SeaLevel;

		// Just above the island rather than 40 m over it: the islands are now
		// a few metres tall, so the old spawn height was most of a minute of
		// falling before the demo started.
		glm::vec2 centre = m_Islands[0].Centre;
		glm::vec3 spawn(centre.x, Height(centre.x, centre.y) + 4.0f, centre.y);

		StreamAround(spawn, s_ChunkWorld * 3.0f, 100000);

		SpawnWalker(spawn);
		SpawnRocks(m_Islands[0]);
		SpawnTrees(m_Islands[0]);
		SpawnTools(spawn);
	}

	void OnDemoDeactivated() override
	{
		m_Controller.SetMouseLook(false);
	}

	// --- Update ---------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		float dt = step;

		m_Controller.UpdateLook(dt);

		if (m_FirstPerson)
			m_Grounded = m_Controller.UpdateWalk(m_World, m_Walker, s_EyeHeight, dt);
		else
			m_Controller.UpdateFly(dt);

		// Wave time comes off the fixed clock, never wall-clock: a surface
		// animated from real time makes the demo unable to reproduce itself.
		m_WaveTime += dt;

		// The sea the player is standing in is the sea being drawn -- the
		// controller is told where the surface is *here*, rather than assuming
		// it is flat.
		{
			const glm::vec3& at = m_World.GetBody(m_Walker).Position;
			m_Controller.Cfg.WaterLevel = WaveHeight(at.x, at.z, m_WaveTime);
		}

		{
			const glm::vec3& v = m_World.GetBody(m_Walker).Velocity;
			float speed = glm::length(glm::vec2(v.x, v.z));

			// Paced by distance covered, not by time, so the legs do not
			// scissor on the spot when the player stops.
			m_Stride += speed * dt * s_StridePerMetre;

			if (speed < 0.05f)
				m_Stride = 0.0f;   // stand still, feet together
		}

		m_Swing = glm::max(0.0f, m_Swing - dt / s_SwingTime);

		UpdateFacing(dt, m_Controller.GetYaw(), m_Controller.GetWish());
		UpdateCarry(dt);
		UpdatePickaxe();
		UpdateRockImpacts();
		// Third person is the camera stepping back from the head, not a
		// different controller: aim, reach and the body are all measured from
		// HeadPosition, so nothing else changes when the view does.
		if (m_ThirdPerson && m_FirstPerson)
		{
			UpdateCameraFocus(dt);

			glm::vec3 back = -m_Camera.GetForward();

			m_Camera.SetPosition(m_CameraFocus + back * s_ThirdPersonBack
				+ glm::vec3(0.0f, s_ThirdPersonUp, 0.0f));
		}
		else
		{
			// Kept level with the player while in first person, so switching
			// views does not start with the camera catching up from wherever
			// it was left.
			m_CameraFocus = HeadPosition();
		}

		ApplyUndertow();
		StreamChunks();

		m_World.Step(step);
	}

	// --- Carrying a rock ---------------------------------------------------
	//
	// A held rock is **kinematic**: moved by whoever sets its position and by
	// nothing else. Static would also stop the solver throwing it around, but
	// a static body is scenery -- it would not push the rocks it is dragged
	// through, and shoving one boulder with another is most of the fun of
	// being able to pick one up.
	//
	// Position is driven directly rather than by velocity, because a velocity
	// that has to close the gap to a moving target either lags or overshoots.
	// The velocity it *would* have had is tracked alongside, so letting go
	// throws it rather than dropping it dead.
	void UpdateCarry(float dt)
	{
		bool pressed = Egss::Input::IsKeyPressed(EGSS_KEY_E);
		bool edge = pressed && !m_WasCarryKey;
		m_WasCarryKey = pressed;

		if (edge)
		{
			if (m_Held >= 0 || m_HeldTool >= 0)
				Release();
			else
				TryPickUp();
		}

		if (m_Held < 0 && m_HeldTool < 0)
			return;

		Egss::RigidBody3D& body = m_Held >= 0
			? m_World.GetBody(m_Rocks[m_Held].Handle)
			: m_World.GetBody(m_Tools[m_HeldTool].Handle);

		// Picking up is a reach, not a teleport: the object travels from where
		// it was lying to the hand over a fifth of a second, and the arm --
		// which is drawn to the same point -- goes with it.
		m_PickupBlend = glm::min(1.0f, m_PickupBlend + dt / s_PickupTime);

		float eased = m_PickupBlend * m_PickupBlend * (3.0f - 2.0f * m_PickupBlend);

		glm::vec3 target = glm::mix(m_PickupFrom, CarryPoint(), eased);

		// What it would be moving at if it were following under its own steam.
		// Kept so a release inherits the swing of the camera.
		m_ThrowVelocity = dt > 1e-6f ? (target - body.Position) / dt : glm::vec3(0.0f);

		body.Position = target;

		// Posed, not left as it fell. Blended in over the same reach, so it
		// turns in the hand rather than snapping upright the instant it lifts.
		body.Orientation = glm::slerp(m_PickupOrientation, GripOrientation(), eased);

		body.Velocity = glm::vec3(0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.Awake = true;
	}

	// The small rock nearest to what the camera is pointing at. Size is the
	// gate: a boulder is not liftable, and saying so with the collider's own
	// half-extents means it stays true if the rocks are ever resized.
	// One hand. A rock or a tool, whichever is better lined up -- and holding
	// either means you cannot take the other without putting this one down.
	void TryPickUp()
	{
		// The guard belongs here, not only in the caller. "Your hands are full"
		// is the rule the whole tool design rests on, and a rule enforced by
		// whoever happens to call is a rule waiting to be bypassed.
		if (m_Held >= 0 || m_HeldTool >= 0)
			return;

		int rock = AimedAtRock(s_PickupReach, s_PickupAlignment, true);
		float rockAlignment = rock >= 0
			? Alignment(m_World.GetBody(m_Rocks[rock].Handle).Position) : -1.0f;

		int tool = AimedAtTool(s_PickupReach, s_PickupAlignment);
		float toolAlignment = tool >= 0
			? Alignment(m_World.GetBody(m_Tools[tool].Handle).Position) : -1.0f;

		if (tool >= 0 && toolAlignment >= rockAlignment)
		{
			m_HeldTool = tool;
			m_ThrowVelocity = glm::vec3(0.0f);

			BeginPickup(m_World.GetBody(m_Tools[tool].Handle));
			m_World.GetBody(m_Tools[tool].Handle).Type = Egss::BodyType::Kinematic;
			return;
		}

		if (rock < 0)
			return;

		m_Held = rock;
		m_ThrowVelocity = glm::vec3(0.0f);

		BeginPickup(m_World.GetBody(m_Rocks[rock].Handle));
		m_World.GetBody(m_Rocks[rock].Handle).Type = Egss::BodyType::Kinematic;
	}

	void BeginPickup(Egss::RigidBody3D& body)
	{
		m_PickupFrom = body.Position;
		m_PickupOrientation = body.Orientation;
		m_PickupBlend = 0.0f;

		// **The collider is put away while the thing is in your hand.**
		//
		// A kinematic body overlapping the player is not a nudge -- measured,
		// one sitting inside the walker threw them 80.7 m in five seconds,
		// because kinematic bodies shove dynamic ones and are not shoved back.
		// Posing the tool so its shaft points away keeps it out of the capsule
		// in the ordinary case, and "the ordinary case" is exactly the wrong
		// thing to rely on: a big rock, a slope or a low branch puts it back
		// inside sooner or later.
		//
		// Shrinking the collider was the first attempt and only *reduced* it,
		// 80.7 m to 13.2 -- a tiny box inside a capsule still makes a contact,
		// and a kinematic body still wins it. So the body keeps its shape and
		// stops colliding with the one carrying it, which is what was actually
		// meant. It still collides with everything else, so a held rock can
		// still be knocked out of the way by a falling one.
		body.IgnoreCollisionWith = (int)m_Walker;
	}

	// Lets it collide with the player again the moment it leaves the hand.
	void RestoreHeldCollider()
	{
		if (m_Held >= 0)
			m_World.GetBody(m_Rocks[m_Held].Handle).IgnoreCollisionWith = -1;
		else if (m_HeldTool >= 0)
			m_World.GetBody(m_Tools[m_HeldTool].Handle).IgnoreCollisionWith = -1;
	}

	int AimedAtTool(float reach, float alignment) const
	{
		int best = -1;
		float bestAlignment = alignment;

		for (size_t i = 0; i < m_Tools.size(); i++)
		{
			if ((int)i == m_HeldTool)
				continue;

			glm::vec3 at = m_World.GetBody(m_Tools[i].Handle).Position;
			if (glm::length(at - m_Camera.GetPosition()) > reach)
				continue;

			float aligned = Alignment(at);
			if (aligned > bestAlignment)
			{
				bestAlignment = aligned;
				best = (int)i;
			}
		}

		return best;
	}

	void Release()
	{
		if (m_Held < 0 && m_HeldTool < 0)
			return;

		Egss::RigidBody3D& body = m_Held >= 0
			? m_World.GetBody(m_Rocks[m_Held].Handle)
			: m_World.GetBody(m_Tools[m_HeldTool].Handle);

		RestoreHeldCollider();

		body.Type = Egss::BodyType::Dynamic;

		// Clamped, so a fast flick of the mouse does not launch a rock across
		// the island -- the throw should come from the player moving, not from
		// how sharply they turned on the frame they let go.
		glm::vec3 throwVelocity = m_ThrowVelocity;
		float speed = glm::length(throwVelocity);

		if (speed > s_ThrowSpeedLimit)
			throwVelocity *= s_ThrowSpeedLimit / speed;

		body.Velocity = throwVelocity;
		body.Awake = true;

		m_Held = -1;
		m_HeldTool = -1;
	}

	// --- Trees -------------------------------------------------------------
	//
	// **The generator moved to `Vegetation.h`** so the solar system's Earth
	// could grow the same trees instead of a second implementation of them.
	// What is left here is the demo's own use of it: where a tree stands, how
	// hard it is to fell, and what happens when it is.
	//
	// The names below are forwarders rather than the call sites being rewritten
	// -- forty-odd of them use `Hash2DUnit` alone. Moving code and renaming its
	// callers in one change would have thrown away the check that makes a move
	// safe, which is that the picture does not change.
	using TreeParams = Veg::TreeParams;

	static uint32_t Hash2D(int x, int y, uint32_t seed) { return Veg::Hash2D(x, y, seed); }
	static float Hash2DUnit(int x, int y, uint32_t seed) { return Veg::Hash2DUnit(x, y, seed); }

	static void Basis(const glm::vec3& dir, glm::vec3& outU, glm::vec3& outV)
	{
		Veg::Basis(dir, outU, outV);
	}

	static void Ring(const glm::vec3& centre, const glm::vec3& dir, float radius,
		int sides, const glm::vec3& u, const glm::vec3& v, std::vector<glm::vec3>& out)
	{
		Veg::Ring(centre, dir, radius, sides, u, v, out);
	}

	static void Segment(Egss::MeshData& data, const glm::vec3& base, const glm::vec3& tip,
		float baseRadius, float tipRadius, int sides)
	{
		Veg::Segment(data, base, tip, baseRadius, tipRadius, sides);
	}

	static void LeafCluster(Egss::MeshData& data, const glm::vec3& centre, float radius,
		int segments, int rings, unsigned int seed, int path)
	{
		Veg::LeafCluster(data, centre, radius, segments, rings, seed, path);
	}

	static void Finish(Egss::MeshData& data) { Veg::Finish(data); }

	static void MakeTreeMesh(unsigned int seed, const TreeParams& tree,
		Egss::MeshData& outBark, Egss::MeshData& outLeaves)
	{
		Veg::MakeTreeMesh(seed, tree, outBark, outLeaves);
	}

	struct Tree
	{
		glm::vec3 Position;      // the base, where the trunk meets the ground
		float Yaw;
		float Scale;
		int Shape;

		// A standing tree is a **static** capsule, so it is something you walk
		// into rather than through. Felling it is a change of body type: the
		// same capsule becomes dynamic and gravity does the rest. Nothing is
		// added or removed, which matters because PhysicsWorld3D has no way to
		// remove a body.
		Egss::PhysicsWorld3D::BodyHandle Body = 0;
		float HalfHeight = 1.0f;
		int Hits = 1;
		bool Felled = false;
	};
	// --- The body you are looking out of --------------------------------------
	//
	// A **viewmodel**, not a ragdoll. The Ragdoll demo's thirteen jointed
	// bodies balance because they are simulated; driving them from a kinematic
	// capsule would mean two things deciding where the player is and fighting
	// over it. What is borrowed is the *proportions* -- the limbs are posed
	// procedurally and drawn, and nothing about them is simulated.
	//
	// Only what a person can see of themselves: hips, legs, arms and hands.
	// No head, no chest, no neck, because from inside your own eyes there is
	// nothing there to draw.
	// What the third-person camera actually follows, which is not the player.
	//
	// A camera welded to the head reports every step, every stumble and every
	// jump as a camera move. This one has a **dead zone** the player can move
	// inside without it noticing, and beyond that it closes the gap on a time
	// constant rather than instantly.
	//
	// The vertical dead zone is much larger than the horizontal one on purpose.
	// A jump is a metre up and a metre back down inside half a second, and it
	// should not move the camera at all; climbing a dune is the same metre held
	// for several seconds, and that should. **The two are told apart by how
	// long the offset lasts, not by asking whether the player is jumping** --
	// no flag to get wrong, and it handles falling off a ledge for free.
	void UpdateCameraFocus(float dt)
	{
		glm::vec3 head = HeadPosition();

		// Horizontal: small dead zone, quick catch-up.
		{
			glm::vec2 offset(head.x - m_CameraFocus.x, head.z - m_CameraFocus.z);
			float distance = glm::length(offset);

			if (distance > s_FocusDeadZone)
			{
				glm::vec2 pull = offset * ((distance - s_FocusDeadZone) / distance);
				float k = 1.0f - std::exp(-dt / s_FocusLag);

				m_CameraFocus.x += pull.x * k;
				m_CameraFocus.z += pull.y * k;
			}
		}

		// Vertical: large dead zone, slow catch-up.
		{
			float offset = head.y - m_CameraFocus.y;

			if (std::fabs(offset) > s_FocusDeadZoneUp)
			{
				float over = offset - (offset > 0.0f ? s_FocusDeadZoneUp : -s_FocusDeadZoneUp);
				float k = 1.0f - std::exp(-dt / s_FocusLagUp);

				m_CameraFocus.y += over * k;
			}
		}
	}

	// **The body's own facing, which is not the camera's.**
	//
	// A person can turn their head about seventy degrees before they have to
	// move their feet. Driving the body straight off the camera yaw -- which is
	// what this did -- means the feet spin on the spot the instant the mouse
	// moves, and everything hung off the body spins with them.
	glm::vec3 BodyForward() const
	{
		float yaw = glm::radians(m_BodyYaw);
		return glm::normalize(glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)));
	}

	static float WrapDegrees(float a)
	{
		while (a > 180.0f) a -= 360.0f;
		while (a < -180.0f) a += 360.0f;
		return a;
	}

	// Eased, not linear. A constant rate starts and stops dead, which is what
	// made the turn look mechanical -- a person accelerates into a turn and
	// coasts out of it. Taking a fixed *fraction* of what is left each second
	// gives the coast for free, and the cap keeps the start from being a snap.
	static float TurnToward(float from, float to, float maxStep, float dt)
	{
		float diff = WrapDegrees(to - from);
		float step = diff * glm::min(1.0f, s_TurnEase * dt);

		return from + glm::clamp(step, -maxStep, maxStep);
	}

	// Three rules, and which one applies is the whole behaviour.
	//
	//   Moving -- the feet go where you are going. The body turns toward the
	//   direction of travel, which is what makes walking sideways-to-camera in
	//   third person turn the character around instead of crab-walking.
	//
	//   Standing, first person -- the head is free inside a cone. Past the
	//   neck's limit the body is dragged along, and past a smaller comfort
	//   angle it eases round to face where you are looking, which is the step
	//   you take when you have been peering over your shoulder too long.
	//
	//   Standing, third person -- nothing. The camera orbits a body that stays
	//   put, because looking at your character is not your character turning.
	void UpdateFacing(float dt, float camYaw, const glm::vec3& wish)
	{
		if (glm::length(wish) > 1e-3f)
		{
			float target = glm::degrees(std::atan2(wish.z, wish.x));
			m_BodyYaw = TurnToward(m_BodyYaw, target, s_TurnRate * dt, dt);
			return;
		}

		if (m_ThirdPerson)
			return;

		float diff = WrapDegrees(camYaw - m_BodyYaw);

		if (std::fabs(diff) > s_NeckLimit)
			m_BodyYaw = camYaw - (diff > 0.0f ? s_NeckLimit : -s_NeckLimit);
		else if (std::fabs(diff) > s_NeckComfort)
			m_BodyYaw = TurnToward(m_BodyYaw, camYaw, s_SettleRate * dt, dt);
	}

	glm::vec3 BodyRight() const
	{
		return glm::normalize(glm::cross(BodyForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
	}

	// Where a held thing sits: in the hand. The physics carry point *is* this,
	// so the tool being drawn and the tool being simulated cannot drift apart.
	// The head, which is where the eyes are whether or not the camera is there.
	// Everything hung off the body measures from this rather than from the
	// camera, so pulling the camera back for third person does not drag the
	// player's hands across the island with it.
	glm::vec3 HeadPosition() const
	{
		return m_World.GetBody(m_Walker).Position + glm::vec3(0.0f, s_EyeHeight, 0.0f);
	}

	glm::vec3 CarryPoint() const
	{
		// **In the body's frame, not the camera's.** A hand is on the end of an
		// arm, and an arm is attached to a shoulder -- so turning your head
		// does not carry the thing you are holding around with it. Only the
		// pitch is taken from the camera, because raising what you are holding
		// to look at it is something a person does.
		glm::vec3 forward = BodyForward();
		float pitch = glm::radians(m_Controller.GetPitch());

		glm::vec3 aim = glm::normalize(forward * std::cos(pitch)
			+ glm::vec3(0.0f, std::sin(pitch), 0.0f));

		glm::vec3 at = HeadPosition()
			+ aim * s_HandForward
			+ BodyRight() * s_HandSide
			- glm::vec3(0.0f, s_HandDrop, 0.0f);

		// The swing. `arc` runs 0 -> -1 -> 0 -> +1 -> 0 across the stroke:
		// negative is the wind-up, back and up over the shoulder; positive is
		// the strike, forward and down. One damped sine gets both, and it ends
		// where it started so nothing has to be reset.
		if (m_Swing > 0.0f)
		{
			float t = 1.0f - m_Swing;
			float arc = -std::sin(t * 6.2831853f) * (1.0f - t);

			at += aim * (s_SwingReach * arc)
				- glm::vec3(0.0f, 1.0f, 0.0f) * (s_SwingDrop * arc);
		}

		return at;
	}

	// A tube through a stack of elliptical rings, which is enough to build a
	// person out of: limbs are two rings and a taper, a torso is five rings
	// that are wider across than deep, a head is an ovoid.
	//
	// Elliptical rather than round is most of what stops it reading as pipes.
	// A chest is not a cylinder and neither is a shin.
	struct BodyRing
	{
		float Y;
		float RadiusX;
		float RadiusZ;
	};

	static Egss::MeshData MakeTube(const std::vector<BodyRing>& rings, int sides,
		bool capEnds = true)
	{
		Egss::MeshData data;

		if (rings.size() < 2)
			return data;

		auto at = [&](size_t r, int i)
		{
			float a = (float)(i % sides) / (float)sides * 6.2831853f;
			return glm::vec3(std::cos(a) * rings[r].RadiusX, rings[r].Y,
				std::sin(a) * rings[r].RadiusZ);
		};

		auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			glm::vec3 n = glm::cross(b - a, c - a);
			if (glm::length(n) < 1e-10f)
				return;

			n = glm::normalize(n);

			unsigned int base = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ a, n, { 0.0f, 0.0f } });
			data.Vertices.push_back({ b, n, { 1.0f, 0.0f } });
			data.Vertices.push_back({ c, n, { 0.5f, 1.0f } });
			data.Indices.push_back(base);
			data.Indices.push_back(base + 1);
			data.Indices.push_back(base + 2);
		};

		for (size_t r = 0; r + 1 < rings.size(); r++)
			for (int i = 0; i < sides; i++)
			{
				glm::vec3 a = at(r, i), b = at(r, i + 1);
				glm::vec3 c = at(r + 1, i + 1), d = at(r + 1, i);

				face(a, b, c);
				face(a, c, d);
			}

		if (capEnds)
		{
			glm::vec3 low(0.0f, rings.front().Y, 0.0f);
			glm::vec3 high(0.0f, rings.back().Y, 0.0f);

			for (int i = 0; i < sides; i++)
			{
				face(low, at(0, i + 1), at(0, i));
				face(high, at(rings.size() - 1, i), at(rings.size() - 1, i + 1));
			}
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();

		return data;
	}

	// An ovoid: a sphere squashed independently on each axis. Heads, hands and
	// feet are all this shape with different numbers.
	static Egss::MeshData MakeOvoid(const glm::vec3& radii, int sides, int rings)
	{
		std::vector<BodyRing> stack;

		for (int j = 0; j <= rings; j++)
		{
			float v = (float)j / (float)rings * 3.14159265f;

			stack.push_back({ -std::cos(v) * radii.y,
				glm::max(std::sin(v) * radii.x, 1e-4f),
				glm::max(std::sin(v) * radii.z, 1e-4f) });
		}

		return MakeTube(stack, sides, false);
	}

	// The rotation taking +y to `to`. Tool meshes are built with the shaft
	// along +y precisely so one of these poses any of them.
	static glm::quat ShaftTowards(const glm::vec3& to)
	{
		glm::vec3 from(0.0f, 1.0f, 0.0f);
		glm::vec3 dir = glm::normalize(to);

		float d = glm::dot(from, dir);

		if (d > 0.9999f)
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		if (d < -0.9999f)
			return glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f));

		glm::vec3 axis = glm::normalize(glm::cross(from, dir));
		return glm::angleAxis(std::acos(glm::clamp(d, -1.0f, 1.0f)), axis);
	}

	// How a held tool is carried: shaft up and tilted forward, head away from
	// the player. Lying-down orientation was the bug -- a 0.42 m shaft left
	// horizontal reaches back *through* the walker's capsule, which is why
	// carrying anything shoved the player around.
	glm::quat GripOrientation() const
	{
		// Off the body for the same reason the hand is: a tool held in a hand
		// does not swing round the moment its owner glances sideways.
		glm::vec3 shaft = glm::normalize(glm::vec3(0.0f, 0.75f, 0.0f)
			+ BodyForward() * 0.66f);

		return ShaftTowards(shaft);
	}

	void DrawBody()
	{
		if (!m_ShowBody || !m_LimbMesh)
			return;

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);

		glm::vec3 head = HeadPosition();
		glm::vec3 forward = BodyForward();
		glm::vec3 right = BodyRight();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		// A frame the whole body is described in: x right, y up, z forward.
		glm::mat4 frame(1.0f);
		frame[0] = glm::vec4(right, 0.0f);
		frame[1] = glm::vec4(up, 0.0f);
		frame[2] = glm::vec4(forward, 0.0f);
		frame[3] = glm::vec4(head, 1.0f);

		auto part = [&](const std::shared_ptr<Egss::Mesh>& mesh, const glm::vec3& local,
			const glm::vec3& scale, const glm::vec4& colour)
		{
			if (!mesh)
				return;

			m_Material->Set("u_Color", colour);
			Egss::Renderer::Submit(m_Material, mesh,
				frame * glm::translate(glm::mat4(1.0f), local)
				* glm::scale(glm::mat4(1.0f), scale));
		};

		float swing = std::sin(m_Stride) * s_StrideSwing;

		// Torso and head are in the body's own frame; the neck is short enough
		// that the head sits straight on it.
		part(m_TorsoMesh, glm::vec3(0.0f, -s_HipDrop, 0.0f), glm::vec3(1.0f), m_ClothColour);

		// **No head in first person.** The camera is inside it, and with
		// culling off its inside faces would fill the screen.
		if (m_ThirdPerson)
		{
			// Neck from the shoulders up, then the head sitting on it.
			part(m_LimbMesh, glm::vec3(0.0f, -0.20f, 0.0f), glm::vec3(0.048f, 0.14f, 0.048f),
				m_SkinColour);
			part(m_HeadMesh, glm::vec3(0.0f, 0.02f, 0.01f), glm::vec3(1.0f), m_SkinColour);
		}

		// Legs. World-space points, because the limbs are drawn between pairs
		// of them and a joint is just where two meet.
		for (int side = 0; side < 2; side++)
		{
			float sign = side == 0 ? -1.0f : 1.0f;
			float phase = side == 0 ? swing : -swing;

			glm::vec3 hip = head + right * (sign * s_HipWidth) - up * s_HipDrop;
			glm::vec3 knee = hip + glm::normalize(-up + forward * phase) * s_ThighLength;

			float bend = glm::max(0.0f, phase) * 1.4f;
			glm::vec3 ankle = knee + glm::normalize(-up + forward * (phase - bend)) * s_ShinLength;

			DrawLimb(hip, knee, 0.085f, 0.070f, m_ClothColour);
			DrawLimb(knee, ankle, 0.070f, 0.052f, m_ClothColour);

			DrawJoint(knee, 0.068f, m_ClothColour);

			// The foot points where the leg is going, so it swings with the
			// stride instead of staying nailed forward.
			glm::vec3 toe = ankle + glm::normalize(forward - up * 0.25f) * 0.06f;
			DrawOriented(m_FootMesh, toe, forward, m_BootColour);
		}

		// Arms. The right hand is wherever a carried thing is; the left swings
		// with the opposite leg, which is what walking looks like.
		glm::vec3 shoulderMid = head - up * s_ShoulderDrop;
		glm::vec3 hand = CarryPoint();

		for (int side = 0; side < 2; side++)
		{
			float sign = side == 0 ? -1.0f : 1.0f;
			glm::vec3 shoulder = shoulderMid + right * (sign * s_ShoulderWidth);

			// **An empty hand hangs.** Sticking one out in front is what a
			// person does when they are holding something, and looks like
			// sleepwalking when they are not -- so the reach only happens on
			// the side that is carrying, and only while it is.
			bool carrying = (m_Held >= 0 || m_HeldTool >= 0);

			float swingArm = std::sin(m_Stride) * (side == 0 ? 1.0f : -1.0f);

			glm::vec3 rest = shoulder
				+ forward * (swingArm * 0.13f)
				+ right * (sign * 0.03f)
				- up * 0.52f;

			glm::vec3 to = (side == 1 && carrying) ? hand : rest;

			glm::vec3 elbow = (shoulder + to) * 0.5f + right * (sign * 0.09f) - up * 0.07f;

			DrawJoint(shoulder, 0.058f, m_ClothColour);
			DrawLimb(shoulder, elbow, 0.062f, 0.050f, m_ClothColour);
			DrawLimb(elbow, to, 0.050f, 0.040f, m_SkinColour);
			DrawJoint(elbow, 0.049f, m_ClothColour);

			DrawOriented(m_HandMesh, to, glm::normalize(to - elbow), m_SkinColour);
		}
	}

	// A tapered tube between two points. Two radii, because a forearm is not
	// the same thickness at both ends and that is most of what stops a limb
	// reading as a pipe.
	void DrawLimb(const glm::vec3& from, const glm::vec3& to, float baseRadius,
		float tipRadius, const glm::vec4& colour)
	{
		glm::vec3 along = to - from;
		float length = glm::length(along);

		if (length < 1e-4f || !m_LimbMesh)
			return;

		glm::mat4 frame = LimbFrame(from, along / length);

		m_Material->Set("u_Color", colour);

		// The unit limb already tapers to 0.72; the scale sets the wide end and
		// the mesh does the rest.
		(void)tipRadius;

		Egss::Renderer::Submit(m_Material, m_LimbMesh,
			frame * glm::scale(glm::mat4(1.0f), glm::vec3(baseRadius, length, baseRadius)));
	}

	// A blob at a joint, so knees and shoulders are round rather than a seam
	// between two tubes.
	void DrawJoint(const glm::vec3& at, float radius, const glm::vec4& colour)
	{
		if (!m_JointMesh)
			return;

		m_Material->Set("u_Color", colour);

		Egss::Renderer::Submit(m_Material, m_JointMesh,
			glm::translate(glm::mat4(1.0f), at) * glm::scale(glm::mat4(1.0f), glm::vec3(radius)));
	}

	void DrawOriented(const std::shared_ptr<Egss::Mesh>& mesh, const glm::vec3& at,
		const glm::vec3& facing, const glm::vec4& colour)
	{
		if (!mesh)
			return;

		m_Material->Set("u_Color", colour);

		Egss::Renderer::Submit(m_Material, mesh,
			glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(ShaftTowards(facing)));
	}

	static glm::mat4 LimbFrame(const glm::vec3& from, const glm::vec3& dir)
	{
		glm::vec3 up = std::fabs(dir.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f)
			: glm::vec3(1.0f, 0.0f, 0.0f);

		glm::vec3 x = glm::normalize(glm::cross(up, dir));
		glm::vec3 z = glm::cross(dir, x);

		glm::mat4 frame(1.0f);
		frame[0] = glm::vec4(x, 0.0f);
		frame[1] = glm::vec4(dir, 0.0f);
		frame[2] = glm::vec4(z, 0.0f);
		frame[3] = glm::vec4(from, 1.0f);

		return frame;
	}

	// --- Tools ---------------------------------------------------------------
	//
	// A tool is a physics object lying on the ground, not an inventory slot.
	// You carry one, and to use another you put this one down -- which is the
	// whole design: the constraint is physical, so it needs no UI, no slots and
	// no rules beyond "your hands are full".
	//
	// The three do different jobs and nothing else: the pickaxe damages rock,
	// the axe cuts trees, the shovel moves ground. Swinging the wrong one at
	// something does nothing, and swinging nothing does nothing.
	enum class ToolKind { Pickaxe = 0, Axe, Shovel, Count };

	struct Tool
	{
		ToolKind Kind;
		Egss::PhysicsWorld3D::BodyHandle Handle;
		glm::vec3 HalfExtents;
	};

	static const char* ToolName(ToolKind kind)
	{
		switch (kind)
		{
			case ToolKind::Pickaxe: return "pickaxe";
			case ToolKind::Axe:     return "axe";
			case ToolKind::Shovel:  return "shovel";
			default:                return "nothing";
		}
	}

	// Handle and head as two meshes, so they can be two colours -- wood and
	// metal -- without a second material or a texture.
	static void MakeToolMesh(ToolKind kind, Egss::MeshData& outWood, Egss::MeshData& outMetal)
	{
		auto box = [](Egss::MeshData& data, const glm::vec3& centre, const glm::vec3& half)
		{
			const glm::vec3 normals[6] = {
				{ 1,0,0 }, { -1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 }
			};

			for (int f = 0; f < 6; f++)
			{
				glm::vec3 n = normals[f];

				// Two axes across the face, from whichever the normal is not.
				glm::vec3 u = std::fabs(n.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
				glm::vec3 v = glm::cross(n, u);

				glm::vec3 c = centre + n * glm::dot(half, glm::abs(n));
				glm::vec3 a = u * glm::dot(half, glm::abs(u));
				glm::vec3 b = v * glm::dot(half, glm::abs(v));

				glm::vec3 p[4] = { c - a - b, c + a - b, c + a + b, c - a + b };

				unsigned int at = (unsigned int)data.Vertices.size();
				for (int i = 0; i < 4; i++)
					data.Vertices.push_back({ p[i], n, { 0.0f, 0.0f } });

				const int order[6] = { 0, 1, 2, 0, 2, 3 };
				for (int i = 0; i < 6; i++)
					data.Indices.push_back(at + (unsigned int)order[i]);
			}
		};

		// Shafts all point along +y from the grip, so a held tool can be
		// oriented once and every kind sits in the hand the same way.
		box(outWood, { 0.0f, 0.36f, 0.0f }, { 0.028f, 0.36f, 0.028f });

		switch (kind)
		{
			case ToolKind::Pickaxe:
				// Long and narrow, across the shaft.
				box(outMetal, { 0.0f, 0.74f, 0.0f }, { 0.30f, 0.035f, 0.035f });
				break;

			case ToolKind::Axe:
				// A wedge, offset to one side.
				box(outMetal, { 0.10f, 0.72f, 0.0f }, { 0.10f, 0.10f, 0.030f });
				break;

			case ToolKind::Shovel:
				// A wide flat blade past the end of the shaft.
				box(outMetal, { 0.0f, 0.80f, 0.0f }, { 0.13f, 0.16f, 0.020f });
				break;

			default:
				break;
		}
	}

	void SpawnTools(const glm::vec3& near)
	{
		for (int i = 0; i < (int)ToolKind::Count; i++)
		{
			Egss::MeshData wood, metal;
			MakeToolMesh((ToolKind)i, wood, metal);

			Finish(wood);
			Finish(metal);

			m_ToolWood[i].reset(new Egss::Mesh(wood, "ToolWood"));
			m_ToolMetal[i].reset(new Egss::Mesh(metal, "ToolMetal"));

			// Laid out in front of the spawn, far enough apart to aim at one.
			glm::vec3 at = near + glm::vec3(1.4f + (float)i * 0.9f, 1.0f, 0.6f);

			glm::vec3 half(0.10f, 0.42f, 0.10f);

			Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(at, half, s_ToolMass);
			body.Friction = 0.8f;
			body.Restitution = 0.0f;
			body.LinearDamping = 0.3f;
			body.AngularDamping = 0.5f;

			m_Tools.push_back({ (ToolKind)i, m_World.AddBody(body), half });
		}
	}

	void DrawTools()
	{
		if (m_Tools.empty())
			return;

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);

		for (int pass = 0; pass < 2; pass++)
		{
			m_Material->Set("u_Color", pass == 0 ? m_BarkColour : m_RockColour);

			for (const Tool& tool : m_Tools)
			{
				const Egss::RigidBody3D& body = m_World.GetBody(tool.Handle);

				glm::mat4 transform = glm::translate(glm::mat4(1.0f), body.Position)
					* glm::mat4_cast(body.Orientation)
					* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -tool.HalfExtents.y, 0.0f));

				int i = (int)tool.Kind;
				Egss::Renderer::Submit(m_Material,
					pass == 0 ? m_ToolWood[i] : m_ToolMetal[i], transform);
			}
		}
	}

	ToolKind HeldTool() const
	{
		return m_HeldTool >= 0 ? m_Tools[m_HeldTool].Kind : ToolKind::Count;
	}

	// --- Breaking a rock ---------------------------------------------------
	//
	// A rock too big to lift is a rock you are allowed to hit. The two rules
	// are deliberately complementary and read off the same number -- the
	// collider's own half-extents -- so there is never a rock that is neither.
	void UpdatePickaxe()
	{
		bool swinging = Egss::Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
		bool edge = swinging && !m_WasSwinging;
		m_WasSwinging = swinging;

		if (!edge || m_Held >= 0)
			return;   // a rock in your hands is not a tool

		// The stroke plays whether or not it connects. A swing that only
		// animates when it hits tells the player what the game found before
		// they have finished swinging.
		if (HeldTool() != ToolKind::Count)
			m_Swing = 1.0f;

		switch (HeldTool())
		{
			case ToolKind::Pickaxe:
			case ToolKind::Axe:
				Strike();
				break;

			case ToolKind::Shovel:
				Dig();
				break;

			default:
				break;   // bare hands do nothing, which is the point of tools
		}
	}

	// The shovel. Takes a bite out of the field where you are looking, remeshes
	// what changed, and -- the part that is easy to forget -- writes the edited
	// chunks back to the cache. Without that, the next run would serve the
	// *pre-dig* chunk as a hit and the hole would quietly heal.
	bool Dig()
	{
		glm::vec3 origin = m_Camera.GetPosition();
		glm::vec3 direction = m_Camera.GetForward();

		float distance = 0.0f;
		glm::vec3 point(0.0f), normal(0.0f);

		// **Marched, not sphere-traced.** VoxelField3D::Raycast is the better
		// algorithm and cannot be used here: an unallocated chunk reads
		// `Far` -- a thousand metres -- and tracing believes it, stepping clean
		// out of the world. Measured: a ray started 3 m above the island read
		// 1000 at its origin and 1.86 one metre lower, and hit nothing.
		// Skipping past the sentinel by hand did not help either, because
		// interpolation near the boundary of an allocated chunk mixes in the
		// neighbour's 1000 and overshoots anyway.
		//
		// Half a voxel at a time over a 4.5 m reach is eighteen samples. It
		// costs nothing at this range, it is immune to the sentinel, and --
		// unlike marching the analytic Density -- it sees holes dug earlier.
		//
		// The real fix belongs in Raycast, which can ask the field whether a
		// chunk is uniform instead of inferring it from a magic number.
		bool hit = false;

		for (float t = 0.0f; t <= s_DigReach; t += s_Voxel * 0.5f)
		{
			glm::vec3 at = origin + direction * t;
			glm::ivec3 lattice = glm::ivec3(glm::floor((at - m_Field->Origin()) / s_Voxel));

			float d = m_Field->DistanceAt(lattice.x, lattice.y, lattice.z);

			if (d < Egss::VoxelField3D::Far * 0.5f && d <= 0.0f)
			{
				point = at;
				distance = t;
				hit = true;
				break;
			}
		}

		int changed = hit ? m_Field->EditSphere(point, s_DigRadius, false) : 0;

		if (!hit || changed == 0)
			return false;

		m_Dug++;

		// Captured before remeshing, which clears the list.
		std::vector<glm::ivec3> edited = m_Field->DirtyChunks();

		RebuildDirtyMeshes(StreamFocus());

		if (m_UseCache)
		{
			for (const glm::ivec3& chunk : edited)
			{
				m_Field->SaveChunk(chunk, m_ChunkBytes);
				m_Cache.Write(chunk, m_ChunkBytes);
			}
		}

		return true;
	}

	// One swing. Separated from the input edge so it can be driven without a
	// mouse -- the swing is the thing worth testing, not the click.
	//
	// A swing goes to whichever of a rock or a standing tree is better lined
	// up, rather than preferring one kind: aiming at a trunk with a boulder
	// somewhere off to the side should hit the trunk.
	bool Strike()
	{
		// Each tool only sees what it is for, so an axe swung at a boulder
		// finds nothing rather than finding it and doing nothing.
		bool forRock = HeldTool() == ToolKind::Pickaxe;
		bool forTree = HeldTool() == ToolKind::Axe;

		int rock = forRock ? AimedAtRock(s_StrikeReach, s_StrikeAlignment, false) : -1;
		float rockAlignment = rock >= 0 ? Alignment(m_World.GetBody(m_Rocks[rock].Handle).Position) : -1.0f;

		int tree = forTree ? AimedAtTree(s_StrikeReach, s_StrikeAlignment) : -1;
		float treeAlignment = tree >= 0 ? Alignment(TrunkAim(m_Trees[tree])) : -1.0f;

		if (tree >= 0 && treeAlignment >= rockAlignment)
		{
			m_Trees[tree].Hits--;
			m_Struck++;

			if (m_Trees[tree].Hits <= 0)
				Fell((size_t)tree);

			return true;
		}

		if (rock < 0)
			return false;

		m_Struck++;

		// A swing is an impact like any other -- it goes through the same
		// damage path a falling rock does, so there is one rule for what
		// breaks a rock rather than two that can disagree.
		DamageRock((size_t)rock, s_PickaxeImpact);

		return true;
	}

	float Alignment(const glm::vec3& point) const
	{
		glm::vec3 to = point - m_Camera.GetPosition();

		float distance = glm::length(to);
		if (distance < 1e-4f)
			return -1.0f;

		return glm::dot(to / distance, m_Camera.GetForward());
	}

	// Chest height on the trunk rather than the capsule's centre, so aiming at
	// the part of a tree you would actually swing at is what selects it.
	glm::vec3 TrunkAim(const Tree& tree) const
	{
		return tree.Position + glm::vec3(0.0f, glm::min(1.2f, tree.HalfHeight), 0.0f);
	}

	int AimedAtTree(float reach, float alignment) const
	{
		int best = -1;
		float bestAlignment = alignment;

		for (size_t i = 0; i < m_Trees.size(); i++)
		{
			if (m_Trees[i].Felled)
				continue;   // a trunk on the ground is not a tree to chop

			glm::vec3 aim = TrunkAim(m_Trees[i]);
			if (glm::length(aim - m_Camera.GetPosition()) > reach)
				continue;

			float aligned = Alignment(aim);
			if (aligned > bestAlignment)
			{
				bestAlignment = aligned;
				best = (int)i;
			}
		}

		return best;
	}

	// Felling is a change of body type, not a new object: the same capsule
	// that was standing there becomes dynamic, and gravity takes it from
	// there. The push is only to decide *which way* it goes -- a trunk cut
	// through at the base falls, it does not need to be thrown.
	void Fell(size_t index)
	{
		Tree& tree = m_Trees[index];
		if (tree.Felled)
			return;

		tree.Felled = true;
		m_Felled++;

		Egss::RigidBody3D& body = m_World.GetBody(tree.Body);

		float mass = s_TreeMass * tree.Scale * tree.Scale * tree.Scale;

		body.Type = Egss::BodyType::Dynamic;
		body.SetMass(mass);
		body.RecalculateInertia();
		body.UpdateInertiaWorld();
		body.Awake = true;

		// Away from whoever swung, applied near the top so it topples rather
		// than slides. Nothing here is chosen for realism -- it is chosen so a
		// felled tree falls away from the player instead of onto them.
		glm::vec3 away = tree.Position - m_Camera.GetPosition();
		away.y = 0.0f;

		if (glm::length(away) < 1e-3f)
			away = glm::vec3(1.0f, 0.0f, 0.0f);

		away = glm::normalize(away);

		glm::vec3 top = body.Position + glm::vec3(0.0f, tree.HalfHeight * 0.9f, 0.0f);
		m_World.ApplyImpulseAt(tree.Body, away * (mass * s_FellPush), top);
	}

	// **Cleaved, not subdivided.** A rock splits along a plane into two pieces
	// that came from it and still fit together -- the first version cut every
	// rock into eight equal octants, which does not read as a rock breaking, it
	// reads as a rock being replaced by eight smaller rocks.
	//
	// The cut is axis-aligned through the longest axis at a hashed fraction, so
	// the two halves are unequal and the collider stays a box. Volume, mass and
	// health divide by that same fraction, so all three are still conserved
	// exactly -- f and 1-f sum to one however the plane falls.
	//
	// The *mesh* is cut by the same plane and capped, which is what makes the
	// pieces look like halves of something rather than two new pebbles.
	void Shatter(size_t index)
	{
		Rock parent = m_Rocks[index];

		const Egss::RigidBody3D& before = m_World.GetBody(parent.Handle);

		glm::vec3 half = parent.HalfExtents;

		int axis = (half.x >= half.y && half.x >= half.z) ? 0 : (half.y >= half.z ? 1 : 2);

		// Never near the middle and never near an edge: a sliver is not a
		// piece, and an even split is the thing this replaced.
		float f = 0.34f + Hash2DUnit((int)index, 5, 613u) * 0.32f;

		float parentMass = before.InverseMass > 0.0f ? 1.0f / before.InverseMass : 0.0f;
		float parentHealth = HealthFor(half);

		glm::vec3 centre = before.Position;
		glm::quat orientation = before.Orientation;
		glm::vec3 velocity = before.Velocity;

		float cut = -half[axis] + 2.0f * half[axis] * f;

		for (int piece = 0; piece < 2; piece++)
		{
			glm::vec3 pieceHalf = half;
			pieceHalf[axis] = half[axis] * (piece == 0 ? f : 1.0f - f);

			glm::vec3 offset(0.0f);
			offset[axis] = piece == 0 ? half[axis] * (f - 1.0f) : half[axis] * f;

			glm::vec3 at = centre + orientation * offset;

			// Apart along the cut, so the two halves separate instead of
			// resting exactly against each other.
			glm::vec3 apart = orientation * glm::normalize(offset + glm::vec3(1e-5f))
				* s_ShatterKick;

			// The parent's mesh, cut by the same plane and re-normalised into
			// the piece's own -1..1 box.
			Egss::MeshData cutMesh;
			{
				Egss::MeshData scaled = *parent.Shape;

				for (Egss::MeshVertex& v : scaled.Vertices)
					v.Position *= half;

				Egss::MeshData clipped;
				ClipMesh(scaled, axis, cut, piece == 0, clipped);

				for (const Egss::MeshVertex& v : clipped.Vertices)
				{
					Egss::MeshVertex moved = v;
					moved.Position = (v.Position - offset) / pieceHalf;
					cutMesh.Vertices.push_back(moved);
				}

				cutMesh.Indices = clipped.Indices;
				Finish(cutMesh);
			}

			std::shared_ptr<Egss::MeshData> shape =
				std::make_shared<Egss::MeshData>(std::move(cutMesh));

			std::shared_ptr<Egss::Mesh> mesh = shape->Indices.empty()
				? nullptr : std::make_shared<Egss::Mesh>(*shape, "RockPiece");

			float pieceFraction = piece == 0 ? f : 1.0f - f;

			if (piece == 0)
			{
				Egss::RigidBody3D& body = m_World.GetBody(parent.Handle);

				body.HalfExtents = pieceHalf;
				body.SetMass(parentMass * pieceFraction);
				body.RecalculateInertia();
				body.Position = at;
				body.PreviousPosition = at;
				body.Velocity = velocity + apart;
				body.Awake = true;
				body.UpdateInertiaWorld();

				m_Rocks[index].HalfExtents = pieceHalf;
				m_Rocks[index].Health = parentHealth * pieceFraction;
				m_Rocks[index].PreviousVelocity = body.Velocity;
				m_Rocks[index].Shape = shape;
				m_Rocks[index].MeshPtr = mesh;
				continue;
			}

			Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(at, pieceHalf,
				parentMass * pieceFraction);

			body.Orientation = orientation;
			body.Velocity = velocity + apart;
			body.Friction = 0.9f;
			body.Restitution = 0.0f;
			body.LinearDamping = 0.2f;
			body.AngularDamping = 0.4f;
			body.UpdateInertiaWorld();

			Rock made;
			made.Handle = m_World.AddBody(body);
			made.HalfExtents = pieceHalf;
			made.Health = parentHealth * pieceFraction;
			made.Shape = shape;
			made.MeshPtr = mesh;

			m_Rocks.push_back(made);
		}
	}

	// Everything that can break a rock comes through here: a tool, a fall, a
	// rock thrown at another rock. `impulse` is in newton-seconds, so the
	// threshold is a real quantity rather than a tuning knob with no units --
	// below it, a knock is a knock.
	void DamageRock(size_t index, float impulse)
	{
		if (impulse <= s_ImpactThreshold)
			return;

		m_Rocks[index].Health -= (impulse - s_ImpactThreshold);

		if (m_Rocks[index].Health <= 0.0f)
			Shatter(index);
	}

	// Collisions, measured as the change the solver made to a body's velocity.
	// Gravity contributes 9.81/60 = 0.16 m/s a step, which times even a heavy
	// rock is far under the threshold, so free fall never damages anything --
	// landing does.
	void UpdateRockImpacts()
	{
		for (size_t i = 0; i < m_Rocks.size(); i++)
		{
			Egss::RigidBody3D& body = m_World.GetBody(m_Rocks[i].Handle);

			if (body.Type != Egss::BodyType::Dynamic)
			{
				m_Rocks[i].PreviousVelocity = body.Velocity;
				continue;
			}

			float mass = body.InverseMass > 0.0f ? 1.0f / body.InverseMass : 0.0f;
			float impulse = mass * glm::length(body.Velocity - m_Rocks[i].PreviousVelocity);

			m_Rocks[i].PreviousVelocity = body.Velocity;

			size_t before = m_Rocks.size();
			DamageRock(i, impulse);

			if (m_Rocks.size() != before)
				m_Rocks[i].PreviousVelocity = m_World.GetBody(m_Rocks[i].Handle).Velocity;
		}
	}

	// Shared by the pickaxe and the pickup: the rock nearest to what the
	// camera is pointing at, filtered by whether it is liftable.
	int AimedAtRock(float reach, float alignment, bool wantLiftable) const
	{
		glm::vec3 eye = m_Camera.GetPosition();
		glm::vec3 forward = m_Camera.GetForward();

		int best = -1;
		float bestAlignment = alignment;

		for (size_t i = 0; i < m_Rocks.size(); i++)
		{
			const glm::vec3& half = m_Rocks[i].HalfExtents;
			bool liftable = glm::max(glm::max(half.x, half.y), half.z) <= s_PickupMaxHalf;

			if (liftable != wantLiftable)
				continue;

			glm::vec3 to = m_World.GetBody(m_Rocks[i].Handle).Position - eye;

			float distance = glm::length(to);
			if (distance > reach || distance < 1e-4f)
				continue;

			float aligned = glm::dot(to / distance, forward);
			if (aligned > bestAlignment)
			{
				bestAlignment = aligned;
				best = (int)i;
			}
		}

		return best;
	}

	// --- Streaming --------------------------------------------------------

	// Where streaming is centred: the walker's feet in first person (so the
	// world ahead of a turn loads before the camera swings onto it as much
	// as behind), the camera itself flying free.
	glm::vec3 StreamFocus() const
	{
		return m_FirstPerson ? m_World.GetBody(m_Walker).Position : m_Camera.GetPosition();
	}

	void StreamChunks()
	{
		StreamAround(StreamFocus(), m_LoadRadius, m_ChunksPerStep);
		UpdateLod(StreamFocus(), m_LodPerStep);
		EvictDistantMeshes(StreamFocus());
	}

	// Fills and meshes whatever chunks within `radius` of `focus` are not
	// already resident, up to `budget` chunks this call -- the budget is
	// what keeps a burst of newly-entered terrain from spiking a frame.
	// Vertical range is not distance-limited: the field is only 13 chunks
	// tall, so filling a whole column once its horizontal distance qualifies
	// costs little and keeps the query simple.
	void StreamAround(const glm::vec3& focus, float radius, int budget)
	{
		glm::ivec3 chunkCount = m_Field->ChunkCount();
		glm::vec3 local = focus - m_Field->Origin();
		glm::ivec2 centre = glm::ivec2(glm::floor(glm::vec2(local.x, local.z) / s_ChunkWorld));

		int reach = (int)std::ceil(radius / s_ChunkWorld) + 1;

		// Double in, float out: the field hands out an exact lattice
		// position for planet-scale callers, and this world is 512 m wide.
		auto sdf = [this](const glm::dvec3& p) { return Density(glm::vec3(p)); };

		// **Nearest first.** This used to walk dz then dx, which fills a disc
		// in scan-line order: the far edge of the first row arrives before the
		// ground the player is standing next to. At a 64 m radius that is hard
		// to notice; at 128 m the disc is four times the area and the world
		// visibly assembles in stripes.
		//
		// The offsets are pre-sorted by distance once per reach rather than
		// sorted per step, which keeps the early-out on `budget` -- the loop
		// still stops at the first few unfilled chunks it finds, it just finds
		// the *closest* ones first. Same cost, useful order.
		const std::vector<glm::ivec2>& ring = RingOffsets(reach);

		// Resume where the last call stopped. Everything before the cursor was
		// either filled or permanently skipped, and neither changes while the
		// centre stays put -- without this the scan walks the whole filled
		// interior every step looking for the first gap, which measured 3.2 ms
		// to 6.2 ms in Debug as the interior grew. The cursor resets when the
		// player crosses into a new chunk, which costs one full scan and then
		// amortises away again.
		if (centre != m_RingCentre)
		{
			m_RingCentre = centre;
			m_RingCursor = 0;
		}

		size_t i = m_RingCursor;

		while (i < ring.size() && budget > 0)
		{
			const glm::ivec2& offset = ring[i];
			{
				int cx = centre.x + offset.x;
				int cz = centre.y + offset.y;
				if (cx < 0 || cz < 0 || cx >= chunkCount.x || cz >= chunkCount.z)
				{
					i++;
					continue;
				}

				glm::vec2 chunkCentreXZ = glm::vec2(m_Field->Origin().x, m_Field->Origin().z)
					+ (glm::vec2(cx, cz) + 0.5f) * s_ChunkWorld;
				if (glm::length(chunkCentreXZ - glm::vec2(focus.x, focus.z)) > radius)
				{
					i++;
					continue;
				}

				// The cursor may only pass a column once every chunk in it is
				// filled; running out of budget half way leaves it here.
				bool columnComplete = true;

				for (int cy = 0; cy < chunkCount.y; cy++)
				{
					glm::ivec3 chunk(cx, cy, cz);
					size_t key = ChunkKey(chunk);

					if (m_Filled.count(key))
						continue;

					if (budget <= 0)
					{
						columnComplete = false;
						break;
					}

					// Cache first. A hit is a seek and a memcpy; a miss is a
					// density evaluation for every one of the chunk's 4,096
					// voxels, and then the bytes go back for next time.
					bool loaded = false;
					if (m_UseCache && m_Cache.Read(chunk, m_ChunkBytes))
						loaded = m_Field->LoadChunk(chunk, m_ChunkBytes.data(), m_ChunkBytes.size());

					if (!loaded)
					{
						m_Field->FillChunk(chunk, sdf, 1);

						if (m_UseCache)
						{
							m_Field->SaveChunk(chunk, m_ChunkBytes);
							m_Cache.Write(chunk, m_ChunkBytes);
						}
					}

					m_Filled.insert(key);

					// The chunk itself, plus its low-x/y/z neighbours if
					// already meshed -- see the note on FillChunk in
					// VoxelField3D.h. Marking dirty at the chunk's own
					// (0,0,0) corner would do this in one call, but it also
					// marks all four *diagonal* combinations (low-x-and-y,
					// low-x-and-z, low-y-and-z, low-x-y-z) that MarkDirty's
					// point-based rule cannot help firing when every
					// coordinate sits on a boundary at once -- correct, since
					// those chunks are not actually stale, but it was
					// measured remeshing 16 chunks for 3 newly-filled ones.
					// Three calls, each with exactly one coordinate on the
					// boundary, hit only that one axis's low neighbour (plus
					// this chunk, redundantly-but-harmlessly, each time).
					int lx = cx * Egss::VoxelField3D::ChunkSize;
					int ly = cy * Egss::VoxelField3D::ChunkSize;
					int lz = cz * Egss::VoxelField3D::ChunkSize;
					const int mid = Egss::VoxelField3D::ChunkSize / 2;

					m_Field->MarkDirtyAt(lx, ly + mid, lz + mid);
					m_Field->MarkDirtyAt(lx + mid, ly, lz + mid);
					m_Field->MarkDirtyAt(lx + mid, ly + mid, lz);

					budget--;
				}

				if (!columnComplete)
					break;

				i++;
			}
		}

		m_RingCursor = i;

		RebuildDirtyMeshes(focus);
	}

	// Chunk-column offsets within `reach`, sorted nearest first. Rebuilt only
	// when the reach changes, which happens when the load-radius slider moves.
	const std::vector<glm::ivec2>& RingOffsets(int reach)
	{
		if (reach == m_RingReach)
			return m_Ring;

		m_Ring.clear();
		m_Ring.reserve((size_t)(2 * reach + 1) * (2 * reach + 1));

		for (int dz = -reach; dz <= reach; dz++)
			for (int dx = -reach; dx <= reach; dx++)
				m_Ring.push_back({ dx, dz });

		std::sort(m_Ring.begin(), m_Ring.end(),
			[](const glm::ivec2& a, const glm::ivec2& b)
			{
				int da = a.x * a.x + a.y * a.y;
				int db = b.x * b.x + b.y * b.y;
				// Ties broken on the coordinates so the order is total, and so
				// two runs fill the same chunks in the same sequence -- the
				// demo has to replay identically.
				if (da != db) return da < db;
				if (a.x != b.x) return a.x < b.x;
				return a.y < b.y;
			});

		// A different reach is a different list, so the cursor into the old one
		// means nothing.
		m_RingReach = reach;
		m_RingCursor = 0;
		return m_Ring;
	}

	void RebuildDirtyMeshes(const glm::vec3& focus)
	{
		const std::vector<glm::ivec3>& dirty = m_Field->DirtyChunks();

		for (const glm::ivec3& chunk : dirty)
		{
			// An edit does not change which band a chunk is in, so a resident
			// chunk is remeshed at the stride it already had. A chunk being
			// meshed for the first time has no previous stride and takes the
			// band outright, with no hysteresis to apply.
			auto it = m_Chunks.find(ChunkKey(chunk));
			int stride = (it != m_Chunks.end())
				? it->second.Stride
				: BandFor(FocusDistance(ChunkCentre(chunk), focus));

			MeshChunk(chunk, stride);
		}

		m_Field->ClearDirtyChunks();
	}

	// Meshes one chunk on a lattice `stride` voxels wide and stores the result.
	//
	// Terrain is meshed with MarchingTetrahedra, not MarchingCubes -- found
	// necessary, not stylistic, while building the LOD transition below.
	// MarchingCubes and MarchingTetrahedra are each internally watertight but
	// do *not* agree with each other on a shared face, even at identical
	// stride: a control test (two chunks split at one plane, no LOD involved)
	// measured 102 open edges where a MarchingCubes chunk met a
	// MarchingTetrahedra one, and zero either way when both sides matched.
	// That is marching cubes' well-known ambiguous-saddle resolution
	// disagreeing with the tetrahedral decomposition's fixed one -- the same
	// ambiguity MarchingTetrahedra was built to sidestep in the first place --
	// and it does not stay local: patching one boundary layer just relocates
	// the mismatch to whatever it borders next. The only fix that terminates
	// is one mesher, consistently, wherever chunks can end up adjacent.
	//
	// The LOD boundary itself is closed by VoxelTransition, which recursively
	// refines a coarse chunk's boundary layer to match a neighbour meshed at
	// half its stride -- see VoxelTransition.h for why (two earlier, reverted
	// attempts fanned a coarse cell's seam face from its centre and left the
	// fan's side faces unreconciled with their neighbours). Only the *coarse*
	// side needs this: the fine side already matches, because both compute
	// the same tetrahedral decomposition from the same field at the same
	// fine lattice positions.
	//
	// A chunk needing the transition on two faces at once -- an LOD boundary
	// corner -- is out of scope for now and meshes plainly on every face,
	// which reproduces the old seam there rather than fixing it. Smaller and
	// rarer than today's full seam, and honestly worse than the single-face
	// case rather than silently dropped.
	void MeshChunk(const glm::ivec3& chunk, int stride)
	{
		glm::ivec3 min, max;
		m_Field->ChunkRange(chunk, min, max);

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
			auto it = m_Chunks.find(ChunkKey(chunk + s_FaceDir[f]));
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

		Egss::MeshData data;

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

			data = Egss::MarchingTetrahedra::Mesh(*m_Field, interiorMin, interiorMax, stride);
			Egss::VoxelTransition::MeshBoundaryLayer(*m_Field, layerMin, layerMax, stride, boundaryMask, ratio, data);

			if (!data.Indices.empty())
			{
				Egss::Submesh all;
				all.IndexCount = (unsigned int)data.Indices.size();
				data.Submeshes.push_back(all);
				data.RecalculateBounds();
			}
		}
		else
		{
			data = Egss::MarchingTetrahedra::Mesh(*m_Field, min, max, stride);
		}

		size_t key = ChunkKey(chunk);

		if (data.Indices.empty())
		{
			m_Chunks.erase(key);
			return;
		}

		ChunkEntry entry;
		entry.MeshPtr = std::make_shared<Egss::Mesh>(data, "OpenWorldChunk");
		entry.Stride = stride;
		entry.Coord = chunk;

		if (m_Grass && stride == 1)
		{
			Egss::MeshData blades = BuildGrass(data, chunk);
			if (!blades.Indices.empty())
				entry.GrassPtr = std::make_shared<Egss::Mesh>(blades, "OpenWorldGrass");
		}

		// The bounds describe the chunk's extent in the field, not its
		// triangles, so they are the same at every stride -- which is what
		// keeps frustum culling unaffected by an LOD change.
		glm::vec3 chunkOrigin = m_Field->Origin() + glm::vec3(min) * s_Voxel;
		glm::vec3 chunkExtent = glm::vec3(max - min) * s_Voxel;
		entry.Bounds = { chunkOrigin, chunkOrigin + chunkExtent };

		m_Chunks[key] = entry;
	}

	// Blades of grass, from the shared module. Everything specific to this
	// world is in the two callbacks: up is +Y because this world is flat, and
	// grass grows above the sand line. See `Grass.h` for why they are template
	// parameters rather than `std::function`s.
	//
	// Only stride-1 chunks get grass. That is not a special case bolted on: a
	// stride-2 chunk is already the renderer saying this is far enough away to
	// halve its detail, and grass is the first thing that should go.
	Egss::MeshData BuildGrass(const Egss::MeshData& terrain, const glm::ivec3& chunk) const
	{
		Grass::Settings settings;
		settings.Density = m_GrassDensity;
		settings.Height = m_GrassHeight;
		settings.Width = m_GrassWidth;

		unsigned int chunkSeed = (unsigned int)(chunk.x * 73 + chunk.y * 19
			+ chunk.z * 131);

		float low = m_GrassLow, high = m_GrassHigh;

		return Grass::Build(terrain, settings, chunkSeed,
			[](const glm::vec3&) { return glm::vec3(0.0f, 1.0f, 0.0f); },
			[low, high](const glm::vec3& at, const glm::vec3&)
			{
				// The same test the shader shades with, so a blade never
				// appears on bare sand.
				return glm::smoothstep(low, high, at.y);
			});
	}

	// --- Level of detail --------------------------------------------------
	//
	// Distance is measured in the horizontal plane only, the same as streaming
	// and eviction: the field is 13 chunks tall and a column directly overhead
	// is not meaningfully further away than the one underfoot.
	static float FocusDistance(const glm::vec3& chunkCentre, const glm::vec3& focus)
	{
		return glm::length(glm::vec2(chunkCentre.x, chunkCentre.z)
			- glm::vec2(focus.x, focus.z));
	}

	glm::vec3 ChunkCentre(const glm::ivec3& chunk) const
	{
		glm::ivec3 min, max;
		m_Field->ChunkRange(chunk, min, max);

		glm::vec3 origin = m_Field->Origin() + glm::vec3(min) * s_Voxel;
		return origin + glm::vec3(max - min) * s_Voxel * 0.5f;
	}

	// Which stride a chunk at this distance belongs on, ignoring where it is
	// now. Powers of two only: the mesher's stride divides the 16-voxel chunk
	// exactly at 1, 2 and 4, and a stride that does not divide it evenly
	// clips a partial cell and widens the seam it already has.
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

	// The band, with a margin that must be crossed before a chunk actually
	// changes. Without it a chunk parked on a boundary remeshes every step
	// the player breathes across it -- and remeshing is the expensive thing
	// LOD exists to avoid, so an LOD that thrashes costs more than none.
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

	// Remeshes up to `budget` chunks whose band no longer matches their mesh.
	// Budgeted for the same reason streaming is: the cost is marching cubes on
	// the CPU, and doing every stale chunk in one step is exactly the spike
	// the budget exists to prevent.
	void UpdateLod(const glm::vec3& focus, int budget)
	{
		m_LodRemeshes = 0;

		// Collected first rather than remeshed in place: MeshChunk can erase
		// its entry when a coarser lattice finds no surface at all, and that
		// invalidates the iterator standing on it.
		std::vector<std::pair<glm::ivec3, int>> work;

		for (const auto& [key, entry] : m_Chunks)
		{
			if ((int)work.size() >= budget)
				break;

			int want = DesiredStride(FocusDistance(entry.Bounds.Centre(), focus), entry.Stride);
			if (want != entry.Stride)
				work.push_back({ entry.Coord, want });
		}

		for (const auto& [coord, stride] : work)
			MeshChunk(coord, stride);

		m_LodRemeshes = (int)work.size();
	}

	// Drops the GPU mesh for anything well outside the load radius. Field
	// data is left alone -- re-filling later would cost the same as filling
	// fresh, so there is nothing to save by discarding it, and keeping it
	// means a chunk re-entering range meshes from data that is already
	// there.
	void EvictDistantMeshes(const glm::vec3& focus)
	{
		float evictRadius = m_LoadRadius * 1.6f;

		for (auto it = m_Chunks.begin(); it != m_Chunks.end(); )
		{
			glm::vec3 centre = it->second.Bounds.Centre();
			float d = glm::length(glm::vec2(centre.x, centre.z) - glm::vec2(focus.x, focus.z));

			if (d > evictRadius)
				it = m_Chunks.erase(it);
			else
				++it;
		}
	}

	static size_t ChunkKey(const glm::ivec3& chunk)
	{
		return ((size_t)chunk.z * 1024 + chunk.y) * 1024 + chunk.x;
	}

	// --- Terrain shape: islands, and water in between --------------------

	struct Island
	{
		glm::vec2 Centre;
		float Radius;
	};

	void BuildIslands()
	{
		m_Islands.clear();

		for (int i = 0; i < s_IslandCount; i++)
		{
			float angle = ((float)i / (float)s_IslandCount) * 6.2831853f
				+ Hash2DUnit(i, 0, 7u) * 1.5f;
			float distance = 55.0f + Hash2DUnit(i, 1, 7u) * 85.0f;
			float radius = s_IslandRadiusMin
				+ Hash2DUnit(i, 2, 7u) * (s_IslandRadiusMax - s_IslandRadiusMin);

			m_Islands.push_back({
				{ std::cos(angle) * distance, std::sin(angle) * distance }, radius });
		}
	}

	// The largest of every island's own radial falloff: +Radius at an
	// island's centre, crossing zero at its edge, and increasingly negative
	// (deep water) wherever no island reaches. Also returns which island
	// won, for Slope -- computing the mask and its gradient in separate
	// passes over the same island list is one of the two redundant costs
	// Slope used to pay; see the note there.
	float IslandMask(float x, float z, const Island** outWinner = nullptr) const
	{
		float best = -80.0f;
		const Island* winner = nullptr;

		for (const Island& island : m_Islands)
		{
			float d = glm::length(glm::vec2(x, z) - island.Centre);
			float local = island.Radius - d;

			if (local > best)
			{
				best = local;
				winner = &island;
			}
		}

		if (outWinner)
			*outWinner = winner;

		return best;
	}

	float Height(float x, float z) const
	{
		float mask = IslandMask(x, z);

		// Relief only matters near or above the coast -- hill noise applied
		// to the seafloor too would look like the islands sit over jagged
		// undersea mountains rather than in open water.
		float coastFade = glm::clamp(mask / 15.0f + 0.5f, 0.0f, 1.0f);

		float relief = s_ReliefBroad * Noise2D(x * 0.02f, z * 0.02f, 401u)
			+ s_ReliefFine * Noise2D(x * 0.05f, z * 0.05f, 402u);

		// s_MaskToHeight: the mask is metres of falloff per metre of distance
		// from an island's edge, which reads as a cliff at 1:1. Scaled right
		// down, a coastline slopes like a beach and the middle of an island is
		// a low sandy rise rather than a mountain -- at 0.10 a 30 m island
		// peaks about 3 m above the sea instead of 19 m.
		// The crease at the waterline is deliberate: a beach really does
		// change slope where it enters the water.
		float base = mask > 0.0f ? mask * s_MaskToHeight : mask * s_SeabedDrop;

		return base + relief * coastFade;
	}

	// Analytic, not finite-difference: the winning island's own mask is
	// `Radius - |p - centre|`, whose gradient is `-(p - centre) / |p -
	// centre|` -- cheap, and exact everywhere except the measure-zero
	// boundary where two islands' masks are equal, which a true max() is
	// not differentiable at either. Relief's own gradient is dropped, which
	// under-reports slope on the hilliest ground by up to relief's
	// amplitude against the mask's much larger one -- a small, known
	// approximation, kept because the alternative (finite-differencing
	// Height, as VoxelField3D::SampleNormal and
	// Heightfield3D::SmoothNormalAt do for exactly this kind of function)
	// measured at 7.8 ms per 4,096-voxel chunk: four extra Height() calls
	// per voxel, each re-running both noise octaves and the island loop.
	// This version measures at 1.7 ms/chunk for the same field -- see the
	// changelog entry.
	glm::vec2 Slope(float x, float z) const
	{
		const Island* winner = nullptr;
		IslandMask(x, z, &winner);

		if (!winner)
			return glm::vec2(0.0f);

		glm::vec2 toCentre = glm::vec2(x, z) - winner->Centre;
		float d = glm::length(toCentre);

		if (d < 1e-4f)
			return glm::vec2(0.0f);

		// **The same constants Height uses, and the same branch.** These were
		// two separate 0.55 literals; shared constants are the only thing
		// stopping a change to the terrain's height from silently leaving the
		// normals describing the old shape -- and now that land and seabed
		// scale differently, the side has to be picked here too.
		float mask = winner->Radius - d;
		float scale = mask > 0.0f ? s_MaskToHeight : s_SeabedDrop;

		return -(toCentre / d) * scale;
	}

	// Same technique VoxelTerrain::Density uses: a height function's
	// gradient has length sqrt(1 + |grad h|^2), which changes faster than a
	// distance does, so dividing by that length gives the first-order true
	// distance -- without it, sphere tracing (and the sparse-chunk test,
	// which relies on the same Lipschitz property) would step past thin
	// slopes.
	float Density(const glm::vec3& p) const
	{
		float h = Height(p.x, p.z);
		glm::vec2 slope = Slope(p.x, p.z);
		return (p.y - h) / std::sqrt(1.0f + glm::dot(slope, slope));
	}

	// Value noise: an integer hash per lattice corner, smoothstep-
	// interpolated. The same idiom as Terrain::ValueNoise and
	// VoxelTerrain::Noise3D -- a hash of the coordinate rather than a
	// generator with state, so the map is a pure function of x, z and seed
	// and does not depend on what was sampled before it. Written fresh
	// rather than reused because both existing versions are private to
	// their own class.
	//
	// **Multiply in `uint32_t`, not in `int`.** `(uint32_t)(x * 374761393)`
	// overflows a signed int for any |x| above 5 and is undefined behaviour --
	// the cast is outside the multiply, so it converts a result that was
	// already illegal. Casting first makes the wraparound the defined kind,
	// and the bit pattern is identical, so the terrain does not move.
	//
	// This was not a theoretical complaint. GCC 16 reasoned from it that a
	// `for (int i = 0; i < 16; i++)` loop calling this could not survive past
	// i=5, concluded the `i < 16` test was therefore not what ended the loop,
	// and deleted it -- leaving an unconditional back edge that spawned rocks
	// until the process was OOM killed at 17 GB. Release only; Debug was fine.
	// See the trap in HANDOVER. Terrain::Hash always had the cast in the right
	// place; these two copies of the idiom did not.
	static float Noise2D(float x, float y, uint32_t seed)
	{
		float xi = std::floor(x), yi = std::floor(y);
		float fx = x - xi, fy = y - yi;
		float sx = fx * fx * (3.0f - 2.0f * fx);
		float sy = fy * fy * (3.0f - 2.0f * fy);

		int xii = (int)xi, yii = (int)yi;
		float n00 = Hash2DUnit(xii, yii, seed);
		float n10 = Hash2DUnit(xii + 1, yii, seed);
		float n01 = Hash2DUnit(xii, yii + 1, seed);
		float n11 = Hash2DUnit(xii + 1, yii + 1, seed);

		// Centred on zero rather than [0,1), so it can push the mask either
		// way instead of only ever raising it.
		return glm::mix(glm::mix(n00, n10, sx), glm::mix(n01, n11, sx), sy) - 0.5f;
	}

	// --- The walker ---------------------------------------------------------

	static constexpr float s_WalkerRadius = 0.35f;
	static constexpr float s_WalkerHalfHeight = 0.55f;
	static constexpr float s_EyeHeight = 0.75f;

	// A lumpy, flat-shaded blob on the unit sphere: a lattice of points whose
	// radius is pushed in and out by the same hash the rest of the world uses,
	// emitted as independent triangles so every face gets its own normal.
	//
	// Flat normals are the point. A smooth-shaded rock under cel banding is a
	// soft gradient with a couple of bands crossing it; a faceted one is a set
	// of flat plates, each a single shade, which is what makes it read as
	// stone in this style at all.
	static Egss::MeshData MakeRockMesh(unsigned int seed)
	{
		const int segments = 16, rings = 10;

		auto point = [&](int i, int j)
		{
			// Wrap the seam so the last column is literally the first.
			int wrapped = i % segments;

			float u = (float)wrapped / (float)segments * 6.2831853f;
			float v = (float)j / (float)rings * 3.14159265f;

			// 0.84..1.0. Was 0.68..1.0 on a 9x6 lattice, which read as a lump
			// of coal -- more facets and a shallower jitter give a boulder that
			// is still faceted but no longer jagged. The ceiling stays at 1.0
			// so the blob cannot leave the box that collides for it.
			float radius = 0.84f + Hash2DUnit(wrapped, j, seed) * 0.16f;

			// Poles pulled in a little, or a jittered pole spikes.
			if (j == 0 || j == rings)
				radius = 0.86f + Hash2DUnit(0, j, seed) * 0.10f;

			return glm::vec3(
				std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)) * radius;
		};

		Egss::MeshData data;

		auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			glm::vec3 n = glm::cross(b - a, c - a);
			if (glm::length(n) < 1e-8f)
				return;

			n = glm::normalize(n);

			unsigned int base = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ a, n, { 0.0f, 0.0f } });
			data.Vertices.push_back({ b, n, { 1.0f, 0.0f } });
			data.Vertices.push_back({ c, n, { 0.5f, 1.0f } });
			data.Indices.push_back(base);
			data.Indices.push_back(base + 1);
			data.Indices.push_back(base + 2);
		};

		for (int j = 0; j < rings; j++)
		{
			for (int i = 0; i < segments; i++)
			{
				glm::vec3 a = point(i, j), b = point(i + 1, j);
				glm::vec3 c = point(i + 1, j + 1), d = point(i, j + 1);

				// Degenerate at the poles, where the whole ring is one point --
				// `face` drops those on the zero-area test.
				face(a, b, c);
				face(a, c, d);
			}
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();

		return data;
	}

	// Cuts a mesh with an axis-aligned plane and caps the hole, so the piece
	// that comes back is a closed solid rather than an open shell.
	//
	// The cap is a fan from the centroid of the cut edges. That is only valid
	// while the cross-section is a simple polygon, which holds for these blobs
	// because every one of them is star-shaped about its own centre -- it would
	// not hold for a torus, and this is not a general mesh boolean.
	static void ClipMesh(const Egss::MeshData& in, int axis, float cut, bool keepBelow,
		Egss::MeshData& out)
	{
		out.Vertices.clear();
		out.Indices.clear();
		out.Submeshes.clear();

		std::vector<glm::vec3> rim;

		auto side = [&](const glm::vec3& p)
		{ return keepBelow ? (p[axis] <= cut) : (p[axis] >= cut); };

		auto cross = [&](const glm::vec3& a, const glm::vec3& b)
		{
			float t = (cut - a[axis]) / (b[axis] - a[axis]);
			return a + (b - a) * glm::clamp(t, 0.0f, 1.0f);
		};

		auto emit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			glm::vec3 n = glm::cross(b - a, c - a);
			if (glm::length(n) < 1e-10f)
				return;

			n = glm::normalize(n);

			unsigned int at = (unsigned int)out.Vertices.size();
			out.Vertices.push_back({ a, n, { 0.0f, 0.0f } });
			out.Vertices.push_back({ b, n, { 1.0f, 0.0f } });
			out.Vertices.push_back({ c, n, { 0.5f, 1.0f } });
			out.Indices.push_back(at);
			out.Indices.push_back(at + 1);
			out.Indices.push_back(at + 2);
		};

		for (size_t t = 0; t < in.Indices.size(); t += 3)
		{
			glm::vec3 p[3] = {
				in.Vertices[in.Indices[t + 0]].Position,
				in.Vertices[in.Indices[t + 1]].Position,
				in.Vertices[in.Indices[t + 2]].Position
			};

			bool keep[3] = { side(p[0]), side(p[1]), side(p[2]) };
			int kept = (keep[0] ? 1 : 0) + (keep[1] ? 1 : 0) + (keep[2] ? 1 : 0);

			if (kept == 0)
				continue;

			if (kept == 3)
			{
				emit(p[0], p[1], p[2]);
				continue;
			}

			// One or two corners survive; the triangle becomes a triangle or a
			// quad, and either way it contributes one segment to the rim.
			if (kept == 1)
			{
				int i = keep[0] ? 0 : (keep[1] ? 1 : 2);
				glm::vec3 a = cross(p[i], p[(i + 1) % 3]);
				glm::vec3 b = cross(p[i], p[(i + 2) % 3]);

				emit(p[i], a, b);
				rim.push_back(a);
				rim.push_back(b);
			}
			else
			{
				int i = !keep[0] ? 0 : (!keep[1] ? 1 : 2);
				glm::vec3 a = cross(p[i], p[(i + 1) % 3]);
				glm::vec3 b = cross(p[i], p[(i + 2) % 3]);

				emit(a, p[(i + 1) % 3], p[(i + 2) % 3]);
				emit(a, p[(i + 2) % 3], b);
				rim.push_back(a);
				rim.push_back(b);
			}
		}

		if (rim.empty())
			return;

		// Cap: fan from the centroid of the rim, wound so the face points away
		// from the half that was kept.
		glm::vec3 centre(0.0f);
		for (const glm::vec3& r : rim)
			centre += r;
		centre /= (float)rim.size();

		glm::vec3 outward(0.0f);
		outward[axis] = keepBelow ? 1.0f : -1.0f;

		for (size_t i = 0; i + 1 < rim.size(); i += 2)
		{
			glm::vec3 a = rim[i], b = rim[i + 1];

			// The rim arrives as unordered segments, so each is capped as its
			// own triangle back to the centre rather than sorted into a loop.
			glm::vec3 n = glm::cross(b - a, centre - a);
			if (glm::dot(n, outward) < 0.0f)
				std::swap(a, b);

			emit(a, b, centre);
		}
	}

	// Grey rocks, scattered on the spawn island and left to the solver.
	//
	// **Boxes, not spheres.** The first version used sphere colliders with a
	// coarse sphere mesh, which matched perfectly and behaved terribly: a
	// sphere on a slope rolls, this island is a dome, and every rock rolled
	// down the beach, into the sea, down the seabed, and eventually far enough
	// inside the field that the narrowphase stopped pushing it out and it fell
	// forever. Measured: a rock landed correctly at Height + radius, then crept
	// 3.69 -> 3.58 -> 3.10 and was at -18.8 m doing -17.6 m/s by step 599.
	//
	// Nothing was wrong with the collision. Real rocks do not roll away because
	// real rocks are not spheres, and a rigid-body solver has no rolling
	// resistance to stand in for that. A box rests on a face.
	//
	// The mesh is a **jittered sphere inscribed in the box**, not the box
	// itself. Drawing the collider is the honest thing and it looked like a
	// crate; a rock has to look like a rock. The mesh never leaves the box --
	// its radius is at most 1 in the box's own units -- so it can only ever be
	// *inside* what it collides with, which is the direction that reads as a
	// rock half-buried rather than as one floating.
	void SpawnRocks(const Island& island)
	{

		// Kept inside the radius the attach-time stream already filled. A rock
		// dropped over a chunk that does not exist yet has nothing to land on,
		// falls, and then has to be ejected once the ground appears under it.
		float scatter = glm::min(island.Radius * 0.8f, s_ChunkWorld * 2.2f);

		for (int i = 0; i < s_RockCount; i++)
		{
			float angle = Hash2DUnit(i, 11, 31u) * 6.2831853f;
			float distance = std::sqrt(Hash2DUnit(i, 12, 31u)) * scatter;

			// sqrt on the radius, or every rock bunches at the centre: the
			// area of a ring grows with r, so a uniform radius does not give a
			// uniform scatter.
			glm::vec2 at = island.Centre + glm::vec2(std::cos(angle), std::sin(angle)) * distance;

			// Squared, so the scatter is mostly small rocks with a few big
			// ones rather than an even spread of sizes. A uniform draw put
			// only one of sixteen under the carry limit, which is a pickup
			// mechanic you would almost never get to use -- and an even spread
			// of boulder sizes is not what a beach looks like either.
			float t = Hash2DUnit(i, 13, 31u);
			float radius = s_RockMinRadius + t * t * (s_RockMaxRadius - s_RockMinRadius);

			// Squashed a little differently on each axis, so sixteen rocks are
			// not sixteen cubes. Kept modest: a very flat box on a slope slides
			// rather than resting, which is the same problem in another shape.
			glm::vec3 half(
				radius * (0.75f + Hash2DUnit(i, 14, 31u) * 0.5f),
				radius * (0.60f + Hash2DUnit(i, 15, 31u) * 0.4f),
				radius * (0.75f + Hash2DUnit(i, 16, 31u) * 0.5f));

			// Dropped from just clear of the surface so it settles onto the
			// ground rather than starting interpenetrating it.
			float y = Height(at.x, at.y) + half.y + 0.30f;

			// Mass from volume, so the big ones behave like big ones: a 0.4 m
			// rock lands near 20 kg and a 1.1 m one near 400.
			float mass = 2000.0f * half.x * half.y * half.z;

			Egss::RigidBody3D rock = Egss::RigidBody3D::MakeBox({ at.x, y, at.y }, half, mass);

			// Turned about the vertical only. A box tipped onto a corner has to
			// fall over before it rests, which is a second of every rock
			// wobbling at startup for no gain.
			rock.Orientation = glm::angleAxis(
				Hash2DUnit(i, 17, 31u) * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
			rock.UpdateInertiaWorld();

			rock.Friction = 0.9f;
			rock.Restitution = 0.0f;
			rock.LinearDamping = 0.2f;
			rock.AngularDamping = 0.4f;

			Rock made;
			made.Handle = m_World.AddBody(rock);
			made.HalfExtents = half;
			made.Shape = std::make_shared<Egss::MeshData>(
				MakeRockMesh(101u + (unsigned int)(i % s_RockShapes) * 37u));
			made.MeshPtr = std::make_shared<Egss::Mesh>(*made.Shape, "Rock");
			made.Health = HealthFor(half);

			m_Rocks.push_back(made);
		}
	}

	// Scattered where the grass is: high enough up the island and flat enough
	// underfoot, which is the same gate the shader uses to decide the ground is
	// green. A tree on the beach or on a dune face would be the giveaway.
	void SpawnTrees(const Island& island)
	{
		for (int i = 0; i < s_TreeShapes; i++)
		{
			Egss::MeshData bark, leaves;
			MakeTreeMesh(313u + (unsigned int)i * 101u, TreeParams(), bark, leaves);

			m_TreeMeshes[i].reset(new Egss::Mesh(bark, "Tree"));

			if (!leaves.Indices.empty())
				m_LeafMeshes[i].reset(new Egss::Mesh(leaves, "TreeLeaves"));
		}

		float scatter = glm::min(island.Radius * 0.7f, s_ChunkWorld * 2.0f);

		for (int i = 0; i < s_TreeAttempts; i++)
		{
			float angle = Hash2DUnit(i, 21, 53u) * 6.2831853f;
			float distance = std::sqrt(Hash2DUnit(i, 22, 53u)) * scatter;

			glm::vec2 at = island.Centre + glm::vec2(std::cos(angle), std::sin(angle)) * distance;

			float ground = Height(at.x, at.y);
			if (ground < m_GrassLow)
				continue;

			// Slope from the same analytic gradient the field uses, rather than
			// from sampling the mesh -- it is exact and it is already there.
			glm::vec2 slope = Slope(at.x, at.y);
			if (glm::length(slope) > s_TreeMaxSlope)
				continue;

			Tree tree;
			tree.Position = { at.x, ground - 0.2f, at.y };   // set slightly in
			tree.Yaw = Hash2DUnit(i, 23, 53u) * 360.0f;
			tree.Scale = 0.8f + Hash2DUnit(i, 24, 53u) * 0.6f;
			tree.Shape = i % s_TreeShapes;

			// Covers the trunk and the first fork rather than the whole crown:
			// a capsule around the outermost twigs would stop the player a
			// couple of metres from the tree.
			tree.HalfHeight = s_TreeTrunkSpan * tree.Scale;
			tree.Hits = s_TreeHits;

			// **A box, not a capsule.** The first version used a capsule, which
			// is the better fit for a trunk and behaved exactly like the
			// spherical rocks did: once felled it rolled, 6.71 m and still
			// moving after fifteen seconds, because a round collider on a
			// slope has no rolling resistance for the solver to spend. A
			// square trunk is invisible at this scale and lies where it falls.
			Egss::RigidBody3D trunk = Egss::RigidBody3D::MakeStaticBox(
				tree.Position + glm::vec3(0.0f, tree.HalfHeight, 0.0f),
				glm::vec3(s_TreeRadius * tree.Scale, tree.HalfHeight, s_TreeRadius * tree.Scale));

			// Carried on the body, so a felled trunk keeps the facing it grew
			// with instead of snapping upright as it topples.
			trunk.Orientation = glm::angleAxis(glm::radians(tree.Yaw), glm::vec3(0.0f, 1.0f, 0.0f));
			trunk.Friction = 0.8f;
			trunk.Restitution = 0.0f;
			trunk.LinearDamping = 0.1f;
			trunk.AngularDamping = 0.25f;

			tree.Body = m_World.AddBody(trunk);

			m_Trees.push_back(tree);

			if ((int)m_Trees.size() >= s_TreeCount)
				break;
		}
	}

	void DrawTrees()
	{
		if (m_Trees.empty() || !m_TreeMeshes[0])
			return;

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);
		m_Material->Set("u_Color", m_BarkColour);

		for (const Tree& tree : m_Trees)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(tree.Body);

			// Drawn from the body in both states, so felling changes nothing
			// about how a tree is rendered -- only what moves it. The mesh
			// grows from its base, which sits one half-height below the
			// capsule's centre.
			glm::mat4 transform =
				glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -tree.HalfHeight, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(tree.Scale));

			Egss::Renderer::Submit(m_Material, m_TreeMeshes[tree.Shape], transform);
		}

		if (!m_Leaves)
			return;

		m_Material->Set("u_Color", m_LeafColour);

		for (const Tree& tree : m_Trees)
		{
			if (!m_LeafMeshes[tree.Shape])
				continue;

			const Egss::RigidBody3D& body = m_World.GetBody(tree.Body);

			glm::mat4 transform =
				glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				* glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -tree.HalfHeight, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(tree.Scale));

			Egss::Renderer::Submit(m_Material, m_LeafMeshes[tree.Shape], transform);
		}
	}

	void SpawnWalker(const glm::vec3& near)
	{
		float ground = 0.0f;
		glm::vec3 normal(0.0f);
		m_World.GroundBelow(near, ground, normal);

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(
			{ near.x, ground + s_EyeHeight + 1.0f, near.z }, s_WalkerRadius,
			s_WalkerHalfHeight, 75.0f);

		body.Friction = 0.4f;
		body.Restitution = 0.0f;
		body.LinearDamping = 0.05f;

		m_Walker = m_World.AddBody(body);

		// The camera's focus starts *on* the player. Left at its default it
		// starts at the world origin, and with third person on from the first
		// frame nothing ever syncs it -- the camera opens somewhere out at sea
		// and creeps toward the island.
		m_CameraFocus = HeadPosition();
	}

	// --- Draw ---------------------------------------------------------------

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		(void)ts;

		// **Set the state rather than inherit it.** Cull mode is global and
		// outlives whichever demo last touched it -- a demo that assumes the
		// default is at the mercy of the one selected before it, which is
		// exactly how this demo's water came to be culled from below.
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		// Under the surface the sky is not the sky. Clearing to the water
		// colour is most of what makes being submerged *look* like being
		// submerged; without it, swimming renders identically to standing on
		// dry sand and the only blue left is the distant sea seen edge-on.
		m_Underwater = m_Camera.GetPosition().y < s_SeaLevel;

		Egss::RenderCommand::SetClearColor(m_Underwater
			? glm::vec4(m_Deep, 1.0f)
			: glm::vec4(0.53f, 0.68f, 0.79f, 1.0f));
		Egss::RenderCommand::Clear();

		Egss::Renderer::ResetStats();

		Egss::Frustum frustum = Egss::Frustum::FromViewProjection(m_Camera.GetViewProjectionMatrix());

		Egss::Renderer::BeginScene(m_Camera);

		m_Material->Set("u_SunDirection", glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)));
		m_Material->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_Material->Set("u_SkyColor", glm::vec3(0.5f, 0.6f, 0.75f));
		m_Material->Set("u_Ambient", 0.35f);
		m_Material->Set("u_Color", m_SandColour);
		m_Material->Set("u_Textured", m_Textured ? 1 : 0);
		m_Material->Set("u_Bands", m_Bands);
		m_Material->Set("u_CameraPosition", m_Camera.GetPosition());
		m_Material->Set("u_Underwater", m_Underwater ? 1 : 0);
		m_Material->Set("u_Deep", m_Deep);
		m_Material->Set("u_FogDensity", m_FogDensity);
		m_Material->Set("u_Grass", m_GrassColour);
		m_Material->Set("u_GrassLow", m_GrassLow);
		m_Material->Set("u_GrassHigh", m_GrassHigh);
		m_Material->Set("u_Terrain", 1);
		m_Material->Set("u_Waves", 0);
		m_Material->Set("u_Time", m_WaveTime);

		for (int i = 0; i < s_WaveCount; i++)
		{
			const Wave& w = Waves()[i];
			std::string slot = "u_WaveA[" + std::to_string(i) + "]";

			m_Material->Set(slot, glm::vec4(w.Direction.x, w.Direction.y, w.Amplitude, w.Wavelength));
			m_Material->Set("u_WaveSpeed[" + std::to_string(i) + "]", w.Speed);
		}
		m_Material->Set("u_Quantise", m_Cel ? 1.0f : 0.0f);

		// Opt-in and blocking -- see RendererAPI::EndGpuTimerMs. Only around
		// the terrain pass, since that is the one whose cost the texture
		// toggle actually changes.
		if (m_MeasureGpu)
			Egss::RenderCommand::BeginGpuTimer();

		m_ChunksDrawn = 0;
		for (const auto& [key, entry] : m_Chunks)
		{
			if (m_Culling && !frustum.Intersects(entry.Bounds))
				continue;

			Egss::Renderer::Submit(m_Material, entry.MeshPtr, glm::mat4(1.0f));
			m_ChunksDrawn++;
		}

		if (m_MeasureGpu)
			m_LastGpuMs = Egss::RenderCommand::EndGpuTimerMs();

		// Its own colour, a shade off the ground's, so the blades read against
		// what they are standing in rather than disappearing into it.
		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);
		m_Material->Set("u_Color", m_BladeColour);

		m_GrassDrawn = 0;
		for (const auto& [key, entry] : m_Chunks)
		{
			if (!entry.GrassPtr)
				continue;
			if (m_Culling && !frustum.Intersects(entry.Bounds))
				continue;

			Egss::Renderer::Submit(m_Material, entry.GrassPtr, glm::mat4(1.0f));
			m_GrassDrawn++;
		}

		// Before the water, so a rock sitting in the shallows gets the water
		// blended over it rather than punched through it.
		DrawRocks();
		DrawTrees();
		DrawTools();
		DrawBody();

		// Water: tested against the depth already in the buffer (so terrain
		// above sea level still occludes it) but not written to it, so it
		// does not wrongly reject whatever renders behind it -- there is
		// nothing else transparent here yet, but the ocean is one surface
		// and does not need to sort against itself.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Waves", 1);
		m_Material->Set("u_Color", m_WaterColour);
		m_Material->Set("u_Textured", 0);
		Egss::Renderer::Submit(m_Material, m_Water, glm::mat4(1.0f));

		m_Material->Set("u_Waves", 0);

		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);

		Egss::Renderer::EndScene();

		m_Stats = Egss::Renderer::GetStats();
	}

	// Identifies the world the cache belongs to, by **sampling the terrain
	// function** rather than by a version number somebody has to remember to
	// bump. Change the islands, the noise, the sea level or the voxel size and
	// at least one of these samples moves, the fingerprint changes, and the
	// stale file is discarded instead of quietly serving the old world.
	//
	// 512 samples is about a tenth of one chunk's worth of evaluation, paid
	// once at attach.
	unsigned long long FingerprintDensity() const
	{
		unsigned long long hash = 1469598103934665603ull;   // FNV-1a, 64-bit

		auto mix = [&hash](const void* data, size_t n)
		{
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < n; i++)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};

		// The lattice geometry matters too: the same density function sampled
		// at a different voxel size is a different set of stored chunks.
		const int dims[3] = { s_SideX, s_SideY, s_SideZ };
		mix(dims, sizeof(dims));
		mix(&s_Voxel, sizeof(s_Voxel));

		// Fixed, arbitrary, and spread across the whole field: fixed so two
		// runs of one build agree, spread so a change anywhere is likely to
		// move at least one sample.
		for (int i = 0; i < 512; i++)
		{
			float t = (float)i;
			glm::vec3 at(
				std::fmod(t * 37.0f, 380.0f) - 190.0f,
				std::fmod(t * 11.0f, 90.0f),
				std::fmod(t * 53.0f, 380.0f) - 190.0f);

			float d = Density(at);
			mix(&d, sizeof(d));
		}

		return hash;
	}

	void DrawRocks()
	{

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);
		m_Material->Set("u_Color", m_RockColour);

		for (const Rock& rock : m_Rocks)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(rock.Handle);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				// The mesh spans -1..1, so the half-extents scale it directly.
				* glm::scale(glm::mat4(1.0f), rock.HalfExtents);

			if (rock.MeshPtr)
				Egss::Renderer::Submit(m_Material, rock.MeshPtr, transform);
		}
	}

	// --- Water --------------------------------------------------------------

	// Past a certain distance the sea simply carries you back. A wall would do
	// the job and would announce that the world stops here; a current that
	// strengthens the further out you get says the same thing without a seam
	// to bump into, and it is three lines.
	void ApplyUndertow()
	{
		Egss::RigidBody3D& body = m_World.GetBody(m_Walker);

		glm::vec2 flat(body.Position.x, body.Position.z);
		float distance = glm::length(flat);

		if (distance <= s_SwimLimit || distance < 1e-3f)
		{
			m_Undertow = 0.0f;
			return;
		}

		// Ramped, so crossing the line is a drift and being well past it is
		// hopeless.
		float over = distance - s_SwimLimit;
		m_Undertow = glm::min(s_UndertowSpeed, 0.5f + over * 0.25f);

		glm::vec2 inward = -flat / distance;

		body.Velocity.x = inward.x * m_Undertow;
		body.Velocity.z = inward.y * m_Undertow;
		body.Awake = true;
	}

	// --- Waves --------------------------------------------------------------
	//
	// Three travelling sines summed. Not an ocean simulation and not trying to
	// be: a spectrum done properly is an FFT per frame, and what this needs is
	// a surface that moves, that the light catches, and that the player can
	// float on.
	//
	// The reason it is worth having *this* way is that the height is an
	// **analytic function of position and time**. The vertex shader displaces
	// the mesh with it and the CPU evaluates the same thing for buoyancy, so
	// the player bobs on the surface being drawn rather than on a flat plane
	// underneath it.
	//
	// **It is written twice**, once in GLSL and once here, and nothing enforces
	// that the two agree. That is the real cost of the approach and it is worth
	// stating plainly; the check that keeps them honest is that the player
	// floats at a constant height above the *wave*, which fails visibly if the
	// two drift apart.
	struct Wave
	{
		glm::vec2 Direction;
		float Amplitude;
		float Wavelength;
		float Speed;
	};

	static const Wave* Waves()
	{
		// Directions are unit vectors; wavelengths are long enough that the
		// water grid resolves them. Deliberately not harmonics of each other,
		// or the sum repeats visibly.
		static const Wave waves[s_WaveCount] = {
			{ {  0.86f,  0.51f }, 0.42f, 37.0f, 4.1f },
			{ { -0.42f,  0.91f }, 0.26f, 23.0f, 3.2f },
			{ {  0.61f, -0.79f }, 0.13f, 13.0f, 2.4f },
		};
		return waves;
	}

	static float WaveHeight(float x, float z, float time)
	{
		float h = 0.0f;

		for (int i = 0; i < s_WaveCount; i++)
		{
			const Wave& w = Waves()[i];
			float k = 6.2831853f / w.Wavelength;

			h += w.Amplitude * std::sin((w.Direction.x * x + w.Direction.y * z) * k
				+ time * w.Speed);
		}

		return s_SeaLevel + h;
	}

	// The largest the sum can possibly be, which is every wave cresting at the
	// same point. Used to bound what the surface can do rather than measuring
	// it and hoping.
	static float WaveAmplitudeBound()
	{
		float total = 0.0f;
		for (int i = 0; i < s_WaveCount; i++)
			total += Waves()[i].Amplitude;

		return total;
	}

	// A grid rather than a quad, because a quad has four vertices and waves
	// have to be displaced somewhere. 5 m cells against a 13 m shortest
	// wavelength -- coarse, and about the least that still reads as a wave
	// rather than as a fold.
	void BuildWater()
	{
		float half = 0.5f * (s_SideX - 1) * s_Voxel;
		float span = half * 2.0f;

		Egss::MeshData data;

		const int n = s_WaterSegments;

		for (int j = 0; j <= n; j++)
		{
			for (int i = 0; i <= n; i++)
			{
				float u = (float)i / (float)n;
				float v = (float)j / (float)n;

				data.Vertices.push_back({
					{ -half + u * span, s_SeaLevel, -half + v * span },
					{ 0.0f, 1.0f, 0.0f },
					{ u * 8.0f, v * 8.0f } });
			}
		}

		for (int j = 0; j < n; j++)
		{
			for (int i = 0; i < n; i++)
			{
				unsigned int a = (unsigned int)(j * (n + 1) + i);
				unsigned int b = a + 1;
				unsigned int c = a + (unsigned int)(n + 1);
				unsigned int d = c + 1;

				data.Indices.push_back(a);
				data.Indices.push_back(c);
				data.Indices.push_back(d);
				data.Indices.push_back(a);
				data.Indices.push_back(d);
				data.Indices.push_back(b);
			}
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();

		m_Water = std::make_shared<Egss::Mesh>(data, "Water");
	}

	// --- Shader ---------------------------------------------------------

	void BuildShader()
	{
		// A sun, not a point light -- the map is 400 m across, and a point
		// light's falloff leaves almost all of it lit by ambient alone. See
		// the same note in VoxelTerrain and the HANDOVER entry it cites.
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Normal;
			out vec2 v_TexCoord;
			out vec3 v_WorldPosition;

			// Three travelling sines, the same three the CPU evaluates for
			// buoyancy. Duplicated by hand; see the note in OpenWorld.h.
			uniform int u_Waves;          // 0 draws the mesh flat
			uniform float u_Time;
			uniform vec4 u_WaveA[3];      // dir.xy, amplitude, wavelength
			uniform float u_WaveSpeed[3];

			float WaveAt(vec2 p, out vec2 slope)
			{
				float h = 0.0;
				slope = vec2(0.0);

				for (int i = 0; i < 3; i++)
				{
					vec2 dir = u_WaveA[i].xy;
					float amplitude = u_WaveA[i].z;
					float k = 6.2831853 / u_WaveA[i].w;

					float phase = dot(dir, p) * k + u_Time * u_WaveSpeed[i];

					h += amplitude * sin(phase);

					// The derivative of the same sum, so the normal is exact
					// rather than differenced from neighbouring vertices.
					slope += dir * (amplitude * k * cos(phase));
				}

				return h;
			}

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);
				vec3 normal = mat3(u_Transform) * a_Normal;

				if (u_Waves == 1)
				{
					vec2 slope;
					world.y += WaveAt(world.xz, slope);

					// A height field's normal is (-dh/dx, 1, -dh/dz).
					normal = normalize(vec3(-slope.x, 1.0, -slope.y));
				}

				v_WorldPosition = world.xyz;
				v_Normal = normal;
				// World-space planar, from MarchingCubes -- see the comment
				// where it is generated. Spatially varying rather than
				// per-vertex-fixed is what makes a texture-cost comparison
				// honest: constant UVs would sample the same texel over
				// and over, which the cache makes artificially cheap.
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_Normal;
			in vec2 v_TexCoord;
			in vec3 v_WorldPosition;

			uniform vec3 u_CameraPosition;
			uniform int u_Underwater;
			uniform vec3 u_Deep;
			uniform float u_FogDensity;

			// Terrain gets the sand/grass blend; water and rocks do not, which
			// is what this gate is for -- without it the sea would sprout grass
			// wherever the quad happened to sit above the line.
			uniform int u_Terrain;
			uniform vec3 u_Grass;
			uniform float u_GrassLow;
			uniform float u_GrassHigh;

			uniform vec4 u_Color;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunColor;
			uniform vec3 u_SkyColor;
			uniform float u_Ambient;
			uniform int u_Textured;
			uniform sampler2D u_BaseColourMap;

			uniform int u_Bands;
			uniform float u_Quantise;

			// The Cel demo's quantiser, unchanged: floor to a level, clamp the
			// point facing the light into the top band, then divide by
			// **bands - 1** so the levels span 0..1 inclusive. Dividing by
			// bands instead caps the brightest band at (bands-1)/bands and
			// lays a haze over everything.
			float Quantise(float x)
			{
				float steps = float(max(u_Bands, 2));
				float level = min(floor(x * steps), steps - 1.0);
				return mix(x, level / (steps - 1.0), u_Quantise);
			}

			void main()
			{
				vec4 base = u_Color;
				if (u_Textured == 1)
					base *= texture(u_BaseColourMap, v_TexCoord);

				vec3 n = normalize(v_Normal);

				if (u_Terrain == 1)
				{
					// Height decides where grass starts; slope decides whether
					// it can hold on. Grass on a near-vertical face looks
					// painted on, and the dunes are steep enough at their edges
					// for that to show.
					// `flatness`, not `flat` -- `flat` is a GLSL interpolation
					// qualifier, and using it as a variable is a syntax error
					// that takes the whole shader out. The engine logs the
					// failure and carries on with an unusable program, which
					// renders white; the log said so immediately and the
					// picture did not.
					float high = smoothstep(u_GrassLow, u_GrassHigh, v_WorldPosition.y);
					float flatness = smoothstep(0.55, 0.88, n.y);

					base.rgb = mix(base.rgb, u_Grass, high * flatness);
				}

				// Both terms are banded, not only the sun. Banding just the
				// sun leaves the sky gradient sliding smoothly underneath the
				// hard sun edges, which reads as a bug rather than as a style
				// -- the flat regions have to agree with each other.
				float sun = Quantise(max(dot(n, -u_SunDirection), 0.0));
				float sky = Quantise(0.5 + 0.5 * n.y);

				vec3 lit = base.rgb * (u_Ambient + sun * u_SunColor + sky * u_SkyColor * 0.35);

				// Beer-Lambert, the same exponential a real attenuating medium
				// follows: what survives over distance d is exp(-density*d), so
				// what the water has replaced is one minus that. Applied to the
				// terrain and to the water surface alike, so the surface seen
				// from below fades into the distance with everything else.
				if (u_Underwater == 1)
				{
					float d = length(v_WorldPosition - u_CameraPosition);
					lit = mix(lit, u_Deep, 1.0 - exp(-u_FogDensity * d));
				}

				color = vec4(lit, base.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("OpenWorldSun", vertexSrc, fragmentSrc));
		m_Material = Egss::Material::Create(m_Shader);

		m_GroundTexture.reset(Egss::Texture2D::Create("assets/models/checker.png"));
		m_Material->SetTexture("u_BaseColourMap", m_GroundTexture);
	}

	// --- Panel ------------------------------------------------------------

	void OnDemoImGui() override
	{
		ImGui::Begin("Open World");

		ImGui::Checkbox("First person", &m_FirstPerson);
		if (m_FirstPerson)
		{
			ImGui::Text("%s", FirstPersonController::MotionName(m_Controller.GetMotion()));
			ImGui::TextDisabled("Space swims up / jumps, Shift dives");
			if (m_Undertow > 0.0f)
				ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
					"the current is carrying you back (%.1f m/s)", m_Undertow);
			ImGui::TextDisabled(m_Held >= 0 ? "E to drop the rock" : "E to pick up a small rock");
			ImGui::TextDisabled("Holding: %s", m_Held >= 0 ? "a rock" : ToolName(HeldTool()));
			ImGui::TextDisabled("Pickaxe breaks rock, axe fells trees, shovel digs");
			ImGui::TextDisabled("%zu rocks, %zu trees | %d struck, %d felled, %d dug",
				m_Rocks.size(), m_Trees.size(), m_Struck, m_Felled, m_Dug);
		}

		m_Controller.MouseLookHelp();

		ImGui::Separator();
		ImGui::SliderFloat("Load radius", &m_LoadRadius, 16.0f, 160.0f);
		ImGui::SliderInt("Chunks per step", &m_ChunksPerStep, 1, 8);
		ImGui::Checkbox("Frustum culling", &m_Culling);

		ImGui::Separator();
		ImGui::Checkbox("Chunk LOD", &m_Lod);
		ImGui::SliderFloat("Stride 2 beyond", &m_LodNear, 16.0f, 200.0f, "%.0f m");
		ImGui::SliderFloat("Stride 4 beyond", &m_LodFar, 32.0f, 300.0f, "%.0f m");
		ImGui::SliderFloat("Hysteresis", &m_LodHysteresis, 0.0f, 32.0f, "%.0f m");
		ImGui::SliderInt("LOD remeshes per step", &m_LodPerStep, 1, 8);

		{
			int perStride[3] = { 0, 0, 0 };
			unsigned int trianglesPerStride[3] = { 0, 0, 0 };
			for (const auto& [key, entry] : m_Chunks)
			{
				int slot = entry.Stride == 1 ? 0 : (entry.Stride == 2 ? 1 : 2);
				perStride[slot]++;
				trianglesPerStride[slot] += entry.MeshPtr->GetTriangleCount();
			}

			ImGui::Text("stride 1: %d chunks, %u tris", perStride[0], trianglesPerStride[0]);
			ImGui::Text("stride 2: %d chunks, %u tris", perStride[1], trianglesPerStride[1]);
			ImGui::Text("stride 4: %d chunks, %u tris", perStride[2], trianglesPerStride[2]);
			ImGui::Text("%d remeshed for LOD last step", m_LodRemeshes);
		}

		ImGui::Separator();
		ImGui::Text("%zu / %zu chunks filled", m_Filled.size(), (size_t)
			m_Field->ChunkCount().x * m_Field->ChunkCount().y * m_Field->ChunkCount().z);
		ImGui::Text("%zu chunks meshed, %d drawn this frame", m_Chunks.size(), m_ChunksDrawn);
		ImGui::Text("%u draw calls, %u triangles", m_Stats.DrawCalls, m_Stats.TriangleCount);

		ImGui::Separator();
		ImGui::Checkbox("Chunk cache", &m_UseCache);
		ImGui::Text("%zu stored, %u hits, %u generated (%.1f MB on disk)",
			m_Cache.Entries(), m_Cache.Hits(), m_Cache.Written(),
			m_Cache.BytesOnDisk() / (1024.0 * 1024.0));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Beside the executable, keyed by a fingerprint of the\n"
				"density function -- change the terrain and it rebuilds itself.");

		ImGui::Separator();
		ImGui::ColorEdit3("Sand", &m_SandColour.x);
		ImGui::ColorEdit3("Grass", &m_GrassColour.x);
		ImGui::ColorEdit3("Bark", &m_BarkColour.x);
		ImGui::Checkbox("Show body", &m_ShowBody);
		ImGui::SameLine();
		ImGui::Checkbox("Third person", &m_ThirdPerson);
		ImGui::Checkbox("Leaves", &m_Leaves);
		ImGui::SameLine();
		ImGui::ColorEdit3("Leaf", &m_LeafColour.x);
		ImGui::SliderFloat("Grass from", &m_GrassLow, 0.0f, 4.0f, "%.1f m");
		ImGui::SliderFloat("Grass by", &m_GrassHigh, 0.0f, 6.0f, "%.1f m");
		ImGui::Checkbox("Grass blades", &m_Grass);
		ImGui::ColorEdit3("Blade", &m_BladeColour.x);
		ImGui::SliderFloat("Blades per triangle", &m_GrassDensity, 0.0f, 2.0f);
		ImGui::SliderFloat("Blade height", &m_GrassHeight, 0.1f, 1.0f, "%.2f m");
		ImGui::TextDisabled("%d chunks of grass drawn (stride 1 only)", m_GrassDrawn);
		ImGui::ColorEdit4("Water", &m_WaterColour.x);
		ImGui::ColorEdit3("Underwater", &m_Deep.x);
		ImGui::SliderFloat("Underwater fog", &m_FogDensity, 0.0f, 0.25f, "%.3f /m");
		ImGui::Checkbox("Cel shading", &m_Cel);
		ImGui::SliderInt("Bands", &m_Bands, 2, 8);
		ImGui::Checkbox("Textured ground", &m_Textured);
		ImGui::Checkbox("Measure terrain GPU time", &m_MeasureGpu);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Blocks the CPU on the GPU's result every frame --\n"
				"a diagnostic for A/B comparison, not something to leave on.");
		if (m_MeasureGpu)
			ImGui::Text("terrain pass: %.3f ms GPU", m_LastGpuMs);

		ImGui::End();
	}

	// --- State ------------------------------------------------------------

	static constexpr float s_Voxel = 0.5f;
	static constexpr int s_SideX = 800;
	static constexpr int s_SideY = 200;
	static constexpr int s_SideZ = 800;
	static constexpr float s_OriginY = -25.0f;
	// Carrying. The size gate is on the collider's half-extents rather than a
	// separate "small" flag, so it cannot disagree with the rock it describes.
	static constexpr float s_PickupMaxHalf = 0.50f;
	static constexpr float s_PickupReach = 3.5f;
	static constexpr float s_PickupAlignment = 0.86f;   // ~30 degrees off centre
	// The hand, and the body hung off the camera. Borrowed from the Ragdoll
	// demo's proportions: a 1.7 m figure whose eyes are 0.75 m above the
	// capsule's centre.
	static constexpr float s_HandForward = 0.62f;
	static constexpr float s_HandSide = 0.26f;
	static constexpr float s_HandDrop = 0.34f;

	static constexpr float s_HipDrop = 0.62f;
	static constexpr float s_HipWidth = 0.11f;
	static constexpr float s_ThighLength = 0.42f;
	static constexpr float s_ShinLength = 0.42f;
	static constexpr float s_ShoulderDrop = 0.12f;
	static constexpr float s_ShoulderWidth = 0.165f;
	static constexpr float s_StrideSwing = 0.55f;
	static constexpr float s_StridePerMetre = 2.6f;

	static constexpr float s_PickupTime = 0.22f;
	static constexpr float s_SwingTime = 0.40f;
	static constexpr float s_SwingReach = 0.34f;
	static constexpr float s_SwingDrop = 0.30f;

	static constexpr float s_ThirdPersonBack = 3.1f;
	static constexpr float s_ThirdPersonUp = 0.55f;

	// How far a neck turns, and how it gives up. 70 degrees is about the limit
	// of a comfortable glance; past 40 the body starts easing round to meet it.
	static constexpr float s_NeckLimit = 70.0f;
	static constexpr float s_NeckComfort = 40.0f;
	static constexpr float s_SettleRate = 70.0f;    // degrees a second
	static constexpr float s_TurnEase = 5.0f;       // fraction of the gap a second
	static constexpr float s_TurnRate = 260.0f;

	// How much the player can move before the camera notices, and how quickly
	// it closes what is left. Up is a much bigger allowance: a jump must not
	// move it, a climb must.
	static constexpr float s_FocusDeadZone = 0.35f;
	static constexpr float s_FocusLag = 0.22f;
	static constexpr float s_FocusDeadZoneUp = 0.95f;
	static constexpr float s_FocusLagUp = 0.55f;
	static constexpr float s_ThrowSpeedLimit = 9.0f;

	// Breaking. Shorter reach than a pickup: you have to be at the rock.
	static constexpr float s_StrikeReach = 2.8f;
	static constexpr float s_StrikeAlignment = 0.80f;
	static constexpr float s_ShatterKick = 1.6f;

	// Hit points a cubic metre of rock, and the impulse below which an impact
	// is just a knock. 40 N.s is roughly a 20 kg rock landing at 2 m/s.
	static constexpr float s_RockToughness = 900.0f;
	static constexpr float s_ImpactThreshold = 40.0f;

	// What a swing delivers, in the same units.
	static constexpr float s_PickaxeImpact = 260.0f;

	static constexpr float s_ToolMass = 4.0f;
	static constexpr float s_DigRadius = 1.1f;
	static constexpr float s_DigReach = 4.5f;

	// Trees: how much capsule a trunk gets, and what it takes to fell one.
	static constexpr float s_TreeTrunkSpan = 1.8f;
	static constexpr float s_TreeRadius = 0.28f;
	static constexpr int s_TreeHits = 4;
	static constexpr float s_TreeMass = 260.0f;
	static constexpr float s_FellPush = 1.1f;

	// Trees. Depth and branching are the two numbers the shape comes from, and
	// both appear in the closed forms the generator is checked against.
	static constexpr int s_TreeDepth = 4;
	static constexpr int s_TreeChildren = 3;
	static constexpr int s_TreeShapes = 3;
	static constexpr int s_TreeCount = 10;
	static constexpr int s_TreeAttempts = 120;
	static constexpr float s_TreeMaxSlope = 0.55f;

	static constexpr int s_RockCount = 16;
	static constexpr int s_RockShapes = 5;
	static constexpr float s_RockMinRadius = 0.35f;
	static constexpr float s_RockMaxRadius = 1.15f;

	// Waves. Three is enough for the sum not to read as one sine, and few
	// enough that the shader can unroll it.
	static constexpr int s_WaveCount = 3;
	static constexpr int s_WaterSegments = 80;

	// How far out the sea turns you back. Set inside the field's own half-width
	// so the boundary is water you are pushed through, not an edge you reach.
	static constexpr float s_SwimLimit = 165.0f;
	static constexpr float s_UndertowSpeed = 3.4f;

	static constexpr float s_SeaLevel = 0.0f;

	// --- Island shape ---
	//
	// Low, small and sandy rather than mountainous. Height at an island's
	// centre is Radius * s_MaskToHeight, so these numbers say "a 22-40 m
	// island rising 2.2-4.0 m above the water", with another metre or so of
	// dune noise on top. Raising s_MaskToHeight is what makes mountains; it
	// is shared with Slope, which needs the same figure for its normals.
	static constexpr float s_IslandRadiusMin = 22.0f;
	static constexpr float s_IslandRadiusMax = 40.0f;

	// **Two scales, not one.** The mask is positive inland and negative at
	// sea, so a single constant shapes the island and the sea floor together
	// -- and flattening the islands with one flattened the seabed with them.
	// At 0.10 the bottom only reached its -80 m mask floor about 80 m out, so
	// every island sat in a huge shin-deep shelf: bright sand under a thin
	// film of water, which reads as more beach rather than as sea. It is why
	// the water looked like it had gone.
	//
	// Land stays flat. Water drops away at the old rate, so the bottom is
	// 5.5 m down within 10 m of the shore and at its floor by about 15 m.
	static constexpr float s_MaskToHeight = 0.10f;
	static constexpr float s_SeabedDrop = 0.55f;
	static constexpr float s_ReliefBroad = 0.9f;
	static constexpr float s_ReliefFine = 0.35f;
	static constexpr int s_IslandCount = 5;

	// Egss::VoxelField3D::ChunkSize is a runtime constant expression too,
	// but this keeps the arithmetic in one place at the top of the file.
	static constexpr float s_ChunkWorld = 16.0f * s_Voxel;

	Egss::PerspectiveCamera m_Camera;
	FirstPersonController m_Controller;

	bool m_FirstPerson = true;
	bool m_Grounded = false;
	bool m_Culling = true;
	float m_LoadRadius = 128.0f;
	// Measured, not guessed: 3 filled + their remesh cascade cost 23-105 ms
	// of CPU time in a single fixed step on this machine's desktop CPU
	// (density evaluation and marching cubes are both CPU-side; the GPU is
	// not involved). 1 keeps the worst case closer to a 16 ms frame, at the
	// cost of the world taking three times as many steps to finish
	// populating around the player. See the changelog entry for the numbers
	// this was tuned against.
	int m_ChunksPerStep = 1;

	// --- LOD ---
	//
	// Deliberately *not* registered as replay parameters, unlike the load
	// radius above. The load radius decides which chunks get **filled**, and
	// the field is what the physics collides against, so moving it changes the
	// run. LOD only decides how finely a chunk that is already filled gets
	// **meshed**, and nothing collides with the mesh -- so it changes the
	// picture and the triangle count, and not the simulation.
	bool m_Lod = true;

	// Skirts (a wall hung off every open mesh edge) were tried and removed:
	// they close a *gap*, and the LOD seam is not one -- a coarse chunk meshes
	// systematically lower than its fine neighbour, so the result was a solid
	// step whose wall you could see, moving the picture by 2 pixels for 23%
	// more triangles even with the bands forced together. Superseded by
	// VoxelTransition, which reconciles the two lattices instead of hanging a
	// curtain off one of them -- see MeshChunk.
	float m_LodNear = 56.0f;    // beyond this, stride 2
	float m_LodFar = 104.0f;     // beyond this, stride 4
	float m_LodHysteresis = 8.0f;
	int m_LodPerStep = 2;
	int m_LodRemeshes = 0;

	std::vector<Island> m_Islands;

	std::shared_ptr<Egss::VoxelField3D> m_Field;

	struct ChunkEntry
	{
		std::shared_ptr<Egss::Mesh> MeshPtr;
		Egss::Aabb Bounds;

		// Which lattice this mesh was built on. Kept per chunk rather than
		// recomputed from the distance, because the distance is what the
		// chunk *wants* and this is what it currently *is* -- the difference
		// between the two is the whole of the LOD update, and hysteresis
		// needs both.
		int Stride = 1;
		glm::ivec3 Coord{ 0 };

		// Null on a coarse chunk, or on one with no grass-worthy ground.
		std::shared_ptr<Egss::Mesh> GrassPtr;
	};
	std::map<size_t, ChunkEntry> m_Chunks;
	std::unordered_set<size_t> m_Filled;

	ChunkCache m_Cache;
	bool m_UseCache = true;
	std::vector<unsigned char> m_ChunkBytes;   // reused, so streaming does not allocate

	std::vector<glm::ivec2> m_Ring;
	int m_RingReach = -1;
	glm::ivec2 m_RingCentre{ INT_MIN };
	size_t m_RingCursor = 0;
	int m_ChunksDrawn = 0;

	std::shared_ptr<Egss::Mesh> m_Water;

	struct Rock
	{
		Egss::PhysicsWorld3D::BodyHandle Handle;
		glm::vec3 HalfExtents;

		// Its own shape, because a rock that has been split is no longer any
		// of the shapes it started as -- it is a piece of one, with a flat face
		// where the cut went. Kept as data as well as a mesh so the next cut
		// has something to cut.
		std::shared_ptr<Egss::MeshData> Shape;
		std::shared_ptr<Egss::Mesh> MeshPtr;

		// Hit points, not a swing count. A rock breaks because something hit
		// it hard enough, whether that was a pickaxe, a fall, or another rock
		// -- so damage arrives as an *impulse* and the rock either has enough
		// left to absorb it or does not.
		float Health = 1.0f;

		// Last step's velocity, so a collision can be measured as the change
		// the solver made to it. There is no contact-impulse report to read,
		// and mass times the change in velocity is the same quantity.
		glm::vec3 PreviousVelocity{ 0.0f };
	};

	// Toughness scales with volume, which is what makes health an attribute
	// that **splits** rather than one that is re-rolled: eight octants at an
	// eighth of the volume have an eighth of the health each, and the total
	// across the pieces is exactly what the parent had. The same arithmetic
	// that conserves volume conserves this.
	static float HealthFor(const glm::vec3& half)
	{
		return s_RockToughness * 8.0f * half.x * half.y * half.z;
	}
	std::vector<Rock> m_Rocks;

	// Grey, and under the 1.525 exposure ceiling like everything else here.
	glm::vec4 m_RockColour{ 0.30f, 0.30f, 0.33f, 1.0f };

	std::vector<Tree> m_Trees;
	std::shared_ptr<Egss::Mesh> m_TreeMeshes[s_TreeShapes];
	std::shared_ptr<Egss::Mesh> m_LeafMeshes[s_TreeShapes];
	glm::vec4 m_BarkColour{ 0.31f, 0.22f, 0.14f, 1.0f };

	// A shade off the grass, so a canopy reads against the ground it is
	// standing on rather than merging with it from above.
	glm::vec4 m_LeafColour{ 0.20f, 0.38f, 0.16f, 1.0f };
	bool m_Leaves = true;

	std::vector<Tool> m_Tools;
	std::shared_ptr<Egss::Mesh> m_ToolWood[(int)ToolKind::Count];
	std::shared_ptr<Egss::Mesh> m_ToolMetal[(int)ToolKind::Count];

	int m_Held = -1;
	int m_HeldTool = -1;
	bool m_WasCarryKey = false;
	glm::vec3 m_ThrowVelocity{ 0.0f };

	glm::vec3 m_PickupFrom{ 0.0f };
	glm::quat m_PickupOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
	float m_PickupBlend = 1.0f;
	float m_Swing = 0.0f;

	bool m_WasSwinging = false;
	int m_Struck = 0;
	int m_Felled = 0;
	int m_Dug = 0;

	std::shared_ptr<Egss::Mesh> m_LimbMesh, m_TorsoMesh, m_HeadMesh;
	std::shared_ptr<Egss::Mesh> m_HandMesh, m_FootMesh, m_JointMesh;
	bool m_ThirdPerson = true;
	float m_BodyYaw = -90.0f;
	glm::vec3 m_CameraFocus{ 0.0f };
	bool m_ShowBody = true;
	float m_Stride = 0.0f;

	glm::vec4 m_SkinColour{ 0.52f, 0.36f, 0.26f, 1.0f };
	glm::vec4 m_ClothColour{ 0.20f, 0.26f, 0.34f, 1.0f };
	glm::vec4 m_BootColour{ 0.14f, 0.12f, 0.11f, 1.0f };

	float m_WaveTime = 0.0f;
	float m_Undertow = 0.0f;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;
	std::shared_ptr<Egss::Texture2D> m_GroundTexture;
	// Off by default now the terrain is meant to read as sand rather than as
	// a test surface -- the checker is a debug texture. The toggle stays,
	// because the textured-vs-untextured GPU comparison still wants it.
	bool m_Textured = false;

	// Chosen by arithmetic, not by eye. The shader's brightest possible
	// multiplier is u_Ambient + sun*sunColor + sky*skyColor*0.35, which is
	// 0.35 + 1.0 + 0.175 = 1.525 on the red channel -- so any albedo above
	// about 0.65 clips, and a clipped surface has no bands left because every
	// level saturates to the same white. A first attempt at (0.84, 0.76, 0.56)
	// measured (255, 255, 213): two channels pinned.
	//
	// At this albedo the brightest band should land on
	// (0.62*1.525, 0.56*1.52, 0.41*1.4925) = (0.945, 0.851, 0.612), or
	// (241, 217, 156).
	glm::vec4 m_SandColour{ 0.62f, 0.56f, 0.41f, 1.0f };

	// Was (0.10, 0.28, 0.42, 0.55), which blended over the sky to (82,138,178)
	// against a sky of (135,173,201) -- measurably different and still readable
	// as haze rather than as sea. Deeper and much more opaque, so the horizon
	// is a line between two clearly different things.
	glm::vec4 m_WaterColour{ 0.06f, 0.26f, 0.40f, 0.82f };

	// What everything fades toward while submerged, and how fast. 0.06 per
	// metre puts the fade at roughly half over 12 m, so the sea floor stays
	// readable underfoot while the distance closes in.
	// Grass takes over as the ground rises. The islands stand 2.2-4.0 m above
	// the sea, so the band sits in the middle of that: beach at the waterline,
	// green over the crown, and no hard line between them.
	//
	// Albedo picked under the same ceiling as the sand -- the brightest
	// multiplier is 1.525, so this peaks at (117, 174, 76) rather than
	// clipping.
	glm::vec3 m_GrassColour{ 0.30f, 0.45f, 0.20f };

	// --- Grass as geometry ---
	bool m_Grass = true;
	float m_GrassDensity = 0.6f;    // blades per qualifying terrain triangle
	float m_GrassHeight = 0.42f;
	float m_GrassWidth = 0.045f;
	int m_GrassDrawn = 0;
	glm::vec4 m_BladeColour{ 0.26f, 0.44f, 0.15f, 1.0f };
	float m_GrassLow = 1.1f;
	float m_GrassHigh = 2.6f;

	glm::vec3 m_Deep{ 0.05f, 0.20f, 0.32f };
	float m_FogDensity = 0.06f;
	bool m_Underwater = false;

	// Cel shading, the same quantiser as the Cel demo. No inverted-hull
	// outline here: a chunk mesh is an *open* surface that stops at the
	// chunk boundary, so an inflated copy would show its back faces along
	// every one of those edges -- hundreds of them -- rather than only at
	// the silhouette. Outlines want a closed mesh or a depth-discontinuity
	// pass, and the second is a different piece of work.
	bool m_Cel = true;
	int m_Bands = 4;

	bool m_MeasureGpu = false;
	double m_LastGpuMs = 0.0;

	Egss::PhysicsWorld3D m_World;
	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;

	Egss::Renderer::Statistics m_Stats;
};
