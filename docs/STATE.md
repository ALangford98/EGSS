# State

**This is the only doc in the repo that is allowed to go stale, and it is kept
short so that fixing it is cheap.** Everything else — `docs/ENGINE.md`, the
trap list, the changelog — describes things that stay true once written. This
file describes where the work is *this week*, which does not.

It exists because the alternative was worse. `docs/HANDOVER.md` used to open
with a "Current state" section naming the commit `main` sat at, and it drifted
22 commits and one abandoned workflow behind reality. A session that read it —
as `CLAUDE.md` told it to, at a cost of about 53k tokens — came away with a
*confidently wrong* answer to "what are we working on", while `git log`, which
costs about 200 tokens and cannot drift, sat unread. **The expensive source was
the least current one.** So the volatile part was cut down to this page.

## Read this first, and read it with `git log`

```sh
git log --oneline -15
git status --short
git branch --show-current
```

**Where those disagree with this file, they win.** The owner commits their own
work between sessions and sometimes while a reply is being written, so any git
state you did not just observe is stale — including the branch name below.

## Where the work is

**Branch: `main`.** The owner asked to stay on it directly for the docs work;
`density-in-double` merged in `cf266b8` and is finished with.

The live area is **`TestEnv/src/TerrainLab.h`** (~11k lines) and the headers
around it — `Critters.h`, `Vegetation.h`, `Rocks.h`, `Climate.h`. It is a
first-person terrain demo that grew a felling-and-building game: you cut trees,
buck them into logs, rive planks, and raise structures out of a grid-snapped
kit of boards, logs and stone.

Last landed, newest first:

- **A self-built text editor, replacing the embedded-Neovim plan.** The
  "Editor" tab (Task 1's stub) is now real: `TextBuffer.h` is a pure-logic
  line buffer (insert/newline-with-indent/backspace/delete/cursor movement,
  load/save), `EditorTheme.h` is an Everforest-dark palette verified against
  the colorscheme's own source (`autoload/everforest.vim`'s dark/medium-
  contrast block) rather than guessed, and `TextEditorPanel.h` (`EditorShell.h`
  now owns one alongside `m_Terminal`) glues both through the same `CellGrid`
  the terminal renders with -- a right-aligned line-number gutter, current-line
  highlight, and an inline path field with Open/Save buttons (no menu
  integration). `WantCaptureKeyboard` is set the instant the panel is
  focused, and `HandleInput` skips the buffer entirely while `io.WantTextInput`
  is true so the path field and the buffer never double-consume the same
  typed character -- confirmed with a standalone probe against the real
  vendored ImGui (mirroring the mouse-capture probe from Task 1 of this same
  plan): a real click-then-type sequence never leaks a character into the
  buffer, even typed the same frame the field is clicked. Verified with a
  deterministic buffer-to-grid self-test (9/9), a supplementary self-test for
  `InsertNewline`'s mid-line split with re-indentation that Task 2's own test
  never exercised (9/9), a byte-identical `--hide-ui` capture against the
  pre-task baseline (`TextEditorPanel` never constructs into active use under
  `--hide-ui`), and an in-engine capture with the "Editor" tab forced selected
  via `imgui.ini`'s own dock-tab ID (no live interaction needed) whose pixels
  were sampled directly and matched the theme's `Background`/`Foreground`
  bytes exactly. Live typing, indent-preserving Enter, and Open/Save against
  the mouse-and-keyboard-driven UI still need a by-hand pass with a mapped
  window -- same open item the terminal's own landing entry recorded, for the
  same reason (no safe way to inject keyboard/mouse input in this session).
- **An embedded terminal, sub-project 1's step after the menu bar.**
  `TerminalPanel.h` replaces the "Terminal" stub with a real PTY-backed
  shell: `Pty.h` wraps `forkpty()`, `libvterm` (vendored,
  `GS/vendor/libvterm`) turns its output into a styled cell grid, and
  `CellGrid.h` (a generic, PTY-agnostic grid-of-cells widget -- the
  embedded text editor after this reuses it) renders it in ImGui.
  Verified with a deterministic self-test feeding a known escape sequence
  directly into libvterm (8/8 checks), and with an in-engine headless
  capture showing a real `forkpty()`-spawned zsh printing its own themed,
  colored prompt into the panel. Typing, panel-resize propagating to the
  shell's reported terminal size, and restart-on-exit still need a by-hand
  pass with a mapped window -- this session ran against a live desktop with
  no safe way to inject keyboard input, so that check is still open.
- **A menu bar and conventional editor layout, sub-project 1's next step
  after scene composition.** `EditorMenuBar.h` adds File/Edit/View/Help;
  File covers project open/save/rename via a new `ProjectManifest`
  (`EditorProject.h`, `project.gsproj`); Edit drives a minimal undo/redo
  command stack (`EditorHistory.h`: `PlaceEntityCommand`/
  `DeleteEntityCommand`/`EditFieldCommand`, wired into the Place/Delete/Edit
  UI actions); View switches named layout presets (`editor_layouts.txt`).
  `EditorShell.h`'s docking is reshuffled to match: Outliner/Tools/Demos
  left, Profiler/Inspector right, Terminal/Build Output/Textures tabbed
  along the bottom (all three still stubs -- the terminal is next, below).
  Verified with a headless capture showing the panel placement, and a
  temporary self-test round-tripping a project manifest through
  create/open/rename/reopen (8/8 passed).
- **Editor scene composition, sub-project 1 of the editor pivot.** The editor
  boots into `EditorShell.h`/`EditorSceneView.h` (`g_ActiveDemo` starts at
  `InvalidDemo`); `g_EditorScene` starts blank only on a first run with no
  `--scene` -- otherwise `LoadEditorProjectFromCommandLine` (`EditorProject.h`)
  reopens whatever `editor_last_scene.txt` names from the previous session.
  `GS::Scene::Save`/`Load`
  round-trip a scene through a `gs-scene` text format keyed by
  `MeshComponent::SourcePath` and resolved through the new `GS::MeshCache`;
  File > Open Demo can open any opted-in demo's placed content as a starting
  scene via `DemoLayer::OnExportToScene`. Cube3D is the one demo that opts in
  so far (`CanOpenInEditor = true`). Verified end-to-end: a hand-built scene
  and the same scene saved-then-reloaded via `--scene` render byte-identical
  captures.
- **Chunk meshes merge into fixed 3x3x3 groups, on the planet too.**
  `VoxelPlanet.h`'s chunks now draw the same way TerrainLab's do: 1,459
  meshed chunks down to 167 groups on a landed Earth. Harder than the flat
  field — chunks stream, so a group is dirtied by `StreamAround`/
  `EvictBeyond` rather than an edit, and each member's vertices have to be
  re-expressed against the group's own double-precision origin before being
  concatenated. Verified by a self-test showing triangle counts exactly
  preserved under grouping, plus byte-identical captures across repeated
  runs and Debug/Release. See the changelog, 2026-09-07. This finishes the
  roadmap item that started with TerrainLab's 93-down-to-9 (changelog,
  2026-09-04).
- **The context pipeline.** This file, a generated trap index
  (`./gs.py traps`, `--check` to detect drift), and a tiered read ladder in
  `CLAUDE.md`. Answering "what are we working on" cold went from ~252k tokens
  and wrong to ~3.9k and right.
- **Instancing.** Panels and animals batched: 764 draw calls → 33, submission
  1420 µs → 66 µs. Verified by rendering both ways and comparing pixels —
  byte-identical, 3,686,400 of 3,686,400 samples. Frame time did not move,
  because 16.667 ms is vsync.
- **`./gs.py prune`.** The checkout was 9.7 GB against 1.2 MB of source.
  Now 1.7 GB.
- **Life.** Boids, an Ornstein–Uhlenbeck lek swarm, active Brownian beetles —
  each checked against a formula `Critters.h` does not evaluate.

## What is next

**In flight: the editor pivot.** Sub-project 1 (editor boot + scene
composition, then the menu bar and layout reshuffle) landed — see "Last
landed" above. Only Cube3D opts into the editor so far; the other 16 demos
each need their own `OnExportToScene` before they can be opened as a
starting scene the same way.

The owner has since reordered what comes next, ahead of the mesh-authoring
tool originally planned as sub-project 2. The embedded text editor (originally
planned as an embedded-Neovim wrapper; decided against that in favor of a
self-built buffer + panel to avoid an external runtime dependency, per
`docs/superpowers/specs/2026-09-09-embedded-text-editor-design.md`) landed —
see "Last landed" above. Agreed order, newest discussion first:

1. **A scripting language runtime** — this is sub-project 3's logic/module
   attachment mechanism, now decided: JavaScript (authored as TypeScript,
   which compiles away before anything runs, so it costs nothing beyond
   whichever JS engine executes it), likely via QuickJS to keep it vendorable
   like the rest of `GS/vendor/`. Comes after the editor so there is
   somewhere to write a script.

Mesh authoring (the original sub-project 2) and play-in-editor (sub-project
4) are still queued; their exact slot in the list above hasn't been fixed.

Unstarted, in the order they were last discussed:

- **Multiplayer/networking foundation.** Nothing exists yet — no transport, no
  replication, no session model. Explicitly out of scope for the editor pivot
  above; scope it out as its own sub-project once the editor's scene model
  exists to replicate.
- **Character attributes.** `s_Strength` in `TerrainLab.h` is deliberately a
  constant with a hook where a stat should be — carry capacity trained by use.
  The owner deferred this ("further down the line"), so **ask before starting
  it**.

## When this file is wrong

Fix it in the same commit as the work that made it wrong. It is one page on
purpose: if updating it feels like a chore, it has grown too big, and the
subsystem table in `docs/HANDOVER.md` is where durable facts belong instead.
