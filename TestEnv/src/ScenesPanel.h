#pragma once

// The "Scenes" panel: browse, create, duplicate, rename, and delete a
// project's scene files, and switch which one is loaded. See
// docs/superpowers/specs/2026-09-13-scene-tree-design.md.

#include <GS.h>
#include <imgui.h>
#include <cstring>
#include <filesystem>

#include "EditorProject.h"
#include "SceneSwapGuard.h"

class ScenesPanel : public GS::Layer
{
public:
	ScenesPanel() : Layer("ScenesPanel") {}

	void OnImGuiRender() override
	{
		ImGui::Begin("Scenes");

		if (g_EditorProjectPath.empty())
		{
			ImGui::TextDisabled("Open or create a project to manage its scenes.");
			ImGui::End();
			return;
		}

		for (const SceneEntry& entry : g_ProjectScenes)
		{
			ImGui::PushID(entry.Name.c_str());

			std::string fullPath = (std::filesystem::path(g_EditorProjectPath) / entry.RelativePath).string();
			bool isActive = (fullPath == g_EditorScenePath);

			if (isActive)
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", entry.Name.c_str());
			else
				ImGui::Text("%s", entry.Name.c_str());

			ImGui::SameLine();
			if (!isActive && ImGui::SmallButton("Open"))
			{
				std::string path = fullPath;
				SceneSwapGuard::RequestSceneSwap([path]
				{
					OpenEditorScene(path);
					SceneSwapGuard::ResetForNewScene();
				});
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Duplicate"))
			{
				m_DuplicateSource = entry.Name;
				std::string suggested = entry.Name + "_copy";
				strncpy(m_NameBuf, suggested.c_str(), sizeof(m_NameBuf) - 1);
				m_NameBuf[sizeof(m_NameBuf) - 1] = '\0';
				m_PendingPopup = "Duplicate Scene";
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Rename"))
			{
				m_RenameTarget = entry.Name;
				strncpy(m_NameBuf, entry.Name.c_str(), sizeof(m_NameBuf) - 1);
				m_NameBuf[sizeof(m_NameBuf) - 1] = '\0';
				m_PendingPopup = "Rename Scene";
			}

			ImGui::SameLine();
			bool canDelete = !isActive && g_ProjectScenes.size() > 1;
			if (!canDelete) ImGui::BeginDisabled();
			if (ImGui::SmallButton("Delete"))
			{
				m_DeleteTarget = entry.Name;
				m_PendingPopup = "Delete Scene";
			}
			if (!canDelete) ImGui::EndDisabled();

			ImGui::PopID();
		}

		ImGui::Separator();
		if (ImGui::Button("New Scene..."))
		{
			m_NameBuf[0] = '\0';
			m_PendingPopup = "New Scene";
		}

		DrawDialogs();
		ImGui::End();
	}

private:
	// Same ID-scope reasoning as EditorMenuBar's own m_PendingPopup:
	// OpenPopup must run at the same root scope as the matching
	// BeginPopupModal, so the actual OpenPopup call is deferred to here
	// rather than fired from inside a SmallButton's own callback above.
	void DrawDialogs()
	{
		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}

		if (ImGui::BeginPopupModal("New Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Create") && CreateSceneInProject(m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Duplicate Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("New Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Duplicate") && DuplicateSceneInProject(m_DuplicateSource, m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Rename Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("New Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Rename") && RenameSceneInProject(m_RenameTarget, m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		// The one destructive, irreversible action in this panel -- unlike
		// everything else here, this gets its own confirmation even though
		// most editor actions don't ask before running.
		if (ImGui::BeginPopupModal("Delete Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Permanently delete '%s'? This cannot be undone.", m_DeleteTarget.c_str());
			if (ImGui::Button("Delete") && DeleteSceneFromProject(m_DeleteTarget))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	char m_NameBuf[128] = "";
	std::string m_DuplicateSource, m_RenameTarget, m_DeleteTarget;
	const char* m_PendingPopup = nullptr;
};
