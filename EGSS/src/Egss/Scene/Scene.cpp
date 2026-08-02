#include "egsspch.h"
#include "Egss/Scene/Scene.h"

#include "Egss/Debug/Instrumentor.h"

namespace Egss {

	Entity Scene::CreateEntity(const std::string& name)
	{
		unsigned int index;

		// Reuse a freed slot if there is one, so the generation array does not
		// grow without bound in a scene that churns entities.
		if (!m_FreeSlots.empty())
		{
			index = m_FreeSlots.back();
			m_FreeSlots.pop_back();
		}
		else
		{
			index = (unsigned int)m_Generations.size();
			// Generation starts at 1, so a valid handle is never 0 and
			// InvalidEntity stays unambiguous.
			m_Generations.push_back(1);
		}

		EntityId id = EntityIds::Make(index, m_Generations[index]);

		m_Live.push_back(id);
		m_LiveCount++;

		AddComponent<TagComponent>(id, { name });
		AddComponent<TransformComponent>(id, {});

		return Entity(this, id);
	}

	void Scene::DestroyEntity(EntityId entity)
	{
		if (!IsValid(entity))
			return;

		unsigned int index = EntityIds::Index(entity);

		// Bumping the generation is what makes every outstanding handle to
		// this entity stale, including ones the caller has stored elsewhere.
		m_Generations[index]++;
		if (m_Generations[index] > EntityIds::MaxGeneration)
			m_Generations[index] = 1;   // wrap; aliasing is now possible but 4096 reuses away

		m_FreeSlots.push_back(index);

		// Every store, without knowing what any of them hold -- the reason
		// IComponentStore exists.
		for (auto& [type, store] : m_Stores)
			store->Remove(entity);

		m_Live.erase(std::remove(m_Live.begin(), m_Live.end(), entity), m_Live.end());
		m_LiveCount--;
	}

	void Scene::Clear()
	{
		m_Stores.clear();
		m_Generations.clear();
		m_FreeSlots.clear();
		m_Live.clear();
		m_LiveCount = 0;
		m_Physics.Clear();
	}

	bool Scene::IsValid(EntityId entity) const
	{
		if (entity == InvalidEntity)
			return false;

		unsigned int index = EntityIds::Index(entity);
		if (index >= m_Generations.size())
			return false;

		// The slot exists, but is this the entity that currently owns it?
		return m_Generations[index] == EntityIds::Generation(entity);
	}

	Entity Scene::Wrap(EntityId entity)
	{
		return Entity(this, entity);
	}

	EntityId Scene::EntityAtIndex(unsigned int index) const
	{
		if (index >= m_Generations.size())
			return InvalidEntity;

		EntityId candidate = EntityIds::Make(index, m_Generations[index]);

		// The slot may have been freed without being reused, in which case the
		// generation is valid but no entity is live there.
		return IsValid(candidate) ? candidate : InvalidEntity;
	}

	void Scene::StepPhysics(float fixedStep)
	{
		EGSS_PROFILE_SCOPE("Scene::StepPhysics");

		ComponentStore<RigidBody2DComponent>& bodies = View<RigidBody2DComponent>();

		// Transforms that drive their body are pushed in *before* the step, or
		// the step would immediately overwrite them.
		for (size_t i = 0; i < bodies.Size(); i++)
		{
			RigidBody2DComponent& link = bodies.Components()[i];
			if (link.DrivenByPhysics)
				continue;

			TransformComponent* transform = GetComponent<TransformComponent>(bodies.Owner(i));
			if (!transform || link.Body >= m_Physics.GetBodyCount())
				continue;

			m_Physics.GetBody(link.Body).Position = glm::vec2(transform->Position);
		}

		m_Physics.Step(fixedStep);

		// ...and physics-driven bodies are read back out afterwards.
		for (size_t i = 0; i < bodies.Size(); i++)
		{
			RigidBody2DComponent& link = bodies.Components()[i];
			if (!link.DrivenByPhysics)
				continue;

			TransformComponent* transform = GetComponent<TransformComponent>(bodies.Owner(i));
			if (!transform || link.Body >= m_Physics.GetBodyCount())
				continue;

			const RigidBody2D& body = m_Physics.GetBody(link.Body);
			transform->Position = glm::vec3(body.Position, transform->Position.z);
		}
	}

}
