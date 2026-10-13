#pragma once

// "Is there unsaved work, and if so, ask before it is thrown away" -- for
// every action that replaces the open scene (open or close a project, open
// a recent one, switch scenes, add a demo scene) and for quitting. One rule
// in one place, rather than each action deciding for itself.
// docs/superpowers/specs/2026-10-13-project-save-load-design.md.

#include <GS.h>
#include <imgui.h>
#include <functional>
#include <string>

#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorProject.h"
#include "MaterialEditorPanel.h"
#include "PlayMode.h"

namespace SceneSwapGuard {

	inline std::function<void()> s_PendingAction;
	inline bool s_IncludeMaterials = false;
	inline std::string s_SaveError;
	// ImGui::OpenPopup must run inside the ImGui frame, not from an
	// arbitrary call site -- the same deferred-open pattern EditorMenuBar's
	// own m_PendingPopup uses for exactly this reason.
	inline bool s_ModalRequested = false;

	// Every action that swaps the scene out from under the editor needs
	// undo history and selection cleared, or Undo/Redo and the current
	// selection end up referencing entities from a scene that no longer
	// exists.
	inline void ResetForNewScene()
	{
		EditorHistory::Clear();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(GS::InvalidEntity);
	}

	// The scene differs from its file: an edit in the undo history, or asset
	// paths rewritten when an older project was opened.
	inline bool SceneIsDirty()
	{
		return EditorHistory::IsDirty() || g_SceneNeedsSave;
	}

	inline bool MaterialIsDirty()
	{
		return g_MaterialEditor && g_MaterialEditor->HasUnsavedChanges();
	}

	// Save (Ctrl+S, File > Save Scene, and the modal's Save): the scene, and
	// with includeMaterials the material editor's graph too.
	inline bool SaveAll(bool includeMaterials, std::string& error)
	{
		if (SceneIsDirty() || !g_EditorScenePath.empty())
		{
			if (!SaveEditorScene(error))
				return false;
			EditorHistory::MarkClean();
		}
		if (includeMaterials && MaterialIsDirty())
			g_MaterialEditor->SaveNow();
		return true;
	}

	// Ends Play, restoring the scene as it was before it -- and keeping the
	// fact that it had unsaved edits. Stop reloads the scene and history is
	// reset with it, which used to make those edits read as saved: Play,
	// Stop, quit lost them without a question.
	inline void StopPlayKeepingUnsaved()
	{
		if (!PlayMode::IsPlaying())
			return;
		bool unsaved = SceneIsDirty();
		PlayMode::Stop();
		ResetForNewScene();
		g_SceneNeedsSave = g_SceneNeedsSave || unsaved;
	}

	inline bool HasPendingAction() { return (bool)s_PendingAction; }

	// Runs `action` now if nothing would be lost; otherwise asks first and
	// runs it after Save or Discard (or drops it on Cancel). Returns whether
	// it ran now. includeMaterials: the action also ends the material
	// editor's session (quitting, closing the project), not only the scene.
	inline bool RequestAction(std::function<void()> action, bool includeMaterials = false)
	{
		// What is asked about, and saved, is the scene being edited -- not
		// the running game Play has made of it.
		StopPlayKeepingUnsaved();
		if (!SceneIsDirty() && !(includeMaterials && MaterialIsDirty()))
		{
			action();
			return true;
		}
		s_PendingAction = std::move(action);
		s_IncludeMaterials = includeMaterials;
		s_SaveError.clear();
		s_ModalRequested = true;
		return false;
	}

	inline bool RequestSceneSwap(std::function<void()> performSwap)
	{
		return RequestAction(std::move(performSwap), false);
	}

	// Call once per frame from wherever the editor's other modals render
	// (EditorMenuBar, alongside its own dialogs).
	inline void DrawUnsavedChangesModal()
	{
		if (s_ModalRequested)
		{
			ImGui::OpenPopup("Unsaved Changes");
			s_ModalRequested = false;
		}

		if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		if (SceneIsDirty())
		{
			std::string scene = g_EditorScenePath.empty() ? std::string("The scene")
				: "Scene '" + std::filesystem::path(g_EditorScenePath).stem().string() + "'";
			ImGui::Text("%s has unsaved changes.", scene.c_str());
		}
		if (s_IncludeMaterials && MaterialIsDirty())
			ImGui::Text("The material being edited has unsaved changes.");
		if (!s_SaveError.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_SaveError.c_str());

		auto finish = []()
		{
			std::function<void()> action = std::move(s_PendingAction);
			s_PendingAction = nullptr;
			ImGui::CloseCurrentPopup();
			if (action)
				action();
		};

		// Nothing to save to only for a scene that was never given a file,
		// which with projects means none at all -- so Save is always there
		// when a scene is open.
		if (!g_EditorScenePath.empty() || !SceneIsDirty())
		{
			if (ImGui::Button("Save"))
			{
				std::string error;
				if (SaveAll(s_IncludeMaterials, error))
					finish();
				else
					s_SaveError = error;
			}
			ImGui::SameLine();
		}
		if (ImGui::Button("Don't Save"))
		{
			// Thrown away on purpose: the dirty markers go with it, so the
			// action (which may itself ask again, like Quit) sees no work.
			EditorHistory::MarkClean();
			g_SceneNeedsSave = false;
			finish();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			s_PendingAction = nullptr;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

}
