# Editor menu bar and layout — design

Follows sub-project 1 (editor boot + scene composition, landed). The owner
reordered the roadmap ahead of mesh authoring: this sub-project lands first,
then an embedded terminal, then an embedded Neovim-based text editor, then
the scripting runtime (JS/TS via a small embeddable engine). See
`docs/STATE.md` for the full sequence and reasoning.

## Goal

Give the editor a conventional menu bar (File/Edit/View/Help) and reshape its
docking layout into the standard game-engine-editor arrangement — tools left,
properties right, scene centre, tabbed output along the bottom — so the
pieces landing in later sub-projects (terminal, text editor, scripting) have
an obvious place to go rather than needing another layout pass each time.

## Why now, and why this shape

Sub-project 1 built real editor content (Outliner, Inspector, an Assets
panel) but no chrome above it: no menu bar, no way to name or rename what
you're working on beyond a bare file path, no undo, and one docking
arrangement baked in at first boot with no way back to it once dragged
apart. The Assets panel in particular has drifted into three unrelated
concerns — scene file I/O, entity creation tools, and (soon) a place to
list demos — because it was the only panel that existed when each of those
needed *somewhere* to live. This sub-project gives each concern its correct
home before more panels arrive and the drift compounds.

## Scope

**In scope:** a main menu bar (File/Edit/View/Help); a Project concept with
its own name distinct from a file path; a minimal undo/redo command stack
covering editor-driven scene mutations; find-by-name over the Outliner;
named, switchable docking-layout presets; moving the Assets panel's
creation controls to a left "Tools" panel; turning the bottom dock node
into a tab well with stub panels for what's coming.

**Explicitly deferred:**
- **Multi-scene projects.** The manifest names one active scene; a level
  list is a real feature for whenever it's actually needed, not now.
- **Find/Replace as text operations.** Nothing to search yet beyond entity
  names. Text-level find/replace is the embedded editor's problem
  (sub-project 3 of this new sequence).
- **A general-purpose ECS undo/transaction system.** This sub-project's
  undo stack covers what the editor itself does, not a reusable layer other
  systems (mesh authoring, scripting) build transactions on. Revisit if a
  second consumer actually needs one.
- **Tabbing the centre viewport with a text editor.** The viewport is drawn
  by setting the GL viewport into a passthrough, untitled central dock
  node — there is no ImGui window there to put a second tab on. Making the
  centre tabbable is real design work that belongs to the sub-project that
  first needs a second centre occupant (the embedded text editor), not
  built speculatively here.
- **The terminal, the text editor, and the scripting runtime themselves.**
  This sub-project only prepares layout slots for them.

## Design

### File structure

- **`TestEnv/src/EditorMenuBar.h`** (new) — the main menu bar and its three
  behaviors: project I/O, undo/redo, layout presets. One file, because all
  three are "things the menu bar does," not because they're related to each
  other.
- **`TestEnv/src/EditorProject.h`** (extended, not replaced) — gains the
  project-manifest concept (name, active scene path) alongside the existing
  `g_EditorScene`/`g_EditorScenePath` globals it already owns.
- **`TestEnv/src/EditorHistory.h`** (new) — the undo/redo command stack.
  Separate from `EditorMenuBar.h` because the command stack is something
  every mutating control (Tools panel buttons, Inspector fields, Outliner
  delete) pushes to, not something only the menu bar touches.
- **`TestEnv/src/EditorShell.h`** (modified) — loses the Assets panel's
  Scene section (moves to `EditorMenuBar.h`'s File menu) and creation
  controls (moves to a renamed "Tools" panel, docked left); gains the
  bottom-row stub panels; `BuildLayout` reshuffled per below.

### Menu contents

**File:** New Project, Open Project…, Save, Save As…, Rename Project…,
a separator, then "Open Demo" as a submenu listing every
`CanOpenInEditor` demo (relocated from the Assets panel's "Open as starting
scene" section — same `s_DemoInstances[i]->OnExportToScene(...)` call,
same selection-clearing already fixed in sub-project 1).

**Edit:** Undo (`Ctrl+Z`), Redo (`Ctrl+Y`), a separator, Find (`Ctrl+F`,
focuses a name filter over the Outliner).

**View:** a list of saved layout presets (radio-style, checked = active),
a separator, "Save current layout as…", "Reset to default".

**Help:** minimal — an About item naming the engine and version. Nothing
else is asked for; nothing else gets built.

### The Project manifest

A project is a folder containing `project.gsproj` (a small hand-written
text format, same convention as `gs-scene`: a tag line, then `name <text>`
and `scene <relative-path>`) and the scene file(s) it names. `New Project`
creates the folder + manifest + an empty scene and opens it. `Open Project`
reads the manifest and loads the named scene via the existing
`GS::Scene::Load`. `Save`/`Save As` write the current scene to the path the
manifest names (or a new one, updating the manifest) — this is additive
over today's `SaveEditorScene`, not a replacement for `GS::Scene::Save`
itself. `Rename Project` rewrites the manifest's `name` field only; the
folder path is untouched, so a rename never breaks anything that held the
old path.

`g_EditorScenePath` keeps meaning exactly what it means today (the scene
file currently open); a new `g_EditorProjectPath`/`g_EditorProjectName`
pair (in `EditorProject.h`) tracks the project wrapping it. Opening a
project without a project file (i.e. today's bare `--scene <path>` boot)
stays valid — the project globals are simply empty, and the menu bar's
title bar / About-adjacent display shows the scene path alone, exactly
today's behavior. No existing boot path changes.

### Undo/redo

```cpp
// EditorHistory.h — sketch, not final code
class EditorCommand
{
public:
    virtual ~EditorCommand() = default;
    virtual void Undo() = 0;
    virtual void Redo() = 0;
};
```

A `std::vector<std::unique_ptr<EditorCommand>>` plus a cursor; pushing a
new command truncates anything after the cursor (standard undo-stack
shape). Each of today's mutating call sites gets a matching command type:

- **Place** (preset/import/camera buttons): `Undo` destroys the created
  entity, `Redo` recreates it with the same components. Since
  `GS::Scene`'s entity ids are index+generation and destroying/recreating
  changes both, the command stores the entity's *data* (tag, transform,
  mesh/camera component values), not its id — recreating is a fresh
  `CreateEntity` + component adds, matching exactly what `OnExportToScene`
  already does when it copies an entity's data into a new scene.
- **Delete** (Outliner/Inspector delete): the inverse of Place — captures
  the entity's full data before destroying it, so `Undo` is a Place.
- **Edit** (Inspector field changes — position, color, fov, etc.): captures
  the field's old and new value; `Undo`/`Redo` just write one back. Bound
  at the point a drag/edit *completes* (ImGui's `IsItemDeactivatedAfterEdit`),
  not per-frame while dragging — a slider held for two seconds must not
  push a thousand commands.

Scene-clearing actions (New Project, Open Project, Open Demo) clear the
undo stack too — undoing across a scene swap has no sensible meaning, and
this project's own precedent (sub-project 1's selection-clearing fix) is
exactly this reasoning applied to a different kind of stale state.

### Layout presets

A preset is a name plus whatever `ImGui::SaveIniSettingsToMemory()`
returns, written to a small file (e.g. `editor_layouts.txt`, one record
per preset, tag-and-body same convention as the rest of this project's
hand-written formats). Switching presets calls
`ImGui::LoadIniSettingsFromMemory` with the stored string. "Reset to
default" clears `m_Built` and re-runs `BuildLayout`'s original arrangement
— the same mechanism that already exists, just re-triggerable instead of
running once.

### The reshuffled layout

```
┌─────────────────────────────────────────────────────────┐
│ File  Edit  View  Help                                   │  <- menu bar
├───────────┬───────────────────────────────┬─────────────┤
│  Outliner │                               │   Profiler  │
├───────────┤          Scene view           ├─────────────┤
│   Tools   │      (unchanged mechanism)     │  Inspector  │
├───────────┴───────────────────────────────┴─────────────┤
│  Terminal  │  Build Output  │  Textures   (stub panels)  │
└─────────────────────────────────────────────────────────┘
```

Left column: Outliner on top (unchanged), "Tools" on the bottom (renamed
from "Assets", carrying the preset-shape/Import/Camera buttons — the
"Open as starting scene" list moves to the File menu instead of staying
here, since it's project-level, not a creation tool). Right column:
unchanged (Profiler / Inspector). Bottom: three stub panels, each just
`ImGui::TextDisabled("Not built yet — see sub-project N")`, docked to the
same bottom node so they appear as tabs.

### Error handling

- Opening a project folder with no `project.gsproj` inside it: treat as
  opening a bare scene if there's exactly one scene file in the folder,
  otherwise warn (`GS_WARN`) and do nothing — same fail-safe posture as
  every other load path in this codebase.
- Undo/redo at either end of the history stack: the menu items grey out
  (standard ImGui pattern), not an error.
- Loading a layout preset whose ini string is stale/invalid: ImGui itself
  tolerates unknown window names in an ini string (ignores them), so no
  extra handling is needed — verify this assumption in the temporary
  self-test.

### Testing

Per this project's convention: temporary self-tests, deleted once verified.
Given this sub-project is mostly ImGui interaction (menu clicks, docking),
which the project's own self-test pattern can't drive directly, verification
leans on the capture-based approach instead:

- A temporary test exercising `EditorHistory` directly (push a Place, undo
  it, confirm the entity is gone; redo, confirm it's back with the same
  component values) — this part *is* pure logic, testable without ImGui.
- A capture-based check that the reshuffled layout's stub panels render
  where the design says (a fresh `imgui.ini`, headless capture, confirm no
  panel is missing/mispositioned) — the same technique sub-project 1 used
  to verify Outliner/Inspector actually docked.
- Manual/human verification for the menu bar's own interactivity (clicking
  File > Save, dragging a layout preset) — same limitation Task 4 hit and
  reported honestly; ImGui clicks can't be driven headlessly in this
  environment.

## What this sub-project does not decide

The terminal's PTY/libvterm integration, the Neovim UI-protocol client, and
the scripting runtime's language-binding design are all separate
brainstorms, each with real open questions of their own (this design's
"Explicitly deferred" section lists what each one will need from the
layout when it lands).
