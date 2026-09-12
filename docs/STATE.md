# State

**This is the only doc in the repo that is allowed to go stale, and it is kept
short so that fixing it is cheap.** Everything else — `docs/ENGINE.md`, the
trap list, the changelog — describes things that stay true once written. This
file describes where the work is *this week*, which does not.

It exists because the alternative was worse. `docs/HANDOVER.md` used to open
with a "Current state" section naming the commit `main` sat at, and it drifted
22 commits and one abandoned workflow behind reality. A session that read it —
as `CLAUDE.md` told it to, at a cost of about 53k tokens — came away with a
*confidently wrong* answer to "what are we working on", while `git log`, which
costs about 200 tokens and cannot drift, sat unread. **The expensive source was
the least current one.** So the volatile part was cut down to this page.

## Read this first, and read it with `git log`

```sh
git log --oneline -15
git status --short
git branch --show-current
```

**Where those disagree with this file, they win.** The owner commits their own
work between sessions and sometimes while a reply is being written, so any git
state you did not just observe is stale — including the branch name below.

## Where the work is

**Branch: `main`.** The owner asked to stay on it directly for the docs work;
`density-in-double` merged in `cf266b8` and is finished with.

The live area is **`TestEnv/src/TerrainLab.h`** (~11k lines) and the headers
around it — `Critters.h`, `Vegetation.h`, `Rocks.h`, `Climate.h`. It is a
first-person terrain demo that grew a felling-and-building game: you cut trees,
buck them into logs, rive planks, and raise structures out of a grid-snapped
kit of boards, logs and stone.

Last landed, newest first:

- **Mesh authoring — a vertex/face editor for meshes already placed in the
  scene, landed via `docs/superpowers/plans/2026-09-12-mesh-authoring.md`
  (9 tasks) plus its spec.** `EditableMesh` (welded points + coplanar-
  grouped faces, `TestEnv/src/EditableMesh.h`), the `Mesh::Create*`/`Load`
  MeshData split (`GS/src/GS/Renderer/Mesh.h`/`.cpp`), a new `ObjWriter`
  (`GS/src/GS/Renderer/ObjWriter.h`/`.cpp`), and editor integration
  (`TestEnv/src/EditorSceneView.h`: an "Edit Mesh" Inspector button, a
  per-point overlay, the retargeted translate gizmo, Delete Point/Undo/
  Redo/Done/Cancel). **Shippable now: Move (drag a point) and Delete
  Point, plus session Undo/Redo and Done (export to `assets/<name>.obj`,
  reload through the entity's own `MeshComponent`) or Cancel.**
  **Known, deliberate scope gap — ask before extending:** Extrude and
  Split Edge (and Delete Face) are fully implemented and tested at the
  `EditableMesh` data layer but have **no UI path to reach them** — the
  plan's own Task 9 heading promised buttons for all four operations but
  its actual code only wired one; wiring the other three needs face-
  picking and edge-picking UI that was never designed anywhere in this
  plan (a genuine gap caught before dispatch, not an oversight discovered
  later). A natural follow-up is a small new sub-project (brainstorm ->
  spec -> plan) for that picking UI.
  A final whole-branch review (after all 9 tasks individually passed
  per-task review) caught two real, reachable bugs no per-task review
  could have seen — both fixed in one closing round: (1) a session never
  recorded *which* entity it belonged to, so deleting that entity (or
  Ctrl+Z past its placement) while editing left the session stuck open
  forever, locking the whole editor's selection; and, separately, seven
  of eight `Select()` call sites could silently redirect a session's
  live-preview/Done onto a different entity mid-edit -- both closed by a
  new `m_MeshEditEntity` + `ValidateMeshEditSession()` check that runs
  unconditionally every frame. (2) `RecalculateNormalsTouching`'s NaN-
  normal risk (dragging one point onto another) was still reachable after
  an earlier fix only closed it at creation time, not on every later
  move -- given the same degenerate-cross guard `Mesh.cpp`'s own
  `RecalculateNormals` already used. Also fixed: "Edit Mesh" no longer
  activates a session on a mesh that failed to load (would have silently
  exported an empty `.obj` on Done); a failed `Done` save no longer
  closes the session, so a save failure doesn't silently discard the
  edit. **Deferred, not fixed (tracked, not forgotten):** no guard against
  Play starting while a session is open; `Rebuild()` hardcodes UV to
  `(0,0)` for every point (the spec wanted UV carried forward from each
  point's originating vertex -- `FromMeshData` drops the mapping that
  would need); several Minor UX rough edges (bare clicks push a no-op
  undo snapshot; global Ctrl+Z isn't redirected to a session's own undo
  stack; the export path doesn't compose with the project's own path);
  no README changelog entry for this feature yet.
- **`ball.gss` graduated to compiled C++ -- the second and, for now, last
  script to (BreakoutRecreation's only other one).** Requested as "graduate
  the rest of the demos in order"; asked which of two readings that meant
  (there being only one remaining `.gss` file made "in order" ambiguous
  against the other candidate, giving every demo `OnExportToScene`
  support) and confirmed it meant this one.
  **Two real, load-bearing findings, both fixed rather than worked
  around**: (1) `ball.gss` read the paddle's position via `scene.
  findByTag("Paddle").getPosition()[0]` -- calling a transform getter on
  a *local* holding a found entity, not the `entity` parameter itself.
  The transpiler's `emitCall` only ever recognized `entity.getPosition()`
  by checking the callee object was literally the identifier `entity`;
  it now applies the same getter/setter mapping to any identifier, since
  `GS::Entity::Get<T>()` works the same regardless of which variable
  holds the handle -- mechanical, not semantic, same trust this file's
  identifier-call handling already places in a plausible name elsewhere.
  (2) The syntax-check step then caught a genuine **engine**
  const-correctness bug: `scene.findByTag` is declared with `const` in
  the GSS (`const paddle = ...`), which becomes a real `const GS::Entity`
  in the generated C++ -- and `GS::Entity::Get<T>()`/`Has<T>()` were never
  marked `const`, so calling either on a const handle failed to compile.
  Fixed in `GS/src/GS/Scene/Scene.h`: both are `const` now (safe --
  `m_Scene` is a pointer member, so constness doesn't propagate through it
  to `Scene::GetComponent`/`HasComponent`, which still need to stay
  non-const since they can lazily allocate a component store).
  Verified: a temporary self-test (5/5) using only externally observable
  `TransformComponent` data (vx/vy are private members of the compiled
  class now, invisible from outside) -- a wall bounce confirmed by x
  clamping exactly at `worldHalfWidth-ballRadius` then *decreasing* the
  next tick (proving vx actually flipped sign, not just that x stopped
  moving); a paddle catch confirmed the same way -- a hand-timed ceiling
  bounce, a hand-timed fall to the paddle's row, the paddle placed
  directly under the ball via a plain Transform write (no script
  involved), then a check that y rose on the following tick instead of
  continuing to fall, which only happens if the compiled `Ball` actually
  read the compiled `Paddle`'s live position through `scene.findByTag` and
  bounced off it -- the one genuinely new risk in this graduation, not
  just re-proving math that was already correct when this file was
  interpreted. Also confirmed visually: the real project, both scripts
  now compiled, run for 25 fixed steps with no error, the ball visibly
  moving under its own independent physics. Clean three-config build and
  a byte-identical Cube3D `--hide-ui` capture. All temporary
  test/generation scaffolding removed afterward; `ball.cpp` and its
  registry entry are the real, permanent artifacts, alongside the
  `emitCall`/`Entity::Get`/`Has` fixes.
- **The Inspector's ID field, requested 2026-09-11, queued behind the
  static-link execution work and the four "make the engine usable"
  sub-projects, both since finished.** Design question it was queued
  with -- "entity handle vs. a user-editable string name" -- resolved in
  favor of the string: `GS::TagComponent::Name` already existed, was
  already what `scene.findByTag(name)` resolves by (Breakout's
  recreation already depends on exactly this), and was already
  serialized -- it just had no Inspector field to edit it from. Every
  entity kept whatever default name `PlaceEntityCommand` gave it at
  creation ("Cube", "Light", ...) with no way to rename it short of
  hand-editing `scene.txt`. Building a second, parallel ID system next to
  one that already solved the same problem would have been exactly the
  duplicate machinery this project's own conventions warn against.
  `EditorSceneView`'s Inspector now shows an editable `InputText` labeled
  "ID" (undo-tracked through the same `EditFieldCommand` template every
  other field already uses -- confirmed it works for a `std::string`
  field, not just the `float`/`vec3`/`vec4`/`bool` ones it had been used
  for so far), a "Copy" button next to it (`SetClipboardText`, so the
  exact string can be pasted straight into a script), a live
  `scene.findByTag("<name>")` hint showing the call that would find this
  entity, and the raw entity handle underneath for reference.
  **Explicitly not built, and disclosed rather than silently left out**:
  no uniqueness is enforced on the name -- `scene.findByTag` already just
  returns the first match by a linear scan, so two entities sharing a
  name means a script asking for either gets the earlier one silently;
  fixing that is a separate, bigger feature nobody asked for.
  **A claim checked before it went in the UI, not assumed**: the disclosed
  caveat text originally said the entity handle is "not stable across a
  Save/Load" -- reading `Scene::Load`'s actual implementation first
  showed that's only true when something was created or destroyed
  in between (`Load` always `Clear()`s and recreates entities in file
  order from scratch, so a churn-free round trip is actually
  deterministic) -- corrected to say precisely that, rather than ship an
  overbroad claim that happened to sound right.
  Verified: a temporary self-test (5/5) confirmed the rename applies,
  undoes, and redoes correctly, and that the renamed value is exactly what
  a `scene.findByTag` linear scan would match against. The Inspector UI
  itself was confirmed with a real capture (`BreakoutRecreation`'s
  "Paddle" entity force-selected) showing the ID field pre-filled,
  editable, with the Copy button and handle caveat both rendering
  correctly. Clean three-config build and a byte-identical Cube3D
  `--hide-ui` capture (Inspector UI only, no demo-layer or rendering path
  touched).
- **Profiler → friendly stats -- last of the four "make the engine usable"
  sub-projects; all four are now landed** (lighting, project I/O, text
  editor above, this one). `ProfilerPanel` gained an "Overview" tab (opens
  first) showing FPS (smoothed over the last 60 frames -- reads the same
  "Frame" scope entry the existing frame-time line already did, not
  `Instrumentor::GetLastFrameMicros()`, which is documented as summing
  every nested scope and is explicitly not "time in the frame"), process
  memory (`/proc/self/status`'s `VmRSS`, Linux-only -- this repo only
  targets Linux), draw calls and triangle count (`GS::Renderer`/
  `Renderer2D::GetStats()`, which already existed and were already
  correct, just never surfaced here), and cached-mesh count/size (a new
  `MeshCache::All()` enumeration -- a thin snapshot of the existing store,
  not new tracking -- times each `Mesh`'s own already-tracked vertex/
  triangle counts). The old detailed scope table, frame/swap breakdown and
  trace-capture button all still exist, moved under a second "Advanced"
  tab rather than deleted -- still genuinely useful for tracking down what
  got slow, which "FPS: 62" alone can't answer.
  **A real gap found and fixed along the way, not just used as-is**:
  `Renderer`/`Renderer2D::GetStats()` were only ever reset by whichever
  demo happened to call `ResetStats()` itself at the top of its own
  `OnUpdate` (about half of them do) -- anything else, including the
  editor's own `EditorSceneView` scene rendering, had no reset at all, so
  reading the counters there would only ever have grown, never reflecting
  one real frame. `ProfilerPanel` now resets both right after reading them
  each frame, which is safe with the demos that already reset (confirmed:
  they read `GetStats()` from their own `OnImGuiRender`, which -- push
  order -- runs before `ProfilerPanel`'s does, so both see the same
  frame's true total before this reset clears it for the next one) and is
  what gives every other case a real number for the first time.
  **Explicitly not attempted, and why, checked rather than assumed**: no
  texture or audio number is shown -- neither has a cache or registry
  anywhere in this engine to enumerate (confirmed: `Texture2D::Create`
  has no dedup/registry the way `MeshCache` does for meshes, and
  `AudioEngine` has no clip cache either), so counting either would mean
  building new tracking infrastructure, not just a display change --
  out of scope for what was asked here.
  Verified: the Overview tab's numbers were sanity-checked against Cube3D
  (a known, small scene) -- draw calls, triangle count and mesh count all
  came back as plausible, small numbers, not zero or something obviously
  wrong; the Advanced tab was confirmed (via a temporary forced-tab-
  selection capture, since removed) to still show its full original
  content unchanged under the new tab structure. Clean three-config build
  and a byte-identical Cube3D `--hide-ui` capture (the whole panel is
  ImGui-only).
- **The embedded text editor: mouse support, a full set of standard
  keyboard shortcuts, and a real font -- third of the four "make the
  engine usable" sub-projects, landed in ranked order after lighting and
  project I/O (see those entries below).**
  **Mouse support, the single biggest gap named when this was scoped**:
  `TextBuffer::MoveTo(row, col, extend)` is the one new entry point --
  clamps both into range (a click past the last line or a short line's own
  end lands at the nearest valid position rather than being rejected), then
  reuses the exact same `BeginOrExtendSelection` path every keyboard `Move*`
  already goes through, so there is exactly one place selection state is
  touched no matter which input drove it. `TextEditorPanel::HandleMouse`
  drives it: a click (gated on `IsWindowHovered`, not just focus) positions
  the cursor and starts a drag; held-down frames after that keep extending
  regardless of hover (`m_MouseDragging`, not a re-check of hover, is what
  lets a fast drag keep working even if the cursor briefly slips outside
  the window); Shift+click extends the existing selection instead of
  starting a new one. `MouseToCell` converts screen space using the exact
  same origin `CellGrid::Render()` independently recomputes from
  `GetCursorScreenPos()` -- captured at the identical point in the frame
  for that reason.
  **Keyboard, all requested**: Ctrl+Left/Right (`MoveWordLeft/Right` --
  skip a whitespace run, then a run of "the same character category",
  matching Ctrl+arrow in most editors); Ctrl+Alt+Up/Down
  (`DuplicateLines`, selection- or current-line-aware, lands the
  cursor/selection on the *new* copy so repeated presses stack more of
  them); Ctrl+Enter / Shift+Enter (`InsertLineBelow`/`InsertLineAbove` --
  a blank, indent-matched line without splitting the current one, unlike a
  plain Enter); Alt+Up/Down (`MoveLinesUp/Down`, block-aware -- a whole
  multi-line selection travels together, not just the cursor's own line);
  Ctrl+F / Ctrl+R (a find bar, described below); Ctrl+Home/End
  (`MoveDocumentStart/End`). **Bells and whistles beyond the literal
  ask, because leaving them out would have made the rest feel
  half-finished**: Tab with a selection now indents every touched line
  (`IndentSelection`) instead of replacing the selection with a literal
  tab; Shift+Tab (`OutdentSelection`) removes up to 4 leading spaces (or
  one leading tab) from the current line or every selected one, each
  independently since a shallow line has less to give up than a deep one.
  **A real, pre-existing collision found and fixed getting Ctrl+F
  wired up**: `EditorMenuBar.h` already owns Ctrl+F for an unrelated
  feature ("Find Entity", searching the scene by tag) via its own
  event-dispatch handler, which had no idea which panel was actually
  focused -- pressing Ctrl+F while typing in the text editor would have
  opened *both* popups at once. Fixed with a small new global,
  `g_TextEditorFocused` (set every frame by `TextEditorPanel::OnImGuiRender`),
  that `EditorMenuBar`'s handler now checks before firing.
  **Find/replace**: `TextBuffer::FindNext`/`FindPrevious` wrap around the
  document and turn a match into the buffer's own selection (anchor at its
  start, cursor at its end), so it renders through the selection-highlight
  path that already existed for Shift-select -- no separate "current
  match" drawing was needed. Repeated presses chain forward/backward
  correctly because each search starts from the *existing* selection's own
  edge, not the cursor. `ReplaceAll` is deliberately a separate,
  **forward-only, non-wrapping** single pass from the document's start --
  proven (not just commented) to terminate even when the replacement
  itself contains the query (a temporary self-test replacing "a" with "aa"
  in "aaa" completes in exactly 3 steps, giving "aaaaaa"), which a naive
  wrap-around find+replace loop could not guarantee.
  **The font, and the actual cause of the reported clipped-"g" bug,
  confirmed rather than guessed**: no custom font was loaded anywhere in
  this engine -- `ImGuiLayer` was rendering with ImGui's compiled-in
  default (ProggyClean, ~13px), and it was specifically this font at this
  specific size that clipped descenders in `CellGrid`'s fixed-height cell
  rendering, not a cell-size mismatch in this project's own code (the cell
  height already came from the same `GetTextLineHeight()` metric ImGui's
  own text layout uses). Fixed by vendoring **DejaVu Sans Mono**
  (`TestEnv/assets/fonts/DejaVuSansMono.ttf` + its license text,
  Bitstream/DejaVu-licensed, explicitly redistributable) and loading it at
  16px -- a real file already installed and licensed for exactly this on
  this machine, not fetched fresh, given no network access in this
  session. `GS::ImGuiLayer::SetFontPath(path, sizePixels)` is a new
  **static** setter (not an instance method): `ImGuiLayer` is constructed
  and attached inside `Application`'s own constructor, before `TestEnv`'s
  constructor body -- the earliest an instance call could run -- ever
  executes, so the one place early enough is `GS::CreateApplication()`
  itself, before `new TestEnv()`. An empty path (the default) changes
  nothing, so every other engine user of `ImGuiLayer` keeps today's font.
  This is a global font swap, not scoped to the text editor -- the whole
  editor UI reads more clearly now, and the terminal (`CellGrid`'s other
  consumer) gets the same descender fix for free.
  Verified: a temporary self-test (36/36) directly exercising every new
  `TextBuffer` method against hand-derived expected positions/content --
  word-skip boundaries character by character, duplicate/move/insert-line
  row arithmetic, indent/outdent column correction on both selection
  directions, find wraparound and the `ReplaceAll` termination case above.
  The UI-level wiring (mouse, the find bar) was checked visually with
  temporary hooks: a real project file opened with the find bar forced
  open showed the query, Next/Prev/Case/Replace/Replace All controls laid
  out correctly, and a search for a real, uniquely-occurring identifier
  produced a highlight rectangle at exactly the right on-screen position
  (confirmed by cropping the actual capture, not by trusting the code) --
  the actual mouse click-through itself still needs a by-hand pass with a
  mapped window, same disclosed gap as every other UI feature in this
  project. The font fix was confirmed the same way: a real `.gss` file
  with descenders in it, captured before and after, shows every "g"
  rendering with its full loop intact. Clean three-config build and a
  byte-identical Cube3D `--hide-ui` capture throughout (the font change is
  ImGui-only and `--hide-ui` renders none of it; nothing here touches any
  demo-layer code path).
- **An in-editor file/folder browser replaces every typed-path field in
  project I/O -- second of the four "make the engine usable" sub-projects,
  next by the biggest-to-smallest ranking after lighting (see that entry
  below).** `FileBrowserPopup.h` is a new, reusable ImGui modal (`PickFolder`/
  `PickFile` modes, an optional extension filter) that reuses
  `FileTreePanel.h`'s own recursive-`TreeNode`, directories-first-then-
  alphabetical walk rather than inventing a second file-list convention --
  the arrow toggles a folder open/closed, clicking its label selects it
  (`ImGuiTreeNodeFlags_OpenOnArrow` plus `IsItemToggledOpen` to tell the two
  apart), and a file `Selectable` picks it directly. **Deliberately not a
  native OS dialog, and why, checked rather than assumed**: no CLI dialog
  helper (`kdialog`/`zenity`/`yad`) is installed on this machine to shell
  out to the way `RunSyntaxCheck` already shells out to `g++`; a real
  `xdg-desktop-portal` integration (confirmed running here, via `busctl
  --user list`) means either hand-rolling its async D-Bus request/response
  exchange by shelling out to `gdbus`/`busctl` and parsing their text
  output, or vendoring a library that already does this
  (nativefiledialog-extended), which pulls in a new build dependency (its
  Linux backend links GTK3) for a project that has otherwise built every
  other editor amenity itself (the text editor, the terminal) rather than
  reach for one. Wired into three places: `EditorMenuBar.h`'s New/Open/
  Save-As project modals each gained a "Browse..." button beside their
  existing "Folder" `InputText` (kept, not replaced -- typing still works)
  through one shared `DrawFolderField()` helper and one shared
  `m_FolderBrowser` instance, since only one modal is ever open at a time;
  the Inspector's `ScriptComponent` path field (`EditorSceneView.h`) gained
  the same beside its `InputText`, in `PickFile` mode filtered to `.gss`,
  rooted at the open project's folder. This second one is almost certainly
  the exact "saved by name, opened by typing the name" pain point named
  when this was requested. The browser resolves an absolute path (browsing
  needs one to walk arbitrary directories); the script field converts it
  back to a path relative to the working directory
  (`std::filesystem::relative`) before storing it, matching how
  `ScriptEngine`/`PlayMode` actually open a `ScriptPath` -- a plain
  `std::ifstream` against it, no project-folder prefix, so an absolute
  path would have worked by accident on this machine and silently broken
  the moment the project moved.
  Verified visually (no GUI-automation tool exists here, same disclosed gap
  every prior by-hand UI check in this project has recorded) via temporary
  hooks forcing the popup open with real data: a `PickFolder` browse of
  `assets/demos` correctly listed `BreakoutRecreation` as an expandable
  node; a `PickFile` browse of `assets/demos/BreakoutRecreation` filtered
  to `.gss` showed exactly `ball.gss`/`paddle.gss` and correctly excluded
  `project.gsproj`/`scene.txt`/`paddle.cpp`, with "Choose File" correctly
  disabled until one was clicked; the real "New Project" modal's layout
  (`InputText` beside "Browse...") captured correctly. All temporary hooks
  removed afterward. Clean three-config build and a byte-identical Cube3D
  `--hide-ui` capture (this only touches `EditorMenuBar`/`EditorSceneView`
  UI code, no demo-layer or rendering path). The actual mouse click-through
  itself -- clicking "Browse...", clicking a folder, clicking "Choose
  Folder" -- still needs a by-hand pass with a mapped window, same
  disclosed gap as everything else in this editor pivot.
- **Placed `GS::LightComponent`s now actually shade meshes in the editor's
  Scene view and Play mode -- the first of four "make the engine usable"
  sub-projects requested 2026-09-11 (see "What is next" for the other
  three, all still unstarted; biggest-to-smallest was the owner's own
  ordering and this was judged the biggest).** Confirmed before touching
  anything: `LightComponent` could already be placed, edited, and
  saved/loaded, but no shader anywhere read it -- `EditorSceneView`'s own
  mesh shader used one fixed half-Lambert direction, and the only thing
  `LightComponent` visibly did was a flat 2D gizmo ring in an unrelated demo
  (`SceneDemo.h`). `EditorSceneView::BuildShader` now reuses Cube3D's own
  point-light math verbatim (inverse-square-times-linear-fade attenuation,
  Lambert diffuse, Blinn-Phong specular -- not reinvented, since Cube3D's
  version was already correct and tested) summed over up to 8 enabled
  lights read from the scene each frame, instead of Cube3D's one hardcoded
  light. `u_AmbientStrength` is 0.25, deliberately higher than Cube3D's
  0.10 default: Cube3D always has exactly one light, an editor scene can
  have zero, and 10% brightness with nothing placed would read as
  "broken," not "unlit." Uniform arrays (`u_LightPositions[8]` etc.) are
  set element-by-element via `Material::Set("u_LightPositions[i]", ...)`
  -- `GS::Shader` has no array-upload method, and indexed uniform names are
  valid GLSL/OpenGL, so this needed no engine-level API change.
  **A second, related gap fixed alongside it, disclosed as scope creep
  because leaving it out would have made the primary fix hard to use**: a
  placed Light entity had no mesh and no gizmo either -- genuinely
  invisible in the viewport the instant it was deselected. `DrawLightGizmos`
  (mirroring the existing `DrawCameraRays` pattern exactly) draws a small
  axis-aligned "plus" through each light's position, tinted by its colour.
  Verified against arithmetic the shader's own code was not copied from:
  a temporary scene (a unit cube, `Mesh::CreateCube`'s own `+Z` face
  normal, and a light placed collinear with the camera so the geometry
  needs no trigonometry) gave a hand-computed expected pixel of
  `(0.7225, 0.7225, 0.7225)`; `Framebuffer::ReadPixelRGBA` measured
  `(0.7216, 0.7216, 0.7216)` -- within 8-bit quantization. **A real bug
  caught during that verification, not in the shading math**: the test's
  first attempt positioned the free-fly camera via `SetOrientation`
  (meant for the separate Play-mode camera), which `MoveCamera()`'s own
  unconditional per-frame `SetRotation(GetYaw(), GetPitch())` call silently
  reverted on the very next frame -- caught by comparing the camera's
  *measured* forward vector against what was actually asked for, not by
  trusting the call succeeded. Confirmed a clean three-config build and a
  byte-identical Cube3D `--hide-ui` capture (its shader is untouched, and
  `EditorSceneView::OnUpdate` -- and therefore this whole change -- returns
  immediately whenever a demo is active). All temporary verification code
  (the debug camera/pixel-read accessors, the test scene, the diagnostic
  traces that found the camera bug) was removed afterward.
- **`paddle.gss` is the first script actually graduated to compiled C++,
  registered in `CompiledScriptRegistry.h` as `GeneratedScripts::Paddle`.**
  Requested and built in the same session as the compiled-script execution
  wiring below, to prove the mechanism against a real script rather than a
  synthetic test class. Graduating it surfaced two things the wiring's own
  self-test (a hand-written stub) couldn't have:
  **(1) a real transpiler bug.** `paddle.gss` originally destructured
  `entity.getPosition()` (`let [x, y, z] = ...`) and read/wrote a shared
  `globalThis.paddleX` for `ball.gss` to see -- both explicitly unsupported
  by the transpiler's own design doc. Rewritten to index access
  (`const pos = entity.getPosition(); let x = pos[0];` -- exactly the
  spec's own prescribed fix) and explicit `: number` on every top-level
  const (a class member, so no `auto`). That alone still failed
  `g++ -fsyntax-only`: `x` auto-deduced as `float` (a `glm::vec3` component)
  compared against `paddleSpeed`/`worldHalfWidth`'s `double` inside
  `Math.min`/`Math.max`, and `std::min`/`std::max` refuse to compare
  different types -- `no matching function for call to 'min(const double&,
  float&)'`. A mechanical transpiler bug the syntax-check step exists
  specifically to catch, not a guess: fixed by giving `x` an explicit
  `: number` too instead of leaving it to deduce from `pos[0]`.
  **(2) the `globalThis` removal broke `ball.gss`'s only channel to read
  the paddle's position**, since a compiled entity has no shared QuickJS
  context to write into. Fixed the way the transpiler spec itself already
  named as the real replacement: `ball.gss` now calls
  `scene.findByTag("Paddle").getPosition()[0]` (already-built, unchanged
  native binding) instead of reading a global `paddle.gss` no longer
  writes. **Real, disclosed consequence of graduating one entity's script
  but not the other's**: `PlayMode::OnFixedUpdate` ticks interpreted
  scripts (`ball.gss`) before compiled ones (`paddle.gss`), so `ball.gss`'s
  read of the paddle's position is one fixed step stale relative to that
  same tick's paddle movement -- invisible at the demo's actual frame rate,
  but a real ordering fact, not an oversight.
  Verified with a temporary self-test (4/4: holding the Right key for 8
  fixed steps drives the compiled `Paddle::OnUpdate` to
  `worldHalfWidth(1.6) - paddleHalfWidth(0.18) = 1.42` exactly, computed by
  hand and clamped correctly rather than overshooting; a temporary
  `console.log` readback -- same "only public channel available" technique
  the original entity-scripting verification used -- confirmed
  `ball.gss`'s `scene.findByTag` read that same 1.42 back from the
  compiled entity; `Stop()` still reverts cleanly) plus a real
  `TranspileToCpp` + `g++ -fsyntax-only` round trip generating the checked-
  in `paddle.cpp`, a clean three-config build, a byte-identical Cube3D
  `--hide-ui` capture, and a visual `--scene`
  `assets/demos/BreakoutRecreation/scene.txt` capture under held Right
  input showing the paddle actually driven to the world edge while the
  ball's independent interpreted motion continued unaffected. All temporary
  test/debug scaffolding (the generation driver, the console.log line, the
  accessor it needed) was removed afterward; `paddle.cpp` and the registry
  entry are the real, permanent artifacts.
- **Compiled-script execution wiring: the GSS-to-C++ transpiler's own
  deliberately-deferred "how does it run" question, resolved as static
  linking and built.** `CompiledScript.h` defines `CompiledScriptEntry` --
  a flat struct of plain function pointers (`Create`/`CallOnStart`/
  `CallOnUpdate`), built per type by `MakeCompiledScriptEntry<T>(name)`
  from three non-capturing lambdas -- so every `GeneratedScripts::<Name>`
  class (all of which share the exact `OnStart(GS::Entity, GS::Scene&)` /
  `OnUpdate(GS::Entity, GS::Scene&, double)` shape `TranspileToCpp` emits)
  is describable without a virtual base touching generated code.
  `CompiledScriptRegistry.h` is the explicit registration table --
  `DemoRegistry.h`'s reasoning applied again verbatim: a generated `.cpp`
  is fully inline, so `#include`-ing it is enough to link it in, but
  forgetting the line should leave a script quietly interpreted (still
  correct), not silently do nothing. Landed empty (see the entry above
  this one for the first script actually registered in it).
  `PlayMode::Play()` now checks
  `FindCompiledScript(ScriptEngine::ClassNameFromPath(script->ScriptPath))`
  before opening the `.gss` file at all -- a hit constructs the native
  instance via `shared_ptr<void>` and calls its `OnStart` directly, a miss
  falls through to the unchanged interpreted path. `OnFixedUpdate`/`Stop`
  each got a second, parallel `s_CompiledPrepared` map beside the existing
  `s_Prepared` one (different lifetimes -- `shared_ptr` destruction vs.
  explicit `JSValue` release -- so they aren't folded into one). Moved
  `ClassNameFromPath` from `TextEditorPanel.h` to a `ScriptEngine` static
  method so both the preview pane and `PlayMode` name a graduated script
  the same way without one including the other.
  Verified with a temporary self-test (6/6: `Play()` routes a
  registry-matching `ScriptComponent` to a hand-written `GeneratedScripts`
  test class instead of touching its nonexistent `.gss` file, `OnStart`
  and two separate `OnUpdate` ticks mutate the entity's `Transform`
  natively, `Stop()` reverts it via the same snapshot an interpreted
  script already uses, and a second entity with a non-matching,
  nonexistent script path still falls through to the unchanged
  interpreted error path rather than crashing) plus a clean three-config
  build and a byte-identical Cube3D `--hide-ui` capture (neither touches
  any demo-layer code path).
- **GSS-to-C++ transpiler, all 6 of 6 tasks landed** (plan:
  `docs/superpowers/plans/2026-09-11-gss-to-cpp-transpiler.md`, spec:
  `docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md`).
  `ScriptEngine::TranspileToCpp(source, className, error, outCpp)`
  mechanically translates typed variables, arithmetic/`===`/`!==`/
  ternary, `if`/`for`/`while`, the native `entity`/`input`/`scene` API,
  `Math.*`, calls to same-file functions, arrow functions, template
  literals, simple classes, and simple generics into real C++ in a
  `GeneratedScripts` namespace, stamped with a `GS-GENERATED: <FNV-1a
  hash>` line the optimize-lock reads to detect a hand-edit. The `.gss`
  file open in the "Editor" tab now gets a live split-pane C++ preview,
  Ctrl+S (or a new "Build (Ctrl+S)" button) transpiles + syntax-checks +
  writes the `.cpp` (protected by the optimize-lock's three-way Cancel/
  Overwrite/Save-a-copy prompt when it's been hand-edited since), and
  saving a module fans that same check out to every script that imports
  it, one consolidated dialog rather than one popup per file. **Still not
  done, by design, not oversight**: imports aren't resolved before
  transpiling yet (calling an imported function fails the same
  "unrecognized call" way anything else unsupported does), and compiled
  code is never wired into Play — the spec's static-vs-dynamic execution
  question (see its "Goal" section) is still open on purpose.
  **Two real design/bug findings from Task 1, found by testing, not
  anticipated in the design**: requiring an explicit type annotation on
  *every* declaration turns out to be unworkable, since
  `entity.getPosition()` has no TypeScript declaration file at all --
  there's no correct annotation to write for `const pos =
  entity.getPosition()` no matter how the script is written. Fixed by
  falling back to C++'s own `auto` for an initialized local with no
  annotation (the transpiler still infers nothing itself -- `auto` is
  deduced by the compiler at the syntax-check step); required only when
  there's neither a type nor an initializer. Class member variables
  (top-level `let`/`const`) still always need one -- C++ doesn't allow
  `auto` on a non-static data member at all. Separately, the codegen
  driver's `JS_Call` had no `m_Deadline` set, unlike every other JS
  execution point in this file -- the first test run hung the process
  outright; fixed to match the established pattern.
  **One more from Task 2**: calling a locally-declared lambda variable
  isn't calling a "top-level function," so the entity/scene-prepending
  rule Task 1 built rejected it outright. Fixed: an identifier call now
  checks the top-level-function set first (prepend entity/scene), then a
  lightweight local-variable table (call directly -- a `[&]` lambda
  already captures what it needs), and only fails if neither recognizes
  the name. Template literal interpolation is deliberately narrower than
  "any expression": only a plain identifier (via that same lightweight,
  non-lexically-scoped type table) or literal, rejecting a general
  expression by name rather than tracking types through binary/call/
  property-access forms too, which would be real inference.
  **Task 3 (simple classes)**: a top-level class becomes its own C++
  class, a sibling of the entity's generated class in the same namespace
  -- not nested inside it, and its methods do *not* get entity/scene
  prepended, since a class has no relation to the entity-script closure
  model that prepending exists to preserve. `this.x` maps to `this->x`;
  `extends` maps to public inheritance; everything comes out `public:`
  (access modifiers/`readonly` aren't translated). Found while
  implementing, not in the original plan: `super(...)` calls aren't a
  recognized expression form, so a derived class's constructor can't call
  its base constructor -- fields/methods/single-inheritance still work.
  **Task 4 (simple generics)**: `<T>` and `<T extends number>` both
  become a plain `template<typename T>` on a top-level function or
  const-assigned arrow function (entity/scene still prepended). Every
  constraint, simple or complex, is uniformly dropped rather than
  translated or specially detected -- C++17 has no `concept` keyword to
  express any of them with, so there was no correctness reason to tell a
  simple constraint from a complex one, only to drop both the same way.
  Generic classes are explicitly rejected by name rather than silently
  mistranslated (`T` used as a field type with no enclosing `template<>`
  would otherwise fail at the syntax-check step with a confusing error
  instead of a clear one).
  Verified with temporary self-tests (15, 11, 12, 8 -- 46 checks total,
  0 failed) that don't stop at "the generated text looks right" -- every
  case is additionally piped through a real `g++ -std=c++17 -fsyntax-only`
  invocation (TestEnv's own Linux/Debug defines) confirming a real
  compiler accepts it, which is what caught the deadline and lambda-call
  bugs above. Clean three-config build after each task.
  **Task 5 (syntax-check + live panel)**: include paths come from
  `premake5.lua`'s real `includedirs`, anchored by a new `GS_REPO_ROOT`
  premake `define` (absolute, baked in at generation time -- the running
  executable has no other reliable way to find `GS/src` from wherever its
  own `bin/<config>/TestEnv/` happens to sit). **Real bug, not
  anticipated**: the second `RunSyntaxCheck` call in the self-test hung
  the whole process. Root cause: `std::system` (fork+exec) from a process
  that already has other threads running (this app's audio thread) is a
  known hazard -- a lock another thread held at the instant of `fork()`
  can leave the forked child deadlocked before `exec()` replaces it.
  Fixed by wrapping the compiler invocation in `timeout 10`, so that class
  of hazard degrades to a reported timeout instead of a frozen editor;
  confirmed empirically to resolve it, though the exact mechanism wasn't
  isolated further than that plausible, well-documented cause.
  **Task 6 (optimize-lock + fan-out)**: `ScriptEngine::
  IsGeneratedFileHandEdited` finds a generated file's body by searching
  for its known starting text rather than hardcoding "skip N header
  lines" (so a future comment-wording change can't silently break it) and
  treats a file with no stamp at all as hand-written -- protected, not
  assumed safe. `PlayMode::FindDependentScripts` parses the dependency
  manifest fresh per call.
  Verified: Tasks 5-6's non-UI logic with a temporary self-test (11/11);
  the UI itself with a `--capture` against the real, running editor
  (via a temporary, since-deleted `EditorShell` accessor and
  `ImGui::SetWindowFocus`, since "Scene"/"Editor" share a tab group) that
  opened the actual `paddle.gss` and showed the split pane, the
  extension-gated "Build (Ctrl+S)" button, and the live preview firing
  for real. **That capture surfaced a genuine, previously-unknown
  finding**: `paddle.gss`'s `const paddleSpeed = 2.2;` (and likely
  similar lines elsewhere) has no type annotation, so it isn't compilable
  as written today under the "class members always need one" rule --
  not a transpiler bug, a real, disclosed consequence of running a new,
  stricter tool against code that predates it. Clicking the popups
  themselves still needs a human -- no GUI automation tool exists here.
  Clean three-config build; Cube3D's byte-identical `--hide-ui` capture
  confirmed unchanged.

- **Auto-closing pairs in the embedded text editor** -- typing `(`/`[`/`{`/
  `"`/`'` inserts its match too, cursor left between them; typing a closer
  that's already sitting right there steps over it instead of doubling
  it; typing an opener with text selected wraps the selection instead of
  replacing it; backspacing inside an empty auto-inserted pair removes
  both characters together. Lives entirely in `TextBuffer::
  InsertCharWithPairing` (new) plus a small addition to `Backspace`;
  `InsertChar`/`InsertText` themselves are untouched, so pasted code and
  `InsertTab`'s spaces aren't paired a second time on top of already-
  balanced text. No lexical awareness -- a `(` typed inside a string or
  comment pairs the same as anywhere else, the same simple-mechanical cut
  the syntax highlighter's own single-line tokenizer already makes.
  Unrelated to the GSS-to-C++ transpiler design in progress -- a general
  editor QoL request, not part of that spec. Verified with a temporary
  self-test (9/9, including the exact `function test(` case from the
  request) and a clean three-config build.

- **The scripting language has a name and an extension: GameStart Script,
  `.gss`.** `ScriptEngine::ResolveImportPath` now tries `.gss` before
  `.ts` when a bare import specifier has no extension; the Breakout
  recreation's `paddle.ts`/`ball.ts` were renamed to `.gss` to match (a
  plain rename, `scene.txt`'s two `script` lines updated, no content
  changed). Confirmed via `--scene` that the renamed project still opens
  and its scripts still resolve with no warnings. `.gs` (the name floated
  before this one) never actually shipped anywhere, so there was nothing
  else to migrate.

- **Cross-file script imports** (spec:
  `docs/superpowers/specs/2026-09-11-script-imports-design.md`). A script
  can now `import { name } from "./relative/module"`; the target may
  itself `export function`/`export const` and import further modules.
  Not real ES modules -- QuickJS modules can't take function parameters,
  and entity/input/scene being parameters (not globals) is specifically
  what keeps two entities running the same script file from sharing
  state, so making an entity's own script a real module would have
  reopened that. Instead `ScriptEngine::PrepareEntityScript` resolves
  imports itself, using the bundled TypeScript compiler's own parser
  (`ts.createSourceFile`, not a hand-rolled scan) to find import/export
  statements, splices each resolved module in as its own IIFE ahead of
  the entity's code, then runs the existing single `ts.transpileModule`
  call over the assembled text -- unchanged from before this feature.
  Real, disclosed deviation from real ES modules: every entity gets its
  own private copy of an imported module, so top-level mutable state in a
  module is **not** shared across entities the way it would be in real
  JS -- modules should be stateless helpers, which is what the actual use
  case (shared movement/collision math) needs anyway. The module-parse
  cache is scoped to one Play() session (cleared at the start of every
  one), not `PlayMode::s_ScriptEngine`'s lifetime (which spans every
  Play/Stop cycle for the whole app run) -- editing a module and pressing
  Play again picks up the change, same freshness guarantee an entity's own
  script already had.
  **Built ahead of its only consumer, on request**: a
  `.gs-dependencies.txt` manifest (`<project>/`) is written fresh at the
  end of every Play() that resolved an import, listing each script's
  transitive module dependencies. Nothing reads it yet -- it exists for
  the not-yet-designed TS-to-C++ transpiler, which will need to know a
  module edit should trigger the same overwrite-conflict handling a
  direct script edit would. Also confirmed, separately: this repo's `.gs`
  extension request needs no code at all -- nothing anywhere (file tree,
  text editor, script runtime) has ever dispatched on file extension.
  Verified with a temporary self-test (11/11: two entities sharing one
  cached module independently, a module importing another module, two
  modules exporting the same name under different import aliases not
  colliding, a missing import and a circular import both failing to
  prepare cleanly with a named error rather than crashing or hanging, the
  manifest's content and its correct omission of a script that failed to
  prepare, and a module edit between Stop and Play actually taking
  effect) and a clean three-config build.
- **Cross-entity scripting: `scene.findByTag`/`spawn`/`destroy`, the two
  capabilities Breakout's recreation exposed as missing rather than
  built.** A third `scene` parameter joins the existing `entity`/`input`
  ones passed into every entity script (`ScriptEngine.h`'s wrapper is now
  `(function(entity, input, scene) {...})` -- additive, no existing script
  needed to change). `scene.findByTag(name)` linear-scans the live scene
  and returns a native `Entity` binding (or `null`); `scene.spawn({
  position?, scale?, mesh?, color?, tag? })` creates a real entity with a
  Transform and, if `mesh` names a `MeshCache` key, a `MeshComponent`,
  returning its binding; `scene.destroy(entity)` removes any entity, not
  just the caller. Deliberately no `script` spawn option -- a spawned
  entity running its own script would need `PlayMode`'s `s_Prepared`
  bookkeeping reachable from `ScriptEngine.h`, a layering change not worth
  it for bricks, which only need to exist and be destroyed.
  **One real bug this created, found and fixed before it shipped**:
  `scene.destroy()` can remove an entity that has its own running script
  (self-destruction, or a ball destroying a brick that also scripts
  itself) -- `PlayMode::OnFixedUpdate` still held its `PreparedScript` and
  would have kept calling its stale `OnUpdate` every tick forever. Fixed
  by checking `g_EditorScene.IsValid()` before each call and dropping any
  entry that's gone stale (releasing its `JSValue`s), checked entity-by-
  entity within the same pass rather than once up front, so an entity
  destroyed earlier in the same tick doesn't get one extra spurious call.
  Verified with a temporary self-test (9/9: findByTag locating and
  mutating another entity, spawn producing a real entity with the
  requested mesh/position, destroy removing the target, and -- reading a
  value back out of the shared JS context via `RunScript`'s
  `console.log`, the only public channel available for it -- confirming
  the destroyed entity's script tick count stops changing across five more
  fixed updates instead of continuing to climb) and a clean three-config
  build. No capture-based regression sweep: this touches script bindings
  and `PlayMode`'s update loop, not rendering or the `Scene::Save`/`Load`
  format, so that check wouldn't have caught anything a lighter one
  couldn't.
- **Selection/clipboard, undo/redo, and syntax highlighting for the
  embedded text editor, fourth and last of the editor-pivot's named
  sub-projects.** `TextBuffer.h`: Shift+move extends a keyboard-driven
  selection (mouse-drag was never wired to a cursor position to begin with,
  so it stays out of scope); Ctrl+C/X/V go through `ImGui::Set/GetClipboardText`
  (the OS clipboard, via the existing GLFW backend); Ctrl+Z/Ctrl+Shift+Z
  (or Ctrl+Y) undo/redo off a snapshot stack that coalesces consecutive
  same-kind edits (typing "hello" is one undo, not five) and breaks the
  run on any cursor move, so type/move-away/move-back/type is two undos.
  `TextEditorPanel.h`: a single-line tokenizer colors keywords/strings/
  `//` comments using the `Keyword`/`StringLiteral`/`Comment` theme colors
  that were already sitting there reserved for this; a construct spanning
  lines (`/* */`, a template literal) just renders uncolored rather than
  being tracked wrong, since the small single-purpose scripts this editor
  actually opens don't use them. Verified with a temporary self-test
  (13/13: coalesced-undo, selection+delete, multi-line select-all, paste
  not inheriting newline's auto-indent, a mixed insert/newline/insert undo
  chain) and a clean three-config build; no capture-based regression sweep
  this time — this is self-contained editor content with no shared
  invariant or serialization format riding on it, so a lighter check was
  the right amount (the owner asked to calibrate verification cost to
  what actually breaks expensively later, not apply the same weight
  everywhere).
- **Breakout's paddle and ball recreated inside the editor, third of four
  sub-projects toward "recreate any existing demo inside the editor" (file
  tree, entity/component scripting, this, text-editor refinements).**
  `TestEnv/assets/demos/BreakoutRecreation/` is a real, checked-in editor
  project (`project.gsproj` + `scene.txt` + `paddle.ts` + `ball.ts`) --
  openable via File > Open or `--scene`, not a code-only construction. A
  flattened-cube paddle and a sphere ball, each with a `ScriptComponent`,
  reuse Breakout.h's exact constants (`s_PaddleSpeed`, `s_PaddleSize`,
  `s_BallRadius`, `s_BallStartSpeed`) so the motion is numerically
  comparable, not just visually similar. Deliberately cut, and disclosed as
  such rather than silently dropped: no bricks (45 entities with no
  spawn/destroy-from-script path to clear one), no score/lives/game-over
  (no UI hook reaches a script), no "ball rides the paddle until Space"
  launch phase, and the floor bounces the ball back instead of ending a
  life (no lives to lose).
  **One gap this exposed and fixed**: `ScriptEngine::KeyNameToCode` only
  recognised single letters/digits and `"SPACE"` -- arrow keys had no name
  at all, even though Breakout.h itself binds movement to both WASD and
  arrows. Added `"LEFT"/"RIGHT"/"UP"/"DOWN"`.
  **One real architectural finding, addressed rather than deferred**:
  Breakout is a 2D game on an orthographic camera; the editor's `Scene`
  viewport is 3D-only and always rendered from its own free-fly camera,
  never from a placed `CameraComponent` -- watching this recreation would
  have meant flying the camera into position by hand every time Play
  starts. `EditorSceneView::ActiveCamera()` now renders from the scene's
  active `CameraComponent` (its own `GS::PerspectiveCamera`, positioned and
  oriented from that entity's `Transform` via the same rotation-only
  `ForwardFromRotation()` helper `DrawCameraRays` uses) whenever Play is
  active and one exists, falling back to the free-fly camera otherwise --
  the free-fly camera's own state is never written to, so Stop() leaves it
  exactly where the user left it.
  **A second real finding, left as a finding rather than built**: the ball
  needs the paddle's live X position for its collision check, and the
  current scripting API has no way for one entity's script to query
  another's Transform ("querying other entities" is the entity-scripting
  spec's own deferred item). Solved without new native API surface: every
  entity's script runs in the same shared QuickJS context for one Play
  session (`PlayMode::s_ScriptEngine` is one instance, not one per entity),
  so `paddle.ts` writes its X to a plain `globalThis.paddleX` each tick and
  `ball.ts` reads it -- ordinary JS closure/global semantics, not a new
  binding. Confirmed genuine (not just two idle simulations) by counting
  actual paddle-catch events during verification.
  Verified by running the actual `Breakout` class's own `Step()` (bricks
  and the Space-to-launch phase neutralised via temporary accessors, since
  neither is part of what this recreation claims to reproduce) side by
  side with `PlayMode::Play()`/`OnFixedUpdate()` against the real project
  files, both driven by the same synthetic `GS::Input::SetPlaybackSnapshot`
  sequence (a simple chase controller steering input from the reference
  ball's live position, so the paddle genuinely catches it instead of the
  two runs diverging after the first miss) for 900 fixed steps (~15
  simulated seconds, several bounce cycles, switching from WASD to arrow
  names partway through to exercise both) -- paddle position matched to
  1.2e-7 and ball position to 1.6e-4 (float rounding), with 5 real paddle
  catches observed. A `--hide-ui` capture during Play shows the paddle and
  ball rendered correctly from the scene's own camera. The same
  byte-identical Cube3D `--hide-ui` capture (step 5) confirmed unchanged.
- **Two small editor-viewport gaps flagged during the Play/Stop work,
  picked up right after it landed: a camera-direction indicator and a way to
  place lights.** `EditorSceneView::DrawCameraRays()` draws one short line
  per `CameraComponent` entity, from its position along the direction its
  `TransformComponent::Rotation` faces (rotation-only, in `GetTransform()`'s
  own X-then-Y-then-Z order, against a local forward of `(0,0,-1)` -- the
  same convention the editor fly-camera's default yaw already assumes) --
  before this, a placed camera had no mesh and rendered as nothing at all.
  A "Light" button next to "Camera" in the Tools panel places a
  `GS::LightComponent` entity via the same `PlaceEntityCommand` path, and
  the Inspector gained a colour/radius/enabled block for it, matching the
  Camera block's shape. Two gaps found and fixed along the way, the same
  class of bug the `ScriptComponent` fix below already went through once:
  `Scene::Save`/`Load` had no `"light"` line at all (would have silently
  dropped every placed light on the first Save), and `DeleteEntityCommand`
  didn't capture/restore `LightComponent`, so undoing a light's deletion
  would have resurrected it without its light. Both fixed to match the
  existing per-component pattern; `Scene.h`'s persistence comment now names
  Light as the sixth (well, seventh) persisted component. Verified with a
  temporary self-test (12/12: forward-vector math against hand rotation,
  Save/Load round-trip keeping `Radius`, undo-after-delete restoring the
  placed `Radius` rather than a default one) and a `--hide-ui` capture
  showing the ray render (a camera at the origin facing +X after a 90°
  yaw shows a short line running right, as expected), plus the same
  byte-identical Cube3D `--hide-ui` capture (step 5) confirmed unchanged
  since neither addition touches any demo-layer code path.
- **Play/Stop in the editor menu bar, second of four sub-projects toward
  "recreate any existing demo inside the editor" (file tree, entity/component
  scripting, recreate one demo, text-editor refinements).** `GS::ScriptComponent
  { std::string ScriptPath; }` (`Components.h`) plus an Inspector Add/Remove
  Script block (`EditorSceneView.h`) let any placed entity point at a `.ts`
  file. `ScriptEngine.h` gained a native `Entity` binding (`getPosition`/
  `setPosition`/`getRotation`/`setRotation`/`getScale`/`setScale`, backed by
  `TransformComponent`) and a shared `Input.isKeyDown`, both passed into a
  script's `OnStart`/`OnUpdate` as function *parameters* rather than globals
  so two entities running the same script source never share state -- a
  two-entity self-test exercised exactly that. `TestEnv/src/PlayMode.h` is
  new: `Play()` snapshots `g_EditorScene` via the same `Scene::Save` a real
  project already uses (to a scratch `play_snapshot.tmp`, outside the project
  folder), prepares every `ScriptComponent`'d entity through `ScriptEngine`,
  and runs each one's `OnUpdate` from a new `EditorMenuBar::OnFixedUpdate`
  override every fixed step; `Stop()` reverts via `Scene::Load` on that same
  snapshot, discarding whatever the scripts did. Plain "Play"/"Stop" buttons
  sit in the menu bar strip itself (`DrawPlayControls()`), gated on
  `g_ActiveDemo` the same way Undo/Redo/Find already are. A review during
  Task 3 found and fixed a real Critical bug one level down: `Scene::Save`/
  `Load` (`Scene.cpp`) never had a `"script"` line at all, so every
  `Stop()` was silently and permanently deleting `ScriptComponent` from the
  snapshot -- fixed with a line matching the existing `mesh` block's
  convention, then re-verified.
  Verified with deterministic self-tests (QuickJS smoke 4/4, `ScriptEngine`
  transpile-run-recover 6/6 including the two-entity closure-collision case,
  `PlayMode`'s own Play/tick/Stop-and-revert cycle 11/11 including the
  script-survives-the-snapshot check the Scene.cpp fix required), a
  byte-identical `--hide-ui` capture (Cube3D, step 5, matching MD5 against a
  pre-this-plan reference build in a throwaway worktree at the prior commit --
  `PlayMode::OnFixedUpdate` early-returns off-editor and `DrawPlayControls()`
  only ever runs from `OnImGuiRender`, which `--hide-ui` skips), and a
  code-driven capture standing in for the live click-through: no GUI-
  automation tool exists in this environment, so a temporary layer
  (deleted after use) placed a scripted cube through the same
  `PlaceEntityCommand` path the Tools panel's "Cube" button uses, then drove
  `PlayMode::Play()`/`OnFixedUpdate()`/`Stop()` from its own `OnFixedUpdate`
  across real frames of a `--lockstep` run -- three captures (before Play,
  mid-Play, after Stop) show the cube centered, then visibly shifted right
  with its rendered position exactly 13 ticks x 0.1 units/tick = 1.3 units
  off origin, then back to a pixel-for-pixel MD5 match of the pre-Play frame.
  This exercises the exact `Play`/`OnFixedUpdate`/`Stop` calls the menu bar
  buttons make, just from code instead of a mouse click -- the actual button
  click-through itself is still open, same disclosed gap as every prior
  by-hand UI check in this pivot.
  **A review caught and this fixed a genuine crash-on-quit:** exiting the
  app while Play was still active (Stop() never clicked -- the single most
  ordinary way to leave this feature) hit a QuickJS shutdown assertion
  (`quickjs.c:2704, JS_FreeRuntime: Assertion 'list_empty(&rt->gc_obj_list)'
  failed`), reproduced 100% in both Debug and Release -- `PlayMode::s_Prepared`'s
  `JSValue`s are only released by `Stop()`'s own `ReleasePreparedScript` loop,
  so a process that exits mid-Play leaves them alive when
  `PlayMode::s_ScriptEngine`'s destructor later calls `JS_FreeRuntime` into a
  runtime that still has live objects. Fixed in `~TestEnv()` (`TestApp.cpp`):
  `if (PlayMode::IsPlaying()) PlayMode::Stop();`, run before
  `EditorHistory::Clear()`/`g_EditorScene.Clear()` (already there for the
  same "before the GL context goes away" reason) so `Stop()`'s own
  `Scene::Load` revert has a normal scene to load into. Verified with a
  second temporary code-driven repro (deleted after use): Play at fixed
  step 2, never call Stop, exit via `--capture-step` -- exit 0, no assertion,
  in both Debug and Release, where the unfixed build crashed 100% of the
  time under the identical repro.
- **A file tree panel, first of four sub-projects toward "recreate any
  existing demo inside the editor" (file tree, entity/component scripting,
  recreate one demo, text-editor refinements).** `FileTreePanel.h` is a new
  "Files" dock tab (tabbed with "Outliner" in the upper-left slot) that
  walks the open project's folder with `std::filesystem` fresh every frame
  rather than caching a tree -- ImGui's own `TreeNode` already tracks each
  node's open/closed state by ID for the running session, so there was
  nothing left to keep in sync by hand -- that state resets on relaunch (it
  isn't serialized to `imgui.ini`, only window/table/dock layout is), which
  is fine for a live filesystem view. A returning session with an existing
  `imgui.ini` from before this landed will show "Files" as a floating window
  rather than docked, until "Reset to default layout" is used -- the same
  pattern every panel added to this editor has had, since `BuildLayout` only
  runs its full split-and-dock sequence on a layout it built itself.
  Directories sort before files, alphabetically within each group, and
  dot-prefixed entries are skipped.
  Clicking a file returns its path from `OnImGuiRender()`, which
  `EditorShell` forwards into a new `TextEditorPanel::OpenFile(path)` --
  the same method the "Editor" tab's own "Open" button now calls, so a
  tree click and typing a path by hand go through identical logic. No
  project open shows a disabled placeholder instead of an empty panel.
  Verified with a byte-identical `--hide-ui` capture (Cube3D, step 5,
  matching MD5 before and after) confirming the panel is never drawn
  outside `EditorShell::OnImGuiRender`, plus a visual capture of a real
  project (a handful of files and subfolders created by hand) showing the
  tree correctly sorted. The live click-through itself (click "Files",
  expand a folder, click a file, confirm it loads into "Editor") was not
  attempted with a real window in this session: `xdotool`/`grim` are
  present but the immediately preceding scripting-runtime entry above
  already reproduced them being unreliable for this exact purpose in this
  environment, and opening a window here steals keyboard focus from
  whatever else the machine is doing. Same disclosed gap as every prior
  by-hand UI check in this pivot; still open.
- **A scripting runtime: TypeScript scripts run from the editor's "Editor"
  tab via a new "Run" button, output routed to the "Build Output" panel.**
  QuickJS (vendored `GS/vendor/quickjs`, `quickjs-libc.c` deliberately
  excluded so no script gets file/process/OS access) is the engine; the real,
  unmodified `lib/typescript.js` from `typescript@5.6.3` is vendored as
  `TestEnv/assets/typescript.js` (refresh instructions in the plan) and
  transpiles a script's TypeScript to JS inside the same QuickJS context that
  then runs it. `ScriptEngine.h` owns one persistent `JSContext` across runs,
  but each run's transpiled code executes inside its own function scope so a
  script's top-level declarations don't collide with the next run's -- and
  captures `console.log`/`console.error`/`console.warn` output and thrown
  errors into a `ScriptResult{Ok, Output, Error}`; `TextBuffer::FullText()`
  joins the buffer's lines the same way `SaveToFile` already does, so what a
  script saves and what it runs agree byte-for-byte. `TextEditorPanel`'s file
  bar gained a "Run" button beside Open/Save; `EditorShell` owns the one
  `ScriptEngine` instance, wires it into the text editor in `OnAttach`, and
  the "Build Output" stub now shows the script's captured output, or its
  error in red, or a placeholder if nothing has run yet. No engine/scene
  bindings exist -- `console.log` is the only I/O surface.
  `ts.transpileModule()` is a single-file, no-`Program` API and never runs the
  type checker -- a genuine type error (`let x: string = 5`) transpiles clean
  and runs; only syntax errors (e.g. `let x: number = ;` -> "Expression
  expected.") surface as diagnostics. Verified with
  deterministic self-tests (QuickJS smoke test 4/4, `ScriptEngine`'s
  transpile-run-recover cycle including a deliberate-throw path 6/6) and a
  byte-identical `--hide-ui` capture (Cube3D, step 5, matching SHA-256 before
  and after) confirming `ScriptEngine` never runs unless "Run" is clicked,
  which `--hide-ui` never allows. The live click-through itself (type a
  script, click Run, read "Build Output", then a deliberately broken script
  for the red-error path) could not be driven in this session: no GUI-
  automation tool exists here, and a first-hand check reproduced the exact
  trap `CLAUDE.md` already documents -- `grim` refuses to capture this
  compositor's output at all, and `xdotool search` returned two window IDs
  for the one live TestEnv window, i.e. it cannot be trusted to target the
  right one. Same disclosed gap as every prior by-hand UI check in this
  pivot; still open.
- **A self-built text editor, replacing the embedded-Neovim plan.** The
  "Editor" tab (Task 1's stub) is now real: `TextBuffer.h` is a pure-logic
  line buffer (insert/newline-with-indent/backspace/delete/cursor movement,
  load/save), `EditorTheme.h` is an Everforest-dark palette verified against
  the colorscheme's own source (`autoload/everforest.vim`'s dark/medium-
  contrast block) rather than guessed, and `TextEditorPanel.h` (`EditorShell.h`
  now owns one alongside `m_Terminal`) glues both through the same `CellGrid`
  the terminal renders with -- a right-aligned line-number gutter, current-line
  highlight, and an inline path field with Open/Save buttons (no menu
  integration). `WantCaptureKeyboard` is set the instant the panel is
  focused, and `HandleInput` skips the buffer entirely while `io.WantTextInput`
  is true so the path field and the buffer never double-consume the same
  typed character -- confirmed with a standalone probe against the real
  vendored ImGui (mirroring the mouse-capture probe from Task 1 of this same
  plan): a real click-then-type sequence never leaks a character into the
  buffer, even typed the same frame the field is clicked. Verified with a
  deterministic buffer-to-grid self-test (9/9), a supplementary self-test for
  `InsertNewline`'s mid-line split with re-indentation that Task 2's own test
  never exercised (9/9), a byte-identical `--hide-ui` capture against the
  pre-task baseline (`TextEditorPanel` never constructs into active use under
  `--hide-ui`), and an in-engine capture with the "Editor" tab forced selected
  via `imgui.ini`'s own dock-tab ID (no live interaction needed) whose pixels
  were sampled directly and matched the theme's `Background`/`Foreground`
  bytes exactly. Live typing, indent-preserving Enter, and Open/Save against
  the mouse-and-keyboard-driven UI still need a by-hand pass with a mapped
  window -- same open item the terminal's own landing entry recorded, for the
  same reason (no safe way to inject keyboard/mouse input in this session).
  There is no horizontal scrolling yet -- a cursor or loaded-file content past
  the visible text width clamps at the right edge (`RenderBufferToGrid`)
  rather than rendering there; a real `m_ScrollCol` mirroring `m_ScrollRow` is
  a known, deferred gap.
- **An embedded terminal, sub-project 1's step after the menu bar.**
  `TerminalPanel.h` replaces the "Terminal" stub with a real PTY-backed
  shell: `Pty.h` wraps `forkpty()`, `libvterm` (vendored,
  `GS/vendor/libvterm`) turns its output into a styled cell grid, and
  `CellGrid.h` (a generic, PTY-agnostic grid-of-cells widget -- the
  embedded text editor after this reuses it) renders it in ImGui.
  Verified with a deterministic self-test feeding a known escape sequence
  directly into libvterm (8/8 checks), and with an in-engine headless
  capture showing a real `forkpty()`-spawned zsh printing its own themed,
  colored prompt into the panel. Typing, panel-resize propagating to the
  shell's reported terminal size, and restart-on-exit still need a by-hand
  pass with a mapped window -- this session ran against a live desktop with
  no safe way to inject keyboard input, so that check is still open.
- **A menu bar and conventional editor layout, sub-project 1's next step
  after scene composition.** `EditorMenuBar.h` adds File/Edit/View/Help;
  File covers project open/save/rename via a new `ProjectManifest`
  (`EditorProject.h`, `project.gsproj`); Edit drives a minimal undo/redo
  command stack (`EditorHistory.h`: `PlaceEntityCommand`/
  `DeleteEntityCommand`/`EditFieldCommand`, wired into the Place/Delete/Edit
  UI actions); View switches named layout presets (`editor_layouts.txt`).
  `EditorShell.h`'s docking is reshuffled to match: Outliner/Tools/Demos
  left, Profiler/Inspector right, Terminal/Build Output/Textures tabbed
  along the bottom (all three still stubs -- the terminal is next, below).
  Verified with a headless capture showing the panel placement, and a
  temporary self-test round-tripping a project manifest through
  create/open/rename/reopen (8/8 passed).
- **Editor scene composition, sub-project 1 of the editor pivot.** The editor
  boots into `EditorShell.h`/`EditorSceneView.h` (`g_ActiveDemo` starts at
  `InvalidDemo`); `g_EditorScene` starts blank only on a first run with no
  `--scene` -- otherwise `LoadEditorProjectFromCommandLine` (`EditorProject.h`)
  reopens whatever `editor_last_scene.txt` names from the previous session.
  `GS::Scene::Save`/`Load`
  round-trip a scene through a `gs-scene` text format keyed by
  `MeshComponent::SourcePath` and resolved through the new `GS::MeshCache`;
  File > Open Demo can open any opted-in demo's placed content as a starting
  scene via `DemoLayer::OnExportToScene`. Cube3D is the one demo that opts in
  so far (`CanOpenInEditor = true`). Verified end-to-end: a hand-built scene
  and the same scene saved-then-reloaded via `--scene` render byte-identical
  captures.
- **Chunk meshes merge into fixed 3x3x3 groups, on the planet too.**
  `VoxelPlanet.h`'s chunks now draw the same way TerrainLab's do: 1,459
  meshed chunks down to 167 groups on a landed Earth. Harder than the flat
  field — chunks stream, so a group is dirtied by `StreamAround`/
  `EvictBeyond` rather than an edit, and each member's vertices have to be
  re-expressed against the group's own double-precision origin before being
  concatenated. Verified by a self-test showing triangle counts exactly
  preserved under grouping, plus byte-identical captures across repeated
  runs and Debug/Release. See the changelog, 2026-09-07. This finishes the
  roadmap item that started with TerrainLab's 93-down-to-9 (changelog,
  2026-09-04).
- **The context pipeline.** This file, a generated trap index
  (`./gs.py traps`, `--check` to detect drift), and a tiered read ladder in
  `CLAUDE.md`. Answering "what are we working on" cold went from ~252k tokens
  and wrong to ~3.9k and right.
- **Instancing.** Panels and animals batched: 764 draw calls → 33, submission
  1420 µs → 66 µs. Verified by rendering both ways and comparing pixels —
  byte-identical, 3,686,400 of 3,686,400 samples. Frame time did not move,
  because 16.667 ms is vsync.
- **`./gs.py prune`.** The checkout was 9.7 GB against 1.2 MB of source.
  Now 1.7 GB.
- **Life.** Boids, an Ornstein–Uhlenbeck lek swarm, active Brownian beetles —
  each checked against a formula `Critters.h` does not evaluate.

## What is next

**Done: the editor pivot's named "recreate any existing demo inside the
editor" thread.** All four sub-projects have landed: 1 (editor boot + scene
composition, menu bar, layout, terminal, text editor, scripting runtime,
file tree), 2 (entity/component scripting: `ScriptComponent`, the native
`Entity`/`Input` binding, `PlayMode`'s Play/Stop), 3 (Breakout's paddle and
ball recreated as a real project under
`TestEnv/assets/demos/BreakoutRecreation/`, plus the Play-mode
active-camera rendering it needed), and 4 (text-editor selection/clipboard,
undo/redo, syntax highlighting) — see "Last landed" above, plus the
camera-ray and light-placement follow-up flagged during sub-project 2.

Only Cube3D opts into the editor's `OnExportToScene`; the other 16 demos
each need their own before they can be opened as a starting scene that way
(Breakout's recreation was built as a fresh project instead, not exported
from the live demo) — whether that's worth doing for more demos hasn't
been decided.

Sub-project 3 exposed two scripting capabilities as genuinely missing;
both have since landed (see "Last landed" above): querying another
entity's Transform (`scene.findByTag`) and spawning/destroying entities
(`scene.spawn`/`scene.destroy`). Breakout's recreation itself hasn't been
updated to use them yet (still no bricks) -- that's now possible, but
nobody's asked for it.

**The GSS-to-C++ transpiler, including static-link execution, is fully
built.** All 6 tasks of `docs/superpowers/plans/2026-09-11-gss-to-cpp-
transpiler.md` landed first (see "Last landed" above for that half); the
"Goal" section's deliberately-deferred execution question was then
resolved and built in this session: **static linking**, chosen over
dynamic `.so` loading (this project's iteration loop only needs a rare,
deliberate "graduate to C++" moment rather than continuous reload, a C++
plugin system's ABI/unsafe-unload risks are a bad trade against how cheap
restarting this editor already is, and it matches the already-established
glob-and-relink build model). See "Last landed" above for what shipped —
`CompiledScript.h`/`CompiledScriptRegistry.h` plus `PlayMode`'s dispatch,
`paddle.gss` graduating as the first real proof of it end to end, and
`ball.gss` graduating after it — **both of BreakoutRecreation's scripts
now run compiled; nothing interpreted is left in that project.** Nothing
else is currently queued against this thread — the transpiler only
supports one language subset (see the spec), so a future script outside
that subset would need either rewriting to fit it or a new transpiler
feature, same as both graduations here needed.

Mesh authoring (a separate, earlier-planned sub-project, before this thread
existed) is **landed** — see "Last landed" above, including the disclosed
scope gap (Extrude/Split Edge/Delete Face have no UI path yet) and the
deferred follow-up items from its closing whole-branch review.

The Inspector's ID field (requested alongside the static-link execution
wiring, above) is **landed** — see "Last landed" above.

**A thread requested 2026-09-11: make the editor itself feel like a complete,
usable tool rather than "disjointed" — four sub-projects, each independent,
worked biggest-to-smallest as ranked when scoped. All four are now landed:
lighting, project loading/saving UX, the text editor, and the profiler --
see "Last landed" above for each. The one gap named repeatedly across all
four and still real: the actual mouse/keyboard click-through for any of
them has not been exercised, since no GUI-automation tool exists in this
environment -- what's verified is the underlying logic and, where
possible, a rendered capture of the real UI.**

Unstarted, in the order they were last discussed:

- **Multiplayer/networking foundation.** Nothing exists yet — no transport, no
  replication, no session model. Explicitly out of scope for the editor pivot
  above; scope it out as its own sub-project once the editor's scene model
  exists to replicate.
- **Character attributes.** `s_Strength` in `TerrainLab.h` is deliberately a
  constant with a hook where a stat should be — carry capacity trained by use.
  The owner deferred this ("further down the line"), so **ask before starting
  it**.

## When this file is wrong

Fix it in the same commit as the work that made it wrong. It is one page on
purpose: if updating it feels like a chore, it has grown too big, and the
subsystem table in `docs/HANDOVER.md` is where durable facts belong instead.
