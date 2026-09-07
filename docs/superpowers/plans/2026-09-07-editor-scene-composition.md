# Editor Boot + Scene Composition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the app into an editable scene by default (building on the existing `EditorShell` docking layout), let a user place imported/preset meshes into it, save/load it, and open an existing demo's placed content as a starting scene — while every existing demo keeps running unchanged via `--demo`.

**Architecture:** Reuse, not rebuild. `GS::Scene`/`Entity`/`Components` (existing ECS) gains one component (`CameraComponent`) and one capability (`Save`/`Load`, in a hand-rolled tagged-text format matching the project's existing `prefabs.txt` convention — `GS::Json` is parse-only and gains no writer here). `Cube3D.h` already contains a working 3D scene editor in miniature — mesh entities, framebuffer picking, a translate gizmo, an outliner, an inspector, a free-fly camera — and that logic is promoted into a new `EditorSceneView` layer that operates on a project-wide `GS::Scene` instead of Cube3D's own. `EditorShell`'s existing "Assets" placeholder panel becomes real. A demo opts in to being opened as starting content via one new virtual on `DemoLayer`; only `Cube3D` implements it here.

**Tech Stack:** C++17, OpenGL via the existing `GS::Renderer`/`Framebuffer`/`Shader`/`Material` types, Dear ImGui (docking branch), the existing hand-rolled `GS::Scene` ECS.

**Spec:** `docs/superpowers/specs/2026-09-07-editor-scene-composition-design.md`

## Corrections found while planning (read before executing)

The spec named `SceneDemo` and `ModelDemo` as the first demos to make openable. Both turned out to be the wrong picks once the actual code was read:

- `SceneDemo.h` is entirely 2D (`OrthographicCamera`, `SpriteComponent`, `Renderer2D`) — it has no `MeshComponent` content at all.
- `ModelDemo.h` draws a glTF hierarchy by walking nodes directly (`DrawNode`/`Walk`); it never touches `GS::Scene`, so there is no ECS content to export.
- `Cube3D.h` is the demo that actually builds `MeshComponent`/`TransformComponent` entities in the ECS, and it already has framebuffer-based picking, a translate gizmo, an outliner list, and an inspector panel built on exactly those components. This plan promotes *that* code and makes Cube3D the (only) demo that implements the export hook. The spec's architecture (opt-in hook, blank-or-last scene boot, `--demo`/`--scene` flags, explicit-component save/load) is unchanged — only which demo proves it changed.

Mesh import is narrower than the spec implied, for a concrete reason: the ECS has no parent/child hierarchy component, and `GltfLoader` produces a node tree with per-node materials (`GltfMaterial`) that nothing bridges to `GS::Material` today. Flattening a glTF's hierarchy into one entity would either silently drop real structure or require building hierarchy support no one has asked for yet — both are scope creep for a foundation step. Import in this plan is **`.obj` only**, via `Mesh::Load` and `LoadMaterialsFor`'s existing `.mtl` handling (already proven end-to-end in `Cube3D.h`). glTF viewing stays exactly where it is, in `ModelDemo`; bringing it into the placeable-scene world is left for a later pass, once there is a reason to.

## Global Constraints

- Every new/changed piece of behavior gets a temporary self-test per this project's convention (`TestEnv/src/*Test.h`, called once from `TestApp`'s constructor, deleted once verified) — this project has no test framework and does not want one.
- `git grep TEMPORARY` must return nothing before the final task is considered done.
- Verify with `./gs.py build all` before the final task's commit — Release has caught things Debug has not, elsewhere in this project.
- No existing demo's behavior, capture, or replay may change. `--demo <name>` must produce byte-identical output to before this plan, for all 17 demos.
- Follow the project's comment convention: comments explain *why*, not what.

---

## Task 1: CameraComponent, mesh source paths, a mesh cache, and scene save/load

**Files:**
- Modify: `GS/src/GS/Scene/Components.h`
- Modify: `GS/src/GS/Scene/Scene.h`
- Modify: `GS/src/GS/Scene/Scene.cpp`
- Create: `GS/src/GS/Renderer/MeshCache.h`
- Create: `GS/src/GS/Renderer/MeshCache.cpp`
- Modify: `GS/src/GS.h`
- Create (temporary): `TestEnv/src/EditorSceneDataTest.h`
- Modify (temporary call site): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `GS::CameraComponent { float Fov = 45.0f; float NearClip = 0.1f; float FarClip = 1000.0f; bool Active = false; }`
- Produces: `GS::MeshComponent::SourcePath` (new `std::string` member, default `""`)
- Produces: `GS::MeshCache::Get(const std::string& path) -> std::shared_ptr<GS::Mesh>` (static)
- Produces: `GS::Scene::Save(const std::string& path) const -> bool`
- Produces: `GS::Scene::Load(const std::string& path) -> bool` (clears the scene first, on success or failure alike — a half-loaded scene is worse than an empty one)

- [ ] **Step 1: Add `CameraComponent` and `MeshComponent::SourcePath`**

In `GS/src/GS/Scene/Components.h`, add after `LightComponent`:

```cpp
	// A viewpoint an entity carries with it. Position and yaw/pitch are read
	// off TransformComponent -- there is no reason for a camera to have two
	// places to be -- so this is only the lens.
	struct CameraComponent
	{
		float Fov = 45.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;

		// At most one camera in a scene drives the runtime view. A second
		// entity with this set simply loses -- whichever is found first wins,
		// which is enough until something needs to switch cameras at runtime.
		bool Active = false;
	};
```

And add one field to `MeshComponent` (after `Geometry`):

```cpp
		// The cache key Geometry was resolved through -- "primitive:cube", or
		// a file path. Empty means this mesh was built ad hoc and has nothing
		// to reload from, which Scene::Save uses to skip it rather than write
		// a reference nothing can follow.
		std::string SourcePath;
```

- [ ] **Step 2: Add `MeshCache`**

Create `GS/src/GS/Renderer/MeshCache.h`:

```cpp
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

namespace GS {

	// One mesh per path, ever, for the run. A scene that places the same
	// preset shape or the same imported file on fifty entities should upload
	// its geometry once -- this is the thing both MeshComponent::SourcePath
	// and the placement panels resolve through, so "load" and "place" always
	// agree on what a given path means.
	//
	// Four names are not files: "primitive:cube", "primitive:sphere",
	// "primitive:plane" and "primitive:cylinder" build the matching
	// Mesh::Create* shape instead of touching disk, so a scene can reference
	// a preset the same way it references an imported one.
	class GS_API MeshCache
	{
	public:
		// Returns the cached mesh for `path`, building or loading it on first
		// request. Returns nullptr (and logs, via Mesh::Load's own logging)
		// for a path that names neither a primitive nor a loadable .obj -- a
		// missing mesh must not take the scene with it.
		static std::shared_ptr<Mesh> Get(const std::string& path);

		// Forgets everything cached. Only a test needs this -- normal use
		// loads a mesh once and shares it for the run.
		static void Clear();
	};

}
```

Create `GS/src/GS/Renderer/MeshCache.cpp`:

```cpp
#include "gspch.h"
#include "GS/Renderer/MeshCache.h"

namespace GS {

	static std::unordered_map<std::string, std::shared_ptr<Mesh>>& Store()
	{
		static std::unordered_map<std::string, std::shared_ptr<Mesh>> store;
		return store;
	}

	std::shared_ptr<Mesh> MeshCache::Get(const std::string& path)
	{
		auto& store = Store();

		auto existing = store.find(path);
		if (existing != store.end())
			return existing->second;

		Mesh* built = nullptr;

		if (path == "primitive:cube")
			built = Mesh::CreateCube();
		else if (path == "primitive:sphere")
			built = Mesh::CreateSphere();
		else if (path == "primitive:plane")
			built = Mesh::CreatePlane();
		else if (path == "primitive:cylinder")
			built = Mesh::CreateCylinder();
		else
			built = Mesh::Load(path);

		std::shared_ptr<Mesh> mesh(built);
		store[path] = mesh;

		return mesh;
	}

	void MeshCache::Clear()
	{
		Store().clear();
	}

}
```

- [ ] **Step 3: Wire `MeshCache.h` into the master include**

In `GS/src/GS.h`, under `// RENDERER ***************`, add `#include "GS/Renderer/MeshCache.h"` next to the other renderer includes (after `GltfLoader.h` reads naturally, since it is the newest asset-facing renderer header).

- [ ] **Step 4: Add `Scene::Save`/`Scene::Load`**

In `GS/src/GS/Scene/Scene.h`, add to the public section (after `StepPhysics`):

```cpp
		// --- Persistence --------------------------------------------------
		// A hand-written, explicit format -- there is no component
		// registration to walk generically, so this names exactly the
		// components a scene file is allowed to carry: Tag, Transform, Mesh
		// (by SourcePath, resolved through MeshCache) and Camera. Adding a
		// fifth persisted component means adding one more explicit case here,
		// the same way adding a demo means adding one more line to
		// DemoRegistry.h.
		bool Save(const std::string& path) const;

		// Clears the scene first, whether this succeeds or fails -- a scene
		// half-overwritten by a truncated file is a worse state than empty.
		bool Load(const std::string& path);
```

In `GS/src/GS/Scene/Scene.cpp`, add `#include "GS/Renderer/MeshCache.h"`, `#include "GS/Log.h"`, `#include <fstream>` and `#include <sstream>` at the top, and add at the end of the file, before the closing namespace brace:

```cpp
	static void WriteFloats(std::ofstream& out, std::initializer_list<float> values)
	{
		for (float v : values)
			out << ' ' << v;
	}

	bool Scene::Save(const std::string& path) const
	{
		std::ofstream out(path);
		if (!out)
		{
			GS_WARN("Scene::Save: could not write '{0}'", path);
			return false;
		}

		out << "egss-scene 1\n";

		Scene* self = const_cast<Scene*>(this);

		for (EntityId entity : m_Live)
		{
			out << "entity\n";

			if (TagComponent* tag = self->GetComponent<TagComponent>(entity))
				out << "tag " << tag->Name << "\n";

			if (TransformComponent* t = self->GetComponent<TransformComponent>(entity))
			{
				out << "transform";
				WriteFloats(out, { t->Position.x, t->Position.y, t->Position.z,
					t->Rotation.x, t->Rotation.y, t->Rotation.z,
					t->Scale.x, t->Scale.y, t->Scale.z });
				out << "\n";
			}

			// A mesh with no SourcePath was built ad hoc and has nothing to
			// reload from -- silently dropped rather than written as a
			// reference nothing can follow.
			if (MeshComponent* mesh = self->GetComponent<MeshComponent>(entity))
			{
				if (!mesh->SourcePath.empty())
				{
					out << "mesh " << mesh->SourcePath;
					WriteFloats(out, { mesh->Color.r, mesh->Color.g, mesh->Color.b, mesh->Color.a });
					out << ' ' << (mesh->Visible ? 1 : 0) << "\n";
				}
			}

			if (CameraComponent* camera = self->GetComponent<CameraComponent>(entity))
			{
				out << "camera";
				WriteFloats(out, { camera->Fov, camera->NearClip, camera->FarClip });
				out << ' ' << (camera->Active ? 1 : 0) << "\n";
			}
		}

		return true;
	}

	bool Scene::Load(const std::string& path)
	{
		std::ifstream in(path);
		if (!in)
		{
			GS_WARN("Scene::Load: could not read '{0}'", path);
			return false;
		}

		std::string tag;
		int version = 0;
		in >> tag >> version;

		if (tag != "egss-scene" || version != 1)
		{
			GS_WARN("Scene::Load: '{0}' is version {1}; this build reads 1", path, version);
			Clear();
			return false;
		}

		std::string line;
		std::getline(in, line);   // rest of the header line

		Clear();
		Entity current;

		while (std::getline(in, line))
		{
			std::istringstream fields(line);
			std::string kind;
			fields >> kind;

			if (kind == "entity")
			{
				current = CreateEntity();
			}
			else if (kind == "tag" && current)
			{
				std::string name;
				std::getline(fields, name);
				size_t from = name.find_first_not_of(' ');
				current.Get<TagComponent>()->Name = from == std::string::npos ? "" : name.substr(from);
			}
			else if (kind == "transform" && current)
			{
				TransformComponent t;
				fields >> t.Position.x >> t.Position.y >> t.Position.z
					>> t.Rotation.x >> t.Rotation.y >> t.Rotation.z
					>> t.Scale.x >> t.Scale.y >> t.Scale.z;
				*current.Get<TransformComponent>() = t;
			}
			else if (kind == "mesh" && current)
			{
				std::string sourcePath;
				fields >> sourcePath;

				MeshComponent mesh;
				mesh.SourcePath = sourcePath;
				fields >> mesh.Color.r >> mesh.Color.g >> mesh.Color.b >> mesh.Color.a;
				int visible = 1;
				fields >> visible;
				mesh.Visible = visible != 0;
				mesh.Geometry = MeshCache::Get(sourcePath);

				current.Add<MeshComponent>(mesh);
			}
			else if (kind == "camera" && current)
			{
				CameraComponent camera;
				int active = 0;
				fields >> camera.Fov >> camera.NearClip >> camera.FarClip >> active;
				camera.Active = active != 0;
				current.Add<CameraComponent>(camera);
			}
		}

		return true;
	}
```

- [ ] **Step 5: Write the temporary self-test**

Create `TestEnv/src/EditorSceneDataTest.h`:

```cpp
// TEMPORARY -- delete after verifying Scene::Save/Load and MeshCache.
#pragma once
#include <GS.h>

namespace EditorSceneDataTest {

	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		GS_TRACE("EditorSceneDataTest:");

		GS::Scene scene;

		GS::Entity a = scene.CreateEntity("Cube A");
		a.Get<GS::TransformComponent>()->Position = { 1.0f, 2.0f, 3.0f };
		GS::MeshComponent meshA;
		meshA.SourcePath = "primitive:cube";
		meshA.Geometry = GS::MeshCache::Get("primitive:cube");
		meshA.Color = { 0.2f, 0.4f, 0.6f, 1.0f };
		a.Add<GS::MeshComponent>(meshA);

		GS::Entity b = scene.CreateEntity("Cube B");
		GS::MeshComponent meshB;
		meshB.SourcePath = "primitive:cube";
		meshB.Geometry = GS::MeshCache::Get("primitive:cube");
		b.Add<GS::MeshComponent>(meshB);

		GS::Entity camera = scene.CreateEntity("Main Camera");
		camera.Add<GS::CameraComponent>({ 60.0f, 0.05f, 500.0f, true });

		Check(meshA.Geometry == meshB.Geometry,
			"two entities requesting the same primitive share one Mesh");

		Check(scene.Save("editor_scene_test.tmp.txt"), "Save reports success");

		scene.Clear();
		Check(scene.GetEntityCount() == 0, "Clear empties the scene");

		Check(scene.Load("editor_scene_test.tmp.txt"), "Load reports success");
		Check(scene.GetEntityCount() == 3, "all three entities came back");

		bool foundCamera = false;
		auto& cameras = scene.View<GS::CameraComponent>();
		for (size_t i = 0; i < cameras.Size(); i++)
		{
			foundCamera = true;
			Check(cameras.Components()[i].Fov == 60.0f, "camera fov round-tripped");
			Check(cameras.Components()[i].Active, "camera active flag round-tripped");
		}
		Check(foundCamera, "a CameraComponent came back at all");

		auto& meshes = scene.View<GS::MeshComponent>();
		Check(meshes.Size() == 2, "both meshes came back");
		if (meshes.Size() == 2)
			Check(meshes.Components()[0].Geometry == meshes.Components()[1].Geometry,
				"reloaded meshes still share one cached Mesh, not two copies");

		GS_TRACE("EditorSceneDataTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
```

- [ ] **Step 6: Call it once from `TestApp`, build, and read the log**

In `TestEnv/src/TestApp.cpp`, temporarily add `#include "EditorSceneDataTest.h"` and, as the first line of `TestEnv`'s constructor, `EditorSceneDataTest::Run();`.

Run: `./gs.py build && ./gs.py run`
Expected: the log prints `EditorSceneDataTest:` followed by nine `ok` lines and `9 passed, 0 failed`, before the window shows anything.

- [ ] **Step 7: Remove the temporary call, keep the header until Task 5's cleanup**

Remove the `#include` and the `EditorSceneDataTest::Run();` line from `TestApp.cpp` (the header file itself is deleted in Task 5's final cleanup, once every task that might want to rerun it is done).

- [ ] **Step 8: Commit**

```bash
git add GS/src/GS/Scene/Components.h GS/src/GS/Scene/Scene.h \
  GS/src/GS/Scene/Scene.cpp GS/src/GS/Renderer/MeshCache.h \
  GS/src/GS/Renderer/MeshCache.cpp GS/src/GS.h
git commit -m "$(cat <<'EOF'
Add CameraComponent, a mesh cache, and scene save/load

Scene::Save/Load use a hand-written tagged text format, matching the
project's existing prefab persistence -- GS::Json is parse-only and
gains no writer here. MeshCache gives import and preset placement one
shared, path-keyed cache so a scene references geometry the same way
whichever route it arrived by.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Gkj1PEW1Vs7yxuP1X1KyZz
EOF
)"
```

---

## Task 2: Boot flow — the editor becomes the default, demos stay reachable

**Files:**
- Modify: `TestEnv/src/Demo.h`
- Modify: `TestEnv/src/DemoRegistry.h`
- Modify: `TestEnv/src/DemoSelector.h`
- Create: `TestEnv/src/EditorProject.h`
- Modify: `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `GS::Scene::Save/Load` (Task 1)
- Produces: `g_EditorScene` (global `GS::Scene`, declared in `EditorProject.h`)
- Produces: `g_EditorScenePath` (global `std::string`, empty when the current scene has never been saved)
- Produces: `OpenEditorScene(const std::string& path) -> bool`, `SaveEditorScene(const std::string& path) -> bool` (both update `g_EditorScenePath` and the last-scene prefs file on success)
- Produces: `LoadEditorProjectFromCommandLine()` (called once from `TestEnv`'s constructor)
- Produces: `DemoEntry::CanOpenInEditor` (new `bool` field, `false` on every existing row)
- Produces: `s_DemoInstances[s_DemoCount]` (array of `DemoLayer*`, filled by `PushAllDemos`, so later code can reach an already-attached demo's live state)
- Produces: `DemoLayer::OnExportToScene(GS::Scene&)` (new virtual, no-op default)
- Changes: `g_ActiveDemo`'s default becomes `InvalidDemo` instead of `16`

- [ ] **Step 1: Give `DemoEntry` an opt-in flag and keep every instance reachable**

In `TestEnv/src/DemoRegistry.h`, add a field to `DemoEntry`:

```cpp
struct DemoEntry
{
	const char* Folder;
	const char* Name;
	const char* ShortName;
	DemoLayer* (*Create)();

	// Whether this demo's placed content (meshes, transforms, camera) can be
	// opened as a starting scene in the editor -- see DemoLayer::OnExportToScene.
	// False for every demo that has never implemented the hook, which is
	// every demo but Cube3D as of this line.
	bool CanOpenInEditor = false;
};
```

Add `CanOpenInEditor: false` is the default, so no existing row needs editing yet (aggregate initialization leaves a trailing field at its default). Confirm this compiles as-is before moving on -- it should, since every `s_Demos[]` row already uses designated positional initialization without naming `CanOpenInEditor`.

Then, in the same file, change `PushAllDemos` to remember every instance it creates:

```cpp
// One pointer per demo, filled by PushAllDemos. A demo is attached (OnAttach
// has run) the moment this is populated, which is what lets the editor ask an
// already-running demo to export its content instead of constructing a second,
// never-attached instance whose OnDemoAttach (where the content is actually
// built) never fires.
inline DemoLayer* s_DemoInstances[s_DemoCount] = {};

inline void PushAllDemos(GS::Application& app)
{
	SelectDemoFromCommandLine();

	for (int i = 0; i < s_DemoCount; i++)
	{
		DemoLayer* demo = s_Demos[i].Create();
		demo->SetDemoId(i);
		s_DemoInstances[i] = demo;
		app.PushLayer(demo);
	}

	// unchanged below this line
```

- [ ] **Step 2: Add the export hook to `DemoLayer`**

In `TestEnv/src/Demo.h`, add alongside the other `OnDemo*` virtuals:

```cpp
	// Describes this demo's placed content -- meshes, transforms, a camera --
	// in terms of the destination scene's own components, so the editor can
	// open it as a starting point. Default does nothing, which is also what
	// keeps a demo out of the editor's "Open" list; see DemoEntry::CanOpenInEditor.
	// Deliberately not part of OnDemoAttach: exporting happens once, on
	// request, into a *different* scene than the one the demo is still
	// running against.
	virtual void OnExportToScene(GS::Scene& scene) { (void)scene; }
```

- [ ] **Step 3: Flip the default boot target, and stop indexing `s_Demos` with it blindly**

In `TestEnv/src/Demo.h`, change:

```cpp
inline DemoId g_ActiveDemo = 16;
```

to:

```cpp
// InvalidDemo means "the editor, not a demo" -- the new default. --demo
// still boots straight into a single demo exactly as before; see
// SelectDemoFromCommandLine in DemoRegistry.h.
inline DemoId g_ActiveDemo = InvalidDemo;
```

`InvalidDemo` is already defined a few lines above as `-1`; no other change needed here since `DemoLayer::IsActive()` already compares `m_DemoId == g_ActiveDemo`, which is simply never true for any real demo while `g_ActiveDemo` is `InvalidDemo` -- every demo stays inactive, which is exactly the state the editor's own view (Task 3) needs to be the only thing drawing.

- [ ] **Step 4: Fix `DemoSelector.h`'s now-reachable out-of-bounds read**

`s_Demos[g_ActiveDemo]` on the line computing `holdsActive` is undefined behaviour once `g_ActiveDemo` can be `-1` -- it could not happen before this task, because `g_ActiveDemo` was always a valid index. In `TestEnv/src/DemoSelector.h`, change:

```cpp
			bool holdsActive = std::strcmp(s_Demos[g_ActiveDemo].Folder, name) == 0;
```

to:

```cpp
			// No folder holds "the active demo" while the editor itself is
			// active -- g_ActiveDemo is InvalidDemo, not an index into s_Demos.
			bool holdsActive = g_ActiveDemo != InvalidDemo
				&& std::strcmp(s_Demos[g_ActiveDemo].Folder, name) == 0;
```

And add a way back to the editor. After the `ImGui::Combo` block, add:

```cpp
			ImGui::SameLine();
			if (ImGui::Button("Editor"))
				g_ActiveDemo = InvalidDemo;
```

(`ImGui::Combo`'s own out-of-range handling of `current == -1` is standard Dear ImGui behaviour -- it shows an empty preview rather than reading out of bounds -- so the combo itself needs no change.)

- [ ] **Step 5: Add `EditorProject.h`**

Create `TestEnv/src/EditorProject.h`:

```cpp
#pragma once

// The project the editor has open: one GS::Scene, a path (empty if it has
// never been saved), and enough bookkeeping to reopen the same one next run.
//
// Kept separate from EditorShell.h on purpose -- that file lays out panels,
// this one owns what they show. A layout change should never risk the scene.

#include <GS.h>

inline GS::Scene g_EditorScene;
inline std::string g_EditorScenePath;

// Not checked in -- generated beside the executable, the same way imgui.ini
// and profile.json are. Remembers the last scene across runs so relaunching
// the editor picks up where you left off rather than opening blank.
inline const char* EditorLastScenePath() { return "editor_last_scene.txt"; }

inline bool OpenEditorScene(const std::string& path)
{
	if (!g_EditorScene.Load(path))
		return false;

	g_EditorScenePath = path;

	std::ofstream last(EditorLastScenePath());
	if (last)
		last << path;

	return true;
}

inline bool SaveEditorScene(const std::string& path)
{
	if (!g_EditorScene.Save(path))
		return false;

	g_EditorScenePath = path;

	std::ofstream last(EditorLastScenePath());
	if (last)
		last << path;

	return true;
}

// --demo/--scene are mutually exclusive by construction: SelectDemoFromCommandLine
// (DemoRegistry.h) only ever moves g_ActiveDemo off InvalidDemo when --demo is
// given, and this function only runs the editor's own boot logic, so whichever
// flag is present decides which of the two ever does anything observable.
inline void LoadEditorProjectFromCommandLine()
{
	const std::vector<std::string>& arguments = GS::Application::GetCommandLine();

	for (size_t i = 1; i + 1 < arguments.size(); i++)
	{
		if (arguments[i] != "--scene")
			continue;

		if (!OpenEditorScene(arguments[i + 1]))
			GS_WARN("--scene '{0}' could not be opened; starting blank", arguments[i + 1]);

		return;
	}

	// No --scene: reopen whatever was open last, if anything. A missing or
	// unreadable last-scene file is not a warning -- the very first run has
	// neither, and that is not a fault.
	std::ifstream last(EditorLastScenePath());
	std::string path;
	if (last && std::getline(last, path) && !path.empty())
		OpenEditorScene(path);
}
```

- [ ] **Step 6: Call it from `TestApp`**

In `TestEnv/src/TestApp.cpp`, add `#include "EditorProject.h"` and call `LoadEditorProjectFromCommandLine();` in `TestEnv`'s constructor, before `PushAllDemos(*this)` (so that if a demo's warmup or attach logic ever wants to know the editor's state, it is already settled -- mirroring why `DemoWarmup` is pushed before the demos).

- [ ] **Step 7: Build and manually verify the flip**

Run: `./gs.py build && ./gs.py run`
Expected: the window opens with every docked panel visible and an empty central viewport (nothing draws there yet -- Task 3 adds the content), rather than TerrainLab. Then:

Run: `./gs.py run -- --demo Cube3D`
Expected: Cube3D boots exactly as it did before this task (same controls, same scene) -- unaffected by the default flip.

- [ ] **Step 8: Commit**

```bash
git add TestEnv/src/Demo.h TestEnv/src/DemoRegistry.h TestEnv/src/DemoSelector.h \
  TestEnv/src/EditorProject.h TestEnv/src/TestApp.cpp
git commit -m "$(cat <<'EOF'
Boot into the editor by default; --demo still boots a single demo

g_ActiveDemo's default becomes InvalidDemo rather than an index, which
made an existing DemoSelector array access reachable out of bounds --
fixed alongside the flip, not left for whoever hits it next.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Gkj1PEW1Vs7yxuP1X1KyZz
EOF
)"
```

---

## Task 3: EditorSceneView — render, pick, gizmo, outliner and inspector for the project scene

**Files:**
- Create: `TestEnv/src/EditorSceneView.h`
- Modify: `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `g_EditorScene` (Task 2), `g_Viewport`/`g_DemoDock` (existing, `Demo.h`), `GS::CameraComponent` (Task 1)
- Produces: `class EditorSceneView : public GS::Layer`, pushed once from `TestApp`
- Produces: `inline EditorSceneView* g_EditorSceneView` (set in `OnAttach`, so Task 4's Assets panel can select an entity it just created)
- Produces: `EditorSceneView::Select(GS::EntityId entity)` (public method)

- [ ] **Step 1: Create `EditorSceneView.h`, adapted from `Cube3D`'s scene/pick/gizmo/camera code**

Create `TestEnv/src/EditorSceneView.h`:

```cpp
#pragma once

// The editor's own view of g_EditorScene: a free-fly camera, framebuffer
// picking, a translate gizmo, an outliner and an inspector.
//
// Every piece of this already existed, proven, in Cube3D.h -- this is that
// code with m_Scene replaced by the shared g_EditorScene and the
// audio/acoustics it was tangled up with left behind. Nothing here is a new
// idea; it is Cube3D's editing half, promoted out of one demo into the shell.
//
// Active exactly when no demo is: g_ActiveDemo == InvalidDemo. A demo forced
// active by --demo owns the whole window instead, unchanged from before this
// existed.

#include <GS.h>
#include <imgui.h>

#include "Demo.h"
#include "EditorProject.h"

class EditorSceneView : public GS::Layer
{
public:
	EditorSceneView()
		: Layer("EditorSceneView"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 1000.0f)
	{
	}

	void OnAttach() override
	{
		g_EditorSceneView = this;

		m_Camera.SetPosition({ 0.0f, 1.6f, 6.0f });
		m_Camera.SetRotation(-90.0f, -12.0f);

		BuildTarget();
		BuildShader();
	}

	bool IsActive() const { return g_ActiveDemo == InvalidDemo; }

	void Select(GS::EntityId entity) { m_Selected = entity; }
	GS::EntityId GetSelected() const { return m_Selected; }

	void OnUpdate(GS::Timestep ts) override
	{
		if (!IsActive())
			return;

		if (g_Viewport.Valid())
			GS::RenderCommand::SetViewport((unsigned int)g_Viewport.X,
				(unsigned int)g_Viewport.Y, (unsigned int)g_Viewport.Width,
				(unsigned int)g_Viewport.Height);

		MoveCamera(ts);
		ResizeTarget();
		UpdateGizmo();

		m_Framebuffer->Bind();

		GS::RenderCommand::SetClearColor({ 0.10f, 0.11f, 0.13f, 1.0f });
		GS::RenderCommand::Clear();
		m_Framebuffer->ClearAttachment(1, -1);

		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		RenderMeshes();
		GS::RenderCommand::SetCullFace(GS::CullFace::None);

		GS::Renderer2D::BeginScene(m_Camera);
		DrawSelectionBox();
		if (m_ShowGizmo)
			DrawGizmo();
		GS::Renderer2D::EndScene();

		ReadHoveredEntity();

		m_Framebuffer->Unbind();

		BlitToWindow();

		if (g_Viewport.Valid())
		{
			GS::Window& window = GS::Application::Get().GetWindow();
			GS::RenderCommand::SetViewport(0, 0, window.GetWidth(), window.GetHeight());
		}
	}

	void OnEvent(GS::Event& e) override
	{
		if (!IsActive())
			return;

		GS::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<GS::WindowResizeEvent>([this](GS::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
				m_Camera.SetAspectRatio((float)e.GetWidth() / (float)e.GetHeight());
			return false;
		});

		dispatcher.Dispatch<GS::MouseButtonPressedEvent>([this](GS::MouseButtonPressedEvent& e)
		{
			// A click on a hovered gizmo handle must grab it, not change the
			// selection -- checking m_HoverAxis rather than m_DragAxis is what
			// distinguishes the two, since m_DragAxis is not set until
			// UpdateGizmo sees the button on the *next* poll.
			if (e.GetMouseButton() == GS_MOUSE_BUTTON_LEFT
				&& !ImGui::GetIO().WantCaptureMouse && m_HoverAxis < 0)
				m_Selected = m_Hovered;
			return false;
		});

		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;
			if (e.GetKeyCode() == GS_KEY_DELETE && g_EditorScene.IsValid(m_Selected))
			{
				g_EditorScene.DestroyEntity(m_Selected);
				m_Selected = GS::InvalidEntity;
			}
			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (!IsActive())
			return;

		ImGui::Begin("Outliner");

		ImGui::Text("Entities: %zu", g_EditorScene.GetEntityCount());
		ImGui::BeginChild("hierarchy", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		for (GS::EntityId entity : g_EditorScene.GetEntities())
		{
			auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(entity);
			if (!tag)
				continue;

			ImGui::PushID((int)entity);
			if (ImGui::Selectable(tag->Name.c_str(), entity == m_Selected))
				m_Selected = entity;
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::End();

		ImGui::Begin("Inspector");

		if (!g_EditorScene.IsValid(m_Selected))
		{
			ImGui::TextDisabled("Nothing selected.");
			ImGui::End();
			return;
		}

		if (auto* tag = g_EditorScene.GetComponent<GS::TagComponent>(m_Selected))
			ImGui::Text("%s  (id %u)", tag->Name.c_str(), m_Selected);

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

		ImGui::End();
	}

private:
	void BuildTarget()
	{
		GS::Window& window = GS::Application::Get().GetWindow();

		GS::FramebufferSpecification spec;
		spec.Width = window.GetWidth() > 0 ? window.GetWidth() : 1280;
		spec.Height = window.GetHeight() > 0 ? window.GetHeight() : 720;
		spec.Attachments = {
			GS::FramebufferTextureFormat::RGBA8,
			GS::FramebufferTextureFormat::RED_INTEGER,
			GS::FramebufferTextureFormat::DEPTH24STENCIL8
		};

		m_Framebuffer.reset(GS::Framebuffer::Create(spec));
		m_BlitCamera.SetProjection(-1.0f, 1.0f, -1.0f, 1.0f);
	}

	// Same shader Cube3D built -- a lit, textured mesh with a picking output
	// -- minus the point light this view has no gizmo-draggable light for.
	// Ambient-only lighting is enough to tell shapes apart; a light entity is
	// exactly the kind of thing a scene *places* rather than the view owning
	// one permanently.
	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;
			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;
			out vec3 v_Normal;
			void main()
			{
				v_Normal = mat3(u_Transform) * a_Normal;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;
			layout(location = 1) out int entityID;
			in vec3 v_Normal;
			uniform vec4 u_Color;
			uniform int u_EntityID;
			void main()
			{
				float shade = 0.5 + 0.5 * max(dot(normalize(v_Normal), normalize(vec3(0.4, 1.0, 0.6))), 0.0);
				color = vec4(u_Color.rgb * shade, u_Color.a);
				entityID = u_EntityID;
			}
		)";

		m_Shader.reset(GS::Shader::Create("EditorSceneView", vertexSrc, fragmentSrc));
		m_SceneMaterial = GS::Material::Create(m_Shader);
	}

	void RenderMeshes()
	{
		GS::Renderer::BeginScene(m_Camera);

		auto& meshes = g_EditorScene.View<GS::MeshComponent>();

		for (size_t i = 0; i < meshes.Size(); i++)
		{
			GS::MeshComponent& mesh = meshes.Components()[i];
			if (!mesh.Visible || !mesh.Geometry)
				continue;

			GS::EntityId entity = meshes.Owner(i);
			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			if (!transform)
				continue;

			const std::vector<GS::Submesh>& submeshes = mesh.Geometry->GetSubmeshes();
			if (mesh.Materials.size() < submeshes.size())
				mesh.Materials.resize(submeshes.size());

			for (size_t s = 0; s < submeshes.size(); s++)
			{
				if (!mesh.Materials[s])
					mesh.Materials[s] = GS::Material::CreateInstance(m_SceneMaterial);

				if (!mesh.MaterialsFromFile)
					mesh.Materials[s]->Set("u_Color", mesh.Color);

				mesh.Materials[s]->Set("u_EntityID", (int)GS::EntityIds::Index(entity));

				GS::Renderer::SubmitSubmesh(mesh.Materials[s], mesh.Geometry,
					(unsigned int)s, transform->GetTransform());
			}
		}

		GS::Renderer::EndScene();
	}

	void ResizeTarget()
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		if (window.GetWidth() > 0 && window.GetHeight() > 0 &&
			(spec.Width != window.GetWidth() || spec.Height != window.GetHeight()))
			m_Framebuffer->Resize(window.GetWidth(), window.GetHeight());
	}

	void BlitToWindow()
	{
		unsigned int handle = m_Framebuffer->GetColorAttachmentRendererID(0);
		if (!m_ColorAttachment || m_ColorHandle != handle)
		{
			m_ColorAttachment.reset(GS::Texture2D::CreateFromHandle(handle,
				m_Framebuffer->GetSpecification().Width,
				m_Framebuffer->GetSpecification().Height));
			m_ColorHandle = handle;
		}

		GS::RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		GS::RenderCommand::Clear();

		GS::Renderer2D::BeginScene(m_BlitCamera);
		GS::Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(2.0f), m_ColorAttachment);
		GS::Renderer2D::EndScene();
	}

	void ReadHoveredEntity()
	{
		m_Hovered = GS::InvalidEntity;

		if (ImGui::GetIO().WantCaptureMouse || m_DragAxis >= 0)
			return;
		if (GS::Application::Get().IsUIHidden())
			return;

		auto [mouseX, mouseY] = GS::Input::GetMousePosition();
		const GS::FramebufferSpecification& spec = m_Framebuffer->GetSpecification();

		int x = (int)mouseX;
		int y = (int)((float)spec.Height - mouseY);
		if (x < 0 || y < 0 || x >= (int)spec.Width || y >= (int)spec.Height)
			return;

		int slot = m_Framebuffer->ReadPixel(1, x, y);
		if (slot < 0)
			return;

		m_Hovered = g_EditorScene.EntityAtIndex((unsigned int)slot);
	}

	void DrawSelectionBox()
	{
		for (int pass = 0; pass < 2; pass++)
		{
			GS::EntityId entity = (pass == 0) ? m_Hovered : m_Selected;
			if (!g_EditorScene.IsValid(entity) || (pass == 0 && entity == m_Selected))
				continue;

			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(entity);
			auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(entity);
			if (!transform || !mesh || !mesh->Geometry)
				continue;

			glm::vec4 color = (pass == 0)
				? glm::vec4(0.45f, 0.85f, 1.0f, 1.0f)
				: glm::vec4(1.00f, 0.85f, 0.3f, 1.0f);

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
		}
	}

	// Returns null when nothing is selected, which is why every caller checks
	// -- there is no light to fall back to dragging here, unlike Cube3D's own
	// version of this function.
	glm::vec3* GizmoPosition()
	{
		auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(m_Selected);
		return transform ? &transform->Position : nullptr;
	}

	bool WorldToScreen(const glm::vec3& world, glm::vec2& outScreen) const
	{
		GS::Window& window = GS::Application::Get().GetWindow();

		glm::vec4 clip = m_Camera.GetViewProjectionMatrix() * glm::vec4(world, 1.0f);
		if (clip.w <= 0.0001f)
			return false;

		glm::vec3 ndc = glm::vec3(clip) / clip.w;
		outScreen = { (ndc.x * 0.5f + 0.5f) * (float)window.GetWidth(),
			(1.0f - (ndc.y * 0.5f + 0.5f)) * (float)window.GetHeight() };
		return true;
	}

	// Pixels -> a ray in world space: the near and far points that project to
	// this pixel, which the perspective divide makes different.
	void ScreenRay(const glm::vec2& mouse, glm::vec3& outOrigin, glm::vec3& outDirection) const
	{
		GS::Window& window = GS::Application::Get().GetWindow();
		float width = (float)window.GetWidth();
		float height = (float)window.GetHeight();

		float x = (mouse.x / width) * 2.0f - 1.0f;
		float y = 1.0f - (mouse.y / height) * 2.0f;

		glm::mat4 inverse = glm::inverse(m_Camera.GetViewProjectionMatrix());

		glm::vec4 nearPoint = inverse * glm::vec4(x, y, -1.0f, 1.0f);
		glm::vec4 farPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);
		nearPoint /= nearPoint.w;
		farPoint /= farPoint.w;

		outOrigin = glm::vec3(nearPoint);
		outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
	}

	// How far along `axis` the point nearest the cursor ray sits -- the heart
	// of axis dragging, collapsing a 3D pick down to one number.
	static bool ClosestPointOnAxis(const glm::vec3& axisOrigin, const glm::vec3& axisDirection,
		const glm::vec3& rayOrigin, const glm::vec3& rayDirection, float& outT)
	{
		glm::vec3 between = axisOrigin - rayOrigin;

		float a = glm::dot(axisDirection, axisDirection);
		float b = glm::dot(axisDirection, rayDirection);
		float c = glm::dot(rayDirection, rayDirection);
		float d = glm::dot(axisDirection, between);
		float e = glm::dot(rayDirection, between);

		float denominator = a * c - b * b;
		if (std::abs(denominator) < 0.00001f)   // looking straight down the axis
			return false;

		outT = (b * e - c * d) / denominator;
		return true;
	}

	float AxisScreenDistance(int axis, const glm::vec2& mouse)
	{
		glm::vec3* origin = GizmoPosition();
		if (!origin)
			return std::numeric_limits<float>::max();

		glm::vec3 direction(0.0f);
		direction[axis] = 1.0f;

		glm::vec2 a, b;
		if (!WorldToScreen(*origin, a) || !WorldToScreen(*origin + direction * m_GizmoLength, b))
			return std::numeric_limits<float>::max();

		glm::vec2 segment = b - a;
		float lengthSquared = glm::dot(segment, segment);
		if (lengthSquared < 0.0001f)
			return glm::length(mouse - a);

		float t = glm::clamp(glm::dot(mouse - a, segment) / lengthSquared, 0.0f, 1.0f);
		return glm::length(mouse - (a + segment * t));
	}

	void UpdateGizmo()
	{
		glm::vec2 mouse = { GS::Input::GetMousePosition().first, GS::Input::GetMousePosition().second };

		bool down = GS::Input::IsMouseButtonPressed(GS_MOUSE_BUTTON_LEFT)
			&& !ImGui::GetIO().WantCaptureMouse;

		// A grab needs the button to go *down* while over a handle, not
		// merely to be held -- polling "is it held" would grab whatever the
		// cursor is near the instant a held button is first seen.
		bool justPressed = down && !m_MouseDownLastFrame;
		m_MouseDownLastFrame = down;

		if (!down)
		{
			m_DragAxis = -1;
			m_HoverAxis = -1;

			for (int axis = 0; axis < 3; axis++)
				if (AxisScreenDistance(axis, mouse) < m_GizmoPickPixels)
				{
					m_HoverAxis = axis;
					break;
				}
			return;
		}

		glm::vec3 rayOrigin, rayDirection;
		ScreenRay(mouse, rayOrigin, rayDirection);

		if (m_DragAxis < 0)
		{
			if (m_HoverAxis < 0 || !justPressed)
				return;

			glm::vec3* target = GizmoPosition();
			if (!target)
				return;

			glm::vec3 axisDirection(0.0f);
			axisDirection[m_HoverAxis] = 1.0f;

			float t;
			if (!ClosestPointOnAxis(*target, axisDirection, rayOrigin, rayDirection, t))
				return;

			m_DragAxis = m_HoverAxis;
			m_DragStartT = t;
			m_DragStartPosition = *target;
			return;
		}

		glm::vec3 axisDirection(0.0f);
		axisDirection[m_DragAxis] = 1.0f;

		float t;
		if (!ClosestPointOnAxis(m_DragStartPosition, axisDirection, rayOrigin, rayDirection, t))
			return;

		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		// Relative to where it was grabbed, so the object does not snap its
		// origin to the cursor.
		*target = m_DragStartPosition + axisDirection * (t - m_DragStartT);
	}

	void DrawGizmo()
	{
		glm::vec3* target = GizmoPosition();
		if (!target)
			return;

		glm::vec3 origin = *target;

		const glm::vec4 axisColors[3] = {
			{ 1.0f, 0.25f, 0.25f, 1.0f }, { 0.30f, 1.0f, 0.35f, 1.0f }, { 0.35f, 0.55f, 1.0f, 1.0f }
		};

		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 direction(0.0f);
			direction[axis] = 1.0f;

			glm::vec4 color = axisColors[axis];
			if (axis == m_DragAxis || (m_DragAxis < 0 && axis == m_HoverAxis))
				color = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);

			glm::vec3 tip = origin + direction * m_GizmoLength;
			GS::Renderer2D::DrawLine(origin, tip, color);

			// A little cross at the tip, so the end of the handle is visible
			// even when the line is nearly edge-on to the camera.
			glm::vec3 a(0.0f), b(0.0f);
			a[(axis + 1) % 3] = 0.06f;
			b[(axis + 2) % 3] = 0.06f;
			GS::Renderer2D::DrawLine(tip - a, tip + a, color);
			GS::Renderer2D::DrawLine(tip - b, tip + b, color);
		}
	}

	// WASD + Q/E + middle-drag look, exactly as Cube3D's fly camera -- the
	// editor needs to move around a scene for the same reasons a demo does.
	void MoveCamera(GS::Timestep ts)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		glm::vec3 position = m_Camera.GetPosition();
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		float move = m_MoveSpeed * ts;

		if (GS::Input::IsKeyPressed(GS_KEY_W)) position += m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_D)) position += m_Camera.GetRight() * move;
		if (GS::Input::IsKeyPressed(GS_KEY_E)) position.y += move;
		if (GS::Input::IsKeyPressed(GS_KEY_Q)) position.y -= move;

		glm::vec2 mouse = { GS::Input::GetMousePosition().first, GS::Input::GetMousePosition().second };
		glm::vec2 delta = mouse - m_PreviousMouse;
		m_PreviousMouse = mouse;

		if (GS::Input::IsMouseButtonPressed(GS_MOUSE_BUTTON_MIDDLE) && !ImGui::GetIO().WantCaptureMouse)
		{
			yaw += delta.x * m_MouseLookSensitivity;
			pitch -= delta.y * m_MouseLookSensitivity;
		}

		m_Camera.SetPosition(position);
		m_Camera.SetRotation(yaw, pitch);
	}

private:
	GS::PerspectiveCamera m_Camera;
	GS::OrthographicCamera m_BlitCamera{ -1.0f, 1.0f, -1.0f, 1.0f };

	std::shared_ptr<GS::Framebuffer> m_Framebuffer;
	std::shared_ptr<GS::Texture2D> m_ColorAttachment;
	unsigned int m_ColorHandle = 0;

	std::shared_ptr<GS::Shader> m_Shader;
	std::shared_ptr<GS::Material> m_SceneMaterial;

	GS::EntityId m_Selected = GS::InvalidEntity;
	GS::EntityId m_Hovered = GS::InvalidEntity;

	bool m_ShowGizmo = true;
	int m_DragAxis = -1;
	int m_HoverAxis = -1;
	bool m_MouseDownLastFrame = false;
	float m_DragStartT = 0.0f;
	glm::vec3 m_DragStartPosition{ 0.0f };
	float m_GizmoLength = 1.0f;
	float m_GizmoPickPixels = 12.0f;

	glm::vec2 m_PreviousMouse{ 0.0f, 0.0f };
	float m_MoveSpeed = 3.0f;
	float m_MouseLookSensitivity = 0.18f;
};

inline EditorSceneView* g_EditorSceneView = nullptr;
```

- [ ] **Step 2: Push it from `TestApp`**

In `TestEnv/src/TestApp.cpp`, add `#include "EditorSceneView.h"` and `PushLayer(new EditorSceneView());` immediately after `PushLayer(new EditorShell());` and before `PushAllDemos(*this);` -- same reasoning as `EditorShell`: ImGui runs in layer order and this view's own panels (Outliner, Inspector) should be laid out consistently with the rest of the shell's docking.

- [ ] **Step 3: Manually verify**

Run: `./gs.py build && ./gs.py run`
Expected: the app opens into the editor (per Task 2), with an "Outliner" panel (empty) and an "Inspector" panel ("Nothing selected.") now visible and dockable, and the central viewport clears to a plain dark colour instead of whatever it did before. WASD/QE/middle-drag move the camera. No crash, no console errors.

Run: `./gs.py run -- --demo Physics3D`
Expected: unaffected -- Physics3D boots and behaves exactly as before this task, since `EditorSceneView::IsActive()` is false whenever a demo is forced.

- [ ] **Step 4: Commit**

```bash
git add TestEnv/src/EditorSceneView.h TestEnv/src/TestApp.cpp
git commit -m "$(cat <<'EOF'
Add EditorSceneView: Cube3D's picking, gizmo and inspector, generalised

Promoted rather than rewritten -- the render/pick/gizmo/camera code is
Cube3D's own, adapted to draw g_EditorScene instead of one demo's
private scene, with the audio and acoustics it was tangled up with
left where they are.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Gkj1PEW1Vs7yxuP1X1KyZz
EOF
)"
```

---

## Task 4: A real Assets panel — import, preset shapes, save/load, and opening a demo's content

**Files:**
- Modify: `TestEnv/src/EditorShell.h`

**Interfaces:**
- Consumes: `GS::MeshCache::Get` (Task 1), `g_EditorScene`/`OpenEditorScene`/`SaveEditorScene`/`g_EditorScenePath` (Task 2), `s_DemoInstances`/`DemoEntry::CanOpenInEditor` (Task 2), `g_EditorSceneView->Select` (Task 3)

- [ ] **Step 1: Replace the placeholder "Assets" panel body**

In `TestEnv/src/EditorShell.h`, add `#include "EditorProject.h"` and `#include "EditorSceneView.h"` near the top, and a small helper plus a text buffer as private members of `EditorShell` (add a `private:` section if there is not already a convenient one -- there is, right above `bool m_Built = false;`):

```cpp
	char m_ImportPath[256] = "assets/models/";
	char m_SavePath[256] = "scene.txt";
```

Then replace the whole current body:

```cpp
		ImGui::Begin("Assets");
		ImGui::TextDisabled("Nothing here yet.");
		ImGui::TextDisabled("Docked bottom-centre, ready for a texture browser");
		ImGui::TextDisabled("or whatever the next thing needs a pane for.");
		ImGui::End();
```

with:

```cpp
		ImGui::Begin("Assets");

		ImGui::SeparatorText("Scene");
		ImGui::TextDisabled("%s", g_EditorScenePath.empty() ? "(unsaved)" : g_EditorScenePath.c_str());
		ImGui::InputText("##savepath", m_SavePath, sizeof(m_SavePath));
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			SaveEditorScene(m_SavePath);
		ImGui::SameLine();
		if (ImGui::Button("Open"))
			OpenEditorScene(m_SavePath);
		ImGui::SameLine();
		if (ImGui::Button("New"))
		{
			g_EditorScene.Clear();
			g_EditorScenePath.clear();
		}

		ImGui::SeparatorText("Preset shapes");
		// One button per MeshCache primitive key -- see MeshCache::Get. Each
		// placed entity is immediately saveable, because its SourcePath is
		// exactly the key the cache resolved it through.
		struct Preset { const char* Label; const char* Path; };
		static const Preset presets[] = {
			{ "Cube", "primitive:cube" }, { "Sphere", "primitive:sphere" },
			{ "Plane", "primitive:plane" }, { "Cylinder", "primitive:cylinder" }
		};
		for (const Preset& preset : presets)
		{
			if (ImGui::Button(preset.Label))
				PlaceMesh(preset.Path, preset.Label);
			ImGui::SameLine();
		}
		ImGui::NewLine();

		ImGui::SeparatorText("Import");
		ImGui::TextDisabled(".obj only -- see the plan's note on why glTF stays in ModelDemo for now.");
		ImGui::InputText("##importpath", m_ImportPath, sizeof(m_ImportPath));
		ImGui::SameLine();
		if (ImGui::Button("Import"))
			PlaceMesh(m_ImportPath, "Imported");

		ImGui::SeparatorText("Open as starting scene");
		for (int i = 0; i < s_DemoCount; i++)
		{
			if (!s_Demos[i].CanOpenInEditor || !s_DemoInstances[i])
				continue;

			if (ImGui::Button(s_Demos[i].Name))
			{
				g_EditorScene.Clear();
				g_EditorScenePath.clear();
				s_DemoInstances[i]->OnExportToScene(g_EditorScene);
			}
		}

		ImGui::End();
```

And add one private method, next to `BuildLayout`:

```cpp
	// Places a mesh -- imported or preset alike, both resolve through
	// MeshCache -- at the origin, tags it, and selects it, so a placed
	// object is immediately the thing the Inspector is showing.
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

- [ ] **Step 2: Manually verify each control**

Run: `./gs.py build && ./gs.py run`

- Click each preset button: a shape appears at the origin, selected (Inspector shows it, Outliner highlights it).
- Type an existing `.obj` path (e.g. `assets/models/torus.obj`) into Import and click Import: it appears.
- Type a bogus path and click Import: nothing appears, nothing crashes (a warning lands in the log, from `Mesh::Load`'s own failure path).
- Type `scene.txt` in the save box, click Save, click New (scene clears), click Open: the placed shapes come back, still selected/moveable.
- Click "Cube3D" under "Open as starting scene": the editor's scene populates with Cube3D's floor/cube/sphere/riser/spinner content, visible and selectable in the new view. (This step's visible result depends on Task 5's `Cube3D::OnExportToScene`; before Task 5 lands, the button is legitimately absent because `CanOpenInEditor` is still `false` for every row -- confirm that absence instead.)

- [ ] **Step 3: Commit**

```bash
git add TestEnv/src/EditorShell.h
git commit -m "$(cat <<'EOF'
Make the Assets panel real: import, presets, save/load, open-as-scene

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Gkj1PEW1Vs7yxuP1X1KyZz
EOF
)"
```

---

## Task 5: Cube3D opts in, an end-to-end regression capture, and cleanup

**Files:**
- Modify: `TestEnv/src/Cube3D.h`
- Modify: `TestEnv/src/DemoRegistry.h`
- Create (temporary): `TestEnv/src/EditorSceneCaptureTest.h`
- Modify (temporary call site): `TestEnv/src/TestApp.cpp`
- Delete (cleanup): `TestEnv/src/EditorSceneDataTest.h`, `TestEnv/src/EditorSceneCaptureTest.h`

**Interfaces:**
- Consumes: `DemoLayer::OnExportToScene` (Task 2), `GS::Scene::Save/Load` (Task 1), `EditorSceneView`/`g_EditorScene` (Tasks 2-3)
- Produces: `Cube3D::OnExportToScene(GS::Scene&)` (override)

- [ ] **Step 1: Implement the export**

In `TestEnv/src/Cube3D.h`, add, near `BuildScene`:

```cpp
	// What OnExportToScene hands the editor: every entity this demo places,
	// translated into SourcePath-tagged MeshComponents so the result is
	// immediately saveable -- not just viewable. The match against
	// m_Primitives/m_Loaded/m_Beacon is what recovers the cache key a
	// pointer alone cannot carry; a mesh built some other way is skipped
	// rather than exported half-described.
	void OnExportToScene(GS::Scene& scene) override
	{
		auto& meshes = m_Scene.View<GS::MeshComponent>();

		for (size_t i = 0; i < meshes.Size(); i++)
		{
			const GS::MeshComponent& source = meshes.Components()[i];
			if (!source.Geometry)
				continue;

			std::string path = SourcePathFor(source.Geometry);
			if (path.empty())
				continue;

			GS::EntityId owner = meshes.Owner(i);
			auto* sourceTag = m_Scene.GetComponent<GS::TagComponent>(owner);
			auto* sourceTransform = m_Scene.GetComponent<GS::TransformComponent>(owner);
			if (!sourceTransform)
				continue;

			GS::Entity entity = scene.CreateEntity(sourceTag ? sourceTag->Name : "Entity");
			*entity.Get<GS::TransformComponent>() = *sourceTransform;

			GS::MeshComponent mesh;
			mesh.SourcePath = path;
			mesh.Geometry = GS::MeshCache::Get(path);
			mesh.Color = source.Color;
			mesh.Visible = source.Visible;
			entity.Add<GS::MeshComponent>(mesh);
		}
	}

	std::string SourcePathFor(const std::shared_ptr<GS::Mesh>& geometry) const
	{
		if (geometry == m_Primitives[0]) return "primitive:cube";
		if (geometry == m_Primitives[1]) return "primitive:sphere";
		if (geometry == m_Primitives[2]) return "primitive:plane";
		if (geometry == m_Loaded) return "assets/models/icosahedron.obj";
		if (geometry == m_Beacon) return "assets/models/beacon.obj";
		return "";
	}
```

- [ ] **Step 2: Turn the flag on**

In `TestEnv/src/DemoRegistry.h`, add `CanOpenInEditor` to Cube3D's row (the field defaults to `false`, so this is the one row that names it explicitly):

```cpp
	{ "Engine",   "Cube3D (3D, lit meshes)",      "Cube3D",    []() -> DemoLayer* { return new Cube3D(); }, true },
```

- [ ] **Step 3: Write the end-to-end regression test**

This is the spec's capture-based check: build the same content two ways -- by hand in code, and via a saved-and-reloaded `--scene` -- and compare renders byte-for-byte.

Create `TestEnv/src/EditorSceneCaptureTest.h`:

```cpp
// TEMPORARY -- delete after verifying the save/load round trip renders
// identically to the scene it was saved from.
#pragma once
#include <GS.h>

#include "EditorProject.h"

namespace EditorSceneCaptureTest {

	inline void Run()
	{
		GS_TRACE("EditorSceneCaptureTest:");

		g_EditorScene.Clear();

		GS::Entity cube = g_EditorScene.CreateEntity("Cube");
		cube.Get<GS::TransformComponent>()->Position = { -1.0f, 0.0f, 0.0f };
		GS::MeshComponent cubeMesh;
		cubeMesh.SourcePath = "primitive:cube";
		cubeMesh.Geometry = GS::MeshCache::Get("primitive:cube");
		cubeMesh.Color = { 0.9f, 0.4f, 0.2f, 1.0f };
		cube.Add<GS::MeshComponent>(cubeMesh);

		GS::Entity sphere = g_EditorScene.CreateEntity("Sphere");
		sphere.Get<GS::TransformComponent>()->Position = { 1.0f, 0.0f, 0.0f };
		GS::MeshComponent sphereMesh;
		sphereMesh.SourcePath = "primitive:sphere";
		sphereMesh.Geometry = GS::MeshCache::Get("primitive:sphere");
		sphereMesh.Color = { 0.3f, 0.6f, 1.0f, 1.0f };
		sphere.Add<GS::MeshComponent>(sphereMesh);

		bool saved = g_EditorScene.Save("editor_capture_test.tmp.txt");
		GS_TRACE("  [{0}] scene saved for round trip", saved ? "ok " : "FAIL");
	}

}
```

- [ ] **Step 4: Run the two captures and compare**

In `TestEnv/src/TestApp.cpp`, temporarily add `#include "EditorSceneCaptureTest.h"` and call `EditorSceneCaptureTest::Run();` as the first line of the constructor (replacing Task 1's now-removed call).

Run: `./gs.py build`

Run (builds the hand-authored scene and captures it):
```sh
./TestEnv --hide-window --lockstep --hide-ui --capture shots/hand.png --capture-step 5
```

Run (loads the just-saved file fresh and captures it):
```sh
./TestEnv --scene editor_capture_test.tmp.txt --hide-window --lockstep --hide-ui --capture shots/loaded.png --capture-step 5
```

Expected: `shots/hand.png` and `shots/loaded.png` are byte-identical (`cmp shots/hand.png shots/loaded.png` prints nothing and exits 0) -- the saved-and-reloaded scene renders exactly the same cube and sphere the hand-built one did. If they differ, the mismatch is almost certainly the camera's default position/orientation differing between the two runs (the first run's camera is `EditorSceneView`'s default; the second is identical since nothing here changes it) -- if a genuine difference shows up, check that before suspecting `Scene::Load`.

- [ ] **Step 5: `git grep TEMPORARY`, and delete every temporary file it finds**

```bash
git grep -n TEMPORARY -- TestEnv/src
```

Expected: exactly `EditorSceneDataTest.h` and `EditorSceneCaptureTest.h`. Delete both files, remove the `#include` and call in `TestApp.cpp`, and delete the two `.tmp.txt`/`shots/*.png` artefacts left in the working directory.

- [ ] **Step 6: Verify every config**

Run: `./gs.py build all`
Expected: Debug, Release (and any other configured configs) all build clean.

Run: `./gs.py run -- --demo Cube3D` for a few of the other 16 demos spot-checked the same way (at minimum one more mesh-heavy one, e.g. `--demo MapBuilding`, and one unrelated to meshes at all, e.g. `--demo Acoustics2D`)
Expected: identical behaviour to before this plan -- the point of `IsActive()`/`InvalidDemo` is that nothing about a running demo changed.

- [ ] **Step 7: Update `docs/STATE.md`**

This sub-project is done; the next ones (mesh authoring, logic/module attachment, play-in-editor) are still ahead. Update the "In flight" note added when this plan's spec was written to say sub-project 1 landed and name what is next, following this file's own instruction to fix it in the same commit as the work that made it wrong.

- [ ] **Step 8: Commit**

```bash
git add TestEnv/src/Cube3D.h TestEnv/src/DemoRegistry.h docs/STATE.md
git commit -m "$(cat <<'EOF'
Cube3D opts in to the editor; verify the save/load round trip renders
identically, and remove the temporary self-tests

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Gkj1PEW1Vs7yxuP1X1KyZz
EOF
)"
```

---

## Self-review notes

- **Spec coverage:** boot flow (Task 2), data model incl. camera/mesh-path/save-load (Task 1), editor UI incl. outliner/inspector/assets (Tasks 3-4), demo-as-prefab export (Tasks 2 + 5), error handling for missing mesh/scene paths (Task 1's `Mesh::Load`/`Scene::Load` failure paths, Task 4's bogus-path manual check), testing incl. self-test and capture-based regression (Tasks 1 and 5) — all covered.
- **Placeholder scan:** no TBDs; every step carries real code or a concrete command.
- **Type consistency:** `GS::MeshCache::Get`, `GS::Scene::Save/Load`, `GS::CameraComponent`, `MeshComponent::SourcePath`, `g_EditorScene`, `g_EditorSceneView`, `DemoLayer::OnExportToScene`, `DemoEntry::CanOpenInEditor` and `s_DemoInstances` are each introduced once and used with the same name and signature everywhere they appear later.
- **Deferred, and worth naming again so it is not lost:** terrain-as-a-scene-object, custom mesh authoring, logic/module attachment, play-in-editor, multiplayer — each is its own sub-project per the spec and `docs/STATE.md`.
