# Handover

For picking this project up cold. Not a tutorial — `docs/ENGINE.md` is the
orientation doc and `README.md` is the reference. This is the working context:
what is in flight, how work is done here, and the specific traps that have cost
time more than once.

---

## What this is

**EGSS** — a game engine written from scratch in C++17, following the shape of
TheCherno's Hazel series but diverging where it made sense. `EGSS/` is the
engine, built as a shared library; `TestEnv/` is a sandbox app that links
against it and holds seven demos.

The owner is building this to **understand** it, not to ship a game. That
matters for how you work: a working black box is worth less here than a
mechanism they can follow. Explain the load-bearing idea; skip the ceremony.

Reading order for a newcomer:

1. `docs/ENGINE.md` — the frame's call path, the decisions that explain the
   rest, and the API you'd use day to day
2. `TestEnv/src/Breakout.h` — a commented worked example
3. `README.md` — layout, build, roadmap, and a changelog that records *why*
   things are the way they are, including the wrong turns

---

## Current state

**Check `git log` and `git status` first — this section goes stale quickly.**
At the time of writing `main` was at `3ddef51` "Added working demo recording
and replay", with **3D rigid bodies** in flight on the worktree branch.

Landed in `main` already: 2D rotation joined into the solver, in-engine frame
capture, and replay. Each has a changelog entry worth reading before touching
that area — the `Step` reordering and the frame-rate-dependence findings in
particular were pre-existing bugs, not new work.

In flight, on the branch, **built in three separable pieces the way 2D was**:

- **Piece one, done.** `RigidBody3D` / `PhysicsWorld3D`: quaternion
  orientation, a real inertia tensor rotated into world space each step, and
  midpoint angular integration. 31 checks.
- **Piece two, done.** `Sat3D`: fifteen candidate axes, clipped manifolds of up
  to eight points. 40 checks.
- **Piece three, done.** The join: a narrowphase calling `Sat3D`, contacts with
  lever arms, and the solver's angular terms. Boxes and spheres collide, stack,
  roll and settle. 21 checks.

**3D stacking is fixed.** It was `Sat3D`, not the solver: `MostFacingFace` wound
a face pointing down a negative axis backwards relative to its own outward
normal, so every clip plane pointed inward and the reference face clipped to
nothing whenever the *upper* box of a pair won the near-tie. The manifold
dropped from four points to one without saying so, and one point cannot hold a
box level. Four boxes now stand at every setting from 4 to 24 velocity
iterations with sleeping on or off, against 11/20 and 4/20 before. The
changelog entry has the sweep and the arithmetic.

3D also still has a brute-force broadphase, as 2D did until a profile asked
otherwise.

The owner commits their own work, often between sessions and sometimes while
a reply is being written. **Never commit to `main` and never push**, and do not
assume something is still outstanding because a previous message said so.

Finished work is handed over as commits on the **isolated worktree branch**,
reviewed with `git log -p main..worktree-<name>` and merged, cherry-picked or
dropped by the owner. This replaced handing over patch files, which went stale
as soon as one was applied.

### Subsystems, roughly in the order they were built

| Area | Where | Notes |
| --- | --- | --- |
| Core loop | `Application`, `Layer`, `LayerStack` | Fixed timestep + render interpolation. `OnFixedUpdate` is simulation, `OnUpdate` is presentation |
| Renderer 2D | `Renderer2D` | Three batches: quads (indexed), lines, triangles. One draw call each. Blend modes: Alpha, Additive, Multiply |
| Renderer 3D | `Renderer`, `Mesh`, `Material`, `ObjLoader` | `Submit` per mesh, no batching. `.obj` loading with smoothing groups. Materials with instance overrides; shaders by name via `ShaderLibrary` |
| Cameras | `Camera` base, `Orthographic`, `Perspective` | `BeginScene` takes any `Camera` |
| Framebuffers | `Framebuffer` | `RED_INTEGER` attachment drives pixel-exact picking |
| Scene | `Scene`, `Entity`, `ComponentStore` | ECS-lite: dense arrays, generational handles |
| Physics | `PhysicsWorld2D`, `RigidBody2D`, `Sat2D` | Warm-started sequential impulses, island sleeping, raycasts, uniform-grid broadphase. **Rotation is in**: oriented SAT manifolds of up to two points, per-point lever arms and angular impulses, oriented rays and bounds |
| Capture | `ScreenCapture`, `Replay` | In-engine PNG of the frame just drawn, and `.dem`-style input recording replayed per fixed step. Both reproducible; see "Capturing frames" |
| Rays 3D | `Raycast3D` | Slab test against mesh bounds in local space, plus graded occlusion |
| Physics 3D | `RigidBody3D`, `PhysicsWorld3D`, `Sat3D`, `Physics3D` and `Ragdoll` demos | Quaternion orientation, real inertia tensor, oriented-box manifolds over fifteen axes, warm-started impulses with two-tangent friction. Boxes, spheres and **capsules**. **Joints**: ball, hinge, angle limits, cone-and-twist, motors. Uniform-grid broadphase above 200 bodies |
| Audio | `AudioEngine` | Lock-free mixer, positional, occlusion, early reflections, three-band convolution reverb behind a 4th-order Butterworth splitter |
| Acoustics | `Acoustics2D` | Ray-traced room response feeding all of the above. Specular *and* diffuse reflection |
| Profiling | `Instrumentor` | `EGSS_PROFILE_SCOPE`, live panel, Chrome trace |

---

## Building and running

```sh
./egss.py build            # debug; always regenerates project files first
./egss.py build release
./egss.py build all        # every config
./egss.py run
./egss.py clean
./egss.py gen              # project files only
./egss.py build --no-gen   # skip regeneration

# Anything after a bare -- is forwarded to TestEnv
./egss.py run -- --demo Breakout --record run.rec
./egss.py run release -- --play run.rec --hide-ui
```

**`TestEnv/` at the repo root is source.** The binary is
`bin/<Config>-linux-x86_64/TestEnv/TestEnv` and has to run from its own
directory, because assets resolve relative to the executable and recordings,
screenshots and `imgui.ini` land beside it. `./egss.py run` does that; running
`./TestEnv` from the root just finds the source directory.

**It always regenerates on purpose.** premake expands file globs at
*generation* time, so a new `.cpp` is invisible to the build until it does.
That costs 0.21 s and removes the most confusing failure in the project — code
that compiles in the editor and never links.

Verify all three configs before calling something done; Release has caught
things Debug did not.

Assets load by path **relative to the executable**. premake copies
`TestEnv/assets` next to the binary after every link. The two platforms need
different arguments there — `cp -rf src dst` puts src *inside* dst once dst
exists, while `xcopy` copies the contents — so the naive form nested
`assets/assets/assets` one level deeper per build.

---

## How work is verified here

This is the most important section. The habit that has repeatedly paid off:

> **Compute the expected value by hand, then compare. And suspect the
> measurement before the code.**

Screenshots prove a thing renders. They do not prove it is *right*. Nearly
every real bug in this project was found by a number disagreeing with
arithmetic, and several were found only because the number was checked against
a formula the code knew nothing about.

Worked examples, all of which caught genuine bugs:

- Mean free path vs `π·Area/Perimeter`
- Traced RT60 vs the Sabine/Eyring formula, across four absorptions
- A rendered reverb tail measured back with Schroeder integration and compared
  to the decay it was asked for (**closed loop** — every stage independent)
- `.obj` vertex counts derived from the format's sharing rules
- Picking: every entity's centre projected to a pixel and read back

### The self-test pattern

Tests are **temporary**, live in `TestEnv/src/`, and are deleted once they have
done their job. The project has no test framework and does not want one; the
value is in the measuring, not in keeping the harness.

```cpp
// TEMPORARY -- delete after verifying X.
#pragma once
#include <Egss.h>

namespace XTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        EGSS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }
    inline void Run() { /* ... */ }
}
```

Call it from the `TestEnv` constructor in `TestApp.cpp`, run the app for a few
seconds, read the log, then remove both the header and the call. Grep for
`TEMPORARY` before declaring finished.

Two specifics:

- **`AudioEngine::RenderForTest`** runs the mixer with no device, so audio is
  checkable as arithmetic on a machine with no sound card. It must not run
  while the device is live — `Shutdown()`, test, `Init()`.
- **GL-dependent checks** need a live context, so they run inside a demo's
  update (e.g. on frame 60) rather than at construction.

### When a measurement disagrees

Check the measurement first. Real examples from this project:

- A per-call `EGSS_PROFILE_SCOPE` inside `Raycast` cost more than the raycast,
  hiding a genuine 1.9× broadphase win. **Never profile hot leaf functions**,
  and always measure in Release.
- A reverb tail read as zero because the measurement window (1024 frames) was
  shorter than the shortest comb delay (1214).
- Convolution taps came out at 0.428 of the expected gain at *every* tap — the
  shape of a wet-level ramp, not of a wrong gain. The test had not let the
  crossfade settle.
- An object appeared to move on its own; it was a leftover held mouse button
  from a previous automated run. (It did reveal a real gap — the gizmo grabbed
  a button that was already down — so the fix was still worth making.)

### Capturing frames

**From inside the engine.** `Application::CaptureFrame(path)` requests a PNG of
the frame being drawn; the read happens between the last draw and the swap,
which is the only moment a finished frame exists. F2 does it interactively.
Unattended:

```sh
./TestEnv --demo Physics --lockstep --hide-ui --capture shots/a.png --capture-step 240
```

`--demo` takes an index or a short name, so exercising a non-default path no
longer means editing `g_ActiveDemo` and rebuilding.

That command is **bit-reproducible**: two runs give byte-identical PNGs, so a
captured frame can be a regression test. All three flags were needed, and each
came from a measured failure rather than a guess:

- **`--capture-step`, not `--capture-frame`.** Frames are skipped while the
  window is being mapped, so frame N is not the same simulation state twice.
- **`--lockstep`** runs exactly one fixed step per frame rather than feeding
  the accumulator real time. Otherwise how much has been simulated by a given
  frame depends on machine speed — two runs measurably differed.
- **`--hide-ui`** drops the panels, which print milliseconds and so differ
  every run however deterministic the simulation is.

**If a test needs input, record it.** `--record <file>` writes a replay while
you play; `--play <file>` reproduces it exactly, and the file names its own
scene. Input is sampled per fixed step, so the frame rate it was recorded at
does not matter.

**Driving the app from outside still does not work**, and is not worth
retrying: `xdotool key --window <id>` does not reach GLFW, `import` hangs
against XWayland, window geometry moves between runs, and clicking ImGui by
coordinate lands on the wrong widget.

Prefer numbers. Use a capture to confirm something *looks* right after the
numbers say it *is* right — and note what numbers cannot see: they verified
rotation to five decimals while discs were being drawn as squares, which one
look caught immediately.

---

## Traps that have bitten more than once

- **A changelog entry can describe work that was never committed.** The glTF
  entry stated `Texture2D::CreateFromMemory` "had to exist" and that the
  `GL_UNPACK_ALIGNMENT` fix was in; neither was in the diff, only in the prose.
  It surfaced only once `ModelDemo.h` — itself fully written but never added to
  `DemoRegistry.h`, so it built and was invisible — got wired in and failed to
  compile. Trust the build over the changelog when picking work back up.
- **A test suite that passes on the first run has not been tested.** The glTF
  loader's 62 checks all passed immediately, which is exactly when to distrust
  them. Six deliberate bugs were injected — ignore `byteStride`, quaternion in
  wxyz order, matrix read row-major, normalise by 65536, never flip strip
  winding, skip sparse accessors — and every one was caught by the check aimed
  at it. Do this whenever a suite comes up green first time; it costs one build
  and it is the difference between evidence and decoration.
- **glTF: the stride is on the bufferView, not implied by the element.** Element
  *i* is at `buffer + view.byteOffset + accessor.byteOffset + i * stride`, and
  `stride` is `byteStride` when present. Using the element size instead reads
  position, then the *normal* as the next position, and an interleaved model
  collapses into a smear. Real exporters write interleaved buffers, so a loader
  that only ever saw de-interleaved test files will look correct until the first
  file from Blender.
- **glTF component orders that are wrong-but-valid.** Quaternions are stored
  **xyzw**; glm's constructor takes **wxyz**. Matrices are **column-major**,
  which is glm's order too — read row-major you get the transpose. Both mistakes
  produce a transform that is still a transform, so nothing fails and the model
  merely appears somewhere unexpected. Pin them with a fact from outside the
  code: a 90° rotation about +y takes +x to −z.
- **The inverse transpose can be invisible and still be right.** For a world
  transform `R·S` with diagonal `S` and axis-aligned normals, `(R·S)⁻ᵀ = R·S⁻¹`
  and both give the same direction once normalised — measured byte-identical on
  the Model demo. It is still the correct matrix for a shear or for normals that
  are not aligned to the scale axes; just do not claim a box demonstrates it.
- **The LOD seam is a step, not a crack.** Chunks on different marching-cubes
  strides do not disagree by a hairline: the coarse lattice cuts corners, so on
  a convex surface it meshes *systematically lower* and leaves a solid terrace
  whose wall reads as "a hole into the ground". Skirts (walling every boundary
  edge downward) were built for this and moved **2 pixels**, even with the bands
  forced to 10 m/20 m -- they work, there is just no gap. Default off; the fix
  is transvoxel transition cells. Turning LOD off is not the alternative, it was
  worth 9x the triangles at full stream.
- **`flat` is a GLSL keyword.** `float flat = ...` is a syntax error (it is an
  interpolation qualifier), the shader fails to compile, and `Shader::Create`
  logs it and hands back an unusable program — which renders **white**, not
  nothing. Same for `sample`, `filter`, `input`, `output`, `active`. When a
  shader edit produces an inexplicable frame, read the log first: it said
  `unexpected FLAT` on the line in question while the picture said only "white".
- **A sphere collider rolls forever.** Scenery rocks spawned as spheres landed
  exactly where predicted and then crept downhill — 3.69 to 3.10 over 300 steps —
  rolled into the sea, down the seabed, and eventually so far inside the SDF that
  the narrowphase stopped ejecting them and they fell out of the world. The
  collision was correct throughout. A rigid-body solver has no rolling
  resistance, so anything round on anything sloped is in permanent motion; use a
  box for things meant to sit still, and draw a mesh *inscribed* in it rather
  than drawing the collider.
- **Render state is global and outlives the demo that set it.** `CullFace`,
  depth write, blend mode and polygon mode all persist across a demo switch,
  because they live in the GL context and not in the layer. `CelShading` set
  front-face culling for its outline pass and restored it to **`Back`** — but
  the engine default is `None` (`OpenGLRendererAPI::Init` never enables
  `GL_CULL_FACE`), so every demo selected afterwards silently ran with back-face
  culling on. OpenWorld's water is a single-sided quad facing +Y, so it vanished
  the moment the camera went under it, and only for someone who had visited the
  Cel demo first. Restore to the *default*, not to what you assume was there —
  and in a demo that cares, set the state at the top of the draw rather than
  inheriting it. `VoxelTerrain` still leaves culling on `Back`; it gets away
  with it.
- **Crossing a boundary needs to look like crossing it.** Being underwater
  rendered identically to standing on dry sand — same sky, same lighting — so
  "the actual water is invisible" was an accurate description of correct-looking
  geometry. A submerged camera now clears to the water colour and fogs by
  `1 - exp(-density*d)`. When a demo has two regimes, check that the picture
  changes at the boundary, not just that each side is drawn right.
- **A signed field scales both sides.** OpenWorld's island mask is positive
  inland and negative at sea, so `mask * s_MaskToHeight` shapes the island *and*
  the sea floor with one number. Flattening the islands from 0.55 to 0.10
  therefore flattened the seabed by the same factor, turning the sea into a
  shin-deep sand shelf 80 m wide that read as more beach — reported as "I still
  can't see the water" after a changelog entry claimed it fixed. Land and water
  now have separate scales. Before scaling anything derived from a signed
  quantity, ask what the negative half of it means.
- **A capture that stops at step 300 cannot see a trend.** The same water bug was
  visible in the numbers that "confirmed" the fix — 12.7% of frame at step 30,
  12.4% at 90, 11.7% at 200, 9.2% at 800 — monotonically shrinking, and 4.7% by
  step 6000, which is where a person actually plays. When measuring something a
  player experiences over minutes, capture at the timescale they experience, and
  read a series as a series rather than as one number per run.
- **A comment doing a constant's job.** OpenWorld's `Height` scaled the island
  mask by a literal `0.55`, and `Slope` scaled its gradient by another literal
  `0.55` under a comment reading "0.55 matches the scale Height applies to the
  mask". That comment is an admission: the two must agree and nothing enforces
  it, so flattening the terrain would have left the normals lighting the old
  shape with no error anywhere. It is `s_MaskToHeight` now. When a comment says
  two numbers have to match, that is the bug report.
- **A light albedo has no cel bands.** The OpenWorld shader's brightest
  multiplier is `ambient + sun + sky*0.35` = 1.525, so an albedo over ~0.65
  clips — and clipped means *every* band saturates to the same white, so the
  banding silently disappears rather than looking too bright. Sand at
  (0.84, 0.76, 0.56) measured (255, 255, 213). Pick the albedo from the
  lighting's maximum, then check the brightest pixel against the product.
- **"It never moved" is a test-setup failure, not a physics failure.** The
  swimming check reported the body's feet at exactly `start - eyeHeight` from
  both starting heights — the same distance, from two different places, is a
  body that was never simulated. It ran from `OnDemoAttach` *before*
  `SpawnWalker`, so the handle did not refer to a body yet. An exactly-preserved
  input is the signature; look at ordering before looking at the model.
- **A cache keyed by a version number is a trap; key it by behaviour.** The
  OpenWorld chunk cache stores generated voxel chunks between runs, and its worst
  failure is not a miss but a *stale hit*: change the terrain function, forget to
  clear the file, and the old world comes back while the code says otherwise. The
  key is therefore a hash of **512 fixed samples of the density function** plus
  the lattice geometry, not a constant somebody has to remember to bump. Any
  change to the islands, noise, sea level or voxel size moves a sample and
  invalidates the file automatically. `openworld.chunks` lives beside the
  executable and `bin/` is gitignored, so it never reaches a commit.
- **Round-trip tests miss whatever the *setup* cannot reach.** Three of five
  mutations against the cache survived the first pass, all for that reason:
  loading uniform-over-dense was invisible because the target field was fresh
  (nothing allocated to free); the length check was invisible because the
  truncated blob was a six-byte uniform chunk caught by an earlier guard; the
  write path's offset was invisible because every read went through a *reopened*
  cache that rebuilds offsets by scanning. None needed a better assertion — each
  needed a different starting state. When a mutation survives, look at what the
  test built before it looked at anything.
- **Streaming order is invisible until the radius grows.** `OpenWorld`'s
  `StreamAround` filled chunks in scan-line order (`dz` then `dx`) for its whole
  life without anyone noticing, because at a 64 m radius the disc is small. At
  128 m — four times the area, 10,455 chunks, three minutes to populate at one a
  step — it assembles in visible stripes and the far edge arrives before the
  ground beside the player. It is now nearest-first, via offsets sorted once per
  reach, with a cursor so the scan does not re-walk the filled interior every
  step. Two lessons: a quantity change makes previously-invisible ordering bugs
  visible, and when you make something bigger, check the *order* things happen
  in and not only the cost.
- **A green suite can be measuring a feature that does nothing.** Chunk LOD
  passed twelve checks — hysteresis correct, every chunk on the right band, the
  mechanism sound — while saving **1.02x**, because the bands sat at 48/96 m and
  the load radius is 64 m, so the chunks they would have coarsened were never
  loaded. Correctness checks cannot tell you a feature is pointless; only the
  magnitude can. Whenever something is built to *save* something, measure the
  saving as well as the behaviour, and say the number out loud.
- **A test that cannot see the line it is testing.** The cel outline's clip-space
  push multiplies by `clip.w` to survive the perspective divide. Deleting that
  multiply left all 20 checks passing, because the test rendered
  **orthographically** and `w` is 1 there — the mutation and the correct code are
  the same expression under that projection. Same shape of hole in the same
  suite: dropping the top-band clamp changed nothing, because no pixel of a
  *curved* surface lands on `N·L = 1.0` exactly, so the clamp never fired. Both
  needed a second geometry (perspective at two distances; a flat plane square-on
  to the light), not a better assertion. When a mutation survives, ask what the
  test set up rather than what it checked.
- **A constant offset is the measurement, not the code.** The outline measured
  6 px too wide at every setting, slope exactly 1. With ambient 0 the darkest
  band is pure black — the same colour as the outline — so the scanline counted
  both as one run, and band 0's ring is `R(1-sqrt(1-1/B^2))` = 5.7 px. The rule
  in CLAUDE.md caught this one exactly as written; when the error is the same at
  every reading, stop looking at the shader.
- **Adding a field to a struct silently rewrites every positional initialiser.**
  `Submesh` gained `MaterialIndex` as its *second* member for glTF. Three sites
  built one as `{ material, firstIndex, indexCount }`, and those three arguments
  simply slid over: the first index became the material index, the count became
  `FirstIndex`, and `IndexCount` fell back to its default of `0`. Every `.obj`
  mesh then drew zero indices — the Cube3D floor, sphere and icosahedron
  vanished while the wireframes and debug lines, which do not go through
  `Submesh`, stayed. **It compiles without a warning**, because every argument
  is still convertible to the field it landed on. Two hours went into blaming a
  stale shared-library build, since a struct layout mismatch across a `.so`
  boundary has the identical signature; `./egss.py clean` disproved that in one
  step and should have been the *first* step, not the fourth. When adding a
  field to an aggregate, `grep` for every brace-initialiser of it and prefer
  named assignment, which is what these sites use now and what `GltfLoader.cpp`
  always did.
- **Which way does yaw go?** `PerspectiveCamera::GetForward` is
  `(cos yaw·cos pitch, sin pitch, sin yaw·cos pitch)` and `GetRight` is
  `cross(forward, up)`, so **increasing yaw turns right**. The voxel demo's
  arrow keys had it backwards from the day they were written — left arrow
  turned the camera right — and nobody noticed, because the fix is to press the
  other key. A sign is 50/50, so it will not be caught by reading it back; turn
  the camera and check the new forward against `GetRight()`, which is a vector
  the look code does not touch. Note also that yaw 0 looks along **+x**, not
  −z, which is why the demos start at −90.
- **Depth, in 2D.** `glm::ortho(-1, 1)` makes **higher z nearer**, and at equal
  z the depth test rejects the later fragment — so the **first** thing drawn
  wins, the opposite of painter's order. This caused three separate bugs,
  including "the lights don't blend" (which was not blending at all).
- **Separate the peak from the settled value before blaming the solver.** The
  ragdoll's joints appeared to stretch 38 mm and its limits to leak 0.56 rad;
  settled, both were essentially zero, and the peaks were one or two frames of
  a 71 kg figure hitting the floor inside one step. Throwing iterations at it
  barely moved the peak and made it worse at 64, which is the tell that the
  measurement rather than the convergence was wrong.
- **A test rig whose bodies all start aligned cannot see a rest-pose bug.**
  Cone-and-twist limits measured swing from the bodies being *aligned* rather
  than from the pose the limits were set in, so a bone built at an angle to its
  parent started outside its own cone. Every cone-twist test passed, because
  every rig in them had the bone and the torso starting aligned and the
  relative rotation was the identity. Every limb on a real skeleton sits at an
  angle to its parent. **Build at least one test rig crooked.**
- **Check the initial state is physical before believing the final one.**
  Setting a body's angular velocity without the matching linear velocity
  describes a body rotating about a pivot whose centre is not moving, which
  does not exist — the solver fixes it on the first step by trading angular for
  linear, and it reads as a joint eating the rotation. Three hinge tests failed
  this way in a row, all with the solver correct underneath. `SpinAbout` in the
  hinge test set both; there is no such helper in the engine, so write one
  again if you need it.
- **An under-solved stiff constraint gains energy rather than merely sagging.**
  A 6-link chain with 20 kg on the end peaked at 7217 J of kinetic energy at
  two velocity iterations, against roughly 500 J of gravitational potential
  available. Too few iterations is an instability, not an inaccuracy.
- **A manifold must describe what is touching, not what the shape spans.** A
  contact constraint resists *approach* along its normal, so a contact point
  emitted where nothing is actually in contact pushes the bodies apart there.
  Clipping a capsule's segment to a box face and emitting both ends
  unconditionally left a tilted capsule resting at 10 degrees for ever, held up
  by a point in mid-air. Drop points with no penetration.
- **A resting test dropped level proves nothing.** Gravity acts through the
  centre, nothing applies a torque, and a single contact point holds a level
  body level — so the check passes with the manifold disabled. Drop it tilted
  and slowly spinning, or it is measuring symmetry.
- **Contact order is part of the answer.** Sequential impulses resolve contacts
  in the order they were generated, so a broadphase that finds pairs in a
  different order produces a different — equally valid — simulation. Both grids
  now sort their candidates into ascending index, which is brute force's order,
  and that is what makes `UseBroadphase` a pure optimisation rather than a
  second physics engine. It is also what makes the body-count thresholds safe:
  without the sort, crossing one would change the run.
- **Cube3D's capture depends on the mouse cursor position.** `UpdateGizmo`
  reads `Input::GetMousePosition()` to pick the highlighted axis, so a shot of
  it differs between sessions by exactly the pixels of one gizmo line — 87 of
  them, all by the same colour delta. It is the ImGui-slider problem wearing a
  different hat: state reaching the demo without going through recorded input
  or the fixed step. **Do not use Cube3D as a byte-exact regression reference
  across sessions.** The other six demos are fine.
- **`--demo` takes the short name from `DemoRegistry.h`, not the class name.**
  `Lighting`, `Physics`, `Scene` — not `Lighting2D`, `Physics2D`, `SceneDemo`.
  An unmatched name logs a warning and **falls back to the default demo**
  (`g_ActiveDemo`, currently `Scene`), so a batch of captures comes back
  looking plausible while being the same demo several times over. The symptom
  is two demos with identical pixel hashes.
- **ImGui config flags are read once, in `OnAttach`**, and `PushOverlay` runs
  `OnAttach` immediately. Anything that has to reach the ImGui context — the
  viewports flag is the live example — must be set *before* the layer is
  pushed, which is why `Application` parses the command line first. Set it
  after and it is applied to a context already built without it, silently.
- **`OnAttach` runs for every pushed layer**, whichever demo is showing. Start
  continuous things (looping sounds) in `OnDemoActivated`, not `OnAttach`.
  Looping audio has escaped this way twice.
- **Member initialisation order follows declaration order.** A camera declared
  before the floats it is constructed from reads uninitialised memory; the
  symptom is an empty viewport and nothing else wrong.
- **Every layer sees the same event.** Return `true` from a dispatcher lambda
  to consume it, or two layers will fight over one key.
- **ImGui takes keys before you do.** `Tab` cycles widget focus and turns
  sliders into text fields. Use function keys for shortcuts. Label collisions
  form the widget ID — two controls named "Light" are one control.
- **Framebuffers with two draw buffers**: a fragment output a shader never
  assigns is *undefined*, not left alone. Every shader in the pass must write
  every attachment. This scribbled noise into the picking buffer for a while.
- **`glClear` will not clear an integer attachment** — use `ClearAttachment`.
- **Picking stores the slot index, not the `EntityId`.** A handle whose
  generation passes 2047 exceeds `INT_MAX` and reads back negative from a
  signed integer texture.
- **`AudioEngine::StopAll` only *requests* a stop.** The mixer notices the flag
  on its next block, deactivates every voice and returns silence for that
  block. Call it and then immediately start the voice you meant to measure and
  the first render kills it — every reading comes back as exactly zero. Flush
  a throwaway block through before setting up.
- **A constant dB offset across every measurement is the measurement's fault.**
  A centred mono voice puts √½ in each channel and a centred tap does it again,
  so anything read back through the reverb carries an extra factor of ½ that
  has nothing to do with what is being measured.
- **float32 bottoms out around −100 dB in the audio path.** The mix, the filter
  states and the convolution accumulator all carry about seven decimal digits.
  A filter designed to reject by 129 dB measures about 102, and *slopes* taken
  between two points down there are pure noise — which reads exactly like a
  filter that does not roll off.
- **The audio thread must never allocate, lock, or block.** Buffers are sized
  when the voice or state is constructed. Parameter updates publish whole
  structs by rotating through a ring and releasing an index.
- **A cube's tilt cannot be measured from its local up axis.** One that has
  yawed reads as perfectly level and one resting on another face reads as 90
  degrees, so the metric says nothing about whether a stack is standing. Use
  the height. This wasted a sweep before the numbers started making sense.
- **Do not rotate bodies in 3D position correction.** 2D does and is right to:
  there the correction is a scalar and a face's two points are symmetric. In 3D
  a small box's inverse inertia is around 24, so the implied rotation dwarfs
  the linear nudge beside it, and applied per point per iteration with stale
  levers it compounds — a four-box stack tilted 99.6 degrees and fell over,
  against 0.24 degrees with it removed. Levelling a tipped crate is the
  velocity solver's job and it does it.
- **A clipped face has to be wound about its own outward normal.** Emitting the
  corners in a fixed `(u, v)` order while flipping the normal with a sign leaves
  faces along negative axes wound backwards, so clip planes built as
  `cross(edge, normal)` all point inward and Sutherland-Hodgman keeps the
  outside of the face. It clips to nothing and the corner fallback quietly
  returns *one* point instead of four. Nothing reports an error: the depth and
  the normal stay correct, and only the point count is wrong. This is what made
  3D stacks collapse chaotically, because which of the two boxes owns the
  reference face is decided by a near-tie in the last bits.
- **Sleeping can hide a broken stack.** Bodies that freeze look like bodies that
  settled. Measure stacks with `AllowSleeping = false` — the collapse rate went
  from 9/20 to 16/20 with sleeping turned off, and the difference was entirely
  bodies falling asleep partway through falling over.
- **A one-sided oracle asserted both ways will contradict a correct answer.**
  The random-direction search behind `Sat3D` proves boxes are *apart* when it
  finds a separating direction and proves nothing when it does not — a narrow
  gap hides in the sampling. Asserting the converse made it "disagree" with a
  right answer, and the comment above the function had already said so.
- **Parallel axes give a zero-length cross product**, which is not a candidate
  axis. Testing it reports zero-width shadows and a false gap; two axis-aligned
  boxes hit this on all nine at once, so the bug breaks the easiest case there
  is rather than an exotic one.
- **In 3D, angular momentum is conserved and angular velocity is not.** Carry
  `w` across a rotation unchanged and a tumbling body conserves the wrong
  thing; capture `L` before the turn and recover `w` from it after. And take
  `w` from the step's *midpoint*, not its start — the difference measured 18.4%
  of invented rotational energy against 0.02%. Substepping is not the fix and
  makes it worse, since each substep renormalises a quaternion.
- **The tennis racket theorem is the test.** A body spun about its intermediate
  principal axis must flip over periodically; about the largest or smallest it
  must not. Nothing in the code knows this, so it catches an angular integrator
  that is merely plausible. Measure it in the **body** frame — in world space
  `w` stays near the fixed `L` and the flip is invisible.
- **Anything that moves belongs in `OnFixedUpdate`, not `OnUpdate`.** Movement
  scaled by frame time makes speed depend on the frame rate, and the symptom is
  worse than it sounds: three demos could not reproduce themselves run to run,
  so nothing about them was testable or recordable. `OnUpdate` is for drawing
  and for reading the interpolation alpha, not for changing state.
- **`Step` must integrate positions *after* the contact solve**, not before.
  Doing both up front gives every body one free-fall sub-step of `g·dt²` that
  the solver never sees. Position correction hides the normal part of it, so on
  flat ground it is invisible; the part along the surface is never undone and a
  block rests on a 20° slope creeping downhill at `g·sinθ·dt`, with friction to
  spare and a perfectly healthy contact. **Position moving while velocity reads
  zero means integration, not friction.** This lived in the engine unnoticed
  until rotation made slopes possible.
- **`DrawRotatedQuad` takes degrees**; everything in the physics is radians.
  Passing radians turns a box about a seventh as far as it should go, which
  reads as a solver bug rather than a units mistake.
- **Anything deriving bounds from `HalfExtents` has to account for rotation.**
  A 45° square reaches 0.707, not 0.5. Bounds that miss it drop broadphase
  pairs with no contact and no warning to show for it — the quietest failure in
  the physics, and worth checking first when something passes through a corner.
- **Cutting too much when editing a demo header** compiles clean, because
  `DemoLayer`'s virtuals have empty defaults. The symptom is a black screen and
  a suspiciously fast `OnUpdate`. Committed work makes this a `git checkout`.
- **A ground probe's origin has to travel with the thing probing.**
  `m_RootAnchor.y` is written once when control begins and only ever moved
  horizontally after, and `DriveRoot` fired the probe from it. Walking uphill
  left the origin behind, the surface rose past it, and the probe correctly
  reported no ground *below* a point that was underground — so the fall test
  ragdolled a character standing on solid ground. It capped him at one standing
  height above his spawn, on any slope, and hid completely on flat floors and on
  a map whose spawn picks the flattest spot going. The probe now fires from the
  pelvis, which cannot be inside the ground while he is standing on it.
- **Friction does not constrain the character**, only what you drop next to
  him. His root is kinematic, so a slope never asks a contact for the force that
  would break: measured, he walks up 40° at full speed with zero slip where a
  box slides at 35. Do not reach for `atan(mu)` when reasoning about the gait —
  the limit is `speed · tan θ ≤ m_ClimbRate`, which is 42.7° walking and 16.7°
  running, both confirmed to within a degree.
- **A test course has to outlast the thing being tested.** Running at 4 m/s
  covers 66 m in the measurement window, and on a 40 m ramp five angles
  "failed" at *identical* step 641 — he had run off the end. Same trap on the
  64 m map, where every running bearing "failed" at exactly |x| or |z| = 32.
  Identical failure steps across different conditions means geometry, not
  physics.
- **`Terrain::NormalAt` is a central difference, and `HeightAt` answers 0 off
  the map**, so the outermost ring used to read as a cliff down to sea level —
  it called a 9 m map 81.3° steep. It delegates to
  `Heightfield3D::SmoothNormalAt` now, which clamps its samples inside the
  field. Any per-sample census over a heightfield should still skip the border
  ring on principle.
- **Anything drawn that follows the mouse breaks capture reproducibility.** Map
  Building's preview block did, and two identical capture runs produced
  different PNGs — which costs a demo the one property that makes a captured
  frame a regression test. `Application::IsUIHidden()` is the check; a cursor is
  UI and goes when the panels go. The simulation was never affected, only the
  picture, which is exactly why it was easy to miss.
- **`glPointSize` does nothing while `GL_PROGRAM_POINT_SIZE` is enabled.** The
  size comes from the vertex shader's `gl_PointSize` instead, and a shader that
  never writes one leaves it undefined — which showed up as points not being
  drawn at all rather than as points one pixel wide. `SetPointSize` disables it.
- **A walking capsule is dynamic with its rotation thrown away.** Being dynamic
  is what makes it fall, climb and get shoved; being a dynamic capsule is what
  makes it topple the instant it touches anything off-centre. The Voxel demo
  resets the orientation and zeroes the angular velocity every step. Set the
  horizontal velocity outright rather than applying a force, or the player
  accelerates for a second after each key press and slides for a second after
  letting go.
- **A rigid body's position has to be its centre of mass.** Put it anywhere else
  and gravity applies a torque that should not exist. `VoxelIsland` carries
  `Centre` (mass) *and* `BoxCentre` (the middle of the extents) for exactly this:
  a single oriented box body goes at `BoxCentre`, a `Compound` goes at `Centre`
  with its children offset from there. For a symmetric piece they coincide, which
  is why getting it wrong would pass every simple test.
- **A compound's inertia tensor is not diagonal**, and it is easy to make it so
  by accident. Each child contributes `I + m(d·d E − d⊗d)` about the body origin,
  and the outer-product term is the off-diagonal part — drop it and a body
  tumbles about the wrong axes, which reads as a solver bug. Checked against a
  hand-worked `Ixy = −3` for two boxes on a diagonal.
- **A moment belongs only to a section that is the sole path to ground.** A
  section that is one of several parallel routes carries a *share* of the load,
  and giving that share the whole lever over-reads it by exactly the number of
  routes — one face gets `6M/s³` where the n-face section it belongs to gets
  `6M/(n s³)`. That is why lone connections used to outrank every real section,
  get broken first and sever nothing. `VoxelStress` blocks a section and walks
  for an anchor to decide; **it only does so when the answer could change the
  verdict**, since the test can only lower a stress, and without that guard every
  section in the field pays for a flood fill (the suite ran past ten minutes).
- **A Jacobi sweep can return a reflection, not a rotation.** `VoxelIsland`'s
  oriented box checks the determinant and flips an axis; a body built from a
  reflection is inside out. And the box is centred on the piece's *extents*, not
  its centre of mass — those differ for anything asymmetric, and centring on the
  mass leaves the box hanging off one side.
- **Region-bounding the stress analysis does not work**, which is worth knowing
  because it is the obvious optimisation and it was on the roadmap. A section's
  stress depends on the load routed through it, and that load comes from
  everything above — outside any box drawn round the edit as well as inside.
  Truncating the region truncates the load. What does work: cache solidity into
  a flat array (the pass asked `field.Solid` seven times per voxel, and each ask
  is a clamp, three divisions, three remainders and a pointer chase), and break
  several independent sections per analysis rather than one.
- **`VoxelField3D::Far` is a sentinel, not a distance.** An unallocated chunk
  reads 1000, meaning "nothing near" — so anything that *steps by the value*
  leaves the map in one bound. Sphere tracing did exactly that and reported a
  miss over a hillside it was pointed at; `GroundBelow`'s downward march had the
  same hole. Both cap their step at one chunk now, which is always safe because a
  chunk is uniform across its extent. **No analytic test could have found this**
  — a field small enough to write a test around has surface in every chunk, and
  the sparse path only exists at map scale.
- **A structural strength has to clear the terrain's own weight.** A column of
  density rho fails under itself past `sigma / (rho g)`; at 180 kPa that is
  13.1 m, and the voxel map's tallest ground is 15 m, so it collapsed before
  being touched. Check `sigma / (rho g)` against the map's height before blaming
  the model.
- **Fade a subtracted field towards +big, never towards zero.** Subtraction is
  `max(A, -B)`. Fading a cave out near the surface by scaling its *value* down
  gives `max(ground, 0)`, which clamps every interior sample to exactly zero and
  drags the surface to wherever the fade starts. It meshes as a terraced
  hillside and reads as a marching-cubes artefact. It is not one.
- **Shapes meant to join have to overlap, at both ends.** A column from y = 3 to
  9, ground ending at 3, cap starting at 9: at exactly y = 3 and y = 9 both
  fields are zero, zero is not solid, and the column touches neither. The flood
  fill correctly reported a floating cap on an intact pillar, twice, for two
  different planes.
- **Voxel counting measures the shape shrunk by half a voxel on every face.** A
  4 x 2 x 4 m box at 0.5 m holds 7 x 3 x 7 = 147 lattice points, not 8 x 4 x 8 —
  so `VoxelIsland::Volume` under-reports a small piece's mass by roughly its
  surface area times half a voxel, and washes out for large ones.
- **`yaw = 0` points along +x** in `PerspectiveCamera`, not -z. A demo placed on
  the +z side with yaw 0 looks past its own scene, which reads as a generation
  bug.
- **`y - h(x, z)` is not a distance field.** Its gradient has length
  `sqrt(1 + |grad h|^2)`, so it changes *faster* than a distance — which is the
  dangerous direction: sphere tracing steps by the stored value and would
  overshoot the surface, and `VoxelField3D`'s sparse chunks would drop chunks
  that contained one. Divide by that length for the first-order true distance.
  Any generator that builds an SDF from a height function needs this.
- **The same error at two different conditions is arithmetic, not physics.** The
  rolling-sphere check came out 100.21% high at 15 degrees *and* 100.21% high at
  25, implying an inertia of −0.3007 both times. Exactly twice the right answer:
  the sphere was pre-settled for thirty steps, so it was already moving when the
  clock started, and `s = at²/2` only holds from rest. Measure a velocity
  difference and the starting state stops mattering.
- **Check that a tangent is tangent.** The same test first reported a *negative*
  acceleration downhill, because the "downhill" vector had a dot product of
  −2 sin θ cos θ with the surface normal and so projected the vertical drop into
  the distance travelled.
- **A ball on any interesting surface rolls away.** Comparing two colliders by
  dropping a sphere on each and comparing where it stopped compares how far it
  rolled. One had left the 16 m map and was 83 m below the world. Ask the
  colliders directly, or use a box.
- **An identical failure count across two resolutions is the test, not the code
  -- until you measure which.** The marching-cubes manifold check failed with
  *exactly* 168 bad edges at both 0.5 m and 0.25 m voxels, on meshes of 1,304 and
  5,240 triangles. It was 30 lattice points sitting exactly on the surface
  (radius 3 lands on both lattices, so both contain the same integer solutions of
  x²+y²+z²=9), each collapsing triangles to zero area. The mesher drops those
  now. Note the count was a real defect *reported by a broken metric* -- the
  right move was to measure what the 168 were, not to change either one blind.
- **A voxel field must be Lipschitz** -- one voxel of travel may change the
  stored distance by at most one voxel. `VoxelField3D`'s sparse chunks decide a
  chunk is entirely rock or air by checking it is more than a voxel from any
  surface, which is only sound under that assumption. Fill it with an arbitrary
  density function instead of a distance and chunks will be dropped that
  contained surface.
- **Mesh chunks with a one-cell overlap.** A chunk's last cell reads the first
  lattice plane of its neighbour; without it every chunk boundary is a crack. The
  manifold check catches it, a screenshot does not.
- **A replay only reproduces the sliders a demo registered.** `RegisterParam` in
  the demo's constructor, one line per knob that reaches the simulation; anything
  unregistered keeps this build's default and the replay will differ from the
  recording for a reason no picture explains. Physics2D and Physics3D are
  adopted; the rest are not yet. If a replay diverges, ask what moved before
  suspecting the simulation.
- **Registration order is load-bearing at both ends.** Recording writes the
  parameter table at `StartRecording`, so it must run *after* the demos are
  built — it used not to, and wrote a table with nothing in it. Playback starts
  in the `Application` constructor, before any demo exists, so it resolves names
  to indices on the first step instead; resolving them at load reports every
  parameter missing.
- **`--capture` and `--play` open no window**, because an unattended run that
  maps one steals the keyboard from whatever the machine is being used for.
  `--show-window` to watch one, `--hide-window` for anything else. If a demo
  "does not start", check the log for `No window: unattended run` before
  suspecting a crash. Rendering is unaffected — the back buffer is the driver's,
  not the compositor's, and hidden and visible captures are byte-identical.
- **Anything drawn from the cursor breaks capture reproducibility, and Cube3D
  had it too.** `ReadHoveredEntity` picks from the mouse and `DrawSelectionBox`
  drew a wireframe round the result with no `IsUIHidden` guard, so a captured
  frame depended on where the mouse was left — found because the same demo
  hashed differently in two sessions with nothing in the renderer changed. Map
  Building's preview block was the same bug. **A hover is a cursor and a cursor
  is UI.** If a capture hash moves and no drawing code changed, check for
  anything that reads `Input::GetMousePosition` before suspecting the renderer.
- **A room assembled from slabs leaks at every joint that does not overlap**, and
  none of the ways to get it wrong look like geometry mistakes from the outside.
  Building Cube3D's enclosure failed three times: walls sized to the floor's
  *scale* rather than half of it stood six metres clear of its edge (241 of 256
  rays escaped, which reads as a short tail); walls sized correctly to the floor
  put the camera, and so the listener, outside the room (0 paths from 3,042
  bounces, which reads as a dead room); walls butted up to the floor instead of
  overlapping left a 0.2 m slot round the base (27 of 256). `RaysEscaped` is the
  number that tells you which, and it is on the panel.
- **A broadphase cost that does not scale with the body count is not being paid
  for the bodies.** The 3D grid cost 1.847 / 1.859 / 1.951 ms at 117 / 217 / 417
  bodies with terrain in the scene — eleven times brute force, and *flat*. The
  flatness is what named the cause: 29,575 cells, paid for whatever is in them.
  Fixed in two parts, and the ratio alone would have hidden the second — taking
  the terrain body out of the cells got 217 bodies to 0.256 ms, which still lost
  to brute force's 0.208 ms, because the terrain was still setting the grid's
  extent. Both halves are the same defect: **a heightfield's bounds are the whole
  map.** See `m_Oversized`.
- **`BroadphaseMinBodies` cannot be tuned from one scene.** After that fix the
  arena wins from ~36 bodies where it needed 123 before, and it is tempting to
  drop the constant. Terrain does not win until past 200 and dips to 0.74× at 67
  bodies on the way, because the terrain body is a candidate for everything
  whatever the broadphase does, so the grid has nothing to reject and pays for
  the rebuild anyway. It stays at 200; measure both curves before touching it.
- **Comparing a raycast against "the first sample under the surface" only works
  if the ray starts above the surface.** A grazing test ray entered the map
  already underground; the marching reference reported the entry point and the
  raycast reported the first real crossing six metres later, and the two were
  answering different questions. Assert the premise in the test.
- **`GroundHeightBelow`'s default floor is 0**, and `Ragdoll`'s
  `m_StandClearance` still takes it. Latent only because the generator puts
  heights in [0, Amplitude]; the first map with ground below zero gets a wrong
  standing clearance, and it will look exactly like feet hovering.

---

## Conventions

**Comments explain *why*, and are worth the space.** The house style is to
record the reasoning and the wrong turn, not to narrate the code. `// Bumping
the generation is what makes every outstanding handle stale` earns its line;
`// increment generation` does not. Match the surrounding density.

**Adding a demo** is one line in `TestEnv/src/DemoRegistry.h`. The demos are
header-only and `DemoLayer` holds the is-this-demo-active guard in `final`
`Layer` overrides, so demos override `OnDemo*` hooks. Self-registration via
static initialisers was deliberately rejected: forgetting an include would
produce no demo and no compile error.

`TestEnv/src/Demo.h` holds `g_ActiveDemo`, the demo shown at startup.

**The README changelog is part of the work**, not an afterthought. Entries
record what was built, what broke, and what the measurement said. Keep writing
them.

**Don't build speculative scaffolding.** `MeshComponent` was deliberately left
unwritten until something read it — the owner has pushed back on scaffolding
before, and "a component with no system" was the phrase used to decline it.

---

## Where things stand, and what is next

`README.md` has the full roadmap (18 items). The clusters:

**Renderer debt** — **this cluster is now empty.** Recording ImGui panel state
into the replay format was the last of it: a demo registers the parameters that
reach its simulation (`RegisterParam`, one line per slider), the recorder samples
them per fixed step beside the input, and format version 2 carries a named table
plus tagged chunks. Verified by recording a session whose sliders move mid-run
and replaying it to a byte-identical frame, against a control with nothing
registered that diverges. Multi-viewport ImGui is done, behind `--viewports` and
off by default; the load-bearing part is the GL context save/restore around
`RenderPlatformWindowsDefault` in `ImGuiLayer::End`, without which the screen
capture reads an undocked panel's window instead of the scene. `ShaderLibrary`
is done; the texture
slot count is now queried from the driver and the sampler switch generated to
match (32 slots on this machine, was a hardcoded 16); `egss.py` fetches a
pinned, checksummed premake instead of telling you to go and download one.

The **`TestEnv` PCH was measured and declined** — it makes a clean build 39%
slower. `TestEnv` is one translation unit, and 67% of its compile is codegen,
which a PCH cannot touch. Do not reopen it without first making the demos
separate `.cpp` files; the changelog entry has the phase breakdown.

**3D** — `.gltf`; rotate/scale gizmo handles. Everything else in this cluster
is done: materials with instance overrides, `ShaderLibrary`, `.mtl` parsing,
submesh ranges, smoothing groups, and `assets/models/beacon.obj` in the demo
actually exercising them. `Raycast3D` answers "what is in the way" in 3D, which
is what Cube3D's emitter occlusion now runs on — and what unblocks 3D acoustics
whenever that is wanted.

**2D** — per-pixel lighting is done and verified (light map, then
`BlendMode::Multiply` for surfaces; 27 checks through `ReadPixelRGBA`). Nothing
outstanding in this cluster, which makes it the thinnest one on the list.

**Physics** — rotation is **done**, built in three separable pieces and each
verified before the next: angular state, then `Sat2D`, then the join. The
narrowphase runs `Sat2D`, `Contact` holds up to two `ContactPoint`s with their
own lever arms and impulses, and the solver has its angular terms. Rays, bounds
and `ResolveCircle` all work in body-local space. What is left in this cluster
is joints and colliders beyond boxes and circles.

The 2D grid was measured after the 3D one and had the same two faults: it was a
*different simulation* from brute force (diverging 0.1 world units by step 56 —
ordering, not dropped pairs, proved by the sort fixing it outright), and it lost
badly below ~100 bodies, 7-10x slower at 13. Both fixed the same way, with one
difference: `BroadphaseMinBodies` gates the pair search only, leaving the grid
dirty so `Raycast` can still build it on demand. Physics2D and Scene captures
moved as a result; Breakout did not, having one moving body and so no ordering
to be sensitive to.

**3D physics** — bodies collide, roll, rest and stack, and there is now a
uniform-grid broadphase. It is **bit-identical to brute force** (candidates are
sorted into brute-force order before testing, because sequential impulses are
order-dependent), which is what makes `UseBroadphase` a real A/B and what makes
it safe to switch on automatically. It does switch automatically:
`BroadphaseMinBodies = 200`, because below ~120 bodies the grid measurably
*loses* — 3x slower at 13 bodies.

**Bodies too large to bucket are kept out of the cells**, and out of the grid's
extent — `m_Oversized`, switchable via `BroadphaseExcludeOversized`, and
bit-identical either way (28 checks, three scene families, including joints and
sleeping). Without it a heightfield went into all 29,575 cells every step *and*
sized the grid from the map rather than from what stood on it, which made the
grid eleven times slower than no broadphase in a scene it switched itself on for.
The rule is a cost comparison — out once the cells a body spans exceed the body
count — rather than a shape test, so it catches a wide static floor too.

**No demo reaches the grid on default settings**: Physics3D caps at 90 bodies and
Map Building needs ~185 blocks placed by hand. Anything about this code has to be
measured in a scene built for it, and a demo capture will not show a regression
here.

**Capsules are in**: segment-with-a-radius along the body's own y axis, with a
closed-form inertia tensor checked against its sphere and thin-rod limits.
Capsule–sphere and capsule–capsule reduce to sphere–sphere once the nearest
points are known; capsule–box clips the segment against the face for a
two-point manifold, without which a capsule rests at a permanent tilt.
**Capsule–capsule is deliberately one point** — two parallel capsules settle
more slowly than two boxes, and the fix is the same clipping if it is ever
wanted.

**Ball-and-socket joints** are in — the first bilateral constraint, with a 3x3
effective mass, warm starting, and contact suppression between jointed bodies.
Verified against the compound-pendulum period to 0.09%. **Use eight velocity
iterations or more for jointed work**: at a 20:1 mass ratio, one or two
iterations do not merely lose accuracy, they gain energy and throw the chain
about. Sleeping propagates along joints, or half a chain freezes and anchors
the rest to mid-air.

**Hinges are in too**, with angle limits — knees and elbows. Built as a ball
joint plus two locked rotations rather than as its own constraint: a 2x2
angular effective mass beside the 3x3 point one. The limit is the only
unilateral piece of a joint and so the only part that clamps its impulse.
Verified to 0.08% against the hinged-bar period, with zero overshoot at the
stops. **Limits are perfectly inelastic** — there is no restitution term, so a
limb striking its stop loses that spin rather than bouncing.

**Cone-and-twist limits** are in for ball joints, so shoulders and hips can be
built. Swing and twist are separated by a **swing-twist decomposition** of the
relative quaternion rather than measured directly, because direct measurements
interfere — a twist read off a reference vector changes when the bone swings.
The relative quaternion's scalar part is forced non-negative first, or a small
swing gets reported as a large one. Verified to within 0.05 degrees of the
limits given, from eight directions, with the swing re-measured independently
of the joint's own decomposition.

**Motors are in**, for both hinges and ball joints. Written as a velocity
constraint with a target rather than as a torque, which makes them PD
controllers without a separate derivative term. Two rules for using them:
**never warm start a motor** (its budget is per step, and a carried-over
impulse sitting at the clamp makes it silently stop pulling), and
**`MotorMaxSpeed` must be matched to `MotorMaxTorque`**, or the motor asks for
a speed it cannot decelerate from and limit-cycles.

**The humanoid rig exists** — `TestEnv/src/Ragdoll.h`, thirteen bodies and
twelve joints, passive or powered on the **M** key. Pose drift over ten seconds
is 0.137 rad powered against 13.900 passive. It topples on its own and that is
correct: motors hold joint angles, and a body tipping about its ankles does not
change one.

**Balance stands, and cannot be pushed.** The measurement is complete and drawn
every step: support polygon from the foot contacts, centre of mass, capture
point (`com + v*sqrt(h/g)`), and a signed margin that goes negative before
anything looks wrong. The **ankle is a tightly-limited ball joint, not a
hinge** — that change is what gave it a roll axis and fixed lateral balance,
which no amount of gain tuning could. The figure stands **35 seconds** unpushed.

**A 20 Ns shove still puts it down in two seconds**, and that is the strategy's
real limit rather than a tuning problem: an ankle can only move where the weight
bears *within* the feet, so once the capture point leaves the support polygon
nothing at the ankle can recover it.

**Stepping costs nothing and buys nothing.** The trigger is fixed: a step now
needs the capture point to be outside the feet *and stay outside* for eight
consecutive fixed steps, which stops it firing during ordinary sway. Measured as
a survival fraction over sixteen perturbed trials, that restores quiet standing
to 12/16 — exactly the no-stepping figure — from 8/16 when it stepped on the
first crossing.

It still does not help under a shove: 8/16 either way at 20 Ns, 0/16 either way
at 40 Ns, because **the swing foot is carrying 363 N -- 52% of body weight -- at
the moment the step tries to lift it**. `Ragdoll::FootLoad` reports this and it
is on the demo panel. Every swing controller so far has been lifting a foot with
half the figure standing on it; the load has to come off before any trajectory
matters.

The standing pose **bounces**, and it cannot be tuned out: motor stiffness 14 is
the measured optimum (7/8 trials standing, against 1/8 at 11 and 3/8 at 18) and
is the only value at which the feet never leave the ground. Stiffer bounces
harder, softer cannot stand. The bounce is what standing costs here.

Two traps in measuring it. **A fallen figure's foot load is not a measurement**
-- check the mean is near 697 N before believing any row, or "turn the balance
off and the bounce goes away" reads as a result. And per-foot matters more than
the total: the pair never unloads (minimum 176 N) but each foot individually
reaches 0 N, spends **11% of its time under 150 N**, with quiet windows of about
**0.10 s**.

**That lead worked.** The lift is now gated on the swing foot's own load: the
trigger decides the step, then it waits for that foot to drop below 150 N,
re-aiming at the capture point while it waits and abandoning the step if no
trough arrives within a quarter second. Foot travel went from **8% of the
distance asked for to 22%** -- the first movement in that number across five
attempts.

Survival is still 8/16 at 20 Ns and 0/16 at 40 Ns, and **nothing since load
gating has changed it**. A shorter swing, a longer swing, and pre-committing to
a predicted trough were all tried and all made things worse or made no
difference. In metres the foot travels 0.11-0.16 m whatever the swing schedule,
against the ~0.5 m it needs.

Two traps found here. Pre-commit fails because the load is a **spike train** --
its derivative is huge and erratic, so extrapolation predicts troughs that never
arrive and the step commits at high load. And **beware the travel percentage**:
a longer swing reports a higher fraction only because it triggers when the
target is nearer, so always convert to metres. That is the second metric in
three sessions to be measuring its own denominator.

**The hip motor is not the limit** and never was: peak torque 166 Nm of a
220 Nm budget, saturated in none of the ten swing frames, peak demanded speed
6.5 rad/s of a 10 rad/s cap. Raising either would do nothing.

**The foot lands again four frames into the swing** and drags for the rest of
it. Load along one swing: 41 N, 0, then 189, 198, 171. Clearing the ground is
the constraint, so `m_StepLift` went from 0.40 to 1.40 rad -- airborne frames
2.3 to 7.1 and foot travel 0.083 m to 0.183 m, more than double from a leg that
is nominally shorter while swinging. The earlier reasoning that a bent knee
costs reach was backwards.

Travel is now 0.183 m of the ~0.5 m needed, and **survival has not moved**:
8/16 at 20 Ns and 0/16 at 40 Ns, unchanged by every controller and parameter
tried.

**The stance leg is fine too** -- averaged over twelve swings it carries 533 N
of the figure's 697, buckles 0.029 rad, slips 0.025 m, and runs its knee motor
at 23.5 Nm of a 160 Nm budget. And the trigger is **not** late: the centre of
mass is at 1.030 m when it fires, against 1.08 m standing. (A single traced
swing suggested 0.845 m and had to be thrown out -- the fourth single-run
false conclusion of that session.)

**Bend-early-straighten-late was tried and is worse**, monotonically: foot
travel 0.183 m at a symmetric knee peak, 0.131 m at a peak of 0.15, with the
airborne fraction flat throughout. Much of the foot's travel comes from the knee
*extending*, so extension late adds reach and extension early is wasted.

**Everything has now been measured and cleared** -- motor torque, speed cap,
trigger timing, stance leg, swing duration, knee profile, weight shift, stance
width. Three changes really did improve the mechanism (load gating, knee lift,
trigger persistence) and took foot travel from 0.06 m to 0.183 m. **Survival has
not moved once through any of it**: 8/16 at 20 Ns, 0/16 at 40 Ns, identical to
not stepping.

**The arithmetic was done and the rig is fine.** Linear inverted pendulum on the
rig's own numbers (71.2 kg, com 1.085 m, tau 0.3325 s, foot half-length 0.110 m):
ankles alone should catch 23.6 Ns, the 0.183 m step already achieved should catch
62.7 Ns, and the geometry allows 181 Ns. **The ankle figure is confirmed** -- 20 Ns
survives half the time and 40 Ns never does -- so the model is trustworthy and
the step should nearly triple what the figure can take.

**It does not, and the reason is one number**: support reach towards the fall is
**0.0019 m before the step and 0.0000 m after the foot lands**. The foot travels
0.183 m and the base it stands on gains nothing. Every controller performed
identically because none of them ever converted foot travel into support.

**Diagnosed: the body outruns the leg.** Along the fall direction over twelve
steps the foot moves **−0.069 m** while the centre of mass moves **+0.332 m**,
so the foot ends 0.58 m behind having started 0.18 m behind. It is not aimed
wrongly -- foot-to-target shrinks 1.243 m to 0.988 m through the swing -- but
the target is 1.24 m away to begin with, because the pelvis has already
travelled **0.724 m past the foot** before the swing runs.

Nothing is saturated: hip at 0.856 rad of a 0.960 rad cone, motor at 76%,
stance leg at 15% of its budget. **The step is not too weak, it is too late** --
a 0.16 s manoeuvre cannot catch a body already moving faster than a leg swings.

**The character is playable, and self-balancing turned out not to be needed.**
There are two modes. *Controlled*: the pelvis is a **kinematic** body -- a new
`BodyType`, infinite mass to the solver but integrated from its own velocity --
so the character is driven about and cannot fall, while every other body stays
dynamic and hangs off it. *Ragdoll*: the pelvis goes dynamic and the motors go
slack. `G` toggles; a hit above `m_RagdollThreshold` (2000 N, about three times
body weight) switches automatically.

Measured: 60 s controlled without falling, walks exactly 8.000 m in 5 s at
1.6 m/s, ragdolls at 40 Ns but not at 20 Ns.

**The trap that cost the most here**: the impact trigger fired on the character
touching *itself*. A forearm against the torso and one shin against the other
are not jointed to each other, so they collide normally and their impulses look
like a car strike. It knocked the character down within a quarter second of
standing up. An impact now requires exactly one side of the contact to be part
of the character.

The walk carries momentum -- speed eases in and coasts out, heading turns at a
limited rate, and the figure walks along its facing rather than along the input.
Weight comes from a vertical bob, a lateral sway and a sharp drop at foot
plant, all driven off the gait phase, tuned to a person's 4-5 cm of vertical and
2-3 cm of lateral hip travel (measured: 4.7 and 2.6).

**The figure faces +z** -- its toes stick out that way. This matters: the knees
were built with the range `(-130, 0)` and could therefore *only* hyperextend,
which is why the walk looked wrong. Knees are now `(0, 130)` and elbows
`(-140, 0)`. Check the facing before reasoning about any joint direction.

**Controls**: `WASD` walks the character (camera-relative), arrows orbit a
third-person camera, `G` ragdolls and gets up, `Space` shoves, `F` swaps back to
the old fly-around. The camera follows the *torso* rather than the pelvis --
the pelvis carries the walk's bob and sway on purpose and a camera pinned to it
is seasick -- and eases at a rate per second so the lag does not vary with frame
rate.

**Getting up is a blend**, not a snap: the pelvis goes kinematic and is carried
back to standing over about a second while the motors come up from limp and drag
the limbs into the pose. It stands on whatever is underneath it and keeps the
heading it fell facing.

**The root follows the ground.** `PhysicsWorld3D::GroundHeightBelow` is new --
highest surface under a point, tested against world AABBs rather than shapes,
which is exact for unrotated boxes and reads slightly high otherwise (the right
way to be wrong for a ground probe). The walk probes under the root for stand
height, *eased* rather than followed because the probe is a step function at a
box edge, and probes a stride ahead to lift the swing leg before it would meet a
riser. Measured: 1.090 m standing on the flat, 1.390 m on a 0.30 m step; knee
target 1.75 rad climbing against 0.90 on the flat.

**The feet plant.** The gait describes the foot, not the joints: a stance foot
is pinned where it landed and the body travels over it, and two-link IK solves
the hip and knee to reach it. Slide is **0.0106 m a frame against 0.0267 for a
dragged foot**.

Two traps that cost the most here, both geometric. The foot target must be at
the **ankle**, not the ground -- the ankle rides 0.10 m above the sole. And the
rig stood with its legs **dead straight** (hip 0.90 m above the ankle, leg
exactly 0.90 m), so there was no slack to place a foot even a centimetre
forward; the character now stands with a slight crouch, which is why people do.

The legs are also driven harder than the rest of the body -- stiffness 40 and
four times the torque -- because they track an IK target that moves every frame.
Stiffer is not better: at 90 it oscillates and slides worse than doing nothing.

**Terrain is seeded and replicable** (`TestEnv/src/Terrain.h`, standalone).
The noise is a **hash of the lattice coordinate**, never a random generator --
a generator's state would make each height depend on how many were drawn before
it. `HeightAt` and the mesh are checked against each other to 0.000000 m,
because one is what a character stands on and the other is what is drawn.

**A heightfield's surface is triangles, not a bilinear patch**, and confusing
the two is a silent centimetre. They agree at the four corners of a cell and
along its diagonal and nowhere else, differing by
`fx*fz*(h00 - h10 - h01 + h11)` -- 1.75 cm on this map. Sampling *on the
lattice* to check them proves nothing, which is how the first version passed.
`Terrain::HeightAt` answers from `Egss::Heightfield3D` now so there is one
surface; keep it that way.

**Terrain contact normals are per triangle and a `Contact3D` has only one.**
The narrowphase adopts the deepest point's normal and re-measures every other
point's depth along it. Do not be tempted to emit one contact per triangle
instead: warm starting is keyed on the body **pair**, so several contacts
between the same two bodies discard each other's impulses every step.

**A box against a heightfield is tested by its corners, vertically.** Closest-
point-on-triangle is the wrong question for a shape with no radius -- it only
yields a direction once the corner is already through the surface. The gap
this leaves is a box wider than a cell straddling a peak; a foot is 0.22 m
across a 0.5 m grid, so nothing here is in that position.

**`m_KneeAt` and `m_AnkleAt` are in the rig's own frame, not world.** The
`ball`/`hinge` helpers in `BuildRagdoll` add `origin` themselves, which is what
sets the convention. Anything else using them has to add it too -- and because
the Ragdoll demo builds at the world origin, getting this wrong is invisible
there and only shows up on terrain, as feet 26 cm in the air and legs reaching
for the middle of the map.

**Ask `GroundBelow` for a height and a normal together, never separately.**
They have to come from the same body. Two independent searches disagree the
moment anything overlaps -- a step at the foot of a hill -- and a foot laid
flat to one surface at the height of another is worse than not conforming.

**A heightfield has two normals and they are not interchangeable.**
`SurfaceAt` gives the triangle's face normal, which is what a contact wants and
what jumps at every edge. `SmoothNormalAt` gives central differences over a
cell, which is what anything *orienting itself* to the ground wants -- a foot
oriented by a face normal snaps every half metre.

**Do not measure sole flatness as a gap.** The first attempt measured the
lowest corner of the foot against the ground under it and read 0.2978 m on the
*flat* demo -- which is that demo's 0.30 m step passing under the footprint. Any
gap under a foot is dominated by ground discontinuity. Measure the angle between
the sole's normal and the ground's instead.

**Run the flat demo as the control before blaming terrain.** Both of the
terrain-shaped bugs so far were not terrain bugs. `Ragdoll` on its slab is the
same code with `origin` at zero, and the numbers it produces (soles 0.0009 m
above the floor, pelvis exactly at `StandClearance`) are what "right" looks
like.

**`acos` of a dot product is not how to measure an angle between unit
vectors.** It is ill-conditioned at 1: two normals identical to every printed
digit still give `sqrt(2*eps) ~ 0.02 deg`. That reading cost an hour twice now.
Use the chord -- `2*asin(|a - b| / 2)`.

**A 64 m map cannot be lit by the `Physics3D` shader.** Its point light
attenuates as `1/(1 + 0.015 d^2)` -- 1.3% at 70 m -- so terrain comes out black.
Map Building has its own sun-plus-sky shader. If another demo goes outdoor
scale, it needs the same.

**Reset what a rebuild rebuilds.** Two pieces of state outlived `BuildScene`
and both were real bugs, not test artefacts. `m_Facing` is only ever written by
the get-up, so a reset after walking left the rig built facing +z while the gait
placed feet for the old heading -- legs crossed, standing knee wandering 4-31
degrees instead of sitting at 5. And `m_StaggerVelocity` was not cleared when he
stood up, so whatever felled him was still there and felled him again. Both are
cleared now; if you add gait state, clear it in the same places.

**Run a standing check first, not last.** Those three symptoms looked like three
regressions from the work in hand. Running the standing knee test immediately
after a build gave 5.1 to 5.1, and later in the suite 4.1 to 30.8 -- which is
what separated leaked state from a real regression in one measurement.

**Impacts are impulses, not forces.** A hit of J newton-seconds on 71.2 kg gives
J/m of velocity; the capture point is v*sqrt(h/g) ahead; a step of reach R
catches R/tau, about 1.5 m/s, and he gets ~2.5 steps. **Do not test the capture
point against the support polygon** -- running normally puts it 1.16 m from the
nearest foot, further than any shove that used to fell him, because running is
controlled falling. Only the *unintended* velocity counts. Reach is 0.50 m
fore-aft and 0.26 m sideways, which is why a side-on shove fells him sooner.

**`m_StaggerThreshold` is body weight (698 N) on purpose.** Below it, his own
weight through his legs registers as a shove and he staggers while standing.

**The lean's sign**: positive tips the top of the body towards +z, which is the
way the figure faces. It was negated, so he leaned backwards at every speed.
The torso does *not* trail the pelvis -- measured within 0.1 degrees, the spine
motor at 260 N.m carries it rigidly -- so leaning the pelvis alone moves the
whole figure as one plank; the lean is split between pelvis and spine.

**Sway and roll must key off which foot is carrying**, not `sin(gaitPhase)`.
Tied to raw phase they were inverted at a walk and correct at a run, because
which foot is down at a given phase depends on the duty factor.

**A jump's wait for a foot-plant cannot be unconditional** -- `m_GaitPhase` is
pinned to 0 below 0.05 m/s, so a standing press would hang forever. Standing
still is an immediate two-foot hop. Landing resets the gait phase to the lead
foot, which removes the need for a per-foot landing timer.

**Controls**: `WASD` walks (camera-relative), `shift` runs, `space` jumps, `X`
shoves, `G` ragdolls and gets up, `F` swaps to the fly-around. `P` is pause and
`R` is reset -- a shove was briefly bound to `P` and was dead code, because the
pause handler above it returns first.

**A run is a duty factor, not a speed.** What fraction of its cycle a foot is
down: 62% walking (the overlap is double support) and 36% running (the gaps are
flight). The gait had this hard-wired at exactly half by a bare
`cos(phase) < 0`, which is neither gait. And **do not scale foot reach with the
running stride** -- a quarter of a 2.4 m stride puts the foot 0.60 m ahead,
needing the hip 0.23 m lower to reach it, which is overstriding and brakes. A
run gets its length from flight; the reach is capped.

**The jump is ballistic height on a kinematic root**, so the apex is `v^2/2g`
and the flight is `2*sqrt(2h/g)` -- measured 0.533 s against 0.534 predicted.
The apex reads ~5% low and that is the explicit integrator's half-step
`v*dt/2`, not a bug.

**Impacts have three bands now**, not two: below the stagger threshold nothing,
between the thresholds the root is pushed and the legs chase it and he recovers,
above the ragdoll threshold he goes down. The single threshold was why he
"ragdolled abruptly".

**Getting up is two stages** -- gather the feet under the hips at kneeling
height, *then* rise over them. One lerp to a standing pose reads as being
placed, not getting up. Blend strength back at twice the gather rate or the legs
have no authority until it is too late, and **lift the feet** during the gather:
dragged along the floor they catch on their own friction and stay where they
fell (one leg was 0.79 m out of place at the rise).

**Prone versus supine is the pelvis's local +z**, the way the figure faces --
not local +y, which runs up the body and is horizontal for anyone lying flat, so
its sign is noise. And a settling ragdoll lands on a *side* every time, so a
test that shoves him over exercises neither branch.

**Do not give the standing leg reach slack.** Standing asks for slightly *more*
reach than the leg has, so the reach clamp pins the knee target and the knee
rests against its extension stop -- steady at 5.1°, and that is how a person
stands. Give it slack and the knee balances near full extension, where the law
of cosines has a gain of about **12° per millimetre**, and the kinematic
pelvis's own micro-motion drives it through both joint limits: at 0.006 m of
crouch the standing knee swung -3° to 150°. Lowering the IK's extension cap to
cut the gain is *also* wrong -- the leg is then asked for a reach it cannot
deliver and fights the floor (-148° at cap 0.99).

**One gait cycle is two steps**, so the foot lands a **quarter** of a stride
ahead of the body, not half. Getting that factor wrong forced the stride down to
0.45 m to keep the foot reachable, which meant **7.1 steps a second at 0.225 m a
step** -- three and a half times human cadence. Now 1.2 m per cycle at 1.3 m/s:
2.2 steps/s, 0.60 m steps, and slide fell to 0.0036 m/frame as a side effect.

**The dip is geometry, not style.** With the feet half a stride apart the hip
cannot stay at full leg length: it must drop `L - sqrt(L^2 - half^2)`, which
grows as the *square* of the stride. The bob used to be a magic number and was
also phased backwards -- lowest at mid-stance, highest at double support, the
opposite of its own comment and the shape of a waddle.

**`GroundHeightBelow` defaults its floor to 0**, so off the edge of the world it
reports y = 0 and the character walks out over the void. Probe it with a floor
far below when the question is "is there any ground here at all".

**The sole is flat to 1.0°** (was 4.4). The trap: the ankle motor's target is
*relative to the shin*, and the shin turns all through the stance -- so the
target has to be built from the shin integrated **one step forward**, or the
sole trails the lean. Neither the cone limit (never engaged, 0% of stance
frames) nor torque (10x changed nothing) had anything to do with it, and
stiffening the ankle made it worse: past roughly `2/dt` a motor rings, the same
ceiling the leg sweep found.

A planted foot now reports **4.53 of its 4 corners** (it was 1.84), so the
support polygon has three or more whenever a foot is down. This was written off
as needing a speculative margin in the narrowphase; a real stance fraction and a
sole flat to 0.9 degrees got there without one.

**The climb signal is per leg, at the foot's landing spot**, not one probe ahead
of the root along `m_Facing`. Facing lags the direction of travel while turning,
so approaching a step out of a turn aimed the probe sideways for the whole
approach. It also looks a stride *beyond* the landing spot (the landing spot
alone is only half a stride of warning) and is **latched for the swing**, or a
foot that clears the riser stops seeing a step and drops its lift halfway over.

The self-balancing and stepping work is documented in the changelog and is no
longer on the critical path. Its diagnosis is complete if anyone wants it: the
step does not widen the support polygon.

**Measure balance as a survival fraction, never as a mean fall time.** Fall
times here are heavy tailed -- most trials fall in a second, a few survive the
whole budget -- so a mean is a summary of its rarest outcomes and six- and
eight-trial means disagreed between sessions on configurations that were not
different. A claim of "+36% under a shove" in an earlier entry was exactly this
artefact and has been withdrawn.

A **wider stance was tried and refuted** — 0.20 m and 0.30 m are both worse than
0.10 m, because splayed legs push outwards and the balance has to fight the
reaction. The slider is still there so the refutation reproduces.

Watch out for one trap it produced: `SignedDistance` returns −1 for a support
polygon of fewer than three points, and a raised foot often leaves exactly that.
Read as a margin, that means "far outside" and triggers another step — 236 steps
in fifteen seconds before it was caught.

Gains were swept, not guessed, and the sweep is in the changelog: there is a
clear optimum with falls on both sides, and roll gain matters more than pitch.

After stepping comes a second character, which is the same rig with a different
driver. Convex hulls are the remaining shape, and would reuse `Sat3D`.

**Voxel terrain**, the newest area and the one being built in pieces. Piece one
is done: `VoxelField3D` (chunked SDF, negative inside, material per voxel) and
`MarchingCubes`, verified against 4πr² and (4/3)πr³ to 0.22% and 0.41% at 0.25 m
voxels, with closed-manifold topology and seamless chunks. **Nothing consumes it
yet, on purpose** — terrain you fall through is not a demo, so it lands with the
collider in piece two.

The intended shape, so the pieces are not built at cross purposes: **the field is
the collider**, not its triangles. Marching cubes is for the eye only. That keeps
slivers out of the narrowphase and reuses the arrangement `Heightfield3D` already
proves works — a field queried locally rather than turned into geometry first.

**Piece two is done too**: `ColliderShape3D::Sdf`, sphere/capsule/box against the
field directly. Bodies rest at exactly one slop; a rolling sphere reports
`I/mr² = 0.4102` against a solid sphere's 0.4 at two angles, which is the body's
inertia arriving through the contact solver; and the same terrain built as a
heightfield and as a distance field agrees on where the ground is to
**0.00009 m** over 121 off-lattice points. `GroundBelow` marches the column with
sphere tracing rather than answering in closed form, because "the height at
(x, z)" only has one answer when the ground is single-valued — under an arch it
correctly reports the floor from inside and the roof from above.

`FieldHit` and `ContactFromField` are shared with the heightfield unchanged, and
should stay that way: the manifold problem is identical and it is the part that
was hard to get right.

**Piece three is done**: the **Voxel terrain** demo — 3D-noise generation,
chunked meshing, digging with `EditSphere`, and severed rock handed to the solver.
Edits are local (1 chunk inside one, 2 across a seam, 0 for a miss), the island
conservation law holds exactly (19,208 attached + 147 in the island = 19,355
before), and cutting a capped pillar's neck frees exactly the cap.

Two approximations to know about. **An island becomes one box** — `Sat3D` is
convex-only, so a long thin slab rests at an angle a real one would not; its
*mesh* is the true surface, so the picture and the collider disagree by design
until convex decomposition exists. And **`Volume` is voxel count times voxel
volume**, which under-reports small pieces.

**Piece four is done**: `VoxelStress`. Load is routed to the anchors along the
connections, each carries the centre of mass of what passes through it, and
coplanar connections are grouped into **sections** judged as `W/A + M r / I`.
Grouping is what makes it statics — judged face by face, a wide neck resists
bending no better than a narrow one.

Verified against two formulas it does not contain. A cantilever's root stress
`3 rho g L^2 / h`: the residual is **exactly `1 + s/L`**, one voxel of length,
asserted as a law rather than bounded as a tolerance, and the thickness sweep is
*constant* across 3/5/7/9 layers, which says the thickness dependence is exact.
That only holds because each face gets its own extent (`A s^2/12` by the parallel
axis theorem, plus a half-voxel on the outer fibre); without it the model reads
low by `n/(n+1)`. And a column's height limit `sigma/(rho g)`: stands to 14 m,
fails from 15, predicted 14.56.

**Cost is settled**: 20.87 ms a pass (solidity cached into a flat array) and
**29 ms an edit** against the original 180, because `Relieve` breaks several
independent sections from one analysis instead of running an analysis per break.
Region-bounding was the plan and does not work — see the trap above.

**The full path is verified end to end**: undermine a propped ledge, three
sections fail, a 3,607-voxel island comes away past the cliff face, meshes, falls
and lands. That is the demo's own sequence, which had never run before.

**Sections are judged by a cut test rather than a threshold.** Bending belongs
only to a section that is the sole path to ground — blocked, walked, and checked
for an anchor — so `MinSectionLinks` is back to 1 and a genuine single-voxel neck
is judged on its merits. Every earlier result survives it unchanged.

**A severed piece is a compound of boxes**, from greedy growing over its voxels —
an L of 2,695 voxels comes out as 2 boxes that tile it exactly, where one box
round it is 60% air. `ColliderShape3D::Compound` reuses every existing box test by
turning a child into a box body in world space, and merges the child manifolds
the way terrain triangles are merged, because a contact still carries one normal.
`VoxelIsland` also carries an oriented box (principal axes, 79% rock against an
axis-aligned 27%) for callers that want one shape rather than several.

**The voxel list is empty.** What is left is not voxel work: the compound's
single-normal manifold is the same compromise the heightfield makes, and a lump
wedged in a corner gets the plane of whichever child is deepest.

Still true: the strength must clear the map's own self-weight or the terrain
collapses untouched — 500 kPa for the demo's 15 m terrain, where 180 kPa does
not.

The original tension design read: per-connection load *with a
moment term*, so length matters and one connection cannot hold a mountain — which
is the gap in Teardown's pure-connectivity model. Checkable against the cantilever
root stress 3ρgL²/h. Two constraints already known from measurements elsewhere in
this project: `Sat3D` is convex-only so a severed overhang needs decomposing or
approximating with boxes, and the broadphase table says a few hundred loose
bodies is where the cost starts to bite, so islands need a minimum size.

**Acoustics** — partitioned FFT convolution (for
dense recorded impulses); more bands with per-material curves. **Per-band
scattering** is the natural follow-on now that scattering exists: it needs one
ray per band, since a ray carries three energy packets but only one direction.

**3D acoustics is done.** `Acoustics3D` traces the *scene* through `Raycast3D`,
so the room is the geometry you can see and there is no second set of collision
boxes to keep in step — the same `Visible` flag hides a wall from the eye and the
ear. Verified in a shoebox against formulas the tracer does not contain: mean
free path within **0.40%** of 4V/S, RT60 within **9%** of Eyring at absorptions
0.10/0.20/0.35, 13 of 13 checks. The 2D footprint of the same room over-predicts
the mean free path by **1.73×** and RT60 by **1.69×**, and the 1.73 is the ratio
of π·A/P to 4V/S reproduced to three figures.

Everything dimension-independent is shared from `Acoustics.h` /
`AcousticsInternal.h` — the tail statistics, the impulse taps, Eyring. Only ray
spreading, the diffuse draw and the cast differ. **`Acoustics2D` was proven
byte-identical across that refactor** (every scalar to nine figures, echogram sum
and centroid, every reflection and tap, two settings); pin it the same way before
touching the shared code again.

Three things about 3D that are not just 2D with a z: rays spread on a
golden-angle spiral rather than a lat/long grid, which would crowd the poles and
so over-sample the floor and ceiling; diffuse bounces come from a
cosine-weighted hemisphere, not a uniform one; and the frame for that draw picks
its helper axis *away* from the normal, or a floor and a ceiling — the two
surfaces 3D adds — cross two parallel vectors and get a zero-length basis.

`Acoustics3D` indexes `PerBodyAbsorption` and `PerBodyScattering` by `EntityId`
rather than by a physics body handle. Same shape, different namespace.

**Build a traced room out of slabs, never one hollow box** — a ray starting
inside a box gets a hit at distance zero, so a source in a single-box room never
gets anywhere.

### Known approximations, stated so they are not mistaken for bugs

- The traced RT60 sits **~10% above** the diffuse-field formula *when every
  surface is a mirror*. Sabine and Eyring assume a diffuse field; a rectangle
  with mirror walls never becomes one, since a ray's direction only takes four
  values. `Scattering` is the fix and it works — +9.9% falls to +2.1% at full
  scattering, reproduced at two absorptions — so a room set up with sensible
  scattering figures should now agree with the formula to a couple of percent.
  A room left at `Scattering = 0` still shows the old bias, by construction.
- **The decay fit resolves about 3 percentage points.** Re-running one trace
  with four different scattering seeds spreads the measured RT60 that far. Any
  difference smaller than that is noise, and a sweep that looks like it has a
  shape probably does not. Widen the trace budget before believing one.
- **Mean free path cannot detect a non-diffuse field.** A specular rectangle
  and a fully diffuse one both land on π·A/P — the specular case by a
  path-weighted average that works out to the same thing. Use `TailRoughness`
  or the RT60 bias instead.
- Rendered per-band decays read slower than traced ones. Once a band's tail has
  died, what remains in that band of any analysis is leakage from its
  neighbours, and the bass tail is still ~20 dB louder. The treble/bass ratio
  is the honest measure. Re-measured after the splitter rebuild: **the chain
  delivers 83–90% of the darkening the trace itself describes**, holding across
  four late windows, and falling off deeper into the tail exactly as residual
  leakage predicts. Quote that ratio, not a bare dB figure — the old 15.4 dB
  number was recorded without its windows, which is what made it impossible to
  compare against.
- 2D acoustics overstates reverb time: a real room's floor and ceiling are half
  its reflecting area. **Now measured rather than asserted** — on one room, 2D
  gives RT60 1.397 s against 3D's 0.828 s, a factor of 1.69. Use `Acoustics3D`
  when the answer matters.
- **`Acoustics3D` bounces sound off a mesh's bounds, not its triangles**, because
  that is what `Raycast3D` tests. For rooms, which are made of walls, the bounds
  *are* the geometry; for a sphere or a torus they are not, and it will sound like
  the box around it.
- The traced-vs-Eyring bias in 3D is **one-sided and grows with absorption**:
  +2.8% at 0.10, +5.1% at 0.20, +8.9% at 0.35, with full scattering on. Same
  cause as the 2D bias — a bounce is heard by the listener regardless of which way
  it was heading — so treat 10% as the accuracy of the method.
- **A corridor needs far more rays than the demo's slider allows.** With the
  blocked-off corridor on and the listener in its dead end, RT60 reads 1.21 s at
  128 rays and 1.99 s at 8192 — few rays ever find the mouth, so its share of
  the tail comes from a thin sample. The slider stops at 1024. Not a bug; the
  variance of a stochastic method, visible for once.
- **Triangle winding is counter-clockwise seen from outside**, and it is worth
  checking rather than assuming: `CreateSphere` and three of the four models
  were inside-out for a long time, behind a comment claiming they agreed. The
  symptom is seeing an object's *interior* once back-face culling is on, and it
  is subtle on a smooth lit shape because the normals can be right while the
  winding is not. Audit with `cross(p1-p0, p2-p0)` against the outward
  direction — but a **non-convex** mesh (the torus) cannot be judged that way,
  and a mesh whose normals were *generated* cannot be judged against them at
  all, since the loader derived them from the winding in the first place.
- **`Tr` and `d` in an .mtl are inverses.** `d` is dissolve (1 opaque), `Tr` is
  transparency (0 opaque). The parser follows the spec; plenty of exporters do
  not, so a model that loads fully transparent is worth suspecting here first.
- **`glDrawElements` takes a byte offset, not an index.** `DrawIndexed`'s
  `firstIndex` counts indices and the backend multiplies. Getting that wrong
  draws from the wrong place while every submesh number stays correct.
- **Screenshot verification from outside the process does not work here.**
  `import` hangs against XWayland (once for two minutes before timing out) and
  `xdotool` window matching picks up stale windows; two sessions were lost to
  it. This is now solved from the inside — see "Capturing frames" — and the
  outside route should not be retried.
- A listener **inside** a body finds zero paths, however many bounces are
  traced, and reports an empty echogram rather than an error. The demo runs
  `ResolveCircle` on the source and listener every frame for this reason; any
  headless trace has to do the same. The default listener (8, 2) sits inside the
  right-hand pillar, so this is the *first* thing that bites.
- **The 2D light's rendered falloff is f², not the linear f the vertex colours
  describe.** `DrawLight` scales colour *and* alpha by the same `f`, and
  additive blending contributes `rgb * alpha`, so they multiply. This has been
  true since the light polygon was written; a comment in `Lighting2D.h` claimed
  otherwise until it was corrected. Measured exponent: 2.0093. It is arguably
  the better look — closer to inverse-square — so it was left alone rather
  than "fixed".
- The light fan's interpolated falloff follows the **chord**, not the radius, so
  it dims slightly too fast mid-triangle. Measured at the ring's floor of 16
  rays it is 1.8%, and at a contrived 45° it is 38%. Known, quantified,
  deliberately not fixed.

---

## Working with the owner

- They read the code. Do not over-explain what is already visible in it.
- They ask for specific things and mean the scope they asked for. "Keep going"
  means take the next roadmap item and say which one you picked and why.
- They spot real problems — a duplicate ImGui ID, a README that had stopped
  being updated. Take the report seriously and check rather than reassure.
- Report failures with the output. Several of the most useful moments in this
  project were a test failing and the *reason* being more interesting than the
  fix.
