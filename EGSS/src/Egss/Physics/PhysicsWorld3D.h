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
	//   * **Brute-force broadphase.** Every pair is tested, which is O(n^2).
	//     2D started here too and only grew a uniform grid once a profile
	//     asked for one; building it before that would be guessing.
	//   * **No joints.**
	class EGSS_API PhysicsWorld3D
	{
	public:
		using BodyHandle = unsigned int;
		static constexpr BodyHandle InvalidHandle = ~0u;

		BodyHandle AddBody(const RigidBody3D& body);
		void Clear();

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
		void WarmStart();
		void SolveVelocities();
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
