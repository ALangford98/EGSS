#pragma once

// The active-ragdoll humanoid: a jointed body that stands, balances, walks,
// runs, jumps, staggers when hit, goes limp when it can no longer catch
// itself, and gets back up. Lifted out of TestEnv/src/Ragdoll.h, where it was
// built and tuned, so that a game's Player or NPC can be one (GameSession,
// Body = Humanoid) -- in editor Play and in every exported game. The Ragdoll
// demo, and Map Building which derives from it, drive this same class.
// docs/superpowers/specs/2026-10-14-play-keep-ball-humanoid-design.md.
//
// The commentary throughout is the rig's own, from the demo: what was tried,
// what was measured, and what is still open (the reactive step does not yet
// widen the base -- see the changelog for 2026-08-09/10).
//
// The physics world is not this class's: it builds its thirteen bodies and
// twelve joints into whichever world it is given, and steps nothing --
// whoever owns the world calls Update() before each world step. What it
// wants to do comes in through m_Control (which way is forward, the movement
// keys, run) and m_WantsJump, set by the owner each step; nothing here reads
// the keyboard or knows about a camera, so a game can drive it with a
// player's input or an NPC's wish alike.
//
// Moved verbatim apart from those inputs, and checked: a recorded session
// through the demo produces bit-identical body states, step by step, before
// and after the move.

#include <GS.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

class Humanoid
{
public:
	struct BalanceState
	{
		glm::vec3 Com{ 0.0f };
		glm::vec3 ComVelocity{ 0.0f };
		glm::vec2 ComGround{ 0.0f };
		glm::vec2 CapturePoint{ 0.0f };
		glm::vec2 SupportCentre{ 0.0f };

		// The hull, in world xz, counter-clockwise.
		std::vector<glm::vec2> Support;

		// How far inside the polygon the capture point is, in metres.
		// Negative means outside, and outside means falling.
		float Margin = 0.0f;
		bool Standing = false;
	};

	// What the owner wants this step. Yaw is the direction "forward" is
	// measured from, in degrees, as the demo's camera yaw: forward is
	// (cos yaw, 0, sin yaw), right (-sin yaw, 0, cos yaw).
	struct Control
	{
		float Yaw = -90.0f;
		bool Enabled = true;
		bool Forward = false, Back = false, Left = false, Right = false;
		bool Run = false;
	};

	explicit Humanoid(GS::PhysicsWorld3D& world) : m_World(world) {}
	virtual ~Humanoid() = default;
	Humanoid(const Humanoid&) = delete;
	Humanoid& operator=(const Humanoid&) = delete;

	Control& GetControl() { return m_Control; }
	void RequestJump() { m_WantsJump = true; }

	// --- For an owner that is a game rather than the demo ----------------

	// What the rig needs of the world it lives in: a fifteen-link chain
	// solved with fewer iterations gains energy and flails (see the demo's
	// BuildScene, where these were set).
	static constexpr unsigned int VelocityIterations = 16;
	static constexpr unsigned int PositionIterations = 6;

	// Builds the rig standing on `origin`, facing `yaw` radians about +y
	// (0 faces +z, the way it is built; facing is (sin yaw, 0, cos yaw)).
	// Built facing +z, then turned as one rigid piece about the origin --
	// every joint's frame is in its bodies' own spaces, so a rigid turn
	// changes nothing between them -- and the controller re-anchored
	// where it now stands.
	void Spawn(const glm::vec3& origin, float yaw, uint32_t seed = 1)
	{
		BuildRagdoll(origin);
		SetSeed(seed);
		if (std::abs(yaw) > 1e-6f)
		{
			glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
			for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
			{
				GS::RigidBody3D& body = m_World.GetBody(handle);
				body.Position = origin + turn * (body.Position - origin);
				body.PreviousPosition = body.Position;
				body.Orientation = glm::normalize(turn * body.Orientation);
				body.PreviousOrientation = body.Orientation;
				body.UpdateInertiaWorld();
			}
			m_Facing = yaw;
			GoControlled();
		}
	}

	const std::vector<GS::PhysicsWorld3D::BodyHandle>& Bodies() const { return m_CharacterBodies; }
	GS::PhysicsWorld3D::BodyHandle PelvisBody() const { return m_Pelvis; }
	GS::PhysicsWorld3D::BodyHandle HeadBody() const { return m_Head; }
	bool IsFoot(GS::PhysicsWorld3D::BodyHandle handle) const { return handle == m_Foot[0] || handle == m_Foot[1]; }
	float FacingRadians() const { return m_Facing; }
	// Down: a ragdoll, no longer driven -- fallen, shoved too hard, or
	// dropped with nothing under it.
	bool IsDown() const { return m_Mode == Mode::Ragdoll; }
	// Hit while standing and catching it with its feet (UpdateStumble).
	bool IsStumbling() const { return m_Mode == Mode::Stumbling; }
	float TimeDown() const { return m_RagdollTimer; }
	void GetUp() { if (m_Mode == Mode::Ragdoll) BeginGetUp(); }
	// The pelvis's height above the ground it was built standing on.
	float RestPelvisHeight() const { return m_StandClearance; }

	// One fixed step of everything that decides what the body does --
	// balance, stagger, walk or get-up, and the stepping strategy while it
	// is falling. Call before the world steps.
	void Update(float dt)
	{
		m_Clock += dt;
		m_Balance = ComputeBalance();

		if (m_Mode == Mode::Controlled)
		{
			// A hard enough hit anywhere but the feet ends control. Checked
			// before the step, so the impulse judged is the one that arrived
			// during the last one.
			glm::vec3 hitFrom(0.0f);
			float hit = ImpactForce(dt, hitFrom);

			// Everything above the noise floor is a stagger now. Whether he
			// actually goes down is decided by whether his feet can catch what
			// it did to him, not by the size of the number.
			if (hit > m_StaggerThreshold)
			{
				if (StumblesPhysically())
				{
					// The root was kinematic when it was hit, so the hit went
					// nowhere -- an infinite mass takes any impulse and does
					// not move. Handed to the body now that it can move.
					BeginStumble();
					m_World.ApplyImpulseAt(m_Torso, m_HitPush * (hit * dt), m_World.GetBody(m_Torso).Position);
				}
				else
					Stagger(hit * dt, hitFrom);
			}

			if (m_Mode == Mode::Controlled)
				DriveWalk(dt);
		}
		else if (m_Mode == Mode::Stumbling)
		{
			UpdateStumble(dt);
		}
		else if (m_Mode == Mode::GettingUp)
		{
			UpdateGetUp(dt);
		}
		else
		{
			m_RagdollTimer += dt;
			UpdateBrace(dt);
		}

		if (m_Mode == Mode::Ragdoll && m_BalanceEnabled)
		{
			// Stepping decides first: while a foot is in the air the ankle
			// strategy must not fight the swing leg for the same joints.
			UpdateStepping(m_Balance, dt);
			DriveSwingLeg();
			ApplyBalance(m_Balance);
		}

	}


	// ---------------------------------------------------------------------
	// Natural variation
	// ---------------------------------------------------------------------
	//
	// Two figures built from the same numbers walk in lockstep, and one
	// standing still is a statue. People differ from each other, and no one
	// does the same thing twice. Both kinds are here:
	//
	//   * per figure, from its seed: walking speed, stride, stance width,
	//     arm swing, posture, bob and sway each a few percent off the
	//     design -- the character of the walk;
	//   * per moment, from the seed and the clock: each step lands a little
	//     differently, the stride breathes, and standing still he breathes,
	//     shifts his weight between his feet and glances about.
	//
	// All of it is a function of the seed and the fixed-step clock -- a
	// hash, never a random-number generator with state of its own -- so a
	// recording replays exactly and a figure is the same figure every run.
	// m_Variation scales it all; 0 is the design itself.

	// A seed of 0 is no variation at all; the default is 1.
	void SetSeed(uint32_t seed)
	{
		m_Seed = seed;
		if (!m_DesignStored)
		{
			m_Design = { m_WalkSpeed, m_RunSpeed, m_StrideLength, m_StanceWidth, m_ArmSwing,
				m_ElbowBend, m_LeanAmount, m_BobAmount, m_SwayAmount, m_TurnRate, m_RunFootLift };
			m_DesignStored = true;
		}
		ApplyVariation();
	}

	void ApplyVariation()
	{
		float amount = m_Seed == 0 ? 0.0f : m_Variation;
		auto vary = [&](int channel, float spread) { return 1.0f + amount * spread * Signed(m_Seed, (uint32_t)channel); };
		const Design& d = m_Design;
		m_WalkSpeed = d.WalkSpeed * vary(1, 0.08f);
		m_RunSpeed = d.RunSpeed * vary(2, 0.06f);
		m_StrideLength = d.StrideLength * vary(3, 0.06f);
		m_StanceWidth = d.StanceWidth * vary(4, 0.15f);
		m_ArmSwing = d.ArmSwing * vary(5, 0.25f);
		m_ElbowBend = d.ElbowBend * vary(6, 0.20f);
		m_LeanAmount = d.LeanAmount * vary(7, 0.30f);
		m_BobAmount = d.BobAmount * vary(8, 0.40f);
		m_SwayAmount = d.SwayAmount * vary(9, 0.30f);
		m_TurnRate = d.TurnRate * vary(10, 0.10f);
		m_RunFootLift = d.RunFootLift * vary(11, 0.12f);
		// A posture: some stand a little forward, some a little back.
		m_Posture = amount * 0.035f * Signed(m_Seed, 12u);
		// Breathing between 3.5 and 5 seconds a breath.
		m_BreathPeriod = 4.2f * (1.0f + amount * 0.18f * Signed(m_Seed, 13u));
	}

	// A hash of two numbers to [0, 1): the same pair, the same answer.
	static float Hash01(uint32_t a, uint32_t b)
	{
		uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
		h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
		return (h >> 8) * (1.0f / 16777216.0f);
	}
	static float Signed(uint32_t a, uint32_t b) { return 2.0f * Hash01(a, b) - 1.0f; }

	// Smooth noise in [-1, 1] over time: a hashed value at each whole t,
	// eased between. Channel picks an independent stream.
	float Noise(float t, uint32_t channel) const
	{
		float whole = std::floor(t);
		int32_t i = (int32_t)whole;
		float f = t - whole;
		float u = f * f * (3.0f - 2.0f * f);
		float a = Signed(m_Seed * 977u + channel, (uint32_t)i);
		float b = Signed(m_Seed * 977u + channel, (uint32_t)(i + 1));
		return a + (b - a) * u;
	}

	float VariationAmount() const { return m_Seed == 0 ? 0.0f : m_Variation; }

	// Standing: a breath in the chest, the weight drifting from one foot to
	// the other, the head glancing about. Faded out as he walks -- except
	// the glances, which stay, smaller. Returns the hips' sideways drift.
	float Idle(float dt, float stride, const glm::vec3& right)
	{
		float amount = VariationAmount();
		m_IdleWeight += ((1.0f - stride) - m_IdleWeight) * (1.0f - std::exp(-dt / 0.6f));
		float idle = m_IdleWeight * amount;

		// Breath: a degree or so of the chest rising and falling.
		m_Breath = idle * 0.018f * std::sin(glm::two_pi<float>() * m_Clock / std::max(m_BreathPeriod, 1.0f));

		// The head: holds a direction, then turns to a new one, every 2-5 s,
		// easing over about a third of a second -- glances, not a wobble.
		if (m_Clock >= m_NextGlance)
		{
			uint32_t n = m_GlanceCount++;
			m_GlanceYaw = 0.45f * Signed(m_Seed * 31u + 7u, n);
			m_GlancePitch = 0.12f * Signed(m_Seed * 31u + 8u, n) - 0.04f;
			// A tilt goes with a glance; on a head with no face it is most of
			// what shows of one.
			m_GlanceTilt = 0.12f * Signed(m_Seed * 31u + 10u, n);
			m_NextGlance = m_Clock + 2.0f + 3.0f * Hash01(m_Seed * 31u + 9u, n);
		}
		float reach = glm::mix(0.35f, 1.0f, m_IdleWeight) * amount;
		float ease = 1.0f - std::exp(-dt / 0.3f);
		m_HeadYaw += (m_GlanceYaw * reach - m_HeadYaw) * ease;
		m_HeadPitch += (m_GlancePitch * reach - m_HeadPitch) * ease;
		m_HeadTilt += (m_GlanceTilt * reach - m_HeadTilt) * ease;
		m_World.GetJoint(m_NeckJoint).MotorTargetRotation = glm::normalize(
			glm::angleAxis(m_HeadYaw, glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(m_HeadPitch, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(m_HeadTilt, glm::vec3(0.0f, 0.0f, 1.0f)) * m_NeckRest);

		// The weight shift: a slow drift of the hips over one foot and
		// back, a few centimetres, on a 6-10 s swell. The drift alone barely
		// shows; what the eye reads is the hip dropping on the unloaded side
		// and the shoulders tilting back against it, so those go with it.
		float shift = Noise(m_Clock / 7.0f, 21u);
		m_IdleRoll = idle * 0.08f * shift;

		// And the arms are never quite still.
		for (int side = 0; side < 2; side++)
			m_IdleArm[side] = idle * 0.08f * Noise(m_Clock / 3.1f, 51u + (uint32_t)side);
		return idle * 0.035f * shift;
	}

	// A shove at the torso, as the demo's X key and a hit in a game: the
	// impulse, and a stagger the controller then has to catch.
	void Shove(float strength, const glm::vec3& direction)
	{
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);

		// A shove applied straight to a body never arrives through a contact,
		// so ImpactForce would never see it. Judged on its own terms: the
		// impulse over one step is the force it represents.
		// The impulse is what was applied; no need to turn it into a force and
		// back again.
		if (m_Mode == Mode::Controlled)
		{
			if (StumblesPhysically())
			{
				// Dynamic first, so the impulse moves the whole body rather
				// than a torso jointed to an immovable pelvis.
				BeginStumble();
				m_World.ApplyImpulseAt(m_Torso, direction * strength, torso.Position);
				return;
			}
			Stagger(strength, direction);
		}
		m_World.ApplyImpulseAt(m_Torso, direction * strength, torso.Position);
	}


	// ---------------------------------------------------------------------
	// The rig
	// ---------------------------------------------------------------------
	//
	// Laid out from the floor up, in metres, for a figure about 1.86 m tall.
	// Masses are roughly anthropometric and total about 71 kg: they matter
	// more than they look like they should, because a joint's behaviour is
	// set by the *ratio* of the masses it connects, and a solver that copes
	// with 1:1 can sag badly at 15:1.
	void BuildRagdoll(const glm::vec3& origin)
	{
		auto capsule = [&](glm::vec3 centre, float radius, float halfHeight, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeCapsule(
				origin + centre, radius, halfHeight, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			// Limbs are damped more than loose debris would be. Flesh is not
			// a pendulum, and without this a passive ragdoll swings its arms
			// for an implausibly long time after it lands.
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		auto box = [&](glm::vec3 centre, glm::vec3 halfExtents, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeBox(
				origin + centre, halfExtents, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		auto sphere = [&](glm::vec3 centre, float radius, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeSphere(
				origin + centre, radius, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		const float hipX = 0.10f;
		const float shoulderX = 0.20f;

		// --- torso column -------------------------------------------------
		m_Pelvis = box({ 0.0f, 1.09f, 0.0f }, { 0.14f, 0.09f, 0.10f }, 11.0f);
		m_Torso  = box({ 0.0f, 1.40f, 0.0f }, { 0.17f, 0.22f, 0.11f }, 24.0f);
		m_Head   = sphere({ 0.0f, 1.74f, 0.0f }, 0.12f, 5.0f);

		// --- legs -----------------------------------------------------------
		//
		// The hips stay where they are and the feet move outwards, so a wider
		// stance splays the legs rather than detaching them from the pelvis.
		// Everything below is placed along the hip-to-ankle line, which is why
		// the capsules need orienting: a leg at an angle is not a leg pointing
		// down.
		for (int side = 0; side < 2; side++)
		{
			float sign = side == 0 ? -1.0f : 1.0f;

			glm::vec3 hipAt(sign * hipX, 1.00f, 0.0f);
			glm::vec3 ankleAt(sign * m_StanceWidth, 0.10f, 0.0f);

			// The knee sits the same fraction along the leg as it did when the
			// leg was vertical.
			glm::vec3 kneeAt = hipAt + (ankleAt - hipAt) * 0.533f;

			glm::vec3 upperAxis = hipAt - kneeAt;
			glm::vec3 lowerAxis = kneeAt - ankleAt;

			glm::quat upperTurn = RotationBetween({ 0.0f, 1.0f, 0.0f }, glm::normalize(upperAxis));
			glm::quat lowerTurn = RotationBetween({ 0.0f, 1.0f, 0.0f }, glm::normalize(lowerAxis));

			m_UpperLeg[side] = capsule((hipAt + kneeAt) * 0.5f, 0.08f,
				glm::length(upperAxis) * 0.5f - 0.08f, 7.5f);
			m_LowerLeg[side] = capsule((kneeAt + ankleAt) * 0.5f, 0.07f,
				glm::length(lowerAxis) * 0.5f - 0.07f, 3.3f);
			m_Foot[side] = box({ sign * m_StanceWidth, 0.05f, 0.03f },
				{ 0.06f, 0.05f, 0.11f }, 1.0f);

			m_World.GetBody(m_UpperLeg[side]).Orientation = upperTurn;
			m_World.GetBody(m_UpperLeg[side]).UpdateInertiaWorld();
			m_World.GetBody(m_LowerLeg[side]).Orientation = lowerTurn;
			m_World.GetBody(m_LowerLeg[side]).UpdateInertiaWorld();

			m_KneeAt[side] = kneeAt;
			m_AnkleAt[side] = ankleAt;
		}

		// --- arms -----------------------------------------------------------
		for (int side = 0; side < 2; side++)
		{
			float x = side == 0 ? -shoulderX : shoulderX;
			float armX = side == 0 ? -0.22f : 0.22f;

			m_UpperArm[side] = capsule({ armX, 1.375f, 0.0f }, 0.055f, 0.12f, 2.2f);
			m_LowerArm[side] = capsule({ armX, 1.040f, 0.0f }, 0.050f, 0.11f, 1.6f);
			(void)x;
		}

		// --- joints ---------------------------------------------------------
		//
		// Every ball joint gets a cone and a twist range, because an unlimited
		// one lets the limb rotate straight through the body -- and every one
		// of these is measured from the pose being built here rather than from
		// the two bodies being aligned, which is why a rig assembled crooked
		// still starts inside its own limits.
		auto ball = [&](GS::PhysicsWorld3D::BodyHandle a,
			GS::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
			glm::vec3 twistAxis, float coneDegrees, float twistDegrees, float torque)
		{
			auto joint = m_World.AddBallJoint(a, b, origin + anchor);
			m_World.SetConeTwistLimits(joint, twistAxis,
				glm::radians(coneDegrees),
				glm::radians(-twistDegrees), glm::radians(twistDegrees));
			m_World.SetJointMotor(joint, m_MotorStiffness, torque);
			m_Joints.push_back(joint);
			return joint;
		};

		auto hinge = [&](GS::PhysicsWorld3D::BodyHandle a,
			GS::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
			glm::vec3 axis, float lowerDegrees, float upperDegrees, float torque)
		{
			auto joint = m_World.AddHingeJoint(a, b, origin + anchor, axis);
			m_World.SetHingeLimits(joint, glm::radians(lowerDegrees), glm::radians(upperDegrees));
			m_World.SetJointMotor(joint, m_MotorStiffness, torque);
			m_Joints.push_back(joint);
			return joint;
		};

		// Spine and neck. The spine carries everything above it, so it is the
		// strongest joint in the rig by a wide margin.
		m_SpineJoint = ball(m_Pelvis, m_Torso, { 0.0f, 1.18f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }, 25.0f, 35.0f, 260.0f);
		m_SpineRest = m_World.GetJoint(m_SpineJoint).MotorTargetRotation;
		m_NeckJoint = ball(m_Torso, m_Head, { 0.0f, 1.62f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 40.0f, 60.0f, 30.0f);
		m_NeckRest = m_World.GetJoint(m_NeckJoint).MotorTargetRotation;

		for (int side = 0; side < 2; side++)
		{
			float x = side == 0 ? -hipX : hipX;
			float armX = side == 0 ? -0.22f : 0.22f;
			float shoulderSide = side == 0 ? -shoulderX : shoulderX;

			// Hip, knee, ankle.
			m_HipJoint[side] = ball(m_Pelvis, m_UpperLeg[side], { x, 1.00f, 0.0f },
				{ 0.0f, 1.0f, 0.0f }, 55.0f, 25.0f, 220.0f);
			// The pose the balance controller offsets from. Stored rather than
			// assumed to be the identity, so the rig could be built in a
			// crouch and the controller would still lean from there.
			m_HipRest[side] = m_World.GetJoint(m_HipJoint[side]).MotorTargetRotation;

			// The thigh body's turn within the frame SolveLegPole builds
			// (+y up the bone, +z forward) -- identity for a leg built
			// straight down, kept exact for one that is not.
			{
				glm::vec3 y = glm::normalize(glm::vec3(x, 1.00f, 0.0f) - m_KneeAt[side]);
				glm::vec3 z = glm::vec3(0.0f, 0.0f, 1.0f) - y * y.z;
				z = glm::normalize(z);
				glm::quat frame = glm::normalize(glm::quat_cast(glm::mat3(glm::cross(y, z), y, z)));
				m_ThighRestLocal[side] = glm::normalize(glm::conjugate(frame) * m_World.GetBody(m_UpperLeg[side]).Orientation);
			}

			// TRY: widen the knee's range past 130 degrees and watch the shin
			// pass through the thigh -- the limit is the only thing stopping
			// it, since jointed bodies do not collide with each other.
			m_KneeJoint[side] = hinge(m_UpperLeg[side], m_LowerLeg[side], m_KneeAt[side],
				// Positive, not negative. The figure faces +z -- its toes stick
				// out that way -- and a positive rotation about +x sends the
				// heel backwards, which is what a knee does. Built the other
				// way round it could *only* hyperextend, and the walk looked
				// like it was bending its legs the wrong way because it was.
				{ 1.0f, 0.0f, 0.0f }, 0.0f, 130.0f, 160.0f);

			// The ankle is a ball joint on a short leash, not a hinge.
			//
			// It was a hinge, and a hinge has one axis: pitch. That left the
			// figure with nothing at all to push sideways with, so the ankle
			// strategy held it forward and back and it fell over sideways
			// every time. A tight cone gives roll as well, which is the axis
			// lateral balance actually needs, and the twist range keeps the
			// foot from spinning under the leg.
			m_AnkleJoint[side] = ball(m_LowerLeg[side], m_Foot[side], m_AnkleAt[side],
				{ 0.0f, 1.0f, 0.0f }, 35.0f, 15.0f, 140.0f);
			m_AnkleRest[side] = m_World.GetJoint(m_AnkleJoint[side]).MotorTargetRotation;

			// Shoulder and elbow. Kept now, rather than created and
			// forgotten -- the walk drives them.
			m_ShoulderJoint[side] = ball(m_Torso, m_UpperArm[side],
				{ shoulderSide, 1.55f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 75.0f, 45.0f, 60.0f);
			m_ShoulderRest[side] = m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation;

			m_ElbowJoint[side] = hinge(m_UpperArm[side], m_LowerArm[side],
				{ armX, 1.20f, 0.0f },
				// And the elbow is the knee's mirror: the forearm swings
				// forwards, so its range is the negative one.
				{ 1.0f, 0.0f, 0.0f }, -140.0f, 0.0f, 35.0f);
		}

		// The rig is built facing +z, so the heading has to say so. It did not:
		// m_Facing is only ever written when getting up, and survived a
		// rebuild. Reset after walking a while and the new rig stood facing +z
		// while the gait placed its feet for wherever he had been going --
		// left and right swapped, the legs crossed, and the knee wandered
		// between 4 and 31 degrees instead of sitting still at 5.
		m_Facing = 0.0f;
		m_RunBlend = 0.0f;
		m_Duty = m_WalkDuty;
		m_Carry = 0.0f;

		SetMotorsEnabled(m_MotorsEnabled);

		// Remembered so going limp and back is reversible -- each joint has its
		// own budget and one number could not restore them.
		m_JointTorque.clear();
		for (auto handle : m_Joints)
			m_JointTorque[handle] = m_World.GetJoint(handle).MotorMaxTorque;

		{
			// Measured from the pose the rig was built in, so changing the
			// skeleton's proportions does not leave the IK solving for a leg
			// that no longer exists.
			const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
			for (int side = 0; side < 2; side++)
			{
				glm::vec3 hipAt(side == 0 ? -0.10f : 0.10f, 1.00f, 0.0f);
				hipAt += origin;

				m_HipLocal[side] = glm::conjugate(pelvis.Orientation) * (hipAt - pelvis.Position);

				// Same for the arm, measured off the pose it was built in, so
				// the get-up's hand IK solves for the arm that exists.
				const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
				glm::vec3 shoulderAt(side == 0 ? -0.22f : 0.22f, 1.55f, 0.0f);
				glm::vec3 elbowAt(side == 0 ? -0.22f : 0.22f, 1.20f, 0.0f);
				glm::vec3 wristAt(side == 0 ? -0.22f : 0.22f, 0.88f, 0.0f);
				shoulderAt += origin; elbowAt += origin; wristAt += origin;

				m_ShoulderLocal[side] = glm::conjugate(torso.Orientation)
					* (shoulderAt - torso.Position);
				m_UpperArmLength = glm::length(shoulderAt - elbowAt);
				m_ForearmLength = glm::length(elbowAt - wristAt);
				// `m_KneeAt` and `m_AnkleAt` are in the **rig's own frame** --
				// the joint lambdas above add `origin` on the way in, so that is
				// the convention they are stored in. Everything from here down
				// is world, so they have to be lifted into it.
				//
				// They were not, and it cost an afternoon. With the rig built at
				// the world origin the two frames are the same numbers and
				// nothing is wrong; build it anywhere else -- on a hill, say --
				// and the thigh measures the distance from the hip to a point
				// near the map's centre, the foot targets are computed against
				// ground on the far side of the map, and the figure stands there
				// with its legs reaching for somewhere it is not. It read as a
				// terrain bug and was a coordinate bug that terrain revealed.
				glm::vec3 kneeWorld = origin + m_KneeAt[side];
				glm::vec3 ankleWorld = origin + m_AnkleAt[side];

				m_ThighLength = glm::length(hipAt - kneeWorld);
				m_ShinLength = glm::length(kneeWorld - ankleWorld);

				// Both feet start planted where they were built. Heights are
				// from the rig's own soles (origin.y), not from whatever ground
				// lies below: built in mid-air -- a Player placed above the
				// floor in the editor, the demo's drop height -- the old
				// measurement made him 5 m tall, and he stood on the air at
				// that height for good.
				m_AnkleHeight = ankleWorld.y - origin.y;
				m_Planted[side] = ankleWorld;
				m_SwingFrom[side] = m_Planted[side];
				m_SwingTo[side] = m_Planted[side];
				m_Swinging[side] = false;
			}
		}

		m_CharacterBodies = { m_Pelvis, m_Torso, m_Head,
			m_UpperLeg[0], m_UpperLeg[1], m_LowerLeg[0], m_LowerLeg[1],
			m_Foot[0], m_Foot[1],
			m_UpperArm[0], m_UpperArm[1], m_LowerArm[0], m_LowerArm[1] };

		// What the impulse model divides by -- summed once the list exists.
		// It used to be summed just above, before the list was filled, so a
		// figure's first build had a mass of zero and Stagger, which divides
		// by it, quietly did nothing: in a game, where the rig is built once,
		// no hit ever staggered him. So did the Ragdoll demo until its first
		// Reset; Map Building builds twice at startup, and the second build
		// summed the first one's list, which is how it went unseen.
		m_BodyMass = 0.0f;
		for (unsigned int body : m_CharacterBodies)
			m_BodyMass += m_World.GetBody(body).GetMass();

		m_StandHeight = m_World.GetBody(m_Pelvis).Position.y;
		// How far the pelvis rides above whatever it stands on, taken from the
		// pose the rig was built in rather than written down twice.
		m_StandClearance = m_StandHeight - origin.y;
		m_RootAnchor = m_World.GetBody(m_Pelvis).Position;
		m_Speed = 0.0f;
		m_SpeedRate = 0.0f;
		m_GaitPhase = 0.0f;
		m_StandComHeight = CentreOfMass().y - origin.y;
		GoControlled();
	}

	void SetMotorsEnabled(bool enabled)
	{
		m_MotorsEnabled = enabled;
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorEnabled = enabled;
			joint.MotorStiffness = m_MotorStiffness;
		}

		ApplyLegStrength();
	}

	// The legs are driven harder than the rest of the body.
	//
	// They are tracking an IK target that moves every frame, and a stance foot
	// that does not keep up with it slides -- which is the whole thing the
	// placement was for. Swept: at the body's own stiffness the foot slid
	// 0.0218 m a frame against 0.0267 for a foot simply dragged along, so the
	// placement was buying almost nothing. At stiffness 40 with four times the
	// torque it slides **0.0107**. Stiffer is worse, not better: 90 oscillates
	// and the slide goes back up to 0.038.
	void ApplyLegStrength()
	{
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_HipJoint[side], m_KneeJoint[side], m_AnkleJoint[side] })
			{
				GS::Joint3D& joint = m_World.GetJoint(handle);
				// The ankle is stiffer than the rest of the leg. It carries a
				// 1 kg foot rather than the body's weight, and it is chasing a
				// target that moves with the shin every frame -- a motor is a
				// velocity constraint, so its steady-state error is roughly the
				// target's speed over its stiffness, and at the leg's 40 that
				// left the sole 3 deg off level all through the stance.
				joint.MotorStiffness = handle == m_AnkleJoint[side]
					? m_AnkleStiffness : m_LegStiffness;
				joint.MotorMaxTorque = m_JointTorque.count(handle)
					? m_JointTorque[handle] * m_LegTorqueScale : joint.MotorMaxTorque;
				// The IK target can move quickly; the default cap throttles it.
				joint.MotorMaxSpeed = 30.0f;
			}
		}
	}

	// ---------------------------------------------------------------------
	// Simulation
	// ---------------------------------------------------------------------
	// The shortest rotation taking `from` to `to`, both unit length.
	//
	// Written out rather than pulled from glm's experimental extensions, which
	// need a define before the include and are not worth dragging into a demo
	// header for four lines.
	static glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
	{
		float dot = glm::dot(from, to);

		// Opposite directions: no unique shortest arc, so any perpendicular
		// axis will do. Picking one deliberately beats letting the normalise
		// below divide by zero.
		if (dot < -0.9999f)
		{
			glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
			if (glm::length(axis) < 1e-4f)
				axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), from);

			return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
		}

		glm::vec3 axis = glm::cross(from, to);
		return glm::normalize(glm::quat(1.0f + dot, axis.x, axis.y, axis.z));
	}

	// The capture point, pulled in to something a leg can actually reach.
	glm::vec2 ClampedStepTarget(const glm::vec2& capturePoint) const
	{
		glm::vec2 hip(m_World.GetBody(m_Pelvis).Position.x,
			m_World.GetBody(m_Pelvis).Position.z);

		glm::vec2 fromHip = capturePoint - hip;
		float reach = glm::length(fromHip);

		return reach > m_MaxStep ? hip + fromHip * (m_MaxStep / reach) : capturePoint;
	}

	// Stepping: the only thing that recovers a fall the ankles cannot -- and
	// it does not work here, for a reason now pinned to one number.
	//
	// The arithmetic says the rig is fine. Linear inverted pendulum, all
	// figures read off this rig: 71.2 kg, centre of mass 1.085 m,
	// tau = sqrt(h/g) = 0.3325 s, foot half-length 0.110 m. A foot placed
	// `reach` beyond the centre of mass catches a fall at (reach + footHalf)/tau,
	// and an impulse J at the chest gives v = J/m, so:
	//
	//     ankles only              0.000 m   0.33 m/s    23.6 Ns
	//     the step actually made   0.183 m   0.88 m/s    62.7 Ns
	//     the step geometry allows 0.737 m   2.55 m/s   181.4 Ns
	//
	// The first line is confirmed by experiment -- 20 Ns survives half the
	// time and 40 Ns never does, bracketing 23.6 exactly. So the model is
	// right, and by it even the modest step already achieved should nearly
	// **triple** the impulse the figure can take.
	//
	// It does not, and here is where it goes:
	//
	//     support reach towards the fall: 0.0019 m before the step,
	//                                     0.0000 m after the foot lands
	//
	// **The base does not get any bigger.** The foot travels 0.183 m and the
	// polygon it stands on gains nothing in the direction it is falling.
	//
	// Chased down, along the fall direction over twelve steps:
	//
	//     the foot moved           -0.069 m   (backwards!)
	//     the centre of mass moved +0.332 m
	//     foot behind the com:     -0.18 m -> -0.58 m
	//
	// The foot is not aimed wrongly -- its distance to the target shrinks
	// steadily through the swing, 1.243 m to 0.988 m. The trouble is that the
	// target is 1.24 m away to begin with, because by the time the swing runs
	// the pelvis has already travelled **0.724 m horizontally past the foot**.
	// The leg closes a quarter of a metre; the body covers a third of a metre
	// in the same time. The foot chases a target bolted to a pelvis that is
	// outrunning it, and finishes further behind than it started.
	//
	// The hip is not at its cone limit while this happens (0.856 of 0.960 rad),
	// the motor is not saturated, and the stance leg is fine. **The step is not
	// too weak, it is too late** -- not in the sense the trigger fires late,
	// which was measured and is false, but in the sense that a step which takes
	// 0.16 s cannot catch a body already moving faster than a leg can swing. Three controllers were tried and all three failed
	// the same way, for a reason that is mechanical rather than about timing.
	//
	// **The foot never leaves the ground.** Measured: asked to move 0.231 m,
	// it moved 0.021 m. The swing foot is still carrying weight, and a loaded
	// foot cannot be lifted however hard the hip pulls. Unloading it means
	// shifting the weight laterally onto the other foot, which is rate limited
	// to about sqrt(h/g) = 0.33 s -- and a push that needs recovering is over
	// in about a second.
	//
	// A backwards shove makes it worse, because it loads *both* feet equally:
	// the body rotates towards its heels rather than onto one side, so neither
	// foot frees itself. A person stepping backwards from a shove unweights one
	// side first, and that lateral move is the part there is no time for.
	//
	// Kept, switchable, and off. What is here is correct as far as it goes:
	// the trigger fires when the capture point leaves the feet and not during
	// sway, the foot choice and the target are right. What is missing is a way
	// to unload a foot quickly, and that is a rig and controller question
	// rather than a tuning one.
	//
	// Once the capture point is outside the feet, no ankle torque brings it
	// back -- an ankle can only move where the weight bears *within* the base.
	// The recovery is to move the base: put a foot where the capture point
	// went, and the figure arrives over its feet again instead of past them.
	//
	// The step itself is a short scripted swing rather than anything clever:
	// lift the foot by bending the knee, point the leg at the target, then
	// straighten and plant. Three quarters of a second of open loop. What makes
	// it work is not the trajectory but *where* it aims, which is the capture
	// point and not the place the body currently is.
	void UpdateStepping(const BalanceState& state, float dt)
	{
		// Waiting for a trough: the step is decided, the foot is not yet moving.
		//
		// The swing foot carries about half the body's weight on average, and
		// no hip motor lifts that. But the standing pose bounces -- each foot
		// passes under 150 N for roughly a ninth of the time, in windows of
		// about a tenth of a second -- so rather than trying to *create* an
		// unload, which is rate limited to sqrt(h/g) and far too slow, this
		// waits for one that is already coming and lifts into it.
		if (m_WaitingForTrough)
		{
			m_WaitTimer += dt;

			// The target keeps moving while we wait, so it is re-aimed rather
			// than fired at where the capture point used to be.
			m_StepTarget = ClampedStepTarget(state.CapturePoint);

			float load = FootLoad(m_SwingSide, dt);

			// Where the load will be a moment from now, not where it is.
			//
			// Waiting for the load to actually cross the gate spends the front
			// of the trough getting the lift started, and the trough is only
			// about 0.10 s long against a swing that needs longer. Committing
			// on the extrapolation centres the free window on the lift instead
			// of starting it there. The rate has to be negative as well as
			// large -- a load climbing through the gate is the *end* of a
			// trough, and lifting into that is lifting into a rising floor.
			float rate = (load - m_PreviousSwingLoad) / dt;
			m_PreviousSwingLoad = load;

			float predicted = load + rate * m_LeadTime;
			bool arriving = rate < 0.0f && predicted < m_LoadGate;

			if (load < m_LoadGate || arriving)
			{
				m_WaitingForTrough = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
				m_TroughsUsed++;
			}
			else if (m_WaitTimer >= m_MaxWait)
			{
				// No trough arrived in time. Going anyway is worse than not
				// going -- it is the loaded lift that never worked -- so the
				// step is abandoned and the ankles carry on.
				m_WaitingForTrough = false;
				m_TroughsMissed++;
				m_StepCooldown = m_StepCooldownTime;
			}
			return;
		}

		// Shifting: both feet still down, weight moving over the stance foot.
		//
		// This phase is the whole difference between a step that helps and one
		// that does not. Lifting a foot from a body whose weight is still
		// spread across both throws the pelvis, because the support under half
		// of it simply stops existing. A person leans first; so does this now.
		if (m_Shifting)
		{
			// A reactive step does not get a shift. Measured: the shift needs
			// sqrt(h/g), about 0.33 s, and the swing another third -- against a
			// fall that is over in a second. Deliberate steps can afford it;
			// a recovery cannot, and going without means accepting a moment of
			// single support, which is what a person catching themselves does.
			if (m_FastStep)
			{
				m_Shifting = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
				return;
			}

			m_ShiftTimer += dt;

			// Done when the weight is over the stance foot, or when it has had
			// long enough. The timeout matters: waiting for a condition that
			// may never arrive is how a balance controller freezes mid-fall.
			const GS::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
			float remaining = std::fabs(state.Com.x - stanceFoot.Position.x);

			if (remaining < m_ShiftTolerance || m_ShiftTimer >= m_ShiftDuration)
			{
				m_Shifting = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
			}
			return;
		}

		if (m_Stepping)
		{
			m_StepTimer += dt;
			if (m_StepTimer >= m_StepDuration)
			{
				m_Stepping = false;
				m_StepsTaken++;

				// The stepped leg holds where it landed instead of returning to
				// the pose it started in.
				//
				// This was the single worst bug in the step: resetting the hip
				// to its rest target swung the leg straight back under the body
				// the instant the foot touched down, undoing the step that had
				// just been taken. The figure would place a foot correctly and
				// then pull it out from under itself.
				const GS::RigidBody3D& thigh = m_World.GetBody(m_UpperLeg[m_SwingSide]);
				const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
				m_HipRest[m_SwingSide] = glm::normalize(
					glm::conjugate(pelvis.Orientation) * thigh.Orientation);

				m_World.GetJoint(m_HipJoint[m_SwingSide]).MotorTargetRotation = m_HipRest[m_SwingSide];
				m_World.GetJoint(m_KneeJoint[m_SwingSide]).MotorTargetAngle = 0.0f;
				m_StepCooldown = m_StepCooldownTime;
			}
			return;
		}

		// Three points or it is not a polygon, and the margin is a placeholder
		// rather than a measurement.
		//
		// This mattered more than it looks: a foot in the air leaves one foot
		// on the ground, which often gives fewer than three contact points, so
		// the margin read -1 and triggered *another* step the moment the last
		// one finished. The figure stepped continuously and fell over its own
		// feet -- 236 steps in fifteen seconds.
		if (!m_SteppingEnabled || state.Support.size() < 3)
			return;

		// Not while the last step is still settling. Without this the figure
		// steps, loses half its support to the lifted foot, reads the smaller
		// polygon as a worse margin, and steps again -- a loop that walks it
		// over in about a second.
		if (m_StepCooldown > 0.0f)
		{
			m_StepCooldown -= dt;
			return;
		}

		// Only when the capture point is genuinely out, and only when it stays
		// out.
		//
		// Two conditions rather than one, because they reject different
		// things. The threshold ignores small excursions; the persistence
		// count ignores brief ones. Ordinary sway produces both -- the capture
		// point dips over an edge for a frame or two and comes back -- and a
		// step taken in answer costs a foot's support for a third of a second
		// to fix something that had already fixed itself.
		if (state.Margin > -m_StepTrigger)
		{
			m_OutsideFor = 0;
			return;
		}

		m_OutsideFor++;
		if (m_OutsideFor < m_StepPersist)
			return;

		glm::vec2 away = state.CapturePoint - state.SupportCentre;
		if (glm::length(away) < 1e-4f)
			return;

		// Step with the foot on the side it is falling towards. Stepping the
		// other one crosses the legs and makes things worse -- and it is what
		// a person does too: you catch yourself on the foot nearest the fall.
		m_SwingSide = away.x > 0.0f ? 1 : 0;

		// Aim at the capture point, but no further than a leg can reach.
		glm::vec2 target = ClampedStepTarget(state.CapturePoint);

		// A step that lands where the foot already is buys nothing and costs
		// the support of a raised foot for a third of a second. If the capture
		// point is that close, the ankles can have it.
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[m_SwingSide]);
		glm::vec2 footNow(foot.Position.x, foot.Position.z);

		if (glm::length(target - footNow) < m_MinStep)
			return;

		m_StepTarget = target;

		if (m_GateOnLoad)
		{
			m_WaitingForTrough = true;
			m_WaitTimer = 0.0f;
			// Seeded, so the first frame's rate is zero rather than the
			// difference against whatever was left from the last step.
			m_PreviousSwingLoad = FootLoad(m_SwingSide, dt);
		}
		else
		{
			m_Shifting = true;
			m_ShiftTimer = 0.0f;
		}
	}

	// Drives the swinging leg. Called every step while a step is in progress.
	void DriveSwingLeg()
	{
		if (!m_Stepping)
			return;

		float t = glm::clamp(m_StepTimer / m_StepDuration, 0.0f, 1.0f);

		// Bend early to clear, straighten late to reach.
		//
		// A plain sin(pi*t) bends and straightens symmetrically, peaking
		// halfway. That spends the second half of the swing -- the half that
		// is supposed to be reaching for the target -- with the leg still
		// substantially folded, and a folded leg converts hip rotation into far
		// less foot travel. Moving the peak earlier clears the ground on the
		// same schedule and leaves the leg extended for longer while it reaches.
		//
		// The phase is warped rather than the curve replaced, so the profile
		// still starts and ends at zero bend and still peaks at exactly
		// m_StepLift -- only *when* moves.
		float peak = glm::clamp(m_LiftPeak, 0.05f, 0.95f);
		float phase = t < peak
			? 0.5f * (t / peak)
			: 0.5f + 0.5f * (t - peak) / (1.0f - peak);

		float lift = std::sin(phase * glm::pi<float>());
		m_World.GetJoint(m_KneeJoint[m_SwingSide]).MotorTargetAngle = m_StepLift * lift;

		// Point the whole leg at the target. The leg hangs along -y at rest,
		// so the hip rotation is whatever takes -y to the direction from the
		// hip to where the foot should land.
		glm::vec3 hip = m_World.GetBody(m_Pelvis).Position;
		hip.y -= 0.09f;   // roughly the hip joint, below the pelvis centre

		glm::vec3 target(m_StepTarget.x, 0.0f, m_StepTarget.y);
		glm::vec3 toTarget = target - hip;

		if (glm::length(toTarget) < 1e-4f)
			return;

		// Into the pelvis's frame before building the rotation.
		//
		// MotorTargetRotation is the thigh relative to the *pelvis*, and
		// `toTarget` is a world direction. Handing one to the other works only
		// while the pelvis happens to be upright -- and a figure that needs to
		// step is a figure whose pelvis is tilting. Measured with the world
		// vector, the foot moved 0.073 m *backwards* along the fall direction
		// while the body moved 0.329 m forwards.
		glm::vec3 localToTarget = glm::conjugate(m_World.GetBody(m_Pelvis).Orientation)
			* glm::normalize(toTarget);

		glm::quat aim = RotationBetween(glm::vec3(0.0f, -1.0f, 0.0f), localToTarget);

		// Eased in, so the leg swings rather than snaps to the target on the
		// first frame and throws the whole body with it -- but a reactive step
		// has no time for a gentle onset, so it reaches full aim by a third of
		// the way through instead of two thirds.
		float blend = glm::smoothstep(0.0f, m_FastStep ? 0.3f : 0.6f, t);
		glm::quat rest = m_HipRest[m_SwingSide];

		m_World.GetJoint(m_HipJoint[m_SwingSide]).MotorTargetRotation =
			glm::normalize(glm::slerp(rest, aim * rest, blend));
	}

	// The ankle-and-hip strategy, which is what people use for small
	// disturbances before they resort to stepping.
	//
	// Both are just a nudge to a motor target, not a torque applied from
	// nowhere: the ankles pitch to shift where the weight bears, and the hips
	// roll the body sideways over the supporting foot. Nothing here can save a
	// figure whose capture point has already left its feet -- that needs a
	// step, and stepping is the next piece.
	void ApplyBalance(const BalanceState& state)
	{
		if (!m_BalanceEnabled || !m_MotorsEnabled || state.Support.size() < 3)
			return;

		// How far the capture point is from the middle of the feet, which is
		// the error the controller is trying to null.
		glm::vec2 error = state.CapturePoint - state.SupportCentre;

		// Both axes at the ankle now. Pitch handles forward and back; roll is
		// what the hinge could not do and what sent every earlier attempt over
		// sideways.
		//
		// The signs were settled by experiment rather than by reasoning about
		// which way a foot bends -- see the changelog. Reasoning about it was
		// how the first three attempts went wrong.
		float pitch = glm::clamp( error.y * m_AnkleGain,     -0.45f, 0.45f);
		float roll  = glm::clamp(-error.x * m_AnkleRollGain, -0.45f, 0.45f);

		// While shifting, add a commanded lean towards the stance foot on top
		// of the correction. Same mechanism the lateral strategy uses -- a
		// positive roll pushes the body towards +x -- but driven by where the
		// weight needs to go rather than by where the capture point is.
		if (m_Shifting)
		{
			const GS::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
			float towards = stanceFoot.Position.x - state.Com.x;
			roll = glm::clamp(roll + m_ShiftRoll * glm::sign(towards), -0.55f, 0.55f);
		}

		glm::quat correction = glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f));

		for (int side = 0; side < 2; side++)
		{
			// A foot in the air is not holding anything up, and correcting it
			// only wastes the swing. During the shift both feet are still
			// down, so both still get corrected.
			if (m_Stepping && side == m_SwingSide)
				continue;

			GS::Joint3D& joint = m_World.GetJoint(m_AnkleJoint[side]);
			joint.MotorTargetRotation = glm::normalize(correction * m_AnkleRest[side]);
		}
	}

	// ---------------------------------------------------------------------
	// The two modes
	// ---------------------------------------------------------------------
	//
	// This is how games do it, and it is not balance.
	//
	// **Controlled.** The pelvis is *kinematic*: infinite mass, driven
	// directly by input, immovable by any contact or joint. The character
	// cannot fall over because the thing holding it up is not simulated. The
	// rest of the skeleton is still fully dynamic and hangs off it, so the
	// limbs swing, collide and react -- which is what sells it as physical
	// rather than as an animation.
	//
	// **Ragdoll.** The pelvis becomes dynamic, inheriting the velocity it had,
	// and the motors go slack. Nothing else changes: the same bodies, the same
	// joints, the same limits. Physics simply takes over.
	//
	// The switch is the whole trick. Balancing a figure on its own skeleton is
	// the hard research problem this demo spent a long time failing at; not
	// having to is what makes a playable character tractable.
	enum class Mode { Controlled, Ragdoll, GettingUp, Stumbling };

	void GoRagdoll(const char* why)
	{
		if (m_Mode == Mode::Ragdoll)
			return;

		// A stumble that is lost goes down with its arms out: falling from
		// one, he knows which way he is going.
		bool braced = m_Bracing && m_Mode == Mode::Stumbling;

		UnpinFeet();
		m_Mode = Mode::Ragdoll;
		m_RagdollReason = why;
		m_RagdollTimer = 0.0f;

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Dynamic;

		// It keeps the velocity it was being driven at, so a character shoved
		// while running tumbles forward rather than dropping on the spot.
		// Everything else is already dynamic and already moving.
		pelvis.Awake = true;
		pelvis.SleepTimer = 0.0f;

		// Limp, not rigid. A corpse that holds its pose reads as a mannequin
		// being dropped -- some residual stiffness keeps the joints from
		// looking like string, which is roughly what muscle tone does.
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_LimpStiffness;
			joint.MotorMaxTorque = m_LimpTorque;
		}

		m_BraceTimer = 0.0f;
		if (braced)
		{
			glm::vec3 velocity = CentreOfMassVelocity();
			glm::vec2 flat(velocity.x, velocity.z);
			if (glm::length(flat) > 0.05f)
			{
				m_BraceTimer = m_BraceTime;
				m_BraceDirection = glm::vec3(flat.x, 0.0f, flat.y) / glm::length(flat);
			}
		}
	}

	// Bracing: Euphoria's "catch fall". For a moment after a lost stumble
	// the arms keep their strength and reach for the ground where he is
	// going -- a hand out ahead and below the shoulder -- so the hands land
	// before the head does and take some of the fall. The rest of him is
	// as limp as any ragdoll.
	void UpdateBrace(float dt)
	{
		if (m_BraceTimer <= 0.0f)
			return;
		m_BraceTimer -= dt;
		bool holding = m_BraceTimer > 0.0f;
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_ShoulderJoint[side], m_ElbowJoint[side] })
			{
				GS::Joint3D& joint = m_World.GetJoint(handle);
				joint.MotorStiffness = holding ? m_MotorStiffness : m_LimpStiffness;
				joint.MotorMaxTorque = holding ? m_JointTorque[handle] : m_LimpTorque;
			}
			if (!holding)
				continue;
			glm::vec3 reach = glm::normalize(m_BraceDirection * m_BraceForward + glm::vec3(0.0f, -1.0f, 0.0f));
			SolveArm(side, ShoulderWorld(side) + reach * (m_UpperArmLength + m_ForearmLength) * 0.95f);
		}
	}

	glm::vec3 CentreOfMassVelocity() const
	{
		glm::vec3 momentum(0.0f);
		float total = 0.0f;
		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			momentum += body.Velocity * body.GetMass();
			total += body.GetMass();
		}
		return total > 0.0f ? momentum / total : glm::vec3(0.0f);
	}

	// Straight to standing, with no blend. Used when the scene is built and
	// when a blend finishes.
	void GoControlled()
	{
		m_Mode = Mode::Controlled;
		m_RagdollTimer = 0.0f;
		m_GetUpTimer = 0.0f;

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);

		m_StandTarget = m_StandHeight;
		m_StandRate = 0.0f;

		// The character stands up where it is lying, not where it fell from.
		m_RootAnchor = pelvis.Position;
		m_RootAnchor.y = m_StandHeight;
		m_Speed = 0.0f;
		m_SpeedRate = 0.0f;
		m_GaitPhase = 0.0f;

		// Whatever knocked him over does not follow him back up. Left behind,
		// the stagger that felled him is still there the instant he stands and
		// fells him again -- and it survived a scene rebuild too, which is how
		// it was found: every case after the first fall reported a fall.
		m_StaggerVelocity = glm::vec3(0.0f);
		m_StaggerTimer = 0.0f;
		m_LastSpeed = 0.0f;
		m_LeanAccel = 0.0f;
		m_JumpPending = false;
		m_JumpLead = -1;
		m_Air = Air::Grounded;
		m_JumpOffset = 0.0f;
		m_JumpVelocity = 0.0f;
		m_LandSpeed = 0.0f;

		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_MotorStiffness;
			joint.MotorMaxTorque = m_JointTorque[handle];
		}

		ApplyLegStrength();
	}

	// Getting up: a blend rather than a snap.
	//
	// The pelvis becomes kinematic immediately -- it has to be drivable to be
	// blended -- and is then carried from wherever it came to rest back to
	// standing, over about a second. The limbs are not animated at all: the
	// motors come back up from limp over the same period and drag them into
	// the pose, which is why the arms trail and settle rather than snapping
	// into place. It is the cheap version of what games do by blending into a
	// get-up animation, and most of what sells it is simply that it takes time.
	void BeginGetUp()
	{
		if (m_Mode == Mode::GettingUp)
			return;

		// Only from the ground. Asked in mid-air -- G pressed while falling
		// off the demo's platform -- it used to hoist the body to standing
		// height over nothing. Asked under his lowest body, which is the one
		// lying on whatever he is lying on, ignoring all of his own.
		{
			GS::PhysicsWorld3D::BodyHandle lowest = m_Pelvis;
			for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
				if (m_World.GetBody(handle).Position.y < m_World.GetBody(lowest).Position.y)
					lowest = handle;
			const GS::RigidBody3D& body = m_World.GetBody(lowest);
			float below = body.Position.y - m_World.GroundHeightBelow(body.Position, m_CharacterBodies, -1e9f);
			if (below > m_GetUpMaxGap)
				return;
		}

		UnpinFeet();
		m_Mode = Mode::GettingUp;
		m_GetUpTimer = 0.0f;

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		m_GetUpFrom = pelvis.Position;
		m_GetUpFromOrientation = pelvis.Orientation;

		// Stand on whatever is underneath, not at the height of the floor it
		// started on -- otherwise getting up on a step sinks into it.
		m_GetUpGround = m_World.GroundHeightBelow(pelvis.Position, m_CharacterBodies);
		m_GetUpTo = glm::vec3(pelvis.Position.x,
			m_GetUpGround + m_StandClearance, pelvis.Position.z);

		// Face down or face up. Which one decides whether he comes up over his
		// hands or has to roll first, and they do not look the same at all.
		// Which way the *front* of him is pointing. The first version of this
		// asked the pelvis's local +y, which runs up the body towards the head
		// -- for a figure lying flat that is horizontal, so its sign was noise
		// and the branch it chose was a coin toss. The figure faces local +z.
		// Face down is -1, face up is +1, and near zero is lying on a side,
		// where a roll is neither needed nor wanted.
		glm::vec3 front = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		m_GetUpFaceDown = front.y < -0.5f;
		m_GetUpSupine = front.y > 0.5f;

		// Keep whatever heading it ended up facing, flattened. Standing up
		// facing the direction you fell reads as a stumble recovered; snapping
		// back to the old heading reads as a rewind.
		glm::vec3 facing = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		if (glm::length(glm::vec2(facing.x, facing.z)) > 0.01f)
			m_Facing = std::atan2(facing.x, facing.z);

		// Where the hands will be planted: ahead of the chest, on the ground,
		// and fixed in the world so the body can push against them instead of
		// carrying them along with it.
		glm::vec3 ahead(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 across(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		for (int side = 0; side < 2; side++)
		{
			m_HandSpot[side] = glm::vec3(pelvis.Position.x, 0.0f, pelvis.Position.z)
				+ ahead * m_HandForward
				+ across * (side == 0 ? -m_HandWidth : m_HandWidth);
			m_HandSpot[side].y = m_GetUpGround + m_HandHeight;
		}

		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);
	}

	// Getting up in two stages, because one was the problem.
	//
	// A single lerp from where the body lies to a standing pose drags the
	// whole figure through the air in the shape it fell in -- the feet arrive
	// under the hips at the same instant the hips arrive at standing height,
	// which is not getting up, it is being placed. A person gathers first and
	// rises second, and the rise happens *over* feet that are already planted.
	//
	// So: gather, then rise. The pelvis comes up only as far as kneeling while
	// the feet are collected underneath it, and only then straightens.
	void UpdateGetUp(float dt)
	{
		m_GetUpTimer += dt;

		float t = glm::clamp(m_GetUpTimer / m_GetUpTime, 0.0f, 1.0f);

		float onHands = glm::smoothstep(0.0f, 1.0f,
			glm::clamp(t / m_GetUpHands, 0.0f, 1.0f));
		float gather = glm::smoothstep(0.0f, 1.0f,
			glm::clamp((t - m_GetUpHands) / std::max(m_GetUpGather - m_GetUpHands, 0.01f),
				0.0f, 1.0f));
		float rise = glm::smoothstep(0.0f, 1.0f,
			glm::clamp((t - m_GetUpGather) / std::max(1.0f - m_GetUpGather, 0.01f),
				0.0f, 1.0f));

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);

		// Three waypoints, not two. Up onto the hands first, then back over
		// the feet as they gather, and only then up.
		//
		// A person getting off the floor does not rise straight from where
		// they are lying -- there is nothing under them there. They put their
		// hands down, push their weight up and forward onto them, walk their
		// feet in underneath, and stand up over the feet. Lerping the pelvis
		// straight from lying to standing skips every part of that, which is
		// why it read as being placed rather than getting up.
		glm::vec3 ahead(std::sin(m_Facing), 0.0f, std::cos(m_Facing));

		glm::vec3 overHands = m_GetUpFrom + ahead * m_GetUpShift;
		overHands.y = m_GetUpGround + m_PushUpHeight;

		glm::vec3 overFeet(m_GetUpTo.x, m_GetUpGround + m_KneelHeight, m_GetUpTo.z);

		glm::vec3 target = glm::mix(m_GetUpFrom, overHands, onHands);
		target = glm::mix(target, overFeet, gather);
		target = glm::mix(target, m_GetUpTo, rise);

		pelvis.Velocity = (target - pelvis.Position) / std::max(dt, 1e-4f);

		// Upright by the end of the gather, not the end of the whole move, so
		// the rise happens from a pose that is already the right way up.
		glm::quat upright = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));

		// Face up, he has to get over onto a side before any of that helps.
		// Rolling the long way round through the back is what makes a supine
		// get-up read differently from a prone one.
		if (m_GetUpSupine)
		{
			float roll = std::sin(glm::clamp(t / m_GetUpHands, 0.0f, 1.0f)
				* glm::pi<float>()) * m_GetUpRoll;
			upright = glm::normalize(upright
				* glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f)));
		}

		pelvis.Orientation = glm::slerp(m_GetUpFromOrientation, upright,
			glm::max(onHands, gather));
		pelvis.UpdateInertiaWorld();

		// Strength returns over the gather, so the legs can be steered before
		// they are asked to carry anything.
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			// Twice the rate of the gather, so the legs have authority while
			// there is still time left to gather with. Ramped over the whole
			// gather instead, strength arrived just as the gather ended and
			// the feet were still 0.49 m from under the hips at the rise.
			// Back before the hands are asked to carry anything, which is
			// earlier than it used to need to be.
			float strength = glm::clamp(onHands * 2.0f, 0.0f, 1.0f);
			joint.MotorStiffness = glm::mix(m_LimpStiffness, m_MotorStiffness, strength);
			joint.MotorMaxTorque = glm::mix(m_LimpTorque, m_JointTorque[handle], strength);
			joint.MotorTargetAngle = 0.0f;
		}

		// The hands are planted while the weight comes over them, and let go
		// once the feet have taken it.
		if (t < m_GetUpGather)
		{
			for (int side = 0; side < 2; side++)
				SolveArm(side, m_HandSpot[side]);
		}
		else
		{
			for (int side = 0; side < 2; side++)
			{
				m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation =
					m_ShoulderRest[side];
				m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = 0.0f;
			}
		}

		// And the feet are collected under the hips during the gather, so that
		// by the time the body rises there is something under it. This is the
		// whole difference between getting up and being teleported upright.
		glm::vec3 right(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 hip = HipWorld(side);
			glm::vec3 foot = hip + right * (side == 0 ? -m_StanceWidth : m_StanceWidth);

			// Picked up on the way, not dragged. A foot slid along the floor
			// catches on its own friction and stays where it fell -- one leg
			// was still 0.79 m from under the hip when the body started to
			// rise. Lifting it clear costs nothing and is what a person does.
			float lift = std::sin(glm::clamp(
				(t - m_GetUpHands) / std::max(m_GetUpGather - m_GetUpHands, 0.01f),
				0.0f, 1.0f) * glm::pi<float>()) * m_GetUpFootLift;
			foot.y = m_GetUpGround + m_AnkleHeight + lift;

			SolveLeg(side, hip, foot, dt);

			// Planted where they were gathered, so the walk picks up from a
			// stance that is actually under him rather than snapping.
			m_Planted[side] = foot;
			m_Swinging[side] = false;
		}

		// Standing again at the height it got up to -- the ground it lay on
		// plus the clearance -- not the height it last stood at before it
		// fell. GoControlled anchors the hips at m_StandHeight, and the fall
		// check measures from it: left stale, a figure that fell off a ledge
		// got up, was hoisted back to the ledge's height, read a drop it had
		// already taken as a new one, and went down again the next step.
		// (The Ragdoll demo showed it too: back to Controlled at step 820 of
		// a recorded session, Ragdoll again at 821.)
		if (t >= 1.0f)
		{
			m_StandHeight = m_GetUpTo.y;
			GoControlled();
		}
	}

	bool IsCharacter(unsigned int body) const
	{
		return std::find(m_CharacterBodies.begin(), m_CharacterBodies.end(), body)
			!= m_CharacterBodies.end();
	}

	// A hit hard enough to knock the character down.
	//
	// Measured as the impulse arriving through contacts on anything that is
	// not a foot -- feet are in contact with the floor constantly and would
	// trigger it every step. Read as a force so the threshold is in units a
	// person can reason about: body weight is 697 N.
	float ImpactForce(float dt)
	{
		glm::vec3 ignored(0.0f);
		return ImpactForce(dt, ignored);
	}

	// The same, and which way it pushed -- a stagger has to go somewhere.
	float ImpactForce(float dt, glm::vec3& direction)
	{
		direction = glm::vec3(0.0f);
		m_HitPush = glm::vec3(0.0f);

		if (dt <= 0.0f)
			return 0.0f;

		float worst = 0.0f;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool mineA = IsCharacter(contact.A);
			bool mineB = IsCharacter(contact.B);

			// Exactly one side must be the character. Both sides being the
			// character is the figure touching *itself* -- a forearm resting
			// against the torso, or one shin brushing the other -- and those
			// are not jointed to each other so they collide like anything else.
			// Counting them knocked the character down on the first frame it
			// stood up.
			if (mineA == mineB)
				continue;

			// The feet are on the floor permanently; only a hit somewhere else
			// is a hit.
			unsigned int mine = mineA ? contact.A : contact.B;
			if (mine == m_Foot[0] || mine == m_Foot[1])
				continue;

			// Nor is a leg brushing the scenery. Walking up stairs the shins
			// scrape the risers, and read as hits that put him into a stumble
			// and down the stairs -- every time, in the stairs test. Fixed or
			// driven things only: something thrown at his legs still counts.
			unsigned int other = mineA ? contact.B : contact.A;
			bool leg = mine == m_UpperLeg[0] || mine == m_UpperLeg[1] || mine == m_LowerLeg[0] || mine == m_LowerLeg[1];
			if (leg && m_World.GetBody(other).Type != GS::BodyType::Dynamic)
				continue;

			float force = contact.TotalNormalImpulse() / dt;
			if (force > worst)
			{
				worst = force;

				// The manifold normal points from A to B, so it has to be
				// flipped when the character is A -- otherwise every stagger
				// goes into the thing that hit him.
				glm::vec3 push = mineA ? -contact.Normal : contact.Normal;
				// The whole of it, for a body that can now move every way --
				// a crate on the head pushes down, and the old stagger,
				// which only moves the root sideways, threw that away.
				m_HitPush = push;
				push.y = 0.0f;
				if (glm::length(push) > 1e-4f)
					direction = glm::normalize(push);
			}
		}

		return worst;
	}

	// A hit worth staggering over but not worth falling over.
	//
	// The complaint this answers: the character ragdolled abruptly at a single
	// threshold, so a shove was either nothing at all or a collapse. A person
	// hit that hard takes a couple of quick steps in the direction they were
	// pushed and stays up, and only goes down when the push outruns what a
	// step can catch.
	// An impulse is a velocity, and that is the whole model.
	//
	// This used to scale the push across an arbitrary band between two force
	// thresholds, which meant the two numbers that decided whether a person
	// stays on their feet were both invented. A hit of `J` newton-seconds on a
	// body of mass `m` changes its velocity by `J/m` -- no band, no scaling,
	// and the units come out on their own.
	void Stagger(float impulse, const glm::vec3& direction)
	{
		if (glm::length(direction) < 1e-4f || m_BodyMass <= 0.0f)
			return;

		m_StaggerVelocity += direction * (impulse / m_BodyMass);
		m_StaggerTimer = m_StaggerTime;
	}

	// How much unintended speed a step can still catch.
	//
	// A body toppling about its feet is an inverted pendulum with time
	// constant tau = sqrt(h/g), and the point it has to get a foot to is its
	// capture point, v*tau ahead. So a step of reach R catches v = R/tau, and
	// several quick steps catch a multiple of it. At the rig's numbers that is
	// about 1.5 m/s for one step.
	//
	// Note this is about *unintended* velocity only. The total is no use: a
	// run puts the capture point 1.16 m from the nearest foot as a matter of
	// course, further than any shove that used to fell him, because running is
	// controlled falling and is supposed to look like that.
	float CatchableSpeed() const
	{
		float ground = m_World.GroundHeightBelow(
			m_World.GetBody(m_Pelvis).Position, m_CharacterBodies, -1000.0f);
		float height = glm::clamp(m_Balance.Com.y - ground, 0.2f, 2.0f);
		float tau = std::sqrt(height / 9.81f);

		// Reach is not the same in every direction. A leg swings a long way
		// forwards and backwards and barely at all across the body -- the feet
		// are 0.20 m apart sideways and a stride is over half a metre -- which
		// is why a shove from the side puts people down that a shove from
		// behind does not.
		float reach = m_CatchReach;
		float speed = glm::length(m_StaggerVelocity);
		if (speed > 1e-4f)
		{
			glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
			float along = std::fabs(glm::dot(m_StaggerVelocity / speed, forward));
			reach = glm::mix(m_CatchReachSide, m_CatchReach, along);
		}

		return (reach * m_CatchSteps) / std::max(tau, 0.05f);
	}

	// ---------------------------------------------------------------------
	// Stumbling: a hit the body has to catch with its own feet
	// ---------------------------------------------------------------------
	//
	// The stagger above moves a kinematic root and lets the gait chase it:
	// it decides up front, from a formula, whether he stays up. This is the
	// physical version, and what GTA IV's Euphoria characters do: the pelvis
	// goes dynamic, so a hit moves the whole body as a body, and whether he
	// stays up is decided by where his feet get to.
	//
	// Seven controllers in August failed at exactly this, and the
	// measurements say why: a leg carrying half the body's weight cannot
	// lift quickly, so by the time it swings the body has outrun it
	// (CHANGELOG 2026-08-09, "the body outruns the leg"). The real body
	// carries itself on the stance leg, which this rig's motors are too
	// soft to do quickly. So the stance leg's job is done by a **virtual
	// leg**: one force on the pelvis, the force a leg standing at the centre
	// of pressure would push with. It is bounded the way a leg is, and the
	// bounds are what make it honest:
	//
	//   * it pushes from a point inside the feet's footprint, never outside
	//     it -- so it can do what an ankle does, and nothing more. A fall
	//     that carries the capture point outside the footprint can only be
	//     caught by moving the footprint, which is a step;
	//   * along the line from that point to the centre of mass, the linear
	//     inverted pendulum's ground reaction, inside the friction cone;
	//   * only while a foot is on the ground and the stance leg can still
	//     reach it from the hip. A body that has outrun its feet loses its
	//     support and falls, which is what falling is.
	//
	// With the weight on the virtual leg, the swing foot is unloaded and
	// moves when asked, and the step lands where the capture point will be
	// when it lands -- the prediction the August steps lacked.

	// The centre of pressure, and so where a virtual leg pushes from: the
	// capture point if the footprint holds it, which brings the body to rest
	// there with time constant sqrt(h/g), or the nearest point of the
	// footprint to it, which is the best an ankle can do.
	static glm::vec2 ClosestInHull(const std::vector<glm::vec2>& hull, const glm::vec2& point)
	{
		if (hull.empty())
			return point;
		if (hull.size() < 3 || SignedDistance(hull, point) < 0.0f)
		{
			glm::vec2 best = hull[0];
			float bestDistance = std::numeric_limits<float>::max();
			for (size_t i = 0; i < hull.size(); i++)
			{
				glm::vec2 a = hull[i], b = hull[(i + 1) % hull.size()];
				glm::vec2 edge = b - a;
				float length2 = glm::dot(edge, edge);
				float t = length2 > 1e-10f ? glm::clamp(glm::dot(point - a, edge) / length2, 0.0f, 1.0f) : 0.0f;
				glm::vec2 on = a + edge * t;
				float distance = glm::length(point - on);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = on;
				}
			}
			return best;
		}
		return point;
	}

	// A foot touching something that is not himself.
	bool FootTouching(int side) const
	{
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			if (contact.PointCount == 0)
				continue;
			if (contact.A == m_Foot[side] && !IsCharacter(contact.B))
				return true;
			if (contact.B == m_Foot[side] && !IsCharacter(contact.A))
				return true;
		}
		return false;
	}

	// The sole's four corners, flattened -- the footprint, from the foot's
	// own box rather than from contact points. The manifold keeps 1.84 of a
	// planted foot's 4 corners on average (CHANGELOG 2026-08-10), which
	// would make the footprint flicker between a line and a polygon.
	void AppendSole(int side, std::vector<glm::vec2>& out) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		glm::vec3 h = foot.HalfExtents;
		for (float x : { -h.x, h.x })
			for (float z : { -h.z, h.z })
			{
				glm::vec3 corner = foot.Position + foot.Orientation * glm::vec3(x, -h.y, z);
				out.push_back({ corner.x, corner.z });
			}
	}

	glm::vec3 AnkleWorld(int side) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		return foot.Position + foot.Orientation * m_FootToAnkle;
	}

	float SoleHeight(int side) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		return foot.Position.y - foot.HalfExtents.y;
	}

	// Physically from standing or a walk, on the ground; a run keeps the
	// kinematic stagger. Stopping a walk costs as much as a 110 Ns shove
	// from standing, so a stumble from a walk does not try to stop: it
	// carries the walk's velocity and steps to keep it (the b offset in
	// UpdateStumble), and hands back to the gait once it is walking at it
	// again. Measured, 4 trials each: 20 Ns 16/16 walk on, 40 Ns 14/16,
	// 60-80 about half; a 1 Ns nudge is handed back after one or two steps
	// at 1.14-1.52 m/s against the walk's 1.30. Running, the capture point
	// is 1.3 m ahead as a matter of course and every physical hit from
	// 20 Ns felled him (64 of 64), so a run stays kinematic.
	bool StumblesPhysically() const
	{
		return m_PhysicalStumble && m_Speed <= m_StumbleEntrySpeed && m_Air == Air::Grounded;
	}

	// The crouch -- how far below standing the centre of mass is wanted --
	// at which a leg from `hip` to `ankle` is at `stretch` of its length.
	// Absolute, from where the hip would have to be: an earlier version
	// added "how much further down" to the crouch it already had, which
	// only ever ratcheted down, and a figure that had crouched once stayed
	// crouched and stepped until it gave up.
	float CrouchToReach(const glm::vec3& hip, const glm::vec3& ankle, float stretch) const
	{
		float legLength = m_ThighLength + m_ShinLength;
		glm::vec2 across(hip.x - ankle.x, hip.z - ankle.z);
		float reach = stretch * legLength;
		float vertical = std::sqrt(std::max(reach * reach - glm::dot(across, across), 0.0f));
		float hipAboveCom = hip.y - m_Balance.Com.y;
		float comWanted = ankle.y + vertical - hipAboveCom;
		return m_StumbleGround + m_StumbleComHeight - comWanted;
	}

	void BeginStumble()
	{
		UnpinFeet();
		m_Mode = Mode::Stumbling;
		m_StumbleTimer = 0.0f;
		m_StumbleSteps = 0;
		m_StumbleSwing = -1;
		m_StumbleCalm = 0.0f;
		m_StumbleUnsupported = 0.0f;
		m_StumbleCrouch = 0.0f;
		m_StumbleLastSwing = -1;
		m_TouchMemory[0] = m_TouchMemory[1] = 0.0f;
		m_StumbleGathering = false;
		m_StaggerVelocity = glm::vec3(0.0f);

		// The walk he was in, which the stumble keeps going rather than
		// trying to stop: a person bumped mid-stride walks on.
		glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		m_StumbleCarry = m_Speed > 0.1f ? glm::vec2(forward.x, forward.z) * m_Speed : glm::vec2(0.0f);

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Dynamic;
		pelvis.Awake = true;
		pelvis.SleepTimer = 0.0f;

		// Where he stands is where each foot is now, not where the gait
		// last meant it to be.
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = AnkleWorld(side);
			m_Swinging[side] = false;
		}
		m_StumbleGround = std::min(SoleHeight(0), SoleHeight(1));
		m_StumbleComHeight = m_StandComHeight;
	}

	// What a step must do, in the inverted pendulum's own terms: with the
	// centre of pressure held at `p`, the capture point runs away from it as
	// xi(t) = p + (xi0 - p) e^(t/tau). A foot landing in `remaining` seconds
	// has to go where xi will be then, not where it is now.
	static glm::vec2 PredictCapture(const glm::vec2& capture, const glm::vec2& pressure, float tau, float remaining)
	{
		return pressure + (capture - pressure) * std::exp(remaining / std::max(tau, 0.05f));
	}

	void UpdateStumble(float dt)
	{
		m_StumbleTimer += dt;
		const BalanceState& state = m_Balance;
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		// Down means on the ground, asked of the geometry as well as the
		// contacts. A planted foot drops out of the manifold for several
		// steps at a time as it settles -- every time it did, the support
		// vanished and the feet stopped taking turns (the same foot was
		// picked again because the other "was not down"), which walked a
		// forward shove out into the splits. A sole within 1.5 cm of the
		// ground under it is down.
		bool touching[2];
		for (int side = 0; side < 2; side++)
		{
			const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
			float ground = m_World.GroundHeightBelow(foot.Position, m_CharacterBodies, -1000.0f);
			m_FootGround[side] = ground;
			touching[side] = FootTouching(side) || SoleHeight(side) - ground < 0.015f;
			if (touching[side])
				m_TouchMemory[side] = 0.0f;
			else
				m_TouchMemory[side] += dt;   // how long it has been off the ground
		}
		// A swinging foot is not support, even when it scuffs the floor.
		bool support[2] = { touching[0] && m_StumbleSwing != 0, touching[1] && m_StumbleSwing != 1 };

		std::vector<glm::vec2> soles;
		for (int side = 0; side < 2; side++)
			if (support[side])
				AppendSole(side, soles);
		std::vector<glm::vec2> footprint = ConvexHull(soles);
		m_StumbleFootprint = footprint;

		for (int side = 0; side < 2; side++)
			if (support[side])
				m_StumbleGround = glm::mix(m_StumbleGround, m_FootGround[side], 0.2f);

		// Measured from the ground he is standing on. BalanceState's own
		// capture point measures height from y = 0, which is the demo's
		// floor and nobody else's.
		float height = std::max(state.Com.y - m_StumbleGround, 0.2f);
		float tau = std::sqrt(height / 9.81f);
		glm::vec2 capture = state.ComGround + glm::vec2(state.ComVelocity.x, state.ComVelocity.z) * tau;
		m_StumbleCapture = capture;
		// Where to push from: the capture point, plus a little beyond it
		// away from the footprint's middle -- the capture point controller
		// (xi' = (xi - p)/tau, so p past xi drives xi back towards the
		// middle). Pushing from xi itself stops the body wherever it is, and
		// measured, that left him at rest leaning over the edge of one foot
		// with the capture point 2 cm inside it -- caught, but not upright.
		glm::vec2 centre(0.0f);
		for (const glm::vec2& point : footprint)
			centre += point;
		if (!footprint.empty())
			centre /= (float)footprint.size();
		// Walking on, the ankles regulate speed instead of position: the
		// pendulum's x'' = (g/h)(x - p), so pressing at p = x - a h / g asks
		// for an acceleration a, and a is chosen to pull the body's velocity
		// to the walk's own -- forward and sideways at once -- as far as the
		// footprint lets it. Two earlier versions, both measured: pressing at
		// the capture point (the standing rule) braked him into a 0.45 m/s
		// shuffle; pressing at the footprint's middle let the speed run away,
		// a 20 Ns bump at a walk building to 3.5 m/s over two seconds until he
		// went over.
		bool walking = glm::length(m_StumbleCarry) > 0.1f;
		glm::vec2 aim = capture + m_StumbleCentring * (capture - centre);
		if (walking)
		{
			glm::vec2 velocity(state.ComVelocity.x, state.ComVelocity.z);
			glm::vec2 wanted = m_StumbleSpeedGain * (m_StumbleCarry - velocity);
			aim = state.ComGround - wanted * height / 9.81f;
		}
		glm::vec2 pressure = ClosestInHull(footprint, aim);
		float margin = footprint.size() >= 3 ? SignedDistance(footprint, capture) : -1.0f;
		m_StumbleMargin = margin;

		// --- the virtual leg ---------------------------------------------
		//
		// How much of a leg there is: none without a foot down, and fading
		// out as the hip runs out of reach of the stance foot. A leg at full
		// stretch pushes nothing -- and without this the body could be held
		// up at standing height while it slid past its own feet.
		float reachable = 0.0f;
		float legLength = m_ThighLength + m_ShinLength;
		for (int side = 0; side < 2; side++)
		{
			if (!support[side])
				continue;
			float stretch = glm::length(HipWorld(side) - AnkleWorld(side)) / legLength;
			// Fading out from 97% of the leg to just past straight (the
			// joints give a little). It first faded over 8% past straight,
			// and measured, a walking lunge then heaved the hips back up
			// hard enough to lift the stance foot 12 cm off the floor -- a
			// leg pushing beyond its own length -- and he fell off his own
			// support. Faded by 100%, it gave a straight-legged stance
			// almost none (the rig stands with its legs straight), so the
			// stumble crouches: m_StumbleRestCrouch.
			reachable = std::max(reachable, glm::clamp((1.01f - stretch) / 0.04f, 0.0f, 1.0f));
		}
		m_StumbleSupport = reachable;
		m_StumbleUnsupported = reachable > 0.0f ? 0.0f : m_StumbleUnsupported + dt;

		glm::vec3 force(0.0f);
		if (reachable > 0.0f && !footprint.empty())
		{
			// Height: held at standing, lowered by however much the swing leg
			// needs to reach -- a lunge drops the hips.
			float wanted = m_StumbleGround + m_StumbleComHeight - m_StumbleCrouch;
			float vertical = m_BodyMass * (9.81f
				+ m_StumbleHeightGain * (wanted - state.Com.y)
				- 2.0f * std::sqrt(m_StumbleHeightGain) * state.ComVelocity.y);
			vertical = glm::clamp(vertical, 0.0f, m_StumbleMaxSupport * m_BodyMass * 9.81f) * reachable;

			// Across: the pendulum's own ground reaction, F_y (x - p) / h,
			// which is what makes the body fall away from where it pushes.
			glm::vec2 lever = state.ComGround - pressure;
			glm::vec2 across = vertical * lever / height;
			float friction = m_StumbleFriction * vertical;
			if (glm::length(across) > friction)
				across *= friction / glm::length(across);

			force = glm::vec3(across.x, vertical, across.y);
			m_World.ApplyForce(m_Pelvis, force);
		}
		m_StumbleForce = force;

		// --- upright ------------------------------------------------------
		//
		// The stance hip's torque, applied straight to the pelvis: it keeps
		// the trunk over the legs. Only as much as there is a leg to push
		// against, so a figure that has lost its feet topples freely.
		{
			glm::quat target = glm::angleAxis(m_Facing, up);
			glm::quat error = target * glm::conjugate(pelvis.Orientation);
			if (error.w < 0.0f)
				error = -error;
			glm::vec3 axis(error.x, error.y, error.z);
			float sinHalf = glm::length(axis);
			glm::vec3 rotation = sinHalf > 1e-6f
				? axis / sinHalf * (2.0f * std::atan2(sinHalf, error.w)) : glm::vec3(0.0f);
			glm::vec3 torque = m_StumbleUprightGain * rotation - m_StumbleUprightDamping * pelvis.AngularVelocity;
			float limit = m_StumbleMaxTorque * reachable;
			if (glm::length(torque) > limit)
				torque *= limit / std::max(glm::length(torque), 1e-6f);
			m_World.ApplyTorque(m_Pelvis, torque);
		}

		// --- a step --------------------------------------------------------
		glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 right(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		glm::vec2 forward2(forward.x, forward.z), right2(right.x, right.z);

		if (m_StumbleCooldown > 0.0f)
			m_StumbleCooldown -= dt;

		// A foot left off the ground and not stepping -- outrun, or left
		// behind at the end of a split -- is brought back beside the other.
		// Measured without this: caught over one foot with the other hanging
		// 0.57 m away, out of reach of the floor, for the rest of the run.
		if (m_StumbleSwing < 0 && m_StumbleCooldown <= 0.0f && m_SteppingCatch && margin >= -m_StumbleTrigger)
		{
			for (int side = 0; side < 2; side++)
			{
				if (support[1 - side] && !support[side] && m_TouchMemory[side] > 0.1f)
				{
					m_StumbleSwing = side;
					m_StumbleGathering = true;
					m_StumbleSwingTimer = 0.0f;
					m_SwingFrom[side] = AnkleWorld(side);
					m_Swinging[side] = true;
					break;
				}
			}
		}

		if (m_StumbleSwing < 0 && m_StumbleCooldown <= 0.0f && m_SteppingCatch
			&& margin < -m_StumbleTrigger && (support[0] || support[1]))
		{
			glm::vec2 away = capture - pressure;
			float lateral = glm::dot(away, right2);
			float along = glm::dot(away, forward2);

			// Sideways: the foot on the side of the fall, or the legs cross.
			// Forwards or back: whichever foot is furthest behind the fall,
			// which gains the most by moving -- and only a foot that is not
			// the only one holding him up.
			// After the first step the feet take turns: the next step is the
			// other foot's, which is a shuffle sideways (it lands beside the
			// one that went out, never across it) and a run of steps
			// forwards. Stepping the same foot again splits the legs.
			int swing;
			if (m_StumbleSteps > 0 && m_StumbleLastSwing >= 0)
				swing = 1 - m_StumbleLastSwing;
			else if (std::fabs(lateral) > 0.7f * std::fabs(along))
				swing = lateral > 0.0f ? 1 : 0;
			else
			{
				glm::vec2 direction = glm::normalize(away);
				glm::vec2 foot0(AnkleWorld(0).x, AnkleWorld(0).z), foot1(AnkleWorld(1).x, AnkleWorld(1).z);
				swing = glm::dot(foot0, direction) < glm::dot(foot1, direction) ? 0 : 1;
			}
			if (!support[1 - swing])
				swing = 1 - swing;

			if (support[1 - swing])
			{
				m_StumbleSwing = swing;
				m_StumbleGathering = false;
				m_StumbleLastSwing = swing;
				m_StumbleSwingTimer = 0.0f;
				m_SwingFrom[swing] = AnkleWorld(swing);
				m_Swinging[swing] = true;
				m_StumbleSteps++;
			}
		}

		// And as far as the legs standing on the ground need, to stay bent:
		// a long split stance with the hips held high is two straight legs,
		// and a straight leg pushes nothing (the fade above). Measured
		// without it, a walking stumble crept on at half a metre a second
		// over legs at 98% stretch, never quite caught.
		float crouchWanted = m_StumbleRestCrouch;
		for (int side = 0; side < 2; side++)
			if (support[side])
				crouchWanted = std::max(crouchWanted, CrouchToReach(HipWorld(side), AnkleWorld(side), 0.94f));
		if (m_StumbleSwing >= 0)
		{
			int swing = m_StumbleSwing, stance = 1 - swing;
			m_StumbleSwingTimer += dt;
			float t = glm::clamp(m_StumbleSwingTimer / m_StumbleStepTime, 0.0f, 1.0f);

			// Re-aimed every step at where the capture point will be when
			// the foot lands, plus a little, so it lands just past it.
			float remaining = std::max(m_StumbleStepTime - m_StumbleSwingTimer, 0.0f);
			glm::vec2 landing = PredictCapture(capture, pressure, tau, remaining);
			glm::vec2 travel = landing - pressure;
			if (glm::length(travel) > 1e-4f && !walking)
				landing += glm::normalize(travel) * m_StumbleOvershoot;

			// Walking on: land short of the capture point by the offset that
			// keeps it moving. With the pressure at the stance foot and a
			// step every T, the capture point runs from b ahead of the foot
			// to b e^(T/tau); landing the next foot b behind that, a stride L
			// on, repeats it exactly when b e^(T/tau) - L = b, so
			//     b = L / (e^(T/tau) - 1),   L = v T.
			// b = 0 is landing on the capture point, which stops him -- the
			// standing stumble is the v = 0 case of the same rule.
			if (walking)
			{
				float period = m_StumbleStepTime + m_StumbleSettle;
				float stride = glm::length(m_StumbleCarry) * period;
				float offset = stride / std::max(std::exp(period / tau) - 1.0f, 0.05f);
				landing -= glm::normalize(m_StumbleCarry) * offset;
			}
			// A gathering foot goes beside the capture point, at a normal
			// stance width, to stand on rather than to catch with.
			if (m_StumbleGathering)
				landing = capture + right2 * (swing == 1 ? 0.5f : -0.5f) * m_StumbleStance;

			// Not across the other leg: the swing foot keeps to its own side
			// of the stance foot.
			glm::vec2 stanceFoot(AnkleWorld(stance).x, AnkleWorld(stance).z);
			float side = glm::dot(landing - stanceFoot, right2);
			float minimum = swing == 1 ? m_StumbleFootGap : -m_StumbleFootGap;
			if ((swing == 1 && side < minimum) || (swing == 0 && side > minimum))
				landing += right2 * (minimum - side);

			// And no further from the hip than a leg goes.
			glm::vec3 hip = HipWorld(swing);
			glm::vec2 hip2(hip.x, hip.z);
			glm::vec2 fromHip = landing - hip2;
			if (glm::length(fromHip) > m_StumbleReach)
				landing = hip2 + fromHip * (m_StumbleReach / glm::length(fromHip));
			m_StumbleTarget = landing;

			// The hips drop as far as the leg needs to get there.
			crouchWanted = std::max(crouchWanted, CrouchToReach(hip, glm::vec3(landing.x, m_StumbleGround + m_AnkleHeight, landing.y), 0.92f));

			glm::vec3 to(landing.x, m_StumbleGround + m_AnkleHeight, landing.y);
			glm::vec3 foot = glm::mix(m_SwingFrom[swing], to, glm::smoothstep(0.0f, 1.0f, t));
			foot.y += m_StumbleLift * std::sin(glm::pi<float>() * t);
			// Pressed into the ground at the end, so it lands rather than
			// arriving exactly at floor height and hovering there: one step
			// in the first trace timed out a few millimetres up.
			foot.y -= m_StumblePress * glm::smoothstep(0.7f, 1.0f, t);
			SolveLeg(swing, hip, foot, dt);

			// Nothing pulls the foot but the leg's own motors. A force on the
			// foot with its reaction on the pelvis was tried, and measured
			// it caught no more: the foot was never the slow part once the
			// virtual leg had the weight off it.
			// Landed means a real contact. The geometric "within 1.5 cm"
			// that keeps a standing foot counted as down also fires as a low
			// swing skims the floor, and measured, it called a step landed
			// half a metre short of where it was going.
			bool landed = (t > 0.5f && FootTouching(swing)) || m_StumbleSwingTimer > 1.5f * m_StumbleStepTime;
			if (landed)
			{
				m_Planted[swing] = AnkleWorld(swing);
				m_Swinging[swing] = false;
				m_StumbleSwing = -1;
				m_StumbleCooldown = m_StumbleSettle;
			}
		}
		crouchWanted = std::min(crouchWanted, m_StumbleMaxCrouch);
		m_StumbleCrouch += glm::clamp(crouchWanted - m_StumbleCrouch, -m_StumbleCrouchRate * dt, m_StumbleCrouchRate * dt);

		// The stance legs hold the feet where they were put.
		for (int side = 0; side < 2; side++)
			if (side != m_StumbleSwing)
				SolveLeg(side, HipWorld(side), m_Planted[side], dt);

		// Arms out for balance, further the worse it gets -- the windmill
		// that reads at once as someone about to go over.
		float alarm = glm::clamp(-margin / 0.3f, 0.0f, 1.0f);
		for (int side = 0; side < 2; side++)
		{
			float out = (side == 0 ? 1.0f : -1.0f) * (0.35f + 0.9f * alarm);
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation = glm::normalize(
				glm::angleAxis(out, glm::vec3(0.0f, 0.0f, 1.0f))
				* glm::angleAxis(-0.4f * alarm, glm::vec3(1.0f, 0.0f, 0.0f)) * m_ShoulderRest[side]);
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = -0.5f * alarm;
		}
		m_World.GetJoint(m_SpineJoint).MotorTargetRotation = m_SpineRest;

		// --- out of it, one way or the other -------------------------------
		glm::vec3 trunkUp = m_World.GetBody(m_Torso).Orientation * up;
		float tilt = std::acos(glm::clamp(trunkUp.y, -1.0f, 1.0f));
		if (tilt > glm::radians(m_StumbleGiveUpTilt))
		{
			GoRagdoll("toppled");
			return;
		}
		if (state.Com.y - m_StumbleGround < m_StumbleGiveUpHeight)
		{
			GoRagdoll("buckled");
			return;
		}
		if (m_StumbleUnsupported > 0.3f)
		{
			GoRagdoll("lost his feet");
			return;
		}
		// Walking on, steps are how he moves at all, so more of them are
		// allowed before it counts as not catching it.
		int maxSteps = walking ? 2 * m_StumbleMaxSteps : m_StumbleMaxSteps;
		if (m_StumbleSteps > maxSteps || m_StumbleTimer > m_StumbleMaxTime)
		{
			GoRagdoll("could not catch it");
			return;
		}

		// Caught: both feet down, the capture point well inside them, nearly
		// still, and staying so -- then the walk takes him back from where
		// he stands.
		glm::vec2 horizontalVelocity(state.ComVelocity.x, state.ComVelocity.z);
		bool calm = m_StumbleSwing < 0 && support[0] && support[1] && margin > m_StumbleCalmMargin
			&& glm::length(horizontalVelocity) < m_StumbleCalmSpeed && tilt < glm::radians(8.0f);
		if (walking)
		{
			// Caught, walking: a step has landed, both feet are down, and he
			// is moving at the walk's own velocity again, upright -- then the
			// gait takes him back without stopping.
			calm = m_StumbleSwing < 0 && support[0] && support[1] && m_StumbleSteps > 0
				&& glm::length(horizontalVelocity - m_StumbleCarry) < m_StumbleCalmWalk && tilt < glm::radians(10.0f);
			if (calm)
			{
				EndStumble();
				return;
			}
		}
		m_StumbleCalm = calm ? m_StumbleCalm + dt : 0.0f;
		if (m_StumbleCalm > m_StumbleCalmTime)
			EndStumble();
	}

	// Back to walking from wherever the stumble left him.
	void EndStumble()
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 front = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		if (glm::length(glm::vec2(front.x, front.z)) > 0.01f)
			m_Facing = std::atan2(front.x, front.z);
		m_StandHeight = pelvis.Position.y;
		glm::vec3 planted[2] = { AnkleWorld(0), AnkleWorld(1) };
		GoControlled();
		pelvis.Velocity = glm::vec3(0.0f);
		// Walking on at the speed he was walking, along the way he faces.
		float carried = glm::length(m_StumbleCarry);
		if (carried > 0.1f)
		{
			m_Facing = std::atan2(m_StumbleCarry.x, m_StumbleCarry.y);
			m_Speed = carried;
			m_SpeedRate = 0.0f;
			m_LastSpeed = carried;
		}
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = planted[side];
			m_SwingFrom[side] = planted[side];
			m_SwingTo[side] = planted[side];
			m_Swinging[side] = false;
		}
		for (int side = 0; side < 2; side++)
		{
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation = m_ShoulderRest[side];
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = 0.0f;
		}
	}

	// ---------------------------------------------------------------------
	// Walking
	// ---------------------------------------------------------------------
	//
	// The pelvis is driven straight from input, and the legs are a gait rather
	// than a controller: hips swing in antiphase, knees bend on the half of
	// the cycle their foot is coming through. It is an animation, driven
	// through the motors so the legs are still physical -- they collide with
	// the world and are pushed about by it, and they are dragged back towards
	// the gait rather than snapped to it.
	// x'' = w^2 (target - x) - 2 w x': reaches the target without
	// overshoot, and with no step in its acceleration anywhere.
	static void Spring(float& x, float& rate, float target, float omega, float dt)
	{
		rate += (omega * omega * (target - x) - 2.0f * omega * rate) * dt;
		x += rate * dt;
	}

	void DriveWalk(float dt)
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);

		// --- steering, with momentum ----------------------------------------
		//
		// Nothing here is instant. A body with mass does not change direction
		// on a key press, and snapping the velocity or the facing is most of
		// what makes a character feel like a cursor rather than a person.
		// Camera-relative, which is what a third-person control scheme means:
		// W goes away from the camera whichever way it is pointing, not along
		// some fixed world axis. Steering in world space is the thing that
		// makes a follow camera feel like it is fighting you.
		float camYaw = glm::radians(m_Control.Yaw);
		glm::vec3 camForward(std::cos(camYaw), 0.0f, std::sin(camYaw));
		glm::vec3 camRight(-std::sin(camYaw), 0.0f, std::cos(camYaw));

		if (glm::length(camForward) > 0.0f) camForward = glm::normalize(camForward);
		if (glm::length(camRight) > 0.0f) camRight = glm::normalize(camRight);

		glm::vec3 wish(0.0f);
		if (m_Control.Enabled)
		{
			if (m_Control.Forward) wish += camForward;
			if (m_Control.Back) wish -= camForward;
			if (m_Control.Left) wish -= camRight;
			if (m_Control.Right) wish += camRight;
		}
		wish.y = 0.0f;

		float wishLength = glm::length(wish);
		if (wishLength > 0.0f)
			wish /= wishLength;

		// The heading turns towards where it is going at a limited rate, the
		// short way round. A character that pivots instantly reads as weightless
		// however good the rest of the gait is.
		float turnLeft = 0.0f;
		if (wishLength > 0.0f)
		{
			float wanted = std::atan2(wish.x, wish.z);
			float error = wanted - m_Facing;
			while (error >  glm::pi<float>()) error -= glm::two_pi<float>();
			while (error < -glm::pi<float>()) error += glm::two_pi<float>();

			// Slower turning the faster he goes: 4 rad/s on the spot, about
			// half that at a walk. At full rate while walking the hips swung
			// round faster than the legs could follow them.
			float turnRate = m_TurnRate / (1.0f + m_Speed / std::max(m_WalkSpeed, 0.01f));
			m_Facing += glm::clamp(error, -turnRate * dt, turnRate * dt);
			turnLeft = std::fabs(error);
		}

		// Speed eases towards the target and coasts to a stop, and the figure
		// walks along its *facing* rather than along the input -- so a turn is
		// a curve rather than a sidestep.
		bool wantsRun = m_Control.Enabled && m_Control.Run;

		// Eased, because everything about the run differs from the walk --
		// stride, stance fraction, arm swing, lean -- and snapping between two
		// gaits mid-step is exactly the tearing that a blend exists to avoid.
		// On a critically damped spring, like the speed below.
		Spring(m_RunBlend, m_RunBlendRate, wantsRun ? 1.0f : 0.0f, m_RunBlendResponse, dt);
		m_RunBlend = glm::clamp(m_RunBlend, 0.0f, 1.0f);

		float topSpeed = glm::mix(m_WalkSpeed, m_RunSpeed, m_RunBlend);

		// Slower the more there is still to turn: full speed straight ahead,
		// half at a right angle, a turn on the spot to go back the way he
		// came. Turning at full speed, the hips ran a 0.33 m circle (v / turn
		// rate) and left the planted foot 1.5 m behind them.
		float wantedSpeed = wishLength > 0.0f ? topSpeed * 0.5f * (1.0f + std::cos(turnLeft)) : 0.0f;
		// Speed follows a critically damped spring rather than ramping at a
		// fixed rate. A ramp switches its acceleration on and off in a single
		// step, and everything scaled by speed -- the bob above all -- kinks
		// there: the hips took a 10-16 m/s^2 jolt at every start, stop and
		// change of direction, which is the hitch a ramp reads as. The spring
		// eases in and out; at m_SpeedResponse = 6 a walk is reached at a
		// peak of about 2.9 m/s^2 (omega * dv / e), where the ramp used 3.
		Spring(m_Speed, m_SpeedRate, wantedSpeed, m_SpeedResponse, dt);
		if (m_Speed < 0.0f)
		{
			m_Speed = 0.0f;
			m_SpeedRate = std::max(m_SpeedRate, 0.0f);
		}

		glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 wanted = forward * m_Speed;

		// --- catching himself ------------------------------------------------
		//
		// A shove moves the body before it moves the feet, which is what being
		// off balance *is*. The root carries the push and bleeds it off; the
		// legs are already placed relative to the root, so they chase it
		// without being told to -- and because the gait phase advances with
		// distance covered, being shoved fast makes the steps quick on its own.
		if (glm::length(m_StaggerVelocity) > 1e-4f)
		{
			// Can he still catch this? Asked every frame rather than once at
			// the moment of impact, so a stagger that is decaying stays
			// survivable and one that arrives too fast goes down at once.
			if (glm::length(m_StaggerVelocity) > CatchableSpeed())
			{
				GoRagdoll("outrun");
				return;
			}

			wanted += m_StaggerVelocity;

			// Exponential bleed-off: this is the recovery, and how fast it
			// runs is the difference between a stumble and a fall.
			m_StaggerVelocity *= std::max(1.0f - m_StaggerRecovery * dt, 0.0f);

			if (glm::length(m_StaggerVelocity) < m_StaggerSettle)
				m_StaggerVelocity = glm::vec3(0.0f);
		}

		if (m_StaggerTimer > 0.0f)
			m_StaggerTimer = std::max(m_StaggerTimer - dt, 0.0f);

		// --- the gait --------------------------------------------------------
		// Phase advances with distance covered, not with time, so the legs do
		// not cycle while standing still and do not moonwalk when the speed
		// changes.
		float gaitSpeed = m_Speed + glm::length(m_StaggerVelocity);
		m_GaitPhase += (gaitSpeed / std::max(CurrentStride(), 0.01f)) * glm::two_pi<float>() * dt;
		if (gaitSpeed < 0.05f)
			m_GaitPhase = 0.0f;

		float stride = std::min(gaitSpeed / std::max(m_WalkSpeed, 0.01f), 1.0f);

		// Stance fraction. A person walks with about 62% of each leg's cycle
		// on the ground -- the overlap is double support -- and runs with
		// about 35%, where the gaps are flight.
		m_Duty = glm::mix(m_WalkDuty, m_RunDuty, m_RunBlend);

		// --- weight ----------------------------------------------------------
		//
		// Three things sell a walk as having mass, and none of them are the
		// legs. All are driven off the same phase so they cannot drift apart.
		//
		//   * **Bob.** The body is lowest when both feet are down and the
		//     weight transfers, highest at mid-stance over a straight leg.
		//     That is twice per stride, hence the doubled phase.
		//   * **Sway.** The hips move over whichever foot is carrying, once
		//     per stride. Without it a walk looks like a puppet slid along a
		//     rail.
		//   * **The drop.** A short, sharp downward nudge as the foot lands,
		//     which is what actually reads as the step *hitting* the ground --
		//     a smooth sinusoid alone feels like floating.
		// The dip is not a style choice, it is geometry. With the feet half a
		// stride apart the hip *cannot* stay at full leg length -- the leg is
		// the hypotenuse of a triangle whose base is half the stride -- so the
		// body has to drop by the difference or the foot target is out of
		// reach and the leg just extends and falls short. It grows as the
		// square of the stride, which is why a long stride sinks so much more
		// than a short one.
		// How far the hips dip at double support so the legs reach the
		// stride. The feet roll heel to toe now (DriveFoot), which is reach
		// the hips no longer have to find by dropping: the dip is worked out
		// for a foot m_RollReach nearer. With a flat foot it was 5.1 cm and,
		// with the bob on top, the hips went up and down 7 cm a step at up
		// to 12 m/s^2 -- a walk of lurches.
		float half = std::max(FootReach(stride) - m_RollReach * stride, 0.0f);
		float legMax = (m_ThighLength + m_ShinLength) * 0.999f;
		float needed = legMax - std::sqrt(std::max(legMax * legMax - half * half, 0.0f));

		// Walking: zero at mid-stance, full at double support -- the vault
		// over a straight stance leg. (The other way round is a waddle.)
		float low = 0.5f * (1.0f - std::cos(2.0f * m_GaitPhase));
		float walkBob = -(needed + m_BobAmount * stride) * low;

		// Running: the spring-mass run, which is what a runner's hips
		// actually do. In stance they dip on a half sine of depth B; in flight
		// they are a projectile. Matching velocities where the two meet
		// fixes B instead of leaving it a knob: flight of T_f leaves the
		// ground at v = g T_f / 2, the half sine leaves at B pi / T_s, so
		// B = v T_s / pi, and the apex is v^2 / 2g. At 4 m/s that is a 3.0 cm
		// dip, a 1 cm arc and 6.6 m/s^2 at the bottom. The previous version
		// put the walk's sine under a flight parabola that started with a
		// kink, and peaked at 44 m/s^2.
		float runBob = 0.0f;
		float running = glm::clamp((0.5f - m_Duty) / std::max(0.5f - m_RunDuty, 0.01f), 0.0f, 1.0f);
		if (running > 0.0f)
		{
			float cycle = CurrentStride() / std::max(m_Speed, 0.01f);
			float stanceTime = m_Duty * cycle;
			float flightTime = (0.5f - m_Duty) * cycle;
			float takeoff = 9.81f * flightTime * 0.5f;
			float depth = takeoff * stanceTime / glm::pi<float>();
			float apex = takeoff * takeoff / (2.0f * 9.81f);
			float air = Airborne();
			if (air >= 0.0f)
				runBob = apex * 4.0f * air * (1.0f - air);
			else
			{
				int stance = CyclePosition(0) < m_Duty ? 0 : 1;
				float t = glm::clamp(CyclePosition(stance) / std::max(m_Duty, 0.01f), 0.0f, 1.0f);
				runBob = -depth * std::sin(glm::pi<float>() * t);
			}
			runBob -= needed;
		}
		float bob = glm::mix(walkBob, runBob, running);

		glm::vec3 right(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		// Sway follows *which foot is carrying*, not the raw phase.
		//
		// Tied to sin(gaitPhase) it was right for one gait and wrong for the
		// other, because which foot is down at a given phase depends on the
		// duty factor: measured, the pelvis sat 0.1033 m from the stance foot
		// and 0.0955 from the swing foot at a walk -- leaning away from the leg
		// carrying it -- while the run happened to come out correct. Keyed to
		// the cycle position it is right for both by construction.
		//
		// Peaks towards side 0 at side 0's mid-stance, which is a quarter of
		// the way through its stance either side of it.
		float carry = -std::cos(glm::two_pi<float>()
			* (CyclePosition(0) - m_Duty * 0.5f));

		m_Carry = carry;
		glm::vec3 sway = right * (m_SwayAmount * carry * stride);

		// --- the ground under him, and the ground he is about to meet --------
		//
		// Two probes. One under the root sets how high he stands; one a stride
		// ahead tells him a step is coming, so the foot is already lifting
		// before it would have hit the riser.
		glm::vec3 under = m_RootAnchor;

		// Fired from the hips rather than from the anchor, and that is not a
		// detail. `m_RootAnchor.y` is written once when control begins and only
		// ever moved *horizontally* afterwards, so walking uphill leaves the
		// origin behind: once the surface rises past it, a probe for "ground
		// below this point" correctly answers that there is none, the sentinel
		// comes back, and the fall test below ragdolls a character who is
		// standing on solid ground.
		//
		// That put a hard ceiling of one standing height on how far he could
		// climb above wherever he spawned, at any angle. Measured on a 15 degree
		// ramp: spawn ground 1.61 m, anchor 2.70 m, and he went down at z =
		// -9.92 -- which is the z where the ramp surface reaches 2.70 m, to the
		// centimetre. It never showed up on the flat demo because the floor
		// there never rises, and never showed up on the generated map because
		// the spawn picks the flattest spot going and he had not yet been walked
		// far enough uphill from it.
		//
		// The pelvis is the honest origin: it cannot be inside the ground while
		// he is standing on it. The foot probes in PlaceFeet lift their own
		// origins for the same reason.
		under.y = pelvis.Position.y;

		// Probed against a floor far below rather than the default zero, so
		// "there is nothing here" is answerable at all. With the default the
		// probe reports y = 0 off the edge of the world and the character
		// walks out over the void on an invisible floor.
		float groundHere = m_World.GroundHeightBelow(under, m_CharacterBodies, -1000.0f);

		// --- the jump --------------------------------------------------------
		//
		// The root is kinematic, so the jump is not a force applied to a mass:
		// it is the height being integrated ballistically instead of being
		// pinned to the ground. Which is the same arithmetic, and lands in a
		// predictable place -- the apex is v^2/2g and nothing else touches it.
		// A jump has to come off a foot that is on the ground. Standing still
		// that is always true and the press launches at once -- the two-foot
		// hop, which is what a person does from a standstill. Moving, it waits
		// for the next foot-plant, which is what turns the jump from a lift
		// into a hurdle.
		//
		// The wait cannot be unconditional: m_GaitPhase is pinned to 0 below
		// 0.05 m/s, so standing still there is no plant coming and a press
		// would hang forever.
		if (m_WantsJump && m_Air == Air::Grounded)
			m_JumpPending = true;
		m_WantsJump = false;

		if (m_JumpPending && m_Air == Air::Grounded)
		{
			bool moving = m_Speed > m_JumpGateSpeed;
			int planting = -1;

			if (moving)
			{
				// Just landed: within the first sliver of its stance.
				for (int side = 0; side < 2; side++)
					if (CyclePosition(side) < m_JumpGateWindow)
						planting = side;
			}

			if (!moving || planting >= 0)
			{
				// The leg that is *not* pushing off is the one that swings
				// through in front, which is what a hurdle looks like.
				m_JumpLead = planting >= 0 ? 1 - planting : -1;
				m_Air = Air::Crouching;
				m_AirTimer = 0.0f;
				m_JumpPending = false;
			}
		}

		if (m_Air == Air::Crouching)
		{
			// You cannot jump without dipping first -- the legs have to be
			// bent to have anything to extend. Skipping this is most of what
			// makes a jump look like a lift.
			m_AirTimer += dt;
			if (m_AirTimer >= m_JumpCrouchTime)
			{
				// Down in the crouch: now the push. The hips rise from the
				// crouch's depth d to standing height under a constant
				// acceleration that leaves them at the take-off speed for
				// the jump's height, a = v^2 / 2d = g h / d (38 m/s^2, about
				// 4 g, at h = 0.55 and d = 0.14). The take-off used to be
				// instant: the hips popped up out of the crouch and reached
				// 3.3 m/s in one step, 560 m/s^2.
				m_Air = Air::Pushing;
				m_JumpOffset = -m_JumpCrouchDepth;
				m_JumpVelocity = 0.0f;
			}
		}
		else if (m_Air == Air::Pushing)
		{
			float takeoff = std::sqrt(2.0f * 9.81f * m_JumpHeight);
			float push = takeoff * takeoff / (2.0f * std::max(m_JumpCrouchDepth, 0.01f));
			m_JumpVelocity += push * dt;
			m_JumpOffset += m_JumpVelocity * dt;
			if (m_JumpOffset >= 0.0f)
				m_Air = Air::Flying;
		}
		else if (m_Air == Air::Flying)
		{
			m_JumpVelocity -= 9.81f * dt;
			m_JumpOffset += m_JumpVelocity * dt;

			if (m_JumpOffset <= 0.0f && m_JumpVelocity < 0.0f)
			{
				// Landing absorbs: the hips carry on down at the speed they
				// landed with and settle, d(t) = v t e^(-t/tau) -- continuous
				// in speed, deepest (v tau / e) at t = tau. Stopped dead, as
				// they were, the landing was another one-step jolt.
				m_LandSpeed = -m_JumpVelocity;
				// Starting from however far this step went past the ground,
				// so the hips carry on from where they are rather than being
				// lifted back to it (a one-step jolt of -112 m/s^2). The
				// clock gains dt again below, before it is used.
				m_LandClock = -m_JumpOffset / std::max(m_LandSpeed, 0.1f) - dt;
				m_JumpOffset = 0.0f;
				m_JumpVelocity = 0.0f;
				m_Air = Air::Grounded;
				// Landing is absorbed rather than stopped dead.
				m_LandTimer = m_LandTime;

				// And the gait resumes with the lead foot arriving, rather
				// than wherever the phase had wandered to while he was in the
				// air. That is what makes a running jump land *into* the next
				// stride instead of stumbling back into step -- and it does
				// the job a per-foot landing timer was going to do, without
				// needing one.
				if (m_JumpLead >= 0)
				{
					m_GaitPhase = 1.5f * glm::pi<float>()
						- (m_JumpLead == 0 ? 0.0f : glm::pi<float>());
				}
				m_JumpLead = -1;
			}
		}

		if (m_LandTimer > 0.0f)
			m_LandTimer = std::max(m_LandTimer - dt, 0.0f);
		if (m_LandSpeed > 0.0f)
		{
			m_LandClock += dt;
			if (m_LandClock > 6.0f * m_LandAbsorb)
				m_LandSpeed = 0.0f;
		}

		// A drop with nothing under it. Measured against the height he is
		// holding rather than the pelvis, so a jump does not read as a fall,
		// and skipped outright while airborne.
		if (m_Air == Air::Grounded
			&& m_StandHeight - (groundHere + m_StandClearance) > m_FallHeight)
		{
			// Nothing to stand on. This is the same door the shove and the
			// impact use: stop driving him and let the physics have him.
			GoRagdoll("fell");
			return;
		}

		// Eased rather than snapped. The probe is a step function at the edge
		// of a box and following it exactly makes the hips jump by the height
		// of the step in one frame, which is the opposite of weighty.
		// Slightly bent, always.
		//
		// The rig was built with the leg dead straight -- hip 0.90 m above the
		// ankle, and a leg of exactly 0.90 m -- so the foot could not be placed
		// a centimetre ahead of the hip without the target going out of reach.
		// The IK dutifully extended the leg as far as it went and the foot
		// finished 0.4 m short every stride, which is what the scuffing was.
		// A person walks with bent knees for the same reason.
		float wantedStand = groundHere + m_StandClearance - m_Crouch;
		// Eased twice: the ground under the hips is a step function at a
		// riser's edge, so the target is smoothed first and then followed
		// on a spring. Rate-limited, as it was, the hips' climb switched from
		// still to 1.2 m/s in one step -- a 72 m/s^2 jolt at every stair.
		// Not with the target's rate fed forward, which was tried: it cut
		// the lag on a long climb, but at a riser the smoothed target's rate
		// spikes and was fed straight in -- 358 m/s^2 on the stairs. The lag
		// that remains on a sustained climb is about 2 r / w + 0.08 r.
		m_StandTarget += (wantedStand - m_StandTarget) * (1.0f - std::exp(-dt / m_StandSmoothing));
		// Never pulled down faster than he would fall: stepping off a 1.2 m
		// ledge, the spring dropped the hips at 47 m/s^2.
		float rateBefore = m_StandRate;
		Spring(m_StandHeight, m_StandRate, m_StandTarget, m_StandResponse, dt);
		if (m_StandRate < rateBefore - 9.81f * dt)
		{
			m_StandHeight -= (m_StandRate - (rateBefore - 9.81f * dt)) * dt;
			m_StandRate = rateBefore - 9.81f * dt;
		}

		// How much higher the next foothold is, as a fraction of the tallest
		// step worth lifting for, is worked out per leg in PlaceFeet -- the
		// foot's own landing spot is the honest place to ask. This is only
		// the larger of the two, for the panel to show.
		m_StepUp = std::max(m_Climb[0], m_Climb[1]);

		// The crouch before the launch, and the absorb after the landing --
		// both dips, both on the way to or from the same jump.
		// Eased down into the crouch (linear, it started at full speed).
		float crouch = m_Air == Air::Crouching
			? m_JumpCrouchDepth * glm::smoothstep(0.0f, 1.0f, glm::clamp(m_AirTimer / m_JumpCrouchTime, 0.0f, 1.0f))
			: 0.0f;
		if (m_LandSpeed > 0.0f)
			crouch += m_LandSpeed * m_LandClock * std::exp(-m_LandClock / m_LandAbsorb);

		// Dropping while recovering is not a flourish. The capture point sits
		// v*sqrt(h/g) ahead, so taking height out of h shortens the reach the
		// feet have to make -- it is the one thing a body can do about its own
		// balance without moving its feet at all.
		crouch += m_RecoverCrouch * glm::clamp(
			glm::length(m_StaggerVelocity) / std::max(CatchableSpeed(), 0.01f), 0.0f, 1.0f);

		glm::vec3 drift = right * Idle(dt, stride, right);
		glm::vec3 target = m_RootAnchor + sway + drift;
		target.y = m_StandHeight + bob + m_JumpOffset - crouch;

		// The anchor is where the character would be with no gait on top; the
		// bob and sway are an offset from it rather than something the root
		// integrates, or they would accumulate into a drift.
		m_RootAnchor += wanted * dt;

		float toTarget = 1.0f / std::max(dt, 1e-4f);
		pelvis.Velocity = (target - pelvis.Position) * toTarget;

		// Lean into acceleration, and roll slightly with the sway. Small
		// angles, but a perfectly upright pelvis is the other half of looking
		// weightless.
		// Two different leans, and only one of them is about speed.
		//
		// The steady one grows with how fast he is going. The other is the
		// transient lean *into acceleration*, and that one is not a taste
		// parameter: a body accelerating at `a` has to put its weight `atan(a/g)`
		// ahead of its feet or the acceleration has nothing to come from. It
		// reverses under braking, which is why stopping looks like leaning back.
		float accel = (m_Speed - m_LastSpeed) / std::max(dt, 1e-4f);
		m_LastSpeed = m_Speed;

		// Smoothed, because the raw derivative of a per-step speed is noise.
		m_LeanAccel += glm::clamp(accel - m_LeanAccel, -m_LeanAccelRate * dt,
			m_LeanAccelRate * dt);

		float lean = (m_Speed / std::max(m_WalkSpeed, 0.01f)) * m_LeanAmount
			+ std::atan(m_LeanAccel / 9.81f) * m_LeanFromAccel;

		lean = glm::clamp(lean + m_Posture, -m_LeanBack, m_LeanForward);

		// **Positive tips the top of the body towards +z, which is the way the
		// figure faces.** This was negated, so he leaned *backwards* the whole
		// time -- measured at -5.7 deg walking and -17.6 deg running, which is
		// exactly the magnitude asked for, pointing the wrong way.
		glm::quat facing = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));
		glm::quat pitch = glm::angleAxis(lean * m_LeanPelvisShare,
			glm::vec3(1.0f, 0.0f, 0.0f));
		// Same signal, so the tilt and the shift cannot drift apart. Positive
		// about +z tips the top of the body towards -x, which is the same side
		// the hips have just moved away from -- the trunk leans over the leg
		// that is carrying.
		glm::quat roll = glm::angleAxis(-m_RollAmount * m_Carry * stride - m_IdleRoll,
			glm::vec3(0.0f, 0.0f, 1.0f));

		pelvis.Orientation = glm::normalize(facing * pitch * roll);
		pelvis.UpdateInertiaWorld();

		// The remainder is carried by the spine, so the lean is a *bend* rather
		// than a plank tipping over. Measuring found the torso tracking the
		// pelvis to within 0.1 deg -- the spine motor is strong enough at 260
		// N.m to carry it rigidly -- so leaning the pelvis alone moved the
		// whole figure as one piece, which is not what a person does. The cone
		// limit here is 25 deg, so the share has to stay well inside that.
		// The breath lifts the chest: the spine eases back a touch on the
		// in-breath.
		m_World.GetJoint(m_SpineJoint).MotorTargetRotation = glm::normalize(
			glm::angleAxis(lean * (1.0f - m_LeanPelvisShare) - m_Breath, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(0.7f * m_IdleRoll, glm::vec3(0.0f, 0.0f, 1.0f))
			* m_SpineRest);

		PlaceFeet(dt, forward, right, stride);
		SwingArms(stride);
	}

	// Arms swing opposite the leg on the same side. That is not decoration:
	// the legs are throwing angular momentum about the vertical axis every
	// step and the arms are what cancels it, which is why walking with your
	// arms folded feels like work. Antiphase is the whole trick -- in phase
	// looks like a toy soldier.
	void SwingArms(float stride)
	{
		for (int side = 0; side < 2; side++)
		{
			// Same phase the leg on this side uses, turned half a cycle round.
			float phase = m_GaitPhase + (side == 0 ? 0.0f : glm::pi<float>())
				+ glm::pi<float>();

			// Each arm's swing wanders a little, and differently.
			float wander = 1.0f + 0.12f * VariationAmount() * Noise(m_Clock / 1.7f, 41u + (uint32_t)side);
			float swing = m_ArmSwing * wander * (1.0f + m_RunBlend) * std::sin(phase) * stride;

			// About the shoulder's pitch axis, in the torso's frame, on top of
			// the pose the rig was built in.
			glm::quat turn = glm::angleAxis(swing + m_IdleArm[side], glm::vec3(1.0f, 0.0f, 0.0f));
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation =
				glm::normalize(turn * m_ShoulderRest[side]);

			// A little more bend on the forward swing than the back, which is
			// what an arm actually does -- it does not stay straight.
			float bend = m_ElbowBend * (1.0f + m_RunBlend)
				* (0.6f + 0.4f * std::sin(phase)) * stride;
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = -bend;
		}
	}

	// ---------------------------------------------------------------------
	// Feet
	// ---------------------------------------------------------------------
	//
	// The gait used to swing the hips and bend the knees on a sine wave and
	// let the feet fall where they might. It reads as walking from a distance
	// and falls apart up close: the feet slide along the ground through the
	// stance, because nothing was holding them anywhere.
	//
	// Now the *foot* is what the gait describes, and the leg is solved to
	// reach it. Through the stance half of the cycle a foot is pinned to the
	// spot it landed on and the body travels over it -- which is what walking
	// actually is -- and through the swing half it arcs to where it will land
	// next. Two-link inverse kinematics turns that world position back into a
	// hip direction and a knee angle.
	// Driven feet. While he is under control, each foot is kinematic and
	// goes exactly where the gait puts it; the thighs and shins between
	// the driven hips and the driven feet stay physical, so the legs still
	// swing, collide and react. When he is hit, falls or gets up the feet
	// are let go (UnpinFoot) and are bodies again.
	//
	// Why: held by the leg motors alone, a foot lagged everything. Planted,
	// it was dragged along by the hips -- measured, it slid at 0.52 m/s of a
	// 1.3 m/s walk and 3.25 of a 4 m/s run, and standing he never touched
	// the floor. Pinned in stance but motor-driven in the swing, it landed
	// short of where it was aimed, out of reach, and tore loose again.
	//
	// In stance the foot rolls as a person's does: on its heel while the
	// hips are behind it (toe up, the heel strike), flat under them, and on
	// its toes once they have gone past (heel up) -- each only as far as
	// keeps the hip within reach. Worked out on the walk's own numbers, a
	// flat foot 0.44 m behind a hip 0.85 m up is 0.96 m from it on a 0.90 m
	// leg; rolling onto the toes is the difference.
	void DriveFoot(int side, glm::vec3 ankle, bool stance, float swingT, float dt)
	{
		if (!m_DriveFeet)
		{
			UnpinFoot(side);
			m_FootTarget[side] = ankle;
			return;
		}
		GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		if (!m_Pinned[side])
		{
			m_Pinned[side] = true;
			m_PinYaw[side] = m_Facing;
			foot.Type = GS::BodyType::Kinematic;
			foot.AngularVelocity = glm::vec3(0.0f);

			// The foot's turn is decided, so the ankle has nothing to hold:
			// its cone and motor only fought the knee and hip. A flat foot
			// under a shin leaning past the 35 degree cone opened the knee
			// joint by 8-14 cm.
			GS::Joint3D& ankleJoint = m_World.GetJoint(m_AnkleJoint[side]);
			ankleJoint.ConeTwistEnabled = false;
			ankleJoint.MotorEnabled = false;
			SetLegsColliding(false);
		}

		// Turning on a planted foot: it pivots once the body has turned more
		// than m_PinTwist past it (held at the yaw it landed with, the hips
		// turned over it and wrenched the knee backwards). In the air it
		// simply turns with him.
		float twist = m_Facing - m_PinYaw[side];
		while (twist > glm::pi<float>()) twist -= glm::two_pi<float>();
		while (twist < -glm::pi<float>()) twist += glm::two_pi<float>();
		float allowed = stance ? m_PinTwist : 0.0f;
		if (std::fabs(twist) > allowed)
			m_PinYaw[side] += twist - glm::sign(twist) * allowed;

		glm::quat flat = glm::normalize(ConformToGround(ankle, side) * glm::angleAxis(m_PinYaw[side], up));
		glm::vec3 axis = flat * glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 ahead = flat * glm::vec3(0.0f, 0.0f, 1.0f);
		float legLength = m_ThighLength + m_ShinLength;
		float limit = 0.97f * legLength;
		glm::vec3 hip = HipWorld(side) + m_World.GetBody(m_Pelvis).Velocity * dt;

		// Positive pitch lifts the heel (rotating about the foot's +x turns
		// +z towards -y), negative lifts the toe.
		float pitch = 0.0f;
		if (stance)
		{
			m_StanceTime[side] += dt;
			bool behind = glm::dot(hip - ankle, ahead) > 0.05f;
			glm::vec3 toeLocal(0.0f, -foot.HalfExtents.y, foot.HalfExtents.z);
			glm::vec3 heelLocal(0.0f, -foot.HalfExtents.y, -foot.HalfExtents.z);
			glm::vec3 pivotLocal = behind ? toeLocal : heelLocal;
			float sign = behind ? 1.0f : -1.0f;
			glm::vec3 pivot = ankle + flat * (pivotLocal - m_FootToAnkle);
			auto ankleAt = [&](float angle)
			{
				return pivot + glm::angleAxis(sign * angle, axis) * flat * (m_FootToAnkle - pivotLocal);
			};
			float angle = 0.0f;
			if (glm::length(hip - ankleAt(0.0f)) > limit && std::fabs(glm::dot(hip - ankle, ahead)) > 0.05f)
			{
				float low = 0.0f, high = behind ? m_MaxHeelLift : m_MaxToeLift;
				for (int i = 0; i < 12; i++)
				{
					float mid = 0.5f * (low + high);
					(glm::length(hip - ankleAt(mid)) > limit ? low : high) = mid;
				}
				angle = high;
			}
			// Just landed, heel first: the toe comes down about the heel over
			// the first tenth of a second rather than slapping flat.
			if (!behind)
				angle = std::max(angle, m_LandingToeUp * (1.0f - glm::smoothstep(0.0f, 0.12f, m_StanceTime[side])));
			pitch = sign * angle;
			ankle = ankleAt(angle);
			m_PinLift[side] = pitch;
		}
		else
		{
			// The swing: leaving toe-down from the push-off and easing level,
			// then toe-up for the heel to land first.
			pitch = m_PinLift[side] * (1.0f - glm::smoothstep(0.0f, 0.4f, swingT))
				- m_LandingToeUp * glm::smoothstep(0.6f, 1.0f, swingT);
		}

		// Never further from the hip than the leg is long: past it the
		// joints would fight the two driven ends. A stance foot held here
		// slides, which is the honest failure of a stride the legs cannot
		// make, and it is counted.
		// Past the leg's full length, not short of it: he stands with the
		// knee on its extension stop (m_Crouch), and clamping at 99.5% pulled
		// both feet up 4.5 mm every frame and set the knees shivering.
		glm::vec3 fromHip = ankle - hip;
		float reach = glm::length(fromHip);
		if (reach > 1.003f * legLength)
		{
			ankle = hip + fromHip * (1.003f * legLength / reach);
			if (stance)
				m_PinTears++;
		}

		// In stance the pose follows from the geometry, which is continuous,
		// and is taken as it is: easing it turned the foot about its ankle and
		// dragged the corner that should stay on the ground. In the air the
		// foot eases towards its pose.
		// And never outside the hip's cone. The hip is a ball joint limited
		// to 55 degrees from straight down; in a sharp turn a planted foot
		// ended up behind and to the side of a pelvis that had swung round,
		// where no thigh can point, and the ankle joint opened by up to
		// 39 cm. The line from hip to ankle is held inside m_HipCone (the
		// knee bend puts the thigh itself a little further round).
		{
			const glm::quat& pelvisTurn = m_World.GetBody(m_Pelvis).Orientation;
			glm::vec3 local = glm::conjugate(pelvisTurn) * (ankle - hip);
			float length = glm::length(local);
			const glm::vec3 straightDown(0.0f, -1.0f, 0.0f);
			if (length > 1e-4f)
			{
				glm::vec3 direction = local / length;
				float angle = std::acos(glm::clamp(glm::dot(direction, straightDown), -1.0f, 1.0f));
				if (angle > m_HipCone)
				{
					glm::vec3 turnAxis = glm::cross(straightDown, direction);
					if (glm::length(turnAxis) > 1e-5f)
					{
						direction = glm::angleAxis(m_HipCone, glm::normalize(turnAxis)) * straightDown;
						ankle = hip + pelvisTurn * (direction * length);
						if (stance)
							m_PinTears++;
					}
				}
			}
		}

		glm::quat pose = glm::normalize(glm::angleAxis(pitch, axis) * flat);
		foot.Orientation = stance ? pose : glm::normalize(glm::slerp(foot.Orientation, pose, 0.6f));
		foot.UpdateInertiaWorld();
		glm::vec3 at = ankle - foot.Orientation * m_FootToAnkle;
		foot.Velocity = (at - foot.Position) / std::max(dt, 1e-4f);
		m_FootTarget[side] = ankle;
	}

	void SolveDrivenLeg(int side, const glm::vec3& hip, float dt)
	{
		if (!m_Pinned[side])
		{
			SolveLeg(side, hip, m_FootTarget[side], dt);
			return;
		}
		glm::vec3 pole = glm::angleAxis(m_PinYaw[side], glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec3(0.0f, 0.0f, 1.0f);
		SolveLegPole(side, hip, m_FootTarget[side], pole);
	}

	void UnpinFoot(int side)
	{
		if (!m_Pinned[side])
			return;
		m_Pinned[side] = false;
		GS::Joint3D& ankleJoint = m_World.GetJoint(m_AnkleJoint[side]);
		ankleJoint.ConeTwistEnabled = true;
		ankleJoint.MotorEnabled = m_MotorsEnabled;
		GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		foot.Type = GS::BodyType::Dynamic;
		// Keeping the motion it had: a swinging foot let go mid-stride
		// carries on rather than stopping dead in the air. Capped, because
		// a driven foot's velocity also carries the occasional correction.
		float speed = glm::length(foot.Velocity);
		if (speed > 6.0f)
			foot.Velocity *= 6.0f / speed;
		foot.Awake = true;
		foot.SleepTimer = 0.0f;
	}

	void UnpinFeet()
	{
		UnpinFoot(0);
		UnpinFoot(1);
		SetLegsColliding(true);
	}

	// The legs' own contacts, off while the feet are driven (see
	// RigidBody3D::NoCollideGroup) and back the moment he is hit or falls,
	// so a falling body's legs still land on each other.
	void SetLegsColliding(bool colliding)
	{
		int group = colliding ? 0 : (int)m_Pelvis + 1;
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_UpperLeg[side], m_LowerLeg[side], m_Foot[side] })
				m_World.GetBody(handle).NoCollideGroup = group;
			// And the scenery: a shin catching a riser between two driven
			// ends could only fight the joints (RigidBody3D::IgnoreStatic).
			for (auto handle : { m_UpperLeg[side], m_LowerLeg[side] })
				m_World.GetBody(handle).IgnoreStatic = !colliding;
		}
	}

	void PlaceFeet(float dt, const glm::vec3& forward, const glm::vec3& right, float stride)
	{
		for (int side = 0; side < 2; side++)
		{
			float phase = m_GaitPhase + (side == 0 ? 0.0f : glm::pi<float>());

			// Planted for the first m_Duty of its cycle, swinging for the
			// rest. This used to be a bare cos(phase) < 0, which is exactly
			// half and half -- and half and half is neither a walk nor a run.
			// A walk overlaps (both feet down through the hand-over) and a run
			// has gaps (neither foot down at all).
			// In the air both feet are off the ground by definition, and they
			// tuck rather than reaching for a floor that is not there.
			bool swinging = m_Air == Air::Flying
				|| (CyclePosition(side) >= m_Duty && stride > 0.05f);

			// Where the hip will be at the end of this step, not where it is:
			// the kinematic pelvis moves by its velocity during the step, and
			// legs solved for its old position chased a target one step stale
			// -- the hip joints sat 2-3 cm open all through a walk, which is
			// v dt = 1.3 / 60.
			glm::vec3 hip = HipWorld(side) + m_World.GetBody(m_Pelvis).Velocity * dt;

			// Where this foot will land: half a stride ahead of the root,
			// offset to its own side, on whatever the ground is there.
			// A quarter of the cycle ahead. One cycle is two steps, so at
			// heel strike the leading foot is half a step -- a quarter of a
			// stride -- in front of the body, and the trailing one the same
			// behind. This was half a stride, which placed the foot twice as
			// far forward as the phase said it should be and forced the stride
			// down to 0.45 m to keep it reachable: 0.22 m steps at seven a
			// second. A person walks at two.
			glm::vec3 landing = m_RootAnchor
				+ forward * (FootReach(stride) + m_StepJitter[side].x * stride)
				+ m_StaggerVelocity * m_StaggerStep
				+ right * ((side == 0 ? -m_StanceWidth : m_StanceWidth) + m_StepJitter[side].y);
			// The ankle rides above the sole, so a target on the ground is a
			// target with the foot buried in it.
			landing.y = m_World.GroundHeightBelow(landing + glm::vec3(0.0f, 1.0f, 0.0f), m_CharacterBodies) + m_AnkleHeight;

			// How much higher this foot's next foothold is than the one it is
			// standing on. This used to be a single probe a stride ahead of
			// the root, along the way the character was facing -- which asks
			// the question in the wrong place twice over. The root is not what
			// has to clear the riser, and facing lags the direction of travel
			// while turning, so walking onto a step out of a turn got the
			// warning late or not at all. The landing spot is already computed
			// right here, on the ground that is actually there.
			// Looked at one stride out as well as at the landing spot, and the
			// higher of the two wins. The landing spot alone is only half a
			// stride ahead, which is half the warning the old root probe gave,
			// and a lift that starts once the foot is already over the riser
			// arrives after the swing has passed its peak.
			glm::vec3 beyond = landing + forward * (CurrentStride() * 0.5f * stride);
			float ahead = m_World.GroundHeightBelow(beyond + glm::vec3(0.0f, 1.0f, 0.0f), m_CharacterBodies) + m_AnkleHeight;

			float climb = glm::clamp(
				(std::max(landing.y, ahead) - m_Planted[side].y) / m_MaxStepUp, 0.0f, 1.0f);

			// Latched for the length of the swing. The target is recomputed
			// every frame, so without this a foot that clears the riser stops
			// seeing a step and drops its lift halfway over -- catching the
			// obstacle it had just cleared.
			m_Climb[side] = swinging ? std::max(m_Climb[side], climb) : climb;

			if (swinging && !m_Swinging[side])
			{
				// No two steps land alike: a few centimetres along and across,
				// chosen as the foot lifts and held for the swing.
				uint32_t n = m_StepCount[side]++;
				float amount = VariationAmount();
				m_StepJitter[side] = amount * glm::vec2(0.035f * Signed(m_Seed * 53u + (uint32_t)side, n),
					0.015f * Signed(m_Seed * 59u + (uint32_t)side, n));
				// Just left the ground: remember where from, and where to.
				m_SwingFrom[side] = m_Planted[side];
				m_SwingTo[side] = landing;
			}
			else if (!swinging && m_Swinging[side])
			{
				// Planted where the driven foot was put down.
				glm::vec3 landed = m_FootTarget[side];
				m_StanceTime[side] = 0.0f;
				// Just landed. Pinned here until it swings again.
				m_Planted[side] = glm::vec3(landed.x, m_SwingTo[side].y, landed.z);
			}
			m_Swinging[side] = swinging;

			glm::vec3 target;
			float swingT = 0.0f;

			if (m_Air == Air::Flying)
			{
				// Standing still this is a symmetric tuck -- knees up, under
				// the body, a two-foot hop. With speed it opens into a stride:
				// the lead leg reaches forward and stays fairly straight, the
				// trailing leg folds up behind. Blended by speed, so there is
				// no moment where the pose changes character.
				float open = m_JumpLead >= 0
					? glm::clamp(m_Speed / std::max(m_WalkSpeed, 0.01f), 0.0f, 1.0f)
					: 0.0f;

				bool lead = side == m_JumpLead;
				float towards = lead ? 1.0f : -1.0f;

				// A leg reaching forward is a leg that is *less* tucked; the
				// one folding up behind is more.
				float tucked = m_TuckFraction
					+ towards * open * m_JumpLegSplitReach;

				glm::vec3 tuck = hip;
				tuck.y -= (m_ThighLength + m_ShinLength) * tucked;
				tuck += forward * (m_StrideLength * 0.05f
					+ towards * open * m_JumpLegSplit * 0.5f);

				// Reaching for the ground on the way down, so the feet are on it
				// at touchdown: planted from the tuck instead, a foot dropped
				// 0.35 m to the floor in one step and opened the leg by 15 cm.
				if (m_JumpVelocity < 0.0f)
				{
					float fall = glm::clamp(1.0f - m_JumpOffset / std::max(m_JumpHeight, 0.05f), 0.0f, 1.0f);
					glm::vec3 floor = tuck;
					floor.y = m_World.GroundHeightBelow(tuck + glm::vec3(0.0f, 1.0f, 0.0f), m_CharacterBodies) + m_AnkleHeight;
					tuck = glm::mix(tuck, floor, glm::smoothstep(0.0f, 1.0f, fall));
				}

				// Eased into the tuck rather than put there: a driven foot goes
				// exactly where it is told, and told to jump to the tuck in one
				// step at take-off it tore the leg open by 33 cm.
				if (m_Pinned[side])
					tuck = m_FootTarget[side] + (tuck - m_FootTarget[side]) * (1.0f - std::exp(-dt / 0.06f));
				DriveFoot(side, tuck, false, 0.5f, dt);
				SolveDrivenLeg(side, hip, dt);
				continue;
			}

			if (swinging)
			{
				// Progress through the swing, whatever fraction of the cycle
				// the swing happens to be at this gait.
				float t = glm::clamp((CyclePosition(side) - m_Duty)
					/ std::max(1.0f - m_Duty, 0.01f), 0.0f, 1.0f);
				swingT = t;

				// Keep the target moving with the aim point rather than the
				// one computed when the foot left the ground, or a turn mid
				// stride plants the foot where the character used to be going.
				m_SwingTo[side] = landing;

				// Eased at both ends: the foot accelerates off the ground and
				// settles onto it. Linear in t, it left at full speed and
				// arrived at full speed -- a jerk at every lift-off and every
				// landing.
				target = glm::mix(m_SwingFrom[side], m_SwingTo[side], glm::smoothstep(0.0f, 1.0f, t));
				// Height changes early: up onto (or down to) the next foothold
				// in the first half of the swing, while the foot is still near
				// where it left -- rising evenly across the stride, it met the
				// riser halfway and drew the shin through it.
				target.y = glm::mix(m_SwingFrom[side].y, m_SwingTo[side].y,
					glm::smoothstep(0.0f, m_SwingTo[side].y > m_SwingFrom[side].y ? 0.5f : 1.0f, t));

				// Over the top. Higher when there is a step to clear, which is
				// what the look-ahead probe is for.
				// The lift. Walking, the foot skims the ground, peaking halfway.
				// Running, the heel kicks up behind early in the swing and the
				// knee drives through: higher, and peaking at m_RunLiftPeak of
				// the swing. With the walk's 6 cm at every speed, a run was a
				// lunge -- both feet low and far apart, no flight to see.
				float peak = glm::mix(0.5f, m_RunLiftPeak, m_RunBlend);
				float warped = t < peak ? 0.5f * t / peak : 0.5f + 0.5f * (t - peak) / (1.0f - peak);
				float over = std::sin(warped * glm::pi<float>());
				float lift = glm::mix(m_FootLift, m_RunFootLift, m_RunBlend);
				target.y += over * lift * (1.0f + m_Climb[side] * m_ClimbKnee);
				// Pulled back under the body on the way up, as a heel kick is.
				target -= forward * (over * m_RunHeelKick * m_RunBlend * (1.0f - t));

			}
			else
			{
				// Planted. The body moves; this does not.
				target = m_Planted[side];
			}

			DriveFoot(side, target, !swinging, swingT, dt);
			SolveDrivenLeg(side, hip, dt);
		}
	}

	// One cycle of travel at the current gait. A run covers twice the ground
	// per cycle that a walk does, and most of the extra is flight, not reach.
	float CurrentStride() const
	{
		// The stride breathes a few percent, slowly, so the cadence is never
		// a metronome.
		float breathe = 1.0f + 0.03f * VariationAmount() * Noise(m_Clock / 2.5f, 31u);
		return glm::mix(m_StrideLength, m_RunStride, m_RunBlend) * breathe;
	}

	// How far ahead of the body a foot lands. Capped, and the cap is the point:
	// a quarter of a running stride would put the foot 0.60 m in front, which
	// needs the hip 0.23 m lower to be reachable at all -- and reaching that
	// far forward is overstriding, which is a braking force, not a run. Real
	// running gets its length from the flight phase instead.
	float FootReach(float stride) const
	{
		// A runner lands nearly under the hips with the knee bent; reaching
		// the walk's 0.32 m ahead at a run was a straight-legged overstride.
		float most = glm::mix(m_MaxFootReach, m_RunFootReach, m_RunBlend);
		return std::min(CurrentStride() * 0.25f, most) * stride;
	}

	// Where this foot is in its own cycle: 0 the instant it lands, wrapping
	// back to 0 at the next landing.
	float CyclePosition(int side) const
	{
		float phase = m_GaitPhase + (side == 0 ? 0.0f : glm::pi<float>())
			- 1.5f * glm::pi<float>();
		float c = std::fmod(phase / glm::two_pi<float>(), 1.0f);
		return c < 0.0f ? c + 1.0f : c;
	}

	// Progress through a flight window, or -1 when a foot is down. There are
	// two windows per cycle, each (0.5 - duty) long, so this is zero for any
	// duty of half or more -- a walk never leaves the ground.
	float Airborne() const
	{
		if (m_Duty >= 0.5f)
			return -1.0f;

		float c = CyclePosition(0);
		float window = 0.5f - m_Duty;

		if (c >= m_Duty && c < 0.5f)
			return (c - m_Duty) / window;
		if (c >= m_Duty + 0.5f)
			return (c - m_Duty - 0.5f) / window;

		return -1.0f;
	}

	// The hip joint's position in the world, which is what the leg has to
	// reach from.
	glm::vec3 HipWorld(int side) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		return pelvis.Position + pelvis.Orientation * m_HipLocal[side];
	}

	// Two-link inverse kinematics: given where the foot should be, what does
	// the hip point at and how far is the knee bent?
	//
	// The law of cosines twice. The knee angle comes from the triangle made by
	// the two leg segments and the straight-line distance to the target; the
	// thigh direction is the direction to the target, rotated by the angle
	// between the thigh and that line. Everything is done in the pelvis's
	// frame, where the knee's hinge axis is +x.
	// Two-link inverse kinematics, for any limb.
	//
	// Given where the end of the limb should be, what does the proximal joint
	// point at and how far is the distal hinge bent? The law of cosines twice,
	// solved in the frame of whatever the limb hangs off -- the pelvis for a
	// leg, the torso for an arm -- where the hinge axis is +x.
	//
	// `bendSign` is +1 for a knee and -1 for an elbow: the two hinges are
	// mirror images, which is why their limits are (0, 130) and (-140, 0).
	void SolveLimb(const glm::quat& rootOrientation, const glm::vec3& anchorWorld,
		const glm::vec3& endWorld, float upperLength, float lowerLength,
		GS::PhysicsWorld3D::BodyHandle proximal, const glm::quat& proximalRest,
		GS::PhysicsWorld3D::BodyHandle distal, float bendSign)
	{
		glm::vec3 toEnd = glm::conjugate(rootOrientation) * (endWorld - anchorWorld);

		float reach = glm::length(toEnd);
		if (reach < 1e-4f)
			return;

		// A limb cannot be longer than itself, and a joint locked exactly
		// straight is a singularity the law of cosines does not enjoy -- so
		// the target is pulled just inside what the limb can do.
		float maximum = (upperLength + lowerLength) * m_LegCap;
		float minimum = std::fabs(upperLength - lowerLength) + 0.01f;
		reach = glm::clamp(reach, minimum, maximum);

		glm::vec3 direction = glm::normalize(toEnd);

		// Interior angle at the distal hinge, and the bend away from straight.
		float cosDistal = (upperLength * upperLength + lowerLength * lowerLength
			- reach * reach) / (2.0f * upperLength * lowerLength);
		float bend = glm::pi<float>() - std::acos(glm::clamp(cosDistal, -1.0f, 1.0f));

		// Angle between the upper segment and the straight line to the end.
		float cosProximal = (upperLength * upperLength + reach * reach
			- lowerLength * lowerLength) / (2.0f * upperLength * reach);
		float alpha = std::acos(glm::clamp(cosProximal, -1.0f, 1.0f));

		// Rotated about the hinge's own axis, which is what makes it bend the
		// right way rather than sideways. The lateral component of the target
		// is carried by the proximal joint pointing at it, so this stays a
		// planar solve.
		glm::quat lift = glm::angleAxis(-alpha * bendSign, glm::vec3(1.0f, 0.0f, 0.0f));
		glm::vec3 upper = lift * direction;

		glm::quat aim = RotationBetween(glm::vec3(0.0f, -1.0f, 0.0f), upper);

		m_World.GetJoint(proximal).MotorTargetRotation =
			glm::normalize(aim * proximalRest);
		m_World.GetJoint(distal).MotorTargetAngle = bend * bendSign;
	}

	// The shoulder's position in the world, which is what an arm reaches from.
	glm::vec3 ShoulderWorld(int side) const
	{
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
		return torso.Position + torso.Orientation * m_ShoulderLocal[side];
	}

	// Put a hand somewhere. Same solve as a leg, mirrored at the elbow.
	void SolveArm(int side, const glm::vec3& handWorld)
	{
		SolveLimb(m_World.GetBody(m_Torso).Orientation, ShoulderWorld(side), handWorld,
			m_UpperArmLength, m_ForearmLength,
			m_ShoulderJoint[side], m_ShoulderRest[side], m_ElbowJoint[side], -1.0f);
	}

	void SolveLeg(int side, const glm::vec3& hipWorld, const glm::vec3& footWorld, float dt)
	{
		SolveLimb(m_World.GetBody(m_Pelvis).Orientation, hipWorld, footWorld,
			m_ThighLength, m_ShinLength,
			m_HipJoint[side], m_HipRest[side], m_KneeJoint[side], 1.0f);

		// And keep the sole level with the ground rather than pointing
		// wherever the shin happens to. Without this the foot lands on an edge
		// and rolls, which is exactly the scuffing this was meant to stop.
		//
		// The ankle motor is a *relative* target, so it has to be expressed in
		// the shin's frame -- and the shin is turning. Aiming at where the shin
		// is now means aiming at where it was by the time the solver runs, and
		// the sole ends up trailing the lean by the shin's rate over the
		// motor's stiffness. So the shin is integrated forward one step first.
		// This is the whole reason the sole was not flat: not the cone limit,
		// which never engaged, and not torque, but chasing a moving target.
		const GS::RigidBody3D& shin = m_World.GetBody(m_LowerLeg[side]);

		glm::quat spin(0.0f, shin.AngularVelocity.x, shin.AngularVelocity.y,
			shin.AngularVelocity.z);
		glm::quat ahead = glm::normalize(shin.Orientation
			+ 0.5f * dt * spin * shin.Orientation);

		// "Level" means level with **the ground here**, not with the world.
		//
		// A sole held flat to the horizon lands on its heel or its toe on any
		// slope, and only the leading edge touches: measured walking across
		// generated terrain, the lowest corner of the planted foot sat 4.61 cm
		// above the surface against 0.54 cm on a flat floor. The foot is asked
		// about the ground it is *going* to, not the ground it is over, so it
		// arrives already matching.
		glm::quat level = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));
		m_World.GetJoint(m_AnkleJoint[side]).MotorTargetRotation =
			glm::normalize(glm::conjugate(ahead) * ConformToGround(footWorld, side) * level);
	}

	// A leg solved with a pole: the knee bends towards `pole` (the foot's
	// forward), in the plane of hip, ankle and pole, and the thigh and shin
	// are given whole orientations -- direction *and* twist -- so the knee's
	// hinge is square to that plane. SolveLimb aims the thigh with the
	// shortest rotation, which leaves its twist to chance: fine for a leg
	// whose foot follows it, but with the foot driven the hinge was forced
	// out of its plane and the knee bent backwards in turns (to -118 degrees
	// in a zigzag, read off the joint itself).
	void SolveLegPole(int side, const glm::vec3& hip, const glm::vec3& ankle, const glm::vec3& pole)
	{
		float thigh = m_ThighLength, shin = m_ShinLength;
		glm::vec3 toAnkle = ankle - hip;
		float reach = glm::clamp(glm::length(toAnkle), std::fabs(thigh - shin) + 0.01f, (thigh + shin) * m_LegCap);
		if (glm::length(toAnkle) < 1e-4f)
			return;
		glm::vec3 down = glm::normalize(toAnkle);
		glm::vec3 forward = pole - down * glm::dot(pole, down);
		if (glm::length(forward) < 1e-4f)
			return;
		forward = glm::normalize(forward);

		float cosHip = (thigh * thigh + reach * reach - shin * shin) / (2.0f * thigh * reach);
		float alpha = std::acos(glm::clamp(cosHip, -1.0f, 1.0f));
		glm::vec3 knee = hip + (down * std::cos(alpha) + forward * std::sin(alpha)) * thigh;
		glm::vec3 kneeToAnkle = glm::normalize(ankle - knee);
		if (glm::length(ankle - knee) < 1e-4f)
			kneeToAnkle = down;

		// The bones' own frames: +y up the bone, +z forward, +x the knee's
		// hinge (the rig is built with both so).
		auto frame = [&](const glm::vec3& upAlong)
		{
			glm::vec3 y = glm::normalize(upAlong);
			glm::vec3 z = forward - y * glm::dot(forward, y);
			z = glm::length(z) > 1e-4f ? glm::normalize(z) : glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 x = glm::cross(y, z);
			return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
		};
		glm::quat thighWorld = frame(hip - knee);
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		m_World.GetJoint(m_HipJoint[side]).MotorTargetRotation =
			glm::normalize(glm::conjugate(pelvis.Orientation) * thighWorld * m_ThighRestLocal[side]);

		float cosKnee = (thigh * thigh + shin * shin - reach * reach) / (2.0f * thigh * shin);
		m_World.GetJoint(m_KneeJoint[side]).MotorTargetAngle =
			glm::pi<float>() - std::acos(glm::clamp(cosKnee, -1.0f, 1.0f));
		(void)kneeToAnkle;
	}

	// The rotation that lays a sole on the slope under `footWorld`.
	//
	// Bounded twice over. `m_SoleToSlope` is how much of the slope to follow at
	// all, and `m_MaxSoleTilt` caps the result -- the ankle is a ball joint on a
	// 35 degree cone, and a foot commanded past that is a motor pushing against
	// a limit for the whole stance, which reads as a shudder rather than as a
	// step.
	glm::quat ConformToGround(const glm::vec3& footWorld, int side) const
	{
		const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		float height = 0.0f;
		glm::vec3 ground = up;

		// From a metre above, so a target already at ground level still finds
		// the surface it is sitting on rather than the one below it.
		if (!m_World.GroundBelow(footWorld + up, height, ground, m_CharacterBodies, -1000.0f))
			return none;

		float tilt = std::acos(glm::clamp(ground.y, -1.0f, 1.0f));
		if (tilt < 1e-4f)
			return none;

		float share = m_SoleToSlope
			* glm::clamp(glm::radians(m_MaxSoleTilt) / tilt, 0.0f, 1.0f);

		return glm::slerp(none, RotationBetween(up, ground), share);
	}

	// ---------------------------------------------------------------------
	// Balance	// ---------------------------------------------------------------------
	// Balance
	// ---------------------------------------------------------------------
	//
	// Everything a balance controller needs, and nothing it does.
	//
	// The idea is standard and worth stating plainly. A standing figure is an
	// inverted pendulum. It stays up while its weight is over its feet, and
	// the honest version of "over its feet" is:
	//
	//   * the **support polygon** -- the convex hull of everywhere the feet
	//     touch the ground, flattened to the horizontal plane; and
	//   * the **capture point** -- not where the centre of mass *is*, but
	//     where it is *going*: `com + velocity * sqrt(height / g)`. That is
	//     the spot a foot would have to reach to bring the body to rest, and
	//     it is what separates leaning from falling. Standing still they are
	//     the same point; moving, the capture point leads.
	//
	// The prediction this makes is falsifiable and nothing here enforces it:
	// the figure should topple when the capture point leaves the polygon, and
	// not before.

	// Andrew's monotone chain. Small, and worth doing properly rather than
	// taking the bounding box of the contacts: a box says a figure up on one
	// toe is as stable as one flat-footed, which is exactly the case balance
	// is about.
	static std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> points)
	{
		if (points.size() < 3)
			return points;

		std::sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b)
		{
			return a.x < b.x || (a.x == b.x && a.y < b.y);
		});

		auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b)
		{
			return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
		};

		std::vector<glm::vec2> hull(points.size() * 2);
		size_t k = 0;

		for (size_t i = 0; i < points.size(); i++)
		{
			while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) k--;
			hull[k++] = points[i];
		}

		for (size_t i = points.size() - 1, t = k + 1; i > 0; i--)
		{
			while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) k--;
			hull[k++] = points[i - 1];
		}

		hull.resize(k > 0 ? k - 1 : 0);
		return hull;
	}

	// Signed distance from a point to a convex polygon: positive inside.
	static float SignedDistance(const std::vector<glm::vec2>& hull, const glm::vec2& point)
	{
		if (hull.size() < 3)
			return -1.0f;

		float smallest = std::numeric_limits<float>::max();
		bool inside = true;

		for (size_t i = 0; i < hull.size(); i++)
		{
			const glm::vec2& a = hull[i];
			const glm::vec2& b = hull[(i + 1) % hull.size()];

			glm::vec2 edge = b - a;
			float length = glm::length(edge);
			if (length < 1e-6f)
				continue;

			// Counter-clockwise winding, so the outward normal is the edge
			// turned clockwise.
			glm::vec2 outward = glm::vec2(edge.y, -edge.x) / length;
			float distance = glm::dot(point - a, outward);

			if (distance > 0.0f)
				inside = false;

			smallest = std::min(smallest, -distance);
		}

		return inside ? smallest : -std::fabs(smallest);
	}

	BalanceState ComputeBalance() const
	{
		BalanceState state;

		// Its own bodies, not every body in the world. In the demo they
		// were the only dynamic ones; in a game a crate counted towards
		// where his weight is.
		float total = 0.0f;
		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			float mass = body.GetMass();
			state.Com += body.Position * mass;
			state.ComVelocity += body.Velocity * mass;
			total += mass;
		}

		if (total <= 0.0f)
			return state;

		state.Com /= total;
		state.ComVelocity /= total;
		state.ComGround = { state.Com.x, state.Com.z };

		// Where the feet actually touch, not where they are. A foot in the air
		// contributes nothing, which is the whole point.
		std::vector<glm::vec2> contacts;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool footA = contact.A == m_Foot[0] || contact.A == m_Foot[1];
			bool footB = contact.B == m_Foot[0] || contact.B == m_Foot[1];
			if (!footA && !footB)
				continue;

			for (int p = 0; p < contact.PointCount; p++)
				contacts.push_back({ contact.Points[p].Position.x, contact.Points[p].Position.z });
		}

		state.Support = ConvexHull(contacts);

		for (const glm::vec2& point : state.Support)
			state.SupportCentre += point;
		if (!state.Support.empty())
			state.SupportCentre /= (float)state.Support.size();

		// sqrt(h/g) is the inverted pendulum's time constant -- the natural
		// timescale over which a lean becomes a fall. Height is measured from
		// the ground contacts to the centre of mass, so a crouching figure
		// gets a shorter one, which is correct: crouching really does buy time.
		float height = std::max(state.Com.y, 0.05f);
		float tau = std::sqrt(height / 9.81f);

		state.CapturePoint = state.ComGround
			+ glm::vec2(state.ComVelocity.x, state.ComVelocity.z) * tau;

		state.Margin = SignedDistance(state.Support, state.CapturePoint);
		state.Standing = state.Support.size() >= 3 && state.Margin > 0.0f;

		return state;
	}

	// How hard a foot is pressing on the ground, in newtons.
	//
	// The solver works in impulses -- a normal impulse is a force applied for
	// one step -- so dividing by the step recovers the force. Worth having as
	// a number rather than an intuition: the whole stepping problem turns on
	// whether a foot is carrying weight at the moment something tries to lift
	// it, and "it looks like it is standing on both" is not a measurement.
	//
	// The figure weighs about 71 kg, so 697 N is all of it and 348 N is half.
	//
	// Two things this measured, both of which had been guessed at for three
	// sessions:
	//
	//   * At the moment a step tries to lift a foot, that foot is carrying
	//     **363 N -- 52% of body weight**. Which is why no swing controller
	//     ever moved it: they were all lifting a foot with half the figure
	//     standing on it.
	//   * The load does not sit at a steady 348 N each while standing. It
	//     oscillates between 0 and over 900 N, and the two feet together
	//     briefly carry 1284 N, nearly twice the figure's weight. The standing
	//     pose is not still -- it is bouncing, and the ankle controller is
	//     driving it.
	float FootLoad(int side, float dt) const
	{
		if (dt <= 0.0f)
			return 0.0f;

		float impulse = 0.0f;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			if (contact.A == m_Foot[side] || contact.B == m_Foot[side])
				impulse += contact.TotalNormalImpulse();
		}

		return impulse / dt;
	}

	glm::vec3 CentreOfMass() const
	{
		glm::vec3 weighted(0.0f);
		float total = 0.0f;

		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			float mass = body.GetMass();
			weighted += body.Position * mass;
			total += mass;
		}

		return total > 0.0f ? weighted / total : glm::vec3(0.0f);
	}

protected:
	GS::PhysicsWorld3D& m_World;
	Control m_Control;


	using Handle = GS::PhysicsWorld3D::BodyHandle;

	Handle m_Pelvis = 0, m_Torso = 0, m_Head = 0;
	Handle m_UpperLeg[2] = {}, m_LowerLeg[2] = {}, m_Foot[2] = {};
	Handle m_UpperArm[2] = {}, m_LowerArm[2] = {};
	Handle m_ShoulderJoint[2] = {}, m_ElbowJoint[2] = {};
	Handle m_SpineJoint = {};
	glm::quat m_SpineRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::quat m_ShoulderRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

	std::vector<GS::PhysicsWorld3D::JointHandle> m_Joints;
	std::vector<GS::PhysicsWorld3D::BodyHandle> m_CharacterBodies;

	GS::PhysicsWorld3D::JointHandle m_HipJoint[2] = {};
	GS::PhysicsWorld3D::JointHandle m_KneeJoint[2] = {};
	glm::vec3 m_KneeAt[2] = {};
	glm::vec3 m_AnkleAt[2] = {};

	// How far apart the feet stand. The hips are fixed at 0.10, so anything
	// wider splays the legs.
	//
	// Widening it was meant to make the two feet load less equally, so a foot
	// could be lifted. **It does the opposite**, measured over eight perturbed
	// trials each: quiet standing falls from 25.1 s at 0.10 to 20.2 s at 0.20
	// and 7.1 s at 0.30, and shove survival from 1.99 s to 1.00 s. Splayed
	// legs push outwards, and the ground reaction that answers them is another
	// thing the balance has to fight.
	//
	// Left as a slider because the refutation is worth being able to reproduce.
	float m_StanceWidth = 0.10f;

	// Stepping state.
	// Off by default, because it currently makes things *worse*: 14.8 s of
	// unpushed standing against 35.1 s without it, and marginally faster falls
	// under every shove tested. The trigger and the aiming are right -- see the
	// changelog -- and the swing is what is not. Left switchable so the failure
	// can be watched rather than described.
	bool m_SteppingEnabled = false;
	bool m_Stepping = false;
	bool m_Shifting = false;

	// Gate the lift on the swing foot's own load rather than trying to create
	// an unload. 150 N is about a fifth of body weight, and the measurement
	// that motivated it: each foot is under that figure roughly 11% of the
	// time, in windows averaging a tenth of a second.
	bool m_GateOnLoad = true;
	bool m_WaitingForTrough = false;
	float m_WaitTimer = 0.0f;
	// 150 N, swept. Foot travel as a fraction of the distance asked for:
	// **8% with no gate, 22% at 150 N**, 20% at 80 N but with eight troughs
	// missed entirely (too strict to catch), and 7% at 250 N -- which is no
	// better than not gating, because 250 N is still most of a body's weight
	// on the foot.
	float m_LoadGate = 150.0f;
	float m_MaxWait = 0.25f;
	// How far ahead to extrapolate the load when deciding to commit.
	//
	// **Zero, and that is a result.** Committing early was supposed to centre
	// the trough on the lift rather than starting it there. It cut foot travel
	// from 22% to 6%, because the load is a spike train swinging 0-900 N
	// several times a second, so its derivative is enormous and erratic and
	// the extrapolation predicts troughs that never arrive -- committing at
	// high load, which is the thing that has never worked.
	//
	// Left as a knob because the failure is instructive and cheap to see.
	float m_LeadTime = 0.0f;
	float m_PreviousSwingLoad = 0.0f;
	int m_TroughsUsed = 0;
	int m_TroughsMissed = 0;
	// The reactive step: no weight shift, a short swing, and a foot thrown at
	// the capture point. The deliberate version is still in here behind this
	// flag, because the two are genuinely different manoeuvres rather than one
	// being a tuning of the other.
	bool m_FastStep = true;
	float m_ShiftTimer = 0.0f;
	// Long enough to move the weight, short enough that the disturbance has
	// not already won. A timeout rather than a fixed duration -- the shift ends
	// early when the weight arrives.
	// 0.35 s, which is not a taste decision: shifting weight sideways is an
	// inverted-pendulum motion and its time constant is sqrt(h/g), about
	// 0.33 s for this figure -- the same constant that appears in the capture
	// point. Asking for it faster does not work. Measured: a 0.22 s shift
	// closed 1 cm of a 10 cm gap, 0.5 s closed 3 cm, 1.0 s closed 5 cm, and
	// *doubling the roll authority changed nothing*. The motion is rate
	// limited by physics, not by how hard the ankles push.
	float m_ShiftDuration = 0.35f;
	float m_ShiftTolerance = 0.04f;
	float m_ShiftRoll = 0.30f;
	int m_SwingSide = 0;
	int m_StepsTaken = 0;
	float m_StepTimer = 0.0f;
	// Short. The swing is a throw, not a stride: the foot has to be down
	// before the body has finished falling towards it.
	//
	// Swept, and 0.16 s is the best of a bad set. Absolute foot travel is
	// about 0.11-0.16 m whatever the duration -- 0.16 s manages 0.158 m,
	// 0.30 s manages 0.113 m, 0.45 s manages 0.144 m. Shortening it to 0.10 s
	// to fit inside the trough made things markedly worse, not better, so the
	// swing was never trough-limited.
	//
	// Beware the percentage: a longer swing reports a *higher* fraction of the
	// distance asked for only because it tends to trigger when the target is
	// closer. Absolute metres is the honest measure and it barely moves.
	float m_StepDuration = 0.16f;
	// How far outside the feet the capture point must get before a step is
	// worth taking. Too small and the figure steps at every sway; too large
	// and it commits too late to reach.
	float m_StepTrigger = 0.06f;
	// Consecutive fixed steps the capture point must stay outside before a
	// step is committed to. One means "step the instant it crosses".
	//
	// Eight, and there is **no trade** against stepping earlier -- which was
	// the obvious worry once the step was diagnosed as too late. Survival
	// fractions over sixteen trials, margin/persist against quiet standing and
	// two shove strengths:
	//
	//     0.00 / 1     1/16 quiet    0/16 at 20 Ns
	//     0.00 / 4     7/16          3/16
	//     0.03 / 4     9/16          7/16
	//     0.06 / 8    10/16          8/16     <- shipped
	//     0.12 / 8     9/16          8/16
	//
	// Both columns move together: triggering earlier is worse at standing
	// still *and* worse at surviving a shove. There is nothing to trade,
	// because the step has no upside to trade against -- it does not extend
	// the support polygon, so every step taken is pure cost and taking more of
	// them earlier is strictly worse.
	//
	// Eight, and this is the whole trigger fix. Measured as a survival
	// fraction over 16 perturbed trials: stepping on the first crossing costs
	// a third of the quiet standing (8/16 against 12/16), because ordinary
	// sway puts the capture point over an edge for a frame or two and a step
	// answers something that had already fixed itself. Requiring it to stay
	// out restores parity exactly (12/16). Raising the threshold to 0.12
	// instead does the same job; persistence is the cleaner of the two,
	// because it rejects brief excursions without also rejecting real ones
	// that happen to be small.
	int m_StepPersist = 8;
	int m_OutsideFor = 0;
	float m_MaxStep = 0.55f;
	// The one genuine tension in the swing, and both sides of it are measured.
	//
	// A bent knee keeps the foot off the ground -- without it the foot lands
	// four frames in and drags -- but it also folds the leg, so the same hip
	// rotation moves the foot far less. The hip reaches 4.48 rad/s, which over
	// a 0.16 s swing on a straight 0.9 m leg would carry the foot about 0.6 m;
	// folded, it delivers 0.183 m. Clearance is currently worth more than
	// reach, but not by as much as the lift sweep alone suggests.
	//
	// Large, and the reasoning that made it small was exactly backwards.
	//
	// It was 0.40 rad on the theory that a bent knee shortens the leg when it
	// needs to reach furthest. Measuring what the foot actually does killed
	// that: it lifts into the trough, then **lands again after four frames**
	// and drags for the rest of the swing. Clearing the ground is not a
	// detail, it is the thing.
	//
	// Knee bend against airborne frames (of ten) and foot travel:
	// 0.40 -> 2.3 frames, 0.083 m. 1.00 -> 6.5, 0.118 m. **1.40 -> 7.1,
	// 0.183 m.** More than double the travel for a leg that is nominally
	// shorter while swinging.
	float m_StepLift = 1.40f;
	// Where in the swing the knee is most bent, as a fraction.
	//
	// **0.5 -- symmetric -- and moving it earlier is worse, monotonically.**
	// Foot travel by peak position: 0.50 -> 0.183 m, 0.35 -> 0.161 m,
	// 0.25 -> 0.142 m, 0.15 -> 0.131 m, with the airborne fraction flat at
	// about 70% throughout.
	//
	// The idea was that bending early and straightening late would clear the
	// ground on the same schedule while leaving the leg extended for longer
	// while it reaches. It does not, and the airborne figure says why it is
	// not a clearance effect: much of the foot's forward travel comes from the
	// knee *extending*, so extension late in the swing adds to the reach and
	// extension early is spent before the hip has turned.
	//
	// Kept as a knob because it is a clean refutation of a plausible idea.
	float m_LiftPeak = 0.5f;
	float m_MinStep = 0.12f;
	float m_StepCooldown = 0.0f;
	float m_StepCooldownTime = 0.25f;
	glm::vec2 m_StepTarget{ 0.0f };

	GS::PhysicsWorld3D::JointHandle m_AnkleJoint[2] = {};
	glm::quat m_HipRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                           glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
	glm::quat m_AnkleRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                             glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

	Mode m_Mode = Mode::Controlled;
	const char* m_RagdollReason = "";
	float m_RagdollTimer = 0.0f;
	std::unordered_map<GS::PhysicsWorld3D::JointHandle, float> m_JointTorque;

	// A hit above this many newtons ends control. Body weight is about 697 N,
	// so the default is roughly three times the character's weight -- enough
	// that walking into scenery does not floor them.
	// Body weight, and that is not a coincidence: 71.2 kg is 698 N, and a
	// contact carrying less than his own weight is him standing on the floor,
	// not something hitting him. Lowered to 400 it fired on his own landing
	// and on ordinary weight-bearing -- he stood there staggering, the knee
	// oscillating between 4 and 31 degrees, and a standing jump came apart.
	float m_StaggerThreshold = 700.0f;
	float m_CatchReach = 0.50f;          // how far a protective step reaches, fore-aft
	float m_CatchReachSide = 0.26f;      // ...and across the body, where it cannot
	float m_CatchSteps = 2.5f;           // and how many of them he gets
	float m_RecoverCrouch = 0.16f;       // how far he sinks while catching himself
	float m_BodyMass = 0.0f;
	glm::vec3 m_StaggerVelocity = glm::vec3(0.0f);

	// Stumbling (see UpdateStumble). On by default: off brings back the
	// kinematic stagger, for comparing the two.
	bool m_PhysicalStumble = true;
	bool m_SteppingCatch = true;          // off: the ankles alone, for measuring
	float m_StumbleEntrySpeed = 2.0f;     // m/s; faster than this (a run) a hit staggers the old way
	glm::vec2 m_StumbleCarry = glm::vec2(0.0f);
	float m_GetUpMaxGap = 0.3f;           // lowest body's centre above the ground, at most, to get up
	bool m_Bracing = true;
	float m_BraceTimer = 0.0f;
	float m_BraceTime = 0.8f;             // s the arms stay strong after a lost stumble
	float m_BraceForward = 1.0f;          // reach ahead per unit down: 1 is 45 degrees
	glm::vec3 m_BraceDirection = glm::vec3(0.0f, 0.0f, 1.0f);   // the walk's velocity, kept through the stumble
	float m_StumbleCalmWalk = 0.35f;
	float m_StumbleSpeedGain = 3.0f;      // 1/s: how hard the ankles pull a walking stumble back to the walk's speed      // m/s from the walk's velocity counts as caught
	glm::vec3 m_HitPush = glm::vec3(0.0f);
	glm::vec3 m_FootToAnkle = glm::vec3(0.0f, 0.05f, -0.03f);
	float m_StandComHeight = 1.0f;        // measured at build
	float m_StumbleTimer = 0.0f;
	int m_StumbleSteps = 0;
	int m_StumbleSwing = -1;              // the foot in the air, or -1
	int m_StumbleLastSwing = -1;
	float m_TouchMemory[2] = { 0.0f, 0.0f };   // seconds off the ground
	bool m_Pinned[2] = { false, false };
	float m_PinYaw[2] = { 0.0f, 0.0f };
	float m_PinLift[2] = { 0.0f, 0.0f };
	float m_HipCone = glm::radians(45.0f);
	bool m_DriveFeet = true;              // off: the old motor-only feet, for comparing
	int m_PinTears = 0;                   // pins let go because the leg could not reach
	float m_PinTwist = glm::radians(0.0f);   // the hip twists 25 and the ankle 15; at 35 the knee took the rest, backwards
	float m_MaxHeelLift = glm::radians(45.0f);
	float m_MaxToeLift = glm::radians(25.0f);
	float m_LandingToeUp = glm::radians(12.0f);   // the swing foot arrives heel first
	glm::vec3 m_FootTarget[2] = {};
	glm::quat m_ThighRestLocal[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
	float m_StanceTime[2] = { 1.0f, 1.0f };
	float m_FootGround[2] = { 0.0f, 0.0f };
	bool m_StumbleGathering = false;
	float m_StumbleStance = 0.22f;
	float m_StumbleCentring = 0.5f;
	float m_StumblePress = 0.03f;
	float m_StumbleSwingTimer = 0.0f;
	float m_StumbleCooldown = 0.0f;
	float m_StumbleCalm = 0.0f;
	float m_StumbleUnsupported = 0.0f;
	float m_StumbleCrouch = 0.0f;
	float m_StumbleGround = 0.0f;
	float m_StumbleMargin = 0.0f;
	float m_StumbleSupport = 0.0f;
	glm::vec3 m_StumbleForce = glm::vec3(0.0f);
	glm::vec2 m_StumbleTarget = glm::vec2(0.0f);
	glm::vec2 m_StumbleCapture = glm::vec2(0.0f);
	std::vector<glm::vec2> m_StumbleFootprint;   // for drawing
	float m_StumbleHeightGain = 100.0f;   // (rad/s)^2: a 10 rad/s spring on the hips' height
	float m_StumbleMaxSupport = 2.0f;     // body weights the virtual leg can push
	float m_StumbleFriction = 0.7f;
	float m_StumbleUprightGain = 900.0f;  // N m per radian
	float m_StumbleUprightDamping = 120.0f;
	float m_StumbleMaxTorque = 500.0f;
	float m_StumbleTrigger = 0.02f;       // m outside the footprint before stepping
	float m_StumbleStepTime = 0.25f;
	float m_StumbleOvershoot = 0.05f;
	float m_StumbleFootGap = 0.14f;
	float m_StumbleReach = 0.60f;         // from the hip, flat
	float m_StumbleLift = 0.08f;
	float m_StumbleSettle = 0.05f;
	float m_StumbleComHeight = 1.0f;
	float m_StumbleRestCrouch = 0.08f;    // knees bent while recovering, so the legs have travel to push with
	float m_StumbleMaxCrouch = 0.30f;
	float m_StumbleCrouchRate = 1.5f;
	float m_StumbleGiveUpTilt = 50.0f;
	float m_StumbleGiveUpHeight = 0.55f;
	int m_StumbleMaxSteps = 8;
	float m_StumbleMaxTime = 5.0f;
	float m_StumbleCalmMargin = 0.03f;
	float m_StumbleCalmSpeed = 0.15f;
	float m_StumbleCalmTime = 0.30f;
	float m_StaggerTimer = 0.0f;
	float m_StaggerTime = 0.9f;
	float m_StaggerRecovery = 2.2f;      // how fast the push bleeds off, per second
	float m_StaggerSettle = 0.12f;
	float m_StaggerStep = 0.22f;         // how far ahead the protective step goes
	float m_LimpStiffness = 1.5f;
	float m_LimpTorque = 8.0f;

	// Walking.
	float m_Facing = 0.0f;
	float m_WalkSpeed = 1.3f;

	// Momentum. Braking is quicker than acceleration, which is true of people
	// and stops the character feeling like it is on ice.
	float m_Speed = 0.0f;
	float m_SpeedResponse = 6.0f;     // rad/s: the speed's spring
	float m_SpeedRate = 0.0f;
	float m_TurnRate = 4.0f;          // radians a second
	glm::vec3 m_RootAnchor{ 0.0f };

	// Weight.
	// Tuned against a person rather than by eye. Gait studies put the hips at
	// roughly 4-5 cm of vertical travel per stride and 2-3 cm of lateral sway;
	// the first pass at these produced 6.5 and 6.0, which reads as a swagger.
	float m_BobAmount = 0.008f;      // on top of what the stride geometrically needs
	float m_FallHeight = 1.5f;       // no ground within this and he is falling
	float m_RollReach = 0.08f;
	float m_RunFootLift = 0.32f;     // swing lift at a run, m
	float m_RunLiftPeak = 0.35f;     // how early in the swing it peaks, running
	float m_RunHeelKick = 0.20f;
	float m_RunFootReach = 0.26f;    // how far ahead a running foot lands, m     // how far the rising heel is drawn back under him, m       // reach the heel-to-toe roll gives a foot, m
	float m_SwayAmount = 0.013f;
	float m_Carry = 0.0f;   // -1 fully over side 0, +1 fully over side 1
	// 0.07 rad per unit of walk speed: about 4 deg walking and 12 deg running,
	// which is roughly what a person does. It was 0.10, giving 17.6 deg at a
	// run -- too much even before it was pointing the wrong way.
	float m_LeanAmount = 0.07f;
	float m_LeanFromAccel = 1.0f;     // 1.0 means exactly atan(a/g)
	float m_LeanAccel = 0.0f;
	float m_LeanAccelRate = 12.0f;    // how fast the smoothed acceleration follows
	float m_LastSpeed = 0.0f;
	float m_LeanForward = 0.45f;
	float m_LeanBack = 0.20f;         // braking leans back, but not as far
	float m_LeanPelvisShare = 0.45f;  // the rest is spine, so it bends
	float m_RollAmount = 0.05f;

	// Terrain.
	//
	// StandClearance is how far the pelvis rides above whatever it is standing
	// on, taken from the pose the rig was built in rather than typed in.
	float m_StandClearance = 1.09f;
	float m_StandTarget = 0.0f;
	float m_StandRate = 0.0f;
	float m_StandSmoothing = 0.08f;  // s
	float m_StandResponse = 12.0f;   // rad/s
	float m_MaxStepUp = 0.35f;
	float m_StepUp = 0.0f;           // 0 flat, 1 the tallest step worth lifting for
	float m_ClimbKnee = 1.1f;        // extra knee bend at a full step

	// Feet.
	float m_Climb[2] = { 0.0f, 0.0f };  // per leg: how much higher its next foothold is
	glm::vec3 m_Planted[2] = {};      // where each foot is pinned while it carries
	glm::vec3 m_SwingFrom[2] = {};
	glm::vec3 m_SwingTo[2] = {};
	bool m_Swinging[2] = { false, false };
	glm::vec3 m_HipLocal[2] = {};     // hip joint in the pelvis's own frame
	glm::vec3 m_ShoulderLocal[2] = {};
	float m_UpperArmLength = 0.35f;
	float m_ForearmLength = 0.32f;
	float m_ThighLength = 0.48f;
	float m_ShinLength = 0.42f;
	// 6 cm of clearance at mid-swing, and the leg tracks that to within a
	// millimetre -- an earlier reading of 0.24 m was the spawn transient being
	// caught by a peak taken over the whole run, not the gait. Every value
	// from 0.02 up still gets him onto the 0.30 m step, because the foot's
	// target is on top of the step either way and the lift only has to clear
	// the riser on the way there.
	float m_FootLift = 0.06f;
	// Jump. Grounded -> Crouching (the windup) -> Flying (ballistic) -> back.
	enum class Air { Grounded, Crouching, Pushing, Flying };
	Air m_Air = Air::Grounded;
	bool m_WantsJump = false;
	float m_AirTimer = 0.0f;
	float m_JumpVelocity = 0.0f;
	float m_JumpOffset = 0.0f;        // height above the stand height
	float m_JumpHeight = 0.55f;       // apex, and it is exactly v^2/2g
	float m_JumpCrouchTime = 0.18f;   // eased: at 0.12 the descent peaked at 6 d / T^2 = 58 m/s^2
	float m_JumpCrouchDepth = 0.14f;
	float m_LandTimer = 0.0f;
	float m_LandSpeed = 0.0f;         // m/s the hips landed at, absorbing
	float m_LandClock = 0.0f;
	float m_LandAbsorb = 0.12f;       // s: tau of the landing's give

	// Variation (see SetSeed).
	struct Design
	{
		float WalkSpeed = 0, RunSpeed = 0, StrideLength = 0, StanceWidth = 0, ArmSwing = 0,
			ElbowBend = 0, LeanAmount = 0, BobAmount = 0, SwayAmount = 0, TurnRate = 0, RunFootLift = 0;
	};
	Design m_Design;
	bool m_DesignStored = false;
	uint32_t m_Seed = 1;
	float m_Variation = 1.0f;         // 0 is the design exactly
	float m_Clock = 0.0f;
	float m_Posture = 0.0f;           // radians of standing lean, + forward
	float m_BreathPeriod = 4.2f;
	float m_Breath = 0.0f;
	float m_IdleWeight = 1.0f;
	float m_NextGlance = 1.0f;
	uint32_t m_GlanceCount = 0;
	float m_GlanceYaw = 0.0f, m_GlancePitch = 0.0f, m_GlanceTilt = 0.0f;
	float m_HeadYaw = 0.0f, m_HeadPitch = 0.0f, m_HeadTilt = 0.0f;
	float m_IdleRoll = 0.0f;           // radians the hips tilt with the weight shift
	float m_IdleArm[2] = { 0.0f, 0.0f };
	uint32_t m_StepCount[2] = { 0, 0 };
	glm::vec2 m_StepJitter[2] = {};   // this step's landing offset: along, across
	GS::PhysicsWorld3D::JointHandle m_NeckJoint = {};
	glm::quat m_NeckRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	float m_LandTime = 0.18f;
	float m_TuckFraction = 0.72f;     // how far the tucked leg reaches down
	bool m_JumpPending = false;       // pressed, waiting for a foot to push off
	int m_JumpLead = -1;              // which leg swings through in front, -1 for a hop
	float m_JumpGateSpeed = 0.5f;     // below this it is a standing hop, no waiting
	float m_JumpGateWindow = 0.10f;   // how soon after a plant counts as "on it"
	float m_JumpLegSplit = 0.34f;     // fore-aft separation of the feet at full speed
	float m_JumpLegSplitReach = 0.16f;
	float m_RunSpeed = 4.0f;
	float m_RunStride = 2.4f;         // m per cycle: 3.3 steps/s at 4 m/s
	float m_RunBlend = 0.0f;
	float m_RunBlendRate = 0.0f;
	float m_RunBlendResponse = 5.0f;
	float m_WalkDuty = 0.62f;         // fraction of its cycle a foot is down
	float m_RunDuty = 0.30f;
	float m_Duty = 0.62f;
	float m_MaxFootReach = 0.32f;     // how far ahead a foot may land
	float m_ArmSwing = 0.42f;         // radians at full stride, about 24 degrees
	float m_ElbowBend = 0.55f;
	float m_AnkleHeight = 0.10f;      // ankle joint above the sole

	// How far the sole follows the ground it is landing on, and the most it
	// will tilt to do it. Both swept -- see the changelog.
	float m_SoleToSlope = 1.0f;
	float m_MaxSoleTilt = 25.0f;      // degrees, inside the ankle's 35 deg cone
	// How far the hips sit below a straight-legged stand. Sets how far forward
	// a foot can be placed: with leg L and hip height v, the reach is
	// sqrt(L^2 - v^2), which is zero when the leg is straight.
	// Just enough to keep the knee off full extension, where the law of
	// cosines is a singularity and the angle is wildly sensitive to reach.
	// This was 0.09, which is not a stand at all: on a 0.48 thigh and a 0.42
	// shin it works out at 51.8 degrees of knee bend -- a half squat. The
	// stride's own dip does the rest of the work now.
	// How much of the leg's length the IK will solve for. Deliberately just
	// under 1: standing asks for slightly more reach than this, so the knee
	// rests against its extension stop instead of balancing near it.
	float m_LegCap = 0.999f;
	// Zero on purpose. Standing demands very slightly more reach than the leg
	// has, so the knee rests against its own extension stop -- which is how a
	// person stands, and is the only configuration that measured steady. Give
	// it slack instead and the knee balances near full extension where the law
	// of cosines has a gain of about 12 degrees per millimetre, and the
	// kinematic pelvis's own micro-motion drives it to thrash: at 0.006 the
	// standing knee swung from -3 to 150 degrees, through both of its limits.
	float m_Crouch = 0.0f;
	// The ankle was tried stiffer than the rest of the leg, on the theory that
	// it carries a 1 kg foot rather than the body's weight. It measured worse:
	// 40 gives 2.6 deg of tilt, 80 gives 3.2, 160 gives 8.3. A motor is a
	// velocity constraint solved once per step, so past roughly 2/dt it
	// overshoots and rings -- the same ceiling the leg stiffness sweep found.
	float m_AnkleStiffness = 40.0f;
	float m_LegStiffness = 40.0f;
	float m_LegTorqueScale = 4.0f;         // how high the foot arcs over

	// Getting up.
	float m_GetUpTime = 2.0f;         // three stages now
	float m_GetUpHands = 0.38f;       // fraction spent getting up onto the hands
	float m_GetUpGather = 0.72f;      // ...and by here the feet are under him
	float m_HandForward = 0.55f;      // how far ahead of the hips the hands land
	float m_HandWidth = 0.22f;
	float m_HandHeight = 0.06f;       // the forearm's own radius off the floor
	float m_PushUpHeight = 0.38f;     // hips at the top of the push
	float m_GetUpShift = 0.20f;       // how far the weight travels onto the hands
	glm::vec3 m_HandSpot[2] = {};
	float m_KneelHeight = 0.55f;      // how high the hips get before rising
	float m_GetUpRoll = 0.6f;
	float m_GetUpFootLift = 0.18f;         // extra roll when face up
	float m_GetUpGround = 0.0f;
	bool m_GetUpFaceDown = true;
	bool m_GetUpSupine = false;
	float m_GetUpTimer = 0.0f;
	glm::vec3 m_GetUpFrom{ 0.0f };
	glm::vec3 m_GetUpTo{ 0.0f };
	glm::quat m_GetUpFromOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
	float m_StandHeight = 1.09f;
	float m_GaitPhase = 0.0f;
	// Bounded by what the leg can reach, not by what looks like a big step.
	// Half a stride forward with the hips 0.81 m up on a 0.90 m leg is about
	// 0.39 m at full extension; 0.45 keeps it inside that with room spare.
	// One full cycle -- two steps -- of travel. 1.2 m at 1.3 m/s is a step
	// length of 0.60 m at 2.2 steps a second, which is roughly what a person
	// does. The foot lands a quarter of this ahead of the body, the hip has to
	// drop 0.051 m for that to be reachable, and both of those are human
	// numbers too. It was 0.45, which is a 0.22 m step at seven a second.
	float m_StrideLength = 1.2f;
	float m_StrideAngle = 0.45f;
	float m_GaitKnee = 0.9f;

	BalanceState m_Balance;
	bool m_BalanceEnabled = true;
	// Swept rather than guessed. Standing time against gain, 20 s budget:
	// pitch 1.5 fell at 4.6 s whatever the roll; pitch 2.0 with roll 4.0 held
	// the full 20 s, as did pitch 3.0/roll 4.0 and pitch 4.0/roll 2.5. Roll
	// gain matters more than pitch, which is the lateral axis being the weak
	// one. These sit in the middle of the region that held rather than on its
	// edge. Too much is as bad as too little -- gain 20 fell in half a second.
	float m_AnkleGain = 3.0f;
	float m_AnkleRollGain = 4.0f;

	bool m_MotorsEnabled = true;
	float m_MotorStiffness = 14.0f;
};
