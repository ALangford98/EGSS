#pragma once
// File > Export Game...: the editor's front end to GameExporter (export
// piece 3, docs/superpowers/specs/2026-10-07-game-runtime-player-design.md).
//
// The export runs on its own thread -- building the player takes about a
// minute, and the editor stays usable meanwhile. It reads the project from
// disk, so unsaved scene or material edits are offered a save first. No
// Cancel: stopping a make halfway is not worth the risk yet.
#include <GS.h>
#include <imgui.h>

#include "EditorHistory.h"
#include "EditorProject.h"
#include "FileBrowserPopup.h"
#include "GameExporter.h"
#include "MaterialEditorPanel.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// One export on a background thread. Everything the UI reads is copied out
// under the lock; the thread owns its own ScriptEngine, so nothing QuickJS
// is shared with the editor's.
class ExportJob
{
public:
	~ExportJob()
	{
		if (m_Thread.joinable())
			m_Thread.join();
	}

	bool Running() const { return m_Running; }

	void Start(GameExporter::Options options)
	{
		if (m_Running)
			return;
		if (m_Thread.joinable())
			m_Thread.join();
		m_Running = true;
		m_Finished = false;
		m_Started = std::chrono::steady_clock::now();
		SetStage("Starting");
		options.OnStage = [this](const std::string& stage) { SetStage(stage); };
		m_Thread = std::thread([this, options]() {
			ScriptEngine engine;
			GameExporter::Result result = GameExporter::Export(options, engine);
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Result = result;
			}
			m_Finished = true;
			m_Running = false;
		});
	}

	// Waits for the export to end -- for a caller with nothing else to do.
	void Wait()
	{
		if (m_Thread.joinable())
			m_Thread.join();
	}

	bool Finished() const { return m_Finished; }

	std::string Stage() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Stage;
	}

	GameExporter::Result Result() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Result;
	}

	float Seconds() const
	{
		return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_Started).count();
	}

private:
	void SetStage(const std::string& stage)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Stage = stage;
	}

	std::thread m_Thread;
	std::atomic<bool> m_Running{ false };
	std::atomic<bool> m_Finished{ false };
	mutable std::mutex m_Mutex;
	std::string m_Stage;
	GameExporter::Result m_Result;
	std::chrono::steady_clock::time_point m_Started;
};

class ExportDialog
{
public:
	void Open() { m_OpenRequested = true; }

	// Every frame, at the menu bar's root window scope (where its other
	// modals are drawn), whether or not Open() was called.
	void Draw()
	{
		if (m_OpenRequested)
		{
			m_OpenRequested = false;
			PrepareFields();
			ImGui::OpenPopup("Export Game");
		}

		ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
		if (ImGui::BeginPopupModal("Export Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (m_Job.Running())
				DrawRunning();
			else if (m_Job.Finished() && m_ShowResult)
				DrawResult();
			else if (m_AskingAboutUnsaved)
				DrawUnsaved();
			else
				DrawSettings();
			ImGui::EndPopup();
		}

		if (m_FolderBrowser.HasResult())
		{
			std::string picked = m_FolderBrowser.TakeResult();
			std::snprintf(m_OutDir, sizeof(m_OutDir), "%s", picked.c_str());
		}
		m_FolderBrowser.Draw();
	}

	ExportJob& Job() { return m_Job; }

private:
	static bool HasUnsavedWork()
	{
		return EditorHistory::IsDirty() || (g_MaterialEditor && g_MaterialEditor->HasUnsavedChanges());
	}

	void PrepareFields()
	{
		// A result stays up until dismissed (Export Again or Close), even
		// if the export finished while the dialog was hidden.
		m_AskingAboutUnsaved = false;
		if (m_OutDir[0] == '\0' && !g_EditorProjectPath.empty())
		{
			// Beside the project, not inside it: an export is not a project
			// asset, and the file tree should not fill up with packages.
			std::filesystem::path project = std::filesystem::path(g_EditorProjectPath).lexically_normal();
			if (project.filename().empty())
				project = project.parent_path();
			std::filesystem::path out = project.parent_path() / (project.filename().string() + " Exports");
			std::snprintf(m_OutDir, sizeof(m_OutDir), "%s", out.string().c_str());
		}
	}

	void DrawSettings()
	{
		ImGui::Text("Project: %s", g_EditorProjectPath.c_str());
		ImGui::Spacing();

		// Only Linux exists yet; the others are listed so the roadmap is
		// visible where the choice is made.
		static const char* targets[] = { "Linux (x86_64)", "Windows -- not yet", "Web -- not yet", "Android -- not yet" };
		if (ImGui::BeginCombo("Target", targets[0]))
		{
			for (int i = 0; i < 4; i++)
				ImGui::Selectable(targets[i], i == 0, i == 0 ? 0 : ImGuiSelectableFlags_Disabled);
			ImGui::EndCombo();
		}

		ImGui::InputText("Output folder", m_OutDir, sizeof(m_OutDir));
		ImGui::SameLine();
		if (ImGui::Button("Browse...##exportout"))
			m_FolderBrowser.Open("Export Into", FileBrowserPopup::Mode::PickFolder, m_OutDir);

		ImGui::RadioButton("Dist (smallest)", &m_Config, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Release (debug symbols)", &m_Config, 1);

		ImGui::Spacing();
		ImGui::BeginDisabled(g_EditorProjectPath.empty() || m_OutDir[0] == '\0');
		if (ImGui::Button("Export"))
		{
			if (HasUnsavedWork())
				m_AskingAboutUnsaved = true;
			else
				StartExport();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Close"))
			ImGui::CloseCurrentPopup();
	}

	void DrawUnsaved()
	{
		ImGui::TextUnformatted("The project has unsaved changes. The export reads the project from disk.");
		if (ImGui::Button("Save and Export"))
		{
			if (EditorHistory::IsDirty() && SaveEditorProject())
				EditorHistory::MarkClean();
			if (g_MaterialEditor && g_MaterialEditor->HasUnsavedChanges())
				g_MaterialEditor->SaveNow();
			StartExport();
		}
		ImGui::SameLine();
		if (ImGui::Button("Export Saved Version"))
			StartExport();
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			m_AskingAboutUnsaved = false;
	}

	void StartExport()
	{
		m_AskingAboutUnsaved = false;
		m_ShowResult = true;
		GameExporter::Options options;
		options.ProjectFolder = g_EditorProjectPath;
		options.OutDir = m_OutDir;
		options.Config = m_Config == 0 ? "dist" : "release";
		m_Job.Start(options);
	}

	void DrawRunning()
	{
		ImGui::Text("Exporting...  %.0f s", m_Job.Seconds());
		ImGui::TextDisabled("%s", m_Job.Stage().c_str());
		ImGui::Spacing();
		ImGui::TextDisabled("The editor stays usable -- close this and it carries on.");
		if (ImGui::Button("Hide"))
			ImGui::CloseCurrentPopup();
	}

	void DrawResult()
	{
		GameExporter::Result result = m_Job.Result();
		if (result.Ok)
		{
			ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Exported.");
			ImGui::TextWrapped("%s", result.PackageDir.c_str());
			ImGui::TextDisabled("%s", result.Archive.c_str());
			ImGui::TextDisabled("%s", result.AppImage.c_str());
			if (ImGui::Button("Open Folder"))
				Launch("xdg-open \"" + result.PackageDir + "\"");
			ImGui::SameLine();
			if (ImGui::Button("Run Game"))
				Launch("\"" + result.Executable + "\"");
			ImGui::SameLine();
		}
		else
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export failed.");
			ImGui::TextWrapped("%s", result.Error.c_str());
		}
		if (ImGui::Button("Export Again"))
			m_ShowResult = false;
		ImGui::SameLine();
		if (ImGui::Button("Close##result"))
			ImGui::CloseCurrentPopup();
	}

	// Detached: the editor must not wait on a game or a file manager.
	static void Launch(const std::string& command)
	{
		std::string detached = command + " > /dev/null 2>&1 &";
		if (std::system(detached.c_str()) != 0)
			GS_WARN("ExportDialog: could not run: {0}", command);
	}

	ExportJob m_Job;
	FileBrowserPopup m_FolderBrowser;
	char m_OutDir[1024] = {};
	int m_Config = 0;
	bool m_OpenRequested = false;
	bool m_AskingAboutUnsaved = false;
	bool m_ShowResult = false;
};
