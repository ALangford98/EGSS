#include "egsspch.h"
#include "Egss/Physics/PhysicsWorld2D.h"

namespace Egss {

	// Bodies are allowed to overlap by this much before the position solver
	// bothers. Without a little slack, correcting every contact to exactly
	// zero overlap makes resting bodies vibrate.
	static const float s_PenetrationSlop = 0.005f;

	// Fraction of the remaining overlap resolved per step. Correcting all of
	// it at once overshoots and adds energy the velocity solver never asked
	// for; this converges over a few steps instead.
	static const float s_CorrectionPercent = 0.8f;

	// Below this closing speed, restitution is ignored. A body resting on the
	// floor re-collides every step at a tiny speed, and bouncing it back each
	// time is exactly the jitter you see in engines that skip this.
	static const float s_RestitutionThreshold = 1.0f;

	PhysicsWorld2D::BodyHandle PhysicsWorld2D::AddBody(const RigidBody2D& body)
	{
		m_Bodies.push_back(body);
		m_Bodies.back().PreviousPosition = body.Position;
		return (BodyHandle)(m_Bodies.size() - 1);
	}

	void PhysicsWorld2D::Clear()
	{
		m_Bodies.clear();
		m_Contacts.clear();
		m_AwakeBodyCount = 0;
	}

	// A stable key for a body pair, so a contact can be recognised as the same
	// one next step.
	static unsigned long long ContactKey(unsigned int a, unsigned int b)
	{
		return ((unsigned long long)a << 32) | (unsigned long long)b;
	}

	// Perpendicular to the normal. Derived from the normal rather than from
	// relative velocity, so the tangent doesn't flip direction between steps
	// and invalidate the friction impulse being carried over.
	static glm::vec2 TangentOf(const glm::vec2& normal)
	{
		return { -normal.y, normal.x };
	}

	// A sleeping body is immovable for as long as it stays asleep. Without
	// this the solver keeps pushing it -- it never integrates, so it doesn't
	// visibly move, but it banks velocity and lurches the moment it wakes.
	static float SolverInverseMass(const RigidBody2D& body)
	{
		return (body.Type == BodyType::Static || !body.Awake) ? 0.0f : body.InverseMass;
	}

	void PhysicsWorld2D::Step(float dt)
	{
		Integrate(dt);
		GenerateContacts();

		// Replay last step's impulses before solving anything.
		WarmStart();

		// Sequential impulses: each pass corrects the error the previous one
		// left behind. One pass resolves a single collision fine; a stack
		// needs several, because fixing the bottom contact disturbs the one
		// above it.
		for (unsigned int i = 0; i < VelocityIterations; i++)
			SolveVelocities();

		CorrectPositions();

		// Hand this step's impulses to the next one.
		m_PreviousImpulses.clear();
		for (const Contact& contact : m_Contacts)
			m_PreviousImpulses[ContactKey(contact.A, contact.B)] = { contact.NormalImpulse, contact.TangentImpulse };

		UpdateSleeping(dt);
	}

	void PhysicsWorld2D::Integrate(float dt)
	{
		m_AwakeBodyCount = 0;

		for (RigidBody2D& body : m_Bodies)
		{
			body.PreviousPosition = body.Position;

			if (body.Type == BodyType::Static || !body.Awake)
				continue;

			m_AwakeBodyCount++;

			// Semi-implicit Euler: velocity first, then position from the
			// *new* velocity. One line different from explicit Euler, and
			// stable at step sizes where explicit Euler gains energy and
			// throws bodies through the floor.
			body.Velocity += Gravity * body.GravityScale * dt;
			body.Position += body.Velocity * dt;
		}
	}

	// --- Narrowphase --------------------------------------------------------
	// Each returns true and fills the contact, or returns false. The normal
	// always points from A towards B.

	static bool CollideCircleCircle(unsigned int ia, const RigidBody2D& a,
		unsigned int ib, const RigidBody2D& b, Contact& out)
	{
		glm::vec2 delta = b.Position - a.Position;
		float radiusSum = a.Radius + b.Radius;

		float distanceSquared = glm::dot(delta, delta);
		if (distanceSquared >= radiusSum * radiusSum)
			return false;

		float distance = std::sqrt(distanceSquared);

		out.A = ia;
		out.B = ib;
		// Exactly concentric has no meaningful normal; pick one so the bodies
		// still separate instead of sitting inside each other forever.
		out.Normal = distance > 0.0001f ? delta / distance : glm::vec2(1.0f, 0.0f);
		out.Penetration = radiusSum - distance;
		out.Point = a.Position + out.Normal * a.Radius;
		return true;
	}

	static bool CollideBoxBox(unsigned int ia, const RigidBody2D& a,
		unsigned int ib, const RigidBody2D& b, Contact& out)
	{
		glm::vec2 delta = b.Position - a.Position;

		float overlapX = a.HalfExtents.x + b.HalfExtents.x - std::abs(delta.x);
		if (overlapX <= 0.0f)
			return false;

		float overlapY = a.HalfExtents.y + b.HalfExtents.y - std::abs(delta.y);
		if (overlapY <= 0.0f)
			return false;

		out.A = ia;
		out.B = ib;

		// Separate along whichever axis is overlapped least -- the shortest
		// way out. This is what makes a box land on a floor rather than being
		// shoved sideways off it.
		if (overlapX < overlapY)
		{
			out.Normal = { delta.x < 0.0f ? -1.0f : 1.0f, 0.0f };
			out.Penetration = overlapX;
		}
		else
		{
			out.Normal = { 0.0f, delta.y < 0.0f ? -1.0f : 1.0f };
			out.Penetration = overlapY;
		}

		out.Point = a.Position + out.Normal * a.HalfExtents;
		return true;
	}

	// A is the circle, B is the box.
	static bool CollideCircleBox(unsigned int ia, const RigidBody2D& a,
		unsigned int ib, const RigidBody2D& b, Contact& out)
	{
		glm::vec2 boxMin = b.Position - b.HalfExtents;
		glm::vec2 boxMax = b.Position + b.HalfExtents;

		// Nearest point on the box to the circle's centre.
		glm::vec2 closest = glm::clamp(a.Position, boxMin, boxMax);
		glm::vec2 toCircle = a.Position - closest;
		float distanceSquared = glm::dot(toCircle, toCircle);

		out.A = ia;
		out.B = ib;

		if (distanceSquared > 0.0001f)
		{
			if (distanceSquared >= a.Radius * a.Radius)
				return false;

			float distance = std::sqrt(distanceSquared);
			out.Normal = -toCircle / distance;   // A towards B
			out.Penetration = a.Radius - distance;
			out.Point = closest;
			return true;
		}

		// Centre is inside the box: clamping gave back the centre itself, so
		// there is no direction to work from. Push out through the nearest
		// face instead.
		glm::vec2 delta = a.Position - b.Position;
		float overlapX = b.HalfExtents.x - std::abs(delta.x);
		float overlapY = b.HalfExtents.y - std::abs(delta.y);

		if (overlapX < overlapY)
		{
			out.Normal = { delta.x < 0.0f ? 1.0f : -1.0f, 0.0f };
			out.Penetration = overlapX + a.Radius;
		}
		else
		{
			out.Normal = { 0.0f, delta.y < 0.0f ? 1.0f : -1.0f };
			out.Penetration = overlapY + a.Radius;
		}

		out.Point = a.Position;
		return true;
	}

	// Shape dispatch, shared by detection and position correction. The latter
	// needs to re-measure overlap as it goes, not reuse a stale figure.
	static bool Collide(unsigned int i, RigidBody2D& a, unsigned int j, RigidBody2D& b, Contact& out)
	{
		if (a.Shape == ColliderShape::Circle && b.Shape == ColliderShape::Circle)
			return CollideCircleCircle(i, a, j, b, out);

		if (a.Shape == ColliderShape::Box && b.Shape == ColliderShape::Box)
			return CollideBoxBox(i, a, j, b, out);

		if (a.Shape == ColliderShape::Circle)
			return CollideCircleBox(i, a, j, b, out);

		// Box vs circle: run it the other way round and flip the normal,
		// rather than writing the test twice.
		if (!CollideCircleBox(j, b, i, a, out))
			return false;

		out.A = i;
		out.B = j;
		out.Normal = -out.Normal;
		return true;
	}

	void PhysicsWorld2D::GenerateContacts()
	{
		m_Contacts.clear();

		// Brute force. Every unordered pair, once. Fine to a few hundred
		// bodies; a uniform grid or sweep-and-prune replaces exactly this
		// loop when it stops being fine.
		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			for (unsigned int j = i + 1; j < m_Bodies.size(); j++)
			{
				RigidBody2D& a = m_Bodies[i];
				RigidBody2D& b = m_Bodies[j];

				// Two immovable bodies can never resolve anything.
				if (a.InverseMass == 0.0f && b.InverseMass == 0.0f)
					continue;

				Contact contact;
				if (!Collide(i, a, j, b, contact))
					continue;

				// Only a moving dynamic body wakes a sleeper. Static geometry
				// is permanently "awake" -- letting it wake things meant
				// anything resting on the floor was woken every step and could
				// never sleep at all.
				// Pick up where the same pair left off last step.
				auto previous = m_PreviousImpulses.find(ContactKey(contact.A, contact.B));
				if (previous != m_PreviousImpulses.end())
				{
					contact.NormalImpulse = previous->second.x;
					contact.TangentImpulse = previous->second.y;
				}

				// Restitution is measured now, against the speed the bodies
				// are actually closing at, and held fixed for the whole solve.
				float approach = glm::dot(b.Velocity - a.Velocity, contact.Normal);
				float restitution = std::min(a.Restitution, b.Restitution);

				contact.RestitutionBias = approach < -s_RestitutionThreshold
					? -restitution * approach
					: 0.0f;

				m_Contacts.push_back(contact);
			}
		}
	}

	void PhysicsWorld2D::WarmStart()
	{
		for (const Contact& contact : m_Contacts)
		{
			RigidBody2D& a = m_Bodies[contact.A];
			RigidBody2D& b = m_Bodies[contact.B];

			glm::vec2 impulse = contact.NormalImpulse * contact.Normal
				+ contact.TangentImpulse * TangentOf(contact.Normal);

			a.Velocity -= impulse * SolverInverseMass(a);
			b.Velocity += impulse * SolverInverseMass(b);
		}
	}

	void PhysicsWorld2D::SolveVelocities()
	{
		for (Contact& contact : m_Contacts)
		{
			RigidBody2D& a = m_Bodies[contact.A];
			RigidBody2D& b = m_Bodies[contact.B];

			float inverseMassA = SolverInverseMass(a);
			float inverseMassB = SolverInverseMass(b);
			float inverseMassSum = inverseMassA + inverseMassB;
			if (inverseMassSum <= 0.0f)
				continue;

			// --- Normal ---------------------------------------------------
			glm::vec2 relativeVelocity = b.Velocity - a.Velocity;
			float alongNormal = glm::dot(relativeVelocity, contact.Normal);

			float deltaImpulse = (-alongNormal + contact.RestitutionBias) / inverseMassSum;

			// Clamp the *total*, not this iteration's increment: a contact may
			// only ever push, and warm starting means the running total is the
			// thing that has to stay valid.
			float previousImpulse = contact.NormalImpulse;
			contact.NormalImpulse = std::max(previousImpulse + deltaImpulse, 0.0f);
			deltaImpulse = contact.NormalImpulse - previousImpulse;

			glm::vec2 impulse = deltaImpulse * contact.Normal;
			a.Velocity -= impulse * inverseMassA;
			b.Velocity += impulse * inverseMassB;

			// --- Friction -------------------------------------------------
			glm::vec2 tangent = TangentOf(contact.Normal);

			relativeVelocity = b.Velocity - a.Velocity;
			float alongTangent = glm::dot(relativeVelocity, tangent);

			float deltaFriction = -alongTangent / inverseMassSum;
			float mu = std::sqrt(a.Friction * b.Friction);

			// Coulomb: total friction can never exceed mu times the total
			// normal impulse. Past that the surfaces slide.
			float maxFriction = mu * contact.NormalImpulse;
			float previousFriction = contact.TangentImpulse;

			contact.TangentImpulse = std::max(-maxFriction,
				std::min(previousFriction + deltaFriction, maxFriction));
			deltaFriction = contact.TangentImpulse - previousFriction;

			glm::vec2 frictionImpulse = deltaFriction * tangent;
			a.Velocity -= frictionImpulse * inverseMassA;
			b.Velocity += frictionImpulse * inverseMassB;
		}
	}

	void PhysicsWorld2D::CorrectPositions()
	{
		// The velocity solver stops bodies approaching, but it cannot undo an
		// overlap that already exists -- gravity sinks a resting body a little
		// every step, and without this it slowly disappears through the floor.
		//
		// Iterated, and re-measuring overlap each pass: pushing the bottom of
		// a stack up drives it into the box above, and a single pass against
		// pre-correction figures never sees that. One pass makes a five-box
		// stack settle roughly 40% shorter than the boxes actually are.
		for (unsigned int iteration = 0; iteration < PositionIterations; iteration++)
		{
			for (const Contact& stale : m_Contacts)
			{
				RigidBody2D& a = m_Bodies[stale.A];
				RigidBody2D& b = m_Bodies[stale.B];

				float inverseMassA = SolverInverseMass(a);
				float inverseMassB = SolverInverseMass(b);
				float inverseMassSum = inverseMassA + inverseMassB;
				if (inverseMassSum <= 0.0f)
					continue;

				Contact current;
				if (!Collide(stale.A, a, stale.B, b, current))
					continue;

				float depth = std::max(current.Penetration - s_PenetrationSlop, 0.0f);
				if (depth <= 0.0f)
					continue;

				glm::vec2 correction = (depth / inverseMassSum) * s_CorrectionPercent * current.Normal;

				a.Position -= correction * inverseMassA;
				b.Position += correction * inverseMassB;
			}
		}
	}

	// Union-find over the contact graph, so bodies that are touching are
	// treated as one unit.
	static unsigned int FindRoot(std::vector<unsigned int>& parent, unsigned int i)
	{
		while (parent[i] != i)
		{
			parent[i] = parent[parent[i]];   // path halving
			i = parent[i];
		}
		return i;
	}

	void PhysicsWorld2D::UpdateSleeping(float dt)
	{
		if (!AllowSleeping)
		{
			for (RigidBody2D& body : m_Bodies)
				body.Awake = true;
			return;
		}

		float threshold = SleepVelocity * SleepVelocity;

		// Per-body: how long has this one been slow?
		for (RigidBody2D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static)
				continue;

			if (glm::dot(body.Velocity, body.Velocity) > threshold)
				body.SleepTimer = 0.0f;
			else
				body.SleepTimer += dt;
		}

		// Group touching dynamic bodies. Static bodies are deliberately not
		// merged -- otherwise the floor would join every pile on it into one
		// island, and nothing could sleep while anything, anywhere, moved.
		std::vector<unsigned int> parent(m_Bodies.size());
		for (unsigned int i = 0; i < parent.size(); i++)
			parent[i] = i;

		for (const Contact& contact : m_Contacts)
		{
			if (m_Bodies[contact.A].Type != BodyType::Dynamic ||
				m_Bodies[contact.B].Type != BodyType::Dynamic)
				continue;

			unsigned int rootA = FindRoot(parent, contact.A);
			unsigned int rootB = FindRoot(parent, contact.B);
			if (rootA != rootB)
				parent[rootA] = rootB;
		}

		// An island sleeps only when every body in it has been quiet long
		// enough, and wakes as a whole the moment any one of them moves.
		//
		// Doing this per body instead produces a limit cycle: the first body
		// to sleep becomes immovable, that jolts its neighbour, the neighbour
		// wakes it again, and a stack never settles.
		std::unordered_map<unsigned int, bool> islandCanSleep;

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			if (m_Bodies[i].Type != BodyType::Dynamic)
				continue;

			unsigned int root = FindRoot(parent, i);
			bool quiet = m_Bodies[i].SleepTimer >= SleepTime;

			auto it = islandCanSleep.find(root);
			if (it == islandCanSleep.end())
				islandCanSleep[root] = quiet;
			else
				it->second = it->second && quiet;
		}

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			RigidBody2D& body = m_Bodies[i];
			if (body.Type != BodyType::Dynamic)
				continue;

			bool canSleep = islandCanSleep[FindRoot(parent, i)];

			if (canSleep)
			{
				body.Awake = false;
				body.Velocity = { 0.0f, 0.0f };
			}
			else if (!body.Awake)
			{
				body.Awake = true;
				body.SleepTimer = 0.0f;
			}
		}
	}

}
