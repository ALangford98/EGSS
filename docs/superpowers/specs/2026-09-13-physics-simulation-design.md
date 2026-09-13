# Physics simulation — design

## Goal

Let an entity with `PhysicsComponent` (Body Type, Mass, Friction,
Restitution — already exists, data-only, added under editor roadmap item 1)
actually fall, collide, and come to rest during Play mode, using the real
rigid-body engine (`GS::PhysicsWorld3D`/`GS::RigidBody3D`) that already
exists in this project but has never been wired into the generic editor
scene — today it's only ever driven directly by specific demos
(`Physics3D.h`, `Ragdoll.h`).

## Why this shape

- **`PlayMode` already does everything a physics integration needs done
  for it.** `PlayMode::Play()` snapshots the whole scene
  (`g_EditorScene.Save(SnapshotPath())`) before anything runs, and
  `PlayMode::Stop()` reloads it — so any transform physics changes during
  Play is already reverted for free on Stop, with zero physics-specific
  undo logic needed. `PlayMode::OnFixedUpdate(float dt)` already exists and
  already only runs `while (s_Playing)`, at fixed-step granularity — which
  is also exactly what this project's own determinism rule requires
  ("anything that moves belongs in `OnFixedUpdate`, not `OnUpdate`").
  Physics is one more thing `OnFixedUpdate` drives, not a new subsystem
  with its own lifecycle to invent.
- **`RigidBody3D::MakeBox`/`MakeStaticBox` already compute correct mass and
  inertia** (`SetMass` + `RecalculateInertia`, both called internally) —
  confirmed by reading the functions, not assumed. Building a `RigidBody3D`
  by hand and computing a box inertia tensor directly would be
  reimplementing something this engine already has, and already uses in
  three other demos.
- **No `RemoveBody` exists on `PhysicsWorld3D`.** A fresh
  `GS::PhysicsWorld3D` is constructed at the top of every `Play()` call
  rather than trying to clear and reuse one — the same "always create
  fresh rather than reuse a stale handle" reasoning `EditorHistory.h`'s own
  comment on `PlaceEntityCommand` already gives for entity ids.
- **Orientation must round-trip through the exact same composition
  `TransformComponent::GetTransform()` uses**, not a second, independently
  written rotation order. `GetTransform()` applies `Rx * Ry * Rz` (X then Y
  then Z, degrees) before scale/translate; building `RigidBody3D::Orientation`
  from anything else would silently rotate the object differently the
  instant physics touches it. The reverse (`RigidBody3D::Orientation` back
  into `TransformComponent::Rotation` after a step) needs
  `glm::extractEulerAngleXYZ` specifically — the one glm function that
  inverts exactly that composition order, not a generic quaternion-to-Euler
  conversion that might assume a different order.
- **A box collider sized from the entity's own mesh bounds needs no new
  data on `PhysicsComponent`.** `MeshComponent::Geometry->GetBoundsMin()/
  GetBoundsMax()` already exists and is already read this same way
  elsewhere (`EditorSceneView::DrawSelectionBox`) — reusing it here is the
  same pattern, not new plumbing.

## Scope

**In scope:**
- Building one `GS::RigidBody3D` per entity that has both
  `TransformComponent` and `PhysicsComponent`, at the top of
  `PlayMode::Play()`.
- Box collider only, sized from `MeshComponent`'s bounds × the entity's
  `TransformComponent::Scale` (component-wise), or `Scale * 0.5` for an
  entity with no `MeshComponent` — an invisible physics volume the size a
  default unit-cube primitive's own bounds would be at that scale.
- Friction/Restitution carried from `PhysicsComponent` onto the body
  directly (`RigidBody3D` already has both fields).
- Stepping the world once per `PlayMode::OnFixedUpdate(dt)` call, then
  copying every body's `Position`/`Orientation` back onto its entity's
  `TransformComponent`.
- Static and Kinematic `PhysicsComponent::Type` both produce a zero-mass
  body (immovable by contacts/forces) — Kinematic support beyond that
  (something actually driving its velocity) is explicitly deferred, see
  below.

**Explicitly out of scope / deferred:**
- **Sphere/Capsule colliders**, or any per-entity shape choice — box only,
  per the earlier scope decision. A future "let the user pick a shape"
  pass can add a `Shape` field to `PhysicsComponent` without touching
  anything else this spec builds.
- **Static obstacles without a `PhysicsComponent`.** Strictly opt-in, per
  the earlier scope decision — an entity with no `PhysicsComponent` is
  invisible to physics entirely, including as a floor.
- **Kinematic bodies actually being driven.** A `Kinematic`-typed body is
  built (zero mass, so nothing can push it) but nothing here ever sets its
  velocity — that needs a way for a script to say "move this," which
  doesn't exist yet and isn't asked for here.
- **Joints, compound/heightfield/SDF colliders, or runtime-spawned physics
  entities** (an entity a script creates *during* Play never gets a body —
  only what exists in the scene at `Play()` time does). All real,
  already-supported `PhysicsWorld3D` features, none of them needed for
  "entities with physics properties fall and collide."
- **2D physics** (`PhysicsWorld2D`/`RigidBody2D`). The editor's scene is
  inherently 3D (`TransformComponent` is `glm::vec3` throughout); nothing
  about this editor's generic entities is 2D-shaped the way `Breakout.h`'s
  own demo-specific `RigidBody2DComponent` usage is.

## Design

### Where the world lives

`PlayMode.h` gains two more file-local globals, alongside `s_ScriptEngine`/
`s_Prepared`:

```cpp
inline GS::PhysicsWorld3D s_PhysicsWorld;
inline std::unordered_map<GS::EntityId, GS::PhysicsWorld3D::BodyHandle> s_PhysicsBodies;
```

`GS::PhysicsWorld3D` needs no constructor arguments — `Gravity` defaults to
`(0, -9.81, 0)`, which is exactly what a first version wants.

### Building bodies (`Play()`)

Reset first (`s_PhysicsWorld = GS::PhysicsWorld3D(); s_PhysicsBodies.clear();`
— the "always fresh" reasoning above), then, in the same
`for (GS::EntityId id : g_EditorScene.GetEntities())` loop `Play()` already
has for scripts:

```cpp
if (auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(id))
    if (auto* physics = g_EditorScene.GetComponent<GS::PhysicsComponent>(id))
    {
        glm::vec3 halfExtents;
        if (auto* mesh = g_EditorScene.GetComponent<GS::MeshComponent>(id); mesh && mesh->Geometry)
            halfExtents = (mesh->Geometry->GetBoundsMax() - mesh->Geometry->GetBoundsMin()) * 0.5f * transform->Scale;
        else
            halfExtents = transform->Scale * 0.5f;

        GS::RigidBody3D body = (physics->Type == GS::BodyType::Dynamic)
            ? GS::RigidBody3D::MakeBox(transform->Position, halfExtents, physics->Mass)
            : GS::RigidBody3D::MakeStaticBox(transform->Position, halfExtents);
        if (physics->Type == GS::BodyType::Kinematic)
            body.Type = GS::BodyType::Kinematic;

        body.Orientation = glm::quat_cast(glm::mat3(
            glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.x), { 1, 0, 0 }) *
            glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.y), { 0, 1, 0 }) *
            glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.z), { 0, 0, 1 })));
        body.Friction = physics->Friction;
        body.Restitution = physics->Restitution;

        s_PhysicsBodies[id] = s_PhysicsWorld.AddBody(body);
    }
```

(`MakeStaticBox` already sets `Type = BodyType::Static` and zero mass;
Kinematic reuses that same zero-mass construction and then overwrites
`Type`, since "immovable by the solver" is what both share — see Scope.)

### Stepping and syncing back (`OnFixedUpdate`)

Added alongside the existing script-ticking loops:

```cpp
s_PhysicsWorld.Step(dt);

for (auto& [id, handle] : s_PhysicsBodies)
{
    if (!g_EditorScene.IsValid(id))
        continue;   // same staleness possibility scripts already guard against -- a script could destroy a physics entity mid-Play

    auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(id);
    if (!transform)
        continue;

    const GS::RigidBody3D& body = s_PhysicsWorld.GetBody(handle);
    transform->Position = body.Position;

    glm::mat4 rotation = glm::mat4_cast(body.Orientation);
    float x, y, z;
    glm::extractEulerAngleXYZ(rotation, x, y, z);
    transform->Rotation = glm::degrees(glm::vec3(x, y, z));
}
```

No stale-handle removal loop is needed the way `s_Prepared`'s is: a
destroyed entity's body simply stops being written back to (its handle is
just skipped by the `IsValid` check above) — `PhysicsWorld3D` has no
`RemoveBody` to call either way, and the whole world is discarded at the
next `Play()`.

### Extra include needed

`glm::extractEulerAngleXYZ` lives in `glm/gtx/euler_angles.hpp`, a gtx
("experimental") header — `GLM_ENABLE_EXPERIMENTAL` must be `#define`d
before including it, the same way `Sat3D.cpp` already does for a different
glm extension. Scoped to `PlayMode.h` only.

## Testing

Temporary self-tests (deleted after), directly checkable without a live
ImGui frame, using `PlayMode::Play()`/`OnFixedUpdate()`/`Stop()` against a
scene built by hand:

- A `Dynamic` box entity with gravity on, no floor: after N fixed steps,
  its `Position.y` has decreased by an amount checked against `y0 - 0.5 *
  9.81 * (N*dt)^2` (free-fall, hand-derived — the standard kinematic
  formula, not re-deriving the engine's own integration) within a small
  tolerance for the integrator's own discretization error.
- A `Dynamic` box resting on a `Static` box directly beneath it: after
  enough steps to settle, `Position.y` stops decreasing (within a small
  epsilon across the last several steps) — confirms the contact solve
  actually stops it rather than falling through.
- A `Dynamic` entity with no `MeshComponent`: confirm it still gets a body
  (half-extents from `Scale * 0.5`) and falls under gravity, same as the
  mesh-having case — the meshless fallback path is real, not dead code.
- `PlayMode::Stop()` after some simulated motion restores
  `TransformComponent::Position` to its exact pre-`Play()` value (this is
  really testing `PlayMode`'s existing snapshot/restore, but from
  physics's own entry point, confirming no physics-specific state survives
  a Stop the snapshot doesn't already know about).
- Orientation round-trip: build a `RigidBody3D` with a known
  `Orientation` set the same way `Play()` sets it from a hand-picked
  `Rotation`, immediately extract it back with the same
  `extractEulerAngleXYZ` call `OnFixedUpdate` uses, and confirm the result
  matches the original `Rotation` (within floating-point tolerance) for a
  rotation away from gimbal lock (e.g. 30°/40°/50°, not 90° on Y) —
  confirms the two ends of the round trip actually agree, not just that
  each compiles.

Visual confirmation (an object actually visibly falling and landing) via a
capture during Play, the same disclosed standing gap every other
interactive feature in this editor already has for live click-through —
`--lockstep` capture during a running Play session is enough to prove the
math and the render agree, even without live input.
