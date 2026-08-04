#include "egsspch.h"
#include "Egss/Physics/PhysicsWorld2D.h"
#include "Egss/Physics/Sat2D.h"
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

	// The furthest a contact point may move between steps and still be treated
	// as the same point for warm starting. Deliberately tight: the case warm
	// starting exists for is a settled stack, whose points barely move at all,
	// and a fast-moving body gains little from it anyway. Too loose and two
	// distinct corners of a tipping crate get matched to each other.
	static const float s_WarmStartRadius = 0.02f;

	// The most overlap the position solver will try to undo in one pass.
	// A deeply interpenetrating pair -- a body spawned inside another, or one
	// that tunnelled -- would otherwise be flung apart, and with the angular
	// term in play it would be flung apart *spinning*.
	static const float s_MaxCorrection = 0.2f;

	// The 2D cross products. With only one axis to turn about, the cross of
	// two vectors is a scalar and the cross of a scalar with a vector is a
	// vector; both fall out of writing the 3D versions with z as the only
	// component that survives.
	static float Cross(const glm::vec2& a, const glm::vec2& b)
	{
		return a.x * b.y - a.y * b.x;
	}

	static glm::vec2 Cross(float angular, const glm::vec2& r)
	{
		return { -angular * r.y, angular * r.x };
	}

	// Axis-aligned bounds, whatever the shape.
	static void BodyBounds(const RigidBody2D& body, glm::vec2& outMin, glm::vec2& outMax)
	{
		glm::vec2 extent;

		if (body.Shape == ColliderShape::Circle)
		{
			extent = glm::vec2(body.Radius);
		}
		else
		{
			// A turned box reaches further than its half extents -- a square
			// at 45 degrees puts its corner 1.41x its half width from the
			// centre. Bounds that ignore that quietly drop pairs the
			// narrowphase would have caught, and a missed collision leaves
			// nothing behind to notice: no contact, no warning, just a body
			// passing through a corner it should have clipped.
			float c = std::fabs(std::cos(body.Rotation));
			float s = std::fabs(std::sin(body.Rotation));

			extent = { body.HalfExtents.x * c + body.HalfExtents.y * s,
					   body.HalfExtents.x * s + body.HalfExtents.y * c };
		}

		outMin = body.Position - extent;
		outMax = body.Position + extent;
	}

	// A body's collider as the geometry Sat2D speaks. Boxes only -- circles
	// have their own test.
	static ObbBox2D ObbOf(const RigidBody2D& body)
	{
		ObbBox2D box;
		box.Centre = body.Position;
		box.HalfExtents = body.HalfExtents;
		box.Rotation = body.Rotation;
		return box;
	}

	// Rotating a vector into a body's frame and back out again. The pair is
	// what lets every oriented test be written once, against a box that is
	// axis-aligned in its own frame.
	static glm::vec2 ToLocal(const glm::vec2& v, float cosine, float sine)
	{
		return { v.x * cosine + v.y * sine, -v.x * sine + v.y * cosine };
	}

	static glm::vec2 ToWorld(const glm::vec2& v, float cosine, float sine)
	{
		return { v.x * cosine - v.y * sine, v.x * sine + v.y * cosine };
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

	// The same rule for the angular half. A static body has no inverse inertia
	// to begin with, but a sleeping dynamic one does, and letting the solver
	// spin it would bank angular velocity it releases the moment it wakes.
	static float SolverInverseInertia(const RigidBody2D& body)
	{
		return (body.Type == BodyType::Static || !body.Awake) ? 0.0f : body.InverseInertia;
	}

	// How fast the material point at `lever` is actually moving: the body's own
	// velocity plus whatever its spin adds out there. This is the quantity the
	// whole angular solver is written against -- using the centre's velocity
	// instead is exactly the bug that leaves a spinning box's contacts
	// unsolved while its centre looks stationary.
	static glm::vec2 PointVelocity(const RigidBody2D& body, const glm::vec2& lever)
	{
		return body.Velocity + Cross(body.AngularVelocity, lever);
	}

	// One impulse applied to both bodies at a contact point: equal and opposite
	// on the linear half, and turning each about its own centre by its own
	// lever arm. Every impulse in the solver goes through here, so there is one
	// place where the sign convention (normal points A -> B) has to be right.
	static void ApplyContactImpulse(RigidBody2D& a, RigidBody2D& b,
		const ContactPoint& point, const glm::vec2& impulse)
	{
		a.Velocity -= impulse * SolverInverseMass(a);
		a.AngularVelocity -= Cross(point.LeverA, impulse) * SolverInverseInertia(a);

		b.Velocity += impulse * SolverInverseMass(b);
		b.AngularVelocity += Cross(point.LeverB, impulse) * SolverInverseInertia(b);
	}

	void PhysicsWorld2D::Step(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics::Step");

		// The order here is load-bearing, and it used to be wrong.
		//
		// Velocities are integrated, then contacts are solved, and only then
		// are positions moved. Doing it the obvious way -- integrating both at
		// once, up front -- lets every body take one free-fall sub-step of
		// g*dt^2 *before* the solver ever sees it. Position correction pushes
		// the normal part of that back out, so on flat ground it is invisible;
		// the part along the surface is never undone, and a block sitting on a
		// 20-degree slope slid downhill at g sin(t) dt = 5.6 cm/s with
		// friction to spare and the contact perfectly healthy. Nothing was
		// wrong with the friction: the body had already moved by the time it
		// was applied.
		//
		// It went unnoticed because until rotation there were no slopes.
		IntegrateVelocities(dt);

		// Against the positions the bodies ended the last step at, which is
		// also where the solve will run.
		MarkGridDirty();
		GenerateContacts();

		// Lever arms and effective masses, once.
		PrepareContacts();

		// Replay last step's impulses before solving anything.
		WarmStart();

		EGSS_PROFILE_SCOPE("Physics::Solve");

		// Sequential impulses: each pass corrects the error the previous one
		// left behind. One pass resolves a single collision fine; a stack
		// needs several, because fixing the bottom contact disturbs the one
		// above it.
		for (unsigned int i = 0; i < VelocityIterations; i++)
			SolveVelocities();

		// Now that the velocities are ones the contacts agree with.
		IntegratePositions(dt);

		CorrectPositions();

		// Hand this step's impulses to the next one, with the points they were
		// applied at so the next step can match them up.
		m_PreviousImpulses.clear();
		for (const Contact& contact : m_Contacts)
		{
			PreviousContact& stored = m_PreviousImpulses[ContactKey(contact.A, contact.B)];
			stored.PointCount = contact.PointCount;

			for (int p = 0; p < contact.PointCount; p++)
			{
				stored.Points[p] = contact.Points[p].Position;
				stored.NormalImpulse[p] = contact.Points[p].NormalImpulse;
				stored.TangentImpulse[p] = contact.Points[p].TangentImpulse;
			}
		}

		UpdateSleeping(dt);
	}

	// Semi-implicit Euler, in two halves so the contact solver can run between
	// them: velocity first, then position from the velocity the solver agreed
	// to. One line different from explicit Euler, and stable at step sizes
	// where explicit Euler gains energy and throws bodies through the floor.
	void PhysicsWorld2D::IntegrateVelocities(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics::IntegrateVelocities");

		m_AwakeBodyCount = 0;

		for (RigidBody2D& body : m_Bodies)
		{
			// Captured here rather than in the position half, so a body that
			// falls asleep mid-step still has a sensible pair to interpolate
			// between.
			body.PreviousPosition = body.Position;
			body.PreviousRotation = body.Rotation;

			if (body.Type == BodyType::Static || !body.Awake)
			{
				// Torque applied to something that cannot spin is discarded
				// rather than accumulating until the body wakes and lurches.
				body.Torque = 0.0f;
				continue;
			}

			m_AwakeBodyCount++;

			body.Velocity += Gravity * body.GravityScale * dt;

			// The same scheme for the angular half, with torque where gravity
			// was. Contacts feed this now: an off-centre impulse from the
			// solver lands in AngularVelocity directly rather than as a
			// torque, since an impulse is already an integral over time.
			body.AngularVelocity += body.Torque * body.InverseInertia * dt;

			// Exponential damping rather than a subtraction, so it cannot push
			// the velocity through zero and reverse the spin at large dt. Also
			// the reason a body given no damping keeps spinning forever, which
			// is correct: there is no air in here.
			if (body.AngularDamping > 0.0f)
				body.AngularVelocity *= 1.0f / (1.0f + body.AngularDamping * dt);

			body.Torque = 0.0f;
		}
	}

	void PhysicsWorld2D::IntegratePositions(float dt)
	{
		EGSS_PROFILE_SCOPE("Physics::IntegratePositions");

		for (RigidBody2D& body : m_Bodies)
		{
			if (body.Type == BodyType::Static || !body.Awake)
				continue;

			body.Position += body.Velocity * dt;
			body.Rotation += body.AngularVelocity * dt;
		}
	}

	void PhysicsWorld2D::ApplyImpulse(unsigned int handle, const glm::vec2& impulse)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody2D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Velocity += impulse * body.InverseMass;
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	void PhysicsWorld2D::ApplyImpulseAt(unsigned int handle, const glm::vec2& impulse,
		const glm::vec2& point)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody2D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Velocity += impulse * body.InverseMass;

		// The 2D cross product: a scalar, because there is only one axis to
		// turn about. An impulse aimed straight at the centre gives r parallel
		// to j, whose cross is zero -- which is exactly why a central hit does
		// not spin anything, and falls out rather than being special-cased.
		glm::vec2 r = point - body.Position;
		body.AngularVelocity += (r.x * impulse.y - r.y * impulse.x) * body.InverseInertia;

		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	void PhysicsWorld2D::ApplyTorque(unsigned int handle, float torque)
	{
		if (handle >= m_Bodies.size())
			return;

		RigidBody2D& body = m_Bodies[handle];
		if (body.Type == BodyType::Static)
			return;

		body.Torque += torque;
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}

	// --- Narrowphase --------------------------------------------------------
	// Each returns true and fills the contact, or returns false. The normal
	// always points from A towards B, so pushing B along it separates them.
	//
	// Every test here is oriented. The box ones work by moving the *query* into
	// the box's own frame rather than the box into the world's -- a rotated box
	// is not an AABB in world space, but a point is still a point and a ray is
	// still a ray in any frame you put them in. Raycast3D established the trick
	// in 3D; this is the same idea two dimensions down.

	// Fills a contact's single point from the summary fields, for the tests
	// that can only ever produce one.
	static void SetSinglePoint(Contact& out)
	{
		out.PointCount = 1;
		out.Points[0].Position = out.Point;
		out.Points[0].Penetration = out.Penetration;
	}

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

		// Halfway into the overlap rather than on A's surface. That keeps the
		// lever arm a full radius long for both bodies, which is what lets
		// friction spin a ball instead of only dragging it.
		out.Point = a.Position + out.Normal * (a.Radius - out.Penetration * 0.5f);

		SetSinglePoint(out);
		return true;
	}

	static bool CollideBoxBox(unsigned int ia, const RigidBody2D& a,
		unsigned int ib, const RigidBody2D& b, Contact& out)
	{
		// The separating-axis test, with its clipped manifold. This replaced a
		// min-overlap AABB test, and the difference is not only that boxes can
		// now be turned: a flat rest used to produce one contact point, and one
		// point cannot hold a crate level.
		Manifold2D manifold = Sat2D::BoxBox(ObbOf(a), ObbOf(b));
		if (!manifold.Touching)
			return false;

		out.A = ia;
		out.B = ib;
		out.Normal = manifold.Normal;
		out.PointCount = manifold.PointCount;

		float deepest = -std::numeric_limits<float>::max();
		for (int i = 0; i < manifold.PointCount; i++)
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

	// A is the circle, B is the box.
	static bool CollideCircleBox(unsigned int ia, const RigidBody2D& a,
		unsigned int ib, const RigidBody2D& b, Contact& out)
	{
		float cosine = std::cos(b.Rotation);
		float sine = std::sin(b.Rotation);

		// Into the box's frame, where it is axis-aligned and the old clamp
		// still works unchanged.
		glm::vec2 local = ToLocal(a.Position - b.Position, cosine, sine);

		glm::vec2 closest = glm::clamp(local, -b.HalfExtents, b.HalfExtents);
		glm::vec2 toCircle = local - closest;
		float distanceSquared = glm::dot(toCircle, toCircle);

		glm::vec2 localNormal;   // A towards B, still in the box's frame
		glm::vec2 localPoint;

		if (distanceSquared > 0.0001f)
		{
			if (distanceSquared >= a.Radius * a.Radius)
				return false;

			float distance = std::sqrt(distanceSquared);
			localNormal = -toCircle / distance;
			localPoint = closest;
			out.Penetration = a.Radius - distance;
		}
		else
		{
			// Centre is inside the box: clamping gave back the centre itself,
			// so there is no direction to work from. Push out through the
			// nearest face instead.
			float overlapX = b.HalfExtents.x - std::abs(local.x);
			float overlapY = b.HalfExtents.y - std::abs(local.y);

			if (overlapX < overlapY)
			{
				localNormal = { local.x < 0.0f ? 1.0f : -1.0f, 0.0f };
				out.Penetration = overlapX + a.Radius;
			}
			else
			{
				localNormal = { 0.0f, local.y < 0.0f ? 1.0f : -1.0f };
				out.Penetration = overlapY + a.Radius;
			}

			localPoint = local;
		}

		out.A = ia;
		out.B = ib;
		out.Normal = ToWorld(localNormal, cosine, sine);
		out.Point = b.Position + ToWorld(localPoint, cosine, sine);

		SetSinglePoint(out);
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
		// warm starting reads. Matched point by point, and by position rather
		// than by index: the clip has no notion of which corner is which, so
		// index 0 this step need not be index 0 last step.
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
					glm::vec2 offset = contact.Points[p].Position - old.Points[q];
					float distanceSquared = glm::dot(offset, offset);

					if (distanceSquared < bestDistance)
					{
						bestDistance = distanceSquared;
						best = q;
					}
				}

				// No match means a genuinely new point, which starts from
				// zero. Guessing an impulse for it would be worse than the
				// extra iteration it costs to find one.
				if (best < 0)
					continue;

				contact.Points[p].NormalImpulse = old.NormalImpulse[best];
				contact.Points[p].TangentImpulse = old.TangentImpulse[best];
			}
		}

		m_Contacts.push_back(contact);
	}

	void PhysicsWorld2D::PrepareContacts()
	{
		for (Contact& contact : m_Contacts)
		{
			RigidBody2D& a = m_Bodies[contact.A];
			RigidBody2D& b = m_Bodies[contact.B];

			float inverseMassA = SolverInverseMass(a);
			float inverseMassB = SolverInverseMass(b);
			float inverseInertiaA = SolverInverseInertia(a);
			float inverseInertiaB = SolverInverseInertia(b);

			glm::vec2 tangent = TangentOf(contact.Normal);
			float restitution = std::min(a.Restitution, b.Restitution);

			for (int p = 0; p < contact.PointCount; p++)
			{
				ContactPoint& point = contact.Points[p];

				point.LeverA = point.Position - a.Position;
				point.LeverB = point.Position - b.Position;

				// The effective mass along an axis: the linear part, plus what
				// each body's resistance to turning contributes. A point far
				// from a centre of mass is *heavier* to push along the normal,
				// because some of the impulse goes into spin instead of
				// travel, and the (r x n)^2 term is exactly how much. With no
				// rotation both angular terms vanish and this collapses back
				// to the sum of inverse masses the old solver divided by.
				float leverNormalA = Cross(point.LeverA, contact.Normal);
				float leverNormalB = Cross(point.LeverB, contact.Normal);

				float normalMass = inverseMassA + inverseMassB
					+ inverseInertiaA * leverNormalA * leverNormalA
					+ inverseInertiaB * leverNormalB * leverNormalB;

				point.NormalMass = normalMass > 0.0f ? 1.0f / normalMass : 0.0f;

				float leverTangentA = Cross(point.LeverA, tangent);
				float leverTangentB = Cross(point.LeverB, tangent);

				float tangentMass = inverseMassA + inverseMassB
					+ inverseInertiaA * leverTangentA * leverTangentA
					+ inverseInertiaB * leverTangentB * leverTangentB;

				point.TangentMass = tangentMass > 0.0f ? 1.0f / tangentMass : 0.0f;

				// Restitution against the approach speed at *this* point. A
				// tumbling box's two corners close at different speeds, and
				// measuring both from the centre bounces the wrong one.
				float approach = glm::dot(PointVelocity(b, point.LeverB)
					- PointVelocity(a, point.LeverA), contact.Normal);

				point.RestitutionBias = approach < -s_RestitutionThreshold
					? -restitution * approach
					: 0.0f;
			}
		}
	}

	void PhysicsWorld2D::WarmStart()
	{
		for (const Contact& contact : m_Contacts)
		{
			RigidBody2D& a = m_Bodies[contact.A];
			RigidBody2D& b = m_Bodies[contact.B];

			glm::vec2 tangent = TangentOf(contact.Normal);

			for (int p = 0; p < contact.PointCount; p++)
			{
				const ContactPoint& point = contact.Points[p];

				glm::vec2 impulse = point.NormalImpulse * contact.Normal
					+ point.TangentImpulse * tangent;

				ApplyContactImpulse(a, b, point, impulse);
			}
		}
	}

	void PhysicsWorld2D::SolveVelocities()
	{
		for (Contact& contact : m_Contacts)
		{
			RigidBody2D& a = m_Bodies[contact.A];
			RigidBody2D& b = m_Bodies[contact.B];

			glm::vec2 tangent = TangentOf(contact.Normal);
			float mu = std::sqrt(a.Friction * b.Friction);

			// Each point solved in turn, and the relative velocity re-read
			// between them: solving one point moves both bodies, so a second
			// point computed against the velocities the first one saw is
			// solving a state that no longer exists.
			for (int p = 0; p < contact.PointCount; p++)
			{
				ContactPoint& point = contact.Points[p];
				if (point.NormalMass <= 0.0f)
					continue;

				// --- Normal ---------------------------------------------------
				glm::vec2 relativeVelocity = PointVelocity(b, point.LeverB)
					- PointVelocity(a, point.LeverA);

				float alongNormal = glm::dot(relativeVelocity, contact.Normal);

				float deltaImpulse = (-alongNormal + point.RestitutionBias) * point.NormalMass;

				// Clamp the *total*, not this iteration's increment: a contact
				// may only ever push, and warm starting means the running
				// total is the thing that has to stay valid.
				float previousImpulse = point.NormalImpulse;
				point.NormalImpulse = std::max(previousImpulse + deltaImpulse, 0.0f);
				deltaImpulse = point.NormalImpulse - previousImpulse;

				ApplyContactImpulse(a, b, point, deltaImpulse * contact.Normal);

				// --- Friction -------------------------------------------------
				relativeVelocity = PointVelocity(b, point.LeverB)
					- PointVelocity(a, point.LeverA);

				float alongTangent = glm::dot(relativeVelocity, tangent);

				float deltaFriction = -alongTangent * point.TangentMass;

				// Coulomb: total friction can never exceed mu times the total
				// normal impulse. Past that the surfaces slide.
				float maxFriction = mu * point.NormalImpulse;
				float previousFriction = point.TangentImpulse;

				point.TangentImpulse = std::max(-maxFriction,
					std::min(previousFriction + deltaFriction, maxFriction));
				deltaFriction = point.TangentImpulse - previousFriction;

				ApplyContactImpulse(a, b, point, deltaFriction * tangent);
			}
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
				float inverseInertiaA = SolverInverseInertia(a);
				float inverseInertiaB = SolverInverseInertia(b);

				if (inverseMassA + inverseMassB <= 0.0f)
					continue;

				Contact current;
				if (!Collide(stale.A, a, stale.B, b, current))
					continue;

				// Per point, and turning the bodies as well as moving them.
				// The angular half is what lets a crate that landed on one
				// corner settle flat: correcting both of its contact points
				// along the normal by different amounts *is* a rotation, and
				// a translation-only solver can only ever average the two and
				// leave the crate tilted.
				for (int p = 0; p < current.PointCount; p++)
				{
					const ContactPoint& point = current.Points[p];

					float depth = std::max(point.Penetration - s_PenetrationSlop, 0.0f);
					if (depth <= 0.0f)
						continue;

					depth = std::min(depth, s_MaxCorrection);

					glm::vec2 leverA = point.Position - a.Position;
					glm::vec2 leverB = point.Position - b.Position;

					float leverNormalA = Cross(leverA, current.Normal);
					float leverNormalB = Cross(leverB, current.Normal);

					// The same effective mass the velocity solver uses. A
					// corner far from the centre of mass is easier to rotate
					// out of an overlap than to push out of one, and using the
					// linear mass alone over-corrects it.
					float normalMass = inverseMassA + inverseMassB
						+ inverseInertiaA * leverNormalA * leverNormalA
						+ inverseInertiaB * leverNormalB * leverNormalB;

					if (normalMass <= 0.0f)
						continue;

					float correction = depth * s_CorrectionPercent / normalMass;

					a.Position -= current.Normal * correction * inverseMassA;
					a.Rotation -= leverNormalA * correction * inverseInertiaA;

					b.Position += current.Normal * correction * inverseMassB;
					b.Rotation += leverNormalB * correction * inverseInertiaB;
				}
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
	//
	// Run in the box's own frame, where the slabs are the coordinate axes. The
	// alternative -- turning the box into a world-space AABB -- cannot tell a
	// plate turned 45 degrees from the square footprint that contains it, and
	// reports hits in the corners it does not occupy.
	static bool RaycastBox(const glm::vec2& origin, const glm::vec2& direction,
		float maxDistance, const RigidBody2D& body, float& outDistance, glm::vec2& outNormal)
	{
		float cosine = std::cos(body.Rotation);
		float sine = std::sin(body.Rotation);

		glm::vec2 localOrigin = ToLocal(origin - body.Position, cosine, sine);
		glm::vec2 localDirection = ToLocal(direction, cosine, sine);

		// A rotation preserves length, so a distance along the local ray is the
		// same distance along the world one -- t needs no conversion back, and
		// only the normal does.
		glm::vec2 boxMin = -body.HalfExtents;
		glm::vec2 boxMax = body.HalfExtents;

		float tMin = 0.0f;
		float tMax = maxDistance;

		int hitAxis = -1;
		float hitSign = 0.0f;

		for (int axis = 0; axis < 2; axis++)
		{
			if (std::abs(localDirection[axis]) < 0.00001f)
			{
				// Parallel to this pair of planes: either always inside the
				// slab or never.
				if (localOrigin[axis] < boxMin[axis] || localOrigin[axis] > boxMax[axis])
					return false;

				continue;
			}

			float inverse = 1.0f / localDirection[axis];
			float t1 = (boxMin[axis] - localOrigin[axis]) * inverse;
			float t2 = (boxMax[axis] - localOrigin[axis]) * inverse;

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
			// Started inside: no face was crossed on the way in. Already a
			// world-space direction, so it needs no rotating.
			outNormal = -direction;
		}
		else
		{
			glm::vec2 localNormal(0.0f);
			localNormal[hitAxis] = hitSign;
			outNormal = ToWorld(localNormal, cosine, sine);
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

			// Spinning counts as moving. Without the angular term a body that
			// had stopped travelling would fall asleep mid-rotation and freeze
			// at whatever angle it happened to be at.
			bool moving = glm::dot(body.Velocity, body.Velocity) > threshold
				|| std::fabs(body.AngularVelocity) > SleepAngularVelocity;

			if (moving)
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
