# EGSS

Every Game Starts Somewhere, Why not here?

A game engine built from scratch, following along with TheCherno's Hazel
series. `EGSS` is the engine itself, built as a shared library; `TestEnv` is a
sandbox application that links against it.

**Current state:** an OpenGL 3.3 core context, an event system, a layer stack,
polled input, and a **fixed-timestep** loop with render interpolation. On top
of that: a **batched 2D renderer** (quads, lines, triangles, circles) with
sprite sheets and framebuffers with integer-attachment mouse picking;
orthographic and perspective cameras; **mesh loading** from Wavefront `.obj`;
a **scene layer** with generational entity handles and dense component stores;
a **2D rigid-body physics** world with rotation, oriented SAT manifolds,
warm-started impulses, island sleeping, raycasts and a uniform-grid broadphase;
a lock-free **audio engine** with
positional sound, occlusion and reverb; an instrumenting **profiler** with
Chrome-trace export; and ImGui (docking) as a debug overlay.

`TestEnv` holds six demos — Breakout, a 3D scene with transform gizmos and
loadable models, a rigid-body sandbox, a 2D visibility-polygon lighting scene,
an entity/component scene with pixel-exact picking, and a room-acoustics scene
where everything you hear is computed by bouncing rays off walls. See
[Roadmap](#roadmap) for what's left.

> **New to the codebase?** Start with **[docs/ENGINE.md](docs/ENGINE.md)** — the
> frame's call path, the decisions that explain the rest, and the whole API
> you'd use day to day. Then read `TestEnv/src/Breakout.h`, a commented worked
> example of building a game on it.
>
> **Picking this up after a break?** **[docs/HANDOVER.md](docs/HANDOVER.md)**
> has the working context: what is in flight, how things get verified here, and
> the traps that have cost time more than once.
>
> **Want to build something?** **[docs/LIGHTING_EXERCISE.md](docs/LIGHTING_EXERCISE.md)**
> is a staged walkthrough of writing a 2D lighting system, kept in the order it
> was actually built — including the wrong turns, which are the useful part.

## Layout

| Path | What it is |
| --- | --- |
| `EGSS/src/Egss/` | Engine core — application loop, layers, events, input, logging |
| `EGSS/src/Egss/Renderer/` | Backend-agnostic renderer interfaces |
| `EGSS/src/Egss/Scene/` | `Scene`, `Entity`, `ComponentStore`, `Components` |
| `EGSS/src/Egss/Physics/` | `PhysicsWorld2D`, `RigidBody2D`, raycasts, broadphase |
| `EGSS/src/Egss/Audio/` | `AudioEngine`, `AudioClip` — mixer, positional sound, reverb; `Acoustics2D` — ray-traced room response |
| `EGSS/src/Egss/Debug/` | `Instrumentor` — scope timers and Chrome-trace capture |
| `EGSS/src/Platform/` | Backends: `Windows/`, `Linux/`, and `OpenGL/` |
| `EGSS/vendor/` | GLFW, spdlog, glm, imgui (submodules); Glad, stb_image and miniaudio (checked in) |
| `TestEnv/src/` | Sandbox app that consumes the engine |
| `TestEnv/assets/` | Sample models; copied next to the executable on build |
| `premake5.lua` | Build definition — the source of truth for both platforms |
| `egss.py` | Build/run wrapper; always regenerates, so new files are never missed |
| `docs/` | `ENGINE.md` (orientation), `HANDOVER.md` (picking the project up cold), `LIGHTING_EXERCISE.md` (worked build) |
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

**Either platform**, via the wrapper script:

```sh
./egss.py                  # build debug and run it
./egss.py build release    # or debug (default), dist, all
./egss.py run release      # build, then launch from beside the binary
./egss.py clean all
```

It regenerates project files every time. That costs 0.2s — the same as a no-op
build — and in exchange the most confusing failure in the project simply
cannot happen: premake expands its file globs at *generation* time, so a newly
added `.cpp` is invisible until they are regenerated, and the symptom is an
undefined-symbol error for a function plainly sitting in the file you just
wrote. Pass `--no-gen` to skip it.

`run` launches from the binary's own directory, because the executable reads
and writes `imgui.ini` and `profile.json` relative to the working directory.

**Linux, by hand:**

```sh
./BuildProject.sh          # generate makefiles -- after adding/renaming a .cpp
make -j$(nproc) config=debug   # or: config=release, config=dist
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

You should see a 1280x720 window running one of the demos, plus:

```
[22:00:24] EGSS: Creating Window Every Game Starts Somewhere (1280, 720)
[22:00:24] EGSS: OpenGL 4.6 (Core Profile) Mesa 26.1.5 | Mesa Intel(R) Iris(R) Xe Graphics (RPL-U)
[22:00:24] EGSS: Renderer2D initialized (10000 quads/batch, 16 texture slots)
[22:00:24] EGSS: Audio PulseAudio | 48000 Hz, 2 ch, 32 voices
[22:00:24] EGSS: ImGui 1.92.9b initialized
```

The **Demos** panel switches between them; **F1** cycles.

- **Breakout** — Left/Right or A/D, Space to launch, P pause, R restart. Its
  panel shows simulation steps per frame and the interpolation alpha, with a
  slider for the simulation rate. Drop it to 10 Hz: the physics coarsens but
  the ball keeps moving smoothly, because rendering interpolates between steps.
- **Cube3D** — WASD to move, Q/E up and down, arrows or **middle-drag** to
  look. Drag the **XYZ gizmo** to move the cube or the light; the panel's
  target combo switches which. Two looping emitters demonstrate positional
  audio as you move.
- **Physics2D** — Space spawns bodies, P pauses, R rebuilds. Sliders for
  gravity, restitution, solver iterations and sleeping; a ray fan and a
  draggable audio listener with occlusion and a reverb zone.
- **Lighting2D** — a visibility-polygon light. **M** cycles between slider,
  keyboard and mouse control; the light collides with the walls. Two more
  orbit the scene, and surfaces are only visible where light reaches them.

The **Profiler** panel is the only honest timing in the app: VSync pins every
frame near 16.7ms regardless of what the frame actually cost.

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

- [ ] **Query `GL_MAX_TEXTURE_IMAGE_UNITS`** at runtime rather than assuming
      the 16-slot floor, and generate the sampler switch to match
- [ ] **A PCH for `TestEnv`.** Only the engine has one. The demo headers now
      pull in most of the engine, so this would actually pay off
- [ ] **Vendor a `premake5` binary per platform**, or script fetching it
- [ ] **Replay: record the ImGui panel state too.** Recording captures input,
      which is everything a person does *through the keyboard and mouse*. It
      does not capture slider drags, which reach the simulation directly —
      move "Gravity" mid-recording and the replay will not. Recording from
      defaults is exact; anything else needs the parameters stored alongside
      the input, which is a natural extension of the header
- [ ] **Multi-viewport ImGui.** Docking is on; `ImGuiConfigFlags_ViewportsEnable`
      would let panels be dragged out into their own OS windows, but it needs
      the platform-window loop in `ImGuiLayer::End` and a GL context restore
- [ ] **`.gltf` loading.** `.obj` carries geometry and nothing else — no
      hierarchy, no skinning, no PBR parameters. glTF is where those live, and
      is the format worth supporting second
- [ ] **Gizmo: rotate and scale handles.** Translate works; rotation rings and
      scale boxes are the same picking maths applied to different geometry
- [ ] **Physics: joints, and shapes beyond boxes and circles.** Rotation is
      done — the narrowphase runs `Sat2D`, contacts carry a lever arm and the
      solver has its angular terms. What is still missing is anything that
      constrains two bodies other than contact, and any collider that is not a
      box or a circle. Convex hulls would reuse the SAT that is already there;
      only the axis list changes
- [ ] **3D rigid bodies.** `Raycast3D` answers "what is in the way"; nothing in
      3D falls, collides or rests. A genuinely large item, and the one 2D
      rotation was meant to settle the ideas for — the lever arm becomes a
      cross product that does not collapse to a scalar, and the scalar inertia
      becomes a tensor that has to be rotated into world space every step

- [ ] **Partitioned FFT convolution.** The convolution reverb takes a *sparse*
      response, which is what the ray tracer produces. A dense recorded impulse
      is 96,000 taps a sample and needs overlap-save with an FFT
- [ ] **More than three bands, and per-material band curves.** Three is enough
      to hear bass outlast treble; real materials are measured in octave bands
      and a soft surface's curve is nothing like a hard one's
- [ ] **3D acoustics.** `Acoustics2D` is 2D because `Raycast` is. A room's floor
      and ceiling are half its reflecting area, so a traced RT60 is longer than
      the same room in 3D

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

Physics wanted the **scene/entity layer** — bodies need owners, and the
transform has to be shared with the renderer rather than duplicated. Both that
and **`DrawLine`** are now in; `RigidBody2DComponent` is the join, and
`Scene::StepPhysics` keeps the two sides in step.

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

### 2026-08-03 (inside-out geometry, found by looking)

Reported from the demo: the beacon and the sphere were showing their **inner
faces**. With back-face culling on that means the near surface is being culled
and you are looking at the far one's interior — the winding is backwards.

It turned out to be four things, only one of which was new:

| | wound | |
| --- | --- | --- |
| `Mesh::CreateSphere` | **inside-out** | pre-existing, in the engine |
| `pyramid.obj` | **inside-out** | pre-existing asset |
| `torus.obj` | **inside-out** | pre-existing asset |
| `beacon.obj` | **inside-out** | added the same day, by me |
| `Mesh::CreateCube`, `CreatePlane`, `icosahedron.obj` | correct | |

The comment in `OpenGLRendererAPI` had asserted for a long time that "Mesh's
primitives and the .obj convention both agree" with `GL_CCW`. It was simply
untrue, and nothing had ever checked it.

**Establishing which side was wrong mattered more than the fix.** When most of
the project disagrees with your test, the test is the likelier culprit — so the
convention was confirmed from three independent directions before anything was
touched:

- `CreateCube` carries **explicit face normals in code**, written next to the
  corners. Its winding agrees with them.
- `icosahedron.obj` agrees too, and is hand-authored.
- `torus.obj` supplies its own `vn` lines. Its winding disagreed with them on
  **2304 of 2304** triangles — so one of the two was wrong, and that alone does
  not say which. A torus settles it: the outward direction at a point is away
  from the nearest point on the tube's centre circle, and the file's normals
  point that way on **1225 of 1225** vertices. The normals were right; the
  winding was not.

The audit is per triangle: winding normal `cross(p1-p0, p2-p0)` against the
outward direction, which for a convex mesh about its centre is just the
centroid. Two traps in the measurement itself, both of which produced confident
wrong answers first:

- **A torus is not convex**, so that heuristic is meaningless for it — its inner
  third genuinely faces inward, and it duly reported 768 "inward" triangles both
  before *and* after the fix. Its `vn` are what can judge it.
- **The audit rebuilt the sphere from its own copy of the recipe**, so after
  fixing `Mesh.cpp` it kept reporting the old winding. The test's own comment
  had warned about exactly that drift.

Checking winding against generated normals is worthless, incidentally: with no
`vn` in the file the loader derives them *from* the winding, so they agree by
construction whichever way round it is. Only a file that states its normals can
be tested that way.

Final: 9/9, every mesh in the project wound counter-clockwise seen from outside.

### 2026-08-05 (replay: recording and playing back input, `.dem` style)

What Quake and Doom meant by a demo file — not a video, but the *input* that
produced one, replayed through the same simulation. `--record <file>` writes it,
`--play <file>` replays it, and the file names the scene it belongs to so
playback selects it without being told.

Called "replay" in code rather than "demo" only because `TestEnv` already uses
that word for Breakout, Physics2D and the rest, and one word for two things in
the same codebase is how you end up reading the wrong file.

**Input is sampled once per fixed step, never per frame.** That is the whole
design. Frames come and go at whatever rate the machine manages; steps are the
simulation's own clock, so a recording indexed by them replays the same on a
slow machine, a fast one, and one being dragged around mid-run. The cost, stated
plainly: a key pressed and released entirely between two steps is never seen, so
never recorded. At 60 Hz that is a 16 ms tap. Doom had the same property for the
same reason.

`Input` grew an `InputSnapshot` — a keycode-indexed bitset plus mouse — and a
playback override, so polling answers from the recording instead of the
hardware. Records are written **only when the state changes**, which is why a
260-step Breakout session with three keypresses is 416 bytes: a 32-byte header
and six records.

Edges become events. Code here uses both halves — Breakout moves the paddle by
polling and launches the ball from a `KeyPressedEvent` — so playback synthesises
presses and releases from state changes and pushes them through the normal path.
Live hardware events are dropped while playing, so a keypress from whoever is
watching cannot desynchronise the run; window events are let through, since
closing and resizing are the host's business, not the recording's.

#### What the measurements found

The first record-then-replay comparison **diverged**, and the reason was the
test rather than the code: the synthetic input driver written to make a
recording without a person at the keyboard set the polled state but dispatched
no events, so the recording launched no ball while the replay of it did. The
replay was right and the fake keyboard was incomplete. Once it produced both,
record and replay came back **byte-identical** at every step checked — 50, 100,
180, 260 and 350 — with a no-input control confirming the comparison was not
vacuous.

Two things that looked like bugs and were not:

- Asking for `--record` and `--play` together silently recorded an *inputless*
  run, because starting a recording stopped the playback first. A plausible file
  with nothing in it is the worst kind of wrong answer, so it now refuses.
- A recording made without `--lockstep` came out 352 bytes against 416 and
  replayed differently. It had simply not reached the last input change:
  `s_MaxFrameTime` clamps a slow frame and *discards* that simulation time, so
  300 frames is fewer than 300 steps. Given enough frames the two agree exactly.

#### The demos had to become frame-rate independent

`Acoustics2DDemo`, `Cube3D` and `Lighting2D` moved things in `OnUpdate` with a
variable `ts`, and the consequence was sharper than expected: they could not
reproduce *themselves* run to run, never mind under replay. Movement, the light
orbit and the spinner's rotation moved to `OnDemoFixedUpdate`. All six demos are
now step-deterministic, and a recording made at a **variable frame rate**
replays to a frame pixel-identical to a canonical `--lockstep` run — checked for
every demo.

Two remain non-reproducible in their *rendering* without `--lockstep`, and
correctly so: Breakout and Physics2D interpolate between simulation states, and
the blend factor is leftover wall-clock time. Their simulation at a given step
is identical; only the sub-step visual phase differs. Playback forces lockstep,
which pins it to zero.

An earlier note in this file claimed those three demos would "drift on replay".
That was half right for the wrong reason — they drift because they were not
step-deterministic at all, and lockstep would have hidden it rather than fixed
it.

### 2026-08-04 (in-engine frame capture, and what it caught)

Screenshots had been declared impossible on this machine, and the diagnosis was
right but the conclusion too broad: the *compositor* route is impossible. This
session is Wayland, `grim` is not installed, and `import` is an X11 tool with no
real root window to grab under XWayland — so it hangs. Nothing about that
prevents the engine photographing its own back buffer.

`ScreenCapture::SaveFrame` reads the bound framebuffer with `glReadPixels` and
writes a PNG through `stb_image_write` (vendored beside `stb_image`, same
pattern, so premake's existing glob picked it up with no build change). The read
happens between the last draw and the swap — the only moment a finished frame
exists. `Application::CaptureFrame` defers to exactly that point, so it is safe
to call from a key handler or an ImGui button. F2 in `TestEnv`.

This cannot go stale the way `import` did, because there is no compositor being
asked what it thinks is on screen; it is the buffer that was just drawn, read on
the thread that drew it. It also works identically on Wayland, X11, or a machine
with no session at all.

**Making it reproducible took three flags, each from a measured failure.** The
goal was a capture that can serve as a regression test, which means two runs
must produce the same image. They did not, and the reasons came out one at a
time:

- Two identical runs captured at frame 240 differed. `m_Accumulator += frameTime`
  is fed *wall-clock* time, so how many steps have run by a given frame depends
  on how fast the machine was. **`--lockstep`** runs exactly one fixed step per
  frame instead, and turns VSync off since nobody is watching.
- Still differed. Frames are skipped while the window is being mapped, so frame
  N is not step N. **`--capture-step`** indexes by the simulation's own clock.
  With it, both runs captured at "frame 239, step 240" — and the *scene* was
  then pixel-identical, confirmed by hashing raw RGB.
- The full frame still differed, because both ImGui panels print frame times in
  milliseconds. **`--hide-ui`** skips the panels while still beginning and
  ending the ImGui frame, so its state stays consistent.

With all three, two separate runs produce **byte-identical PNG files**. There is
also `--demo <index|shortname>`, which retires the "change the default, rebuild,
look, revert" dance the handover recommended for reaching a non-default path.

#### What one look caught that 45 checks had not

The rotation work was verified numerically to five decimal places, and the
capture immediately showed **discs being drawn as squares**. The demo had used a
quad as a stand-in behind a comment saying there was no circle primitive; there
is one, added at some point after that comment. Static squares were a harmless
placeholder. Rotating squares are not: a ball rolling down a slope drawn as a
spinning box reads as a tumbling crate, so the change that made rotation visible
also made the stand-in a lie. `DrawCircle` now draws the fill.

Numbers cannot see that. They said the angle was right, and it was — nothing
was wrong with the physics or with the drawn rotation. The wrongness was in what
the shape claimed to be, which is exactly the class of thing a picture is for.
The reverse holds too, and is why the numbers came first: a units error between
degrees and radians would leave every box drawn at a fifty-seventh of its angle,
and the capture confirms it is not.

### 2026-08-04 (rotation, joined up — and the ordering bug it exposed)

The join the last entry left open: the narrowphase now calls `Sat2D`, contacts
carry a lever arm, and the impulse solver has its angular terms. Bodies rotate,
and the demo draws them rotated, which it deliberately would not do before.

**`Contact` became a manifold.** Up to two `ContactPoint`s, each with its own
lever arms, penetration, accumulated impulses and effective masses. The effective
mass along an axis picked up the term that makes rotation work:

```
1 / ( imA + imB + iIA (rA x n)^2 + iIB (rB x n)^2 )
```

A point far from a centre of mass is *heavier* to push, because part of the
impulse becomes spin rather than travel. With no rotation both angular terms
vanish and it collapses to the sum of inverse masses the old solver divided by,
which is the reassuring part: nothing was replaced, one term was added.

Warm starting had to become per point, and matched **by position rather than by
index** — clipping hands the two corners back in either order and drops one
entirely as a crate tips, so index 0 is not the same corner from step to step.
Points more than 2 cm apart are treated as new and start from zero.

Circles got the same treatment. Their contact point moved from A's surface to
halfway into the overlap, which keeps the lever arm a full radius long for both
bodies — that is what lets friction *roll* a disc instead of dragging it.

**Everything that assumed an AABB had to be fixed, and one of them silently.**

- `BodyBounds` ignored rotation, so the broadphase grid dropped pairs the
  narrowphase would have caught. A 45° square reaches 0.707, not 0.5. A missed
  collision leaves nothing behind to notice — no contact, no warning, a body
  passing through a corner — which makes it the worst of the three.
- `RaycastBox` and the circle-box test now work in the body's **local space**,
  the trick `Raycast3D` established: a rotated box is not an AABB in world
  space, but a ray is still a ray in any frame you put it in. `ResolveCircle`
  came along for free, since it was already built on the shared narrowphase.

**Position correction gained an angular half.** Correcting a crate's two contact
points by different amounts *is* a rotation; a translation-only solver can only
average the two and leave the crate tilted forever.

#### The measurement that mattered

45 checks. The useful ones were deliberately *not* re-derivations of the
solver's own arithmetic — copying a formula twice proves only that it was copied
twice. They were against physics the code knows nothing about:

- **Total angular and linear momentum are unchanged across an impact.** Contact
  impulses are internal: equal and opposite at one shared point, so
  `ΔL = -(p x J) + (p x J) = 0` however messy the manifold. This is the single
  strongest check on the lever arms and the cross-product signs, and it is the
  one that caught the bug below. Read 2.00000 against 2.00000.
- **A disc rolls down a 20° slope at `(2/3) g sin θ`** — the textbook
  `g sin θ / (1 + I/mr²)` for a solid disc. Measured 2.218 against 2.237,
  0.86% out, with `v = -ωr` holding to 1.3%: it is genuinely rolling, and
  measurably slower than the `g sin θ` a frictionless slide would give.
- **A block on the same slope slides at `g(sin θ - μ cos θ)`** when μ = 0.1
  (1.197 m against 1.217, 1.65% out) and holds when μ = 0.8, since
  tan 20° = 0.364.
- **A stack's bottom contact carries exactly the weight above it**, `m g dt` per
  step. Five 1 kg boxes at 1/60 s: 0.81750 against 0.81750, to five decimals.

#### The bug: `Step` integrated position too early

The μ = 0.8 block did **not** hold. It slid 5.5 cm every second, and the
interesting part was that all the obvious explanations were wrong: friction had
double the headroom it needed (0.123 available against 0.056 used), 64 solver
iterations behaved identically to 8, the contact normal was exactly the slope
normal with two healthy points, and setting `PositionIterations = 0` changed
nothing. The block's velocity read exactly **zero** while it drifted.

Position moving with velocity at zero can only be integration. `Step` was
integrating velocity *and* position up front, so every body took one free-fall
sub-step of `g dt²` before the solver ever saw it. Position correction pushes
the normal part of that back out — which is why it was invisible on flat ground
for the entire life of the engine — but the part *along* the surface is never
undone. Friction cannot object to a movement that has already happened.

`g sin θ dt² × 60 = 0.0559 m/s`, against 0.055 measured. The arithmetic named
the cause before the fix confirmed it.

The fix is the standard ordering: integrate velocities, solve contacts, *then*
integrate positions (`IntegrateVelocities` / `IntegratePositions`, with contacts
generated at the positions the bodies ended the last step at). Creep went to
**exactly 0.00000** in the second and third seconds, and the rolling disc's
error improved from 2.60% to 0.86% as a free side effect.

It had gone unnoticed because **until rotation there were no slopes.** Every
contact in every demo was axis-aligned, and the error was purely along the
normal where correction hides it. A whole class of bug was sitting behind the
absence of a feature.

One failure on the way was mine and in the test: the momentum check computed the
solve-time positions as `p + v dt`, which was right for the old ordering and
wrong the moment it changed. It read as broken conservation rather than as a
stale assumption — worth remembering, because a conservation law failing is
alarming enough to make you doubt the physics first.

#### The demo

`Physics2D` draws rotated now — `DrawRotatedQuad` with `PreviousRotation`
interpolated exactly as position already was, oriented collider outlines walked
from their own corners, and a spoke on each disc so rolling is visible. Contact
points are drawn individually rather than one per pair, since one versus two is
the difference between a crate that rocks and one that rests.

The staircase of axis-aligned boxes that stood in for a ramp is now an actual
ramp at −22°, with a shallower one below it. Every third spawn is a crate
dropped already tilted and spinning, so it lands on a corner, tips, tumbles down
the slope and settles flush against it. 90 bodies over 900 steps settle to a
fastest 0.012 m/s with nothing escaping the walls.

**`DrawRotatedQuad` takes degrees**; the physics is radians throughout. Passing
radians draws a box turned about a seventh of the way it should be, which looks
convincingly like a solver bug.

### 2026-08-03 (rotation, the two halves that can be checked alone)

Rotation is the largest item on the roadmap and the one most likely to be left
half-finished, so it is being built in pieces that can each be verified without
the others. **Neither piece changes how anything collides yet** — that is the
point of doing them first.

**Angular state.** `RigidBody2D` gained `Rotation`, `AngularVelocity`, `Torque`,
`PreviousRotation`, `InverseInertia` and `AngularDamping`, integrated by the
same semi-implicit Euler the linear half uses. Inertia is a *scalar* in 2D —
there is one axis to turn about, so the whole tensor collapses to a number.
`PhysicsWorld2D` gained `ApplyImpulseAt`, `ApplyImpulse` and `ApplyTorque`.

Checked against the textbook, 25 checks: a 2×3 box of mass 6 has inertia
`m(w²+h²)/12 = 6.5`; a disc of radius 2 mass 5 has `mr²/2 = 10`; inertia scales
with the *square* of size, so doubling an equal-mass box's extents quadruples it
to 26. An off-centre impulse spins by `(r × j) / I` — 4 units applied one above
the centre of a 2×2 box of mass 4 gives exactly −1.5 rad/s — while the same
impulse through the centre gives none at all, which falls out of `r × j` being
zero rather than being special-cased. Two opposite impulses on opposite sides
spin it and move it nowhere.

Two decisions worth naming. Damping is **exponential**, `w /= 1 + d·dt`, not
subtractive: a subtraction overshoots zero at a large step and spins the body
*backwards*, and the test drives it with damping 50 over a 1-second step to
prove it cannot. And the sleep test now includes angular velocity — without it a
body that had stopped travelling would fall asleep mid-rotation and freeze at
whatever angle it reached, which looks like dropped frames rather than physics.

**`Sat2D`.** The separating-axis test for oriented boxes, with manifolds of up
to two points. Deliberately not built on `RigidBody2D`: it is geometry with no
mass, no velocity and no solver, which is what lets it be checked against
hand-worked answers and what will let the solver be checked separately against
*it*.

44 checks. Beyond the obvious ones, two properties matter more than any
individual number:

- **Pushing B along the normal by the depth separates them.** That checks the
  normal and the depth together, and it is the property the solver will actually
  rely on. Verified for two boxes at 30° and −20°, and for a tilted crate on a
  floor.
- **Face contacts give two points, corner contacts give one.** A single point
  cannot resist rotation about itself, so a resting crate held by one point
  rocks forever. The two points for a flat rest come back at exactly the crate's
  two bottom corners, and a crate overhanging a short platform has its contacts
  clipped to the platform's width — a point out past the edge is resting on
  nothing, and solving it would hold the crate up in mid-air.

Both failures on the way were arithmetic in the tests, not in the code: the
angular test's expected fall distance and, in the SAT test, an expected
half-diagonal written as a nonsense product that came to 0.5 where a 1×1 square
turned 45° plainly reaches `0.5√2 = 0.7071`.

### 2026-08-03 (a multi-material model, smoothing groups, and a 3D ray)

Three roadmap items, each closing a gap the previous session opened or named.

**A model that brings its own materials.** `assets/models/beacon.obj` is a base,
a post and a lamp head with three `usemtl` lines — one vertex buffer, three
submeshes, three draws. The `.mtl` machinery had 52 checks against it but
nothing in `TestEnv` exercised it, which is a different thing from working.

- `MeshComponent::Material` became **`Materials`**, one per submesh. A mesh with
  one submesh has one entry; the renderer fills any the caller left null.
- `MeshComponent::MaterialsFromFile` tells the renderer to leave the colour
  alone. Overwriting it with the component's `Color` would throw away the thing
  the file was loaded for, and the demo's global tint moved to its own `u_Tint`
  uniform rather than being folded into `u_Color` as it was.

Verified by reading each submesh's material back: `Slate → (0.280, 0.300,
0.340)`, `Brass → (0.780, 0.600, 0.220)`, `Lamp → (0.950, 0.850, 0.450)`,
exactly the file's `Kd` values in the file's order. That mapping is the whole
risk — assign them in the wrong order and the base comes out brass with nothing
else noticing. Six meshes now produce eight submesh draws, and the panel says
so.

**Smoothing groups.** `s 1` and `s 2` both counted as "smooth", so two groups
meeting at an edge shared vertices and averaged their normals across the seam —
destroying the exact crease the file asked for. The fix is in the vertex-sharing
key: `FlatFace` became `Share`, holding `-1` for a supplied normal, `-2 - face`
for flat, and the group number for smooth. The negative encoding for faces is
what stops face 1 and group 1 comparing equal.

Tested on a strip of four quads round a bend, where sharing is visible as a
vertex count: **10** with one group throughout, **12** with a seam at one column,
**16** with `s off`. A loader ignoring the group number gives 10 for the middle
case. All three existing models load byte-identically.

**`Raycast3D`.** `PhysicsWorld2D::Raycast` is 2D, and two things that wanted it
are not — 3D emitters could not be occluded, and `Acoustics2D` is 2D purely
because the ray it stands on is. This is the cheap half of that roadmap item: a
slab test against mesh bounds, plus a graded `Occlusion` that casts a ring of
rays so a source clipping a corner reads as partly blocked rather than flipping.

The ray is transformed into each object's **local space** rather than its box
into world space. That is what makes rotation work: a rotated box is not an AABB
in world space, but a ray is still a ray in any space you put it in. A thin plate
turned 45° is hit inside its turned footprint and missed outside it, in the same
position where the unrotated plate is hit — which a world-space AABB cannot tell
apart. 30 checks.

Two failures on the way, both mine and both in the test rather than the code:

- The smoothing test's expected face normal was **upside down**. The winding
  gives `(-dz, 0, dx)` and I wrote its negative, so both normal checks failed
  while every count passed. Counts right, expectation inverted.
- The raycast test fired its "should miss" probe at `x = 2.5`, which is exactly
  the *near cube's* face — so it hit, correctly, something I had not thought
  about. The plate now gets a clear lane at `y = 3` where nothing else can
  answer for it.

### 2026-08-03 (.mtl parsing, and submeshes)

The `.obj` loader's comment said materials were "deliberately left for whenever
a material system exists to receive them". One does now, so:

- **`MtlLoader`** — `newmtl`, `Ka` / `Kd` / `Ks` / `Ke`, `Ns`, `d`, `Tr`, `Ni`,
  `illum`, and the `map_*` lines. Parses text, produces `ObjMaterial` structs,
  and touches no GL at all — the same property that makes `ObjLoader::Parse`
  testable.
- **`MeshData::Submeshes`** — `usemtl` now produces something usable instead of
  more skipped text. A file with three materials is one vertex buffer and three
  index ranges, not three meshes, and `mtllib` references are recorded in order.
- **`RenderCommand::DrawIndexed` gained a `firstIndex`**, and
  `Renderer::SubmitSubmesh` draws one range with its own material.
- **`Material::FromObj`** bridges the two. An `.mtl` says "the diffuse colour is
  this"; a shader has a uniform with a name of its own, and nothing in either
  file agrees on what that is. `ObjMaterialUniforms` is where the two are
  introduced — once, rather than assumed at every call site.

`ObjMaterial` is deliberately **not** an `Egss::Material`. Keeping the parsed
form separate is what lets one file feed two different shaders, and what keeps
the parser free of any GPU dependency.

Verified with **52 checks**, most of which need no GPU. The ones worth naming
are the disagreements between the spec and what exporters actually write:

- **`Tr` is the inverse of `d`.** `d` is dissolve (1 is opaque), `Tr` is
  transparency (0 is opaque). Getting it backwards makes solid objects vanish,
  which looks like a renderer fault rather than a parser one.
- **`map_Kd` must not be read as `Kd`.** Prefix matching without a delimiter
  check is exactly how that happens, so `Keyword` requires whitespace after.
- **`Kd xyz 0.9 0.2 0.1` is a CIEXYZ colour, not RGB.** Reading the first number
  as red would silently produce a wrong colour — worse than ignoring the line.
- **Map paths keep their spaces.** `map_Kd -s 1 1 1 -bm 0.2 my textures/brick
  wall.png` has to skip options of varying arity and then take the *rest of the
  line*, not the next token.
- A value before any `newmtl` is an error rather than silent data loss.

Then the part the arithmetic could not settle: `glDrawElements` takes a **byte**
offset, so an index offset passed straight through would draw from the wrong
place while every integer above stayed correct. Two quads side by side, each its
own submesh, rendered off-screen: submesh 0 lights the left half and nothing on
the right, submesh 1 the reverse.

One measurement mistake, with a familiar shape. The end-to-end colour check read
**exactly half** the expected value on every channel — because `d 0.5` becomes
the colour's alpha and the default blend mode is `GL_SRC_ALPHA`. A constant
factor across every channel is the measurement's fault, and this project has
that written down from the last time it happened. The fix made it a better test:
it now draws twice, unblended for the colour and blended for the dissolve, and
the second is what proves `d` arrives as alpha rather than merely being stored.

Also checked that all three existing models still load as one unnamed submesh
each — the regression this change could most easily have caused.

### 2026-08-03 (materials, and shaders by name)

The 3D renderer had no word for "how this object looks". `Cube3D` set six
uniforms by hand against a bound shader and then submitted, which works only
because the draw happens immediately afterwards — the values lived in the GL
program, not in anything describing the object. Nothing could be stored on an
entity, loaded from a file, or shared between meshes.

- **`Material`** — a shader plus the values to give it, and a texture binding
  that sets the sampler uniform *and* binds the texture, since doing one without
  the other is the classic way to get a black object with no error anywhere.
- **`Material::CreateInstance(base)`** is the half that matters. Most uniforms in
  a scene are not per-object at all — the light, the camera, the ambient level
  are the same for everything drawn that frame. An instance holds only its
  overrides and falls back to its base. Binding uploads the base's parameters
  first and its own after, so overriding is just the later write winning: no
  merge step, no lookup, and the ordering *is* the mechanism.
- **`MeshComponent::Material`**, so per-object appearance lives on the entity.
  It may be null — adding geometry before deciding how it looks is normal — and
  the renderer supplies an instance the first time it draws one.
- **`ShaderLibrary`**, with `Renderer::GetShaderLibrary()` owning one. A missing
  name logs and returns null rather than asserting: a missing shader should show
  up as an unlit object, not take the program with it.
- **`Renderer::Submit(material, mesh, transform)`**, which sets
  `u_ViewProjection` and `u_Transform` *after* the material's own values, so a
  material cannot shadow the transform it is being drawn with.

Instances store parameters in a `std::vector`, not a map. Materials hold a
handful of values, walking a contiguous few beats hashing each one every frame,
and it keeps upload order stable — which makes a frame capture readable.

Verified with **26 checks**, and the split between them is the point. Half test
the parameter table, and are nearly worthless on their own: a table that
overrides perfectly and never uploads anything would pass every one. The other
half render into an off-screen buffer through a shader whose fragment is exactly
`u_Base * u_Tint`, so a pixel read back is an arithmetic statement about which
material the value came from. Those establish that:

- an instance that sets **only** `u_Tint` still renders with the base's
  `u_Base` — inheritance reaches the shader, not just the table
- two instances differing only in tint render exactly 2:1
- a material rendered **after** one that overrode `u_Base` gets the correct
  value back rather than the stale one left in the program
- changing the base afterwards is seen by its instances

Then the converted demo itself, which the synthetic shader says nothing about:
run under a debug GL context it logs no missing uniforms and no GL errors, and
a scan of its own framebuffer reads 854 lit samples across 5 meshes with 1,840
carrying an entity id — so `u_EntityID` is still reaching the integer attachment
that picking depends on.

I also gave up on screenshot verification for the second session running.
`import` hangs against XWayland here and cost two minutes before timing out.
Reading pixels back through `ReadPixelRGBA` is better anyway — it produces
numbers to compare rather than a picture to squint at.

### 2026-08-03 (a blocked-off corridor, and the darkening re-measured)

Two things, both about the acoustics demo's room.

**A dead-end corridor**, wrapped around the bottom-left of the main room: in at
the top left, down the west wall, east along the south wall, then north into a
dead end against the divider. One mouth, no way through. It is behind a
checkbox, because flipping it is the point.

```
.##########################################################################.
###................................................................####...###
###......#######################...................................####...###
###......#######################...................................####...###
###................................................................####...###
###.......###......................................................####...###   <- mouth, above the wall
###.......###.....................................................#######.###
###.......###.............................................................###
###.......###.......................######................##########......###
###.......###..................S....######................###...x..####...###
###.......###.............................................###......####...###
###.......###################################################......####...###
###................................................................####...###
.##########################################################################.
```

A corridor is the case a room-sized mental model gets wrong. Sabine and Eyring
both assume a diffuse field — energy spread evenly through the space — and a
long dead end fed by a single opening is the standard counterexample.

Verified by **flood fill rather than by eye**, because "it looks enclosed" and
"it is enclosed" are different claims, and a one-cell crack turns a corridor
into an alcove. Rasterise the room from the same point-in-body test the physics
uses, fill from the main room, and count: with the mouth open the fill reaches
the dead end and covers 30,380 cells; with the mouth artificially plugged it
reaches 25,663 and does *not* reach the dead end. So the 4,717 cells behind that
mouth are reachable only through it — exactly one opening, confirmed rather than
assumed. The cap deliberately laps 0.2 m into the divider: boxes that merely
touch leave a zero-width seam, and a ray that finds one leaks into the main room.

What it does, traced:

| | open room | with the corridor |
| --- | --- | --- |
| RT60 | 2.72 s | 2.38 s |
| mean free path | 7.99 m | 5.58 m |
| tail roughness | 8.63 dB | 6.01 dB |

And with the listener *in* the dead end: occlusion 100%, no direct path, RT60
2.05 s. **It is also a warning about ray budget**, which is the more useful
finding. Few rays ever find the mouth, so the corridor's share of the tail is
estimated from a thin sample — the dead-end RT60 reads 1.21 s at 128 rays,
1.28 s at 512, 1.84 s at 2048 and 1.99 s at 8192. The demo's slider tops out at
1024. That is not a bug, it is the variance of a stochastic method being visible
for once, and it is worth being able to see.

---

**The 15.4 dB darkening figure, re-measured.** It predated the band-splitter
rebuild and had been sitting flagged as stale. The old splitter leaked 33 dB of
bass into the treble band and the new one leaks 102 dB, and leakage can only
ever *weaken* a measured darkening — bass bleeding into the treble band props
the treble tail up long after its own energy has gone.

The end-to-end delta on its own is a weak measurement, because the windows the
original used were never written down. So this measures the **traced** echogram
as well, which no mixer has been near: the trace is ground truth, and the
question worth asking is how much of its darkening survives being rendered.

With the early window at 0.10–0.30 s:

| late window | traced | rendered | share |
| --- | --- | --- | --- |
| 0.50–0.90 s | 11.4 dB | 10.1 dB | 88% |
| 0.90–1.40 s | 19.2 dB | 17.3 dB | 90% |
| 1.20–1.70 s | 25.7 dB | 22.2 dB | 86% |
| 1.40–1.90 s | 30.2 dB | 24.9 dB | 83% |

**The chain now delivers 83–90% of the darkening the trace describes**, and that
ratio is the honest headline rather than the raw dB figure — it holds across
every window, so it is a property of the chain and not of where the windows were
put. It also degrades in exactly the direction residual leakage predicts: the
deeper into the tail you look, the more the remaining floor matters. The
comparable single number is 17.3 dB against 15.4, but the windows differ, so
treat that as direction rather than as a delta.

Two mistakes on the way, both mine and both in the measurement:

- **The first run found 0 paths from 600,924 bounces.** The default listener
  position (8, 2) is *inside* the right-hand pillar, and a listener inside
  geometry is never visible from any bounce. The demo calls `ResolveCircle` on
  both source and listener every frame and I had not; the trace was correct and
  reported an empty echogram, exactly as it should.
- **The "rendered" figure in that run was 37.3 dB** — a number better than
  anything real, produced by measuring a bare click decaying into silence with
  no reverb taps set at all. A result that beats the theoretical ceiling is a
  broken measurement, not a triumph.

### 2026-08-03 (per-pixel 2D lighting)

Surfaces were shaded per *body*: one raycast from each light to each body's
centre, one colour for the whole body. A wall is as bright at its far end as
under the lamp, and no amount of care in that calculation fixes it — the
question has one answer per object.

The fix is an order swap, not a better calculation. The light polygons go down
**first**, onto a buffer cleared to ambient, which makes that buffer a light
map: every pixel holds how much light reaches it, and the visibility polygon
has already done the shadow test per direction. The surfaces are then drawn
over it with **`BlendMode::Multiply`**, giving `albedo * light` per pixel.

- **`BlendMode::Multiply`** (`GL_DST_COLOR, GL_ZERO`) is the one new engine
  primitive. Additive can only ever brighten; it cannot tint, so a red wall
  under a white lamp came out white.
- **Surfaces have an albedo** now, from a small palette. With everything white
  the difference between multiplying and adding is invisible, which is exactly
  the case that hides the bug.
- **The old path is kept behind a toggle.** Flipping between them on a long
  wall is the entire point, and is far more convincing than a description.

Ambient is now the clear colour rather than a term added per body, so a pixel
no light reaches holds exactly the ambient value — which is what would make the
unlit case checkable by reading one pixel back.

One thing found while setting up to measure it, which is a correction to a
comment rather than a bug: **the rendered falloff is quadratic, not linear.**
`Falloff` returns a linear `1 - d/r`, and `DrawLight` scales both the vertex
colour *and* its alpha by it — then additive blending (`GL_SRC_ALPHA, GL_ONE`)
contributes `rgb * alpha`, multiplying the two together. So what lands in the
buffer falls off as f², and has since the light polygon was first written. It
looks more natural than the law it is computed from, which is presumably why it
was never questioned. It also means the hardware interpolation is not exact
along a fan triangle: f is interpolated and then squared per fragment, which is
not the same as interpolating f².

**Verified numerically — 27 checks, all passing.** `Framebuffer::ReadPixel`
only reads the integer attachment, so this needed one new engine call:
**`Framebuffer::ReadPixelRGBA`**, which reads a colour attachment back as 0..1
floats. With it, a throwaway test rendered into a 256×256 off-screen buffer and
compared individual pixels against arithmetic worked out by hand. RGBA8
quantises to 1/255, so the tolerance was one to three of those steps depending
on how many blends deep the value was; nothing needed more than 2.4.

What it established:

- `Multiply` really is `dst * src`, and **ignores the source alpha** — worth
  pinning down, since every other blend mode in the engine uses it.
- Additive contributes exactly `rgb * alpha` (0.8 at alpha 0.5 reads 0.4000).
- **The falloff exponent, fitted rather than asserted: 2.0093.** Two samples on
  a fan triangle's median, `p = log(c₁/c₂) / log(f₁/f₂)`. Linear falloff would
  have given 1.0. That is the f² finding above, measured rather than reasoned
  about.
- The composite holds per channel: `albedo × light`, agreeing to within half a
  step, and the channel *ratios* stay the albedo's — the red surface is still
  red under the white lamp, which was the whole reason for the multiply.
- An unlit pixel reads `albedo × ambient`, confirming ambient really is only
  the clear colour.

And a number that was worth having: **the fan's chord sag is 1.8%.** The
interpolated falloff runs out along the chord between two rim vertices, which is
shorter than the radius by `cos θ`, so the light dims slightly too fast in the
middle of a triangle. At a deliberately extreme 45° the reading is 38.1% below
ideal — but the demo's ring is clamped to at least 16 rays, and at that floor
the error is 1.8%. Real, quantified, and not worth fixing. Measuring the worst
case the code can actually reach, rather than the worst case that can be
constructed, is what turned this from a defect into a footnote.

### 2026-08-03 (the band splitter, rebuilt)

The roadmap asked for *steeper* crossovers. Measuring first showed steeper
crossovers could not have worked, and that the slope was never the problem.

The old splitter took a cascade of one-pole lowpasses and read the three bands
off by subtraction — `low`, `mid − low`, `input − mid`. That guarantees the
bands sum back to the input exactly, whatever the filters are, which is a real
and useful property. But **a subtracted band rejects the other side at
6 dB/octave no matter how steep the filter is**: `1 − Hᴺ` has a single
first-order zero however large `N` gets. Measured, the treble band's bass
rejection came out at 6.02 dB/octave with one pole per crossover, with three,
with six and with twelve. Going from one pole to three had visibly improved
things earlier, which made "add more poles" look like the obvious next step —
it improved the *lowpass skirts*, and did nothing at all for the leak that
mattered.

At 100 Hz the treble band sat only **33 dB** down. The bass tail is still
around 20 dB louder than the treble one at the point where treble has died, so
that leak was most of why a dead treble tail stayed audibly masked.

- **4th-order Butterworth per band**, two biquad sections each, in transposed
  direct form II. Low is a lowpass, high a highpass, mid a highpass into a
  lowpass. 24 dB/octave on every skirt.
- **Amplitude-complementary is replaced by power-complementary.** Butterworth
  gives `|LP|² + |HP|² = 1` exactly at every order, and power is the right
  thing to conserve here: a diffuse tail is incoherent taps, so it is energy
  that has to survive the split, not amplitude.
- **A fourth, unfiltered convolution path** for taps belonging to no band.
  Exact broadband response was the one thing the old topology bought, so it is
  kept — by not filtering broadband taps at all rather than by reassembling
  them from three filtered copies. It is now bit-exact instead of 6e-8, and
  costs **one** convolution instead of three.

Measured through `RenderForTest`, against the transfer functions worked out
independently:

| | 100 Hz | 500 Hz | 1 kHz | 4 kHz | 8 kHz |
| --- | --- | --- | --- | --- | --- |
| low | −0.0 | −3.0 | −24.1 | −73.0 | −85.2 |
| mid | −55.9 | −3.0 | −0.0 | −3.0 | −26.7 |
| high | −101.9 | −72.9 | −48.9 | −3.0 | −0.0 |
| *predicted* | *−129.0* | *−73.0* | *−48.9* | *−3.0* | *−0.0* |

Every band matches prediction to within 0.1 dB wherever the prediction is above
−70 dB, the three bands' powers sum to unity to within 0.5 dB at every
frequency tested, and the rejection slope measures **24.2 dB/octave** against
the old 6.0. Treble band at 100 Hz: **−101.9 dB, from −33.2 dB.**

Three things measurement caught:

- **Everything read exactly zero at first.** `StopAll` only *requests* a stop;
  the mixer notices the flag on its next block, deactivates every voice and
  returns silence for that block. The test called it and then immediately
  started the voice it wanted to measure, so the first render killed it. The
  flag has to be flushed through before setting up.
- **Then everything read exactly 6.0 dB low** — a constant offset, which is the
  signature of a term the *measurement* forgot rather than a filter that is
  wrong. A centred mono voice puts √½ in each channel and a centred tap does it
  again, so two constant-power pans multiply to exactly ½.
- **The filter is better than float32 can show.** The prediction at 100 Hz is
  −129 dB; the measurement bottoms out around −102. The mix, the biquad states
  and the convolution accumulator all carry about seven decimal digits, so a
  response 100 dB down is within two decades of the epsilon and what comes back
  is arithmetic noise. Measuring the rejection *slope* down there gave
  6.5 dB/octave — two points in the same noise floor, which looks exactly like
  a filter that does not roll off. The slope is measured at 1–2 kHz instead,
  where the response is real.

Verified with 21 checks.

### 2026-08-03 (scattering coefficients)

Every surface was a mirror. That is a worse assumption than it sounds: in a
rectangular room a specularly bouncing ray's direction only ever takes **four
values**, so the field never becomes diffuse however many rays you throw at it.
The tail arrives as a comb of spikes with gaps between them, and the decay
comes out long — the ~10% bias over the diffuse-field formula that has been
sitting in the known-approximations list since the tracer was written.

- **`AcousticsSettings::Scattering`**, 0 for a mirror and 1 for a surface that
  has forgotten which way the sound arrived, with `PerBodyScattering` for
  per-material figures on the same terms as absorption. Decided per bounce
  rather than by splitting the ray, so tracing costs the same whatever the
  room is made of.
- **Lambert's cosine law, sampled by inverting its CDF.** In 2D the law is
  `pdf(θ) = cos(θ)/2` over the half-circle, whose CDF inverts to
  `θ = asin(2u − 1)` — one call, no rejection loop, and every sample lands in
  the correct half-circle by construction. Sampling *uniformly* over the
  half-circle is the obvious thing and is wrong: it sends too much energy along
  the wall and lengthens the decay.
- **`TailRoughness`**, in dB RMS: how far each late echogram bin sits from its
  own local average. This is what "gaps in the tail" actually means.
- Deliberately **not per band**. A ray carries three energy packets but only
  one direction, so it can only scatter or mirror as a whole; a surface that
  mirrors bass and diffuses treble needs one ray per band.

Measured in a bare 20 × 12 m rectangle — no pillars, because pillars would hide
exactly the effect being measured. Against Eyring fed π·A/P:

| scattering | RT60 | vs Eyring | tail roughness |
| --- | --- | --- | --- |
| 0.0 (mirror) | 2.337 s | **+9.9%** | 3.66 dB |
| 0.1 | 2.281 s | +7.3% | 2.10 dB |
| 0.3 | 2.205 s | +3.7% | 1.22 dB |
| 1.0 (diffuse) | 2.170 s | +2.1% | 0.98 dB |

Repeating the sweep at half the absorption reproduces it: +10.1% down to
+2.1%, with roughness 2.99 dB down to 0.76 dB. The comb becomes noise.

Three things measurement caught:

- **Counting empty echogram bins was useless.** It was the obvious way to
  measure "gaps in the tail" and it read **100% for a mirror room and 100% for
  a diffuse one** — at a few hundred rays and 5 ms bins, every bin gets
  *something* either way. What differs is the spread, not the occupancy, which
  is why the shipped metric compares each bin to its neighbours instead.
- **A hand-derived mean free path disagreed with the trace, and the formula was
  the thing that was wrong.** Unfolding the rectangle gives a mean chord of
  `1/n(θ)` per direction, which averages to 12.14 m — but rays here run out of
  *path length* before they run out of bounces, so a direction that bounces
  twice as often contributes twice as many samples. The path-weighted answer is
  `1/⟨n⟩ = π/2 · WH/(W+H) = π·A/P` exactly: 11.78 m, and the trace says 11.74.
  So the specular rectangle lands on the *diffuse* figure too, and mean free
  path cannot detect this problem at all — which is why a purely specular
  tracer passed that check all along.
- **The sweep's shape is mostly fit noise.** It does not fall monotonically
  (+3.7% at s=0.3 against +2.1% at s=1.0), but re-running one setting with four
  different scattering seeds spreads the answer over 3.1 points. Only the two
  ends are outside that; the curve between them is not. The first version of
  this check said the dip was real, and it was reporting a spread of 0.7 points
  measured under a slightly different trace budget.

Verified with 29 checks: `s = 0` still traces **bit-identically** to the old
specular path, a room that has not moved traces identically twice, a different
seed lands within 10% of the same RT60, and the reported roughness matches an
independent implementation of it at every scattering value.

Also fixed while in the demo: three tooltips containing `\\n` rendered a
literal backslash-n, and a fourth was a stray duplicate left dangling after the
band-scale block, so it attached itself to whatever widget happened to precede
it.

### 2026-08-03 (frequency-dependent absorption)

Almost everything absorbs treble faster than bass, which is why a real tail
grows *duller* as it decays rather than just quieter. Until now the tracer
carried one energy figure per ray and the tail stayed the same colour all the
way down.

- **Three bands** — low under 500 Hz, mid, high over 4 kHz. Each ray carries
  three energy packets that start equal and diverge as they bounce; after a
  dozen surfaces the treble packet is a fraction of the bass one.
  `BandAbsorptionScale` defaults to `{0.55, 1.0, 1.9}` as multiples of a
  surface's figure, so the existing single knob still means something. Setting
  all three to 1 gives the old behaviour back exactly.
- **`BandReverbTime[3]`**, and an impulse tail per band. The budget is shared
  by how much tail each band has to cover, so a short treble tail does not
  spend impulses a long bass tail needs.
- **A complementary band splitter in the mixer**: two cascaded lowpasses, with
  the bands read off as `low`, `high - low` and `input - high`. That sums back
  to the input *exactly* whatever the slope, which is what makes a broadband
  response transparent — a tap with no band set still comes out at precisely
  the gain it would have without any splitter.

Two things measurement caught:

- **One-pole crossovers were far too gentle.** At 8 kHz a 6 dB/octave split at
  4 kHz still puts nearly as much signal in the mid band as the high one, so
  the two tails averaged together and the treble never audibly died first.
  Three poles per crossover took the rendered treble decay from 2.28 s to
  1.78 s against a traced 0.97 s.
- **A band's tail was being measured to where its energy underflowed**, not to
  where it stopped being audible. Bass and treble reach zero at nearly the same
  bin — the difference is entirely in *when they got quiet* — so every band was
  claiming an equal share of the impulse budget (169/169/168). Ending each tail
  60 dB below its own peak gives 197/196/107. Taps past the mixer's two-second
  buffer were also being built and then silently dropped, quietly halving the
  density of the part that did fit.

Verified with 18 checks. Per-band absorption is exact, and each band's RT60
matches Eyring fed that band's own figure to within 10% — the same ~10% bias
the broadband tracer has, for the same reason. The splitter reconstructs its
input to 6e-8. End to end, in a room traced and rendered through the whole
chain: **the tail measures 15.4 dB darker late than early.**

The rendered per-band decays are only checked to an order of magnitude, and
deliberately. Once a band's own tail has died, what remains in that band of any
analysis is leakage from its neighbours — and the bass tail is still 20 dB
louder at that point, so it drags the measured figure towards its own. The
treble/bass energy ratio is the honest test: contamination can only weaken it,
so seeing 15 dB survive is evidence the effect is really there.

### 2026-08-02 (convolution reverb)

The tail stops being described and starts being *played*. The echogram the
tracer already produces is a coarse impulse response; convolving the mix with
it replaces the comb-and-allpass reverb with the room's own decay.

- **`AudioEngine::SetReverbImpulse`** — direct convolution with a **sparse**
  response, a few hundred impulses rather than a dense recording. A dense
  two-second response is 96,000 taps a sample and needs partitioned FFT
  convolution; a sparse one is affordable as a plain loop, and is what a ray
  tracer naturally produces anyway. Setting an impulse switches the mixer over;
  clearing it puts the parametric reverb back.
- **`Acoustics2D::BuildImpulseTaps`** turns a traced echogram into those
  impulses, placed one per equal interval at a jittered offset — velvet noise.
  Even spacing combs; fully random placement clumps and leaves gaps; one per
  interval, jittered, gives even density without periodicity.
- **The signs are random, and that is the whole trick.** Same-sign impulses sum
  coherently into a ringing comb; random signs sum incoherently into noise,
  which is what a diffuse tail *is*. It also fixes the amplitude: n incoherent
  impulses of amplitude a carry n·a² of energy.
- **Pans are spread rather than aimed.** A late tail is diffuse by definition —
  if you can tell which direction it came from, it is still an early
  reflection.
- **The tail is fixed for a given room**, from a seeded xorshift rather than
  `rand`, so a stationary listener does not hear the reverb shimmer as it is
  rebuilt.

One real bug, found by measuring energy at three densities: impulses were given
`energy / (impulses in that bin)`, which looks equivalent to the right answer
and is not. At low density an interval is about one bin wide, jitter pushes
taps into neighbouring bins, and bins left empty lose their energy outright — a
sparse tail measured 7.5% quiet compared to a dense one. Each impulse now
carries the energy of *its own interval*, which is exact at every density.

Verified with 18 checks, the important one being a closed loop: build an
echogram with a decay chosen in advance, turn it into impulses, render what the
mixer makes of them, and measure the RT60 back out with the same backward
integration the tracer uses. **Asked for 1.200 s, rendered tail measures
1.216 s — 1.3% off**, with every stage independent of the last. Plus: taps land
at their exact delay and gain, negative taps stay negative, energy is preserved
to 0.0% at densities of 200, 400 and 800, and clearing brings the comb reverb
back audibly.

Two of the three initial failures were the *test's* fault, not the code's — the
reverb wet level is crossfaded over a fraction of a second, and measuring after
a single block reads the ramp rather than the taps. It came out at 0.428 of the
right answer at every tap, which is the shape of a ramp and not of a wrong gain.

### 2026-08-02 (ray-traced acoustics)

Sound treated as geometry. Rays leave the source, bounce off walls losing
energy to absorption, and at every bounce ask whether the listener can be seen
from there. Each yes is a path the sound could actually take. Collected into an
energy-versus-time histogram, those paths *are* the room's impulse response,
coarsely sampled — early reflections at the front, a decaying tail behind.

- **`Acoustics2D::Trace`** — the whole thing, and it touches no audio code and
  no GL. It takes a `PhysicsWorld2D` and two points and returns numbers, which
  is what makes it testable against formulas rather than by listening.
- **One trace drives three separate things**: occlusion from whether the direct
  line is blocked, early reflections from the loudest paths inside 80 ms, and
  the reverb tail from how fast the traced energy decays.
- **`AudioEngine::SetVoiceReflections`** — a per-voice multi-tap delay, up to
  eight taps over 200 ms, each with its own delay, gain and pan. Tap sets are
  published by rotating through a ring of eight and releasing an index, so the
  mixer picks up whole sets and never a half-updated one. No allocation on the
  audio thread; the history buffer is allocated when the voice is constructed.
- **The engine still does not work acoustics out for itself.** It takes three
  numbers per tap and asks no questions about where they came from — the same
  boundary occlusion already had.
- **An effective radius**, which is the thing a falloff distance cannot
  express: sound carries 40 m down a corridor and 5.6 m across a small room of
  equally hard walls.

Four bugs found by measuring rather than listening:

- **Spreading was applied to the whole path** instead of only the last leg. The
  packet a ray carries keeps its energy as it travels; what varies is how much
  of what a surface re-radiates the listener happens to catch. Applying `1/d²`
  over the total path counts the distance falloff twice, and since path length
  grows with time it showed up as decay — every measured RT60 came out at
  roughly half what the room's absorption says it should be.
- **Running out of path budget was being counted as escaping**, which made a
  sealed room report 512 of 512 rays leaking out. One extra unbounded cast on
  the miss tells the two apart.
- **The decay fit ran into its own truncation.** Backward integration of an
  echogram that stops early turns the cliff into a plunge that looks exactly
  like a very dead room — a confident, wrong number. It now fits only inside
  the traced span, and *refuses* when the tail outlasts the trace, falling back
  to the Sabine estimate and saying which one you got.
- **Late/direct energy divided by what got through**, so a fully occluded
  source reported a dry room — at exactly the moment when all there is to hear
  *is* the room. The reference is now the unoccluded direct energy.

Verified with 40 checks. The geometry against analytic results the tracer knows
nothing about: mean free path against `pi x Area / Perimeter` (3.5%), and RT60
against Sabine/Eyring across four absorptions (within 10% at every one). The
DSP against arithmetic, using `RenderForTest` to run the mixer with no device:
an impulse plus one tap at 0.5 gain lands **0.3536 at sample 480 exactly**, a
hard-left tap appears only on the left, and zero-delay, over-long and silent
taps are refused.

The traced RT60 sits consistently ~10% *above* the formula. That is not noise
and not a bug: Sabine and Eyring assume a diffuse field, and a rectangle with
mirror walls never becomes one — a ray's direction only ever takes four values,
so it runs a periodic orbit. Adding round scatterers to break the orbits moves
the bias from +9.6% to +7.5%, which is the direction that explanation predicts.
The residual is chord-length variance, which makes any renewal process decay
more slowly than the exponential of its mean.

### 2026-08-02 (a 3D scene, and clicking things in it)

Cube3D stops tracking one object by hand and becomes a small scene: five
entities, each with a transform and a **`MeshComponent`**, drawn by a system
that walks the store. Click an object to select it; the gizmo, the inspector
and the hierarchy all follow.

- **Pixel-exact picking in 3D.** The mesh shader writes `u_EntityID` into the
  framebuffer's integer attachment alongside the colour. Meshes are submitted
  one at a time rather than batched, so it can be a *uniform* — no per-vertex
  attribute, unlike the 2D path. Free of geometry maths, which matters more in
  3D than in 2D: it is right for a torus's hole, which any bounding volume
  would claim you had clicked.
- **A latent bug this uncovered.** The line/triangle shader wrote only
  attachment 0. When a framebuffer has two draw buffers, a fragment output the
  shader never assigns is *undefined* — not left alone — so every debug line
  scribbled noise into the picking attachment. It had been doing this in the
  2D scene demo already. Both shaders now write `-1`.
- **Smoothing groups.** `s off` is the .obj default and it means *flat*: a
  face's corners are not shared with its neighbours, so each keeps its own
  normal. Ignoring it made every low-poly model look like a balloon — the
  icosahedron rendered as a smooth blob. The fix is one extra field in the
  vertex-dedup key: a face index when the normal has to be invented and
  smoothing is off, `-1` when the corner may be shared. Then the *same*
  averaging pass produces flat shading for unshared corners and smooth shading
  for shared ones, because a flat corner belongs to exactly one face.
- **Normals the file supplied are never overwritten**, even when other faces
  in the same file have none.
- **`RenderCommand::SetBackfaceCulling`.** Off by default — it is only safe
  once every mesh in the pass is wound consistently, and one flipped model
  shows up as holes. Enabled for the 3D mesh pass, disabled again before the
  debug lines, which are not closed geometry.
- **The gizmo grabs on a press *edge*, not a held button.** Polling "is the
  button down" grabs whatever the cursor is near if the button was already
  held — which is what a button held across a demo switch looks like. Found
  when a stray automated click leaked into the next run and dragged an object
  across the scene on startup.
- **The selection box is built from the mesh's bounds** and transformed with
  the object, so it rotates rather than swelling into an axis-aligned shell.

Verified with a self-test projecting each entity's centre to a pixel and
reading the attachment back: **5 entities named correctly, empty sky returned
-1, 0 mismatches** — then interactively, hovering four objects and getting four
correct names. The loader's 48 checks all pass against the new flat-shading
expectations, including that each triangle's three corners share one normal
when flat and do not when smooth.

### 2026-08-02 (mesh loading)

- **`Mesh`** — geometry on the GPU, plus the numbers worth knowing about it:
  vertex and triangle counts, and a bounding box. No material, no transform,
  no hierarchy, which is what lets one mesh be drawn a thousand times without
  being copied once.
- **`Mesh::CreateCube` / `CreateSphere` / `CreatePlane`.** The cube used to be
  24 vertices and 36 indices written out inline in the demo; a demo now asks
  for geometry instead of describing it.
- **`ObjLoader`** — Wavefront `.obj`, written rather than vendored. It is a
  plain-text format with one item per line, and the parts worth getting right
  are the parts a library would hide: `v` / `vt` / `vn` / `f`, faces of any
  size fan-triangulated, negative indices, and files carrying no normals.
- **A face corner names three things**, and two corners are the same vertex
  only if all three match. That is why a cube from an `.obj` still ends up
  with 24 vertices from 8 positions — the dedup key is the whole triplet.
- **Generated normals are area-weighted**, which costs nothing: the cross
  product's length is already twice the triangle's area, so adding it
  un-normalised weights each face by its size for free. Averaging face normals
  equally would let one sliver pull as hard as a large face.
- **`Mesh::Load` returns `nullptr` and logs**, and a failed parse leaves the
  caller's `MeshData` untouched. A missing model should not take the program
  with it, nor leave half a previous one behind.
- **`ObjLoader::Parse` takes text, not a path**, so geometry can be verified
  without a window, a GL context or the filesystem.
- **`PerspectiveCamera::GetFov`** and a **"Frame it"** button. A loaded model
  can be 0.1 units across or 500, and neither is worth discovering by flying
  around looking for it. The distance that fits a sphere of radius r is
  `r / sin(fov / 2)`.
- **Assets are copied next to the executable** by premake after every link.
  The two platforms need different arguments: `cp -rf src dst` puts src
  *inside* dst when dst exists, while `xcopy` copies the contents — so the
  naive form nested `assets/assets/assets` one level deeper per build. Caught
  by running the build three times and looking.

Three sample models ship in `TestEnv/assets/models`, chosen to exercise
different parts of the parser: a torus with full `v`/`vt`/`vn` quads, an
icosahedron with positions only and a negative-index face, and a pyramid small
enough to read end to end.

Verified with 39 checks against hand-computed values -- vertex and triangle
counts, bounds, unit-length generated normals, all four face forms, negative
indices, n-gon fanning, CRLF and comment handling, and five malformed files
that must be *rejected*. One failure, which was the test's arithmetic and not
the loader's: 48x24 is 1152 quads, not 2304.

`MeshComponent` was deliberately not written. Nothing would read it yet, and a
component with no system is the scaffolding this project keeps trying to avoid.

### 2026-08-02 (scene layer and pixel-exact picking)

- **`Scene`, `Entity`, `ComponentStore`** — the entity layer the physics and
  audio work kept asking for. A component is any struct; there is nothing to
  register. Stores are dense arrays plus an owner array and an index map, so a
  system walking one component type walks contiguous memory, and removal is a
  swap with the last element.
- **Generational handles.** An `EntityId` packs a 20-bit slot index and a
  12-bit generation. Destroying an entity bumps its slot's generation, which
  is what makes every outstanding handle to it stale -- including ones stored
  somewhere the scene cannot see. The alternative, a raw index, silently
  starts referring to whatever entity reuses the slot.
- **`Scene::StepPhysics`** joins the two directions: transforms that *drive* a
  body are pushed in before the step, bodies that drive their transform are
  read back out after. `RigidBody2DComponent` holds only the body index, so
  `PhysicsWorld2D` still knows nothing about entities and could be swapped for
  Box2D without the scene noticing.
- **Pixel-exact picking** through the framebuffer's `RED_INTEGER` attachment.
  The attachment and `ReadPixel` were built weeks ago and had never returned
  anything meaningful; the scene layer is what finally gave them something to
  name. The fragment shader writes colour and entity ID in the same pass, so
  picking costs one `glReadPixels` of one pixel and no geometry maths -- it is
  right for rotated, overlapping and irregular shapes that a bounding-box test
  gets wrong.
- **The buffer stores the slot index, not the handle.** A full `EntityId` does
  not survive a round trip through a *signed* 32-bit integer texture: once the
  generation passes 2047 the handle exceeds `INT_MAX` and reads back negative.
  `Scene::EntityAtIndex` turns the slot back into a live handle, and returns
  `InvalidEntity` if that slot is now empty.
- **`Texture2D::CreateFromHandle`** wraps a GL texture this engine did not
  create -- a framebuffer attachment -- so the offscreen result can be blitted
  with the ordinary `DrawQuad` path. It does not own the handle and its
  destructor leaves it alone; deleting it would pull the texture out from
  under the framebuffer.

Verified numerically rather than by eye: the demo projected all eleven sprite
centres to pixels and read the attachment back at each. **11 entities named
correctly, empty space returned -1, 0 mismatches.** Screenshot comparison was
tried first and was, as usual here, less reliable than the numbers.

### 2026-08-02 (3D transform gizmos and a point light)

- Cube3D's light became a **point light** with inverse-square falloff and a
  linear fade to its range. A directional light has no position, so there was
  nothing for a gizmo to drag.
- **A translate gizmo** — X red, Y green, Z blue — attached to either the cube
  or the light, chosen from the panel. It rests on two pieces of maths: a
  **ray through the cursor** (a mouse position is a line in 3D, not a point),
  and the **closest point between that ray and the axis line**, which turns
  "where the cursor is" into "how far along X". Grabbing stores where on the
  axis you took hold, so the object keeps its offset instead of snapping.
- **Middle-drag mouse look**, driven by mouse *delta* tracked every frame
  whether the button is held or not — otherwise the first frame of a drag
  jumps by however far the cursor moved while nobody was looking. Deliberately
  not scaled by the timestep: the mouse has already moved a real distance.
- Transform fields for position, rotation and scale, and a single cube by
  default rather than a grid.

Verified against a known camera pose rather than by dragging, because the
window-automation coordinates here do not match what the application reads:
+200px gives t = +1.38, -200px gives -1.38, centre is 0, and world-to-screen
round-trips to within 0.0004 pixels.

**A hazard introduced by the DemoLayer refactor showed up here.** A bad edit
deleted `OnDemoUpdate` entirely and it *compiled clean* -- the base class has
an empty default, so removing an override is silent. The only symptom was a
black screen and `Layer::OnUpdate` reading 0.001ms in the profiler.

### 2026-08-02 (light collision)

- **`PhysicsWorld2D::ResolveCircle`** pushes a circle out of anything it
  overlaps, reusing the existing narrowphase via a throwaway probe body rather
  than a second copy of circle-vs-box. Useful for anything that moves itself:
  characters, cameras, a light you can drive.
- The lighting demo's light now sweeps and slides instead of passing through
  walls. Both control modes share one mover, which is why the mouse needed no
  special case -- "follow the cursor" is just a delta towards it.
- Two passes: travel until contact, then spend the remainder along the surface
  with the into-wall component projected out. Without the second, hitting a
  wall at an angle stops you dead instead of sliding.

Verified: nine targets, driving the light at every wall and diagonally into
all four corners with steps far larger than any real frame, 200 steps each.
**0 escapes, 0 left overlapping.**

Teleport-to-cursor was considered and rejected. Sweeping never gets stuck --
every move starts from a valid position and `ResolveCircle` handles the rest
-- and teleporting would let light cross a wall, which breaks the premise of a
demo about walls blocking light. A "Collides with walls" toggle allows
comparing the two.

### 2026-08-02 (build wrapper)

- **`egss.py`** — `build`, `run`, `clean`, `gen`, with `all` for every config.
- It **always regenerates** project files. That costs 0.21s, the same as a
  no-op build, and in exchange the most confusing failure in the project
  cannot happen: premake expands its file globs at *generation* time, so a
  newly added `.cpp` is invisible until they are regenerated, and the symptom
  is an undefined-symbol error for a function plainly sitting in the file you
  just wrote. Demonstrated both ways before committing to it.
- `run` launches from the binary's own directory, because the executable reads
  and writes `imgui.ini` and `profile.json` relative to the working directory.

### 2026-08-02 (demo scaffolding)

- **`DemoLayer`** — demos override `OnDemo*` hooks; the `Layer` entry points
  are `final` and hold the is-this-demo-active guard. That guard used to be
  hand-written four times per demo, and forgetting it was not a compile error
  but a demo drawing over another, or a looping sound playing under a demo
  that never started it. Both happened.
- **`OnDemoActivated` / `OnDemoDeactivated`** for continuous things. `OnAttach`
  runs for *every* pushed layer whichever demo is showing, which is exactly
  how those looping sounds escaped.
- **`DemoRegistry.h`** — one table holding name, short name and factory
  together. Adding a demo went from five edits across three files to one line.

Self-registration via static initialisers was deliberately not used: these
demos are header-only, so the initialiser only runs if some translation unit
includes the header, and forgetting that include would produce no demo, no
entry and no compile error.

### 2026-08-02 (light blending and lit surfaces)

- **`BlendMode`** (alpha / additive / none) and **`SetDepthTest`** on
  `RendererAPI`.
- Multiple lights now blend. The bug was not blending but **depth**: every
  light polygon sits at the same z, and the depth test rejects fragments at
  equal depth, so the second light was discarded exactly where it overlapped
  the first. Additive blending cannot help if the fragments never arrive.
- Surfaces are lit by raycast rather than drawn flat — one ray per light at
  each body's centre, asking whether the *first* thing it meets is that body.
  Aiming at the centre works even for a wall, whose centre is inside itself:
  the ray hits its near face, which is the face being lit.
- Light polygons overshoot their hit slightly so the light laps onto the
  surface it stopped at. Landing exactly on the surface leaves it unlit.

### 2026-08-02 (2D lighting: visibility polygons)

- **`Renderer2D::DrawTriangle`** (flat or per-corner colour) and
  **`DrawCircle`**, both in one batch — any number costs one draw call. The
  per-corner overload is what gives a light its falloff for free.
- **`RigidBody2D::MakeStaticCircle`**, symmetric with `MakeStaticBox`.
- A **visibility-polygon light**: rays cast at obstacle corners, sorted by
  angle, with the gaps between neighbouring hits filled by triangles. The
  ±0.0001 offsets either side of each corner are essential — a ray aimed
  exactly at a corner stops there, and the nudged pair slip past it to draw
  the shadow's edge.
- **Circle silhouettes via tangent rays.** A circle has no corners, so it
  blocked rays but never attracted one, and its shadow was whatever the ring
  rays happened to graze. `base = atan2(toCentre)`, `half = asin(r / d)`, then
  four rays: an outer pair for the shadow and an inner pair so the lit
  crescent is not clipped by a chord. Guarded against the light being *inside*
  the circle, where `asin` leaves its domain and a NaN poisons the angle sort.
- **Ring rays scale with radius.** The rim's sag from a true circle is
  `R x (1 - cos(pi/N))` — proportional to R — so a fixed count is correct at
  one size only. That was the "square shadows at a distance" symptom.
- Three control modes for the light, cycled with **M**, including mouse with
  a screen-to-world unprojection.

Measured: 32 ring rays with 239 total gave the same shadow quality as 8 with
206. Fewer, better-aimed rays beat more evenly-spread ones, which is the whole
argument for a visibility polygon over a fan.

### 2026-08-02 (uniform-grid broadphase)

- **`PhysicsWorld2D` now buckets bodies into a uniform grid.** Pair generation
  visits only bodies sharing a cell; raycasts walk the grid cell by cell (DDA)
  instead of testing everything, and stop as soon as the nearest hit is closer
  than the next cell.
- A per-body query stamp does the de-duplication, so a body spanning several
  cells is tested once per query without allocating a set.
- `UseBroadphase` and `CellSize` are left switchable, because the only reason
  to build this was to be able to measure it rather than assume it.
- Falls back to brute force where the grid cannot help: an empty world, a cell
  size that would need too many cells, or a ray starting outside the grid.

Correctness first: **2912 rays across the map, grid versus brute force, 0
mismatches and a worst distance delta of 0.0.** Identical results, not merely
similar ones.

Then the scaling, Release, 4000 rays per run:

| bodies | brute force | grid | speedup |
| --- | --- | --- | --- |
| 16 | 1333 us | 707 us | 1.9x |
| 64 | 3663 us | 784 us | 4.7x |
| 256 | 12611 us | 1053 us | 12.0x |
| 1024 | 46004 us | 2424 us | 19.0x |

Brute force grows with body count as expected; the grid barely moves — 64x the
bodies costs it 3.4x the time.

**A measurement mistake worth recording.** The first attempt compared the two
modes live and found *no difference at all* — both 0.4 us per call. That figure
was the profiler: `Raycast` carried an `EGSS_PROFILE_SCOPE`, and a scope timer
costs more than a raycast against a small world, so the instrumentation was
most of what was being timed. Removing the per-call scope revealed a real 1.9x
that had been invisible. Profile the loop that issues the rays, not the leaf.

### 2026-08-01 (reverb zones)

- **`AudioEngine::SetReverb`** with wet, room size, damping and width, applied
  to the master bus. A Schroeder arrangement: four damped comb filters in
  parallel into two allpasses in series, per channel, with Freeverb's delay
  lengths scaled from 44.1kHz to 48kHz.
- The delay lengths matter more than they look. Lengths sharing common factors
  make echoes line up, which is heard as a ringing tone rather than a room;
  the right channel's are offset by 25 samples, and that offset is the whole
  of the stereo image.
- Damping is a low pass **inside** each comb's feedback path, so every trip
  round the delay loses a little more top end -- which is what real rooms do,
  and what stops the tail sounding metallic.
- Settings crossfade over ~0.35s, so a "zone" is just a region the game tests
  the listener against. It needs to know nothing about fading. When the wet
  level reaches zero the effect is skipped entirely, so a game with no reverb
  pays nothing for it.
- Buffers are allocated once in `Init`; the audio thread never allocates.
- Physics2D has a zone rectangle that lights up when the listener is inside.

Verified numerically: dry gives a tail energy of exactly **0** (the bypass is
clean), a room size of 0.85 gives **106.2**, and 0.40 gives **49.1** -- a
bigger room, a longer tail.

That test first reported zero tail for every setting. The reverb was fine: the
shortest comb delay is 1214 samples and I was measuring the first 1024, so no
echo had arrived yet. Measuring past the delay line was all it needed.

### 2026-08-01 (occlusion)

- **`AudioEngine::SetVoiceOcclusion`** — 0 is a clear line, 1 is fully
  blocked. Drops the voice to 30% gain *and* rolls its top end off with a
  one-pole low pass (18 kHz down to 480 Hz). Attenuation alone just sounds
  further away; losing the high frequencies is what sells "behind a wall".
- Changes are smoothed over ~50 ms, so a ray flicking across an edge does not
  click. `VoiceDebug` reports both requested and applied values.
- **The engine deliberately does not raycast for itself.** What counts as an
  occluder is a game question, and the audio system has no business depending
  on the physics one — especially as physics is 2D and audio is 3D. The demo
  casts three rays from listener to source and feeds in the blocked fraction,
  which grades the effect instead of snapping it on and off.
- `GetVoiceDebug` now fills a `VoiceDebug` struct rather than four out
  parameters, which stops the signature growing every time something is added.

Verified numerically, with predictions worked out independently of the code:

| | measured | predicted |
| --- | --- | --- |
| clear DC | 0.70711 | 0.70711 (centre pan) |
| occluded DC | 0.21213 | 0.21213 (0.30x gain, DC passes the filter) |
| clear Nyquist | 0.70711 | unfiltered |
| occluded Nyquist | 0.006662 | 0.00666 (one-pole at 480 Hz) |

The first run of that test looked wrong -- occluded DC read 0.267 against an
expected 0.212. It was right: the smoothing had only reached 0.888 of the way,
and 0.7071 x (1 + (0.3 - 1) x 0.888) is 0.267 exactly. The exponential settles
to 1 - 0.9467^n per block, which is precisely what it did. The test now renders
long enough to converge.

### 2026-08-01 (positional audio)

- **`AudioListener`** — position, forward, up and velocity. `PerspectiveCamera`
  already exposes the first three in exactly that form, which is not an
  accident.
- **`AudioEngine::PlayAt`** with `Audio3DParams`: world position, velocity,
  min/max distance and a Doppler factor. Gain, pan and pitch are recomputed
  from the listener **on every audio block**, not once at Play, so a moving
  source or a turning listener is heard immediately without the caller doing
  anything.
- **`VoiceHandle`** with a generation counter, so a handle to a finished
  one-shot cannot control whatever sound later reused its slot. `Stop`,
  `IsPlaying`, `SetVoicePosition/Volume/Pitch`, and `GetVoiceDebug` for
  showing why something sounds the way it does.
- Attenuation is inverse-distance multiplied by a linear fade, so it actually
  reaches zero at `MaxDistance`. Pure inverse-distance never does, and distant
  sources pile up and muddy everything.
- Voice parameters that can change mid-playback are individually atomic; the
  mixer still takes no locks.
- Cube3D puts two looping emitters in the world with the listener riding the
  fly camera — the only way to demonstrate this, since it needs movement.
  Physics2D positions its impacts and has a draggable listener.

**A bug worth recording.** Every spatial reading came back as uninitialised
garbage. The cause was in handle packing: I OR'd `0x80000000` in as a
"never zero" marker, but the generation field occupies bits 8-31, so the
marker landed inside it and *every* handle failed its own generation check.
The marker was never needed — generation starts at 1, so a valid handle is
always at least 0x100. My test also deserved blame for printing uninitialised
floats instead of checking the returned bool; it now fails loudly.

Verified numerically with the device stopped: a source inside MinDistance is
at full gain and centred; one 3 units to the right reads gain 0.25926
(= (1/3) x (1 - 2/9)) and pan +1.0, mirrored exactly on the left; beyond
MaxDistance the output is silent; a source receding at 34.3 m/s gives a pitch
ratio of 0.90909 (= 343/377.3) and approaching gives 1.11111 (= 343/308.7);
and a stale handle reports not-playing. All exact.

Live check against the geometry: with the camera at (0, 1.6, 6), emitters at
(-1.6, 0, 0) and (1.6, 0, -1.6) report 6.41m / 7.93m — matching their true
distances — with pan -0.25 and +0.20, and gains matching the attenuation
formula to two decimal places.

### 2026-08-01 (audio playback)

- **miniaudio vendored** as a single header plus one C translation unit. It is
  compiled as C, so premake marks `**.c` `NoPCH` — handing a C file the C++
  precompiled header does not end well.
- **`AudioEngine`** on a raw `ma_device` with a mixer of our own rather than
  miniaudio's higher-level engine, because per-voice gain, pan and pitch are
  exactly what positional audio will need to drive. 32 voices, f32 stereo at
  48kHz, equal-power panning, linear interpolation for fractional pitch.
- **`AudioClip`** decodes to the engine's format *on load*, so the mixer never
  resamples on the audio thread. `CreateFromSamples` covers procedural audio.
- **No locks.** The mixer runs on a device-driven thread that must never block
  or allocate, so voices are claimed with atomics: the main thread only writes
  to an inactive voice and publishes with a release store, and the audio
  thread only touches voices it acquired as active. A finished voice releases
  its slot but never drops its clip reference — freeing memory in the audio
  callback is the same class of mistake as taking a lock there.
- A missing device is not an error: `Init` warns, `IsAvailable` returns false,
  and everything else carries on silently.
- Both demos use it. Physics impacts play from the contact's accumulated
  normal impulse — which is precisely "how hard did these two hit" — and only
  for *new* contacts, since a resting body has a large normal impulse every
  step and would otherwise buzz continuously. Breakout pans its bounces by x.

Verified with `RenderForTest`, which runs the mixer into a buffer with no
device attached: centre pan gives 0.35355 in both channels (cos(pi/4) x 0.5),
hard left gives 0.5 and 0.0, doubling the pitch consumes 100 source frames in
50, two voices sum to 0.70711, clips free their voice on completion, and
StopAll clears them. All exact.

The first run failed one case — pitch showed 0.8536 instead of 0.3536. The
mixer was right and the test was wrong: a 100-frame voice from the previous
case was still playing after only 50 frames had been rendered, and 0.5 + 0.3536
is exactly what should come out. The test now clears voices between cases.

### 2026-08-01 (physics raycasts)

- **`PhysicsWorld2D::Raycast`** returning nearest hit: body, point, normal,
  distance and fraction along the ray. Ray-circle solves the quadratic;
  ray-box uses the slab method and reports which face was crossed. The
  direction is normalised internally, so a plain "to minus from" vector works,
  and an `ignore` handle skips the caster.
- A ray starting inside a body reports distance 0 with the normal facing back
  down the ray — there is no correct answer there, but this one never hands
  back a zero-length normal.
- The `Physics2D` demo draws a 360-degree ray fan with normals at each hit,
  which is the same query audio occlusion will make.

Verified headlessly against known geometry — both box faces, a circle behind
an ignored body, misses, a too-short ray, an unnormalised direction, and an
origin inside a body. All seven exact.

Cost, Release, 96 rays against ~100 bodies: **0.069 ms per frame**, about 0.4%
of the frame and roughly the same as the whole physics step. Raycasting is
O(rays x bodies), so it — not collision pairs — is what would eventually
justify a spatial broadphase.

### 2026-08-01 (profiler)

- **`Instrumentor`** — RAII scope timers behind `EGSS_PROFILE_SCOPE`, with two
  outputs: a live per-frame summary, and a Chrome trace written to JSON for
  `chrome://tracing` or `ui.perfetto.dev`. `EGSS_PROFILE` is defined in Debug
  and Release; Dist compiles the macros to nothing.
- Instrumented the frame loop, the fixed-update loop, ImGui, the buffer swap,
  `Renderer2D::Flush`, and each phase of `PhysicsWorld2D::Step`.
- **`ProfilerPanel`** in `TestEnv` shows the live table and captures traces.

The live view exists because VSync makes the frame counter useless: the swap
blocks until the display is ready, so a frame doing 2ms of work and one doing
12ms both report 16.7ms.

What it immediately settled:

- **Debug numbers are worthless for tuning.** Physics went from 2.456 ms per
  step in Debug to **0.118 ms** in Release — 21x. The broadphase alone went
  from 1.257 ms to **0.011 ms**, a factor of 110.
- **The O(n^2) broadphase does not need replacing.** At ~150 bodies it is
  0.011 ms, roughly 0.06% of the frame. It stays on the roadmap, unstarted,
  which is the right outcome — that decision was going to be a guess.
- **Attributing GPU wait needs care.** Between Debug and Release the swap wait
  fell from 5.6 ms to 0.4 ms while `Layer::OnUpdate` doubled: with VSync the
  driver blocks wherever it likes, often inside a GL call. Scopes that touch
  no GL are trustworthy; the rest are work plus an unknown wait. The panel
  says so rather than implying a clean split.

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
