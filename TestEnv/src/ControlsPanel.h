#pragma once

// The "Controls" panel: the open project's game controls (GS::InputMap) --
// every action the runtime reads, what it is bound to, rebinding by
// pressing the key -- and, below, the editor's own shortcuts, read-only.
//
// Edits apply at once, to the map a running game reads (so they take effect
// mid-Play), and are saved straight into the project's controls.txt, which
// an export ships. Opening another project loads its controls; one with no
// file has the defaults.
//
// Rebinding listens by polling GS::Input for a key that went down this
// frame. While it listens, ImGui is told it wants the keyboard, so the key
// pressed to bind is not also an editor shortcut (1 switching the gizmo,
// Delete deleting the selection). A mouse button is bound by clicking it on
// the listening button itself -- a click anywhere else is just a click.

#include <GS.h>
#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, a window's dock

#include "EditorProject.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class ControlsPanel : public GS::Layer
{
public:
	ControlsPanel() : Layer("ControlsPanel") {}

	void OnImGuiRender() override
	{
		FollowProject();

		// The dock layout is built once, so a layout saved before this panel
		// existed does not place it: the first time, it tabs in beside the
		// Terminal, as a fresh layout puts it.
		if (ImGuiWindow* terminal = ImGui::FindWindowByName("Terminal"); terminal && terminal->DockId)
			ImGui::SetNextWindowDockID(terminal->DockId, ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Controls");
		GS::InputMap& map = GS::InputMap::Active();

		if (g_EditorProjectPath.empty())
			ImGui::TextDisabled("No project open: changes last until you quit, and are not saved.");
		else
			ImGui::TextDisabled("%s -- saved with the project as %s", g_EditorProjectName.c_str(), GS::InputMap::FileName());
		ImGui::TextDisabled("Click a binding to change it, + to add one, x to remove it.");
		ImGui::Spacing();

		Listen(map);

		// Which actions each binding belongs to, to warn about one bound twice.
		std::map<std::pair<bool, int>, std::vector<std::string>> uses;
		for (const GS::InputMap::Action& action : map.Actions())
			for (const GS::Binding& binding : action.Bindings)
				uses[{ binding.Mouse, binding.Code }].push_back(action.Label);

		std::string group;
		bool open = false;
		for (size_t a = 0; a < map.Actions().size(); a++)
		{
			GS::InputMap::Action& action = map.Actions()[a];
			if (action.Group != group)
			{
				if (open && !group.empty())
					ImGui::EndTable();
				group = action.Group;
				ImGui::SeparatorText(group.c_str());
				open = ImGui::BeginTable(("##controls" + group).c_str(), 3,
					ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg);
				if (open)
				{
					ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 200.0f);
					ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				}
			}
			if (!open)
				continue;
			ImGui::PushID((int)a);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(action.Label.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Scripts read it as input.isActionDown(\"%s\")", action.Id.c_str());

			ImGui::TableSetColumnIndex(1);
			for (size_t b = 0; b < action.Bindings.size(); b++)
			{
				ImGui::PushID((int)b);
				const GS::Binding binding = action.Bindings[b];
				bool listening = m_Listening == (int)a && m_Slot == (int)b;
				const std::vector<std::string>& sharers = uses[{ binding.Mouse, binding.Code }];
				bool clash = sharers.size() > 1;
				if (clash)
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.05f, 1.0f));
				std::string label = listening ? std::string("press a key...") : GS::InputMap::Name(binding);
				if (ListeningButton(label, listening, map))
					StartListening((int)a, (int)b);
				if (clash)
				{
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered())
					{
						std::string also;
						for (const std::string& other : sharers)
							if (other != action.Label)
								also += (also.empty() ? "" : ", ") + other;
						ImGui::SetTooltip("Also bound to: %s", also.c_str());
					}
				}
				ImGui::SameLine(0.0f, 2.0f);
				if (ImGui::SmallButton("x"))
				{
					action.Bindings.erase(action.Bindings.begin() + (long)b);
					Changed(map);
					ImGui::PopID();
					break;
				}
				ImGui::SameLine();
				ImGui::PopID();
			}
			bool adding = m_Listening == (int)a && m_Slot < 0;
			if (adding)
			{
				ListeningButton("press a key...", true, map);
				ImGui::SameLine();
				if (ImGui::SmallButton("cancel"))
					m_Listening = -1;
			}
			else if (ImGui::SmallButton("+"))
				StartListening((int)a, -1);

			ImGui::TableSetColumnIndex(2);
			if (action.Bindings != action.Defaults)
			{
				if (ImGui::SmallButton("Reset"))
				{
					action.Bindings = action.Defaults;
					Changed(map);
				}
				if (ImGui::IsItemHovered())
				{
					std::string defaults;
					for (const GS::Binding& binding : action.Defaults)
						defaults += (defaults.empty() ? "" : " / ") + GS::InputMap::Name(binding);
					ImGui::SetTooltip("Back to %s", defaults.c_str());
				}
			}
			ImGui::PopID();
		}
		if (open)
			ImGui::EndTable();

		ImGui::Spacing();
		if (m_Listening >= 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Listening: press a key, or click a mouse button on the highlighted button.");
			if (ImGui::Button("Stop listening"))
				m_Listening = -1;
		}
		else if (ImGui::Button("Reset all to defaults"))
		{
			map.ResetToDefaults();
			Changed(map);
		}

		ImGui::Spacing();
		if (ImGui::CollapsingHeader("Editor shortcuts (fixed)"))
			DrawEditorShortcuts();

		if (!m_Error.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", m_Error.c_str());

		ImGui::End();
	}

private:
	// A project opened (or closed) since the last frame: its controls.
	void FollowProject()
	{
		if (g_EditorProjectPath == m_Project)
			return;
		m_Project = g_EditorProjectPath;
		m_Listening = -1;
		m_Error.clear();
		if (m_Project.empty())
			return;
		GS::InputMap::Active().Load((std::filesystem::path(m_Project) / GS::InputMap::FileName()).string());
	}

	void Changed(GS::InputMap& map)
	{
		if (m_Project.empty())
			return;
		std::string path = (std::filesystem::path(m_Project) / GS::InputMap::FileName()).string();
		m_Error = map.Save(path) ? std::string() : "Could not write " + path;
	}

	void StartListening(int action, int slot)
	{
		m_Listening = action;
		m_Slot = slot;
		m_Armed = false;   // not this frame: the click that started it is still down
	}

	// A binding's button; while listening, a mouse button clicked on it is
	// the binding.
	bool ListeningButton(const std::string& label, bool listening, GS::InputMap& map)
	{
		if (listening)
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.1f, 1.0f));
		bool clicked = ImGui::SmallButton(label.c_str());
		if (listening)
		{
			ImGui::PopStyleColor();
			if (m_Armed && ImGui::IsItemHovered())
				for (int button = 0; button < 5; button++)
					if (ImGui::IsMouseClicked(button))
					{
						Bind(map, GS::Binding{ true, button });
						return false;
					}
			return false;
		}
		return clicked;
	}

	// The key pressed this frame, while listening.
	void Listen(GS::InputMap& map)
	{
		if (m_Listening < 0)
		{
			m_Held.clear();
			return;
		}
		// The keyboard is ours while listening, so the key is not also a
		// shortcut somewhere else in the editor.
		ImGui::SetNextFrameWantCaptureKeyboard(true);
		std::vector<int> down;
		for (const GS::Binding& binding : GS::InputMap::Bindable())
			if (!binding.Mouse && GS::Input::IsKeyPressed(binding.Code))
				down.push_back(binding.Code);
		if (m_Armed)
			for (int code : down)
				if (std::find(m_Held.begin(), m_Held.end(), code) == m_Held.end())
				{
					Bind(map, GS::Binding{ false, code });
					break;
				}
		m_Held = down;
		m_Armed = true;
	}

	void Bind(GS::InputMap& map, const GS::Binding& binding)
	{
		if (m_Listening < 0 || m_Listening >= (int)map.Actions().size())
			return;
		std::vector<GS::Binding>& bindings = map.Actions()[(size_t)m_Listening].Bindings;
		if (std::find(bindings.begin(), bindings.end(), binding) == bindings.end())
		{
			if (m_Slot >= 0 && m_Slot < (int)bindings.size())
				bindings[(size_t)m_Slot] = binding;
			else
				bindings.push_back(binding);
		}
		m_Listening = -1;
		Changed(map);
	}

	// What the editor itself answers to. Fixed: these are the editor's,
	// not the game's.
	void DrawEditorShortcuts()
	{
		static const char* rows[][2] =
		{
			{ "W A S D", "move the editor camera" },
			{ "E / Q", "editor camera up / down" },
			{ "Arrow keys", "turn the editor camera" },
			{ "Middle mouse drag", "look around" },
			{ "Left click", "select (Ctrl+click adds to the selection)" },
			{ "Right click", "the entity's menu" },
			{ "1 / 2 / 3, T", "move / rotate / scale gizmo; T cycles" },
			{ "Delete", "delete the selection" },
			{ "Ctrl+G", "group the selection" },
			{ "Ctrl+S", "save the scene" },
			{ "Ctrl+Z / Ctrl+Y", "undo / redo" },
			{ "Ctrl+F", "find text" },
			{ "F1", "next demo" },
			{ "F2", "screenshot" },
		};
		if (ImGui::BeginTable("##editorkeys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			for (const auto& row : rows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(row[0]);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextDisabled("%s", row[1]);
			}
			ImGui::EndTable();
		}
		ImGui::TextDisabled("While a game plays, its clicks and keys are the game's.");
	}

	std::string m_Project = "\x01";   // never a real path: the first frame loads
	int m_Listening = -1;              // the action being rebound
	int m_Slot = -1;                   // which binding of it; -1 adds one
	bool m_Armed = false;
	std::vector<int> m_Held;           // keys already down when listening began
	std::string m_Error;
};
