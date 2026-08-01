# EGSS

Every Game Starts Somewhere, Why not here?

A game engine built from scratch, following along with TheCherno's Hazel
series. `EGSS` is the engine itself, built as a shared library; `TestEnv` is a
sandbox application that links against it.

**Current state:** a window with an OpenGL 3.3 core context, a working event
system, a layer stack, polled input, frame timing, a renderer abstraction, a
**batched 2D quad renderer** with sprite-sheet support, framebuffers with
integer-attachment mouse picking, perspective and orthographic cameras, and
ImGui (docking) as a debug overlay. `TestEnv` holds two demos — a playable
Breakout and a lit 3D cube grid — with **F1** switching between them. See [Roadmap](#roadmap) for what's left.

> **New to the codebase?** Start with **[docs/ENGINE.md](docs/ENGINE.md)** — the
> frame's call path, the five decisions that explain the rest, and the whole
> API you'd use day to day. Then read `TestEnv/src/TestApp.cpp`, which is a
> commented worked example of building on it.

## Layout

| Path | What it is |
| --- | --- |
| `EGSS/src/Egss/` | Engine core — application loop, layers, events, input, logging |
| `EGSS/src/Egss/Renderer/` | Backend-agnostic renderer interfaces |
| `EGSS/src/Platform/` | Backends: `Windows/`, `Linux/`, and `OpenGL/` |
| `EGSS/vendor/` | GLFW, spdlog, glm, imgui (submodules); Glad and stb_image (checked in) |
| `TestEnv/src/` | Sandbox app that consumes the engine |
| `premake5.lua` | Build definition — the source of truth for both platforms |
| `.vscode/` | Editor tasks, IntelliSense config, and debug launch configs |

Project and solution files are generated from `premake5.lua`. Don't hand-edit
them; your changes will be overwritten on the next generation.

---

# Development environment

## Prerequisites

Clone with submodules, or pull them afterwards:

```sh
git submodule update --init --recursive
```

**Linux** — a C++17 compiler, `make`, and GLFW's X11 build dependencies. On
Fedora:

```sh
sudo dnf install gcc-c++ make libX11-devel libXcursor-devel libXrandr-devel \
                 libXinerama-devel libXi-devel mesa-libGL-devel
```

On Debian/Ubuntu the equivalents are `build-essential libx11-dev
libxcursor-dev libxrandr-dev libxinerama-dev libxi-dev libgl1-mesa-dev`.

GLFW is built with the X11 backend (`_GLFW_X11`), so under a Wayland session it
runs through XWayland. That works transparently; there is no Wayland-native
path yet.

**Windows** — Visual Studio with the C++ desktop workload.

A `premake5` binary is expected at `vendor/bin/premake/` (`premake5` on Linux,
`premake5.exe` on Windows). It is gitignored, so grab it from
[premake.github.io](https://premake.github.io/download) if it's missing.

## The build pipeline

`premake5.lua` is the only build file you edit. Everything else is generated:

```
premake5.lua  ──[ BuildProject.sh ]──>  Makefile, EGSS/Makefile, TestEnv/Makefile
              ──[ BuildProject.bat ]──> EGSS.sln, *.vcxproj
```

Five projects build in dependency order: **GLFW**, **Glad**, and **ImGui**
(static libs) → **EGSS** (shared lib) → **TestEnv** (executable). GLFW and Glad
carry their own `premake5.lua`; ImGui ships none, so EGSS supplies one at
`EGSS/vendor/imgui_premake5.lua` — deliberately outside the submodule, since a
file added inside it would be lost on re-clone and would leave the submodule
permanently dirty. All three are pulled in by `include` directives at the top
of the root script.

**Linux:**

```sh
./BuildProject.sh          # generate makefiles
make config=debug          # or: config=release, config=dist
make -j$(nproc) config=debug   # parallel build
make clean config=debug
```

**Windows:**

```
BuildProject.bat
```

then open `EGSS.sln` and build.

### When to regenerate

Premake expands the `files` globs **at generation time**, not build time. Run
`./BuildProject.sh` again whenever you:

- add, rename, delete, or move a source file
- change `premake5.lua` or a vendor `premake5.lua`
- pull changes that did either of the above

Forgetting this is the most common confusing failure: your new `.cpp` compiles
fine in the editor but never gets linked, and you get undefined-symbol errors.

### Build configurations

| Config | Defines | Symbols | Optimized |
| --- | --- | --- | --- |
| `debug` | `EGSS_DEBUG`, `EGSS_ENABLE_ASSERTS` | yes | no |
| `release` | `EGSS_RELEASE` | yes | `On` |
| `dist` | `EGSS_DIST` | no | `Full` |

`debug` is the only config with asserts compiled in, and the only one that
requests an OpenGL debug context.

Platform defines are `EGSS_PLATFORM_LINUX` or `EGSS_PLATFORM_WINDOWS`, set by
the `system:` filters. `Core.h` `#error`s on any other platform.

### Output layout

```
bin/<Config>-<system>-x86_64/
  ├── EGSS/       libEGSS.so
  └── TestEnv/    TestEnv + a copy of libEGSS.so
bin-int/          object files and precompiled headers
```

Vendor static libraries build into their own directories rather than the
top-level `bin/`, because each vendor `premake5.lua` resolves `targetdir`
relative to itself — for example
`EGSS/vendor/glfw/bin/<Config>-<system>-x86_64/GLFW/libGLFW.a`. They are linked
into `libEGSS.so`, so nothing needs to ship them.

The engine library is copied next to the executable by a post-build step. On
Linux `TestEnv` is also linked with an `$ORIGIN` rpath, so it resolves
`libEGSS.so` from its own directory rather than the system path — you can move
the folder anywhere and it still runs.

## Running

```sh
./bin/Debug-linux-x86_64/TestEnv/TestEnv
```

On Windows, `bin\Debug-windows-x86_64\TestEnv\TestEnv.exe`.

You should see a 1280x720 window running one of the two demos, plus:

```
[22:00:24] EGSS: Creating Window Every Game Starts Somewhere (1280, 720)
[22:00:24] EGSS: OpenGL 4.6 (Core Profile) Mesa 26.1.5 | Mesa Intel(R) Iris(R) Xe Graphics (RPL-U)
[22:00:24] EGSS: Renderer2D initialized (10000 quads/batch, 16 texture slots)
[22:00:24] EGSS: ImGui 1.92.9b initialized
```

The **Demos** panel switches between them; **F1** cycles.

- **Breakout** — Left/Right or A/D to move, Space to launch, P pause, R restart.
  Its panel shows simulation steps per frame and the interpolation alpha, with
  a slider for the simulation rate. Drop it to 10 Hz: the physics coarsens but
  the ball keeps moving smoothly, because rendering interpolates between steps.
- **Cube3D** — WASD to move, Q/E up and down, arrows to look, Space to pause the
  spin. Its grid slider shows that meshes cost one draw call each, unlike the
  2D batcher.

Closing the window exits cleanly.

## VS Code

Nothing about this project requires a heavyweight IDE — premake generates plain
makefiles, and GCC plus GDB cover build and debug.

Recommended extensions: **C/C++** (`ms-vscode.cpptools`) for IntelliSense and
debugging, and optionally **Shader languages support** for GLSL highlighting.

Working configuration is committed in `.vscode/`, so opening the folder should
give you build, IntelliSense, and debugging without further setup:

| File | Provides |
| --- | --- |
| `c_cpp_properties.json` | Include paths, defines, C++17 mode |
| `tasks.json` | Build, regenerate, clean, and compile-database tasks |
| `launch.json` | GDB launch configs for the debug and release builds |

### Tasks

`Ctrl+Shift+B` runs **build debug**, the default build task. The rest are under
*Terminal → Run Task*:

| Task | What it does |
| --- | --- |
| `build debug` | `make config=debug -j$(nproc)` |
| `build release` | `make config=release -j$(nproc)` |
| `regenerate makefiles` | `./BuildProject.sh` — run after adding or renaming files |
| `clean` | `make clean config=debug` |
| `generate compile_commands.json` | `bear -- make config=debug`, for better IntelliSense |

All of them use `$gcc` as the problem matcher, which turns compiler errors into
clickable entries in the Problems panel.

### IntelliSense

`c_cpp_properties.json` ships an explicit `includePath` and `defines` list
mirroring `premake5.lua`, so IntelliSense works immediately. That list has to be
maintained by hand, though, and will drift as the build changes.

The better setup is a compile-commands database, which gives the extension the
exact flags for every file. Premake's `gmake` action doesn't emit one, so
generate it with `bear`:

```sh
sudo dnf install bear         # or: apt install bear
```

then run the **generate compile_commands.json** task. The config already points
at `${workspaceFolder}/compile_commands.json` and prefers it whenever the file
exists, silently falling back to the hardcoded paths when it doesn't. The
generated database is gitignored; regenerate it after adding source files.

### Debugging

**Debug TestEnv** in the Run panel (`F5`) builds first, then launches under
GDB. There's a matching config for the release build.

Both set `cwd` to the executable's own directory — that's how the `$ORIGIN`
rpath resolves `libEGSS.so`. Changing it will break the launch with a
library-not-found error.

Breakpoints inside `libEGSS.so` work normally; GDB resolves them once the
shared library loads. If a breakpoint stays hollow, confirm you built the
`debug` config.

For GL problems specifically, a stepping debugger is often the wrong tool —
see [Debugging rendering](#debugging-rendering) below.

## Adding source files

Engine code goes under `EGSS/src/Egss/`; platform-specific code goes under
`EGSS/src/Platform/<Platform>/`. The per-platform directories are mutually
exclusive at build time — each `system:` filter `removefiles` the other's
subtree — so a Linux backend never has to `#ifdef` around Windows code.

Anything you add to the engine's public surface needs `EGSS_API` on the class
so it's exported from the shared library:

```cpp
class EGSS_API Thing { ... };
```

Then regenerate. New engine headers should be reachable from `EGSS/src`, which
is on both projects' include path.

The engine uses a precompiled header, `egsspch.h`, holding the common standard
library includes. Every engine `.cpp` must include it first. `TestEnv` does not
use a PCH.

---

# Rendering

This section documents how the current triangle actually gets on screen. It's
deliberately detailed, because most of it is scaffolding that later work will
build on rather than replace.

## Why a function loader is needed

OpenGL is not a normal library you link against. The system's GL library
exposes only a very old baseline — on Windows, `opengl32.lib` stops at OpenGL
1.1 from 1997 — and everything since is reached through function pointers you
query from the driver at runtime.

**Glad** is a generated file that declares every GL 3.3 core function as a
function pointer and fills them in from a loader callback. `EGSS/vendor/Glad/`
holds a loader generated for exactly `gl=3.3, profile=core`. Regenerate with:

```sh
pip install glad
python -m glad --profile core --api gl=3.3 --generator c --out-path <dir>
```

It's checked into the repo rather than added as a submodule because
TheCherno's `Glad` repo no longer exists.

Two rules follow from this:

- **Glad's header must be included before GLFW's.** Both would otherwise
  declare GL symbols. `GLFW_INCLUDE_NONE` is defined project-wide in
  `premake5.lua` to stop GLFW pulling in its own GL headers, which makes the
  ordering safe rather than merely conventional.
- **No GL function may be called before the loader runs.** Calling one earlier
  dereferences a null pointer, and the crash gives no hint about the cause.

## Context creation

In `LinuxWindow::Init` (and its Windows counterpart), in order:

```cpp
glfwInit();                                          // once per process

glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);       // request 3.3 core
glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

m_Window = glfwCreateWindow(...);
glfwMakeContextCurrent(m_Window);                    // context → this thread

gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);  // now GL is callable
```

The hints must be set *before* `glfwCreateWindow`; they configure the context
that call creates. `glfwMakeContextCurrent` must come before
`gladLoadGLLoader`, because the loader queries the current context.

3.3 core is a floor, not a ceiling. The driver may hand back a newer context —
the log line above reports 4.6 — but the code only uses what Glad was
generated for.

**Core profile has no default vertex array object.** In a compatibility
profile, object 0 is a usable VAO, so tutorials that skip `glGenVertexArrays`
still work. In core profile the same code draws nothing and reports no error.
This is the single most common reason a first triangle comes out black.

## The triangle

This walkthrough describes drawing a single triangle by hand. That code no
longer lives in the engine — `Renderer2D` now wraps all of it behind
`DrawQuad`, and `TestEnv` never touches GL directly. It is kept because every
concept below still describes what `Renderer2D` does internally, one layer
down, and because reading it is the fastest way to understand what the batcher
is actually batching.

### Vertex array and vertex buffer

```cpp
glGenVertexArrays(1, &m_VertexArray);
glBindVertexArray(m_VertexArray);
```

A **vertex array object (VAO)** stores the *description* of your vertex data:
which attributes are enabled, their format, and which buffer each one reads
from. It is not storage. Binding one and then configuring attributes records
that configuration into it, so drawing later is a single bind rather than a
replay of every attribute call.

```cpp
float vertices[3 * 3] = {
    -0.5f, -0.5f, 0.0f,
     0.5f, -0.5f, 0.0f,
     0.0f,  0.5f, 0.0f
};

glGenBuffers(1, &m_VertexBuffer);
glBindBuffer(GL_ARRAY_BUFFER, m_VertexBuffer);
glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
```

A **vertex buffer object (VBO)** is the actual GPU-side storage. `glBufferData`
uploads the bytes; `GL_STATIC_DRAW` is a hint that the contents will be set
once and drawn many times.

Those coordinates are in **normalized device coordinates**: x, y, and z each
run from -1 to +1 across the visible volume, with (0, 0) at the center and +y
up. Nothing here transforms them, so the triangle is fixed in place — it
doesn't respond to a camera, and it stretches when the window's aspect ratio
changes. Cameras and transform matrices come later.

### Describing the layout

```cpp
glEnableVertexAttribArray(0);
glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
```

This says: attribute 0 is 3 floats, unnormalized, with a **stride** of 12 bytes
between consecutive vertices and an **offset** of 0 into the buffer. The
attribute index 0 matches `layout(location = 0)` in the vertex shader.

`glVertexAttribPointer` implicitly associates the attribute with whatever is
currently bound to `GL_ARRAY_BUFFER`. That implicitness is a frequent source of
bugs — the VBO must be bound at this moment, not just at draw time.

### Shaders

The GPU pipeline for this draw runs two programmable stages:

```
vertices ─> [ vertex shader ] ─> clipping, rasterization ─> [ fragment shader ] ─> framebuffer
              per vertex           (fixed function)            per pixel
```

The **vertex shader** runs once per vertex and must write `gl_Position`. The
**fragment shader** runs once per pixel covered by the triangle and writes a
color:

```glsl
// vertex
layout(location = 0) in vec3 a_Position;
out vec3 v_Position;
void main() {
    v_Position  = a_Position;
    gl_Position = vec4(a_Position, 1.0);
}
```

```glsl
// fragment
layout(location = 0) out vec4 color;
in vec3 v_Position;
void main() {
    color = vec4(v_Position * 0.5 + 0.5, 1.0);
}
```

The `out` in the vertex shader pairs by name with the `in` in the fragment
shader. Values are **interpolated across the triangle** between the two stages,
which is why the output is a gradient rather than three flat colors: each
fragment receives a blend of the three vertices' positions weighted by
distance. The `* 0.5 + 0.5` remaps the -1..1 position range into the 0..1 color
range.

Compilation follows the standard sequence — create, source, compile, then
create a program, attach, link:

```
glCreateShader → glShaderSource → glCompileShader  (per stage)
glCreateProgram → glAttachShader → glLinkProgram   (once)
glDetachShader → glDeleteShader                    (cleanup)
```

`CompileShader` in `Application.cpp` checks `GL_COMPILE_STATUS` and logs the
driver's message via `EGSS_CORE_ERROR` on failure; linking checks
`GL_LINK_STATUS` the same way. **Keep these checks.** GLSL failures are
otherwise completely silent — you get a black window and no diagnostic.

After linking, the program holds its own copy of each stage, so the individual
shader objects are detached and deleted.

## The frame loop

`Application::Run` does, every frame:

```cpp
glClearColor(0.1f, 0.1f, 0.1f, 1.0f);   // set the clear color (state)
glClear(GL_COLOR_BUFFER_BIT);           // apply it to the color buffer

glUseProgram(m_Shader);                 // bind the program
glBindVertexArray(m_VertexArray);       // bind the layout + buffer binding
glDrawArrays(GL_TRIANGLES, 0, 3);       // draw 3 vertices as one triangle

m_Window->OnUpdate();                   // poll events, swap buffers
```

`glClearColor` sets state and `glClear` consumes it — a distinction that shows
up throughout GL. `glDrawArrays` reads vertices straight from the bound buffer
in order; an index buffer, which lets vertices be reused across triangles,
comes later.

`Window::OnUpdate` calls `glfwPollEvents` then `glfwSwapBuffers`. Rendering
targets a back buffer while the front buffer is displayed, and the swap
presents the finished frame — this is what prevents a partially drawn frame
from being visible. VSync is on by default (`glfwSwapInterval(1)`), so the swap
blocks until the display refreshes and the loop is capped at the monitor's
refresh rate. That is currently the only thing keeping this loop from spinning
a core at 100%.

`Run` also derives a delta time each frame from a monotonic clock and hands it
to `Layer::OnUpdate` as a `Timestep`, so animation rates are independent of
framerate.

## Render targets

By default, draw calls land in the window's back buffer — the only target the
context creates. A **framebuffer object** is a second target you allocate
yourself: bind it, draw, and the result ends up in a texture instead of on
screen.

`Framebuffer` wraps one. The OpenGL implementation allocates two attachments:

| Attachment | Format | Why |
| --- | --- | --- |
| Colour | `GL_RGBA8` texture | What gets displayed; a texture rather than a renderbuffer so it can be sampled |
| Depth + stencil | `GL_DEPTH24_STENCIL8` texture | Packed into one attachment — no more memory than depth alone, and the stencil is there when it's needed |

Attachment storage is immutable once allocated, so `Resize` cannot resize in
place: it deletes the objects and rebuilds them. That's what `Invalidate` does,
and it is why resizing is a per-panel-drag operation rather than a per-frame
one. `glCheckFramebufferStatus` is asserted after each rebuild — an incomplete
framebuffer silently discards every draw call otherwise.

Two details are easy to get wrong:

- **The viewport is global state.** `Bind` sets it to the framebuffer's size,
  because it will otherwise still be sized to the window and the render will be
  cropped or letterboxed. `Unbind` deliberately does not restore it; ImGui's
  backend sets its own viewport before drawing, and anything else that draws to
  the window afterwards must set it itself.
- **The V axis is flipped.** GL's texture origin is bottom-left, ImGui's is
  top-left, so the image renders upside down unless the UVs are swapped:
  `ImGui::Image(id, size, {0, 1}, {1, 0})`.

The panel's size is only known after ImGui has laid it out, so the sandbox
records it during `OnImGuiRender` and acts on it at the top of the next
`OnUpdate`. One frame of lag while dragging is invisible, and it avoids
resizing a target that is currently bound. The same measurement drives
`OrthographicCamera::SetProjection` — without that, the scene stretches with
the panel instead of revealing more world.

## Mouse picking

A framebuffer can carry more than the picture. Adding a `RED_INTEGER`
attachment gives every pixel a 32-bit signed integer alongside its colour, and
`Renderer2D` writes an entity ID into it from a second fragment output:

```glsl
layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;
```

Hit testing then costs one `glReadPixels` at the cursor. No ray casting, no
CPU-side bounding boxes, and it is exact for rotated quads, irregular sprite
shapes, and overlapping geometry — whatever the depth test decided is visible
is what the ID buffer holds. `-1` is the clear value, meaning nothing was drawn
there.

Four things make this work:

- **The ID travels as a vertex attribute.** A batched renderer has no per-draw
  channel — 10,000 quads are one draw call — so the ID is per-vertex, declared
  `flat` so it is not interpolated across the quad.
- **`glVertexAttribIPointer`, not `glVertexAttribPointer`.** The float variant
  converts the bytes on the way in, so an `int` read in the shader comes out
  as garbage. `OpenGLVertexArray` switches on the attribute type.
- **`glDrawBuffers` must list every attachment.** Fragment outputs are routed
  by draw-buffer index; without it only attachment 0 is written and the ID
  buffer stays empty.
- **`glClear` cannot clear it.** It only carries a float colour, so the integer
  attachment is cleared separately with `glClearBufferiv` via
  `ClearAttachment`. Integer textures also cannot be filtered — `GL_LINEAR`
  makes the framebuffer incomplete, so they are created `GL_NEAREST`.

The read is synchronous and stalls the pipeline, which is fine once per frame
under the cursor and not something to do in a loop.

Together with render-to-texture this is the piece an editor needs; the same
framebuffer is also what any post-processing pass will be built on, since it
needs the rendered result readable rather than already presented.

## Debugging rendering

A black window with no error message is the normal failure mode, since most GL
mistakes are silent. Work through this list before reaching for a debugger:

1. **Is the shader compiling and linking?** The logs will say if not.
2. **Is a VAO bound?** Core profile requires one. This is the most likely
   cause.
3. **Is the attribute enabled and pointing at the right buffer?**
   `glEnableVertexAttribArray` and a correct stride.
4. **Is the geometry inside the -1..1 NDC volume,** and wound so it isn't
   discarded? Face culling is off by default, so winding shouldn't matter yet.
5. **Are you calling GL before `gladLoadGLLoader`?** That's a null function
   pointer, and often a segfault rather than a blank screen.

Debug builds request a debug context and install `glDebugMessageCallback`, so
driver messages reach the log by severity without any `glGetError` calls. If
the window is black and the log is empty, the problem is more likely one of the
five above than a GL error.

A sixth failure mode arrives with framebuffers: if the scene disappears the
moment it moves into a panel, check that the target is bound *before* the
clear, that the viewport matches its size, and that the framebuffer is
complete.

[**RenderDoc**](https://renderdoc.org/) is the tool worth installing when the
above isn't enough. It captures a frame and lets you inspect every draw call,
the bound state, buffer contents, and the shaders as they were compiled.

---

# Roadmap

Groups 1-5 from the original plan are done. What follows is what remains.

## Still outstanding

- [ ] **A `ShaderLibrary`**, so shaders are looked up by name rather than
      passed around as `shared_ptr`s
- [ ] **A profiler** — instrumentation timers and scope macros, ideally
      feeding a Chrome-tracing JSON
- [ ] **Query `GL_MAX_TEXTURE_IMAGE_UNITS`** at runtime rather than assuming
      the 16-slot floor, and generate the sampler switch to match
- [ ] **A PCH for `TestEnv`.** Only the engine has one
- [ ] **Vendor a `premake5` binary per platform**, or script fetching it
- [ ] **Multi-viewport ImGui.** Docking is on; `ImGuiConfigFlags_ViewportsEnable`
      would let panels be dragged out into their own OS windows, but it needs
      the platform-window loop in `ImGuiLayer::End` and a GL context restore
- [ ] **A scene/entity layer.** Picking returns an integer ID, but nothing owns
      those IDs yet — the sandbox invents them per draw
- [ ] **Mesh loading.** 3D geometry is hand-built in the demo; nothing reads an
      `.obj` or `.gltf` yet. tinyobjloader is the small first step
- [ ] **Back-face culling.** The cube is wound consistently for it, but it is
      not switched on — 2D quads would need checking first
- [ ] **Material handling.** The 3D demo sets uniforms by hand; there is no
      notion of a material binding a shader to its parameters
- [ ] **Physics: rotation.** Bodies translate but never spin. Needs SAT for
      oriented shapes, multi-point manifolds and angular impulses
- [ ] **Physics: raycasts.** Needed by audio occlusion, and by anything that
      wants to ask what is in front of it
- [ ] **Physics: a real broadphase.** Brute-force pair testing is O(n^2); a
      uniform grid or sweep-and-prune replaces exactly one loop
- [ ] **Audio and acoustics** — see [Audio and acoustics](#audio-and-acoustics)
      below

---

## Physics

### The prerequisite: a fixed timestep

`Application::Run` currently measures wall-clock time between frames and hands
that straight to every layer:

```cpp
Timestep timestep = time - m_LastFrameTime;   // Application.cpp:94
```

That is right for rendering and wrong for simulation. With a variable `dt` the
same scene gives different results at 60 and 144 fps, a stall makes bodies
tunnel through walls in a single step, and nothing is reproducible.

The fix is an accumulator: bank the elapsed time, run the simulation in fixed
slices, and keep the remainder for next frame.

```
accumulator += frameTime          // clamped, or a long stall spirals
while (accumulator >= FIXED_DT)   // FIXED_DT = 1/60 s
    world.Step(FIXED_DT)
    accumulator -= FIXED_DT
alpha = accumulator / FIXED_DT    // for interpolating the render transform
```

Rendering then interpolates between the previous and current physics state by
`alpha`, otherwise motion stutters whenever the loop runs at a rate that isn't
a multiple of the step. This changes the shape of the main loop and affects
every layer, so it wants doing before anything is built on top.

### Build it or vendor it

Worth deciding deliberately, because the answer differs by dimension:

- **2D — write it.** A rigid-body solver with gravity, collisions, friction and
  resting contacts is tractable and is the single best way to understand what
  a physics engine actually does. Box2D remains the reference if you get stuck.
- **3D — vendor it.** Jolt or Bullet. A correct 3D solver — convex hulls, GJK/EPA,
  constraint islands, continuous collision — is a multi-month project on its own,
  and not the interesting part if the goal is building games.

### What a 2D engine needs

- **`RigidBody2D`** — position, velocity, angular velocity, inverse mass
  (storing the inverse means static bodies are just `invMass = 0`, with no
  branching), restitution, friction, and a type: static, dynamic, or kinematic.
- **Integration** — semi-implicit Euler: apply forces to velocity first, then
  velocity to position. One line different from explicit Euler and dramatically
  more stable for the same cost.
- **Gravity** — a world-level acceleration (`{0, -9.81}`), applied to every
  dynamic body each step, plus a per-body scale so a character can feel floaty
  or heavy without a second world.
- **Broadphase** — brute-force pair testing is genuinely fine to a few hundred
  bodies. Replace with a uniform grid or sweep-and-prune when profiling says to,
  not before.
- **Narrowphase** — circles and AABBs first, then SAT for oriented boxes and
  convex polygons. Output is a contact manifold: normal, penetration depth,
  contact points.
- **Resolution** — sequential impulses, iterated a handful of times per step.
  Then positional correction (Baumgarte, with a small slop) so stacked bodies
  don't sink into each other, and a tangential impulse for friction.
- **Sleeping** — bodies below a velocity threshold for long enough stop being
  integrated. Both a large performance win and the cure for resting jitter.

Determinism comes from a fixed `dt` plus a fixed iteration order. Get both and
a replay reproduces exactly; miss either and it won't.

### What it depends on

Physics wants the **scene/entity layer** that's already outstanding — bodies
need owners, and the transform has to be shared with the renderer rather than
duplicated. It also wants **`DrawLine`**, because debugging a solver without
seeing colliders and contact normals is guesswork.

---

## Audio and acoustics

These are two very different problems and the roadmap should keep them apart.
Making sound come out is a week. Simulating how sound behaves in a space is
open-ended.

### Stage 1 — playback

- **Backend.** [miniaudio](https://miniaud.io/) is the pragmatic pick: single
  header, public domain, and it handles device setup, decoding and mixing.
  OpenAL Soft is the alternative if 3D positioning and HRTF matter more than
  simplicity — it does those natively.
- **`AudioEngine`** initialised alongside `Renderer::Init`, torn down with it.
- **`AudioClip`** for decoded, fully-resident effects; streaming for music,
  which shouldn't be held in memory.
- **`AudioSource`** — play, pause, stop, loop, gain, pitch.

The real hazard here is not the DSP, it's threading. Audio runs on a callback
thread driven by the device, not by your frame loop. Anything the game touches
while a sound is playing has to be lock-free or double-buffered; taking a mutex
in the audio callback produces glitches you'll struggle to reproduce.

### Stage 2 — positional audio

- **`AudioListener`** attached to the camera. `PerspectiveCamera` already
  exposes `GetPosition`, `GetForward` and `GetUp`, which is exactly the frame a
  listener needs.
- **Distance attenuation** — inverse, linear or exponential rolloff between a
  min and max distance.
- **Panning** from the listener-relative direction, or HRTF for real spatial
  cues over headphones.
- **Doppler** from relative velocity — which is only available once physics is
  tracking velocities, so it naturally follows that work.

### Stage 3 — acoustics

This is where it stops being playback. Ordered by value per unit of effort:

- **Occlusion and obstruction** — raycast from listener to source through the
  physics broadphase; attenuate and low-pass by whatever it passes through. A
  muffled sound behind a wall is most of the perceived realism, and it's why
  acoustics depends on physics rather than on audio.
- **Reverb zones** — per-region parameters with a crossfade on transitions.
  Cheap, and convincing enough that most shipped games stop here.
- **Early reflections** — image-source method against nearby planes. Gives a
  real sense of room size.
- **Convolution reverb** — baked impulse responses per space, convolved at
  runtime. Needs an FFT, and is the practical ceiling for a project this size.
- **Wave simulation** — FDTD or ray-traced impulse responses computed offline.
  Genuinely a research-scale undertaking; worth knowing it exists and that it
  is not the next step.

### Suggested order

1. Fixed timestep — small, and blocks physics
2. Scene/entity layer — already outstanding, wanted by both
3. `DrawLine` — small, makes the next item debuggable
4. 2D physics with gravity and collisions
5. Audio playback
6. Positional audio (Doppler once velocities exist)
7. Occlusion and reverb zones

---

## Done

**Events and the window.** GLFW callbacks for size, close, key, char, mouse
button, scroll, and cursor position, each translated into the matching `Event`
subclass and routed through `Application::OnEvent`. The close button exits;
resize updates the viewport; a zero-size window sets a minimized flag that
skips rendering. `KeyEvent.h` and `MouseEvent.h` were fixed — neither had ever
been compiled, because nothing included them.

**Layers and input.** `Layer` and `LayerStack` with overlays held above
regular layers, events propagating top-down until handled and updates running
bottom-up. `Input` polling with a per-platform backend, `KeyCodes.h` and
`MouseButtonCodes.h` so client code never sees GLFW constants, and `Timestep`
delta time passed to `Layer::OnUpdate`.

**Renderer.** `VertexBuffer`, `IndexBuffer`, `BufferLayout`, `VertexArray`,
`Shader` (including `#type`-delimited file loading and a uniform-location
cache), `Texture2D`, `RendererAPI` / `RenderCommand`, `Renderer` with
`BeginScene`/`Submit`/`EndScene`, and `OrthographicCamera`. OpenGL
implementations live under `Platform/OpenGL/`; glm provides the math and
stb_image the image loading.

**Sprite sheets.** `SubTexture2D` describes a rectangular region of a texture,
with `CreateFromCoords` cutting cells out of a regular grid (and `spriteSize`
for sprites spanning several cells). `Renderer2D::DrawQuad` and
`DrawRotatedQuad` take one directly. Because the slot is resolved from the
underlying atlas, any number of distinct sprites cut from the same sheet share
one texture slot and batch together.

**Renderer2D.** A batched quad renderer: `DrawQuad` and `DrawRotatedQuad` in
flat-colour and textured forms, accumulating geometry into one dynamic vertex
buffer and flushing when it runs out of vertex room or texture slots. Draw
calls track the number of distinct textures rather than the number of quads —
10,000 quads render in one call. `Renderer2D::GetStats()` exposes draw calls,
quad, vertex, and index counts.

**Framebuffers.** `Framebuffer` with an OpenGL implementation, taking a list of
attachment formats: an RGBA8 colour texture, an optional `RED_INTEGER`
attachment, and a packed depth-stencil, all recreated on resize. The sandbox
renders the scene into one and displays it in an ImGui panel, with the camera's
projection following the panel's aspect ratio.

**Mouse picking.** Every `Renderer2D` draw takes an optional entity ID, written
to the integer attachment by a second fragment output. `ReadPixel` returns the
ID under the cursor, so hit testing costs one pixel read rather than any
CPU-side geometry work, and it is exact for rotated and overlapping quads.

**Docking.** The ImGui submodule tracks the `docking` branch and
`ImGuiLayer::Begin` opens a dockspace over the main viewport, so panels can be
docked, tabbed, and split.

**Tooling.** ImGui as an overlay layer, with the GLFW and OpenGL3 backends and
input capture so ImGui windows swallow clicks instead of leaking them to the
game. GL debug context plus `glDebugMessageCallback` in debug builds, routing
driver messages to the log by severity. `EGSS_ENABLE_ASSERTS` defined in the
debug config — it had been defined nowhere, so every assert compiled to
nothing, including the GLFW and Glad init checks.

**Build and repo.** `release` optimizes, `dist` optimizes and strips symbols
(5.8 MB to 315 KB). Deleted the stale root `src/`, checked-in build output,
generated `.sln`/`.vcxproj` files (now gitignored), and the dead `GLFW` entry
in `.gitmodules`.

**Windows.** The port was verified on Windows after the Linux work: the
solution generates, builds, and runs. `NOMINMAX` and `WIN32_LEAN_AND_MEAN`
guard the `<Windows.h>` include in the PCH, C4251 is suppressed for the
exported classes, and the system libraries GLFW needs are named explicitly.

---

# Changelog

### 2026-08-01 (2D physics)

- **`PhysicsWorld2D`** — a standalone rigid-body world in the shape Box2D and
  Jolt use: it owns its bodies and knows nothing about the renderer. Semi-
  implicit Euler, gravity with a per-body scale, brute-force broadphase,
  circle/box/circle-box narrowphase, sequential impulses with restitution and
  Coulomb friction, iterated position correction, and sleeping.
- **`RigidBody2D`** stores *inverse* mass, so a static body is simply
  `InverseMass = 0` and the solver needs no special case for it.
- Bodies keep their previous position, so the `Physics2D` demo renders
  interpolated against the fixed timestep's alpha.
- Limits, deliberately: **no rotation**, circles and axis-aligned boxes only,
  and an O(n^2) broadphase. All three are listed in the roadmap.

Three bugs found by measuring rather than watching:

- **Nothing could sleep.** Static bodies default to awake and never sleep, so
  "wake the sleeper on contact" woke anything resting on the floor every step.
- **Stacks sank into themselves.** Position correction ran once per step
  against penetrations measured before correcting, so pushing the bottom box
  up drove it into the one above and that overlap went unseen until the next
  step. Now iterated, re-measuring each pass.
- **Stacks would not settle.** Eight solver iterations left 0.05-0.14 of
  residual velocity; forty got it to 0.003. Warm starting -- carrying each
  contact's accumulated impulse into the next step -- reaches 0.00005 at
  eight, better than forty without it.

A fourth appeared once sleeping worked: the first body in a stack to sleep
became immovable, which jolted its neighbour, which woke it again -- a limit
cycle that never settled. Fixed by sleeping whole contact islands atomically,
via union-find over the contact graph.

Verified headlessly: a lone box rests at exactly the 0.005 slop below the
floor's surface and sleeps; a five-box stack settles to within 0.0002-0.0019
of ideal spacing and the whole island sleeps.

### 2026-08-01 (line rendering)

- **`Renderer2D::DrawLine` and `DrawRect`**, backed by a second batch with its
  own vertex buffer and shader. Lines can't share the quads' draw call — a
  different primitive type needs a different call — so debug geometry costs
  exactly one extra call however many segments it contains. Drawn unindexed
  through new `RendererAPI::DrawLines` / `SetLineWidth` entry points.
- **`Renderer2D::BeginScene` now takes `const Camera&`** instead of a concrete
  orthographic one, so the line batch works under a perspective camera. The
  Cube3D demo uses it for a ground grid and world axes.
- Breakout draws its play area, every brick collider, and the ball's velocity
  vector, all behind a "Show colliders" toggle. Measured: 2 draw calls, 46
  quads, 189 lines — 4 + 44x4 + 4 + 4 + 1, exactly as expected.
- `Statistics` gained `LineCount`.
- The sandbox now opens on Breakout rather than Cube3D.

### 2026-08-01 (fixed timestep)

- **`Application::Run` now runs simulation on a fixed step.** Real elapsed time
  goes into an accumulator, which is spent in equal slices; the remainder
  carries to the next frame. Clamped at 0.25s so a breakpoint or a dragged
  window can't queue thousands of steps the loop will never catch up on.
- **`Layer::OnFixedUpdate(Timestep)`** for simulation, called zero or more
  times per frame; `OnUpdate` stays presentation-only and runs exactly once.
- **`GetInterpolationAlpha()`** for rendering between the last two simulation
  states, plus `Get/SetFixedTimestep` and `GetFixedStepsLastFrame`.
- Breakout's ball and paddle moved to `OnFixedUpdate` and now render
  interpolated, so the motion stays smooth when the sim rate and frame rate
  don't divide evenly. Its panel shows steps-per-frame and alpha, with a slider
  for the sim rate. Cube3D deliberately has no `OnFixedUpdate` — nothing in it
  is simulated.

Measured at ~60 fps: 60 Hz → 1 step per frame, 240 Hz → 3–4, 10 Hz → mostly 0
with a step every sixth frame. Alpha stayed within 0..1 throughout.

### 2026-08-01 (demo selector)

- A **`DemoSelector`** layer with a "Demos" panel: a dropdown, quick-select
  buttons with the live one highlighted, and F1 to cycle. Adding a demo means
  an enumerator, a name in `s_DemoNames`, and a `PushLayer` — a `static_assert`
  catches the enum and the name list drifting apart.
- Switching now lives in **one** layer. Previously each demo handled the key
  itself, and since every layer sees the same event, one would select the other
  demo and the second would select it straight back. Centralising it removed
  the problem instead of working around it.
- Demo panels get a default position clear of the selector.
- Roadmap: outlined **physics** (fixed timestep first, then a 2D rigid-body
  solver with gravity) and **audio/acoustics** (playback, positional, then
  occlusion via physics raycasts), with dependencies and a suggested order.

### 2026-08-01 (3D: camera hierarchy and a lit cube)

- **`Camera` base class** holding projection, view, and their product.
  `OrthographicCamera` now derives from it, and `Renderer::BeginScene` takes
  `const Camera&` instead of a concrete orthographic one — the only thing that
  was actually blocking 3D.
- **`PerspectiveCamera`** — field of view, yaw/pitch with pitch clamped to ±89
  where the view matrix degenerates, and `GetForward`/`GetRight`/`GetUp` for
  movement code.
- **`Cube3D` demo** — 24-vertex cube (a corner needs three normals, so
  positions can't be shared), Blinn-Phong directional lighting, checkerboard
  texture, and a keyboard fly camera. Drawn through `Renderer::Submit`, which
  had been implemented since the renderer abstraction went in and never called.
- `TestEnv` now holds both demos, **F1** switches. Split into `Breakout.h`,
  `Cube3D.h`, and `Demo.h`.
- **Fixed: `OpenGLRendererAPI::DrawIndexed` unbound the texture after every
  draw.** Invisible to `Renderer2D`, which rebinds its slots on every flush,
  but it meant any caller binding once and issuing several draws got one
  textured object and the rest black. Found immediately by the cube grid.

### 2026-08-01 (a game, and orientation docs)

- **[docs/ENGINE.md](docs/ENGINE.md)** — the frame's call path from `main` to
  `glDrawElements`, the five design decisions that explain the rest, and the
  day-to-day API on one screen.
- `TestEnv` is now **Breakout** — paddle, ball, brick grid, AABB collision,
  lives and score, in ~300 commented lines against nothing but the public API.
  It replaces the tilemap/viewport sandbox, which is still at commit `a282175`.
- **Dropped `ImGuiConfigFlags_NavEnableKeyboard`.** With it, any nav-focused
  widget set `io.WantCaptureKeyboard`, and `ImGuiLayer::OnEvent` then marked
  every key event handled — so a focused Restart button silently swallowed the
  game's keys. Found by the ball refusing to launch.

### 2026-08-01 (docking and mouse picking)

- The ImGui submodule now tracks the **docking** branch, `.gitmodules` records
  it, and `ImGuiLayer` sets `ImGuiConfigFlags_DockingEnable` and opens a
  `DockSpaceOverViewport` in `Begin`, so panels dock, tab, and split.
  `EnableDockspace(false)` turns it off for a game that owns the whole window.
- `FramebufferSpecification` now takes a list of attachment formats — `RGBA8`,
  `RED_INTEGER`, `DEPTH24STENCIL8` — instead of a fixed colour+depth pair, with
  `ReadPixel` and `ClearAttachment` for the integer ones.
- Every `Renderer2D` draw takes an optional `entityID`, carried as a `flat`
  vertex attribute and written by a second fragment output. The sandbox reads
  it back under the cursor and highlights the hovered tile.
- **Fixed a latent bug in `OpenGLVertexArray`:** integer attributes were set up
  with `glVertexAttribPointer`, which converts to float. Nothing used an `int`
  attribute before, so it had never shown. Now switches to
  `glVertexAttribIPointer` for the `Int*` and `Bool` types.

Verified by a deterministic in-code readback with the camera pinned at the
origin: the framebuffer's centre pixel and a computed corner pixel returned
exactly the entity IDs of the quads drawn there.

### 2026-08-01 (framebuffers and a viewport panel)

- `Framebuffer` and `OpenGLFramebuffer` — an RGBA8 colour texture plus a packed
  `GL_DEPTH24_STENCIL8` attachment, with completeness asserted on every
  rebuild. `Bind` sets the viewport to the target's size; `Resize` recreates
  the attachments, since their storage is immutable once allocated.
- The sandbox now renders into a framebuffer and shows it in an ImGui
  **Viewport** panel, flipping the UVs so GL's bottom-left origin matches
  ImGui's top-left one. The camera's projection follows the panel's aspect
  ratio, and camera keys only act while the panel has focus.
- Documented render targets under [Rendering](#render-targets), including the
  two things that bite: the viewport being global state, and the V flip.
- Untracked `EGSS/vendor/Glad/Makefile`. It is premake output, so it changed on
  every regeneration and flip-flopped between platforms.

Verified at 900x501 and again at 341x153 after a resize: the tilemap stays
square, and 402 quads still render in **1 draw call**.

### 2026-07-31 (sprite sheets)

- `SubTexture2D`, describing a rectangular region of a texture in normalised
  coordinates. `CreateFromCoords` cuts cells out of a regular grid, with an
  optional `spriteSize` for sprites spanning more than one cell.
- `Renderer2D::DrawQuad` and `DrawRotatedQuad` overloads taking a
  `SubTexture2D`. The texture slot is resolved from the underlying atlas, so
  every sprite cut from one sheet shares a slot and batches together.
- `SubmitQuad` now takes explicit texture coordinates rather than hardcoding
  the unit square; whole-texture draws pass a shared constant.
- The sandbox is now a tilemap: 400 tiles drawn from 16 distinct sprites in a
  single atlas, measured at 402 quads in **1 draw call**.

### 2026-07-31 (Renderer2D and batching)

- `Renderer2D` with `DrawQuad` / `DrawRotatedQuad`, in flat-colour and
  textured variants, plus tiling and tint. Quads accumulate into a single
  dynamic vertex buffer and are flushed in as few draw calls as possible.
  Measured: 1,602 quads in 1 draw call; 10,002 quads in 2.
- A batch flushes when it exhausts vertex room (10,000 quads) or texture slots
  (16). Slot 0 is a permanent 1x1 white texture, so flat-colour quads take the
  same shader path as textured ones instead of needing a second shader.
- `VertexBuffer::SetData` and a size-only `Create` overload, for buffers
  respecified every frame with `GL_DYNAMIC_DRAW`.
- `RenderCommand::DrawIndexed` takes an optional index count, so a partly
  filled batch draws only what it wrote.
- `Renderer2D::GetStats()` / `ResetStats()`, surfaced in the sandbox's ImGui
  panel alongside a grid-size slider for stress testing.
- The batching shader uses a `switch` over texture slots rather than dynamic
  sampler-array indexing, which GLSL 330 does not permit. This keeps the GL
  3.3 floor instead of forcing a 4.5 context.

### 2026-07-31 (textures and ImGui)

- `Texture2D` interface with an OpenGL implementation, backed by stb_image
  (vendored as a single header). Supports loading from disk and uploading raw
  pixel data.
- ImGui as an overlay layer pushed by `Application`, bracketing every layer's
  `OnImGuiRender`. Its `OnEvent` marks mouse and keyboard events handled when
  ImGui wants capture, so panels don't leak input into the game.
- ImGui's premake project lives at `EGSS/vendor/imgui_premake5.lua`, outside
  the submodule — a build file added inside it would be lost on re-clone and
  would leave the submodule permanently dirty.
- `TestEnv` now draws a textured, index-buffered quad behind the triangle and
  exposes frame time, camera position/rotation, and a quad tint in an ImGui
  panel.
- `Event::SetHandled` added, which ImGui's input capture needs.

### 2026-07-31 (later)

**Events, layers, input, and a renderer.** The engine went from "opens a
window" to something you can build on.

- Event system wired end to end: GLFW callbacks translate into `Event`
  subclasses, `Application::OnEvent` dispatches them, and the window's close
  button now exits.
- `Layer` / `LayerStack` with overlays, plus `Input` polling, key and mouse
  code constants, and `Timestep` delta time.
- Renderer abstraction: `VertexBuffer`, `IndexBuffer`, `BufferLayout`,
  `VertexArray`, `Shader` (with `#type`-delimited file loading),
  `RendererAPI` / `RenderCommand`, `Renderer`, and `OrthographicCamera`.
  OpenGL implementations live under `Platform/OpenGL/`.
- Added glm as a submodule for matrix and vector math.
- The triangle moved out of `Application` and into `TestEnv`'s example layer,
  where it now uses per-vertex colors and a movable camera.
- GL debug context plus `glDebugMessageCallback` in debug builds.
- `EGSS_ENABLE_ASSERTS` is now defined in the debug config; it had been defined
  nowhere, so every assert compiled to nothing.
- `release` and `dist` now optimize, and `dist` no longer ships symbols —
  5.8 MB down to 315 KB.
- Deleted the stale root `src/`, checked-in build output, generated
  `.sln`/`.vcxproj` files (now gitignored), and the dead `.gitmodules` entry.

Fixed two headers that had **never been compiled**, because nothing included
them until the event system needed them:

- `KeyEvent.h` — a missing semicolon after `int m_KeyCode`, and `GerKeyCode`.
- `MouseEvent.h` — `Std::stringstream ss:`, a missing `<<` before `m_Button`,
  a missing semicolon after `return ss.str()`, and `: Public MouseButtonEvent`.

### 2026-07-31

**A triangle.** The window now renders something.

- Added Glad as an OpenGL function loader, under `EGSS/vendor/Glad`. The
  loader is generated for GL 3.3 core and checked into the repo rather than
  added as a submodule, because TheCherno's `Glad` repo no longer exists.
  Regenerate with `pip install glad` and
  `python -m glad --profile core --api gl=3.3 --generator c --out-path <dir>`.
- Both window backends now request a 3.3 core context, call
  `gladLoadGLLoader` after making the context current, and log the GL version
  and renderer.
- `Application` creates a vertex array, a vertex buffer, and a shader program,
  and issues the draw call in the run loop. Shader compilation and linking
  report the driver's log on failure. All of this is temporary and moves into
  a renderer later.
- Defined `GLFW_INCLUDE_NONE` so GLFW doesn't pull in its own GL headers
  alongside Glad's.

**Linux support.** The project previously built only on Windows.

- Added an `EGSS_PLATFORM_LINUX` branch to `Core.h`. `EGSS_API` uses
  `__attribute__((visibility("default")))` when building the library and
  expands to nothing when consuming it, since ELF has no import side.
- Added `EGSS_DEBUGBREAK()` — `raise(SIGTRAP)` on Linux, `__debugbreak()` on
  Windows.
- Added `Platform/Linux/LinuxWindow.{h,cpp}`, mirroring the Windows backend.
- Added `system:linux` filters to `premake5.lua`: position-independent code,
  GLFW's transitive X11 dependencies, and an `$ORIGIN` rpath on `TestEnv`.
  Each platform now excludes the other's `Platform/` subtree, so
  `Window::Create` has exactly one definition.
- Moved `opengl32.lib` into the Windows filter; it had been linked
  unconditionally.
- Un-gated `main()` in `EntryPoint.h`, which was `#ifdef EGSS_PLATFORM_WINDOWS`.
- Added `BuildProject.sh` as the Linux counterpart to `BuildProject.bat`.

Fixed three case-sensitivity bugs that were invisible on Windows' case-
insensitive filesystem:

- `premake5.lua` referenced `EGSS/vendor/GLFW`; the submodule is at
  `EGSS/vendor/glfw`.
- The `EGSS` project used `location "Egss"`, which collapsed into the `EGSS/`
  source folder on Windows but created a second directory on Linux.
- `EventDispatcher::Dispatch` assigned to `m_handled`; the member is
  `m_Handled`.

Fixed pre-existing bugs that MSVC had tolerated:

- `EVENT_CLASS_TYPE` used `EventType::##type`. Token-pasting `::` with an
  identifier is an MSVC extension and not a valid preprocessing token.
- The assert macros called `HZ_ERROR` (left over from Hazel) with a misspelled
  `__VAARGS__`, and would not compile once `EGSS_ENABLE_ASSERTS` was defined.
- `WindowsWindow::Init` never set `s_GLFWInitialized`, so `glfwInit()` ran on
  every window creation.

Also gitignored the premake-generated makefiles, and expanded this README with
build/run instructions and this changelog.

### 2023-08-25

Added GLFW as a submodule and set up basic window creation, with a
platform-agnostic `Window` interface behind a `WindowsWindow` backend.

### 2023-08-24

Added precompiled headers (`egsspch.h`).

### 2023-08-23

Added a blocking event system — `Event` base class, category/type dispatch
macros, and application/key/mouse event classes. Added spdlog for logging and
premake for project generation.

### 2023-08-22

Initial engine skeleton: `Application`, `EntryPoint`, and the `EGSS_API` export
macro, split into an engine library and a sandbox app.
