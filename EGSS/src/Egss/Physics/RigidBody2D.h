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
		Dynamic
	};

	enum class ColliderShape
	{
		Circle = 0,
		Box
	};

	// A body and its collider in one struct.
	//
	// Real engines separate the two so one body can carry several shapes; that
	// is worth doing when it is needed and not before. Everything here is
	// axis-aligned and has no rotation -- see the note on PhysicsWorld2D.
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

		static RigidBody2D MakeCircle(const glm::vec2& position, float radius, float mass = 1.0f)
		{
			RigidBody2D body;
			body.Shape = ColliderShape::Circle;
			body.Position = position;
			body.PreviousPosition = position;
			body.Radius = radius;
			body.SetMass(mass);
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
			return body;
		}

		// Static bodies are just dynamic ones with no inverse mass.
		static RigidBody2D MakeStaticBox(const glm::vec2& position, const glm::vec2& halfExtents)
		{
			RigidBody2D body = MakeBox(position, halfExtents, 0.0f);
			body.Type = BodyType::Static;
			return body;
		}
	};

}
