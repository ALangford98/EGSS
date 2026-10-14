# Tools panel, characters and terrain

**Status:** built on 2026-10-14 after the owner asked to "plan and build".
The decisions below were made during that work and are recorded for review.

## Tools panel (`TestEnv/src/ToolsPanel.h`)

- **Sections:**
  - **Shapes:** cube, sphere, plane, cylinder, and a new capsule.
  - **Scene:** camera and light, which used to sit among the shapes.
  - **Characters:** player and NPC.
  - **Terrain:** preset, size, New Terrain, Apply to Selection.
  - **Import.**
- **Wrapping:** each section's buttons share one width and wrap at the
  panel's edge. They used to sit on one row, which scrolled sideways.
- **Placement:** new things go where the editor camera is looking (the
  y = 0 ground within 30 m, otherwise 6 m ahead), resting on the ground,
  rather than at the origin.

## Terrain (`TerrainComponent`, `GS/Procedural/TerrainMesh`)

- **Any primitive** can take terrain: plane, cube, sphere, cylinder or
  capsule. Loaded models can't, since their vertex count is the file's.
- **Settings:** resolution (the vertex count), height (in the shape's own
  units, before scale), feature scale, detail layers, roughness, seed, and
  a ridged option. The noise is a new 3D gradient noise (`Noise::Perlin3`
  and `Fbm3`), normalised so that height means height.
- **Displacement direction:**
  - A plane rises along +Y.
  - Other shapes push out from their centre, or from their axis for a
    cylinder or capsule.
  - This direction is the same for every copy of a duplicated vertex (cube
    edges, sphere seams), so the surface can't crack. Pushing along normals
    would split it.
  - Normals are averaged across those copies, so seams don't show in the
    shading.
- **Saving:** the scene file stores only the numbers, and every load
  rebuilds the mesh. The editor rebuilds whenever the numbers differ from
  what the mesh was built from, which catches edits and undo alike.
- **Collision:**
  - A static, unrotated plane with equal X and Z scale collides as a
    heightfield. The heightfield samples the same displacement at the
    mesh's own vertices, so the ground a character stands on is the ground
    that's drawn.
  - Any other terrain collides as its bounding box, and the Inspector
    says so.
- **New Terrain** makes a plane with uniform scale (so height is a fraction
  of the size), resolution 128, and a static physics body.
- **Presets:** hills, mountains, dunes, rough.
- **Edit Mesh and Export UV Template** on a terrain work on the generated
  surface, which bakes it into the `.obj`.

## Characters (`PlayerComponent`, `NpcComponent`, `Runtime/CharacterController.h`)

- **They work everywhere a game runs:** editor Play, GSPlayer, and the web
  and mobile exports, because they run in GameSession.
- **Each character is an upright capsule.**
  - Its size comes from scale: the capsule mesh is 1 wide and 2 tall.
  - Its rotational inertia is removed, so nothing can tip it over.
  - Friction is zero and its horizontal velocity is *set* every step, so it
    stops the moment it's told to. Gravity, jumps, slopes and pushing boxes
    are left to the physics.
  - It's grounded when the physics world's ground probe finds a surface
    within 8 cm of its feet.
- **The player:**
  - **Controls:** WASD moves relative to the camera's facing, Shift
    sprints, and Space jumps on the press (holding it doesn't bounce).
    Jump speed is √(2·g·height).
  - **Camera:** third person (distance and height), first person, or
    leave the scene's camera alone. Right-drag or the arrow keys orbit.
    If the scene has no active camera, one is made when the game starts;
    editor Play removes it again on Stop.
  - **Multiple players:** only the first Player is driven by input.
- **NPCs** have four behaviours:
  - **Idle.**
  - **Wander:** random points within a radius of where it started, with
    pauses between.
  - **Follow:** approaches the player when within its notice radius, and
    stops at a set distance.
  - **Flee:** runs from the player while within its notice radius.
- **Determinism:** everything runs per fixed step from polled input. Mouse
  movement is the difference between steps, and each NPC draws from a
  hashed random stream of its own. Replays reproduce exactly.
- **Not built:** pointer or touch orbiting on mobile, camera collision with
  terrain, and pathfinding. For anything richer than these behaviours,
  attach a script.

## Editor plumbing

- **`EntitySnapshot`:** one list of components shared by the Place, Delete
  and Duplicate commands, where there were three copies kept in step.
- **`ReplaceEntityCommand`** applies a snapshot to an existing entity in
  place. It makes Add/Remove Terrain and Make Player/NPC undoable.

## Verification

- **A temporary self-test, 34 checks:**
  - **Geometry:** vertex and triangle counts against formulas, relief
    within the set height, no cracks, outward winding, determinism, `Fbm3`
    within [-1, 1], and the capsule's bounds.
  - **Player:** it settles 0.9 m up, walks 5.000 m in a second at 5 m/s,
    stops on release, and jumps 1.1599 m against 1.1596 predicted (1.2 m
    less the integrator's v₀·dt/2). It doesn't bounce on a held key. The
    follow camera ends up exactly 6 m from its target, facing it.
  - **On hills,** the feet rest within 1.2 cm of the displacement function.
  - **NPCs:** follow stops at 2 m; flee runs at 2.5 m/s until 10 m away;
    wander stays within its radius and walks the same route twice.
  - **Save/load** round-trips every field and rebuilds the mesh.
  - **The test found two real bugs:** the cylinder primitive was wound
    inward (a fix in its own commit), and the follow camera was being made
    inactive.
- **Byte-identical runs, anti-aliasing off:** a world with terrain, a
  planet, a player and an NPC matches between editor Play, GSPlayer and the
  browser at step 150.
- **Existing captures are unchanged.**
- **A finding about the capture method:** at 4× MSAA, a dense mesh doesn't
  render bit-for-bit the same from run to run on this driver, although its
  generated data hashes identically. The same applies to anything drawing
  into the window's 4× buffer; the Physics3D and Ragdoll demos show the
  same 1–65 pixel noise. Byte-identical captures of rich scenes need
  `--antialiasing 1`, and CLAUDE.md now says so.
