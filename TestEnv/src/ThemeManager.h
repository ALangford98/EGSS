#pragma once

// Loads and saves Theme as JSON at TestEnv/assets/themes/*.json. Reading
// goes through GS::JsonValue::Parse (already vendored, built for glTF);
// writing is a small hand-rolled ofstream writer, the same style
// GS/src/GS/Debug/Instrumentor.cpp already uses for profile.json -- this
// file is small enough that a serializer would cost more than it saves.
// See docs/superpowers/specs/2026-09-13-editor-theming-design.md.

#include <GS.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "Theme.h"

namespace ThemeManager {

	inline const char* ThemesFolder() { return "assets/themes"; }

	inline ImU32 ReadColor(const GS::JsonValue& object, const char* key, ImU32 fallback)
	{
		ImU32 out = fallback;
		if (object.Has(key))
			ParseHexColor(object[key].GetString(), out);
		return out;
	}

	inline ThemeFontSlot ReadFontSlot(const GS::JsonValue& object, const char* key, const ThemeFontSlot& fallback)
	{
		if (!object.Has(key))
			return fallback;
		const GS::JsonValue& slot = object[key];
		return { slot["path"].GetString(fallback.Path), slot.Has("size") ? slot["size"].GetFloat(fallback.Size) : fallback.Size };
	}

	inline bool LoadThemeFile(const std::string& filename, Theme& out, std::string& error)
	{
		std::filesystem::path path = std::filesystem::path(ThemesFolder()) / filename;
		std::ifstream in(path);
		if (!in)
		{
			error = "could not open '" + path.string() + "'";
			return false;
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();

		GS::JsonValue root;
		if (!GS::JsonValue::Parse(buffer.str(), root, error))
			return false;

		Theme theme;
		theme.Name = root["name"].GetString(filename);

		const GS::JsonValue& colors = root["colors"];
		theme.Background = ReadColor(colors, "background", theme.Background);
		theme.Foreground = ReadColor(colors, "foreground", theme.Foreground);
		theme.FrameBackground = ReadColor(colors, "frameBackground", theme.FrameBackground);
		theme.Accent = ReadColor(colors, "accent", theme.Accent);
		theme.Border = ReadColor(colors, "border", theme.Border);

		const GS::JsonValue& fonts = root["fonts"];
		theme.ControlsFont = ReadFontSlot(fonts, "controls", theme.ControlsFont);
		theme.EditorFont = ReadFontSlot(fonts, "editor", theme.EditorFont);
		theme.TerminalFont = ReadFontSlot(fonts, "terminal", theme.TerminalFont);

		const GS::JsonValue& terminal = root["terminal"];
		theme.TerminalBackground = ReadColor(terminal, "background", theme.TerminalBackground);
		theme.TerminalForeground = ReadColor(terminal, "foreground", theme.TerminalForeground);

		const GS::JsonValue& syntax = root["editorSyntax"];
		theme.SyntaxBackground = ReadColor(syntax, "background", theme.SyntaxBackground);
		theme.SyntaxForeground = ReadColor(syntax, "foreground", theme.SyntaxForeground);
		theme.SyntaxLineNumberFg = ReadColor(syntax, "lineNumber", theme.SyntaxLineNumberFg);
		theme.SyntaxCursorColor = ReadColor(syntax, "cursor", theme.SyntaxCursorColor);
		theme.SyntaxKeyword = ReadColor(syntax, "keyword", theme.SyntaxKeyword);
		theme.SyntaxStringLiteral = ReadColor(syntax, "stringLiteral", theme.SyntaxStringLiteral);
		theme.SyntaxComment = ReadColor(syntax, "comment", theme.SyntaxComment);
		theme.SyntaxSelectionBg = ReadColor(syntax, "selectionBg", theme.SyntaxSelectionBg);

		const GS::JsonValue& advanced = root["advanced"];
		for (size_t i = 0; i < advanced.Size(); i++)
		{
			ImU32 value;
			if (ParseHexColor(advanced.ValueAt(i).GetString(), value))
				theme.AdvancedOverrides[advanced.KeyAt(i)] = value;
		}

		out = theme;
		return true;
	}

	inline void WriteFontSlot(std::ofstream& out, const char* key, const ThemeFontSlot& slot, bool trailingComma)
	{
		out << "    \"" << key << "\": { \"path\": \"" << slot.Path << "\", \"size\": " << slot.Size << " }"
			<< (trailingComma ? ",\n" : "\n");
	}

	inline bool SaveThemeFile(const std::string& filename, const Theme& theme)
	{
		std::filesystem::create_directories(ThemesFolder());
		std::filesystem::path path = std::filesystem::path(ThemesFolder()) / filename;

		std::ofstream out(path);
		if (!out)
			return false;

		out << "{\n";
		out << "  \"name\": \"" << theme.Name << "\",\n";
		out << "  \"colors\": {\n";
		out << "    \"background\": \"" << FormatHexColor(theme.Background) << "\",\n";
		out << "    \"foreground\": \"" << FormatHexColor(theme.Foreground) << "\",\n";
		out << "    \"frameBackground\": \"" << FormatHexColor(theme.FrameBackground) << "\",\n";
		out << "    \"accent\": \"" << FormatHexColor(theme.Accent) << "\",\n";
		out << "    \"border\": \"" << FormatHexColor(theme.Border) << "\"\n";
		out << "  },\n";
		out << "  \"fonts\": {\n";
		WriteFontSlot(out, "controls", theme.ControlsFont, true);
		WriteFontSlot(out, "editor", theme.EditorFont, true);
		WriteFontSlot(out, "terminal", theme.TerminalFont, false);
		out << "  },\n";
		out << "  \"terminal\": {\n";
		out << "    \"background\": \"" << FormatHexColor(theme.TerminalBackground) << "\",\n";
		out << "    \"foreground\": \"" << FormatHexColor(theme.TerminalForeground) << "\"\n";
		out << "  },\n";
		out << "  \"editorSyntax\": {\n";
		out << "    \"background\": \"" << FormatHexColor(theme.SyntaxBackground) << "\",\n";
		out << "    \"foreground\": \"" << FormatHexColor(theme.SyntaxForeground) << "\",\n";
		out << "    \"keyword\": \"" << FormatHexColor(theme.SyntaxKeyword) << "\",\n";
		out << "    \"stringLiteral\": \"" << FormatHexColor(theme.SyntaxStringLiteral) << "\",\n";
		out << "    \"comment\": \"" << FormatHexColor(theme.SyntaxComment) << "\",\n";
		out << "    \"lineNumber\": \"" << FormatHexColor(theme.SyntaxLineNumberFg) << "\",\n";
		out << "    \"cursor\": \"" << FormatHexColor(theme.SyntaxCursorColor) << "\",\n";
		out << "    \"selectionBg\": \"" << FormatHexColor(theme.SyntaxSelectionBg) << "\"\n";
		out << "  },\n";
		out << "  \"advanced\": {\n";
		size_t i = 0;
		for (auto& [name, color] : theme.AdvancedOverrides)
		{
			out << "    \"" << name << "\": \"" << FormatHexColor(color) << "\"";
			out << (++i < theme.AdvancedOverrides.size() ? ",\n" : "\n");
		}
		out << "  }\n";
		out << "}\n";

		return true;
	}

	inline Theme s_Current;

	inline const Theme& Current() { return s_Current; }

	inline void SetColor(ImGuiStyle& style, ImGuiCol_ index, ImU32 color)
	{
		style.Colors[index] = ImGui::ColorConvertU32ToFloat4(color);
	}

	inline void Apply(const Theme& theme)
	{
		s_Current = theme;

		ImGuiStyle& style = ImGui::GetStyle();
		ImU32 accentHover = Lighten(theme.Accent, 0.15f);
		ImU32 accentActive = Darken(theme.Accent, 0.10f);
		ImU32 frameHover = Lighten(theme.FrameBackground, 0.15f);
		ImU32 dimOverlay = IM_COL32(0, 0, 0, 90);   // ~35% alpha, fixed regardless of theme -- a screen-dimming effect, not a themed color

		// Background
		for (ImGuiCol_ index : { ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_TitleBg,
			ImGuiCol_TitleBgCollapsed, ImGuiCol_MenuBarBg, ImGuiCol_ScrollbarBg, ImGuiCol_DockingEmptyBg,
			ImGuiCol_TableRowBg })
			SetColor(style, index, theme.Background);
		SetColor(style, ImGuiCol_TableRowBgAlt, Lighten(theme.Background, 0.05f));

		// Foreground
		SetColor(style, ImGuiCol_Text, theme.Foreground);
		SetColor(style, ImGuiCol_InputTextCursor, theme.Foreground);
		style.Colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(theme.Foreground);
		style.Colors[ImGuiCol_TextDisabled].w *= 0.5f;

		// FrameBackground
		for (ImGuiCol_ index : { ImGuiCol_FrameBg, ImGuiCol_Tab, ImGuiCol_TabDimmed })
			SetColor(style, index, theme.FrameBackground);

		// Accent (idle) + its Hovered/Active variants
		for (ImGuiCol_ index : { ImGuiCol_CheckMark, ImGuiCol_CheckboxSelectedBg, ImGuiCol_SliderGrab,
			ImGuiCol_Button, ImGuiCol_Header, ImGuiCol_TabSelected, ImGuiCol_TabSelectedOverline,
			ImGuiCol_TabDimmedSelected, ImGuiCol_TabDimmedSelectedOverline, ImGuiCol_TitleBgActive,
			ImGuiCol_TextSelectedBg, ImGuiCol_TextLink, ImGuiCol_PlotLines, ImGuiCol_PlotHistogram,
			ImGuiCol_DragDropTarget, ImGuiCol_NavCursor, ImGuiCol_DockingPreview, ImGuiCol_UnsavedMarker,
			ImGuiCol_NavWindowingHighlight })
			SetColor(style, index, theme.Accent);
		for (ImGuiCol_ index : { ImGuiCol_ButtonHovered, ImGuiCol_HeaderHovered, ImGuiCol_TabHovered,
			ImGuiCol_PlotLinesHovered, ImGuiCol_PlotHistogramHovered })
			SetColor(style, index, accentHover);
		for (ImGuiCol_ index : { ImGuiCol_ButtonActive, ImGuiCol_HeaderActive, ImGuiCol_SliderGrabActive })
			SetColor(style, index, accentActive);
		SetColor(style, ImGuiCol_FrameBgHovered, frameHover);
		SetColor(style, ImGuiCol_FrameBgActive, accentActive);

		// Border
		for (ImGuiCol_ index : { ImGuiCol_Border, ImGuiCol_BorderShadow, ImGuiCol_Separator,
			ImGuiCol_ScrollbarGrab, ImGuiCol_ResizeGrip, ImGuiCol_TableBorderStrong,
			ImGuiCol_TableBorderLight, ImGuiCol_TreeLines })
			SetColor(style, index, theme.Border);
		SetColor(style, ImGuiCol_SeparatorHovered, Lighten(theme.Border, 0.15f));
		SetColor(style, ImGuiCol_SeparatorActive, Darken(theme.Border, 0.10f));
		SetColor(style, ImGuiCol_ScrollbarGrabHovered, Lighten(theme.Border, 0.15f));
		SetColor(style, ImGuiCol_ScrollbarGrabActive, Darken(theme.Border, 0.10f));
		SetColor(style, ImGuiCol_ResizeGripHovered, Lighten(theme.Border, 0.15f));
		SetColor(style, ImGuiCol_ResizeGripActive, Darken(theme.Border, 0.10f));

		// Fixed dimming overlays -- not role-derived.
		SetColor(style, ImGuiCol_NavWindowingDimBg, dimOverlay);
		SetColor(style, ImGuiCol_ModalWindowDimBg, dimOverlay);
		SetColor(style, ImGuiCol_DragDropTargetBg, dimOverlay);

		// Advanced overrides win last. GetStyleColorName returns the bare
		// suffix ("ChildBg"), not the fully-qualified enum identifier --
		// verified against imgui.cpp's own implementation, not assumed --
		// so the JSON's more explicit "ImGuiCol_ChildBg" keys have their
		// prefix stripped before comparing.
		for (auto& [name, color] : theme.AdvancedOverrides)
		{
			std::string bareName = name.rfind("ImGuiCol_", 0) == 0 ? name.substr(9) : name;
			for (int i = 0; i < ImGuiCol_COUNT; i++)
				if (bareName == ImGui::GetStyleColorName(i))
				{
					style.Colors[i] = ImGui::ColorConvertU32ToFloat4(color);
					break;
				}
		}

		GS::Application::Get().GetImGuiLayer()->RequestFontReload(
			theme.ControlsFont.Path, theme.ControlsFont.Size,
			theme.EditorFont.Path, theme.EditorFont.Size,
			theme.TerminalFont.Path, theme.TerminalFont.Size);
	}

	inline std::vector<std::string> ListThemes()
	{
		std::vector<std::string> names;
		std::error_code ec;
		for (auto& entry : std::filesystem::directory_iterator(ThemesFolder(), ec))
			if (entry.is_regular_file() && entry.path().extension() == ".json")
				names.push_back(entry.path().filename().string());
		return names;
	}

	inline const char* LastThemePath() { return "editor_last_theme.txt"; }

	inline bool ApplyByFilename(const std::string& filename)
	{
		Theme theme;
		std::string error;
		if (!LoadThemeFile(filename, theme, error))
		{
			GS_WARN("ThemeManager: could not load '{0}': {1}", filename, error);
			return false;
		}

		Apply(theme);

		std::ofstream last(LastThemePath());
		if (last)
			last << filename;

		return true;
	}

}
