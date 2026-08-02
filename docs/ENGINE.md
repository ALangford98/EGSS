# How EGSS works

Orientation, not reference. The README covers build and rendering theory; this
is the shape of the thing — what calls what, and the handful of decisions that
explain the rest.

---

## Where your code goes

You write one class. Everything else already exists:

```cpp
class MyGame : public Egss::Layer
{
    void OnAttach() override            {}  // load textures, build state
    void OnFixedUpdate(Egss::Timestep step) override {}  // simulate
    void OnUpdate(Egss::Timestep ts) override {}         // draw
    void OnImGuiRender() override       {}  // debug panels
    void OnEvent(Egss::Event& e) override {} // one-shot input
};

class App : public Egss::Application
{
public:
    App() { PushLayer(new MyGame()); }
};

Egss::Application* Egss::CreateApplication() { return new App(); }
```

That last function is the only thing the engine requires of you. `TestEnv/src/TestApp.cpp`
is exactly this and nothing more.

---

## The call path

One frame, top to bottom:

```
main()                                    EntryPoint.h  (engine-owned)
  └─ CreateApplication()                  yours
  └─ Application::Run()
       ├─ frameTime = now - lastFrame     clamped to 0.25s
       ├─ accumulator += frameTime
       ├─ while accumulator >= fixedStep  ← 0, 1, or many times per frame
       │    ├─ for each layer: OnFixedUpdate(fixedStep)   ← simulation
       │    └─ accumulator -= fixedStep
       ├─ alpha = accumulator / fixedStep
       ├─ for each layer: OnUpdate(frameTime)             ← exactly once
       │    └─ Renderer2D::DrawQuad(...)  accumulates into a CPU buffer
       ├─ ImGuiLayer::Begin()             opens the dockspace
       ├─ for each layer: OnImGuiRender()
       ├─ ImGuiLayer::End()               uploads + draws ImGui's vertices
       └─ Window::OnUpdate()              poll events, swap buffers
```

`DrawQuad` does **not** talk to OpenGL. It appends four vertices to an array in
memory. Nothing reaches the driver until `EndScene`:

```
Renderer2D::EndScene()
  └─ Flush()                              Renderer2D.cpp:228
       ├─ QuadVertexBuffer->SetData(...)  one glBufferSubData for the whole batch
       ├─ bind each texture to its slot
       └─ RenderCommand::DrawIndexed(...) RenderCommand.h:33
            └─ OpenGLRendererAPI::DrawIndexed  OpenGLRendererAPI.cpp:56
                 └─ glDrawElements
```

So 400 `DrawQuad` calls produce **one** `glDrawElements`. That is the entire
point of the design, and it is why draw-call count tracks distinct *textures*,
not sprites.

---

## Five decisions that explain everything else

**1. Layers, not a monolithic update.** `Application` owns a `LayerStack`.
Updates run bottom-up (so later layers draw on top); events travel top-down and
stop at the first layer that marks them handled. That's why ImGui — pushed as an
*overlay*, above everything — can swallow a click before your game sees it.

**2. Events for edges, polling for state.** `OnEvent` fires once when something
changes ("key went down"). `Input::IsKeyPressed` asks what's true right now. Use
polling for movement, events for actions:

```cpp
if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))   // held → continuous
    x -= speed * ts;
```

**3. Simulation and presentation run at different rates.** `OnFixedUpdate` is
called with a step that never varies — zero, one, or several times a frame,
whatever it takes to keep up with real time. `OnUpdate` is called exactly once
with the real elapsed time. Physics goes in the first, drawing and camera feel
in the second.

The reason is that variable-length steps make a simulation depend on frame
rate: the same scene resolves differently at 60 and 144 fps, one long frame
lets a fast body pass straight through a wall, and nothing replays. A fixed
step removes all three.

The cost is that the simulation and the display drift out of phase, which shows
up as judder. `GetInterpolationAlpha()` is the fix — keep the previous state
and render between the two:

```cpp
float alpha = Application::Get().GetInterpolationAlpha();
glm::vec2 drawAt = glm::mix(m_PreviousPosition, m_Position, alpha);
```

`Timestep` itself is just seconds wrapped so the unit is explicit, and it
converts to float. `speed * ts` is the idiom; a bare `speed` is a bug.

**4. The renderer is split in three.** `Renderer2D` (batching, sprite logic) →
`RenderCommand` (a thin static façade) → `RendererAPI` (the virtual backend
interface, implemented by `OpenGLRendererAPI`). You call the first, never the
third. This is what makes a second backend possible without touching game code.

There are **two paths through it**, and they don't overlap:

| | `Renderer2D` | `Renderer::Submit` |
| --- | --- | --- |
| geometry | quads and lines | any vertex array |
| batching | thousands → 1 draw call per primitive type | one draw call each |
| use for | sprites, tiles, UI, debug lines | meshes, 3D |

`Renderer2D`'s batching is quad-specific and does not generalise to meshes, so
3D goes through `Submit`.

Lines are a second batch inside `Renderer2D` with their own buffer and shader.
They can't merge with the quads — a different primitive type needs a different
draw call — so anything drawn with `DrawLine` costs exactly one extra call, no
matter how many segments. They are drawn unindexed, via `glDrawArrays(GL_LINES)`.

`Renderer2D::BeginScene` takes any `Camera`, so debug lines work under a
perspective camera too. The "2D" is about the primitives, not the projection.

**5. Interfaces live in `Egss/`, implementations in `Platform/`.** `Texture2D::Create`
is declared in `Renderer/Texture.h` and *defined* in `Platform/OpenGL/OpenGLTexture.cpp`.
If you're looking for how something actually works, it's in `Platform/`. If
you're looking for what you can call, it's in `Egss/`.

---

## The whole API you'd use daily

```cpp
// Lifecycle — override what you need
OnAttach() / OnDetach() / OnImGuiRender() / OnEvent(Event&)
OnFixedUpdate(Timestep fixedStep)   // simulation, 0..n times a frame
OnUpdate(Timestep ts)               // presentation, exactly once a frame

// The loop
Application::Get().GetInterpolationAlpha();   // 0..1, for blending render state
Application::Get().GetFixedTimestep();        // seconds per simulation step
Application::Get().SetFixedTimestep(1/120.f);
Application::Get().GetFixedStepsLastFrame();  // >1 consistently = falling behind

// Drawing — always between BeginScene and EndScene
Renderer2D::BeginScene(camera);
Renderer2D::DrawQuad(pos, size, color);                    // vec2 or vec3 pos
Renderer2D::DrawQuad(pos, size, texture, tiling, tint);
Renderer2D::DrawQuad(pos, size, subTexture, tiling, tint); // sprite-sheet region
Renderer2D::DrawRotatedQuad(pos, size, degrees, ...);      // same four forms
Renderer2D::EndScene();

// Lines — their own batch, one extra draw call however many segments
Renderer2D::DrawLine(from, to, color);              // vec2 or vec3
Renderer2D::DrawRect(centre, size, color);          // outline, DrawQuad's convention
Renderer2D::DrawRect(transform, color);             // rotated / scaled
Renderer2D::SetLineWidth(2.0f);                     // >1 ignored by most drivers

// Filled triangles — a third batch, for generated geometry
Renderer2D::DrawTriangle(a, b, c, color);
Renderer2D::DrawTriangle(a, b, c, colA, colB, colC);  // per-corner: free gradients
Renderer2D::DrawCircle(centre, radius, color, segments);   // a fan; same batch

// Global render state. Blending and depth are NOT per-draw: anything already
// batched must be flushed (EndScene) before changing them, or it is drawn with
// whatever is set at flush time rather than what was set when it was submitted.
RenderCommand::SetBlendMode(BlendMode::Additive);   // Alpha, Additive, None
RenderCommand::SetDepthTest(false);                 // additive overlays must not
                                                    // depth-test against each other
RenderCommand::SetBackfaceCulling(true);            // 3D meshes only; turn it off
                                                    // again for open geometry
                                                    // like debug lines

// Screen
RenderCommand::SetClearColor({r,g,b,a});
RenderCommand::Clear();

// Input
Input::IsKeyPressed(EGSS_KEY_SPACE);
Input::IsMouseButtonPressed(EGSS_MOUSE_BUTTON_LEFT);
Input::GetMousePosition();          // {x, y}, window coordinates

// Profiling — compiled out in Dist
EGSS_PROFILE_SCOPE("MyThing");                 // RAII, times the enclosing scope
EGSS_PROFILE_FUNCTION();                       // same, named after the function
EGSS_PROFILE_BEGIN_SESSION("run", "trace.json");  // Chrome trace capture
EGSS_PROFILE_END_SESSION();
Instrumentor::GetLastFrame();                  // live per-scope totals

// Audio — Init/Shutdown are handled by Application
auto clip = AudioClip::Create("hit.wav");            // or CreateFromSamples
AudioEngine::Play(clip, { volume, pitch, pan, loop });
AudioEngine::SetMasterVolume(0.8f);
AudioEngine::IsAvailable();                          // false = no device, still safe

// Positional — gain, pan and Doppler recomputed from the listener every block
AudioEngine::SetListener({ cam.GetPosition(), cam.GetForward(), cam.GetUp(), velocity });
VoiceHandle v = AudioEngine::PlayAt(clip, { position, velocity, volume, pitch, loop,
                                            minDistance, maxDistance, dopplerFactor });
AudioEngine::SetVoicePosition(v, newPosition, newVelocity);   // ignored if stale
AudioEngine::Stop(v);

// Occlusion — you raycast, the engine muffles. 0 clear, 1 fully blocked.
auto hit = world.Raycast(listenerPos, toSource, distance);
AudioEngine::SetVoiceOcclusion(v, hit.Hit ? 1.0f : 0.0f);
VoiceDebug d; AudioEngine::GetVoiceDebug(v, d);   // Distance/Gain/Pan/Occlusion

// Reverb — a "zone" is just a region you test the listener against;
// the engine crossfades between whatever settings you hand it.
AudioEngine::SetReverb({ wet, roomSize, damping, width });

// Early reflections — delayed, quietened, panned copies behind the direct
// sound. Three numbers each; where they came from is not the mixer's business.
AudioReflection taps[] = { { 0.021f, 0.4f, -0.6f }, { 0.037f, 0.25f, 0.3f } };
AudioEngine::SetVoiceReflections(v, taps, 2);

// Convolution tail — the room's own decay instead of filters shaped to
// resemble one. Sparse: a few hundred impulses, not a dense recording.
auto impulse = Acoustics2D::BuildImpulseTaps(traced);
AudioEngine::SetReverbImpulse(impulse.data(), (unsigned int)impulse.size());
AudioEngine::ClearReverbImpulse();     // back to the parametric reverb

// Acoustics — where those three numbers can come from. Rays leave the source,
// bounce off the physics world losing energy, and report what reached the ear.
AcousticsSettings settings;
settings.RayCount = 192;
settings.Absorption = 0.15f;                    // energy lost per bounce
settings.PerBodyAbsorption = &absorptionByBody; // optional, indexed by handle

AcousticsResult r = Acoustics2D::Trace(world, sourcePos, listenerPos, settings);

r.Occlusion;          // graded, from five probes across the listener
r.Reflections;        // delay, gain, arrival direction, bounce count
r.ReverbTime;         // RT60 -- check ReverbTimeMeasured before trusting it
r.EffectiveRadius;    // how far the sound actually carries
r.Echogram;           // energy per 5 ms bin -- a coarse impulse response,
                      // which is what BuildImpulseTaps convolves with

// Physics — a standalone world; step it from OnFixedUpdate
PhysicsWorld2D world;
world.Gravity = { 0.0f, -9.81f };
auto handle = world.AddBody(RigidBody2D::MakeCircle(pos, radius, mass));
world.AddBody(RigidBody2D::MakeStaticBox(pos, halfExtents));
world.Step(fixedStep);
const RigidBody2D& body = world.GetBody(handle);   // Position, Velocity, Awake

RaycastHit hit = world.Raycast(origin, direction, maxDistance);  // dir auto-normalised
world.ResolveCircle(position, radius);        // push a circle out of geometry --
                                              // character controllers, cameras
world.UseBroadphase = true;   // uniform grid; false for brute force, to compare
world.CellSize = 0.25f;       // roughly the size of a typical body
if (hit.Hit) { hit.Point; hit.Normal; hit.Distance; hit.Fraction; }

// Assets
Texture2D::Create("path.png");      // or Create(width, height) + SetData
SubTexture2D::CreateFromCoords(atlas, cell, cellSize, spriteSize);

// Cameras — both derive from Camera, which is all Renderer::BeginScene wants
OrthographicCamera cam2d(-aspect, aspect, -1, 1);
cam2d.SetPosition({x, y, 0});

PerspectiveCamera cam3d(45.0f, aspect, 0.1f, 100.0f);
cam3d.SetPosition({x, y, z});
cam3d.SetRotation(yawDegrees, pitchDegrees);   // pitch clamped to ±89
cam3d.GetForward(); cam3d.GetRight();          // for movement

// Meshes — a vertex array plus the numbers describing it
std::shared_ptr<Mesh> mesh(Mesh::CreateCube(1.0f));   // or CreateSphere / CreatePlane
std::shared_ptr<Mesh> model(Mesh::Load("assets/models/torus.obj"));  // nullptr on failure

mesh->GetVertexCount(); mesh->GetTriangleCount();
mesh->GetBoundsCentre(); mesh->GetBoundsRadius();     // for framing a camera

Renderer::BeginScene(camera);
shader->Bind();
shader->SetFloat3("u_LightPosition", pos);     // your own uniforms first
Renderer::Submit(shader, mesh, transform);     // sets u_ViewProjection + u_Transform
Renderer::EndScene();
```

A `Mesh` is geometry and nothing else — no material, no transform, no
hierarchy. That is what lets one mesh be drawn a thousand times without being
copied once; the transform belongs to whatever is *using* it.

`Mesh::Load` reads Wavefront `.obj`: `v` / `vt` / `vn` / `f` / `s`, faces of any
size, negative indices, and files missing texture coordinates or normals.

Normals are generated when absent, honouring the smoothing flag: `s off` (the
default) gives each face its own normal, `s 1` averages them across shared
corners. That is why a positions-only cube loads as 24 vertices, not 8 — flat
shading needs corners *unshared* so each can hold its face's normal. Normals the
file did supply are never overwritten. Materials and free-form surfaces are
skipped. It returns `nullptr` and logs on failure rather
than throwing — a missing model should not take the program with it.

To parse without touching the GPU (or the filesystem), `ObjLoader::Parse` fills
a `MeshData` — plain vectors of `MeshVertex` and indices. No GL context needed,
which is what makes geometry testable by looking at numbers.

Assets are loaded by **path relative to the executable**. premake copies
`TestEnv/assets` next to the binary after every link, so `"assets/models/x.obj"`
works from the output directory.

`Submit` binds the shader itself, but uniform values belong to the program and
survive a rebind, so setting your own before the call is fine.

### Acoustics, briefly

`Acoustics2D::Trace` is stochastic ray tracing: rays leave the source, reflect
specularly, lose a fraction of their energy at every surface, and at each bounce
ask whether the listener is visible from there. Every yes is one path sound
could take — its length gives a delay, its surviving energy a gain, its last leg
a direction. Binned by arrival time, they are the room's impulse response.

It costs milliseconds, not microseconds, and geometry changes slowly. Trace when
something has moved enough to matter, not every frame, and never on the audio
thread.

Two things are worth knowing before you trust a number it gives you:

- **`ReverbTime` is only measured if the trace ran long enough.** Fitting a
  decay needs the tail to reach 25 dB down inside the traced span; past that
  the bins are zero because tracing stopped, not because the room went quiet,
  and backward integration turns that cliff into a convincing wrong answer.
  When it cannot measure, it says so via `ReverbTimeMeasured` and falls back to
  `SabineTime` — the formula fed the geometry the trace did measure.
- **Spreading applies to the last leg, not the whole path.** A ray's energy
  packet does not shrink as it travels; what varies is how much of what a
  surface re-radiates the listener catches, which depends only on how far away
  that surface is. Applying `1/d²` over the total path counts distance twice
  and halves every RT60 you measure.

Positions are **world units**, not pixels. The camera decides how many units fit
on screen, so `{0.5f, 0.5f}` is half a unit wide regardless of resolution.

---

## Entities, if you want them

Nothing above needs a `Scene`. Breakout tracks its bricks in a `std::vector` and
that is the right call for Breakout. Reach for `Scene` when *several* systems
need to agree on the same object -- when the renderer, physics and a light all
have to mean the same brick.

```cpp
Scene scene;

Entity box = scene.CreateEntity("Box 1");        // Tag + Transform come free
box.Get<TransformComponent>()->Position = { 0.0f, 2.0f, 0.0f };
box.Add<SpriteComponent>({ { 1, 0, 0, 1 } });
box.Add<LightComponent>({ colour, radius, true });

// A component is any struct. There is nothing to register.
struct Health { int Current = 100; };
box.Add<Health>();
```

A "system" is a loop you write. Ask for a store and walk it:

```cpp
auto& sprites = scene.View<SpriteComponent>();
for (size_t i = 0; i < sprites.Size(); i++)
{
    EntityId owner = sprites.Owner(i);
    auto* transform = scene.GetComponent<TransformComponent>(owner);
    Renderer2D::DrawQuad(transform->Position, glm::vec2(transform->Scale),
        sprites.Components()[i].Color,
        (int)EntityIds::Index(owner));     // last arg = picking
}
```

Note what that loop does *not* need to know: whether the entity also has a
body, a light, or a `Health`. Adding a component never touches the systems that
do not care about it -- which is the entire point.

For physics, add a `RigidBody2DComponent` holding the body index and call
`scene.StepPhysics(fixedStep)` from `OnFixedUpdate`. It pushes transform-driven
bodies in, steps, and reads physics-driven bodies back out.

**Handles are generational.** `DestroyEntity` bumps the slot's generation, so
every stored handle to it goes stale rather than quietly pointing at whatever
entity reuses the slot. Check `IsValid` (or `if (entity)`) after anything that
could have destroyed it; `GetComponent` on a stale handle returns `nullptr`
rather than lying.

### Picking

Pass an entity's *slot index* as the last argument to `DrawQuad`, render into a
framebuffer with a `RED_INTEGER` attachment, and read one pixel back:

```cpp
framebuffer->Bind();
RenderCommand::Clear();
framebuffer->ClearAttachment(1, -1);     // glClear only carries a float colour
// ... draw the scene ...
Renderer2D::EndScene();                  // the batch must be flushed first

int slot = framebuffer->ReadPixel(1, mouseX, height - mouseY);   // GL y is up
EntityId hovered = scene.EntityAtIndex((unsigned int)slot);      // InvalidEntity if empty
framebuffer->Unbind();
```

Pixel-exact and free of geometry maths: whatever the renderer drew there is
what gets picked, including rotated and overlapping shapes a bounding-box test
would get wrong. Store the **slot**, not the `EntityId` -- the attachment is a
*signed* integer texture, and a handle whose generation passes 2047 exceeds
`INT_MAX` and reads back negative.

To show the result, wrap the colour attachment and draw it as one quad:

```cpp
auto tex = std::shared_ptr<Texture2D>(Texture2D::CreateFromHandle(
    framebuffer->GetColorAttachmentRendererID(0), width, height));
```

Rebuild that wrapper whenever the framebuffer resizes -- a resize makes new
textures and the old handle is gone. It does not own the handle, so it must not
outlive the framebuffer.

`SceneDemo.h` is all of the above in one file.

---

## Gotchas that will actually bite you

- **New `.cpp` file? Re-run `./BuildProject.sh`** (or `./egss.py build`, which
  always regenerates). premake expands file globs at *generation* time, so
  your file will compile in the editor and never link.
- **Depth testing is on, and `z` decides what covers what — not draw order.**
  Higher `z` is nearer. At *equal* `z` the depth test (`GL_LESS`) rejects the
  later fragment, so the **first** thing submitted wins, which is the opposite
  of the painter's-algorithm behaviour you might expect. Give overlapping
  sprites different `z` values; don't rely on ordering.

  Measured: two overlapping quads at `z = 0` — the one drawn first stays whole
  and the second is clipped. Raise the second to `z = 0.2` and it covers the
  first even when drawn earlier.
- **`EndScene` is not optional.** Forget it and the last batch never flushes —
  you get a blank screen with no error.
- **Asserts only exist in Debug.** `EGSS_ENABLE_ASSERTS` is only defined there.
- **Every layer sees the same event.** If two layers act on one key press they
  will fight. Return `true` from a dispatcher lambda to mark the event handled;
  `Application` then stops walking the stack. Returning `false` means "I looked,
  let it through".
- **ImGui takes some keys before you do.** `Tab` cycles widget focus and turns
  sliders into text fields, which then sets `io.WantCaptureKeyboard` and
  swallows everything. Pick function keys for global shortcuts.
- **`glClear` will not clear an integer attachment.** It only carries a float
  colour. Use `Framebuffer::ClearAttachment(index, -1)`, or picking reads
  whatever the previous frame left there.
- **`ReadPixel` after `EndScene`, before `Unbind`.** The batch has to have
  reached the driver, and the framebuffer has to still be bound.
- **Every shader used in the pass must write *every* attachment.** With two
  draw buffers bound, a fragment output a shader never assigns is *undefined*,
  not left alone — so a shader that only writes colour scribbles noise into the
  picking buffer. Write `-1` for things that should not be pickable.
- **Window y counts down, GL y counts up.** Flip with `height - mouseY` before
  reading a pixel, or picking works perfectly, upside down.
