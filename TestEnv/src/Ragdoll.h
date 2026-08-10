#pragma once

// A jointed humanoid: thirteen bodies, twelve joints, and a switch between
// falling like a corpse and standing like a person.
//
// This demo is the point of all the joint work. Nothing here is new physics --
// it is a rig, and the interesting thing about a rig is that it is where the
// constraint code stops being tested in twos and starts being tested in a
// chain of fifteen. A ragdoll is the case that finds solver weaknesses: long
// kinematic chains, wildly different masses meeting at one joint (a 24 kg
// torso against a 1.6 kg forearm), and limits that must hold while contacts
// push back.
//
// Two modes, one key apart:
//
//   * **Motors off** -- a passive ragdoll. It falls, folds at its limits, and
//     lies still. This is what joints alone give you.
//   * **Motors on** -- every joint drives towards the pose it was built in. It
//     stands, sways, and gives when shoved. This is not balance: nothing here
//     knows where its feet are or what its centre of mass is doing. Push it
//     hard enough and it topples, still trying to hold the pose on the way
//     down, which is exactly what a powered ragdoll without a balance
//     controller looks like.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Demo.h"

class Ragdoll : public DemoLayer
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

	Ragdoll()
		: DemoLayer("Ragdoll"), m_Camera(50.0f, 16.0f / 9.0f, 0.1f, 200.0f)
	{
	}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.2f, 1.6f, 4.2f });
		m_Camera.SetRotation(-90.0f, -6.0f);

		m_Cube.reset(Egss::Mesh::CreateCube(1.0f));
		m_Sphere.reset(Egss::Mesh::CreateSphere(0.5f, 24, 12));
		// Unit radius and unit half-height, so the scale in the draw call is
		// the capsule's own diameter and segment length.
		m_Cylinder.reset(Egss::Mesh::CreateCylinder(0.5f, 0.5f, 24));

		// The lit solid-colour shader Physics3D registers. Shared through the
		// library rather than copied, which is what the library is for --
		// and safe to depend on because `OnAttach` is deliberately not guarded
		// by the active demo, so every demo's assets are built at startup.
		m_SceneMaterial = Egss::Material::Create(
			Egss::Renderer::GetShaderLibrary().Get("Physics3D"));

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		m_SteppingEnabled = false;
		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();

		BuildScene();
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
			Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(
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
			Egss::RigidBody3D body = Egss::RigidBody3D::MakeBox(
				origin + centre, halfExtents, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		auto sphere = [&](glm::vec3 centre, float radius, float mass)
		{
			Egss::RigidBody3D body = Egss::RigidBody3D::MakeSphere(
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
		auto ball = [&](Egss::PhysicsWorld3D::BodyHandle a,
			Egss::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
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

		auto hinge = [&](Egss::PhysicsWorld3D::BodyHandle a,
			Egss::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
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
		ball(m_Pelvis, m_Torso, { 0.0f, 1.18f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 25.0f, 35.0f, 260.0f);
		ball(m_Torso, m_Head, { 0.0f, 1.62f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 40.0f, 60.0f, 30.0f);

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

			// TRY: widen the knee's range past 130 degrees and watch the shin
			// pass through the thigh -- the limit is the only thing stopping
			// it, since jointed bodies do not collide with each other.
			m_KneeJoint[side] = hinge(m_UpperLeg[side], m_LowerLeg[side], m_KneeAt[side],
				{ 1.0f, 0.0f, 0.0f }, -130.0f, 0.0f, 160.0f);

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

			// Shoulder and elbow.
			ball(m_Torso, m_UpperArm[side], { shoulderSide, 1.55f, 0.0f },
				{ 0.0f, 1.0f, 0.0f }, 75.0f, 45.0f, 60.0f);

			hinge(m_UpperArm[side], m_LowerArm[side], { armX, 1.20f, 0.0f },
				{ 1.0f, 0.0f, 0.0f }, 0.0f, 140.0f, 35.0f);
		}

		SetMotorsEnabled(m_MotorsEnabled);

		// Remembered so going limp and back is reversible -- each joint has its
		// own budget and one number could not restore them.
		m_JointTorque.clear();
		for (auto handle : m_Joints)
			m_JointTorque[handle] = m_World.GetJoint(handle).MotorMaxTorque;

		m_CharacterBodies = { m_Pelvis, m_Torso, m_Head,
			m_UpperLeg[0], m_UpperLeg[1], m_LowerLeg[0], m_LowerLeg[1],
			m_Foot[0], m_Foot[1],
			m_UpperArm[0], m_UpperArm[1], m_LowerArm[0], m_LowerArm[1] };

		m_StandHeight = m_World.GetBody(m_Pelvis).Position.y;
		GoControlled();
	}

	void BuildScene()
	{
		m_World.Clear();
		m_Joints.clear();
		m_Stepping = false;
		m_Shifting = false;
		m_ShiftTimer = 0.0f;
		m_WaitingForTrough = false;
		m_WaitTimer = 0.0f;
		m_TroughsUsed = 0;
		m_TroughsMissed = 0;
		m_StepsTaken = 0;
		m_StepTimer = 0.0f;

		m_World.Gravity = { 0.0f, -9.81f, 0.0f };
		// A ragdoll is a fifteen-link chain, and a chain solved with too few
		// iterations does not merely sag -- it gains energy and flails. Eight
		// is the floor; this is comfortably above it because the mass ratios
		// here are severe.
		m_World.VelocityIterations = 16;
		m_World.PositionIterations = 6;

		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox(
			{ 0.0f, -0.5f, 0.0f }, { 8.0f, 0.5f, 8.0f }));

		// Something to fall over.
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox(
			{ 1.6f, 0.15f, 0.0f }, { 0.4f, 0.15f, 0.4f }));

		BuildRagdoll({ 0.0f, m_DropHeight, 0.0f });
	}

	void SetMotorsEnabled(bool enabled)
	{
		m_MotorsEnabled = enabled;
		for (auto handle : m_Joints)
		{
			Egss::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorEnabled = enabled;
			joint.MotorStiffness = m_MotorStiffness;
		}
	}

	// A shove at chest height, in the direction the camera is looking.
	void Push(float strength)
	{
		glm::vec3 direction = m_Camera.GetForward();
		direction.y = 0.0f;
		if (glm::length(direction) < 0.001f)
			direction = { 0.0f, 0.0f, -1.0f };

		direction = glm::normalize(direction);

		const Egss::RigidBody3D& torso = m_World.GetBody(m_Torso);
		m_World.ApplyImpulseAt(m_Torso, direction * strength, torso.Position);

		// A shove applied straight to a body never arrives through a contact,
		// so ImpactForce would never see it. Judged on its own terms: the
		// impulse over one step is the force it represents.
		if (strength * 60.0f > m_RagdollThreshold)
			GoRagdoll("shove");
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
			const Egss::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
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
				const Egss::RigidBody3D& thigh = m_World.GetBody(m_UpperLeg[m_SwingSide]);
				const Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
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
		const Egss::RigidBody3D& foot = m_World.GetBody(m_Foot[m_SwingSide]);
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
		m_World.GetJoint(m_KneeJoint[m_SwingSide]).MotorTargetAngle = -m_StepLift * lift;

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
			const Egss::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
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

			Egss::Joint3D& joint = m_World.GetJoint(m_AnkleJoint[side]);
			joint.MotorTargetRotation = glm::normalize(correction * m_AnkleRest[side]);
		}
	}

	void OnDemoFixedUpdate(Egss::Timestep fixedStep) override
	{
		MoveCamera(fixedStep);

		if (m_PendingPush != 0.0f)
		{
			Push(m_PendingPush);
			m_PendingPush = 0.0f;
		}

		if (m_Paused)
			return;

		m_Balance = ComputeBalance();

		if (m_Mode == Mode::Controlled)
		{
			// A hard enough hit anywhere but the feet ends control. Checked
			// before the step, so the impulse judged is the one that arrived
			// during the last one.
			if (ImpactForce(fixedStep) > m_RagdollThreshold)
				GoRagdoll("impact");

			if (m_Mode == Mode::Controlled)
				DriveWalk(fixedStep);
		}
		else
		{
			m_RagdollTimer += fixedStep;
		}

		if (m_Mode == Mode::Ragdoll && m_BalanceEnabled)
		{
			// Stepping decides first: while a foot is in the air the ankle
			// strategy must not fight the swing leg for the same joints.
			UpdateStepping(m_Balance, fixedStep);
			DriveSwingLeg();
			ApplyBalance(m_Balance);
		}

		m_World.Step(fixedStep);
	}

	void MoveCamera(Egss::Timestep step)
	{
		float move = m_CameraSpeed * step;
		glm::vec3 position = m_Camera.GetPosition();

		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += move;

		m_Camera.SetPosition(position);

		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();
		float turn = 70.0f * step;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= turn;

		m_Camera.SetRotation(yaw, pitch);
	}

	// ---------------------------------------------------------------------
	// Drawing -- every body as its own collider
	// ---------------------------------------------------------------------
	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		float alpha = Egss::Application::Get().GetInterpolationAlpha();

		Egss::RenderCommand::SetClearColor({ 0.05f, 0.06f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();
		Egss::RenderCommand::SetDepthTest(true);

		Egss::Renderer::BeginScene(m_Camera);

		m_SceneMaterial->Set("u_LightPosition", glm::vec3(2.0f, 4.0f, 3.0f));
		m_SceneMaterial->Set("u_LightColor", glm::vec3(1.0f, 0.97f, 0.9f));
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", 0.30f);

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody3D& body = bodies[i];

			glm::vec3 position = glm::mix(body.PreviousPosition, body.Position, alpha);
			glm::quat orientation = glm::slerp(body.PreviousOrientation, body.Orientation, alpha);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
				* glm::mat4_cast(orientation);

			glm::vec4 colour;
			if (body.Type == Egss::BodyType::Static)
				colour = { 0.34f, 0.36f, 0.42f, 1.0f };
			else if (i == m_Head)
				colour = { 0.92f, 0.78f, 0.62f, 1.0f };
			else if (i == m_Torso || i == m_Pelvis)
				colour = { 0.36f, 0.55f, 0.80f, 1.0f };
			else
				colour = { 0.80f, 0.55f, 0.35f, 1.0f };

			if (body.Type != Egss::BodyType::Static && !body.Awake)
				colour = glm::vec4(glm::vec3(colour) * 0.45f, 1.0f);

			m_SceneMaterial->Set("u_Color", colour);

			if (body.Shape == Egss::ColliderShape3D::Box)
			{
				Egss::Renderer::Submit(m_SceneMaterial, m_Cube,
					glm::scale(transform, body.HalfExtents * 2.0f));
			}
			else if (body.Shape == Egss::ColliderShape3D::Capsule)
			{
				// Cylinder shaft, spherical caps -- exactly what a capsule is.
				float d = body.Radius * 2.0f;

				Egss::Renderer::Submit(m_SceneMaterial, m_Cylinder,
					glm::scale(transform, { d, body.HalfHeight * 2.0f, d }));

				for (float end : { -1.0f, 1.0f })
				{
					glm::mat4 cap = glm::translate(transform,
						{ 0.0f, end * body.HalfHeight, 0.0f });
					Egss::Renderer::Submit(m_SceneMaterial, m_Sphere,
						glm::scale(cap, glm::vec3(d)));
				}
			}
			else
			{
				Egss::Renderer::Submit(m_SceneMaterial, m_Sphere,
					glm::scale(transform, glm::vec3(body.Radius * 2.0f)));
			}
		}

		Egss::Renderer::EndScene();

		// The balance state, drawn as lines in the perspective scene --
		// Renderer2D::BeginScene takes any camera, which is what makes debug
		// geometry in a 3D scene possible without a second renderer.
		if (m_ShowBalance && m_Balance.Support.size() >= 3)
		{
			Egss::RenderCommand::SetDepthTest(false);
			Egss::Renderer2D::BeginScene(m_Camera);

			const float y = 0.02f;   // just off the floor, or it z-fights

			// The support polygon: everywhere the feet actually touch.
			for (size_t i = 0; i < m_Balance.Support.size(); i++)
			{
				const glm::vec2& a = m_Balance.Support[i];
				const glm::vec2& b = m_Balance.Support[(i + 1) % m_Balance.Support.size()];

				Egss::Renderer2D::DrawLine({ a.x, y, a.y }, { b.x, y, b.y },
					{ 0.4f, 0.9f, 1.0f, 1.0f });
			}

			// The centre of mass, dropped to the floor: a plumb line and a
			// cross where it lands.
			glm::vec3 com = m_Balance.Com;
			Egss::Renderer2D::DrawLine(com, { com.x, y, com.z }, { 1.0f, 1.0f, 0.4f, 1.0f });

			auto cross = [&](glm::vec2 at, glm::vec4 colour, float size)
			{
				Egss::Renderer2D::DrawLine({ at.x - size, y, at.y }, { at.x + size, y, at.y }, colour);
				Egss::Renderer2D::DrawLine({ at.x, y, at.y - size }, { at.x, y, at.y + size }, colour);
			};

			cross(m_Balance.ComGround, { 1.0f, 1.0f, 0.4f, 1.0f }, 0.06f);

			// The capture point, which is the one that decides. Green while it
			// is over the feet, red once it is not -- and it turns red before
			// the figure looks like it is falling, which is the whole idea.
			cross(m_Balance.CapturePoint,
				m_Balance.Standing ? glm::vec4(0.4f, 1.0f, 0.5f, 1.0f)
				                   : glm::vec4(1.0f, 0.35f, 0.3f, 1.0f), 0.09f);

			Egss::Renderer2D::EndScene();
			Egss::RenderCommand::SetDepthTest(true);
		}
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);
		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& key)
		{
			if (key.GetKeyCode() == EGSS_KEY_R) { BuildScene(); return true; }
			if (key.GetKeyCode() == EGSS_KEY_P) { m_Paused = !m_Paused; return true; }
			if (key.GetKeyCode() == EGSS_KEY_M) { SetMotorsEnabled(!m_MotorsEnabled); return true; }
			if (key.GetKeyCode() == EGSS_KEY_B) { m_BalanceEnabled = !m_BalanceEnabled; return true; }
			if (key.GetKeyCode() == EGSS_KEY_G)
			{
				m_Mode == Mode::Controlled ? GoRagdoll("key") : GoControlled();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_SPACE) { m_PendingPush = m_PushStrength; return true; }
			return false;
		});
	}

	// ---------------------------------------------------------------------
	// Panel
	// ---------------------------------------------------------------------
	void OnDemoImGui() override
	{
		ImGui::Begin("Ragdoll");

		ImGui::TextColored(m_Mode == Mode::Controlled
			? ImVec4(0.5f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
			m_Mode == Mode::Controlled ? "CONTROLLED" : "RAGDOLL (%s, %.1f s)",
			m_RagdollReason, m_RagdollTimer);

		ImGui::Text("IJKL walk   G toggle ragdoll   Space push   R reset");
		ImGui::Text("M motors   B balance   P pause");
		ImGui::Text("Steps taken: %d", m_StepsTaken);
		ImGui::Text("WASD/QE move   arrows look");

		// Said here because otherwise it reads as a bug. It is not: a standing
		// figure is an inverted pendulum on two small feet, and the motors
		// hold *joint angles*, which do not change when the whole body tips
		// about its ankles. Staying up needs a balance controller -- centre of
		// mass against the support polygon, and a step when it leaves -- and
		// there is not one yet.
		ImGui::TextWrapped("Controlled: the pelvis is kinematic, so the character cannot "
			"fall and does not balance -- the limbs still hang off it and react. A hit "
			"harder than the threshold below makes the pelvis dynamic and physics takes "
			"over. That switch is how games do this; balancing on the skeleton is the "
			"hard problem it avoids.");

		ImGui::Separator();
		ImGui::Text("Bodies: %zu   Joints: %zu", m_World.GetBodyCount(), m_World.GetJointCount());
		ImGui::Text("Contacts: %zu", m_World.GetContacts().size());

		// The honest measure of whether the solver is keeping up with a chain
		// this long. A limb visibly pulling out of its socket shows up here
		// first, as millimetres.
		ImGui::Text("Worst joint separation: %.4f m", m_World.GetWorstJointSeparation());

		ImGui::Separator();
		ImGui::Text("Support polygon: %zu points", m_Balance.Support.size());
		ImGui::Text("Centre of mass: (%.2f, %.2f, %.2f)",
			m_Balance.Com.x, m_Balance.Com.y, m_Balance.Com.z);

		// The number that decides whether it stays up. Positive is the capture
		// point inside the feet; it crosses zero *before* the figure visibly
		// starts to fall, which is what makes it worth watching rather than
		// watching the figure.
		ImGui::TextColored(m_Balance.Standing ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
			                                  : ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
			"Capture margin: %+.3f m  (%s)", m_Balance.Margin,
			m_Balance.Standing ? "over its feet" : "falling");

		// Body weight is about 697 N, so half of it on each foot is standing
		// square. A foot cannot be lifted until its share is near zero.
		ImGui::Text("Foot load: left %.0f N, right %.0f N  (weight is 697 N)",
			FootLoad(0, 1.0f / 60.0f), FootLoad(1, 1.0f / 60.0f));

		ImGui::Text("Frame: %.2f ms", m_FrameTime);

		ImGui::Separator();
		if (ImGui::Checkbox("Motors", &m_MotorsEnabled))
			SetMotorsEnabled(m_MotorsEnabled);

		// TRY: drop the stiffness towards 2 and the figure sags like something
		// exhausted; raise it towards 40 and it snaps rigidly upright. Neither
		// is balance -- both topple identically once the centre of mass leaves
		// the feet.
		if (ImGui::SliderFloat("Motor stiffness", &m_MotorStiffness, 1.0f, 40.0f))
			SetMotorsEnabled(m_MotorsEnabled);

		ImGui::Checkbox("Balance", &m_BalanceEnabled);
		ImGui::SameLine();
		// TRY: turn this on and watch it make things worse. The step fires at
		// the right moment and aims at the right place; what it does not do is
		// shift the weight onto the stance leg first, so lifting a foot throws
		// the body as much as the step recovers.
		ImGui::Checkbox("Stepping (worse)", &m_SteppingEnabled);
		ImGui::SameLine();
		ImGui::Checkbox("Show balance", &m_ShowBalance);

		// TRY: turn the ankle gain down to zero and push gently. The figure
		// leans and keeps leaning, because nothing is shifting its weight back
		// over its feet.
		ImGui::SliderFloat("Ankle pitch gain", &m_AnkleGain, 0.0f, 8.0f);
		ImGui::SliderFloat("Ankle roll gain", &m_AnkleRollGain, 0.0f, 8.0f);
		ImGui::SliderFloat("Push strength", &m_PushStrength, 10.0f, 400.0f);
		// TRY: drop this towards 500 and the character falls over at the
		// slightest contact; raise it past 10000 and nothing knocks them down.
		ImGui::SliderFloat("Ragdoll threshold (N)", &m_RagdollThreshold, 200.0f, 8000.0f);
		ImGui::SliderFloat("Walk speed", &m_WalkSpeed, 0.0f, 4.0f);
		ImGui::SliderFloat("Drop height", &m_DropHeight, 0.0f, 3.0f);
		ImGui::SliderInt("Velocity iterations", (int*)&m_World.VelocityIterations, 4, 40);
		ImGui::Checkbox("Allow sleeping", &m_World.AllowSleeping);

		if (ImGui::Button("Reset"))
			BuildScene();

		ImGui::End();
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
	enum class Mode { Controlled, Ragdoll };

	void GoRagdoll(const char* why)
	{
		if (m_Mode == Mode::Ragdoll)
			return;

		m_Mode = Mode::Ragdoll;
		m_RagdollReason = why;
		m_RagdollTimer = 0.0f;

		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = Egss::BodyType::Dynamic;

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
			Egss::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_LimpStiffness;
			joint.MotorMaxTorque = m_LimpTorque;
		}
	}

	void GoControlled()
	{
		m_Mode = Mode::Controlled;
		m_RagdollTimer = 0.0f;

		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = Egss::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);

		for (auto handle : m_Joints)
		{
			Egss::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_MotorStiffness;
			joint.MotorMaxTorque = m_JointTorque[handle];
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
	float ImpactForce(float dt) const
	{
		if (dt <= 0.0f)
			return 0.0f;

		float worst = 0.0f;
		for (const Egss::Contact3D& contact : m_World.GetContacts())
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

			worst = std::max(worst, contact.TotalNormalImpulse() / dt);
		}

		return worst;
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
	void DriveWalk(float dt)
	{
		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);

		// --- steering -------------------------------------------------------
		glm::vec3 wish(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_I)) wish.z -= 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_K)) wish.z += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_J)) wish.x -= 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_L)) wish.x += 1.0f;

		float wishLength = glm::length(wish);
		if (wishLength > 0.0f)
		{
			wish /= wishLength;
			m_Facing = std::atan2(wish.x, wish.z);
		}

		glm::vec3 wanted = wish * m_WalkSpeed;

		// Held at its standing height by velocity rather than by teleporting,
		// so the render interpolation still has something continuous to work
		// with between fixed steps.
		float heightError = m_StandHeight - pelvis.Position.y;
		wanted.y = heightError / std::max(dt, 1e-4f);

		pelvis.Velocity = wanted;
		pelvis.Orientation = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));
		pelvis.UpdateInertiaWorld();

		// --- the gait --------------------------------------------------------
		// Phase advances with distance covered, not with time, so the legs do
		// not cycle while standing still and do not moonwalk when the speed
		// changes.
		float speed = glm::length(glm::vec2(pelvis.Velocity.x, pelvis.Velocity.z));
		m_GaitPhase += (speed / std::max(m_StrideLength, 0.01f)) * glm::two_pi<float>() * dt;

		if (speed < 0.05f)
		{
			// Ease back to standing rather than freezing mid-stride.
			m_GaitPhase = 0.0f;
		}

		for (int side = 0; side < 2; side++)
		{
			float phase = m_GaitPhase + (side == 0 ? 0.0f : glm::pi<float>());

			float swing = std::sin(phase) * m_StrideAngle * std::min(speed / m_WalkSpeed, 1.0f);
			// Knee bends only while the leg is coming through, which is the
			// half of the cycle where the foot would otherwise scuff.
			float bend = std::max(0.0f, -std::cos(phase)) * m_GaitKnee
				* std::min(speed / m_WalkSpeed, 1.0f);

			glm::quat hipTarget = glm::angleAxis(swing, glm::vec3(1.0f, 0.0f, 0.0f));
			m_World.GetJoint(m_HipJoint[side]).MotorTargetRotation =
				glm::normalize(hipTarget * m_HipRest[side]);
			m_World.GetJoint(m_KneeJoint[side]).MotorTargetAngle = -bend;
		}
	}

	// ---------------------------------------------------------------------
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

		float total = 0.0f;
		for (const Egss::RigidBody3D& body : m_World.GetBodies())
		{
			if (body.Type == Egss::BodyType::Static)
				continue;

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
		for (const Egss::Contact3D& contact : m_World.GetContacts())
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
		for (const Egss::Contact3D& contact : m_World.GetContacts())
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

		for (const Egss::RigidBody3D& body : m_World.GetBodies())
		{
			if (body.Type == Egss::BodyType::Static)
				continue;

			float mass = body.GetMass();
			weighted += body.Position * mass;
			total += mass;
		}

		return total > 0.0f ? weighted / total : glm::vec3(0.0f);
	}

private:
	Egss::PerspectiveCamera m_Camera;
	Egss::PhysicsWorld3D m_World;

	std::shared_ptr<Egss::Mesh> m_Cube;
	std::shared_ptr<Egss::Mesh> m_Sphere;
	std::shared_ptr<Egss::Mesh> m_Cylinder;
	std::shared_ptr<Egss::Material> m_SceneMaterial;

	using Handle = Egss::PhysicsWorld3D::BodyHandle;

	Handle m_Pelvis = 0, m_Torso = 0, m_Head = 0;
	Handle m_UpperLeg[2] = {}, m_LowerLeg[2] = {}, m_Foot[2] = {};
	Handle m_UpperArm[2] = {}, m_LowerArm[2] = {};

	std::vector<Egss::PhysicsWorld3D::JointHandle> m_Joints;
	std::vector<Egss::PhysicsWorld3D::BodyHandle> m_CharacterBodies;

	Egss::PhysicsWorld3D::JointHandle m_HipJoint[2] = {};
	Egss::PhysicsWorld3D::JointHandle m_KneeJoint[2] = {};
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

	Egss::PhysicsWorld3D::JointHandle m_AnkleJoint[2] = {};
	glm::quat m_HipRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                           glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
	glm::quat m_AnkleRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                             glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

	Mode m_Mode = Mode::Controlled;
	const char* m_RagdollReason = "";
	float m_RagdollTimer = 0.0f;
	std::unordered_map<Egss::PhysicsWorld3D::JointHandle, float> m_JointTorque;

	// A hit above this many newtons ends control. Body weight is about 697 N,
	// so the default is roughly three times the character's weight -- enough
	// that walking into scenery does not floor them.
	float m_RagdollThreshold = 2000.0f;
	float m_LimpStiffness = 1.5f;
	float m_LimpTorque = 8.0f;

	// Walking.
	float m_Facing = 0.0f;
	float m_WalkSpeed = 1.6f;
	float m_StandHeight = 1.09f;
	float m_GaitPhase = 0.0f;
	float m_StrideLength = 0.85f;
	float m_StrideAngle = 0.45f;
	float m_GaitKnee = 0.9f;

	BalanceState m_Balance;
	bool m_BalanceEnabled = true;
	bool m_ShowBalance = true;
	// Swept rather than guessed. Standing time against gain, 20 s budget:
	// pitch 1.5 fell at 4.6 s whatever the roll; pitch 2.0 with roll 4.0 held
	// the full 20 s, as did pitch 3.0/roll 4.0 and pitch 4.0/roll 2.5. Roll
	// gain matters more than pitch, which is the lateral axis being the weak
	// one. These sit in the middle of the region that held rather than on its
	// edge. Too much is as bad as too little -- gain 20 fell in half a second.
	float m_AnkleGain = 3.0f;
	float m_AnkleRollGain = 4.0f;

	bool m_MotorsEnabled = true;
	bool m_Paused = false;
	float m_MotorStiffness = 14.0f;
	float m_PushStrength = 120.0f;
	float m_DropHeight = 0.0f;
	float m_PendingPush = 0.0f;
	float m_CameraSpeed = 3.0f;
	float m_FrameTime = 0.0f;
};
