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
#include "EditorSettings.h"

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

	// Ending Play, keeping the scene as the game left it or not. Either
	// way the history is reset (Stop swaps the scene); kept changes, and
	// edits made before Play, count as unsaved.
	inline void FinishStop(bool keep)
	{
		if (!PlayMode::IsPlaying())
			return;
		bool unsaved = SceneIsDirty();
		PlayMode::Stop(keep);
		ResetForNewScene();
		g_SceneNeedsSave = g_SceneNeedsSave || unsaved || keep;
	}

	// The Stop button: asks whether to keep what happened during Play,
	// unless a "Don't ask again" answer is remembered. The game is held
	// still while the question is up, so "as it is" means at the press.
	inline bool s_StopQuestion = false;
	inline bool s_StopQuestionRequested = false;
	inline bool s_StopRemember = false;

	inline const char* KeepSettingKey() { return "play.keep-changes"; }

	inline void RequestStop()
	{
		std::string remembered = EditorSettings::Get(KeepSettingKey(), "ask");
		if (remembered == "keep")
			FinishStop(true);
		else if (remembered == "discard")
			FinishStop(false);
		else
		{
			s_StopQuestion = true;
			s_StopQuestionRequested = true;
			s_StopRemember = false;
		}
	}

	inline bool IsStopQuestionOpen() { return s_StopQuestion; }

	inline void DrawStopQuestion()
	{
		if (s_StopQuestionRequested)
		{
			ImGui::OpenPopup("Stop Playing");
			s_StopQuestionRequested = false;
		}
		if (!ImGui::BeginPopupModal("Stop Playing", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		ImGui::TextUnformatted("Keep the changes made during Play?");
		ImGui::TextDisabled("Keep leaves the scene as it is now (unsaved). Discard puts it back as it was when Play started.");
		ImGui::Checkbox("Don't ask again", &s_StopRemember);
		auto answer = [](bool keep)
		{
			if (s_StopRemember)
				EditorSettings::Set(KeepSettingKey(), keep ? "keep" : "discard");
			s_StopQuestion = false;
			ImGui::CloseCurrentPopup();
			FinishStop(keep);
		};
		if (ImGui::Button("Keep"))
			answer(true);
		ImGui::SameLine();
		if (ImGui::Button("Discard"))
			answer(false);
		ImGui::SameLine();
		if (ImGui::Button("Keep Playing"))
		{
			s_StopQuestion = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
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
