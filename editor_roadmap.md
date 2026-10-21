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
  Wishlist #3 (landed 2026-09-28) added `console.*`, `number[]` and
  `.length`/`.map`/`.filter`/`.toArray()` to compiled scripts, but `.find`
  and any way to get a *collection* of entities are still missing, and
  scripts only reach `Transform`/`Input`/`scene.findByTag`/`spawn`/
  `destroy`. **Physics bindings landed 2026-10-02** (impulses, velocity,
  `isTouching`); a script still cannot trigger a sound -- that binding is
  next.

**Not on this list, and why:** normal maps, shadow mapping, a particle
system, and post-processing are real, but they're visual-quality gaps a
game can ship without at small scale — genuinely blocking gaps come first.

## Exporting games (requested 2026-10-07)

A core engine function: package a game so someone else can install and play
it, on Linux, Windows and the web, then Android and iOS. Seven pieces, each
its own spec, in dependency order -- see
`docs/superpowers/specs/2026-10-07-game-runtime-player-design.md`:

1. ~~Runtime + standalone player~~ -- done 2026-10-07 (`GSPlayer`).
2. ~~Packager + Linux export~~ -- done 2026-10-08 (`TestEnv --export`,
   folder + `.tar.gz`); AppImage added 2026-10-12.
3. ~~Editor "Export Game..." dialog~~ -- done 2026-10-12.
4. ~~Windows~~ -- done 2026-10-12, cross-compiled with llvm-mingw, verified
   under Wine. Deferred: an embedded `.ico`, code signing.
5. ~~Web~~ -- done 2026-10-12 (Emscripten + WebGL2), verified in headless
   Firefox.
6. ~~Android~~ -- built 2026-10-12: an arm64 APK (NativeActivity, no
   Gradle), touch as the GSS pointer (`input.isPointerDown/pointerX/Y`),
   `docs/superpowers/specs/2026-10-12-android-export-design.md`. **Verified
   statically only** -- no device or KVM here; the first run on a phone
   (`adb install -r`, `adb logcat -s GS APPLICATION`) is the open item.
   Deferred: x86_64, a release keystore, app id / orientation in the
   project manifest, engine flags via intent extras.
7. ~~iOS~~ -- built 2026-10-12 as an Xcode project to finish on a Mac
   (`docs/superpowers/specs/2026-10-12-ios-export-design.md`): UIScene +
   CAEAGLLayer (GLES 3), CADisplayLink driving `RunFrame`. **Never built by
   Xcode** -- checked here by compiling and linking every source under the
   iOS defines and type-checking the `.mm` against stand-in headers. Open
   items: the first Xcode build and device run; OpenGL ES is deprecated on
   iOS, so Metal (natively or via ANGLE) is the long-term answer.

## Humanoid: RAGE / Euphoria-style physical character (owner priority, 2026-10-14)

**Direction changed 2026-10-21.** The owner: tone the realism back, make it
snappy -- GTA / Red Dead for moving and being hit, Assassin's Creed for
parkour's flow, the player's control first. Three projects, in order:

1. ~~**Locomotion**~~ -- done 2026-10-21 (`Runtime/Locomotion.h`; changelog,
   same date): the keys decide, the body is posed from CMU strides,
   kinematic while controlled. `locomotion old` keeps the physical walk.
   Open: more than one stride per speed; start, stop and pivot recordings
   (the pivot is procedural); a free foot skimming at heel strike.
2. **Parkour on the new base** -- next, the climb first (it pulls him up
   chest first and over on his front, legs kicking -- the owner's report,
   2026-10-21, and the same on the old walk). Forgiving triggers and generous
   snapping, moves chaining as AC's do, the wall run rebuilt (two fixes
   landed: a wall run may start from the jump's crouch, the side ray reaches
   1.25 m). Mixamo has the clips CMU lacks (vaults, wall runs; royalty-free
   in a game, needs an FBX reader).
3. **Hits** -- a physical stumble from a posed walk ran away forward and
   never handed back, so walking hits are the kinematic stagger for now;
   standing ones stay physical. Make walking hits physical again from the
   posed body.

Decision for the owner: delete the old walk (`DriveWalk`, `PlaceFeet`, the
set pieces, turn-stepping) or keep it as the clip-less fallback.

Everything below predates the change and describes the physical walk.

The owner wants GTA IV-style ragdoll behaviour: balancing, stumbling,
catching yourself, falling realistically. Where it stands:

- **The rig and its controllers** live in `Runtime/Humanoid.h`, extracted
  from the Ragdoll demo bit-identically on 2026-10-14.
  - The demo, Map Building and games (Player or NPC with Body = Humanoid)
    all drive the one class.
  - **Improve it in the demo**, where the tuning panel is; games get the
    change automatically.
  - **Check any change** with a recorded session and a per-step body dump.
    `docs/CHANGELOG.md` 2026-10-14 says how. Images won't do: the demo isn't
    reproducible at 4× MSAA.
- **Working (2026-10-15):**
  - **the walk and run:**
    - driven feet that roll heel to toe;
    - pole-vector legs;
    - a spring-mass run;
    - no jolts on starting, stopping, turning, steps, stairs or jumps.

    Measured over 14 scenarios, see the changelog for 2026-10-15. Hips
    and feet are driven; the thighs, shins, torso and arms are physical;
  - standing balance with the ankle strategy;
  - **the stumble.** Hit while standing or walking, he catches it with the
    ankles or real steps (walking on, if walking), or goes down. It holds
    80–100 Ns standing; walking, 20 Ns 16/16, 40 Ns 13/16, 60 Ns 12/16;
  - **bracing.** A lost stumble goes down hands first;
  - jumping (crouch, push, flight, absorbing landing), ragdoll, get-up
    from the ground.
- **Working (2026-10-16):** any body (height, mass, masculine to feminine
  build; the dummy follows), walking styles (eleven presets, nineteen
  numbers each, in the Inspector and the terminal), and a choreographed
  get-up (face down and, rolling over first, face up). See the
  changelog's 2026-10-16 entry for the measurements.
- **Working (2026-10-16, later): parkour.** Vault, step on, climb, drop
  and roll, from rays ahead (knee, waist, chest, head); walls stop him.
  Jump starts a move, a run vaults by itself. Measured over five bodies
  and eight courses, 40/40 on their feet; see the changelog.
- **Next for parkour**, toward the "flowing" the owner asked for:
  - ~~moves into moves~~ -- done 2026-10-16: each move hands straight to
    the next once he is on his feet; a vault off an edge rolls;
  - ~~wall run and wall jump; a speed vault; hanging and shimmying along
    a ledge; dropping to a hang; feet pushing in the climb~~ -- done
    2026-10-16;
  - speculative contacts were tried for the deep steps of a hard landing
    and did not help: the hips are carried, and a carried body wins over
    contacts through the joints. **Hips driven by forces** were tried
    next (2026-10-16): a dynamic pelvis set each step to the velocity
    that reaches its path, gravity taken off in advance, its mass scaled
    up so the limbs do not drag it. Worse at every scale, over 70 runs
    (limb depth median / p90 / steps over 8 cm): carried 6.0 / 13.6 cm /
    36; driven x1 8.5 / 16.4 / 99; x4 6.8 / 17.8 / 62; x12 9.7 / 22.9 /
    111; x40 11.2 / 27.4 / 191 -- the stiffer it holds the path the worse,
    the velocity reset each step pumping the chain. Taken out. Nor the
    legs' motors (x2 torque 6.5 / 11.2 / 36, x4 7.3 / 16.2 / 93, speed
    cap 60 rad/s 7.1 / 16.7 / 50, stiffness x2 8.2 / 14.1 / 75), nor the
    solver (32 velocity iterations 5.6 / 10.2 / 37, 64 worse, position
    iterations no change). The depth left is case by case -- a target, a
    landing -- which is how every fix so far was found;
  - ~~a wall run round a corner; the cat leap; hanging round a corner;
    the tic-tac; precision jumps~~ -- done 2026-10-16;
  - ~~a course to try each move on; climbing crates; the get-up's arms~~
    -- done 2026-10-17: the Parkour course demo (a lane per move,
    `station <n>`), loose heavy boxes climbable, natural elbows and no
    flick on standing, and get-ups that no longer fly apart (27 in 40
    among rolling cannonballs, 4 on a clear floor; now 0 and 1);
  - next: balancing along a beam or a wall top (narrow footing, arms
    out); swinging on a bar; climbing a ladder or a pipe; a wall climb up
    a face with holds; sliding under a low gap; a dive roll over
    something; hanging round an inside corner; the moves for NPCs (an
    NPC choosing a route through them).
  - open from the course (2026-10-17): ~~a tilted box cannot be a ramp~~
    -- done 2026-10-17, later: `GroundBelow` asks a tilted box through its
    own frame, and the stand height feeds the slope's rate forward (lane
    13 of the yard). Still open: launcher balls roll for half a minute (no
    rolling resistance) and keep GameSession splitting its steps -- the
    cannon run's own balls are cleared 2 s after they land.
  - ~~a dodge~~ -- done 2026-10-17, later: sidestep, cut, brake or dive
    into a roll by speed and direction (`TryDodge`). The dive found the
    forward roll's faults: feet blended through the air between a tuck and
    the ground, the trunk and a thigh wedged into the floor at up to 140
    kN, hands hanging into the floor. All fixed; see the changelog.
  - get-ups, measured from eight directions (2026-10-17, later): 82% of
    142 knock-downs natural, 87% from his front, 74% from his back.
    Front-right (72%) and back (71%) are one get-up short of 75%. What is
    left is an arm caught as he rolls from his back onto his front --
    sending it overhead made things worse (73% to 51%) and was taken out.
    Next to try: roll him the other way round, over the arm that is free,
    or lift the trunk off the floor before the roll rather than during.
  - the owner's three (2026-10-17, evening), one at a time: ~~limbs
    bending unnaturally (the legs passing through each other turning
    back)~~ -- done: a turn steps, the hips wait for the feet, a run
    brakes first (changelog, same date). Next: **limbs clipping into the
    ground**, then **rotations that look wrong** (a planted foot still
    pivots on its ankle under a turn, up to 95 degrees). The owner wants
    more scripted, coordinated responses and less emergence.
  - modelled on recordings (2026-10-17, night): CMU motion capture, a
    `ghost` in the yard, and the walk and run taken from a person's (see
    the changelog). ~~Clips driving the set pieces~~ -- done 2026-10-18
    for turning round, stopping and setting off (`Humanoid::SetPiece`).
    Next along this line: 90 degree turns, pivots and the run's stop from
    subject 69 and 16; more than one recording per piece, chosen by
    speed and foot; and a second spine joint when the get-ups and rolls
    are next worked on (measured: 4-9 cm of chest lost to the one joint,
    the rounded back the real gain).
- **The cannon run** (2026-10-17, later; `TestEnv/src/CannonRun.h`,
  `Gunnery.h`, `RunCourse.h`). A course against the clock under fair
  cannon fire, hand-made or from a seed. Where it could go, in order: a
  difficulty curve validated by a ghost runner, height (platforms, gaps,
  wall runs across them), branching routes, cannons placed as a pass of
  their own, themes -- `docs/superpowers/specs/2026-10-17-procedural-
  courses-scope.md`. To ship it as a game: `Gunnery` as a runtime system
  with a component (as the launcher is), the course built from a seed
  component, best times and ghosts saved with the project.
- **Cover** (asked for 2026-10-16). Builds on parkour's sensing (`Sense`
  already knows a face's height and where it ends):
  - take cover: up against a wall or a low wall, back or shoulder to
    it, crouched behind low cover, standing at high cover;
  - move along it, staying in cover; round a corner onto the next face;
    across a gap to the next piece of cover;
  - peek and lean out (over low cover, round a corner), and back;
  - leave it: step out, or vault over low cover (the parkour vault);
  - whether a spot is covered from a threat: rays from the threat to
    the body's parts, the cannon being the hall's threat;
  - for NPCs too: find cover from a threat and go to it.
  - Needs from the owner: player only or NPCs too, and what the threat
    is beyond the cannon -- there is no shooting in the engine yet.
- **Open:**
  1. **Running hits, physically.** They still use the kinematic stagger.
  2. **The steepest slope he walks up** since the hips' climb became a
     spring (the old cap gave 42° walking). Not measured; needs a
     heightfield ramp.
  3. **A standing shove from the left at 80 Ns** catches 0/4, against
     4/4 from the right. It's at the limit, and was 2–4/4 on 2026-10-14;
     worth a look for an asymmetry.
  4. **Loads from above.** A tuning choice: how much should buckle him.
  5. **Joint gaps of 2.5–5 cm** between the driven ends while moving
     (1 cm with motor feet). It's the price of keeping thigh and shin
     physical. Stiffer leg motors or more solver iterations might close
     it.

## Game UI: a visual editor for `.gsui` screens (stretch goal, owner 2026-10-15)

The game UI kit (`Runtime/GameUI.h`) is built.
- Screens are JSON files in the project's `ui/` folder.
- They are drawn over the game or onto meshes (`ScreenComponent`).
- They are driven by values the game publishes and commands it
  understands.

What's left is editing them visually: an editor panel with the screen on a
canvas, widgets placed and anchored by dragging, properties in a side panel,
and Save to `.gsui`. The owner called it "a nice stretch goal but not
necessary right now".

What it needs:
- **A preview.** `GameUI::Context::DrawScreen` draws into any ImDrawList,
  so it can draw into an ImGui child window.
- **Hit-testing.** Widgets keep their laid-out rectangles, so a widget can
  be picked from where it was drawn.
- **A writer.** `GameUI::Parse` exists; the reverse doesn't yet.

## Wishlist, easiest to hardest

### 1. ~~Right-click context menu~~ — done (2026-09-12)
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
saved/loaded with the scene). Play has simulated these fields since
2026-09-13 (`044600d`, see "Foundational gaps" above). Still open: what
other technical categories (rendering overrides? collision debug?)
belong in the same `Advanced` tab bar alongside Physics.

### 2. ~~Theme config.json + ImGui restyle~~ — done (2026-09-13)
Landed as JSON themes with live colours/fonts and an Appearance panel
(`302071d`, spec `docs/superpowers/specs/2026-09-13-editor-theming-
design.md`). The original scoping notes follow.

`EditorTheme.h` already has this exact pattern (a struct of `ImU32`
colors) but scoped only to text-editor syntax colors. Extend it to the
full `ImGuiStyle::Colors[]` array, plus a small hand-rolled JSON loader
(no JSON library is vendored, but none is needed — this project already
hand-writes JSON elsewhere, e.g. the profiler's trace file). Apply once
at startup.
**Additional scoping required:** whether the theme needs to hot-reload
while the editor runs, or just load once at startup.

### 3. ~~GSS stdlib functions~~ — done except `.find` (2026-09-28)
Landed for the compiled path: `console.*`, `number[]` (a
`std::vector<double>`), `vec3.length`/`.toArray()`, `number[].length`/
`.map`/`.filter`. `.find`, array literals and a collection-returning
native call were cut during brainstorming — see
`docs/superpowers/specs/2026-09-23-gss-array-stdlib-design.md`, "Explicitly
out of scope". The original scoping notes follow.

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

### 5. Normal maps — done for the editor shader (2026-09-30)
Linked procedural materials render their normal maps, with the tangent frame
rebuilt per pixel from screen-space derivatives rather than per-vertex
tangents (owner's choice: no vertex-format change). Not done: per-vertex
tangents, normal maps from imported `.mtl`/glTF, and any shader other than the
editor's. The original scoping notes, which assumed per-vertex tangents,
follow.
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

### 9. ~~Mesh texture map exports~~ — done (2026-09-14)
Scoped to arbitrary meshes, not primitives-only; see the changelog's
2026-09-14 entry. The original scoping notes follow.

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

### 11. ~~Texture editor~~ — done (2026-10-01), as procedural materials
Redirected in brainstorming from a paint application to a node-graph
procedural material generator (the owner wanted generated, tileable
materials, not brushwork). Landed: the graph, a Material panel, `.gsmat`,
albedo/height/normal/roughness export, and a live link to scene meshes; see
the changelog's 2026-09-30 entries. Piece 2, normal-map rendering, landed the
same day (see #5), and piece 3, roughness driving Blinn-Phong shininess, on
2026-10-01 (the owner kept Blinn-Phong over Cook-Torrance). The paint half (canvas, brushes, layers)
is not planned. The original scoping notes follow.

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
