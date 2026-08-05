#include "egsspch.h"
#include "Egss/Physics/PhysicsWorld3D.h"
#include "Egss/Debug/Instrumentor.h"

namespace Egss {

	PhysicsWorld3D::BodyHandle PhysicsWorld3D::AddBody(const RigidBody3D& body)
	{
		m_Bodies.push_back(body);

		RigidBody3D& added = m_Bodies.back();
		added.PreviousPosition = body.Position;
		added.PreviousOrientation = body.Orientation;
		// In case the caller built the body by hand and set an orientation
		// after the Make* helper ran.
		added.UpdateInertiaWorld();

		return (BodyHandle)(m_Bodies.size() - 1);
	}

	void PhysicsWorld3D::Clear()
	{
		m_Bodies.clear();
		m_AwakeBodyCount = 0;
	}

	void PhysicsWorld3D::Step(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics3D::Step");

		IntegrateVelocities(dt);

		// The contact solve goes here when there is one. Ordered this way from
		// the start deliberately -- see the note in the header.

		IntegratePositions(dt);
	}

	void PhysicsWorld3D::IntegrateVelocities(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics3D::IntegrateVelocities");

		m_AwakeBodyCount = 0;

		for (RigidBody3D& body : m_Bodies)
		{
			body.PreviousPosition = body.Position;
			body.PreviousOrientation = body.Orientation;

			if (body.Type == BodyType::Static || !body.Awake)
			{
				body.Force = glm::vec3(0.0f);
				body.Torque = glm::vec3(0.0f);
				continue;
			}

			m_AwakeBodyCount++;

			body.Velocity += (Gravity * body.GravityScale + body.Force * body.InverseMass) * dt;

			// Torque into angular velocity through the *world* tensor. Using
			// the body-space one would apply the torque as though the body
			// were facing down its own axes, which it generally is not.
			body.AngularVelocity += body.InverseInertiaWorld * body.Torque * dt;

			// Exponential damping, so it cannot push a velocity through zero
			// and reverse it at a large step -- the same reasoning as 2D.
			if (body.LinearDamping > 0.0f)
				body.Velocity *= 1.0f / (1.0f + body.LinearDamping * dt);

			if (body.AngularDamping > 0.0f)
				body.AngularVelocity *= 1.0f / (1.0f + body.AngularDamping * dt);

			body.Force = glm::vec3(0.0f);
			body.Torque = glm::vec3(0.0f);
		}
	}

	void PhysicsWorld3D::IntegratePositions(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics3D::IntegratePositions");

		for (RigidBody3D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static || !body.Awake)
				continue;

			body.Position += body.Velocity * dt;

			// --- Orientation -------------------------------------------------
			//
			// Angular momentum is captured *before* the body turns and used to
			// recover angular velocity *after*, rather than carrying angular
			// velocity across the rotation unchanged.
			//
			// That is the whole of the difference from 2D, and it is not a
			// refinement. L is what is conserved when nothing pushes; angular
			// velocity is not, because the body's resistance to turning
			// changes as it turns. Carry w across instead and a tumbling box
			// conserves the wrong quantity: the intermediate-axis flip -- what
			// a thrown book or a spinning phone actually does -- never happens.
			//
			// The exponential map: the actual rotation of |w|*h about w, rather
			// than the linearisation q += (h/2) w q, which steps along the
			// tangent and off the unit sphere. World-space w pre-multiplies;
			// body-space would post-multiply, and the two differ by exactly
			// the rotation being integrated.
			auto turnBy = [](const glm::quat& orientation, const glm::vec3& angular, float h)
			{
				float speed = glm::length(angular);
				if (speed <= 1e-8f)
					return orientation;

				return glm::normalize(glm::angleAxis(speed * h, angular / speed) * orientation);
			};

			// L does not change while the body merely turns, so it is the thing
			// to carry across the step. w is recovered from it afterwards, and
			// *that* is where tumbling comes from: the tensor has moved, so
			// the same momentum implies a different angular velocity.
			glm::vec3 momentum = body.GetAngularMomentum();

			// Midpoint. Turning the whole step with w as it is at the start is
			// first order, and a tumbling body's w changes fast enough for
			// that to matter -- it invented 18.4% of the rotational energy
			// over ten seconds. Taking w from the half-step instead is second
			// order and brings that to 0.02%, for one extra tensor rebuild.
			glm::quat halfway = turnBy(body.Orientation, body.AngularVelocity, dt * 0.5f);

			glm::mat3 halfRotation = glm::mat3_cast(halfway);
			glm::vec3 midpointAngular =
				(halfRotation * body.InverseInertiaLocal * glm::transpose(halfRotation)) * momentum;

			body.Orientation = turnBy(body.Orientation, midpointAngular, dt);
			body.UpdateInertiaWorld();
			body.AngularVelocity = body.InverseInertiaWorld * momentum;
		}
	}

	void PhysicsWorld3D::ApplyImpulse(BodyHandle handle, const glm::vec3& impulse)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody3D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Velocity += impulse * body.InverseMass;
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	void PhysicsWorld3D::ApplyImpulseAt(BodyHandle handle, const glm::vec3& impulse,
		const glm::vec3& point)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody3D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Velocity += impulse * body.InverseMass;

		// r x j, which in 3D stays a vector -- it is the axis the body starts
		// turning about. An impulse aimed through the centre gives r parallel
		// to j and a zero cross, so a central hit produces no spin without
		// that being special-cased, exactly as in 2D.
		glm::vec3 lever = point - body.Position;
		body.AngularVelocity += body.InverseInertiaWorld * glm::cross(lever, impulse);

		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	void PhysicsWorld3D::ApplyTorque(BodyHandle handle, const glm::vec3& torque)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody3D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Torque += torque;
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	void PhysicsWorld3D::ApplyForce(BodyHandle handle, const glm::vec3& force)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody3D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Force += force;
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	glm::vec3 PhysicsWorld3D::GetTotalAngularMomentum() const
	{
		glm::vec3 total(0.0f);

		for (const RigidBody3D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static || body.InverseMass <= 0.0f)
				continue;

			// Spin about the centre of mass, plus the orbital part the centre
			// of mass carries about the origin. Leaving the second term out
			// makes the total look unconserved the moment anything moves.
			total += body.GetAngularMomentum();
			total += body.GetMass() * glm::cross(body.Position, body.Velocity);
		}

		return total;
	}

	glm::vec3 PhysicsWorld3D::GetTotalLinearMomentum() const
	{
		glm::vec3 total(0.0f);

		for (const RigidBody3D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static || body.InverseMass <= 0.0f)
				continue;

			total += body.GetMass() * body.Velocity;
		}

		return total;
	}

	float PhysicsWorld3D::GetTotalKineticEnergy() const
	{
		float total = 0.0f;

		for (const RigidBody3D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static || body.InverseMass <= 0.0f)
				continue;

			total += 0.5f * body.GetMass() * glm::dot(body.Velocity, body.Velocity);
			total += body.GetRotationalEnergy();
		}

		return total;
	}

}
