#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	enum class BodyType
	{
		// Never moves and never responds to a collision. The floor, walls,
		// level geometry.
		Static = 0,
		// Moved by forces and collisions.
		Dynamic,
		// Moved by whoever sets its velocity, and by nothing else.
		//
		// Infinite mass to the solver, exactly like a static body -- contacts
		// and joints cannot shift it, and gravity does not pull on it. Unlike
		// a static body it *does* integrate its position from its velocity, so
		// it can be driven about and will shove dynamic bodies out of its way
		// without being shoved back.
		//
		// This is what a game character is while it is under control: it does
		// not balance because it cannot fall. Switching it to Dynamic is what
		// "going ragdoll" means.
		Kinematic
	};

	enum class ColliderShape
	{
		Circle = 0,
		Box
	};

	// A body and its collider in one struct.
	//
	// Real engines separate the two so one body can carry several shapes; that
	// is worth doing when it is needed and not before. One body, one collider,
	// and the collider turns with the body -- see the note on PhysicsWorld2D.
	struct EGSS_API RigidBody2D
	{
		BodyType Type = BodyType::Dynamic;
		ColliderShape Shape = ColliderShape::Circle;

		glm::vec2 Position = { 0.0f, 0.0f };
		glm::vec2 Velocity = { 0.0f, 0.0f };

		// Where the body was at the start of the current step. The world keeps
		// this up to date so rendering can interpolate -- see
		// Application::GetInterpolationAlpha.
		glm::vec2 PreviousPosition = { 0.0f, 0.0f };

		// --- Angular state ---------------------------------------------------
		//
		// Integrated, and produced by collisions: a contact away from the
		// centre of mass spins the body through its lever arm. The state, the
		// oriented narrowphase and the solver's angular terms were built and
		// checked in that order, each on its own, before being joined up.
		//
		// Radians. Degrees are for the UI and for .obj files; every trig
		// function here takes radians and converting at the boundary is how you
		// end up converting twice.
		float Rotation = 0.0f;
		float AngularVelocity = 0.0f;
		float Torque = 0.0f;
		float PreviousRotation = 0.0f;

		// The *inverse* moment of inertia, on the same terms as InverseMass: 0
		// means "cannot be spun", which is what a static body wants and what
		// keeps it out of the solver's divisions.
		//
		// In 2D this is a scalar, not a tensor -- there is only one axis to
		// rotate about, so the whole 3x3 collapses to one number.
		float InverseInertia = 0.0f;

		float AngularDamping = 0.05f;

		// Circle uses Radius; Box uses HalfExtents. Kept as plain fields
		// rather than a union so a body can be reshaped at runtime.
		float Radius = 0.5f;
		glm::vec2 HalfExtents = { 0.5f, 0.5f };

		// The *inverse* mass, so an immovable body is simply 0 and the solver
		// never divides. This is why static bodies need no special case in the
		// impulse maths.
		float InverseMass = 1.0f;

		// 0 = perfectly inelastic, 1 = bounces back at full speed.
		float Restitution = 0.2f;
		// Coulomb friction coefficient, applied along the contact tangent.
		float Friction = 0.4f;
		// Per-body multiplier on world gravity, so one object can feel floaty
		// without needing a second world.
		float GravityScale = 1.0f;

		// Bodies that have been slow for long enough stop being integrated.
		// Both a performance win and the cure for resting jitter.
		bool Awake = true;
		float SleepTimer = 0.0f;

		void SetMass(float mass) { InverseMass = mass > 0.0f ? 1.0f / mass : 0.0f; }
		float GetMass() const { return InverseMass > 0.0f ? 1.0f / InverseMass : 0.0f; }

		float GetInertia() const { return InverseInertia > 0.0f ? 1.0f / InverseInertia : 0.0f; }

		// The moment of inertia this body's shape and mass imply, about its
		// centre. A mass of 0 gives 0, so a static body stays unspinnable
		// without a special case.
		//
		//   solid box     m (w^2 + h^2) / 12
		//   solid disc    m r^2 / 2
		//
		// Called by the Make* helpers, and worth calling again by hand if you
		// change a body's size or mass after creating it -- inertia depends on
		// both and nothing here watches for that.
		void RecalculateInertia()
		{
			float mass = GetMass();
			if (mass <= 0.0f)
			{
				InverseInertia = 0.0f;
				return;
			}

			float inertia = (Shape == ColliderShape::Box)
				? mass * (4.0f * HalfExtents.x * HalfExtents.x
					+ 4.0f * HalfExtents.y * HalfExtents.y) / 12.0f
				: mass * Radius * Radius * 0.5f;

			InverseInertia = (inertia > 0.0f) ? 1.0f / inertia : 0.0f;
		}

		static RigidBody2D MakeCircle(const glm::vec2& position, float radius, float mass = 1.0f)
		{
			RigidBody2D body;
			body.Shape = ColliderShape::Circle;
			body.Position = position;
			body.PreviousPosition = position;
			body.Radius = radius;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		static RigidBody2D MakeBox(const glm::vec2& position, const glm::vec2& halfExtents, float mass = 1.0f)
		{
			RigidBody2D body;
			body.Shape = ColliderShape::Box;
			body.Position = position;
			body.PreviousPosition = position;
			body.HalfExtents = halfExtents;
			body.SetMass(mass);
			body.RecalculateInertia();
			return body;
		}

		// Static bodies are just dynamic ones with no inverse mass.
		static RigidBody2D MakeStaticBox(const glm::vec2& position, const glm::vec2& halfExtents)
		{
			RigidBody2D body = MakeBox(position, halfExtents, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}

		static RigidBody2D MakeStaticCircle(const glm::vec2& position, float radius)
		{
			RigidBody2D body = MakeCircle(position, radius, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}
	};

}
