#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/RigidBody3D.h"

#include <glm/glm.hpp>

namespace Egss {

	// A rigid-body world in three dimensions.
	//
	// **Nothing collides yet.** Bodies fall, spin and tumble; they pass
	// through each other and through the floor. That is deliberate and it is
	// the same order 2D was built in: the angular state and its integration
	// are separable from the contact maths, and shipping them apart means each
	// can be checked on its own against arithmetic. The 2D version was built
	// the other way round once and the join hid a bug in each half.
	//
	// What is worth checking *here*, before any collision exists, is the thing
	// 2D never had to get right: a body's resistance to rotation depends on
	// which way it is facing, so it changes as the body turns.
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
		// Split into velocity and position halves, with nothing between them
		// for now -- that gap is where the contact solve will go. Ordering it
		// this way from the start is deliberate: the 2D world integrated
		// position before solving for its whole life, and the resulting drift
		// only became visible once there were slopes to see it on.
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

		unsigned int GetAwakeBodyCount() const { return m_AwakeBodyCount; }
	private:
		void IntegrateVelocities(float dt);
		void IntegratePositions(float dt);
	private:
		std::vector<RigidBody3D> m_Bodies;
		unsigned int m_AwakeBodyCount = 0;
	};

}
