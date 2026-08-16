#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/RigidBody2D.h"   // BodyType, shared by both dimensions
#include "Egss/Physics/Heightfield3D.h"
#include "Egss/Voxel/VoxelField3D.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

namespace Egss {

	enum class ColliderShape3D
	{
		Sphere = 0,
		Box,
		// A segment with a radius: the line from -HalfHeight to +HalfHeight
		// along the body's own **y** axis, swept by Radius. Total height is
		// 2*(HalfHeight + Radius), so a capsule with HalfHeight 0 is a sphere
		// and every test below has to stay correct in that limit.
		//
		// Worth having because it is what characters are made of: it has no
		// corners to catch on a step, and it stands up rather than rolling
		// away like a sphere.
		Capsule,
		// A grid of heights, described by `Field`. **Static only, and never
		// turned**: the collider is a function from (x, z) to a height, which
		// stops being one the moment it is tilted. Nothing enforces that beyond
		// this comment and `MakeHeightfield`, because a check that costs a
		// branch per body to catch a mistake made once is a poor trade.
		//
		// This is the first collider that is not convex, and the first whose
		// size has nothing to do with the body's -- a heightfield is usually
		// the whole world, and its bounds say so.
		Heightfield,
		// Several boxes rigidly fixed together, described by `Children`, each
		// axis-aligned in the body's own frame.
		//
		// **This is how a concave shape is simulated by a convex solver.**
		// `Sat3D` needs convex, and a rock broken off a cliff is not -- so it is
		// approximated by a set of boxes that between them fill it. Each box is
		// convex, the set is not, and every existing box test is reused by
		// treating a child as a box body in world space.
		//
		// The cost is that the manifold still carries **one normal**, the same
		// compromise a heightfield makes: several children touching different
		// surfaces have the deepest one's normal adopted and the rest
		// re-measured along it. A lump resting on flat ground -- which is what
		// debris does -- has one normal anyway.
		Compound,
		// A signed distance field, described by `Voxels`. Static and never
		// turned, on the same terms as Heightfield.
		//
		// The difference that matters: a heightfield is a *function* of x and z
		// and so is single-valued in y, which is why it can never hold an
		// overhang, a cave or an arch. A distance field is a volume and holds
		// all three. Everything else about it is cheaper -- the stored value is
		// already the depth of a point, and the field's gradient is already the
		// surface normal, so the narrowphase does no searching at all.
		Sdf
	};

	// One box of a Compound collider, in the body's own frame.
	struct EGSS_API CompoundChild
	{
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
	};

	// A rigid body in three dimensions.
	//
	// Two things stop this being RigidBody2D with an extra component, and both
	// are the reason 2D was built first:
	//
	//   * **Orientation is a quaternion, not an angle.** Three angles cannot
	//     represent orientation without gimbal lock, and a matrix has six
	//     redundant numbers to keep consistent. A quaternion has one
	//     constraint -- unit length -- and renormalising it is one line.
	//
	//   * **Inertia is a tensor, not a scalar.** In 2D there is one axis to
	//     turn about, so the whole 3x3 collapses to a number. In 3D a body
	//     resists rotation differently about each axis, the tensor is written
	//     in *body* space, and it therefore has to be rotated into world space
	//     every time the body turns. Skip that and a box behaves correctly
	//     only while it happens to be axis-aligned -- which looks like a
	//     solver bug and is not one.
	struct EGSS_API RigidBody3D
	{
		BodyType Type = BodyType::Dynamic;
		ColliderShape3D Shape = ColliderShape3D::Sphere;

		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };

		// Unit length. Anything that changes it must renormalise: integration
		// adds a small non-unit part every step, and left alone that grows
		// until the body visibly shears.
		glm::quat Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		// In *world* space, radians per second, direction is the axis and
		// length is the rate. World rather than body space because that is
		// what contacts and impulses speak.
		glm::vec3 AngularVelocity = { 0.0f, 0.0f, 0.0f };

		glm::vec3 Force = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Torque = { 0.0f, 0.0f, 0.0f };

		// Where the body was at the start of the current step, so rendering
		// can interpolate. The orientation pair wants slerp, not mix.
		glm::vec3 PreviousPosition = { 0.0f, 0.0f, 0.0f };
		glm::quat PreviousOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		float InverseMass = 1.0f;

		// The inertia tensor in body space, and its inverse. Diagonal for
		// every shape here, because a box and a sphere are both symmetric
		// about their own axes -- but stored as full matrices, since the
		// world-space versions are not diagonal and the two want the same
		// type.
		//
		// The forward tensor is kept as well as the inverse purely so nothing
		// has to invert a matrix to ask what a body's angular momentum is.
		glm::mat3 InertiaLocal = glm::mat3(1.0f);
		glm::mat3 InverseInertiaLocal = glm::mat3(1.0f);

		// The same tensor rotated into world space: R * I_local^-1 * R^T.
		// Cached because it changes only when the orientation does, and the
		// solver would otherwise rebuild it per contact.
		glm::mat3 InverseInertiaWorld = glm::mat3(1.0f);

		float LinearDamping = 0.01f;
		float AngularDamping = 0.05f;

		// Sphere uses Radius; Box uses HalfExtents; Capsule uses both Radius
		// and HalfHeight.
		float Radius = 0.5f;
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };

		// Half the length of the capsule's *segment*, not of the whole capsule
		// -- the caps add Radius at each end on top of this.
		float HalfHeight = 0.5f;

		// The samples a Heightfield collider stands on, centred on Position and
		// with its heights measured from Position.y.
		//
		// Shared rather than held by value because bodies are copied whenever
		// the world's vector grows, and a 129x129 field is 66 KB of that. It is
		// const because two bodies may share one field and because the
		// narrowphase must never be able to move the ground it is testing
		// against.
		std::shared_ptr<const Heightfield3D> Field;

		// The signed distance field an Sdf collider is made of, on the same
		// terms as Field above.
		//
		// A separate member rather than a variant, because the two answer
		// different questions and the narrowphase never has to ask which kind it
		// is holding -- Shape already says.
		std::shared_ptr<const VoxelField3D> Voxels;

		// The boxes a Compound collider is made of. Shared for the same reason
		// the fields are: a decomposed rock can be a few hundred of them, and a
		// body is copied every time the world's vector grows.
		std::shared_ptr<const std::vector<CompoundChild>> Children;

		float Restitution = 0.2f;
		float Friction = 0.4f;
		float GravityScale = 1.0f;

		bool Awake = true;

		// One body this one does not collide with, by handle, or -1.
		//
		// Deliberately a single handle rather than layers or a mask: the case
		// that exists is "this object is being carried by that one", and it is
		// one pair. A mask system would be a component with no system -- see
		// the note in CLAUDE.md -- until something needs more than a pair.
		int IgnoreCollisionWith = -1;
		float SleepTimer = 0.0f;

		void SetMass(float mass) { InverseMass = mass > 0.0f ? 1.0f / mass : 0.0f; }
		float GetMass() const { return InverseMass > 0.0f ? 1.0f / InverseMass : 0.0f; }

		// Rebuilds the world tensor from the current orientation. Called by
		// the world every step; call it by hand if you set Orientation
		// directly and want to ask about inertia before the next step.
		//
		// R I^-1 R^T, and the transpose is what makes it a *similarity*
		// transform rather than merely a rotation of the numbers: the tensor
		// maps angular momentum to angular velocity, and both of those live in
		// world space, so the frame has to be changed on the way in and back
		// again on the way out.
		void UpdateInertiaWorld()
		{
			glm::mat3 rotation = glm::mat3_cast(Orientation);
			InverseInertiaWorld = rotation * InverseInertiaLocal * glm::transpose(rotation);
		}

		// Angular momentum, in world space. The quantity that is actually
		// conserved when nothing is pushing: angular *velocity* is not, and a
		// tumbling box changes it constantly while L stays put.
		glm::vec3 GetAngularMomentum() const
		{
			if (Type == BodyType::Static || InverseMass <= 0.0f)
				return glm::vec3(0.0f);

			glm::mat3 rotation = glm::mat3_cast(Orientation);
			glm::mat3 inertia = rotation * InertiaLocal * glm::transpose(rotation);
			return inertia * AngularVelocity;
		}

		// Rotational kinetic energy, 1/2 w . (I w).
		float GetRotationalEnergy() const
		{
			return 0.5f * glm::dot(AngularVelocity, GetAngularMomentum());
		}

		// The inertia this body's shape and mass imply, about its centre and
		// along its own axes:
		//
		//   solid box      m/12 * (h^2 + d^2, w^2 + d^2, w^2 + h^2)
		//   solid sphere   2/5 m r^2, the same about every axis
		//
		// Note which extents pair with which axis: the moment about x is
		// resisted by the *other* two dimensions, not by x. Getting that wrong
		// is invisible on a cube and obvious on anything long.
		void RecalculateInertia()
		{
			float mass = GetMass();
			if (mass <= 0.0f)
			{
				InertiaLocal = glm::mat3(0.0f);
				InverseInertiaLocal = glm::mat3(0.0f);
				InverseInertiaWorld = glm::mat3(0.0f);
				return;
			}

			glm::vec3 diagonal(0.0f);

			if (Shape == ColliderShape3D::Box)
			{
				glm::vec3 size = HalfExtents * 2.0f;
				diagonal = (mass / 12.0f) * glm::vec3(
					size.y * size.y + size.z * size.z,
					size.x * size.x + size.z * size.z,
					size.x * size.x + size.y * size.y);
			}
			else if (Shape == ColliderShape3D::Capsule)
			{
				// A cylinder plus two hemispheres, each contributing about the
				// capsule's centre. Mass is split between them by volume, so
				// the tensor stays right when the caps dominate a stubby
				// capsule or the cylinder dominates a long one.
				float r = Radius;
				float h = HalfHeight * 2.0f;              // the cylinder's length

				float cylinderVolume = glm::pi<float>() * r * r * h;
				float capsVolume = (4.0f / 3.0f) * glm::pi<float>() * r * r * r;
				float total = cylinderVolume + capsVolume;

				if (total <= 0.0f)
				{
					diagonal = glm::vec3(0.0f);
				}
				else
				{
					float mc = mass * cylinderVolume / total;    // cylinder
					float mh = mass * capsVolume / total * 0.5f; // one hemisphere

					// About y, the axis of symmetry: everything is a disc.
					float yy = 0.5f * mc * r * r + 2.0f * (0.4f * mh * r * r);

					// About x and z. The hemisphere term looks arbitrary and is
					// not: shifting (2/5) m r^2 from the sphere centre to the
					// capsule centre via the parallel-axis theorem leaves
					// exactly h^2/4 + 3hr/8, because the 9r^2/64 from the
					// hemisphere's own centre of mass cancels out.
					float xx = mc * (h * h / 12.0f + r * r * 0.25f)
						+ 2.0f * mh * (0.4f * r * r + h * h * 0.25f + 0.375f * h * r);

					diagonal = { xx, yy, xx };
				}
			}
			else if (Shape == ColliderShape3D::Compound)
			{
				// The one shape whose tensor is not diagonal, and the reason the
				// members below are full matrices rather than three numbers.
				//
				// Each child contributes its own box tensor about its own centre,
				// carried to the body's origin by the parallel axis theorem:
				// `I + m (d.d E - d (x) d)`. The outer-product term is what makes
				// the result off-diagonal, and dropping it -- which is easy to do
				// by writing only the diagonal -- gives a body that tumbles about
				// the wrong axes and looks like a solver bug.
				//
				// Mass is shared between children by volume, so a decomposition
				// into unequal boxes weighs them correctly.
				InertiaLocal = glm::mat3(0.0f);

				float total = 0.0f;
				if (Children)
					for (const CompoundChild& child : *Children)
						total += 8.0f * child.HalfExtents.x * child.HalfExtents.y
							* child.HalfExtents.z;

				if (Children && total > 0.0f)
				{
					for (const CompoundChild& child : *Children)
					{
						float volume = 8.0f * child.HalfExtents.x * child.HalfExtents.y
							* child.HalfExtents.z;
						float m = mass * volume / total;

						glm::vec3 size = child.HalfExtents * 2.0f;
						glm::vec3 own = (m / 12.0f) * glm::vec3(
							size.y * size.y + size.z * size.z,
							size.x * size.x + size.z * size.z,
							size.x * size.x + size.y * size.y);

						glm::mat3 tensor(0.0f);
						for (int axis = 0; axis < 3; axis++)
							tensor[axis][axis] = own[axis];

						const glm::vec3& d = child.Offset;
						float dd = glm::dot(d, d);

						for (int r = 0; r < 3; r++)
						{
							for (int c = 0; c < 3; c++)
							{
								float shift = (r == c ? dd : 0.0f) - d[r] * d[c];
								tensor[c][r] += m * shift;
							}
						}

						InertiaLocal += tensor;
					}
				}

				InverseInertiaLocal = glm::determinant(InertiaLocal) > 1e-12f
					? glm::inverse(InertiaLocal)
					: glm::mat3(0.0f);

				UpdateInertiaWorld();
				return;
			}
			else
			{
				float value = 0.4f * mass * Radius * Radius;
				diagonal = glm::vec3(value);
			}

			InertiaLocal = glm::mat3(0.0f);
			InverseInertiaLocal = glm::mat3(0.0f);

			for (int axis = 0; axis < 3; axis++)
			{
				InertiaLocal[axis][axis] = diagonal[axis];
				InverseInertiaLocal[axis][axis] =
					diagonal[axis] > 0.0f ? 1.0f / diagonal[axis] : 0.0f;
			}

			UpdateInertiaWorld();
		}

		static RigidBody3D MakeSphere(const glm::vec3& position, float radius, float mass = 1.0f)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Sphere;
			body.Position = position;
			body.PreviousPosition = position;
			body.Radius = radius;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		static RigidBody3D MakeBox(const glm::vec3& position, const glm::vec3& halfExtents, float mass = 1.0f)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Box;
			body.Position = position;
			body.PreviousPosition = position;
			body.HalfExtents = halfExtents;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		// `halfHeight` is half the segment, so the capsule stands
		// 2*(halfHeight + radius) tall.
		static RigidBody3D MakeCapsule(const glm::vec3& position, float radius,
			float halfHeight, float mass = 1.0f)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Capsule;
			body.Position = position;
			body.PreviousPosition = position;
			body.Radius = radius;
			body.HalfHeight = halfHeight;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		// Static bodies are dynamic ones with no inverse mass, exactly as in 2D.
		static RigidBody3D MakeStaticBox(const glm::vec3& position, const glm::vec3& halfExtents)
		{
			RigidBody3D body = MakeBox(position, halfExtents, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}

		static RigidBody3D MakeStaticSphere(const glm::vec3& position, float radius)
		{
			RigidBody3D body = MakeSphere(position, radius, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}

		static RigidBody3D MakeStaticCapsule(const glm::vec3& position, float radius,
			float halfHeight)
		{
			RigidBody3D body = MakeCapsule(position, radius, halfHeight, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}

		// `position` is the centre of the field in x and z, and the height its
		// samples are measured from. There is no dynamic form: a heightfield
		// with a mass would have to be turned by an impulse, and a turned
		// heightfield is not a heightfield.
		static RigidBody3D MakeHeightfield(const glm::vec3& position,
			const std::shared_ptr<const Heightfield3D>& field)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Heightfield;
			body.Type = BodyType::Static;
			body.Position = position;
			body.PreviousPosition = position;
			body.Field = field;
			body.SetMass(0.0f);
			body.RecalculateInertia();
			return body;
		}

		// A body made of boxes. `children` are in the body's frame, and the mass
		// is shared between them by volume -- so a long thin piece and a squat
		// one of the same mass get different tensors, which is the whole point
		// of not using one box round the lot.
		static RigidBody3D MakeCompound(const glm::vec3& position,
			const std::shared_ptr<const std::vector<CompoundChild>>& children,
			float mass = 1.0f)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Compound;
			body.Position = position;
			body.PreviousPosition = position;
			body.Children = children;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		// A signed distance field as a collider: the same idea as the
		// heightfield above, with the single-valued-in-y restriction removed. It
		// is what an overhang, a cave and an arch need, and what a heightfield
		// cannot express at all.
		//
		// **The field is collided against directly, never its triangles.** The
		// marching-cubes surface is for the eye; a distance field already
		// answers what the narrowphase asks -- the value is the depth and the
		// gradient is the normal -- and does it without ever handing the solver
		// one of the long thin triangles that isosurface extraction produces.
		//
		// Static only, for the same reason a heightfield is: `Position` places
		// the field's origin, and a rotated distance field would need every
		// query transformed into its frame. Not hard, and not needed until
		// something wants a spinning cave.
		static RigidBody3D MakeSdf(const glm::vec3& position,
			const std::shared_ptr<const VoxelField3D>& voxels)
		{
			RigidBody3D body;
			body.Shape = ColliderShape3D::Sdf;
			body.Type = BodyType::Static;
			body.Position = position;
			body.PreviousPosition = position;
			body.Voxels = voxels;
			body.SetMass(0.0f);
			body.RecalculateInertia();
			return body;
		}

		// The capsule's segment in world space. Everything that touches a
		// capsule starts here, so it is written once.
		void GetSegment(glm::vec3& outA, glm::vec3& outB) const
		{
			glm::vec3 axis = Orientation * glm::vec3(0.0f, HalfHeight, 0.0f);
			outA = Position - axis;
			outB = Position + axis;
		}
	};

}
