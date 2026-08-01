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

// Debug geometry — same batch, one extra draw call however many segments
Renderer2D::DrawLine(from, to, color);              // vec2 or vec3
Renderer2D::DrawRect(centre, size, color);          // outline, DrawQuad's convention
Renderer2D::DrawRect(transform, color);             // rotated / scaled
Renderer2D::SetLineWidth(2.0f);                     // >1 ignored by most drivers

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

// Physics — a standalone world; step it from OnFixedUpdate
PhysicsWorld2D world;
world.Gravity = { 0.0f, -9.81f };
auto handle = world.AddBody(RigidBody2D::MakeCircle(pos, radius, mass));
world.AddBody(RigidBody2D::MakeStaticBox(pos, halfExtents));
world.Step(fixedStep);
const RigidBody2D& body = world.GetBody(handle);   // Position, Velocity, Awake

RaycastHit hit = world.Raycast(origin, direction, maxDistance);  // dir auto-normalised
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

// Meshes — build a vertex array, then submit it with a transform
Renderer::BeginScene(camera);
shader->Bind();
shader->SetFloat3("u_LightDirection", dir);    // your own uniforms first
Renderer::Submit(shader, vertexArray, transform);  // sets u_ViewProjection + u_Transform
Renderer::EndScene();
```

`Submit` binds the shader itself, but uniform values belong to the program and
survive a rebind, so setting your own before the call is fine.

Positions are **world units**, not pixels. The camera decides how many units fit
on screen, so `{0.5f, 0.5f}` is half a unit wide regardless of resolution.

---

## Gotchas that will actually bite you

- **New `.cpp` file? Re-run `./BuildProject.sh`.** premake expands file globs at
  *generation* time. Your file will compile in the editor and never link.
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
