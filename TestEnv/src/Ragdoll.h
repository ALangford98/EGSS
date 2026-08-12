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
// **WASD** walks, relative to the camera, **shift** runs and **space** jumps.
// **Arrows** orbit. **G** drops the character into a ragdoll, and again to get
// back up. **X** shoves them -- gently and they stagger and recover, hard
// enough and they go down. **F** swaps the follow camera for the old
// fly-around.
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
		: Ragdoll("Ragdoll")
	{
	}

	// For a demo that reuses this rig and wants its own name in the selector.
	explicit Ragdoll(const std::string& name)
		: DemoLayer(name), m_Camera(50.0f, 16.0f / 9.0f, 0.1f, 200.0f)
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
		m_SpineJoint = ball(m_Pelvis, m_Torso, { 0.0f, 1.18f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }, 25.0f, 35.0f, 260.0f);
		m_SpineRest = m_World.GetJoint(m_SpineJoint).MotorTargetRotation;
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

		// What the impulse model divides by.
		m_BodyMass = 0.0f;
		for (unsigned int body : m_CharacterBodies)
			m_BodyMass += m_World.GetBody(body).GetMass();

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
			const Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
			for (int side = 0; side < 2; side++)
			{
				glm::vec3 hipAt(side == 0 ? -0.10f : 0.10f, 1.00f, 0.0f);
				hipAt += origin;

				m_HipLocal[side] = glm::conjugate(pelvis.Orientation) * (hipAt - pelvis.Position);

				// Same for the arm, measured off the pose it was built in, so
				// the get-up's hand IK solves for the arm that exists.
				const Egss::RigidBody3D& torso = m_World.GetBody(m_Torso);
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

				// Both feet start planted where they were built.
				m_AnkleHeight = ankleWorld.y - m_World.GroundHeightBelow(
					ankleWorld + glm::vec3(0.0f, 1.0f, 0.0f), m_Pelvis);
				m_Planted[side] = ankleWorld;
				m_Planted[side].y = m_World.GroundHeightBelow(
					ankleWorld + glm::vec3(0.0f, 1.0f, 0.0f), m_Pelvis) + m_AnkleHeight;
				m_SwingFrom[side] = m_Planted[side];
				m_SwingTo[side] = m_Planted[side];
				m_Swinging[side] = false;
			}
		}

		m_CharacterBodies = { m_Pelvis, m_Torso, m_Head,
			m_UpperLeg[0], m_UpperLeg[1], m_LowerLeg[0], m_LowerLeg[1],
			m_Foot[0], m_Foot[1],
			m_UpperArm[0], m_UpperArm[1], m_LowerArm[0], m_LowerArm[1] };

		m_StandHeight = m_World.GetBody(m_Pelvis).Position.y;
		// How far the pelvis rides above whatever it stands on, taken from the
		// pose the rig was built in rather than written down twice.
		m_StandClearance = m_StandHeight
			- m_World.GroundHeightBelow(m_World.GetBody(m_Pelvis).Position, m_Pelvis);
		m_RootAnchor = m_World.GetBody(m_Pelvis).Position;
		m_CameraFocus = m_World.GetBody(m_Torso).Position;
		m_Speed = 0.0f;
		m_GaitPhase = 0.0f;
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

		BuildGround();
		BuildRagdoll(SpawnPoint());
	}

	// The world the character stands in, and the one thing a demo that reuses
	// this rig is expected to replace. Everything else here -- the gait, the
	// balance, the get-up -- already asks the world where the ground is rather
	// than assuming it is at zero, which is what makes swapping this out a
	// three-line change instead of a rewrite.
	virtual void BuildGround()
	{
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox(
			{ 0.0f, -0.5f, 0.0f }, { 8.0f, 0.5f, 8.0f }));

		// A step to walk up. 0.30 m to the top, which is a tall domestic stair
		// and about the limit of what the gait will lift a foot over.
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox(
			{ 0.0f, 0.15f, 2.6f }, { 1.4f, 0.15f, 1.2f }));

		// And something smaller to trip over.
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox(
			{ 1.6f, 0.08f, 0.0f }, { 0.3f, 0.08f, 0.3f }));
	}

	// Where the rig is assembled. On flat ground that is the origin; on terrain
	// it has to be the surface, because the whole rig is laid out upwards from
	// this point and a figure built 5 m under a hill starts inside it.
	virtual glm::vec3 SpawnPoint() const
	{
		return { 0.0f, m_DropHeight, 0.0f };
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
				Egss::Joint3D& joint = m_World.GetJoint(handle);
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
		// The impulse is what was applied; no need to turn it into a force and
		// back again.
		if (m_Mode == Mode::Controlled)
			Stagger(strength, direction);
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
			glm::vec3 hitFrom(0.0f);
			float hit = ImpactForce(fixedStep, hitFrom);

			// Everything above the noise floor is a stagger now. Whether he
			// actually goes down is decided by whether his feet can catch what
			// it did to him, not by the size of the number.
			if (hit > m_StaggerThreshold)
				Stagger(hit * fixedStep, hitFrom);

			if (m_Mode == Mode::Controlled)
				DriveWalk(fixedStep);
		}
		else if (m_Mode == Mode::GettingUp)
		{
			UpdateGetUp(fixedStep);
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

	// A camera that follows rather than one that is flown.
	//
	// It orbits a point on the character at a fixed distance, and both the
	// point and the distance are eased rather than followed exactly. The lag
	// is the whole reason it feels attached to something with weight -- a
	// camera pinned rigidly to the hips inherits the walk's bob and sway and
	// makes the picture seasick.
	void MoveCamera(Egss::Timestep step)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		float dt = step;

		// Orbit on the arrows. Pitch is clamped short of straight down, which
		// is where a follow camera flips over and loses its horizon.
		float turn = 90.0f * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  m_CameraYaw -= turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) m_CameraYaw += turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    m_CameraPitch -= turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  m_CameraPitch += turn;

		m_CameraPitch = glm::clamp(m_CameraPitch, -60.0f, 25.0f);

		if (m_FreeCamera)
		{
			// The old fly-around, kept for looking at things. WASD belongs to
			// the character now, so this borrows it back while it is on.
			glm::vec3 position = m_Camera.GetPosition();
			float move = m_CameraSpeed * dt;

			if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += m_Camera.GetForward() * move;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= m_Camera.GetForward() * move;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= m_Camera.GetRight() * move;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += m_Camera.GetRight() * move;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= move;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += move;

			m_Camera.SetPosition(position);
			m_Camera.SetRotation(m_CameraYaw, -m_CameraPitch);
			return;
		}

		// Follow the torso rather than the pelvis: the pelvis carries the bob
		// and the sway deliberately, and the camera should not.
		glm::vec3 focus = m_World.GetBody(m_Torso).Position;
		focus.y = m_World.GetBody(m_Pelvis).Position.y + m_CameraHeight;

		// Eased towards it. A follow that is exact is a follow that shakes.
		float blend = 1.0f - std::exp(-m_CameraLag * dt);
		m_CameraFocus += (focus - m_CameraFocus) * blend;

		// The camera sits back along its own forward vector, so pointing it at
		// its own yaw and pitch looks exactly at the focus with no aiming
		// arithmetic at all.
		float yaw = glm::radians(m_CameraYaw);
		float pitch = glm::radians(-m_CameraPitch);

		glm::vec3 forward(std::cos(yaw) * std::cos(pitch),
			std::sin(pitch),
			std::sin(yaw) * std::cos(pitch));

		glm::vec3 wanted = m_CameraFocus - forward * m_CameraDistance;

		// Never let it drop through the floor, which is the one way an orbit
		// camera ends up underground looking at the inside of the world.
		float ground = m_World.GroundHeightBelow(wanted, m_Pelvis);
		wanted.y = std::max(wanted.y, ground + 0.35f);

		m_Camera.SetPosition(wanted);
		m_Camera.SetRotation(m_CameraYaw, -m_CameraPitch);
	}

	// ---------------------------------------------------------------------
	// Drawing -- every body as its own collider
	// ---------------------------------------------------------------------
	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		float alpha = Egss::Application::Get().GetInterpolationAlpha();

		Egss::RenderCommand::SetClearColor(ClearColour());
		Egss::RenderCommand::Clear();
		Egss::RenderCommand::SetDepthTest(true);

		Egss::Renderer::BeginScene(m_Camera);

		SetSceneLighting();
		DrawWorld();

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody3D& body = bodies[i];

			// A heightfield has no primitive to stand in for it; whoever put it
			// in the world draws it, in DrawWorld.
			if (body.Shape == Egss::ColliderShape3D::Heightfield)
				continue;

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
		// Only while ragdolling. Under control the character is not balancing --
		// the capture point sits outside its feet constantly and means nothing,
		// and drawing it implies a failure that is not happening.
		if (m_ShowBalance && m_Mode == Mode::Ragdoll && m_Balance.Support.size() >= 3)
		{
			Egss::RenderCommand::SetDepthTest(false);
			Egss::Renderer2D::BeginScene(m_Camera);

			// Just off the floor, or it z-fights -- and the floor is wherever
			// the character is standing, not zero. On flat ground this is the
			// 0.02 it always was; on terrain the polygon would otherwise be
			// drawn buried in a hillside.
			const float y = m_World.GroundHeightBelow(m_Balance.Com, m_Pelvis, -1000.0f) + 0.02f;

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

	// --- What a demo reusing this rig overrides to change the scenery -------

	virtual glm::vec4 ClearColour() const { return { 0.05f, 0.06f, 0.09f, 1.0f }; }

	// A point light a few metres up, which is right for a room-sized scene and
	// useless outdoors -- its `1/(1 + 0.015 d^2)` falloff leaves anything past
	// about 40 m lit by ambient alone.
	virtual void SetSceneLighting()
	{
		m_SceneMaterial->Set("u_LightPosition", glm::vec3(2.0f, 4.0f, 3.0f));
		m_SceneMaterial->Set("u_LightColor", glm::vec3(1.0f, 0.97f, 0.9f));
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", 0.30f);
	}

	// Geometry that is not a rigid body's collider. Inside the scene, so a
	// subclass does not have to know how one is begun or ended.
	virtual void DrawWorld() {}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);
		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& key)
		{
			if (key.GetKeyCode() == EGSS_KEY_R) { BuildScene(); return true; }
			if (key.GetKeyCode() == EGSS_KEY_P) { m_Paused = !m_Paused; return true; }
			if (key.GetKeyCode() == EGSS_KEY_M) { SetMotorsEnabled(!m_MotorsEnabled); return true; }
			if (key.GetKeyCode() == EGSS_KEY_B) { m_BalanceEnabled = !m_BalanceEnabled; return true; }
			if (key.GetKeyCode() == EGSS_KEY_F) { m_FreeCamera = !m_FreeCamera; return true; }
			if (key.GetKeyCode() == EGSS_KEY_G)
			{
				m_Mode == Mode::Controlled ? GoRagdoll("key") : BeginGetUp();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_X) { m_PendingPush = m_PushStrength; return true; }
			if (key.GetKeyCode() == EGSS_KEY_SPACE) { m_WantsJump = true; return true; }
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
			m_Mode == Mode::Controlled ? "CONTROLLED"
				: (m_Mode == Mode::GettingUp ? "GETTING UP" : "RAGDOLL (%s, %.1f s)"),
			m_RagdollReason, m_RagdollTimer);

		ImGui::Text("WASD walk   shift run   space jump   arrows orbit   G ragdoll / get up");
		ImGui::Text("X push   R reset   P pause   F free camera%s",
			m_FreeCamera ? " (ON -- WASD flies)" : "");
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
		// TRY: drop the reach or the step count and he becomes glass -- these
		// two are now the whole of what decides whether a hit puts him down.
		ImGui::SliderFloat("Catch reach (m)", &m_CatchReach, 0.1f, 1.0f);
		ImGui::SliderFloat("Catch reach sideways", &m_CatchReachSide, 0.1f, 1.0f);
		ImGui::SliderFloat("Recovery steps", &m_CatchSteps, 0.5f, 5.0f);
		ImGui::Text("Can still catch %.2f m/s; carrying %.2f", CatchableSpeed(),
			glm::length(m_StaggerVelocity));
		ImGui::SliderFloat("Walk speed", &m_WalkSpeed, 0.0f, 4.0f);
		ImGui::SliderFloat("Run speed", &m_RunSpeed, 1.0f, 8.0f);
		ImGui::SliderFloat("Jump height (m)", &m_JumpHeight, 0.1f, 1.5f);
		// TRY: bring this up to the ragdoll threshold and nothing staggers --
		// every hit is either ignored or a collapse, which is how it used to be.
		ImGui::SliderFloat("Stagger threshold (N)", &m_StaggerThreshold, 100.0f, 3000.0f);
		ImGui::SliderFloat("Arm swing", &m_ArmSwing, 0.0f, 1.2f);
		// TRY: on terrain, take this to 0 and watch the soles go back to being
		// level with the horizon -- the figure keeps walking, but it lands on a
		// heel or a toe every step and the mean sole tilt goes from 2 deg to 10.
		// On flat ground it does nothing at all, by construction.
		ImGui::SliderFloat("Sole follows slope", &m_SoleToSlope, 0.0f, 1.0f);
		ImGui::SliderFloat("Max sole tilt (deg)", &m_MaxSoleTilt, 0.0f, 35.0f);
		ImGui::Text("Gait: %.0f%% of the cycle on the ground, %s",
			100.0f * m_Duty, m_Duty < 0.5f ? "so it has flight" : "so it has double support");
		ImGui::SliderFloat("Get up over (s)", &m_GetUpTime, 0.2f, 3.0f);
		ImGui::SliderFloat("Camera distance", &m_CameraDistance, 1.5f, 10.0f);
		ImGui::SliderFloat("Camera lag", &m_CameraLag, 1.0f, 20.0f);
		ImGui::Text("Standing %.2f m above ground; step ahead %.0f%%",
			m_StandClearance, 100.0f * m_StepUp);
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
	enum class Mode { Controlled, Ragdoll, GettingUp };

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

	// Straight to standing, with no blend. Used when the scene is built and
	// when a blend finishes.
	void GoControlled()
	{
		m_Mode = Mode::Controlled;
		m_RagdollTimer = 0.0f;
		m_GetUpTimer = 0.0f;

		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = Egss::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);

		// The character stands up where it is lying, not where it fell from.
		m_RootAnchor = pelvis.Position;
		m_RootAnchor.y = m_StandHeight;
		m_Speed = 0.0f;
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

		for (auto handle : m_Joints)
		{
			Egss::Joint3D& joint = m_World.GetJoint(handle);
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

		m_Mode = Mode::GettingUp;
		m_GetUpTimer = 0.0f;

		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		m_GetUpFrom = pelvis.Position;
		m_GetUpFromOrientation = pelvis.Orientation;

		// Stand on whatever is underneath, not at the height of the floor it
		// started on -- otherwise getting up on a step sinks into it.
		m_GetUpGround = m_World.GroundHeightBelow(pelvis.Position, m_Pelvis);
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

		pelvis.Type = Egss::BodyType::Kinematic;
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

		Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);

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
			Egss::Joint3D& joint = m_World.GetJoint(handle);
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

		if (t >= 1.0f)
			GoControlled();
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
		glm::vec3 ignored(0.0f);
		return ImpactForce(dt, ignored);
	}

	// The same, and which way it pushed -- a stagger has to go somewhere.
	float ImpactForce(float dt, glm::vec3& direction) const
	{
		direction = glm::vec3(0.0f);

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

			float force = contact.TotalNormalImpulse() / dt;
			if (force > worst)
			{
				worst = force;

				// The manifold normal points from A to B, so it has to be
				// flipped when the character is A -- otherwise every stagger
				// goes into the thing that hit him.
				glm::vec3 push = mineA ? -contact.Normal : contact.Normal;
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
			m_World.GetBody(m_Pelvis).Position, m_Pelvis, -1000.0f);
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

		// --- steering, with momentum ----------------------------------------
		//
		// Nothing here is instant. A body with mass does not change direction
		// on a key press, and snapping the velocity or the facing is most of
		// what makes a character feel like a cursor rather than a person.
		// Camera-relative, which is what a third-person control scheme means:
		// W goes away from the camera whichever way it is pointing, not along
		// some fixed world axis. Steering in world space is the thing that
		// makes a follow camera feel like it is fighting you.
		float camYaw = glm::radians(m_CameraYaw);
		glm::vec3 camForward(std::cos(camYaw), 0.0f, std::sin(camYaw));
		glm::vec3 camRight(-std::sin(camYaw), 0.0f, std::cos(camYaw));

		if (glm::length(camForward) > 0.0f) camForward = glm::normalize(camForward);
		if (glm::length(camRight) > 0.0f) camRight = glm::normalize(camRight);

		glm::vec3 wish(0.0f);
		if (!m_FreeCamera)
		{
			if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) wish += camForward;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) wish -= camForward;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) wish -= camRight;
			if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) wish += camRight;
		}
		wish.y = 0.0f;

		float wishLength = glm::length(wish);
		if (wishLength > 0.0f)
			wish /= wishLength;

		// The heading turns towards where it is going at a limited rate, the
		// short way round. A character that pivots instantly reads as weightless
		// however good the rest of the gait is.
		if (wishLength > 0.0f)
		{
			float wanted = std::atan2(wish.x, wish.z);
			float error = wanted - m_Facing;
			while (error >  glm::pi<float>()) error -= glm::two_pi<float>();
			while (error < -glm::pi<float>()) error += glm::two_pi<float>();

			m_Facing += glm::clamp(error, -m_TurnRate * dt, m_TurnRate * dt);
		}

		// Speed eases towards the target and coasts to a stop, and the figure
		// walks along its *facing* rather than along the input -- so a turn is
		// a curve rather than a sidestep.
		bool wantsRun = !m_FreeCamera
			&& Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT);

		// Eased, because everything about the run differs from the walk --
		// stride, stance fraction, arm swing, lean -- and snapping between two
		// gaits mid-step is exactly the tearing that a blend exists to avoid.
		m_RunBlend += glm::clamp((wantsRun ? 1.0f : 0.0f) - m_RunBlend,
			-m_RunBlendRate * dt, m_RunBlendRate * dt);

		float topSpeed = glm::mix(m_WalkSpeed, m_RunSpeed, m_RunBlend);

		float wantedSpeed = wishLength > 0.0f ? topSpeed : 0.0f;
		float rate = wantedSpeed > m_Speed ? m_Acceleration : m_Braking;
		m_Speed = glm::clamp(m_Speed + glm::clamp(wantedSpeed - m_Speed, -rate * dt, rate * dt),
			0.0f, topSpeed);

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
		float half = FootReach(stride);
		float legMax = (m_ThighLength + m_ShinLength) * 0.999f;
		float needed = legMax - std::sqrt(std::max(legMax * legMax - half * half, 0.0f));

		// Zero at mid-stance, full at double support. This was the wrong way
		// round -- the comment above described the walk correctly and the code
		// did the opposite, so the body sank over the straight stance leg and
		// rose as the legs splayed. That is the shape of a waddle.
		float low = 0.5f * (1.0f - std::cos(2.0f * m_GaitPhase));
		float bob = -(needed + m_BobAmount * stride) * low;

		// Flight. A run is not a fast walk: below half the cycle spent in
		// stance there are moments with no foot down at all, and the body is
		// then a projectile. The arc is not integrated -- it is evaluated from
		// the phase, so it lands exactly when the next foot is due rather than
		// whenever gravity happens to bring it down.
		//
		// The apex is not a tuning knob either. A flight of T seconds is
		// g*T^2/8 tall, which at a run works out at a few centimetres.
		float air = Airborne();
		if (air >= 0.0f)
		{
			float cycle = CurrentStride() / std::max(m_Speed, 0.01f);
			float flightTime = (0.5f - m_Duty) * cycle;
			float apex = 9.81f * flightTime * flightTime * 0.125f;

			// A parabola through the window: zero at both ends, apex in the
			// middle.
			bob += apex * (1.0f - (2.0f * air - 1.0f) * (2.0f * air - 1.0f));
		}

		// And the landing nudge belongs at the landing, which is double
		// support -- it was firing at mid-stance, with both feet nowhere near
		// touching down.
		float impact = std::max(0.0f, -std::cos(2.0f * m_GaitPhase));
		bob -= m_HeelStrike * impact * impact * impact * stride;

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
		float groundHere = m_World.GroundHeightBelow(under, m_Pelvis, -1000.0f);

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
				m_Air = Air::Flying;
				m_JumpVelocity = std::sqrt(2.0f * 9.81f * m_JumpHeight);
			}
		}
		else if (m_Air == Air::Flying)
		{
			m_JumpVelocity -= 9.81f * dt;
			m_JumpOffset += m_JumpVelocity * dt;

			if (m_JumpOffset <= 0.0f && m_JumpVelocity < 0.0f)
			{
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
		m_StandHeight += glm::clamp(wantedStand - m_StandHeight,
			-m_ClimbRate * dt, m_ClimbRate * dt);

		// How much higher the next foothold is, as a fraction of the tallest
		// step worth lifting for, is worked out per leg in PlaceFeet -- the
		// foot's own landing spot is the honest place to ask. This is only
		// the larger of the two, for the panel to show.
		m_StepUp = std::max(m_Climb[0], m_Climb[1]);

		// The crouch before the launch, and the absorb after the landing --
		// both dips, both on the way to or from the same jump.
		float crouch = m_Air == Air::Crouching
			? m_JumpCrouchDepth * glm::clamp(m_AirTimer / m_JumpCrouchTime, 0.0f, 1.0f)
			: 0.0f;
		crouch += m_JumpCrouchDepth * (m_LandTimer / std::max(m_LandTime, 0.01f));

		// Dropping while recovering is not a flourish. The capture point sits
		// v*sqrt(h/g) ahead, so taking height out of h shortens the reach the
		// feet have to make -- it is the one thing a body can do about its own
		// balance without moving its feet at all.
		crouch += m_RecoverCrouch * glm::clamp(
			glm::length(m_StaggerVelocity) / std::max(CatchableSpeed(), 0.01f), 0.0f, 1.0f);

		glm::vec3 target = m_RootAnchor + sway;
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

		lean = glm::clamp(lean, -m_LeanBack, m_LeanForward);

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
		glm::quat roll = glm::angleAxis(-m_RollAmount * m_Carry * stride,
			glm::vec3(0.0f, 0.0f, 1.0f));

		pelvis.Orientation = glm::normalize(facing * pitch * roll);
		pelvis.UpdateInertiaWorld();

		// The remainder is carried by the spine, so the lean is a *bend* rather
		// than a plank tipping over. Measuring found the torso tracking the
		// pelvis to within 0.1 deg -- the spine motor is strong enough at 260
		// N.m to carry it rigidly -- so leaning the pelvis alone moved the
		// whole figure as one piece, which is not what a person does. The cone
		// limit here is 25 deg, so the share has to stay well inside that.
		m_World.GetJoint(m_SpineJoint).MotorTargetRotation = glm::normalize(
			glm::angleAxis(lean * (1.0f - m_LeanPelvisShare), glm::vec3(1.0f, 0.0f, 0.0f))
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

			float swing = m_ArmSwing * (1.0f + m_RunBlend) * std::sin(phase) * stride;

			// About the shoulder's pitch axis, in the torso's frame, on top of
			// the pose the rig was built in.
			glm::quat turn = glm::angleAxis(swing, glm::vec3(1.0f, 0.0f, 0.0f));
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

			glm::vec3 hip = HipWorld(side);

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
				+ forward * FootReach(stride)
				+ m_StaggerVelocity * m_StaggerStep
				+ right * (side == 0 ? -m_StanceWidth : m_StanceWidth);
			// The ankle rides above the sole, so a target on the ground is a
			// target with the foot buried in it.
			landing.y = m_World.GroundHeightBelow(landing + glm::vec3(0.0f, 1.0f, 0.0f),
				m_Pelvis) + m_AnkleHeight;

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
			float ahead = m_World.GroundHeightBelow(beyond + glm::vec3(0.0f, 1.0f, 0.0f),
				m_Pelvis) + m_AnkleHeight;

			float climb = glm::clamp(
				(std::max(landing.y, ahead) - m_Planted[side].y) / m_MaxStepUp, 0.0f, 1.0f);

			// Latched for the length of the swing. The target is recomputed
			// every frame, so without this a foot that clears the riser stops
			// seeing a step and drops its lift halfway over -- catching the
			// obstacle it had just cleared.
			m_Climb[side] = swinging ? std::max(m_Climb[side], climb) : climb;

			if (swinging && !m_Swinging[side])
			{
				// Just left the ground: remember where from, and where to.
				m_SwingFrom[side] = m_Planted[side];
				m_SwingTo[side] = landing;
			}
			else if (!swinging && m_Swinging[side])
			{
				// Just landed. Pinned here until it swings again.
				m_Planted[side] = m_SwingTo[side];
			}
			m_Swinging[side] = swinging;

			glm::vec3 target;

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

				SolveLeg(side, hip, tuck, dt);
				continue;
			}

			if (swinging)
			{
				// Progress through the swing, whatever fraction of the cycle
				// the swing happens to be at this gait.
				float t = glm::clamp((CyclePosition(side) - m_Duty)
					/ std::max(1.0f - m_Duty, 0.01f), 0.0f, 1.0f);

				// Keep the target moving with the aim point rather than the
				// one computed when the foot left the ground, or a turn mid
				// stride plants the foot where the character used to be going.
				m_SwingTo[side] = landing;

				target = glm::mix(m_SwingFrom[side], m_SwingTo[side], t);

				// Over the top. Higher when there is a step to clear, which is
				// what the look-ahead probe is for.
				float over = std::sin(t * glm::pi<float>());
				target.y += over * m_FootLift * (1.0f + m_Climb[side] * m_ClimbKnee);

			}
			else
			{
				// Planted. The body moves; this does not.
				target = m_Planted[side];
			}

			SolveLeg(side, hip, target, dt);
		}
	}

	// One cycle of travel at the current gait. A run covers twice the ground
	// per cycle that a walk does, and most of the extra is flight, not reach.
	float CurrentStride() const
	{
		return glm::mix(m_StrideLength, m_RunStride, m_RunBlend);
	}

	// How far ahead of the body a foot lands. Capped, and the cap is the point:
	// a quarter of a running stride would put the foot 0.60 m in front, which
	// needs the hip 0.23 m lower to be reachable at all -- and reaching that
	// far forward is overstriding, which is a braking force, not a run. Real
	// running gets its length from the flight phase instead.
	float FootReach(float stride) const
	{
		return std::min(CurrentStride() * 0.25f, m_MaxFootReach) * stride;
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
		const Egss::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
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
		Egss::PhysicsWorld3D::BodyHandle proximal, const glm::quat& proximalRest,
		Egss::PhysicsWorld3D::BodyHandle distal, float bendSign)
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
		const Egss::RigidBody3D& torso = m_World.GetBody(m_Torso);
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
		const Egss::RigidBody3D& shin = m_World.GetBody(m_LowerLeg[side]);

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
		if (!m_World.GroundBelow(footWorld + up, height, ground, m_Foot[side], -1000.0f))
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

// Protected rather than private because Map Building inherits the whole
// character from here. Everything below this line is the rig, its gait and its
// tuning, and there is exactly one copy of it -- see the note at the top of
// MapBuilding.h for why that demo is a subclass rather than a second rig.
protected:
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
	Handle m_ShoulderJoint[2] = {}, m_ElbowJoint[2] = {};
	Handle m_SpineJoint = {};
	glm::quat m_SpineRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::quat m_ShoulderRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

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
	float m_Acceleration = 3.0f;
	float m_Braking = 6.0f;
	float m_TurnRate = 4.0f;          // radians a second
	glm::vec3 m_RootAnchor{ 0.0f };

	// Weight.
	// Tuned against a person rather than by eye. Gait studies put the hips at
	// roughly 4-5 cm of vertical travel per stride and 2-3 cm of lateral sway;
	// the first pass at these produced 6.5 and 6.0, which reads as a swagger.
	float m_BobAmount = 0.008f;      // on top of what the stride geometrically needs
	float m_FallHeight = 1.5f;       // no ground within this and he is falling
	float m_HeelStrike = 0.012f;
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
	float m_ClimbRate = 1.2f;        // metres a second the hips may rise
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
	enum class Air { Grounded, Crouching, Flying };
	Air m_Air = Air::Grounded;
	bool m_WantsJump = false;
	float m_AirTimer = 0.0f;
	float m_JumpVelocity = 0.0f;
	float m_JumpOffset = 0.0f;        // height above the stand height
	float m_JumpHeight = 0.55f;       // apex, and it is exactly v^2/2g
	float m_JumpCrouchTime = 0.12f;
	float m_JumpCrouchDepth = 0.14f;
	float m_LandTimer = 0.0f;
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
	float m_RunBlendRate = 2.5f;
	float m_WalkDuty = 0.62f;         // fraction of its cycle a foot is down
	float m_RunDuty = 0.35f;
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

	// Third person.
	bool m_FreeCamera = false;
	float m_CameraYaw = -90.0f;      // looking down +z, the way the figure faces
	float m_CameraPitch = 12.0f;
	float m_CameraDistance = 4.0f;
	float m_CameraHeight = 0.55f;    // above the pelvis, so about chest high
	float m_CameraLag = 6.0f;        // how quickly it catches up, per second
	glm::vec3 m_CameraFocus{ 0.0f };
	float m_FrameTime = 0.0f;
};
