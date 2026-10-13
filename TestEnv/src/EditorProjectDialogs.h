#pragma once

// The project dialogs (New, Open, Duplicate, Rename, Save Scene As) and the
// Projects window that shows while no project is open. Owned and drawn by
// EditorMenuBar; the File menu only asks for them.
// docs/superpowers/specs/2026-10-13-project-save-load-design.md.
//
// Each dialog keeps its own fields -- they used to share two buffers, so
// whatever was typed into one turned up in the next -- and shows its own
// error under the fields rather than only in the log.

#include <GS.h>
#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Demo.h"
#include "EditorProject.h"
#include "EditorRecentProjects.h"
#include "FileBrowserPopup.h"
#include "SceneSwapGuard.h"

class EditorProjectDialogs
{
public:
	void OpenNewProject()
	{
		Copy(m_NewName, "My Game");
		Copy(m_NewLocation, DefaultLocation());
		Ask("New Project");
	}

	void OpenOpenProject()
	{
		m_OpenPath[0] = '\0';
		Ask("Open Project");
	}

	void OpenDuplicateProject()
	{
		Copy(m_NewName, g_EditorProjectName + " copy");
		Copy(m_NewLocation, std::filesystem::path(g_EditorProjectPath).parent_path().string());
		Ask("Duplicate Project");
	}

	void OpenRenameProject()
	{
		Copy(m_NewName, g_EditorProjectName);
		Ask("Rename Project");
	}

	void OpenSaveSceneAs()
	{
		Copy(m_SceneName, UniqueSceneName(g_EditorScenePath.empty() ? std::string("Scene")
			: std::filesystem::path(g_EditorScenePath).stem().string() + "_copy"));
		Ask("Save Scene As");
	}

	// Opens a project, asking about unsaved work first. A failure is shown
	// in a popup, since the click that asked may have come from a menu.
	void RequestOpenProject(const std::string& path)
	{
		SceneSwapGuard::RequestAction([this, path]
		{
			std::string error;
			if (!OpenEditorProject(path, error))
				ShowError(error);
			SceneSwapGuard::ResetForNewScene();
			m_RecentStale = true;
		}, true);
	}

	void RequestCloseProject()
	{
		SceneSwapGuard::RequestAction([this]
		{
			CloseEditorProject();
			SceneSwapGuard::ResetForNewScene();
			m_RecentStale = true;
		}, true);
	}

	void ShowError(const std::string& error)
	{
		m_Error = error;
		m_ErrorPopup = true;
	}

	// The File > Open Recent entries, and the Projects window's list.
	struct Recent
	{
		std::string Path;
		std::string Name;
		bool Exists = false;
	};

	const std::vector<Recent>& Recents()
	{
		if (m_RecentStale)
		{
			m_Recents.clear();
			for (const std::string& path : RecentProjects::Load())
			{
				Recent recent;
				recent.Path = path;
				ProjectManifest manifest;
				recent.Exists = ReadProjectManifest(path, manifest);
				recent.Name = recent.Exists && !manifest.Name.empty()
					? manifest.Name : std::filesystem::path(path).filename().string();
				m_Recents.push_back(recent);
			}
			m_RecentStale = false;
		}
		return m_Recents;
	}

	void ForgetRecent(const std::string& path)
	{
		RecentProjects::Remove(path);
		m_RecentStale = true;
	}

	// Every frame, at the menu bar's root scope.
	void Draw()
	{
		DrawProjectsWindow();

		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}
		if (m_ErrorPopup)
		{
			ImGui::OpenPopup("Project Error");
			m_ErrorPopup = false;
		}

		DrawNewProject();
		DrawOpenProject();
		DrawDuplicateProject();
		DrawRenameProject();
		DrawSaveSceneAs();

		if (ImGui::BeginPopupModal("Project Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
			if (ImGui::Button("OK"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

	}

private:
	template<size_t N>
	static void Copy(char (&buffer)[N], const std::string& value) { Copy(buffer, N, value); }
	static void Copy(char* buffer, size_t size, const std::string& value)
	{
		strncpy(buffer, value.c_str(), size - 1);
		buffer[size - 1] = '\0';
	}

	void Ask(const char* popup)
	{
		m_Error.clear();
		m_PendingPopup = popup;
	}

	// Where new projects go by default: beside the most recent one, or
	// ~/GS Projects for the very first.
	static std::string DefaultLocation()
	{
		std::vector<std::string> recent = RecentProjects::Load();
		if (!recent.empty())
			return std::filesystem::path(recent.front()).parent_path().string();
		const char* home = std::getenv("HOME");
		return home ? (std::filesystem::path(home) / "GS Projects").string() : std::string(".");
	}

	static std::string Home()
	{
		const char* home = std::getenv("HOME");
		return home ? home : ".";
	}

	template<size_t N>
	void Browse(const char* title, FileBrowserPopup::Mode mode, char (&field)[N], const std::string& start, const char* filter = "")
	{
		m_BrowseInto = field;
		m_BrowseSize = N;
		std::error_code ec;
		std::string root = std::filesystem::is_directory(start, ec) ? start : Home();
		m_Browser.Open(title, mode, root, filter);
	}

	// The file browser, drawn *inside* the dialog that asked for it. Drawn
	// at the root instead -- as the old File dialogs did -- its OpenPopup
	// runs at popup level 0, and ImGui closes every popup above that level
	// to make room (ClosePopupToLevel in OpenPopupEx): clicking Browse
	// closed the dialog, and the folder then picked went nowhere.
	void DrawBrowser()
	{
		if (m_Browser.HasResult() && m_BrowseInto)
			Copy(m_BrowseInto, m_BrowseSize, m_Browser.TakeResult());
		m_Browser.Draw();
	}

	void ErrorLine()
	{
		if (!m_Error.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
	}

	// Name, location, and what that makes -- shared by New and Duplicate.
	bool NameAndLocation()
	{
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("Name", m_NewName, sizeof(m_NewName), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::InputText("Location", m_NewLocation, sizeof(m_NewLocation));
		ImGui::SameLine();
		if (ImGui::Button("Browse...##location"))
			Browse("Choose Location", FileBrowserPopup::Mode::PickFolder, m_NewLocation, m_NewLocation);
		ImGui::TextDisabled("Creates the folder %s", NewProjectFolder(m_NewLocation, m_NewName).string().c_str());
		ErrorLine();
		DrawBrowser();
		return enter;
	}

	void DrawNewProject()
	{
		ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("New Project", nullptr))
			return;
		bool enter = NameAndLocation();
		if (ImGui::Button("Create") || enter)
		{
			// Asked about unsaved work only once the input is known good:
			// the dialog closes for the guard's question, and a bad name
			// should be fixed here, not after it.
			std::string location = m_NewLocation, name = m_NewName;
			std::filesystem::path folder = NewProjectFolder(location, name);
			if (IsValidProjectName(name, m_Error) && !location.empty() && IsFreeFolder(folder, m_Error))
			{
				ImGui::CloseCurrentPopup();
				SceneSwapGuard::RequestAction([this, location, name]
				{
					std::string error;
					if (!CreateEditorProject(location, name, error))
						ShowError(error);
					SceneSwapGuard::ResetForNewScene();
					m_RecentStale = true;
				}, true);
			}
			else if (location.empty())
				m_Error = "Choose where the project folder should go.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void DrawOpenProject()
	{
		ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Open Project", nullptr))
			return;
		ImGui::TextDisabled("A project folder, or the project.gsproj inside it.");
		bool enter = ImGui::InputText("Project", m_OpenPath, sizeof(m_OpenPath), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Browse...##open"))
			Browse("Choose project.gsproj", FileBrowserPopup::Mode::PickFile, m_OpenPath,
				m_OpenPath[0] ? std::filesystem::path(m_OpenPath).parent_path().string() : DefaultLocation(), ".gsproj");
		ErrorLine();
		DrawBrowser();
		if (ImGui::Button("Open") || enter)
		{
			// Checked here so a wrong path is said in this dialog.
			std::filesystem::path folder(m_OpenPath);
			if (folder.filename() == ProjectManifestFilename())
				folder = folder.parent_path();
			std::error_code ec;
			if (!std::filesystem::is_regular_file(folder / ProjectManifestFilename(), ec))
				m_Error = "There is no " + std::string(ProjectManifestFilename()) + " in '" + folder.string() + "'.";
			else
			{
				ImGui::CloseCurrentPopup();
				RequestOpenProject(m_OpenPath);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void DrawDuplicateProject()
	{
		ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Duplicate Project", nullptr))
			return;
		ImGui::TextDisabled("Copies the whole project folder, as saved, and opens the copy.");
		bool enter = NameAndLocation();
		if (ImGui::Button("Duplicate") || enter)
		{
			std::string location = m_NewLocation, name = m_NewName;
			if (IsValidProjectName(name, m_Error) && IsFreeFolder(NewProjectFolder(location, name), m_Error))
			{
				ImGui::CloseCurrentPopup();
				SceneSwapGuard::RequestAction([this, location, name]
				{
					std::string error;
					if (!DuplicateEditorProject(location, name, error))
						ShowError(error);
					SceneSwapGuard::ResetForNewScene();
					m_RecentStale = true;
				}, true);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void DrawRenameProject()
	{
		if (!ImGui::BeginPopupModal("Rename Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("Name", m_NewName, sizeof(m_NewName), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::TextDisabled("The name shown and exported; the folder keeps its name.");
		ErrorLine();
		if ((ImGui::Button("Rename") || enter) && RenameEditorProject(m_NewName, m_Error))
		{
			m_RecentStale = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void DrawSaveSceneAs()
	{
		if (!ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("Scene name", m_SceneName, sizeof(m_SceneName), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::TextDisabled("A new scene in this project, saved as scenes/%s.txt, and opened.", m_SceneName);
		ErrorLine();
		if ((ImGui::Button("Save") || enter) && SaveSceneAs(m_SceneName, m_Error))
		{
			EditorHistory::MarkClean();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Shown while no project (and no loose scene) is open in the editor: an
	// ordinary window, not a modal, so the demo selector and the rest of
	// the UI stay usable around it.
	void DrawProjectsWindow()
	{
		bool show = g_EditorProjectPath.empty() && g_EditorScenePath.empty() && g_ActiveDemo == InvalidDemo;
		if (!show)
		{
			m_WasShowing = false;
			return;
		}
		if (!m_WasShowing)
			m_RecentStale = true;   // re-read on each appearance; projects come and go between
		m_WasShowing = true;

		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_Appearing);
		if (!ImGui::Begin("Projects", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
		{
			ImGui::End();
			return;
		}

		if (ImGui::Button("New Project..."))
			OpenNewProject();
		ImGui::SameLine();
		if (ImGui::Button("Open Project..."))
			OpenOpenProject();

		ImGui::Separator();
		ImGui::TextDisabled("Recent");
		const std::vector<Recent>& recents = Recents();
		if (recents.empty())
			ImGui::TextDisabled("Nothing yet -- create a project, or open one.");

		std::string open, forget;
		for (const Recent& recent : recents)
		{
			ImGui::PushID(recent.Path.c_str());
			ImGui::BeginDisabled(!recent.Exists);
			if (ImGui::Selectable(recent.Name.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
				ImVec2(ImGui::GetContentRegionAvail().x - 70.0f, 0.0f)))
				open = recent.Path;
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				forget = recent.Path;
			ImGui::TextDisabled("    %s%s", recent.Path.c_str(), recent.Exists ? "" : "  (missing)");
			ImGui::PopID();
		}
		ImGui::End();

		if (!open.empty())
			RequestOpenProject(open);
		if (!forget.empty())
			ForgetRecent(forget);
	}

	char m_NewName[128] = "";
	char m_NewLocation[512] = "";
	char m_OpenPath[512] = "";
	char m_SceneName[128] = "";
	std::string m_Error;
	bool m_ErrorPopup = false;
	const char* m_PendingPopup = nullptr;

	FileBrowserPopup m_Browser;
	char* m_BrowseInto = nullptr;
	size_t m_BrowseSize = 0;

	std::vector<Recent> m_Recents;
	bool m_RecentStale = true;
	bool m_WasShowing = false;
};
