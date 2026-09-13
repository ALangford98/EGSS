# Scene Tree / Multi-Scene Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a project hold several scene files ("levels"), with a panel to
browse/create/duplicate/rename/delete them and switch which one is loaded,
plus a consistent unsaved-changes prompt everywhere the editor swaps scenes.

**Architecture:** Extend `ProjectManifest` with an additive scene-entry list
(old manifests synthesize one entry, new manifests degrade gracefully under
old code); add exact dirty-tracking to `EditorHistory`'s existing undo
cursor; a small `SceneSwapGuard` namespace centralizes "check for unsaved
changes before swapping the scene" and is used by both the existing
New/Open Project/Open Demo actions and the new Scenes panel.

**Tech Stack:** C++17, ImGui (immediate-mode panels/modals), the project's
own hand-rolled tagged-text file formats (no JSON/XML library involved
here).

**Spec:** `docs/superpowers/specs/2026-09-13-scene-tree-design.md`

## Global Constraints

- Editor-time only. No runtime scene-loading API for scripts this round.
- One scene resident at a time (`g_EditorScene`, unchanged) -- never
  multiple scenes open as tabs.
- The `scene` manifest line keeps its exact current meaning (the active
  scene's path); no manifest version bump; unrecognized lines are already
  silently skipped by `ReadProjectManifest`, which is what makes the format
  change additive in both directions.
- Scene names: non-empty, no spaces, unique within a project.
- Verify all three configs (Debug/Release/Dist) via `./gs.py build all`
  before any task is marked done, per this project's own build discipline.
- Every temporary test file is deleted, and `grep -rn TEMPORARY TestEnv/src`
  comes back empty, before the final task is marked done.

---

## Task 1: Manifest format -- scene entries, read/write, backward compat

**Files:**
- Modify: `TestEnv/src/EditorProject.h` (the `ProjectManifest` struct,
  `ReadProjectManifest`, `WriteProjectManifest`)
- Test: `TestEnv/src/SceneManifestFormatTest.h` (new, temporary)

**Interfaces:**
- Produces: `struct SceneEntry { std::string Name; std::string
  RelativePath; };`, `ProjectManifest::Scenes` (a `std::vector<SceneEntry>`),
  `inline std::vector<SceneEntry> g_ProjectScenes;` (global cache, empty
  until a project is opened/created -- populated in Task 2).

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/SceneManifestFormatTest.h`:

```cpp
// TEMPORARY -- delete after verifying Task 1 (scene manifest format).
#pragma once
#include <GS.h>
#include "EditorProject.h"
#include <filesystem>
#include <fstream>

namespace SceneManifestFormatTest {
	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		std::string dir = ".claude_scenetest_manifest";
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);

		// Old-format manifest: name + scene only, no sceneentry lines --
		// exactly what every manifest written before this task looks like.
		{
			std::ofstream out(dir + "/project.gsproj");
			out << "gs-project 1\n";
			out << "name OldProj\n";
			out << "scene scene.txt\n";
		}
		ProjectManifest manifest;
		bool read = ReadProjectManifest(dir, manifest);
		Check(read, "old-format manifest reads");
		Check(manifest.Name == "OldProj", "old-format name preserved");
		Check(manifest.SceneRelativePath == "scene.txt", "old-format scene path preserved");
		Check(manifest.Scenes.size() == 1, "old-format synthesizes one scene entry");
		if (manifest.Scenes.size() == 1)
		{
			Check(manifest.Scenes[0].Name == "scene", "synthesized name is the file's stem");
			Check(manifest.Scenes[0].RelativePath == "scene.txt", "synthesized path matches the scene field");
		}

		// New-format manifest: multiple sceneentry lines round-trip exactly.
		ProjectManifest toWrite;
		toWrite.Name = "NewProj";
		toWrite.SceneRelativePath = "scenes/Level1.txt";
		toWrite.Scenes = { { "Main", "scenes/Main.txt" }, { "Level1", "scenes/Level1.txt" } };
		Check(WriteProjectManifest(dir, toWrite), "new-format manifest writes");

		ProjectManifest roundTripped;
		Check(ReadProjectManifest(dir, roundTripped), "new-format manifest reads back");
		Check(roundTripped.Name == "NewProj", "new-format name round-trips");
		Check(roundTripped.SceneRelativePath == "scenes/Level1.txt", "new-format active scene round-trips");
		Check(roundTripped.Scenes.size() == 2, "new-format keeps both scene entries");
		if (roundTripped.Scenes.size() == 2)
		{
			Check(roundTripped.Scenes[0].Name == "Main" && roundTripped.Scenes[0].RelativePath == "scenes/Main.txt", "first entry round-trips");
			Check(roundTripped.Scenes[1].Name == "Level1" && roundTripped.Scenes[1].RelativePath == "scenes/Level1.txt", "second entry round-trips");
		}

		std::filesystem::remove_all(dir);
		GS_TRACE("SceneManifestFormatTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it into `TestEnv/src/TestApp.cpp`: add `#include "SceneManifestFormatTest.h"` near the other includes, and `SceneManifestFormatTest::Run();` as the first line of the `TestEnv()` constructor body (before `PushLayer(new DemoWarmup());`) -- it touches no ImGui/GL state, so it can run before anything else.

- [ ] **Step 2: Build to confirm it fails**

Run: `./gs.py build`
Expected: fails to compile -- `ProjectManifest` has no member `Scenes` yet.

- [ ] **Step 3: Extend `ProjectManifest` and the reader/writer**

In `TestEnv/src/EditorProject.h`, replace the `ProjectManifest` struct:

```cpp
struct SceneEntry
{
	std::string Name;
	std::string RelativePath;
};

struct ProjectManifest
{
	std::string Name;
	std::string SceneRelativePath;
	std::vector<SceneEntry> Scenes;
};
```

Add `#include <vector>` to the file's includes if not already present via a
transitive include (it is not -- `EditorProject.h` currently includes only
`<GS.h>`, `<fstream>`, `<filesystem>`, `<sstream>`, `"ThemeManager.h"`).

In `ReadProjectManifest`, add a `sceneentry` case to the parsing loop
(alongside the existing `name`/`scene` cases):

```cpp
		else if (key == "sceneentry")
		{
			SceneEntry entry;
			fields >> entry.Name >> entry.RelativePath;
			out.Scenes.push_back(entry);
		}
```

Then, right before the existing `return !out.SceneRelativePath.empty();`,
add the synthesis for an old-format manifest:

```cpp
	// Old-format manifest (no sceneentry lines): synthesize the one scene
	// it already names, so every caller of ReadProjectManifest can rely on
	// Scenes being non-empty for any manifest with a valid `scene` line,
	// without needing to know the format's history.
	if (out.Scenes.empty() && !out.SceneRelativePath.empty())
	{
		std::string stem = std::filesystem::path(out.SceneRelativePath).stem().string();
		out.Scenes.push_back({ stem, out.SceneRelativePath });
	}

	return !out.SceneRelativePath.empty();
```

In `WriteProjectManifest`, add the `sceneentry` lines after the existing
`scene` line:

```cpp
	out << "gs-project 1\n";
	out << "name " << manifest.Name << "\n";
	out << "scene " << manifest.SceneRelativePath << "\n";
	for (const SceneEntry& entry : manifest.Scenes)
		out << "sceneentry " << entry.Name << " " << entry.RelativePath << "\n";
	return true;
```

Finally, add the in-memory cache global, next to `g_EditorProjectPath`/
`g_EditorProjectName`:

```cpp
// Every scene the open project knows about (mirrors the manifest's
// sceneentry lines). Empty when no project is open. Task 2's
// Create/Duplicate/Rename/DeleteSceneFromProject keep this in sync with
// disk; nothing here re-reads the manifest mid-session.
inline std::vector<SceneEntry> g_ProjectScenes;
```

- [ ] **Step 4: Run the test**

Run: `./gs.py build && ./TestEnv --hide-window --capture-step 1 --capture .claude_scenetest.png` from
`bin/Debug-linux-x86_64/TestEnv` (any flags that make it exit quickly are
fine -- the test runs at construction, before the first frame). Read the
log for `SceneManifestFormatTest:`.
Expected: `SceneManifestFormatTest: 13 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

Delete `TestEnv/src/SceneManifestFormatTest.h` and its two lines in
`TestApp.cpp` (`#include` and the `Run()` call).

- [ ] **Step 6: Build all three configs and commit**

Run: `./gs.py build all`
Expected: clean build, all three configs.

```bash
git add TestEnv/src/EditorProject.h
git commit -m "Add multi-scene entries to the project manifest format"
```

---

## Task 2: Create / duplicate / rename / delete scenes in a project

**Files:**
- Modify: `TestEnv/src/EditorProject.h` (`CreateEditorProject`,
  `SaveEditorProjectAs`, `OpenEditorProject` -- each needs to populate
  `g_ProjectScenes`; four new functions)
- Test: `TestEnv/src/SceneMutationTest.h` (new, temporary)

**Interfaces:**
- Consumes: `SceneEntry`, `ProjectManifest`, `g_ProjectScenes`,
  `ReadProjectManifest`/`WriteProjectManifest` (Task 1).
- Produces:
  `bool CreateSceneInProject(const std::string& name)`,
  `bool DuplicateSceneInProject(const std::string& sourceName, const std::string& newName)`,
  `bool RenameSceneInProject(const std::string& oldName, const std::string& newName)`,
  `bool DeleteSceneFromProject(const std::string& name)`.
  All four return `false` and leave state untouched on any failure
  (invalid name, collision, missing source, filesystem error, or --
  for delete -- the two refusal cases below).

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/SceneMutationTest.h`:

```cpp
// TEMPORARY -- delete after verifying Task 2 (scene create/duplicate/rename/delete).
#pragma once
#include <GS.h>
#include "EditorProject.h"
#include <filesystem>

namespace SceneMutationTest {
	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		std::string dir = ".claude_scenetest_mutation";
		std::filesystem::remove_all(dir);

		Check(CreateEditorProject(dir, "MutationTest"), "project creates");
		Check(g_ProjectScenes.size() == 1 && g_ProjectScenes[0].Name == "Main", "new project starts with one 'Main' scene");

		Check(CreateSceneInProject("Level1"), "creates Level1");
		Check(g_ProjectScenes.size() == 2, "scene count is 2 after create");
		Check(std::filesystem::exists(dir + "/scenes/Level1.txt"), "Level1's file exists on disk");

		Check(!CreateSceneInProject("Level1"), "refuses a colliding name");
		Check(!CreateSceneInProject("Bad Name"), "refuses a name with a space");

		Check(DuplicateSceneInProject("Level1", "Level2"), "duplicates Level1 as Level2");
		Check(g_ProjectScenes.size() == 3, "scene count is 3 after duplicate");
		Check(std::filesystem::exists(dir + "/scenes/Level2.txt"), "Level2's file exists on disk");
		Check(!DuplicateSceneInProject("DoesNotExist", "Whatever"), "refuses to duplicate a missing source");

		Check(RenameSceneInProject("Level2", "Level2Renamed"), "renames Level2");
		bool foundRenamed = false;
		for (auto& e : g_ProjectScenes) if (e.Name == "Level2Renamed") foundRenamed = true;
		Check(foundRenamed, "renamed entry present under new name");
		Check(std::filesystem::exists(dir + "/scenes/Level2.txt"), "rename left the file's path untouched");
		Check(RenameSceneInProject("Level2Renamed", "Level2Renamed"), "renaming to the same name is a no-op success, not a collision");

		Check(!DeleteSceneFromProject("Main"), "refuses to delete the active scene");
		Check(DeleteSceneFromProject("Level2Renamed"), "deletes a non-active scene");
		Check(g_ProjectScenes.size() == 2, "scene count is 2 after delete");
		Check(!std::filesystem::exists(dir + "/scenes/Level2.txt"), "deleted scene's file is removed from disk");

		Check(DeleteSceneFromProject("Level1"), "deletes down to one scene");
		Check(!DeleteSceneFromProject("Main"), "refuses to delete the project's last remaining scene");
		Check(g_ProjectScenes.size() == 1, "exactly one scene remains");

		std::filesystem::remove_all(dir);
		GS_TRACE("SceneMutationTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it into `TestApp.cpp` the same way as Task 1's test (include +
`SceneMutationTest::Run();` right after `SceneManifestFormatTest::Run();`
if that hasn't been removed yet, otherwise as the first line).

- [ ] **Step 2: Run to confirm it fails**

Run: `./gs.py build`
Expected: fails to compile -- `CreateSceneInProject` etc. don't exist yet.

- [ ] **Step 3: Implement the four functions**

In `TestEnv/src/EditorProject.h`, add after `RenameEditorProject`:

```cpp
// Non-empty, no spaces, and not already used by another scene in this
// project (matching against `ignoreExisting` lets a no-op rename -- to
// the name it already has -- succeed instead of colliding with itself).
inline bool IsValidSceneName(const std::string& name, const std::string& ignoreExisting = "")
{
	if (name.empty())
		return false;
	for (char c : name)
		if (std::isspace((unsigned char)c))
			return false;
	for (const SceneEntry& entry : g_ProjectScenes)
		if (entry.Name == name && entry.Name != ignoreExisting)
			return false;
	return true;
}

// Rewrites project.gsproj from g_ProjectScenes plus whichever entry's path
// matches the currently-active g_EditorScenePath -- the manifest's `scene`
// line is derived here rather than stored separately, so the two can never
// drift apart.
inline void PersistProjectScenes()
{
	ProjectManifest manifest;
	manifest.Name = g_EditorProjectName;
	manifest.Scenes = g_ProjectScenes;

	for (const SceneEntry& entry : g_ProjectScenes)
	{
		std::string joined = (std::filesystem::path(g_EditorProjectPath) / entry.RelativePath).string();
		if (joined == g_EditorScenePath)
		{
			manifest.SceneRelativePath = entry.RelativePath;
			break;
		}
	}

	WriteProjectManifest(g_EditorProjectPath, manifest);
}

inline bool CreateSceneInProject(const std::string& name)
{
	if (g_EditorProjectPath.empty() || !IsValidSceneName(name))
		return false;

	std::filesystem::path scenesDir = std::filesystem::path(g_EditorProjectPath) / "scenes";
	std::error_code ec;
	std::filesystem::create_directories(scenesDir, ec);
	if (ec)
	{
		GS_WARN("Could not create '{0}': {1}", scenesDir.string(), ec.message());
		return false;
	}

	std::string relativePath = "scenes/" + name + ".txt";
	std::string fullPath = (std::filesystem::path(g_EditorProjectPath) / relativePath).string();

	// Truly blank -- no entities -- the same "nothing speculative" shape
	// CreateEditorProject's own initial scene already uses.
	GS::Scene blank;
	if (!blank.Save(fullPath))
	{
		GS_WARN("Could not create the new scene's file at '{0}'", fullPath);
		return false;
	}

	g_ProjectScenes.push_back({ name, relativePath });
	PersistProjectScenes();
	return true;
}

inline bool DuplicateSceneInProject(const std::string& sourceName, const std::string& newName)
{
	if (g_EditorProjectPath.empty() || !IsValidSceneName(newName))
		return false;

	const SceneEntry* source = nullptr;
	for (const SceneEntry& entry : g_ProjectScenes)
		if (entry.Name == sourceName)
			source = &entry;
	if (!source)
		return false;

	std::string relativePath = "scenes/" + newName + ".txt";
	std::filesystem::path sourcePath = std::filesystem::path(g_EditorProjectPath) / source->RelativePath;
	std::filesystem::path destPath = std::filesystem::path(g_EditorProjectPath) / relativePath;

	std::error_code ec;
	std::filesystem::create_directories(destPath.parent_path(), ec);
	std::filesystem::copy_file(sourcePath, destPath, ec);
	if (ec)
	{
		GS_WARN("Could not duplicate '{0}' to '{1}': {2}", sourcePath.string(), destPath.string(), ec.message());
		return false;
	}

	g_ProjectScenes.push_back({ newName, relativePath });
	PersistProjectScenes();
	return true;
}

inline bool RenameSceneInProject(const std::string& oldName, const std::string& newName)
{
	if (g_EditorProjectPath.empty() || !IsValidSceneName(newName, oldName))
		return false;

	for (SceneEntry& entry : g_ProjectScenes)
	{
		if (entry.Name != oldName)
			continue;
		entry.Name = newName;
		PersistProjectScenes();
		return true;
	}
	return false;
}

// Refuses the active scene (switch away first) and the project's last
// remaining scene (a project with zero scenes has nothing to open).
inline bool DeleteSceneFromProject(const std::string& name)
{
	if (g_EditorProjectPath.empty() || g_ProjectScenes.size() <= 1)
		return false;

	for (size_t i = 0; i < g_ProjectScenes.size(); i++)
	{
		if (g_ProjectScenes[i].Name != name)
			continue;

		std::string fullPath = (std::filesystem::path(g_EditorProjectPath) / g_ProjectScenes[i].RelativePath).string();
		if (fullPath == g_EditorScenePath)
			return false;

		std::error_code ec;
		std::filesystem::remove(fullPath, ec);
		g_ProjectScenes.erase(g_ProjectScenes.begin() + i);
		PersistProjectScenes();
		return true;
	}
	return false;
}
```

Add `#include <cctype>` (for `std::isspace`) to `EditorProject.h`'s
includes.

Now populate `g_ProjectScenes` at the three existing places a project's
scene list becomes known. In `CreateEditorProject`, after `manifest.
SceneRelativePath = "scene.txt";`, add:

```cpp
	manifest.Scenes = { { "Main", "scene.txt" } };
```

and after the existing `g_EditorScenePath = scenePath;` at the end of the
function, add:

```cpp
	g_ProjectScenes = manifest.Scenes;
```

In `SaveEditorProjectAs`, same two additions (a `Save As` starts the new
project folder with exactly one scene -- the one that was open -- not a
copy of the whole source project's scene list, since it names a single new
folder from what's currently loaded): after `manifest.SceneRelativePath =
"scene.txt";` add `manifest.Scenes = { { "Main", "scene.txt" } };`, and
after `g_EditorScenePath = scenePath;` add `g_ProjectScenes = manifest.
Scenes;`.

In `OpenEditorProject`, after the existing `g_EditorProjectName =
manifest.Name;`, add:

```cpp
	g_ProjectScenes = manifest.Scenes;
```

- [ ] **Step 4: Run the tests**

Run: `./gs.py build && ` then run `TestEnv --hide-window --capture-step 1
--capture .claude_scenetest.png` from `bin/Debug-linux-x86_64/TestEnv` and
read the log.
Expected: `SceneMutationTest: 22 passed, 0 failed` (and Task 1's test,
if not yet removed, still fully passing).

- [ ] **Step 5: Remove the temporary test**

Delete `TestEnv/src/SceneMutationTest.h` and its `TestApp.cpp` wiring.

- [ ] **Step 6: Build all three configs and commit**

Run: `./gs.py build all`

```bash
git add TestEnv/src/EditorProject.h
git commit -m "Add scene create/duplicate/rename/delete to EditorProject"
```

---

## Task 3: Exact dirty-tracking on EditorHistory's undo cursor

**Files:**
- Modify: `TestEnv/src/EditorHistory.h`
- Test: `TestEnv/src/SceneDirtyTest.h` (new, temporary)

**Interfaces:**
- Consumes: `EditorHistory::s_Cursor`, `Push`, `Undo`, `Redo`, `Clear`
  (all pre-existing).
- Produces: `EditorHistory::IsDirty()` (bool), `EditorHistory::MarkClean()`.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/SceneDirtyTest.h`:

```cpp
// TEMPORARY -- delete after verifying Task 3 (scene dirty-tracking).
#pragma once
#include <GS.h>
#include "EditorHistory.h"

namespace SceneDirtyTest {
	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();
		EditorHistory::Clear();

		Check(!EditorHistory::IsDirty(), "clean immediately after Clear()");

		GS::TransformComponent transform;
		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("DirtyTestEntity", transform, std::nullopt, std::nullopt, std::nullopt));
		Check(EditorHistory::IsDirty(), "dirty after one Push");

		EditorHistory::MarkClean();
		Check(!EditorHistory::IsDirty(), "clean again after MarkClean()");

		EditorHistory::Undo();
		Check(EditorHistory::IsDirty(), "dirty after Undo moves off the clean mark");

		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("DirtyTestEntity2", transform, std::nullopt, std::nullopt, std::nullopt));
		EditorHistory::MarkClean();
		EditorHistory::Undo();
		EditorHistory::Redo();
		Check(!EditorHistory::IsDirty(), "clean again after Undo then Redo lands back on the clean mark");

		g_EditorScene.Clear();
		EditorHistory::Clear();
		GS_TRACE("SceneDirtyTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp` the same way as the previous two tasks' tests.
This one needs a live `g_EditorScene` (via `PlaceEntityCommand::Redo`,
which calls `g_EditorScene.CreateEntity`), which is a global available from
construction, so it still runs fine before `PushLayer`.

- [ ] **Step 2: Run to confirm it fails**

Run: `./gs.py build`
Expected: fails to compile -- `EditorHistory::IsDirty`/`MarkClean` don't
exist yet.

- [ ] **Step 3: Add the dirty-tracking**

In `TestEnv/src/EditorHistory.h`'s `namespace EditorHistory` block, add
next to `s_Cursor`:

```cpp
	// The cursor value at the last successful save. Dirty is "the cursor
	// has moved since then" -- exact, not a coarse boolean: undoing back
	// to precisely the point a save happened reads as clean again, because
	// position (not just "has anything happened") is what's compared.
	inline size_t s_CleanCursor = 0;

	inline bool IsDirty() { return s_Cursor != s_CleanCursor; }
	inline void MarkClean() { s_CleanCursor = s_Cursor; }
```

In `Clear()`, add `s_CleanCursor = 0;` alongside the existing resets:

```cpp
	inline void Clear()
	{
		s_Commands.clear();
		s_Cursor = 0;
		s_CleanCursor = 0;
		s_Remap.clear();
	}
```

Do **not** call `MarkClean()` from inside `EditorProject.h`'s save
functions -- that file must not include `EditorHistory.h` (which already
includes `EditorProject.h`; the reverse would be circular, and this
project's own `EditorHistory.h` header comment already calls out keeping
this dependency one-way on purpose). `MarkClean()` is called by the UI
call sites that invoke a save, in Task 4.

- [ ] **Step 4: Run the test**

Run: `./gs.py build && ` then run `TestEnv --hide-window --capture-step 1
--capture .claude_scenetest.png` from `bin/Debug-linux-x86_64/TestEnv` and
read the log.
Expected: `SceneDirtyTest: 5 passed, 0 failed`.

- [ ] **Step 5: Remove the temporary test**

Delete `TestEnv/src/SceneDirtyTest.h` and its `TestApp.cpp` wiring.

- [ ] **Step 6: Build all three configs and commit**

Run: `./gs.py build all`

```bash
git add TestEnv/src/EditorHistory.h
git commit -m "Add exact dirty-tracking to EditorHistory's undo cursor"
```

---

## Task 4: SceneSwapGuard -- unsaved-changes prompt on every scene swap

**Files:**
- Create: `TestEnv/src/SceneSwapGuard.h`
- Modify: `TestEnv/src/EditorMenuBar.h` (route New Project/Open
  Project/Open Demo through the guard; remove the now-shared
  `ResetForNewScene`; call `MarkClean()` on Save/Save As; draw the modal)
- Test: `TestEnv/src/SceneSwapGuardTest.h` (new, temporary)

**Interfaces:**
- Consumes: `EditorHistory::IsDirty`/`MarkClean` (Task 3),
  `g_EditorSceneView` (`EditorSceneView.h`, pre-existing).
- Produces:
  `bool SceneSwapGuard::RequestSceneSwap(std::function<void()> performSwap)`
  (returns `true` if `performSwap` already ran synchronously, `false` if
  deferred to the modal),
  `void SceneSwapGuard::ResetForNewScene()` (moved here from
  `EditorMenuBar`, unchanged behavior),
  `void SceneSwapGuard::DrawUnsavedChangesModal()`,
  `bool SceneSwapGuard::HasPendingSwap()`,
  `void SceneSwapGuard::ClearPendingSwap()`.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/SceneSwapGuardTest.h`:

```cpp
// TEMPORARY -- delete after verifying Task 4 (scene swap guard).
#pragma once
#include <GS.h>
#include "EditorHistory.h"
#include "SceneSwapGuard.h"

namespace SceneSwapGuardTest {
	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();
		EditorHistory::Clear();

		int callCount = 0;
		bool ranNow = SceneSwapGuard::RequestSceneSwap([&callCount]{ callCount++; });
		Check(ranNow, "clean scene: RequestSceneSwap runs performSwap immediately");
		Check(callCount == 1, "performSwap actually ran once");
		Check(!SceneSwapGuard::HasPendingSwap(), "nothing pending after an immediate run");

		GS::TransformComponent transform;
		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("SwapGuardTestEntity", transform, std::nullopt, std::nullopt, std::nullopt));

		ranNow = SceneSwapGuard::RequestSceneSwap([&callCount]{ callCount++; });
		Check(!ranNow, "dirty scene: RequestSceneSwap defers instead of running");
		Check(callCount == 1, "deferred performSwap has not run yet");
		Check(SceneSwapGuard::HasPendingSwap(), "a swap is now pending");

		SceneSwapGuard::ClearPendingSwap();
		Check(!SceneSwapGuard::HasPendingSwap(), "ClearPendingSwap drops it");

		g_EditorScene.Clear();
		EditorHistory::Clear();
		GS_TRACE("SceneSwapGuardTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp` the same way as the previous tasks.

- [ ] **Step 2: Run to confirm it fails**

Run: `./gs.py build`
Expected: fails to compile -- `SceneSwapGuard.h` doesn't exist yet.

- [ ] **Step 3: Create `SceneSwapGuard.h`**

```cpp
#pragma once

// Centralizes "does the scene have unsaved changes, and if so, ask before
// swapping it out" -- used by every action that replaces g_EditorScene
// (New/Open Project, Open Demo, and the Scenes panel's switch action) so
// all of them follow the same rule instead of each silently discarding
// edits the way this editor's project actions all did before this file
// existed. See docs/superpowers/specs/2026-09-13-scene-tree-design.md.

#include <GS.h>
#include <imgui.h>
#include <functional>

#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorProject.h"

namespace SceneSwapGuard {

	inline std::function<void()> s_PendingSwap;
	// ImGui::OpenPopup must run inside the ImGui frame, not from an
	// arbitrary call site -- the same deferred-open pattern EditorMenuBar's
	// own m_PendingPopup and EditorSceneView's viewport right-click already
	// use for exactly this reason.
	inline bool s_ModalRequested = false;

	// Every action that swaps the scene out from under the editor needs
	// undo history and selection cleared, or Undo/Redo and the current
	// selection end up referencing entities from a scene that no longer
	// exists. Centralised so no call site can forget it -- moved here from
	// EditorMenuBar so the Scenes panel's switch action can share it too.
	inline void ResetForNewScene()
	{
		EditorHistory::Clear();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(GS::InvalidEntity);
	}

	inline bool HasPendingSwap() { return (bool)s_PendingSwap; }
	inline void ClearPendingSwap() { s_PendingSwap = nullptr; s_ModalRequested = false; }

	// Runs `performSwap` right now if the scene has no unsaved changes.
	// Otherwise stashes it and requests the confirmation modal, which runs
	// it once the user chooses Save or Discard (or drops it on Cancel).
	// Returns true if performSwap already ran.
	inline bool RequestSceneSwap(std::function<void()> performSwap)
	{
		if (!EditorHistory::IsDirty())
		{
			performSwap();
			return true;
		}

		s_PendingSwap = std::move(performSwap);
		s_ModalRequested = true;
		return false;
	}

	// Call once per frame from wherever the editor's other modals already
	// render (EditorMenuBar, alongside its own project dialogs).
	inline void DrawUnsavedChangesModal()
	{
		if (s_ModalRequested)
		{
			ImGui::OpenPopup("Unsaved Changes");
			s_ModalRequested = false;
		}

		if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::Text("This scene has unsaved changes.");

		// No path to save to yet (a bare --scene session that was never
		// saved) -- Save would silently fail, so it's not offered; the
		// menu bar's own "Save" item is disabled for the same reason.
		if (!g_EditorScenePath.empty())
		{
			if (ImGui::Button("Save"))
			{
				if (!g_EditorProjectPath.empty())
					SaveEditorProject();
				else
					SaveEditorScene(g_EditorScenePath);
				EditorHistory::MarkClean();

				if (s_PendingSwap)
					s_PendingSwap();
				ClearPendingSwap();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
		}

		if (ImGui::Button("Discard"))
		{
			if (s_PendingSwap)
				s_PendingSwap();
			ClearPendingSwap();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			ClearPendingSwap();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

}
```

- [ ] **Step 4: Update `EditorMenuBar.h` to use the guard**

Add `#include "SceneSwapGuard.h"` to its includes.

Delete the private `ResetForNewScene()` method entirely (lines currently
just before `DrawFileMenu`).

Replace the "New Project" popup body:

```cpp
		if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			DrawFolderField();
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Create"))
			{
				bool succeeded = false;
				bool ranNow = SceneSwapGuard::RequestSceneSwap([this, &succeeded]
				{
					succeeded = CreateEditorProject(m_DialogPath, m_DialogName);
					if (succeeded)
						SceneSwapGuard::ResetForNewScene();
				});
				// Either it ran now and we know whether it succeeded, or
				// it was handed to the Unsaved Changes modal -- either
				// way, this dialog's own job is done.
				if (!ranNow || succeeded)
					ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
```

Replace the "Open Project" popup body:

```cpp
		if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			DrawFolderField();
			if (ImGui::Button("Open"))
			{
				bool succeeded = false;
				bool ranNow = SceneSwapGuard::RequestSceneSwap([this, &succeeded]
				{
					// GS::Scene::Load clears the scene before returning
					// false on a wrong tag/version, so a failed open can
					// still have wiped g_EditorScene -- reset regardless.
					succeeded = OpenEditorProject(m_DialogPath);
					SceneSwapGuard::ResetForNewScene();
				});
				if (!ranNow || succeeded)
					ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
```

Replace the "Open Demo" `MenuItem` body:

```cpp
					if (ImGui::MenuItem(s_Demos[i].Name))
					{
						SceneSwapGuard::RequestSceneSwap([this, i]
						{
							g_EditorScene.Clear();
							g_EditorScenePath.clear();
							g_EditorProjectPath.clear();
							g_EditorProjectName.clear();
							SceneSwapGuard::ResetForNewScene();
							s_DemoInstances[i]->OnExportToScene(g_EditorScene);
						});
					}
```

In `DrawPlayControls`'s `Stop` button, replace the bare `ResetForNewScene();`
call with `SceneSwapGuard::ResetForNewScene();` (Play/Stop is not one of
the guarded actions -- it keeps its existing, un-prompted revert-to-snapshot
behavior; only the helper it calls moved).

In the "Save" `MenuItem` handler, add a `MarkClean()` call:

```cpp
		if (ImGui::MenuItem("Save", nullptr, false, !g_EditorScenePath.empty()))
		{
			if (!g_EditorProjectPath.empty())
				SaveEditorProject();
			else
				SaveEditorScene(g_EditorScenePath);
			EditorHistory::MarkClean();
		}
```

In the "Save Project As" popup's `Save` button, add the same:

```cpp
			if (ImGui::Button("Save") && SaveEditorProjectAs(m_DialogPath, m_DialogName))
			{
				EditorHistory::MarkClean();
				ImGui::CloseCurrentPopup();
			}
```

Finally, in `OnImGuiRender`, add the modal draw call alongside the
existing ones:

```cpp
		DrawFileDialogs();
		DrawViewDialogs();
		DrawFindPopup();
		SceneSwapGuard::DrawUnsavedChangesModal();
```

- [ ] **Step 5: Run the test**

Run: `./gs.py build && ` then run `TestEnv --hide-window --capture-step 1
--capture .claude_scenetest.png` from `bin/Debug-linux-x86_64/TestEnv` and
read the log.
Expected: `SceneSwapGuardTest: 7 passed, 0 failed`.

- [ ] **Step 6: Remove the temporary test**

Delete `TestEnv/src/SceneSwapGuardTest.h` and its `TestApp.cpp` wiring.

- [ ] **Step 7: Build all three configs and commit**

Run: `./gs.py build all`

```bash
git add TestEnv/src/SceneSwapGuard.h TestEnv/src/EditorMenuBar.h
git commit -m "Add SceneSwapGuard and route project actions through it"
```

---

## Task 5: Scenes panel

**Files:**
- Create: `TestEnv/src/ScenesPanel.h`
- Modify: `TestEnv/src/EditorShell.h` (dock "Scenes" into `left`)
- Modify: `TestEnv/src/TestApp.cpp` (push the new layer)

**Interfaces:**
- Consumes: `g_ProjectScenes`, `g_EditorProjectPath`, `g_EditorScenePath`,
  `OpenEditorScene`, `CreateSceneInProject`, `DuplicateSceneInProject`,
  `RenameSceneInProject`, `DeleteSceneFromProject` (Tasks 1-2),
  `SceneSwapGuard::RequestSceneSwap`/`ResetForNewScene` (Task 4).
- Produces: nothing further consumes this panel -- it is a leaf UI layer.

- [ ] **Step 1: Create `ScenesPanel.h`**

```cpp
#pragma once

// The "Scenes" panel: browse, create, duplicate, rename, and delete a
// project's scene files, and switch which one is loaded. See
// docs/superpowers/specs/2026-09-13-scene-tree-design.md.

#include <GS.h>
#include <imgui.h>
#include <cstring>
#include <filesystem>

#include "EditorProject.h"
#include "SceneSwapGuard.h"

class ScenesPanel : public GS::Layer
{
public:
	ScenesPanel() : Layer("ScenesPanel") {}

	void OnImGuiRender() override
	{
		ImGui::Begin("Scenes");

		if (g_EditorProjectPath.empty())
		{
			ImGui::TextDisabled("Open or create a project to manage its scenes.");
			ImGui::End();
			return;
		}

		for (const SceneEntry& entry : g_ProjectScenes)
		{
			ImGui::PushID(entry.Name.c_str());

			std::string fullPath = (std::filesystem::path(g_EditorProjectPath) / entry.RelativePath).string();
			bool isActive = (fullPath == g_EditorScenePath);

			if (isActive)
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", entry.Name.c_str());
			else
				ImGui::Text("%s", entry.Name.c_str());

			ImGui::SameLine();
			if (!isActive && ImGui::SmallButton("Open"))
			{
				std::string path = fullPath;
				SceneSwapGuard::RequestSceneSwap([path]
				{
					OpenEditorScene(path);
					SceneSwapGuard::ResetForNewScene();
				});
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Duplicate"))
			{
				m_DuplicateSource = entry.Name;
				std::string suggested = entry.Name + "_copy";
				strncpy(m_NameBuf, suggested.c_str(), sizeof(m_NameBuf) - 1);
				m_NameBuf[sizeof(m_NameBuf) - 1] = '\0';
				m_PendingPopup = "Duplicate Scene";
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Rename"))
			{
				m_RenameTarget = entry.Name;
				strncpy(m_NameBuf, entry.Name.c_str(), sizeof(m_NameBuf) - 1);
				m_NameBuf[sizeof(m_NameBuf) - 1] = '\0';
				m_PendingPopup = "Rename Scene";
			}

			ImGui::SameLine();
			bool canDelete = !isActive && g_ProjectScenes.size() > 1;
			if (!canDelete) ImGui::BeginDisabled();
			if (ImGui::SmallButton("Delete"))
			{
				m_DeleteTarget = entry.Name;
				m_PendingPopup = "Delete Scene";
			}
			if (!canDelete) ImGui::EndDisabled();

			ImGui::PopID();
		}

		ImGui::Separator();
		if (ImGui::Button("New Scene..."))
		{
			m_NameBuf[0] = '\0';
			m_PendingPopup = "New Scene";
		}

		DrawDialogs();
		ImGui::End();
	}

private:
	// Same ID-scope reasoning as EditorMenuBar's own m_PendingPopup:
	// OpenPopup must run at the same root scope as the matching
	// BeginPopupModal, so the actual OpenPopup call is deferred to here
	// rather than fired from inside a SmallButton's own callback above.
	void DrawDialogs()
	{
		if (m_PendingPopup)
		{
			ImGui::OpenPopup(m_PendingPopup);
			m_PendingPopup = nullptr;
		}

		if (ImGui::BeginPopupModal("New Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Create") && CreateSceneInProject(m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Duplicate Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("New Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Duplicate") && DuplicateSceneInProject(m_DuplicateSource, m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Rename Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("New Name", m_NameBuf, sizeof(m_NameBuf));
			if (ImGui::Button("Rename") && RenameSceneInProject(m_RenameTarget, m_NameBuf))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		// The one destructive, irreversible action in this panel -- unlike
		// everything else here, this gets its own confirmation even though
		// most editor actions don't ask before running.
		if (ImGui::BeginPopupModal("Delete Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Permanently delete '%s'? This cannot be undone.", m_DeleteTarget.c_str());
			if (ImGui::Button("Delete") && DeleteSceneFromProject(m_DeleteTarget))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	char m_NameBuf[128] = "";
	std::string m_DuplicateSource, m_RenameTarget, m_DeleteTarget;
	const char* m_PendingPopup = nullptr;
};
```

- [ ] **Step 2: Dock it into the shell's default layout**

In `TestEnv/src/EditorShell.h`, in the dock-builder function, change:

```cpp
		ImGui::DockBuilderDockWindow("Files", left);
		ImGui::DockBuilderDockWindow("Outliner", left);
```

to:

```cpp
		ImGui::DockBuilderDockWindow("Files", left);
		ImGui::DockBuilderDockWindow("Outliner", left);
		ImGui::DockBuilderDockWindow("Scenes", left);
```

- [ ] **Step 3: Push the layer**

In `TestEnv/src/TestApp.cpp`, add `#include "ScenesPanel.h"` next to
`#include "AppearancePanel.h"`, and add `PushLayer(new ScenesPanel());`
right after `PushLayer(new AppearancePanel());`.

- [ ] **Step 4: Build all three configs**

Run: `./gs.py build all`
Expected: clean build, all three configs.

- [ ] **Step 5: Visual capture -- panel with multiple scenes**

Add a temporary hook to `TestApp.cpp`'s `TestEnv()` constructor, right
after `PushLayer(new ScenesPanel());` (removed in Step 7 below):

```cpp
	// TEMPORARY -- Task 5 visual capture hook, removed right after.
	{
		CreateEditorProject(".claude_scenetest_visual", "VisualTest");
		CreateSceneInProject("Level1");
		CreateSceneInProject("Level2");
		ImGui::SetWindowFocus("Scenes");
	}
```

Delete any stale `imgui.ini` beside the executable, then from
`bin/Debug-linux-x86_64/TestEnv`:

```sh
rm -f imgui.ini
./TestEnv --hide-window --lockstep --capture <worktree>/.claude_scenes_panel.png --capture-step 60
```

View the resulting image. Expected: the "Scenes" tab is visible (grouped
with Files/Outliner), listing "Main" (highlighted, active), "Level1", and
"Level2".

- [ ] **Step 6: Visual capture -- unsaved-changes modal**

Extend the same temporary hook to also dirty the scene and trigger a
guarded swap, so the modal is open when the capture runs:

```cpp
	// TEMPORARY -- Task 5 visual capture hook, removed right after.
	{
		CreateEditorProject(".claude_scenetest_visual", "VisualTest");
		CreateSceneInProject("Level1");
		CreateSceneInProject("Level2");

		GS::TransformComponent transform;
		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("VisualTestEntity", transform, std::nullopt, std::nullopt, std::nullopt));

		SceneSwapGuard::RequestSceneSwap([]{ /* deferred; never runs during this capture */ });
	}
```

Run the same capture command again (a fresh `--capture-step`, since the
modal takes one frame to open via `s_ModalRequested`):

```sh
./TestEnv --hide-window --lockstep --capture <worktree>/.claude_scenes_modal.png --capture-step 60
```

View the resulting image. Expected: the "Unsaved Changes" modal is open,
showing Save/Discard/Cancel.

- [ ] **Step 7: Remove the temporary hook and scratch files**

Remove the temporary block from `TestApp.cpp`. Delete
`.claude_scenetest_visual/`, `.claude_scenes_panel.png`,
`.claude_scenes_modal.png`, and any stale `imgui.ini` the captures left
behind.

- [ ] **Step 8: Grep for leftover TEMPORARY markers and build all three configs**

Run: `grep -rn TEMPORARY TestEnv/src`
Expected: no output.

Run: `./gs.py build all`
Expected: clean build, all three configs.

- [ ] **Step 9: Commit**

```bash
git add TestEnv/src/ScenesPanel.h TestEnv/src/EditorShell.h TestEnv/src/TestApp.cpp
git commit -m "Add the Scenes panel"
```

---

## Final task: finish the branch

- [ ] Grep for `TEMPORARY` across `TestEnv/src` one more time (empty).
- [ ] `./gs.py build all` one more time on the final state.
- [ ] Invoke `superpowers:finishing-a-development-branch`.
