#pragma once

#include "GS/Core.h"
#include "GS/Scene/Scene.h"

#include <string>
#include <glm/glm.hpp>

namespace GS::ScriptPhysics {

	// What a GSS script may do to its entity's rigid body -- applyImpulse,
	// applyForce, get/setVelocity, isTouching -- routed through one place for
	// both ways a script runs. The interpreted path's natives call these; the
	// transpiler emits calls to these. Declared here in GS rather than beside
	// the editor's Play mode because generated C++ is syntax-checked against
	// GS's headers alone, and must compile with nothing but <GS.h>.
	//
	// The engine owns no physics world for a Scene, so whoever runs one
	// implements Backend and installs it for as long as it is simulating --
	// the editor's PlayMode, between Play and Stop. See
	// docs/superpowers/specs/2026-10-02-gss-physics-bindings-design.md.
	class GS_API Backend
	{
	public:
		virtual ~Backend() = default;

		// Each returns false when the entity has no body (no PhysicsComponent,
		// or not part of the running simulation). A static body *has* one:
		// those calls return true and change nothing, because it is
		// immovable, not missing.
		virtual bool ApplyImpulse(EntityId entity, const glm::vec3& impulse) = 0;
		virtual bool ApplyForce(EntityId entity, const glm::vec3& force) = 0;
		virtual bool GetVelocity(EntityId entity, glm::vec3& out) = 0;
		virtual bool SetVelocity(EntityId entity, const glm::vec3& velocity) = 0;
		virtual bool IsTouching(EntityId entity, const std::string& tag, bool& out) = 0;
	};

	// Null clears it. Installing a backend also forgets which entities have
	// already been warned about, so a new Play session warns afresh.
	GS_API void SetBackend(Backend* backend);

	// A call on an entity with no body does nothing (GetVelocity reads zero,
	// IsTouching false) and logs one warning for that entity -- one, not one
	// per frame, since the call usually sits in OnUpdate.
	GS_API void ApplyImpulse(Entity entity, const glm::vec3& impulse);
	GS_API void ApplyForce(Entity entity, const glm::vec3& force);
	GS_API glm::vec3 GetVelocity(Entity entity);
	GS_API void SetVelocity(Entity entity, const glm::vec3& velocity);
	GS_API bool IsTouching(Entity entity, const std::string& tag);

	// How many no-body warnings have been logged, for checking the once-only
	// rule.
	GS_API int WarningCount();

}
