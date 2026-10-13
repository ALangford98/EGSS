#pragma once

// The "Scenes" panel: the open project's scenes -- switch, create,
// duplicate, rename, delete. See
// docs/superpowers/specs/2026-09-13-scene-tree-design.md and, for how it
// fits the project model, 2026-10-13-project-save-load-design.md.

#include <GS.h>
#include <imgui.h>
#include <cstring>
#include <filesystem>

#include "EditorProject.h"
#include "PlayMode.h"
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
			ImGui::TextDisabled("Open or create a project to see its scenes.");
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("%s", g_EditorProjectName.c_str());
		bool playing = PlayMode::IsPlaying();

		for (const SceneEntry& entry : g_ProjectScenes)
		{
			ImGui::PushID(entry.Name.c_str());
			bool isActive = ProjectScenePath(entry) == g_EditorScenePath;

			// The row itself opens the scene -- a click, as a file list does.
			ImGui::BeginDisabled(playing && !isActive);
			if (ImGui::Selectable(entry.Name.c_str(), isActive, ImGuiSelectableFlags_AllowOverlap,
				ImVec2(ImGui::GetContentRegionAvail().x - 150.0f, 0.0f)) && !isActive)
			{
				std::string name = entry.Name;
				SceneSwapGuard::RequestSceneSwap([this, name]
				{
					if (!OpenProjectScene(name, m_Error))
						m_ErrorPopup = true;
					SceneSwapGuard::ResetForNewScene();
				});
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::SmallButton("Duplicate"))
			{
				m_Target = entry.Name;
				SetName(UniqueSceneName(entry.Name + "_copy"));
				Ask("Duplicate Scene");
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Rename"))
			{
				m_Target = entry.Name;
				SetName(entry.Name);
				Ask("Rename Scene");
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(isActive || g_ProjectScenes.size() <= 1);
			if (ImGui::SmallButton("Delete"))
			{
				m_Target = entry.Name;
				Ask("Delete Scene");
			}
			ImGui::EndDisabled();

			ImGui::PopID();
		}

		ImGui::Separator();
		if (ImGui::Button("New Scene..."))
		{
			SetName(UniqueSceneName("Scene"));
			Ask("New Scene");
		}

		DrawDialogs();
		ImGui::End();
	}

private:
	void SetName(const std::string& name)
	{
		strncpy(m_NameBuf, name.c_str(), sizeof(m_NameBuf) - 1);
		m_NameBuf[sizeof(m_NameBuf) - 1] = '\0';
	}

	void Ask(const char* popup)
	{
		m_Error.clear();
		m_PendingPopup = popup;
	}

	// One name field, a confirm button that runs `action`, the error it gave.
	template<typename Action>
	void NameDialog(const char* title, const char* confirm, Action action)
	{
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::TextDisabled("Saved as scenes/%s.txt", m_NameBuf);
		if (!m_Error.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
		if ((ImGui::Button(confirm) || enter) && action())
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Same ID-scope reasoning as EditorMenuBar's own m_PendingPopup:
	// OpenPopup must run at the same root scope as the matching
	// BeginPopupModal, so the actual OpenPopup call is deferred to here.
	void DrawDialogs()
	{
		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}
		if (m_ErrorPopup)
		{
			ImGui::OpenPopup("Scene Error");
			m_ErrorPopup = false;
		}

		NameDialog("New Scene", "Create", [this] { return CreateSceneInProject(m_NameBuf, m_Error); });
		NameDialog("Duplicate Scene", "Duplicate", [this] { return DuplicateSceneInProject(m_Target, m_NameBuf, m_Error); });
		NameDialog("Rename Scene", "Rename", [this] { return RenameSceneInProject(m_Target, m_NameBuf, m_Error); });

		// The one destructive, irreversible action in this panel, so it asks.
		if (ImGui::BeginPopupModal("Delete Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Permanently delete scene '%s' and its file? This cannot be undone.", m_Target.c_str());
			if (!m_Error.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
			if (ImGui::Button("Delete") && DeleteSceneFromProject(m_Target, m_Error))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Scene Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_Error.c_str());
			if (ImGui::Button("OK"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	char m_NameBuf[128] = "";
	std::string m_Target;
	std::string m_Error;
	bool m_ErrorPopup = false;
	const char* m_PendingPopup = nullptr;
};
