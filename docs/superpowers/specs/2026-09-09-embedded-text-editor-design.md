# Embedded text editor — brief spec

Follows the embedded terminal. Reuses `CellGrid.h` for rendering, as
planned. Kept short per the owner's request — build it, review happens
later.

## Goal

A basic text editor living in a new "Editor" tab in the center viewport,
tabbed alongside the existing "Scene" view. v1: open a file, edit it with
line numbers and auto-indent, save it. No vim/Neovim — a small
self-contained editor (own text buffer, own input handling), decided over
embedding Neovim specifically to avoid an external runtime dependency and
keep it as self-contained/testable as the rest of this codebase.

**Explicitly deferred** (named now so the seams are obvious later, not
built now): syntax highlighting, tab completion, function lookup/go-to-
definition, find & replace, undo/redo, selection/clipboard, editable
theme picker (v1 ships one hardcoded Everforest-like theme).

## Part 1 — center viewport tabbing

Today the center dockspace node has **no ImGui window in it at all** —
`EditorShell.h`'s `PassthruCentralNode` flag just means "nothing paints
where nothing is docked," and `g_Viewport` (`Demo.h`) is read straight off
`DockBuilderGetCentralNode()`'s raw rect every frame, independent of any
window. Making the viewport tabbable means:

- Dock a real `"Scene"` window into the center node (`NoBackground` +
  the same chrome-suppressing flags `##EditorShell` already uses),
  replacing the implicit passthru hole with an explicit transparent tab.
- Dock a real `"Editor"` window into the same node, alongside it.
- `g_Viewport` is set from `"Scene"`'s own content-region rect **only
  when its `Begin()` returns true** (i.e. it's the selected tab);
  otherwise `g_Viewport = ViewportRect()` (invalid), same pattern
  `EditorShell.h:83` already uses for `--no-editor`.
- `TextEditorPanel::OnImGuiRender()` only does real work when *its*
  `Begin()` returns visible — same `visible`-gating the terminal panel's
  final fix wave already established.
- Under `--hide-ui`, neither window's `Begin()` ever runs, so `g_Viewport`
  reverts to the full-window rect exactly as today. No capture/replay
  regression.

## Part 2 — the editor itself

- **`TestEnv/src/TextBuffer.h`** (new) — pure data, no ImGui: `std::vector
  <std::string>` lines (always ≥1), a `{row, col}` cursor (byte-indexed,
  ASCII-only — inherits `CellGrid`'s existing limit, same disclosed cut as
  the terminal), a remembered "desired column" for vertical moves through
  shorter lines. Ops: `InsertChar`, `InsertNewline` (splits the line and
  copies the current line's leading whitespace onto the new one --- the
  "indenting" ask), `Backspace`/`Delete` (join lines at a boundary),
  `InsertTab` (4 spaces, not a raw tab byte), cursor movement (arrows/
  Home/End/PageUp/PageDown), `LoadFromFile`/`SaveToFile`.
- **`TestEnv/src/EditorTheme.h`** (new) — a named struct (`Background`,
  `Foreground`, `LineNumberFg`, `CurrentLineNumberFg`, `CursorColor`, plus
  `Keyword`/`StringLiteral`/`Comment` defined now for the future
  syntax-highlighting module but unused in v1) with one factory,
  `EverforestDark()`. Real hex values pulled from Everforest's published
  palette at implementation time, not guessed.
- **`TestEnv/src/TextEditorPanel.h`** (new) — glue: owns a `TextBuffer` +
  `CellGrid` + `EditorTheme` + an inline file-path field (text input +
  Open/Save buttons at the top of the panel, same pattern as the Tools
  panel's Import field — no `EditorMenuBar.h` changes). Sets
  `io.WantCaptureKeyboard` the instant it's focused (the terminal's
  Critical bug, avoided from day one here). Reserves a left gutter sized
  to the line count for right-aligned line numbers (current line
  highlighted). Scrolls to keep the cursor visible.
- **`TestEnv/src/EditorShell.h`** (modified) — Part 1's tabbing, plus owns
  a `TextEditorPanel` instance the same way it owns the Terminal panel.
- **`docs/STATE.md`** (modified) — same convention as every prior
  sub-project's landing entry.

## Testing

Same self-test convention as the whole editor pivot: `TextBuffer`'s edit
operations are pure logic, testable directly (no ImGui/GL needed) —
insert/newline-with-indent/backspace/delete/cursor-movement each get a
temporary `Check()`-based self-test, deleted once verified. Rendering and
the viewport-tabbing mechanism need a live capture, same technique
`CellGrid`/`TerminalPanel` already used. Interactive typing is a by-hand
check, disclosed as such rather than faked, matching every other
UI-interaction gap already accepted in this pivot.
