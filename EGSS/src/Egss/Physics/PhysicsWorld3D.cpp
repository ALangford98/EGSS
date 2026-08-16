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
	// Kinematic bodies are immovable to the solver in exactly the way static
	// ones are: a contact or a joint may not shift them. The difference lives
	// in the integrator, not here.
	static bool Immovable(const RigidBody3D& body)
	{
		return body.Type == BodyType::Static
			|| body.Type == BodyType::Kinematic
			|| !body.Awake;
	}

	static float SolverInverseMass(const RigidBody3D& body)
	{
		return Immovable(body) ? 0.0f : body.InverseMass;
	}

	static glm::mat3 SolverInverseInertia(const RigidBody3D& body)
	{
		return Immovable(body) ? glm::mat3(0.0f) : body.InverseInertiaWorld;
	}

	// Velocity of the material point at `lever`: the body's own velocity plus
	// what its spin adds out there.
	static glm::vec3 PointVelocity(const RigidBody3D& body, const glm::vec3& lever)
	{
		return body.Velocity + glm::cross(body.AngularVelocity, lever);
	}

	// World-space axis-aligned bounds, whatever the shape and whichever way it
	// is facing.
	static void BodyBounds(const RigidBody3D& body, glm::vec3& outMin, glm::vec3& outMax)
	{
		glm::vec3 extent;

		if (body.Shape == ColliderShape3D::Sphere)
		{
			// A sphere looks the same from every direction, which is the one
			// case where orientation genuinely does not matter.
			extent = glm::vec3(body.Radius);
		}
		else if (body.Shape == ColliderShape3D::Capsule)
		{
			// The union of two spheres at the segment's ends, which is exactly
			// the capsule's bounds -- the middle never reaches past them.
			glm::vec3 p, q;
			body.GetSegment(p, q);

			outMin = glm::min(p, q) - glm::vec3(body.Radius);
			outMax = glm::max(p, q) + glm::vec3(body.Radius);
			return;
		}
		else if (body.Shape == ColliderShape3D::Heightfield)
		{
			// The whole map. A demo did then put a few hundred bodies on a
			// large heightfield, and both halves of what that costs are now
			// handled in the broadphase rather than here: the body is kept out
			// of the cells instead of being stamped into all 29,575 of them,
			// and it no longer sizes the grid. See the note on m_Oversized.
			//
			// These bounds still describe the *surface*, the band between
			// Lowest and Highest, while the narrowphase treats everything below
			// the surface as inside the collider and ejects it. That
			// disagreement is why the broadphase cannot use a bounds test to
			// reject terrain pairs, and it is the line to come back to.
			//
			// Confirmed inert rather than assumed so: RebuildGrid always
			// classifies the heightfield as m_Oversized (a whole map's cell
			// span dwarfs any body count), so it is brute-forced against every
			// body regardless of these bounds -- the Y range computed here is
			// never actually read. And even a body found impossibly far below
			// the surface would not be ejected violently if it were read:
			// CorrectPositions clamps per-step correction to s_MaxCorrection
			// regardless of depth, and the velocity side (RestitutionBias)
			// comes from approach velocity, not penetration. So this cannot
			// explode; it is a latent inconsistency, not a live hazard.
			//
			// The fix, whenever a broadphase optimisation wants to trust these
			// bounds: report outMin.y as unbounded downward (a large negative
			// sentinel, not `Lowest`), matching what the narrowphase already
			// treats as solid, rather than trying to give the narrowphase a
			// floor it does not have. The ground is supposed to extend
			// downward forever; the bounds are what is lying about it.
			if (!body.Field || body.Field->Empty())
			{
				outMin = body.Position;
				outMax = body.Position;
				return;
			}

			float half = body.Field->HalfExtent();
			outMin = body.Position + glm::vec3(-half, body.Field->Lowest, -half);
			outMax = body.Position + glm::vec3(half, body.Field->Highest, half);
			return;
		}
		else if (body.Shape == ColliderShape3D::Compound)
		{
			// The union of the children's world boxes. Each child is
			// axis-aligned in the *body's* frame, so a turned compound needs the
			// same corner sweep a turned box does.
			if (!body.Children || body.Children->empty())
			{
				outMin = body.Position;
				outMax = body.Position;
				return;
			}

			outMin = glm::vec3(std::numeric_limits<float>::max());
			outMax = glm::vec3(-std::numeric_limits<float>::max());

			glm::mat3 rotation = glm::mat3_cast(body.Orientation);

			for (const CompoundChild& child : *body.Children)
			{
				// The extent of a rotated box along each world axis is the
				// absolute rotation applied to its half extents -- the same
				// identity `ObbOf` relies on.
				glm::vec3 centre = body.Position + rotation * child.Offset;
				glm::mat3 magnitude(
					glm::abs(rotation[0]), glm::abs(rotation[1]), glm::abs(rotation[2]));
				glm::vec3 reach = magnitude * child.HalfExtents;

				outMin = glm::min(outMin, centre - reach);
				outMax = glm::max(outMax, centre + reach);
			}

			return;
		}
		else if (body.Shape == ColliderShape3D::Sdf)
		{
			// The field's whole box. Unlike the heightfield's, these bounds
			// genuinely do contain the collider -- there is no "solid below the
			// surface" that the bounds fail to describe, because a distance
			// field's inside is inside the box.
			if (!body.Voxels || body.Voxels->Empty())
			{
				outMin = body.Position;
				outMax = body.Position;
				return;
			}

			glm::vec3 span = glm::vec3(body.Voxels->Size() - glm::ivec3(1))
				* body.Voxels->VoxelSize();

			outMin = body.Position + body.Voxels->Origin();
			outMax = outMin + span;
			return;
		}
		else
		{
			// The 3D form of the trap 2D already fell into: a turned box
			// reaches further than its half extents. Along each world axis it
			// reaches the sum of its three half extents projected onto that
			// axis, which is |R| * h -- the rotation matrix with every entry
			// made positive.
			//
			// Bounds that ignore the orientation are too small, so the grid
			// drops pairs the narrowphase would have caught. A missed
			// collision leaves nothing behind to notice: no contact, no
			// warning, just a body passing through a corner it should have
			// clipped.
			glm::mat3 rotation = glm::mat3_cast(body.Orientation);
			glm::mat3 absolute;

			for (int column = 0; column < 3; column++)
				for (int row = 0; row < 3; row++)
					absolute[column][row] = std::fabs(rotation[column][row]);

			extent = absolute * body.HalfExtents;
		}

		outMin = body.Position - extent;
		outMax = body.Position + extent;
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

		// The grid indexes bodies by position in m_Bodies, so it is stale the
		// moment the vector grows.
		MarkGridDirty();

		return (BodyHandle)(m_Bodies.size() - 1);
	}

	void PhysicsWorld3D::Clear()
	{
		m_Bodies.clear();
		m_Contacts.clear();
		m_Joints.clear();
		m_AwakeBodyCount = 0;
		MarkGridDirty();
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

	// Closest point to `point` on the segment pq, and where along it that is.
	static glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& q,
		const glm::vec3& point)
	{
		glm::vec3 along = q - p;
		float lengthSquared = glm::dot(along, along);

		// A zero-length segment is a point, which is the HalfHeight == 0 case
		// -- a capsule that is really a sphere.
		if (lengthSquared < 1e-12f)
			return p;

		float t = glm::clamp(glm::dot(point - p, along) / lengthSquared, 0.0f, 1.0f);
		return p + along * t;
	}

	// Closest pair of points between two segments. The unclamped solution is
	// two lines' closest approach; the clamping is what turns lines into
	// segments, and it has to be done in both parameters and then re-solved,
	// because clamping one moves where the other should be.
	static void ClosestBetweenSegments(const glm::vec3& p1, const glm::vec3& q1,
		const glm::vec3& p2, const glm::vec3& q2, glm::vec3& outA, glm::vec3& outB)
	{
		glm::vec3 d1 = q1 - p1;
		glm::vec3 d2 = q2 - p2;
		glm::vec3 r = p1 - p2;

		float a = glm::dot(d1, d1);
		float e = glm::dot(d2, d2);
		float f = glm::dot(d2, r);

		const float epsilon = 1e-12f;
		float s = 0.0f, t = 0.0f;

		if (a <= epsilon && e <= epsilon)
		{
			outA = p1;
			outB = p2;
			return;
		}

		if (a <= epsilon)
		{
			// First segment is a point.
			t = glm::clamp(f / e, 0.0f, 1.0f);
		}
		else
		{
			float c = glm::dot(d1, r);

			if (e <= epsilon)
			{
				// Second segment is a point.
				s = glm::clamp(-c / a, 0.0f, 1.0f);
			}
			else
			{
				float b = glm::dot(d1, d2);
				float denominator = a * e - b * b;

				// Zero denominator means the segments are parallel, and any s
				// is as good as any other -- pick the start and let the clamp
				// below place t. Parallel is the common case for two capsules
				// lying side by side, so this branch is not exotic.
				s = denominator > epsilon
					? glm::clamp((b * f - c * e) / denominator, 0.0f, 1.0f)
					: 0.0f;

				t = (b * s + f) / e;

				// Clamping t invalidates s, so s is recomputed against the
				// clamped t. Skipping this is the classic bug: it shows up
				// only when one segment's nearest point falls off its end.
				if (t < 0.0f)
				{
					t = 0.0f;
					s = glm::clamp(-c / a, 0.0f, 1.0f);
				}
				else if (t > 1.0f)
				{
					t = 1.0f;
					s = glm::clamp((b - c) / a, 0.0f, 1.0f);
				}
			}
		}

		outA = p1 + d1 * s;
		outB = p2 + d2 * t;
	}

	// Two spheres of the given radii at the given centres, written once so the
	// capsule tests can reduce to it. `a` and `b` name the bodies the contact
	// belongs to, which need not be spheres at all.
	static bool ContactFromSpheres(unsigned int ia, const glm::vec3& centreA, float radiusA,
		unsigned int ib, const glm::vec3& centreB, float radiusB, Contact3D& out)
	{
		glm::vec3 delta = centreB - centreA;
		float radiusSum = radiusA + radiusB;

		float distanceSquared = glm::dot(delta, delta);
		if (distanceSquared >= radiusSum * radiusSum)
			return false;

		float distance = std::sqrt(distanceSquared);

		out.A = ia;
		out.B = ib;
		out.Normal = distance > 0.0001f ? delta / distance : glm::vec3(1.0f, 0.0f, 0.0f);
		out.Penetration = radiusSum - distance;
		out.Point = centreA + out.Normal * (radiusA - out.Penetration * 0.5f);

		out.PointCount = 1;
		out.Points[0].Position = out.Point;
		out.Points[0].Penetration = out.Penetration;
		return true;
	}

	// A is the capsule, B is the sphere. The nearest point on the capsule's
	// segment is the centre of the sphere the capsule looks like from there.
	static bool CollideCapsuleSphere(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		glm::vec3 p, q;
		a.GetSegment(p, q);

		glm::vec3 centre = ClosestOnSegment(p, q, b.Position);
		return ContactFromSpheres(ia, centre, a.Radius, ib, b.Position, b.Radius, out);
	}

	static bool CollideCapsuleCapsule(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		glm::vec3 p1, q1, p2, q2;
		a.GetSegment(p1, q1);
		b.GetSegment(p2, q2);

		glm::vec3 centreA, centreB;
		ClosestBetweenSegments(p1, q1, p2, q2, centreA, centreB);

		// One point, even for two capsules lying exactly alongside each other.
		// That is a real limitation rather than an oversight: a single point
		// cannot resist roll, so two stacked parallel capsules will settle
		// more slowly than two boxes would. Stated in the changelog; the fix
		// is a clipped manifold, which is what CollideCapsuleBox does.
		return ContactFromSpheres(ia, centreA, a.Radius, ib, centreB, b.Radius, out);
	}

	// A is the capsule, B is the box.
	//
	// Everything happens in the box's frame, where it is an AABB -- the same
	// move CollideSphereBox and Raycast3D make. The extra work over the sphere
	// case is the manifold: a capsule lying on a floor touches along a line,
	// and one contact point cannot hold a line level. That is the lesson the
	// box stacking bug already taught, arriving in a different shape.
	static bool CollideCapsuleBox(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		glm::quat inverse = glm::conjugate(b.Orientation);

		glm::vec3 worldP, worldQ;
		a.GetSegment(worldP, worldQ);

		glm::vec3 p = inverse * (worldP - b.Position);
		glm::vec3 q = inverse * (worldQ - b.Position);
		const glm::vec3& half = b.HalfExtents;

		// Closest pair between the segment and the box, by alternating: clamp
		// the current segment point into the box, then find the nearest point
		// on the segment to that. A few rounds is plenty -- it is a descent on
		// a convex problem, and the first step is already close.
		glm::vec3 segPoint = ClosestOnSegment(p, q, glm::clamp((p + q) * 0.5f, -half, half));
		glm::vec3 boxPoint = glm::clamp(segPoint, -half, half);

		for (int i = 0; i < 4; i++)
		{
			segPoint = ClosestOnSegment(p, q, boxPoint);
			boxPoint = glm::clamp(segPoint, -half, half);
		}

		glm::vec3 away = segPoint - boxPoint;      // box towards capsule
		float distanceSquared = glm::dot(away, away);

		glm::vec3 normal;      // box towards capsule, in the box's frame
		float penetration;

		if (distanceSquared > 1e-12f)
		{
			if (distanceSquared >= a.Radius * a.Radius)
				return false;

			float distance = std::sqrt(distanceSquared);
			normal = away / distance;
			penetration = a.Radius - distance;
		}
		else
		{
			// The segment reaches the box's interior, so there is no direction
			// to be had from the closest pair. Out through the nearest face,
			// as the sphere case does.
			int axis = 0;
			float leastOverlap = std::numeric_limits<float>::max();

			for (int i = 0; i < 3; i++)
			{
				float overlap = half[i] - std::fabs(segPoint[i]);
				if (overlap < leastOverlap)
				{
					leastOverlap = overlap;
					axis = i;
				}
			}

			normal = glm::vec3(0.0f);
			normal[axis] = segPoint[axis] < 0.0f ? -1.0f : 1.0f;
			penetration = leastOverlap + a.Radius;
		}

		out.A = ia;
		out.B = ib;
		// Contacts point from A towards B, and `normal` points from the box to
		// the capsule -- so it is negated on the way out.
		out.Normal = b.Orientation * -normal;

		auto toWorld = [&](const glm::vec3& local)
		{
			return b.Position + b.Orientation * local;
		};

		// Is this a face contact with the capsule lying along the face? Only
		// then is there a line to clip, and only then does a second point mean
		// anything.
		int axis = 0;
		for (int i = 1; i < 3; i++)
			if (std::fabs(normal[i]) > std::fabs(normal[axis]))
				axis = i;

		glm::vec3 along = q - p;
		float length = glm::length(along);

		bool faceContact = std::fabs(normal[axis]) > 0.9f;
		bool lyingAlong = length > 1e-6f
			&& std::fabs(glm::dot(along / length, normal)) < 0.3f;

		if (faceContact && lyingAlong)
		{
			// The stretch of the segment that is actually over the face: clip
			// against the box's extent in the two axes that are not the
			// normal's.
			float t0 = 0.0f, t1 = 1.0f;

			for (int i = 0; i < 3 && t0 < t1; i++)
			{
				if (i == axis)
					continue;

				float origin = p[i];
				float direction = along[i];

				if (std::fabs(direction) < 1e-6f)
				{
					// Parallel to this slab: either wholly inside it or wholly
					// out, and wholly out means no overlap to clip to.
					if (std::fabs(origin) > half[i])
					{
						t0 = 1.0f;
						t1 = 0.0f;
					}
					continue;
				}

				float near = (-half[i] - origin) / direction;
				float far = (half[i] - origin) / direction;
				if (near > far)
					std::swap(near, far);

				t0 = std::max(t0, near);
				t1 = std::min(t1, far);
			}

			// A meaningful span, not two points a hair apart -- those would
			// behave worse than one honest point.
			if (t1 - t0 > 0.05f)
			{
				float sign = normal[axis] < 0.0f ? -1.0f : 1.0f;
				int count = 0;

				for (int i = 0; i < 2; i++)
				{
					float t = i == 0 ? t0 : t1;
					glm::vec3 onSegment = p + along * t;

					// How far this end of the segment has sunk past the face,
					// measured along the normal rather than reusing the single
					// closest-pair depth -- a tilted capsule is deeper at one
					// end than the other, and that difference is exactly what
					// levels it.
					float gap = sign * onSegment[axis] - half[axis];
					float depth = a.Radius - gap;

					// Only the ends that are actually touching.
					//
					// Emitting both unconditionally kept a tilted capsule
					// tilted: a contact resists *approach* along its normal, so
					// a point out in mid-air under the raised end held that end
					// up and the capsule rested at about 10 degrees for ever.
					// The manifold has to describe what is touching, not what
					// the shape spans.
					if (depth <= 0.0f)
						continue;

					glm::vec3 onFace = onSegment;
					onFace[axis] = sign * half[axis];

					out.Points[count].Position = toWorld(onFace);
					out.Points[count].Penetration = depth;
					count++;
				}

				if (count > 0)
				{
					out.PointCount = count;
					out.Penetration = out.Points[0].Penetration;
					out.Point = out.Points[0].Position;

					for (int i = 1; i < count; i++)
					{
						if (out.Points[i].Penetration > out.Penetration)
						{
							out.Penetration = out.Points[i].Penetration;
							out.Point = out.Points[i].Position;
						}
					}
					return true;
				}
			}
		}

		// End-on, edge, or corner: one point is the honest answer.
		out.Penetration = penetration;
		out.Point = toWorld(boxPoint);
		out.PointCount = 1;
		out.Points[0].Position = out.Point;
		out.Points[0].Penetration = penetration;
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

	// --- Heightfield --------------------------------------------------------
	//
	// Every shape meets the terrain the same way: reduce it to a handful of
	// places where it might be touching, ask the field about each, then hand
	// the answers to one routine that assembles a manifold.
	//
	// The assembling is where the interesting constraint lives. A `Contact3D`
	// carries **one normal** for all of its points, and the terrain does not --
	// two triangles under one foot generally face different ways. So the
	// deepest point's normal is adopted and every other point's depth is
	// re-measured along it. On terrain this smooth that is a fraction of a
	// degree; on a crease it makes the foot rest on the plane of whichever
	// triangle it is most into, which is the same compromise `CollideBoxBox`
	// makes when it picks a reference face.
	//
	// The alternative is one contact per triangle, and it was not taken: the
	// solver keys warm starting on the *pair*, so several contacts between the
	// same two bodies would each throw the others' impulses away every step.
	struct FieldHit
	{
		glm::vec3 Surface = { 0.0f, 0.0f, 0.0f };   // on the terrain, local
		glm::vec3 Centre = { 0.0f, 0.0f, 0.0f };    // of the query sphere, local
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };    // out of the terrain
		float Radius = 0.0f;
		float Depth = 0.0f;
	};

	// A sphere of `radius` about `centre`, both in the field's frame.
	static bool SphereOnField(const Heightfield3D& field, const glm::vec3& centre,
		float radius, FieldHit& out)
	{
		glm::vec3 closest, normal;
		float distance;

		if (!field.ClosestPoint(centre, radius, closest, normal, distance))
			return false;

		// `distance` is measured along the normal, so it goes negative once the
		// centre is under the surface and this stays the right depth without a
		// second case.
		if (distance >= radius)
			return false;

		out.Surface = closest;
		out.Centre = centre;
		out.Normal = normal;
		out.Radius = radius;
		out.Depth = radius - distance;
		return true;
	}

	// A is the shape, B is the heightfield, and `origin` is B's position --
	// everything in `hits` is in B's frame.
	static bool ContactFromField(unsigned int ia, unsigned int ib,
		const glm::vec3& origin, const FieldHit* hits, int count, Contact3D& out)
	{
		if (count <= 0)
			return false;

		int deepest = 0;
		for (int i = 1; i < count; i++)
			if (hits[i].Depth > hits[deepest].Depth)
				deepest = i;

		glm::vec3 up = hits[deepest].Normal;

		out.A = ia;
		out.B = ib;
		// Contacts point from A towards B. The field's normal points up out of
		// the ground, so the contact normal is its opposite.
		out.Normal = -up;

		int written = 0;
		for (int i = 0; i < count && written < 8; i++)
		{
			// Re-measured along the adopted normal rather than reused. Same
			// lesson as the tilted capsule on a box: a point that is not
			// actually touching still resists approach, so it props the shape
			// up at whatever angle it happened to be found at.
			float depth = hits[i].Radius
				- glm::dot(hits[i].Centre - hits[i].Surface, up);

			if (depth <= 0.0f)
				continue;

			out.Points[written].Position = origin + hits[i].Surface;
			out.Points[written].Penetration = depth;
			written++;
		}

		if (written == 0)
			return false;

		out.PointCount = written;
		out.Penetration = out.Points[0].Penetration;
		out.Point = out.Points[0].Position;

		for (int i = 1; i < written; i++)
		{
			if (out.Points[i].Penetration > out.Penetration)
			{
				out.Penetration = out.Points[i].Penetration;
				out.Point = out.Points[i].Position;
			}
		}

		return true;
	}

	static bool CollideSphereHeightfield(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Field)
			return false;

		FieldHit hit;
		if (!SphereOnField(*b.Field, a.Position - b.Position, a.Radius, hit))
			return false;

		return ContactFromField(ia, ib, b.Position, &hit, 1, out);
	}

	// The segment is sampled rather than solved against the triangles exactly.
	//
	// Spacing is half a cell, which is what makes it safe: the field cannot
	// carry a feature narrower than a cell, so no bump can hide between two
	// samples. An exact segment-triangle closest pair would be the honest
	// version and is a great deal more code for a limb that is a third of a
	// cell long -- but it is the thing to reach for if a demo ever lays
	// something long across coarse terrain.
	static bool CollideCapsuleHeightfield(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Field || b.Field->Empty())
			return false;

		glm::vec3 worldP, worldQ;
		a.GetSegment(worldP, worldQ);

		glm::vec3 p = worldP - b.Position;
		glm::vec3 q = worldQ - b.Position;

		float length = glm::length(q - p);
		float spacing = std::max(b.Field->CellSize() * 0.5f, 0.01f);

		// HalfHeight 0 is a capsule that is really a sphere; two samples at the
		// same place would be two identical contact points, which the solver
		// would treat as twice the support.
		int samples = length < 1e-6f
			? 1
			: glm::clamp((int)std::ceil(length / spacing) + 1, 2, 8);

		FieldHit hits[8];
		int count = 0;

		for (int i = 0; i < samples; i++)
		{
			float t = samples > 1 ? (float)i / (float)(samples - 1) : 0.0f;

			FieldHit hit;
			if (SphereOnField(*b.Field, p + (q - p) * t, a.Radius, hit))
				hits[count++] = hit;
		}

		return ContactFromField(ia, ib, b.Position, hits, count, out);
	}

	// Corners against the column of terrain each one stands over.
	//
	// A box is the one shape here with no radius to work with, so the closest
	// point on a triangle is the wrong question -- it would answer with a
	// direction only once the corner is already through the surface. The right
	// question for a heightfield is the vertical one: what is under this
	// corner, and is the corner below it.
	//
	// The limitation is a box wider than a cell straddling a peak: every corner
	// can be above ground while the peak pushes through the underside, and
	// nothing here sees it. Nothing in this project is in that position -- a
	// foot is 0.22 m across a 0.5 m grid -- and the fix is to add the cell
	// corners under the box as query points too.
	static bool CollideBoxHeightfield(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Field || b.Field->Empty())
			return false;

		glm::vec3 corners[8];
		ObbOf(a).Corners(corners);

		FieldHit hits[8];
		int count = 0;

		for (int i = 0; i < 8; i++)
		{
			glm::vec3 local = corners[i] - b.Position;

			float height;
			glm::vec3 normal;
			if (!b.Field->SurfaceAt(local.x, local.z, height, normal))
				continue;

			if (local.y >= height)
				continue;

			FieldHit& hit = hits[count++];
			hit.Surface = { local.x, height, local.z };
			hit.Centre = local;
			hit.Normal = normal;
			hit.Radius = 0.0f;
			// The vertical drop turned into a perpendicular one. The factor is
			// the normal's own y, which is the cosine of the slope -- on the
			// flat it is 1 and the two measures agree.
			hit.Depth = (height - local.y) * normal.y;
		}

		return ContactFromField(ia, ib, b.Position, hits, count, out);
	}

	// --- Signed distance fields ---------------------------------------------
	//
	// The same three tests as the heightfield above, and all three are shorter,
	// because a distance field answers directly what the heightfield has to
	// search for. `SampleDistance` *is* how deep a point is and `SampleNormal`
	// *is* which way the surface faces, so there is no closest-point search, no
	// vertical special case, and no assumption that the ground is single-valued
	// in y -- which is the whole reason for having it.
	//
	// `FieldHit` and `ContactFromField` are reused unchanged. A hit is a point,
	// a surface, a normal and a depth whichever kind of field produced it, and
	// the manifold-assembly problem -- one normal per contact, depths
	// re-measured along it -- is identical.

	// A sphere of `radius` about `centre`, both in the field's frame.
	static bool SphereOnSdf(const VoxelField3D& field, const glm::vec3& centre,
		float radius, FieldHit& out)
	{
		float distance = field.SampleDistance(centre);

		// Negative once the centre is inside, so this is the same one-line test
		// the heightfield version makes and needs no second case for "under".
		if (distance >= radius)
			return false;

		glm::vec3 normal = field.SampleNormal(centre);

		out.Surface = centre - normal * distance;
		out.Centre = centre;
		out.Normal = normal;
		out.Radius = radius;
		out.Depth = radius - distance;
		return true;
	}

	static bool CollideSphereSdf(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Voxels || b.Voxels->Empty())
			return false;

		FieldHit hit;
		if (!SphereOnSdf(*b.Voxels, a.Position - b.Position, a.Radius, hit))
			return false;

		return ContactFromField(ia, ib, b.Position, &hit, 1, out);
	}

	// Sampled along the segment, like the heightfield capsule. Spacing is half a
	// voxel for the same reason it is half a cell there: the field cannot carry
	// a feature narrower than a voxel, so nothing can hide between two samples.
	static bool CollideCapsuleSdf(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Voxels || b.Voxels->Empty())
			return false;

		glm::vec3 worldP, worldQ;
		a.GetSegment(worldP, worldQ);

		glm::vec3 p = worldP - b.Position;
		glm::vec3 q = worldQ - b.Position;

		float length = glm::length(q - p);
		float spacing = glm::max(b.Voxels->VoxelSize() * 0.5f, 0.01f);

		int samples = length < 1e-6f
			? 1
			: glm::clamp((int)std::ceil(length / spacing) + 1, 2, 8);

		FieldHit hits[8];
		int count = 0;

		for (int i = 0; i < samples; i++)
		{
			float t = samples > 1 ? (float)i / (float)(samples - 1) : 0.0f;

			FieldHit hit;
			if (SphereOnSdf(*b.Voxels, p + (q - p) * t, a.Radius, hit))
				hits[count++] = hit;
		}

		return ContactFromField(ia, ib, b.Position, hits, count, out);
	}

	// Corners against the field, and unlike the heightfield version this asks
	// the honest question: a corner has no radius, so it is touching exactly
	// when its distance is negative, in whatever direction the surface happens
	// to face. The heightfield has to ask a *vertical* question instead, and
	// pays for it with a hole -- a box wider than a cell straddling a peak. This
	// has no such case, because a distance is not measured along an axis.
	//
	// What it does share is the limitation that only corners are queried: a face
	// resting across a spike narrower than the box still sees nothing. The fix
	// is the same one the heightfield's comment names -- add the field's own
	// lattice points under the box as query points.
	static bool CollideBoxSdf(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!b.Voxels || b.Voxels->Empty())
			return false;

		glm::vec3 corners[8];
		ObbOf(a).Corners(corners);

		FieldHit hits[8];
		int count = 0;

		for (int i = 0; i < 8; i++)
		{
			glm::vec3 local = corners[i] - b.Position;

			// Radius zero, so `SphereOnSdf` reduces to "is this point inside",
			// which is what a corner is asking.
			FieldHit hit;
			if (SphereOnSdf(*b.Voxels, local, 0.0f, hit))
				hits[count++] = hit;
		}

		return ContactFromField(ia, ib, b.Position, hits, count, out);
	}

	// --- Compounds ----------------------------------------------------------
	//
	// A compound is tested by turning each of its children into a box body in
	// world space and running the box tests that already exist. Nothing new is
	// written about how a box meets a sphere or a distance field, which is the
	// point: those took a long time to get right and there is one copy of each.
	//
	// The children are what merging costs. A `Contact3D` carries **one normal**
	// -- the solver's warm starting is keyed on the body pair, so several
	// contacts between the same two bodies would discard each other's impulses
	// every step -- so the child manifolds have to become one. The deepest
	// point's normal is adopted and every other point re-measured along it,
	// which is exactly what `ContactFromField` does for terrain triangles and
	// for the same reason. Debris resting on flat ground has one normal anyway;
	// a lump wedged in a corner gets the plane of whichever child is deepest.

	// A child as a body of its own, in world space.
	static RigidBody3D ChildAsBox(const RigidBody3D& body, const CompoundChild& child)
	{
		RigidBody3D box = body;
		box.Shape = ColliderShape3D::Box;
		box.HalfExtents = child.HalfExtents;
		box.Position = body.Position + body.Orientation * child.Offset;
		box.Children.reset();
		return box;
	}

	// Merges the points of `from` into `into`, along `into`'s normal.
	static void MergeContact(Contact3D& into, const Contact3D& from, bool first)
	{
		if (first)
		{
			into = from;
			return;
		}

		// Deeper wins the normal, for the whole manifold.
		if (from.Penetration > into.Penetration)
		{
			glm::vec3 keptNormal = from.Normal;

			// Everything already collected, re-measured along the new normal.
			// A point that was deep against one child's face is shallower
			// against another's, and using its old depth would prop the body up
			// at an angle nothing is actually touching at.
			float scale = glm::dot(into.Normal, keptNormal);
			for (int i = 0; i < into.PointCount; i++)
				into.Points[i].Penetration *= glm::max(scale, 0.0f);

			into.Normal = keptNormal;
			into.Penetration = from.Penetration;
			into.Point = from.Point;
		}

		for (int i = 0; i < from.PointCount && into.PointCount < 8; i++)
		{
			float depth = from.Points[i].Penetration
				* glm::max(glm::dot(from.Normal, into.Normal), 0.0f);

			if (depth <= 0.0f)
				continue;

			into.Points[into.PointCount].Position = from.Points[i].Position;
			into.Points[into.PointCount].Penetration = depth;
			into.PointCount++;
		}
	}

	static bool Collide(unsigned int i, const RigidBody3D& a, unsigned int j,
		const RigidBody3D& b, Contact3D& out);

	// `a` is the compound. Every child against `b`, merged.
	static bool CollideCompound(unsigned int ia, const RigidBody3D& a,
		unsigned int ib, const RigidBody3D& b, Contact3D& out)
	{
		if (!a.Children || a.Children->empty())
			return false;

		bool any = false;

		for (const CompoundChild& child : *a.Children)
		{
			RigidBody3D box = ChildAsBox(a, child);

			Contact3D contact;
			if (!Collide(ia, box, ib, b, contact))
				continue;

			MergeContact(out, contact, !any);
			any = true;
		}

		if (any)
		{
			out.A = ia;
			out.B = ib;
		}

		return any;
	}

	static bool Collide(unsigned int i, const RigidBody3D& a, unsigned int j,
		const RigidBody3D& b, Contact3D& out)
	{
		const ColliderShape3D sphere = ColliderShape3D::Sphere;
		const ColliderShape3D box = ColliderShape3D::Box;
		const ColliderShape3D capsule = ColliderShape3D::Capsule;
		const ColliderShape3D field = ColliderShape3D::Heightfield;
		const ColliderShape3D sdf = ColliderShape3D::Sdf;
		const ColliderShape3D compound = ColliderShape3D::Compound;

		// Compounds first, and recursively: a compound against a compound
		// expands the first into children, and each of those lands here again
		// against the second, which expands in turn. Two levels, then ordinary
		// box tests -- no special case for the pair.
		if (a.Shape == compound)
			return CollideCompound(i, a, j, b, out);

		if (b.Shape == compound)
		{
			// Canonical order, then flipped, the same way every other mixed pair
			// is handled below.
			if (!CollideCompound(j, b, i, a, out))
				return false;

			out.Normal = -out.Normal;
			std::swap(out.A, out.B);
			return true;
		}

		// Two fields of any kind can only ever be two static bodies, which
		// TestPair has already dropped. Named anyway so the dispatch is total.
		bool aIsField = a.Shape == field || a.Shape == sdf;
		bool bIsField = b.Shape == field || b.Shape == sdf;

		if (aIsField && bIsField)
			return false;

		// Like-with-like first.
		if (a.Shape == sphere && b.Shape == sphere)
			return CollideSphereSphere(i, a, j, b, out);

		if (a.Shape == box && b.Shape == box)
			return CollideBoxBox(i, a, j, b, out);

		if (a.Shape == capsule && b.Shape == capsule)
			return CollideCapsuleCapsule(i, a, j, b, out);

		// Then the mixed pairs in their canonical order.
		if (a.Shape == sphere && b.Shape == box)
			return CollideSphereBox(i, a, j, b, out);

		if (a.Shape == capsule && b.Shape == sphere)
			return CollideCapsuleSphere(i, a, j, b, out);

		if (a.Shape == capsule && b.Shape == box)
			return CollideCapsuleBox(i, a, j, b, out);

		if (a.Shape == sphere && b.Shape == field)
			return CollideSphereHeightfield(i, a, j, b, out);

		if (a.Shape == capsule && b.Shape == field)
			return CollideCapsuleHeightfield(i, a, j, b, out);

		if (a.Shape == box && b.Shape == field)
			return CollideBoxHeightfield(i, a, j, b, out);

		if (a.Shape == sphere && b.Shape == sdf)
			return CollideSphereSdf(i, a, j, b, out);

		if (a.Shape == capsule && b.Shape == sdf)
			return CollideCapsuleSdf(i, a, j, b, out);

		if (a.Shape == box && b.Shape == sdf)
			return CollideBoxSdf(i, a, j, b, out);

		// Everything else is one of those with the bodies the other way round.
		// Run the canonical test and flip the normal, rather than writing each
		// test twice -- the swapped versions were where the 2D narrowphase
		// grew its bugs.
		bool hit = false;

		if (a.Shape == box && b.Shape == sphere)
			hit = CollideSphereBox(j, b, i, a, out);
		else if (a.Shape == sphere && b.Shape == capsule)
			hit = CollideCapsuleSphere(j, b, i, a, out);
		else if (a.Shape == box && b.Shape == capsule)
			hit = CollideCapsuleBox(j, b, i, a, out);
		else if (a.Shape == field && b.Shape == sphere)
			hit = CollideSphereHeightfield(j, b, i, a, out);
		else if (a.Shape == field && b.Shape == capsule)
			hit = CollideCapsuleHeightfield(j, b, i, a, out);
		else if (a.Shape == field && b.Shape == box)
			hit = CollideBoxHeightfield(j, b, i, a, out);
		else if (a.Shape == sdf && b.Shape == sphere)
			hit = CollideSphereSdf(j, b, i, a, out);
		else if (a.Shape == sdf && b.Shape == capsule)
			hit = CollideCapsuleSdf(j, b, i, a, out);
		else if (a.Shape == sdf && b.Shape == box)
			hit = CollideBoxSdf(j, b, i, a, out);

		if (!hit)
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

		// Against the positions the bodies ended the last step at, which is
		// also where the solve will run. Velocities have changed by now but
		// nothing has moved yet, so the bounds are still the ones the grid was
		// last built from -- rebuilding here rather than after IntegratePositions
		// is what keeps the grid describing the geometry being tested.
		MarkGridDirty();
		GenerateContacts();
		PrepareContacts();
		PrepareJoints(dt);
		WarmStart();

		{
			EGSS_PROFILE_SCOPE("Physics3D::Solve");

			// Joints and contacts share the iteration loop rather than each
			// getting their own pass. They are coupled -- a jointed chain
			// resting on the floor is both at once -- and solving all of one
			// then all of the other lets each undo the other's work. Joints
			// go first because a contact correcting a limb that is about to be
			// pulled somewhere else is wasted.
			for (unsigned int i = 0; i < VelocityIterations; i++)
			{
				SolveJoints();
				SolveVelocities();
			}
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

	// Buckets every body into the cells its bounds overlap. Rebuilt whenever
	// anything has moved, which for a simulated world is every step -- that
	// rebuild is the price the pair search pays for being cheap.
	void PhysicsWorld3D::RebuildGrid()
	{
		m_GridDirty = false;
		m_Cells.clear();
		m_Oversized.clear();
		m_OutsideCells.assign(m_Bodies.size(), 0);
		m_GridWidth = 0;
		m_GridHeight = 0;
		m_GridDepth = 0;

		if (m_Bodies.empty())
			return;

		m_GridCellSize = std::max(CellSize, 0.01f);
		m_Bounds.resize(m_Bodies.size());

		// Bounds and classification first, because the extent below is built
		// from the bodies that are actually going into cells.
		//
		// The cell span is estimated from the body's own size rather than read
		// off a CellRange, which is what makes that ordering possible: how many
		// cells a body covers depends on where the grid's origin falls only to
		// within one cell per axis, so its size answers the question before the
		// grid exists. An upper bound, deliberately -- being one cell out either
		// way cannot matter to a threshold that terrain clears by two orders of
		// magnitude.
		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			BodyBounds(m_Bodies[i], m_Bounds[i].Min, m_Bounds[i].Max);

			glm::vec3 size = m_Bounds[i].Max - m_Bounds[i].Min;
			double spanned =
				(double)((int)(size.x / m_GridCellSize) + 2) *
				(double)((int)(size.y / m_GridCellSize) + 2) *
				(double)((int)(size.z / m_GridCellSize) + 2);

			// Past this size the bucketing costs more than the pair tests it
			// saves, and the body is tested directly instead -- see the note on
			// m_Oversized. A heightfield is always on this side of the line.
			if (BroadphaseExcludeOversized && spanned > (double)m_Bodies.size())
			{
				m_Oversized.push_back(i);
				m_OutsideCells[i] = 1;
			}
		}

		glm::vec3 worldMin(std::numeric_limits<float>::max());
		glm::vec3 worldMax(-std::numeric_limits<float>::max());
		bool any = false;

		// **Only the bodies being bucketed set the extent.** A heightfield left
		// in here sizes the grid from the map rather than from what is standing
		// on it -- 29,575 cells to hold the few hundred that are occupied, on
		// the 64 m map -- and an empty cell is not free, because the array is
		// rebuilt every step. That was worth 0.256 ms against brute force's
		// 0.208 ms at 217 bodies: still losing, with the terrain already out of
		// the cells. Sizing the grid to the bodies in it took the same scene to
		// 0.056 ms.
		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			if (m_OutsideCells[i])
				continue;

			worldMin = glm::min(worldMin, m_Bounds[i].Min);
			worldMax = glm::max(worldMax, m_Bounds[i].Max);
			any = true;
		}

		// Nothing left to bucket -- a world of nothing but terrain. The grid
		// stays unbuilt and every row falls through to brute force, which for
		// one body is the right answer anyway.
		if (!any)
			return;

		m_GridOrigin = worldMin;

		glm::vec3 span = worldMax - worldMin;
		m_GridWidth = std::max(1, (int)(span.x / m_GridCellSize) + 1);
		m_GridHeight = std::max(1, (int)(span.y / m_GridCellSize) + 1);
		m_GridDepth = std::max(1, (int)(span.z / m_GridCellSize) + 1);

		// A pathological cell size would otherwise allocate unboundedly, and
		// in 3D that arrives far sooner than in 2D: halving the cell size
		// costs eight times the cells rather than four. Smaller cap than the
		// 2D world's for the same reason.
		const long long maxCells = 1 << 18;
		if ((long long)m_GridWidth * m_GridHeight * m_GridDepth > maxCells)
		{
			m_GridWidth = 0;
			m_GridHeight = 0;
			m_GridDepth = 0;
			return;
		}

		m_Cells.assign((size_t)m_GridWidth * m_GridHeight * m_GridDepth, {});

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			if (m_OutsideCells[i])
				continue;

			int x0, y0, z0, x1, y1, z1;
			CellRange(m_Bounds[i].Min, m_Bounds[i].Max, x0, y0, z0, x1, y1, z1);

			// A body large relative to the cell lands in several -- the floor
			// in the demo spans most of the grid -- which is what makes the
			// per-query stamp necessary.
			for (int z = z0; z <= z1; z++)
				for (int y = y0; y <= y1; y++)
					for (int x = x0; x <= x1; x++)
						m_Cells[CellIndex(x, y, z)].push_back(i);
		}

		m_QueryStamp.assign(m_Bodies.size(), 0);
		m_QueryCounter = 0;
	}

	void PhysicsWorld3D::GenerateContacts()
	{
		EGSS_PROFILE_SCOPE("Physics3D::Broadphase+Narrowphase");

		m_Contacts.clear();
		m_Candidates = 0;

		// Small worlds skip the grid entirely -- not just its use, but its
		// rebuild, which is most of what it costs.
		bool wantGrid = UseBroadphase && m_Bodies.size() >= BroadphaseMinBodies;

		if (wantGrid && m_GridDirty)
			RebuildGrid();

		// Falls back to brute force if the grid could not be built -- an empty
		// world, or a cell size that would have needed too many cells. The
		// answer is the same either way; only the cost differs.
		bool useGrid = wantGrid && m_GridWidth > 0;

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			// A body the grid is not holding has no cells to walk, so its own
			// row is brute force -- which is also the cheapest thing it could
			// be, since walking the cells it covers is exactly the cost it was
			// taken out of the grid to avoid.
			bool viaCells = useGrid && !m_OutsideCells[i];

			if (viaCells)
			{
				// Only bodies sharing a cell with this one can touch it.
				m_QueryCounter++;
				m_Neighbours.clear();

				// The bounds the rebuild classified and bucketed this body on,
				// rather than a second BodyBounds. They have to be the same
				// bounds or a body is looked for in cells it was not put in,
				// and nothing has moved since -- the rebuild runs from here.
				int x0, y0, z0, x1, y1, z1;
				CellRange(m_Bounds[i].Min, m_Bounds[i].Max, x0, y0, z0, x1, y1, z1);

				for (int z = z0; z <= z1; z++)
				{
					for (int y = y0; y <= y1; y++)
					{
						for (int x = x0; x <= x1; x++)
						{
							for (unsigned int j : m_Cells[CellIndex(x, y, z)])
							{
								// j > i keeps each pair once; the stamp keeps
								// a pair sharing several cells from being
								// collected twice.
								if (j <= i)
									continue;
								if (m_QueryStamp[j] == m_QueryCounter)
									continue;
								m_QueryStamp[j] = m_QueryCounter;

								m_Neighbours.push_back(j);
							}
						}
					}
				}

				// The bodies the grid is not holding. No cell walk can turn one
				// up, and the terrain has to be a candidate for everything
				// standing on it in any case -- which is what being in every
				// cell was achieving, at the price of being put in every cell.
				for (unsigned int k : m_Oversized)
				{
					if (k <= i)
						continue;

					m_QueryStamp[k] = m_QueryCounter;
					m_Neighbours.push_back(k);
				}

				// Sorted so the pairs are tested in ascending j -- exactly the
				// order brute force would have produced, restricted to the
				// candidates. Cell iteration order is deterministic but it is
				// not *that* order, and contacts are solved by sequential
				// impulses, which are order-dependent: without this the grid
				// and brute force reach slightly different, both-valid answers
				// and the toggle stops being a pure optimisation. It is worth
				// far more as an A/B that provably changes nothing.
				std::sort(m_Neighbours.begin(), m_Neighbours.end());

				for (unsigned int j : m_Neighbours)
				{
					m_Candidates++;
					TestPair(i, j);
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

	void PhysicsWorld3D::TestPair(unsigned int i, unsigned int j)
	{
		const RigidBody3D& a = m_Bodies[i];
		const RigidBody3D& b = m_Bodies[j];

		// Two immovable bodies can never resolve anything. Asked of the solver
		// rather than of InverseMass, so a kinematic body -- which keeps a real
		// mass but is immovable to contacts -- counts.
		if (SolverInverseMass(a) == 0.0f && SolverInverseMass(b) == 0.0f)
			return;

		// An upper and lower arm overlap at the elbow permanently. Without
		// this the contact and the joint push against each other every step
		// and the limb buzzes.
		if (JointSuppressesContact(i, j))
			return;

		// A body carried by another has to stop colliding with its carrier.
		// The general case is the same one as the elbow: two bodies that are
		// *meant* to occupy the same space, where a contact is not information
		// about the world but an artefact of how they are attached.
		//
		// Without it, a kinematic object held inside a dynamic one shoves it
		// without being shoved back -- measured at 80.7 m in five seconds for
		// a tool held inside a walking character.
		if (a.IgnoreCollisionWith == (int)j || b.IgnoreCollisionWith == (int)i)
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

	// The matrix that turns a vector into a cross product: skew(r) * v == r x v.
	// Needed because the joint's effective mass has to be assembled as a
	// matrix, and `r x (I^-1 (r x P))` only becomes one when the crosses are
	// written this way.
	static glm::mat3 Skew(const glm::vec3& r)
	{
		// glm is column-major: the first index is the column.
		glm::mat3 m(0.0f);
		m[1][0] = -r.z;  m[2][0] =  r.y;
		m[0][1] =  r.z;  m[2][1] = -r.x;
		m[0][2] = -r.y;  m[1][2] =  r.x;
		return m;
	}

	PhysicsWorld3D::JointHandle PhysicsWorld3D::AddBallJoint(BodyHandle a, BodyHandle b,
		const glm::vec3& worldAnchor)
	{
		Joint3D joint;
		joint.A = a;
		joint.B = b;

		// Into each body's frame, which is what freezes the current relative
		// placement as the one the joint will hold.
		const RigidBody3D& bodyA = m_Bodies[a];
		const RigidBody3D& bodyB = m_Bodies[b];

		joint.LocalAnchorA = glm::conjugate(bodyA.Orientation) * (worldAnchor - bodyA.Position);
		joint.LocalAnchorB = glm::conjugate(bodyB.Orientation) * (worldAnchor - bodyB.Position);

		m_Joints.push_back(joint);
		return (JointHandle)(m_Joints.size() - 1);
	}

	PhysicsWorld3D::JointHandle PhysicsWorld3D::AddHingeJoint(BodyHandle a, BodyHandle b,
		const glm::vec3& worldAnchor, const glm::vec3& worldAxis)
	{
		JointHandle handle = AddBallJoint(a, b, worldAnchor);

		Joint3D& joint = m_Joints[handle];
		joint.Type = JointType3D::Hinge;

		const RigidBody3D& bodyA = m_Bodies[a];
		const RigidBody3D& bodyB = m_Bodies[b];

		glm::vec3 axis = glm::normalize(worldAxis);
		joint.LocalAxisA = glm::conjugate(bodyA.Orientation) * axis;
		joint.LocalAxisB = glm::conjugate(bodyB.Orientation) * axis;

		// A reference direction perpendicular to the axis, stored in both
		// frames. Because the *same* world direction goes into both, the angle
		// measured between them reads zero right now -- so a limit of
		// (-2.4, 0) means "from here, bend one way only".
		glm::vec3 reference, unused;
		BuildTangents(axis, reference, unused);

		joint.LocalRefA = glm::conjugate(bodyA.Orientation) * reference;
		joint.LocalRefB = glm::conjugate(bodyB.Orientation) * reference;

		return handle;
	}

	void PhysicsWorld3D::SetConeTwistLimits(JointHandle handle, const glm::vec3& worldTwistAxis,
		float coneAngle, float twistLower, float twistUpper)
	{
		Joint3D& joint = m_Joints[handle];
		const RigidBody3D& a = m_Bodies[joint.A];
		const RigidBody3D& b = m_Bodies[joint.B];

		glm::vec3 axis = glm::normalize(worldTwistAxis);

		// The same world direction into both frames, so the swing angle reads
		// zero in the pose this was called in and the cone opens around where
		// the bone currently points.
		joint.LocalAxisA = glm::conjugate(a.Orientation) * axis;
		joint.LocalAxisB = glm::conjugate(b.Orientation) * axis;

		// The pose the limits are measured from. Without this a bone built at
		// an angle to its parent reads that angle as swing and starts outside
		// its own cone -- which is invisible on a test rig whose bodies all
		// start aligned, and wrong on every real skeleton.
		joint.ConeTwistRest = glm::normalize(glm::conjugate(a.Orientation) * b.Orientation);

		joint.ConeTwistEnabled = true;
		joint.ConeAngle = std::max(coneAngle, 0.0f);
		joint.TwistLower = std::min(twistLower, twistUpper);
		joint.TwistUpper = std::max(twistLower, twistUpper);
	}

	void PhysicsWorld3D::SetJointMotor(JointHandle handle, float stiffness, float maxTorque)
	{
		Joint3D& joint = m_Joints[handle];
		const RigidBody3D& a = m_Bodies[joint.A];
		const RigidBody3D& b = m_Bodies[joint.B];

		joint.MotorEnabled = true;
		joint.MotorStiffness = std::max(stiffness, 0.0f);
		joint.MotorMaxTorque = std::max(maxTorque, 0.0f);

		// Hold the current pose until told otherwise, which is nearly always
		// what is wanted on a rig that was just assembled in its rest pose.
		joint.MotorTargetRotation = glm::normalize(glm::conjugate(a.Orientation) * b.Orientation);
		joint.MotorTargetAngle = joint.Angle;
	}

	void PhysicsWorld3D::SetHingeLimits(JointHandle handle, float lower, float upper)
	{
		Joint3D& joint = m_Joints[handle];
		joint.LimitEnabled = true;
		joint.LowerLimit = std::min(lower, upper);
		joint.UpperLimit = std::max(lower, upper);
	}

	bool PhysicsWorld3D::GroundBelow(const glm::vec3& point,
		float& outHeight, glm::vec3& outNormal, BodyHandle ignore, float floor) const
	{
		float best = floor;
		glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
		bool found = false;

		for (unsigned int i = 0; i < m_Bodies.size(); i++)
		{
			if (i == ignore)
				continue;

			const RigidBody3D& body = m_Bodies[i];

			// Only things that hold still are ground. A crate the character
			// happens to be standing on is a fair question and a different
			// one; answering it here would make the stand height chase every
			// loose object that wandered underfoot.
			if (body.Type == BodyType::Dynamic)
				continue;

			// Terrain is asked directly rather than through its bounds, which
			// would report the highest point on the map from anywhere on it.
			// This is the one collider whose ground answer is *exact* -- a
			// heightfield is a height as a function of x and z, which is
			// precisely the question being asked.
			if (body.Shape == ColliderShape3D::Heightfield)
			{
				if (!body.Field)
					continue;

				float local = 0.0f;
				glm::vec3 face(0.0f, 1.0f, 0.0f);
				if (!body.Field->SurfaceAt(point.x - body.Position.x,
					point.z - body.Position.z, local, face))
					continue;

				float surface = body.Position.y + local;
				if (surface <= point.y && surface > best)
				{
					best = surface;
					bestNormal = body.Field->SmoothNormalAt(
						point.x - body.Position.x, point.z - body.Position.z);
					found = true;
				}

				continue;
			}

			// A distance field is asked by marching down it. There is no
			// closed-form "height at (x, z)" here and there cannot be -- that
			// question only has one answer when the ground is single-valued,
			// which is exactly the restriction this collider exists to lift. So
			// the honest answer is the *first* surface at or below the point,
			// which is what somebody standing under an arch is standing on.
			if (body.Shape == ColliderShape3D::Sdf)
			{
				if (!body.Voxels || body.Voxels->Empty())
					continue;

				glm::vec3 local = point - body.Position;

				glm::vec3 low, high;
				BodyBounds(body, low, high);
				if (point.x < low.x || point.x > high.x || point.z < low.z || point.z > high.z)
					continue;

				// Sphere tracing: the field says how far it is safe to step, so
				// stepping by it can never pass through a surface. That is the
				// same Lipschitz property the sparse storage leans on, and it is
				// what makes this terminate rather than creep down by a fixed
				// increment chosen by guesswork.
				const float voxel = body.Voxels->VoxelSize();
				const float floorLocal = body.Voxels->Origin().y;

				// Capped for the same reason the raycast's step is: an
				// unallocated chunk reads `Far`, which is a sentinel and not a
				// distance, and a single step of it leaves the field.
				const float maximumStep = (float)VoxelField3D::ChunkSize * voxel;

				float y = local.y;
				float distance = body.Voxels->SampleDistance({ local.x, y, local.z });

				// Already underground: the surface is at the point itself.
				float crossing = y;
				bool hit = distance <= 0.0f;

				for (int step = 0; !hit && step < 512 && y > floorLocal; step++)
				{
					// Never less than a fraction of a voxel, or a ray running
					// parallel to a wall inches down forever.
					y -= glm::clamp(distance, voxel * 0.25f, maximumStep);

					float next = body.Voxels->SampleDistance({ local.x, y, local.z });
					if (next <= 0.0f)
					{
						// Bisect the last interval. Ten halvings of a voxel is
						// well under a millimetre, and the alternative -- taking
						// the step's end -- reports the ground up to a whole
						// step too low.
						float above = y + glm::clamp(distance, voxel * 0.25f, maximumStep);
						float below = y;

						for (int i = 0; i < 10; i++)
						{
							float middle = (above + below) * 0.5f;
							if (body.Voxels->SampleDistance({ local.x, middle, local.z }) <= 0.0f)
								below = middle;
							else
								above = middle;
						}

						crossing = above;
						hit = true;
						break;
					}

					distance = next;
				}

				if (!hit)
					continue;

				float surface = body.Position.y + crossing;
				if (surface <= point.y && surface > best)
				{
					best = surface;
					bestNormal = body.Voxels->SampleNormal({ local.x, crossing, local.z });
					found = true;
				}

				continue;
			}

			glm::vec3 low, high;
			BodyBounds(body, low, high);

			if (point.x < low.x || point.x > high.x || point.z < low.z || point.z > high.z)
				continue;

			// Below the point, and the highest such surface wins -- a step on
			// a floor should report the step.
			if (high.y <= point.y && high.y > best)
			{
				best = high.y;
				// The bounds' top face, which is the surface this probe
				// reported. Level whatever the body is, because that is what an
				// axis-aligned box's lid is.
				bestNormal = glm::vec3(0.0f, 1.0f, 0.0f);
				found = true;
			}
		}

		if (!found)
			return false;

		outHeight = best;
		outNormal = bestNormal;
		return true;
	}

	// Kept as its own entry point because most callers only want the height,
	// and because its "nothing under you" answer is the floor rather than a
	// failure -- which is the behaviour a stand height wants and a foot
	// orientation does not.
	float PhysicsWorld3D::GroundHeightBelow(const glm::vec3& point,
		BodyHandle ignore, float floor) const
	{
		float height = floor;
		glm::vec3 normal(0.0f, 1.0f, 0.0f);

		return GroundBelow(point, height, normal, ignore, floor) ? height : floor;
	}

	float PhysicsWorld3D::GetWorstJointSeparation() const
	{
		float worst = 0.0f;
		for (const Joint3D& joint : m_Joints)
			worst = std::max(worst, joint.Separation);

		return worst;
	}

	bool PhysicsWorld3D::JointSuppressesContact(unsigned int a, unsigned int b) const
	{
		// Linear, because a body is jointed to two or three others at most and
		// a humanoid has about fifteen joints in total. A map would cost more
		// to maintain than it saves.
		for (const Joint3D& joint : m_Joints)
		{
			if (joint.CollideConnected)
				continue;

			if ((joint.A == a && joint.B == b) || (joint.A == b && joint.B == a))
				return true;
		}

		return false;
	}

	void PhysicsWorld3D::PrepareJoints(float dt)
	{
		for (Joint3D& joint : m_Joints)
		{
			RigidBody3D& a = m_Bodies[joint.A];
			RigidBody3D& b = m_Bodies[joint.B];

			joint.LeverA = a.Orientation * joint.LocalAnchorA;
			joint.LeverB = b.Orientation * joint.LocalAnchorB;

			// How far the two anchors have drifted apart. This is the
			// constraint error, and driving it to zero is the whole job.
			glm::vec3 error = (b.Position + joint.LeverB) - (a.Position + joint.LeverA);
			joint.Separation = glm::length(error);

			float inverseMassA = SolverInverseMass(a);
			float inverseMassB = SolverInverseMass(b);
			glm::mat3 inverseInertiaA = SolverInverseInertia(a);
			glm::mat3 inverseInertiaB = SolverInverseInertia(b);

			// K = (1/ma + 1/mb) I - [ra]x Ia^-1 [ra]x - [rb]x Ib^-1 [rb]x
			//
			// The 3x3 form of the scalar EffectiveMass used for contacts. The
			// minus signs are not a sign error: skew(r) * skew(r) is negative
			// semi-definite, so subtracting makes K positive definite and
			// therefore invertible.
			glm::mat3 skewA = Skew(joint.LeverA);
			glm::mat3 skewB = Skew(joint.LeverB);

			glm::mat3 k = glm::mat3(inverseMassA + inverseMassB)
				- skewA * inverseInertiaA * skewA
				- skewB * inverseInertiaB * skewB;

			// Two static bodies jointed together leave K singular. Nothing can
			// move, so the identity is as good an answer as any and avoids a
			// division by zero.
			float determinant = glm::determinant(k);
			joint.EffectiveMass = std::fabs(determinant) > 1e-12f
				? glm::inverse(k)
				: glm::mat3(0.0f);

			// Baumgarte: feed a fraction of the position error back in as a
			// velocity target, so drift is corrected over a few steps rather
			// than all at once. Correcting all of it adds energy -- the same
			// reason CorrectPositions uses a percentage.
			const float beta = 0.2f;
			joint.Bias = dt > 0.0f ? (beta / dt) * error : glm::vec3(0.0f);

			if (joint.Type != JointType3D::Hinge)
			{
				joint.LimitState = 0;
				joint.SwingState = 0;
				joint.TwistState = 0;

				// --- the ball motor, driving towards a target orientation ---
				if (joint.MotorEnabled)
				{
					joint.MotorMaxImpulse = joint.MotorMaxTorque * dt;

					// Reset rather than carried over. The limits and the point
					// constraint are warm started, but a motor is not: its
					// budget is per step, and an accumulated impulse left
					// sitting at the clamp from last step means this step's
					// solve computes a delta of nothing and the motor silently
					// stops pulling. That is exactly what happened -- a motor
					// given three times the torque it needed still could not
					// hold an arm up.
					joint.AccumulatedMotorImpulse3 = glm::vec3(0.0f);

					glm::mat3 sumInverse = inverseInertiaA + inverseInertiaB;
					float motorDeterminant = glm::determinant(sumInverse);

					joint.MotorMass3 = std::fabs(motorDeterminant) > 1e-12f
						? glm::inverse(sumInverse)
						: glm::mat3(0.0f);

					// The rotation that would take B where it is now to where
					// it is wanted, expressed in A's frame.
					glm::quat current = glm::normalize(
						glm::conjugate(a.Orientation) * b.Orientation);
					glm::quat error = glm::normalize(
						joint.MotorTargetRotation * glm::conjugate(current));

					if (error.w < 0.0f)
						error = glm::quat(-error.w, -error.x, -error.y, -error.z);

					// Axis-angle. For a small error the vector part is already
					// half the rotation vector, so this is the usual 2*xyz --
					// but written through the angle so a large error is still
					// right rather than merely small-angle correct.
					glm::vec3 axis(error.x, error.y, error.z);
					float axisLength = glm::length(axis);

					glm::vec3 rotationVector(0.0f);
					if (axisLength > 1e-6f)
					{
						float angle = 2.0f * std::atan2(axisLength, error.w);
						rotationVector = (axis / axisLength) * angle;
					}

					// Proportional term. The derivative half is not written
					// anywhere: constraining the spin *to* this value removes
					// any excess, which is what a D term does.
					glm::vec3 targetSpin = joint.MotorStiffness
						* (a.Orientation * rotationVector);

					float speed = glm::length(targetSpin);
					if (speed > joint.MotorMaxSpeed && speed > 0.0f)
						targetSpin *= joint.MotorMaxSpeed / speed;

					joint.MotorTargetSpin = targetSpin;
				}
				else
				{
					joint.AccumulatedMotorImpulse3 = glm::vec3(0.0f);
				}

				if (!joint.ConeTwistEnabled)
				{
					joint.AccumulatedSwingImpulse = 0.0f;
					joint.AccumulatedTwistImpulse = 0.0f;
					continue;
				}

				// How far B has moved from the pose the limits were set in,
				// expressed in A's frame. Everything below happens in that
				// frame and is rotated out to world at the end.
				//
				// Measured against the rest pose rather than against the
				// bodies being aligned: a shoulder's cone opens around where
				// the arm was built, not around the torso's own axes.
				glm::quat relative = glm::normalize(
					glm::normalize(glm::conjugate(a.Orientation) * b.Orientation)
					* glm::conjugate(joint.ConeTwistRest));

				// A quaternion and its negation are the same rotation, but the
				// decomposition below is not sign-agnostic -- forcing a
				// non-negative scalar part is what makes it take the short way
				// round rather than reporting a 350 degree swing as a 10.
				if (relative.w < 0.0f)
					relative = glm::quat(-relative.w, -relative.x, -relative.y, -relative.z);

				const glm::vec3& twistAxisLocal = joint.LocalAxisA;

				// Swing-twist decomposition. The part of the rotation about
				// the bone's own axis is found by projecting the quaternion's
				// vector part onto that axis; whatever is left is the lean.
				//
				// Done this way rather than measuring the two angles directly
				// because the direct measurements interfere: a twist read off
				// a reference vector changes when the bone swings even though
				// nothing twisted.
				glm::vec3 vector(relative.x, relative.y, relative.z);
				glm::vec3 projected = glm::dot(vector, twistAxisLocal) * twistAxisLocal;

				glm::quat twist(relative.w, projected.x, projected.y, projected.z);
				float twistLength = glm::length(twist);
				twist = twistLength > 1e-6f
					? twist / twistLength
					: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

				glm::quat swing = relative * glm::conjugate(twist);

				joint.TwistAngle = 2.0f * std::atan2(
					glm::dot(glm::vec3(twist.x, twist.y, twist.z), twistAxisLocal),
					twist.w);
				joint.SwingAngle = 2.0f * std::acos(glm::clamp(swing.w, -1.0f, 1.0f));

				glm::mat3 sum = inverseInertiaA + inverseInertiaB;
				const float slop = 0.005f;

				// --- swing: is the bone outside its cone? ------------------
				glm::vec3 swingVector(swing.x, swing.y, swing.z);
				float swingVectorLength = glm::length(swingVector);

				if (swingVectorLength > 1e-6f && joint.SwingAngle > joint.ConeAngle - slop)
				{
					// The axis the swing happened about. Positive relative
					// spin about it leans the bone further out.
					joint.SwingAxis = glm::normalize(a.Orientation * (swingVector / swingVectorLength));

					float mass = glm::dot(joint.SwingAxis, sum * joint.SwingAxis);
					joint.SwingMass = mass > 0.0f ? 1.0f / mass : 0.0f;

					joint.SwingState = 1;
					joint.SwingBias = dt > 0.0f
						? (beta / dt) * std::max(joint.SwingAngle - joint.ConeAngle, 0.0f)
						: 0.0f;
				}
				else
				{
					joint.AccumulatedSwingImpulse = 0.0f;
				}

				// --- twist -------------------------------------------------
				joint.TwistAxis = glm::normalize(a.Orientation * twistAxisLocal);

				float twistMass = glm::dot(joint.TwistAxis, sum * joint.TwistAxis);
				joint.TwistMass = twistMass > 0.0f ? 1.0f / twistMass : 0.0f;

				if (joint.TwistAngle >= joint.TwistUpper - slop)
				{
					joint.TwistState = 1;
					joint.TwistBias = dt > 0.0f
						? (beta / dt) * std::max(joint.TwistAngle - joint.TwistUpper, 0.0f)
						: 0.0f;
				}
				else if (joint.TwistAngle <= joint.TwistLower + slop)
				{
					joint.TwistState = -1;
					joint.TwistBias = dt > 0.0f
						? (beta / dt) * std::min(joint.TwistAngle - joint.TwistLower, 0.0f)
						: 0.0f;
				}
				else
				{
					joint.AccumulatedTwistImpulse = 0.0f;
				}

				continue;
			}

			// --- the two locked rotations ---------------------------------
			glm::vec3 axisA = a.Orientation * joint.LocalAxisA;
			glm::vec3 axisB = b.Orientation * joint.LocalAxisB;

			joint.WorldAxis = glm::normalize(axisA);
			BuildTangents(joint.WorldAxis, joint.AxisTangent[0], joint.AxisTangent[1]);

			// The axes have drifted apart by this much. Their cross product is
			// the rotation that would bring them back together, and projecting
			// it onto the two tangents gives the error in each locked
			// direction.
			glm::vec3 misalignment = glm::cross(axisA, axisB);
			joint.AxisError = glm::length(misalignment);

			glm::mat3 sum = inverseInertiaA + inverseInertiaB;

			// A 2x2 for the same reason the point constraint takes a 3x3: the
			// two locked directions are coupled through the inertia tensor,
			// and solving them separately converges badly.
			glm::mat2 angular;
			for (int row = 0; row < 2; row++)
			{
				for (int column = 0; column < 2; column++)
				{
					angular[column][row] = glm::dot(joint.AxisTangent[row],
						sum * joint.AxisTangent[column]);
				}
			}

			float angularDeterminant = angular[0][0] * angular[1][1]
				- angular[0][1] * angular[1][0];

			joint.AngularMass = std::fabs(angularDeterminant) > 1e-12f
				? glm::inverse(angular)
				: glm::mat2(0.0f);

			joint.AngularBias = dt > 0.0f
				? (beta / dt) * glm::vec2(glm::dot(misalignment, joint.AxisTangent[0]),
					glm::dot(misalignment, joint.AxisTangent[1]))
				: glm::vec2(0.0f);

			// --- the angle, and whether it is against a limit --------------
			glm::vec3 referenceA = a.Orientation * joint.LocalRefA;
			glm::vec3 referenceB = b.Orientation * joint.LocalRefB;

			// Signed angle from A's reference to B's, about the hinge axis.
			// atan2 rather than acos: acos loses the sign, and a knee needs to
			// know which side of straight it is on.
			joint.Angle = std::atan2(
				glm::dot(glm::cross(referenceA, referenceB), joint.WorldAxis),
				glm::dot(referenceA, referenceB));

			joint.LimitState = 0;

			// The hinge's motor: one number, about one axis. Shares the
			// limit's effective mass, since both act along the hinge axis.
			if (joint.MotorEnabled)
			{
				joint.MotorMaxImpulse = joint.MotorMaxTorque * dt;
				joint.AccumulatedMotorImpulse = 0.0f;   // per step; see the ball case

				float axisMass = glm::dot(joint.WorldAxis, sum * joint.WorldAxis);
				joint.LimitMass = axisMass > 0.0f ? 1.0f / axisMass : 0.0f;

				// Shortest way round, so a motor asked to go from +3 to -3
				// radians turns through the gap rather than the long way.
				float error = joint.MotorTargetAngle - joint.Angle;
				while (error > glm::pi<float>()) error -= glm::two_pi<float>();
				while (error < -glm::pi<float>()) error += glm::two_pi<float>();

				float targetSpin = glm::clamp(joint.MotorStiffness * error,
					-joint.MotorMaxSpeed, joint.MotorMaxSpeed);

				joint.MotorTargetSpin = glm::vec3(targetSpin, 0.0f, 0.0f);
			}
			else
			{
				joint.AccumulatedMotorImpulse = 0.0f;
			}

			if (joint.LimitEnabled)
			{
				float axisMass = glm::dot(joint.WorldAxis, sum * joint.WorldAxis);
				joint.LimitMass = axisMass > 0.0f ? 1.0f / axisMass : 0.0f;

				// A small slop, as contacts have: correcting a limit to
				// exactly its value every step makes a resting limb buzz.
				const float slop = 0.005f;

				if (joint.Angle <= joint.LowerLimit + slop)
				{
					joint.LimitState = -1;
					joint.LimitBias = dt > 0.0f
						? (beta / dt) * std::min(joint.Angle - joint.LowerLimit + slop, 0.0f)
						: 0.0f;
				}
				else if (joint.Angle >= joint.UpperLimit - slop)
				{
					joint.LimitState = 1;
					joint.LimitBias = dt > 0.0f
						? (beta / dt) * std::max(joint.Angle - joint.UpperLimit - slop, 0.0f)
						: 0.0f;
				}

				// The stored impulse only means anything while the joint stays
				// against the same limit.
				if (joint.LimitState == 0)
					joint.AccumulatedLimitImpulse = 0.0f;
			}
		}
	}

	void PhysicsWorld3D::SolveJoints()
	{
		for (Joint3D& joint : m_Joints)
		{
			RigidBody3D& a = m_Bodies[joint.A];
			RigidBody3D& b = m_Bodies[joint.B];

			// Angular parts first, then the point constraint. The point
			// constraint is the one that must hold visibly -- a limb detaching
			// looks far worse than one bending a degree past its stop -- so it
			// gets the last word each iteration.
			//
			// One shared shape for every angular limit here: measure the
			// relative spin along the limit's axis, solve for the impulse that
			// stops it, and clamp the running total to a single sign so the
			// stop resists but never holds.
			auto solveLimit = [&](const glm::vec3& axis, float mass, float bias,
				float& accumulated, bool positiveOnly)
			{
				float along = glm::dot(b.AngularVelocity - a.AngularVelocity, axis);
				float impulse = -mass * (along + bias);

				float previous = accumulated;
				accumulated = positiveOnly
					? std::max(previous + impulse, 0.0f)
					: std::min(previous + impulse, 0.0f);

				glm::vec3 world = (accumulated - previous) * axis;
				a.AngularVelocity -= SolverInverseInertia(a) * world;
				b.AngularVelocity += SolverInverseInertia(b) * world;
			};

			// Motors before limits, so a limit always has the last word over a
			// motor driving into it. A muscle cannot pull a knee backwards.
			if (joint.MotorEnabled && joint.Type != JointType3D::Hinge)
			{
				glm::vec3 relativeSpin = b.AngularVelocity - a.AngularVelocity;
				glm::vec3 impulse = joint.MotorMass3
					* (joint.MotorTargetSpin - relativeSpin);

				glm::vec3 previous = joint.AccumulatedMotorImpulse3;
				glm::vec3 total = previous + impulse;

				// Clamped in magnitude, not per axis: a torque budget is a
				// scalar, and clamping the components separately would let a
				// diagonal pull exceed it by root three.
				float maximum = joint.MotorMaxImpulse;
				float length = glm::length(total);
				if (length > maximum && length > 0.0f)
					total *= maximum / length;

				joint.AccumulatedMotorImpulse3 = total;

				glm::vec3 applied = total - previous;
				a.AngularVelocity -= SolverInverseInertia(a) * applied;
				b.AngularVelocity += SolverInverseInertia(b) * applied;
			}

			if (joint.MotorEnabled && joint.Type == JointType3D::Hinge)
			{
				float along = glm::dot(b.AngularVelocity - a.AngularVelocity,
					joint.WorldAxis);
				float impulse = joint.LimitMass * (joint.MotorTargetSpin.x - along);

				float previous = joint.AccumulatedMotorImpulse;
				float maximum = joint.MotorMaxImpulse;
				joint.AccumulatedMotorImpulse =
					glm::clamp(previous + impulse, -maximum, maximum);

				glm::vec3 applied = (joint.AccumulatedMotorImpulse - previous)
					* joint.WorldAxis;

				a.AngularVelocity -= SolverInverseInertia(a) * applied;
				b.AngularVelocity += SolverInverseInertia(b) * applied;
			}

			if (joint.ConeTwistEnabled && joint.Type != JointType3D::Hinge)
			{
				if (joint.SwingState != 0)
				{
					// Leaning further out is what must be refused, so the
					// impulse may only ever push the bone back in.
					solveLimit(joint.SwingAxis, joint.SwingMass, joint.SwingBias,
						joint.AccumulatedSwingImpulse, false);
				}

				if (joint.TwistState != 0)
				{
					solveLimit(joint.TwistAxis, joint.TwistMass, joint.TwistBias,
						joint.AccumulatedTwistImpulse, joint.TwistState < 0);
				}
			}

			if (joint.Type == JointType3D::Hinge)
			{
				glm::vec3 relativeSpin = b.AngularVelocity - a.AngularVelocity;

				// The two locked rotations.
				glm::vec2 spinError(glm::dot(relativeSpin, joint.AxisTangent[0]),
					glm::dot(relativeSpin, joint.AxisTangent[1]));

				glm::vec2 angularImpulse = joint.AngularMass
					* -(spinError + joint.AngularBias);

				joint.AccumulatedAngularImpulse += angularImpulse;

				glm::vec3 worldAngular = angularImpulse.x * joint.AxisTangent[0]
					+ angularImpulse.y * joint.AxisTangent[1];

				a.AngularVelocity -= SolverInverseInertia(a) * worldAngular;
				b.AngularVelocity += SolverInverseInertia(b) * worldAngular;

				// The limit, which unlike everything else here is unilateral:
				// a knee stop resists bending further but must not hold the
				// knee *at* the stop, so the accumulated impulse is clamped to
				// one sign and the joint is free to leave.
				if (joint.LimitState != 0)
				{
					float along = glm::dot(b.AngularVelocity - a.AngularVelocity,
						joint.WorldAxis);

					float impulse = -joint.LimitMass * (along + joint.LimitBias);

					float previous = joint.AccumulatedLimitImpulse;
					if (joint.LimitState < 0)
						joint.AccumulatedLimitImpulse = std::max(previous + impulse, 0.0f);
					else
						joint.AccumulatedLimitImpulse = std::min(previous + impulse, 0.0f);

					impulse = joint.AccumulatedLimitImpulse - previous;

					glm::vec3 worldLimit = impulse * joint.WorldAxis;
					a.AngularVelocity -= SolverInverseInertia(a) * worldLimit;
					b.AngularVelocity += SolverInverseInertia(b) * worldLimit;
				}
			}

			glm::vec3 relative = PointVelocity(b, joint.LeverB)
				- PointVelocity(a, joint.LeverA);

			// No clamping anywhere in here. That is the bilateral part: the
			// joint pulls as readily as it pushes, and an accumulated impulse
			// that wants to reverse sign is allowed to.
			glm::vec3 impulse = joint.EffectiveMass * -(relative + joint.Bias);

			joint.AccumulatedImpulse += impulse;

			a.Velocity -= impulse * SolverInverseMass(a);
			a.AngularVelocity -= SolverInverseInertia(a) * glm::cross(joint.LeverA, impulse);

			b.Velocity += impulse * SolverInverseMass(b);
			b.AngularVelocity += SolverInverseInertia(b) * glm::cross(joint.LeverB, impulse);
		}
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
		for (Joint3D& joint : m_Joints)
		{
			RigidBody3D& a = m_Bodies[joint.A];
			RigidBody3D& b = m_Bodies[joint.B];

			const glm::vec3& impulse = joint.AccumulatedImpulse;

			a.Velocity -= impulse * SolverInverseMass(a);
			a.AngularVelocity -= SolverInverseInertia(a) * glm::cross(joint.LeverA, impulse);

			b.Velocity += impulse * SolverInverseMass(b);
			b.AngularVelocity += SolverInverseInertia(b) * glm::cross(joint.LeverB, impulse);

			if (joint.Type != JointType3D::Hinge)
			{
				if (joint.ConeTwistEnabled)
				{
					glm::vec3 stored(0.0f);
					if (joint.SwingState != 0)
						stored += joint.AccumulatedSwingImpulse * joint.SwingAxis;
					if (joint.TwistState != 0)
						stored += joint.AccumulatedTwistImpulse * joint.TwistAxis;

					a.AngularVelocity -= SolverInverseInertia(a) * stored;
					b.AngularVelocity += SolverInverseInertia(b) * stored;
				}
				continue;
			}

			// The angular impulses need carrying over for the same reason the
			// linear one does. The axis tangents are rebuilt every step and can
			// swing round as the body turns, so the stored pair is applied
			// along the *current* tangents -- an approximation, but the
			// alternative is storing the impulse as a world vector and having
			// it fight the new frame.
			glm::vec3 worldAngular =
				joint.AccumulatedAngularImpulse.x * joint.AxisTangent[0]
				+ joint.AccumulatedAngularImpulse.y * joint.AxisTangent[1];

			if (joint.LimitState != 0)
				worldAngular += joint.AccumulatedLimitImpulse * joint.WorldAxis;

			a.AngularVelocity -= SolverInverseInertia(a) * worldAngular;
			b.AngularVelocity += SolverInverseInertia(b) * worldAngular;
		}

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

			// Kinematic bodies are steered, not simulated: no gravity, no
			// forces, no damping. Whatever velocity was set is the velocity
			// they keep until it is set again.
			if (body.Type == BodyType::Static || body.Type == BodyType::Kinematic
				|| !body.Awake)
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
			// Kinematic bodies are integrated here and nowhere else -- this is
			// the whole difference between them and static ones.
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

			// A sleeping kinematic body would stop being integrated and so
			// stop moving, which is the one thing it exists to do.
			if (body.Type == BodyType::Kinematic)
			{
				body.Awake = true;
				body.SleepTimer = 0.0f;
				continue;
			}

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

		}

		// Jointed bodies sleep together or not at all.
		//
		// Sleeping makes a body immovable to the solver, so half a sleeping
		// chain would act as an anchor bolted to mid-air: the still-moving
		// half would swing from it, and the moment the sleeper woke the whole
		// thing would lurch. Wakefulness has to spread along the joints, and
		// spread *transitively* -- waking a link's neighbour can wake its
		// neighbour in turn -- so this runs until nothing changes.
		//
		// This is the poor relation of the island solver 2D has. It is enough
		// for chains and ragdolls, which are what joints are for here.
		if (AllowSleeping && !m_Joints.empty())
		{
			bool changed = true;
			unsigned int guard = 0;

			while (changed && guard++ <= m_Joints.size())
			{
				changed = false;

				for (const Joint3D& joint : m_Joints)
				{
					RigidBody3D& a = m_Bodies[joint.A];
					RigidBody3D& b = m_Bodies[joint.B];

					// Static bodies are never awake and must not drag their
					// partner awake either, or nothing anchored to the world
					// would ever settle.
					bool awakeA = a.Type != BodyType::Static && a.Awake;
					bool awakeB = b.Type != BodyType::Static && b.Awake;

					if (awakeA == awakeB)
						continue;

					RigidBody3D& sleeper = awakeA ? b : a;
					if (sleeper.Type == BodyType::Static)
						continue;

					sleeper.Awake = true;
					sleeper.SleepTimer = 0.0f;
					changed = true;
				}
			}
		}

		for (const RigidBody3D& body : m_Bodies)
		{
			if (body.Type != BodyType::Static && body.Awake)
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
