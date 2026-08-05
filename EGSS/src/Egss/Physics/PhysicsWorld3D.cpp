#include "egsspch.h"
#include "Egss/Physics/PhysicsWorld3D.h"
#include "Egss/Debug/Instrumentor.h"

namespace Egss {

	// The same figures the 2D solver settled on, and for the same reasons --
	// see the notes there. They are repeated rather than shared because the
	// two worlds are independent and a change that suits one need not suit
	// the other.
	static const float s_PenetrationSlop = 0.005f;
	static const float s_CorrectionPercent = 0.8f;
	static const float s_RestitutionThreshold = 1.0f;
	static const float s_MaxCorrection = 0.2f;

	// How far a contact point may move between steps and still count as the
	// same one for warm starting.
	static const float s_WarmStartRadius = 0.02f;

	static unsigned long long ContactKey(unsigned int a, unsigned int b)
	{
		return ((unsigned long long)a << 32) | (unsigned long long)b;
	}

	// A sleeping body is immovable for as long as it stays asleep. Without
	// this the solver keeps pushing it: it never integrates, so it does not
	// visibly move, but it banks velocity and lurches when it wakes.
	static float SolverInverseMass(const RigidBody3D& body)
	{
		return (body.Type == BodyType::Static || !body.Awake) ? 0.0f : body.InverseMass;
	}

	static glm::mat3 SolverInverseInertia(const RigidBody3D& body)
	{
		return (body.Type == BodyType::Static || !body.Awake)
			? glm::mat3(0.0f) : body.InverseInertiaWorld;
	}

	// Velocity of the material point at `lever`: the body's own velocity plus
	// what its spin adds out there.
	static glm::vec3 PointVelocity(const RigidBody3D& body, const glm::vec3& lever)
	{
		return body.Velocity + glm::cross(body.AngularVelocity, lever);
	}

	// Two unit vectors spanning the plane perpendicular to `normal`.
	//
	// Built by crossing the normal with whichever world axis it is least
	// aligned with. Picking a fixed axis instead breaks whenever the normal
	// happens to be parallel to it -- the cross product vanishes and friction
	// silently acts along a zero vector, which looks like a surface that is
	// slippery from one direction only.
	static void BuildTangents(const glm::vec3& normal, glm::vec3& first, glm::vec3& second)
	{
		glm::vec3 reference = std::fabs(normal.x) < 0.57735f
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);

		first = glm::normalize(glm::cross(normal, reference));
		second = glm::cross(normal, first);
	}

	// One impulse applied to both bodies at a contact point: equal and
	// opposite linearly, and turning each about its own centre by its own
	// lever. Every impulse goes through here, so there is one place where the
	// sign convention (normal points A -> B) has to be right.
	static void ApplyContactImpulse(RigidBody3D& a, RigidBody3D& b,
		const ContactPoint3D& point, const glm::vec3& impulse)
	{
		a.Velocity -= impulse * SolverInverseMass(a);
		a.AngularVelocity -= SolverInverseInertia(a) * glm::cross(point.LeverA, impulse);

		b.Velocity += impulse * SolverInverseMass(b);
		b.AngularVelocity += SolverInverseInertia(b) * glm::cross(point.LeverB, impulse);
	}

	// The inverse effective mass along `direction` at one contact point.
	//
	// The 2D form was a scalar `(r x n)^2 / I`. Here the cross product stays a
	// vector, the inertia is a tensor, and the term becomes
	// `n . ((I^-1 (r x n)) x r)` -- which is the same quantity written in a
	// space where "which way the body is facing" matters.
	static float EffectiveMass(const glm::vec3& direction,
		const glm::vec3& leverA, const glm::vec3& leverB,
		float inverseMassA, float inverseMassB,
		const glm::mat3& inverseInertiaA, const glm::mat3& inverseInertiaB)
	{
		glm::vec3 angularA = glm::cross(inverseInertiaA * glm::cross(leverA, direction), leverA);
		glm::vec3 angularB = glm::cross(inverseInertiaB * glm::cross(leverB, direction), leverB);

		float total = inverseMassA + inverseMassB + glm::dot(direction, angularA + angularB);
		return total > 0.0f ? 1.0f / total : 0.0f;
	}

	// A body's collider as the geometry Sat3D speaks.
	static Obb3D ObbOf(const RigidBody3D& body)
	{
		Obb3D box;
		box.Centre = body.Position;
		box.HalfExtents = body.HalfExtents;
		box.Orientation = body.Orientation;
		return box;
	}

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

	// --- Narrowphase --------------------------------------------------------
	// Each returns true and fills the contact, or false. The normal always
	// points from A towards B, so pushing B along it separates them.

	static bool CollideSphereSphere(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		glm::vec3 delta = b.Position - a.Position;
		float radiusSum = a.Radius + b.Radius;

		float distanceSquared = glm::dot(delta, delta);
		if (distanceSquared >= radiusSum * radiusSum)
			return false;

		float distance = std::sqrt(distanceSquared);

		out.A = ia;
		out.B = ib;
		out.Normal = distance > 0.0001f ? delta / distance : glm::vec3(1.0f, 0.0f, 0.0f);
		out.Penetration = radiusSum - distance;

		// Halfway into the overlap, so the lever arm is a full radius long for
		// both -- which is what lets friction roll a sphere rather than only
		// drag it.
		out.Point = a.Position + out.Normal * (a.Radius - out.Penetration * 0.5f);

		out.PointCount = 1;
		out.Points[0].Position = out.Point;
		out.Points[0].Penetration = out.Penetration;
		return true;
	}

	// A is the sphere, B is the box.
	static bool CollideSphereBox(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		// Into the box's frame, where it is axis-aligned and the clamp works
		// unchanged. The same trick Raycast3D and the 2D narrowphase use: a
		// turned box is not an AABB in world space, but a point is still a
		// point in any frame you put it in.
		glm::quat inverse = glm::conjugate(b.Orientation);
		glm::vec3 local = inverse * (a.Position - b.Position);

		glm::vec3 closest = glm::clamp(local, -b.HalfExtents, b.HalfExtents);
		glm::vec3 toSphere = local - closest;
		float distanceSquared = glm::dot(toSphere, toSphere);

		glm::vec3 localNormal;   // A towards B, still in the box's frame
		glm::vec3 localPoint;

		if (distanceSquared > 0.000001f)
		{
			if (distanceSquared >= a.Radius * a.Radius)
				return false;

			float distance = std::sqrt(distanceSquared);
			localNormal = -toSphere / distance;
			localPoint = closest;
			out.Penetration = a.Radius - distance;
		}
		else
		{
			// Centre inside the box: clamping gave the centre back, so there
			// is no direction to work from. Out through the nearest face.
			int axis = 0;
			float leastOverlap = std::numeric_limits<float>::max();

			for (int i = 0; i < 3; i++)
			{
				float overlap = b.HalfExtents[i] - std::fabs(local[i]);
				if (overlap < leastOverlap)
				{
					leastOverlap = overlap;
					axis = i;
				}
			}

			localNormal = glm::vec3(0.0f);
			localNormal[axis] = local[axis] < 0.0f ? 1.0f : -1.0f;
			localPoint = local;
			out.Penetration = leastOverlap + a.Radius;
		}

		out.A = ia;
		out.B = ib;
		out.Normal = b.Orientation * localNormal;
		out.Point = b.Position + b.Orientation * localPoint;

		out.PointCount = 1;
		out.Points[0].Position = out.Point;
		out.Points[0].Penetration = out.Penetration;
		return true;
	}

	static bool CollideBoxBox(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		Manifold3D manifold = Sat3D::BoxBox(ObbOf(a), ObbOf(b));
		if (!manifold.Touching)
			return false;

		out.A = ia;
		out.B = ib;
		out.Normal = manifold.Normal;
		out.PointCount = std::min(manifold.PointCount, 8);

		float deepest = -std::numeric_limits<float>::max();
		for (int i = 0; i < out.PointCount; i++)
		{
			out.Points[i].Position = manifold.Points[i];
			out.Points[i].Penetration = manifold.Depths[i];

			if (manifold.Depths[i] > deepest)
			{
				deepest = manifold.Depths[i];
				out.Point = manifold.Points[i];
			}
		}

		out.Penetration = std::max(deepest, 0.0f);
		return true;
	}

	static bool Collide(unsigned int i, const RigidBody3D& a, unsigned int j,
		const RigidBody3D& b, Contact3D& out)
	{
		if (a.Shape == ColliderShape3D::Sphere && b.Shape == ColliderShape3D::Sphere)
			return CollideSphereSphere(i, a, j, b, out);

		if (a.Shape == ColliderShape3D::Box && b.Shape == ColliderShape3D::Box)
			return CollideBoxBox(i, a, j, b, out);

		if (a.Shape == ColliderShape3D::Sphere)
			return CollideSphereBox(i, a, j, b, out);

		// Box vs sphere: run it the other way round and flip the normal,
		// rather than writing the test twice.
		if (!CollideSphereBox(j, b, i, a, out))
			return false;

		out.A = i;
		out.B = j;
		out.Normal = -out.Normal;
		return true;
	}

	void PhysicsWorld3D::Step(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics3D::Step");

		IntegrateVelocities(dt);

		GenerateContacts();
		PrepareContacts();
		WarmStart();

		{
			EGSS_PROFILE_SCOPE("Physics3D::Solve");

			for (unsigned int i = 0; i < VelocityIterations; i++)
				SolveVelocities();
		}

		IntegratePositions(dt);
		CorrectPositions();

		// Hand this step's impulses to the next one, with the points they were
		// applied at so the next step can match them up.
		m_PreviousImpulses.clear();
		for (const Contact3D& contact : m_Contacts)
		{
			PreviousContact& stored = m_PreviousImpulses[ContactKey(contact.A, contact.B)];
			stored.PointCount = contact.PointCount;

			for (int p = 0; p < contact.PointCount; p++)
			{
				stored.Points[p] = contact.Points[p].Position;
				stored.NormalImpulse[p] = contact.Points[p].NormalImpulse;
				stored.TangentImpulse[p][0] = contact.Points[p].TangentImpulse[0];
				stored.TangentImpulse[p][1] = contact.Points[p].TangentImpulse[1];
			}
		}

		UpdateSleeping(dt);
	}

	void PhysicsWorld3D::GenerateContacts()
	{
		EGSS_PROFILE_SCOPE("Physics3D::Narrowphase");

		m_Contacts.clear();

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			for (unsigned int j = i + 1; j < m_Bodies.size(); j++)
				TestPair(i, j);
		}
	}

	void PhysicsWorld3D::TestPair(unsigned int i, unsigned int j)
	{
		const RigidBody3D& a = m_Bodies[i];
		const RigidBody3D& b = m_Bodies[j];

		// Two immovable bodies can never resolve anything.
		if (a.InverseMass == 0.0f && b.InverseMass == 0.0f)
			return;

		Contact3D contact;
		if (!Collide(i, a, j, b, contact))
			return;

		// Pick up where the same pair left off last step, point by point and
		// matched by position rather than index.
		auto previous = m_PreviousImpulses.find(ContactKey(contact.A, contact.B));
		if (previous != m_PreviousImpulses.end())
		{
			const PreviousContact& old = previous->second;

			for (int p = 0; p < contact.PointCount; p++)
			{
				int best = -1;
				float bestDistance = s_WarmStartRadius * s_WarmStartRadius;

				for (int q = 0; q < old.PointCount; q++)
				{
					glm::vec3 offset = contact.Points[p].Position - old.Points[q];
					float distanceSquared = glm::dot(offset, offset);

					if (distanceSquared < bestDistance)
					{
						bestDistance = distanceSquared;
						best = q;
					}
				}

				if (best < 0)
					continue;

				contact.Points[p].NormalImpulse = old.NormalImpulse[best];
				contact.Points[p].TangentImpulse[0] = old.TangentImpulse[best][0];
				contact.Points[p].TangentImpulse[1] = old.TangentImpulse[best][1];
			}
		}

		m_Contacts.push_back(contact);
	}

	void PhysicsWorld3D::PrepareContacts()
	{
		for (Contact3D& contact : m_Contacts)
		{
			RigidBody3D& a = m_Bodies[contact.A];
			RigidBody3D& b = m_Bodies[contact.B];

			float inverseMassA = SolverInverseMass(a);
			float inverseMassB = SolverInverseMass(b);
			glm::mat3 inverseInertiaA = SolverInverseInertia(a);
			glm::mat3 inverseInertiaB = SolverInverseInertia(b);

			BuildTangents(contact.Normal, contact.Tangent[0], contact.Tangent[1]);

			float restitution = std::min(a.Restitution, b.Restitution);

			for (int p = 0; p < contact.PointCount; p++)
			{
				ContactPoint3D& point = contact.Points[p];

				point.LeverA = point.Position - a.Position;
				point.LeverB = point.Position - b.Position;

				point.NormalMass = EffectiveMass(contact.Normal, point.LeverA, point.LeverB,
					inverseMassA, inverseMassB, inverseInertiaA, inverseInertiaB);

				for (int t = 0; t < 2; t++)
				{
					point.TangentMass[t] = EffectiveMass(contact.Tangent[t],
						point.LeverA, point.LeverB,
						inverseMassA, inverseMassB, inverseInertiaA, inverseInertiaB);
				}

				float approach = glm::dot(PointVelocity(b, point.LeverB)
					- PointVelocity(a, point.LeverA), contact.Normal);

				point.RestitutionBias = approach < -s_RestitutionThreshold
					? -restitution * approach
					: 0.0f;
			}
		}
	}

	void PhysicsWorld3D::WarmStart()
	{
		for (const Contact3D& contact : m_Contacts)
		{
			RigidBody3D& a = m_Bodies[contact.A];
			RigidBody3D& b = m_Bodies[contact.B];

			for (int p = 0; p < contact.PointCount; p++)
			{
				const ContactPoint3D& point = contact.Points[p];

				glm::vec3 impulse = point.NormalImpulse * contact.Normal
					+ point.TangentImpulse[0] * contact.Tangent[0]
					+ point.TangentImpulse[1] * contact.Tangent[1];

				ApplyContactImpulse(a, b, point, impulse);
			}
		}
	}

	void PhysicsWorld3D::SolveVelocities()
	{
		for (Contact3D& contact : m_Contacts)
		{
			RigidBody3D& a = m_Bodies[contact.A];
			RigidBody3D& b = m_Bodies[contact.B];

			float mu = std::sqrt(a.Friction * b.Friction);

			// Every point's normal impulse first, then every point's friction,
			// so friction is bounded by a normal impulse every point has
			// already contributed to rather than by a partial one.
			for (int p = 0; p < contact.PointCount; p++)
			{
				ContactPoint3D& point = contact.Points[p];
				if (point.NormalMass <= 0.0f)
					continue;

				glm::vec3 relative = PointVelocity(b, point.LeverB) - PointVelocity(a, point.LeverA);
				float alongNormal = glm::dot(relative, contact.Normal);

				float deltaImpulse = (-alongNormal + point.RestitutionBias) * point.NormalMass;

				// Clamp the total, not this iteration's increment: a contact
				// may only ever push.
				float previousImpulse = point.NormalImpulse;
				point.NormalImpulse = std::max(previousImpulse + deltaImpulse, 0.0f);
				deltaImpulse = point.NormalImpulse - previousImpulse;

				ApplyContactImpulse(a, b, point, deltaImpulse * contact.Normal);
			}

			for (int p = 0; p < contact.PointCount; p++)
			{
				ContactPoint3D& point = contact.Points[p];
				if (point.NormalMass <= 0.0f)
					continue;

				// --- Friction, in both tangent directions ---
				//
				// Solved one axis at a time and clamped against a circle of
				// radius mu*Jn rather than each axis being clamped alone. A
				// per-axis clamp bounds friction by a *square*, which lets a
				// body slide 41% harder along a diagonal than along either
				// axis -- and which axis is which depends on the tangent basis,
				// so the effect rotates with the contact normal.
				float maxFriction = mu * point.NormalImpulse;

				glm::vec2 before(point.TangentImpulse[0], point.TangentImpulse[1]);
				glm::vec2 after = before;

				glm::vec3 relative = PointVelocity(b, point.LeverB) - PointVelocity(a, point.LeverA);

				for (int t = 0; t < 2; t++)
				{
					float alongTangent = glm::dot(relative, contact.Tangent[t]);
					after[t] += -alongTangent * point.TangentMass[t];
				}

				float length = glm::length(after);
				if (length > maxFriction && length > 0.0001f)
					after *= maxFriction / length;

				point.TangentImpulse[0] = after.x;
				point.TangentImpulse[1] = after.y;

				// Warm starting means the running totals are what has to stay
				// valid, so the clamp applies to them -- and what actually
				// gets applied this iteration is the difference.
				glm::vec2 change = after - before;
				glm::vec3 frictionImpulse =
					change.x * contact.Tangent[0] + change.y * contact.Tangent[1];

				ApplyContactImpulse(a, b, point, frictionImpulse);
			}
		}
	}

	void PhysicsWorld3D::IntegrateVelocities(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics3D::IntegrateVelocities");

		// The awake count is recomputed by UpdateSleeping at the end of the
		// step, which is the only place that knows the final answer.
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

	void PhysicsWorld3D::CorrectPositions()
	{
		EGSS_PROFILE_SCOPE("Physics3D::CorrectPositions");

		// The velocity solver stops bodies approaching but cannot undo an
		// overlap that already exists -- gravity sinks a resting body a little
		// every step. Iterated, and re-measuring overlap each pass, because
		// pushing the bottom of a stack up drives it into the box above.
		for (unsigned int iteration = 0; iteration < PositionIterations; iteration++)
		{
			for (const Contact3D& stale : m_Contacts)
			{
				RigidBody3D& a = m_Bodies[stale.A];
				RigidBody3D& b = m_Bodies[stale.B];

				float inverseMassA = SolverInverseMass(a);
				float inverseMassB = SolverInverseMass(b);

				if (inverseMassA + inverseMassB <= 0.0f)
					continue;

				glm::mat3 inverseInertiaA = SolverInverseInertia(a);
				glm::mat3 inverseInertiaB = SolverInverseInertia(b);

				Contact3D current;
				if (!Collide(stale.A, a, stale.B, b, current))
					continue;

				for (int p = 0; p < current.PointCount; p++)
				{
					const ContactPoint3D& point = current.Points[p];

					float depth = std::max(point.Penetration - s_PenetrationSlop, 0.0f);
					if (depth <= 0.0f)
						continue;

					depth = std::min(depth, s_MaxCorrection);

					glm::vec3 leverA = point.Position - a.Position;
					glm::vec3 leverB = point.Position - b.Position;

					float normalMass = EffectiveMass(current.Normal, leverA, leverB,
						inverseMassA, inverseMassB, inverseInertiaA, inverseInertiaB);

					if (normalMass <= 0.0f)
						continue;

					float correction = depth * s_CorrectionPercent * normalMass;
					glm::vec3 impulse = current.Normal * correction;

					// Linear only. The 2D solver also rotates bodies here, and
					// that is right there -- the correction is a scalar and the
					// two contact points of a face are symmetric.
					//
					// In 3D it wrecks a stack. A small box has an inverse
					// inertia around 24, so the rotation implied by a
					// correction impulse is large next to the linear nudge it
					// accompanies; applied per point, four points deep, eight
					// iterations a step, each using a lever measured before the
					// previous point turned the body. The asymmetries compound
					// and friction then locks in whatever lean they produce.
					// Measured on a four-box stack over 200 steps: 99.6 degrees
					// of tilt and collapsed, against 0.24 degrees and standing
					// with the rotation removed.
					//
					// Nothing is lost. Levelling a crate that landed on a
					// corner is the *velocity* solver's job, and it does it --
					// a crate dropped flat onto an 18 degree slope still
					// settles flush to within 0.21 degrees.
					a.Position -= impulse * inverseMassA;
					b.Position += impulse * inverseMassB;
				}
			}
		}
	}

	void PhysicsWorld3D::UpdateSleeping(float dt)
	{
		m_AwakeBodyCount = 0;

		for (RigidBody3D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static)
				continue;

			if (!AllowSleeping)
			{
				body.Awake = true;
				body.SleepTimer = 0.0f;
			}
			else
			{
				// A body still turning is still moving, even if it has stopped
				// travelling -- without the angular test it would fall asleep
				// mid-tumble and freeze at whatever angle it reached.
				bool slow = glm::length(body.Velocity) < SleepVelocity
					&& glm::length(body.AngularVelocity) < SleepAngularVelocity;

				if (slow)
				{
					body.SleepTimer += dt;
					if (body.SleepTimer >= SleepTime)
						body.Awake = false;
				}
				else
				{
					body.SleepTimer = 0.0f;
					body.Awake = true;
				}
			}

			if (body.Awake)
				m_AwakeBodyCount++;
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
