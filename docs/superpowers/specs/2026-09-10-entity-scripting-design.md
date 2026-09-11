# Entity/component scripting + play mode — design

Second of four sub-projects toward "recreate any existing demo inside the
editor" (file tree done; this; recreate one demo; text-editor
refinements). Bigger than the last few sub-projects -- a new native JS
binding layer and a real Play/Stop mode -- so this spec is fuller than the
"brief" ones, but still short.

## Goal

Attach a TypeScript file to an entity via the Inspector. Press Play: each
scripted entity's `OnStart`/`OnUpdate(dt)` runs against a small, fixed API
(its own transform, basic keyboard input). Press Stop: the scene reverts
to exactly how it was before Play, discarding whatever the scripts did.

No physics, no spawning/destroying entities, no querying other entities,
no camera/audio access -- all deferred until "recreate one demo" (the next
sub-project) reveals what's actually needed.

## Design

### Play/Stop and scene reversion

Reuses `GS::Scene::Save`/`Load` (already exist, already round-trip the
`gs-scene` format) rather than building new snapshot machinery:

- **Play**: `g_EditorScene.Save(<a fixed scratch path next to the
  executable, e.g. "play_snapshot.tmp">)` -- outside
  `g_EditorProjectPath`, so it never shows up in the new file tree, same
  reasoning `editor_layouts.txt` already sits next to the executable
  rather than inside a project. Then, for every entity with a
  `ScriptComponent`, prepare and run its `OnStart` (see below).
- **Stop**: free every prepared script's held JS function values, then
  `g_EditorScene.Load(<the scratch path>)`, discarding any script-driven
  changes. Sets play state back to false.
- **While playing**: every `OnFixedUpdate(dt)` (not `OnUpdate` -- this
  repo's own convention is "anything that moves belongs in
  `OnFixedUpdate`," and a script moving an entity is exactly that; also
  keeps the door open for Play sessions to be capture/replay-deterministic
  later), call every prepared entity's held `OnUpdate` function with `dt`.
- Editor camera navigation (`EditorSceneView::MoveCamera`) stays active
  during Play -- no reason to disable it.
- Play/Stop are plain buttons in the main menu bar (not a dropdown item),
  always visible regardless of which tab (Scene/Editor) is active.

### `ScriptComponent`

```cpp
struct ScriptComponent
{
    std::string ScriptPath; // relative to the executable, same convention MeshComponent::SourcePath uses
};
```

Fits the existing "any struct can be a component, nothing to register"
model with zero ECS changes. Added to `GS/src/GS/Scene/Components.h`
alongside the others.

### Inspector UI

Same pattern every other component block in `EditorSceneView.h`'s
Inspector already uses (`if (auto* comp = g_EditorScene.GetComponent<T>
(m_Selected)) { ...fields... }`), plus an "Add Script" button when the
component is absent (Mesh/Camera don't need this because they're only
ever added via the Tools panel's Place buttons at creation time --
Script is different, since it gets attached to an entity that already
exists):

- No `ScriptComponent`: an "Add Script" button that adds one with an
  empty path.
- Has one: a path field (reusing the file tree/text editor's own
  path-field look) + "Remove" button.

### The native JS binding layer

**One shared `ScriptEngine`, not one per entity** (already exists,
already hardened against the redeclaration/leak/hang bugs its own final
review found) -- reloading the 8.9MB TypeScript compiler per entity would
be wasteful. Extending it with:

- **A new driver shape**, wrapping a script's source as a function taking
  `entity` and `input` as *parameters* rather than globals:
  ```js
  (function(entity, input) {
      <the entity's transpiled script source>
      return {
          onStart: typeof OnStart === 'function' ? OnStart : null,
          onUpdate: typeof OnUpdate === 'function' ? OnUpdate : null
      };
  })
  ```
  This matters: if `entity` were a shared *global* instead, every
  entity's `OnStart`/`OnUpdate` closures would capture the same mutable
  binding, and the last entity prepared would silently clobber every
  earlier one's view of "its own" entity. Binding `entity` as a
  per-invocation function *parameter* gives each entity's closures their
  own, independent capture -- ordinary JS closure semantics, applied
  correctly.
- **A native `Entity` class** (QuickJS's class mechanism: one
  `JS_NewClassID`/`JS_NewClass` registered once on the shared runtime, a
  small heap-allocated `{GS::Scene*, GS::EntityId}` opaque struct per
  instance, freed in the class's finalizer). Methods: `getPosition()`/
  `setPosition(x,y,z)`, `getRotation()`/`setRotation(x,y,z)` (degrees),
  `getScale()`/`setScale(x,y,z)` -- each reads the opaque pair and calls
  `scene->GetComponent<TransformComponent>(entityId)`.
- **A native `Input` object** (one shared instance, not per-entity --
  input state isn't per-entity): `isKeyDown(keyName)`, reading through
  whatever this engine's existing `GS::Input` polling API already is.
- **`PlayModeController`** (new, small): owns the play/stop state, the
  scratch-snapshot path, and a `std::unordered_map<GS::EntityId, PreparedScript>`
  where `PreparedScript` holds the two `JSValue`s (`onStart`/`onUpdate`,
  `JS_DupValue`'d to keep them alive across frames) plus the native
  `Entity` object's `JSValue`. `Play()` builds this map (loading each
  script file fresh, transpiling, calling the driver, calling `onStart`
  once); `Stop()` frees every held `JSValue` in it and clears it.

## Explicitly deferred

Physics, spawning/destroying entities from a script, querying/referencing
other entities, camera/audio access, hot-reloading a script while
Play is active, an `OnStop`/cleanup hook, capture/replay determinism for
Play sessions (the `OnFixedUpdate` choice keeps this open, doesn't
deliver it).

## Testing

`PlayModeController`'s Play/Stop lifecycle and the native `Entity`
binding are testable without a live editor session the same way
`ScriptEngine` itself was: a temporary self-test constructing a scene
with a scripted entity, driving `Play()` then several `OnFixedUpdate`
calls then `Stop()`, and asserting the entity's `TransformComponent`
changed as the test script's `OnUpdate` should have caused, then reverted
exactly back to its pre-Play values after `Stop()`. The Inspector's
Add/Remove Script UI and the menu-bar Play/Stop buttons need a manual
click-through, disclosed as such per every other UI-interaction check in
this editor pivot.
