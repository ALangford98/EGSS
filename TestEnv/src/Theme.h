#pragma once

// The unit of "look" this editor's theming saves/loads: 5 UI color roles,
// 3 font slots, 2 terminal colors, the editor's own syntax colors (moved
// here from the old hardcoded EditorTheme.h), and a raw ImGuiCol_ override
// map for anything the 5 roles don't reach. See
// docs/superpowers/specs/2026-09-13-editor-theming-design.md.

#include <imgui.h>

#include <cstdio>
#include <string>
#include <unordered_map>

struct ThemeFontSlot
{
	std::string Path;
	float Size = 16.0f;
};

struct Theme
{
	std::string Name;

	// The 5 roles. Hover/active variants are Lighten/Darken(role) at
	// Apply() time (Task 4), not stored here.
	ImU32 Background = IM_COL32(0, 0, 0, 255);
	ImU32 Foreground = IM_COL32(255, 255, 255, 255);
	ImU32 FrameBackground = IM_COL32(0, 0, 0, 255);
	ImU32 Accent = IM_COL32(255, 255, 255, 255);
	ImU32 Border = IM_COL32(255, 255, 255, 255);

	ThemeFontSlot ControlsFont;
	ThemeFontSlot EditorFont;
	ThemeFontSlot TerminalFont;

	ImU32 TerminalBackground = IM_COL32(0, 0, 0, 255);
	ImU32 TerminalForeground = IM_COL32(255, 255, 255, 255);

	// EditorTheme.h's old fields, renamed with a Syntax prefix. Background/
	// Foreground stay independent of the roles above on purpose -- see the
	// design spec's own note on why (today's real text editor already
	// looks different from the rest of the UI). CurrentLineNumberFg isn't
	// here: it was always just Foreground, so it's derived, not stored.
	ImU32 SyntaxBackground = IM_COL32(0, 0, 0, 255);
	ImU32 SyntaxForeground = IM_COL32(255, 255, 255, 255);
	ImU32 SyntaxLineNumberFg = IM_COL32(128, 128, 128, 255);
	ImU32 SyntaxCursorColor = IM_COL32(255, 255, 255, 180);
	ImU32 SyntaxKeyword = IM_COL32(255, 255, 255, 255);
	ImU32 SyntaxStringLiteral = IM_COL32(255, 255, 255, 255);
	ImU32 SyntaxComment = IM_COL32(128, 128, 128, 255);
	ImU32 SyntaxSelectionBg = IM_COL32(64, 64, 64, 255);

	// Keyed by the literal ImGuiCol_ name ("ImGuiCol_TitleBgActive"),
	// applied after the role mapping so it always wins. The JSON's
	// "complex theming" half -- the panel (Task 9) never edits this map.
	std::unordered_map<std::string, ImU32> AdvancedOverrides;
};

// "#rrggbb" (alpha defaults to 255) or "#rrggbbaa". Returns false and
// leaves `out` untouched on anything else -- same "never throws, caller's
// fallback survives" contract GS::JsonValue's own accessors use.
inline bool ParseHexColor(const std::string& text, ImU32& out)
{
	if (text.empty() || text[0] != '#')
		return false;
	if (text.size() != 7 && text.size() != 9)
		return false;

	unsigned int r, g, b, a = 255;
	int matched = (text.size() == 7)
		? std::sscanf(text.c_str() + 1, "%2x%2x%2x", &r, &g, &b)
		: std::sscanf(text.c_str() + 1, "%2x%2x%2x%2x", &r, &g, &b, &a);
	int expected = (text.size() == 7) ? 3 : 4;
	if (matched != expected)
		return false;

	out = IM_COL32(r, g, b, a);
	return true;
}

inline std::string FormatHexColor(ImU32 color)
{
	char buf[10];
	std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
		(color >> IM_COL32_R_SHIFT) & 0xFF, (color >> IM_COL32_G_SHIFT) & 0xFF,
		(color >> IM_COL32_B_SHIFT) & 0xFF, (color >> IM_COL32_A_SHIFT) & 0xFF);
	return buf;
}

// Per-channel lerp toward white/black; alpha is untouched by either --
// lightening or darkening a color is about the color, not its opacity.
inline ImU32 Lighten(ImU32 color, float amount)
{
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
	c.x += (1.0f - c.x) * amount;
	c.y += (1.0f - c.y) * amount;
	c.z += (1.0f - c.z) * amount;
	return ImGui::ColorConvertFloat4ToU32(c);
}

inline ImU32 Darken(ImU32 color, float amount)
{
	ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
	c.x -= c.x * amount;
	c.y -= c.y * amount;
	c.z -= c.z * amount;
	return ImGui::ColorConvertFloat4ToU32(c);
}
