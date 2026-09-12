#pragma once

// Pure, ImGui-free helpers over g_EditorScene for group membership and
// multi-selection centroid math -- kept separate from EditorSceneView.h so
// the logic here can be exercised by a temporary self-test without a live
// ImGui frame, the same separation EditableMesh.h uses for mesh authoring.
// See docs/superpowers/specs/2026-09-12-entity-grouping-design.md.

#include <GS.h>

#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

#include "EditorProject.h"

namespace EntityGroups {

	// Alphabetical by group name (std::map, not unordered) -- a reasonable
	// free default for display order, and irrelevant at editor-scene sizes.
	inline std::map<std::string, std::vector<GS::EntityId>> GroupEntitiesByName()
	{
		std::map<std::string, std::vector<GS::EntityId>> groups;
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (!group->GroupName.empty())
					groups[group->GroupName].push_back(entity);
		return groups;
	}

	inline bool AnyAlreadyGrouped(const std::vector<GS::EntityId>& entities)
	{
		for (GS::EntityId entity : entities)
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (!group->GroupName.empty())
					return true;
		return false;
	}

	// Overwrites, not adds to, any existing membership -- single-group-
	// membership rule from the spec applies uniformly, whether this is
	// called from Ctrl+G or the management dialog.
	inline void AssignGroup(const std::vector<GS::EntityId>& entities, const std::string& name)
	{
		for (GS::EntityId entity : entities)
			g_EditorScene.AddComponent<GS::GroupComponent>(entity, GS::GroupComponent{ name });
	}

	inline void RenameGroup(const std::string& oldName, const std::string& newName)
	{
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (group->GroupName == oldName)
					group->GroupName = newName;
	}

	inline void DeleteGroup(const std::string& name)
	{
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (group->GroupName == name)
					g_EditorScene.RemoveComponent<GS::GroupComponent>(entity);
	}

	inline glm::vec3 ComputeCentroid(const std::vector<glm::vec3>& positions)
	{
		if (positions.empty())
			return glm::vec3(0.0f);
		glm::vec3 sum(0.0f);
		for (const glm::vec3& p : positions)
			sum += p;
		return sum / (float)positions.size();
	}

}
