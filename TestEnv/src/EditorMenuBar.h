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
#include <cstring>

#include "Demo.h"
#include "DemoRegistry.h"
#include "EditorProject.h"
#include "EditorProjectDialogs.h"
#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorShell.h"
#include "FileBrowserPopup.h"
#include "MaterialEditorPanel.h"
#include "ExportDialog.h"
#include "PlayMode.h"
#include "SceneSwapGuard.h"

// Whether Ctrl+Z/Ctrl+Y mean the scene's history this frame. Not while a
// panel with its own history has focus -- the text editor (its buffer's
// undo) or the material editor (a graph is not the scene) -- since both
// would act on one keypress. The text editor's was missing: Ctrl+Z there
// undid the text *and* the last scene edit.
inline bool SceneOwnsUndoKeys()
{
	return !g_TextEditorFocused && !g_MaterialEditorFocused;
}

class EditorMenuBar : public GS::Layer
{
public:
	EditorMenuBar() : Layer("EditorMenuBar")
	{
		// The window's close button asks first when work would be lost.
		// With the UI hidden nothing could answer the question, so it quits.
		GS::Application::Get().SetCloseRequestHandler([]
		{
			if (GS::Application::Get().IsUIHidden())
				return true;
			return SceneSwapGuard::RequestAction([] { GS::Application::Get().Close(); }, true);
		});
	}

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

			// Ctrl+S saves the scene -- except where a panel saves its own
			// file on it (the text editor saves and builds the script, the
			// material editor saves the material), and during Play.
			if (e.GetKeyCode() == GS_KEY_S && SceneOwnsUndoKeys())
			{
				if (!PlayMode::IsPlaying() && !g_EditorScenePath.empty())
					SaveNow();
				return true;
			}

			if (SceneOwnsUndoKeys())
			{
				if (e.GetKeyCode() == GS_KEY_Z) { DoUndo(); return true; }
				if (e.GetKeyCode() == GS_KEY_Y) { DoRedo(); return true; }
			}
			// Not while the text editor itself is focused -- it has its own
			// Ctrl+F (find text, not find entity), checked by polling
			// rather than this event dispatch, and g_TextEditorFocused is
			// what keeps the two from both firing off the same keypress.
			if (e.GetKeyCode() == GS_KEY_F && !g_TextEditorFocused) { m_FindRequested = true; return true; }

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
			DrawPlayControls();
			ImGui::EndMainMenuBar();
		}

		UpdateTitle();
		DrawFileDialogs();
		DrawViewDialogs();
		DrawFindPopup();
		SceneSwapGuard::DrawUnsavedChangesModal();
	}

	void OnFixedUpdate(GS::Timestep step) override
	{
		if (g_ActiveDemo != InvalidDemo)
			return;
		PlayMode::OnFixedUpdate(step);
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

	void DrawFileMenu()
	{
		if (!ImGui::BeginMenu("File"))
			return;

		bool project = !g_EditorProjectPath.empty();
		bool playing = PlayMode::IsPlaying();

		if (ImGui::MenuItem("New Project..."))
			m_Projects.OpenNewProject();
		if (ImGui::MenuItem("Open Project..."))
			m_Projects.OpenOpenProject();
		if (ImGui::BeginMenu("Open Recent"))
		{
			const auto& recents = m_Projects.Recents();
			if (recents.empty())
				ImGui::TextDisabled("(none)");
			std::string open;
			for (const auto& recent : recents)
			{
				std::string label = recent.Name + "  --  " + recent.Path;
				if (ImGui::MenuItem(label.c_str(), nullptr, recent.Path == g_EditorProjectPath, recent.Exists))
					open = recent.Path;
			}
			if (!open.empty())
				m_Projects.RequestOpenProject(open);
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Close Project", nullptr, false, project && !playing))
			m_Projects.RequestCloseProject();

		ImGui::Separator();

		// Not during Play: the scene then is the running game, not the one
		// being edited -- Stop restores that.
		if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !g_EditorScenePath.empty() && !playing))
			SaveNow();
		if (ImGui::MenuItem("Save Scene As...", nullptr, false, project && !playing))
			m_Projects.OpenSaveSceneAs();

		ImGui::Separator();

		if (ImGui::MenuItem("Rename Project...", nullptr, false, project))
			m_Projects.OpenRenameProject();
		if (ImGui::MenuItem("Duplicate Project...", nullptr, false, project && !playing))
			m_Projects.OpenDuplicateProject();
		if (ImGui::MenuItem("Export Game...", nullptr, false, project && !playing))
			m_ExportDialog.Open();

		ImGui::Separator();

		if (ImGui::MenuItem("New Material...", nullptr, false, g_MaterialEditor != nullptr))
		{
			// Starts in the project, which is where a material the scene
			// links to belongs; the Browse button can go anywhere else.
			if (m_MaterialFolder[0] == '\0' && project)
			{
				strncpy(m_MaterialFolder, g_EditorProjectPath.c_str(), sizeof(m_MaterialFolder) - 1);
				m_MaterialFolder[sizeof(m_MaterialFolder) - 1] = '\0';
			}
			m_PendingPopup = "New Material";
		}

		// A demo's content, as a new scene of the open project -- every
		// scene belongs to a project, so this needs one.
		if (ImGui::BeginMenu("Add Demo Scene", project && !playing))
		{
			for (int i = 0; i < s_DemoCount; i++)
			{
				if (!s_Demos[i].CanOpenInEditor || !s_DemoInstances[i])
					continue;

				if (ImGui::MenuItem(s_Demos[i].Name))
				{
					SceneSwapGuard::RequestSceneSwap([this, i]
					{
						std::string name = UniqueSceneName(s_Demos[i].Name);
						std::string error;
						bool ok = CreateSceneInProject(name, error) && OpenProjectScene(name, error);
						SceneSwapGuard::ResetForNewScene();
						if (ok)
						{
							s_DemoInstances[i]->OnExportToScene(g_EditorScene);
							ok = SaveEditorScene(error);
						}
						if (!ok)
							m_Projects.ShowError(error);
					});
				}
			}
			ImGui::EndMenu();
		}

		ImGui::EndMenu();
	}

	// "<Project> -- <Scene>*  ·  GS Editor": what is open, and whether it
	// has unsaved changes, without opening a menu. Set only when it changes.
	void UpdateTitle()
	{
		std::string title;
		std::string scene = g_EditorScenePath.empty() ? std::string()
			: std::filesystem::path(g_EditorScenePath).stem().string() + (SceneSwapGuard::SceneIsDirty() ? "*" : "");
		if (!g_EditorProjectPath.empty())
			title = g_EditorProjectName + " -- " + scene + "  \xC2\xB7  GS Editor";
		else if (!scene.empty())
			title = scene + "  \xC2\xB7  GS Editor";
		else
			title = "GS Editor";
		if (title != m_Title)
		{
			m_Title = title;
			GS::Application::Get().GetWindow().SetTitle(title);
		}
	}

	// File > Save Scene and Ctrl+S.
	void SaveNow()
	{
		std::string error;
		if (!SceneSwapGuard::SaveAll(false, error))
			m_Projects.ShowError("Could not save: " + error);
	}

	void DrawFileDialogs()
	{
		// --open-export-dialog: opens it on the first frame -- how a capture
		// shows the dialog, since nothing here can click a menu.
		if (!m_CheckedExportFlag)
		{
			m_CheckedExportFlag = true;
			for (const std::string& argument : GS::Application::GetCommandLine())
				if (argument == "--open-export-dialog")
					m_ExportDialog.Open();
		}
		m_ExportDialog.Draw();
		m_Projects.Draw();

		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}

		if (ImGui::BeginPopupModal("New Material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_MaterialFolder, sizeof(m_MaterialFolder));
			ImGui::SameLine();
			if (ImGui::Button("Browse...##material"))
				m_FolderBrowser.Open("Choose Folder", FileBrowserPopup::Mode::PickFolder, m_MaterialFolder);
			ImGui::InputText("Name", m_MaterialName, sizeof(m_MaterialName));
			ImGui::TextDisabled("Creates <folder>/<name>.gsmat and opens it in the Material panel.");
			if (ImGui::Button("Create") && m_MaterialName[0] != '\0' && g_MaterialEditor
				&& g_MaterialEditor->NewMaterial((std::filesystem::path(m_MaterialFolder) / (std::string(m_MaterialName) + ".gsmat")).string()))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();

			// Inside the modal, so it stacks on it rather than closing it --
			// see EditorProjectDialogs::DrawBrowser.
			if (m_FolderBrowser.HasResult())
			{
				std::string picked = m_FolderBrowser.TakeResult();
				strncpy(m_MaterialFolder, picked.c_str(), sizeof(m_MaterialFolder) - 1);
				m_MaterialFolder[sizeof(m_MaterialFolder) - 1] = '\0';
			}
			m_FolderBrowser.Draw();
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

		// Brings the tab to the front wherever it's currently docked (or
		// floating) -- for a panel a user has dragged off somewhere and lost
		// track of, not a show/hide toggle (every panel here is always drawn
		// regardless; there is nothing to hide).
		if (ImGui::MenuItem("Appearance"))
			ImGui::SetWindowFocus("Appearance");
		if (ImGui::MenuItem("Material"))
			ImGui::SetWindowFocus("Material");

		ImGui::Separator();

		// A project setting, not an editor one: the exported game draws with
		// it too, so what the view shows is what the game will.
		if (ImGui::BeginMenu("Anti-aliasing"))
		{
			bool project = !g_EditorProjectPath.empty();
			bool forced = AntialiasingOverride() != 0;
			if (!project)
				ImGui::TextDisabled("A project setting -- open a project to change it.");
			if (forced)
				ImGui::TextDisabled("Set by --antialiasing for this run.");
			static const struct { const char* Label; unsigned int Samples; } levels[] = {
				{ "Off", 1 }, { "2x MSAA", 2 }, { "4x MSAA", 4 }, { "8x MSAA", 8 } };
			for (const auto& level : levels)
				if (ImGui::MenuItem(level.Label, nullptr, EditorAntialiasing() == level.Samples, project && !forced))
				{
					g_ProjectAntialiasing = level.Samples;
					PersistProjectScenes();
				}
			ImGui::EndMenu();
		}

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
			ImGui::InputText("Name", m_LayoutName, sizeof(m_LayoutName));
			if (ImGui::Button("Save"))
			{
				SaveLayoutPreset(m_LayoutName);
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

	// Plain buttons in the menu bar strip itself, not a dropdown -- always
	// visible, matching how a game engine editor's play button typically
	// works. Gated on g_ActiveDemo, same pattern Undo/Redo/Find already use:
	// Play mode operates on g_EditorScene, meaningless while looking at a
	// hardcoded C++ demo.
	void DrawPlayControls()
	{
		if (g_ActiveDemo != InvalidDemo)
			return;

		ImGui::Separator();
		if (!PlayMode::IsPlaying())
		{
			if (ImGui::Button("Play"))
				PlayMode::Play();
		}
		else
		{
			if (ImGui::Button("Stop"))
			{
				SceneSwapGuard::StopPlayKeepingUnsaved();
			}
		}
	}
private:
	char m_MaterialFolder[512] = "";
	char m_MaterialName[128] = "";
	char m_LayoutName[128] = "";
	FileBrowserPopup m_FolderBrowser;
	EditorProjectDialogs m_Projects;
	std::string m_Title;
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
	ExportDialog m_ExportDialog;
	bool m_CheckedExportFlag = false;
};
