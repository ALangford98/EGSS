#include "egsspch.h"
#include "Egss/Physics/PhysicsWorld2D.h"
#include "Egss/Debug/Instrumentor.h"

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

	// Axis-aligned bounds, whatever the shape.
	static void BodyBounds(const RigidBody2D& body, glm::vec2& outMin, glm::vec2& outMax)
	{
		glm::vec2 extent = body.Shape == ColliderShape::Circle
			? glm::vec2(body.Radius)
			: body.HalfExtents;

		outMin = body.Position - extent;
		outMax = body.Position + extent;
	}

	PhysicsWorld2D::BodyHandle PhysicsWorld2D::AddBody(const RigidBody2D& body)
	{
		m_Bodies.push_back(body);
		m_Bodies.back().PreviousPosition = body.Position;
		MarkGridDirty();
		return (BodyHandle)(m_Bodies.size() - 1);
	}

	void PhysicsWorld2D::Clear()
	{
		m_Bodies.clear();
		m_Contacts.clear();
		m_AwakeBodyCount = 0;
		MarkGridDirty();
	}

	// Buckets every body into the cells its bounds overlap. Rebuilt whenever
	// anything has moved, which for a simulated world is every step -- the
	// cost of that rebuild is the price the queries pay for being cheap.
	void PhysicsWorld2D::RebuildGrid() const
	{
		m_GridDirty = false;
		m_Cells.clear();
		m_GridWidth = 0;
		m_GridHeight = 0;

		if (m_Bodies.empty())
			return;

		glm::vec2 worldMin(std::numeric_limits<float>::max());
		glm::vec2 worldMax(-std::numeric_limits<float>::max());

		for (const RigidBody2D& body : m_Bodies)
		{
			glm::vec2 boundsMin, boundsMax;
			BodyBounds(body, boundsMin, boundsMax);
			worldMin = glm::min(worldMin, boundsMin);
			worldMax = glm::max(worldMax, boundsMax);
		}

		m_GridCellSize = std::max(CellSize, 0.01f);
		m_GridOrigin = worldMin;

		glm::vec2 span = worldMax - worldMin;
		m_GridWidth = std::max(1, (int)(span.x / m_GridCellSize) + 1);
		m_GridHeight = std::max(1, (int)(span.y / m_GridCellSize) + 1);

		// A pathological cell size would otherwise allocate unboundedly.
		const int maxCells = 1 << 20;
		if ((long long)m_GridWidth * m_GridHeight > maxCells)
		{
			m_GridWidth = 0;
			m_GridHeight = 0;
			return;
		}

		m_Cells.assign((size_t)m_GridWidth * m_GridHeight, {});

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			glm::vec2 boundsMin, boundsMax;
			BodyBounds(m_Bodies[i], boundsMin, boundsMax);

			int x0 = std::max(0, (int)((boundsMin.x - m_GridOrigin.x) / m_GridCellSize));
			int y0 = std::max(0, (int)((boundsMin.y - m_GridOrigin.y) / m_GridCellSize));
			int x1 = std::min(m_GridWidth - 1, (int)((boundsMax.x - m_GridOrigin.x) / m_GridCellSize));
			int y1 = std::min(m_GridHeight - 1, (int)((boundsMax.y - m_GridOrigin.y) / m_GridCellSize));

			// A body large relative to the cell lands in several, which is
			// what makes the per-query stamp necessary.
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
					m_Cells[(size_t)y * m_GridWidth + x].push_back(i);
		}

		m_QueryStamp.assign(m_Bodies.size(), 0);
		m_QueryCounter = 0;
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
		EGSS_PROFILE_SCOPE("Physics::Step");

		Integrate(dt);

		// Positions changed, so last frame's buckets are stale.
		MarkGridDirty();
		GenerateContacts();

		// Replay last step's impulses before solving anything.
		WarmStart();

		EGSS_PROFILE_SCOPE("Physics::Solve");

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
		EGSS_PROFILE_SCOPE("Physics::Integrate");

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
		EGSS_PROFILE_SCOPE("Physics::Broadphase+Narrowphase");

		m_Contacts.clear();
		m_Candidates = 0;

		if (UseBroadphase)
		{
			if (m_GridDirty)
				RebuildGrid();
		}

		// Falls back to brute force if the grid could not be built -- an empty
		// world, or a cell size that would have needed too many cells.
		bool useGrid = UseBroadphase && m_GridWidth > 0 && m_GridHeight > 0;

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			if (useGrid)
			{
				// Only bodies sharing a cell with this one can touch it.
				m_QueryCounter++;

				glm::vec2 boundsMin, boundsMax;
				BodyBounds(m_Bodies[i], boundsMin, boundsMax);

				int x0 = std::max(0, (int)((boundsMin.x - m_GridOrigin.x) / m_GridCellSize));
				int y0 = std::max(0, (int)((boundsMin.y - m_GridOrigin.y) / m_GridCellSize));
				int x1 = std::min(m_GridWidth - 1, (int)((boundsMax.x - m_GridOrigin.x) / m_GridCellSize));
				int y1 = std::min(m_GridHeight - 1, (int)((boundsMax.y - m_GridOrigin.y) / m_GridCellSize));

				for (int y = y0; y <= y1; y++)
				{
					for (int x = x0; x <= x1; x++)
					{
						for (unsigned int j : m_Cells[(size_t)y * m_GridWidth + x])
						{
							// j > i keeps each pair once; the stamp keeps a
							// pair that shares several cells from being
							// tested several times.
							if (j <= i)
								continue;
							if (m_QueryStamp[j] == m_QueryCounter)
								continue;
							m_QueryStamp[j] = m_QueryCounter;

							m_Candidates++;
							TestPair(i, j);
						}
					}
				}
			}
			else
			{
				for (unsigned int j = i + 1; j < m_Bodies.size(); j++)
				{
					m_Candidates++;
					TestPair(i, j);
				}
			}
		}
	}

	// The narrowphase and bookkeeping for one candidate pair, shared by both
	// the grid path and the brute-force one.
	// The narrowphase and bookkeeping for one candidate pair, shared by both
	// the grid path and the brute-force one.
	void PhysicsWorld2D::TestPair(unsigned int i, unsigned int j)
	{
		RigidBody2D& a = m_Bodies[i];
		RigidBody2D& b = m_Bodies[j];

		// Two immovable bodies can never resolve anything.
		if (a.InverseMass == 0.0f && b.InverseMass == 0.0f)
			return;

		Contact contact;
		if (!Collide(i, a, j, b, contact))
			return;

		// Pick up where the same pair left off last step -- this is what
		// warm starting reads.
		auto previous = m_PreviousImpulses.find(ContactKey(contact.A, contact.B));
		if (previous != m_PreviousImpulses.end())
		{
			contact.NormalImpulse = previous->second.x;
			contact.TangentImpulse = previous->second.y;
		}

		// Restitution is measured now, against the speed the bodies are
		// actually closing at, and held fixed for the whole solve.
		float approach = glm::dot(b.Velocity - a.Velocity, contact.Normal);
		float restitution = std::min(a.Restitution, b.Restitution);

		contact.RestitutionBias = approach < -s_RestitutionThreshold
			? -restitution * approach
			: 0.0f;

		m_Contacts.push_back(contact);
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
		EGSS_PROFILE_SCOPE("Physics::CorrectPositions");

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

	glm::vec2 PhysicsWorld2D::ResolveCircle(const glm::vec2& position, float radius,
		int iterations) const
	{
		// A throwaway body, purely so the existing narrowphase can be reused
		// rather than written a second time.
		RigidBody2D probe = RigidBody2D::MakeCircle(position, radius, 1.0f);

		for (int pass = 0; pass < iterations; pass++)
		{
			bool moved = false;

			for (unsigned int i = 0; i < m_Bodies.size(); i++)
			{
				Contact contact;
				if (!Collide(0, probe, i, const_cast<RigidBody2D&>(m_Bodies[i]), contact))
					continue;

				// The normal runs from the probe towards the body, so backing
				// along it is the shortest way out.
				probe.Position -= contact.Normal * contact.Penetration;
				moved = true;
			}

			// Settled: no overlap left to resolve.
			if (!moved)
				break;
		}

		return probe.Position;
	}

	// --- Raycasting ---------------------------------------------------------

	// Quadratic in t: |origin + dir*t - centre|^2 = r^2. Only the nearer root
	// matters, and only if it lies within the ray.
	static bool RaycastCircle(const glm::vec2& origin, const glm::vec2& direction,
		float maxDistance, const RigidBody2D& body, float& outDistance, glm::vec2& outNormal)
	{
		glm::vec2 toOrigin = origin - body.Position;

		float b = glm::dot(toOrigin, direction);
		float c = glm::dot(toOrigin, toOrigin) - body.Radius * body.Radius;

		// Outside and pointing away.
		if (c > 0.0f && b > 0.0f)
			return false;

		float discriminant = b * b - c;
		if (discriminant < 0.0f)
			return false;

		float t = -b - std::sqrt(discriminant);

		// Negative means the origin is inside the circle.
		if (t < 0.0f)
			t = 0.0f;

		if (t > maxDistance)
			return false;

		glm::vec2 point = origin + direction * t;
		glm::vec2 normal = point - body.Position;

		float length = glm::length(normal);
		outNormal = length > 0.0001f ? normal / length : -direction;
		outDistance = t;
		return true;
	}

	// Slab method: clip the ray against each axis' pair of planes and keep the
	// overlap. If the overlap ever empties, the ray misses.
	static bool RaycastBox(const glm::vec2& origin, const glm::vec2& direction,
		float maxDistance, const RigidBody2D& body, float& outDistance, glm::vec2& outNormal)
	{
		glm::vec2 boxMin = body.Position - body.HalfExtents;
		glm::vec2 boxMax = body.Position + body.HalfExtents;

		float tMin = 0.0f;
		float tMax = maxDistance;

		int hitAxis = -1;
		float hitSign = 0.0f;

		for (int axis = 0; axis < 2; axis++)
		{
			if (std::abs(direction[axis]) < 0.00001f)
			{
				// Parallel to this pair of planes: either always inside the
				// slab or never.
				if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis])
					return false;

				continue;
			}

			float inverse = 1.0f / direction[axis];
			float t1 = (boxMin[axis] - origin[axis]) * inverse;
			float t2 = (boxMax[axis] - origin[axis]) * inverse;

			float sign = -1.0f;
			if (t1 > t2)
			{
				std::swap(t1, t2);
				sign = 1.0f;
			}

			if (t1 > tMin)
			{
				tMin = t1;
				hitAxis = axis;
				hitSign = sign;
			}

			tMax = std::min(tMax, t2);

			if (tMin > tMax)
				return false;
		}

		outDistance = tMin;

		if (hitAxis < 0)
		{
			// Started inside: no face was crossed on the way in.
			outNormal = -direction;
		}
		else
		{
			outNormal = { 0.0f, 0.0f };
			outNormal[hitAxis] = hitSign;
		}

		return true;
	}

	RaycastHit PhysicsWorld2D::Raycast(const glm::vec2& origin, const glm::vec2& direction,
		float maxDistance, unsigned int ignore) const
	{
		// Deliberately NOT profiled per call. A scope timer costs a few
		// hundred nanoseconds -- more than a raycast against a small world --
		// so instrumenting here measures the instrumentation. Time the loop
		// that issues the rays instead.
		RaycastHit result;

		float length = glm::length(direction);
		if (length < 0.00001f || maxDistance <= 0.0f)
			return result;

		glm::vec2 unit = direction / length;
		float nearest = maxDistance;

		// One body, tested against the ray. Kept separate so the grid walk and
		// the brute-force loop share exactly the same narrowphase.
		auto test = [&](unsigned int i)
		{
			if (i == ignore)
				return;

			const RigidBody2D& body = m_Bodies[i];

			float distance = 0.0f;
			glm::vec2 normal(0.0f);

			bool hit = body.Shape == ColliderShape::Circle
				? RaycastCircle(origin, unit, nearest, body, distance, normal)
				: RaycastBox(origin, unit, nearest, body, distance, normal);

			if (!hit || distance > nearest)
				return;

			// Keeping `nearest` tight also prunes later candidates.
			nearest = distance;

			result.Hit = true;
			result.Body = i;
			result.Distance = distance;
			result.Fraction = distance / maxDistance;
			result.Point = origin + unit * distance;
			result.Normal = normal;
		};

		if (UseBroadphase && m_GridDirty)
			RebuildGrid();

		bool useGrid = UseBroadphase && m_GridWidth > 0 && m_GridHeight > 0;

		// The origin has to be inside the grid for the walk to start in the
		// right cell; outside it, fall back rather than guess.
		glm::vec2 local = origin - m_GridOrigin;
		if (useGrid)
		{
			useGrid = local.x >= 0.0f && local.y >= 0.0f
				&& local.x < m_GridWidth * m_GridCellSize
				&& local.y < m_GridHeight * m_GridCellSize;
		}

		if (!useGrid)
		{
			for (unsigned int i = 0; i < m_Bodies.size(); i++)
				test(i);

			return result;
		}

		// --- Grid walk (DDA) ------------------------------------------------
		// Steps cell by cell along the ray rather than testing everything.
		// Because cells are visited in order, the first hit found in a cell is
		// final once the ray has left that cell -- so a short ray in a busy
		// world touches almost nothing.
		m_QueryCounter++;

		int x = std::min((int)(local.x / m_GridCellSize), m_GridWidth - 1);
		int y = std::min((int)(local.y / m_GridCellSize), m_GridHeight - 1);

		int stepX = unit.x > 0.0f ? 1 : (unit.x < 0.0f ? -1 : 0);
		int stepY = unit.y > 0.0f ? 1 : (unit.y < 0.0f ? -1 : 0);

		const float infinity = std::numeric_limits<float>::max();

		// Distance along the ray to the next cell boundary on each axis, and
		// how far apart those boundaries are.
		float tDeltaX = stepX != 0 ? m_GridCellSize / std::abs(unit.x) : infinity;
		float tDeltaY = stepY != 0 ? m_GridCellSize / std::abs(unit.y) : infinity;

		float nextBoundaryX = m_GridOrigin.x + (x + (stepX > 0 ? 1 : 0)) * m_GridCellSize;
		float nextBoundaryY = m_GridOrigin.y + (y + (stepY > 0 ? 1 : 0)) * m_GridCellSize;

		float tMaxX = stepX != 0 ? (nextBoundaryX - origin.x) / unit.x : infinity;
		float tMaxY = stepY != 0 ? (nextBoundaryY - origin.y) / unit.y : infinity;

		float travelled = 0.0f;

		while (travelled <= maxDistance)
		{
			for (unsigned int i : m_Cells[(size_t)y * m_GridWidth + x])
			{
				// A body spanning several cells would otherwise be tested
				// once per cell the ray crosses.
				if (m_QueryStamp[i] == m_QueryCounter)
					continue;
				m_QueryStamp[i] = m_QueryCounter;

				m_Candidates++;
				test(i);
			}

			// Anything beyond this cell is further away than what we have.
			if (result.Hit && nearest <= travelled)
				break;

			if (tMaxX < tMaxY)
			{
				x += stepX;
				travelled = tMaxX;
				tMaxX += tDeltaX;
			}
			else
			{
				y += stepY;
				travelled = tMaxY;
				tMaxY += tDeltaY;
			}

			if (x < 0 || y < 0 || x >= m_GridWidth || y >= m_GridHeight)
				break;
		}

		return result;
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
		EGSS_PROFILE_SCOPE("Physics::Sleeping");

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
