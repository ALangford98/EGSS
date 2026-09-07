# Editor boot + scene composition — design

Sub-project 1 of the editor pivot. Full context and the rest of the sequence
(mesh authoring, logic/module attachment, play-in-editor) are tracked in
`docs/STATE.md`; multiplayer/networking is backlog, not part of this or any
scoped sub-project yet.

## Goal

Boot the app into a real, editable scene instead of a selected demo, and let
existing demos be opened as editable content ("prefabs") rather than only run
as fixed programs. This is the foundation the later sub-projects (mesh
authoring, logic modules, play mode) build on.

## Why now, and why this shape

`EditorShell.h` already provides docking chrome (Outliner-shaped left panel,
profiler, an "Assets" placeholder) around a central viewport — but that
viewport shows whichever single demo `g_ActiveDemo` names, not a scene the
user composed. `GS::Scene`/`Entity`/`Components` (`GS/src/GS/Scene/`) is
a working, minimal ECS with no reflection or registration by design — "any
struct can be a component, there is nothing to register." `SceneDemo.h`
(457 lines) already exercises that ECS as something close to an editor:
mouse-picking via `Scene::EntityAtIndex`, an outliner list, a Tag/Transform
inspector, hover/select highlighting, delete-to-remove. `Mesh::CreateCube/
CreateSphere/CreatePlane/CreateCylinder` already exist as preset-shape
generators, and `GltfLoader` already imports external meshes.

None of that is wired together at the app-boot level, and two real gaps exist:
`Scene` has no `Save`/`Load`, and nothing maps a scene's meshes back to the
file they came from. This sub-project closes those two gaps and promotes
`SceneDemo`'s proven pieces into `EditorShell`, rather than building new ones.

## Scope

**In scope:** editor-first boot, scene save/load, placing an imported mesh or
a preset primitive shape into a scene via the Outliner/Inspector/Assets
panels, an editor-owned free-fly camera, and an opt-in mechanism for an
existing demo to expose its content as an openable scene.

**Explicitly deferred**, each its own later sub-project:
- **Terrain-as-a-scene-object.** VoxelField3D/OpenWorld/MapBuilding are the
  most complex existing systems (chunk streaming, LOD, double-precision
  origins) and do not belong in a foundation step.
- **Custom mesh authoring.** Defining a *new* mesh from scratch inside the
  editor is sub-project 2. This sub-project only places meshes that already
  exist (imported or preset).
- **Logic/module attachment.** A demo's behavior (its `OnFixedUpdate`
  simulation code) stays exactly as compiled C++. Opening a demo as a prefab
  exposes only its *content* — meshes, transforms, camera — never its
  behavior. Swappable/attachable logic is sub-project 3.
- **Play-in-editor.** An edit/run toggle depends on there being a composed
  scene and (eventually) attachable logic to run; sub-project 4.
- **Multiplayer/networking.** Backlog only — see `docs/STATE.md`.

## Design

### Boot flow

`EditorShell` continues to wrap everything, as it does today. With no
relevant flags, it opens a blank scene, or the last-saved scene if a small
local prefs file (not checked in) names one. `--demo <name>` is **unchanged**:
it boots straight into that demo's own `OnDemo*` hooks exactly as it does
today, for regression captures and engine development — the editor chrome
still wraps it, but no export or scene-open behavior kicks in. A new
`--scene <path>` flag boots straight into a saved scene, for the same reason
`--demo` exists: a capture or a test should be able to point at exact content
without clicking through the UI first.

### Data model

Reuse `TagComponent`, `TransformComponent`, and `MeshComponent` unchanged.

Add `CameraComponent` (fov, near, far, and an "is the active viewpoint" flag)
— there is currently no ECS camera; every demo owns a raw camera object
directly. This is new, additive, and touches no existing component.

Add `Scene::Save(path)` / `Scene::Load(path)`, using the existing `GS::Json`
dependency. Given the ECS's explicit "no registration, no reflection"
philosophy, the serializer is a hand-written, explicit list of the components
this sub-project cares about (Tag, Transform, Mesh-by-path, Camera) — not a
generic reflective walk of whatever a `ComponentStore` happens to hold. Adding
a new persisted component later means adding one more explicit case, the same
way adding a demo means adding one more line to `DemoRegistry.h`.

`MeshComponent` gains a path string (or a small sibling struct — implementer's
call) recording where its `Geometry` came from. A shared, load-once mesh cache
keyed by that path backs both import and preset creation, so saving a scene
writes the path rather than the geometry, and loading resolves through the
cache rather than re-triangulating or duplicating GPU buffers for the second
entity that uses the same mesh. Preset shapes get synthetic cache keys
(`primitive:cube`, `primitive:sphere`, `primitive:plane`, `primitive:cylinder`)
so they serialize and reload through the exact same path as an imported file.

### Editor UI

`SceneDemo`'s picking, outliner, and inspector logic is lifted out of that one
demo and into `EditorShell` as two panels — Outliner and Inspector — operating
on whatever `GS::Scene` is currently open, not rewritten from scratch. The
existing "Assets" panel placeholder becomes real: an Import action (file
dialog → `GltfLoader` → cache), four preset-shape buttons, and a list of
openable items — saved scenes plus any demo implementing the export hook
below. `EditorShell` owns its own free-fly edit camera, independent of any
`CameraComponent` an opened scene may contain, because editing needs to look
around regardless of what the scene's own "game" camera is doing.

### Demo-as-prefab export

`DemoLayer` gains one optional virtual:

```cpp
virtual void OnExportToScene(GS::Scene& scene) {}
```

Default is a no-op, and a demo that doesn't override it simply doesn't appear
in the editor's "Open" list — it stays reachable only via `--demo`, unchanged.
`SceneDemo` and `ModelDemo` implement it first, translating whatever they
currently manage by hand into the same Tag/Transform/Mesh/Camera components
the editor already understands. Opening one runs the export once into a fresh
in-editor scene; from that point it behaves like any other scene — edit it,
save it under a new name. The original demo file and its `--demo` boot path
are untouched.

No other demo is touched by this sub-project. The other fifteen keep running
exactly as they do today.

### Error handling

- Loading a scene that references a missing/unreadable mesh path: log a
  warning (matching `GS_WARN` usage elsewhere, e.g. `DemoRegistry.h`'s
  `--demo` handling) and leave that entity's `MeshComponent::Geometry` null —
  the renderer already tolerates a `MeshComponent` with no geometry ("normal
  thing to have while a scene is being built").
- A `--scene <path>` that doesn't exist: warn and fall back to a blank scene,
  the same fallback as no flag at all.
- A demo's `OnExportToScene` producing zero entities is valid (nothing to
  place yet) and just yields an empty scene, not an error.

### Testing

A temporary self-test (`TestEnv/src/EditorSceneTest.h`, deleted after
verifying, per the project's self-test convention): build a scene with an
imported mesh, two preset shapes, and a camera; `Save`; `Clear`; `Load`;
assert entity count and every persisted component's values match, and that
the two entities sharing a preset shape resolve to the *same* cached
`Geometry` pointer rather than two copies.

A capture-based regression check, per the project's verification convention:
build the same content two ways — by hand in code, and via `--scene
saved.json` — and compare `--lockstep --hide-ui --capture` output
byte-for-byte between the two. That proves the serialized form renders
identically to its authored form, not just that the numbers round-trip.

## What this sub-project does not decide

The mesh-authoring file format (sub-project 2), the logic/module attachment
mechanism (sub-project 3, and the highest-risk open question in the whole
pivot — no scripting language exists in the engine today), and play-mode
semantics (sub-project 4) are all out of scope here and get their own
brainstorm before design.
