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

**Renderer debt** — what is left here is recording ImGui panel state into the
replay format. Multi-viewport ImGui is done, behind `--viewports` and off by
default; the load-bearing part is the GL context save/restore around
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

Not done: **getting up** is a snap rather than a blend, and the feet do not
reliably plant while walking (fine for a kinematic root, not fine for anything
wanting traction).

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

**Acoustics**, the most recently active area — 3D acoustics (`Acoustics2D` is
2D because `Raycast` is); partitioned FFT convolution (for dense recorded
impulses); more bands with per-material curves. **Per-band scattering** is the
natural follow-on now that scattering exists: it needs one ray per band, since
a ray carries three energy packets but only one direction.

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
  its reflecting area.
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
