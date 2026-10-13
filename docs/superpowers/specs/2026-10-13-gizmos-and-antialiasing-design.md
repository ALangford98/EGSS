# Rotate/scale gizmos and anti-aliasing

**Status:** built on 2026-10-13 after the owner asked to "plan and build".
The decisions below were made during that work and are recorded here for
review.

## Gizmo modes

- **Three modes: Move, Rotate, Scale.**
  - **Switching:** 1, 2 and 3 select a mode, and T cycles through them.
    The mode also shows beside Play in the menu bar.
  - **Not W/E/R, the usual editor keys:** WASD and Q/E fly the camera.
  - **Where the keys are ignored:** while typing in a field, while Ctrl is
    held, and during a drag.
- **Rotate:**
  - A ring per world axis. The angle comes from where the cursor meets the
    ring's plane, summed frame by frame so a drag can go round more than
    once.
  - When a ring is seen edge-on, sideways mouse movement turns it instead.
  - The turn is composed onto the existing rotation as a world-axis
    rotation. It is written back as the X-then-Y-then-Z Euler angles
    `TransformComponent` stores, picking whichever of the two equivalent
    angle sets is nearest the start, so the Inspector's numbers don't jump.
- **Scale:**
  - One handle per *local* axis, which is the direction scale actually
    stretches, since scale applies before rotation.
  - A centre handle scales uniformly: right or up grows, left or down
    shrinks.
  - Scale never goes below 1%.
- **Ctrl snaps** rotation to 15° and scale to steps of 0.1.
- **Groups:**
  - Rotate turns a group about its centre: each member's orientation turns
    and its position orbits.
  - Scale scales each member in place, along its own axes. Scaling the
    gaps between members was left out.
- **Mesh-edit sessions** move single points, so they stay Move-only.
- **Undo and feedback:** one drag is one undo step
  (`SetTransformsCommand`), and a tooltip shows the value while dragging.
- **Two fixes to the existing gizmo:**
  - **Constant on-screen size:** the gizmo's length is 0.15 × its distance
    to the camera. It used to be a fixed metre.
  - **Drawn on top, without depth testing.** At an object's centre, any
    object bigger than the handles hid it completely.
- **The math lives in `TestEnv/src/GizmoMath.h`**, separate from mouse
  handling.

## Anti-aliasing

- **The gap:** the demos draw into the window, which has 4× MSAA. The
  editor's scene view and GSPlayer drew into an offscreen framebuffer with
  none.
- **Framebuffers now take a sample count** (`Samples`, `SetSamples`).
  - Drawing goes into multisampled renderbuffers, which are resolved into
    the attachment textures on `Unbind` and before any pixel read.
  - The integer entity-id attachment is resolved by taking one sample per
    pixel; an average of two ids would be meaningless.
  - Requests are clamped to what the driver supports. If a multisampled
    framebuffer is incomplete, drawing falls back to no anti-aliasing.
- **It is a project setting:** manifest `antialiasing N`, default 4, and
  written only when it isn't the default.
  - It's set under View > Anti-aliasing, and the player reads the same
    value, so the editor shows what the game will look like.
  - `--antialiasing N` overrides it for one run, in the editor or the
    player. Regression captures use `--antialiasing 1`.
- **GLES can't multisample integer formats.** GSPlayer therefore drops the
  entity-id attachment it never read, so the web, Android and iOS players
  keep their anti-aliasing.

## Verification

- **Gizmo math**, by a temporary self-test against expectations that don't
  come from the code: the right-hand rule, composed rotations, a group
  orbiting its pivot, Euler round-trips and matrix equality including
  gimbal lock, a full 360° turn with no jumps, and undo/redo.
  - The test found that `asin` loses precision at ±90° of pitch, so pitch
    is now computed with `atan2`.
- **Captures of all three modes** on a rotated box, looked at by eye.
- **Anti-aliasing:**
  - **Off reproduces the old output exactly:** with `--antialiasing 1`,
    the editor, material and Breakout captures are byte-identical to
    before, and so is the web build's.
  - **4× changes only edges:** every pixel that changed is on an edge of
    the unaliased frame, and no interior pixel changed.
  - **Editor and player still agree:** editor Play and GSPlayer are
    byte-identical to each other at 4×.
  - **The web build at 4×** is anti-aliased, and differs from the editor
    by at most 1/255 on 12 edge pixels, because the browser's resolve
    rounds slightly differently.
  - **Picking survives multisampling:** a temporary test read entity ids
    back correctly through the multisampled resolve, both while the
    framebuffer was bound and after.
