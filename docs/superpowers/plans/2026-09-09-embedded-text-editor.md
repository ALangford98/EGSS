# Embedded Text Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A self-contained text editor (no Neovim) living in a new "Editor" tab that shares the center viewport with the existing "Scene" view, with line numbers, indent-preserving newlines, and an Everforest-like theme.

**Architecture:** Two independent pieces land first (center-viewport tabbing in `EditorShell.h`; a pure-logic `TextBuffer`), then a `TextEditorPanel` glues a `CellGrid` (reused unchanged from the embedded terminal) to both.

**Tech Stack:** C++17, ImGui (docking branch, already vendored). No new vendored libraries.

**Spec:** `docs/superpowers/specs/2026-09-09-embedded-text-editor-design.md`

## Global Constraints

- **No git commits.** This repo's `CLAUDE.md` forbids commits to `main` and pushing. `git add` after each task's review passes is the task-boundary marker, same adaptation as the two prior plans in this repo.
- **Temporary self-test per task**, deleted once it has done its job. `git grep TEMPORARY` must be clean before Task 3 (the last task) finishes.
- **`./gs.py build all` must succeed with zero errors before Task 3 finishes.**
- **No existing demo's behavior, capture, or replay may change.** `EditorShell.h`'s `OnImGuiRender` (Task 1's file) never runs under `--hide-ui`/`--capture`/`--play` (same guarantee the terminal and menu-bar plans already established and verified) — so nothing in this plan can affect a headless capture's bytes. Task 1 additionally verifies non-hidden demo captures still render correctly through the new "Scene" tab, since that task changes the mechanism every demo's viewport already depends on.
- **ASCII-only.** `CellGrid`'s existing renderer only draws codepoints `< 128` — inherited here without modification. A loaded file's non-ASCII bytes render as blank cells, same disclosed cut the embedded terminal already made.

---

### Task 1: Center-viewport tabbing (`EditorShell.h`)

**Files:**
- Modify: `TestEnv/src/EditorShell.h`

**Interfaces:**
- Produces: a `"Scene"` and a `"Editor"` window, both docked into the center node as tabs. `"Editor"` is a stub in this task (`TextDisabled("Not built yet")`), replaced by the real thing in Task 3. `g_Viewport` (`Demo.h`) keeps its existing meaning and type (`ViewportRect`) — only *how* it's computed changes.

Today the center dock node has no ImGui window in it at all: `PassthruCentralNode` just means "paint nothing where nothing is docked," and `g_Viewport` is read straight off `DockBuilderGetCentralNode()`'s raw rect every frame (`EditorShell.h:133-146`), independent of any window. This task replaces that with two real windows.

- [ ] **Step 1: Read the current `OnImGuiRender()` and `BuildLayout()` in full**

```bash
grep -n "OnImGuiRender\|BuildLayout\|g_Viewport\|DockBuilderDockWindow\|DockBuilderFinish" TestEnv/src/EditorShell.h
```

Confirm the line numbers below still match what's on disk before editing — if the file has drifted, adapt the edits to the real surrounding code rather than applying a stale diff blindly.

- [ ] **Step 2: Replace the central-node viewport calculation with two real windows**

Find this block in `OnImGuiRender()` (currently around lines 133-146):

```cpp
		if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock))
		{
			ImVec2 size = ImGui::GetIO().DisplaySize;

			// **OpenGL's origin is bottom-left and ImGui's is top-left.**
			// Reading the rect straight through puts the viewport upside down
			// in the window -- the demo appears at the top when the panels are
			// at the bottom, which reads as a layout bug rather than an axis
			// one.
			g_Viewport.X = (int)central->Pos.x;
			g_Viewport.Y = (int)(size.y - central->Pos.y - central->Size.y);
			g_Viewport.Width = (int)central->Size.x;
			g_Viewport.Height = (int)central->Size.y;
		}

		ImGui::End();
```

Replace it with:

```cpp
		ImGui::End();

		// "Scene" and "Editor" tab together in the center node. g_Viewport
		// reflects whichever one is the selected tab this frame (or neither,
		// same invalid-rect meaning the !g_EditorShell branch above already
		// uses) -- ImGui::Begin() returns false for a docked window that
		// isn't the currently selected tab, which is exactly the signal we
		// need. Under --hide-ui neither Begin() call below ever runs (this
		// whole function returns before reaching them, via the !g_EditorShell
		// check, on any run that never draws UI at all -- and on a normal
		// run where the UI later gets hidden mid-session, OnImGuiRender
		// itself is simply never invoked by Application.cpp), so g_Viewport
		// reverts to the full-window rect exactly as before this change.
		g_Viewport = GS::ViewportRect();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		bool sceneVisible = ImGui::Begin("Scene", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground);
		ImGui::PopStyleVar();

		if (sceneVisible)
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImVec2 size = ImGui::GetContentRegionAvail();
			ImVec2 displaySize = ImGui::GetIO().DisplaySize;

			// Same axis flip the old central-node code needed: OpenGL's
			// origin is bottom-left, ImGui's is top-left.
			g_Viewport.X = (int)pos.x;
			g_Viewport.Y = (int)(displaySize.y - pos.y - size.y);
			g_Viewport.Width = (int)size.x;
			g_Viewport.Height = (int)size.y;
		}
		ImGui::End();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar);
		ImGui::PopStyleVar();
		ImGui::TextDisabled("Not built yet -- text editor sub-project.");
		ImGui::End();
```

Check `ViewportRect`'s real namespace before using `GS::ViewportRect()` above — `grep -n "struct ViewportRect\|class ViewportRect" TestEnv/src/Demo.h GS/src -r` and use whatever qualification (or none, if it's unqualified in this file already) the existing code at `EditorShell.h:83`'s `g_Viewport = ViewportRect();` uses. Match that exactly rather than guessing a namespace.

- [ ] **Step 3: Dock the two new windows in `BuildLayout()`**

Find the end of `BuildLayout()`, just before `ImGui::DockBuilderFinish(dock);` (the `centre` variable at that point holds whatever remains after the Left/Right/Bottom splits — the actual central dock node id). Add, right before that line:

```cpp
		ImGui::DockBuilderDockWindow("Scene", centre);
		ImGui::DockBuilderDockWindow("Editor", centre);
```

Order matters: docking `"Scene"` first makes it the default-selected tab on a fresh layout (matches today's behavior — the scene is what you see on first boot).

- [ ] **Step 4: Build and verify no existing demo capture changed**

```bash
./gs.py build
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/et_cube3d_hideui.png --capture-step 5
```

Compare this capture's bytes against a capture taken from the current `main` (before this task's edit) the same way — they must be byte-identical. `EditorShell::OnImGuiRender` never runs under `--hide-ui` regardless of what's inside it, so this should hold trivially; confirm it does rather than assuming.

- [ ] **Step 5: Verify the "Scene" tab still shows the demo correctly, and the tab strip appears**

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --capture /tmp/et_cube3d_ui.png --capture-step 5
```

(No `--hide-ui` this time — panels render.) Open the PNG and confirm: the demo's 3D content renders inside the center area (not blank/black), and a small tab strip reading "Scene" / "Editor" appears at the very top of that area — a new, expected sliver of chrome this task intentionally adds, not a regression. Also boot the actual editor headless with no `--demo` for a sanity check that "Editor" opens the stub text correctly when clicked/selected as a tab — since clicking can't be driven headlessly here, confirming the stub window exists and reads "Not built yet -- text editor sub-project." via a capture with `ImGui::SetNextWindowFocus()`-style forced-tab-selection isn't necessary for this task; visually confirming the "Scene" tab (the default-selected one) renders correctly is the load-bearing check here, since Task 3 will re-verify the "Editor" tab once it has real content.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/src/EditorShell.h
git status --short
```

---

### Task 2: `TextBuffer.h` — pure text-buffer data and edit operations

**Files:**
- Create: `TestEnv/src/TextBuffer.h`
- Create (temporary): `TestEnv/src/TextBufferTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `class TextBuffer` with `LineCount()`, `Line(int row)`, `CursorRow()`, `CursorCol()`, `InsertChar(char)`, `InsertNewline()`, `Backspace()`, `Delete()`, `InsertTab()`, `MoveLeft/Right/Up/Down()`, `MoveHome/End()`, `MovePageUp/Down(int rows)`, `LoadFromFile(const std::string&)`, `SaveToFile(const std::string&) const`. Task 3 consumes all of this exactly.

No dependency on Task 1 — independent files, can be built and reviewed on its own.

- [ ] **Step 1: Write `TextBuffer.h`**

```cpp
#pragma once

// Pure text-buffer data and edit operations for the embedded text editor.
// No ImGui, no rendering -- TextEditorPanel (Task 3) copies this into a
// CellGrid. ASCII-only, byte-indexed columns: inherits CellGrid's existing
// ASCII rendering limit (a loaded file's non-ASCII bytes will show as
// blank cells, same disclosed cut the embedded terminal already made).

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

class TextBuffer
{
public:
	TextBuffer() : m_Lines(1) {}

	int LineCount() const { return (int)m_Lines.size(); }
	const std::string& Line(int row) const { return m_Lines[(size_t)row]; }

	int CursorRow() const { return m_CursorRow; }
	int CursorCol() const { return m_CursorCol; }

	void InsertChar(char c)
	{
		m_Lines[(size_t)m_CursorRow].insert((size_t)m_CursorCol, 1, c);
		m_CursorCol++;
		m_DesiredCol = m_CursorCol;
	}

	// Splits the current line at the cursor. The new line inherits the
	// current line's leading whitespace, so pressing Enter mid-block keeps
	// the same indent -- the "line indenting" this editor exists to have.
	void InsertNewline()
	{
		std::string& current = m_Lines[(size_t)m_CursorRow];
		std::string indent = LeadingWhitespace(current);
		std::string rest = current.substr((size_t)m_CursorCol);
		current.erase((size_t)m_CursorCol);

		m_Lines.insert(m_Lines.begin() + m_CursorRow + 1, indent + rest);
		m_CursorRow++;
		m_CursorCol = (int)indent.size();
		m_DesiredCol = m_CursorCol;
	}

	void Backspace()
	{
		if (m_CursorCol > 0)
		{
			m_Lines[(size_t)m_CursorRow].erase((size_t)m_CursorCol - 1, 1);
			m_CursorCol--;
		}
		else if (m_CursorRow > 0)
		{
			int prevLen = (int)m_Lines[(size_t)m_CursorRow - 1].size();
			m_Lines[(size_t)m_CursorRow - 1] += m_Lines[(size_t)m_CursorRow];
			m_Lines.erase(m_Lines.begin() + m_CursorRow);
			m_CursorRow--;
			m_CursorCol = prevLen;
		}
		m_DesiredCol = m_CursorCol;
	}

	void Delete()
	{
		std::string& current = m_Lines[(size_t)m_CursorRow];
		if (m_CursorCol < (int)current.size())
		{
			current.erase((size_t)m_CursorCol, 1);
		}
		else if (m_CursorRow + 1 < LineCount())
		{
			current += m_Lines[(size_t)m_CursorRow + 1];
			m_Lines.erase(m_Lines.begin() + m_CursorRow + 1);
		}
	}

	void InsertTab()
	{
		for (int i = 0; i < 4; i++)
			InsertChar(' ');
	}

	void MoveLeft()
	{
		if (m_CursorCol > 0)
			m_CursorCol--;
		else if (m_CursorRow > 0)
		{
			m_CursorRow--;
			m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveRight()
	{
		if (m_CursorCol < (int)m_Lines[(size_t)m_CursorRow].size())
			m_CursorCol++;
		else if (m_CursorRow + 1 < LineCount())
		{
			m_CursorRow++;
			m_CursorCol = 0;
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveUp() { MoveVertical(-1); }
	void MoveDown() { MoveVertical(1); }

	void MoveHome()
	{
		m_CursorCol = 0;
		m_DesiredCol = m_CursorCol;
	}

	void MoveEnd()
	{
		m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		m_DesiredCol = m_CursorCol;
	}

	void MovePageUp(int rows) { MoveVertical(-rows); }
	void MovePageDown(int rows) { MoveVertical(rows); }

	bool LoadFromFile(const std::string& path)
	{
		std::ifstream file(path);
		if (!file.is_open())
			return false;

		m_Lines.clear();
		std::string line;
		while (std::getline(file, line))
			m_Lines.push_back(line);
		if (m_Lines.empty())
			m_Lines.push_back("");

		m_CursorRow = 0;
		m_CursorCol = 0;
		m_DesiredCol = 0;
		return true;
	}

	bool SaveToFile(const std::string& path) const
	{
		std::ofstream file(path);
		if (!file.is_open())
			return false;

		for (size_t i = 0; i < m_Lines.size(); i++)
		{
			file << m_Lines[i];
			if (i + 1 < m_Lines.size())
				file << '\n';
		}
		return true;
	}

private:
	static std::string LeadingWhitespace(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		return line.substr(0, i);
	}

	void MoveVertical(int delta)
	{
		int target = m_CursorRow + delta;
		if (target < 0)
			target = 0;
		if (target >= LineCount())
			target = LineCount() - 1;
		m_CursorRow = target;
		m_CursorCol = std::min(m_DesiredCol, (int)m_Lines[(size_t)m_CursorRow].size());
	}

	std::vector<std::string> m_Lines;
	int m_CursorRow = 0;
	int m_CursorCol = 0;
	int m_DesiredCol = 0; // preserved across vertical moves through shorter lines
};
```

- [ ] **Step 2: Write the temporary self-test**

Create `TestEnv/src/TextBufferTest.h`:

```cpp
// TEMPORARY -- delete after verifying TextBuffer (Task 2 of the embedded-
// text-editor plan).
#pragma once
#include <GS.h>
#include "TextBuffer.h"
#include <cstdio>

namespace TextBufferTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        // Typing and indent-preserving newline.
        TextBuffer buf1;
        for (char c : std::string("    if x:")) buf1.InsertChar(c);
        Check(buf1.Line(0) == "    if x:", "typed line matches exactly");
        buf1.InsertNewline();
        Check(buf1.LineCount() == 2, "newline adds a second line");
        Check(buf1.Line(1) == "    ", "new line inherits the 4-space indent");
        Check(buf1.CursorRow() == 1 && buf1.CursorCol() == 4, "cursor lands right after the copied indent");

        // Backspace joins lines at column 0.
        TextBuffer buf2;
        for (char c : std::string("ab")) buf2.InsertChar(c);
        buf2.InsertNewline();
        for (char c : std::string("cd")) buf2.InsertChar(c);
        buf2.MoveHome();
        Check(buf2.CursorCol() == 0, "MoveHome moves to column 0");
        buf2.Backspace();
        Check(buf2.LineCount() == 1, "Backspace at column 0 joins with the previous line");
        Check(buf2.Line(0) == "abcd", "joined line is the concatenation");
        Check(buf2.CursorRow() == 0 && buf2.CursorCol() == 2, "cursor lands at the join point");

        // Delete joins lines from the end.
        TextBuffer buf3;
        for (char c : std::string("ab")) buf3.InsertChar(c);
        buf3.InsertNewline();
        for (char c : std::string("cd")) buf3.InsertChar(c);
        buf3.MoveUp();
        Check(buf3.CursorRow() == 0, "MoveUp moves to the previous line");
        buf3.MoveEnd();
        Check(buf3.CursorCol() == 2, "MoveEnd moves to the end of 'ab'");
        buf3.Delete();
        Check(buf3.LineCount() == 1, "Delete at end-of-line joins with the next line");
        Check(buf3.Line(0) == "abcd", "joined line is the concatenation");

        // Desired column preserved through a shorter line.
        TextBuffer buf4;
        for (char c : std::string("abcdef")) buf4.InsertChar(c);
        buf4.InsertNewline();
        for (char c : std::string("ab")) buf4.InsertChar(c);
        buf4.InsertNewline();
        for (char c : std::string("abcdef")) buf4.InsertChar(c);
        buf4.MoveLeft();
        buf4.MoveLeft();
        Check(buf4.CursorCol() == 4, "two MoveLefts land on column 4");
        buf4.MoveUp();
        Check(buf4.CursorRow() == 1 && buf4.CursorCol() == 2, "moving up into the shorter line clamps to its length");
        buf4.MoveUp();
        Check(buf4.CursorRow() == 0 && buf4.CursorCol() == 4, "moving up again restores the original desired column 4");

        // Tab inserts 4 spaces.
        TextBuffer buf5;
        buf5.InsertTab();
        Check(buf5.Line(0) == "    ", "Tab inserts 4 spaces");
        Check(buf5.CursorCol() == 4, "cursor advances past the inserted spaces");

        // Save/load round-trip.
        TextBuffer buf6;
        for (char c : std::string("line one")) buf6.InsertChar(c);
        buf6.InsertNewline();
        for (char c : std::string("line two")) buf6.InsertChar(c);
        std::string path = "textbuffer_test_roundtrip.txt";
        Check(buf6.SaveToFile(path), "SaveToFile succeeds");

        TextBuffer buf7;
        Check(buf7.LoadFromFile(path), "LoadFromFile succeeds");
        Check(buf7.LineCount() == 2, "loaded buffer has 2 lines");
        Check(buf7.Line(0) == "line one", "first loaded line matches");
        Check(buf7.Line(1) == "line two", "second loaded line matches");
        std::remove(path.c_str());
    }
}
```

- [ ] **Step 3: Call it, build, and run**

Temporarily add `#include "TextBufferTest.h"` and `TextBufferTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 21 `[ok ]` lines, `g_Pass == 21`, `g_Fail == 0`. This test is pure logic (no GL/ImGui), so it runs and completes before the window even needs to exist.

- [ ] **Step 4: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/TextBufferTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty. Also confirm no `textbuffer_test_roundtrip.txt` file was left behind in the binary's working directory (`bin/Debug-linux-x86_64/TestEnv/`) — the test removes it itself, but check.

- [ ] **Step 5: Stage**

```bash
git add TestEnv/src/TextBuffer.h
git status --short
```

---

### Task 3: `EditorTheme.h` + `TextEditorPanel.h` — glue, and wiring into `EditorShell.h`

**Files:**
- Create: `TestEnv/src/EditorTheme.h`
- Create: `TestEnv/src/TextEditorPanel.h`
- Modify: `TestEnv/src/EditorShell.h` (replace the `"Editor"` stub from Task 1 with a real `TextEditorPanel`)
- Modify: `docs/STATE.md`
- Create (temporary): `TestEnv/src/TextEditorGlueTest.h`
- Modify (temporary, reverted before this task ends): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `TextBuffer` (Task 2), `CellGrid`/`Cell` (already in the repo from the embedded terminal plan — `Resize`/`At`/`Cols`/`Rows`/`SetCursor`/`Render`), the `"Editor"` dock tab (Task 1).
- Produces: `struct EditorTheme` + `EverforestDark()`; `class TextEditorPanel` with a single public `void OnImGuiRender()`; the free function `void RenderBufferToGrid(const TextBuffer&, CellGrid&, const EditorTheme&, int scrollRow, int gutterDigits, int gutterCols, int textCols, int rows)` (exposed, same reason `TerminalPanel`'s `CopyVTermScreenToGrid` was: so a self-test can exercise the buffer-to-grid glue directly, without a live ImGui frame). Nothing later in this plan consumes either — this is the last task.

- [ ] **Step 1: Verify the real Everforest palette before writing `EditorTheme.h`**

Fetch the published Everforest color palette (dark variant, medium contrast) — e.g. from `https://github.com/sainnhe/everforest`'s README/palette table — and use the *actual* published hex values below, not the placeholders. Do not skip this: the design explicitly asked for this to be verified against the real palette rather than guessed.

- [ ] **Step 2: Write `EditorTheme.h`**

```cpp
#pragma once

#include <imgui.h>

// Everforest-inspired dark palette (github.com/sainnhe/everforest).
// Keyword/StringLiteral/Comment are defined now for the syntax-
// highlighting module that comes later -- unused until then.
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
	t.Background          = IM_COL32(0x2d, 0x35, 0x3b, 255); // verify against the real palette -- placeholder
	t.Foreground          = IM_COL32(0xd3, 0xc6, 0xaa, 255); // verify against the real palette -- placeholder
	t.LineNumberFg        = IM_COL32(0x7a, 0x84, 0x78, 255); // verify against the real palette -- placeholder
	t.CurrentLineNumberFg = t.Foreground;
	t.CursorColor         = IM_COL32(0xd3, 0xc6, 0xaa, 180);
	t.Keyword             = IM_COL32(0xe6, 0x7e, 0x80, 255); // verify against the real palette -- placeholder
	t.StringLiteral       = IM_COL32(0xa7, 0xc0, 0x80, 255); // verify against the real palette -- placeholder
	t.Comment             = IM_COL32(0x85, 0x92, 0x89, 255); // verify against the real palette -- placeholder
	return t;
}
```

Replace every value marked "placeholder" with the real, verified hex codes from Step 1 (keep the `IM_COL32(R, G, B, 255)` shape — just correct the actual R/G/B bytes). Remove the "placeholder" comments once corrected.

- [ ] **Step 3: Write the buffer-to-grid glue as a free function, and `TextEditorPanel`**

Create `TestEnv/src/TextEditorPanel.h`:

```cpp
#pragma once

// Glues TextBuffer + CellGrid into the editor's "Editor" panel.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "EditorTheme.h"
#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>
#include <string>

// Copies the buffer's visible window into the CellGrid: a right-aligned
// line-number gutter (current line highlighted), then the line text. A
// free function (not a TextEditorPanel method) so a self-test can
// exercise this glue directly, without a live ImGui frame -- same pattern
// TerminalPanel's CopyVTermScreenToGrid already established.
inline void RenderBufferToGrid(const TextBuffer& buffer, CellGrid& grid, const EditorTheme& theme,
	int scrollRow, int gutterDigits, int gutterCols, int textCols, int rows)
{
	for (int screenRow = 0; screenRow < rows; screenRow++)
	{
		int bufferRow = scrollRow + screenRow;
		bool isCurrentLine = (bufferRow == buffer.CursorRow());

		for (int col = 0; col < gutterCols; col++)
		{
			Cell& cell = grid.At(col, screenRow);
			cell.Bg = theme.Background;
			cell.Fg = isCurrentLine ? theme.CurrentLineNumberFg : theme.LineNumberFg;
			cell.Codepoint = U' ';
		}

		bool hasLine = bufferRow < buffer.LineCount();
		if (hasLine)
		{
			char numberText[16];
			snprintf(numberText, sizeof(numberText), "%*d", gutterDigits, bufferRow + 1);
			for (int i = 0; i < gutterDigits; i++)
				grid.At(i, screenRow).Codepoint = (char32_t)(unsigned char)numberText[i];
		}

		const std::string* line = hasLine ? &buffer.Line(bufferRow) : nullptr;
		for (int col = 0; col < textCols; col++)
		{
			Cell& cell = grid.At(gutterCols + col, screenRow);
			cell.Bg = theme.Background;
			cell.Fg = theme.Foreground;
			cell.Codepoint = (line && col < (int)line->size()) ? (char32_t)(unsigned char)(*line)[col] : U' ';
		}
	}

	int cursorScreenRow = buffer.CursorRow() - scrollRow;
	grid.SetCursor(gutterCols + buffer.CursorCol(), cursorScreenRow,
		cursorScreenRow >= 0 && cursorScreenRow < rows);
}

class TextEditorPanel
{
public:
	void OnImGuiRender()
	{
		bool visible = ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar);

		if (ImGui::IsWindowFocused())
			ImGui::GetIO().WantCaptureKeyboard = true;

		if (!visible)
		{
			ImGui::End();
			return;
		}

		DrawFileBar();

		ImVec2 avail = ImGui::GetContentRegionAvail();
		float cellWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();

		int gutterDigits = std::max(2, (int)std::to_string(m_Buffer.LineCount()).size());
		int gutterCols = gutterDigits + 1; // one column of spacing after the number

		int totalCols = std::max(gutterCols + 8, (int)(avail.x / cellWidth));
		int textCols = totalCols - gutterCols;
		int rows = std::max(2, (int)(avail.y / cellHeight));

		HandleInput(rows);
		ScrollToCursor(rows);

		m_Grid.Resize(totalCols, rows);
		RenderBufferToGrid(m_Buffer, m_Grid, m_Theme, m_ScrollRow, gutterDigits, gutterCols, textCols, rows);
		m_Grid.Render();

		ImGui::End();
	}

private:
	void DrawFileBar()
	{
		ImGui::PushItemWidth(300.0f);
		ImGui::InputText("##editorpath", m_PathBuffer, sizeof(m_PathBuffer));
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGui::Button("Open"))
			m_StatusMessage = m_Buffer.LoadFromFile(m_PathBuffer) ? "Opened." : "Could not open file.";
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			m_StatusMessage = m_Buffer.SaveToFile(m_PathBuffer) ? "Saved." : "Could not save file.";
		if (!m_StatusMessage.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_StatusMessage.c_str());
		}
	}

	// Skips buffer-editing keys entirely while the path field (or any other
	// text widget) is the one capturing keyboard text this frame -- both it
	// and this function would otherwise read the same io.InputQueueCharacters
	// this frame, double-handling every typed character.
	void HandleInput(int visibleRows)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput)
			return;

		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			ImWchar c = io.InputQueueCharacters[i];
			if (c >= 32 && c < 127) // printable ASCII only, matching CellGrid's own cut
				m_Buffer.InsertChar((char)c);
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Enter, true)) m_Buffer.InsertNewline();
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) m_Buffer.Backspace();
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) m_Buffer.Delete();
		if (ImGui::IsKeyPressed(ImGuiKey_Tab, true)) m_Buffer.InsertTab();
		if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) m_Buffer.MoveLeft();
		if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) m_Buffer.MoveRight();
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) m_Buffer.MoveUp();
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) m_Buffer.MoveDown();
		if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) m_Buffer.MoveHome();
		if (ImGui::IsKeyPressed(ImGuiKey_End, true)) m_Buffer.MoveEnd();
		if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) m_Buffer.MovePageUp(visibleRows);
		if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) m_Buffer.MovePageDown(visibleRows);
	}

	void ScrollToCursor(int visibleRows)
	{
		if (m_Buffer.CursorRow() < m_ScrollRow)
			m_ScrollRow = m_Buffer.CursorRow();
		else if (m_Buffer.CursorRow() >= m_ScrollRow + visibleRows)
			m_ScrollRow = m_Buffer.CursorRow() - visibleRows + 1;
		if (m_ScrollRow < 0)
			m_ScrollRow = 0;
	}

	TextBuffer m_Buffer;
	CellGrid m_Grid;
	EditorTheme m_Theme = EverforestDark();
	char m_PathBuffer[512] = "";
	std::string m_StatusMessage;
	int m_ScrollRow = 0;
};
```

- [ ] **Step 4: Write the deterministic glue self-test**

Create `TestEnv/src/TextEditorGlueTest.h`:

```cpp
// TEMPORARY -- delete after verifying TextEditorPanel's buffer-to-grid
// glue (Task 3 of the embedded-text-editor plan).
#pragma once
#include <GS.h>
#include "TextBuffer.h"
#include "TextEditorPanel.h"

namespace TextEditorGlueTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        TextBuffer buffer;
        buffer.InsertChar('H');
        buffer.InsertChar('i');
        buffer.InsertNewline();
        buffer.InsertChar('B');
        buffer.InsertChar('y');
        buffer.InsertChar('e');
        // buffer: ["Hi", "Bye"], cursor at (1,3)

        EditorTheme theme = EverforestDark();
        CellGrid grid;
        int gutterDigits = 2, gutterCols = 3, textCols = 10, rows = 4;
        grid.Resize(gutterCols + textCols, rows);

        RenderBufferToGrid(buffer, grid, theme, /*scrollRow*/0, gutterDigits, gutterCols, textCols, rows);

        Check(grid.At(0, 0).Codepoint == U' ' && grid.At(1, 0).Codepoint == U'1', "line 1's gutter shows ' 1'");
        Check(grid.At(gutterCols + 0, 0).Codepoint == U'H', "line 1 text starts with 'H'");
        Check(grid.At(gutterCols + 1, 0).Codepoint == U'i', "line 1 text continues 'i'");
        Check(grid.At(gutterCols + 2, 0).Codepoint == U' ', "line 1 past its text is blank");
        Check(grid.At(1, 1).Codepoint == U'2', "line 2's gutter shows ' 2'");
        Check(grid.At(gutterCols + 0, 1).Codepoint == U'B', "line 2 text starts with 'B'");
        Check(grid.At(1, 0).Fg == theme.LineNumberFg, "non-current line number uses the dim gutter color");
        Check(grid.At(1, 1).Fg == theme.CurrentLineNumberFg, "the cursor's line number is highlighted");
        Check(grid.At(gutterCols + 3, 2).Codepoint == U' ', "a row past the buffer's line count is blank");
    }
}
```

- [ ] **Step 5: Call it, build, and run**

Temporarily add `#include "TextEditorGlueTest.h"` and `TextEditorGlueTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 9 `[ok ]` lines, `g_Pass == 9`, `g_Fail == 0`.

- [ ] **Step 6: Revert the temporary insertion**

Remove the `TextEditorGlueTest.h` include/call from `TestApp.cpp`.

- [ ] **Step 7: Wire `TextEditorPanel` into `EditorShell.h`**

Add `#include "TextEditorPanel.h"` to `EditorShell.h`'s include block.

Add a member to the `EditorShell` class (`private:` section, alongside `bool m_Built = false;` and `TerminalPanel m_Terminal;`):

```cpp
	TextEditorPanel m_TextEditor;
```

Replace Task 1's stub:

```cpp
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar);
		ImGui::PopStyleVar();
		ImGui::TextDisabled("Not built yet -- text editor sub-project.");
		ImGui::End();
```

with:

```cpp
		m_TextEditor.OnImGuiRender();
```

(`TextEditorPanel::OnImGuiRender()` already does its own `Begin`/`End` and its own `WindowPadding` push/pop internally — matching how `TerminalPanel::OnImGuiRender()` is called directly by `EditorShell.h` with no wrapping.)

- [ ] **Step 8: Build all three configs and verify no existing demo is affected**

```bash
./gs.py build all
```

Expected: exit 0, zero "error" occurrences, Debug/Release/Dist.

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/et_final_hideui.png --capture-step 5
```

Compare against Task 1's Step 4 capture (or a pre-this-plan capture) — must be byte-identical. `TextEditorPanel` is never even constructed-into-active-use unless `EditorShell::OnImGuiRender` runs, which `--hide-ui` prevents.

- [ ] **Step 9: Visual check of the real Editor tab**

```bash
./gs.py run
```

With no `--demo`, click the "Editor" tab (next to "Scene" in the center area). Verify by hand: a themed (Everforest-dark) grid with a line-number gutter appears; typing inserts text; pressing Enter after an indented line keeps the indent; the path field + Open/Save buttons work against a real file path. This cannot be driven headlessly here (no GUI-automation tool) — report the actual result of clicking through it, not an assumption, same disclosed-limitation pattern as every other UI-interaction check in this editor pivot.

- [ ] **Step 10: Clean up every temporary test**

Delete `TestEnv/src/TextEditorGlueTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty, then:

```bash
git grep TEMPORARY
```

Expected: no output.

- [ ] **Step 11: Update `docs/STATE.md`**

Add a "Last landed" bullet ahead of the existing embedded-terminal entry, in the same style as every prior sub-project's landing entry (name the new files, the theme, and how it was verified — self-test counts and the capture/hand-check split, matching this plan's own actual verification). Update "What is next" to drop the embedded-editor item and renumber whatever follows it (per the current file, the scripting runtime), fixing any cross-reference wording the same way the terminal plan's own STATE.md update did.

- [ ] **Step 12: Stage**

```bash
git add TestEnv/src/EditorTheme.h TestEnv/src/TextEditorPanel.h TestEnv/src/EditorShell.h docs/STATE.md
git status --short
```

---

## Self-Review

**Spec coverage:** center-viewport tabbing with `g_Viewport` gated on the visible tab (Task 1) ✓. `TextBuffer` with indent-preserving newline, line-join backspace/delete, desired-column vertical movement, tab-as-spaces, load/save (Task 2) ✓. `EditorTheme` as a named, real-palette-verified concept with future syntax-highlighting fields defined but unused (Task 3) ✓. Inline file-path field, not a menu integration (Task 3) ✓. `WantCaptureKeyboard` set from the start, avoiding the terminal's own Critical bug class (Task 3) ✓. Deterministic buffer-to-grid self-test bypassing a live frame (Task 3) ✓. ASCII-only inherited from `CellGrid`, stated not hidden (Global Constraints, `TextBuffer.h`'s header comment) ✓. All explicitly-deferred features (syntax highlighting, tab completion, function lookup, find/replace, undo/redo, selection/clipboard, theme picker) named in the spec and not built here ✓.

**Placeholder scan:** no "TBD"/vague-instruction placeholders. `EditorTheme.h`'s hex values ARE marked "placeholder" deliberately, with an explicit, actionable instruction (Task 3 Step 1) to replace them with real, verified values before finishing that step — same shape as the terminal plan's Task 3 pixel-coordinate measurement (a concrete, actionable procedure, not a fabricated literal this plan can't respons­ibly know without checking).

**Type consistency:** `RenderBufferToGrid`'s parameter order (`buffer, grid, theme, scrollRow, gutterDigits, gutterCols, textCols, rows`) matches exactly between its definition (`TextEditorPanel.h`) and both call sites (`TextEditorPanel::OnImGuiRender`, `TextEditorGlueTest.h`). `grid.At(col, row)` vs `TextBuffer`'s `{row, col}`-ordered cursor accessors are not crossed anywhere — double-checked, since this exact transposition class bit the terminal plan's own libvterm glue and is worth re-verifying every time two different (row,col) conventions meet. `TextBuffer`'s method names (`MoveLeft/Right/Up/Down`, `MoveHome/End`, `MovePageUp/Down`, `InsertChar/Newline/Tab`, `Backspace`, `Delete`, `LoadFromFile`, `SaveToFile`) are used identically in Task 2's self-test and Task 3's `TextEditorPanel::HandleInput`.
