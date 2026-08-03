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
against it and holds six demos.

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
At the time of writing, `HEAD` was `37f8e41` "Added Scattering and
PerBodyScattering in AcousticsSettings" — the acoustics work (frequency-
dependent absorption, then scattering coefficients) is committed. Uncommitted,
building in all three configs:

- a **rebuilt band splitter** — 4th-order Butterworth per band plus a fourth
  unfiltered path for broadband taps, which took the treble band's bass
  rejection from −33 dB to −102 dB at 100 Hz. Verified, 21 checks.
- **per-pixel 2D lighting** — a light map plus a new `BlendMode::Multiply`,
  and `Framebuffer::ReadPixelRGBA` so the result could be checked. Verified,
  27 checks.

The owner commits their own work, often between sessions and sometimes while
a reply is being written. **Do not commit or push unless asked**, and do not
assume something is still outstanding because a previous message said so.

### Subsystems, roughly in the order they were built

| Area | Where | Notes |
| --- | --- | --- |
| Core loop | `Application`, `Layer`, `LayerStack` | Fixed timestep + render interpolation. `OnFixedUpdate` is simulation, `OnUpdate` is presentation |
| Renderer 2D | `Renderer2D` | Three batches: quads (indexed), lines, triangles. One draw call each. Blend modes: Alpha, Additive, Multiply |
| Renderer 3D | `Renderer`, `Mesh`, `ObjLoader` | `Submit` per mesh, no batching. `.obj` loading with smoothing groups |
| Cameras | `Camera` base, `Orthographic`, `Perspective` | `BeginScene` takes any `Camera` |
| Framebuffers | `Framebuffer` | `RED_INTEGER` attachment drives pixel-exact picking |
| Scene | `Scene`, `Entity`, `ComponentStore` | ECS-lite: dense arrays, generational handles |
| Physics | `PhysicsWorld2D`, `RigidBody2D` | Warm-started sequential impulses, island sleeping, raycasts, uniform-grid broadphase. **No rotation** |
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
```

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

### Screenshot automation is unreliable here

It works often enough to be tempting and fails in ways that look like bugs:

- `xdotool key --window <id>` **does not reach GLFW.** Activate the window and
  send to the focused window instead. An earlier "cycled every demo" check
  silently never switched demos.
- `import -window <id>` sometimes returns a **stale** frame. Two probes coming
  back byte-identical means the capture, not the app.
- Window geometry changes between runs. Re-read it every time; hard-coded
  coordinates land on the wrong widget and quietly change a slider.
- Clicking ImGui controls by coordinate is fragile. To test a non-default code
  path, temporarily change the default, rebuild, screenshot, revert.

Prefer numbers. Use screenshots to confirm something *looks* right after the
numbers say it *is* right.

---

## Traps that have bitten more than once

- **Depth, in 2D.** `glm::ortho(-1, 1)` makes **higher z nearer**, and at equal
  z the depth test rejects the later fragment — so the **first** thing drawn
  wins, the opposite of painter's order. This caused three separate bugs,
  including "the lights don't blend" (which was not blending at all).
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

**Renderer debt** — `ShaderLibrary`; query `GL_MAX_TEXTURE_IMAGE_UNITS`; a PCH
for `TestEnv`; vendor a premake binary; multi-viewport ImGui.

**3D** — material handling (the 3D demo sets uniforms by hand); `.gltf`;
rotate/scale gizmo handles; `.obj` smoothing *groups* rather than the on/off
flag.

**2D** — per-pixel lighting is done and verified (light map, then
`BlendMode::Multiply` for surfaces; 27 checks through `ReadPixelRGBA`). Nothing
outstanding in this cluster, which makes it the thinnest one on the list.

**Physics** — rotation. Bodies translate but never spin; needs SAT, multi-point
manifolds and angular impulses. The hardest item on the list.

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
  is the honest measure — it showed 15.4 dB of darkening end to end.
  **That figure predates the splitter rebuild and has not been re-measured.**
  The leak it was fighting went from −33 dB to −102 dB at 100 Hz, so the
  end-to-end number should now be better; nobody has checked how much. Doing
  that measurement again is a cheap, worthwhile piece of work.
- 2D acoustics overstates reverb time: a real room's floor and ceiling are half
  its reflecting area.
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
