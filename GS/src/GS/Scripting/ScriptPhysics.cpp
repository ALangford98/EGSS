#include "gspch.h"
#include "GS/Scripting/ScriptPhysics.h"
#include "GS/Scene/Components.h"

#include <unordered_set>

namespace GS::ScriptPhysics {

	namespace {
		Backend* s_Backend = nullptr;
		std::unordered_set<EntityId> s_Warned;
		int s_Warnings = 0;

		void Missing(Entity entity, const char* call)
		{
			if (!s_Warned.insert(entity.GetId()).second)
				return;
			s_Warnings++;
			const TagComponent* tag = entity.IsValid() ? entity.Get<TagComponent>() : nullptr;
			GS_CORE_WARN("{0} on '{1}' ignored: it has no physics body (give it a PhysicsComponent; bodies exist only in Play)",
				call, tag ? tag->Name : std::string("?"));
		}
	}

	void SetBackend(Backend* backend)
	{
		s_Backend = backend;
		s_Warned.clear();
	}

	void ApplyImpulse(Entity entity, const glm::vec3& impulse)
	{
		if (!s_Backend || !s_Backend->ApplyImpulse(entity.GetId(), impulse))
			Missing(entity, "applyImpulse");
	}

	void ApplyForce(Entity entity, const glm::vec3& force)
	{
		if (!s_Backend || !s_Backend->ApplyForce(entity.GetId(), force))
			Missing(entity, "applyForce");
	}

	glm::vec3 GetVelocity(Entity entity)
	{
		glm::vec3 velocity(0.0f);
		if (!s_Backend || !s_Backend->GetVelocity(entity.GetId(), velocity))
		{
			Missing(entity, "getVelocity");
			return glm::vec3(0.0f);
		}
		return velocity;
	}

	void SetVelocity(Entity entity, const glm::vec3& velocity)
	{
		if (!s_Backend || !s_Backend->SetVelocity(entity.GetId(), velocity))
			Missing(entity, "setVelocity");
	}

	bool IsTouching(Entity entity, const std::string& tag)
	{
		bool touching = false;
		if (!s_Backend || !s_Backend->IsTouching(entity.GetId(), tag, touching))
		{
			Missing(entity, "isTouching");
			return false;
		}
		return touching;
	}

	int WarningCount()
	{
		return s_Warnings;
	}

}
