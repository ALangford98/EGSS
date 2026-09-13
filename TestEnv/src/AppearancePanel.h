#pragma once

// The in-editor "Appearance" panel: pick a theme from the folder, tweak
// its 5 color roles / 3 fonts / 2 terminal colors, and save the result as
// a new (or overwritten) theme file. See
// docs/superpowers/specs/2026-09-13-editor-theming-design.md.

#include <GS.h>
#include <imgui.h>
#include <cstring>

#include "FileBrowserPopup.h"
#include "ThemeManager.h"

class AppearancePanel : public GS::Layer
{
public:
	AppearancePanel() : Layer("AppearancePanel") {}

	void OnAttach() override
	{
		RefreshFromCurrent();
	}

	void OnImGuiRender() override
	{
		ImGui::Begin("Appearance");

		ImGui::SeparatorText("Themes");
		for (const std::string& filename : ThemeManager::ListThemes())
		{
			if (ImGui::Selectable(filename.c_str(), filename == m_LastAppliedFilename))
			{
				if (ThemeManager::ApplyByFilename(filename))
				{
					m_LastAppliedFilename = filename;
					RefreshFromCurrent();
				}
			}
		}

		ImGui::SeparatorText("Colors");
		ColorRole("Background", &m_Working.Background);
		ColorRole("Foreground", &m_Working.Foreground);
		ColorRole("Frame Background", &m_Working.FrameBackground);
		ColorRole("Accent", &m_Working.Accent);
		ColorRole("Border", &m_Working.Border);

		ImGui::SeparatorText("Fonts");
		FontRow("Controls", m_Working.ControlsFont, m_ControlsBrowser);
		FontRow("Editor", m_Working.EditorFont, m_EditorBrowser);
		FontRow("Terminal", m_Working.TerminalFont, m_TerminalBrowser);

		ImGui::SeparatorText("Terminal Colors");
		ColorRole("Terminal Background", &m_Working.TerminalBackground);
		ColorRole("Terminal Foreground", &m_Working.TerminalForeground);

		ImGui::SeparatorText("Save");
		ImGui::InputText("##save_as_name", m_SaveAsBuf, sizeof(m_SaveAsBuf));
		ImGui::SameLine();
		bool canSave = m_SaveAsBuf[0] != '\0';
		if (!canSave) ImGui::BeginDisabled();
		if (ImGui::Button("Save As"))
		{
			std::string filename = m_SaveAsBuf;
			if (filename.find(".json") == std::string::npos)
				filename += ".json";
			m_Working.Name = filename;
			ThemeManager::SaveThemeFile(filename, m_Working);
		}
		if (!canSave) ImGui::EndDisabled();

		ImGui::End();
	}

private:
	void RefreshFromCurrent()
	{
		m_Working = ThemeManager::Current();
	}

	// IsItemDeactivatedAfterEdit-gated, the same pattern EditorSceneView's
	// Inspector already uses -- Apply() queues a font reload and rewrites
	// ImGuiStyle, so it should run once per edit, not once per dragged frame.
	void ColorRole(const char* label, ImU32* color)
	{
		ImVec4 asFloat = ImGui::ColorConvertU32ToFloat4(*color);
		if (ImGui::ColorEdit4(label, &asFloat.x))
			*color = ImGui::ColorConvertFloat4ToU32(asFloat);
		if (ImGui::IsItemDeactivatedAfterEdit())
			ThemeManager::Apply(m_Working);
	}

	// Each row's FileBrowserPopup gets a title unique to that row --
	// ImGui::BeginPopupModal keys its window by that exact string, so
	// three rows sharing one title ("Choose Font") would all drive the
	// same popup state instead of three independent ones.
	void FontRow(const char* label, ThemeFontSlot& slot, FileBrowserPopup& browser)
	{
		ImGui::PushID(label);
		char pathBuf[512];
		strncpy(pathBuf, slot.Path.c_str(), sizeof(pathBuf) - 1);
		pathBuf[sizeof(pathBuf) - 1] = '\0';
		ImGui::SetNextItemWidth(250.0f);
		ImGui::InputText(label, pathBuf, sizeof(pathBuf));
		slot.Path = pathBuf;
		ImGui::SameLine();
		if (ImGui::Button("Browse..."))
			browser.Open((std::string("Choose ") + label + " Font").c_str(), FileBrowserPopup::Mode::PickFile, "assets/fonts", ".ttf");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		ImGui::DragFloat("Size", &slot.Size, 0.5f, 6.0f, 72.0f);
		if (ImGui::IsItemDeactivatedAfterEdit())
			ThemeManager::Apply(m_Working);

		if (browser.HasResult())
		{
			slot.Path = browser.TakeResult();
			ThemeManager::Apply(m_Working);
		}
		browser.Draw();
		ImGui::PopID();
	}

	Theme m_Working;
	std::string m_LastAppliedFilename;
	FileBrowserPopup m_ControlsBrowser, m_EditorBrowser, m_TerminalBrowser;
	char m_SaveAsBuf[256] = {};
};
