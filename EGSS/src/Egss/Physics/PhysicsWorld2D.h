#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/RigidBody2D.h"

#include <glm/glm.hpp>

namespace Egss {

	// One point of an overlap, solved independently of the others.
	//
	// A box-box contact can have two, and the difference is the whole reason
	// rotation is worth having: a single point can only resist rotation about
	// itself, so a crate resting on one rocks forever.
	struct EGSS_API ContactPoint
	{
		// World space, on the surface the two bodies are sharing.
		glm::vec2 Position = { 0.0f, 0.0f };

		// From each body's centre of mass out to the point. The lever arm --
		// what turns a contact impulse into a spin, and the reason a crate
		// caught on one corner tips instead of being shoved flat.
		glm::vec2 LeverA = { 0.0f, 0.0f };
		glm::vec2 LeverB = { 0.0f, 0.0f };

		// How far the two have interpenetrated at this point. Clipping can
		// leave one corner deeper than the other on a box resting at a slight
		// angle, so this is per point and not per contact.
		float Penetration = 0.0f;

		// Total impulse applied along the normal and tangent so far. These are
		// carried into the next step ("warm starting"), so the solver resumes
		// from the answer it found last time instead of rediscovering it from
		// zero. It is the difference between a stack settling in 8 iterations
		// and needing 40.
		float NormalImpulse = 0.0f;
		float TangentImpulse = 0.0f;

		// The *inverse* effective mass along each axis, worked out once per
		// step in PrepareContacts rather than inside every iteration. Zero
		// means nothing here can move, and the solver skips the point.
		float NormalMass = 0.0f;
		float TangentMass = 0.0f;

		// Target separation speed, captured before solving. Restitution has to
		// be measured against the approach speed on the step the bodies first
		// touched, not against whatever the solver has left mid-iteration --
		// and against the speed *at this point*, since a turning body's two
		// contact points close at different speeds.
		float RestitutionBias = 0.0f;
	};

	// One overlap between two bodies, produced by detection and consumed by
	// the solver. Kept around after the step so it can be drawn.
	struct EGSS_API Contact
	{
		unsigned int A = 0;
		unsigned int B = 0;

		// Points from A towards B, unit length. Shared by every point of the
		// contact: the separating axis is a property of the pair, not of the
		// individual corners resting on it.
		glm::vec2 Normal = { 0.0f, 0.0f };

		// The deepest point and its penetration. The solver works per point,
		// but ResolveCircle and the debug draw both want one answer, and the
		// deepest is the one that decides whether they are apart.
		float Penetration = 0.0f;
		glm::vec2 Point = { 0.0f, 0.0f };

		int PointCount = 0;
		ContactPoint Points[2] = {};

		// How hard the two hit, over the whole contact. A face contact splits
		// one total between two points, so reading a single point's impulse
		// as "the impact" reports roughly half of it.
		float TotalNormalImpulse() const
		{
			float total = 0.0f;
			for (int i = 0; i < PointCount; i++)
				total += Points[i].NormalImpulse;

			return total;
		}
	};

	// What a ray found, if anything.
	struct EGSS_API RaycastHit
	{
		bool Hit = false;
		unsigned int Body = ~0u;

		glm::vec2 Point = { 0.0f, 0.0f };
		// Surface normal at the hit, pointing back towards the ray's origin.
		glm::vec2 Normal = { 0.0f, 0.0f };

		float Distance = 0.0f;
		// Where along the ray, 0..1 of maxDistance. Handy for attenuating by
		// how far something is without dividing again.
		float Fraction = 0.0f;
	};

	// A standalone rigid-body world, in the shape Box2D and Jolt use: it owns
	// its bodies and knows nothing about the renderer, the scene, or entities.
	// Whatever ends up owning game objects later just holds handles into this.
	//
	// Bodies rotate. Box-box overlap goes through Sat2D, contacts carry a
	// lever arm, and the solver has its angular terms -- so an off-centre hit
	// spins a crate and friction rolls a ball rather than dragging it. The
	// three pieces landed separately and each was checked on its own before
	// being joined up; the changelog entries are worth reading in that order
	// if any of it needs revisiting.
	//
	// Still worth being explicit about:
	//
	//   * **Boxes and circles only.** No general convex hulls, and no
	//     compound shapes -- one body carries exactly one collider.
	//   * **No joints.** Nothing constrains two bodies except contact.
	//   * The broadphase is a uniform grid, switchable back to brute force.
	//
	// Step expects a fixed dt. Feed it from Layer::OnFixedUpdate.
	class EGSS_API PhysicsWorld2D
	{
	public:
		// Handles are indices, so they survive the body vector reallocating.
		// A raw pointer would not.
		using BodyHandle = unsigned int;
		static constexpr BodyHandle InvalidHandle = ~0u;

		BodyHandle AddBody(const RigidBody2D& body);
		void Clear();

		RigidBody2D& GetBody(BodyHandle handle) { return m_Bodies[handle]; }
		const RigidBody2D& GetBody(BodyHandle handle) const { return m_Bodies[handle]; }

		std::vector<RigidBody2D>& GetBodies() { return m_Bodies; }
		const std::vector<RigidBody2D>& GetBodies() const { return m_Bodies; }
		size_t GetBodyCount() const { return m_Bodies.size(); }

		// Advance by exactly dt. Integrate, find contacts, solve them, then
		// push overlapping bodies apart.
		void Step(float dt);

		// Nearest body along the ray, or a miss.
		//
		// The direction need not be unit length -- it is normalised here, so
		// callers can pass a "to minus from" vector directly. Pass a handle in
		// `ignore` to skip the caster, which is nearly always what you want
		// when a body casts a ray from its own centre.
		//
		// A ray starting inside a body reports a hit at distance 0 with the
		// normal facing back down the ray; there is no single correct answer
		// in that case, and this one at least never leaves the caller with a
		// zero-length normal.
		RaycastHit Raycast(const glm::vec2& origin, const glm::vec2& direction,
			float maxDistance, unsigned int ignore = ~0u) const;

		// --- Applying things to a body -----------------------------------------
		//
		// An impulse at the centre of mass changes only where a body is going.
		// The *same* impulse off-centre also spins it, and how much is the
		// cross product of the offset with the impulse -- which in 2D is one
		// scalar, r.x*j.y - r.y*j.x.
		//
		// This is the whole of "why does hitting a box in the corner make it
		// turn". Contacts now generate the same thing on their own, through
		// the same cross product -- this is the manual version of what the
		// solver does per contact point.
		//
		// `point` is in world space. Wakes the body: an impulse applied to
		// something asleep would otherwise be integrated away to nothing.
		void ApplyImpulseAt(unsigned int body, const glm::vec2& impulse,
			const glm::vec2& point);

		// Straight to the centre of mass: no rotation, whatever the geometry.
		void ApplyImpulse(unsigned int body, const glm::vec2& impulse);

		// Accumulated and applied on the next Step, then cleared -- unlike an
		// impulse, which takes effect at once. Torque is to angular velocity
		// what force is to velocity.
		void ApplyTorque(unsigned int body, float torque);

		// Pushes a circle out of anything it overlaps and returns where it ends
		// up. Nothing needs to be a rigid body -- this is for things that move
		// themselves: characters, cameras, a light you can drive around.
		//
		// Iterated, because pushing out of one body can push into another; a
		// couple of passes settles a corner.
		glm::vec2 ResolveCircle(const glm::vec2& position, float radius,
			int iterations = 3) const;

		// Contacts from the last Step, for drawing.
		const std::vector<Contact>& GetContacts() const { return m_Contacts; }

		unsigned int GetAwakeBodyCount() const { return m_AwakeBodyCount; }

		glm::vec2 Gravity = { 0.0f, -9.81f };

		// More iterations means stacks settle better and cost more. Under
		// about 4 a tall stack visibly sags.
		unsigned int VelocityIterations = 8;

		// Passes of overlap correction, each re-measuring how deep the bodies
		// actually are. One pass is not enough for a stack -- see the note in
		// CorrectPositions.
		unsigned int PositionIterations = 6;

		// --- Broadphase ---------------------------------------------------
		// A uniform grid: bodies are bucketed by the cells their bounds
		// overlap, so a query only looks at bodies that could plausibly be
		// near. Brute force is O(n^2) for pairs and O(rays x bodies) for
		// raycasts; the grid makes both roughly proportional to how much space
		// the query actually covers.
		//
		// Left switchable so the two can be compared with the profiler rather
		// than assumed -- which is the only reason to have built it.
		bool UseBroadphase = true;

		// Cells should be around the size of a typical body. Much smaller and
		// a body spans many cells; much larger and each cell holds everything,
		// which is brute force with extra steps.
		float CellSize = 0.25f;

		unsigned int GetBroadphaseCellCount() const { return (unsigned int)m_Cells.size(); }
		unsigned int GetBroadphaseCandidates() const { return m_Candidates; }

		// Bodies slower than this for longer than SleepTime stop integrating.
		float SleepVelocity = 0.08f;
		// And turning slower than this, in radians per second. A body still
		// spinning is still moving, even if it has stopped travelling.
		float SleepAngularVelocity = 0.05f;
		float SleepTime = 0.5f;
		bool AllowSleeping = true;
	private:
		// Mutable, because Raycast is const but has to be able to build the
		// grid on first use -- a world that never steps still needs one.
		void RebuildGrid() const;
		void MarkGridDirty() { m_GridDirty = true; }

		// Two halves, with the contact solve between them. See the note in
		// Step: integrating position before the solver runs lets gravity move
		// a body a step's worth before friction can object, which reads as a
		// resting block creeping down a slope.
		void IntegrateVelocities(float dt);
		void IntegratePositions(float dt);
		void GenerateContacts();
		void TestPair(unsigned int i, unsigned int j);
		// Lever arms, effective masses and restitution targets: everything the
		// iterations need that does not change between them.
		void PrepareContacts();
		void WarmStart();
		void SolveVelocities();
		void CorrectPositions();
		void UpdateSleeping(float dt);
	private:
		std::vector<RigidBody2D> m_Bodies;
		std::vector<Contact> m_Contacts;

		// Last step's impulses for one pair, and where they were applied.
		//
		// The points are stored because warm starting matches by *position*
		// rather than by index. Clipping can hand the two corners back in
		// either order, and can drop one entirely as a crate tips -- feeding a
		// point the other point's impulse is worse than not warm starting it.
		struct PreviousContact
		{
			int PointCount = 0;
			glm::vec2 Points[2] = {};
			float NormalImpulse[2] = { 0.0f, 0.0f };
			float TangentImpulse[2] = { 0.0f, 0.0f };
		};

		// Keyed by body pair, so a contact that persists picks up where it
		// left off.
		std::unordered_map<unsigned long long, PreviousContact> m_PreviousImpulses;

		unsigned int m_AwakeBodyCount = 0;

		// --- Grid ---
		mutable std::vector<std::vector<unsigned int>> m_Cells;
		mutable glm::vec2 m_GridOrigin = { 0.0f, 0.0f };
		mutable int m_GridWidth = 0;
		mutable int m_GridHeight = 0;
		mutable float m_GridCellSize = 0.25f;
		mutable bool m_GridDirty = true;

		// Per-body stamp, so a query can skip bodies it has already considered
		// without allocating a set. Cheaper than the alternative and the
		// reason a pair spanning several cells is only tested once.
		mutable std::vector<unsigned int> m_QueryStamp;
		mutable unsigned int m_QueryCounter = 0;
		mutable unsigned int m_Candidates = 0;
	};

}
