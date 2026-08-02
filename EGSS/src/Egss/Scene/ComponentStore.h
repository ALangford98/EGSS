#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Scene/Entity.h"

namespace Egss {

	// Type-erased base, so a Scene can hold stores of different component
	// types in one container and still destroy an entity's components without
	// knowing what they are.
	class IComponentStore
	{
	public:
		virtual ~IComponentStore() = default;
		virtual void Remove(EntityId entity) = 0;
		virtual bool Has(EntityId entity) const = 0;
		virtual size_t Size() const = 0;
	};

	// Components of one type, stored contiguously.
	//
	// Two arrays kept in step: a dense array of the components themselves, and
	// a parallel array saying which entity owns each. A map takes an entity to
	// its slot.
	//
	// The point of the dense array is iteration. A system that walks every
	// sprite walks memory in order, with no gaps and no pointer chasing --
	// which is the whole reason to store components this way rather than
	// hanging them off an entity object.
	//
	// Removal swaps the last element into the hole rather than shifting, so it
	// is O(1). The cost is that **iteration order is not stable** and a removal
	// can move another component; do not hold a reference across one.
	template<typename T>
	class ComponentStore : public IComponentStore
	{
	public:
		T& Add(EntityId entity, const T& component)
		{
			auto existing = m_Index.find(entity);
			if (existing != m_Index.end())
			{
				m_Components[existing->second] = component;
				return m_Components[existing->second];
			}

			m_Index[entity] = m_Components.size();
			m_Components.push_back(component);
			m_Owners.push_back(entity);

			return m_Components.back();
		}

		T* Get(EntityId entity)
		{
			auto it = m_Index.find(entity);
			return it == m_Index.end() ? nullptr : &m_Components[it->second];
		}

		const T* Get(EntityId entity) const
		{
			auto it = m_Index.find(entity);
			return it == m_Index.end() ? nullptr : &m_Components[it->second];
		}

		bool Has(EntityId entity) const override { return m_Index.count(entity) != 0; }

		void Remove(EntityId entity) override
		{
			auto it = m_Index.find(entity);
			if (it == m_Index.end())
				return;

			size_t slot = it->second;
			size_t last = m_Components.size() - 1;

			// Swap the last component into the hole so the array stays dense.
			if (slot != last)
			{
				m_Components[slot] = std::move(m_Components[last]);
				m_Owners[slot] = m_Owners[last];
				m_Index[m_Owners[slot]] = slot;
			}

			m_Components.pop_back();
			m_Owners.pop_back();
			m_Index.erase(it);
		}

		size_t Size() const override { return m_Components.size(); }

		// Contiguous, for systems that want to walk everything.
		std::vector<T>& Components() { return m_Components; }
		const std::vector<T>& Components() const { return m_Components; }

		// Owner of the component at a dense index -- how a system gets back to
		// the entity while iterating.
		EntityId Owner(size_t index) const { return m_Owners[index]; }
	private:
		std::vector<T> m_Components;
		std::vector<EntityId> m_Owners;
		std::unordered_map<EntityId, size_t> m_Index;
	};

}
