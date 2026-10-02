# GSS physics bindings

**Status:** approved 2026-10-02
**Scope:** the physics half of `editor_roadmap.md`'s foundational gap "scripts
cannot trigger a sound or touch physics". Sound is a separate, later spec.

## Problem

Play mode simulates `PhysicsComponent` entities (`PlayMode.h`,
`PhysicsWorld3D`), but a GSS script cannot push a body, read or set its
velocity, or ask whether it hit something. Kinematic bodies are built but
nothing drives them (the 2026-09-13 physics spec's own open gap). And a
script's `setPosition` on a physics entity is silently undone: Play mode
copies body to transform every step.

## Decisions

1. **Polling, not a callback**, for collisions: `isTouching("Tag")`. An
   `OnCollision(other)` hook needs a new lifecycle method in both execution
   paths and an entity value type compiled scripts don't have.
2. **A bridge declared in GS, implemented by Play mode.** The transpiler's
   syntax check (`ScriptEngine::RunSyntaxCheck`) compiles generated code with
   only GS include paths, so whatever compiled scripts call must be reachable
   from `<GS.h>`. `GS/src/GS/Scripting/ScriptPhysics.h` declares the calls and
   a `Backend` interface; `PlayMode` implements it over its world while
   playing. Interpreted natives and generated C++ call the same functions, so
   the two paths cannot disagree.

## Script surface

On `entity`, and on any entity value from `scene.findByTag`:

| GSS | Meaning |
| --- | --- |
| `applyImpulse(x, y, z)` | velocity += J / m now, wakes the body |
| `applyForce(x, y, z)` | accumulated, applied over the next step |
| `getVelocity()` | vec3 (`.length` / `.toArray()` work in compiled code) |
| `setVelocity(x, y, z)` | replaces velocity, wakes the body -- drives Kinematic bodies |
| `isTouching("Tag")` | true when this body touched any body whose entity has that tag in the last step |

## Edge cases

- **No body** (no `PhysicsComponent`, or not playing): calls do nothing,
  `getVelocity` is zero, `isTouching` is false, and one warning per entity
  (not per frame) names the entity and what to add.
- **Static bodies:** impulse/force/velocity are ignored (infinite mass), no
  warning -- the body exists.
- **`setPosition` on a physics entity teleports the body.** Play mode records
  the position it last wrote to each transform; if the transform differs at
  the start of the next tick, the body moves there, keeping its velocity.
  Position only -- rotation stays body-owned.

## Verification

Temporary self-test, through the shared bridge and both paths:

- Impulse J on mass m: velocity changes by exactly J/m.
- `setVelocity` launch under gravity: y = y0 + v t - g t^2 / 2 within the
  integrator's O(dt) bound.
- Kinematic `setVelocity`: moves exactly v t, no gravity.
- A box dropped from h onto a "Floor": `isTouching("Floor")` first true
  within one step of t = sqrt(2 h' / g) (h' = gap between the surfaces).
- No body: no effect, exactly one warning over many calls.
- Teleport: a body moved by `setPosition` is there after the next tick.
- An interpreted script's `entity.applyImpulse` lands on the body.
- Each call transpiles and passes the g++ syntax check; `getVelocity()` is
  vec3-typed.
- BreakoutRecreation in Play captures byte-identically before and after (its
  scripts do not use physics).
- Three configs build.

## Out of scope

`OnCollision`, contact details, raycasts, joints, rotation teleport, sound.
