#pragma once

// The Tools panel: everything that adds something to the scene, in sections
// -- Shapes, Scene (camera, light), Characters, Terrain, Import. See
// docs/superpowers/specs/2026-10-14-tools-characters-terrain-design.md.
//
// Buttons flow: each section's buttons share one width and wrap to the next
// line at the panel's edge. They used to sit on one SameLine row, which a
// narrow panel scrolled sideways and so hid.
//
// New things are placed where the editor camera is looking (on the ground
// when it is looking down at it), not at the origin -- which was usually
// behind the camera, or inside whatever was already there.

#include <GS.h>
#include <imgui.h>
#include <GS/Procedural/TerrainMesh.h>

#include <algorithm>
#include <cstring>

#include "EditorHistory.h"
#include "EditorSceneView.h"

class ToolsPanel
{
public:
	void OnImGuiRender()
	{
		ImGui::Begin("Tools");

		if (Section("Shapes"))
		{
			Flow flow({ "Cube", "Sphere", "Plane", "Cylinder", "Capsule" });
			if (flow.Button("Cube")) PlaceShape("primitive:cube", "Cube");
			if (flow.Button("Sphere")) PlaceShape("primitive:sphere", "Sphere");
			if (flow.Button("Plane")) PlaceShape("primitive:plane", "Plane");
			if (flow.Button("Cylinder")) PlaceShape("primitive:cylinder", "Cylinder");
			if (flow.Button("Capsule")) PlaceShape("primitive:capsule", "Capsule");
		}

		if (Section("Scene"))
		{
			Flow flow({ "Camera", "Light" });
			if (flow.Button("Camera")) PlaceCamera();
			Tip("A camera. The one marked Active is what Play and an exported game look through.");
			if (flow.Button("Light")) PlaceLight();
			Tip("A point light.");
		}

		if (Section("Characters"))
		{
			Flow flow({ "Player", "NPC" });
			if (flow.Button("Player")) PlaceCharacter(true);
			Tip("A humanoid that balances, walks, staggers, falls and gets up (Body in the Inspector for a capsule or ball).\n"
				"Played in Play and in an exported game: WASD to move, Space to jump, Shift to run;\n"
				"the camera follows -- right-drag or the arrow keys to orbit.\n"
				"Needs ground to stand on: a terrain, or anything with a static physics body.");
			if (flow.Button("NPC")) PlaceCharacter(false);
			Tip("A character that wanders, follows the player or flees it -- set in the Inspector.");
			if (CountPlayers() > 1)
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "More than one Player: input drives the first.");
		}

		if (Section("Terrain"))
			DrawTerrainSection();

		if (Section("Import"))
		{
			ImGui::TextDisabled(".obj files");
			ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Import").x
				- ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x));
			ImGui::InputText("##importpath", m_ImportPath, sizeof(m_ImportPath));
			ImGui::SameLine();
			if (ImGui::Button("Import"))
				PlaceShape(m_ImportPath, "Imported");
		}

		ImGui::End();
	}

private:
	// Buttons of one width that wrap at the panel's right edge.
	class Flow
	{
	public:
		Flow(std::initializer_list<const char*> labels)
		{
			for (const char* label : labels)
				m_Width = std::max(m_Width, ImGui::CalcTextSize(label).x);
			m_Width += ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
			m_Right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		}

		bool Button(const char* label)
		{
			if (!m_First)
			{
				float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + m_Width;
				if (next <= m_Right)
					ImGui::SameLine();
			}
			m_First = false;
			return ImGui::Button(label, ImVec2(m_Width, 0.0f));
		}

	private:
		float m_Width = 0.0f;
		float m_Right = 0.0f;
		bool m_First = true;
	};

	static bool Section(const char* name)
	{
		return ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
	}

	static void Tip(const char* text)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			ImGui::SetTooltip("%s", text);
	}

	static glm::vec3 PlacementPoint()
	{
		return g_EditorSceneView ? g_EditorSceneView->PlacementPoint() : glm::vec3(0.0f);
	}

	static void SelectPlaced(GS::EntityId entity)
	{
		if (g_EditorSceneView)
			g_EditorSceneView->Select(entity);
	}

	// A shape sitting on the ground at the placement point, not half sunk
	// into it: lifted by the bottom of its bounds.
	void PlaceShape(const std::string& path, const std::string& name)
	{
		std::shared_ptr<GS::Mesh> geometry = GS::MeshCache::Get(path);
		if (!geometry)
			return;
		EntitySnapshot snapshot;
		snapshot.Name = name;
		snapshot.Transform.Position = PlacementPoint() - glm::vec3(0.0f, std::min(0.0f, geometry->GetBoundsMin().y), 0.0f);
		GS::MeshComponent mesh;
		mesh.SourcePath = path;
		mesh.Geometry = geometry;
		snapshot.Mesh = mesh;
		SelectPlaced(EditorHistory::Push(std::make_unique<PlaceEntityCommand>(snapshot)));
	}

	void PlaceCamera()
	{
		EntitySnapshot snapshot;
		snapshot.Name = "Camera";
		snapshot.Transform.Position = PlacementPoint() + glm::vec3(0.0f, 1.6f, 0.0f);
		snapshot.Camera = GS::CameraComponent{};
		SelectPlaced(EditorHistory::Push(std::make_unique<PlaceEntityCommand>(snapshot)));
	}

	void PlaceLight()
	{
		EntitySnapshot snapshot;
		snapshot.Name = "Light";
		snapshot.Transform.Position = PlacementPoint() + glm::vec3(0.0f, 3.0f, 0.0f);
		snapshot.Light = GS::LightComponent{};
		SelectPlaced(EditorHistory::Push(std::make_unique<PlaceEntityCommand>(snapshot)));
	}

	// A humanoid -- the active ragdoll -- standing on the ground at the
	// placement point (a humanoid's position is the ground under its feet).
	// Blue for the player, orange for NPCs, so the two read apart at a
	// glance. The capsule mesh it carries is what it looks like if its Body
	// is switched to the upright capsule.
	void PlaceCharacter(bool player)
	{
		EntitySnapshot snapshot;
		snapshot.Name = player ? (CountPlayers() == 0 ? "Player" : "Player 2") : "NPC";
		snapshot.Transform.Position = PlacementPoint();
		snapshot.Transform.Scale = glm::vec3(0.8f, 0.9f, 0.8f);
		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:capsule";
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Color = player ? glm::vec4(0.25f, 0.55f, 0.95f, 1.0f) : glm::vec4(0.95f, 0.55f, 0.2f, 1.0f);
		snapshot.Mesh = mesh;
		if (player)
		{
			GS::PlayerComponent p;
			p.Body = 2;
			snapshot.Player = p;
		}
		else
		{
			GS::NpcComponent n;
			n.Body = 2;
			snapshot.Npc = n;
		}
		SelectPlaced(EditorHistory::Push(std::make_unique<PlaceEntityCommand>(snapshot)));
	}

	static int CountPlayers()
	{
		return (int)g_EditorScene.View<GS::PlayerComponent>().Size();
	}

	// --- Terrain -------------------------------------------------------------

	struct Preset
	{
		const char* Name;
		const char* Description;
		GS::TerrainComponent Terrain;
	};

	static const Preset* Presets(int& count)
	{
		auto make = [](float height, float frequency, int octaves, float roughness, bool ridged)
		{
			GS::TerrainComponent t;
			t.Height = height;
			t.Frequency = frequency;
			t.Octaves = octaves;
			t.Roughness = roughness;
			t.Ridged = ridged;
			return t;
		};
		static const Preset presets[] = {
			{ "Hills", "Rolling, rounded hills", make(0.06f, 3.0f, 5, 0.5f, false) },
			{ "Mountains", "Sharp ridges and peaks", make(0.16f, 2.5f, 6, 0.5f, true) },
			{ "Dunes", "Low, smooth swells", make(0.025f, 5.0f, 3, 0.35f, false) },
			{ "Rough", "Broken, rocky ground -- best on a cube or sphere", make(0.08f, 6.0f, 6, 0.6f, true) },
		};
		count = (int)(sizeof(presets) / sizeof(presets[0]));
		return presets;
	}

	void DrawTerrainSection()
	{
		int count = 0;
		const Preset* presets = Presets(count);
		m_Preset = std::clamp(m_Preset, 0, count - 1);

		ImGui::SetNextItemWidth(std::min(160.0f, ImGui::GetContentRegionAvail().x));
		if (ImGui::BeginCombo("Preset", presets[m_Preset].Name))
		{
			for (int i = 0; i < count; i++)
			{
				if (ImGui::Selectable(presets[i].Name, i == m_Preset))
					m_Preset = i;
				Tip(presets[i].Description);
			}
			ImGui::EndCombo();
		}
		ImGui::SetNextItemWidth(std::min(160.0f, ImGui::GetContentRegionAvail().x));
		ImGui::DragFloat("Size (m)", &m_TerrainSize, 1.0f, 2.0f, 2000.0f, "%.0f");

		Flow flow({ "New Terrain", "Apply to Selection" });
		if (flow.Button("New Terrain"))
			PlaceTerrain(presets[m_Preset].Terrain);
		Tip("A square of ground this size, walkable: a plane with terrain and a static physics body.");

		GS::EntityId selected = g_EditorSceneView ? g_EditorSceneView->Selected() : GS::InvalidEntity;
		auto* mesh = g_EditorScene.IsValid(selected) ? g_EditorScene.GetComponent<GS::MeshComponent>(selected) : nullptr;
		bool applicable = mesh && GS::TerrainMesh::Supports(mesh->SourcePath);
		ImGui::BeginDisabled(!applicable);
		if (flow.Button("Apply to Selection"))
			ApplyTerrain(selected, presets[m_Preset].Terrain);
		ImGui::EndDisabled();
		Tip(applicable ? "Gives the selected shape this terrain -- a sphere becomes a small planet, a cube a rock.\n"
				"Fine-tune it, and its vertex count, in the Inspector."
			: "Select a primitive shape (cube, sphere, plane, cylinder, capsule) first.");
	}

	void PlaceTerrain(GS::TerrainComponent terrain)
	{
		EntitySnapshot snapshot;
		snapshot.Name = "Terrain";
		glm::vec3 point = PlacementPoint();
		snapshot.Transform.Position = glm::vec3(point.x, 0.0f, point.z);
		// Uniform scale, so Height is a share of the size whatever it is:
		// 0.06 on 50 m is 3 m of relief.
		snapshot.Transform.Scale = glm::vec3(m_TerrainSize);
		terrain.Resolution = 128;
		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:plane";
		mesh.Color = glm::vec4(0.36f, 0.52f, 0.30f, 1.0f);
		mesh.Geometry = GS::TerrainMesh::Get(mesh.SourcePath, terrain);
		terrain.BuiltKey = GS::TerrainMesh::Key(mesh.SourcePath, terrain);
		snapshot.Mesh = mesh;
		snapshot.Terrain = terrain;
		GS::PhysicsComponent ground;
		ground.Type = GS::BodyType::Static;
		ground.Friction = 0.8f;
		snapshot.Physics = ground;
		SelectPlaced(EditorHistory::Push(std::make_unique<PlaceEntityCommand>(snapshot)));
	}

	// The selected shape gets the preset, at a resolution suited to it: a
	// plane needs more across than a closed shape needs per face. One undo
	// step, as a whole-entity replace -- the entity, its transform and
	// everything else on it unchanged.
	void ApplyTerrain(GS::EntityId entity, GS::TerrainComponent terrain)
	{
		auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
		if (!mesh)
			return;
		terrain.Resolution = mesh->SourcePath == "primitive:plane" ? 128 : 48;
		EntitySnapshot before = EntitySnapshot::Capture(entity);
		EntitySnapshot after = before;
		after.Terrain = terrain;
		after.Mesh->Geometry = GS::TerrainMesh::Get(after.Mesh->SourcePath, terrain);
		after.Terrain->BuiltKey = GS::TerrainMesh::Key(after.Mesh->SourcePath, terrain);
		SelectPlaced(EditorHistory::Push(std::make_unique<ReplaceEntityCommand>(entity, before, after)));
	}

	char m_ImportPath[256] = "assets/models/";
	int m_Preset = 0;
	float m_TerrainSize = 50.0f;
};
