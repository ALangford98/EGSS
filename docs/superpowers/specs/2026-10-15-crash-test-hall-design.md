# Crash test hall: walk-up interaction, a cannon HUD, game UI, and a look worth showing

Date: 2026-10-15. Builds on the crash test scene of the same day
(`TestEnv/src/CrashTest.h`, `Runtime/Launcher.h`, `Runtime/GameConsole.h`).

## What the owner asked for

- Fly the camera around the scene. Walk up to the ragdoll and an on-screen
  prompt offers to take control of it; then walk it around.
- The terminal becomes a **physical computer attached to the cannon**, which
  also prompts when approached.
- Controlling the cannon uses a HUD: a crosshair, a trajectory line, a list of
  objects to scroll through, sliders for size and velocity.
- High-fidelity models for the ragdoll, the computer and the cannon; better
  prefab models, HUD, lighting and environment generally.
- Tools to build in-game UIs, scoped and built as part of this demo.

Decided with the owner (2026-10-15):

- Models are **procedural, in code**. Nothing downloaded.
- UI tools are a **runtime widget kit plus `.gsui` layout files** the game
  loads, which scripts can drive. A visual UI editor goes on the roadmap as a
  stretch goal and is not built now.
- The rendering upgrade is **opt-in per scene**, through an Environment
  component. A scene without one renders byte-identically to today.

Success is the owner opening File > Add Demo Scene > Crash Test, pressing Play,
and getting a good-looking hall to fly around. Everything else in this spec
works from inside the game, with no editor panel touched.

## 1. Interaction (Runtime)

**Fly camera.** `FlyCameraComponent { Speed, Sensitivity }` on a camera entity.
While nothing is controlled, GameSession flies that camera:

- WASD to move, Space and Ctrl for up and down, Shift for faster.
- The mouse looks around while the right button is held. The cursor is never
  captured, because a captured cursor in the editor's Play would steal the
  editor.
- Movement runs on the fixed step from `GS::Input`, so it records and replays
  like any other input.

**Interactables.** `InteractableComponent { Prompt, Action, Range }`. Each step
the session finds the nearest one within `Range` of the viewer that is also
within 30° of where the viewer is looking. The viewer is the fly camera, or
the controlled body. The prompt is drawn at the object's projected position:
"[E] Take control", "[E] Use terminal", "[E] Operate cannon". E runs its
`Action`, which is a console command (`possess Dummy`, `terminal`, `operate`).
So the scene file says what pressing E does, in the same language the
terminal speaks, and needs no code.

**Possession.** `CharacterController::Possess(entity)` and `Release()`:

- A humanoid NPC becomes the controlled character. It takes player input, and
  its third-person camera is the fly camera's entity.
- E or Esc lets go. The camera flies again from where it was, and the NPC goes
  back to its own behaviour.
- A downed dummy still gets up on its own, as now.

## 2. The computer and the cannon

**The computer** is part of the cannon model: a monitor on an arm, a keyboard
and a tower.

- Its screen shows the terminal **in the world**. The terminal is drawn by the
  UI kit (section 3) into a texture, and that texture lights the monitor's
  screen.
- E moves the camera smoothly to face the screen and gives it the keyboard.
  Esc steps back.
- Its scrollback stays visible on the monitor while you fly around.

**Operating the cannon** (E at the cannon, or `operate`):

- The camera sits behind and above the barrel. The mouse aims: yaw and pitch
  move the barrel's turntable and its elevation, within limits.
- The `LauncherComponent` gains a manual aim (`AimYaw`, `AimPitch`) that is
  used when no target is set or while it is being operated.
- **The trajectory line** is the ballistic path, with gravity, from the muzzle
  at the set speed. It is drawn as a 3D strip and cut at the first hit,
  found by ray-casting each segment. The crosshair sits where it lands.
- Left click fires; right click or Esc steps out.
- The HUD (a `.gsui` screen) holds:
  - the object list, scrolled with the mouse wheel or clicked;
  - sliders for size, mass, speed and spin, with fire and auto buttons;
  - a readout of momentum and energy.
- Every control submits a console command (`preset car`, `speed 22`). So the
  HUD, the terminal and a script all change the cannon in one way, on the
  fixed step.

## 3. Game UI kit (Runtime/GameUI.h)

The UI kit is a retained tree of widgets, laid out every frame and drawn
into an `ImDrawList`:

- **Screen:** `ImGui::GetForegroundDrawList()`.
- **Texture:** a private `ImDrawList` rendered into a `Framebuffer` by a small
  renderer of ImGui's own vertex format. This is the same font atlas and the
  same look, which is what puts the terminal on the monitor.

**Widgets:**

- Panel, Label, Button, Slider, List and TextInput (the terminal's input line).
- Bar (a meter), Crosshair, Prompt (a key cap plus text) and Image (a
  framebuffer's texture).

**Layout:**

- Anchors (nine points, plus stretch), offset and size.
- Padding, and vertical or horizontal stacks.
- Sizes in points, scaled by the viewport's height against a 1080-line
  reference, so a HUD is the same proportion in any window.

**Theme:** colours, rounding, border and font scale. It is part of the screen
file, with a default.

**Data flow:**

- A widget's text may contain `{name}`, filled from values the game publishes
  (`ui.Set("speed", 22)`).
- A slider binds a value name. Moving it publishes the value and submits its
  `command` with the value appended.
- A button submits its `command`; a list item submits `command` plus the item.

UI is drawn per frame and never changes the simulation itself: it only queues
commands, which run on the fixed step.

**Files.** `.gsui` is JSON, using the engine's own `GS::Json`. It holds a tree
of `{ "type", "id", "anchor", "pos", "size", "text", "command", "bind",
"children" }`. Screens are found under the project's assets. A screen
missing from the project falls back to one the runtime builds in, so the demo
works in a project that predates it.

**Scripts** (GSS/JS):

- `ui.show(name)` and `ui.hide(name)`
- `ui.set(name, value)`
- `ui.get(name)`

**Roadmap only:** the visual editor for `.gsui` files.

## 4. Rendering, opt-in (EnvironmentComponent)

A scene whose entity carries `EnvironmentComponent` is drawn with a second
shader path. Without one, today's shader runs unchanged, and the
byte-identical captures check that.

- **Sun:** a directional light with a shadow map.
  - One 2048² depth map, fitted orthographically around the scene's bounds,
    with 3×3 PCF and slope-scaled bias.
  - Point lights stay unshadowed fills.
- **Shading:** GGX microfacet specular, Schlick Fresnel and Lambert diffuse,
  per mesh with a `Roughness`, `Metallic` and `Emission` (`MeshComponent`
  fields, defaulted so old files load). Linked procedural materials keep
  feeding the albedo, normal and roughness maps.
- **Ambient:** a hemisphere (sky colour above, ground colour below), with a
  sky-gradient reflection term for metals.
- **Sky:** a gradient and a sun disc, drawn behind everything.
- **Fog:** exponential height fog.
- **Tone mapping:** exposure and the ACES fit, then sRGB encode, in the
  forward shader. There's no HDR target, so no bloom: it stays a stretch goal.
- The editor's Play, GSPlayer and the demo all go through `SceneRenderer`, so
  they stay identical to each other.

## 5. Procedural models (GS/Procedural/ModelBuilder)

**Mesh building:**

- Rounded (bevelled) boxes; lathe, from a 2D profile turned about an axis;
  tubes swept along a polyline.
- Discs, cylinders with chamfers, and torus segments.
- Flat or smooth normals by crease angle, box-projected UVs, and one submesh
  per material group.

Models are reachable through `MeshCache` as `model:<name>`, so a scene saves
and loads them by name like `primitive:cube`.

**Models:**

- **Crash-test dummy:** a part per ragdoll body, sized from that body's
  collider.
  - Head with a face plate and ear bolts; a neck of discs.
  - Chest with a rib plate and shoulder pucks; pelvis.
  - Segmented limbs with joint caps; mitten hands; booted feet.
  - Black-and-yellow calibration targets on the head, chest and hips.
  - Chosen per humanoid by `Look` (0 mannequin, 1 dummy), on the
    Player/NPC component. The mannequin stays the default.
- **Cannon:**
  - Base plate with bolts, a turntable, a yoke, and a lathe-turned barrel with
    reinforcing rings and a flared muzzle.
  - A breech, an air tank and hoses.
  - The computer arm, monitor, keyboard and tower.
  - The turntable and barrel are separate parts the launcher poses each step.
- **Hall props:**
  - A plank crate.
  - A padded impact wall and a crash barrier with stripes.
  - Light fixtures with emissive panels.
  - A wall panel and a ceiling truss.

## 6. The hall (CrashTestScene::Build)

**The room:**

- A closed hall about 30 × 40 m, with a polished concrete floor (a procedural
  material) and panelled walls.
- Ceiling trusses with fixtures, and skylights that let the sun's shadows in.
- The lane, painted with hazard stripes, and an impact wall with crates.

**What's in it:**

- The dummy, the cannon with its computer, and a sign rendered by the UI kit
  into a texture.
- **Cameras:**
  - **Fly:** active, the default.
  - The others from today: Side, Behind Launcher, Wide and Close.

## Verification

- **Opt-in rendering:**
  - Breakout and an old scene capture byte-identical before and after.
  - Editor Play and GSPlayer stay byte-identical on the hall.
- **Shadows:** a box's shadow edge lands where the arithmetic of sun
  direction and box height puts it, read with `ReadPixelRGBA`.
- **Trajectory:** where the drawn line is cut matches where the projectile is
  measured to land, within a substep's travel.
- **UI layout:** anchors and points scaling match hand-computed rectangles at
  two window sizes.
- **Interaction:** the prompt shows exactly inside range and the cone. A
  replayed recording of possess, walk, release and fire reproduces itself.
- **Models:** each dummy part's bounds match its collider within 10%.

## Build order

1. UI kit and `.gsui`, with the screen backend.
2. Fly camera, interactables, possession.
3. Cannon operation: manual aim, the trajectory line, the HUD screen.
4. The draw-to-texture backend; the terminal on the computer's monitor.
5. ModelBuilder and the models.
6. Environment rendering.
7. The hall, then the docs and the roadmap entry for the visual UI editor.
