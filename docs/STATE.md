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

The live area is **the humanoid** — `Runtime/Humanoid.h`, `Locomotion.h`,
`MotionClip.h`, tried in the Parkour, CannonRun and Ragdoll demos. Since
2026-10-21 the direction is snappy and player-first (GTA / Red Dead for
moving and being hit, Assassin's Creed for parkour) in three projects:
locomotion (landed), parkour on it (next), hits. `editor_roadmap.md`'s
"Humanoid" section holds the detail; its wishlist is still the editor's
todo list, ordered easiest to hardest.

Last landed, newest first. **One line each; the write-up lives in
`docs/CHANGELOG.md`** under the same date — this list used to hold the full
write-ups and reached 1,100 lines before they were moved out on 2026-09-30.

- **2026-10-21 — Locomotion**: the keys decide where he goes (a run in
  0.45 s, turned back in 0.33), the body is posed every step from CMU walk,
  jog and run strides (`Runtime/Locomotion.h`) -- no limb into the ground,
  a real run, pivots not shuffles; physics has him back for hits, moves and
  falls. `locomotion old` for the physical walk. Walking hits are the
  stagger now; standing ones still physical. The hips follow the body's
  Femininity and the walking style.
- **2026-10-18 — Set pieces from recordings**: turning round, stopping
  and setting off are a person's (CMU 47_01, 16_33, 69_01) played on his
  bodies, hips and feet carried, feet held where they land, joined where
  his feet are and handed back to the walk smoothly (hips never past 25
  m/s^2). Cut short by the keys, a jump or a hit; the walk's own where the
  ground is not flat or there is no room.
- **2026-10-17 — Modelled on recordings**: CMU motion capture
  (`TestEnv/assets/mocap`, `Runtime/MotionClip.h`), `ghost walk` in the
  parkour yard plays a person on his bodies beside him. Measured against
  them, the walk's swing, toe-off, hip and chest turn, arms and step width
  now follow the person's (swing knee 44 -> 58 of 73, chest turn 0 -> 8 of
  9). The run's posture and arms too; the head leads into turns; he leans
  into curves. A second spine joint measured and left for the get-up work.
- **2026-10-17 — Turning on his feet**: a turn steps, the hips wait for
  the feet, the swing goes round the planted leg. The legs no longer pass
  through each other turning back (12-15 cm inside each other, crossed
  for seconds; now apart). A run brakes before it turns back. Quicker:
  180 degrees standing 0.92 -> 0.60 s, walking 1.15 -> 0.90 s, running
  1.75 -> 1.27 s. Next of the owner's three: limbs into the ground.
- **2026-10-17 — The cannon run** (`--demo CannonRun`): a course under
  fire, against the clock, from pieces (hand-made, or any seed: `course
  4711`); fair cannons that charge, lead and hold; sounds; a camera that
  turns to what is coming (hold RMB). The dodge (LMB + direction:
  sidestep, cut, brake or dive by speed); slopes (tilted boxes are
  ramps); get-ups natural in 82% of 142 knock-downs (87% front, 74% back).
- **2026-10-17 — The parkour course** (`--demo Parkour`): a lane per
  move, `station <n>`; crates climbable; the get-up's arms (natural
  elbows, no flick on standing) and its blow-ups (27 in 40 among
  rolling cannonballs, 4 on a clear floor, now 0 and 1); stairs, the
  corner run, hanging keys, both cameras' pitch, screens' glow fixed.
- **2026-10-16 — More parkour**: corner wall runs, cat leap, hanging
  round corners, tic-tac, precision jumps. Force-driven hips, stronger
  legs and more solver iterations measured against limb depth: no help.
- **2026-10-16 — Wall runs, speed vault, hanging**: wall run and wall
  jump, the one-handed speed vault, hanging (shimmy, climb up, let go)
  and dropping to a hang off a high edge. Yard: Run Wall, High Ledge.
- **2026-10-16 — Parkour chains**: one move straight into the next
  (vault -> vault, drop -> vault, vault off an edge into a roll); rolls
  only where there is room.
- **2026-10-16 — Parkour**: vault, step on, climb, drop and roll, and
  walls that stop him; the get-up's floor penetration halved; run held
  standing no longer throws the rig. Try it in the crash test hall's
  parkour yard (Runner Dummy).
- **2026-10-16 — Humanoid bodies, walking styles, a natural get-up**:
  height/mass/build per humanoid (the dummy follows), eleven walking
  styles with every parameter editable, and a get-up choreographed from
  how people do it (push up, all fours, lunge, rise; rolling over first
  from on the back). Default rig bit-identical. Next: parkour.
- **2026-10-16 — Controls**: View > Controls rebinds the game's actions
  per project (`controls.txt`, `GS::InputMap`); prompts, hints and
  `.gsui` screens follow; scripts get `input.isActionDown`.
- **2026-10-16 — The crash test hall**:
  - fly the camera, walk up to things and press E: take control of the
    dummy, use the terminal on the cannon's computer, or operate the
    cannon (cursor aim, trajectory line, HUD).
  - New: the game UI kit (`.gsui` screens, `ui.*` in scripts), game modes,
    procedural models (`model:`), and an opt-in renderer
    (`EnvironmentComponent`).
  - The Windows player links again.
- **2026-10-15 — Crash test demo**: File > Add Demo Scene > Crash Test,
  then Play: a dummy, a launcher and crates, driven from an in-game
  terminal (` opens it). New runtime pieces: GameConsole, LauncherComponent,
  physics substeps for fast bodies, RigidBody3D::Enabled.
- **2026-10-15 — Natural variation**: each humanoid walks its own way (seeded
  by entity), steps never land alike, and standing he breathes, shifts
  his weight and glances about. Deterministic: replays exact.
- **2026-10-15 — The humanoid moves fluidly**: driven feet rolling heel to
  toe (planted feet no longer skate), pole-vector legs (no backwards
  knees), springs instead of rate limits (no jolts), a spring-mass run,
  a phased jump; stairs and steps work. Plus `--capture-every` and
  physics collision groups / `IgnoreStatic`.
- **2026-10-14 — Walking on through a stumble; bracing**: hits mid-walk are
  physical and he walks on (capture-point step offset b = L/(e^(T/τ)−1));
  lost stumbles brace (the head lands at 2 m/s, not 5); no get-up in
  mid-air; stand heights from his own feet (built in the air, he falls).
  Open: running hits, loads from above (editor_roadmap.md, Humanoid).
- **2026-10-14 — The humanoid stumbles and catches itself**: a hit while
  standing is caught physically, by ankles and real steps, through a
  footprint-bounded virtual leg. It holds 80–100 Ns and falls beyond; the
  step widens the base, which fixes the August problem. Also: the editor
  camera returns after Play, and physics bodies get a launch velocity.
- **2026-10-14 — Keep Play changes; ball body; humanoid in games**: Stop
  asks whether to keep; Body = Ball rolls (5/7 g sin θ measured);
  `Runtime/Humanoid.h` extracted bit-identically from the Ragdoll demo and
  playable as Player/NPC (mannequin). **Live research thread:** RAGE-style
  stumble/catch -- see editor_roadmap.md "Humanoid".
- **2026-10-14 — Tools panel, characters, terrain**: sectioned Tools that
  wrap; Player/NPC (capsule controller in Runtime, works in exports);
  terrain on any primitive with a vertex count, heightfield collision.
  Fixed the cylinder's inward winding. Captures of dense scenes need
  `--antialiasing 1` to be bit-reproducible.
- **2026-10-13 — Rotate/scale gizmos and anti-aliasing**: 1/2/3 or T
  switch Move/Rotate/Scale; 4x MSAA in the editor view and exported games
  (a project setting, View > Anti-aliasing; `--antialiasing 1` for
  regression captures).
- **2026-10-13 — Project save/load reworked** (project-centric): the
  editor reopens the last project or shows a Projects window; Ctrl+S;
  quit asks; asset paths project-relative (older projects migrate on
  open). Fixed Browse… closing its own dialog.
- **2026-10-12 — iOS export** (export piece 7, the last): an Xcode project
  to build on a Mac. **Never built by Xcode** — compiled/linked here under
  the iOS defines with stand-ins for UIKit. All seven export pieces landed.
- **2026-10-12 — Android export** (export piece 6): arm64 APK via the NDK,
  NativeActivity, touch as the GSS pointer. **Verified statically only** —
  never run on a device yet. GSS gained `input.isPointerDown/pointerX/Y`.
- **2026-10-12 — Web export** (export piece 5): Emscripten + WebGL2,
  byte-identical to editor Play in headless Firefox.
- **2026-10-12 — Windows export + Linux AppImage** (export pieces 4 and
  2b): cross-built with fetched llvm-mingw; byte-identical under Wine.
- **2026-10-12 — File > Export Game…** (export piece 3 of 7): background
  export with stages, Open Folder / Run Game; `--project`,
  `--open-export-dialog`.
- **2026-10-08 — Linux export** (export piece 2 of 7):
  `TestEnv --export <project> --out <dir>` → a self-contained folder +
  `.tar.gz`; scripts ship as JS, no TypeScript compiler; per-project compiled
  scripts. Verified unpacked elsewhere and run from `/`.
- **2026-10-07 — GSPlayer** (export piece 1 of 7): `Runtime/` shared by the
  editor and a player that runs a project with no editor; byte-identical to
  editor Play (`--start-play`). Editor Play no longer draws overlays.
- **2026-10-02 — GSS physics bindings**: `applyImpulse`/`applyForce`/
  `get`/`setVelocity`/`isTouching` in both execution paths via
  `GS::ScriptPhysics`; Kinematic bodies now drivable; `setPosition`
  teleports bodies. Also: Ctrl+Z in the text editor no longer undoes the
  scene.
- **2026-10-01 — roughness drives shininess** (piece 3 of 3): Beckmann-mapped
  exponent, normalised strength, in the existing Blinn-Phong. Procedural
  materials are complete.
- **2026-09-30 — normal maps for linked materials** (piece 2 of 3): the
  editor shader renders the graph's Normal output, frame from screen-space
  derivatives, no vertex tangents.
- **2026-09-30 — procedural material graph** (wishlist #11, piece 1 of 3):
  node-graph editor panel, `.gsmat`, four-map export, live scene link.
  Only albedo renders; normal mapping (#5) and roughness shading follow.
- **2026-09-28 — GSS array stdlib** (wishlist #3): `console.*`, `number[]`,
  `.length`/`.map`/`.filter`/`.toArray()` in the compiled path. `.find`,
  array literals and a collection-returning native call are deferred —
  see the spec's "Explicitly out of scope".
- **2026-09-15 — keyboard look in the editor view** (`307133f`).
- **2026-09-14 — mesh UV template export** (wishlist #9).
- **2026-09-13/14 — multiplayer foundation.**
- **2026-09-13 — Scenes panel / multi-scene projects** (`bc11510`, spec
  `2026-09-13-scene-tree-design.md`; no changelog entry).
- **2026-09-13 — editor theming** (wishlist #2, `302071d`; no changelog
  entry — the spec is the record).
- **2026-09-13 — entity grouping** (`a22ed36`; no changelog entry).
- **2026-09-13 — physics simulation during Play.**
- **2026-09-12 — right-click context menu** (wishlist #1; no changelog
  entry).
- **2026-09-12 — mesh authoring.**
- **2026-09-07 → 09-11 — the editor pivot**: scene composition, menu bar,
  terminal, text editor, scripting runtime, file tree, Play/Stop, Breakout
  recreation, cross-entity scripting, imports, the transpiler and static-link
  execution, both Breakout scripts graduated to C++, lighting, project I/O,
  profiler, Inspector ID field. One changelog entry each.

## What is next

Nothing is in progress. Next agreed: **parkour on the new locomotion** --
forgiving triggers, AC-style chaining, the wall run rebuilt -- then **hits**
(physical stumbles from a posed walk). Whether to delete the old walk
(`DriveWalk`, set pieces) is the owner's call. Older open threads, none
started:

- **GSS transpiler follow-ups** — `.find` (needs an optional/no-match type),
  a collection-returning native call such as `scene.findAllByTag`, array
  literals, vec3-typed class members, and script bindings for sound and
  physics (the roadmap's "foundational gaps" names the last as the real
  blocker for gameplay code).
- **Exporting games, pieces 6–7** — Android, then iOS; the owner asked for
  all of them without stopping. Windows `.ico` embedding is
  a small deferred follow-up. Roadmap and the mobile assessment in
  `editor_roadmap.md`.
- **Sound bindings for GSS** — the other half of the scripts gap; paused
  for the export work.
- **Procedural materials beyond the editor shader** — per-vertex tangents,
  and normal/roughness from imported `.mtl`/glTF, if materials spread to
  imported models or the planet. Nobody has asked yet.
- **Mesh authoring follow-ups** — face/edge-picking UI (its own sub-project),
  and the deferred fixes listed in `editor_roadmap.md`.
- **Breakout's recreation still has no bricks** — possible since
  `scene.spawn` landed; nobody has asked for it.
- **Only Cube3D implements `OnExportToScene`**; whether the other demos should
  is undecided.
- **Character attributes** — deferred by the owner; **ask before starting**.
- **Editor click-through** — lighting, project I/O, text editor and profiler
  have never been exercised by real mouse/keyboard input; no GUI-automation
  tool exists here.

## When this file is wrong

Fix it in the same commit as the work that made it wrong. It is one page on
purpose: if updating it feels like a chore, it has grown too big. A landing
gets **one line** here and its write-up in `docs/CHANGELOG.md`; durable facts
about a subsystem belong in `docs/HANDOVER.md`'s subsystem table. Older plans
in `docs/superpowers/plans/` tell their final task to "add a Last landed
bullet in the same style" as the full write-ups — that instruction is what
grew this file, so read it as "one line here, the write-up in the changelog".
