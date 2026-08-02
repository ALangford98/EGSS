#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Scene/Entity.h"
#include "Egss/Scene/ComponentStore.h"
#include "Egss/Scene/Components.h"
#include "Egss/Physics/PhysicsWorld2D.h"

#include <typeindex>

namespace Egss {

	class Entity;

	// Owns entities and their components.
	//
	// Deliberately small. There is no component registration, no archetypes,
	// no system scheduler -- a component is any struct, and a "system" is a
	// loop somewhere else that asks for a store and walks it. That is enough
	// to give things identity and let the renderer, physics, audio and
	// lighting all attach to the same object, which is the whole reason this
	// exists.
	//
	// A real ECS would sort entities by which components they have so that
	// iterating two component types together stays contiguous. Worth doing
	// when a profile says so, and not before.
	class EGSS_API Scene
	{
	public:
		Entity CreateEntity(const std::string& name = "Entity");
		void DestroyEntity(EntityId entity);
		void Clear();

		// False for a handle whose entity has been destroyed -- including one
		// whose slot has since been reused by a different entity.
		bool IsValid(EntityId entity) const;

		size_t GetEntityCount() const { return m_LiveCount; }

		// Every live entity, in creation order.
		const std::vector<EntityId>& GetEntities() const { return m_Live; }

		Entity Wrap(EntityId entity);

		// The live handle for a slot, or InvalidEntity if that slot is empty.
		//
		// This exists for picking. A full EntityId does not survive a round
		// trip through a signed 32-bit integer texture: once the generation
		// climbs past 2047 the handle exceeds INT_MAX and reads back negative.
		// So the picking buffer stores the *slot index* -- always small and
		// positive -- and this turns it back into a handle.
		EntityId EntityAtIndex(unsigned int index) const;

		// --- Components -------------------------------------------------
		template<typename T>
		T& AddComponent(EntityId entity, const T& component = T())
		{
			return Store<T>().Add(entity, component);
		}

		template<typename T>
		T* GetComponent(EntityId entity)
		{
			return IsValid(entity) ? Store<T>().Get(entity) : nullptr;
		}

		template<typename T>
		bool HasComponent(EntityId entity)
		{
			return IsValid(entity) && Store<T>().Has(entity);
		}

		template<typename T>
		void RemoveComponent(EntityId entity)
		{
			Store<T>().Remove(entity);
		}

		// The whole contiguous store, for systems that walk one component type.
		template<typename T>
		ComponentStore<T>& View()
		{
			return Store<T>();
		}

		// --- Physics ----------------------------------------------------
		// The scene owns a physics world and keeps transforms in step with it.
		PhysicsWorld2D& GetPhysics() { return m_Physics; }
		const PhysicsWorld2D& GetPhysics() const { return m_Physics; }

		// Steps physics, then copies body positions into transforms (or the
		// other way round for bodies the transform drives). Call from
		// OnFixedUpdate.
		void StepPhysics(float fixedStep);
	private:
		template<typename T>
		ComponentStore<T>& Store()
		{
			std::type_index type(typeid(T));

			auto it = m_Stores.find(type);
			if (it == m_Stores.end())
				it = m_Stores.emplace(type, std::make_unique<ComponentStore<T>>()).first;

			return *static_cast<ComponentStore<T>*>(it->second.get());
		}
	private:
		// Generation per slot. A slot's generation is bumped on destroy, which
		// is what invalidates outstanding handles to it.
		std::vector<unsigned int> m_Generations;
		std::vector<unsigned int> m_FreeSlots;
		std::vector<EntityId> m_Live;
		size_t m_LiveCount = 0;

		std::unordered_map<std::type_index, std::unique_ptr<IComponentStore>> m_Stores;

		PhysicsWorld2D m_Physics;
	};

	// A handle plus the scene it belongs to, so call sites read as
	// `entity.Add<SpriteComponent>()` rather than threading the scene through
	// everywhere. It is a value: copying one is free and it owns nothing.
	class EGSS_API Entity
	{
	public:
		Entity() = default;
		Entity(Scene* scene, EntityId id)
			: m_Scene(scene), m_Id(id) {}

		EntityId GetId() const { return m_Id; }
		bool IsValid() const { return m_Scene && m_Scene->IsValid(m_Id); }
		explicit operator bool() const { return IsValid(); }

		template<typename T>
		T& Add(const T& component = T()) { return m_Scene->AddComponent<T>(m_Id, component); }

		template<typename T>
		T* Get() { return m_Scene->GetComponent<T>(m_Id); }

		template<typename T>
		bool Has() { return m_Scene->HasComponent<T>(m_Id); }

		template<typename T>
		void Remove() { m_Scene->RemoveComponent<T>(m_Id); }

		bool operator==(const Entity& other) const { return m_Id == other.m_Id && m_Scene == other.m_Scene; }
		bool operator!=(const Entity& other) const { return !(*this == other); }
	private:
		Scene* m_Scene = nullptr;
		EntityId m_Id = InvalidEntity;
	};

}
