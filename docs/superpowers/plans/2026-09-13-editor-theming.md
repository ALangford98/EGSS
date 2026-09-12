# Editor Theming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the editor's whole look — general UI chrome, text-editor
syntax colors, terminal colors, and three independent fonts (Controls,
Editor, Terminal) — be defined by a JSON theme file, switchable live from a
new "Appearance" panel, with `basic.json` (today's real look, unchanged)
and `default.json` (a full Everforest palette) shipped from day one.

**Architecture:** A `Theme` struct (5 UI color roles + 3 font slots + 2
terminal colors + 8 syntax colors + a raw-`ImGuiCol_` override map) is
loaded/saved as JSON (`GS::JsonValue::Parse` for reading, a small
hand-rolled `ofstream` writer for saving) by a new `ThemeManager`, which
also maps the 5 roles onto `ImGuiStyle::Colors[]` and queues a live
font-atlas rebuild on `ImGuiLayer`. `TextEditorPanel` and `TerminalPanel`
each read the active theme's fields directly at render time instead of
carrying their own hardcoded copy.

**Tech Stack:** C++17, ImGui 1.92.9b (vendored), `GS::JsonValue` (vendored,
read-only), libvterm (vendored). No new external dependency.

**Spec:** `docs/superpowers/specs/2026-09-13-editor-theming-design.md`

## Global Constraints

- **Testing convention for this repo, not a generic framework:** a task
  whose logic doesn't need a live ImGui/GL context adds a *temporary*
  self-test header (`XyzTest.h`, marked `// TEMPORARY -- delete after
  verifying`), wires one `Run()` call into `TestEnv/src/TestApp.cpp`'s
  `TestEnv()` constructor, builds with `./gs.py build`, runs with `./gs.py
  run -- --hide-window --lockstep --capture <path> --capture-step N`, reads
  the `GS_TRACE`'d pass/fail lines from the output, then **deletes the test
  header and the two lines that wired it in**. There is no permanent test
  suite in this project — do not add one.
- **UI-only tasks are checked by building and a visual capture instead**,
  per the design spec's own Testing section — the live font-atlas rebuild
  actually redrawing, the panel's pickers, and the terminal actually
  recoloring are all in this category. The standing, disclosed gap is live
  mouse/keyboard click-through itself: no GUI-automation tool exists in
  this environment.
- **A visual capture needs the app fully warmed up.** This project's
  `DemoWarmup` layer does real, slow work (terrain/planet generation) on
  every startup regardless of which demo is active — a `--capture-step 30`
  capture can take 60-90 real seconds even hidden. Use a generous timeout
  (90s+) rather than assuming a hang. Delete `imgui.ini` beside the binary
  before a capture that depends on which panel/tab is focused —
  `ImGui::SetWindowFocus("PanelName")` added temporarily at the top of that
  panel's `OnImGuiRender` (removed right after the capture) is the
  reliable way to bring a specific docked tab to the front for one capture.
- **Verify at all three configs** (`./gs.py build all`) before considering
  any task's code changes final.
- **Never commit, never push mid-task.** This repo's own rule: the owner
  commits their own work in an interactive session. (This plan may instead
  be executed by a background session already following a separate,
  explicit worktree-and-merge-back policy recorded in `CLAUDE.md` — if so,
  follow that; otherwise leave every task's changes staged in the working
  tree for the owner to review and commit.)
- **Compute expected values by hand before asserting them.** Every test
  below states the hand-derived expected value in its own check message,
  not just in this plan. Colors used in tests are the real, sourced values
  this plan already grounds in `imgui_draw.cpp`/`pen.c`/`everforest.vim` —
  never invent a placeholder hex value.

---

### Task 1: `Theme` struct + hex-color helpers

**Files:**
- Create: `TestEnv/src/Theme.h`
- Test: `TestEnv/src/ThemeColorTest.h` (temporary)

**Interfaces:**
- Produces: `struct ThemeFontSlot { std::string Path; float Size = 16.0f; };`,
  `struct Theme { ... }` (fields below), `bool ParseHexColor(const
  std::string& text, ImU32& out)`, `std::string FormatHexColor(ImU32
  color)`, `ImU32 Lighten(ImU32 color, float amount)`, `ImU32 Darken(ImU32
  color, float amount)` — every later task includes this header and uses
  these exact names.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/ThemeColorTest.h`:

```cpp
// TEMPORARY -- delete after verifying Theme's hex-color helpers.
#pragma once
#include <GS.h>

#include "Theme.h"

namespace ThemeColorTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		ImU32 color;

		Check(ParseHexColor("#2d353b", color) && color == IM_COL32(0x2d, 0x35, 0x3b, 255),
			"6-digit hex parses with alpha defaulted to 255");

		Check(ParseHexColor("#2d353b80", color) && color == IM_COL32(0x2d, 0x35, 0x3b, 0x80),
			"8-digit hex parses its own alpha byte (0x80)");

		ImU32 before = IM_COL32(1, 2, 3, 4);
		color = before;
		Check(!ParseHexColor("not-a-color", color) && color == before,
			"a malformed string returns false and leaves the output untouched");

		Check(FormatHexColor(IM_COL32(0x2d, 0x35, 0x3b, 0x80)) == "#2d353b80",
			"FormatHexColor always writes 8 digits (rrggbbaa), lowercase");

		Check(Lighten(IM_COL32(0, 0, 0, 255), 0.5f) == IM_COL32(127, 127, 127, 255),
			"Lighten(black, 0.5) is exactly mid-grey (0 + (255-0)*0.5 = 127.5 -> 127)");
		Check(Lighten(IM_COL32(255, 255, 255, 255), 0.5f) == IM_COL32(255, 255, 255, 255),
			"Lighten(white, ...) stays white -- already at the ceiling");
		Check(Darken(IM_COL32(255, 255, 255, 255), 0.5f) == IM_COL32(127, 127, 127, 255),
			"Darken(white, 0.5) is exactly mid-grey");
		Check(Darken(IM_COL32(0, 0, 0, 255), 0.5f) == IM_COL32(0, 0, 0, 255),
			"Darken(black, ...) stays black -- already at the floor");

		GS_TRACE("ThemeColorTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it in: add `#include "ThemeColorTest.h"   // TEMPORARY` near the other
includes in `TestEnv/src/TestApp.cpp`, and `ThemeColorTest::Run();   //
TEMPORARY -- delete after verifying` as the first line of `TestEnv()`'s
constructor body.

- [ ] **Step 2: Build to confirm it fails**

Run: `./gs.py build`
Expected: FAIL — `Theme.h: No such file or directory`.

- [ ] **Step 3: Write `Theme.h`**

```cpp
#pragma once

// The unit of "look" this editor's theming saves/loads: 5 UI color roles,
// 3 font slots, 2 terminal colors, the editor's own syntax colors (moved
// here from the old hardcoded EditorTheme.h), and a raw ImGuiCol_ override
// map for anything the 5 roles don't reach. See
// docs/superpowers/specs/2026-09-13-editor-theming-design.md.

#include <imgui.h>

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
```

Add `#include <cstdio>` if the build complains about `sscanf`/`snprintf`
(this project's precompiled header may already pull it in via `gspch.h`;
add it explicitly here regardless since `Theme.h` doesn't include the pch).

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/themecolor.png --capture-step 2
```
Expected: `ThemeColorTest: 8 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

Delete `TestEnv/src/ThemeColorTest.h` and the two lines in `TestApp.cpp`
that referenced it. Rebuild to confirm `TestApp.cpp` still compiles clean.

- [ ] **Step 6: Mark task done**

---

### Task 2: `ThemeManager` — JSON load/save

**Files:**
- Create: `TestEnv/src/ThemeManager.h`
- Test: `TestEnv/src/ThemeLoadSaveTest.h` (temporary)

**Interfaces:**
- Consumes: Task 1's `Theme`, `ThemeFontSlot`, `ParseHexColor`,
  `FormatHexColor`
- Produces: `namespace ThemeManager { bool LoadThemeFile(const
  std::string& filename, Theme& out, std::string& error); bool
  SaveThemeFile(const std::string& filename, const Theme& theme); inline
  const char* ThemesFolder() { return "assets/themes"; } }` — Tasks 4-9 all
  build on this. `filename` is relative to `ThemesFolder()`, not an
  absolute path.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/ThemeLoadSaveTest.h`:

```cpp
// TEMPORARY -- delete after verifying ThemeManager's JSON load/save.
#pragma once
#include <GS.h>
#include <filesystem>

#include "ThemeManager.h"

namespace ThemeLoadSaveTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		std::filesystem::create_directories(ThemeManager::ThemesFolder());

		Theme original;
		original.Name = "Round Trip Test";
		original.Background = IM_COL32(0x11, 0x22, 0x33, 255);
		original.Foreground = IM_COL32(0xaa, 0xbb, 0xcc, 255);
		original.FrameBackground = IM_COL32(0x22, 0x33, 0x44, 255);
		original.Accent = IM_COL32(0x33, 0x44, 0x55, 255);
		original.Border = IM_COL32(0x44, 0x55, 0x66, 255);
		original.ControlsFont = { "assets/fonts/DejaVuSansMono.ttf", 14.0f };
		original.EditorFont = { "assets/fonts/DejaVuSansMono.ttf", 18.0f };
		original.TerminalFont = { "assets/fonts/DejaVuSansMono.ttf", 16.0f };
		original.TerminalBackground = IM_COL32(0x00, 0x00, 0x00, 255);
		original.TerminalForeground = IM_COL32(0xf0, 0xf0, 0xf0, 255);
		original.SyntaxBackground = IM_COL32(0x2d, 0x35, 0x3b, 255);
		original.SyntaxForeground = IM_COL32(0xd3, 0xc6, 0xaa, 255);
		original.SyntaxLineNumberFg = IM_COL32(0x7a, 0x84, 0x78, 255);
		original.SyntaxCursorColor = IM_COL32(0xd3, 0xc6, 0xaa, 180);
		original.SyntaxKeyword = IM_COL32(0xe6, 0x7e, 0x80, 255);
		original.SyntaxStringLiteral = IM_COL32(0xa7, 0xc0, 0x80, 255);
		original.SyntaxComment = IM_COL32(0x85, 0x92, 0x89, 255);
		original.SyntaxSelectionBg = IM_COL32(0x42, 0x4a, 0x50, 255);
		original.AdvancedOverrides["ImGuiCol_ChildBg"] = IM_COL32(0, 0, 0, 0);

		Check(ThemeManager::SaveThemeFile("round_trip_test.json", original), "SaveThemeFile succeeds");

		Theme loaded;
		std::string error;
		Check(ThemeManager::LoadThemeFile("round_trip_test.json", loaded, error), "LoadThemeFile succeeds: " + error);

		Check(loaded.Name == original.Name, "Name round-trips");
		Check(loaded.Background == original.Background, "Background round-trips");
		Check(loaded.Foreground == original.Foreground, "Foreground round-trips");
		Check(loaded.FrameBackground == original.FrameBackground, "FrameBackground round-trips");
		Check(loaded.Accent == original.Accent, "Accent round-trips");
		Check(loaded.Border == original.Border, "Border round-trips");
		Check(loaded.ControlsFont.Path == original.ControlsFont.Path && loaded.ControlsFont.Size == original.ControlsFont.Size,
			"ControlsFont round-trips");
		Check(loaded.EditorFont.Size == 18.0f, "EditorFont's distinct size round-trips");
		Check(loaded.TerminalFont.Size == 16.0f, "TerminalFont's distinct size round-trips");
		Check(loaded.TerminalBackground == original.TerminalBackground, "TerminalBackground round-trips");
		Check(loaded.TerminalForeground == original.TerminalForeground, "TerminalForeground round-trips");
		Check(loaded.SyntaxBackground == original.SyntaxBackground, "SyntaxBackground round-trips");
		Check(loaded.SyntaxForeground == original.SyntaxForeground, "SyntaxForeground round-trips");
		Check(loaded.SyntaxKeyword == original.SyntaxKeyword, "SyntaxKeyword round-trips");
		Check(loaded.SyntaxStringLiteral == original.SyntaxStringLiteral, "SyntaxStringLiteral round-trips");
		Check(loaded.SyntaxComment == original.SyntaxComment, "SyntaxComment round-trips");
		Check(loaded.SyntaxLineNumberFg == original.SyntaxLineNumberFg, "SyntaxLineNumberFg round-trips");
		Check(loaded.SyntaxCursorColor == original.SyntaxCursorColor, "SyntaxCursorColor round-trips");
		Check(loaded.SyntaxSelectionBg == original.SyntaxSelectionBg, "SyntaxSelectionBg round-trips");
		Check(loaded.AdvancedOverrides.count("ImGuiCol_ChildBg") == 1 && loaded.AdvancedOverrides.at("ImGuiCol_ChildBg") == 0,
			"AdvancedOverrides round-trips its one entry");

		std::string parseError;
		Theme missing;
		Check(!ThemeManager::LoadThemeFile("does_not_exist.json", missing, parseError) && !parseError.empty(),
			"loading a missing file fails with a non-empty error, not a crash");

		std::filesystem::remove(std::filesystem::path(ThemeManager::ThemesFolder()) / "round_trip_test.json");

		GS_TRACE("ThemeLoadSaveTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Wire in and build to confirm it fails**

Expected: FAIL — `ThemeManager.h: No such file or directory`.

- [ ] **Step 3: Write `ThemeManager.h`'s load/save half**

```cpp
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

	inline std::string HexOr(const GS::JsonValue& value, const std::string& fallback)
	{
		std::string text = value.GetString(fallback);
		ImU32 dummy;
		return ParseHexColor(text, dummy) ? text : fallback;
	}

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

}
```

(`GS::JsonValue`'s member names/keys/`Has`/`GetString`/`GetFloat`/`Size`/
`KeyAt`/`ValueAt` are exactly as declared in `GS/src/GS/Json.h` — no new
JSON code needed on the reading side.)

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/themeloadsave.png --capture-step 2
```
Expected: `ThemeLoadSaveTest: 21 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

Also delete the `round_trip_test.json` it left in
`bin/Debug-linux-x86_64/TestEnv/assets/themes/` if the test's own cleanup
didn't run (e.g. an earlier failed attempt).

- [ ] **Step 6: Mark task done**

---

### Task 3: Live font reload on `ImGuiLayer`

**Files:**
- Modify: `GS/src/GS/ImGui/ImGuiLayer.h`, `GS/src/GS/ImGui/ImGuiLayer.cpp`

**Interfaces:**
- Consumes: Task 1's `ThemeFontSlot`
- Produces: `void ImGuiLayer::RequestFontReload(const ThemeFontSlot&
  controls, const ThemeFontSlot& editor, const ThemeFontSlot& terminal)`,
  `ImFont* ImGuiLayer::GetControlsFont() const`, `ImFont*
  ImGuiLayer::GetEditorFont() const`, `ImFont*
  ImGuiLayer::GetTerminalFont() const` — Task 4 calls `RequestFontReload`
  from `Apply()`; Tasks 6 and 7 call the two panel-specific getters via
  `GS::Application::Get().GetImGuiLayer()`.

No self-test — this only does anything with a live GL context and backend,
which this project's own convention (and the design spec's Testing
section) puts under visual-capture verification, not a self-test.

- [ ] **Step 1: Add the new members and methods to `ImGuiLayer.h`**

Add `#include "Theme.h"`-shaped forward declare — actually simplest is a
plain forward struct, since `ImGuiLayer` is engine-side (`GS/`) and
`ThemeFontSlot` is editor-side (`TestEnv/`); rather than reaching across
that boundary, declare the three fields inline as plain parameters instead
of taking a `ThemeFontSlot` by name:

```cpp
		// Runtime font switching, unlike the static SetFontPath above (which
		// only works pre-OnAttach) -- called any time after the ImGui
		// context exists. Queues the request; Begin() applies it at the
		// start of the next frame, since rebuilding io.Fonts mid-frame
		// would leave already-drawn widgets pointing at freed glyphs.
		void RequestFontReload(const std::string& controlsPath, float controlsSize,
			const std::string& editorPath, float editorSize,
			const std::string& terminalPath, float terminalSize)
		{
			m_PendingControlsPath = controlsPath; m_PendingControlsSize = controlsSize;
			m_PendingEditorPath = editorPath; m_PendingEditorSize = editorSize;
			m_PendingTerminalPath = terminalPath; m_PendingTerminalSize = terminalSize;
			m_FontReloadPending = true;
		}

		ImFont* GetControlsFont() const { return m_ControlsFont; }
		ImFont* GetEditorFont() const { return m_EditorFont; }
		ImFont* GetTerminalFont() const { return m_TerminalFont; }
	private:
		bool m_BlockEvents = true;
		bool m_DockspaceEnabled = true;
		bool m_ViewportsEnabled = false;

		static std::string s_FontPath;
		static float s_FontSizePixels;

		bool m_FontReloadPending = false;
		std::string m_PendingControlsPath, m_PendingEditorPath, m_PendingTerminalPath;
		float m_PendingControlsSize = 16.0f, m_PendingEditorSize = 16.0f, m_PendingTerminalSize = 16.0f;
		ImFont* m_ControlsFont = nullptr;
		ImFont* m_EditorFont = nullptr;
		ImFont* m_TerminalFont = nullptr;

		void ApplyPendingFontReload();
```

(Replace the file's existing `private:` block, which today only has the
first three bools and the two static members, with the version above --
adding the new members and the new private method declaration.)

- [ ] **Step 2: Set `m_ControlsFont` after the existing startup font load**

In `ImGuiLayer.cpp`'s `OnAttach` (the `if (!s_FontPath.empty()) { ... }`
block), capture what it loads:

```cpp
		if (!s_FontPath.empty())
		{
			ImFont* font = io.Fonts->AddFontFromFileTTF(s_FontPath.c_str(), s_FontSizePixels);
			if (!font)
			{
				GS_CORE_WARN("ImGuiLayer: could not load font '{0}', falling back to the default", s_FontPath);
				font = io.Fonts->AddFontDefault();
			}
			m_ControlsFont = m_EditorFont = m_TerminalFont = font;
		}
```

(All three getters point at the same one font until something actually
calls `RequestFontReload` -- matching today's real, single-font behavior
exactly, the same "don't change behavior while relocating it" care this
project's other refactors already take.)

- [ ] **Step 3: Implement `ApplyPendingFontReload` and call it from `Begin()`**

Add to `ImGuiLayer.cpp`:

```cpp
	void ImGuiLayer::ApplyPendingFontReload()
	{
		if (!m_FontReloadPending)
			return;
		m_FontReloadPending = false;

		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();

		auto loadOrDefault = [&io](const std::string& path, float size) -> ImFont*
		{
			if (path.empty())
				return io.Fonts->AddFontDefault();
			ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
			if (!font)
			{
				GS_CORE_WARN("ImGuiLayer: could not load font '{0}', falling back to the default", path);
				font = io.Fonts->AddFontDefault();
			}
			return font;
		};

		m_ControlsFont = loadOrDefault(m_PendingControlsPath, m_PendingControlsSize);
		m_EditorFont = loadOrDefault(m_PendingEditorPath, m_PendingEditorSize);
		m_TerminalFont = loadOrDefault(m_PendingTerminalPath, m_PendingTerminalSize);

		io.FontDefault = m_ControlsFont;
		io.Fonts->Build();

		// Zeroes the backend's font-texture handle; the ImGui_ImplOpenGL3_
		// NewFrame() call right after this function returns (see Begin(),
		// below) recreates it from the freshly-built atlas automatically --
		// that's the documented Dear ImGui pattern for runtime font
		// switching with this backend, not a workaround.
		ImGui_ImplOpenGL3_DestroyFontsTexture();
	}
```

In `Begin()`, call it as the very first line, before
`ImGui_ImplOpenGL3_NewFrame()`:

```cpp
	void ImGuiLayer::Begin()
	{
		ApplyPendingFontReload();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
```

- [ ] **Step 4: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 5: Visual capture**

Add a temporary hook in `TestApp.cpp` (after `PushLayer(new
EditorSceneView());`, the same spot earlier features hooked a one-off
capture setup):

```cpp
GS::Application::Get().GetImGuiLayer()->RequestFontReload(
    "assets/fonts/DejaVuSansMono.ttf", 32.0f,
    "assets/fonts/DejaVuSansMono.ttf", 12.0f,
    "assets/fonts/DejaVuSansMono.ttf", 12.0f);   // TEMPORARY -- Task 3 capture hook
```

```sh
./gs.py build
rm -f bin/Debug-linux-x86_64/TestEnv/imgui.ini
./gs.py run -- --hide-window --lockstep --capture /tmp/fontreload.png --capture-step 30
```
Expected: the general UI (menu bar, panel titles) renders visibly larger
than before (32px controls font) — confirms the reload actually replaced
the atlas and `io.FontDefault` took effect, not just that it compiled.
Remove the temporary hook afterward.

- [ ] **Step 6: Mark task done**

---

### Task 4: `ThemeManager` — role mapping, `Apply`, `Current`, `ListThemes`

**Files:**
- Modify: `TestEnv/src/ThemeManager.h`
- Test: `TestEnv/src/ThemeApplyTest.h` (temporary)

**Interfaces:**
- Consumes: Task 1's `Theme`/`Lighten`/`Darken`; Task 2's
  `LoadThemeFile`/`ThemesFolder`; Task 3's `ImGuiLayer::RequestFontReload`
  (via `GS::Application::Get().GetImGuiLayer()`)
- Produces: `void ThemeManager::Apply(const Theme& theme)`, `const Theme&
  ThemeManager::Current()`, `std::vector<std::string>
  ThemeManager::ListThemes()`, `bool ThemeManager::ApplyByFilename(const
  std::string& filename)`, `inline const char* ThemeManager::LastThemePath()
  { return "editor_last_theme.txt"; }` — Tasks 6-9 all call `Current()`;
  Task 8 calls `ApplyByFilename`/`LastThemePath`; Task 9's panel calls all
  four.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/ThemeApplyTest.h`:

```cpp
// TEMPORARY -- delete after verifying ThemeManager::Apply and ListThemes.
#pragma once
#include <GS.h>
#include <filesystem>
#include <fstream>

#include "ThemeManager.h"

namespace ThemeApplyTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		Theme theme;
		theme.Background = IM_COL32(0x10, 0x20, 0x30, 255);
		theme.Foreground = IM_COL32(0xf0, 0xe0, 0xd0, 255);
		theme.FrameBackground = IM_COL32(0x20, 0x30, 0x40, 255);
		theme.Accent = IM_COL32(0x30, 0x40, 0x50, 255);
		theme.Border = IM_COL32(0x40, 0x50, 0x60, 255);
		theme.AdvancedOverrides["ImGuiCol_ChildBg"] = IM_COL32(0, 0, 0, 0);

		ThemeManager::Apply(theme);

		ImGuiStyle& style = ImGui::GetStyle();
		Check(style.Colors[ImGuiCol_WindowBg] == ImGui::ColorConvertU32ToFloat4(theme.Background),
			"Apply writes Background into WindowBg");
		Check(style.Colors[ImGuiCol_Text] == ImGui::ColorConvertU32ToFloat4(theme.Foreground),
			"Apply writes Foreground into Text");
		Check(style.Colors[ImGuiCol_FrameBg] == ImGui::ColorConvertU32ToFloat4(theme.FrameBackground),
			"Apply writes FrameBackground into FrameBg");
		Check(style.Colors[ImGuiCol_Button] == ImGui::ColorConvertU32ToFloat4(theme.Accent),
			"Apply writes Accent into Button");
		Check(style.Colors[ImGuiCol_Border] == ImGui::ColorConvertU32ToFloat4(theme.Border),
			"Apply writes Border into Border");
		Check(style.Colors[ImGuiCol_ButtonHovered] == ImGui::ColorConvertU32ToFloat4(Lighten(theme.Accent, 0.15f)),
			"ButtonHovered is Accent lightened by the same formula Lighten() itself uses");
		Check(style.Colors[ImGuiCol_ChildBg] == ImGui::ColorConvertU32ToFloat4(IM_COL32(0, 0, 0, 0)),
			"the advanced override for ChildBg wins over the role mapping");

		Check(&ThemeManager::Current() != nullptr, "Current() returns a real reference");
		Check(ThemeManager::Current().Background == theme.Background, "Current() reflects the just-applied theme");

		std::filesystem::create_directories(ThemeManager::ThemesFolder());
		{
			std::ofstream a(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_a.json");
			a << "{}";
		}
		{
			std::ofstream b(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_b.json");
			b << "{}";
		}
		{
			std::ofstream notjson(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_readme.txt");
			notjson << "not a theme";
		}

		auto themes = ThemeManager::ListThemes();
		bool hasA = std::find(themes.begin(), themes.end(), "zz_test_a.json") != themes.end();
		bool hasB = std::find(themes.begin(), themes.end(), "zz_test_b.json") != themes.end();
		bool hasTxt = std::find(themes.begin(), themes.end(), "zz_test_readme.txt") != themes.end();
		Check(hasA && hasB, "ListThemes finds both seeded .json files");
		Check(!hasTxt, "ListThemes excludes a non-.json file in the same folder");

		std::filesystem::remove(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_a.json");
		std::filesystem::remove(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_b.json");
		std::filesystem::remove(std::filesystem::path(ThemeManager::ThemesFolder()) / "zz_test_readme.txt");

		GS_TRACE("ThemeApplyTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Confirm it fails to build** (`Apply`/`Current`/`ListThemes`
don't exist yet)

- [ ] **Step 3: Add the role mapping and `Apply`/`Current`/`ListThemes`/`ApplyByFilename` to `ThemeManager.h`**

```cpp
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

		// Advanced overrides win last.
		for (auto& [name, color] : theme.AdvancedOverrides)
		{
			for (int i = 0; i < ImGuiCol_COUNT; i++)
				if (name == ImGui::GetStyleColorName(i))
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
```

(`ImGui::GetStyleColorName` is a real, existing ImGui function -- returns
the literal `"ImGuiCol_Text"`-style name for an index, which is exactly
what makes matching `AdvancedOverrides`' string keys against the enum
possible without a second, hand-maintained name table.)

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/themeapply.png --capture-step 2
```
Expected: `ThemeApplyTest: 9 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Mark task done**

---

### Task 5: Write `basic.json` and `default.json`

**Files:**
- Create: `TestEnv/assets/themes/basic.json`, `TestEnv/assets/themes/default.json`
- Test: `TestEnv/src/ThemeFilesTest.h` (temporary)

**Interfaces:**
- Consumes: Task 2's `LoadThemeFile`
- Produces: the two theme files every later task (6-9) assumes exist.

Real values, not guessed: `basic.json`'s 5 roles come from
`GS/vendor/imgui/imgui_draw.cpp`'s `StyleColorsDark()` (`WindowBg
(0.06,0.06,0.06,0.94)`, `Text (1,1,1,1)`, `FrameBg (0.16,0.29,0.48,0.54)`,
`Button (0.26,0.59,0.98,0.40)`, `Border (0.43,0.43,0.50,0.50)`); its
terminal colors are `GS/vendor/libvterm/src/pen.c`'s
`vterm_state_newpen` default (`fg (240,240,240)`, `bg (0,0,0)`); its
`editorSyntax` block is `TestEnv/src/EditorTheme.h`'s current
`EverforestDark()` values, unmodified. `default.json`'s `editorSyntax`
block is identical to `basic.json`'s (Everforest is already correct
there); its 5 roles and terminal colors are also Everforest, sourced from
the same `everforest.vim` palette `EditorTheme.h`'s own comments already
cite (`bg0`/`fg`/`grey0`/`grey1`/green/red).

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/ThemeFilesTest.h`:

```cpp
// TEMPORARY -- delete after verifying basic.json/default.json.
#pragma once
#include <GS.h>

#include "ThemeManager.h"

namespace ThemeFilesTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		Theme basic;
		std::string error;
		Check(ThemeManager::LoadThemeFile("basic.json", basic, error), "basic.json loads: " + error);
		Check(basic.Background == IM_COL32(15, 15, 15, 240), "basic.json Background matches StyleColorsDark's WindowBg (0.06,0.06,0.06,0.94)");
		Check(basic.Foreground == IM_COL32(255, 255, 255, 255), "basic.json Foreground matches StyleColorsDark's Text (white)");
		Check(basic.TerminalForeground == IM_COL32(240, 240, 240, 255), "basic.json terminal foreground matches libvterm's real default (240,240,240)");
		Check(basic.TerminalBackground == IM_COL32(0, 0, 0, 255), "basic.json terminal background matches libvterm's real default (0,0,0)");
		Check(basic.SyntaxBackground == IM_COL32(0x2d, 0x35, 0x3b, 255), "basic.json keeps today's real Everforest syntax background");
		Check(basic.AdvancedOverrides.count("ImGuiCol_ChildBg") == 1, "basic.json overrides ChildBg back to transparent");

		Theme def;
		Check(ThemeManager::LoadThemeFile("default.json", def, error), "default.json loads: " + error);
		Check(def.Background == IM_COL32(0x2d, 0x35, 0x3b, 255), "default.json Background matches Everforest bg0, same as its own syntax background");
		Check(def.SyntaxBackground == def.Background, "default.json's editor and general backgrounds are the same coherent color");
		Check(def.SyntaxKeyword == IM_COL32(0xe6, 0x7e, 0x80, 255), "default.json keeps the real, sourced Everforest keyword color");

		GS_TRACE("ThemeFilesTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Confirm it fails** (files don't exist yet, `LoadThemeFile`
returns false)

- [ ] **Step 3: Write `TestEnv/assets/themes/basic.json`**

```json
{
  "name": "Basic",
  "colors": {
    "background": "#0f0f0ff0",
    "foreground": "#ffffffff",
    "frameBackground": "#294a7a8a",
    "accent": "#4296fa66",
    "border": "#6e6e8080"
  },
  "fonts": {
    "controls": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "editor": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "terminal": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 }
  },
  "terminal": {
    "background": "#000000ff",
    "foreground": "#f0f0f0ff"
  },
  "editorSyntax": {
    "background": "#2d353bff",
    "foreground": "#d3c6aaff",
    "keyword": "#e67e80ff",
    "stringLiteral": "#a7c080ff",
    "comment": "#859289ff",
    "lineNumber": "#7a8478ff",
    "cursor": "#d3c6aab4",
    "selectionBg": "#424a50ff"
  },
  "advanced": {
    "ImGuiCol_ChildBg": "#00000000"
  }
}
```

(`cursor`'s alpha `0xb4` is 180 decimal, matching `EditorTheme.h`'s
`IM_COL32(0xd3, 0xc6, 0xaa, 180)` exactly.)

- [ ] **Step 4: Write `TestEnv/assets/themes/default.json`**

```json
{
  "name": "Everforest",
  "colors": {
    "background": "#2d353bff",
    "foreground": "#d3c6aaff",
    "frameBackground": "#3a4147ff",
    "accent": "#a7c080ff",
    "border": "#4f585eff"
  },
  "fonts": {
    "controls": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "editor": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 },
    "terminal": { "path": "assets/fonts/DejaVuSansMono.ttf", "size": 16 }
  },
  "terminal": {
    "background": "#2d353bff",
    "foreground": "#d3c6aaff"
  },
  "editorSyntax": {
    "background": "#2d353bff",
    "foreground": "#d3c6aaff",
    "keyword": "#e67e80ff",
    "stringLiteral": "#a7c080ff",
    "comment": "#859289ff",
    "lineNumber": "#7a8478ff",
    "cursor": "#d3c6aab4",
    "selectionBg": "#424a50ff"
  },
  "advanced": {}
}
```

(`frameBackground`/`border` are `everforest.vim`'s `bg2`/`bg3` steps one
and two lighter than `bg0` -- verify the exact two hex values against that
file at implementation time the same way `EditorTheme.h`'s existing
comments already point to it, rather than re-typing them here as if this
plan were the source of truth for the palette.)

- [ ] **Step 5: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/themefiles.png --capture-step 2
```
Expected: `ThemeFilesTest: 10 passed, 0 failed`.

- [ ] **Step 6: Remove the temporary test**

- [ ] **Step 7: Mark task done**

---

### Task 6: `TextEditorPanel` reads the live theme

**Files:**
- Modify: `TestEnv/src/TextEditorPanel.h:201-213` (`OnImGuiRender`'s top),
  `:251-252` (the render call), `:798` (the old hardcoded member)

**Interfaces:**
- Consumes: Task 4's `ThemeManager::Current()`; Task 3's
  `ImGuiLayer::GetEditorFont()`

- [ ] **Step 1: Remove the hardcoded theme member**

Delete line 798: `EditorTheme m_Theme = EverforestDark();`. Remove the
`#include "EditorTheme.h"` from this file's includes if nothing else in it
still needs `EditorTheme`/`EverforestDark` (check with `grep -n
EditorTheme TestEnv/src/TextEditorPanel.h` after this change — anything
left besides the deleted line means keep the include).

- [ ] **Step 2: Read the theme's syntax fields directly at render time**

Replace the call at line 251:

```cpp
RenderBufferToGrid(m_Buffer, m_Grid, m_Theme, m_ScrollRow, gutterDigits, gutterCols, textCols, rows);
```

with:

```cpp
const Theme& theme = ThemeManager::Current();
EditorTheme syntaxTheme{};
syntaxTheme.Background = theme.SyntaxBackground;
syntaxTheme.Foreground = theme.SyntaxForeground;
syntaxTheme.LineNumberFg = theme.SyntaxLineNumberFg;
syntaxTheme.CurrentLineNumberFg = theme.SyntaxForeground;
syntaxTheme.CursorColor = theme.SyntaxCursorColor;
syntaxTheme.Keyword = theme.SyntaxKeyword;
syntaxTheme.StringLiteral = theme.SyntaxStringLiteral;
syntaxTheme.Comment = theme.SyntaxComment;
syntaxTheme.SelectionBg = theme.SyntaxSelectionBg;
RenderBufferToGrid(m_Buffer, m_Grid, syntaxTheme, m_ScrollRow, gutterDigits, gutterCols, textCols, rows);
```

(`RenderBufferToGrid`'s own signature — `const EditorTheme&` — is
unchanged; this keeps `EditorTheme.h`'s existing struct as the internal
type that function and `TokenizeLineColors` already use, and only changes
where its values come from each frame. Keep `#include "EditorTheme.h"`
and add `#include "ThemeManager.h"` to this file's includes.)

- [ ] **Step 3: Wrap the grid render in the editor font**

Replace line 252 (`m_Grid.Render();`) with:

```cpp
ImGui::PushFont(GS::Application::Get().GetImGuiLayer()->GetEditorFont());
m_Grid.Render();
ImGui::PopFont();
```

- [ ] **Step 4: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 5: Visual capture**

```sh
rm -f bin/Debug-linux-x86_64/TestEnv/imgui.ini
./gs.py run -- --hide-window --lockstep --capture /tmp/editortheme.png --capture-step 30
```
(Add a temporary `ImGui::SetWindowFocus("Editor");` at the top of
`OnImGuiRender`, per this plan's Global Constraints note, to bring the tab
forward for the capture; remove it after.) Confirm the text editor still
renders with Everforest colors (background/foreground/syntax) exactly as
before this task — this task changes *where* the colors come from, not
what they are, since `default.json`/`basic.json`'s syntax blocks are
today's real values.

- [ ] **Step 6: Mark task done**

---

### Task 7: `TerminalPanel` reads the live theme

**Files:**
- Modify: `TestEnv/src/TerminalPanel.h:69-132` (`OnImGuiRender`), member
  section near `:228-229`

**Interfaces:**
- Consumes: Task 4's `ThemeManager::Current()`; Task 3's
  `ImGuiLayer::GetTerminalFont()`

- [ ] **Step 1: Add cached last-applied terminal colors**

Near the existing `VTerm* m_Vt = nullptr;` / `VTermScreen* m_Screen =
nullptr;` members:

```cpp
	ImU32 m_LastTerminalBg = 0;
	ImU32 m_LastTerminalFg = 0;
	bool m_TerminalColorsInitialised = false;
```

- [ ] **Step 2: Apply theme colors once per frame, only on change**

At the top of `OnImGuiRender`, right after `bool visible =
ImGui::Begin("Terminal");` (line 71):

```cpp
const Theme& theme = ThemeManager::Current();
if (!m_TerminalColorsInitialised || theme.TerminalBackground != m_LastTerminalBg || theme.TerminalForeground != m_LastTerminalFg)
{
	if (m_Screen)
	{
		VTermColor fg, bg;
		ImVec4 fgFloat = ImGui::ColorConvertU32ToFloat4(theme.TerminalForeground);
		ImVec4 bgFloat = ImGui::ColorConvertU32ToFloat4(theme.TerminalBackground);
		vterm_color_rgb(&fg, (uint8_t)(fgFloat.x * 255.0f), (uint8_t)(fgFloat.y * 255.0f), (uint8_t)(fgFloat.z * 255.0f));
		vterm_color_rgb(&bg, (uint8_t)(bgFloat.x * 255.0f), (uint8_t)(bgFloat.y * 255.0f), (uint8_t)(bgFloat.z * 255.0f));
		vterm_screen_set_default_colors(m_Screen, &fg, &bg);
	}
	m_LastTerminalBg = theme.TerminalBackground;
	m_LastTerminalFg = theme.TerminalForeground;
	m_TerminalColorsInitialised = true;
}
```

(Guarded on `m_Screen` being non-null: a fresh panel with no shell spawned
yet has no screen to color, and the same check runs again next frame once
one exists — cheap, and consistent with how this same function already
checks `m_Vt`/`m_Pty.IsAlive()` before doing anything screen-related.)

- [ ] **Step 3: Wrap the grid render in the terminal font**

Replace the `m_Grid.Render();` call (line 129) with:

```cpp
ImGui::PushFont(GS::Application::Get().GetImGuiLayer()->GetTerminalFont());
m_Grid.Render();
ImGui::PopFont();
```

(The `avail`/`cellWidth`/`cellHeight` measurement above it, lines 76-80,
already reads `ImGui::GetFont()` — since that measurement happens *before*
this push in the function's current order, either move the `PushFont` up
to wrap the measurement too, or accept that the terminal's column/row count
is computed from whatever font was active before this push. Move the
`PushFont` up to right after line 75 (before `avail` is read) and the
matching `PopFont` to just after the `m_Grid.Render()` call, so both the
sizing and the rendering agree on which font is active — sizing off one
font and rendering with another would make columns miscount.)

Add `#include "ThemeManager.h"` to this file's includes.

- [ ] **Step 4: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 5: Visual capture**

```sh
rm -f bin/Debug-linux-x86_64/TestEnv/imgui.ini
./gs.py run -- --hide-window --lockstep --capture /tmp/terminaltheme.png --capture-step 60
```
(`ImGui::SetWindowFocus("Terminal");`, temporary, same pattern as Task 6 —
give it more warmup frames than usual since a shell needs to actually spawn
and print its prompt before there's visible text to check the color of.)
Confirm terminal text renders in the active theme's foreground/background
rather than libvterm's own hardcoded default.

- [ ] **Step 6: Mark task done**

---

### Task 8: Load the last-applied theme (or `default.json`) at startup

**Files:**
- Modify: `TestEnv/src/EditorProject.h` (near `LoadEditorProjectFromCommandLine`)

**Interfaces:**
- Consumes: Task 4's `ThemeManager::ApplyByFilename`/`LastThemePath`

- [ ] **Step 1: Add the startup load, mirroring the existing last-scene pattern**

In `EditorProject.h`, add right after the existing `#include`s (this file
already includes `<fstream>`; add `#include "ThemeManager.h"`):

```cpp
inline void LoadEditorThemeFromLastRun()
{
	std::ifstream last(ThemeManager::LastThemePath());
	std::string filename;
	if (last && std::getline(last, filename) && !filename.empty()
		&& std::filesystem::exists(std::filesystem::path(ThemeManager::ThemesFolder()) / filename))
	{
		ThemeManager::ApplyByFilename(filename);
		return;
	}

	// No last-theme record, or it names a file that's gone -- same "not a
	// warning, the first run has neither" reasoning LoadEditorProjectFromCommandLine
	// already uses for the last-scene file.
	ThemeManager::ApplyByFilename("default.json");
}
```

- [ ] **Step 2: Call it from `TestApp.cpp`**

Right after `PushLayer(new EditorSceneView());` and before
`LoadEditorProjectFromCommandLine();` (themes should be applied before any
demo's warmup might read font metrics):

```cpp
LoadEditorThemeFromLastRun();
```

- [ ] **Step 3: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 4: Visual capture**

```sh
rm bin/Debug-linux-x86_64/TestEnv/editor_last_theme.txt 2>/dev/null; true
./gs.py run -- --hide-window --lockstep --capture /tmp/startuptheme.png --capture-step 30
```
Expected: the capture shows Everforest colors (no last-theme file present,
so `default.json` applies) — confirms first-run behavior. Then:
```sh
echo -n "basic.json" > bin/Debug-linux-x86_64/TestEnv/editor_last_theme.txt
./gs.py run -- --hide-window --lockstep --capture /tmp/startuptheme2.png --capture-step 30
```
Expected: this capture shows the plain dark look instead — confirms the
last-applied theme is actually remembered and reapplied, not just always
falling back to `default.json`.

- [ ] **Step 5: Mark task done**

---

### Task 9: The "Appearance" panel

**Files:**
- Create: `TestEnv/src/AppearancePanel.h`
- Modify: `TestEnv/src/TestApp.cpp` (push the new layer)

**Interfaces:**
- Consumes: Task 4's `ThemeManager::ListThemes`/`ApplyByFilename`/
  `SaveThemeFile`/`Current`; Task 1's `Theme`
- Produces: nothing further downstream — this is the feature's last task.

- [ ] **Step 1: Write `AppearancePanel.h`**

```cpp
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
			browser.Open("Choose Font", FileBrowserPopup::Mode::PickFile, "assets/fonts", ".ttf");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::DragFloat("Size", &slot.Size, 0.5f, 6.0f, 72.0f))
			{}
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
```

(`FileBrowserPopup::Open`'s exact signature — `Open(title, Mode, rootDir,
filterExtension)` — and `HasResult()`/`TakeResult()`/`Draw()` are as
declared in `TestEnv/src/FileBrowserPopup.h`, the same component "Add
Script" already uses.)

- [ ] **Step 2: Push the layer**

In `TestApp.cpp`, add `#include "AppearancePanel.h"` and, alongside
`PushLayer(new ProfilerPanel());`:

```cpp
PushLayer(new AppearancePanel());
```

- [ ] **Step 3: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 4: Visual capture**

```sh
rm -f bin/Debug-linux-x86_64/TestEnv/imgui.ini
```
Add a temporary `ImGui::SetWindowFocus("Appearance");` at the top of
`AppearancePanel::OnImGuiRender`, then:
```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/appearance.png --capture-step 30
```
Confirm the panel shows both theme files in the list, 5 color swatches, 3
font rows with working path fields and Browse buttons, 2 terminal color
swatches, and the Save As row. Remove the temporary focus call after.

- [ ] **Step 5: Mark task done**

---

## Plan self-review notes

- **Spec coverage:** `Theme` struct + hex helpers (Task 1); JSON load/save
  (Task 2); live font reload (Task 3); role mapping + `Apply`/`Current`/
  `ListThemes` (Task 4); `basic.json`/`default.json` with real, sourced
  values (Task 5); `TextEditorPanel` and `TerminalPanel` reading the live
  theme plus their own fonts (Tasks 6-7); startup last-theme persistence
  (Task 8); the Appearance panel itself, including Save As (Task 9). Every
  in-scope bullet from the spec has a task; every explicitly-deferred item
  (a picker per raw `ImGuiCol_`, the 16-color ANSI palette, non-TTF fonts,
  folder file-watching, per-project themes) has none, on purpose.
- **Type consistency checked:** `Theme`/`ThemeFontSlot` (Task 1) fields are
  exactly what Task 2's JSON keys map to, what Task 4's `Apply` reads, and
  what Task 9's panel edits — no renamed field anywhere. `ImGuiLayer::
  RequestFontReload`'s six-parameter signature (Task 3) matches its one
  real call site in `ThemeManager::Apply` (Task 4). `ThemeManager::
  Current()`'s return type (`const Theme&`) matches every call site in
  Tasks 6, 7, and 9.
