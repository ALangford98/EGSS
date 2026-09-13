#pragma once

// Centralizes "does the scene have unsaved changes, and if so, ask before
// swapping it out" -- used by every action that replaces g_EditorScene
// (New/Open Project, Open Demo, and the Scenes panel's switch action) so
// all of them follow the same rule instead of each silently discarding
// edits the way this editor's project actions all did before this file
// existed. See docs/superpowers/specs/2026-09-13-scene-tree-design.md.

#include <GS.h>
#include <imgui.h>
#include <functional>

#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorProject.h"

namespace SceneSwapGuard {

	inline std::function<void()> s_PendingSwap;
	// ImGui::OpenPopup must run inside the ImGui frame, not from an
	// arbitrary call site -- the same deferred-open pattern EditorMenuBar's
	// own m_PendingPopup and EditorSceneView's viewport right-click already
	// use for exactly this reason.
	inline bool s_ModalRequested = false;

	// Every action that swaps the scene out from under the editor needs
	// undo history and selection cleared, or Undo/Redo and the current
	// selection end up referencing entities from a scene that no longer
	// exists. Centralised so no call site can forget it -- moved here from
	// EditorMenuBar so the Scenes panel's switch action can share it too.
	inline void ResetForNewScene()
	{
		EditorHistory::Clear();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(GS::InvalidEntity);
	}

	inline bool HasPendingSwap() { return (bool)s_PendingSwap; }
	inline void ClearPendingSwap() { s_PendingSwap = nullptr; s_ModalRequested = false; }

	// Runs `performSwap` right now if the scene has no unsaved changes.
	// Otherwise stashes it and requests the confirmation modal, which runs
	// it once the user chooses Save or Discard (or drops it on Cancel).
	// Returns true if performSwap already ran.
	inline bool RequestSceneSwap(std::function<void()> performSwap)
	{
		if (!EditorHistory::IsDirty())
		{
			performSwap();
			return true;
		}

		s_PendingSwap = std::move(performSwap);
		s_ModalRequested = true;
		return false;
	}

	// Call once per frame from wherever the editor's other modals already
	// render (EditorMenuBar, alongside its own project dialogs).
	inline void DrawUnsavedChangesModal()
	{
		if (s_ModalRequested)
		{
			ImGui::OpenPopup("Unsaved Changes");
			s_ModalRequested = false;
		}

		if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::Text("This scene has unsaved changes.");

		// No path to save to yet (a bare --scene session that was never
		// saved) -- Save would silently fail, so it's not offered; the
		// menu bar's own "Save" item is disabled for the same reason.
		if (!g_EditorScenePath.empty())
		{
			if (ImGui::Button("Save"))
			{
				if (!g_EditorProjectPath.empty())
					SaveEditorProject();
				else
					SaveEditorScene(g_EditorScenePath);
				EditorHistory::MarkClean();

				if (s_PendingSwap)
					s_PendingSwap();
				ClearPendingSwap();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
		}

		if (ImGui::Button("Discard"))
		{
			if (s_PendingSwap)
				s_PendingSwap();
			ClearPendingSwap();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			ClearPendingSwap();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

}
