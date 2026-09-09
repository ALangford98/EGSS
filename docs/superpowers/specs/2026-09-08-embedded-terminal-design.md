# Embedded terminal — design

Follows sub-project 1's menu bar/layout landing, which reserved a "Terminal"
tab in the bottom dock well as a stub. This sub-project fills it in: a real
PTY-backed shell rendered inside the editor. See `docs/STATE.md` for the full
roadmap sequence and reasoning.

## Goal

Turn the existing "Terminal" stub panel into a working, interactive shell —
run builds, `git`, `ls`, arbitrary commands — without leaving the editor.
Built as a generic grid-of-styled-cells widget rather than something
terminal-specific, because sub-project 2 (an embedded Neovim, driven by its
own msgpack-RPC UI protocol, not a shell) needs the exact same rendering
piece to draw the grid Neovim describes to it.

## Scope

**In scope:**
- A POSIX PTY wrapper spawning `$SHELL` (falling back to `/bin/sh`).
- Vendoring `libvterm` (the VT100/xterm state machine Neovim's own
  `:terminal` uses) to turn the shell's raw output into a grid of styled
  cells.
- A reusable `CellGrid` widget: a plain `Cell{codepoint, fg, bg, bold}`
  buffer plus an ImGui-rendered display of it. No PTY or libvterm
  dependency — sub-project 2 fills the same structure from its own data.
- Keyboard input forwarded to the shell; panel resize forwarded to the PTY
  (`SIGWINCH`) and to libvterm.
- Basic interactive use: color/style rendering, typing, resizing.

**Explicitly deferred:**
- **Scrollback buffer / scroll UI.** Not wired up this pass — libvterm's
  scrollback callbacks are left unregistered, so scrolled-off lines are
  simply dropped, same as a terminal with no scrollback history.
- **Alt-screen support.** Full-screen TUI apps (`vim`, `htop`, `tmux`) are
  not a target for this pass and may render oddly. Sub-project 2 (Neovim)
  does not need this either — it speaks its own RPC UI protocol, not a
  shell running inside this terminal.
- **Mouse reporting into the PTY.** Click/drag/scroll inside a running
  program (e.g. `less`, `htop`) is out of scope.
- **Copy/paste.**
- **Non-ASCII rendering.** ImGui's default compiled-in font (ProggyClean)
  is fixed-width, which is what makes a character grid straightforward to
  render without loading a new font — but it only covers ASCII. Non-ASCII
  shell output (emoji, Nerd Font prompt glyphs, box-drawing from some
  tools) renders as blank/tofu, not garbage. Acceptable for the target use
  (builds, git, `ls`, general shell commands); revisit if a real need for
  a wider font shows up.
- **Windows.** POSIX PTYs (`forkpty`/`openpty`) and Windows ConPTY are
  genuinely different code paths; this pass targets Linux only, matching
  the actual dev platform. A Windows backend is a separate addition later
  if the engine ever needs one there.

## Design

### File structure

- **`GS/vendor/libvterm`** (new git submodule) —
  `github.com/neovim/libvterm`, the fork Neovim's own `:terminal` uses.
  MIT licensed. Plain portable C, no build-system baggage.
- **`GS/vendor/libvterm_premake5.lua`** (new) — same convention as
  `imgui_premake5.lua`: libvterm ships no premake file, so this lives
  outside the submodule and compiles just the core sources (`vterm.c`,
  `state.c`, `screen.c`, `parser.c`, `encoding.c`, `keyboard.c`, `mouse.c`,
  `pen.c`, `unicode.c`) into a static lib — none of the GTK/pangoterm
  frontend bits, which this project doesn't use.
- **`TestEnv/src/Pty.h`** (new) — POSIX PTY wrapper. No terminal-emulation
  or rendering knowledge; just an OS-level byte pipe to a spawned shell.
- **`TestEnv/src/CellGrid.h`** (new) — the reusable widget: cell storage
  plus an ImDrawList-based renderer. No PTY or libvterm dependency.
- **`TestEnv/src/TerminalPanel.h`** (new) — glues the three together.
- **`TestEnv/src/EditorShell.h`** (modified) — its current
  `ImGui::Begin("Terminal"); ... "Not built yet" ...` stub is replaced by
  a `TerminalPanel` instance, owned and rendered the same place the stub
  was.

### `Pty.h`

```cpp
// Sketch, not final code.
class Pty
{
public:
    bool Open(int cols, int rows);      // forkpty + execvp($SHELL or /bin/sh)
    int Read(char* buf, int max);       // non-blocking; 0 = no data, -1 = child exited
    void Write(const char* data, int len);
    void Resize(int cols, int rows);    // ioctl(TIOCSWINSZ) -> SIGWINCH to the child
    bool IsAlive();                     // waitpid(WNOHANG)
    ~Pty();                             // close master fd, reap child (SIGKILL after a grace check if needed)
};
```

The master fd is opened non-blocking (`fcntl(..., O_NONBLOCK)`) so `Read()`
never stalls the frame loop — `Application::Run()` is single-threaded start
to finish, and a blocking read on a shell that isn't producing output would
freeze rendering. No new thread: a non-blocking poll once per frame is
simpler and introduces no new synchronization surface, and a shell's output
isn't latency-sensitive the way audio is.

### `CellGrid.h`

```cpp
// Sketch, not final code.
struct Cell { char32_t Codepoint = ' '; ImU32 Fg, Bg; bool Bold = false; };

class CellGrid
{
public:
    void Resize(int cols, int rows);
    Cell& At(int col, int row);
    void SetCursor(int col, int row, bool visible);
    void Render();   // ImDrawList: AddRectFilled per non-default background,
                      // AddText per cell, at fixed cell pitch from the
                      // current font's advance width -- not ImGui's
                      // word-wrapped text flow.
};
```

Deliberately dumb: nothing in here knows what a PTY or an escape sequence
is. Sub-project 2 (Neovim) fills the same `Cell` grid from its own RPC
`grid_line`/`grid_cursor_goto` events instead of from libvterm.

### `TerminalPanel.h` — data flow, once per frame

Runs inside `OnImGuiRender`, which (like every other editor panel) never
executes under `--hide-ui`/`--capture`/`--play`.

1. `Pty::Read()` — non-blocking; zero or more bytes, or "child exited."
2. Whatever bytes came back go into `vterm_input_write()` (libvterm's
   parser), which updates its internal `VTermScreen` state. No callbacks
   are registered for v1 — the whole screen is re-read fresh each frame
   instead of tracking damage incrementally (a terminal-sized screen is
   ~80x24 = ~2000 cells; copying all of it every frame is trivial, and
   avoids wiring up libvterm's damage-region callback machinery for no
   real benefit at this size).
3. `vterm_screen_get_cell()` for every `(row, col)` into the panel's
   `CellGrid`, plus `vterm_screen_get_cursor()` for cursor position.
4. `CellGrid::Render()`.
5. Keyboard/text input captured by ImGui while the panel is focused ->
   `Pty::Write()` (raw bytes for printable characters; a small translation
   table for Enter/Backspace/arrows/Ctrl-combinations into their VT escape
   sequences).
6. Panel resize (docked size changes) -> recompute cols/rows from pixel
   size / font cell size -> `vterm_set_size()` + `Pty::Resize()`.

### Lifecycle and error handling

- **Spawn timing:** the `Pty` opens lazily, on the terminal panel's first
  actual render — not at construction. Since the panel's `OnImGuiRender`
  never runs under `--hide-ui`/`--capture`/`--play`, and the panel only
  exists inside `EditorShell` (never inside any demo), the shell child
  process is never created at all during any existing demo's capture or
  replay run. This is a stronger guarantee than "output is hidden" — the
  subprocess simply never exists in that mode.
- **Shell exits** (user types `exit`, or it crashes): detected via
  `waitpid`/EOF on the master fd. Panel shows "shell exited (code N)" and
  respawns on the next keypress, rather than staying dead or auto-
  respawning in a loop.
- **Spawn failure** (`forkpty` fails; `$SHELL` unset and `/bin/sh`
  missing): shown as a `TextDisabled` message in the panel, matching the
  existing "Not built yet" stub style. No crash, no per-frame retry storm
  — retried only on explicit user action.
- **App shutdown:** `Pty`'s destructor closes the master fd and reaps the
  child. Worth stating explicitly: `Pty` holds no GL resources at all —
  it's OS process/fd state only — so none of the "GL object destructor
  runs after context teardown" bug class (already fixed three times
  across the previous two plans) applies here.

### Testing

- libvterm itself is vendored/trusted, like ImGui or GLFW — not something
  this project tests.
- What *is* this project's own code — `Pty` (thin OS wrapper) and the
  libvterm-screen-state -> `CellGrid` copy, including color/attribute
  translation — gets a temporary self-test that feeds a known byte
  sequence straight into `vterm_input_write()`, bypassing any real shell
  entirely (e.g. `"Hello\x1b[31mRED\x1b[0m\n"`), then asserts the
  resulting `CellGrid` cells and cursor position match a hand-computed
  expectation. Deterministic, no subprocess involved.
- **Known limitation, stated plainly rather than glossed over:** a live
  PTY session's actual output is not frame-reproducible the way this
  project's other captures are — it is real OS scheduling of a real
  shell process, not `--lockstep` fixed-step simulation — so there is no
  bit-identical capture regression test for the interactive terminal
  itself, only for the parsing/rendering glue above (previous bullet).
- The actual interactive experience (typing, resizing the panel, running
  a real command) needs a human clicking through it once, the same
  limitation the project-dialog work hit last sub-project.

## What this sub-project does not decide

The Neovim RPC client (sub-project 2) and the scripting runtime (sub-project
3) are separate brainstorms with their own open questions. This design's
only commitment to either is `CellGrid`'s shape, which sub-project 2 will
fill from its own data rather than reuse any PTY/libvterm code from this
sub-project.
