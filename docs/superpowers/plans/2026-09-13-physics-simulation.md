# Physics Simulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an entity with `PhysicsComponent` actually fall, collide, and
come to rest during Play mode, using the existing `GS::PhysicsWorld3D`/
`GS::RigidBody3D` rigid-body engine.

**Architecture:** `PlayMode.h` gains a `PhysicsWorld3D` instance and an
`EntityId -> BodyHandle` map. `Play()` builds one box-collider body per
entity with both `TransformComponent` and `PhysicsComponent` (box sized
from `MeshComponent` bounds, or `Scale * 0.5` with no mesh); `OnFixedUpdate`
steps the world once and copies each body's position/orientation back onto
its entity's transform; `Stop()` needs no physics-specific code at all,
since `PlayMode` already reverts the whole scene from its pre-Play snapshot.

**Tech Stack:** C++17, GLM (including `glm/gtx/euler_angles.hpp`, an
"experimental" header requiring `GLM_ENABLE_EXPERIMENTAL`), the existing
`GS::PhysicsWorld3D`/`GS::RigidBody3D` engine code. No new dependency.

**Spec:** `docs/superpowers/specs/2026-09-13-physics-simulation-design.md`

## Global Constraints

- **Testing convention for this repo, not a generic framework:** every task
  adds a *temporary* self-test header (`XyzTest.h`, marked `// TEMPORARY --
  delete after verifying`), wires one `Run()` call into
  `TestEnv/src/TestApp.cpp`'s `TestEnv()` constructor, builds with `./gs.py
  build`, runs with `./gs.py run -- --hide-window --lockstep --capture
  <path> --capture-step N`, reads the `GS_TRACE`'d pass/fail lines from the
  output, then **deletes the test header and the two lines that wired it
  in**. There is no permanent test suite in this project — do not add one.
- **Compute expected values by hand before asserting them, from the real
  integration scheme, not idealized continuous kinematics.**
  `PhysicsWorld3D::IntegrateVelocities`/`IntegratePositions` (`GS/src/GS/
  Physics/PhysicsWorld3D.cpp:2740`/`2782`) use semi-implicit Euler with
  exponential linear damping: `v += Gravity * GravityScale * dt`, then `v
  *= 1/(1 + LinearDamping * dt)`, then (next call) `p += v * dt`.
  `RigidBody3D`'s real defaults are `GravityScale = 1.0`, `LinearDamping =
  0.01`; `PhysicsWorld3D::Gravity` defaults to `(0, -9.81, 0)`. Task 2's
  free-fall test's expected value is computed from this exact recurrence,
  not `y = y0 - 0.5*g*t^2` (which assumes continuous-time integration this
  engine does not use, and would be a plausible-looking but wrong
  expectation for a discrete stepper).
- **Verify at all three configs** (`./gs.py build all`) before considering
  any task's code changes final.
- **Never commit, never push mid-task.** Leave every task's changes staged
  for the owner to review and commit (this repo's rule for an interactive
  session), unless a background session's own separate, explicit
  worktree-and-merge-back policy in `CLAUDE.md` applies instead.

---

### Task 1: Build physics bodies in `Play()`

**Files:**
- Modify: `TestEnv/src/PlayMode.h:19-31` (includes), `:35-37` (globals),
  `:141-186` (the entity loop inside `Play()`)
- Test: `TestEnv/src/PhysicsBodyConstructionTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::PhysicsComponent` (`Type`/`Mass`/`Friction`/
  `Restitution`, already exist), `GS::RigidBody3D::MakeBox`/`MakeStaticBox`
  (already exist, `GS/src/GS/Physics/RigidBody3D.h:388`/`417`),
  `GS::PhysicsWorld3D::AddBody`/`GetBody` (already exist, `GS/src/GS/
  Physics/PhysicsWorld3D.h:284`/`370`)
- Produces: `PlayMode::s_PhysicsWorld` (`GS::PhysicsWorld3D`),
  `PlayMode::s_PhysicsBodies` (`std::unordered_map<GS::EntityId,
  GS::PhysicsWorld3D::BodyHandle>`) — Task 2 steps `s_PhysicsWorld` and
  reads `s_PhysicsBodies` to sync transforms back.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/PhysicsBodyConstructionTest.h`:

```cpp
// TEMPORARY -- delete after verifying Play() builds correct physics bodies.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "PlayMode.h"

namespace PhysicsBodyConstructionTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		g_EditorScene.Clear();

		// A: Dynamic, has a unit-cube mesh, non-axis-aligned-free rotation
		// kept to a single hand-verifiable axis (90 degrees about Y) so the
		// resulting quaternion has a known, independently-derivable value:
		// rotating 90 degrees about Y is exactly quat(cos45, 0, sin45, 0).
		GS::Entity a = g_EditorScene.CreateEntity("A");
		a.Get<GS::TransformComponent>()->Position = { 1.0f, 2.0f, 3.0f };
		a.Get<GS::TransformComponent>()->Rotation = { 0.0f, 90.0f, 0.0f };
		a.Get<GS::TransformComponent>()->Scale = { 2.0f, 2.0f, 2.0f };
		a.Add<GS::MeshComponent>({ std::shared_ptr<GS::Mesh>(GS::Mesh::CreateCube()), "primitive:cube" });
		a.Add<GS::PhysicsComponent>({ GS::BodyType::Dynamic, 2.0f, 0.3f, 0.1f });

		// B: Static, no mesh -- exercises the Scale*0.5 fallback.
		GS::Entity b = g_EditorScene.CreateEntity("B");
		b.Get<GS::TransformComponent>()->Position = { 5.0f, 0.0f, 0.0f };
		b.Get<GS::TransformComponent>()->Scale = { 3.0f, 1.0f, 1.0f };
		b.Add<GS::PhysicsComponent>({ GS::BodyType::Static, 1.0f, 0.5f, 0.0f });

		// C: Kinematic, with a mesh -- confirms Kinematic also ends up
		// immovable (zero inverse mass), not treated like Dynamic.
		GS::Entity c = g_EditorScene.CreateEntity("C");
		c.Add<GS::MeshComponent>({ std::shared_ptr<GS::Mesh>(GS::Mesh::CreateCube()), "primitive:cube" });
		c.Add<GS::PhysicsComponent>({ GS::BodyType::Kinematic, 1.0f, 0.5f, 0.0f });

		// D: no PhysicsComponent at all -- must get no body (opt-in only).
		GS::Entity d = g_EditorScene.CreateEntity("D");

		PlayMode::Play();

		Check(PlayMode::s_PhysicsBodies.count(a.GetId()) == 1, "A (Dynamic) got a physics body");
		Check(PlayMode::s_PhysicsBodies.count(b.GetId()) == 1, "B (Static) got a physics body");
		Check(PlayMode::s_PhysicsBodies.count(c.GetId()) == 1, "C (Kinematic) got a physics body");
		Check(PlayMode::s_PhysicsBodies.count(d.GetId()) == 0, "D (no PhysicsComponent) got no body -- opt-in only");

		{
			const GS::RigidBody3D& body = PlayMode::s_PhysicsWorld.GetBody(PlayMode::s_PhysicsBodies[a.GetId()]);
			Check(body.Type == GS::BodyType::Dynamic, "A's body is Dynamic");
			Check(body.HalfExtents == glm::vec3(1.0f, 1.0f, 1.0f), "A's HalfExtents = unit-cube bounds (0.5) * Scale (2) = 1");
			Check(std::abs(body.InverseMass - 0.5f) < 1e-5f, "A's InverseMass = 1/Mass = 1/2.0 = 0.5");
			Check(std::abs(body.Friction - 0.3f) < 1e-5f, "A's Friction carried from PhysicsComponent");
			Check(std::abs(body.Restitution - 0.1f) < 1e-5f, "A's Restitution carried from PhysicsComponent");

			// Hand-derived: rotating 90 degrees about Y is exactly
			// quat(cos(45deg), 0, sin(45deg), 0) = (0.70710678, 0, 0.70710678, 0)
			// -- not re-deriving Play()'s own construction formula, this is
			// the independently known quaternion for that specific rotation.
			glm::quat expected(0.70710678f, 0.0f, 0.70710678f, 0.0f);
			// abs(dot) close to 1 means "the same rotation," accounting for
			// a quaternion and its negation representing one identical
			// rotation (the double-cover property) -- a plain equality
			// check would be a false failure on the equally-correct sign.
			Check(std::abs(glm::dot(body.Orientation, expected)) > 0.9999f,
				"A's Orientation matches the known quaternion for a 90-degree Y rotation");
		}
		{
			const GS::RigidBody3D& body = PlayMode::s_PhysicsWorld.GetBody(PlayMode::s_PhysicsBodies[b.GetId()]);
			Check(body.Type == GS::BodyType::Static, "B's body is Static");
			Check(body.HalfExtents == glm::vec3(1.5f, 0.5f, 0.5f), "B's HalfExtents = Scale * 0.5 (no mesh)");
			Check(body.InverseMass == 0.0f, "B (Static) has zero inverse mass -- immovable");
		}
		{
			const GS::RigidBody3D& body = PlayMode::s_PhysicsWorld.GetBody(PlayMode::s_PhysicsBodies[c.GetId()]);
			Check(body.Type == GS::BodyType::Kinematic, "C's body keeps Type Kinematic, not overwritten to Static");
			Check(body.InverseMass == 0.0f, "C (Kinematic) has zero inverse mass -- immovable like Static");
		}

		PlayMode::Stop();
		g_EditorScene.Clear();

		GS_TRACE("PhysicsBodyConstructionTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it in: add `#include "PhysicsBodyConstructionTest.h"   // TEMPORARY`
near the other includes in `TestEnv/src/TestApp.cpp`, and
`PhysicsBodyConstructionTest::Run();   // TEMPORARY -- delete after
verifying` as the first line of `TestEnv()`'s constructor body.

- [ ] **Step 2: Build to confirm it fails**

Run: `./gs.py build`
Expected: FAIL — `s_PhysicsBodies`/`s_PhysicsWorld` don't exist on
`PlayMode` yet.

- [ ] **Step 3: Add the includes and globals**

In `TestEnv/src/PlayMode.h`, add to the includes (near the top, alongside
the existing `#include <GS.h>`):

```cpp
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
```

(`GLM_ENABLE_EXPERIMENTAL` must be defined before this header is included
anywhere in this translation unit -- placed right above the `#include` for
that reason, the same way `GS/src/GS/Physics/Sat3D.cpp` already does it for
a different glm extension.)

Add the two globals alongside the existing `s_Prepared`:

```cpp
inline GS::PhysicsWorld3D s_PhysicsWorld;
inline std::unordered_map<GS::EntityId, GS::PhysicsWorld3D::BodyHandle> s_PhysicsBodies;
```

- [ ] **Step 4: Build the bodies in `Play()`**

Right after the existing snapshot-save block (`if (!g_EditorScene.Save(...))
{ ...; return; }`) -- once `Play()` knows it's actually proceeding, not
before it:

```cpp
	s_PhysicsWorld = GS::PhysicsWorld3D();
	s_PhysicsBodies.clear();
```

Inside the existing `for (GS::EntityId id : g_EditorScene.GetEntities())`
loop in `Play()`, add this **before** the existing `if
(!g_EditorScene.HasComponent<GS::ScriptComponent>(id)) continue;` line, not
after it -- that `continue` would otherwise skip every entity that has a
`PhysicsComponent` but no script, which is the common case:

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
					glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.x), glm::vec3(1, 0, 0)) *
					glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.y), glm::vec3(0, 1, 0)) *
					glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.z), glm::vec3(0, 0, 1))));
				body.Friction = physics->Friction;
				body.Restitution = physics->Restitution;

				s_PhysicsBodies[id] = s_PhysicsWorld.AddBody(body);
			}
```

(This mirrors `TransformComponent::GetTransform()`'s own rotation
composition -- `Rx * Ry * Rz`, degrees -- exactly, so a physics body starts
at the same orientation the object was already rendering at.)

- [ ] **Step 5: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/physicsbody.png --capture-step 2
```
Expected: `PhysicsBodyConstructionTest: 15 passed, 0 failed` (4 got-a-body/
opt-in checks + 6 for A's body + 3 for B's + 2 for C's).

- [ ] **Step 6: Remove the temporary test**

Delete `TestEnv/src/PhysicsBodyConstructionTest.h` and the two lines in
`TestEnv/src/TestApp.cpp` that referenced it. Rebuild to confirm
`TestApp.cpp` still compiles clean.

- [ ] **Step 7: Mark task done**

No commit — leave the `PlayMode.h` changes staged in the working tree for
the owner to review.

---

### Task 2: Step the world and sync transforms in `OnFixedUpdate`

**Files:**
- Modify: `TestEnv/src/PlayMode.h:213-258` (`OnFixedUpdate`)
- Test: `TestEnv/src/PhysicsSimulationTest.h` (temporary)

**Interfaces:**
- Consumes: Task 1's `s_PhysicsWorld`/`s_PhysicsBodies`
- Produces: nothing further downstream — this is the feature's last task.

- [ ] **Step 1: Write the failing test**

Create `TestEnv/src/PhysicsSimulationTest.h`:

```cpp
// TEMPORARY -- delete after verifying physics stepping and transform sync.
#pragma once
#include <GS.h>

#include "EditorProject.h"
#include "PlayMode.h"

namespace PhysicsSimulationTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	constexpr float kDt = 1.0f / 60.0f;   // this project's real fixed timestep (GS::Application's default)

	inline void Run() {
		// --- Orientation round trip: construct the same way Play() does, ---
		// --- extract the same way OnFixedUpdate will, confirm they agree ---
		{
			glm::vec3 originalRotation(30.0f, 40.0f, 50.0f);
			glm::quat orientation = glm::quat_cast(glm::mat3(
				glm::rotate(glm::mat4(1.0f), glm::radians(originalRotation.x), glm::vec3(1, 0, 0)) *
				glm::rotate(glm::mat4(1.0f), glm::radians(originalRotation.y), glm::vec3(0, 1, 0)) *
				glm::rotate(glm::mat4(1.0f), glm::radians(originalRotation.z), glm::vec3(0, 0, 1))));

			float x, y, z;
			glm::extractEulerAngleXYZ(glm::mat4_cast(orientation), x, y, z);
			glm::vec3 extractedRotation = glm::degrees(glm::vec3(x, y, z));

			Check(glm::all(glm::lessThan(glm::abs(extractedRotation - originalRotation), glm::vec3(0.01f))),
				"extractEulerAngleXYZ inverts the same Rx*Ry*Rz construction Play() uses, within 0.01 degrees");
		}

		// --- Free fall: hand-derived from the engine's own real integrator ---
		// --- (semi-implicit Euler + exponential linear damping), not the  ---
		// --- idealized continuous-time y = y0 - 0.5*g*t^2 formula, which  ---
		// --- this discrete stepper does not implement.                   ---
		{
			g_EditorScene.Clear();
			GS::Entity ball = g_EditorScene.CreateEntity("Ball");
			ball.Get<GS::TransformComponent>()->Position = { 0.0f, 10.0f, 0.0f };
			// No mesh -- also exercises the Scale*0.5 fallback path (Task 1)
			// under real stepping, not just at construction time.
			ball.Add<GS::PhysicsComponent>({ GS::BodyType::Dynamic, 1.0f, 0.5f, 0.0f });

			PlayMode::Play();
			for (int i = 0; i < 30; i++)
				PlayMode::OnFixedUpdate(kDt);

			float actualY = ball.Get<GS::TransformComponent>()->Position.y;
			// Computed from v(n) = (v(n-1) + g*dt) / (1 + damping*dt), p += v*dt
			// each step, g=-9.81, dt=1/60, damping=0.01 (RigidBody3D's real
			// defaults) -- the exact recurrence PhysicsWorld3D::Step actually
			// runs, evaluated numerically over 30 steps: delta_y = -1.264875.
			float expectedY = 10.0f - 1.264875f;
			Check(std::abs(actualY - expectedY) < 0.01f,
				"after 30 fixed steps of free fall, Position.y matches the hand-computed semi-implicit-Euler result (expected " + std::to_string(expectedY) + ", got " + std::to_string(actualY) + ")");

			PlayMode::Stop();
			Check(std::abs(ball.Get<GS::TransformComponent>()->Position.y - 10.0f) < 1e-4f,
				"Stop() reverts Position.y to its exact pre-Play value (10.0) -- PlayMode's existing snapshot, exercised from physics's own entry point");
			g_EditorScene.Clear();
		}

		// --- Resting on a static floor: qualitative settle check, not an ---
		// --- exact closed form (contact-solve convergence isn't one)    ---
		{
			g_EditorScene.Clear();
			GS::Entity floor = g_EditorScene.CreateEntity("Floor");
			floor.Get<GS::TransformComponent>()->Position = { 0.0f, 0.0f, 0.0f };
			floor.Get<GS::TransformComponent>()->Scale = { 10.0f, 1.0f, 10.0f };
			floor.Add<GS::PhysicsComponent>({ GS::BodyType::Static, 1.0f, 0.5f, 0.0f });

			GS::Entity box = g_EditorScene.CreateEntity("Box");
			box.Get<GS::TransformComponent>()->Position = { 0.0f, 3.0f, 0.0f };
			box.Add<GS::PhysicsComponent>({ GS::BodyType::Dynamic, 1.0f, 0.5f, 0.0f });

			PlayMode::Play();
			for (int i = 0; i < 180; i++)   // 3 real seconds at 60Hz -- plenty to land and settle
				PlayMode::OnFixedUpdate(kDt);

			float ySettled = box.Get<GS::TransformComponent>()->Position.y;
			for (int i = 0; i < 10; i++)
				PlayMode::OnFixedUpdate(kDt);
			float yAfterMore = box.Get<GS::TransformComponent>()->Position.y;

			Check(std::abs(yAfterMore - ySettled) < 0.01f,
				"the box's height stops changing once it has settled on the floor (contact solve holds it, it doesn't fall through)");
			Check(ySettled > 0.0f, "the settled box is still above the floor's own position (0.0), not sunk through it");

			PlayMode::Stop();
			g_EditorScene.Clear();
		}

		GS_TRACE("PhysicsSimulationTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Build to confirm it fails for the right reason**

Run: `./gs.py build`
Expected: compiles (Task 1 already added everything this test's setup
needs), but the free-fall and resting-on-floor checks report `FAIL` since
`OnFixedUpdate` never steps `s_PhysicsWorld` yet — `Position.y` stays
`10.0`/`3.0` unchanged. Confirm by running once before Step 3:
```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/physicssim_before.png --capture-step 2
```

- [ ] **Step 3: Step the world and sync transforms**

In `OnFixedUpdate`, add right after the existing `if (!s_Playing) return;`
guard, before the script-ticking loops:

```cpp
		s_PhysicsWorld.Step(dt);

		for (auto& [id, handle] : s_PhysicsBodies)
		{
			if (!g_EditorScene.IsValid(id))
				continue;   // a script could destroy a physics entity mid-Play, same staleness possibility s_Prepared's own loop already guards against

			auto* transform = g_EditorScene.GetComponent<GS::TransformComponent>(id);
			if (!transform)
				continue;

			const GS::RigidBody3D& body = s_PhysicsWorld.GetBody(handle);
			transform->Position = body.Position;

			float x, y, z;
			glm::extractEulerAngleXYZ(glm::mat4_cast(body.Orientation), x, y, z);
			transform->Rotation = glm::degrees(glm::vec3(x, y, z));
		}
```

- [ ] **Step 4: Build and run the test**

```sh
./gs.py build
./gs.py run -- --hide-window --lockstep --capture /tmp/physicssim.png --capture-step 2
```
Expected: `PhysicsSimulationTest: 5 passed, 0 failed` (orientation round
trip, free-fall value, Stop revert, settled-height-stable, settled-above-floor).

- [ ] **Step 5: Build all three configs**

Run: `./gs.py build all`
Expected: clean build.

- [ ] **Step 6: Remove the temporary test**

Delete `TestEnv/src/PhysicsSimulationTest.h` and its two `TestApp.cpp`
lines.

- [ ] **Step 7: Visual capture**

A `Dynamic` box actually visibly falling and landing on a `Static` floor,
confirmed the same way the free-fall test's own scene is built (a floor
entity and a box entity, both given `PhysicsComponent`), but this time
watched rather than asserted on:

```sh
./gs.py run -- --hide-window --lockstep --capture /tmp/physicsvisual.png --capture-step 180
```
using a temporary hook in `TestApp.cpp` (after `PushLayer(new
EditorSceneView());`) that builds the same floor+box scene as the
resting-on-floor test and calls `PlayMode::Play();` once, so the capture
is taken mid-simulation. Expected: the box renders resting on top of the
floor, not sunk into it or still floating above it. Remove the hook after.

- [ ] **Step 8: Mark task done**

---

## Plan self-review notes

- **Spec coverage:** body construction (roles, box-from-mesh-bounds,
  `Scale*0.5` fallback, Static/Kinematic zero-mass, Friction/Restitution,
  orientation) — Task 1. Stepping, transform sync, orientation extraction,
  free-fall correctness, resting-on-static correctness, `Stop()` revert —
  Task 2. Every in-scope bullet from the spec has a task; every
  explicitly-deferred item (sphere/capsule shapes, driven Kinematic
  bodies, joints/compound colliders, runtime-spawned physics entities, 2D
  physics) has none, on purpose.
- **Type consistency checked:** `PlayMode::s_PhysicsWorld`/
  `s_PhysicsBodies` (Task 1) are the exact names and types Task 2's
  `OnFixedUpdate` code reads. The box-construction formula (mesh-bounds
  and `Scale*0.5` fallback) and the orientation construction/extraction
  formulas appear identically in both the implementation steps and their
  own tests — no drift between what's asserted and what's built.
