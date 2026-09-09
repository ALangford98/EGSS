# Embedded Terminal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the editor's existing "Terminal" stub panel into a real, interactive, PTY-backed shell rendered inside ImGui.

**Architecture:** Three independent pieces. `Pty` (POSIX PTY wrapper, no terminal-emulation knowledge) spawns `$SHELL` non-blocking. `libvterm` (vendored) turns its raw byte stream into VT100/xterm state (a screen of styled cells, cursor position). `CellGrid` (a generic, PTY/libvterm-agnostic `Cell{codepoint,fg,bg,bold}` buffer plus an ImDrawList renderer) is the only piece the future embedded-Neovim sub-project reuses. `TerminalPanel` glues the three together and replaces the stub in `EditorShell.h`.

**Tech Stack:** C++17, ImGui (already vendored, docking branch), libvterm (new vendored submodule, MIT), POSIX PTY APIs (`forkpty`, `<pty.h>`).

**Spec:** `docs/superpowers/specs/2026-09-08-embedded-terminal-design.md`

## Global Constraints

- **No git commits.** This repo's `CLAUDE.md` forbids commits to `main` and forbids pushing. `git add -A` (or `git add <files>`) after each task's review passes is the task-boundary marker, standing in for the "Commit" step this skill's template otherwise shows — same adaptation used by the two prior plans in this repo (`.superpowers/sdd/2026-09-07-editor-scene-composition/`, `.superpowers/sdd/2026-09-08-editor-menu-layout/`).
- **Temporary self-test per task**, deleted once it has done its job — this repo has no test framework by design (`CLAUDE.md`, "The self-test pattern"). `git grep TEMPORARY` must be clean before Task 4 (the last task) finishes.
- **`./gs.py build all` (Debug/Release/Dist) must succeed with zero errors before Task 4 finishes.**
- **No existing demo's behavior, capture, or replay may change.** Every file this plan touches or creates is editor-only (`TestEnv/src/*.h`, none of which any demo includes) or a new vendored library nothing links against except `TestEnv`. `TerminalPanel` additionally never spawns its PTY child process at all except on its own first real render — and its panel exists only inside `EditorShell` (never inside any demo, and `EditorShell.h`'s own `OnImGuiRender` — like every other panel in it — never runs under `--hide-ui`/`--capture`/`--play`). This is a *stronger* guarantee than "output is hidden": the shell subprocess provably never exists during any existing demo's headless run.
- **`GS::`/`GS_*` namespace only for engine-side code.** `Pty`, `CellGrid`, `TerminalPanel` are TestEnv/editor-side code, matching `EditorMenuBar.h`/`EditorHistory.h`/etc., which are likewise not under `GS::`.
- **Linux only for this pass.** POSIX PTYs (`forkpty`), not Windows ConPTY. Don't add a `system:windows` code path for `Pty.h` — the premake `filter "system:windows"` blocks in this plan are for the libvterm static-lib build only (matching how `imgui_premake5.lua` already has to handle both platforms for the libraries it ships), not for a Windows `Pty` implementation, which does not exist yet.

---

### Task 1: Vendor libvterm

**Files:**
- Create (git submodule): `GS/vendor/libvterm` — `https://github.com/neovim/libvterm.git`
- Create: `GS/vendor/libvterm_premake5.lua`
- Modify: `premake5.lua:44-54` (add `IncludeDir["libvterm"]` and the new `include`)
- Modify: `premake5.lua:232-244` (TestEnv `includedirs`/`links`)
- Modify: `premake5.lua:281-285` (Linux-only `links`, add `"util"` for `forkpty`)
- Create (temporary): `TestEnv/src/LibvtermSmokeTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `#include <vterm.h>` becomes available to any file in `TestEnv/src` (the libvterm static lib, linked into `TestEnv`). Task 4 is the only later task that uses it.

- [ ] **Step 1: Add libvterm as a git submodule**

```bash
git submodule add https://github.com/neovim/libvterm.git GS/vendor/libvterm
```

This is the fork Neovim's own `:terminal` uses (MIT licensed — confirm by reading `GS/vendor/libvterm/LICENSE` after cloning, matching the convention every other vendored dependency in this repo already follows of vendoring via submodule rather than a copied snapshot). `git submodule add` stages `.gitmodules` and the new submodule gitlink automatically — do not run `git commit` (see Global Constraints).

- [ ] **Step 2: Write the libvterm premake file**

libvterm ships no premake file (only a Makefile) — same situation ImGui was in. This file lives outside the submodule (`GS/vendor/libvterm_premake5.lua`, a sibling of `GS/vendor/imgui_premake5.lua`) so it survives a re-clone and never leaves the submodule dirty. Compiles only the core VT100/xterm state-machine sources — none of libvterm's own `bin/` frontend tools, which this engine doesn't use.

```lua
-- libvterm ships no premake file (just a Makefile), so GS supplies one, the
-- same way it does for ImGui (see imgui_premake5.lua). Lives outside the
-- submodule so it survives a re-clone and never leaves the submodule dirty.
-- Compiles only the core VT100/xterm state-machine sources -- none of
-- libvterm's own bin/ frontend tools, which this engine doesn't use.
project "libvterm"
	kind "StaticLib"
	language "C"
	cdialect "C99"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"libvterm/include/vterm.h",
		"libvterm/include/vterm_keycodes.h",
		"libvterm/src/vterm_internal.h",
		"libvterm/src/encoding.c",
		"libvterm/src/keyboard.c",
		"libvterm/src/mouse.c",
		"libvterm/src/parser.c",
		"libvterm/src/pen.c",
		"libvterm/src/screen.c",
		"libvterm/src/state.c",
		"libvterm/src/unicode.c",
		"libvterm/src/vterm.c",
	}

	-- "src" is needed too, not just "include": encoding.c and unicode.c
	-- #include their own sibling headers/tables ("utf8.h", "encoding/*.inc",
	-- "fullwidth.inc") with quoted, relative-to-src includes.
	includedirs
	{
		"libvterm/include",
		"libvterm/src",
	}

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"
```

- [ ] **Step 3: Wire it into the root premake file**

In `premake5.lua`, add to the `IncludeDir` table (after the existing `IncludeDir["miniaudio"]` line, around line 50):

```lua
IncludeDir["libvterm"] = "GS/vendor/libvterm/include"
```

Add to the `include` block right after it (after `include "GS/vendor/imgui_premake5.lua"`, around line 54):

```lua
include "GS/vendor/libvterm_premake5.lua"
```

In the `TestEnv` project's `includedirs` block (around line 232-238), add `"%{IncludeDir.libvterm}"` alongside the existing `"%{IncludeDir.ImGui}"` entry:

```lua
    includedirs
    {
        "GS/vendor/spdlog/include",
        "GS/src",
        "%{IncludeDir.glm}",
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.libvterm}"
    }
```

In the same project's `links` block (around line 240-244), add `"libvterm"`:

```lua
    links 
    {
        "GS",
        "ImGui",
        "libvterm"
    }
```

In the `filter "system:linux"` block further down (around line 281-285), add `"util"` to the existing `links` list — `forkpty`/`openpty` (Task 2) live in `libutil` on Linux:

```lua
        links
        {
            "pthread",
            "dl",
            "util"
        }
```

- [ ] **Step 4: Write a temporary smoke test confirming libvterm actually builds and links**

Create `TestEnv/src/LibvtermSmokeTest.h`:

```cpp
// TEMPORARY -- delete after verifying libvterm builds, links, and its
// basic API round-trips (Task 1 of the embedded-terminal plan).
#pragma once
#include <GS.h>
#include <vterm.h>
#include <cstring>

namespace LibvtermSmokeTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        VTerm* vt = vterm_new(5, 20);
        Check(vt != nullptr, "vterm_new returns a non-null VTerm");

        vterm_set_utf8(vt, 1);
        VTermScreen* screen = vterm_obtain_screen(vt);
        Check(screen != nullptr, "vterm_obtain_screen returns a non-null VTermScreen");
        vterm_screen_reset(screen, 1);

        const char* text = "Hi";
        vterm_input_write(vt, text, strlen(text));

        VTermScreenCell cell;
        VTermPos pos{ 0, 0 };
        vterm_screen_get_cell(screen, pos, &cell);
        Check(cell.chars[0] == (uint32_t)'H', "libvterm parsed 'H' into cell (0,0)");

        vterm_free(vt);
    }
}
```

- [ ] **Step 5: Call it once, build, and run**

In `TestEnv/src/TestApp.cpp`, temporarily add `#include "LibvtermSmokeTest.h"` to the include block and `LibvtermSmokeTest::Run();` as the first line of the `TestEnv` constructor body.

```bash
./gs.py build
./gs.py run
```

Expected in the log: three `[ok ]` lines from `LibvtermSmokeTest`, `g_Pass == 3`, `g_Fail == 0`. This confirms the submodule cloned correctly, the premake wiring compiles and links the static lib, and libvterm's parser genuinely runs.

- [ ] **Step 6: Revert the temporary insertion and delete the test file**

Remove the `#include "LibvtermSmokeTest.h"` line and the `LibvtermSmokeTest::Run();` call from `TestApp.cpp` (net zero diff on that file for this task), and delete `TestEnv/src/LibvtermSmokeTest.h`. Confirm with `git diff -- TestEnv/src/TestApp.cpp` (empty) and `git status --short` (no `LibvtermSmokeTest.h`).

- [ ] **Step 7: Stage**

```bash
git add GS/vendor/libvterm GS/vendor/libvterm_premake5.lua premake5.lua .gitmodules
git status --short
```

Confirm the submodule, the new premake file, the root premake edits, and `.gitmodules` are staged, and nothing else changed.

---

### Task 2: `Pty.h` — POSIX PTY wrapper

**Files:**
- Create: `TestEnv/src/Pty.h`
- Create (temporary): `TestEnv/src/PtyTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `class Pty` with `bool Open(int cols, int rows, const char* command = nullptr)`, `int Read(char* buf, int max)`, `void Write(const char* data, int len)`, `void Resize(int cols, int rows)`, `bool IsAlive()`. No default copy/move (holds an OS fd and a child pid). Task 4 consumes this exactly.

Does not depend on Task 1 or Task 3 — can be built and reviewed independently.

- [ ] **Step 1: Write `Pty.h`**

```cpp
#pragma once

// A POSIX pseudo-terminal wrapper. Knows nothing about terminal emulation
// or rendering -- just an OS-level byte pipe to a spawned shell, non-
// blocking so it never stalls Application::Run()'s single-threaded frame
// loop (a blocking read() on a shell producing no output would freeze
// rendering entirely).

#include <GS.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

class Pty
{
public:
	Pty() = default;
	Pty(const Pty&) = delete;
	Pty& operator=(const Pty&) = delete;

	~Pty()
	{
		if (m_MasterFd >= 0)
		{
			close(m_MasterFd);
			m_MasterFd = -1;
		}

		if (m_ChildPid > 0)
		{
			kill(m_ChildPid, SIGHUP);

			int status = 0;
			bool reaped = false;
			for (int i = 0; i < 20 && !reaped; i++) // ~200ms grace, 10ms per check
			{
				if (waitpid(m_ChildPid, &status, WNOHANG) != 0)
					reaped = true;
				else
					usleep(10000);
			}
			if (!reaped)
			{
				kill(m_ChildPid, SIGKILL);
				waitpid(m_ChildPid, &status, 0);
			}
		}
	}

	// `command`, when non-null, runs via `/bin/sh -c command` instead of the
	// interactive `$SHELL` -- exists for this task's own deterministic self-
	// test (a real interactive shell's prompt/output depends on the user's
	// rc files, which a test can't predict). TerminalPanel (Task 4) always
	// calls this with no `command`, for a real interactive session.
	bool Open(int cols, int rows, const char* command = nullptr)
	{
		struct winsize ws{};
		ws.ws_col = (unsigned short)cols;
		ws.ws_row = (unsigned short)rows;

		m_ChildPid = forkpty(&m_MasterFd, nullptr, nullptr, &ws);
		if (m_ChildPid < 0)
		{
			GS_ERROR("Pty::Open: forkpty failed: {0}", strerror(errno));
			return false;
		}

		if (m_ChildPid == 0)
		{
			// Child: replace this process image. Never returns on success.
			if (command)
			{
				execl("/bin/sh", "/bin/sh", "-c", command, (char*)nullptr);
			}
			else
			{
				const char* shell = getenv("SHELL");
				if (!shell || shell[0] == '\0')
					shell = "/bin/sh";
				execl(shell, shell, (char*)nullptr);
			}
			_exit(127); // execl only returns on failure
		}

		// Parent.
		int flags = fcntl(m_MasterFd, F_GETFL, 0);
		fcntl(m_MasterFd, F_SETFL, flags | O_NONBLOCK);
		return true;
	}

	// Non-blocking. Returns bytes read (>= 0, possibly 0 if nothing is
	// available this frame), or -1 once the child has exited and there is
	// nothing left to read.
	int Read(char* buf, int max)
	{
		if (m_MasterFd < 0)
			return -1;

		ssize_t n = read(m_MasterFd, buf, (size_t)max);
		if (n > 0)
			return (int)n;
		if (n == 0)
			return -1; // EOF: the child closed its end.
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0; // Nothing available this frame.
		return -1; // A real error (commonly EIO, once the child has exited).
	}

	// Best-effort: a full kernel pipe drops input rather than blocking the
	// frame loop. A human typing faster than a shell can drain its input
	// buffer is not a scenario that happens at interactive typing speed.
	void Write(const char* data, int len)
	{
		if (m_MasterFd < 0)
			return;
		(void)write(m_MasterFd, data, (size_t)len);
	}

	void Resize(int cols, int rows)
	{
		if (m_MasterFd < 0)
			return;
		struct winsize ws{};
		ws.ws_col = (unsigned short)cols;
		ws.ws_row = (unsigned short)rows;
		ioctl(m_MasterFd, TIOCSWINSZ, &ws); // delivers SIGWINCH to the child
	}

	bool IsAlive()
	{
		if (m_ChildPid <= 0)
			return false;

		int status = 0;
		pid_t result = waitpid(m_ChildPid, &status, WNOHANG);
		if (result == 0)
			return true;

		m_ChildPid = -1; // reaped -- don't waitpid an already-reaped pid again
		return false;
	}

private:
	int m_MasterFd = -1;
	pid_t m_ChildPid = -1;
};
```

- [ ] **Step 2: Write the temporary self-test**

Create `TestEnv/src/PtyTest.h`:

```cpp
// TEMPORARY -- delete after verifying Pty (Task 2 of the embedded-terminal
// plan).
#pragma once
#include <GS.h>
#include "Pty.h"
#include <unistd.h>

namespace PtyTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        Pty pty;
        Check(pty.Open(80, 24, "printf hello"), "Pty::Open with a one-shot command succeeds");

        // Poll the way a real frame loop would (non-blocking Read() each
        // pass), bounded so a stuck test fails fast instead of hanging.
        std::string collected;
        char buf[256];
        for (int i = 0; i < 200 && collected.find("hello") == std::string::npos; i++)
        {
            int n = pty.Read(buf, sizeof(buf));
            if (n > 0)
                collected.append(buf, (size_t)n);
            else if (n < 0)
                break; // child exited
            else
                usleep(5000);
        }
        Check(collected.find("hello") != std::string::npos,
            "Pty::Read eventually returns the child's output (got: '" + collected + "')");

        for (int i = 0; i < 200 && pty.IsAlive(); i++)
            usleep(5000);
        Check(!pty.IsAlive(), "Pty::IsAlive() becomes false once the one-shot command exits");
    }
}
```

- [ ] **Step 3: Call it once, build, and run**

Temporarily add `#include "PtyTest.h"` and `PtyTest::Run();` to the `TestEnv` constructor in `TestApp.cpp` (same pattern as Task 1's Step 5).

```bash
./gs.py build
./gs.py run
```

Expected: three `[ok ]` lines from `PtyTest`, `g_Pass == 3`, `g_Fail == 0`.

If `Read` never sees "hello": check `/bin/sh` actually exists at that path (it does on every Linux system this targets) and that `forkpty` succeeded (`Pty::Open` would have logged a `GS_ERROR` and returned false — check the log for that first).

- [ ] **Step 4: Revert the temporary insertion and delete the test file**

Remove the `PtyTest.h` include and call from `TestApp.cpp`; delete `TestEnv/src/PtyTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty.

- [ ] **Step 5: Stage**

```bash
git add TestEnv/src/Pty.h
git status --short
```

---

### Task 3: `CellGrid.h` — the reusable cell-grid widget

**Files:**
- Create: `TestEnv/src/CellGrid.h`
- Create (temporary): `TestEnv/src/CellGridTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/Cube3D.h` (a few lines inside its `OnDemoImGuiRender`, or equivalent existing ImGui-render hook — grep the file for `OnDemoImGuiRender` to find it)
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `struct Cell { char32_t Codepoint; ImU32 Fg; ImU32 Bg; bool Bold; }` and `class CellGrid` with `void Resize(int cols, int rows)`, `int Cols() const`, `int Rows() const`, `Cell& At(int col, int row)`, `void SetCursor(int col, int row, bool visible)`, `void Render()`. Task 4 consumes all of this exactly.

Does not depend on Task 1 or Task 2 — no PTY, no libvterm. Can be built and reviewed independently.

- [ ] **Step 1: Write `CellGrid.h`**

```cpp
#pragma once

// A generic grid of styled character cells, plus an ImGui renderer for it.
// Deliberately knows nothing about PTYs, shells, or libvterm -- the
// embedded-Neovim sub-project that follows this one fills the same `Cell`
// grid from its own msgpack-RPC UI events instead of from a terminal.
//
// v1 renders ASCII only (codepoints >= 128 are left blank, not garbage --
// ImGui's default compiled-in font only covers ASCII, which is also what
// keeps a fixed-pitch character grid simple to compute: every glyph this
// pass draws is exactly one cell wide).

#include <GS.h>
#include <imgui.h>
#include <vector>

struct Cell
{
	char32_t Codepoint = U' ';
	ImU32 Fg = IM_COL32(220, 220, 220, 255);
	ImU32 Bg = IM_COL32(0, 0, 0, 0); // alpha 0 == "paint no background"
	bool Bold = false;
};

class CellGrid
{
public:
	void Resize(int cols, int rows)
	{
		if (cols == m_Cols && rows == m_Rows)
			return;
		m_Cols = cols;
		m_Rows = rows;
		m_Cells.assign((size_t)cols * (size_t)rows, Cell{});
	}

	int Cols() const { return m_Cols; }
	int Rows() const { return m_Rows; }

	Cell& At(int col, int row)
	{
		GS_ASSERT(col >= 0 && col < m_Cols && row >= 0 && row < m_Rows,
			"CellGrid::At out of range");
		return m_Cells[(size_t)row * (size_t)m_Cols + (size_t)col];
	}

	void SetCursor(int col, int row, bool visible)
	{
		m_CursorCol = col;
		m_CursorRow = row;
		m_CursorVisible = visible;
	}

	// Draws at the current ImGui cursor position, using the current font's
	// own advance width as the cell pitch -- a fixed grid, not ImGui's
	// word-wrapped text flow.
	void Render()
	{
		if (m_Cols == 0 || m_Rows == 0)
			return;

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 origin = ImGui::GetCursorScreenPos();
		ImFont* font = ImGui::GetFont();
		float fontSize = ImGui::GetFontSize();
		float cellWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();

		for (int row = 0; row < m_Rows; row++)
		{
			for (int col = 0; col < m_Cols; col++)
			{
				const Cell& cell = m_Cells[(size_t)row * (size_t)m_Cols + (size_t)col];
				ImVec2 cellMin(origin.x + col * cellWidth, origin.y + row * cellHeight);
				ImVec2 cellMax(cellMin.x + cellWidth, cellMin.y + cellHeight);

				if ((cell.Bg & IM_COL32_A_MASK) != 0)
					drawList->AddRectFilled(cellMin, cellMax, cell.Bg);

				if (cell.Codepoint > U' ' && cell.Codepoint < 128)
				{
					char ch = (char)cell.Codepoint;
					ImU32 fg = cell.Bold ? Brighten(cell.Fg) : cell.Fg;
					drawList->AddText(font, fontSize, cellMin, fg, &ch, &ch + 1);
				}
			}
		}

		if (m_CursorVisible && m_CursorCol >= 0 && m_CursorCol < m_Cols &&
			m_CursorRow >= 0 && m_CursorRow < m_Rows)
		{
			ImVec2 cursorMin(origin.x + m_CursorCol * cellWidth, origin.y + m_CursorRow * cellHeight);
			ImVec2 cursorMax(cursorMin.x + cellWidth, cursorMin.y + cellHeight);
			drawList->AddRectFilled(cursorMin, cursorMax, IM_COL32(200, 200, 200, 120));
		}

		// Reserve the layout space ImGui itself doesn't know about, since
		// everything above was drawn directly with the draw list.
		ImGui::Dummy(ImVec2(m_Cols * cellWidth, m_Rows * cellHeight));
	}

private:
	static ImU32 Brighten(ImU32 color)
	{
		auto lift = [](ImU32 channel) { return channel + (255 - channel) * 2 / 5; };
		ImU32 r = lift((color >> IM_COL32_R_SHIFT) & 0xFF);
		ImU32 g = lift((color >> IM_COL32_G_SHIFT) & 0xFF);
		ImU32 b = lift((color >> IM_COL32_B_SHIFT) & 0xFF);
		ImU32 a = (color >> IM_COL32_A_SHIFT) & 0xFF;
		return IM_COL32(r, g, b, a);
	}

	std::vector<Cell> m_Cells;
	int m_Cols = 0;
	int m_Rows = 0;
	int m_CursorCol = -1;
	int m_CursorRow = -1;
	bool m_CursorVisible = false;
};
```

- [ ] **Step 2: Write the temporary data-structure self-test (no GL context needed)**

Create `TestEnv/src/CellGridTest.h`:

```cpp
// TEMPORARY -- delete after verifying CellGrid (Task 3 of the embedded-
// terminal plan). Covers the pure data-structure logic; the rendering path
// is checked separately, in a live ImGui frame, by this task's Step 3-5.
#pragma once
#include <GS.h>
#include "CellGrid.h"

namespace CellGridTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        CellGrid grid;
        grid.Resize(10, 5);
        Check(grid.Cols() == 10 && grid.Rows() == 5, "Resize sets Cols()/Rows()");

        grid.At(3, 2).Codepoint = U'X';
        Check(grid.At(3, 2).Codepoint == U'X', "At() returns a mutable reference that persists");
        Check(grid.At(0, 0).Codepoint == U' ', "an untouched cell defaults to a space");

        // Resize to the same size must not clear existing content -- a
        // terminal panel calls Resize() every frame with the same cols/rows
        // whenever the ImGui panel size hasn't changed, and clearing on
        // every one of those frames would erase the screen constantly.
        grid.Resize(10, 5);
        Check(grid.At(3, 2).Codepoint == U'X', "Resize to an unchanged size does not clear existing cells");

        // Resizing to a genuinely different size does reset -- there is no
        // sensible way to preserve old content across a different grid
        // shape at this layer.
        grid.Resize(20, 10);
        Check(grid.At(3, 2).Codepoint == U' ', "Resize to a different size resets cell content");

        grid.SetCursor(4, 1, true);
        // No public getter for cursor state -- Render() is what consumes it,
        // checked visually in this task's capture-based step instead.
        Check(true, "SetCursor accepted without asserting (no public getter by design)");
    }
}
```

- [ ] **Step 3: Call the data-structure test, build, and run**

Temporarily add `#include "CellGridTest.h"` and `CellGridTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: five `[ok ]` lines, `g_Pass == 5`, `g_Fail == 0`.

- [ ] **Step 4: Verify `Render()` actually draws, with a live capture**

`Render()` needs a real ImGui frame (a live GL context), so — per this repo's established convention ("GL-dependent checks need a live context, so they run inside a demo's update rather than at construction") — this step temporarily hooks a tiny fixture into an existing demo's own ImGui-render method rather than testing at construction time.

Open `TestEnv/src/Cube3D.h` and find its `OnDemoImGuiRender()` (or whichever method draws that demo's own ImGui panel — `grep -n "OnDemoImGuiRender" TestEnv/src/Cube3D.h` to confirm the exact name and signature before editing). Temporarily add, as the first lines of that method's body:

```cpp
// TEMPORARY -- CellGrid render verification, Task 3 of the embedded-
// terminal plan. Removed once the capture below has been checked.
ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Always);
ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_Always);
ImGui::Begin("CellGridRenderTest");
static CellGrid s_TestGrid;
static bool s_TestGridInit = [] {
    s_TestGrid.Resize(10, 5);
    s_TestGrid.At(2, 1).Bg = IM_COL32(0, 255, 0, 255); // solid, unmistakable green
    return true;
}();
s_TestGrid.Render();
ImGui::End();
```

Add `#include "CellGrid.h"` to `Cube3D.h`'s include block if it isn't already indirectly available.

Run once with a visible window to find where the green cell actually lands on screen:

```bash
./gs.py run -- --demo Cube3D --show-window
```

The "CellGridRenderTest" window appears pinned at (50, 50). Note the screen-pixel coordinate of a point clearly inside the solid green cell (col 2, row 1) — e.g. by taking a screenshot and reading pixel coordinates in an image viewer, or by adding a one-line temporary `GS_TRACE` printing `ImGui::GetCursorScreenPos()` right before `s_TestGrid.Render()` and computing `origin + (2.5, 1.5) * cell size` from the logged value.

Once you have that coordinate (call it `(px, py)`), replace the temporary fixture's last line with an actual check, using `GS::Framebuffer::ReadPixelRGBA` (`GS/src/GS/Renderer/Framebuffer.h` — confirm the exact signature there before calling it) to read the live framebuffer at that point, immediately after `s_TestGrid.Render()`:

```cpp
s_TestGrid.Render();
ImGui::End();

// One-shot check, a few frames in so the window has settled at its pinned
// position -- replace 128,64 below with the (px, py) you measured above.
static int s_FrameCount = 0;
if (++s_FrameCount == 30)
{
    auto [r, g, b, a] = GS::Framebuffer::ReadPixelRGBA(128, 64);
    bool isGreen = (g > 200 && r < 50 && b < 50);
    GS_TRACE("  [{0}] CellGrid::Render() drew the specified background color at the expected pixel (got r={1} g={2} b={3})",
        isGreen ? "ok " : "FAIL", r, g, b);
}
```

(Adjust the destructuring/return shape to match whatever `ReadPixelRGBA` actually returns — read its declaration first rather than guessing the type.)

```bash
./gs.py build
./TestEnv --demo Cube3D --hide-window --lockstep --capture /tmp/cellgrid_check.png --capture-step 40
```

Expected in the log: one `[ok ]` line confirming the measured pixel is green. `--hide-window` still runs `OnImGuiRender` (only `--hide-ui` skips it), so the check fires; the `--capture` output image also visually confirms the panel and its green cell if you want to look at it directly.

- [ ] **Step 5: Revert every temporary insertion**

Remove the temporary block from `Cube3D.h`'s `OnDemoImGuiRender()` (and the `#include "CellGrid.h"` there if it was added solely for this test), remove the `CellGridTest.h` include/call from `TestApp.cpp`, and delete `TestEnv/src/CellGridTest.h`. Confirm `git diff -- TestEnv/src/Cube3D.h TestEnv/src/TestApp.cpp` is empty and `git status --short` shows no test files.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/src/CellGrid.h
git status --short
```

---

### Task 4: `TerminalPanel.h` — glue, and wiring into `EditorShell.h`

**Files:**
- Create: `TestEnv/src/TerminalPanel.h`
- Modify: `TestEnv/src/EditorShell.h` (replace the `"Terminal"` stub with a real `TerminalPanel`)
- Modify: `docs/STATE.md` ("Last landed" / "What is next")
- Create (temporary): `TestEnv/src/TerminalGlueTest.h`
- Modify (temporary, reverted before this task ends): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `Pty` (`Open`/`Read`/`Write`/`Resize`/`IsAlive`, Task 2), `CellGrid`/`Cell` (`Resize`/`At`/`Cols`/`Rows`/`SetCursor`/`Render`, Task 3), `#include <vterm.h>` (Task 1).
- Produces: `class TerminalPanel` with a single public `void OnImGuiRender()`; the free function `void CopyVTermScreenToGrid(VTerm*, VTermScreen*, CellGrid&, int cols, int rows)` (exposed specifically so this task's own self-test can exercise the libvterm-to-`CellGrid` glue without a real shell). Nothing later in this plan consumes either — this is the last task.

- [ ] **Step 1: Write `TerminalPanel.h`**

```cpp
#pragma once

// Glues Pty + libvterm + CellGrid into the editor's "Terminal" panel.
//
// Once per frame: read whatever the shell produced (non-blocking) and feed
// it to libvterm's parser; copy libvterm's *entire* current screen state
// into the CellGrid (a terminal is ~80x24 = ~2000 cells -- cheap enough to
// copy in full every frame, which avoids wiring up libvterm's damage-region
// callbacks for no real benefit at this size); render; forward keyboard
// input and panel-resize the other way.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "Pty.h"

#include <vterm.h>

#include <algorithm>
#include <cstdint>

// Copies libvterm's current screen state into a CellGrid. A free function
// (not a TerminalPanel method) so a self-test can exercise this glue by
// feeding vterm_input_write() directly, without spawning a real shell.
inline void CopyVTermScreenToGrid(VTerm* vt, VTermScreen* screen, CellGrid& grid, int cols, int rows)
{
	for (int row = 0; row < rows; row++)
	{
		for (int col = 0; col < cols; col++)
		{
			VTermScreenCell cell;
			VTermPos pos{ row, col };
			vterm_screen_get_cell(screen, pos, &cell);

			// Resolves indexed/default colors to concrete RGB via the
			// screen's own palette -- after this, .fg.rgb/.bg.rgb are
			// always valid regardless of what kind of color it started as.
			vterm_screen_convert_color_to_rgb(screen, &cell.fg);
			vterm_screen_convert_color_to_rgb(screen, &cell.bg);

			Cell& out = grid.At(col, row);
			out.Codepoint = cell.chars[0] == 0 ? U' ' : (char32_t)cell.chars[0];
			out.Fg = IM_COL32(cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue, 255);
			out.Bg = IM_COL32(cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue, 255);
			out.Bold = cell.attrs.bold != 0;
		}
	}

	VTermState* state = vterm_obtain_state(vt);
	VTermPos cursor;
	vterm_state_get_cursorpos(state, &cursor);
	grid.SetCursor(cursor.col, cursor.row, true);
}

class TerminalPanel
{
public:
	~TerminalPanel()
	{
		if (m_Vt)
			vterm_free(m_Vt); // frees the VTermScreen/VTermState it owns too
	}

	void OnImGuiRender()
	{
		ImGui::Begin("Terminal");

		ImVec2 avail = ImGui::GetContentRegionAvail();
		float cellWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();
		int cols = std::max(1, (int)(avail.x / cellWidth));
		int rows = std::max(1, (int)(avail.y / cellHeight));

		if (!m_Vt)
		{
			Spawn(cols, rows);
			if (!m_Vt)
			{
				ImGui::TextDisabled("Terminal unavailable: could not start a shell.");
				ImGui::End();
				return;
			}
		}
		else if (!m_Pty.IsAlive())
		{
			ImGui::TextDisabled("Shell exited. Press any key to restart.");
			ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsWindowFocused() &&
				(io.InputQueueCharacters.Size > 0 || ImGui::IsKeyPressed(ImGuiKey_Enter)))
			{
				vterm_free(m_Vt);
				m_Vt = nullptr;
				m_Screen = nullptr;
			}
			ImGui::End();
			return;
		}
		else if (cols != m_Cols || rows != m_Rows)
		{
			Resize(cols, rows);
		}

		Pump();
		HandleInput();
		m_Grid.Render();

		ImGui::End();
	}

private:
	void Spawn(int cols, int rows)
	{
		if (!m_Pty.Open(cols, rows))
			return;

		m_Vt = vterm_new(rows, cols);
		vterm_set_utf8(m_Vt, 1);
		m_Screen = vterm_obtain_screen(m_Vt);
		vterm_screen_reset(m_Screen, 1);
		vterm_output_set_callback(m_Vt, &TerminalPanel::OnVtermOutput, this);

		m_Cols = cols;
		m_Rows = rows;
		m_Grid.Resize(cols, rows);
	}

	void Resize(int cols, int rows)
	{
		m_Cols = cols;
		m_Rows = rows;
		vterm_set_size(m_Vt, rows, cols);
		m_Pty.Resize(cols, rows);
		m_Grid.Resize(cols, rows);
	}

	// libvterm invokes this synchronously, from inside vterm_keyboard_key()/
	// vterm_keyboard_unichar() (see HandleInput()), with the exact bytes a
	// real terminal would send a program for that key. We just forward them.
	static void OnVtermOutput(const char* s, size_t len, void* user)
	{
		static_cast<TerminalPanel*>(user)->m_Pty.Write(s, (int)len);
	}

	void Pump()
	{
		char buf[4096];
		int n = m_Pty.Read(buf, sizeof(buf));
		if (n > 0)
			vterm_input_write(m_Vt, buf, (size_t)n);

		CopyVTermScreenToGrid(m_Vt, m_Screen, m_Grid, m_Cols, m_Rows);
	}

	void HandleInput()
	{
		if (!ImGui::IsWindowFocused())
			return;

		ImGuiIO& io = ImGui::GetIO();

		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			ImWchar c = io.InputQueueCharacters[i];
			if (c > 0 && c < 0x10000)
				vterm_keyboard_unichar(m_Vt, (uint32_t)c, VTERM_MOD_NONE);
		}

		struct KeyMap { ImGuiKey Key; VTermKey VKey; };
		static const KeyMap keys[] = {
			{ ImGuiKey_Enter, VTERM_KEY_ENTER },
			{ ImGuiKey_KeypadEnter, VTERM_KEY_ENTER },
			{ ImGuiKey_Backspace, VTERM_KEY_BACKSPACE },
			{ ImGuiKey_Tab, VTERM_KEY_TAB },
			{ ImGuiKey_Escape, VTERM_KEY_ESCAPE },
			{ ImGuiKey_UpArrow, VTERM_KEY_UP },
			{ ImGuiKey_DownArrow, VTERM_KEY_DOWN },
			{ ImGuiKey_LeftArrow, VTERM_KEY_LEFT },
			{ ImGuiKey_RightArrow, VTERM_KEY_RIGHT },
			{ ImGuiKey_Insert, VTERM_KEY_INS },
			{ ImGuiKey_Delete, VTERM_KEY_DEL },
			{ ImGuiKey_Home, VTERM_KEY_HOME },
			{ ImGuiKey_End, VTERM_KEY_END },
			{ ImGuiKey_PageUp, VTERM_KEY_PAGEUP },
			{ ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN },
		};
		for (const KeyMap& k : keys)
			if (ImGui::IsKeyPressed(k.Key, true))
				vterm_keyboard_key(m_Vt, k.VKey, VTERM_MOD_NONE);

		if (io.KeyCtrl)
			for (int letter = 0; letter < 26; letter++)
				if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + letter), false))
					vterm_keyboard_unichar(m_Vt, (uint32_t)('a' + letter), VTERM_MOD_CTRL);
	}

	Pty m_Pty;
	VTerm* m_Vt = nullptr;
	VTermScreen* m_Screen = nullptr;
	CellGrid m_Grid;
	int m_Cols = 0;
	int m_Rows = 0;
};
```

- [ ] **Step 2: Write the deterministic glue self-test**

This is the important automated check per the spec's Testing section: it feeds a known byte sequence straight into `vterm_input_write()`, bypassing any real shell, to verify `CopyVTermScreenToGrid`'s parsing/color-translation glue.

Create `TestEnv/src/TerminalGlueTest.h`:

```cpp
// TEMPORARY -- delete after verifying TerminalPanel's libvterm-to-CellGrid
// glue (Task 4 of the embedded-terminal plan).
#pragma once
#include <GS.h>
#include "CellGrid.h"
#include "TerminalPanel.h"
#include <cstring>

namespace TerminalGlueTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        VTerm* vt = vterm_new(5, 20);
        vterm_set_utf8(vt, 1);
        VTermScreen* screen = vterm_obtain_screen(vt);
        vterm_screen_reset(screen, 1);

        // "Hi" in the default color, then "RED" in ANSI red (SGR 31), then
        // back to default -- all on row 0.
        const char* bytes = "Hi\x1b[31mRED\x1b[0m";
        vterm_input_write(vt, bytes, strlen(bytes));

        CellGrid grid;
        grid.Resize(20, 5);
        CopyVTermScreenToGrid(vt, screen, grid, 20, 5);

        Check(grid.At(0, 0).Codepoint == U'H', "cell (0,0) is 'H'");
        Check(grid.At(1, 0).Codepoint == U'i', "cell (1,0) is 'i'");
        Check(grid.At(2, 0).Codepoint == U'R', "cell (2,0) is 'R', the start of RED");
        Check(grid.At(4, 0).Codepoint == U'D', "cell (4,0) is 'D', the end of RED");
        Check(grid.At(5, 0).Codepoint == U' ', "cell (5,0), past the written text, is blank");

        // libvterm's built-in default palette maps ANSI red (index 1) to
        // RGB(224,0,0) -- read directly from GS/vendor/libvterm/src/pen.c's
        // `ansi_colors[]` table before trusting this number; if the
        // vendored version's table differs, use its actual value instead.
        ImU32 expectedRed = IM_COL32(224, 0, 0, 255);
        Check(grid.At(2, 0).Fg == expectedRed, "'R' in RED is rendered in ANSI red");
        Check(grid.At(0, 0).Fg != expectedRed, "'H', written before the color escape, is NOT red");

        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
        Check(cursor.row == 0 && cursor.col == 5, "cursor sits right after \"HiRED\" (row 0, col 5)");

        vterm_free(vt);
    }
}
```

- [ ] **Step 3: Call it, build, and run**

Temporarily add `#include "TerminalGlueTest.h"` and `TerminalGlueTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: eight `[ok ]` lines, `g_Pass == 8`, `g_Fail == 0`. If the color check fails, read `GS/vendor/libvterm/src/pen.c`'s `ansi_colors[]` table directly and use whatever RGB value it actually has for index 1 rather than assuming the plan's number is right for whatever commit the submodule happened to pin.

- [ ] **Step 4: Revert the temporary insertion (test file itself is deleted in Step 8, once wiring is also verified)**

Remove the `TerminalGlueTest.h` include/call from `TestApp.cpp`.

- [ ] **Step 5: Wire `TerminalPanel` into `EditorShell.h`**

In `TestEnv/src/EditorShell.h`, add `#include "TerminalPanel.h"` to the include block (alongside the existing `#include "EditorSceneView.h"`).

Add a member to the `EditorShell` class (in the `private:` section, alongside `bool m_Built = false;`):

```cpp
	TerminalPanel m_Terminal;
```

Replace the existing stub:

```cpp
		ImGui::Begin("Terminal");
		ImGui::TextDisabled("Not built yet -- terminal sub-project.");
		ImGui::End();
```

with:

```cpp
		m_Terminal.OnImGuiRender();
```

- [ ] **Step 6: Build all three configs and verify no existing demo is affected**

```bash
./gs.py build all
```

Expected: exit 0, zero occurrences of "error" in the log, for Debug, Release, and Dist.

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/cube3d_after.png --capture-step 5
```

Compare this capture's byte size (or better, its bytes directly) against a capture taken from `main` before this plan's changes, to confirm the addition of a compiled-in-but-unrendered `TerminalPanel` member has zero effect on any existing demo capture. They must match exactly — `TerminalPanel` is never even constructed-into-active-use (`Spawn()` never runs) unless `EditorShell::OnImGuiRender` itself runs, which `--hide-ui` prevents.

- [ ] **Step 7: Manual interactive verification (cannot be automated — say so plainly, don't skip it silently)**

```bash
./gs.py run
```

With no `--demo` (or `--no-editor` absent), the editor boots and the Terminal panel (bottom dock, per the menu-bar plan's layout) should show a real, live shell prompt. Verify by hand:
- Typing a command (e.g. `ls`) and pressing Enter runs it and shows output.
- Colored output (e.g. `ls --color` or `git status` in a repo with changes) shows actual colors, not all-default-color text.
- Resizing the panel (drag its dock border) changes the shell's reported terminal size — running `stty size` in it before and after a resize should print different numbers.
- Typing `exit` shows the "Shell exited... press any key to restart" message, and pressing a key brings up a fresh prompt.

This cannot be driven headlessly in this environment (no GUI-automation tool, and `--play` replay never reaches ImGui — confirmed for the previous plan's project dialogs). Report the actual result of clicking through it, not an assumption.

- [ ] **Step 8: Clean up every temporary test**

Delete `TestEnv/src/TerminalGlueTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty (no leftover temporary includes/calls from this task or any earlier one) and run:

```bash
git grep TEMPORARY
```

Expected: no output at all (every temporary test from Tasks 1-4 has been deleted).

- [ ] **Step 9: Update `docs/STATE.md`**

In the "Last landed" section (top of the list, newest first), add a new bullet ahead of the existing "A menu bar and conventional editor layout..." entry:

```markdown
- **An embedded terminal, sub-project 1's step after the menu bar.**
  `TerminalPanel.h` replaces the "Terminal" stub with a real PTY-backed
  shell: `Pty.h` wraps `forkpty()`, `libvterm` (vendored,
  `GS/vendor/libvterm`) turns its output into a styled cell grid, and
  `CellGrid.h` (a generic, PTY-agnostic grid-of-cells widget -- the
  embedded Neovim editor after this reuses it) renders it in ImGui.
  Verified with a deterministic self-test feeding a known escape sequence
  directly into libvterm (8/8 checks) and by hand: typing, colored output,
  panel-resize propagating to the shell's reported terminal size, and
  restart-on-exit all confirmed interactively.
```

In the "What is next" numbered list, remove item 1 ("An embedded terminal...", now done) and renumber the rest (Neovim becomes 1, scripting becomes 2), updating any cross-references to the old numbering in that section's surrounding prose.

- [ ] **Step 10: Stage**

```bash
git add TestEnv/src/TerminalPanel.h TestEnv/src/EditorShell.h docs/STATE.md
git status --short
```

Confirm exactly these three files are staged for this task (plus whatever Tasks 1-3 already staged), and no temporary test file or reverted-file diff remains.

---

## Self-Review

**Spec coverage:** PTY wrapper (Task 2) ✓. libvterm vendoring (Task 1) ✓. `CellGrid` as a reusable, PTY/libvterm-agnostic widget (Task 3) ✓. Frame-by-frame data flow — read, parse, copy-to-grid, render, input-forward, resize (Task 4's `OnImGuiRender`/`Pump`/`HandleInput`/`Resize`) ✓. Lazy spawn on first render, shell-exit handling, spawn-failure handling (Task 4's `OnImGuiRender` branching) ✓. `Pty` holding no GL resources, stated explicitly (Task 2's file header comment) ✓. Deterministic glue self-test bypassing a real shell (Task 4 Step 2) ✓. Manual interactive verification, disclosed as such rather than faked (Task 4 Step 7) ✓. Linux-only scope (Global Constraints, `Pty.h` has no `#ifdef` platform split) ✓. Non-ASCII rendering as a stated cut, not silently broken (Task 3's `CellGrid.h` header comment and `Render()`'s `< 128` guard) ✓.

**Placeholder scan:** no "TBD"/"TODO"/"handle appropriately" anywhere above. The one place a plan can't supply an exact literal value up front — Task 3 Step 4's screen-pixel coordinate for the capture check — is written as a concrete, actionable measurement procedure (run, observe, compute), not a vague placeholder; this matches how this repo's own self-tests are actually calibrated (run, read the log, iterate), not something this plan invented to dodge specificity.

**Type consistency:** `Pty::Open(int cols, int rows, const char* command = nullptr)` (Task 2) is called exactly that way from `TerminalPanel::Spawn` (Task 4, no `command` argument, i.e. real interactive shell) and from `PtyTest.h` (Task 2's own test, with a `command`). `CellGrid::At(int col, int row)` / `Resize(int cols, int rows)` / `SetCursor(int col, int row, bool visible)` (Task 3) are called with that exact column-then-row argument order everywhere they're used in Task 4 and in both tasks' self-tests — double-checked, since libvterm's own `VTermPos{row, col}` uses the *opposite* order internally, which is exactly the kind of transposition bug that would silently pass a build and fail only visually; `CopyVTermScreenToGrid` explicitly constructs `VTermPos pos{ row, col }` for libvterm calls and `grid.At(col, row)` for `CellGrid` calls, keeping the two conventions from leaking into each other. `vterm_new`/`vterm_set_size` (Task 4) are consistently called as `(rows, cols)`, matching libvterm's real signature (confirmed by reading the vendored header directly, not from memory).
