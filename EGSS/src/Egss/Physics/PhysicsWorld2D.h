#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/RigidBody2D.h"

#include <glm/glm.hpp>

namespace Egss {

	// One overlap between two bodies, produced by detection and consumed by
	// the solver. Kept around after the step so it can be drawn.
	struct EGSS_API Contact
	{
		unsigned int A = 0;
		unsigned int B = 0;

		// Points from A towards B, unit length.
		glm::vec2 Normal = { 0.0f, 0.0f };
		// How far the two have interpenetrated along the normal.
		float Penetration = 0.0f;
		// Representative point, for debug drawing only -- the solver doesn't
		// use it, because without rotation there is no torque to apply.
		glm::vec2 Point = { 0.0f, 0.0f };

		// Total impulse applied along the normal and tangent so far. These are
		// carried into the next step ("warm starting"), so the solver resumes
		// from the answer it found last time instead of rediscovering it from
		// zero. It is the difference between a stack settling in 8 iterations
		// and needing 40.
		float NormalImpulse = 0.0f;
		float TangentImpulse = 0.0f;

		// Target separation speed, captured before solving. Restitution has to
		// be measured against the approach speed on the step the bodies first
		// touched, not against whatever the solver has left mid-iteration.
		float RestitutionBias = 0.0f;
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
	// Deliberately limited, and worth being explicit about:
	//
	//   * **No rotation.** Bodies translate but never spin. Adding it means
	//     SAT for oriented shapes, multi-point manifolds and angular impulses
	//     -- a much larger job, and not needed for platformers or Breakout.
	//   * **Circles and axis-aligned boxes only.**
	//   * **Brute-force broadphase.** Every pair is tested, which is O(n^2) and
	//     genuinely fine into the hundreds. Replace it when a profile says to.
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
		float SleepTime = 0.5f;
		bool AllowSleeping = true;
	private:
		// Mutable, because Raycast is const but has to be able to build the
		// grid on first use -- a world that never steps still needs one.
		void RebuildGrid() const;
		void MarkGridDirty() { m_GridDirty = true; }

		void Integrate(float dt);
		void GenerateContacts();
		void TestPair(unsigned int i, unsigned int j);
		void WarmStart();
		void SolveVelocities();
		void CorrectPositions();
		void UpdateSleeping(float dt);
	private:
		std::vector<RigidBody2D> m_Bodies;
		std::vector<Contact> m_Contacts;

		// Last step's impulses, keyed by body pair, so a contact that persists
		// can pick up where it left off.
		std::unordered_map<unsigned long long, glm::vec2> m_PreviousImpulses;

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
