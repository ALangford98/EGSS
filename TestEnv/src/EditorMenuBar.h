#pragma once

// File/Edit/View/Help, and the three things they actually do: project I/O,
// undo/redo, and named layout presets. Kept out of EditorShell.h on
// purpose -- that file lays out panels, this one is chrome above them, and
// the two must not be confused about what "the layout" means: BuildLayout
// builds the panel arrangement, this file's layout presets save and
// restore it.
//
// Pushed before EditorShell in TestApp.cpp, and that order is load-bearing:
// ImGui::BeginMainMenuBar() shrinks the main viewport's work area for the
// rest of the frame, and EditorShell's dockspace reads that work area to
// size itself. Rendered after EditorShell instead, the dockspace would
// claim the strip the menu bar draws over.

#include <GS.h>
#include <imgui.h>
#include <fstream>
#include <cctype>

#include "Demo.h"
#include "DemoRegistry.h"
#include "EditorProject.h"
#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorShell.h"

class EditorMenuBar : public GS::Layer
{
public:
	EditorMenuBar() : Layer("EditorMenuBar") {}

	void OnEvent(GS::Event& e) override
	{
		GS::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			// Undo/Redo/Find all act on g_EditorScene, which is invisible
			// while a demo owns the viewport -- the same reasoning
			// EditorSceneView::IsActive() already applies to its own
			// event handling.
			if (g_ActiveDemo != InvalidDemo)
				return false;

			bool ctrl = GS::Input::IsKeyPressed(GS_KEY_LEFT_CONTROL)
				|| GS::Input::IsKeyPressed(GS_KEY_RIGHT_CONTROL);
			if (!ctrl)
				return false;

			if (e.GetKeyCode() == GS_KEY_Z) { DoUndo(); return true; }
			if (e.GetKeyCode() == GS_KEY_Y) { DoRedo(); return true; }
			if (e.GetKeyCode() == GS_KEY_F) { m_FindRequested = true; return true; }

			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (!g_EditorShell)
			return;

		if (ImGui::BeginMainMenuBar())
		{
			DrawFileMenu();
			DrawEditMenu();
			DrawViewMenu();
			DrawHelpMenu();
			ImGui::EndMainMenuBar();
		}

		DrawFileDialogs();
		DrawViewDialogs();
		DrawFindPopup();
	}
private:
	void DoUndo()
	{
		GS::EntityId selection = EditorHistory::Undo();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	void DoRedo()
	{
		GS::EntityId selection = EditorHistory::Redo();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	// Every action that swaps the scene out from under the editor (New,
	// Open, Open Demo) needs undo history and selection cleared -- that
	// part is centralised here so no call site can forget it (Task 2's own
	// comment about a stale selection aliasing an unrelated entity is
	// exactly the bug this prevents recurring). Resetting the project
	// globals (g_EditorProjectPath/g_EditorProjectName) is each call site's
	// own responsibility instead: New/Open Project already get the correct
	// values from CreateEditorProject/OpenEditorProject themselves, so
	// clearing them here would immediately wipe out what those functions
	// just set. Open Demo has no project of its own, so it clears them
	// directly at its own call site.
	void ResetForNewScene()
	{
		EditorHistory::Clear();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(GS::InvalidEntity);
	}

	void DrawFileMenu()
	{
		if (!ImGui::BeginMenu("File"))
			return;

		if (ImGui::MenuItem("New Project..."))
			m_PendingPopup = "New Project";
		if (ImGui::MenuItem("Open Project..."))
			m_PendingPopup = "Open Project";

		ImGui::Separator();

		// "Save" works with or without an open Project: with one, it goes
		// through the manifest; without one (a bare --scene session), it
		// falls through to the same SaveEditorScene a --scene boot always
		// had -- the spec calls this "additive... not a replacement".
		if (ImGui::MenuItem("Save", nullptr, false, !g_EditorScenePath.empty()))
		{
			if (!g_EditorProjectPath.empty())
				SaveEditorProject();
			else
				SaveEditorScene(g_EditorScenePath);
		}
		if (ImGui::MenuItem("Save As..."))
			m_PendingPopup = "Save Project As";
		if (ImGui::MenuItem("Rename Project...", nullptr, false, !g_EditorProjectPath.empty()))
			m_PendingPopup = "Rename Project";

		ImGui::Separator();

		if (ImGui::BeginMenu("Open Demo"))
		{
			for (int i = 0; i < s_DemoCount; i++)
			{
				if (!s_Demos[i].CanOpenInEditor || !s_DemoInstances[i])
					continue;

				if (ImGui::MenuItem(s_Demos[i].Name))
				{
					g_EditorScene.Clear();
					g_EditorScenePath.clear();
					// A demo's exported content isn't a Project (no folder, no
					// manifest) -- leaving the previous project's path/name
					// behind would keep Save/Rename Project enabled while
					// pointing at a manifest that no longer matches what's
					// on screen.
					g_EditorProjectPath.clear();
					g_EditorProjectName.clear();
					ResetForNewScene();
					s_DemoInstances[i]->OnExportToScene(g_EditorScene);
				}
			}
			ImGui::EndMenu();
		}

		ImGui::EndMenu();
	}

	void DrawFileDialogs()
	{
		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}

		if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Create") && CreateEditorProject(m_DialogPath, m_DialogName))
			{
				ResetForNewScene();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			if (ImGui::Button("Open"))
			{
				bool opened = OpenEditorProject(m_DialogPath);
				// GS::Scene::Load clears the scene before returning false
				// on a wrong tag/version, so a failed open can still have
				// wiped g_EditorScene -- history/selection must be reset
				// regardless of whether the open itself succeeded, or
				// Undo/Redo and the current selection end up referencing
				// entities from a scene that no longer exists.
				ResetForNewScene();
				if (opened)
					ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Save Project As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Save") && SaveEditorProjectAs(m_DialogPath, m_DialogName))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Rename Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Rename") && RenameEditorProject(m_DialogName))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void DrawEditMenu()
	{
		if (!ImGui::BeginMenu("Edit"))
			return;

		// Same reasoning as OnEvent's key dispatch above.
		bool editorActive = (g_ActiveDemo == InvalidDemo);

		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editorActive && EditorHistory::CanUndo()))
			DoUndo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, editorActive && EditorHistory::CanRedo()))
			DoRedo();

		ImGui::Separator();

		if (ImGui::MenuItem("Find", "Ctrl+F", false, editorActive))
			m_FindRequested = true;

		ImGui::EndMenu();
	}

	void DrawFindPopup()
	{
		if (m_FindRequested)
		{
			ImGui::OpenPopup("Find Entity");
			m_FindRequested = false;
			m_FindQuery[0] = '\0';
		}

		if (!ImGui::BeginPopup("Find Entity"))
			return;

		ImGui::SetKeyboardFocusHere();
		ImGui::InputText("##findquery", m_FindQuery, sizeof(m_FindQuery));

		std::string query = m_FindQuery;
		for (char& c : query)
			c = (char)std::tolower((unsigned char)c);

		for (GS::EntityId entity : g_EditorScene.GetEntities())
		{
			auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
			if (!tag)
				continue;

			std::string lowerName = tag->Name;
			for (char& c : lowerName)
				c = (char)std::tolower((unsigned char)c);

			if (!query.empty() && lowerName.find(query) == std::string::npos)
				continue;

			if (ImGui::Selectable(tag->Name.c_str()))
			{
				if (g_EditorSceneView)
					g_EditorSceneView->Select(entity);
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::EndPopup();
	}

	// --- Layout presets ------------------------------------------------
	// A tagged text file, same convention as the rest of this project's
	// hand-written formats: one header line, then repeated `layout <name>`
	// / body lines / `end` blocks. A bare "end" line inside a real ImGui
	// ini body is not a pattern ImGui's own format produces, which is why
	// this is safe without escaping -- the same kind of accepted, checked
	// assumption as this project's other hand-rolled formats.
	struct LayoutPreset { std::string Name; std::string Ini; };

	static const char* LayoutPresetsPath() { return "editor_layouts.txt"; }

	static std::vector<LayoutPreset> ReadLayoutPresets()
	{
		std::vector<LayoutPreset> presets;
		std::ifstream in(LayoutPresetsPath());
		if (!in)
			return presets;

		std::string tag;
		int version = 0;
		in >> tag >> version;
		if (tag != "gs-layouts" || version != 1)
			return presets;

		std::string line;
		std::getline(in, line);

		LayoutPreset* current = nullptr;
		while (std::getline(in, line))
		{
			if (line.rfind("layout ", 0) == 0)
			{
				presets.push_back({ line.substr(7), "" });
				current = &presets.back();
			}
			else if (line == "end")
			{
				current = nullptr;
			}
			else if (current)
			{
				current->Ini += line;
				current->Ini += "\n";
			}
		}

		return presets;
	}

	static void WriteLayoutPresets(const std::vector<LayoutPreset>& presets)
	{
		std::ofstream out(LayoutPresetsPath());
		if (!out)
			return;

		out << "gs-layouts 1\n";
		for (const LayoutPreset& preset : presets)
		{
			out << "layout " << preset.Name << "\n";
			out << preset.Ini;
			out << "end\n";
		}
	}

	void SaveLayoutPreset(const std::string& name)
	{
		std::vector<LayoutPreset> presets = ReadLayoutPresets();

		size_t iniSize = 0;
		const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
		std::string iniString(ini, iniSize);

		for (LayoutPreset& preset : presets)
		{
			if (preset.Name == name)
			{
				preset.Ini = iniString;
				WriteLayoutPresets(presets);
				return;
			}
		}

		presets.push_back({ name, iniString });
		WriteLayoutPresets(presets);
	}

	void LoadLayoutPreset(const std::string& name)
	{
		for (const LayoutPreset& preset : ReadLayoutPresets())
		{
			if (preset.Name == name)
			{
				ImGui::LoadIniSettingsFromMemory(preset.Ini.c_str(), preset.Ini.size());
				return;
			}
		}
	}

	void DrawViewMenu()
	{
		if (!ImGui::BeginMenu("View"))
			return;

		for (const LayoutPreset& preset : ReadLayoutPresets())
			if (ImGui::MenuItem(preset.Name.c_str()))
				LoadLayoutPreset(preset.Name);

		ImGui::Separator();

		if (ImGui::MenuItem("Save current layout as..."))
			m_PendingPopup = "Save Layout";
		if (ImGui::MenuItem("Reset to default") && g_EditorShellInstance)
			g_EditorShellInstance->ResetToDefaultLayout();

		ImGui::EndMenu();
	}

	// Same ID-scope reasoning as DrawFileDialogs -- BeginPopupModal has to
	// run at the same root scope m_PendingPopup's OpenPopup call runs in
	// (inside DrawFileDialogs), not nested inside DrawViewMenu's own
	// BeginMenu/EndMenu scope.
	void DrawViewDialogs()
	{
		if (ImGui::BeginPopupModal("Save Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Save"))
			{
				SaveLayoutPreset(m_DialogName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void DrawHelpMenu()
	{
		if (!ImGui::BeginMenu("Help"))
			return;

		ImGui::Text("GS editor");
		ImGui::TextDisabled("Built from source -- no version number yet.");

		ImGui::EndMenu();
	}
private:
	char m_DialogPath[256] = "";
	char m_DialogName[128] = "";
	char m_FindQuery[128] = "";
	bool m_FindRequested = false;
	// OpenPopup must run at the same ID-stack scope as the matching
	// BeginPopupModal, or ImGui hashes two different ids for the "same"
	// popup and it silently never opens. Calling OpenPopup from inside a
	// File/View-menu MenuItem callback (a different window scope than
	// DrawFileDialogs'/DrawViewDialogs' root-level one) hit exactly that.
	// Deferring the actual OpenPopup call to here is the same fix already
	// used for the Find popup via m_FindRequested, below.
	const char* m_PendingPopup = nullptr;
};
