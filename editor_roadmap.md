# Editor Roadmap

Consolidates `wishlist.md` (scoped, ordered easiest to hardest) and the
pending items already tracked in `docs/STATE.md`. Where an entry needs a
decision only the owner can make before it can be scoped further, that's
called out explicitly rather than guessed at.

## Foundational gaps (assessed 2026-09-15)

Found by surveying this file, `docs/STATE.md`, and the README's "Still
outstanding" section together and asking: what still blocks using this as a
viable engine to make something in, not just extend the editor's polish.
Bigger and more structural than the wishlist below — each is closer to a
sub-project than a feature.

- ~~**The editor scene has no physics simulation wired in at all.**~~
  **Wrong when this was written (2026-09-15) — already done.** This
  assessment was based on reading `PhysicsComponent`'s data-only fields and
  never actually opening `PlayMode.h`, where `PlayMode::Play()` already
  built a `RigidBody3D` per `PhysicsComponent`'d entity as of
  2026-09-13's `044600d`, two days before this list was written. `git log`
  would have caught this; the doc-only survey that produced this section
  didn't check it. See `docs/superpowers/specs/2026-09-13-physics-
  simulation-design.md` for the real design and its own, still-open scope
  cuts: box colliders only (no sphere/capsule shape choice), no static
  obstacle without an explicit `PhysicsComponent`, and Kinematic bodies are
  built immovable but nothing yet drives their velocity.
- **No first-class 2D workflow in the editor.** `SpriteComponent` and
  `RigidBody2DComponent` exist in the ECS and back the old hand-coded 2D
  demos (`Breakout`, `Physics2D`), but neither has any Inspector/Outliner
  wiring — confirmed by grep, nothing in `EditorSceneView.h` references
  either. A 2D game can only be built today by writing a new `DemoLayer`
  by hand, the way `BreakoutRecreation` faked 2D with flattened 3D meshes
  on an orthographic camera rather than using the real sprite pipeline.
- **No animation system anywhere in the codebase** — no bones, no
  skeleton, no animation component (also called out under wishlist #12).
  Any game with an animated character is blocked on this before anything
  else in this list.
- **No in-game UI/HUD system.** ImGui is the only UI layer, and it's a
  dev-facing debug overlay wired through `Layer::OnImGuiRender` — nothing
  produces a title screen, HUD, or menu meant for a player to see.
- **The GSS entity-scripting stdlib is too thin for real gameplay code.**
  `console.log`/`.length`/`.map`/`.filter`/`.find` don't exist for
  entity scripts yet (wishlist #3 covers this precisely). Scripts also
  only reach `Transform`/`Input`/`scene.findByTag`/`spawn`/`destroy` — no
  way to trigger a sound or touch physics from a script, since neither
  has a binding yet.

**Not on this list, and why:** normal maps, shadow mapping, a particle
system, and post-processing are real, but they're visual-quality gaps a
game can ship without at small scale — genuinely blocking gaps come first.

## Wishlist, easiest to hardest

### 1. Right-click context menu (Add Script / Edit Mesh / Rename / advanced options)
`ImGui::BeginPopupContextItem()` on each Outliner row, opening a popup
that calls the same functions the Inspector buttons already call —
"Add Script" exists at `EditorSceneView.h:509`, "Edit Mesh" is the
button built this session, "Rename" reuses the ID field's `InputText` +
`EditFieldCommand<TagComponent,string>` pattern already in place. No new
logic, just a second entry point to existing actions.
**Additional scoping required:** what "advanced options" should contain.
**Resolved (2026-09-12):** the normal right-click menu (Add Script/Remove
Script, Rename, Edit Mesh, Duplicate, Delete) is the flat, common-action
tier -- built. "Advanced options" turned out to mean something more
technical: an `Advanced` submenu, structured as a tab bar so it can grow
more categories later, currently holding one tab, **Physics**
(`PhysicsComponent`: Body Type, Mass, Friction, Restitution -- data-only,
saved/loaded with the scene, nothing simulates it yet since the editor
scene has no physics world wired in at all). Deliberate scaffolding ahead
of a real physics-in-editor system, done because it was asked for
explicitly rather than discovered as a gap -- the note to come back to
is: decide whether/how the editor scene should actually run a
`PhysicsWorld2D`/`PhysicsWorld3D` and simulate these fields, and what
other technical categories (rendering overrides? collision debug?)
belong in the same `Advanced` tab bar alongside Physics.

### 2. Theme config.json + ImGui restyle
`EditorTheme.h` already has this exact pattern (a struct of `ImU32`
colors) but scoped only to text-editor syntax colors. Extend it to the
full `ImGuiStyle::Colors[]` array, plus a small hand-rolled JSON loader
(no JSON library is vendored, but none is needed — this project already
hand-writes JSON elsewhere, e.g. the profiler's trace file). Apply once
at startup.
**Additional scoping required:** whether the theme needs to hot-reload
while the editor runs, or just load once at startup.

### 3. GSS stdlib functions (console.log, filter, find, map, .length, .toArray())
The transpiler already has this shape of mapping (`Math.min` →
`std::min`, a flat lookup table in `ScriptEngine.h:1523`). `console.log`
and `.length` are one-line additions on the same pattern. `map`/
`filter`/`find` are the real work — they need the transpiler to turn a
callback argument into a C++ lambda and choose the matching
`<algorithm>` call (`std::transform`/`std::copy_if`/`std::find_if`),
which nothing in the transpiler does yet (it only handles plain function
calls, not passing a function as a value). Best split into one task per
function; each lands independently.
**Additional scoping required:** what "array" means for `.toArray()` —
the concrete C++ type being converted to/from.

### 4. Low-fidelity editor rendering mode
A toggle that shrinks the viewport framebuffer's render resolution
and/or skips the lighting pass added this session. Contained to the
existing render path, but touches the same shader/framebuffer code the
Cube3D regression capture depends on being byte-identical — needs to
preserve that capture exactly when the toggle is off.

### 5. Normal maps
`GS::Material` already supports diffuse textures with named-uniform
mapping via `.mtl` (`Material.h`) — not from scratch. But `MeshVertex`
has no tangent yet (`Mesh.h`'s own comment already flags this as
deliberately deferred). Needs: add `Tangent` to `MeshVertex`, generate
tangents in every geometry loader/primitive builder, add a normal-map
uniform slot to `ObjMaterialUniforms`, add TBN-matrix sampling to the lit
shader. Touches the vertex format every mesh in the engine shares, so it
needs the same regression-capture discipline as everything else here.

### 6. Object measurements for engineer-level accuracy
An overlay similar in spirit to the mesh-edit point overlay (`Renderer2D
::DrawLine`-based rulers/labels), reading bounds off the selected
entity's components.
**Additional scoping required:** units, precision, and whether it needs
live dimension labels while dragging (CAD-style) or just a static
readout.

### 7. Vegetation improvements
A real system already exists to extend — `Vegetation.h`, `Grass.h`,
`WindStreaks.h`, `Critters.h`, `Rocks.h`, `Climate.h` — this is
refinement, not a new system.
**Additional scoping required:** what specifically feels wrong (density,
variety, performance, visual quality) — cannot be sized further without
this.

### 8. TS language-server-style tab completion
The engine already embeds the real TypeScript compiler
(`TestEnv/assets/typescript.js`, run inside QuickJS) to parse GSS for the
transpiler — confirmed live `ts.SyntaxKind`/`sourceFile.
getLineAndCharacterOfPosition` calls in `ScriptEngine.h`. This can
plausibly call `ts.createLanguageService(...)` directly from the same
embedded module rather than needing a separate `typescript-language-
server` process and an LSP client. The hard part is the UI:
`TextBuffer.h`/`TextEditorPanel.h` are a fully hand-rolled text editor,
so the completion popup, trigger-character detection, filtering-as-you-
type, and insertion all need building with no existing precedent.

### 9. Mesh texture map exports (UV unwrapping to a Photoshop-editable template)
Real UV unwrapping (seam-finding, chart packing) is a genuinely hard
geometry problem in general. Could be scoped down to just the primitives
(`CreateCubeData` etc. already have simple, known UV layouts — exporting
those as a flat template is easy), but "designer can edit the template in
Photoshop" implies wanting this to work on arbitrary imported meshes too,
which is the hard version.
**Additional scoping required:** primitives-only, or arbitrary imported
meshes too.

### 10. Mesh deforming tools for natural bending
Needs either a skinning system (bones + per-vertex weights) or a lattice/
cage deformer — a new subsystem with real weight-painting or falloff-
function math. Nothing in the engine currently supports either. Well
beyond the vertex-level editing built this session, which edits topology
directly and one point at a time; this needs smooth influence over many
vertices from one control.

### 11. Texture editor (color pickers, noise generation, transparency, etc.)
A small paint application in its own right: a canvas widget, brush/fill
tools, a procedural noise generator (no Perlin/Simplex noise library is
vendored anywhere in this engine), layer/blend handling, and a path to
push the result into `GS::Texture2D`. Genuinely large — closer to
building a second application than adding a feature.

### 12. Visual scripting graph (linking scripts/shaders/animations)
No node-editor UI library is vendored, so the graph UI itself would be
built from scratch. Worse: **there is no animation system anywhere in
this codebase** — no bones, no skeleton, no animation component — so a
third of what this asks for has no target to link to yet; that would
need to be its own sub-project first. Scripts and shaders at least have
something to hook into already (the GSS transpiler, `GS::Material`).
Tied hardest with #13.

### 13. CAE / electrical engineering tools
An entirely separate domain — schematic capture, circuit simulation —
with nothing in this codebase to build on. Effectively founding a
second, unrelated product inside the same engine. Tied hardest with #12.

## Pending editor todos (already tracked in docs/STATE.md)

### Mesh authoring follow-up: face/edge-picking UI
Extrude, Delete Face, and Split Edge are fully implemented and tested at
the `EditableMesh` data layer (`TestEnv/src/EditableMesh.h`) but have no
UI path to reach them — only Move and Delete Point are wired. Wiring the
other three needs face-picking and edge-picking UI concepts that don't
exist anywhere yet (today's picking is single-point only).
**Additional scoping required:** this is its own brainstorm → spec →
plan sub-project, not a small addition.

### Mesh authoring: deferred fixes from the closing review
- No guard against Play starting while a mesh-edit session is open.
- `Rebuild()` hardcodes UV to `(0,0)` for every point; the spec wanted UV
  carried forward from each point's originating vertex, and
  `FromMeshData` currently discards the mapping that would require.
- Minor UX rough edges: a bare click pushes a no-op undo snapshot; global
  Ctrl+Z isn't redirected to a session's own undo stack while one is
  open; the export path doesn't compose with the project's own path.
- No `docs/CHANGELOG.md` entry yet for the mesh-authoring feature.

### Multiplayer/networking foundation — done (2026-09-14)
Landed: UDP transport, a custom reliable-ordered channel, host-as-server
session model, opt-in `NetworkIdentity`/`NetworkTransform` replication,
manual RPCs, and an editor Network panel. See `docs/CHANGELOG.md`'s
2026-09-14 entry and `docs/STATE.md` for what's still deliberately out of
scope (client prediction, a dedicated server, matchmaking/NAT traversal,
generic component replication). Left here only long enough to record that
this item closed — remove on the next pass through this file.

### Character attributes
`s_Strength` in `TerrainLab.h` is deliberately a constant with a hook
where a real stat should be (carry capacity trained by use). Owner
deferred this explicitly — **ask before starting.**

### Editor UX click-through verification gap
Lighting, project loading/saving UX, the text editor, and the profiler
(all landed) share one unverified gap: the actual mouse/keyboard click-
through has never been exercised, since no GUI-automation tool exists in
this environment. What's verified is the underlying logic and, where
possible, a rendered capture of the real UI — not a live interaction
test. Not a defect, just a standing caveat on all four.
