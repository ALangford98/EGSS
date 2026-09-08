# Editor Menu Bar and Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the editor a File/Edit/View/Help menu bar backed by real behavior (a named Project concept, minimal undo/redo, named layout presets), and reshape the docking layout into the conventional tools-left/properties-right/bottom-tabs arrangement so later sub-projects (terminal, text editor, scripting) have an obvious place to land.

**Architecture:** A new `EditorMenuBar.h` owns the menu bar and its three behaviors (project I/O, undo/redo, layout presets). A new `EditorHistory.h` owns a small command-stack that every scene-mutating control now goes through instead of mutating `g_EditorScene` directly. `EditorProject.h` gains a Project manifest wrapping today's single scene. `EditorShell.h` loses the controls that migrated to the menu bar and gains the reshuffled layout.

**Tech Stack:** C++17, Dear ImGui (docking branch, already vendored), the existing hand-rolled `GS::Scene` ECS, `std::filesystem` (already used elsewhere in this codebase for path handling).

**Spec:** `docs/superpowers/specs/2026-09-08-editor-menu-layout-design.md`

## Global Constraints

- Every new/changed piece of behavior gets a temporary self-test per this project's convention (`TestEnv/src/*Test.h`, called once from `TestApp`'s constructor, deleted once verified) — this project has no test framework and does not want one.
- `git grep TEMPORARY` must return nothing before the final task is considered done.
- Verify with `./gs.py build all` before the final task — Release has caught things Debug has not, elsewhere in this project.
- No existing demo's behavior, capture, or replay may change.
- Engine namespace/macros are `GS::`/`GS_*`. Never reintroduce `Egss`/`EGSS`.
- Comments explain *why*, not what, matching the surrounding density.
- This repo's CLAUDE.md forbids committing to `main` or pushing — every task stages its work (`git add -A`) and leaves it there. Do not run `git commit`.

---

## Task 1: EditorHistory — the undo/redo command stack

**Files:**
- Create: `TestEnv/src/EditorHistory.h`
- Create (temporary): `TestEnv/src/EditorHistoryTest.h`
- Modify (temporary call site, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `GS::Scene` (`g_EditorScene`, `TestEnv/src/EditorProject.h`), `GS::TagComponent`/`TransformComponent`/`MeshComponent`/`CameraComponent` (`GS/src/GS/Scene/Components.h`)
- Produces: `class EditorCommand` (base, with a protected `GS::EntityId m_Entity` and a public `GS::EntityId SelectionAfter() const`), `class PlaceEntityCommand`, `class DeleteEntityCommand`, `template<typename Component, typename Field> class EditFieldCommand`, and the `EditorHistory` namespace: `Push(std::unique_ptr<EditorCommand>) -> GS::EntityId`, `Undo() -> GS::EntityId`, `Redo() -> GS::EntityId`, `CanUndo() -> bool`, `CanRedo() -> bool`, `Clear()`.

This file deliberately does **not** include `EditorSceneView.h`. `Push`/`Undo`/`Redo` return the `GS::EntityId` that selection should become; the caller (which already knows about `EditorSceneView`) applies it. This is what keeps `EditorHistory.h` → `EditorProject.h` → `GS.h` acyclic, with `EditorSceneView.h`/`EditorShell.h`/`EditorMenuBar.h` depending on `EditorHistory.h` and not the other way around.

- [ ] **Step 1: Write `EditorHistory.h`**

```cpp
#pragma once

// A small command-history stack for the editor's own actions -- placing an
// entity, deleting one, editing a field in the Inspector. Not a
// general-purpose ECS transaction system: it covers what the editor itself
// does, and nothing a future system (mesh authoring, scripting) should
// build its own transactions on without a reason to widen this.
//
// Every command exposes SelectionAfter() rather than reaching into
// EditorSceneView to set the selection itself -- that would require this
// file to include EditorSceneView.h, and EditorSceneView.h needs to include
// *this* file for its Inspector edits to push commands. Returning the
// answer and letting the caller apply it keeps the dependency one-way.

#include <GS.h>
#include <optional>

#include "EditorProject.h"

class EditorCommand
{
public:
	virtual ~EditorCommand() = default;
	virtual void Undo() = 0;
	virtual void Redo() = 0;

	// Whichever entity this command concerns, if it currently exists --
	// GS::InvalidEntity otherwise. Implemented once here rather than by
	// each derived command, because "is my entity valid right now" is the
	// same question after every Undo/Redo regardless of what kind of
	// command it was: a Place command's entity stops existing after Undo,
	// a Delete command's entity starts existing again after Undo, and an
	// Edit command's entity was never destroyed either way.
	GS::EntityId SelectionAfter() const
	{
		return g_EditorScene.IsValid(m_Entity) ? m_Entity : GS::InvalidEntity;
	}
protected:
	GS::EntityId m_Entity = GS::InvalidEntity;
};

// Creates an entity with the given components. Undo destroys it; Redo
// creates it again. A fresh CreateEntity call every time rather than
// reusing an id -- GS::Scene's ids are index+generation, and a destroyed
// slot's next occupant always gets a bumped generation, so "the same
// entity" after a Redo is a different id from before the matching Undo.
class PlaceEntityCommand : public EditorCommand
{
public:
	PlaceEntityCommand(const std::string& name, const GS::TransformComponent& transform,
		const std::optional<GS::MeshComponent>& mesh, const std::optional<GS::CameraComponent>& camera)
		: m_Name(name), m_Transform(transform), m_Mesh(mesh), m_Camera(camera)
	{
	}

	void Redo() override
	{
		GS::Entity entity = g_EditorScene.CreateEntity(m_Name);
		*entity.Get<GS::TransformComponent>() = m_Transform;
		if (m_Mesh)
			entity.Add<GS::MeshComponent>(*m_Mesh);
		if (m_Camera)
			entity.Add<GS::CameraComponent>(*m_Camera);
		m_Entity = entity.GetId();
	}

	void Undo() override
	{
		if (g_EditorScene.IsValid(m_Entity))
			g_EditorScene.DestroyEntity(m_Entity);
	}
private:
	std::string m_Name;
	GS::TransformComponent m_Transform;
	std::optional<GS::MeshComponent> m_Mesh;
	std::optional<GS::CameraComponent> m_Camera;
};

// The inverse of Place: captures an existing entity's data at construction,
// destroys it on Redo, recreates it from the captured data on Undo.
class DeleteEntityCommand : public EditorCommand
{
public:
	explicit DeleteEntityCommand(GS::EntityId entity)
	{
		m_Entity = entity;

		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity))
			m_Name = tag->Name;
		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity))
			m_Transform = *transform;
		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity))
			m_Mesh = *mesh;
		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(entity))
			m_Camera = *camera;
	}

	void Redo() override
	{
		if (g_EditorScene.IsValid(m_Entity))
			g_EditorScene.DestroyEntity(m_Entity);
	}

	void Undo() override
	{
		GS::Entity entity = g_EditorScene.CreateEntity(m_Name);
		*entity.Get<GS::TransformComponent>() = m_Transform;
		if (m_Mesh)
			entity.Add<GS::MeshComponent>(*m_Mesh);
		if (m_Camera)
			entity.Add<GS::CameraComponent>(*m_Camera);
		m_Entity = entity.GetId();
	}
private:
	std::string m_Name;
	GS::TransformComponent m_Transform;
	std::optional<GS::MeshComponent> m_Mesh;
	std::optional<GS::CameraComponent> m_Camera;
};

// One field of one component, changed once. `Component` and `Field` are
// deduced from the pointer-to-member passed in -- e.g.
// EditFieldCommand<GS::TransformComponent, glm::vec3>(entity,
// &GS::TransformComponent::Position, before, after). The component pointer
// is re-resolved through GetComponent on every Undo/Redo rather than
// captured once: ComponentStore's removal swaps the last element into a
// freed slot, which can silently move another entity's component, so a
// pointer captured at edit time and used later is exactly the kind of
// stale reference that pattern warns against.
template<typename Component, typename Field>
class EditFieldCommand : public EditorCommand
{
public:
	using MemberPtr = Field Component::*;

	EditFieldCommand(GS::EntityId entity, MemberPtr member, const Field& oldValue, const Field& newValue)
		: m_Member(member), m_Old(oldValue), m_New(newValue)
	{
		m_Entity = entity;
	}

	void Redo() override { Apply(m_New); }
	void Undo() override { Apply(m_Old); }
private:
	void Apply(const Field& value)
	{
		if (auto* component = g_EditorScene.GetComponent<Component>(m_Entity))
			(*component).*m_Member = value;
	}

	MemberPtr m_Member;
	Field m_Old, m_New;
};

namespace EditorHistory {

	inline std::vector<std::unique_ptr<EditorCommand>> s_Commands;
	inline size_t s_Cursor = 0;

	inline bool CanUndo() { return s_Cursor > 0; }
	inline bool CanRedo() { return s_Cursor < s_Commands.size(); }

	// Performs the command (its Redo()) and adds it to the stack, discarding
	// anything after the cursor -- a new action after undoing replaces the
	// redo branch rather than keeping it around, the standard shape of an
	// undo stack. Returns what selection should become.
	inline GS::EntityId Push(std::unique_ptr<EditorCommand> command)
	{
		s_Commands.resize(s_Cursor);

		command->Redo();
		GS::EntityId selection = command->SelectionAfter();

		s_Commands.push_back(std::move(command));
		s_Cursor = s_Commands.size();

		return selection;
	}

	inline GS::EntityId Undo()
	{
		if (!CanUndo())
			return GS::InvalidEntity;

		s_Cursor--;
		s_Commands[s_Cursor]->Undo();
		return s_Commands[s_Cursor]->SelectionAfter();
	}

	inline GS::EntityId Redo()
	{
		if (!CanRedo())
			return GS::InvalidEntity;

		s_Commands[s_Cursor]->Redo();
		GS::EntityId selection = s_Commands[s_Cursor]->SelectionAfter();
		s_Cursor++;
		return selection;
	}

	// Called whenever the scene itself is swapped out from under the
	// history (New Project, Open Project, Open Demo) -- undoing across a
	// scene swap has no sensible meaning, the same reasoning that already
	// motivated clearing selection on those exact same actions.
	inline void Clear()
	{
		s_Commands.clear();
		s_Cursor = 0;
	}

}
```

- [ ] **Step 2: Write the temporary self-test**

Create `TestEnv/src/EditorHistoryTest.h`:

```cpp
// TEMPORARY -- delete after verifying EditorHistory's undo/redo.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "EditorHistory.h"

namespace EditorHistoryTest {

	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS_TRACE("EditorHistoryTest:");

		g_EditorScene.Clear();
		EditorHistory::Clear();

		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:cube";
		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("Cube",
			GS::TransformComponent{}, mesh, std::nullopt));

		Check(g_EditorScene.GetEntityCount() == 1, "Place creates one entity");
		Check(EditorHistory::CanUndo(), "history has something to undo after Place");
		Check(!EditorHistory::CanRedo(), "nothing to redo yet");

		EditorHistory::Undo();
		Check(g_EditorScene.GetEntityCount() == 0, "Undo removes the placed entity");
		Check(EditorHistory::CanRedo(), "Undo leaves something to redo");

		EditorHistory::Redo();
		Check(g_EditorScene.GetEntityCount() == 1, "Redo brings the entity back");

		GS::EntityId placed = g_EditorScene.GetEntities()[0];
		auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(placed);
		Check(tag && tag->Name == "Cube", "recreated entity has the same tag");

		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(placed);
		glm::vec3 before = transform->Position;
		glm::vec3 after = { 1.0f, 2.0f, 3.0f };
		transform->Position = after;
		EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
			placed, &GS::TransformComponent::Position, before, after));

		Check(g_EditorScene.GetComponent<GS::TransformComponent>(placed)->Position == after,
			"Edit command leaves the new value in place");

		EditorHistory::Undo();
		Check(g_EditorScene.GetComponent<GS::TransformComponent>(placed)->Position == before,
			"Undo restores the old value");

		EditorHistory::Redo();
		Check(g_EditorScene.GetComponent<GS::TransformComponent>(placed)->Position == after,
			"Redo re-applies the new value");

		EditorHistory::Push(std::make_unique<DeleteEntityCommand>(placed));
		Check(g_EditorScene.GetEntityCount() == 0, "Delete removes the entity");

		EditorHistory::Undo();
		Check(g_EditorScene.GetEntityCount() == 1, "Undoing a delete restores the entity");
		GS::EntityId restored = g_EditorScene.GetEntities()[0];
		auto* restoredTransform = g_EditorScene.GetComponent<GS::TransformComponent>(restored);
		Check(restoredTransform && restoredTransform->Position == after,
			"restored entity keeps the edited position, not the original");

		EditorHistory::Undo();
		Check(g_EditorScene.GetEntityCount() == 0, "back to zero entities");
		EditorHistory::Push(std::make_unique<PlaceEntityCommand>("Sphere",
			GS::TransformComponent{}, std::nullopt, std::nullopt));
		Check(!EditorHistory::CanRedo(),
			"pushing a new command after Undo discards the old redo branch");

		GS_TRACE("EditorHistoryTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 3: Run it and read the log**

Temporarily add `#include "EditorHistoryTest.h"` and `EditorHistoryTest::Run();` as the first line of `TestEnv`'s constructor in `TestEnv/src/TestApp.cpp`.

Run: `./gs.py build && ./gs.py run`
Expected: the log shows `EditorHistoryTest:` followed by 13 `ok` lines and `13 passed, 0 failed`, before the window shows anything.

- [ ] **Step 4: Remove the temporary call, keep the header until the final task's cleanup**

Remove the `#include` and the `EditorHistoryTest::Run();` line from `TestApp.cpp`. Leave `EditorHistoryTest.h` itself on disk — Task 5 deletes it.

- [ ] **Step 5: Stage**

```bash
git add TestEnv/src/EditorHistory.h TestEnv/src/EditorHistoryTest.h
```

(No commit — see Global Constraints.)

---

## Task 2: Wire Place/Delete/Edit through EditorHistory

**Files:**
- Modify: `TestEnv/src/EditorShell.h`
- Modify: `TestEnv/src/EditorSceneView.h`

**Interfaces:**
- Consumes: `PlaceEntityCommand`, `DeleteEntityCommand`, `EditFieldCommand<Component, Field>`, `EditorHistory::Push/Undo/Redo` (Task 1)

- [ ] **Step 1: `EditorShell.h` — route `PlaceMesh` and `PlaceCamera` through a command**

Add `#include "EditorHistory.h"` near the top (alongside the existing includes).

Replace:

```cpp
	void PlaceMesh(const std::string& path, const std::string& name)
	{
		std::shared_ptr<GS::Mesh> geometry = GS::MeshCache::Get(path);
		if (!geometry)
			return;

		GS::Entity entity = g_EditorScene.CreateEntity(name);

		GS::MeshComponent mesh;
		mesh.SourcePath = path;
		mesh.Geometry = geometry;
		entity.Add<GS::MeshComponent>(mesh);

		if (g_EditorSceneView)
			g_EditorSceneView->Select(entity.GetId());
	}
```

with:

```cpp
	void PlaceMesh(const std::string& path, const std::string& name)
	{
		std::shared_ptr<GS::Mesh> geometry = GS::MeshCache::Get(path);
		if (!geometry)
			return;

		GS::MeshComponent mesh;
		mesh.SourcePath = path;
		mesh.Geometry = geometry;

		GS::EntityId selection = EditorHistory::Push(
			std::make_unique<PlaceEntityCommand>(name, GS::TransformComponent{}, mesh, std::nullopt));

		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}
```

and replace:

```cpp
	void PlaceCamera()
	{
		GS::Entity entity = g_EditorScene.CreateEntity("Camera");
		entity.Add<GS::CameraComponent>();

		if (g_EditorSceneView)
			g_EditorSceneView->Select(entity.GetId());
	}
```

with:

```cpp
	void PlaceCamera()
	{
		GS::EntityId selection = EditorHistory::Push(std::make_unique<PlaceEntityCommand>(
			"Camera", GS::TransformComponent{}, std::nullopt, GS::CameraComponent{}));

		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}
```

- [ ] **Step 2: `EditorSceneView.h` — route the Delete key, the Delete button, and every Inspector field edit through commands**

Add `#include "EditorHistory.h"` near the top.

In `OnEvent`, replace:

```cpp
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
			{
				g_EditorScene.DestroyEntity(m_Selected);
				m_Selected = GS::InvalidEntity;
			}
```

with:

```cpp
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
				m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));
```

In `OnImGuiRender`, replace the whole Inspector body from the Transform block through the Delete button:

```cpp
		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
		{
			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			ImGui::DragFloat3("Rotation", &transform->Rotation.x, 0.5f);
			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.02f, 20.0f);
		}

		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))
		{
			ImGui::ColorEdit4("Mesh colour", &mesh->Color.x);
			ImGui::Checkbox("Visible", &mesh->Visible);
			ImGui::TextDisabled("Source: %s",
				mesh->SourcePath.empty() ? "(none)" : mesh->SourcePath.c_str());
		}

		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(m_Selected))
		{
			ImGui::SliderFloat("Fov", &camera->Fov, 10.0f, 120.0f);
			ImGui::Checkbox("Active camera", &camera->Active);
		}

		if (ImGui::Button("Delete"))
		{
			g_EditorScene.DestroyEntity(m_Selected);
			m_Selected = GS::InvalidEntity;
		}
```

with:

```cpp
		if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected))
		{
			glm::vec3 beforePosition = transform->Position;
			ImGui::DragFloat3("Position", &transform->Position.x, 0.01f);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Position, beforePosition, transform->Position));

			glm::vec3 beforeRotation = transform->Rotation;
			ImGui::DragFloat3("Rotation", &transform->Rotation.x, 0.5f);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Rotation, beforeRotation, transform->Rotation));

			glm::vec3 beforeScale = transform->Scale;
			ImGui::DragFloat3("Scale", &transform->Scale.x, 0.01f, 0.02f, 20.0f);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::TransformComponent, glm::vec3>>(
					m_Selected, &GS::TransformComponent::Scale, beforeScale, transform->Scale));
		}

		if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(m_Selected))
		{
			glm::vec4 beforeColor = mesh->Color;
			ImGui::ColorEdit4("Mesh colour", &mesh->Color.x);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, glm::vec4>>(
					m_Selected, &GS::MeshComponent::Color, beforeColor, mesh->Color));

			bool beforeVisible = mesh->Visible;
			ImGui::Checkbox("Visible", &mesh->Visible);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::MeshComponent, bool>>(
					m_Selected, &GS::MeshComponent::Visible, beforeVisible, mesh->Visible));

			ImGui::TextDisabled("Source: %s",
				mesh->SourcePath.empty() ? "(none)" : mesh->SourcePath.c_str());
		}

		if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(m_Selected))
		{
			float beforeFov = camera->Fov;
			ImGui::SliderFloat("Fov", &camera->Fov, 10.0f, 120.0f);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::CameraComponent, float>>(
					m_Selected, &GS::CameraComponent::Fov, beforeFov, camera->Fov));

			bool beforeActive = camera->Active;
			ImGui::Checkbox("Active camera", &camera->Active);
			if (ImGui::IsItemDeactivatedAfterEdit())
				EditorHistory::Push(std::make_unique<EditFieldCommand<GS::CameraComponent, bool>>(
					m_Selected, &GS::CameraComponent::Active, beforeActive, camera->Active));
		}

		if (ImGui::Button("Delete"))
			m_Selected = EditorHistory::Push(std::make_unique<DeleteEntityCommand>(m_Selected));
```

Note the ImGui-widget-driven edits (`DragFloat3`, `ColorEdit4`, `SliderFloat`, `Checkbox`) already write the new value directly into the component before `IsItemDeactivatedAfterEdit()` is checked, so by the time `EditorHistory::Push` calls the new command's `Redo()`, it is writing back a value that is already there — a harmless, intentional no-op re-assertion, not a bug.

- [ ] **Step 3: Build and manually verify**

Run: `./gs.py build && ./gs.py run`
Expected: place a cube from the Assets panel, move it with the gizmo, press Ctrl+Z — no menu exists yet to trigger this from, so this step is a **visual smoke test only**: confirm the app still runs, placing/deleting/editing still work exactly as before (nothing regressed), since `EditorHistory::Undo`/`Redo` have no UI trigger until Task 4. The temporary self-test from Task 1 already proved the underlying logic.

Run headless: `./bin/Debug-linux-x86_64/TestEnv/TestEnv --hide-window --lockstep --hide-ui --capture /tmp/task2.png --capture-step 5` from `bin/Debug-linux-x86_64/TestEnv/` — confirm exit code 0.

- [ ] **Step 4: Stage**

```bash
git add TestEnv/src/EditorShell.h TestEnv/src/EditorSceneView.h
```

---

## Task 3: The Project manifest

**Files:**
- Modify: `TestEnv/src/EditorProject.h`

**Interfaces:**
- Produces: `struct ProjectManifest { std::string Name; std::string SceneRelativePath; }`, `g_EditorProjectPath` (`std::string`, empty when no project is open), `g_EditorProjectName` (`std::string`), `CreateEditorProject(folderPath, name) -> bool`, `OpenEditorProject(folderPath) -> bool`, `SaveEditorProject() -> bool`, `SaveEditorProjectAs(folderPath, name) -> bool`, `RenameEditorProject(newName) -> bool`

- [ ] **Step 1: Add `#include <filesystem>` and `#include <sstream>`**

At the top of `TestEnv/src/EditorProject.h`, alongside the existing `#include <fstream>`.

- [ ] **Step 2: Add the manifest read/write helpers and the Project functions**

Append to `TestEnv/src/EditorProject.h`, after the existing `LoadEditorProjectFromCommandLine`:

```cpp
// A project is a folder holding a manifest (name, which scene is active)
// wrapping today's single g_EditorScene. Deliberately one scene per
// project for now -- a project that can hold several is a real feature for
// when something actually needs it, not built speculatively here.
struct ProjectManifest
{
	std::string Name;
	std::string SceneRelativePath;
};

inline const char* ProjectManifestFilename() { return "project.gsproj"; }

// Shared by OpenEditorProject and RenameEditorProject so the manifest's
// tagged-text format is parsed in exactly one place.
inline bool ReadProjectManifest(const std::string& folderPath, ProjectManifest& out)
{
	std::filesystem::path manifestPath = std::filesystem::path(folderPath) / ProjectManifestFilename();

	std::ifstream in(manifestPath.string());
	if (!in)
		return false;

	std::string tag;
	int version = 0;
	in >> tag >> version;

	if (tag != "gs-project" || version != 1)
		return false;

	std::string line;
	std::getline(in, line);   // rest of the header line

	while (std::getline(in, line))
	{
		std::istringstream fields(line);
		std::string key;
		fields >> key;

		if (key == "name")
		{
			std::string name;
			std::getline(fields, name);
			size_t from = name.find_first_not_of(' ');
			out.Name = from == std::string::npos ? "" : name.substr(from);
		}
		else if (key == "scene")
		{
			fields >> out.SceneRelativePath;
		}
	}

	return !out.SceneRelativePath.empty();
}

inline bool WriteProjectManifest(const std::string& folderPath, const ProjectManifest& manifest)
{
	std::filesystem::path manifestPath = std::filesystem::path(folderPath) / ProjectManifestFilename();

	std::ofstream out(manifestPath.string());
	if (!out)
		return false;

	out << "gs-project 1\n";
	out << "name " << manifest.Name << "\n";
	out << "scene " << manifest.SceneRelativePath << "\n";
	return true;
}

// The project's own name, distinct from its folder path -- empty when no
// project is open (a bare --scene session, exactly today's behaviour).
inline std::string g_EditorProjectPath;
inline std::string g_EditorProjectName;

// Creates a new, blank project: the folder, a fresh empty scene inside it,
// and a manifest naming both. Clears g_EditorScene first -- "new" means
// starting over, unlike SaveEditorProjectAs below.
inline bool CreateEditorProject(const std::string& folderPath, const std::string& name)
{
	std::error_code ec;
	std::filesystem::create_directories(folderPath, ec);
	if (ec)
	{
		GS_WARN("Could not create project folder '{0}': {1}", folderPath, ec.message());
		return false;
	}

	ProjectManifest manifest;
	manifest.Name = name;
	manifest.SceneRelativePath = "scene.txt";

	g_EditorScene.Clear();

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();
	if (!g_EditorScene.Save(scenePath))
	{
		GS_WARN("Could not create the new project's scene file at '{0}'", scenePath);
		return false;
	}

	if (!WriteProjectManifest(folderPath, manifest))
	{
		GS_WARN("Could not write a project manifest in '{0}'", folderPath);
		return false;
	}

	g_EditorProjectPath = folderPath;
	g_EditorProjectName = name;
	g_EditorScenePath = scenePath;
	return true;
}

// Writes whatever scene is *currently open* into a new project folder,
// without touching it first -- the opposite of CreateEditorProject, which
// is why the two are separate functions rather than one with a flag.
inline bool SaveEditorProjectAs(const std::string& folderPath, const std::string& name)
{
	std::error_code ec;
	std::filesystem::create_directories(folderPath, ec);
	if (ec)
	{
		GS_WARN("Could not create project folder '{0}': {1}", folderPath, ec.message());
		return false;
	}

	ProjectManifest manifest;
	manifest.Name = name;
	manifest.SceneRelativePath = "scene.txt";

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();
	if (!g_EditorScene.Save(scenePath))
	{
		GS_WARN("Could not save the current scene to '{0}'", scenePath);
		return false;
	}

	if (!WriteProjectManifest(folderPath, manifest))
	{
		GS_WARN("Could not write a project manifest in '{0}'", folderPath);
		return false;
	}

	g_EditorProjectPath = folderPath;
	g_EditorProjectName = name;
	g_EditorScenePath = scenePath;
	return true;
}

inline bool OpenEditorProject(const std::string& folderPath)
{
	ProjectManifest manifest;
	if (!ReadProjectManifest(folderPath, manifest))
	{
		GS_WARN("'{0}' has no readable {1}", folderPath, ProjectManifestFilename());
		return false;
	}

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();
	if (!OpenEditorScene(scenePath))
	{
		GS_WARN("Project '{0}' names scene '{1}', which could not be opened", folderPath, scenePath);
		return false;
	}

	g_EditorProjectPath = folderPath;
	g_EditorProjectName = manifest.Name;
	return true;
}

// Writes the current scene back to wherever the open project's manifest
// says it lives. False if no project is open.
inline bool SaveEditorProject()
{
	if (g_EditorProjectPath.empty() || g_EditorScenePath.empty())
		return false;

	return SaveEditorScene(g_EditorScenePath);
}

// Changes only the manifest's name field. The folder and the scene file it
// names are untouched, so a rename never breaks anything that held the old
// path.
inline bool RenameEditorProject(const std::string& newName)
{
	if (g_EditorProjectPath.empty())
		return false;

	ProjectManifest manifest;
	if (!ReadProjectManifest(g_EditorProjectPath, manifest))
		return false;

	manifest.Name = newName;
	if (!WriteProjectManifest(g_EditorProjectPath, manifest))
		return false;

	g_EditorProjectName = newName;
	return true;
}
```

- [ ] **Step 2: Build**

Run: `./gs.py build`
Expected: exit code 0. Nothing calls these functions yet (Task 4 wires the menu bar to them), so this step only proves the new code compiles standalone.

- [ ] **Step 3: Manually verify with a throwaway call**

This is worth a quick manual check before Task 4 builds the UI on top of it, since a filesystem bug is easier to isolate now than after. Temporarily add to `TestEnv`'s constructor in `TestApp.cpp`:

```cpp
bool created = CreateEditorProject("test_project", "Test Project");
GS_TRACE("CreateEditorProject: {0}", created);
bool opened = OpenEditorProject("test_project");
GS_TRACE("OpenEditorProject: {0}, name='{1}'", opened, g_EditorProjectName);
```

Run: `./gs.py build && ./gs.py run`
Expected: both log `true`, `name='Test Project'`, and `test_project/project.gsproj` plus `test_project/scene.txt` exist on disk (check from `bin/Debug-linux-x86_64/TestEnv/`, since that's where the binary runs from). Remove these two lines and the folder (`rm -rf bin/Debug-linux-x86_64/TestEnv/test_project`) once confirmed — this was a throwaway check, not a kept self-test (no `TEMPORARY` marker needed since it never became a file of its own).

- [ ] **Step 4: Stage**

```bash
git add TestEnv/src/EditorProject.h
```

---

## Task 4: EditorMenuBar, and the layout reshuffle

**Files:**
- Create: `TestEnv/src/EditorMenuBar.h`
- Modify: `TestEnv/src/EditorShell.h`
- Modify: `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `EditorHistory::Undo/Redo/CanUndo/CanRedo` (Task 1), `CreateEditorProject`/`OpenEditorProject`/`SaveEditorProject`/`SaveEditorProjectAs`/`RenameEditorProject`/`g_EditorProjectPath`/`g_EditorProjectName` (Task 3), `s_Demos`/`s_DemoInstances`/`DemoEntry::CanOpenInEditor` (`TestEnv/src/DemoRegistry.h`), `g_EditorSceneView`/`EditorSceneView::Select` (`TestEnv/src/EditorSceneView.h`)
- Produces: `class EditorMenuBar : public GS::Layer`; `class EditorShell` gains a public `void ResetToDefaultLayout()`; `inline class EditorShell* g_EditorShellInstance` (forward-declared before the class, same pattern `g_EditorSceneView` already uses)

- [ ] **Step 1: Give `EditorShell` a way to force a layout rebuild**

In `TestEnv/src/EditorShell.h`, add a forward declaration and global pointer immediately before `class EditorShell`, matching the pattern already used for `g_EditorSceneView` in `EditorSceneView.h`:

```cpp
class EditorShell;
inline EditorShell* g_EditorShellInstance = nullptr;

class EditorShell : public GS::Layer
{
```

In `EditorShell::OnAttach`, add `g_EditorShellInstance = this;` as the first line.

Add a public method, and one new private member:

```cpp
	// "Reset to default" (EditorMenuBar.h) needs a way back to the original
	// arrangement even after BuildLayout has already run once this session.
	// Setting m_Built false alone is not enough -- BuildLayout's own guard
	// treats an already-split dock as "leave it alone", which is exactly
	// what must be bypassed here and nowhere else.
	void ResetToDefaultLayout()
	{
		m_Built = false;
		m_ForceDefault = true;
	}
```

Change `BuildLayout`'s guard from:

```cpp
		if (ImGui::DockBuilderGetNode(dock)
			&& ImGui::DockBuilderGetNode(dock)->IsSplitNode())
			return;
```

to:

```cpp
		bool alreadyArranged = ImGui::DockBuilderGetNode(dock)
			&& ImGui::DockBuilderGetNode(dock)->IsSplitNode();

		if (alreadyArranged && !m_ForceDefault)
			return;

		m_ForceDefault = false;
```

Add `bool m_ForceDefault = false;` next to the existing `bool m_Built = false;`.

- [ ] **Step 2: Reshuffle `BuildLayout` and rename/trim the Assets panel**

Replace the whole `BuildLayout` body's dock-splitting section:

```cpp
		ImGuiID centre = dock;

		ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left,
			0.24f, nullptr, &centre);

		ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
			0.26f, nullptr, &centre);

		ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
			0.22f, nullptr, &centre);

		// The selector goes under the controls rather than beside them: it is
		// a list that is read top to bottom and it is used far less often than
		// whatever is above it.
		ImGuiID lower = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
			0.40f, nullptr, &left);

		// The right column is split too: the Inspector under the Profiler.
		// Both describe "the thing currently selected" -- a component's live
		// values on top, a scope's timings below -- so they share a column
		// rather than fighting the left column for space.
		ImGuiID rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
			0.45f, nullptr, &right);

		// **The demo's own panel goes here, and `DemoLayer` puts it there by
		// id rather than by name.** Docking by title was tried first and is
		// too fragile: a demo's panel is titled whatever its author chose,
		// which is not the name in the registry, and a list of both in a third
		// file is exactly the kind of thing that silently falls out of step.
		g_DemoDock = (unsigned int)left;

		ImGui::DockBuilderDockWindow("Demos", lower);
		ImGui::DockBuilderDockWindow("Profiler", right);
		ImGui::DockBuilderDockWindow("Assets", bottom);

		// The Outliner and Inspector are core editor panels -- an entity list
		// and the properties of whatever is selected -- so they get the two
		// prominent slots left column/right column would otherwise have gone
		// to nobody: left (above the demo selector) and under the Profiler.
		ImGui::DockBuilderDockWindow("Outliner", left);
		ImGui::DockBuilderDockWindow("Inspector", rightLower);

		ImGui::DockBuilderFinish(dock);
```

with:

```cpp
		ImGuiID centre = dock;

		ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left,
			0.24f, nullptr, &centre);

		ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
			0.26f, nullptr, &centre);

		// The output well: terminal, build log, textures -- none built yet,
		// but the slot exists now so each lands here without another layout
		// pass, the same reasoning DockBuilderDockWindow below applies to it.
		ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
			0.22f, nullptr, &centre);

		// Tools under the Outliner: creation controls read far less often
		// than the entity list above them, the same relationship the demo
		// selector used to have with the controls above it.
		ImGuiID lower = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
			0.40f, nullptr, &left);

		// The right column is split too: the Inspector under the Profiler.
		// Both describe "the thing currently selected" -- a component's live
		// values on top, a scope's timings below -- so they share a column
		// rather than fighting the left column for space.
		ImGuiID rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
			0.45f, nullptr, &right);

		// **The demo's own panel goes here, and `DemoLayer` puts it there by
		// id rather than by name.** Docking by title was tried first and is
		// too fragile: a demo's panel is titled whatever its author chose,
		// which is not the name in the registry, and a list of both in a third
		// file is exactly the kind of thing that silently falls out of step.
		//
		// **Unchanged from before this reshuffle: `g_DemoDock` is `left`, not
		// `lower`.** `left` is where the Outliner sits and where each demo's
		// own panel tabs in alongside it; `lower` is the smaller strip
		// reserved for the two lists (Demos, Tools) below it. Pointing
		// `g_DemoDock` at `lower` instead would dock every demo's own panel
		// into that small list strip rather than the main left column -- a
		// real regression this reshuffle must not introduce.
		g_DemoDock = (unsigned int)left;

		ImGui::DockBuilderDockWindow("Demos", lower);
		ImGui::DockBuilderDockWindow("Tools", lower);
		ImGui::DockBuilderDockWindow("Profiler", right);
		ImGui::DockBuilderDockWindow("Terminal", bottom);
		ImGui::DockBuilderDockWindow("Build Output", bottom);
		ImGui::DockBuilderDockWindow("Textures", bottom);

		ImGui::DockBuilderDockWindow("Outliner", left);
		ImGui::DockBuilderDockWindow("Inspector", rightLower);

		ImGui::DockBuilderFinish(dock);
```

Note "Demos" and "Tools" now share the same `lower` node (they'll appear as two tabs in that slot) — both are secondary to the Outliner above them, and neither is used often enough to deserve its own column. `g_DemoDock` still points at `lower`, unchanged in meaning: it's still "wherever the active demo's own panel should dock."

Rename the panel and remove its Scene section and its "Open as starting scene" section (both move to `EditorMenuBar.h`'s File menu). Replace:

```cpp
		// The spare pane. Named rather than left blank so it has somewhere to
		// dock to before there is anything to put in it.
		ImGui::Begin("Assets");

		ImGui::SeparatorText("Scene");
		ImGui::TextDisabled("%s", g_EditorScenePath.empty() ? "(unsaved)" : g_EditorScenePath.c_str());
		ImGui::InputText("##savepath", m_SavePath, sizeof(m_SavePath));
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			SaveEditorScene(m_SavePath);
		ImGui::SameLine();
		if (ImGui::Button("Open"))
		{
			OpenEditorScene(m_SavePath);
			// The scene may have just been rebuilt with its generation counters
			// reset (Scene::Clear, inside Scene::Load) -- a selection held from
			// before this looks valid but can now alias an unrelated entity.
			if (g_EditorSceneView)
				g_EditorSceneView->Select(GS::InvalidEntity);
		}
		ImGui::SameLine();
		if (ImGui::Button("New"))
		{
			g_EditorScene.Clear();
			g_EditorScenePath.clear();
			if (g_EditorSceneView)
				g_EditorSceneView->Select(GS::InvalidEntity);
		}

		ImGui::SeparatorText("Preset shapes");
```

with:

```cpp
		// Named "Tools" now -- scene save/open/new moved to the File menu
		// (EditorMenuBar.h), and "Open as starting scene" moved there too,
		// below. What is left here is exactly the entity-creation controls,
		// which is what "Tools" means in a conventional editor layout.
		ImGui::Begin("Tools");

		ImGui::SeparatorText("Preset shapes");
```

and remove the whole `"Open as starting scene"` block (it moves to `EditorMenuBar.h`'s File > Open Demo submenu, reading the same `s_Demos`/`s_DemoInstances`):

```cpp
		ImGui::SeparatorText("Open as starting scene");
		for (int i = 0; i < s_DemoCount; i++)
		{
			if (!s_Demos[i].CanOpenInEditor || !s_DemoInstances[i])
				continue;

			if (ImGui::Button(s_Demos[i].Name))
			{
				g_EditorScene.Clear();
				g_EditorScenePath.clear();
				if (g_EditorSceneView)
					g_EditorSceneView->Select(GS::InvalidEntity);
				s_DemoInstances[i]->OnExportToScene(g_EditorScene);
			}
		}

		ImGui::End();
	}
```

becomes just:

```cpp
		ImGui::End();
	}
```

Remove the now-unused `m_SavePath` member (`char m_SavePath[256] = "scene.txt";`) — `m_ImportPath` stays, still used by the Import section.

Add the three bottom stub panels, right after the "Tools" panel's `ImGui::End();` (still inside `OnImGuiRender`, before the function's closing brace):

```cpp
		ImGui::Begin("Terminal");
		ImGui::TextDisabled("Not built yet -- terminal sub-project.");
		ImGui::End();

		ImGui::Begin("Build Output");
		ImGui::TextDisabled("Not built yet.");
		ImGui::End();

		ImGui::Begin("Textures");
		ImGui::TextDisabled("Not built yet.");
		ImGui::End();
```

- [ ] **Step 3: Write `EditorMenuBar.h`**

Create `TestEnv/src/EditorMenuBar.h`:

```cpp
#pragma once

// File/Edit/View/Help, and the three things they actually do: project I/O,
// undo/redo, and named layout presets. Kept out of EditorShell.h on
// purpose -- that file lays out panels, this one is chrome above them, and
// the two must not be confused about what "the layout" means: BuildLayout
// builds the panel arrangement, this file's layout presets save and
// restore it.
//
// Pushed before EditorShell in TestApp.cpp, and that order is load-bearing:
// ImGui::BeginMainMenuBar() shrinks the main viewport's work area for the
// rest of the frame, and EditorShell's dockspace reads that work area to
// size itself. Rendered after EditorShell instead, the dockspace would
// claim the strip the menu bar draws over.

#include <GS.h>
#include <imgui.h>
#include <fstream>
#include <cctype>

#include "Demo.h"
#include "DemoRegistry.h"
#include "EditorProject.h"
#include "EditorHistory.h"
#include "EditorSceneView.h"
#include "EditorShell.h"

class EditorMenuBar : public GS::Layer
{
public:
	EditorMenuBar() : Layer("EditorMenuBar") {}

	void OnEvent(GS::Event& e) override
	{
		GS::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			bool ctrl = GS::Input::IsKeyPressed(GS_KEY_LEFT_CONTROL)
				|| GS::Input::IsKeyPressed(GS_KEY_RIGHT_CONTROL);
			if (!ctrl)
				return false;

			if (e.GetKeyCode() == GS_KEY_Z) { DoUndo(); return true; }
			if (e.GetKeyCode() == GS_KEY_Y) { DoRedo(); return true; }
			if (e.GetKeyCode() == GS_KEY_F) { m_FindRequested = true; return true; }

			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (!g_EditorShell)
			return;

		if (ImGui::BeginMainMenuBar())
		{
			DrawFileMenu();
			DrawEditMenu();
			DrawViewMenu();
			DrawHelpMenu();
			ImGui::EndMainMenuBar();
		}

		DrawFileDialogs();
		DrawFindPopup();
	}
private:
	void DoUndo()
	{
		GS::EntityId selection = EditorHistory::Undo();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	void DoRedo()
	{
		GS::EntityId selection = EditorHistory::Redo();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(selection);
	}

	// Every action that swaps the scene out from under the editor (New,
	// Open, Open Demo) needs the same three follow-ups: clear undo history,
	// clear selection, and reset the project globals if the caller didn't
	// already set them. Selection/history clearing is centralised here so
	// no call site can forget one -- Task 2's own comment about a stale
	// selection aliasing an unrelated entity is exactly the bug this
	// prevents recurring.
	void ResetForNewScene()
	{
		EditorHistory::Clear();
		if (g_EditorSceneView)
			g_EditorSceneView->Select(GS::InvalidEntity);
	}

	void DrawFileMenu()
	{
		if (!ImGui::BeginMenu("File"))
			return;

		if (ImGui::MenuItem("New Project..."))
			ImGui::OpenPopup("New Project");
		if (ImGui::MenuItem("Open Project..."))
			ImGui::OpenPopup("Open Project");

		ImGui::Separator();

		if (ImGui::MenuItem("Save", nullptr, false, !g_EditorProjectPath.empty()))
			SaveEditorProject();
		if (ImGui::MenuItem("Save As..."))
			ImGui::OpenPopup("Save Project As");
		if (ImGui::MenuItem("Rename Project...", nullptr, false, !g_EditorProjectPath.empty()))
			ImGui::OpenPopup("Rename Project");

		ImGui::Separator();

		if (ImGui::BeginMenu("Open Demo"))
		{
			for (int i = 0; i < s_DemoCount; i++)
			{
				if (!s_Demos[i].CanOpenInEditor || !s_DemoInstances[i])
					continue;

				if (ImGui::MenuItem(s_Demos[i].Name))
				{
					g_EditorScene.Clear();
					g_EditorScenePath.clear();
					ResetForNewScene();
					s_DemoInstances[i]->OnExportToScene(g_EditorScene);
				}
			}
			ImGui::EndMenu();
		}

		ImGui::EndMenu();
	}

	void DrawFileDialogs()
	{
		if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Create") && CreateEditorProject(m_DialogPath, m_DialogName))
			{
				ResetForNewScene();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			if (ImGui::Button("Open") && OpenEditorProject(m_DialogPath))
			{
				ResetForNewScene();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Save Project As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Folder", m_DialogPath, sizeof(m_DialogPath));
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Save") && SaveEditorProjectAs(m_DialogPath, m_DialogName))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal("Rename Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Rename") && RenameEditorProject(m_DialogName))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void DrawEditMenu()
	{
		if (!ImGui::BeginMenu("Edit"))
			return;

		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, EditorHistory::CanUndo()))
			DoUndo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, EditorHistory::CanRedo()))
			DoRedo();

		ImGui::Separator();

		if (ImGui::MenuItem("Find", "Ctrl+F"))
			m_FindRequested = true;

		ImGui::EndMenu();
	}

	void DrawFindPopup()
	{
		if (m_FindRequested)
		{
			ImGui::OpenPopup("Find Entity");
			m_FindRequested = false;
			m_FindQuery[0] = '\0';
		}

		if (!ImGui::BeginPopup("Find Entity"))
			return;

		ImGui::SetKeyboardFocusHere();
		ImGui::InputText("##findquery", m_FindQuery, sizeof(m_FindQuery));

		std::string query = m_FindQuery;
		for (char& c : query)
			c = (char)std::tolower((unsigned char)c);

		for (GS::EntityId entity : g_EditorScene.GetEntities())
		{
			auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
			if (!tag)
				continue;

			std::string lowerName = tag->Name;
			for (char& c : lowerName)
				c = (char)std::tolower((unsigned char)c);

			if (!query.empty() && lowerName.find(query) == std::string::npos)
				continue;

			if (ImGui::Selectable(tag->Name.c_str()))
			{
				if (g_EditorSceneView)
					g_EditorSceneView->Select(entity);
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::EndPopup();
	}

	// --- Layout presets ------------------------------------------------
	// A tagged text file, same convention as the rest of this project's
	// hand-written formats: one header line, then repeated `layout <name>`
	// / body lines / `end` blocks. A bare "end" line inside a real ImGui
	// ini body is not a pattern ImGui's own format produces, which is why
	// this is safe without escaping -- the same kind of accepted, checked
	// assumption as this project's other hand-rolled formats.
	struct LayoutPreset { std::string Name; std::string Ini; };

	static const char* LayoutPresetsPath() { return "editor_layouts.txt"; }

	static std::vector<LayoutPreset> ReadLayoutPresets()
	{
		std::vector<LayoutPreset> presets;
		std::ifstream in(LayoutPresetsPath());
		if (!in)
			return presets;

		std::string tag;
		int version = 0;
		in >> tag >> version;
		if (tag != "gs-layouts" || version != 1)
			return presets;

		std::string line;
		std::getline(in, line);

		LayoutPreset* current = nullptr;
		while (std::getline(in, line))
		{
			if (line.rfind("layout ", 0) == 0)
			{
				presets.push_back({ line.substr(7), "" });
				current = &presets.back();
			}
			else if (line == "end")
			{
				current = nullptr;
			}
			else if (current)
			{
				current->Ini += line;
				current->Ini += "\n";
			}
		}

		return presets;
	}

	static void WriteLayoutPresets(const std::vector<LayoutPreset>& presets)
	{
		std::ofstream out(LayoutPresetsPath());
		if (!out)
			return;

		out << "gs-layouts 1\n";
		for (const LayoutPreset& preset : presets)
		{
			out << "layout " << preset.Name << "\n";
			out << preset.Ini;
			out << "end\n";
		}
	}

	void SaveLayoutPreset(const std::string& name)
	{
		std::vector<LayoutPreset> presets = ReadLayoutPresets();

		size_t iniSize = 0;
		const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
		std::string iniString(ini, iniSize);

		for (LayoutPreset& preset : presets)
		{
			if (preset.Name == name)
			{
				preset.Ini = iniString;
				WriteLayoutPresets(presets);
				return;
			}
		}

		presets.push_back({ name, iniString });
		WriteLayoutPresets(presets);
	}

	void LoadLayoutPreset(const std::string& name)
	{
		for (const LayoutPreset& preset : ReadLayoutPresets())
		{
			if (preset.Name == name)
			{
				ImGui::LoadIniSettingsFromMemory(preset.Ini.c_str(), preset.Ini.size());
				return;
			}
		}
	}

	void DrawViewMenu()
	{
		if (ImGui::BeginMenu("View"))
		{
			for (const LayoutPreset& preset : ReadLayoutPresets())
				if (ImGui::MenuItem(preset.Name.c_str()))
					LoadLayoutPreset(preset.Name);

			ImGui::Separator();

			if (ImGui::MenuItem("Save current layout as..."))
				ImGui::OpenPopup("Save Layout");
			if (ImGui::MenuItem("Reset to default") && g_EditorShellInstance)
				g_EditorShellInstance->ResetToDefaultLayout();

			ImGui::EndMenu();
		}

		if (ImGui::BeginPopupModal("Save Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", m_DialogName, sizeof(m_DialogName));
			if (ImGui::Button("Save"))
			{
				SaveLayoutPreset(m_DialogName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void DrawHelpMenu()
	{
		if (!ImGui::BeginMenu("Help"))
			return;

		ImGui::Text("GS editor");
		ImGui::TextDisabled("Built from source -- no version number yet.");

		ImGui::EndMenu();
	}
private:
	char m_DialogPath[256] = "";
	char m_DialogName[128] = "";
	char m_FindQuery[128] = "";
	bool m_FindRequested = false;
};
```

- [ ] **Step 4: Wire it into `TestApp.cpp`**

Add `#include "EditorMenuBar.h"` and `PushLayer(new EditorMenuBar());` **immediately before** `PushLayer(new EditorShell());`. The order matters (see the comment at the top of `EditorMenuBar.h`) — layers render in push order, and the menu bar must shrink the viewport's work area before `EditorShell` reads it to size the dockspace.

- [ ] **Step 5: Build and manually verify**

Run: `./gs.py build && ./gs.py run`

Expected, checked by hand (this step's verification is necessarily interactive — ImGui clicks can't be driven headlessly in this environment, the same limitation an earlier task in this project's history hit and reported honestly):
- A menu bar reading File / Edit / View / Help appears at the top, and the Outliner/Tools/Profiler/Inspector panels are still visible below it, not obscured by it.
- **Run with `--demo Cube3D` (or any other demo) and confirm that demo's own panel (e.g. Cube3D's own "Cube3D" window, with its WASD instructions) still docks into the main left column alongside the Outliner, not into the smaller Demos/Tools strip.** This is exactly the placement `g_DemoDock` controls, and getting it wrong is a real, easy-to-miss regression (a demo's panel silently landing in the wrong slot rather than failing to appear at all).
- File > New Project prompts for a folder and a name, creates both, and the scene clears.
- File > Save/Save As/Rename Project behave as described.
- File > Open Demo > Cube3D populates the scene the same way the old Assets-panel button did.
- Edit > Undo/Redo work after placing/moving/deleting something; Ctrl+Z/Ctrl+Y do the same.
- Edit > Find (or Ctrl+F) opens a popup, typing filters the list, clicking a result selects it.
- View lists no presets initially; "Save current layout as..." then naming one adds it to the list; picking it after dragging a panel elsewhere puts panels back; "Reset to default" rebuilds the original arrangement.

Then verify headlessly that nothing broke for demos: from `bin/Debug-linux-x86_64/TestEnv/`, `./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/task4_demo.png --capture-step 5` — exit code 0, plausible file size.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/src/EditorMenuBar.h TestEnv/src/EditorShell.h TestEnv/src/TestApp.cpp
```

---

## Task 5: Layout regression capture, docs, and cleanup

**Files:**
- Create (temporary): `TestEnv/src/EditorLayoutTest.h`
- Modify (temporary call site): `TestEnv/src/TestApp.cpp`
- Modify: `docs/STATE.md`
- Delete (cleanup): `TestEnv/src/EditorHistoryTest.h`, `TestEnv/src/EditorLayoutTest.h`

**Interfaces:**
- Consumes: everything from Tasks 1-4

- [ ] **Step 1: Capture-verify the reshuffled layout**

A fresh `imgui.ini` (deleted before this run) forces `BuildLayout` to actually run, so this proves the new dock arrangement — not a leftover `imgui.ini` from development — produces the panels in the places the plan describes.

From `bin/Debug-linux-x86_64/TestEnv/`:

```bash
rm -f imgui.ini
./TestEnv --hide-window --lockstep --hide-ui --capture /tmp/layout_hideui.png --capture-step 5
```

Expected: exit code 0. `--hide-ui` draws no panels at all (same as every other capture in this project), so this only proves the app still boots cleanly with a fresh layout — the panel *placement* itself has to be checked with the UI visible, which cannot be captured headlessly the same way. Do that check interactively instead: delete `imgui.ini`, run `./gs.py run`, and confirm by eye that Outliner/Tools sit left, Profiler/Inspector sit right, Terminal/Build Output/Textures are tabbed along the bottom, and Demos/Tools share their slot as tabs.

- [ ] **Step 2: Write and run a temporary self-test for the Project manifest round trip**

This is the one piece of Task 3/4 that *can* be verified without a window — a manifest write/read round trip is plain file I/O.

Create `TestEnv/src/EditorLayoutTest.h`:

```cpp
// TEMPORARY -- delete after verifying the project manifest round-trips.
#pragma once
#include <GS.h>

#include "EditorProject.h"

namespace EditorLayoutTest {

	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS_TRACE("EditorLayoutTest:");

		const char* folder = "editor_layout_test_project";

		Check(CreateEditorProject(folder, "Layout Test"), "CreateEditorProject succeeds");
		Check(g_EditorProjectName == "Layout Test", "project name is set after create");

		g_EditorProjectPath.clear();
		g_EditorProjectName.clear();

		Check(OpenEditorProject(folder), "OpenEditorProject succeeds after clearing globals");
		Check(g_EditorProjectName == "Layout Test", "project name is restored by Open");

		Check(RenameEditorProject("Renamed"), "RenameEditorProject succeeds");
		Check(g_EditorProjectName == "Renamed", "name is updated immediately after rename");

		g_EditorProjectName.clear();
		Check(OpenEditorProject(folder), "OpenEditorProject re-reads after rename");
		Check(g_EditorProjectName == "Renamed", "renamed name persisted to disk");

		GS_TRACE("EditorLayoutTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

Temporarily add `#include "EditorLayoutTest.h"` and `EditorLayoutTest::Run();` as the first line of `TestEnv`'s constructor.

Run: `./gs.py build && ./gs.py run`
Expected: `EditorLayoutTest:` followed by 6 `ok` lines and `6 passed, 0 failed`.

Remove the temporary call from `TestApp.cpp`, and remove the test project folder: `rm -rf bin/Debug-linux-x86_64/TestEnv/editor_layout_test_project`.

- [ ] **Step 3: `git grep TEMPORARY`, and delete every temporary file it finds**

```bash
git grep -n TEMPORARY -- TestEnv/src
```

Expected: exactly `EditorHistoryTest.h` and `EditorLayoutTest.h`. Delete both files, and confirm `TestApp.cpp` has no leftover `#include`/call for either (it shouldn't — both were removed in their own task's cleanup step already; this is a final confirmation, not new work).

- [ ] **Step 4: Verify every config**

Run: `./gs.py build all`
Expected: Debug, Release, and Dist all build clean.

Spot-check two or three demos headlessly, one mesh-heavy and one not, from `bin/Debug-linux-x86_64/TestEnv/`:

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/final_cube3d.png --capture-step 5
./TestEnv --demo Physics3D --hide-window --lockstep --hide-ui --capture /tmp/final_physics3d.png --capture-step 5
```

Expected: both exit 0, plausible file sizes, unaffected by anything in this plan (neither demo's own code was touched).

- [ ] **Step 5: Update `docs/STATE.md`**

This sub-project is done; the terminal is next per the sequence already recorded there. Update the "What is next" section's numbered list to mark step 1 (menu bar + layout) landed, following the file's own instruction to fix it in the same commit as the work that made it wrong — name what got built (the menu bar, the Project manifest, minimal undo/redo, named layout presets, the reshuffled layout with stub bottom panels) and that step 2 (the embedded terminal) is next.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/src/TestApp.cpp docs/STATE.md
git status --short
```

Confirm the status shows only files under `TestEnv/src/`, `GS/` (none expected this plan), and `docs/STATE.md` — no stray root-level runtime artifacts (`imgui.ini`, `editor_layouts.txt`, `editor_layout_test_project/`, etc., all of which are gitignored build-directory content, not repo-root files, as long as every command in this plan was run from `bin/Debug-linux-x86_64/TestEnv/` as instructed).

```bash
git add -A
```

---

## Self-review notes

- **Spec coverage:** menu bar with File/Edit/View/Help (Task 4); Project concept with its own name (Task 3); minimal undo/redo (Tasks 1-2); Find over the Outliner (Task 4); named layout presets (Task 4); the layout reshuffle with stub bottom panels (Task 4); explicitly deferred items (multi-scene projects, text-level find/replace, a general ECS undo system, tabbing the centre viewport, the terminal/editor/scripting themselves) — none of them touched by any task. Testing section's three verification approaches (EditorHistory self-test, capture-based layout check, manual menu-bar interaction) each map to a task.
- **Placeholder scan:** no TBDs; every step carries real code or an exact command.
- **Type consistency:** `EditorHistory::Push/Undo/Redo` return `GS::EntityId` everywhere they're used (Tasks 2 and 4); `PlaceEntityCommand`/`DeleteEntityCommand`/`EditFieldCommand<Component, Field>` constructor signatures match between Task 1's definition and Tasks 2/4's call sites; `g_EditorProjectPath`/`g_EditorProjectName`/`CreateEditorProject`/`OpenEditorProject`/`SaveEditorProject`/`SaveEditorProjectAs`/`RenameEditorProject` (Task 3) match their Task 4 call sites exactly; `g_EditorShellInstance`/`ResetToDefaultLayout` (Task 4) match.
- **Acyclic includes, checked explicitly:** `EditorHistory.h` → `EditorProject.h` only. `EditorSceneView.h` → `EditorProject.h`, and (new in Task 2) → `EditorHistory.h`. `EditorShell.h` → `EditorProject.h`, `EditorSceneView.h`, and (new in Task 2) → `EditorHistory.h`. `EditorMenuBar.h` → all four of the above. Nothing points back toward `EditorMenuBar.h`. No cycle.
