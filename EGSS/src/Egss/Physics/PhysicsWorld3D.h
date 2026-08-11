#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/RigidBody3D.h"
#include "Egss/Physics/Sat3D.h"

#include <glm/glm.hpp>

namespace Egss {

	// One point of an overlap, solved independently of the others.
	//
	// The 2D version of this had a scalar lever arm and one tangent. Here the
	// lever stays a vector, and friction needs *two* tangents: a contact plane
	// in 3D is a plane, and a body can slide across it in any direction.
	struct EGSS_API ContactPoint3D
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };

		// From each body's centre of mass out to the point.
		glm::vec3 LeverA = { 0.0f, 0.0f, 0.0f };
		glm::vec3 LeverB = { 0.0f, 0.0f, 0.0f };

		float Penetration = 0.0f;

		float NormalImpulse = 0.0f;
		float TangentImpulse[2] = { 0.0f, 0.0f };

		// Inverse effective masses along the normal and the two tangents,
		// worked out once per step rather than inside every iteration.
		float NormalMass = 0.0f;
		float TangentMass[2] = { 0.0f, 0.0f };

		float RestitutionBias = 0.0f;
	};

	struct EGSS_API Contact3D
	{
		unsigned int A = 0;
		unsigned int B = 0;

		// Points from A towards B, unit length.
		glm::vec3 Normal = { 0.0f, 0.0f, 0.0f };

		// The two directions spanning the contact plane. Built from the normal
		// rather than from relative velocity, so they do not spin between
		// steps and invalidate the friction being carried over.
		glm::vec3 Tangent[2] = {};

		// The deepest point and its penetration, for callers wanting one
		// answer rather than the manifold.
		float Penetration = 0.0f;
		glm::vec3 Point = { 0.0f, 0.0f, 0.0f };

		int PointCount = 0;
		ContactPoint3D Points[8] = {};

		float TotalNormalImpulse() const
		{
			float total = 0.0f;
			for (int i = 0; i < PointCount; i++)
				total += Points[i].NormalImpulse;

			return total;
		}
	};

	// A ball-and-socket joint: two bodies pinned at a shared point, free to
	// turn about it however they like.
	//
	// This is the first *bilateral* constraint in the engine, and the word is
	// what makes it different from a contact. A contact may only push -- its
	// impulse is clamped at zero, because a floor cannot pull a box down. A
	// joint may push and pull without limit, because a shoulder holds an arm
	// on from both directions. That single difference is most of why the solve
	// below looks like the contact solve with pieces missing: no clamping, no
	// restitution, no friction.
	//
	// The other difference is that it takes a 3x3 effective mass rather than a
	// scalar. A contact constrains one direction, so its effective mass is one
	// number. A ball joint constrains all three at once, and they are coupled
	// through the inertia tensor -- solving each axis separately converges
	// slowly and visibly stretches under load, which for a ragdoll reads as
	// limbs pulling out of their sockets.
	enum class JointType3D
	{
		// Pinned at a point, free to turn any way about it. Shoulders, hips.
		Ball = 0,
		// Pinned at a point *and* about an axis, leaving one degree of freedom.
		// Knees, elbows, doors.
		Hinge
	};

	struct EGSS_API Joint3D
	{
		JointType3D Type = JointType3D::Ball;

		unsigned int A = 0;
		unsigned int B = 0;

		// The shared point, written in each body's own frame. Stored per body
		// rather than once in world space because that is what makes it a
		// constraint: the two anchors are the same point now, and the solver's
		// job is to keep them the same point.
		glm::vec3 LocalAnchorA = { 0.0f, 0.0f, 0.0f };
		glm::vec3 LocalAnchorB = { 0.0f, 0.0f, 0.0f };

		// Whether the two bodies still collide with each other. Off by
		// default: jointed bodies almost always overlap at the joint -- an
		// upper and lower arm share the elbow -- and letting the narrowphase
		// see that pair means the contact and the joint fight for ever.
		bool CollideConnected = false;

		// Carried between steps, exactly as contact impulses are. Warm
		// starting matters more here than it does for contacts: a chain
		// solved from cold sags visibly on the first frame and never quite
		// catches up.
		glm::vec3 AccumulatedImpulse = { 0.0f, 0.0f, 0.0f };

		// Rebuilt each step in PrepareJoints.
		glm::vec3 LeverA = { 0.0f, 0.0f, 0.0f };
		glm::vec3 LeverB = { 0.0f, 0.0f, 0.0f };
		glm::mat3 EffectiveMass = glm::mat3(1.0f);
		glm::vec3 Bias = { 0.0f, 0.0f, 0.0f };

		// How far apart the two anchors actually are, in world units. Zero is
		// the constraint being met; it is the honest measure of whether the
		// solver is keeping up, and the only reason it is stored.
		float Separation = 0.0f;

		// --- Hinge ---------------------------------------------------------
		// The axis the two bodies may still turn about, in each body's frame.
		// Keeping the angular constraint separate from the point constraint is
		// what makes a hinge a ball joint plus two locked rotations rather than
		// a different constraint written from scratch.
		glm::vec3 LocalAxisA = { 0.0f, 0.0f, 1.0f };
		glm::vec3 LocalAxisB = { 0.0f, 0.0f, 1.0f };

		// A direction perpendicular to the axis in each frame, used only to
		// measure the hinge angle. Chosen at construction so the angle reads
		// zero wherever the bodies happen to be, which makes limits relative
		// to the pose the joint was built in.
		glm::vec3 LocalRefA = { 1.0f, 0.0f, 0.0f };
		glm::vec3 LocalRefB = { 1.0f, 0.0f, 0.0f };

		// Radians, measured about the axis from A to B. A knee wants roughly
		// (-2.4, 0): it bends one way and stops straight.
		bool LimitEnabled = false;
		float LowerLimit = 0.0f;
		float UpperLimit = 0.0f;

		// The current angle, in radians. Readable, and the thing a limit test
		// or a motor would look at.
		float Angle = 0.0f;

		// Rebuilt each step.
		glm::vec3 WorldAxis = { 0.0f, 0.0f, 1.0f };
		glm::vec3 AxisTangent[2] = {};
		glm::mat2 AngularMass = glm::mat2(1.0f);
		glm::vec2 AngularBias = { 0.0f, 0.0f };
		glm::vec2 AccumulatedAngularImpulse = { 0.0f, 0.0f };

		// -1 pressed against the lower limit, +1 against the upper, 0 free.
		int LimitState = 0;
		float LimitMass = 0.0f;
		float LimitBias = 0.0f;
		float AccumulatedLimitImpulse = 0.0f;

		// How far the hinge axes have drifted out of alignment, in radians.
		// The angular counterpart of Separation.
		float AxisError = 0.0f;

		// --- Cone and twist, for ball joints -------------------------------
		// A shoulder is a ball joint, and an unlimited one lets the arm rotate
		// through the torso. Two separate limits describe what a shoulder
		// actually does:
		//
		//   * **Swing** -- how far the bone may lean away from its rest
		//     direction, bounded by a cone of half-angle ConeAngle.
		//   * **Twist** -- how far it may rotate *about* its own length,
		//     bounded by TwistLower and TwistUpper.
		//
		// They are separated by a swing-twist decomposition of the relative
		// rotation rather than measured independently, because the naive
		// measurements interfere: twist read off a reference vector changes
		// when the bone swings, even if nothing twisted.
		bool ConeTwistEnabled = false;
		float ConeAngle = 0.0f;        // radians, half-angle of the cone
		float TwistLower = 0.0f;
		float TwistUpper = 0.0f;

		// B's orientation relative to A at the moment the limits were set.
		// Both angles are measured as deviations from *this*, not from the
		// bodies being aligned -- otherwise a limb built at an angle to its
		// parent, which is every limb on a real skeleton, starts life outside
		// its own cone and gets shoved to the boundary.
		glm::quat ConeTwistRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		// Readable results of the decomposition.
		float SwingAngle = 0.0f;
		float TwistAngle = 0.0f;

		// Rebuilt each step. The axes are the directions positive relative
		// spin about which *increases* the corresponding angle.
		int SwingState = 0;            // 1 when outside the cone
		glm::vec3 SwingAxis = { 0.0f, 0.0f, 1.0f };
		float SwingMass = 0.0f;
		float SwingBias = 0.0f;
		float AccumulatedSwingImpulse = 0.0f;

		int TwistState = 0;            // -1 below TwistLower, +1 above TwistUpper
		glm::vec3 TwistAxis = { 0.0f, 1.0f, 0.0f };
		float TwistMass = 0.0f;
		float TwistBias = 0.0f;
		float AccumulatedTwistImpulse = 0.0f;

		// --- Motor ---------------------------------------------------------
		// A limit says where a joint may not go. A motor says where it should
		// be, and is the difference between a ragdoll and a body: a corpse
		// falls, a person holds their arm up.
		//
		// Written as a velocity constraint with a target rather than as a
		// torque, which is what makes it a *PD* controller without a separate
		// derivative term. The proportional half is the target spin, set from
		// how far the joint is from its goal; the derivative half falls out of
		// constraining the spin to that value, since any excess is removed.
		// MaxTorque is what keeps it a muscle rather than a weld -- push hard
		// enough and the joint gives, which is the whole point for something
		// meant to stumble.
		bool MotorEnabled = false;

		// How hard it converges, in inverse seconds: the target spin is this
		// times the remaining angle error. Roughly 10 is a firm limb, 2 is a
		// lazy one.
		float MotorStiffness = 8.0f;

		// The most it may exert, in newton-metres. Set it low and the limb is
		// weak, which is what makes a shove push a character around instead of
		// bouncing off them.
		float MotorMaxTorque = 20.0f;

		// The largest spin the motor will ask for, so a big angle error does
		// not turn into a limb being flung at the target.
		float MotorMaxSpeed = 10.0f;

		// Where it is driving to. For a hinge, an angle about the hinge axis.
		// For a ball joint, B's orientation relative to A.
		float MotorTargetAngle = 0.0f;
		glm::quat MotorTargetRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		// Rebuilt each step.
		float AccumulatedMotorImpulse = 0.0f;         // hinge, about the axis
		glm::vec3 AccumulatedMotorImpulse3 = { 0.0f, 0.0f, 0.0f };   // ball
		glm::mat3 MotorMass3 = glm::mat3(0.0f);
		glm::vec3 MotorTargetSpin = { 0.0f, 0.0f, 0.0f };

		// MaxTorque converted to an impulse budget for this step. Worked out
		// in PrepareJoints because the solve does not know dt.
		float MotorMaxImpulse = 0.0f;
	};

	// A rigid-body world in three dimensions.
	//
	// Built in three pieces, each verified before the next: the angular state
	// and its integration, then `Sat3D`, then this join. That ordering is
	// deliberate and worth keeping to -- the 2D version's join hid a bug in
	// each half until they were separated.
	//
	// Deliberately limited, and worth being explicit about:
	//
	//   * **Boxes and spheres only**, one collider per body.
	//   * **Ball-and-socket joints only.** No hinges, no angular limits, no
	//     motors -- a ragdoll built from these alone has elbows that bend
	//     every way and will look like a bag of sticks. Limits are the next
	//     piece, and the reason this one was built first is that everything
	//     else is this plus angular terms.
	class EGSS_API PhysicsWorld3D
	{
	public:
		using BodyHandle = unsigned int;
		static constexpr BodyHandle InvalidHandle = ~0u;

		BodyHandle AddBody(const RigidBody3D& body);
		void Clear();

		using JointHandle = unsigned int;

		// Pins the two bodies at `worldAnchor`, which must be where they are
		// meant to be joined *right now* -- it is converted into each body's
		// frame on the way in, so wherever they are at this moment becomes the
		// rest state.
		JointHandle AddBallJoint(BodyHandle a, BodyHandle b, const glm::vec3& worldAnchor);

		// `worldAxis` need not be unit length. As with the ball joint, the
		// bodies' current placement becomes the rest state -- and the hinge
		// angle reads zero there, so limits are relative to the pose you built
		// it in.
		JointHandle AddHingeJoint(BodyHandle a, BodyHandle b,
			const glm::vec3& worldAnchor, const glm::vec3& worldAxis);

		// Radians, about the hinge axis. lower <= 0 <= upper for a joint built
		// in its neutral pose; a knee is roughly (-2.4, 0).
		void SetHingeLimits(JointHandle handle, float lower, float upper);

		// Cone-and-twist limits for a ball joint.
		//
		// `worldTwistAxis` is the bone's direction right now -- the axis the
		// cone opens around and twist is measured about. `coneAngle` is the
		// cone's half-angle, so a shoulder allowing 60 degrees of lean in any
		// direction takes radians(60). The twist range is measured from the
		// current pose, so both limits are relative to however the bodies sit
		// when this is called.
		void SetConeTwistLimits(JointHandle handle, const glm::vec3& worldTwistAxis,
			float coneAngle, float twistLower, float twistUpper);

		// Turns on the motor and sets its strength. For a hinge the target is
		// an angle about the hinge axis; for a ball joint it is B's orientation
		// relative to A, and defaults to **wherever the bodies are now** -- so
		// calling this on an assembled ragdoll means "hold this pose".
		//
		// Drive it by writing to the target each fixed step:
		// `world.GetJoint(h).MotorTargetAngle = ...`.
		void SetJointMotor(JointHandle handle, float stiffness, float maxTorque);

		const std::vector<Joint3D>& GetJoints() const { return m_Joints; }
		size_t GetJointCount() const { return m_Joints.size(); }
		Joint3D& GetJoint(JointHandle handle) { return m_Joints[handle]; }
		const Joint3D& GetJoint(JointHandle handle) const { return m_Joints[handle]; }

		// The highest surface directly below `point`, ignoring one body -- the
		// character asking the question, normally. Returns `floor` if nothing
		// is under it.
		//
		// Tested against each body's **world axis-aligned bounds**, not its
		// actual shape. For the level geometry this exists to stand on -- boxes
		// that are not rotated -- the bounds *are* the shape and the answer is
		// exact. For a rotated box or a sphere it reports the bounding volume
		// and so reads slightly high. That is the right trade for a ground
		// probe, where being a centimetre high is invisible and being late is
		// a character sinking into a step.
		float GroundHeightBelow(const glm::vec3& point,
			BodyHandle ignore = InvalidHandle, float floor = 0.0f) const;

		// The worst anchor separation across every joint, in world units. The
		// number to watch: sequential impulses do not solve a chain exactly,
		// and this says by how much they are missing.
		float GetWorstJointSeparation() const;

		RigidBody3D& GetBody(BodyHandle handle) { return m_Bodies[handle]; }
		const RigidBody3D& GetBody(BodyHandle handle) const { return m_Bodies[handle]; }

		std::vector<RigidBody3D>& GetBodies() { return m_Bodies; }
		const std::vector<RigidBody3D>& GetBodies() const { return m_Bodies; }
		size_t GetBodyCount() const { return m_Bodies.size(); }

		// Advance by exactly dt. Feed it from Layer::OnFixedUpdate.
		//
		// Velocities are integrated, contacts are solved, and only then are
		// positions moved. The 2D world integrated position before solving for
		// its whole life, and the drift that caused only became visible once
		// there were slopes to see it on -- so this was ordered correctly from
		// the start rather than inherited.
		void Step(float dt);

		// An impulse through the centre of mass changes only where the body is
		// going.
		void ApplyImpulse(BodyHandle handle, const glm::vec3& impulse);

		// The same impulse off-centre also spins it. In 2D the cross product
		// collapsed to a scalar; here it stays a vector, and it has to be run
		// through the *world-space* inverse tensor -- an impulse on the corner
		// of a long box produces very different spin depending on which way
		// the box is facing, and only the world tensor knows that.
		void ApplyImpulseAt(BodyHandle handle, const glm::vec3& impulse, const glm::vec3& point);

		// Accumulated and applied on the next Step, then cleared.
		void ApplyTorque(BodyHandle handle, const glm::vec3& torque);
		void ApplyForce(BodyHandle handle, const glm::vec3& force);

		// Summed over every dynamic body, about the world origin. Conserved
		// when nothing external pushes, which makes it the strongest check
		// available on the angular integration.
		glm::vec3 GetTotalAngularMomentum() const;
		glm::vec3 GetTotalLinearMomentum() const;
		float GetTotalKineticEnergy() const;

		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };

		// There is deliberately no substep count here.
		//
		// One was added, because stepping the orientation with angular
		// velocity taken at the start of the step leaked 18.4% of the
		// rotational energy into existence over ten seconds of free tumbling,
		// and substepping bought that back roughly proportionally -- 4.2% at
		// four, 1.31% at sixteen. Taking the velocity from the *midpoint*
		// instead fixed it outright: 0.02% at a single step, for one extra
		// tensor rebuild rather than four times the work.
		//
		// The knob then turned out to be actively harmful -- 0.08% at four
		// substeps, 0.31% at sixteen -- because each substep renormalises a
		// quaternion and the rounding is now the largest error left. It was
		// removed rather than defaulted to one.

		// Contacts from the last Step, for drawing and for asking how hard
		// something hit.
		const std::vector<Contact3D>& GetContacts() const { return m_Contacts; }

		// More iterations means stacks settle better and cost more.
		unsigned int VelocityIterations = 8;
		unsigned int PositionIterations = 4;

		// --- Broadphase ---------------------------------------------------
		// A uniform grid, the same shape as the 2D one: bodies are bucketed by
		// the cells their world bounds overlap, and only bodies sharing a cell
		// are handed to the narrowphase. Brute force is O(n^2) pairs, which is
		// what this world did until a profile asked otherwise.
		//
		// Left switchable, because a broadphase that cannot be turned off
		// cannot be shown to be an improvement -- or shown to agree with the
		// brute-force answer, which matters more.
		bool UseBroadphase = true;

		// Below this many bodies the grid *loses*, which was measured rather
		// than assumed. Speedup over brute force, three runs each:
		//
		//     13 bodies   0.30x    203 bodies   1.31 - 1.43x
		//     43          0.65     253          1.48 - 1.56x
		//     83          0.76 - 0.83      503  2.80 - 3.49x
		//    123          0.99 - 1.01     1003  8.11 - 8.72x
		//    153          0.84 - 1.10
		//
		// Rebuilding the grid and sorting the candidate lists costs more than
		// the pairs it rejects until there are enough pairs to matter. Break
		// even is around 123; 153 straddles 1.0x depending on the run, and 203
		// is the first count where the win is unambiguous in every run. Set
		// here rather than at break-even so the switch only happens where it
		// is really a gain.
		//
		// Safe to switch on automatically precisely because the two paths are
		// bit-identical -- see the note on sorting in GenerateContacts. If they
		// disagreed, the simulation would change the moment a body count
		// crossed this, which is far worse than being slow.
		//
		// Set to 0 to force the grid on regardless, which is what an A/B
		// measurement wants.
		unsigned int BroadphaseMinBodies = 200;

		// Cells want to be about the size of a typical body. Much smaller and
		// one body spans many cells; much larger and every cell holds
		// everything, which is brute force paying rent for a grid.
		//
		// Larger than the 2D default because a 3D world is physically bigger
		// and the cell count grows with the cube of the span, not the square.
		float CellSize = 1.0f;

		unsigned int GetBroadphaseCellCount() const { return (unsigned int)m_Cells.size(); }
		// Pairs the narrowphase was actually asked about this step. The number
		// to watch: against n(n-1)/2 it says what the grid saved.
		unsigned int GetBroadphaseCandidates() const { return m_Candidates; }

		// Bodies slower than this for longer than SleepTime stop integrating.
		float SleepVelocity = 0.08f;
		float SleepAngularVelocity = 0.05f;
		float SleepTime = 0.5f;
		bool AllowSleeping = true;

		unsigned int GetAwakeBodyCount() const { return m_AwakeBodyCount; }
	private:
		void IntegrateVelocities(float dt);
		void IntegratePositions(float dt);
		void GenerateContacts();
		void TestPair(unsigned int i, unsigned int j);
		void PrepareContacts();
		void PrepareJoints(float dt);
		void WarmStart();
		void SolveVelocities();
		void SolveJoints();
		// True when a joint holds these two and has asked them not to collide.
		bool JointSuppressesContact(unsigned int a, unsigned int b) const;
		void CorrectPositions();
		void UpdateSleeping(float dt);

		void RebuildGrid();
		void MarkGridDirty() { m_GridDirty = true; }

		// Both the build and the query need to turn world bounds into a cell
		// range, and they must agree exactly: a body bucketed into cells the
		// query does not look at is a body the narrowphase never sees. Shared
		// rather than written twice for that reason.
		size_t CellIndex(int x, int y, int z) const
		{
			return ((size_t)z * m_GridHeight + y) * m_GridWidth + x;
		}

		void CellRange(const glm::vec3& boundsMin, const glm::vec3& boundsMax,
			int& x0, int& y0, int& z0, int& x1, int& y1, int& z1) const
		{
			glm::vec3 low = (boundsMin - m_GridOrigin) / m_GridCellSize;
			glm::vec3 high = (boundsMax - m_GridOrigin) / m_GridCellSize;

			x0 = std::max(0, (int)low.x);
			y0 = std::max(0, (int)low.y);
			z0 = std::max(0, (int)low.z);
			x1 = std::min(m_GridWidth - 1, (int)high.x);
			y1 = std::min(m_GridHeight - 1, (int)high.y);
			z1 = std::min(m_GridDepth - 1, (int)high.z);
		}
	private:
		std::vector<RigidBody3D> m_Bodies;
		std::vector<Contact3D> m_Contacts;
		std::vector<Joint3D> m_Joints;

		// --- Grid ---
		// Body indices per cell, in x-major order: (z * H + y) * W + x.
		std::vector<std::vector<unsigned int>> m_Cells;
		glm::vec3 m_GridOrigin = { 0.0f, 0.0f, 0.0f };
		int m_GridWidth = 0;
		int m_GridHeight = 0;
		int m_GridDepth = 0;
		float m_GridCellSize = 1.0f;
		bool m_GridDirty = true;

		// A body large relative to a cell sits in several, so the same pair can
		// be reached more than once per query. The stamp is what makes a
		// candidate list a *set* -- without it the narrowphase runs repeatedly
		// on one pair and the contact is pushed more than once, which the
		// solver then treats as several separate contacts.
		std::vector<unsigned int> m_QueryStamp;
		unsigned int m_QueryCounter = 0;
		unsigned int m_Candidates = 0;

		// One body's candidate partners, gathered then sorted. A member rather
		// than a local so the allocation is reused across steps.
		std::vector<unsigned int> m_Neighbours;

		// Last step's impulses for one pair, with the points they were applied
		// at. Matched by position rather than index, because clipping has no
		// notion of which corner is which and can return them in any order.
		struct PreviousContact
		{
			int PointCount = 0;
			glm::vec3 Points[8] = {};
			float NormalImpulse[8] = {};
			float TangentImpulse[8][2] = {};
		};

		std::unordered_map<unsigned long long, PreviousContact> m_PreviousImpulses;

		unsigned int m_AwakeBodyCount = 0;
	};

}
