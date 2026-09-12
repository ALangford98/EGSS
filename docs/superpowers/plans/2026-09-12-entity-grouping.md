# Entity Grouping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the editor treat several entities as one named, persisted group
shown together in the Outliner; a transient Ctrl+click multi-selection in
the viewport or Outliner that drags together from one shared gizmo; Ctrl+G
to promote a multi-selection into a group; and a dialog that creates,
renames, deletes, and reassigns membership for every group in the scene.

**Architecture:** `GroupComponent{ std::string GroupName }` is a plain
component like `ScriptComponent`, serialized with the scene, carried through
Duplicate/Delete. A new `EntityGroups.h` holds pure, ImGui-free helpers over
`g_EditorScene` (group lookup, membership edits, centroid math) that both the
Outliner display and the management dialog call, and that the temporary
self-tests exercise directly. Multi-selection is a `std::vector<EntityId>`
alongside the existing single `m_Selected`; the gizmo's existing
`GizmoPosition()` indirection (already used by mesh authoring's point-drag)
grows a third case returning a centroid stand-in, so `UpdateGizmo`'s drag
math is unchanged and only what happens after a write differs.

**Tech Stack:** C++17, GLM, the existing `GS::Scene`/`EditorSceneView`/
`EditorHistory` editor code and ImGui panels. No new external dependency.

**Spec:** `docs/superpowers/specs/2026-09-12-entity-grouping-design.md`

## Global Constraints

- **Testing convention for this repo, not a generic framework:** every task
  that touches non-ImGui logic adds a *temporary* self-test header
  (`XyzTest.h`, marked `// TEMPORARY -- delete after verifying`), wires one
  `Run()` call into `TestEnv/src/TestApp.cpp`'s `TestEnv()` constructor,
  builds with `./gs.py build`, runs with `./gs.py run -- --hide-window
  --lockstep --capture <path> --capture-step N`, reads the `GS_TRACE`'d
  pass/fail lines from the command output, and then **deletes the test
  header and the two lines that wired it in** before moving on. There is no
  permanent test suite in this project — do not add one.
- **ImGui-only steps (dialog layout, context-menu wiring, Ctrl+click/Ctrl+G
  polling) are checked by building, running, and a visual capture where one
  adds real confidence — not by a self-test.** The live mouse/keyboard
  click-through itself stays the same disclosed, unautomatable gap already
  noted for every other editor UI feature in `docs/STATE.md` — no
  GUI-automation tool exists in this environment.
- **Verify at all three configs** (`./gs.py build all`) before considering
  any task's code changes final.
- **Never commit, never push.** This repo's own rule: the owner commits
  their own work. Each task's last step is "mark the task done", not `git
  commit` — leave all changes staged in the working tree.
- **Compute expected values by hand before asserting them**, per this
  project's established verification habit — every test below states the
  hand-derived expected number in the test's own check message.

---

### Task 1: `GroupComponent` + Save/Load serialization

**Files:**
- Modify: `GS/src/GS/Scene/Components.h:128-131` (right after `ScriptComponent`)
- Modify: `GS/src/GS/Scene/Scene.cpp:210-214` (`Scene::Save`, alongside the
  `script` block), `:319-323` (`Scene::Load`, the `else if (kind ==
  "script"...)` branch)
- Test: `TestEnv/src/GroupComponentTest.h` (temporary)

**Interfaces:**
- Produces: `struct GS::GroupComponent { std::string GroupName; };` — every
  later task reads/writes this exact field name.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/GroupComponentTest.h`:

```cpp
// TEMPORARY -- delete after verifying GroupComponent's Save/Load round trip.
#pragma once
#include <GS.h>
#include <filesystem>

#include "EditorProject.h"

namespace GroupComponentTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();

		GS::Entity a = g_EditorScene.CreateEntity("Barrel");
		a.Add<GS::GroupComponent>({ "Crates" });
		g_EditorScene.CreateEntity("Lonely");   // no group -- must serialize as if the component were never there

		std::string path = "group_component_test.scene";
		Check(g_EditorScene.Save(path), "Scene::Save succeeds");

		g_EditorScene.Clear();
		Check(g_EditorScene.Load(path), "Scene::Load succeeds");

		bool foundBarrel = false, foundLonely = false;
		for (GS::EntityId entity : g_EditorScene.GetEntities()) {
			auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
			auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity);
			if (tag && tag->Name == "Barrel") {
				foundBarrel = true;
				Check(group && group->GroupName == "Crates", "Barrel's GroupName round-trips as 'Crates'");
			}
			if (tag && tag->Name == "Lonely") {
				foundLonely = true;
				Check(group == nullptr, "an entity with no group loads back with no GroupComponent at all");
			}
		}
		Check(foundBarrel && foundLonely, "both entities survived the round trip");

		std::error_code ec;
		std::filesystem::remove(path, ec);
		g_EditorScene.Clear();

		GS_TRACE("GroupComponentTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it in: add `#include "GroupComponentTest.h"   // TEMPORARY` near the
other includes in `TestEnv/src/TestApp.cpp`, and
`GroupComponentTest::Run();   // TEMPORARY -- delete after verifying` as the
first line of `TestEnv()`'s constructor body.

- [ ] **Step 2: Build to confirm it fails**

Run: `./gs.py build`
Expected: FAIL — `GS::GroupComponent` doesn't exist yet, and `a.Add<GS::GroupComponent>` won't compile.

- [ ] **Step 3: Add `GroupComponent`**

In `GS/src/GS/Scene/Components.h`, right after the existing `ScriptComponent`
struct (ends at line 131):

```cpp
	// A named tag shared by every entity that carries the same GroupName --
	// there is no separate group entity or registry. An entity belongs to
	// at most one group; joining a new one overwrites the old value, it
	// doesn't add to it. See docs/superpowers/specs/2026-09-12-entity-
	// grouping-design.md for why this is a plain string rather than a
	// group entity or a set.
	struct GroupComponent
	{
		std::string GroupName;
	};
```

- [ ] **Step 4: Serialize it in `Scene::Save`**

In `GS/src/GS/Scene/Scene.cpp`, right after the existing `light` block
(ends at line 221, just before the closing `}` of the `for (EntityId
entity : m_Live)` loop):

```cpp
			// An entity with an empty GroupName was never really grouped --
			// silently dropped, same reasoning the mesh/script blocks above
			// already use for their own optional strings.
			if (GroupComponent* group = self->GetComponent<GroupComponent>(entity))
			{
				if (!group->GroupName.empty())
					out << "group " << group->GroupName << "\n";
			}
```

- [ ] **Step 5: Parse it in `Scene::Load`**

In `GS/src/GS/Scene/Scene.cpp`, right after the existing `else if (kind ==
"light" ...)` branch (ends at line 326, just before the closing `}` of the
`while (std::getline(in, line))` loop):

```cpp
			else if (kind == "group" && current)
			{
				std::string name;
				std::getline(fields, name);
				size_t from = name.find_first_not_of(' ');
				current.Add<GroupComponent>(GroupComponent{ from == std::string::npos ? "" : name.substr(from) });
			}
```

(`std::getline` on the remaining stream, then trimming the leading space —
the exact pattern the `tag` branch a few lines above already uses, since a
group name can contain spaces the way an entity's tag name can.)

- [ ] **Step 6: Build all three configs and run the test**

```sh
./gs.py build all
./gs.py run -- --hide-window --lockstep --capture /tmp/groupcomponent.png --capture-step 2
```
Expected: `GroupComponentTest: 5 passed, 0 failed` in the output (Save,
Load, Barrel's GroupName, Lonely's absent GroupComponent, both-found).

- [ ] **Step 7: Remove the temporary test**

Delete `TestEnv/src/GroupComponentTest.h` and the two lines in
`TestEnv/src/TestApp.cpp` that referenced it. Rebuild to confirm `TestApp.cpp`
still compiles clean.

- [ ] **Step 8: Mark task done**

No commit — leave the `Components.h`/`Scene.cpp` changes staged for the
owner to review.

---

### Task 2: `GroupComponent` carried through Duplicate/Delete

**Files:**
- Modify: `TestEnv/src/EditorHistory.h:106-163` (`DeleteEntityCommand`),
  `:176-224` (`DuplicateEntityCommand`)
- Test: `TestEnv/src/GroupCommandTest.h` (temporary)

**Interfaces:**
- Consumes: Task 1's `GS::GroupComponent`
- Produces: no new interface — both existing commands now also
  capture/restore `GroupComponent`, the exact pattern each already uses for
  `m_Physics`/`GS::PhysicsComponent`.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/GroupCommandTest.h`:

```cpp
// TEMPORARY -- delete after verifying Delete/Duplicate carry GroupComponent.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "EditorHistory.h"

namespace GroupCommandTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();
		EditorHistory::Clear();

		GS::Entity source = g_EditorScene.CreateEntity("Crate");
		source.Add<GS::GroupComponent>({ "Crates" });
		GS::EntityId sourceId = source.GetId();

		GS::EntityId dup = EditorHistory::Push(std::make_unique<DuplicateEntityCommand>(sourceId));
		if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(dup))
			Check(group->GroupName == "Crates", "Duplicate copies GroupComponent's GroupName");
		else
			Check(false, "Duplicate copies GroupComponent at all");

		GS::EntityId deleted = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(sourceId));
		EditorHistory::Undo();
		GS::EntityId restoredId = g_EditorScene.IsValid(sourceId) ? sourceId : GS::InvalidEntity;
		// sourceId's slot generation is unaffected by Delete's own Undo path
		// (it recreates through the same id-remap DeleteEntityCommand::Undo
		// already performs) -- resolve through the same history remap the
		// class itself uses rather than assuming sourceId is still current.
		GS::EntityId current = EditorHistory::Resolve(sourceId);
		if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(current))
			Check(group->GroupName == "Crates", "Undoing a Delete restores GroupComponent's GroupName");
		else
			Check(false, "Undoing a Delete restores GroupComponent at all");

		g_EditorScene.Clear();
		EditorHistory::Clear();

		GS_TRACE("GroupCommandTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Build to confirm it fails**

Run: `./gs.py build`
Expected: FAIL — `source.Add<GS::GroupComponent>` compiles (Task 1 added the
type), but the two `Check` calls both report `false`/missing, since neither
command captures it yet. (Confirm by running once before Step 3, same as
every other task's "see it fail for the right reason" check — here that
means both `Check`s print `FAIL`, not a build error.)

- [ ] **Step 3: `DeleteEntityCommand` captures/restores `GroupComponent`**

In `TestEnv/src/EditorHistory.h`, in `DeleteEntityCommand`'s constructor
(right after the existing `if (auto* physics = ...) m_Physics = *physics;`
around line 126):

```cpp
		if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
			m_Group = *group;
```

In its `Undo()` (right after the existing `if (m_Physics) entity.Add<GS::PhysicsComponent>(*m_Physics);` around line 151):

```cpp
		if (m_Group)
			entity.Add<GS::GroupComponent>(*m_Group);
```

Add the member (alongside the existing `std::optional<GS::PhysicsComponent> m_Physics;`):

```cpp
	std::optional<GS::GroupComponent> m_Group;
```

- [ ] **Step 4: `DuplicateEntityCommand` captures/restores `GroupComponent`**

Same three additions in `DuplicateEntityCommand` (constructor around line
194, `Redo()` around line 210, member declaration) — identical shape, just
in the second class.

- [ ] **Step 5: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/groupcommand.png --capture-step 2
```
Expected: `GroupCommandTest: 2 passed, 0 failed`.

- [ ] **Step 6: Remove the temporary test**

- [ ] **Step 7: Mark task done**

---

### Task 3: `EntityGroups.h` — pure group-membership and centroid helpers

**Files:**
- Create: `TestEnv/src/EntityGroups.h`
- Test: `TestEnv/src/EntityGroupsTest.h` (temporary)

**Interfaces:**
- Consumes: Task 1's `GS::GroupComponent`
- Produces: `std::map<std::string, std::vector<GS::EntityId>>
  EntityGroups::GroupEntitiesByName()`, `bool
  EntityGroups::AnyAlreadyGrouped(const std::vector<GS::EntityId>&)`, `void
  EntityGroups::AssignGroup(const std::vector<GS::EntityId>&, const
  std::string&)`, `void EntityGroups::RenameGroup(const std::string& oldName,
  const std::string& newName)`, `void EntityGroups::DeleteGroup(const
  std::string& name)`, `glm::vec3 EntityGroups::ComputeCentroid(const
  std::vector<glm::vec3>& positions)` — Tasks 4, 6, 7, 8 call these
  directly; none of them re-implement group scanning or centroid math.

This is the one file in this feature with no ImGui in it at all — pure
functions over `g_EditorScene`, the same "logic first, UI calls it" shape
`EditableMesh.h` uses for mesh authoring.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/EntityGroupsTest.h`:

```cpp
// TEMPORARY -- delete after verifying EntityGroups' helpers.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "EntityGroups.h"

namespace EntityGroupsTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();

		GS::Entity a = g_EditorScene.CreateEntity("A");
		a.Add<GS::GroupComponent>({ "Crates" });
		GS::Entity b = g_EditorScene.CreateEntity("B");
		b.Add<GS::GroupComponent>({ "Crates" });
		GS::Entity c = g_EditorScene.CreateEntity("C");   // ungrouped

		auto groups = EntityGroups::GroupEntitiesByName();
		Check(groups.size() == 1, "exactly 1 group exists (ungrouped C isn't one)");
		Check(groups.count("Crates") == 1 && groups["Crates"].size() == 2, "'Crates' has exactly A and B, 2 members");

		Check(!EntityGroups::AnyAlreadyGrouped({ c.GetId() }), "an ungrouped entity reports not-already-grouped");
		Check(EntityGroups::AnyAlreadyGrouped({ a.GetId(), c.GetId() }), "a mixed set reports already-grouped if any member is");

		EntityGroups::AssignGroup({ c.GetId() }, "Rocks");
		if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(c.GetId()))
			Check(group->GroupName == "Rocks", "AssignGroup adds a GroupComponent with the given name");
		else
			Check(false, "AssignGroup adds a GroupComponent at all");

		EntityGroups::RenameGroup("Crates", "Boxes");
		if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(a.GetId()))
			Check(group->GroupName == "Boxes", "RenameGroup rewrites every member's GroupName ('Crates' -> 'Boxes')");
		else
			Check(false, "RenameGroup leaves A's GroupComponent intact");

		EntityGroups::DeleteGroup("Boxes");
		Check(g_EditorScene.GetComponent<GS::GroupComponent>(a.GetId()) == nullptr, "DeleteGroup removes A's GroupComponent");
		Check(g_EditorScene.GetComponent<GS::GroupComponent>(b.GetId()) == nullptr, "DeleteGroup removes B's GroupComponent too");

		glm::vec3 centroid = EntityGroups::ComputeCentroid({ { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f }, { 1.0f, 3.0f, 0.0f } });
		Check(centroid == glm::vec3(1.0f, 1.0f, 0.0f), "ComputeCentroid: mean of (0,0,0),(2,0,0),(1,3,0) is exactly (1,1,0)");
		Check(EntityGroups::ComputeCentroid({}) == glm::vec3(0.0f), "ComputeCentroid of an empty set is (0,0,0), not a crash");

		g_EditorScene.Clear();

		GS_TRACE("EntityGroupsTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Wire in and build to confirm it fails**

Add the include and `EntityGroupsTest::Run();` call in `TestApp.cpp` (same
pattern as before), then `./gs.py build`.
Expected: FAIL — `EntityGroups.h: No such file or directory`.

- [ ] **Step 3: Write `EntityGroups.h`**

```cpp
#pragma once

// Pure, ImGui-free helpers over g_EditorScene for group membership and
// multi-selection centroid math -- kept separate from EditorSceneView.h so
// the logic here can be exercised by a temporary self-test without a live
// ImGui frame, the same separation EditableMesh.h uses for mesh authoring.
// See docs/superpowers/specs/2026-09-12-entity-grouping-design.md.

#include <GS.h>

#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

#include "EditorProject.h"

namespace EntityGroups {

	// Alphabetical by group name (std::map, not unordered) -- a reasonable
	// free default for display order, and irrelevant at editor-scene sizes.
	inline std::map<std::string, std::vector<GS::EntityId>> GroupEntitiesByName()
	{
		std::map<std::string, std::vector<GS::EntityId>> groups;
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (!group->GroupName.empty())
					groups[group->GroupName].push_back(entity);
		return groups;
	}

	inline bool AnyAlreadyGrouped(const std::vector<GS::EntityId>& entities)
	{
		for (GS::EntityId entity : entities)
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (!group->GroupName.empty())
					return true;
		return false;
	}

	// Overwrites, not adds to, any existing membership -- single-group-
	// membership rule from the spec applies uniformly, whether this is
	// called from Ctrl+G or the management dialog.
	inline void AssignGroup(const std::vector<GS::EntityId>& entities, const std::string& name)
	{
		for (GS::EntityId entity : entities)
			g_EditorScene.AddComponent<GS::GroupComponent>(entity, GS::GroupComponent{ name });
	}

	inline void RenameGroup(const std::string& oldName, const std::string& newName)
	{
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (group->GroupName == oldName)
					group->GroupName = newName;
	}

	inline void DeleteGroup(const std::string& name)
	{
		for (GS::EntityId entity : g_EditorScene.GetEntities())
			if (auto* group = g_EditorScene.GetComponent<GS::GroupComponent>(entity))
				if (group->GroupName == name)
					g_EditorScene.RemoveComponent<GS::GroupComponent>(entity);
	}

	inline glm::vec3 ComputeCentroid(const std::vector<glm::vec3>& positions)
	{
		if (positions.empty())
			return glm::vec3(0.0f);
		glm::vec3 sum(0.0f);
		for (const glm::vec3& p : positions)
			sum += p;
		return sum / (float)positions.size();
	}

}
```

(`RemoveComponent`/`AddComponent` calls while iterating `GetEntities()` are
safe here the same way the Outliner's existing Add/Remove Script menu items
already are: `GetEntities()` returns the scene's live-entity vector, which
only Create/DestroyEntity mutate — a component add/remove never touches it.)

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/entitygroups.png --capture-step 2
```
Expected: `EntityGroupsTest: 10 passed, 0 failed` (2 for
GroupEntitiesByName, 2 for AnyAlreadyGrouped, 1 AssignGroup, 1 RenameGroup,
2 DeleteGroup, 2 ComputeCentroid).

- [ ] **Step 5: Remove the temporary test**

Delete `EntityGroupsTest.h` and its two `TestApp.cpp` lines. `EntityGroups.h`
itself stays — it's the real deliverable.

- [ ] **Step 6: Mark task done**

---

### Task 4: Outliner grouped display + group header context menu

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h:211-236` (the Outliner's
  `ImGui::Begin("Outliner")` block)

**Interfaces:**
- Consumes: Task 3's `EntityGroups::GroupEntitiesByName()`
- Produces: a new private method `void DrawOutlinerRow(GS::EntityId entity,
  GS::EntityId& pendingDuplicate, GS::EntityId& pendingDelete)` — extracted
  from the existing per-row body so both the grouped and ungrouped render
  passes below call the same code. No self-test (pure ImGui layout); verify
  by building, running, and a capture.

- [ ] **Step 1: Extract `DrawOutlinerRow`**

Add this private method near `DrawEntityContextMenu` (`EditorSceneView.h`,
around line 562), built from the existing per-row body at lines 221-233:

```cpp
	void DrawOutlinerRow(GS::EntityId entity, GS::EntityId& pendingDuplicate, GS::EntityId& pendingDelete)
	{
		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
		if (!tag)
			return;

		ImGui::PushID((int)entity);
		ImGui::BeginDisabled(m_MeshEditActive);
		if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
			m_Selected = entity;

		if (ImGui::BeginPopupContextItem("row_context"))
		{
			DrawEntityContextMenu(entity, pendingDuplicate, pendingDelete);
			ImGui::EndPopup();
		}

		ImGui::EndDisabled();
		ImGui::PopID();
	}
```

(Multi-selection highlighting/Ctrl+click is added to this same method in
Task 5 — this step is a pure extraction with no behavior change, so it can
be verified on its own first.)

- [ ] **Step 2: Replace the Outliner's render body with two passes**

Replace lines 215-234 (the `for (GS::EntityId entity : g_EditorScene.GetEntities()) { ... }` loop) with:

```cpp
			auto groups = EntityGroups::GroupEntitiesByName();
			std::unordered_set<GS::EntityId> inAnyGroup;
			for (auto& [name, members] : groups)
				for (GS::EntityId member : members)
					inAnyGroup.insert(member);

			for (auto& [name, members] : groups)
			{
				ImGui::PushID(name.c_str());
				bool open = ImGui::TreeNode(name.c_str());

				if (ImGui::BeginPopupContextItem("group_context"))
				{
					if (ImGui::MenuItem("Select All"))
					{
						m_MultiSelection = members;
						if (!members.empty())
							m_Selected = members.back();
					}
					if (ImGui::MenuItem("Ungroup"))
						EntityGroups::DeleteGroup(name);
					ImGui::EndPopup();
				}

				if (open)
				{
					for (GS::EntityId entity : members)
						DrawOutlinerRow(entity, pendingDuplicate, pendingDelete);
					ImGui::TreePop();
				}
				ImGui::PopID();
			}

			for (GS::EntityId entity : g_EditorScene.GetEntities())
				if (!inAnyGroup.count(entity))
					DrawOutlinerRow(entity, pendingDuplicate, pendingDelete);
```

Add `#include "EntityGroups.h"` and `#include <unordered_set>` to
`EditorSceneView.h`'s existing include block. Add the member (see Task 5 for
where it's actually used — declaring it now avoids a second pass over this
same file):

```cpp
	std::vector<GS::EntityId> m_MultiSelection;
```

placed alongside `GS::EntityId m_Hovered = GS::InvalidEntity;` (line 1493).

- [ ] **Step 3: Build all three configs**

Run: `./gs.py build all`
Expected: clean build, no errors.

- [ ] **Step 4: Visual capture**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/outliner_groups.png --capture-step 30
```
Open a project/scene with at least one grouped and one ungrouped entity
first (or place a couple of primitives, right-click isn't wired to "Add to
Group" until Task 8 — for this check, an entity can be given a
`GroupComponent` by temporarily calling `EntityGroups::AssignGroup` from
the same kind of one-off hook mesh authoring's own UI capture used, deleted
right after). Confirm the capture shows a collapsible group header
containing the grouped entity, and the ungrouped entity as a plain row
below it — then delete the temporary hook.

- [ ] **Step 5: Mark task done**

---

### Task 5: Multi-selection — Ctrl+click and highlighting

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h:143-166` (`OnEvent`'s
  `MouseButtonPressedEvent` dispatcher), the `DrawOutlinerRow` method added
  in Task 4, `:969-1001` (`DrawSelectionBox`)

**Interfaces:**
- Consumes: Task 3's nothing new (uses `EntityGroups::ComputeCentroid`
  indirectly via Task 6, not this task); Task 4's `m_MultiSelection` member
- Produces: `void ToggleMultiSelect(GS::EntityId entity)` — Task 6's Ctrl+G
  and Task 6's gizmo both read `m_MultiSelection` this task populates.

- [ ] **Step 1: Add `ToggleMultiSelect` and a Ctrl-held helper**

Add near `DrawOutlinerRow`:

```cpp
	static bool CtrlHeld()
	{
		return GS::Input::IsKeyPressed(GS_KEY_LEFT_CONTROL) || GS::Input::IsKeyPressed(GS_KEY_RIGHT_CONTROL);
	}

	// Toggling rather than always-add is what makes Ctrl+click a real
	// multi-select (click a selected member again to drop it), not just an
	// accumulator. m_Selected always follows the last entity touched, so
	// the Inspector keeps showing something sensible even with several
	// entities in m_MultiSelection.
	void ToggleMultiSelect(GS::EntityId entity)
	{
		auto it = std::find(m_MultiSelection.begin(), m_MultiSelection.end(), entity);
		if (it != m_MultiSelection.end())
			m_MultiSelection.erase(it);
		else
			m_MultiSelection.push_back(entity);
		m_Selected = entity;
	}
```

Add `#include <algorithm>` to `EditorSceneView.h` if not already present
(it is, via existing `std::find` use elsewhere in this file).

- [ ] **Step 2: Wire Ctrl+click into the viewport's left-click handler**

In `OnEvent`'s `MouseButtonPressedEvent` dispatcher (line 143), replace:

```cpp
			if (e.GetMouseButton() == GS_MOUSE_BUTTON_LEFT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0 && !m_MeshEditActive)
				m_Selected = m_Hovered;
```

with:

```cpp
			if (e.GetMouseButton() == GS_MOUSE_BUTTON_LEFT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0 && !m_MeshEditActive)
			{
				if (CtrlHeld())
				{
					if (g_EditorScene.IsValid(m_Hovered))
						ToggleMultiSelect(m_Hovered);
				}
				else
				{
					m_MultiSelection.clear();
					m_Selected = m_Hovered;
				}
			}
```

- [ ] **Step 3: Wire Ctrl+click into `DrawOutlinerRow`**

In the `DrawOutlinerRow` method added in Task 4, replace:

```cpp
		if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
			m_Selected = entity;
```

with:

```cpp
		bool isSelected = m_MultiSelection.empty()
			? (entity == m_Selected)
			: (std::find(m_MultiSelection.begin(), m_MultiSelection.end(), entity) != m_MultiSelection.end());

		if (ImGui::Selectable(tag->Name.c_str(), isSelected))
		{
			if (CtrlHeld())
				ToggleMultiSelect(entity);
			else
			{
				m_MultiSelection.clear();
				m_Selected = entity;
			}
		}
```

- [ ] **Step 4: Validity pruning**

At the top of `OnImGuiRender` (right after the `if (!IsActive()) return;`
guard), add:

```cpp
		// An entity Ctrl-selected earlier can be deleted from under the
		// multi-selection (Delete key, or another session action) --
		// pruned once per frame, the same defensive shape
		// ValidateMeshEditSession already applies to m_MeshEditEntity.
		m_MultiSelection.erase(
			std::remove_if(m_MultiSelection.begin(), m_MultiSelection.end(),
				[](GS::EntityId e) { return !g_EditorScene.IsValid(e); }),
			m_MultiSelection.end());
```

- [ ] **Step 5: Highlight every multi-selected entity in the viewport**

In `DrawSelectionBox` (line 969), the existing two-pass loop only ever draws
`m_Hovered` and `m_Selected`. Replace the whole method body with a version
that also draws every `m_MultiSelection` entry when there are 2+:

```cpp
	void DrawSelectionBox()
	{
		auto drawBoxFor = [this](GS::EntityId entity, const glm::vec4& color)
		{
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
			if (!transform || !mesh || !mesh->Geometry)
				return;

			glm::vec3 lo = mesh->Geometry->GetBoundsMin();
			glm::vec3 hi = mesh->Geometry->GetBoundsMax();
			glm::mat4 model = transform->GetTransform();

			glm::vec3 corner[8];
			for (int i = 0; i < 8; i++)
				corner[i] = glm::vec3(model * glm::vec4(
					(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z, 1.0f));

			static const int edges[12][2] = {
				{0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7}
			};
			for (auto& edge : edges)
				GS::Renderer2D::DrawLine(corner[edge[0]], corner[edge[1]], color);
		};

		const glm::vec4 hoverColor(0.45f, 0.85f, 1.0f, 1.0f);
		const glm::vec4 selectedColor(1.00f, 0.85f, 0.3f, 1.0f);

		if (g_EditorScene.IsValid(m_Hovered) && m_Hovered != m_Selected)
			drawBoxFor(m_Hovered, hoverColor);

		if (m_MultiSelection.size() > 1)
		{
			for (GS::EntityId entity : m_MultiSelection)
				drawBoxFor(entity, selectedColor);
		}
		else if (g_EditorScene.IsValid(m_Selected))
			drawBoxFor(m_Selected, selectedColor);
	}
```

(This is the same drawing logic as before, factored into a lambda so the
0/1-selection path and the multi-selection path share it instead of
duplicating the corner/edge math — behavior for 0 or 1 selected entities is
identical to today.)

- [ ] **Step 6: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 7: Visual capture**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/multiselect.png --capture-step 30
```
Since Ctrl+click itself can't be scripted (no GUI-automation tool), verify
by temporarily setting `m_MultiSelection` to 2+ known entities via a one-off
hook in the same spot Task 4's capture used, confirm both draw the yellow
selection box, then remove the hook.

- [ ] **Step 8: Mark task done**

---

### Task 6: Multi-select gizmo + `MultiMoveCommand`

**Files:**
- Modify: `TestEnv/src/EditorHistory.h` (new `MultiMoveCommand`)
- Modify: `TestEnv/src/EditorSceneView.h:1181-1188` (`GizmoPosition`),
  `:1300-1399` (`UpdateGizmo`)
- Test: `TestEnv/src/MultiMoveCommandTest.h` (temporary)

**Interfaces:**
- Consumes: Task 5's `m_MultiSelection`
- Produces: `class MultiMoveCommand : public EditorCommand` (constructed
  with `std::vector<GS::EntityId>`, before-positions, after-positions) —
  used only by `UpdateGizmo` in this task, but a standalone, independently
  testable command like every other one in `EditorHistory.h`.

- [ ] **Step 1: Write the failing test for `MultiMoveCommand`**

Create `TestEnv/src/MultiMoveCommandTest.h`:

```cpp
// TEMPORARY -- delete after verifying MultiMoveCommand.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "EditorHistory.h"

namespace MultiMoveCommandTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();
		EditorHistory::Clear();

		GS::Entity a = g_EditorScene.CreateEntity("A");
		a.Get<GS::TransformComponent>()->Position = { 0.0f, 0.0f, 0.0f };
		GS::Entity b = g_EditorScene.CreateEntity("B");
		b.Get<GS::TransformComponent>()->Position = { 10.0f, 0.0f, 0.0f };

		std::vector<GS::EntityId> entities = { a.GetId(), b.GetId() };
		std::vector<glm::vec3> before = { { 0.0f, 0.0f, 0.0f }, { 10.0f, 0.0f, 0.0f } };
		std::vector<glm::vec3> after = { { 5.0f, 0.0f, 0.0f }, { 15.0f, 0.0f, 0.0f } };   // both shifted by the same (5,0,0)

		EditorHistory::Push(std::make_unique<MultiMoveCommand>(entities, before, after));
		Check(a.Get<GS::TransformComponent>()->Position == glm::vec3(5.0f, 0.0f, 0.0f), "Redo (via Push) applies the delta to A");
		Check(b.Get<GS::TransformComponent>()->Position == glm::vec3(15.0f, 0.0f, 0.0f), "Redo (via Push) applies the same delta to B");

		EditorHistory::Undo();
		Check(a.Get<GS::TransformComponent>()->Position == glm::vec3(0.0f, 0.0f, 0.0f), "Undo puts A back exactly");
		Check(b.Get<GS::TransformComponent>()->Position == glm::vec3(10.0f, 0.0f, 0.0f), "Undo puts B back exactly");

		EditorHistory::Redo();
		Check(a.Get<GS::TransformComponent>()->Position == glm::vec3(5.0f, 0.0f, 0.0f), "Redo re-applies to A");
		Check(b.Get<GS::TransformComponent>()->Position == glm::vec3(15.0f, 0.0f, 0.0f), "Redo re-applies to B");

		g_EditorScene.Clear();
		EditorHistory::Clear();

		GS_TRACE("MultiMoveCommandTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Wire in and build to confirm it fails**

Expected: FAIL to compile — `MultiMoveCommand` doesn't exist yet.

- [ ] **Step 3: Add `MultiMoveCommand` to `EditorHistory.h`**

Add after `DuplicateEntityCommand`'s closing `};`:

```cpp
// The multi-select gizmo's equivalent of EditFieldCommand<TransformComponent,
// glm::vec3>, generalized to N entities so one Ctrl+Z reverses a whole
// group-drag gesture at once rather than one entity at a time. Does not
// override SelectionAfter() -- a multi-move never changes what's selected,
// unlike Place/Delete/Duplicate, so the base class's "no entity of my own"
// default (returns GS::InvalidEntity) is correct as-is.
class MultiMoveCommand : public EditorCommand
{
public:
	MultiMoveCommand(std::vector<GS::EntityId> entities, std::vector<glm::vec3> before, std::vector<glm::vec3> after)
		: m_Entities(std::move(entities)), m_Before(std::move(before)), m_After(std::move(after))
	{
	}

	void Redo() override { Apply(m_After); }
	void Undo() override { Apply(m_Before); }
private:
	void Apply(const std::vector<glm::vec3>& positions)
	{
		for (size_t i = 0; i < m_Entities.size(); i++)
			if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Entities[i]))
				transform->Position = positions[i];
	}

	std::vector<GS::EntityId> m_Entities;
	std::vector<glm::vec3> m_Before;
	std::vector<glm::vec3> m_After;
};
```

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/multimove.png --capture-step 2
```
Expected: `MultiMoveCommandTest: 6 passed, 0 failed` (A and B checked after
Redo-via-Push, after Undo, and after Redo -- 2 checks x 3 = 6).

- [ ] **Step 5: Remove the temporary test**

- [ ] **Step 6: Wire the gizmo itself — `GizmoPosition`**

In `EditorSceneView.h`, `GizmoPosition()` (line 1181), add the new case
before the existing single-entity one:

```cpp
	glm::vec3* GizmoPosition()
	{
		if (m_MeshEditActive && m_MeshEditSelectedPoint >= 0)
			return &m_MeshEditSessionWorldPoint;

		if (m_MultiSelection.size() > 1)
		{
			std::vector<glm::vec3> positions;
			for (GS::EntityId entity : m_MultiSelection)
				if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
					positions.push_back(transform->Position);
			m_MultiSelectionPivot = EntityGroups::ComputeCentroid(positions);
			return &m_MultiSelectionPivot;
		}

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		return transform ? &transform->Position : nullptr;
	}
```

Add the member (alongside `m_MultiSelection`):

```cpp
	glm::vec3 m_MultiSelectionPivot{ 0.0f };
	std::vector<std::pair<GS::EntityId, glm::vec3>> m_MultiSelectionStartPositions;
```

- [ ] **Step 7: Capture per-member start positions when a pivot drag begins**

In `UpdateGizmo` (line 1300), the drag-start branch (`if (m_DragAxis < 0)
{ ... m_DragStartPosition = *target; return; }` around line 1381) — add
right after `m_DragStartPosition = *target;`:

```cpp
			if (m_MultiSelection.size() > 1)
			{
				m_MultiSelectionStartPositions.clear();
				for (GS::EntityId entity : m_MultiSelection)
					if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
						m_MultiSelectionStartPositions.push_back({ entity, memberTransform->Position });
			}
```

- [ ] **Step 8: Apply the shared delta to every member while dragging**

Still in `UpdateGizmo`, the final block (line 1396-1398:
`*target = m_DragStartPosition + axisDirection * (t - m_DragStartT);`) —
replace with:

```cpp
		glm::vec3 newPivot = m_DragStartPosition + axisDirection * (t - m_DragStartT);
		*target = newPivot;

		if (m_MultiSelection.size() > 1)
		{
			glm::vec3 delta = newPivot - m_DragStartPosition;
			for (auto& [entity, startPos] : m_MultiSelectionStartPositions)
				if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
					memberTransform->Position = startPos + delta;
		}
```

- [ ] **Step 9: Push `MultiMoveCommand` on drag release**

In `UpdateGizmo`'s `if (!down) { ... }` branch (line 1323), replace the
existing single-entity push:

```cpp
			if (m_DragAxis >= 0 && !m_MeshEditActive)
			{
				if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
					if (transform->Position != m_DragStartPosition)
						EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
							m_Selected, &GS::TransformComponent::Position, m_DragStartPosition, transform->Position));
			}
```

with:

```cpp
			if (m_DragAxis >= 0 && !m_MeshEditActive)
			{
				if (m_MultiSelection.size() > 1)
				{
					std::vector<GS::EntityId> entities;
					std::vector<glm::vec3> before, after;
					bool anyMoved = false;
					for (auto& [entity, startPos] : m_MultiSelectionStartPositions)
						if (auto* memberTransform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
						{
							entities.push_back(entity);
							before.push_back(startPos);
							after.push_back(memberTransform->Position);
							if (memberTransform->Position != startPos)
								anyMoved = true;
						}
					if (anyMoved)
						EditorHistory::Push(std::make_unique<MultiMoveCommand>(entities, before, after));
				}
				else if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
					if (transform->Position != m_DragStartPosition)
						EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
							m_Selected, &GS::TransformComponent::Position, m_DragStartPosition, transform->Position));
			}
```

- [ ] **Step 10: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 11: Byte-identical Cube3D regression check**

This task touches `GizmoPosition`/`UpdateGizmo`, both reachable from
non-editor demos only indirectly (they're `EditorSceneView`-only methods) —
Cube3D itself never calls them, so no regression capture is needed here
unlike Task 1's `Mesh::Create*` change. Skip.

- [ ] **Step 12: Visual capture**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/multigizmo.png --capture-step 30
```
Using the same one-off `m_MultiSelection` hook as Task 5's capture, confirm
the gizmo renders at the midpoint between the two selected entities rather
than at either one individually, then remove the hook.

- [ ] **Step 13: Mark task done**

---

### Task 7: Ctrl+G — promote a multi-selection to a group

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h:168-175` (`OnEvent`'s
  `KeyPressedEvent` dispatcher), `OnImGuiRender` (new popup handling near
  the top, alongside the existing viewport-context-menu popup block)

**Interfaces:**
- Consumes: Task 3's `EntityGroups::AnyAlreadyGrouped`/`AssignGroup`, Task
  5's `m_MultiSelection`
- Produces: no new interface for later tasks — this is a leaf feature.

- [ ] **Step 1: Add member state**

Alongside `m_ViewportContextEntity` (line 1505):

```cpp
	bool m_GroupPromptRequested = false;
	bool m_GroupPromptNeedsConfirm = false;
	char m_GroupNameBuf[256] = {};
```

- [ ] **Step 2: Poll Ctrl+G in the `KeyPressedEvent` dispatcher**

In `OnEvent` (line 168), add an `else if` after the existing
`GS_KEY_DELETE` branch:

```cpp
			else if (e.GetKeyCode() == GS_KEY_G && CtrlHeld() && m_MultiSelection.size() >= 2)
			{
				m_GroupPromptRequested = true;
				m_GroupPromptNeedsConfirm = EntityGroups::AnyAlreadyGrouped(m_MultiSelection);
			}
```

(`e.GetRepeatCount() > 0` already returns early at the top of this
dispatcher, so a held Ctrl+G doesn't fire repeatedly.)

- [ ] **Step 3: Open and draw the two popups in `OnImGuiRender`**

Add right after the existing viewport-context-menu block (the one guarding
`m_ViewportContextRequested`):

```cpp
		if (m_GroupPromptRequested)
		{
			ImGui::OpenPopup(m_GroupPromptNeedsConfirm ? "Confirm Regroup" : "New Group");
			if (!m_GroupPromptNeedsConfirm)
				m_GroupNameBuf[0] = '\0';
			m_GroupPromptRequested = false;
		}

		if (ImGui::BeginPopupModal("Confirm Regroup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Some of these entities already belong to a group.");
			ImGui::Text("Move them into a new group?");
			if (ImGui::Button("Move Them"))
			{
				m_GroupNameBuf[0] = '\0';
				ImGui::CloseCurrentPopup();
				ImGui::OpenPopup("New Group");
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("New Group", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_GroupNameBuf, sizeof(m_GroupNameBuf));
			bool canCreate = m_GroupNameBuf[0] != '\0';
			if (!canCreate) ImGui::BeginDisabled();
			if (ImGui::Button("Create"))
			{
				EntityGroups::AssignGroup(m_MultiSelection, m_GroupNameBuf);
				ImGui::CloseCurrentPopup();
			}
			if (!canCreate) ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
```

(Group assignment via Ctrl+G is not pushed onto `EditorHistory` — the same,
already-accepted precedent as "Add Script"/"Add Physics", neither of which
is undoable either, per the spec's own note on this.)

- [ ] **Step 4: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 5: Visual capture**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/ctrlg.png --capture-step 30
```
Using the same one-off multi-selection hook, additionally force
`m_GroupPromptRequested = true;` once to confirm the "New Group" popup
renders with a working text field and buttons, then remove the hook. The
actual Ctrl+G keypress-through stays the disclosed, unautomatable gap.

- [ ] **Step 6: Mark task done**

---

### Task 8: "Add to Group" + "Manage Groups" dialog

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h` (`DrawEntityContextMenu`, around
  line 562; new `DrawManageGroupsDialog` method; `OnImGuiRender`'s popup
  handling, alongside Task 7's)

**Interfaces:**
- Consumes: Task 3's `EntityGroups::GroupEntitiesByName`/`RenameGroup`/
  `DeleteGroup`
- Produces: no new interface — this is the feature's last task.

- [ ] **Step 1: Add member state**

```cpp
	bool m_GroupDialogRequested = false;
	GS::EntityId m_GroupDialogEntity = GS::InvalidEntity;
	std::string m_RenamingGroup;   // empty = nothing being renamed right now
	char m_GroupRenameBuf[256] = {};
	char m_NewGroupBuf[256] = {};
```

- [ ] **Step 2: Add "Add to Group" to `DrawEntityContextMenu`**

In `DrawEntityContextMenu` (around line 562), right after the existing
`if (ImGui::MenuItem("Delete")) pendingDelete = entity;` line:

```cpp
		if (ImGui::MenuItem("Add to Group"))
		{
			m_GroupDialogEntity = entity;
			m_GroupDialogRequested = true;
		}
```

- [ ] **Step 3: Add `DrawManageGroupsDialog`**

Add as a new private method, near `DrawEntityContextMenu`:

```cpp
	void DrawManageGroupsDialog()
	{
		auto groups = EntityGroups::GroupEntitiesByName();

		for (auto& [name, members] : groups)
		{
			ImGui::PushID(name.c_str());
			if (ImGui::CollapsingHeader(name.c_str()))
			{
				if (m_RenamingGroup == name)
				{
					ImGui::SetNextItemWidth(150.0f);
					if (ImGui::InputText("##rename_group", m_GroupRenameBuf, sizeof(m_GroupRenameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
					{
						EntityGroups::RenameGroup(name, m_GroupRenameBuf);
						m_RenamingGroup.clear();
					}
				}
				else if (ImGui::Button("Rename"))
				{
					m_RenamingGroup = name;
					strncpy(m_GroupRenameBuf, name.c_str(), sizeof(m_GroupRenameBuf) - 1);
					m_GroupRenameBuf[sizeof(m_GroupRenameBuf) - 1] = '\0';
				}
				ImGui::SameLine();
				if (ImGui::Button("Delete Group"))
					EntityGroups::DeleteGroup(name);

				for (GS::EntityId member : members)
				{
					auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(member);
					ImGui::PushID((int)member);
					ImGui::BulletText("%s", tag ? tag->Name.c_str() : "?");
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove"))
						g_EditorScene.RemoveComponent<GS::GroupComponent>(member);
					ImGui::PopID();
				}

				ImGui::SetNextItemWidth(200.0f);
				if (ImGui::BeginCombo("##add_to_group", "Add entity..."))
				{
					for (GS::EntityId candidate : g_EditorScene.GetEntities())
					{
						if (std::find(members.begin(), members.end(), candidate) != members.end())
							continue;
						auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(candidate);
						if (!tag)
							continue;
						if (ImGui::Selectable(tag->Name.c_str()))
							g_EditorScene.AddComponent<GS::GroupComponent>(candidate, GS::GroupComponent{ name });
					}
					ImGui::EndCombo();
				}
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("##new_group_name", m_NewGroupBuf, sizeof(m_NewGroupBuf));
		ImGui::SameLine();
		bool canCreate = m_NewGroupBuf[0] != '\0' && g_EditorScene.IsValid(m_GroupDialogEntity);
		if (!canCreate) ImGui::BeginDisabled();
		if (ImGui::Button("Create New Group"))
		{
			g_EditorScene.AddComponent<GS::GroupComponent>(m_GroupDialogEntity, GS::GroupComponent{ m_NewGroupBuf });
			m_NewGroupBuf[0] = '\0';
		}
		if (!canCreate) ImGui::EndDisabled();
	}
```

(Every action here — rename, delete, add, remove — runs immediately with no
confirmation, per the spec: this dialog is already an explicit, deliberate
action a user opened on purpose, unlike the fast Ctrl+G shortcut Task 7
gates with a confirm step.)

- [ ] **Step 4: Open the dialog from `OnImGuiRender`**

Add alongside Task 7's popup handling:

```cpp
		if (m_GroupDialogRequested)
		{
			ImGui::OpenPopup("Manage Groups");
			m_GroupDialogRequested = false;
		}
		if (ImGui::BeginPopupModal("Manage Groups", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			DrawManageGroupsDialog();
			ImGui::Separator();
			if (ImGui::Button("Close"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
```

- [ ] **Step 5: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 6: Visual capture**

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/managegroups.png --capture-step 30
```
Using a one-off hook that sets up 1-2 groups (via `EntityGroups::AssignGroup`)
and forces `m_GroupDialogRequested = true; m_GroupDialogEntity = <some
entity>;` once, confirm the dialog renders each group's header, its members
with Remove buttons, the Rename/Delete Group buttons, and the New Group
field — then remove the hook.

- [ ] **Step 7: Mark task done**

---

## Plan self-review notes

- **Spec coverage:** `GroupComponent` + serialization (Task 1); carried
  through Duplicate/Delete (Task 2); grouped Outliner display + group
  header Select All/Ungroup (Task 4); Ctrl+click multi-selection +
  highlighting (Task 5); translate-only centroid gizmo + undo (Task 6);
  Ctrl+G with confirm-if-already-grouped (Task 7); "Add to Group" +
  full-management dialog (Task 8). Every in-scope bullet from the spec has
  a task. Explicitly-deferred items (nested groups, rigid rotate/scale,
  multi-entity Inspector editing, Outliner drag-and-drop) have no task, on
  purpose.
- **Type consistency checked:** `GS::GroupComponent{ std::string GroupName
  }` (Task 1) is the exact type every later task's `GetComponent<GS::
  GroupComponent>()`/`Add<GS::GroupComponent>()` call uses.
  `EntityGroups::GroupEntitiesByName()`'s return type (`std::map<std::
  string, std::vector<GS::EntityId>>`) matches every structured binding
  (`for (auto& [name, members] : groups)`) used against it in Tasks 4 and
  8. `m_MultiSelection` (`std::vector<GS::EntityId>`, declared in Task 4,
  populated in Task 5) is the exact member every later task reads.
  `MultiMoveCommand`'s constructor signature (Task 6) matches its one call
  site in `UpdateGizmo`.
