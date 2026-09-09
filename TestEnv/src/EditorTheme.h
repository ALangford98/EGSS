#pragma once

#include <imgui.h>

// Everforest-inspired dark palette (github.com/sainnhe/everforest, dark
// background / medium contrast). Values verified against the project's own
// source (autoload/everforest.vim: bg0/fg/grey0/grey1/grey2 under the
// "medium" + "dark" branch, red/green under the shared dark-background
// accent block), not guessed. Keyword/StringLiteral/Comment are defined now
// for the syntax-highlighting module that comes later -- unused until then.
struct EditorTheme
{
	ImU32 Background;
	ImU32 Foreground;
	ImU32 LineNumberFg;
	ImU32 CurrentLineNumberFg;
	ImU32 CursorColor;
	ImU32 Keyword;
	ImU32 StringLiteral;
	ImU32 Comment;
};

inline EditorTheme EverforestDark()
{
	EditorTheme t{};
	t.Background          = IM_COL32(0x2d, 0x35, 0x3b, 255); // bg0
	t.Foreground          = IM_COL32(0xd3, 0xc6, 0xaa, 255); // fg
	t.LineNumberFg        = IM_COL32(0x7a, 0x84, 0x78, 255); // grey0
	t.CurrentLineNumberFg = t.Foreground;
	t.CursorColor         = IM_COL32(0xd3, 0xc6, 0xaa, 180);
	t.Keyword             = IM_COL32(0xe6, 0x7e, 0x80, 255); // red
	t.StringLiteral       = IM_COL32(0xa7, 0xc0, 0x80, 255); // green
	t.Comment             = IM_COL32(0x85, 0x92, 0x89, 255); // grey1
	return t;
}
