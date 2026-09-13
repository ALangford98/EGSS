# Scene Tree / Multi-Scene Support -- Design

## Purpose

A `ProjectManifest` today wraps exactly one `GS::Scene`: `Scene.h`'s own
comment calls this "deliberately one scene per project for now." This
project builds "self contained levels": a project that can hold *several*
scene files, a panel to browse and manage them, and controls to create new
ones, switch which is active, duplicate, rename, and delete.

## Scope

**Editor-time only.** Switching which scene is loaded is something a person
does through the editor UI. No runtime API for a running script to load a
different scene during Play -- that is a separate, later feature, once
there is a reason to build it.

**One active scene at a time.** The editor keeps exactly one `GS::Scene`
resident (`g_EditorScene`, unchanged), matching every existing subsystem
that assumes it (`EditorHistory`, `EntityGroups`, `PlayMode`, the
Outliner). Switching scenes unloads the current one and loads the target,
the same shape `OpenEditorScene` already has -- it does not keep N scenes
resident as tabs.

**A save-changes prompt is genuinely new scope**, added deliberately as
part of this project rather than smuggled in as a side effect: today
*nothing* in the editor tracks unsaved changes, and New Project/Open
Project/Open Demo all silently discard them. This project adds
dirty-tracking and applies the same prompt to those three existing actions
as well as the new scene-switch action, so the editor has one consistent
rule instead of the new code obeying a rule the old code doesn't.

## Discarded alternative

Treating each scene as a sibling project folder (reusing
`OpenEditorProject`'s machinery unchanged, one manifest per level) was
considered and rejected: it does not produce "a project containing several
levels" (what was asked for), just several unrelated projects, and it
duplicates project-open plumbing for no benefit over extending the one
manifest.

## Project manifest format

`ProjectManifest` gains a scene list:

```cpp
struct SceneEntry
{
	std::string Name;           // display name, no spaces (see below)
	std::string RelativePath;   // relative to the project folder
};

struct ProjectManifest
{
	std::string Name;
	std::string SceneRelativePath;     // unchanged meaning: the *active* scene
	std::vector<SceneEntry> Scenes;    // every scene the project knows about
};
```

On disk, one new repeatable line kind alongside the existing `name` and
`scene` lines:

```
gs-project 1
name MyProject
scene scenes/Level1.txt
sceneentry Main scenes/Main.txt
sceneentry Level1 scenes/Level1.txt
```

`scene` keeps its exact current meaning -- the active scene's path -- so
every existing reader of `g_EditorScenePath`/`ProjectManifest::
SceneRelativePath` needs no change. `sceneentry` is new and purely
additive.

**Backward and forward compatible without a version bump:**
`ReadProjectManifest`'s parsing loop already ignores any `key` it doesn't
recognize (there is no `else` branch after `name`/`scene`), so:

- An **old-format** manifest (no `sceneentry` lines) reads fine under the
  new code: `Scenes` comes back empty, and the loader synthesizes one
  entry from the existing `scene` field, named from that file's stem
  (`scenes/Level1.txt` -> `Level1`; `scene.txt` -> `scene`). No file
  rewrite happens until the user does something that changes the scene
  list (e.g. New Scene), at which point the manifest is written in the
  new format going forward.
- A **new-format** manifest read by old code (a manifest.gsproj format
  older builds don't know) still opens correctly -- old code finds `name`
  and `scene`, ignores every `sceneentry` line, and opens the one scene it
  already knew how to open.

**Scene names** are constrained to non-empty, space-free tokens (letters,
digits, `_`, `-`) -- the same shape file stems already have -- so
`sceneentry <name> <path>` can be parsed with plain `>>` extraction, no
quoting scheme needed. The New/Duplicate/Rename dialogs enforce this and
reject a name that collides with an existing entry.

**New scenes** are written under a `scenes/` subfolder inside the project
folder. Existing single-scene projects are untouched -- their one scene
stays wherever their manifest already points (typically the project root),
and nothing forces it to move.

## Dirty-tracking

`EditorHistory` already threads every undoable mutation through one choke
point (`Push`, plus `Undo`/`Redo` replaying from the stack) with a single
cursor, `s_Cursor`, into `s_Commands`. Add:

```cpp
inline size_t s_CleanCursor = 0;

inline bool IsDirty() { return s_Cursor != s_CleanCursor; }
inline void MarkClean() { s_CleanCursor = s_Cursor; }
```

`MarkClean()` is called wherever the scene is successfully written to disk
(`SaveEditorScene`, `SaveEditorProject`, `SaveEditorProjectAs`,
`CreateEditorProject` after its initial blank save). `Clear()` (called on
every scene swap already) resets both `s_Cursor` and `s_CleanCursor` to 0,
so a freshly loaded scene reads as clean.

This is exact, not a coarse boolean: undoing back to exactly the point a
save happened reads as clean again, because the cursor position -- not
just "has anything happened" -- is what's compared. A mutation that never
goes through `EditorHistory::Push` (if any such path exists) will not be
seen as dirty, the same blind spot that already exists for undo today --
this project does not introduce a new one.

## Unsaved-changes prompt

A new, small file, `TestEnv/src/SceneSwapGuard.h`:

```cpp
namespace SceneSwapGuard {
	// Runs `performSwap` now if the scene is clean; otherwise stashes it
	// and opens the confirmation modal, which runs it once the user
	// chooses Save or Discard (or drops it on Cancel).
	inline void RequestSceneSwap(std::function<void()> performSwap);

	// Renders the "Unsaved Changes" modal if one is pending. Called once
	// per frame from EditorMenuBar, beside the project popups it already
	// owns.
	inline void DrawUnsavedChangesModal();
}
```

Internally: `std::function<void()> s_PendingSwap` plus a
`s_ModalRequested` flag (the same open-inside-the-frame pattern
`EditorSceneView`'s viewport-right-click already uses, since
`ImGui::OpenPopup` must run inside the ImGui frame, not from an arbitrary
call site). The modal offers **Save** (calls `SaveEditorProject()` if a
project is open, else `SaveEditorScene(g_EditorScenePath)`, then runs and
clears `s_PendingSwap`), **Discard** (runs and clears `s_PendingSwap`
without saving), **Cancel** (clears `s_PendingSwap` without running it).

`EditorMenuBar.h`'s New Project / Open Project / Open Demo handlers, and
the new Scenes panel's switch action, all wrap their existing swap logic
in `RequestSceneSwap(...)` instead of calling it directly. This is the one
behavioral change to existing entry points: previously-silent data loss on
those three actions now prompts, consistently with the new one.

## Scenes panel

New `TestEnv/src/ScenesPanel.h`, a `Layer` docked into the same `left`
dock node Files and Outliner already share (`EditorShell.h`), as a third
tab. Hidden/disabled when no project is open (`g_EditorProjectPath`
empty), matching how "Rename Project" is already gated in
`EditorMenuBar.h`.

Lists `g_ProjectScenes` (a new `inline std::vector<SceneEntry>` in
`EditorProject.h`, alongside the existing `g_EditorProjectPath`/
`g_EditorProjectName` cache pair) -- populated by `ProjectScenes()` when a
project opens, and refreshed by that same call at the end of every
`CreateSceneInProject`/`DuplicateSceneInProject`/`RenameSceneInProject`/
`DeleteSceneFromProject`, so the panel never re-parses the manifest file
itself on every frame. Rows highlight whichever entry's `RelativePath`
matches `g_EditorScenePath`. Per-row and toolbar actions:

- **New Scene** -- prompts for a name, writes a blank `GS::Scene` (same
  "just an empty scene, nothing speculative" shape `CreateEditorProject`
  already uses) to `scenes/<name>.txt`, appends a `SceneEntry`, rewrites
  the manifest. Does not switch to it.
- **Switch** (click a row's "Open" button) -- `SceneSwapGuard::
  RequestSceneSwap([path]{ OpenEditorScene(path); })`.
- **Duplicate** -- copies the source scene's file bytes to a new path
  under `scenes/`, appends a new `SceneEntry` with a name the user
  confirms (default: `<name>_copy`, incremented if that collides).
- **Rename** -- manifest-only, `RenameSceneInProject(oldName, newName)`;
  the file and its path are untouched, matching `RenameEditorProject`'s
  own "the folder is untouched" precedent.
- **Delete** -- disabled (greyed out) for the active scene and when it is
  the project's only scene. Otherwise asks for confirmation in its own
  modal (unlike most editor actions, this one is irreversible file
  deletion with no undo), then removes the file and the manifest entry.

## New `EditorProject.h` functions

```cpp
// Reads the manifest's scene list, synthesizing one entry from
// SceneRelativePath if it's empty (old-format manifest).
inline std::vector<SceneEntry> ProjectScenes();

inline bool CreateSceneInProject(const std::string& name);
inline bool DuplicateSceneInProject(const std::string& sourceName, const std::string& newName);
inline bool RenameSceneInProject(const std::string& oldName, const std::string& newName);
inline bool DeleteSceneFromProject(const std::string& name);
```

Each rewrites `project.gsproj` via `WriteProjectManifest` (extended to
also emit `sceneentry` lines) after mutating the in-memory list. All
return `false` (and warn via `GS_WARN`, matching every existing
`EditorProject.h` function) on a filesystem failure or a name collision,
leaving the manifest and files as they were.

## Testing

Temporary self-tests (`SceneManifestTest.h`), deleted after verifying:

1. An old-format manifest (`name`/`scene` only, no `sceneentry`) round
   -trips through `ReadProjectManifest` with a synthesized one-entry
   `Scenes` list, name equal to the scene file's stem.
2. A new-format manifest with multiple `sceneentry` lines round-trips
   exactly (name, path, active scene all preserved).
3. `CreateSceneInProject`/`DuplicateSceneInProject`/`RenameSceneInProject`/
   `DeleteSceneFromProject` each produce the expected manifest and
   filesystem state, including the two documented Delete refusals (active
   scene, last scene) and a name collision refusal.
4. `EditorHistory::IsDirty()` cursor math: clean after `Clear()`, dirty
   after a `Push`, clean again after `MarkClean()`, dirty again after one
   `Undo()` moves the cursor off the clean mark, clean again after a
   second `Undo()` lands back exactly on it.

Visual verification: a `--lockstep --capture-step N` capture of the
Scenes panel with 2+ entries showing the active one highlighted, and a
second capture with the Unsaved Changes modal open over it.
