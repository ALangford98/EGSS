# GSS Physics Bindings Implementation Plan

> Executed natively, committed on `main`.

**Spec:** `docs/superpowers/specs/2026-10-02-gss-physics-bindings-design.md`

## Global Constraints

- Generated C++ must need only `<GS.h>`.
- One warning per entity for calls on an entity without a body.
- Scripts that don't use physics behave byte-identically.

## Review Focus

1. A script destroys its own physics entity mid-Play, then the next tick
   runs: no stale handle use (bodies of destroyed entities are skipped).
2. `isTouching` on a tag no entity has: false, no warning.
3. Play stopped and restarted: the backend is re-registered and the
   warned-once set cleared, so a new session warns again.

### Task 1: Bridge and Play-mode backend
- [ ] Baseline capture of BreakoutRecreation in Play.
- [ ] Self-test (fails to build): impulse J/m, launch vs kinematics,
      kinematic v t, isTouching landing time, no-body warn-once, teleport.
- [ ] `GS/Scripting/ScriptPhysics.{h,cpp}`; backend in `PlayMode.h`;
      teleport in the tick; register in Play, clear in Stop.
- [ ] Green, three configs, commit.

### Task 2: Interpreted natives and transpiler
- [ ] Self-test: an interpreted script's applyImpulse lands; each call
      transpiles + syntax-checks; getVelocity().length emits 3.
- [ ] Natives on the entity class; transpiler mapping in the identifier
      branch and `isVec3Expr`.
- [ ] Green, Breakout capture byte-identical, three configs, commit.

### Task 3: Finish
- [ ] Delete the self-test; changelog, STATE, roadmap; commit.
