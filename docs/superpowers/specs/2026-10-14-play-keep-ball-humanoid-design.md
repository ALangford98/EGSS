# Keeping play changes, a ball body, and the humanoid in games

**Status:** design approved by the owner on 2026-10-14 ("Go ahead, write
the spec and build it"). The decisions below were made together, or are
noted as mine.

## 1. Keeping changes made in Play

- **Today:** Play snapshots the scene and Stop reloads the snapshot, so
  everything done in Play is discarded. Nothing says so, which reads as
  "changes don't save".
- **Now:** the Stop button asks *Keep changes made during Play?*
  - **Keep** leaves the scene as it is at Stop and marks it unsaved.
  - **Discard** reverts, as before.
- **Entities the game made for itself are removed either way.** Today
  that's the follow camera the character controller creates. GameSession
  records what it creates, and keeping changes doesn't keep these.
- **"Don't ask again"** remembers the choice per user, beside the
  recent-projects list (`$XDG_CONFIG_HOME/gs-editor/settings.txt`), and Edit
  > *Ask again about keeping Play changes* resets it.
- **Other ways out of Play don't ask:** quitting, closing or switching
  projects, and Add Demo Scene all discard. The unsaved-changes question
  follows them anyway.

## 2. A body setting for characters: Character, Ball, Humanoid

`PlayerComponent` and `NpcComponent` gain an `int Body`:

- **0 Character:** the upright capsule controller, as before.
- **1 Ball:** a real dynamic sphere, described below.
- **2 Humanoid:** the active ragdoll, described in section 3.

It is saved as an extra field at the end of the `player` and `npc` lines.
Files without it read as Character, and older builds skip it.

**The ball:**
- **The body:** a dynamic sphere with radius 0.5 × the largest scale, full
  rotational inertia, friction 0.9, and little damping.
- **Driving it:** WASD (or the NPC's wish) gives a target spin, the speed ÷
  the radius about the axis up × direction. Torque is applied towards that
  spin, capped. In the air, a small horizontal force steers.
- **Jumping:** Space, when grounded, sets the vertical speed to √(2 g h).
- **What the player sees:** the transform takes the body's orientation, so
  the ball visibly rolls. The camera follows the centre without rolling
  with it.
- **Defaults:** Make Player or Make NPC on a sphere picks Ball.

## 3. The humanoid in games

- **The extraction.**
  - The rig and its controllers move out of `TestEnv/src/Ragdoll.h` into
    `Runtime/Humanoid.h`, one class:
    - the 13-body rig, built in a physics world it doesn't own;
    - the joint motors;
    - the balance measurement (centre of mass, support polygon, capture
      point) and the ankle strategy;
    - the reactive stepping (experimental, as now);
    - the walk and run, with leg and arm IK;
    - jumping, stagger, ragdoll mode and get-up.
  - It's driven by a small input struct: the wished direction and speed,
    sprint, jump, shove, and go limp or get up.
- **The demos stay as they are to use.** `Ragdoll` (and `MapBuilding`,
  which derives from it) keep their camera, scene, keys, tuning panel and
  drawing, and drive a `Humanoid`.
- **The refactor must not change behaviour.** A recorded input session
  through the Ragdoll demo, dumped as every body's position and orientation
  each step, must be identical before and after. Images can't serve as the
  check: the demo isn't run-to-run reproducible, an effect of the window's
  4× MSAA measured on 2026-10-14.
- **In games**, GameSession creates a Humanoid for a Body = Humanoid
  character:
  - It stands at the entity's position, facing its yaw.
  - It's fed by the same player input or NPC wish as the other bodies.
  - The entity's transform follows the pelvis.
- **The look:** a mannequin of the rig's own shapes. Each body is drawn as
  its collider (capsule, box or sphere), in the entity's colour, with the
  head and hands lighter. In the editor the rest pose is drawn where the
  entity stands; in Play, the live bodies are drawn. A skinned mesh is out
  of scope, since there is no skeletal skinning yet.
- **Defaults:** new Players and NPCs from the Tools panel are Humanoid.

## 4. In parallel: stepping research

Stumbling and catching a fall needs a step that widens the base, and
that's the open problem in the changelog (2026-08-09/10). It continues in
the Ragdoll demo, starting with the changelog's own proposed next
experiment. Because the demo now drives the shared class, a fix there
reaches games. It's a separate thread from this build.

## Verification

- **Play keep:** a test that plays, moves things, and checks the scene after
  Keep, after Discard, and that the follow camera is gone after both.
- **Ball:**
  - Measured against formulas the code doesn't use: on a flat floor, held
    forward, it reaches v = ω r and rolls without slipping (v ≈ ω r while
    it accelerates).
  - Released on a slope with no input, it rolls down with
    a = (5/7) g sin θ, the solid-sphere result.
  - It jumps √(2 g h).
- **Humanoid:**
  - The extraction is proved by the step-by-step state comparison above.
  - In GameSession, the humanoid stands for 10 s without falling, walks
    forward when W is held, and editor Play matches GSPlayer byte for byte
    with anti-aliasing off.
