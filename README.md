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

### Sanitizers

```sh
./egss.py sanitize              # build instrumented, run every demo under it
./egss.py sanitize release      # the config where UB actually bites
./egss.py build --sanitize      # just the build
./egss.py run --sanitize -- --demo OpenWorld
```

`--sanitize` is a **generation** option rather than a fourth configuration, so
it composes with all three: it adds `-fsanitize=address,undefined` to the two
first-party projects and builds into `bin/<Config>-<system>-x86_64-sanitize/`.
The separate tree is the load-bearing part — make cannot tell that the flags
changed, so instrumented and plain objects sharing a directory would link
whatever was there.

The sweep runs each demo in its own process, in lockstep with the window
hidden, and reports per demo. It reads the demo list out of `DemoRegistry.h`, so
adding a demo adds it to the sweep. Leak detection is left **on**: it was
measured rather than assumed, and 300 steps of all thirteen demos report
nothing, so there is no driver noise here to suppress.

Instrumented costs about **2.1x to build** and **5.3x to run**: a cold debug
build is 1 m 05 s against 2 m 20 s, and 300 lockstep steps of Physics3D are 6.1
to 6.3 s against 32.5 to 33.0 s. That is why it is a command you reach for
rather than something the normal build does.

### The wallpaper, and the window bridge

```sh
./egss.py run release -- --demo Slime --wallpaper
./egss.py windows        # in another terminal: react to open windows
```

`--wallpaper` marks the window `_NET_WM_WINDOW_TYPE_DESKTOP` and sizes it to the
union of every monitor. `--wallpaper-scale N` and `--wallpaper-density F` trade
detail against cost; `--show-ui` puts the panels back so the breeds can be tuned
while it runs; `--no-windows` ignores the desk even when the bridge is up.

`./egss.py windows` is the KWin bridge described in the 2026-08-21 changelog
entry. It runs until interrupted and unloads its KWin script on the way out. It
is not required: without it the colonies simply do not know about your windows.

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
[22:00:24] EGSS: GL: 32 fragment texture units reported, using 32
[22:00:24] EGSS: Renderer2D initialized (10000 quads/batch, 32 texture slots)
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

The texture-slot line is a driver query, so the number varies by machine. 16 is
the OpenGL 3.3 floor and the smallest you should ever see; this machine reports
32, which is how many distinct textures one batch can hold before it flushes.

Pass **`--viewports`** to let panels be dragged out of the window into their
own OS windows. Off by default — each one is a real window with its own GL
context.

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

- [x] **Query `GL_MAX_TEXTURE_IMAGE_UNITS`** at runtime rather than assuming
      the 16-slot floor, and generate the sampler switch to match — done, and
      this driver reports 32, so a batch now holds twice the textures
- [ ] ~~**A PCH for `TestEnv`.**~~ **Declined on measurement.** `TestEnv` is
      one translation unit, and 67% of its compile is codegen, which a PCH
      cannot touch. Building the PCH costs more than it saves. See the
      changelog entry for the numbers; reopen this only if the demos ever
      become separate `.cpp` files
- [x] **Vendor a `premake5` binary per platform**, or script fetching it —
      `egss.py` now fetches a pinned, checksummed premake on first use
- [x] **Replay: record the ImGui panel state too** — a demo registers the
      parameters that reach its simulation, one line per slider, and the
      recorder samples them per fixed step beside the input. Format version 2:
      a named table in the header and tagged chunks in the body. Verified by
      recording a session whose sliders move mid-run and replaying it to a
      byte-identical frame, with the unregistered control diverging
- [x] **Multi-viewport ImGui** — done, behind `--viewports` /
      `ImGuiLayer::EnableViewports`. Off by default: every undocked panel is a
      real OS window with its own GL context
- [x] **`.gltf` loading** — a written-not-vendored JSON parser, the accessor
      machinery (stride, sparse overrides, normalised integers, both container
      formats), a node hierarchy that composes, and materials with textures.
      Verified by mutation: 62 checks passed first try, six deliberate bugs
      injected, all six caught. The Model demo shows a jointed figure built
      from one 24-vertex cube referenced by twenty nodes
- [x] **Distant-chunk LOD** — OpenWorld picks a stride per chunk from its
      distance (24 m to stride 2, 48 m to stride 4) with an 8 m hysteresis
      band, and budgets the remeshing like streaming. Triangles fall as
      1/stride² (measured 4.08x and 16.09x against 4 and 16); see the
      2026-08-18 entry for the current triangle-count picture, which changed
      with the mesher swap below.
- [x] **The cross-stride LOD seam** — `VoxelTransition` closes it: 64 holes
      to 0 on a curved test surface, verified against a mutation reproducing
      the two earlier reverted attempts' bug. Chunk corners needing the fix
      on two faces at once are out of scope and keep the old seam there —
      smaller and rarer than before, not silently dropped. Closing it
      surfaced that `MarchingCubes` and `MarchingTetrahedra` disagree on any
      shared face, LOD or not (102 holes, measured), so OpenWorld now meshes
      all terrain with `MarchingTetrahedra` — **~8.4x more triangles at the
      same settings (81,413 → 684,718 at 128 m), not yet offset by anything.**
      See the changelog entry for what was deliberately left: forcing
      `MarchingCubes`'s ambiguous cases to agree instead (keeps the triangle
      count down, its own research-grade task), and dedicated verification
      of the 4:1 stride case — **the 4:1 case is now verified** (96 open edges
      to 0, and 108 left open by a deliberately one-level-short subdivision);
      forcing `MarchingCubes`'s ambiguous cases to agree stays declined, now on
      a measurement: the swap costs 0.33 ms a step, and 3.4x of the recorded
      8.4x was the LOD bands rather than the mesher
- [x] **Chunk persistence** — `VoxelField3D::SaveChunk`/`LoadChunk` plus a
      demo-owned append-only cache file, keyed by a fingerprint of the density
      function so a changed world discards a stale file automatically. 6.21 s
      cold against 4.16 s warm over 600 steps at a 128 m radius, and the frame
      is byte-identical between them. 2.8 MB for 951 chunks, because a chunk
      that never nears the surface is uniform and stores as six bytes
- [ ] **glTF: skinning and animation.** Joints, weights and samplers have
      nowhere to be played back to until something poses a skeleton — parsing
      them now would be a component with no system
- [ ] **Gizmo: rotate and scale handles.** Translate works; rotation rings and
      scale boxes are the same picking maths applied to different geometry
- [ ] **Physics: joints, and shapes beyond boxes and circles.** Rotation is
      done — the narrowphase runs `Sat2D`, contacts carry a lever arm and the
      solver has its angular terms. What is still missing is anything that
      constrains two bodies other than contact, and any collider that is not a
      box or a circle. Convex hulls would reuse the SAT that is already there;
      only the axis list changes
- [x] **3D physics: a broadphase** — a uniform grid, bit-identical to brute
      force and switched on automatically above 200 bodies, because below that
      it measurably loses. Bodies too large to be worth bucketing are kept out
      of the cells and out of the extent, without which a scene with terrain in
      it made the grid *eleven times slower* than no broadphase
- [x] **Give a heightfield bounds that describe the solid, not the surface** —
      `outMin.y` is `-infinity`, matching a narrowphase that is solid to any
      depth (verified to 1000 m, and on all three shape paths, against
      `r + drop*cos θ`). The grid keeps a second, finite box (`IndexBounds`) and
      lowers such a body to the grid's own floor before stamping, which fixed a
      real divergence from brute force under
      `BroadphaseExcludeOversized = false`. It is *not* the lever that lowers
      `BroadphaseMinBodies` for terrain: a bounds test would still reject almost
      nothing, since everything standing on a map is inside the map's box. What
      would pay there is a cheap vertical reject — the highest sample under a
      body's footprint, which a coarse max-pyramid over the field could answer
      without touching a triangle
- [x] **3D physics: shapes beyond boxes and spheres** — capsules are in, with a
      two-point manifold against boxes so one rests flat. Convex hulls are the
      next shape worth having, and would reuse `Sat3D`
- [x] **3D joints** — ball-and-socket, hinge, angle limits, cone-and-twist, and
      motors. Enough to build a ragdoll that falls convincingly and one that
      holds itself up
- [x] **A humanoid rig** — thirteen jointed bodies with limits, in the Ragdoll
      demo, passive or powered
- [x] **Balance: the measurement and the ankle strategy.** Support polygon,
      centre of mass, capture point, signed margin; two-axis ankles holding the
      figure upright for 35 seconds unpushed, against one or two before
- [x] **A controllable character** — kinematic pelvis, walk cycle, and a switch
      to full ragdoll on a hard enough hit
- [x] **Getting up, terrain, and stepping over things** — a blend rather than a
      snap, a root that follows the ground, and a leg that lifts higher for a
      step it can see coming
- [x] **Feet that plant** — the gait describes the *foot* now and the leg is
      solved to reach it, so a stance foot stays where it landed
- [x] **Footsteps that land on something** — a planted foot now reports **4.53**
      contact points, up from 1.84, so the support polygon has three or more
      whenever a foot is down. This was written off as needing a speculative
      margin in the narrowphase; it did not. A real stance fraction and a sole
      flat to 0.9° got there on their own
- [ ] **Stepping, if it is ever wanted.** Self-balancing is no longer on the
      critical path for a playable character — the kinematic root removed the
      need. Left documented because the diagnosis is complete and someone may
      want it: the step does not widen the support polygon, and that one fact
      is where any further work starts
- [ ] **A second character, and pushing.** Once one figure balances, the second
      is the same rig with a different driver; shoving is `ApplyImpulseAt`,
      which already works
- [x] **3D physics: a heightfield collider** — static, never turned, with
      sphere/capsule/box narrowphase against the triangulated surface and an
      exact `GroundHeightBelow`. Verified against `tan θ` on a slope to 1.4%
- [x] **Generated terrain, and a character walking on it** — Map Building is a
      subclass of Ragdoll rather than a second rig, and the gait needed no
      changes because it already asked the world where the ground was

- [ ] **Partitioned FFT convolution.** The convolution reverb takes a *sparse*
      response, which is what the ray tracer produces. A dense recorded impulse
      is 96,000 taps a sample and needs overlap-save with an FFT
- [ ] **More than three bands, and per-material band curves.** Three is enough
      to hear bass outlast treble; real materials are measured in octave bands
      and a soft surface's curve is nothing like a hard one's
- [x] **3D acoustics** — `Acoustics3D` traces the *scene*, through `Raycast3D`,
      so the room the sound bounces off is the geometry you can see. Verified in
      a shoebox against formulas it does not contain: mean free path within 0.40%
      of 4V/S, RT60 within 9% of Eyring across three absorptions. The same room
      traced in 2D over-predicts the mean free path by 1.73× and RT60 by 1.69×,
      which is what the missing floor and ceiling were worth. Cube3D is the
      consumer, with a default-off enclosure so there is a room to hear
- [x] **Voxel terrain, piece one: the field and the mesher** — `VoxelField3D`
      (chunked signed distance field, material per voxel) and `MarchingCubes`.
      Verified against 4πr² and (4/3)πr³, closed-manifold topology, seamless
      chunk boundaries, and a shape no heightfield can hold
- [x] **Voxel terrain, piece two: the collider** — `ColliderShape3D::Sdf`, with
      sphere, capsule and box against the field directly rather than its
      triangles. Bodies rest at exactly one slop, a rolling sphere reports its
      own inertia as 0.4102 against 0.4, and the same terrain as a heightfield
      and as a distance field puts the ground within 0.00009 m over 121 points.
      `GroundBelow` marches the column, so it answers correctly under an arch
- [x] **Voxel terrain, piece three: the demo, editing and islands** — a Voxel
      terrain demo with 3D-noise generation, chunked meshing, digging, and
      severed rock that falls. Edits are local (1 chunk inside, 2 on a seam, 0
      for a miss), islands conserve exactly (19,208 + 147 = 19,355), and cutting
      a capped pillar's neck frees exactly the cap. Islands are approximated by
      one box each, which is what convex decomposition would replace
- [x] **Voxel terrain, piece four: tension** — `VoxelStress` routes load to the
      anchors, groups coplanar connections into sections and fails them above a
      stress. Checked against a cantilever's `3 rho g L^2 / h` (the residual is
      exactly `1 + s/L`, one voxel of length, and the thickness dependence is
      exact) and against a column's height limit `sigma / (rho g)` — stands to
      14 m, fails from 15, predicted 14.56
- [x] **Voxel stress: make the pass cheap, and take fewer of them** — 31.33 ms
      to 20.87 ms by caching solidity into a flat array, and 180 ms to 29 ms an
      edit by breaking several independent sections from one analysis.
      *Analysing a region rather than the map was the original plan and is
      wrong*: a section's load comes from everything routed through it, most of
      which is outside any box drawn around the edit
- [x] **Voxel stress: sections judged by a cut test, not a threshold** —
      bending belongs only to a section that is the sole path to ground, which is
      tested by blocking it and walking for an anchor. `MinSectionLinks` is back
      to 1. Affordable because the test can only lower a stress, so it is only
      run where the answer could change the verdict: a pass is still 21 ms
- [x] **Voxel islands: an oriented box** — the piece's own principal axes, from
      the covariance of its voxels. On a slab tilted 35 degrees the box is 79%
      rock against an axis-aligned box's 27%, and the tilt comes back to within
      half a degree
- [x] **Voxel islands: convex decomposition** — `ColliderShape3D::Compound`
      (several boxes rigidly fixed, every existing box test reused by treating a
      child as a box body) plus greedy box growing over an island's voxels. An
      L-shaped piece of 2,695 voxels becomes 2 boxes that tile it exactly, where
      one box round it is 60% air. Inertia matches the analytic tensor to six
      figures including the off-diagonal terms
- [x] **A landing that is not in the sea** — the approach asks the planet for
      the nearest dry ground rather than aiming at the sunward point and hoping,
      which on a planet that is 29.2% land failed seven times in ten. The
      clearance it promises is 10 m because that is where two independent probe
      sets stop agreeing about the shoreline (100% of directions at 3 m, 98.8%
      at 10, 92.7% at 28), and the arrival height is the ground at the site plus
      20 m rather than a multiple of the mean radius. 15 checks, four mutations
      caught
- [x] **Rings for Saturn and Uranus** — one annulus mesh with the radii as
      uniforms, banded by 1D noise of the radius alone, with the Cassini
      division, the planet's shadow, the opposition surge, and the negative
      seen from the shadowed side. The tilt is on the body now, not the ring —
      see the axial-tilt changelog entry — though on two planets with opaque
      atmospheres it was always a difference nothing could observe
- [x] **Body scale, and gas giants with no ground under the air** — a second
      exponent for everything local to a body (`q = 3/4`, against `p = 1/2` for
      heliocentric distance) puts Jupiter at 6.03 Earths instead of 3.3 and
      improves every clearance in the table; deep, dense atmospheres put 5.9
      optical depths over a surface that should not be there. Needed
      `BlendMode::Premultiplied` in the engine, a multiple-scattering term, and
      a step count set by the scale height
- [x] **An hour to the day, 365 hours to the year**, and a craft that turns with
      the air it is hovering in — the second only became reasonable once the
      first made the frame turn a tenth of a degree a step instead of six
- [x] **Axial tilt, and therefore seasons.** A general spin axis through
      `ToScene`/`ToFixed`, `SpinMatrix`, the co-rotating air and the rings,
      tilted about the same reference `SkyDirection` already anchors the star
      catalogue to. Retrograde is no longer a negative period: Uranus (97.77°)
      and Venus (177.4°) turn backward, as seen from the ecliptic pole, purely
      because their tilt passes 90° — verified against `cos(tilt)`, a formula
      the rotation code does not otherwise compute, sign included. See the
      changelog for the map-lookup and ring-basis fixes tilt required
- [ ] **A moon's orbit in its planet's equator**, once there is an equator to
      speak of. Moon orbits are in the ecliptic here, so Titan does not go round
      Saturn the way the rings do
- [x] **A starfield** — 44 real stars at J2000 coordinates through Earth's
      obliquity into the ecliptic, a procedural field of about 5,000 behind
      them, and the Milky Way on the real galactic pole. Verified against
      published ecliptic latitudes to 0.003 degrees and against Orion's belt.
      The stated reason for wanting it was wrong, though: stars are at infinity
      and give no parallax, so they are an *orientation* reference and never a
      speed one — see the changelog
- [x] **A planet the size of a planet** — both exponents at one, so every ratio
      in the system is true, and Earth at 250 km: a 3.92 km atmosphere, 2,216 m/s
      to escape it, a 949 m horizon and a Sun half a degree across. Needed a
      sparse chunk store in the engine (the dense one wanted 1.1e10 GB at true
      scale), a 21-bit chunk key, a relief spectrum with a local layer on top,
      and distance proxies so one depth buffer covers 0.15 m to 4.5e12 m
- [x] **A local origin for the surface** — `PositionFrom` and
      `SampleDistanceFrom` subtract on the integers, chunk meshes carry their
      own origin, plants carry their chunk's, and the surface physics world is
      centred on the landing site's lattice point. Placing a mesh vertex at 1:1
      went from 0.571 m of error — 36% of a voxel — to a micrometre, and
      `--earth-radius 6371000` now streams, renders and can be stood on, with
      escape velocity within 0.014% of the real 11,186 m/s. Cost two false
      starts, both caught by OpenWorld: rounding a field origin that is
      deliberately half a voxel off (13.5% of its pixels) and re-associating a
      float addition (34 of them)
- [x] **Sample the density in double, which is the other half of 1:1** —
      `PositionOfFixed`, a `dvec3` through `Fill`/`FillChunk`, and a
      `DensityFixed` that takes `|p| - Radius` in double. Measured against the
      generator's own exact zero (`Radius + Relief(d)`, arithmetic the chunk
      store and the reconstruction know nothing about): the surface error at
      1:1 falls from 0.1673 m mean / 0.8509 m worst to 0.0121 / 0.1247, against
      a hand-computed bound of 0.866 m. At the default 250 km scale it changes
      the frame by 1.7 of 255, which is what it should. Fixing it exposed that
      the cache fingerprint sampled the generator rather than the fill, and
      that it had been hashing four bytes of uninitialised stack since it was
      written — see the changelog
- [x] **The terrain shader's height, the GPU half of the same thing** — each
      draw carries a reference point near its own geometry, so the shader forms
      `world.xyz - u_Reference` (small, exact) instead of a planet-centred
      `length`, and rebuilds the height from a double-computed reference
      altitude plus `(2|C|(n·e) + e·e) / (|C+e| + |C|)` — an identity, so the
      horizon mesh's fourteen-kilometre offsets are fine. Predicted from the
      float format that `fract(height)` could take exactly 2 distinct values at
      1:1 and exactly 64 at 250 km; counted 2 and 64, and 256 after. The height
      was carrying 1.145 m of error at 1:1 and 0.047 m at 250 km. It changes
      1,520 pixels by one level, because the only band narrow enough to notice
      is the 4.78 m beach and neither captured view has a single pixel of
      shoreline in it — see the changelog
- [x] **Instance the trees** — a landed frame went from about 23,000 draw calls
      to 1,001, and from 16.66 M triangles to 9.49 M once trees behind the camera
      stopped being submitted. Needed instancing in the engine: a divisor on
      `BufferLayout`, `mat4` as a vertex attribute (four locations, which the
      enum offered and the backend rejected), `DrawIndexedInstanced`,
      `Mesh::SetInstanceBuffer` and `Renderer::SubmitInstanced`. CPU per frame
      halved
- [x] **Tree level of detail** — three levels per shape, chosen per tree from
      distance over its own scale, which took the landed frame from 26.30 ms of
      GPU to 15.74 and under the 60 fps budget. Levels 0 and 1 differ only in
      tessellation so the near switch is invisible; level 2 is a generation
      shallower and grows its leaf clusters by the cube root of the tips it
      lost. Found by ablation: trees were 6.14 M of the frame's 9.07 M triangles
- [ ] **Merge or LOD the chunk meshes.** 963 of the remaining 1,001 draw calls
      are terrain chunks. They are distinct geometry so instancing does not
      apply — they want merging into larger buffers, or a coarser mesh past a
      distance, which is the same machinery terrain LOD needs anyway
- [x] **Terrain LOD on a sphere** — stride 1/2/4 with hysteresis, scaled by the
      body's voxel size, which took a landed Earth's terrain from 2,774,250
      triangles to 500,825 and the frame from about 16.2 ms of GPU to about
      12.2. Almost no new code: the lattice is Cartesian whatever shape the
      density is, so `VoxelTransition` transferred unchanged. Verified by
      welding the whole resident shell into an edge table and counting the open
      edges *interior* to it — 125 without LOD, 2,943 with, of which 2,642 are
      the documented two-faces-at-once case. Found and fixed a much older bug
      on the way: a mesh reads into seven neighbours, not three
- [ ] **Fix the two-faces-at-once transition**, or stop needing it. It is 90% of
      the LOD seam residual and a sphere hits it three times harder than a flat
      field — 25 of 82 transition chunks on a landed Earth. Invisible today only
      because the horizon sphere is behind the holes
- [x] **Biomes, out of the drainage rather than out of noise** — Priority-Flood
      from the sea, steepest descent, flow accumulation, and moisture and
      warmth derived from the result. Verified by conservation: the water
      arriving in the sea is the land's own area, 0.000% out, with no cell left
      undrained. The colour rule and the tree placement both read the two
      fields where they used to read latitude
- [x] **Editing and edit persistence on a planet** — left mouse digs, right
      fills, and only the chunks somebody changed are stored, so the world
      stays procedural. Verified by carving a hole, discarding all 16,039
      resident chunks of voxels, regenerating from scratch and finding the same
      hole to the centimetre. Found a latent streaming bug on the way
      (`ReleaseBeyond` left the scan watermark past the chunks it released) and
      a `ChunkCache` key that aliased above 512 chunks an axis
- [x] **Water where water can get to** — the sea is drawn from the drainage
      pass's wet mask rather than from an altitude, so 9,978 km² of ground
      below sea level is correctly dry, lakes sit at their own spill heights
      (level to 0.00000 m), and 185 basins too arid to hold one are salt flats
- [x] **Near-field water, at the resolution of the ground** — `SurfaceWater`,
      a 128×128 grid of columns over the streamed region, flooded from its rim
      against the terrain the mesher actually cut. Verified as the requirement
      was stated: a pit 0.75 m below the waterline and 76 m from water stays
      dry, and fills after 74.2 m of channel is cut to it. Every sheet level to
      0.00000 m
- [x] **A ship that is a physical object to get into** — a lander body that
      comes down with you, stands where it landed, and has to be walked back to
      before `L` will lift off. Found a camera that had been one step of
      planetary rotation — 7.3 m — away from the player for as long as walking
      has existed
- [ ] **Water that takes time to arrive.** The fill is a re-flood, so it is the
      right end state and shows nothing moving. A volume-conserving relaxation
      over the same columns — move a bounded amount toward equalising levels
      each step — would turn it into flow without changing what it converges to
- [ ] **Water in a tunnel.** A column has one water surface, so digging *under*
      a ridge does not carry water through it. Wants either a small number of
      surfaces per column or a genuinely volumetric store near the player
- [ ] **Rivers, drawn.** Flow accumulation names every channel, but at 1.5 km a
      texel a "river cell" is a 1.5 km swath. Rivers need either a much finer
      local pass or a channel network carried as geometry rather than as a
      raster
- [x] **Swimming.** A spring toward the surface plus drag while submerged, and
      a screen tint when the eye goes under the local water level. Finding the
      water level under the player surfaced a pre-existing bug in how the
      local flood was seeded — see the changelog
- [x] **Clouds and atmospheric haze**, and the orbital biome speckle that
      turned out to be a missing mip chain rather than a biome-logic bug — see
      the changelog for both
- [x] **A static portal to an empty pocket dimension**, standing in for a
      toolshed — five static colliders, a live offscreen-rendered window, and
      two-way crossing with a carried lateral/height offset. See the changelog
      and `docs/HANDOVER.md`
- [ ] **Place the portal on an arbitrary surface**, as a carried tool rather
      than a fixed landing-site offset. `PocketDimension::Place()` already
      takes position and facing as parameters, so this only changes the caller
- [ ] **Stock the room.** OpenWorld's `Tool`/`m_HeldTool` pickup pattern,
      adapted for a non-radial "up" the same way this pass adapted movement,
      with physics bodies dropped at `RoomLocal()`

- [ ] **Acoustics: a surface is a mesh's bounds, not its triangles.** Fine for
      rooms, which are made of walls, but a sphere currently sounds like the cube
      around it. Wants `Raycast3D` to test triangles for meshes it is worth
      doing so for — which is also the moment it wants a BVH, since the reason it
      is bounds-only is that a torus would go from one test to 2,304

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

### 2026-08-29 (grass that thins around you, not around the map)

The level-of-detail sliders looked dead. Moving them changed something, but not
anything near where you were standing, and walking about did not move the effect
at all.

The distance the shrink is keyed to was `length(world.xyz)` -- **the distance
from the world origin, not from the eye.** The block is centred on the origin,
so the thinning was a fixed bullseye painted on the terrain: tall grass in the
middle of the map, collapsed grass at the corners, whoever was looking and from
wherever. `u_LodFar` is 75 m and a corner cell centre is 76.4 m out, so standing
in one put *every* blade past the far end.

Measured, standing at the corner cell and at the mid-edge cell, as mean
|Laplacian| of luminance over the bottom quarter of the frame -- which is how
much fine detail there is at your feet, and so whether there are blades there at
all:

| spawn | before | after |
| --- | --- | --- |
| corner cell (76.4 m from the map centre) | 2.284 | 6.804 |
| mid-edge cell (56.9 m) | 7.573 | 10.998 |

The per-chunk `u_Fade` sitting three lines away had been measured from the
camera all along, so the *shading* converged with distance from the viewer while
the *geometry* thinned by position on the map -- two level-of-detail schemes
disagreeing about where the viewer was, one of them right.

### 2026-08-29 (the ground was reading a nine-cell grid out of sixteen)

Grass growing on sand, and sand under grass. The grid went from three by three
to four by four; `s_Grid` changed, and the copy of it hard-coded in the ground
shader's GLSL did not.

So the CPU filled `u_Cells[j * 4 + i]` for sixteen cells and the shader read
`u_Cells[j * 3 + i]` out of an array nine long. Seven of the sixteen writes
landed past the end and went nowhere; the nine that landed were shuffled into
the wrong squares. **The grass never took part in any of this** -- it asks
`ClimateAt` on the CPU, which was right the whole time -- so what the change
actually broke was the agreement between the ground and the things growing out
of it.

Standing in a cell the grid says is Desert, colour of the ground in the bottom
quarter of the frame:

| spawn | green before | sand before | green after | sand after |
| --- | --- | --- | --- | --- |
| cell 7 (Desert) | 100.00% | 0.00% | 0.00% | 97.45% |
| cell 11 (Desert) | 100.00% | 0.00% | 0.00% | 100.00% |
| cell 0 (Wetland, control) | 99.72% | 0.00% | 99.72% | 0.00% |

Cell 0 is the control and index 0 maps to itself under both schemes, which is
why it does not move.

The fix is not the corrected index. It is that **the grid's size is now written
once** and injected into the shader as `#define GRID` from `s_Grid`, so the two
cannot drift apart again. A constant that has to be written out twice will
eventually be wrong in one of the two places, and the symptom will not look like
an indexing bug -- this one looked like a biome rule.

`--spawn N` stands the camera at the centre of cell N, which is what let a
capture ask the question at all. The spawn buttons were the only way to be
somewhere specific and an unattended run has nobody to press one.

### 2026-08-29 (a door you can carry, and a room that is not where the door is)

`E` plants a doorway in front of you and `E` again nearby pockets it. Walk
through and you are in a toolshed; walk out of the shed's door and you are back
where you left it. One portal, and the shed is built once and stays built --
deploying does not create a room, it creates a *way in*, which is what a pocket
dimension is.

**Crossing is a plane test, not a trigger volume.** A box you must be inside for
a frame can be outrun: the player moves six metres a second and a fixed step is
a sixtieth, so a half-metre trigger is missed one time in five. Testing which
*side* of the doorway the player was on last step and is on now cannot be
outrun, because the two positions bracket the crossing however fast it happened.

Two faults, and the second is the interesting one.

The crossing test was `was in front && is behind` -- one direction only -- and
the portal is planted two metres *in front* of you, so you begin behind it and
walk the other way. It never fired once. A door is a door from both sides, and a
sign change says "crossed" without caring which way.

**The shed was 400 m underground, and the ground collider threw the player out
of it at ninety-five metres a step.** The placement was exactly right -- y
-398.80 with the floor at -400 -- and then the solver shoved them upward,
continuously. It was doing precisely its job: the ground is an SDF collider over
the voxel field, the field is only defined across the block, and *below* it
every query reads as solid. The player was being pushed out of rock that goes
down for ever.

Above the field the same query reads as air. So the pocket dimension is in the
sky instead, which costs nothing because nobody can see it -- and the asymmetry
is worth remembering, because anything else placed outside the block will meet
the same wall. Verified: into the shed at y 400.9 standing on its floor, back
out at y 3.0 on the terrain.

### 2026-08-29 (an editor layout, and the demo gets a viewport)

Every panel in the sandbox was an independent ImGui window dropped wherever it
last happened to be, over a demo drawn across the whole framebuffer. Fine for
one panel and unreadable by the time there are four: they overlap the thing they
describe, they move when the window resizes, and the scene is always partly
behind something.

`EditorShell` lays them out -- controls down the left with the demo selector
under them, the profiler on the right, a spare `Assets` pane along the bottom,
and the demo in the middle. **Nothing about any demo changed to make that
happen.**

**The demo is drawn into the central node by setting the viewport, not by
rendering to a framebuffer.** A framebuffer is the textbook answer and it would
have broken every capture in this project: `--hide-ui` exists so an unattended
run draws no panels at all, and with an off-screen target there would be nothing
to blit it with. Setting the viewport degrades correctly on its own -- no panels
means the rect is invalid means the demo owns the whole framebuffer, exactly as
before. Verified: `--hide-ui` still produces a full 1280x720 frame with all four
corners lit.

The camera's aspect follows the pane rather than the window. Leaving it alone
stretched the scene vertically by the ratio between them, which is the kind of
wrong that is easy to look at and hard to name -- nothing is obviously broken,
the trees are simply too tall.

Three things went wrong on the way and all three are worth keeping:

**Docking by window title is too fragile.** A demo's panel is titled whatever
its author chose, which is not its name in the registry, and a list of both in a
third file is exactly the thing that silently falls out of step. The shell
publishes the dock id instead and `DemoLayer` applies it to the window it is
about to open, so a demo written next year lands in the right place without
anyone adding it to a list.

**ImGui runs in layer order, and the shell has to go first.** Pushed after the
demos, it published the dock id *after* they had already opened their windows --
and `FirstUseEver` only fires once, so the panel floated for ever. It handles no
events, so sitting at the bottom of the stack costs nothing.

**`PassthruCentralNode` is what leaves the middle transparent.** Without it the
central node paints itself and the demo behind it is never seen, which looks
exactly like the scene failing to render.

The layout is built into `imgui.ini` once and only if that file does not already
describe one, so it is a starting point rather than a cage -- drag a panel
somewhere better and it stays. `--no-editor` turns the whole thing off.

### 2026-08-28 (leaves that are blobs, and grass that stops being blades)

**Leaf clusters were blocky for one reason: flat normals.** `LeafCluster` builds
a low-polygon sphere with the radius jittered per point and gave every triangle
its own face normal, so flat shading drew each facet as its own flat patch of
colour -- and the jitter that was there to make the blob look organic instead
outlined every triangle in it.

A cluster of leaves is a blob, and the normal a blob has at a point is the
direction from its centre. That was already known here and free to compute. The
silhouette is exactly as jagged as it was; what changed is that the lumpy radius
now reads as a rough surface rather than as a modelling error.

**The grass sparkle is not a smoothing problem, and smoothing would not have
fixed it.** A blade is six millimetres across, so beyond a couple of metres it
is well under a pixel wide. Coverage is then a coin-flip per pixel -- the blade
either contains the sample point or it does not -- and every blade carries its
own normal, its own colour jitter *and* its own root-to-tip gradient. Three
high-frequency signals, all aliasing, all moving in the wind.

The proposal on the table was to texture-map distant grass. That is the standard
endgame and it is a lot of machinery -- bake an atlas, cross-quads, cross-fade
between two representations, and impostors bring their own popping. There is a
cheaper thing that targets the actual complaint: **make the two outcomes of the
coin-flip look the same.** Converge each blade's normal toward the ground's, and
its colour and gradient toward the field's mean, as it recedes. A pixel that
lands on a blade and a pixel that lands on the gap beside it then shade almost
identically, and the aliasing stops *showing* even though it is still there.

It is also the physically sensible thing. A field of grass at a distance does
not shade like a million independent leaves; it shades like a surface. Close up
you are looking at blades, far away you are looking at a meadow.

Blades now stop entirely past sixty metres rather than thinning to a fifth --
a fifth of the blades is still a fifth of the sparkle, and the terrain beneath
is already tinted green by the same climate the grass grew from and already
carries a noise texture. Measured as mean Laplacian of luminance, which is what
a per-pixel sparkle *is*:

| band | before | after | |
|---|---|---|---|
| far hillside | 7.462 | 3.133 | **−58%** |
| mid | 10.324 | 8.218 | −20% |
| near | 14.319 | 14.285 | −0.2% |

The near field is deliberately untouched: at two metres a blade is about 3.7
pixels wide, so what varies there is detail rather than aliasing, and flattening
it would be throwing away the thing the density was bought for.

Worth writing down for whoever reaches for impostors later: **there is no MSAA
anywhere in this engine**, which is why sub-pixel geometry aliases as hard as it
does here. Turning it on is probably worth more than any of this, and is a
window-creation flag rather than a rendering feature.

### 2026-08-28 (digging was on the wrong input path, and trees that bend rather than fall)

**The dig was fine; the way it was being asked for was not.** Every part of the
path measured clean -- the raycast hit at 3.29 m, `EditSphere` touched 52
voxels, one chunk went dirty, the triangle count moved -- so nothing about the
editing was broken. It was hung off a `MouseButtonPressedEvent`.

`VoxelTerrain` polls the mouse on the fixed step instead, and says why in a
comment that has been there since it was written: *"the mouse and the keyboard
are in the replay stream and events are not, so a digging session records and
replays."* An event can also be consumed before it reaches a demo layer, which
is the symptom; the replay problem is the reason. The lab simply had not
followed a convention the project already had.

Everything that edits or teleports is polled now -- dig, add, the clipping
toggle, mouse-look and the nine spawn keys -- all edge-triggered, because one
edit per *press* rather than per step is the difference between digging a hole
and hollowing the block out in a second.

**Trees bend now rather than falling over.** The displacement goes as the square
of the height *and* the square of the wind, so a crown six metres up in a gust
was thrown many times its own height and a wood came out as a tangle of
stretched triangles. Capped at a share of how high up the tree a vertex is, so a
low branch is held tighter than the crown and the shape stays a tree rather than
being sheared uniformly. A tenth of the height is already a lot of movement;
grass gets 0.85 because grass really does lie flat and a tree really does not.

**Six habits rather than three seeds.** Three seeds of the same parameters give
three trees that are recognisably the same tree. What makes a wood look like a
wood is different *architecture*, and the three parameters that decide it are
the branching count, how far a child leans off its parent, and how fast length
falls off with depth. Between them: a spire, a vase, a mop, a sapling, a parasol
and a scrub.

They are a set to choose from rather than one tuned answer, and each has its own
checkbox -- turning all but one off gives a stand of a single habit, which is
the only way to judge whether that habit is any good. The sapling earns its
place for a reason worth stating: a stand all of one size reads as an orchard.

### 2026-08-28 (trees in the lab, and a theory the lab disproved)

Three tree shapes, scattered by biome, leaning in the same three-layer wind the
grass does. Each shape is a different *habit* rather than a different seed — a
tall narrow conifer, a broad open-grown hardwood, and one between — because
three seeds of the same parameters give three trees that are recognisably the
same tree.

They want it wetter than grass does: grass will grow on a steppe and a tree will
not, so the threshold sits well above the grass's, which is what puts the edge
of a wood *inside* a biome transition rather than on it. Nothing grows within a
metre and a half of the waterline either — a trunk half in the water looks like
a mistake even when the shoreline is exactly right.

The bend is the trunk's: height *squared*, because a trunk is a beam clamped at
the ground, so the root stays vertical and the crown does the moving. The wind
is sampled at the tree's own root rather than per vertex, so the whole tree
agrees with itself — a trunk leaning one way while its own canopy leans another
is the artefact that avoids. And because there is one draw per tree, the root is
the transform's own translation: known exactly, with nothing to reconstruct.

**The theory this was built to test is wrong, and the lab is what proved it.**
The planet's trees currently draw as flat canopy slabs lying across the ground —
buried to the crown. The standing guess was that the cause is *where* they are
placed: the planet asks the generator for the surface radius along a direction,
an analytic answer, while the ground you see is the isosurface a mesher found in
a sampled field, and the two need not agree.

So the lab scatters trees over the chunk's own triangles instead — a triangle of
the mesh cannot disagree with the mesh — and then measured both, over 156 trees:

| | worst |
|---|---|
| field distance at a trunk's foot (on the mesh) | **0.1159 m** |
| gap between the mesh position and the analytic height | **0.1268 m** |

They agree to about a tenth of a metre, which is the marching-cubes
interpolation error and nothing more. **Whatever buries the planet's trees, it
is not this.** The comment that had already been written into the file asserting
that cause has been replaced with the disproof, because the next person to look
will look there.

Scattering off the mesh stays, on its own merits: it is exact by construction
rather than by luck, and it needs no surface search at all.

### 2026-08-28 (wind as a field, a lake in a pit, and digging that stays put)

**Wind is a field, not a vector.** A single direction and speed for the whole
world is what made the grass read as a machine: every blade leaning the same way
by the same amount, for ever. Real wind over open ground has structure at
several scales at once, so there are three layers and each is a different
*size* rather than a different amplitude of the same thing — the prevailing
wind, gusts about 70 m across that multiply the speed between a lull and a
squall, and eddies about 18 m across that turn the direction by up to a quarter
turn and stop a gust front being a straight edge.

All of it is **advected with the mean**, which is the part that makes it look
like weather rather than like noise: a gust is a structure travelling downwind,
so it is sampled at `position − mean × time`. Stand still and the pattern comes
past you. The same three layers run in the grass shader, because a gust is
metres across and a chunk is sixteen — done per chunk it would be one number for
a whole gust front.

Measured over the block: mean 5.0 m/s, field **2.41 to 5.92 m/s**, direction
**±45°** off the mean, and the speed at a fixed point drifts as gusts pass. The
first version had the noise offsets at 0.75 and 0.80, so the product averaged
0.6 and every reading came out well under the number on the slider — the sort of
quiet lie that makes a tuning session take twice as long. They are 1.0 now, so a
zero-mean noise gives a field whose mean *is* the slider.

**A cap on how far a blade may lean.** The displacement goes as the square of
the wind, so at the top of the slider a blade was thrown several times its own
length and the field turned into a smear of stretched triangles. A real blade
lies flat and stops. The cap is a share of the blade's own length, so a short
blade in the understorey is capped shorter than a tall one and the two stay in
proportion.

**Digging no longer teleports you.** `Dig` called `BuildWorld`, which calls
`SpawnWalker`, which puts the player back at forty metres above the origin — so
every dig threw you into the sky and digging read as broken rather than as
working and moving you. The collider holds the field by pointer so the shape
follows on its own; what does not follow is the broadphase bound. Replacing the
ground body alone fixes that and leaves the player, their velocity and where
they were looking exactly as they were.

**Nine by nine by nine, with the grid still three by three.** 144 m of ground,
each of the nine cells owning a 3×3 group of chunks and covering 48 m.
Decoupling the block from the grid is what lets it grow without the panel
growing with it: eighty-one checkboxes would be a worse tool than nine.

**A water pit, and the water is one quad.** Noise does not make lakes — a basin
has to be a *bowl*, ground that falls away smoothly and comes back up on every
side, and nothing built from summed octaves reliably closes like that. So the
pit is put in on purpose: a smooth depression subtracted after the noise,
squared so the sides are steep near the rim and the floor is broad and flat,
which is the profile a lake bed actually has.

Then the water needs no mesh of its own at all. A lake surface only has to exist
where the ground is below it, and the depth buffer already knows where that is —
draw one horizontal plane across the block and everything underground is
occluded by the ground in front of it. What is left is exactly the water in the
pit, with a shoreline that follows the terrain to the pixel and cost nothing to
find. **That is worth taking back to the planet**, where the ocean is currently
a whole sphere carrying a wet mask.

Grass stops at the waterline with a metre of margin, so the shore is a band
rather than a line drawn on the water. The water level is measured from the rim
rather than from zero, so deepening the pit does not also empty it — which is
what anyone dragging the slider expects and the opposite of what a fixed level
gives.

Spawns now face the middle of the block. A spawn tool that drops you looking
whichever way you happened to be facing makes you turn round before you can see
anything, and the interesting thing is nearly always toward the centre.

### 2026-08-28 (the lab is a 3x3x3 cube with a biome grid on it)

**Three chunks a side, and the three is the same three as the grid.** The lab
was built nine chunks across, which was not what was asked for and was wrong for
the job besides: at 48 m a side each of the nine columns is 16 m square, small
enough to stand in the middle of one and see its neighbours on every side, which
is what a biome grid is for. It is a *cube* -- 48 m of vertical extent with the
terrain in the middle -- so there is real rock underneath to dig into and real
air above to dig out into, rather than a surface with nothing either side.

**Nine checkboxes, nine biomes, and the blend is the point.** A cell's biome is
a property of a 16 m square, and a 16 m square of desert against a 16 m square
of meadow with a hard line between them reads as a tiled floor rather than as
country. What makes a boundary look like a boundary is that it is *wide*.

So the cells are treated as samples at their own centres and read back
bilinearly, smoothstepped -- which gives the stated value at each centre and a
transition a full cell wide between any two neighbours. The same expression runs
twice, once on the CPU for where grass grows and once in the fragment shader for
what the ground looks like, because the grass has to agree with the soil it
comes out of. Nine `vec3`s of (moisture, warmth, weight) is cheaper than a
texture and needs no upload path.

**An unticked cell contributes nothing rather than contributing zero.** Those
are different: zero moisture is a desert, and a hole in the ground should not
make its neighbours arid, so the weights of the cells that exist are
renormalised. And unticked means *no ground* -- the column is empty, you can
walk into the gap and look at the section of its neighbours, which is the
cheapest way to see what the generator is doing under the surface.

Measured on the opening grid, which runs wet-to-dry across and cold-to-warm
down so that every neighbouring pair is a transition:

| check | result |
|---|---|
| each cell centre reads back its own biome | all nine exact |
| moisture across the middle row | 0.85 → 0.83 → 0.76 → 0.72 → 0.60 → 0.40 → 0.34 |
| biggest jump between samples 2 m apart | **0.0698** |
| density at the centre of an unticked cell | **+4.00** (empty) |

The row is monotone from forest through meadow to steppe with no step at either
cell line, which is the whole claim.

The five named spawn points are gone and the number keys now stand you in a
grid cell instead. A named list was the right idea while the climate was two
sliders; now that every cell has its own biome the useful thing to stand in the
middle of is a cell, and there are exactly nine of them.

### 2026-08-28 (a lab for the ground, and the trees were never drawing)

**`TerrainLab`, nine chunks of ground with every knob on a slider.** The solar
demo is where the terrain has to work and a poor place to find out why it does
not: a change costs a planet-wide rebuild, the interesting ground is wherever
the camera happens to point, and half the effects only appear at a scale you
have to fly to. Nine chunks of sixteen voxels is 144 m across — big enough for
a hill and a hollow, small enough that the whole field regenerates in a fraction
of a second, so a slider can rebuild the world on release.

Feature size, octaves, amplitude, a rolling-to-ridged mix, domain warp, the
plateau control the planet's continental shelf came from, caves, seed and voxel
size. Digging with the mouse. And the climate is **two sliders** rather than a
hydrology pass, which is the entire reason a desert is reachable here: on a
planet it takes a landing-site search.

**Developer tools, built here to be ported.** Five named spawn points on the
number keys — meadow, desert, steppe, tundra, wetland — and each one carries the
climate that makes it what it is, because a spawn that only moves the camera
shows you the same ground from somewhere else. And a clipping toggle on `V`.

The tempting way to write no-clip is to keep simulating and turn the collider
off, which leaves the solver pushing a shape nothing pushes back on — so gravity
still accumulates and letting go of the keys drops you through the floor at
whatever speed you had reached. Making the body **kinematic** and integrating
the position by hand is simpler and is what a person means by "let me through
the ground": while it is on, the world does not act on you at all.

**Ground is dirt with grass on it, not a green surface.** Painting the ground
the colour of what grows on it works from orbit and fails underfoot — what shows
between blades is *soil*, and painting it green is the single thing that makes a
field read as a carpet. So the ground is brown and the green is a tint over it,
which means the blades and the earth between them are different colours and the
eye reads depth. The texture is three octaves of value noise on the world
position; two things about it are worth more than a bitmap would be, that it is
in world space so it does not swim as you walk, and that it moves the *colour*
rather than the brightness, because a surface varied only in value reads as
dirty rather than as soil.

Blades come in two passes now, a tall sparse one and a short dense one. One
length reads as a brush: every tip at the same height is a flat plane of green
with nothing behind it. The short pass is what hides the ground between the tall
blades, which is the job density was being asked to do alone. Colour varies
blade to blade off the per-blade ticket the scatterer already writes — a
brightness spread *and* a small hue shift, because brightness alone reads as
noise and both together read as different plants.

**And the reason there were no trees: the tree shader had not compiled since
the sway work.** Two commits ago a `.replace` meant for the grass shader also
matched the tree one — they share a `u_Compliance` line — and left
`u_GustOffset` declared twice. `SolarSystemTrees` failed to compile from that
point on and drew nothing.

That is worth writing down twice over, because the measurement taken at the time
said **"656 instances drawn"** and was believed. It was counting *submissions*,
not pixels: the batch was built, the buffer uploaded, the draw issued, and the
program behind it was dead. A count taken on the CPU says nothing about whether
anything reached the framebuffer, and the log line that would have said so —
`Shader 'SolarSystemTrees' compilation failed` — was scrolling past under a
`grep` for the word "error", which it does not contain.

With the shader fixed the trees draw, and they are visibly wrong: enormous flat
leaf polygons lying across the ground, which is what a buried tree looks like
when only its canopy clears the terrain. Disabling the sway entirely leaves the
image byte-identical, so it is a placement fault and not a bending one. Not yet
root-caused; written down rather than guessed at.

### 2026-08-28 (the geometry artifacts were the grass, and grass LOD)

**The artifact was the grass all along.** Reported as flat angular slabs cutting
through the hillside, and they looked like terrain — big, planar, dark. Two
hypotheses were measured and both were wrong before the right one turned up:

- The **horizon mesh** stands 192 spokes at 340 m, which is 11 m triangles
  against 1.5 m chunks, so it should bridge valleys and poke through. Measured
  as mean-of-two-ends against the generator's own height at the midpoint, net
  of the droop meant to keep it under: it stands **0.08 m proud, at 340 m out**.
  Not it.
- **Terrain LOD.** `--no-terrain-lod` against the same frame differs by 5.6% of
  pixels, all of it at the coarse strides where it should be. Not it either.

The blades were `Width = Height * 0.10`, so 9 cm across. A real blade is four or
five millimetres. Standing in it you were not looking at grass but at a heap of
flat green shards the size of dinner plates — correct geometry at an absurd
scale, which is exactly what "geometry artifact" looks like from the inside.

**Narrow blades need many more of them, and that needs LOD.** At 6 mm and 60
blades a square metre over the full stride-1 radius the frame went from 12.4 ms
to **57 ms**. Three things fixed that, and two of them failed first:

1. **Density became blades per square metre.** Per terrain triangle is a number
   that means nothing on its own — it depends on how finely the ground happens
   to be meshed. The triangle's own area makes it a density anyone can reason
   about, and makes the cost of a change predictable.
2. **Grass exists only on stride-1 chunks, so that radius *is* the grass
   budget.** Pulling it from 100 m to 55 m is what pays for sixty blades a
   square metre instead of six. The terrain loses very little: stride 2 at 55 m
   is 3 m between samples on ground 55 m away.
3. **A keep-fraction per chunk against a per-blade ticket.** Both sides of the
   comparison have to be constant across a blade. The first attempt computed
   the distance *per vertex*, so blades near the threshold had some vertices
   collapse and some not, and the field filled with long black slivers stretched
   to the collapse point — about two per cent of blades. Moving the blade's root
   into the position slot fixed that exactly, but cost the normal its attribute,
   and a **derivative normal is noise on geometry six millimetres wide** — the
   whole field went black. The version that works keeps the normal and makes the
   *threshold* a uniform set per chunk, so it cannot vary across a blade at all.

| | ms per step |
|---|---|
| 60 blades/m² over 100 m, no LOD | 57.0 |
| the same over 55 m, no LOD | 28.5 |
| per-chunk keep, full inside 20 m to a fifth at 52 m | **12.5** |

Ten times the blade density of two commits ago, at the frame time it had before
any of this.

**The gust was still tied to walking speed, and the frame was only half of it.**
The phase became camera-independent last commit; the *wavelength* did not. It
was an 18 m pattern travelling at 8.9 m/s, and walking at 6 m/s through an 18 m
pattern changes the rate you meet it by most of itself. A real gust front is
tens of metres across and travels with the air, and both of those are the fix:
one radian per 55 m, and a time term that is that scale times the wind speed, so
the pattern advects at exactly the speed the air is moving and nothing else.

**Trees are placed and drawn** — 2,061 made, 656 batched — but the nearest to
the default site is **151 m away**, so the opening view has none in it. The
landing clearing is 20 m and the batch cap is 16,384, so neither of those is
it; this is not yet root-caused and is written down rather than guessed at. The
tree line is at least honest now: it was `height / (Amplitude/2 - sea)`, a
fraction of the relief that stopped trees 87 m above sea level while the shader
painted green forest floor for another five hundred. It is the same isotherm the
shader uses, filled from the same model, so the two cannot drift.

**Wind streaks down from 0.11 to 0.035.** Third time; they were built to be
found and kept being far too loud for something standing in for air.

**And the solar scale is already exact.** From the demo's own numbers at
`p = q = 1.000`: the Sun is 27,311 km drawn at 5.87e9 m, which is **0.5331°**
across against a real 0.533°, and the Moon 0.5177° against a real 0.518°. Both
are within a tenth of a per cent. They look small because the real ones are
small — half a degree is about a pencil eraser at arm's length. Games nearly
always exaggerate them; the `Bodies q` slider is that exaggeration, and it
defaults to true scale on purpose.

### 2026-08-28 (the forest was swaying in time with how fast you walked)

**A real bug, reported as "the trees are tied to the camera".** The gust phase
in both the tree and the grass shader was taken from a world position — and in
this demo "world" is camera-relative, because everything is drawn that way to
keep planet-sized coordinates off the GPU. So the phase changed every time the
player took a step, and the whole forest swayed in time with walking speed.

The comment beside it claimed the root position "is fixed for the life of the
tree". In a frame fixed to the planet it is. That was the wrong frame, and the
comment was confidently describing a property the code did not have.

The fix is that `a_Model`'s translation is the tree's offset from the forest
origin, and that origin follows the camera — so the two move by equal and
opposite amounts and their **sum** is exactly the planet-fixed position. The CPU
adds the origin's half in double and hands it over as a uniform, folded into
`[0, 2π)` first: a phase only means anything modulo a turn, and folding is what
lets a 250 km coordinate survive the cast to float. The grass does the same
thing per chunk, with the gust axis taken in the planet's frame while the lean
stays in the scene frame — the same vector seen from two places, and keeping
them apart is the whole fix.

Measured over a 400 m walk past a fixed tree, as the sine of the gust phase,
where 0 to 2 is the entire range:

| | swing |
|---|---|
| taken from the camera-relative position | **1.6313** |
| taken from the planet-fixed position | **0.0000274** |

So the old gust went through most of a full cycle from walking alone. That is
the reported symptom, in a number.

**Grass that is grass rather than needles.** Two things were wrong and only one
of them was the shape.

The shape was one triangle a blade — two corners at the root and a point at the
tip, which is a *needle*. It has no length along which anything can happen, so
it cannot curve, and the only way to make it read as grass is to make it fat,
which makes it read as a leaf. Three triangles and a middle pair of corners at
55% of the height costs five vertices instead of three and buys the thing that
matters: the blade bends **along itself**. The wind lean now goes as the square
of height up the blade rather than the first power, which is a beam bending
under a load rather than a hinge at the root, so the root stays vertical and the
tip lies over.

The larger error was density. It ran at 0.6 blades per terrain triangle, which
on metre-scale triangles is a few blades a square metre — no shape or width
rescues that, because it reads as sparse spikes by being sparse spikes. A real
lawn is thousands a square metre. Now 6 on the planet and 4 in the open world,
with the blade half again as tall.

Cost, measured: 400 steps of the open world in 3.49 s and 600 of the solar demo
in 8.03 s, both including generation. Roughly twenty times the grass geometry
for no change anyone would notice in the frame time, which is the answer to
whether three triangles a blade was affordable.

**And the wind streaks are down from 0.42 to 0.11.** They were drawn to be
found, and once found they were far too loud for something standing in for air.

### 2026-08-28 (grass, wind you can see, and the first two shared modules)

Two new modules, and they are the first pieces built as modules rather than
found inside a demo and left there.

**`Grass.h`** is `OpenWorld::BuildGrass` lifted out so the planet can have grass
too. That move was most of the work: the original assumed `+Y` was up in four
separate places, which is true on a flat world and false everywhere on a sphere.
Nothing in the module knows which way up is — it asks.

It asks through a **template parameter rather than a `std::function`**, and the
reason is the one place in this refactor where the choice has a cost either way.
The callbacks run once per terrain triangle, thousands of times per chunk while
chunks stream in. A `std::function` is an indirect call the optimiser cannot see
through, so it cannot inline the body or hoist anything out of the loop; a
template is resolved at compile time, and OpenWorld's `up` — which returns a
constant — compiles away to nothing. The price is that a template lives in a
header and every user compiles its own copy. **Things called in a loop are worth
a template; things called once are worth a `.cpp`.**

On the planet, the vertical has to be recovered by adding the chunk's
float-sized local position back onto its planet-sized origin *in double*, which
is the same split the whole chunk system rests on. Where grass belongs is asked
of the hydrology's own moisture field, so the grass line and the biome colour
cannot disagree.

The blade bends by the height it already carries in `a_TexCoord.y` — zero at the
root, one at the tip — so the root stays planted without anything knowing where
it is. Grass goes as the **first** power of that where a trunk goes as the
square: a trunk is a cantilever, a blade is closer to a hinge.

**Bending preserves length, and leaving that out was immediately visible.** The
first version displaced the tip sideways and nothing else, so at this site's
8 m/s the blade did not bend, it *stretched* — the meadow came out as long dark
streaks lying across the ground. A blade pivots, so its tip travels an arc of
its own length and must drop by `h - sqrt(h² - d²)`. That caps the lean on its
own, since `d` can never exceed `h`.

**`WindStreaks.h`** draws the wind. Everything else the weather does is a force,
legible only if you already know to look for it; this is the wind as something
you can see, drifting at exactly the speed the model says the air is moving, so
the grass and the strokes above it are two views of one number. Painterly on
purpose — air is transparent and there is nothing there to draw, so what these
stand in for is what a painter puts on a canvas to say "windy". The geometry is
honest and the appearance is a brush stroke.

One static mesh, advected on the GPU and wrapped in a box centred on the camera,
so a streak that leaves downwind reappears upwind and the field is endless for
one draw call. The first pass made them 0.1 to 0.45 m wide, which at a hundred
metres is well under a pixel — every soft profile in the fragment shader was
thrown away by the rasteriser and the field came out as hard thin scratches. A
brush stroke has to cover enough pixels for its own taper to show, which is a
lower bound in *pixels* and therefore in metres once the box fixes the distance.

**And the default landing site had to be re-surveyed.** It is a hard-coded
direction, chosen once against relief that no longer exists — after the
continent work the old site came out underwater, which looks like a broken
camera rather than a moved site. The new one is 54.8 m above sea level, slope
0.066, 138 m of relief inside 400 m, shore 240 m away. **Anything that changes
`Relief` invalidates that constant.**

### 2026-08-28 (the planet had no continents, and four reasons why)

The orbital view had been called "malformed terrain" three times, and looking at
it was never going to settle it. Dumping the baked map did, immediately: the
land was not continents but a **spidery filigree**, a net of threads with no
landmass anywhere in it. Two numbers off that dump:

- The wet mask had **exactly two distinct alpha values** — a hard binary
  land/sea test, no antialiasing at all.
- **129 land/sea crossings per row** of 1024 texels. A handful of continents
  would give five or six.

Four separate causes, each measured on its own.

**1. The coastline was cut across flat ground.** Sea level is solved for a
target land fraction, so it goes wherever it must — and the height field was
near-Gaussian with only **8.6% of its variance at continent scale** and 41%
below 6 km. Cutting a unimodal distribution near its mode is the case that
maximises boundary length. Earth's hypsometry is strongly bimodal — continental
platform, abyssal plain four kilometres down, sea level in the gap — which is
exactly why its coastlines are crisp. `ContinentEdge` squashes the broad noise
into two plateaus joined by a slope, so the same detail noise now shifts the
coast a little way along a gradient instead of shredding it. 129 → 87.

**2. Mountains were being built on the sea floor.** `Landscape` was added at
full strength everywhere, and on this planet it is the *larger* of the two: up
to 700 m of range on a continental step of about 270 m. The shape that decides
where land is was outvoted by the shape that decides what land looks like.
Weighting it by the platform is what the real thing does anyway — ocean floor is
smooth because nothing uplifts it, coastal plains are flat because they are the
drowned edge of the platform, ranges are inland. 87 → 59, and with the
continental share raised, 59 → 55.

**3. The map was aliasing the landscape into noise, and the drainage pass
believed it.** This was the big one, and the cap that was supposed to prevent it
was already there and already carried a comment saying exactly what would happen
("an aliased height field routes water into pits that are not there"). It cut
octaves at the finest wavelength the sampler could represent — but `1 - |n|`
puts a corner wherever the noise crosses zero, so **a ridge field of wavelength
L carries detail at L/2**, and the cap never accounted for the fold. It also
floored at one octave, so the layer went into the map no matter what. A 4 km
ridge field with 700 m of rise, on a 1.5 km texel, came out as per-texel salt
and pepper across every continent.

That is not just a rendering problem. The drainage pass runs on that grid, and
it reported **a quarter of all land under a lake** against about two per cent
for the real thing. Doubling the cut and dropping the layer when even its
coarsest octave is below the line: **55 → 24.8 crossings**, and lakes 23.3% →
12.1%. The ground you walk on is meshed at 1.5 m and passes no cap, so it keeps
every octave — this only ever trims the map.

**4. And then the continents came out grey.** Fixing the coastline made the
hypsometry bimodal, which broke a ramp that had been fine before:
`smoothstep(0.45, 0.75, height/top)` for bare rock. A fraction of the tallest
land reads as scale-independence and is not — it assumes land heights are spread
evenly from sea level to the summit, and a continental platform sits near the
*top* of the range by construction. Every interior landed above `f = 0.7` and
was painted rock with a thin green rim at the shore.

Height does not decide what grows; temperature does. The tree line is the
isotherm where the warm part of the year stops being warm enough to build wood,
and driving it the same way as the snow line means the two cannot cross. That
removed `u_LandTop` and `ReliefHigh` again on the way — both had been added an
hour earlier for a ramp that no longer exists.

From orbit: land 23.7% → 19.8% of the disc against 24% expected from the
hydrology's own dry-land figure, and it is continents now rather than lace.

### 2026-08-28 (a snow line that is a temperature, and a pole that is cold)

Wiring the weather into the terrain found a real flaw in the model committed an
hour earlier. `Redistribution` was answering two different questions with one
number: how much of noon's heat survives to midnight, and how much of the
tropics' heat reaches the poles. Those are different physics — the first is
thermal inertia, ground and water still warm at dawn; the second is *transport*,
air and ocean actually carrying heat thousands of kilometres — and the second is
far less complete, which is the entire reason Earth has ice caps. With one
factor at 0.97, correct for Earth's small day/night swing, the equator came out
**1.6 K** warmer than the pole against a real 44.

Split in two, with the latitude's own share of the beam from a two-term
Legendre expansion. `s2` is not fitted: a pole's annual mean is exactly
`S sin(ε)/π`, and matching the expansion to it gives `s2 = 4 sin(ε)/π − 1`,
which is −0.494 for Earth against a published −0.482. It has the right limit at
the other end too — a body with no tilt gets `s2 = −1` and poles that receive
nothing, which is correct.

| | model | measured |
|---|---|---|
| Earth equatorial annual mean | **26.6 °C** | ~26 °C |
| Earth polar annual mean | −13.9 °C | ~−16 °C (Arctic) |
| equator-to-pole range | 40.5 K | ~44 K |

The pole is the weaker of the two, and knowably so: this model has no
ice-albedo feedback, so it cannot reach an Antarctic −50.

**The snow line is now a temperature.** It was `smoothstep(0.74, 0.93, f)` on
height over relief, so a planet whose tallest hill was 600 m grew a snow cap on
it and a planet whose tallest was 16 km put snow at the same *fraction*. Both
cannot be right and neither was. A snow line is an altitude in metres set by how
fast air cools as it rises. `Warmth` stopped being two things at once as well —
it carried its own altitude term quoted against the relief, on the reasoning
that "a planet with more relief has a snow line in the same place relative to
its own mountains", which is precisely the wrong invariant. It is latitude now,
and altitude enters once, as `g/c_p` slackened by moisture.

What that gives on Earth: sea-level snow **poleward of 68.8°** against a real
Arctic circle at 66.5°, and an equator needing 3365 m to freeze against 1757 m
of relief — so no equatorial snow here, but a tall mid-latitude peak gets some.
Neither number was aimed at.

Sea ice comes off the same band with no lapse rate in it, because the sea is at
sea level wherever it is. That means the ice edge and the snow line reach the
coast at the same latitude **by construction** rather than by being tuned to
each other. Measured by diffing the same orbital frame before and after: 10,161
pixels change, all of them in one contiguous cap around the pole.

**And the clouds ride the wind.** The drift was `2π × 365.25/9` radians a year
for every body in the system — a cloud going round Jupiter in the same nine days
it goes round the Moon, which for a body forty times wider is a cloud moving
forty times faster for no reason anyone could point at. The accumulator counts
seconds now and each body converts at `v/R`. Earth's clouds circle in 1.9 days
at the drawn 250 km radius; the same line at 1:1 gives **49 days**, which is
what a mid-latitude weather system actually takes.

One limitation written down rather than fixed: `ClimateBand` is meaningless on
an airless body. Temperature goes as the fourth root of flux, which is concave,
so the temperature of the mean flux is not the mean of the temperature, and
with nothing to carry heat through the night the two are hundreds of degrees
apart — asked for the Moon's equator it returns 26 °C against a real −20. It
does not matter, because `Biome` takes its airless branch and returns before
reaching the snow line, and computing the integral properly would be arithmetic
nothing reads.

### 2026-08-28 (wind goes to zero at the ground, and the rocks were already rolling)

The first landed capture after the drag went in showed the boulders in
different places, which looked exactly like a 3.9 m/s breeze rolling rocks
across a landscape. It was not, and finding out which is a good example of
suspecting the measurement.

Measured with the wind disabled entirely: the rocks reach **150 m/s and keep
climbing**. They are spheres with `Friction 0.7` but no rolling resistance and
`LinearDamping` and `AngularDamping` both zeroed (deliberately -- see the
comment where they are spawned, which is about orbits), sitting on a site with
233 m of relief inside 400 m. They roll downhill and nothing ever stops them.
That is pre-existing and predates every change in this session.

With the drag on they settle at 8-13 m/s instead. Which is to say the wind
force is not blowing the boulders around, it is **the only thing giving them a
terminal velocity**: for a 40 kg sphere of 2.54 m² presented area, balancing
½ρCdAv² against `g sin θ` at 13 m/s puts the slope at 32°, which is about what
that hillside is. Worth writing down that the rocks are 40 kg -- a real rock of
that radius is eleven tonnes, so this is a gameplay mass and the wind moving it
at all is correct.

**Wind still had to go to zero at the ground, though.** A wind speed is quoted
at ten metres by convention, and the air below is slower because the ground is
not moving. That is the log wind profile,
`v(z) = v_ref ln(z/z0) / ln(z_ref/z0)`, out of the turbulent boundary layer and
the correction every wind measurement in the world gets. `z0` is a measured
property of a surface: 0.0002 m over open water, 0.03 m over grass, 0.1 m over
scattered obstacles, a metre over forest.

Without it a boulder lying on the ground was being handed the wind from a storey
above it. With it, a settled rock sits in 53% of the reference wind and so 28%
of the force, and a person's feet are in slower air than their head. The clamp
is at 1.5 rather than 1 on purpose: a body forty metres up genuinely gets 1.31×
the ten-metre wind, and that is the profile being right rather than overflowing.

### 2026-08-28 (the wind pushes things)

Drag, which is the only way air touches anything: `F = ½ ρ Cd A |v| v`, with ρ
out of the weather model, A off the collider, and **v the wind relative to the
body**. That last part is what makes it drag rather than a push — standing
still in a gale is shoved hardest, moving downwind is barely touched, and
something already travelling at wind speed feels nothing at all. The same line
is what slows a thrown rock with no wind blowing.

The area is a theorem rather than a choice. **Cauchy's formula** — averaged
over every orientation, a convex body's projected area is a quarter of its
surface area — covers the sphere, the capsule and the box with one rule, and
reduces correctly: a sphere's 4πr² over four is πr², which is the disc it
presents from every direction anyway. The alternative was orienting each
collider against the wind every step, which for a tumbling rock is a more
precise answer to a question nobody asked.

Checked by hand against the engine. The player is a capsule of radius 0.4 and
half-height 0.9, so its surface is 2π(0.4)(1.8) + 4π(0.4)² = 6.5345 m² and
Cauchy gives **1.6336 m²** — matching the engine to the digit. Then
½(1.2078)(0.8)(1.6336)(3.91²) = **12.066 N** against a measured 12.0662, on a
78 kg body, so 0.155 m/s². That is meant to be nearly imperceptible: it is what
a 3.9 m/s breeze does to a person. The same expression at 30 m/s gives 720 N
and 9.2 m/s², more than this planet's gravity, which would take you off your
feet. Four orders of magnitude out of one line.

**Trees bend like cantilevers.** The load is wind pressure ½ρv², which is why
twice the wind bends a tree four times as far and why nothing bends in a
vacuum; the shape is the first bending mode of a beam clamped at one end, which
goes as the square of the height above the clamp, and is what keeps the trunk
planted while the crown swings. The gust phase comes off the root position, so
neighbouring trees are out of step with each other and each one is in step with
itself.

The compliance is the one stand-in here, and it is split in two for a reason
that is real: a beam's compliance goes as 1/(E I) and I goes as the fourth
power of the section radius, so a twig a tenth the thickness of a trunk is ten
thousand times easier to bend. That is the whole reason a tree in a light
breeze is still at the bottom and moving at the top. The mesh has one trunk
radius and no twigs, so the r⁴ cannot be computed from it; the canopy gets 20×
the trunk's compliance instead. At the default site's 3.9 m/s that is 1.1 cm at
the top of a ten-metre trunk and 22 cm in its canopy — a stirring crown over a
still trunk, which is what the calibration is trying to buy.

Measured by capturing the same frame with the compliance zeroed and diffing:
**35,152 pixels move, 3.81% of the frame**, worst channel delta 175, and every
one of them is in rows 48–413. The ground below y=420 is untouched, which is
the check that nothing else moved with it.

### 2026-08-28 (weather, derived rather than authored)

`TestEnv/src/Climate.h`: one temperature and one wind for any point on any
body, out of the numbers the system already had. Nothing in it is a designer's
dial, which is the whole point — a wind you can lean into means very little if
someone typed the number and a great deal if it came out of the same orbit
that decides when the sun rises.

The chain is five steps: the star's output spread over a sphere gives the flux
arriving; what is not reflected is absorbed and re-radiated, and setting those
equal gives a temperature; air is transparent to sunlight and not to heat, so
it raises that temperature by a closed form in one optical depth; ground higher
up is colder at a rate that is `g/c_p` and nothing else; and air moves from
where there is more of it to where there is less while the planet turns
underneath, which is what makes the bands.

Axial tilt and time of day never appear in the code. They do not have to — the
zenith angle is `dot(up, toSun)` with both vectors in scene coordinates, so
the seasons happen because the axis the body turns about is not its orbit's,
and the sun rises because the body turns. That was the test that the frames
were already right.

**Twenty-five checks against numbers the model contains no fit for**, run from
a temporary self-test and then deleted:

| | model | measured | off |
|---|---|---|---|
| solar constant, 1 AU | 1361.17 W/m² | 1361 | 0.01% |
| Earth equilibrium temperature | 254.04 K | 254 | 0.02% |
| Earth scale height | 8429 m | 8500 | 0.84% |
| Earth sea-level air density | 1.23 kg/m³ | 1.225 | 0.03% |
| dry adiabatic lapse rate | 9.76 K/km | 9.76 | 0.01% |
| saturated lapse rate | 6.54 K/km | 6.5 | 0.62% |
| **lunar subsolar temperature** | **380.9 K** | **390** | **2.3%** |
| lunar night temperature | 101.7 K | 100 | 1.7% |
| **Mars equilibrium temperature** | **209.8 K** | **210** | **0.09%** |

The Moon and Mars are the ones worth having: the greenhouse coefficient is
calibrated so Earth lands on 288 K, so Earth's surface temperature is not
evidence of anything, but nothing is fitted to an airless body's subsolar
point or to Mars's distance and albedo. The circulation was checked
structurally instead — the prevailing wind is zero at 0°, 30°, 60° and 90°,
easterly in the trades, westerly in the mid-latitudes and easterly again at
the pole, all out of `sin(π |lat| / 30)` with no cases in it.

Two things the checks turned up. `Colour` is not an albedo: its luminance puts
Earth at 0.46 against a real Bond albedo of 0.306, which would have made the
equilibrium temperature 17 K too cold, so `BondAlbedo` is now a measured column
in the table. And Mars's surface pressure comes out at **15.9% of Earth's**
against a real 0.6% — the table's `AtmosphereFraction × AtmosphereDensity` are
render depths chosen to make a sky look right, not weights of gas. The model is
reading them as weights, and that is written down rather than tuned around.

Known to be wrong: Venus. Its column gives a grey optical depth of 17 and a
surface of 442 K against a real 737 K. A grey atmosphere is simply the wrong
model for an optically thick CO₂ one, and no single coefficient fixes it.

At the default landing site the model reads 18.4 °C at latitude 4.1° and 22 m
up, with 600 W/m² absorbed, 101.1 kPa, and a 3.9 m/s wind from the east
blowing toward the equator. Each of those is checkable by hand: 1361 × 0.694 ×
0.635 = 600.0, and 101.325 × exp(−22/8430) = 101.06. The wind is the trades,
because latitude 4.1 is in them.

### 2026-08-28 (a planet with continents on it, and the seam down the middle)

Two bugs that between them had been erasing every landmass on Earth from
orbit, both found by arithmetic rather than by looking.

**The haze was drawn over 750 km of vacuum.** The terrain shader ended with
`haze = 1.0 - exp(-camDist * u_HazeDensity)` — the full camera distance and
nothing else. `u_HazeDensity` is `AtmosphereDensity * m_HazeScale`, and for
Earth that is `3.0 * 3.3e-3 = 9.9e-3` per metre: a half-hazed distance of 70 m,
which is the right order for standing in a landscape and is exactly what the
constant's own comment says it was tuned against. From `--orbit` the camera
sits four radii out, so `camDist` to the near surface is 750,000 m, the
exponent is **7425**, and `haze` is 1.0 to the bit. Every land pixel on the
disc came out *exactly* `u_Sky`.

So the planet had no continents on it because the terrain was never drawn —
only the sky colour was. The mottled dark speckle that read as malformed
ground was the atmosphere shell's raymarch over a flat grey ball, and the
reason nobody had caught it is that it is invisible while anything else is
wrong: the shell used to be 55% transparent and the terrain used to paint
itself blue below sea level, so the disc looked plausibly like a water world
either way.

The missing term is the air. Extinction is the integral of density along the
ray and density falls off exponentially with height, so a path that spends all
but four of its 750 kilometres above the atmosphere carries almost none.
Sampling the density at the **midpoint** of the eye-to-fragment segment is the
cheapest thing with the right limits at both ends: on the ground both
endpoints are at zero altitude, the factor is 1, and the landed tuning is
untouched to within 0.2%; from orbit the midpoint is 375 km up and the factor
underflows to zero. The scale height is the shell's own — a quarter of its
thickness — because the two are describing one atmosphere and a fragment sits
under both, and drifting them apart would put a step in the haze exactly at
the horizon, which is the one place it would be seen.

**Checked against the hydrology, which the shader never sees.** Green-dominant
pixels over the disc went from 1.8% to 18.9%, and the 1.8% had been neutral
cloud grey (108/107/104) rather than anything green. The drainage pass reports
land at 29.3% of the sphere with 30.68% of it under a lake, so dry land is
29.3 × (1 − 0.3068) = **20.3%** of the surface. Measured as a share of the lit,
cloud-free disc: 18.9 / (100 − 9.0 dark − 2.3 cloud) = **21.3%**. One
percentage point, from a Priority-Flood accounting the fragment shader has no
access to — it gets a wet mask and a Whittaker square. The small excess is
what a projection over-weighting the sub-camera point and blending partial
coastal coverage should give.

**And a two-pixel blue line down the middle of the planet.** Every one of
these shaders builds `u` from `atan`, so it wraps from 1 back to 0 along one
meridian. The *value* is right on both sides; the derivative is not, and the
hardware picks a mip level from the screen-space derivative of the coordinate.
Across the wrap that derivative is a whole texture wide, so it selects the very
top of the chain, where a texel is the average of the entire map — and at the
top of the chain the wet mask averages to roughly the ocean fraction, so along
the seam the shell decided there was sea and painted it over whatever land the
meridian crossed.

No sane sampling of a sphere moves half a texture in one pixel, so a derivative
that claims to has wrapped and subtracting the nearest whole turn recovers the
true one. `SampleSphere` does that and hands the result to `textureGrad`; it is
spliced in after the `#version` line of the terrain, water and cloud shaders,
all three of which had the bug. Measured on a uniform stretch of ocean at
y = 400, where any deviation is unambiguous: the seam columns went from
(52, 67, 94) against a background of (27, 47, 84) to **exactly** (27, 47, 84).
Across the disc the seam column is now 13.5% anomalous by a crude
neighbour test against controls elsewhere running 10.4–23.5% — indistinguishable
from ordinary coastline, which is all that test can really measure.

The sanitizer reports 16 of 16 demos failing, and all of it is LeakSanitizer
inside system ALSA reached through vendored miniaudio's
`ma_context_open_pcm__alsa`. It hits demos nothing has touched in weeks because
every demo opens audio. With `detect_leaks=0` there are no ASan or UBSan
reports at all.

**And a ring of missing sea, from a circle standing back over a square.** The
ocean shell discards inside a cone around the local water so the two never
blend over each other, and it was given `SurfaceWater::Reach()`. But
`BuildMesh` leaves `s_SeedMargin` = 6 columns off each edge of a 128-column
grid, so what it actually draws is a *square* of half-width
`Reach × (1 − 12/127)` = 0.9055 Reach. The largest circle inside that square
has the same radius, and the shell had stood back over a disc of radius Reach:
between the two there was nothing at all.

The uncovered part is the disc minus the square, which is four circular
segments cut at distance `a` from the centre — a shape the code contains no
formula for. `4[R² acos(a/R) − a√(R²−a²)] / πR²` gives **6.87%** of the local
water disc; two million Monte Carlo samples of the same question gave 6.858%.
Four bites at the edge midpoints of the square, each 9.4% of the reach deep,
at the far edge of the sheet you are standing on — which is what "the water
falls away into the middle of the body" looks like from a shore.

`DrawnReach()` derives the radius from the same two constants `BuildMesh`
loops over, so the cone cannot drift from the mesh again; with it the
uncovered fraction is exactly 0. The wet-column and quad counts are unchanged
(3482 and 1966), which is the point — nothing about the local answer moved,
only how much of the sphere stood back for it.

While measuring this, two comments turned out to be wrong: both said the map
and the terrain disagree at the seeded rim "by up to a couple of metres" and
"up to 2.5 m". At the dry `--land Earth` site it is 1.859 m, and at the default
shore site it is **59.375 m** — a 1.5 km texel that straddles a coast averages
a cliff. The margin was already wide enough; the number in the comment was
measured somewhere the question is easy.

### 2026-08-27 (the shading height, and a frame with no shoreline in it)

The GPU half of 1:1, and the last precision item on the surface. The terrain
shader computed its shading height as `length(v_Position) - u_SeaRadius`, where
`v_Position = world.xyz - u_Origin` is planet-centred — about 6.4e6 at 1:1, in
`float32`. Both the subtraction that forms it and the `length` that reads it
happen at that magnitude, and the answer wanted is a few hundred metres, so the
entire shading height was computed in the bits the format had already thrown
away. `Biome`'s waterline had the same shape one line up: `u_SeaRadius -
u_Radius`, two values near 6.4e6 differenced on the GPU for an answer of a few
hundred metres.

**Predicted from the format, then counted in a frame.** Floats in
[2²², 2²³) — which is where 6.371e6 lives — are exactly the multiples of 0.5,
so `length(v_Position)` lands on a 0.5 m grid, `u_SeaRadius` is on the same
grid, and the difference is a multiple of 0.5 m. Therefore `fract(height)` can
take **exactly two values**. At the default 250 km the same argument gives
2⁻⁶ = 0.015625 m and therefore **exactly 64**. A temporary debug output painted
`fract(height)` into the red channel with green pinned at 1 to mark the pixels
this shader drew — the first count included the trees and the lander and was
meaningless — and captured the landing site:

| | distinct levels | spacing in 8-bit |
|---|---|---|
| 6,371 km, before | **2** (0, 128) | — |
| 250 km, before | **64** | 4 |
| 6,371 km, after | 256 | 1 |
| 250 km, after | 256 | 1 |

Two scales, two different predictions, both exact. After the change the count
is 256, which is all an 8-bit buffer can show.

**The fix is the same one the CPU side already uses: never form the large
number.** Each draw now carries a reference point near its own geometry —
a chunk's origin, the horizon mesh's site — as `u_Reference` in camera-relative
coordinates, so `v_Offset = world.xyz - u_Reference` is a difference of two
small numbers and is exact. The CPU sends `u_ReferenceAltitude = |C| - sea`
computed in double, and the shader adds `|C + e| - |C|` written as

    (2 |C| (n·e) + e·e) / (|C+e| + |C|)

which is an **identity, not an expansion** — there is no small-`e` assumption in
it, and that matters because the horizon mesh's `e` reaches fourteen kilometres,
where a second-order Taylor term would be 15 m. Every quantity in it is either
small or needs only relative precision. The stand-in sphere keeps the old path
and does not want a reference: it is body-sized, so `e·e` would be 1e14, and
when it is drawn at all you are looking at it from space. `u_SeaDepth` replaces
the waterline subtraction with the answer, taken in double on the CPU.

**How wrong the old height was, measured rather than estimated.** The old
formula is the true height *rounded*, so the difference between the two is the
error it was carrying. Painted into the blue channel of the same debug frame:

| | mean | worst |
|---|---|---|
| Earth at 6,371 km | +0.157 m | **1.145 m** |
| Earth at 250 km | +0.012 m | 0.047 m |

Both bigger than the 0.5 m / 0.016 m this roadmap item was opened with, because
that estimate covered `length()` alone and the `world.xyz - u_Origin`
subtraction contributes as much again. The two scales agree with each other to
4% — 1.145 × 250/6371 = 0.045 against 0.047 measured — which is what a purely
relative float error should do and is the check that the number is the format
rather than the terrain.

**And it changes almost nothing on screen, for a reason worth writing down.**
Every band the height drives is hundreds of metres or wider — the altitude ramp
spans `u_Relief`, which is 15.9 km at 1:1 — except one: the beach, at
`3 × voxelSize = 4.78 m`, where 1.145 m is a quarter of the band and can put a
pixel on the wrong side of the waterline outright. So the A/B of the real frame
is 1,520 pixels differing by a single 8-bit level at the default site and
**zero** pixels at `--land Earth`. The reason is not that the fix does nothing:
the same debug pass counted the pixels whose height falls inside the beach band,
and there are **0 of 579,001 and 0 of 443,698**. Neither view contains a
shoreline. The defect is real, measured, and currently invisible for want of a
frame that looks at a beach — which is worth saying plainly rather than
implying a visual improvement nobody can see.

**Still float, and deliberately.** `settings.OceanRadius` is a `float` member,
so at 1:1 the sea radius itself is only known on a 0.5 m grid. That is a uniform
offset in where the waterline sits, not a per-pixel quantisation, and the water
shell reads the same value, so the sea surface and the terrain's shore agree
with each other. Making it a double ripples into the bisection that finds it and
into the water material; listed under known approximations rather than done.

### 2026-08-27 (the density sampled in double, and a fingerprint that was half stack)

The other half of 1:1. `--earth-radius 6371000` has streamed, rendered and been
walked on since 25 August, but only the *reading* of the field was exact:
`PositionFrom` and `SampleDistanceFrom` subtract on the integers, chunk meshes
carry their own double origin, and the surface physics world is centred on the
landing site. The *writing* was not. `FillChunk` evaluated the generator at
`PositionOf` — `origin + i * voxelSize` in float, both terms about 6.4e6 — so
the sample stored at a lattice point was the density at somewhere else.

**The bound, from the float format alone.** At Earth's radius the origin is
−6,386,935.5 m and the voxel is 1.59275 m. A float carries 24 bits, so in that
range an ulp is 0.5 m; each axis of `PositionOf` is wrong by up to 0.5 m and
the radial displacement by up to `sqrt(3)/2 = 0.866` m — 54% of a voxel, which
is the "half a voxel" the roadmap had been asserting without a number behind
it.

**The measurement, against arithmetic the field does not contain.** `Density`
has an exact zero: along a direction `d` the surface is at radius
`Radius + Relief(d)`, one line, and nothing in the path being tested computes
it — not the sparse chunk store, not the encoding, not the trilinear
reconstruction, not the relief patch the fill actually interpolates. So stand
on that point and ask the field how far it thinks it is from the surface. It
should say zero. `VoxelPlanet::ReportSurfaceError` walks 8,192 points on a
sunflower spiral over the landing site and reports what it says instead:

| | mean | worst | worst as a voxel |
|---|---|---|---|
| Earth at 6,371 km, before | 0.1673 m | 0.8509 m | 53.4% |
| Earth at 6,371 km, after | 0.0121 m | 0.1247 m | 7.8% |
| Earth at 250 km, before | 0.0051 m | 0.2016 m | 13.4% |
| Earth at 250 km, after | 0.0042 m | 0.2015 m | 13.4% |

The 0.8509 m sits just under the hand-computed 0.866 m bound, which is the
agreement that says the diagnosis was right rather than merely plausible. What
is left after the fix is the relief patch's own interpolation error — measured
on the same frame at 0.0721 m worst — plus the trilinear reconstruction of a
curved surface, and the lattice contributes nothing further.

**At the default 250 km scale it changes almost nothing, and that is the
honest result.** The float lattice error there is 0.047 m, well under the patch
error that dominates the residual; the frame does change, but the mean
difference over the pixels that move is **1.7 of 255** — a shading dither. At
1:1 the same comparison is **21.4 of 255 over 68% of the frame**, and 218,000
pixels move by more than 32. The change was never for the scale the demo opens
at.

The fix is three lines of arithmetic and one signature. `VoxelField3D` gained
`PositionOfFixed`, and `Fill`/`FillChunk` now hand the generator a `dvec3`;
`VoxelPlanet::DensityFixed` takes the length and the `- Radius` in double and
drops to float immediately after, because the result is small and the
*direction* never had a magnitude problem; `ReliefPatch::Centre` became a
`dvec3` so that subtracting it from a nearby point is not the difference of two
numbers each already rounded to half a metre. Cost: nothing measurable. A
chunk fill is thirteen octaves of value noise, and this is three multiplies and
three adds in front of it.

**OpenWorld and the Voxel terrain demo are provably untouched**, which is
better than a screenshot: their origins and voxel sizes are exact dyadic
rationals, so `origin + i * voxelSize` is bit-identical in float and in double
over every lattice coordinate either field holds — checked over all of both.
Confirmed anyway by generating OpenWorld's terrain fresh against chunks its
cache had stored on 19 August under the old code: the captured frame is
byte-identical.

**And then the cache stopped rebuilding itself.** `VoxelPlanet::Fingerprint`
exists so that changing the terrain function throws the stored chunks away
instead of handing back a world that no longer exists — and it sampled
`Density` at a float position, which is neither where a voxel is nor the
arithmetic that computes one. So the day the fill moved to `PositionOfFixed`,
every `.site` and `.edits` file on disk described a planet the generator no
longer makes, and the hash did not change by a bit. It samples `DensityFixed`
at a lattice point now, through the same function the fill calls.

Printing the hash to check that was working turned up a second, older bug in
the same eight lines. `mix` copied a **four**-byte float into an
**eight**-byte `unsigned long long` that was never initialised, and hashed all
sixty-four bits. Half of every sample was whatever the stack held. It showed as
`Fingerprint()` returning two different values at its two call sites —
`OpenEdits` and `OpenSiteCache`, three lines apart with nothing in between —
agreeing exactly in the low thirty-two bits, which is the signature of the
thing. That is why the site cache had been rebuilding on runs where nothing had
changed: a fingerprint that is partly garbage sometimes reproduces and
sometimes does not, and the failure mode looks exactly like a cache working.
Debug site preparation is **10,461 ms cold against 3,181 ms warm** now, to a
byte-identical frame, and the warm path is taken every run instead of some of
them. Neither ASan nor UBSan catches an uninitialised read; the trace did.

**Still float on the GPU, and now with a number on it.** The terrain shader
computes its shading height as `length(v_Position) - u_SeaRadius`, where
`v_Position` is `world.xyz - u_Origin` — planet-centred, and at 1:1 about
6.4e6 in `float32`. Measured over 20,000 directions, `length()` alone
quantises the height by 0.019 m on average and 0.5 m at worst, and the
subtraction that produces `v_Position` costs another 0.5 m an axis before it.
Against a beach band of 4.8 m that is up to a tenth of the band, so the visible
consequence is terracing within a few metres of the shore, at 1:1 only — 0.016 m
worst at the default scale. It cannot be fixed by rearranging the shader,
because `float32` has no more bits: it wants a per-vertex altitude computed on
the CPU in double, or a per-chunk reference altitude plus a first-order
expansion over the chunk (the second-order term is 4e-5 m over a 22 m chunk
half-diagonal, so one term is plenty). Split out as its own roadmap item,
because it is a different mechanism from this one and the sphere stand-in, the
horizon mesh and the water all share the shader.

### 2026-08-26 (clouds, haze, a mip chain that never existed, and a static portal)

Three requests landed together: the planet "looks pretty good on the ground
but no longer looks like a natural planet from space," clouds and fog to
soften that, and a first gameplay mechanic — a portal to a small pocket
dimension, standing in for a toolshed. Sequenced smallest and most isolated
first.

**The orbital speckle wasn't the biome logic — it was that no texture in the
engine had ever had a mip chain.** `BuildColourMap`'s moisture/warmth channels
come from a flow-accumulation log ratio, inherently noisy texel-to-texel, and
a first attempt blurred a copy of those arrays before writing them into the
map (`BoxBlurEquirect`, radius 1, leaving `BuildHydrology`'s and
`DeriveWater`'s own conservation checks reading the untouched originals). That
produced **zero visible change** — before/after captures of Earth from orbit
were identical. The real cause was upstream of any blur: `OpenGLTexture2D`
called `glTexStorage2D` with a hardcoded level count of 1 and `GL_LINEAR`
minification everywhere a texture is created, so a 1024×512 equirect map
viewed from four planet-radii out — where many texels land on one pixel, worst
at the poles — was sampled with a single bilinear tap per pixel: exactly as
aliased as `GL_NEAREST`, which reads as fine uncorrelated speckle no amount of
smoothing the *source data* can fix, because the *sampling* was the problem.
Fixed engine-wide in `OpenGLTexture.cpp`: a `MipLevelCount` helper sizes
`glTexStorage2D`'s immutable storage for a full chain, `GL_LINEAR_MIPMAP_LINEAR`
replaces plain `GL_LINEAR` on the minifying side (both in the constructors and
in `SetSmooth`), and `glGenerateMipmap` runs after every upload
(`UploadDecoded` and `SetData`). The blur stayed — it's still the right fix for
noise baked into adjacent texels *within* one mip level — but the mip chain is
what made the difference visible at all. Verified with a before/after capture
of Earth from orbit (smooth coastline gradients, no pixel-scale noise) and a
ground-level regression capture that reads identically to before, since the
ground shader never samples the map.

**Clouds and haze, additive.** `VoxelPlanet::BuildCloudMap` bakes a 512×256
equirect coverage texture from four octaves of the existing `Noise3D` at a
cloud-specific seed offset — low frequency, so it doesn't need the biome map's
resolution. `SolarSystem::DrawClouds` draws it as a translucent shell partway
between the surface and the atmosphere's outer radius, rotated by its own
accumulated `m_CloudDrift` about the body's spin axis rather than following
`SpinAngle` directly, so the cloud deck visibly drifts relative to the ground.
**Verifying the drift found a scale mismatch, not a bug**: captures thirty and
three hundred steps apart were pixel-identical even with `--years-per-second`
turned up, because a nearby view (`--goto`, within six radii) drives the clock
from the slow real-time day cycle, not the orbital rate that flag controls —
`dt` came out to `1.26e-8` years/step, far too slow to show anything in a few
hundred steps. Confirmed the mechanism was correct all along with
`--capture-step 200000` (terrain rotated ~332°, clouds ~37° relative,
matching the hand-computed rates exactly), which is the number that mattered,
not the earlier captures. Haze mixes terrain colour toward `u_Sky` by
`1 - exp(-camDist * u_HazeDensity)`, the same extinction shape
`DrawAtmosphere` already uses, scaled from `AtmosphereDensity` so an airless
body (the Moon) gets `haze ≈ 0` for free. The first calibration
(`3.0e-5`) cited a stale "12.6 km horizon" figure left over from before the
system was rescaled to 1:1 — the real horizon at this scale is 949 m,
confirmed against `docs/HANDOVER.md`. Pixel-value comparisons (not
eyeballing) walked the constant through 100x, too strong and visibly tinting
nearby terrain, before settling on 10x (`3.3e-3`) as the shipped default,
verified as a clean monotonic gradient toward `u_Sky` with distance. The
"Haze" ImGui slider's range predates this number and tops out at `1.5e-3` —
narrower than the default it's meant to expose — and hasn't been widened yet.

**A static portal to an empty pocket dimension**, standing in for a toolshed
until there's something to put in it. `PocketDimension` (new,
`TestEnv/src/PocketDimension.h`) owns five static box colliders for a 6x6x3 m
room with one open doorway, a small offscreen `Framebuffer` rendered from a
fixed interior camera each frame and shown through the doorway as a live
"window" (`Egss::Renderer::Submit` with a small unlit textured shader — not
`Renderer2D::DrawRotatedQuad`, which only rotates about one axis and can't
orient a window tangent to a sphere), and plane-crossing detection
(`UpdateCrossing`) that teleports the player between the portal and the room
in either direction, carrying the lateral/height offset they crossed at so
walking through slightly left of centre comes out slightly left of centre on
the other side. Gravity, grounding and the water/horizon follow-rebuilds all
branch on `Egss::PhysicsWorld3D::InPocket()` — the room has its own fixed
"down" rather than a radial one, and the water/horizon rebuilds are the
required half of that branch, not an optional one: the room sits 2,000 m from
the landing site along the portal's own facing, which every step would
otherwise read as "walked off the edge of the streamed terrain."

**Debugging this cost more than building it, and the actual bug was never
where it looked.** The window was invisible — not even a debug-magenta,
non-culled quad showed up — for long enough to suspect the transform math
first. It wasn't: a trace of the submitted quad's basis vectors, hand-checked
against the landing site's own `up`/`along`/`lateral` triad, confirmed the
geometry was exactly where it should be. The actual cause was the *view*: a
trace of the angle between the camera's default heading (which faces back
toward the ship) and the direction to the portal measured 68 degrees, well
outside even a 65-97 degree frustum — the portal was being drawn correctly,
off-screen. Confirmed by forcibly orienting the camera at it for one capture,
which showed the window exactly as designed. Fixed at the source rather than
patched at the symptom: the portal's placement in `BuildSurfaceWorld` moved
from a pure lateral offset to a blend of `lateral*6 + along*4`, landing it
inside the default view (measured ~27 degrees off-axis on arrival) instead of
outside it. The same pattern repeated one level in: standing inside the room
and looking sideways read as almost pure black, which forcing the material's
`u_Emissive` to 1 (bypassing all lighting) immediately proved was full,
correctly-transformed wall geometry, not a missing draw call — the sampled
pixel value `(8, 8, 12)` matched the *hand-computed* ambient-only shade for
that exact wall color and sky term to within rounding, not the scene's clear
color it happened to resemble. The room is genuinely dim from some angles by
design (`u_Sky = (0.12, 0.12, 0.16)`, one fixed light, no windows) and reads
clearly enough facing into it, which is what a player who just walked through
the doorway actually does. Both crossing directions were verified with a
scripted position override (no recorded input exists for this landing site)
and an `EGSS_TRACE` on every `InPocket` transition, confirmed firing exactly
once each way for a clean approach-and-return.

**Deliberately not built**: placing the portal on an arbitrary surface, and
stocking the room with anything. `Place()` already takes the portal's position
and facing as parameters rather than hardcoding them, so a placeable portal
later only changes the caller; stocking the room is additive physics bodies
dropped at `RoomLocal()` plus OpenWorld's existing `Tool`/`m_HeldTool` carry
pattern, adapted the same way this pass adapted movement for a non-radial
"up." See `docs/HANDOVER.md` for both.

### 2026-08-26 (a phantom lake under the lander)

**Swimming exists now**: `ApplyBuoyancy` gives the player a spring toward the
local water's surface plus drag while submerged, and the eye going under it
draws a translucent full-screen quad the same way `Cube3D` blits its own
framebuffer — after the 3D scene, not through it, so it survives `--hide-ui`
and shows up in a capture. `SurfaceWater` gained `LevelNear`, a query at an
arbitrary point rather than only at an edit, sharing the direction-to-column
math `Touch` already had (now `ColumnAt`, factored out rather than duplicated
a third time).

**Testing it at the actual default landing site found something the feature
itself didn't cause.** `SurfaceWater::Report()` said 63.3% of the local grid
was wet before any of this landed — nothing had ever asked, because nothing
before this physically responded to the answer. The lander's own column read
18 m "underwater": a rim seed pulled from the planet-wide hydrology map,
sampled at a texel wide enough to cover the *entire* 380 m grid several times
over, said there was a lake 39.5 m above sea level there. The lander's real,
field-sampled ground is 21 m above sea level and has no connection to that
basin — the map's 1.5 km resolution simply cannot see the difference, and
nothing was checking whether the fine terrain agreed before letting that
number seed a flood.

**Fixed by making the map's seed prove itself against the ground it is
seeding.** A rim cell now only accepts the map's wet claim when the claimed
level is within a few voxels of that cell's own sampled ground — a real
shoreline's water sits close to the ground beside it, and a coarse texel
guessing at a point past its own resolution does not. `s_SeedMargin` already
existed for this class of problem but only hid the seam in the *drawn* mesh;
it never stopped the flood from believing a bad seed and propagating it
inward as far as the real terrain permitted. Local wet coverage at the
default site: 63.3% to 4.9%, and the real shore 150 m away (verified with the
camera turned to face it) is unchanged — the fix removes the seed that
disagreed with its own ground, not the ones that agree with it.

### 2026-08-26 (far bodies stopped paying for triangles nobody could see)

Landed while chasing a jitter report: every one of the sixteen bodies in the
table got the same 128x64 stand-in sphere every frame, whether it was the
planet underfoot or Phobos, 11.3 km across and true-scale distant, worth
nowhere near a pixel. Ablating all fifteen non-current bodies cut roughly
230,000 triangles a frame and changed measured frame time by nothing —
confirming the actual cost is draw-call submission (each one a fresh
`Material` instance), not triangle throughput, which is also why the fix here
is about not calling `Submit` at all rather than a coarser mesh.

`WorthDrawing` compares a body's angular radius (`DrawnRadius * scale /
distance`, the small-angle tangent) against 4e-4, under half a pixel at
1080p and this demo's field of view — nothing culled by it could have shown
as more than the single aliased dot the star field already draws behind it.
Gates `DrawBody`, `DrawRings`, `DrawOcean` and `DrawAtmosphere` for every body
except whichever one is currently underfoot or being orbited, and the Sun.
Verified the Moon still draws as a full disc when it is the body being
approached — the cull only drops bodies nothing was going to see anyway.

### 2026-08-26 (axial tilt, and retrograde for free)

**Every body here spun about +Y**, an axis chosen once with no data behind
it, so Venus and Uranus's backward spins had to be faked with a negative
`RotationHours`, and Saturn and Uranus's rings carried their own separate
tilt that agreed with nothing else about the body. The real fix — a general
spin axis — was named and declined twice already, once for the rings and
once for the terrain, both times as "a bigger change than [it] is worth."

**One axis serves every body**, tilted out of the ecliptic pole about the
same reference `SkyDirection` already anchors the star catalogue to: global
+X, tipping +Y toward +Z by each body's real obliquity. That is a real
simplification — no two planets' poles actually point the same way relative
to their own orbits — but it is the one direction this demo already checks
against real data, and Earth's tilt (23.4392911°) now matches the constant
`SkyDirection` was independently verified against to 0.003 degrees, so the
ground agrees with the sky.

`RotateY` generalised to `RotateAboutAxis`, a Rodrigues rotation restated in
the project's own sign convention (`+x` toward `+z`, not glm's `+x` toward
`-z`); `SpinMatrix` rebuilt as the matrix form of the same formula, checked
against the vector form rather than derived from it on paper. `ToScene`,
`ToFixed`, the two places that called `RotateY` directly (`SetTimeOfDay`'s
daylight search, `CarryWithTheAir`'s co-rotating hover) and the ring basis
all moved onto it.

**Retrograde stops being a sign.** `RotationHours` is positive for every body
now (Uranus 17.24, Venus 5832.5), and Uranus (97.77°) and Venus (177.4°) turn
backward, as seen from the ecliptic pole, purely because their tilt passes
90°. Checked against a formula the rotation code does not otherwise compute:
`RotateAboutAxis((1,0,0), axis, angle)` at small angle drops its third term
(the axis never has an x-component, so `axis · (1,0,0) = 0`), leaving
`(cos angle, -sin(tilt) sin angle, cos(tilt) sin angle)` — an azimuth that
grows at rate `cos(tilt)` as `angle → 0`. Measured: Earth 0.917482, Saturn
0.893136, Uranus -0.135197, Venus -0.998971, each matching `cos(tilt)`
computed independently, sign included.

**The rings moved onto the same axis as the body**, closing exactly the gap
the old comment named: Saturn's ring tilt and Saturn's own axial tilt used to
be the same number, 26.73, entered twice by hand. Building the ring's
azimuthal basis by crossing the normal against +Z — fine at Saturn's 26.73 —
is within three degrees of parallel to Uranus's axis at 97.77, a
near-degenerate basis caught before it shipped rather than after. Crossed
against +X instead, which the axis never has a component along, for any
tilt.

**The equirectangular map lookup needed the real inverse rotation, not a
shift along its azimuth.** The old `u_Spin` uniform subtracted a fraction of
a turn from `atan(up.z, up.x)`, exactly right when the only thing spin ever
does is rotate about +Y — and that rotation happens to leave `up.y` alone
too, which is what let `latitude = up.y` pass as correct while actually
being scene-frame rather than body-fixed. Both the terrain and water
fragment shaders now take a `u_Unspin` matrix (the spin matrix's transpose,
which is its inverse for a rotation) and unrotate `up` fully before reading
the map or computing latitude — otherwise a walked, tilted Earth would read
part of another point's climate off the map as its own for part of the day.

Verified: 7 checks — reduction to the old formula at zero tilt, length
preservation, the axis as a fixed point of its own rotation, `SpinMatrix`
against `RotateAboutAxis`, and the retrograde-from-tilt formula above —
passing identically in Debug, Release and Dist. Saturn's rings render
unchanged; Uranus's render nearly edge-on, matching the real planet's
appearance from its near-90-degree tilt, which is the strongest confirmation
available without a second, independent renderer to check against.

### 2026-08-26 (one microphone, two pumps, and which one is broken)

**A single channel carrying two sources is underdetermined, so the whole
question is what breaks the symmetry between the machines.** No inter-aural
delay, no level difference, nothing to beamform with. The premise for this one
was the hard version: two nominally identical fixed-speed pumps on a shared
grid, one microphone, and a fault to place on one of them. Fixed speed removes
the textbook answer -- separating the two harmonic combs -- because the combs
land on top of each other.

What is left is that the two machines stand in different places in the same
room, and a room is a filter. Pump A reaches the mic through H_A and pump B
through H_B, so anything a pump emits arrives wearing its own channel's
colouration. With a healthy baseline recorded for each machine alone, that
becomes a regression: subtract both baselines from the mixture and fit what is
left as a non-negative combination of the two, and the coefficients are the
fractional rise in each machine's own band energy. The unknown fault spectrum
cancels in the ratio.

**The failure mode is in the algebra rather than hidden in the code**, which is
the reason this method was worth building over the alternatives. If the two
baselines become parallel -- two pumps equidistant in a symmetric room -- the
design matrix is rank deficient and no attribution exists at any signal to
noise ratio. The condition number of that 2x2 system is therefore a *site
survey* number: it says whether a microphone position is any use before anyone
installs one. Measured in the demo's own room, 2.4 asymmetric against 12.4 with
the pumps mirrored about the mic, and the second refuses to attribute a fault
it can plainly detect.

Four things were wrong on the way, and all four were found by a number
disagreeing with arithmetic rather than by anything looking wrong.

**The PSD summed to N times what it should.** A sine of amplitude A has mean
square A^2/2, and the Welch estimate came out 4095.96 times that -- a
suspiciously round number, and exactly the FFT size, which said the fault was a
missing constant rather than a broken transform. The normalisation now divides
by the transform length and the check is part of the file's contract.

**Every tolerance in the solver was dimensional.** `fabs(det) > 1e-20` compared
a determinant carrying the units of the basis to the fourth power against a
fixed epsilon; the baselines are PSD bins around 1e-8, so the determinant sits
near 1e-27 and the guard failed on every call. The two-variable NNLS silently
returned its one-variable fallback for ever, which looks exactly like a real
answer because one of the two pumps genuinely does come out at zero. The basis
is now normalised to unit norm before solving, so every epsilon is a pure
number. **The tell was the error bars: all of them exactly zero.**

**Smoothing the spectrum destroys the method, which is the opposite of the
usual advice.** The first version used a coarse FFT and a moving average, on
the reasoning that room colouration is broad and resolution could be traded for
variance. Measured at a 512-point transform, the correlation between the two
baselines rose from 0.930 to 0.987 as smoothing went from none to +/-4 bins --
and transform length pulls the same lever the other way, the unsmoothed
correlation running 0.964 at 256 points, 0.930 at 512 and 0.754 at 2048 --
because what actually separates two room paths is the fine comb structure their
reflections cut into the spectrum, and a moving average is precisely the
operation that removes it.
Finer transforms, no smoothing.

**The fit has to be done in sub-bands.** Its model needs the fault's own
spectrum to be flat across whatever is fitted, and a bearing resonance is not
flat across two kilohertz. Fitted as one band the misfit ran 60x the estimator
noise, the error bars had to be widened to compensate, and a *larger* fault
became harder to place than a small one. Split into eight, each fit is honest
and they are combined by inverse covariance: fit quality went 0.20 -> 0.91,
reduced chi-square 59.5 -> 6.6, and attribution started working in both
directions instead of always naming pump A.

**Detection and attribution turned out to be different questions**, and
conflating them was the last bug. Testing each coefficient against zero asks
"is A worse than its baseline" and "is B worse than its baseline" separately,
and on a single-machine fault both answer yes -- so every fault was reported as
"both", while the correct pump always had the larger coefficient. The ranking
was never wrong; the test was asking the wrong thing. Now the sum answers "is
anything wrong" and the difference answers "which machine", the latter needing
the covariance term because the two coefficients are strongly anti-correlated.
A related subtlety: those statistics use the *unclamped* estimates. Clamping at
zero is right for a health score and wrong for a test statistic -- it makes the
sum one-sided, and a 3 sigma threshold that should fire on 0.1% of clean
windows was firing on one window in five.

Three other methods are in the demo because their limits are the finding. A
broadband level meter detects and cannot attribute, by construction. The
spectral residual isolates what broke and still not which machine. Envelope
demodulation -- the standard bearing method -- works exactly as well as the two
shaft speeds differ, and the demo reports the Fourier limit directly: two
defect lines 0.7625 Hz apart need a window of at least 1/0.7625 = 1.31 s, and
measured, a 0.8 s window reports them unresolvable and a 16 s window separates
them by 10.1 dB against 4.5 dB. Worth stating that a shared grid locks the
*line* frequency and not the shaft: slip tracks load, so two unequally loaded
4-pole motors sit at 24.75 and 24.50 Hz and that 0.25 Hz gap resolves in four
seconds. Whether it exists on a given site is a measurement, not an assumption.

The cepstral method is the one with a hand-check the code cannot fake. An echo
at delay d becomes an additive ripple in the log spectrum, so it appears as a
cepstral peak at quefrency d -- and d is predictable from the traced geometry as
(reflected path - direct distance)/343, arithmetic the cepstrum is never told.
Predicted 0.004665 s and 0.015160 s for the two pumps in the test room;
measured 0.005000 and 0.015000, which is one analysis bin. On a synthetic echo
at 137 samples the peak lands on sample 137 exactly.

Scored against its own ground truth over a scripted run covering all four
cases -- neither faulty, A only, B only, both -- the channel method is correct
on every settled window and never names the wrong machine; the only
disagreements are the one analysis window of lag at each transition, which an
8 s window cannot avoid. Frame cost 0.08 ms in `OnFixedUpdate`.

`TestEnv/src/PumpSignal.h` holds the arithmetic and includes no engine headers,
so it is checkable in a second rather than in a build.
`TestEnv/src/PumpDiagnostics.h` builds the room, traces it with `Acoustics3D`,
and draws the answer. **What you hear and what is measured are deliberately not
the same signal**: the mixer is stereo and pans each voice by direction, which
would hand the analysis a left/right cue a real mono microphone does not have,
so the speakers get `PlayAt` with the traced reflections and the numbers get a
mono sparse convolution built from the same trace.

### 2026-08-26 (a landscape you can see, and the ten seconds that were water)

Three complaints, all of them right: the planet had no landscapes, the water
did not work, and landing was unusable. Each one measured before anything was
changed, and each measurement said something different from what the symptom
suggested.

**The relief existed and none of it was at a size you could see.** The
planetary spectrum is a 1/f fractal anchored at `FeatureSize`, and 1/f is the
problem: Earth's 625 m of relief spread from a 28.75 km wavelength downward
puts `625 * 400/28750 = 8.7 m` of ground inside the four hundred metres you can
see from head height. Measured over two dozen sites on the sphere: **8.5 m**.
That is a lawn, and it is *correct* for a fractal of that shape -- which is why
adding octaves could never have fixed it. An octave fine enough to be seen is
an octave whose amplitude is a centimetre.

So `Settings::Landscape` is its own layer with its own anchor: ridged, six
octaves from 4.2 km down, 700 m of amplitude, masked by a slow uplift field so
that mountain country is sixty kilometres apart and the plains between keep 18%
of it. A real landscape is not self-similar; it has a scale, and that scale
comes from erosion and uplift rather than from the shape of the planet. The
same two dozen sites now measure **49 m of relief inside 400 m** on average,
133 m in the ranges and 18 m out on the flats, and 340 m inside 1.6 km where
the mountains are.

**And past four hundred metres there was no geometry at all.** Streamed chunks
stop at the load radius; beyond it the planet is drawn as its stand-in sphere,
inset below the deepest valley so real terrain always wins the depth test. With
the landscape layer in, that inset is **879 m** -- so the ground ran out at four
hundred metres and the horizon was a smooth ball most of a kilometre below your
feet. A 300 m mountain clears the horizon from `sqrt(2 R h) = 12.2 km` away
here, forty times further than anything was being drawn.

`HorizonMesh` is a polar grid about the landing site, 340 m to 23 km, displaced
by the same `Relief` the voxels come from. Rings spaced geometrically, so a
quad subtends the same angle at both ends: 16 m between samples at the near
edge and 665 m at the far one, 18,432 vertices for the other twenty-two
kilometres of the world. Where it overlaps the chunks it is sunk by four
millimetres per metre out, which is 1.2 m at the seam and invisible at 0.17
degrees, and guarantees the real geometry is in front. It agrees with the
generator to **26 mm** on the inner ring, where the sink is zero.

**Landing cost 32 s in Debug and 24.6 s of it was water.** The attribution was
not close to the guess. Filling chunks was 8.5 s, meshing was inside that, the
sixteen-body orbital integration was **4 ms**, and the rest was the surface
water being rebuilt fifteen times while the ground streamed in -- correctly,
since water here is a consequence of the shape of the ground and the ground
kept arriving.

The fix is to stop the ground arriving in pieces. `PrefillSite` streams the
whole load radius with a budget nothing can exhaust, before the first frame is
drawn, and the water is built once on ground that is already finished.
Repeatedly, until a pass changes nothing: a chunk is only meshed once its seven
high neighbours are filled, so the outermost shell's meshes always lag their
fills by a pass.

**Then the fill itself, which was 93% of what was left.** `Density(p)` is
`|p| - Radius - Relief(p/|p|)`, and the third term is thirteen octaves of value
noise -- measured at 1.74 ms for a chunk's 4,096 voxels against 0.146 ms to
march the same chunk into triangles. It is a function of *two* variables being
sampled over three: every voxel in a radial column has the same direction and
therefore the same relief, and the cube holds sixteen of them.

So the relief over a chunk is sampled onto a small grid in the tangent plane
and read back bilinearly, in gnomonic coordinates -- `u = (p.t)/(p.n)` --
which is scale-invariant, so a voxel's `(u, v)` costs three dot products and no
normalise at all, and which inverts exactly, so the grid is built at precisely
the directions it is later read at. Nothing can crack: `FillOneChunk` writes a
chunk's own cells and no others, so every lattice point takes its value from
exactly one grid. **1,610 chunks in 2,795 ms became 286 ms.**

The spacing is solved for rather than picked, and the first attempt got the
criterion wrong. Sixteen samples across the shortest wavelength is right only
while every term has the same amplitude, and the landscape layer's finest
octave is 22 m at 131 m against the planetary spectrum's 0.6 m at 56 m --
**six times the curvature at twice the wavelength**. The wavelength rule gave
3.51 m spacing and 0.26 m of error, a sixth of a voxel. Solving
`h = (1/pi) sqrt(2 tol / max(A/L^2))` for two per cent of a voxel gives 1.55 m
and **98 mm worst, 3.2 mm mean** -- reported in the log on every landing,
because it is an approximation and the number should not have to be taken on
trust. It is not the quadratic bound, and the reason is that `1 - |n|` has a
corner where the noise crosses zero: a kink is not a sinusoid, and error across
one falls off linearly.

**The default landing site is a constant, and that is the load-bearing part.**
The demo opened four Earth radii out looking at a blue marble; it now opens
standing beside the lander, which is where the game this is a prototype for
spends ninety per cent of its time. The direction was chosen by scoring forty
thousand points on the Fibonacci spiral for dry ground, standable slope, relief
close enough to see, high ground within six kilometres and water within walking
distance. It measures 21 m above sea level, slope 0.103 where the lander
stands, 233 m of relief inside 400 m, 187 m of high ground within 6 km and the
shore 150 m away -- a beach at the foot of a mountain. `--orbit` gets the old
opening back.

Fixed means it can be kept. `OpenSiteCache` stores the prefilled chunks beside
the edits, keyed by the same density fingerprint so changing the terrain throws
them away rather than loading a landscape that no longer exists. Only the
chunks that hold something, and only while prefilling -- caching sky and rock
would multiply the file by twelve to save nothing, and caching everything a
walk touches is how a site cache becomes unbounded. Verified bit-exact: a cold
run and a warm one produce identical frames.

    landing, Debug      before          after
    to the first frame  8.4 s           12.9 s   (10.4 s of prefill -> 3.1 s)
    then                400 frames at   no landing phase; 27 ms a frame
                        94-133 ms

**The clock is wound, and the sun goes behind you.** At `m_Time = 0` the spin
is the identity, so whether the default site opens in daylight was decided by
an epoch chosen for the orbits. It opened at dawn. Winding the clock is the
physical answer -- the sun is where the sun is, and this is what time it is
when you arrive -- and it costs a fraction of a day of orbital motion. The
first version wound it to two hours before local noon and produced a black
mountain against a blue sky, because facing the high ground with the sun beyond
it means facing the one face of it the sun does not reach.

**Which is also why the sky is now a light.** Terrain was `0.05 + 0.95 *
diffuse` and everything else `0.06 + 0.94 * diffuse`, where the floor existed
to stop pure black and produced exactly that: five per cent of a dark green is
four units out of 255. On a body with air a slope facing away from the sun is
lit by the whole dome above it, which is why a shaded hillside on Earth is
blue-grey and the same hillside on the Moon is black -- `SkyLight` is zero
without an atmosphere, so both come out of one expression. `0.5 + 0.5 *
dot(normal, up)` is the share of the dome a surface can see; it is geometry,
not a tuning constant. Two things had to be got right: `Scatter` is an
extinction ratio and not the colour of the sky, so using it directly turned
every hillside teal, and the strength is a third of the direct beam rather than
the physical fifth, because the frame is written to an 8-bit buffer with no
exposure curve on it.

**A tree through the hull, twice.** The forest vanished at the default site --
zero plants in 1,467 chunks -- and the reason was a line whose comment claimed
it put clearings in a wood. It sampled noise at `Radius / (FeatureSize * 1.5)`,
which on Earth is 5.8 cycles around the whole planet: a 43 km wavelength, which
over the four hundred metres you can see is not a field but a constant. Either
the whole landing site was forest or none of it was, and thirty-five per cent
of the planet drew the short straw. Measured there: 20,538 candidates, 4,188
reaching that line, **every one rejected**. Six hundred metres is a clearing
you can walk across.

Then a tree came up through the lander, and a twenty-metre clearing did not
stop it. `m_SiteFixed` is the lattice point nearest where the ship *arrived*,
which is twenty metres up, so a straight-line distance put a tree standing on
the pad at exactly the clearing radius. A clearing is a radius on the ground;
the radial component comes out.

**Water was tinted by the view angle and nothing else.** `mix(u_Deep,
u_Shallow, facing)` says how much sky is being reflected and says nothing about
what is underneath, so a puddle two centimetres deep and a lake forty metres
deep were the same colour and the surface read as a blue sheet laid over the
ground. The flood already knows the depth -- the level it settled at, less the
ground it settled on -- and it travels in a texture coordinate the mesh was not
using. Beer's law on twice that depth, since light goes down, off the bottom
and back up, and the same number floors the alpha so shallow water is
see-through and an opaque sheet no longer runs up the beach. The view angle
keeps its real job as Fresnel.

Verified: Solar byte-identical across Debug, Release and Dist and between runs,
and identical whether the site cache was cold or warm. `VoxelPlanet.h`,
`SurfaceWater.h` and `HorizonMesh.h` are included by `SolarSystem.h` and
nothing else, and no engine file was touched, so no other demo can have moved.

### 2026-08-26 (a ship to get back to, and a camera seven metres from the player)

**The vehicle did not exist.** `L` toggled between a physics capsule and a
camera, and the ship was only the fact that you could leave. Making it an
object is what turns a landing into a place you have to get back to -- and it
cost almost nothing, because the player was already a rigid body and gravity
here is applied per body rather than as a world vector.

A lander built from three primitives and no asset file: a tapered hexagonal
hull, three splayed legs and a nozzle, 66 triangles. It comes down with you,
stands where it landed, and `L` now refuses to lift off unless you are within
six metres of it -- which is the whole loop the demo was missing. Walk, dig,
and then find your way back to the one object that can take you off.

Underneath it is a capsule rather than a box: the narrowphase only tests a
box's corners against a distance field, so one resting on rough ground sinks a
corner or jitters, while a capsule is sampled along its segment. It is held
upright the same way the player is -- set, not solved -- because a capsule left
to the solver tips over on a slope and then rolls to the equator, there being
always a downhill on a sphere. The first one did exactly that and ended up on
its side with its legs out sideways. It is still a dynamic body, so digging the
ground out from under it drops it; it just does not lie down.

**Then it was in the wrong place, and the ship was not the thing that was
wrong.** The physics said the hull was 5.12 m away and 0.2 degrees off the
view; it drew at the edge of the screen. Measuring the *player* the same way
gave the answer: **1.20 m expected, 7.37 m measured**.

At 250 km with an hour to the day the surface moves 436.3 m/s, and a sixtieth
of that is 7.3 m. The walking camera was converted from the planet's frame into
scene coordinates inside the movement code, using the spin angle as it stood
*before* the clock advanced -- and everything else in that frame is drawn with
the angle as it stands after. The camera sat one step of rotation away from the
body it belongs to, and the ground sat seven metres from where the player was
standing on it.

**Invisible for as long as it has existed**, because everything in that frame
moved together and the player is not drawn. The lander is the first object
whose position on screen could be predicted from the physics, and it came out
wrong by exactly one step of spin. The surface camera is kept in the planet's
own frame now and converted once, at the end of the step, after the clock. The
player measures 1.20 m from the camera, which is the eye height, to the
centimetre.

Two smaller things fell out of it. The rocks were scattered over a forty-metre
square centred on the touchdown point -- which is also where the ship stands and
where the player steps out -- so the moment the player stopped starting *at* the
landing point, a 1.12 m boulder sat 2.43 m from the eye and filled 27 degrees
of the view. Nothing was wrong with it; it was simply in the way. They scatter
in an annulus from 11 m out now. And the player steps out of the *back* of the
hull along the heading it came in on, so the thing it has to walk back to is in
front of it when it arrives, rather than off the edge of the screen.

Landed captures move, because the camera did. They are byte-identical across
all three configs and between runs; every other demo is unchanged.


### 2026-08-26 (water at the resolution of the ground)

**The global drainage grid is right and cannot be drawn.** A texel is 1.5 km;
a lake is smaller than one. Yesterday's attempt to draw a surface from it put
8.14% of the frame in the sky. So the same algorithm now runs again, locally,
over the terrain that is actually there.

`SurfaceWater` is a grid of **columns** over the streamed region -- 128 by 128,
about six metres apart -- each holding the radius of the rock and the radius of
the water above it. Two things follow from columns rather than voxels. A lake
at rest is one number a column instead of a stack of full cells, which is what
makes it affordable to re-run live. And "level" means *radius from the planet's
centre*: over 800 m of a 250 km sphere a tangent plane is out by 0.32 m, which
would be a fifth of a voxel of slope across every lake.

Priority-Flood again, and seeded by the **rim** of the region rather than by
the sea -- water can leave across the edge, at whatever height the planet-wide
answer says it stands at out there, which is how the local answer and the
global one agree at the join. Interior columns are never seeded: whether they
hold water is exactly what is being asked.

**The requirement, tested as stated.** Digging down to the level of the sea in
the middle of a continent must not produce water; cutting a channel to it must:

```
before: 10654 wet columns, 30292.7 m of standing water; digging into
        ground 0.94 m above a waterline 76.64 m away
after a 1.1 m pit, floor 0.75 m below the waterline: 10654 wet columns
  [ok ] a hole below the waterline with no path to water stays dry
the channel reached water after 74.2 m of digging (88 bites of 0.8 m)
  [ok ] and fills once a channel is cut from it to something wet
```

Two holes at the same depth; the only difference is whether anything connects
them. That is the whole model.

**The test was wrong five times before the code was once.** Worth listing,
because they were five different ways of being wrong: a pit dug at a fixed
distance landed in a lake, and the volume going up was read as a failure when
what it had done was deepen a lake; a rim of 0.55 m was removed by the same 8 m
sphere that made the hole, so the pit connected on the stroke that dug it; a
demand for a four-metre rim two dozen metres from water found nothing on a
gentle shore; a neighbour search ran off the end of its rows and reported water
682 m away from a scan that only looks 84 m; and a channel of sixteen bites at
0.8 m gave up eleven metres into a seventy-six metre walk. The scenario is
derived from the terrain now -- find the dry column furthest from any water and
size the pit to it -- rather than prescribed.

**Two real defects came out of it.** The ground was read from the *generator*
at build time and from the *field* after a dig, and those disagree by a few
centimetres -- enough, at a waterline, to put twenty-four columns under water
for a pit narrower than one of them. The ground had not moved; the question
had. One source now, the field, with the generator as the fallback where
nothing has streamed yet and a rebuild each time materially more ground
arrives. And the wet threshold was an epsilon, which is enough to stop float
equality calling dry land wet and not enough once the ground is a sampled,
interpolated surface that dips a few centimetres everywhere: a quarter of a
voxel is the shallowest puddle the terrain can represent.

**Every sheet is level to 0.00000 m** -- once the measurement stopped comparing
across the seeded rim. It reported 0.36 m at two columns in and nothing at six,
which is how far the planet-wide map's disagreement (up to 2.45 m) reaches
before the local flood overrides it. The drawn mesh leaves the same six columns
out, so what is on screen is exactly what the local terrain decided.

**What this does not do**, stated so it is not mistaken for done. The fill is
instantaneous: a dig re-runs the flood, which gives the correct *end state* and
does not show water moving. And a column has one water surface, so digging
*under* a ridge does not carry water through it -- digging *across* one does,
which is the case that was asked for.

Captures byte-identical across all three configs and between runs; every other
demo is unchanged.


### 2026-08-26 (water where water can get to, and a lake that is smaller than a texel)

**The sea was a shell at a radius, which says one thing: that water is
everywhere below a given altitude.** That is the thing this was always meant to
stop saying. The drainage pass already knew better; it just was not being
asked.

Three corrections to the model, each of which the report now measures:

**The flood was seeded from every cell below sea level.** Connected to the
ocean or not -- so a basin ringed by land and floored below sea level was
seeded *as* ocean and came out full, with no path by which a drop could have
reached it. Flood-filling from the actual sea instead leaves it as what it is.
Lakes went from 6.17% of the land to 13.74% as enclosed basins stopped being
counted as sea.

**Every hollow filled, which assumes it rains everywhere and never
evaporates.** Death Valley is below sea level and bone dry. Each basin is taken
as a whole now -- its own connected component -- and kept or dropped on the
moisture over it, as a whole because deciding cell by cell leaves a lake with
holes in it. **185 basins** are too arid to hold one.

Which gives the number the whole design exists for:

```
water: 11.95% of land under a lake, every lake level to within 0.00000 m of
       itself, no ground stands above its own surface by more than 0.00000 m
and 9978 km^2 of land sits below sea level and dry, across 185 basins too arid
       to hold a lake
```

The sea shader now discards on that mask rather than re-deriving a coastline
from the height channel, which could only ever express "below sea level".

**Three of my own measurements were wrong before any of the code was.** Worth
listing, because each was a different way of being wrong:

- The levelness check compared any two adjacent wet cells -- which includes two
  *different* basins meeting at a saddle, and those are supposed to differ by
  exactly the height between their spill points. It reported 25 mm of slope on
  lakes that were flat all along. Checked per connected basin now.
- With that fixed it reported 40 mm, and **that one was real**: the priority
  queue was ordered by the epsilon-filled surface while the *level* was being
  propagated. Priority-Flood is correct because a cell is first reached along
  its lowest path, and "lowest" has to mean the same thing as the value being
  carried. Ordering on the level took it to exactly zero.
- The dry-below-sea-level count skipped every cell below sea level on its way
  in, so it could only ever report zero, which it duly did for two rounds.

**And a near-field water surface was built, measured, and thrown away.** The
ocean sphere is one radius, which is no use for a lake two hundred metres up a
valley, so lakes got a grid on the water surface over the landing site -- each
vertex at the height the drainage pass says water stands at there.

It drew **8.14% of the frame as water standing in the sky**. Not a bug in the
mesh: a texel of the drainage grid is 1.5 km of ground, and the terrain you
stand in is 1.5 m voxels of a rougher surface with more octaves in it. Asking
the real generator whether each vertex had water above the actual ground did
not help -- 48.5% of them did, and the quad count did not change by one --
because what is wrong is not the *height* of the surface but its **extent**:
the map's basin does not exist in this terrain at this resolution, so its
surface cuts through hillsides that the map cannot see.

So the grid can say where lakes *are* and cannot say where their shores are.
Near-field water needs its own pass at voxel resolution, which is the local
cellular automaton that was always the second tier of this item -- and which
now has a measured reason to exist rather than an assumed one. The tile is
gone; the model it was drawing is not.

Captures byte-identical across all three configs and between runs.


### 2026-08-26 (a spade, and a hole that stays dug)

**On a planet, eviction is regeneration.** Chunks are streamed from a density
function and thrown away behind you, so a hole dug and walked away from healed
itself the moment the chunk was released. That is the thing standing between
the demo and water you can dig down to, so it went first.

The pieces existed and none of them were wired up: `EditSphere` in the field,
`ChunkCache` next door in OpenWorld, `SaveChunk`/`LoadChunk` in between. What
had to be worked out is *what to store*. OpenWorld caches everything, because
there a cache buys time and the world fits in a file. Here it buys **data**, and
a planet does not fit in anything -- so only the chunks somebody changed are
written, the fill checks that set before it generates, and everything else stays
procedural.

Left mouse digs, right fills, on the fixed step rather than in an event handler
-- the mouse is in the replay stream and events are not, so a session spent
digging records and replays as itself. `DigRadius` is registered for the same
reason.

**The test is the only one worth running here.** Carve a sphere, throw away
*every voxel the planet has resident* -- not the meshes, the voxels, so the next
stream is a regeneration and not a re-mesh -- then stream it back and look:

```
1.71 m of rock at the spot before digging
69 voxels changed sign, distance now +3.10 m, 2 chunks in the edit file
dropped 16039 chunks of voxels (0 left), regenerating
after regenerating from scratch: +3.10 m
```

Six checks, and the last one is the one that matters: not merely *a* hole, the
same hole, to the centimetre.

**It failed the first time, and the reason was better than the fix.** The
regenerated sample read exactly +1000 m -- `Far`, the sentinel an unallocated
chunk returns, so the chunk had never been refilled at all. `ReleaseBeyond`
takes chunks out of `m_Filled` without resetting `m_ScanFrom`, the watermark
that says how far along the distance-sorted scan everything is known to be
filled. The scan started past the released chunk and never went back. Normal
play hides it completely: release happens at three times the load radius, which
you only reach by moving, and moving resets the watermark anyway. It took a
test that released *without* moving to find that the ground had quietly stopped
existing.

**And the edit file was 75 times too big.** A dig marks two sets of chunks --
the ones the sphere reached, which changed, and the ones below them, whose
meshes now read stale data through the plane they share. The first belongs in
the file; the second belongs in the remesh queue and nowhere near it. Writing
both stored the generator's own output as though somebody had dug it: **150
chunks a hole instead of 2**.

Two more things needed widening or moving:

**`ChunkCache`'s key packed +/-512 into a thousand per axis.** Ample for
OpenWorld's fifty chunks and a two-hundredth of what a planet needs -- at
250 km a chunk index reaches 20,900, and every one past 511 aliased onto
another chunk, which is a cache that hands back the wrong ground. Twenty-one
bits an axis now, the same as the planet's own chunk key, which hit the same
wrap and was widened for it a day earlier. The file is untouched: a record
carries its own coordinates.

**`EditSphere` had `PositionOf` in it**, so at 1:1 a spade would move in
half-metre jumps and take a bite the shape of the lattice. There is a
lattice-relative form now, and the two share one loop with the frame spelled
out as an explicit shift -- so the world-space form evaluates the *same
expression* it always did rather than an equivalent grouping. That care is not
theoretical: it is what kept all six demos byte-identical through this.

The dig ray is sphere-traced in the landing site's frame rather than going
through `VoxelField3D::Raycast`, which works in the field's own coordinates --
the ones everything here has just been moved off.

Captures byte-identical across all three configs and between runs, and every
other demo is byte-identical to its hash from before any of this.


### 2026-08-26 (biomes, and the drainage they come out of)

**The obvious way to place biomes produces coloured noise.** Two more noise
fields, one called temperature and one called humidity, a Whittaker lookup
between them -- and rainforest on a ridge, desert in a valley, and no
relationship at all between where the green is and where the water would go.
What makes a biome map read as a world is that the wet places are the places
water collects, and knowing those means solving the problem a river network
solves. So the drainage came first and the biomes came out of it.

One grid -- the same 1024x512 the height map already used, so a texel means one
thing in both -- and four passes. Sample the relief. **Priority-Flood** seeded
from the sea, raising every land cell until it has a strictly downhill path to
the ocean. Steepest descent on the filled surface. Then one sweep from high to
low accumulating catchment area, which needs no recursion and no visited set
because after the fill every cell's downstream neighbour is strictly lower.

A sphere has no boundary to seed the flood from, which is the one thing that
differs from the textbook version on a rectangle: **the sea is the boundary**,
and a planet without one has no outlet, which is why this only runs on Earth.

**The check is a conservation law the code knows nothing about.** Sea cells
start at zero and only ever receive, so the water arriving in them is the total
catchment of the land -- which is the land's area. The accumulation adds a
number to a neighbour, repeatedly, and has no idea what the planet's land area
is:

```
drainage: 2.201e+05 km^2 of land (28.0% of the sphere),
          2.201e+05 delivered to the sea (0.000% out), 0 stranded, 126 ms
```

Exact, and no cell left without a downhill neighbour. It is also the check that
catches a depression the fill missed -- an unfilled basin keeps its catchment
and the sea comes up short by exactly that basin.

Two things the report says out loud rather than smoothing over. The grid's land
is **28.0%** where the Fibonacci sampler measured 29.1%: the hydrology sees the
relief capped at the octaves a texel can carry, so its coastline is smoother
than the one the mesher cuts. And **standing water covers 7.39%** of the land
-- filled depressions, which is where lakes will go when there is water to put
in them.

Moisture is three parts: the flow through a cell as **decades above its own
catchment** (a log, because drainage areas span the planet and a linear scale
gives one bright river and a dry world), distance to the sea, and a latitude
band from one cosine with a sixty-degree period -- rising air at the equator
and at sixty, sinking at thirty and at the poles, which is a great deal less
arbitrary than a noise field called humidity. Warmth is the cosine of latitude
with a lapse rate quoted against the relief, so a planet with bigger mountains
has its snow line in the same place relative to them.

Over land: **1.6% steppe, 17.3% temperate, 33.6% desert, 47.5% tropical**, mean
moisture 0.57. (The quadrant split is as much about where the 0.5 thresholds
sit as about the climate -- what it is really checking is that the field is a
field and not a constant, which a map that still conserved and still drained
could easily have been.)

The colour rule takes moisture and warmth where it took latitude, and the two
new corners of the square are named: desert and steppe. Warmth drives the
tundra and ice lines instead of latitude, so a treeline **bends up a valley and
down over a plateau** rather than running dead straight round the planet. The
map's green and blue channels carry the two -- they used to be the same height
byte written three times, which was a greyscale image stored in colour.

Trees read the same two numbers: none below 0.22 warmth or 0.42 moisture, and
the canopy thins across the wet edge instead of stopping at a map texel. The
old rule was a latitude cut at `|y| > 0.58`, which had exactly the stripe
problem the colouring did.

**And a beach turned out to be 14 m tall.** The sand ramp was a *percentage of
the relief*, so on a planet with 625 m of it the shore ran fourteen metres up
the hill -- which is why a landing site reading moisture 0.83, warmth 0.99 and
"tropical" on the panel had pale coastal-plain ground under a dense rainforest.
At 1:1 it would have run 400 m up. It is a tide line now, three voxels of it,
quoted in metres.

The panel says what climate you are standing in, because otherwise it is only
inferable from the colour of the ground.

Captures byte-identical between runs and across all three configs.


### 2026-08-26 (the hover was losing its grip 70 m up)

**Reported as "the ground is moving below us at over 30 m/s", and the model
said 30.6.** Co-rotation had been added a few days ago and was working; what
was wrong was how much of it a craft kept. The share was the *same exponential
the scattering uses* -- full at the surface, a fiftieth at the top of the shell
-- on the theory that thin air grips less.

That is not what an atmosphere does. The whole of it turns with the planet,
near enough rigidly: a balloon at 10 km is over the same city as one at 100 m.
Weighting by density meant a hover at 70 m kept only 93% of the turn, and 7% of
436.3 m/s is exactly the number that got reported.

Carried completely through the bulk of the shell now, and released over the top
quarter of it with a smoothstep, which is the only part a taper was ever needed
for -- so that leaving the air is not a step.

| hover | ground slides | was |
| --- | --- | --- |
| 10 m | 0.0 m/s | 4.5 |
| 70 m | 0.0 m/s | 30.6 |
| 200 m | 0.0 m/s | 82.0 |
| 1,000 m | 0.0 m/s | 284.1 |
| 3,000 m | 4.1 m/s | 423.6 |
| 3,925 m (top) | 436.3 m/s | 436.3 |

The equatorial figure the percentages are of is `2*pi*R/T` = 436.3 m/s, which
the demo computes nowhere -- the test brought its own. The coupling is a named
function now (`AirCoupling`) rather than eight lines inside the thing that
applies it, because "how fast does the ground slide" is a question worth being
able to ask.

The landed capture is byte-identical: none of this runs while walking.


### 2026-08-25 (a local origin, and a planet that is actually planet-sized)

**A float carries 24 bits, so at Earth's own radius it carries half a metre.**
Everything at the surface -- chunk vertices, plant positions, physics bodies --
was in the planet's frame, where the magnitude is the radius. At the 250 km the
demo runs at that costs 6.7 mm and does not matter. At 6,371 km it costs
**0.571 m placing a mesh vertex, 36% of a voxel**, which is the "turns the
meshes to rubble" the roadmap had been promising for two days.

The fix is one subtraction moved from after a cast to before it, in about six
places. Measured by asking the same field for the same points both ways:

| | one voxel step | placing a vertex between two lattice points |
| --- | --- | --- |
| 250 km, `PositionOf` | 0.0000 m | 0.0067 m (0.4% of a voxel) |
| 250 km, `PositionFrom` | 0.000000 m | 0.000001 m |
| 1:1, `PositionOf` | 0.5927 m (37.2%) | 0.5710 m (35.9%) |
| 1:1, `PositionFrom` | 0.000001 m | 0.000001 m |

571,000 times better, and `PositionOf`'s error is exactly the float spacing at
that magnitude (0.5 m at 6.37e6), which is the check that it is the arithmetic
and not something else. **The first version of that test measured a clean zero
at 250 km** and had proved nothing: it sampled crossings at `step / 8`, and
eighths of a voxel are exactly representable. Non-dyadic fractions gave the
6.7 mm above.

**Nothing large is ever cast; the small half is.** `PositionFrom(x, y, z,
about)` subtracts on the *integers*, where it is exact, so a chunk's vertices
come out measured from its own lattice origin and never exceed a chunk
diagonal. `SampleDistanceFrom` does the same in reverse -- the caller hands in
a small offset and a lattice point, and the integer part is put back as an
integer rather than added as a float. Chunk origins, plant positions and body
centres are composed in double and cast only once the answer is small.

Three frames now exist where there was one: chunk meshes carry their own
origin (so a level-of-detail change never has to rebase them), the forest
shares one origin that follows the player (they are one instance buffer, so
they have to), and the surface physics world is centred on the lattice point
nearest wherever the ship came down.

**A 1:1 Earth stands up.** `--earth-radius 6371000` streams, meshes, renders a
forest with no cracks, and lets you walk on it -- and the panel reports surface
gravity 9.82 m/s² and **escape velocity 11,184.4 m/s against the real 11,186**,
0.014% out, from a number the code does not contain. The default stays at
250 km: a planet is only worth the radius you can stream across, and at 1:1 the
streamed 400 m is a flat disc under a horizon 71 km away. What changed is that
the *representation* now goes all the way.

**Two false starts, both caught by demos that were not being worked on.**

The first: a body's field coordinates needed a lattice point to be measured
from, and I derived it by rounding the field's origin to the nearest one. That
is right for every field whose origin is a whole number of voxels and wrong for
OpenWorld, whose lattice is deliberately centred *between* points -- it moved
that demo's ground by a quarter of a metre and **13.5% of its pixels** with it.
The remainder is carried explicitly now, and the default body reduces to the
same expression the old code evaluated rather than an equivalent one.

The second was subtler and is worth stating on its own: **float addition does
not associate**. Writing the contact gradient as `SampleNormalFrom(centre -
bias)` groups it `(centre - bias) + h`, where the original has `(centre + h) -
bias`. Same value, different last bit, **34 of OpenWorld's pixels** -- and a
simulation that no longer reproduces itself. `SampleNormal` keeps its own
arithmetic and the narrowphase groups the gradient the original way.

With both fixed, OpenWorld's landed capture is byte-identical to its hash from
before any of this. All six demos reproduce themselves, and the Solar capture
is byte-identical across all three configs.

**What is still planet-space, and stated so it is not mistaken for done.** The
density is *sampled* at `PositionOf` inside `FillChunk`, so at 1:1 the sample
lattice jitters by up to half a voxel -- the terrain is displaced slightly, not
broken, because neighbouring chunks read the same cell values and still agree.
The terrain shader computes height as `world - u_Origin` in float on the GPU,
which at 1:1 quantises the colour ramp to about half a metre. And
`SurfaceRadius` returns a float, so the altitude readout and the grounded test
are half-metre answers at 1:1. All three are shading or reporting, not
geometry.


### 2026-08-25 (terrain LOD, and the 422 meshes that were quietly wrong)

**Stride-based level of detail for a planet's terrain, which needed almost no
new code.** A planet is a sphere in the *density*, not in the grid — the
lattice is as Cartesian as OpenWorld's flat field — so `VoxelTransition`,
`DesiredStride` and the hysteresis band all transferred unchanged. Three bands,
quoted at Earth's 1.5 m voxel and scaled by voxel size so one pair of sliders
serves sixteen bodies: stride 2 beyond 100 m, stride 4 beyond 200 m.

Landed on Earth, terrain went from 2,774,250 triangles to **500,825** — 55
chunks at stride 1, 179 at stride 2, 636 at stride 4. The frame went from about
16.2 ms of GPU to about 12.2 (both ±0.5 between runs; the triangle counts are
exact and deterministic). Mars, which has no forest to share the cost, runs at
8.03 ms.

**Then the seams had to be counted, because a picture cannot prove a hole is
absent.** Every resident chunk welded into one edge table, in the planet's own
frame, on exact float bits: a closed surface uses every edge twice. The
streamed region has a genuinely open outer boundary so the count is never zero,
which makes the raw number useless — what matters is *where*. An open edge on a
chunk face with resident chunks on both sides is a hole in the middle of the
terrain, and that is the only kind LOD can make.

| | interior open edges |
| --- | --- |
| LOD off | 125 |
| LOD on | 2,943 |

**90% of the difference is one documented limitation.** 2,642 of those 2,943
touch a chunk that needs the transition on two or more faces at once, which
`VoxelTransition` says up front it does not handle and meshes plainly. A planet
hits it far harder than a flat field does: the bands are curved shells cutting
diagonally through a cubic lattice, so 25 of the 82 chunks needing a transition
need it twice. Accepted rather than fixed, and invisible in practice because
the horizon sphere sits *below* the terrain floor — a hole shows ground, not
sky, which is why the whole change moves 0.23% of pixels and all of them within
64 rows of the horizon.

**And the seam test found something much worse that had nothing to do with LOD.**
422 of 963 stored meshes did not match what the mesher would produce from the
field as it finally stood — **and the number was identical with LOD off**.

`VoxelField3D::ChunkRange` adds a plane in *every axis at once*, so a chunk is
marched on a lattice that includes the point at (+16, +16, +16) — which belongs
to the **diagonal** neighbour. `HighNeighboursFilled` waited for three
neighbours and filling staled three, which is right along the faces and wrong
along the edges and at the corner, and the corner is shared by four chunks
nobody was telling. The engine's own comment on `FillChunk` said "those three
neighbours", so the mistake was inherited rather than invented; it now says
seven and says why.

With all seven, **422 stale meshes becomes 0**. The visible payoff was on Mars:
a row of black dashes along the horizon, present in *both* the LOD and the
no-LOD captures and therefore easy to blame on nothing, is simply gone. A
chunk's mesh now also waits one neighbour longer before it appears, so the
streamed region at a given step is a chunk thinner — 870 rather than 963. That
is the correct region; the old one was drawing geometry meshed against data
that had since changed.

**A note on the measurement, which was wrong first.** The seam test rebuilds
every chunk to count its edges, so it measures the *mesher* and cannot see a
stale *mesh*. That is why adding neighbour restitching changed the seam counts
by exactly nothing — identical to the byte, which is the tell that a test is
answering a different question than the one being asked. Comparing each
rebuild's triangle count against the one actually on the GPU is what caught it.
The restitching is still there and still right, for the case where a coarse
chunk is meshed before the finer chunk beside it exists.

`--no-terrain-lod` turns it off from a shell, which is how the A/B above was
run. The panel shows chunks and triangles per band.

Captures are byte-identical between runs and across all three configs.


### 2026-08-25 (the forest was two thirds of the frame)

**The landed frame was GPU-bound at 26.30 ms, and 9.59 of them were trees.**
Instancing had already taken the submission cost out — 963 chunks and six tree
draws, 5.26 ms of CPU — so what was left had to be the GPU, and it was. A
`GL_TIME_ELAPSED` query around the frame said 26.30 ms against a 16.67 ms
budget: 37 fps, which is what "we need to maintain 60" was measuring against.

**The split came from ablation, one category at a time.** Skipping the trees
took the frame to 16.71 ms; skipping the terrain chunks, 19.95; the atmosphere,
25.77; the starfield, 26.18. Skipping everything left 1.30 ms, so the deltas do
not add up to the total and are not meant to — removing an occluder makes what
was behind it shade, which is also why *dropping* the sea made the frame 1 ms
slower. What the numbers do establish is the ranking, and trees won it twice
over: 5,077 of them at 1,210 triangles each is 6.14 M of the frame's 9.07 M,
against 2.86 M for all 963 chunks of terrain.

**1,210 triangles is the right number for a tree you are standing under and an
absurd one for a tree thirteen pixels tall.** Trees stream to 400 m and a disc
is mostly its rim, so 86% of them are past 150 m, where a 5.6 m tree subtends
about forty pixels. Three levels now, chosen per tree from distance over the
tree's own scale — the trees vary 0.6 to 1.15 and it is *apparent* size that
decides how much geometry is worth spending, not range:

| Level | Beyond | Bark | Leaves | Total | Drawn |
| --- | --- | --- | --- | --- | --- |
| 0 | — | 400 | 810 | 1,210 | 118 |
| 1 | 45 m | 240 | 432 | 672 | 686 |
| 2 | 150 m | 78 | 144 | 222 | 4,273 |

Every one of those counts is a closed form the generator does not contain,
which is how they were checked. Bark is `segments x sides x 2` and the segment
count is the geometric series `(c^(d+1) - 1)/(c - 1)`: 40 at depth 3, 13 at
depth 2. Leaves are `tips x rings x segments x 2` with `c^d` tips: 27 and 9.
All six numbers matched.

**Level 1 changes the tessellation and not the structure**, which is the whole
reason the 45 m switch is invisible: same seed, same depth, same branching, five
sides down to three and the leaf blobs coarsened. Level 2 does change it — a
shallower tree has 9 tips where there were 27 — so its leaf clusters are grown
by `(27/9)^(1/3)` to cover the crown the missing generation used to fill.

**26.30 ms to 15.74.** Tree triangles fell 6.14 M to 1.55 M, a factor of 3.96,
and the frame came in under the 16.67 ms budget for the first time. The
captured frame differs from the old one in 1.52% of its pixels, all of them in
the far treeline.

**Tightening the bands further buys nothing.** At 35 m and 110 m — 69 trees at
full detail instead of 118 — the frame is 15.45 ms. 0.29 ms for a visibly
earlier switch says the remaining cost is no longer tree vertices, so the bands
stayed where they look right. What is left is the terrain, and terrain LOD is
already on the roadmap.

The panel now shows the frame's draw calls and triangles, and the tree count
split by level. `Renderer::ResetStats` had never been called here, so anything
that had read those numbers would have been reading every frame since the demo
started.

Captures are byte-identical between runs and across all three configs.


### 2026-08-25 (two small ones)

**`--goto Sun` reported that the Sun matches no body.** One loop serves both
`--goto` and `--land`, and it started at index one because the star is not
somewhere you land — so the flag that only wants to *look* at it was refused
along with the one that does not make sense. It starts at zero now; `Land`
already declines the star on its own and says "nothing to land on out here",
which is the better place for that to be decided.

**And `DrawAtmosphere` took a `scale` it never used.** Its caller pre-multiplies
the radius, so the parameter existed only to be silenced with a `(void)` cast —
which is a comment that the signature is wrong, written in the wrong language.

The landed capture is byte-identical across the change.


### 2026-08-25 (a forest in six draw calls)

**A landed frame was submitting about 23,000 draw calls.** 11,010 trees at two
each -- bark and leaves are separate geometry with separate materials -- plus 963
chunk meshes. That is the CPU cost measured in the previous entry, and it is why
an integrated GPU sat at 60% without being the thing that was slow: the expense
was submission, not shading.

It is **1,001** now. Three tree shapes times two materials is six calls whatever
the forest does, and the rest is the terrain.

**The engine had no instancing, and one piece of it was broken in a way nothing
had noticed.** `ShaderDataType::Mat4` was in the enum and could not be used as a
vertex attribute: GL caps an attribute at four components, so declaring one with
sixteen is an error rather than a matrix. A `mat4` is four consecutive locations
sixteen bytes apart, which `AddVertexBuffer` now emits -- along with
`glVertexAttribDivisor` where the layout asks for it.

The rest is small: a `Divisor` on `BufferLayout` (a property of the buffer, since
mixing per-vertex and per-instance attributes in one is legal in GL and has never
been what anybody meant), `RendererAPI::DrawIndexedInstanced`,
`Mesh::SetInstanceBuffer` and `Renderer::SubmitInstanced`.

**What differs goes in the buffer; what is shared stays a uniform.** The planet's
placement and spin is common to every tree on it, so it stays in `u_Transform`
and only the per-tree translate-orient-scale goes down the instance buffer. That
saves a matrix multiply per tree on the CPU and keeps the large translation out
of every instance's matrix.

Getting that wrong is how the first version drew **no trees at all**:
`SubmitInstanced` forced `u_Transform` to the identity, on the reasoning that the
transform is the thing that differs and therefore cannot be a uniform. It is
half right -- what differs cannot be, but what is *shared* must be, and wiping it
put the whole forest at the camera's own position. It takes a transform now.

**One buffer, two vertex arrays.** A trunk and its leaves stand in the same place,
so the shape's instance buffer belongs to both meshes -- a `VertexBuffer` can join
any number of vertex arrays. It is allocated once at full size rather than grown,
because attribute locations are handed out in the order buffers are added: a
replacement would land at location 7 while the shader went on reading 3.

**And then the trees that were never visible stopped being drawn.** There had
been no culling of any kind -- every tree in every resident chunk went down the
pipe, which while it was two draw calls each was not the thing worth fixing.
Tested a chunk at a time, which is 963 tests instead of 11,010 for the same
answer, because trees in a chunk are within 24 m of each other. A cone at 75
degrees against a half-angle of about 50, widened by the chunk's reach and a
canopy so nothing whose leaves are in view is dropped for having its trunk
outside. **The capture is byte-identical with it on**, which is the whole
argument: what it removes was not being seen.

| | before | after |
|---|---|---|
| draw calls | ~23,000 | 1,001 |
| triangles | 16.66 M | 9.49 M |
| CPU, 1,800 frames | 33.81 s | 18.15 s |

The wall clock did not move -- this machine was already vsync-bound at about 58
FPS and still is. What changed is that it now spends half the CPU getting there,
which is headroom on a machine that had none and the difference between 60 and
40 on one that is slower.

Against the pre-instancing capture, 265 pixels of 921,600 differ, all on tree
silhouettes: `frame * local` used to be multiplied on the CPU and is now
associated the other way on the GPU, which is the same arithmetic and not the
same rounding. Byte-identical across all three configs and across repeated runs,
and the other demos are untouched.

**What is left is the 963 chunk meshes**, which are distinct geometry and so
cannot be instanced -- they want merging into larger buffers, or a coarser mesh
past a distance. That is the next lever and it is a smaller one: the trees were
95% of the calls.


### 2026-08-25 (the landing stopped freezing, and why it was never the GPU)

**The tell was that the GPU went idle.** Landing froze the demo; the report was
that the integrated GPU climbed to about 60% and then fell to ~1% as it locked
up. A GPU at 1% is not a GPU that is struggling, it is a GPU with nothing to do
-- the CPU had stopped submitting frames. Everything here is GLSL and has been:
terrain, atmosphere, rings, ocean, the starfield. What stalls is chunk
generation, on the main thread, inside the fixed step.

**Measured before touching anything**: 150 ms a step in Debug and 21 ms in
Release, against the 16.7 ms a 60 Hz frame has in total. And 7,200 chunks filled
to produce 808 that had any surface in them.

Four things were wrong, and only the last is interesting as a policy.

**The shell test asked the wrong question.** `TouchesSurface` compared a chunk
against the *mean* radius with the whole relief range as its band -- on a planet
whose hills are 380 m, a shell 800 m thick, every chunk in it generated in full.
The band is a property of the planet; the surface is a property of the direction.
Sampling `Relief` over the chunk's own extent costs 27 evaluations against the
53,000 a fill costs, so rejecting a chunk that way is hundreds of times cheaper
than filling it and finding out. Verified one-sidedly, which is the only way that
matters: over 2,197 chunks around a landing site, **169 held a surface and none
of them were rejected**; the waste that remained went from 72.8% to 39.9% when
the sampling went from 9 points to 27 and the margin came off.

**A chunk with nothing in it was still written 4,096 times.** The rejects went
through `FillChunk` with a constant generator, which allocates a 16 KB scratch
buffer, writes 4,096 identical floats and then discovers they are all the same.
`VoxelField3D::SetUniform` says it in one assignment.

**The scan re-walked the whole neighbourhood every step.** 50,653 sorted offsets,
hashed against the filled set, to rediscover that the first several thousand were
already done. That was a **2.9 ms floor no budget could get under** -- the demo
converged to a stable three milliseconds a step of finding nothing. The offsets
are sorted by distance and filling goes outward, so the scan resumes where it
left off while the focus chunk has not moved.

**And the budget counted the wrong thing.** It counted chunks *filled*, so a step
that rejected four hundred and meshed five called that one chunk of work. Now
fills, rejects and meshes all spend one budget, weighted by what they cost --
and meshing goes first, on half of it, because otherwise the two starve each
other: fill-first means a budget of one is always spent on a fill, the dirty
queue never drains, and terrain is generated and never drawn.

**The budget is milliseconds now, and it is banked.** One chunk is the atom of
this work and it costs about four milliseconds here, so an *integer* budget has a
four-millisecond floor -- and seven times that in Debug, where no integer can
hold the frame. Carrying the unspent fraction from step to step lets the answer
be "a chunk every third step", which is what a machine that cannot afford one a
step should do: fill in more slowly, at frame rate, rather than stop.

**Except under lockstep, and that is not a detail.** The ground collider is
`MakeSdf` over the *field*, so how much has streamed is part of the simulation
and not only of the picture -- a chunk that has not arrived reads as air and the
player falls through it. A budget that depends on the wall clock would therefore
make a replay depend on the machine. `Application::IsLockstep()` exists now for
exactly this, and every capture and recording runs under it.

**Where it landed.** Streaming holds **5.5 ms against a 5 ms target** while the
ground fills in, and the steady state on the surface is **16.65 ms a frame --
60.1 FPS** in Release, measured over frames 1200 to 1800 with streaming
converged. Lockstep captures are byte-identical across all three configs and
across repeated runs, and the Voxel, Map Building and Open World demos are
unchanged.

**What is left is draw calls, and the number is 8,300.** A landed frame submits
about 1,174 chunk meshes and 3,556 trees at *two calls each* -- bark and leaves
-- which is the 16.65 ms and is why an integrated GPU sits at 60% without being
the thing that is slow. Instancing the trees takes 7,112 of those to six. That is
the next lever and it is on the roadmap.

Debug is about seven times slower than Release on this path, and `./egss.py run`
builds Debug: 40 FPS against 60 for the same scene. Worth knowing before
concluding anything about a frame rate from it.


### 2026-08-24 (a planet the size of a planet)

**Earth was 360 m across and everything wrong with it was one thing.** The
atmosphere was 16 m deep, twice the height of a tree. Venus hung in the sky as a
disc the size of a moon. Escape velocity was 84 m/s, which is not a rocket, it is
a jump. Those are not three complaints; they are a body too small for anything
about it to feel like a body.

Both exponents are **one** now, so the scale map is the identity and every ratio
in the system is the true one: the Sun is its real half a degree across, Venus is
a point of light, the Moon's orbit is 60 Earth radii, and Phobos clears Mars by
the 2.76 it actually clears it by. Earth is **250 km**, which is 1/25.5 of true
size -- the metre is the only thing left that is not real, and the reason is
exact, below.

What that buys, in the numbers that matter to standing on it:

| | before | now |
|---|---|---|
| Earth's radius | 360 m | 250,000 m |
| atmosphere | 16 m | 3.92 km |
| escape velocity | 84 m/s | 2,216 m/s |
| horizon, eye height 1.8 m | 36 m | 949 m |
| Sun's apparent diameter | 24.9° | 0.53° |

**The chunk store had to become sparse first.** `VoxelField3D` kept one `Chunk`
per chunk of the lattice in a `std::vector`, written to or not, at 56 bytes
each -- nothing for a field a few hundred voxels across, and the entire reason a
planet could not grow. Earth at its own radius and 1.5 m voxels is 578,699 chunks
a side, which is 1.9e17 of them and **1.1e10 GB of empty structs**; even a 31 km
planet needs 1,250 GB. An `unordered_map` keyed by the packed chunk index holds
only what has been filled, which is what the streamer keeps near the player.
Verified by the three voxel demos coming back byte-identical.

**And the chunk key had to get wider.** `VoxelPlanet` packed each axis into 16
bits, which tops out at 65,535 -- a body 25 km across. It does not overflow, it
*wraps*, so a streamer on a real planet would find the far side of the world
already filled. Twenty-one bits reaches 2,097,152, which is 3,145 km of chunks.

**Terrain needed a different spectrum, and a second one.** Relief was 8.5% of the
radius: 31 m of hills on the old planet and 541 km -- sixty Everests -- on this
one. A quarter of a percent gives 625 m, which is Earth's real range with the
trench filled in. But a 1/f fractal anchored at a continent leaves nothing
underfoot, so local roughness is its own layer with its own amplitude, and it
stops where the arithmetic does: the noise is sampled at `direction * Radius /
wavelength`, so a metre-scale wavelength on a planet asks for coordinates near
10^6, where a float's spacing is a sixteenth of a noise cell. The colour map
also stops early now, at the octave whose wavelength is two texels -- below that
it is not detail, it is aliasing, and it is most of the cost.

**Distant bodies are drawn nearer and smaller.** No depth buffer spans 4.5e12 m
and 0.15 m, so everything except the body you are at is moved in along its own
direction and scaled by the same factor: same direction, same ratio of radius to
distance, therefore the same picture, in a depth bucket that exists. The map is
logarithmic past 200 km and the identity below, so it is continuous and monotone
-- and monotone is the property that matters, because it means ordinary depth
testing still sorts one body against another.

**Three bugs, and each was invisible at 360 m for the same reason.**

- **A landing arrived at a fraction of the radius rather than at a height.**
  `max(1.06 radii, ground + 20 m)` is a sensible-looking expression while those
  are the same order of thing -- 1.06 radii of a 360 m planet is 21 m up. At
  250 km it is **15 km**, so `--land Earth` returned a photograph of the horizon
  from orbit.
- **The shell test bounded the relief at half the amplitude, and the relief is
  skewed.** `shape * Amplitude` is bounded to ±A/2 and then the measured bias is
  *subtracted*, which slides the range down without narrowing it -- so ground
  reaches `A/2 + bias` below the mean radius and only `A/2 - bias` above. Chunks
  in the difference were classified "nowhere near the surface" and filled with
  solid rock, which draws as a wall. At a 31 m amplitude the gap was ten metres,
  less than half a chunk. The bound is analytic now and printed at generation
  beside a sample of what it is bounding, because a sampled min and max would
  miss exactly the rare deep chunks nobody would think to look at.
- **The near plane was 0.4 of the height above the *mean radius*.** On a 360 m
  planet with 15 m hills that is 3.5 m while standing: a sliver of ground missing
  at the very bottom of the frame, which nobody ever noticed. At 250 km the hills
  are 380 m and the same expression put the near plane **76 metres** out. It
  clipped away the grass underfoot and the trees you were standing among, leaving
  a hard horizontal line across the frame with the stand-in sphere and the
  insides of the chunk meshes showing through beneath it -- which reads as a
  mesher bug, and was a camera one. On foot the answer is the eye height.

**The ceiling is a float, and it is exact.** The chunk meshes, the plant
positions and the physics bodies of a surface all live in planet-fixed
coordinates whose magnitude is the radius, and a float carries 24 bits -- so the
spacing of representable positions at the surface is `R / 2^23`. At Earth's own
radius that is **0.76 m**, half a voxel: the meshes come out as rubble and the
physics with them, and a capture from the ground shows the smooth stand-in sphere
and nothing else, because nothing else survived. This was measured rather than
predicted -- 6,371 km, 1,000 km and 60 km were built and photographed, and the
artefacts scale with the radius exactly as the spacing does. 250 km puts it at
**30 mm**, under the collider's own slop and a fiftieth of a voxel.

Lifting it means carrying the surface about a local origin that follows the
player, through `VoxelField3D`, the mesher, the SDF collider and the plant
placement. That is what stands between this and 1:1, and it is on the roadmap
with these numbers. So is terrain LOD: streamed ground reaches 400 m and the
horizon is 949, so the last half of the view is the stand-in sphere.

All three configs build clean and byte-identical, repeated runs are identical,
and the three other voxel demos are unchanged by the sparse store.


### 2026-08-24 (a sky with real stars in it)

**Forty-four of them are real, at J2000 coordinates.** Right ascension and
declination through Earth's 23.44-degree obliquity into the ecliptic, then into
this demo's axes, so Orion is Orion, the Plough points at Polaris, and Dubhe is
orange because its B-V is 1.07. A made-up star is a star nobody can check, and
the entire reason to type coordinates in is that the angle between two of them is
a number the code does not contain.

**What is not real is where the planets sit against it.** The bodies start at
longitudes this demo chose rather than at an epoch, so the constellation behind
Jupiter means nothing. The sky is right relative to itself.

Behind the catalogue is a procedural field of about **5,000 anonymous stars** --
what the naked eye gets on a good night -- on a cube-face grid so they do not
bunch at the poles, sized by the screen-space derivative so a star is a couple of
pixels at any field of view, and scaled to sit under Megrez at magnitude 3.31,
which is where the catalogue stops. The Milky Way is a band about the real
galactic pole, which is why it crosses Cassiopeia and Crux the way it does.

**And the roadmap's reason for wanting it was wrong.** It said 40 km/s looked
like standing still and a starfield would fix it. It does not: stars are at
infinity, which is exactly why they give no parallax, and you cannot tell you are
moving by looking at them in life either. What they give is an *orientation*
reference -- turning is now visible where it was not -- and a background for the
planets to be objects against. The speed cue was always the planets themselves.

**Verification.** A temporary self-test, since deleted, ended at 15 of 15. All
**946 pairwise separations** survive the conversion to within 2.7e-13 degrees,
which says the tilt was applied as a rotation; a rotation that preserves angles
can still be the wrong rotation, so the rest are facts about the ecliptic that
nothing in the table states. Regulus comes out **+0.465** degrees off it against
+0.465 published, Spica -2.054 against -2.055, Antares -4.570 against -4.567,
Aldebaran -5.467 against -5.467. Polaris lands **23.899** degrees from the
ecliptic pole, which is the obliquity plus its own 0.74 from the celestial one.
Orion's belt is spaced 1.39 and 1.36 degrees with a 172.4-degree bend -- it is
bent, and by about that. The pointers are 5.37 degrees apart with Polaris 5.3
times that beyond Dubhe, which is the rule every child is taught.

Three mutations, each caught by the number naming it: skipping the obliquity put
Regulus at **+11.967** degrees off the ecliptic, which is exactly its
declination and is the tell; applying it backwards doubled the error to +21.862;
and confusing the demo's north with the ecliptic's gave +30.170. The separation
check passed under all three, which is the point of having both kinds.

**Five things looked wrong before they were right**, and each was a different
kind of wrong:

- **49,000 stars is a wall of noise.** Two layers at 90 and 200 cells a face
  left no black in the sky at all. The count is `cells^2 * share * 6`, and the
  number to aim at is what the eye actually sees.
- **A sub-pixel Gaussian is not a small star, it is an aliased one.** At 0.8
  pixels of sigma the sky came out as **dashes**, each a point source caught by
  a scanline. The floor is 1.5.
- **`fract(sin(dot(...)) * 43758.5)` fails at the magnitudes a starfield uses.**
  Cell index times a few hundred plus a layer salt reaches eighteen thousand,
  where a float carries a thousandth of absolute precision and `sin` needs a
  millionth of phase to decorrelate neighbours. The sky came out as **grey
  rectangles** -- whole cells sharing a hash. Replaced with an integer hash;
  same family as the `Hash2D` overflow of 2026-08-17, reached from the other
  direction.
- **A cube root squashed 86:1 of flux to 4.4:1**, at which the faintest
  catalogue star and the brightest anonymous one behind it are the same dot.
  **Orion was not findable in a frame pointed straight at it.** Two fifths gives
  5.9:1. Even then the Plough was invisible until the quads went from half a
  degree to 0.85 -- the pattern had been exactly right and two pixels wide, which
  a debug capture at four times the size settled in one look.
- **Daylight has to be saturating, not proportional.** The fade started as
  `1 - air * day`, and an exponential with a scale height a quarter of the shell
  is down to 0.21 six metres up -- so a player standing on the ground in the
  afternoon kept a fifth of the starfield. **Stars, in a blue daytime sky.** The
  error is in what `air` measures: the density at the eye is not what outshines
  a star, the lit column above it is, and a fiftieth of that column still wins.

**And one that cost bit-reproducibility, which is the one worth reading.** Three
runs of the same binary on the same scene produced three different PNGs --
twelve faint pixels, always the same twelve, flickering between two values. With
the sky switched off the same scene was byte-identical across three runs and all
three configs, so it was the starfield and not the demo.

The cause is that `fwidth` was called **after** the early return for a cell with
no star in it. GLSL leaves derivatives undefined in non-uniform control flow, and
that return is exactly that: a 2x2 quad straddling a cell boundary has helper
invocations that took the other path, and what they contribute to the derivative
is whatever the driver had lying around. Hoisting the one line above the branch
is the whole fix, and all three configs are byte-identical again over repeated
runs.


### 2026-08-24 (a second exponent, rings, and air that carries you)

**Jupiter was 3.3 Earths across, where it is really 11.** One exponent applied
to every length is what kept the system 302 km wide instead of 8,453, and it was
also what made the gas giants read as another Earth in a different colour. There
are two now: heliocentric distance keeps `p = 1/2`, and everything *inside* a
body's own system -- its radius, its moons' radii, its moons' orbits -- goes
through `q = 3/4`. Jupiter is **6.03 Earths**, the Sun 33.8, and within any one
system the map is still a single uniform power, which is the property the whole
design rests on: nothing overtakes anything and no moon needs special-casing to
stay outside its planet.

`q = 3/4` is where two constraints meet. Below it the giants shrink back toward
Earth. Above it the Sun grows as `109.24^q` while Mercury's orbit is pinned at
`9088^p`, and it **swallows Mercury outright at `q = 0.97`**. Three quarters
leaves Mercury 2.8 solar radii clear, and every clearance in the table improved:
the tightest is now Phobos at **2.04**, where it used to be 1.57.

What it costs, stated rather than hidden. A moon's orbit grows with its planet's
system rather than with the Sun's, so the Jovian system is 26 km across where
Jupiter's own orbit is 126 km -- really that ratio is 0.24% and here it is 20%.
And the Sun is **24.9 degrees across seen from Earth**, against 0.53 in life,
because its radius went up by `q` while the orbit it is seen from did not. Both
exponents are sliders.

**An hour to the day and 365 hours to the year.** The clock near a body is set so
that body's day takes `SecondsPerDay`, and far from one it runs at the orbital
rate; asking for a 3,600 s day on Earth implies 7.584e-7 yr/s, and 365 hours to
the year is 7.610e-7. They agree to **0.34%**, so the handover at six radii is no
longer a change of pace -- at the 60 s day this started with the two differed by
four orders of magnitude and leaving a planet made the sky lurch. The sliders are
logarithmic now, because a linear one cannot represent 7.6e-7 at all. The cost is
that nothing completes an orbit while you watch and the Kepler column stays
empty until you drag the rate up.

**A craft in an atmosphere turns with the atmosphere.** The ship's position is
held in the frame body's inertial frame, which is right for an orbit and wrong
for a hover: the coastline slid out from under a stationary ship at a full turn a
day. It is carried round now, weighted by how much air there is to be carried by
-- full at the surface, nothing at the top of the shell, on the same exponential
the scattering uses. **This was not worth having until the day got longer**: at
60 s to the day the frame turned six degrees a step, which is a fairground ride,
and the note that used to sit here rejecting co-rotation was right at the time.

**Gas giants get air deep enough to have no ground under it.** Jupiter's visible
air is really about 1.4% of its radius, no deeper in proportion than Earth's --
the thing that is actually true about a gas giant is that there is no altitude at
which you land. This demo builds every body out of voxels, so there is a surface
down there whether it belongs or not, and the honest way to draw a planet with no
ground is air you never get through. Straight down through Jupiter's shell is now
5.9 optical depths.

That took three fixes, each of which was a different way of being wrong:

- **An additive atmosphere cannot hide anything.** Air both adds light and stops
  it, and `BlendMode::Additive` can only do the first, so Jupiter came out as a
  hard disc of *ground* with a bright halo round it. The engine has a
  `Premultiplied` mode now -- `src + dst * (1 - src.a)` -- and the shader fills
  alpha with the extinction along the same path the scattering integral walks.
- **Single scattering makes thick air dark.** Every photon that bounces twice is
  dropped by the integral, and at Jupiter's optical depth almost all of them do,
  so the planet went from a hard disc to a **grey ball dimmer than Earth beside
  it**. One extra term stands in for the bounces: it appears only where the air
  is thick and is lit by where the deepest visible air sits relative to the star,
  so there is still a terminator. It is zero for Earth and Mars, so nothing about
  a thin atmosphere changed.
- **A fixed step count over a path length that jumps is two different
  quadratures.** Stopping the ray at the voxel surface drew a **hard circle
  across Saturn**; moving the stop below the surface only moved the circle. What
  steps is the path, not the ground. A body with no surface does not stop at all
  now -- density saturates below the drawn radius, so the far half buries itself
  -- and the step count comes from the scale height rather than from the
  constant 8 that suited a shell 4.5% of Earth's radius.

**Rings, on Saturn and Uranus.** One annulus mesh; the radii are uniforms, so the
fragment shader gets the radius in units of planet radii, which is the unit every
published number about a ring is quoted in. Banding is 1D value noise of that
radius alone, which is what makes it read as orbits rather than as a texture, and
the **Cassini division** is a smoothed notch at 1.95 to 2.02. Seen from the
shadowed side the ring is a negative of itself -- thin parts pass light, thick
parts stop it -- which is one line and the whole difference between a ring system
and a grey band. The planet's shadow falls across it, and the opposition surge
is there because ring particles really do throw light back the way it came, and
because without it a ring tilted 26.73 degrees sits at a third of the brightness
of the planet lighting it.

**The tilt is on the ring, not on the planet, and that is deliberate.** Every
body here spins about +Y, so an untilted ring lies in the ecliptic with the star
in its plane: drawn that way Saturn's rings came out **dark grey**, which is not
a shading bug but a flat annulus receiving nothing. Tilting the body is the
accurate fix and a much larger change -- the spin drives the terrain's frame, the
walking player and the co-rotating air. It is also, on these two bodies, a change
nobody could see: their atmospheres are opaque, so there is no observation in this
demo that distinguishes a tilted Saturn from a Saturn with a tilted ring. If a
body with a visible surface ever gets rings, that is the line that has to change.

**Verification.** A temporary self-test, since deleted, ended at 21 of 21. The
two exponents were checked against arithmetic written out longhand rather than
against `DrawnLength` agreeing with itself, and the sharpest of them is that **a
moon's orbit measured in its planet's radii is the true ratio raised to `q`** --
`(a/R_e)^q / (R_p/R_e)^q` collapses to `(a/R_p)^q` with nothing about Earth left
in it, and that holds only because the orbit and the radius go through the same
exponent. The clearance check is not a substitute for it: a moon compressed on
the wrong exponent still clears its planet, it is merely in the wrong place, and
the mutation proving that was the one mutation the first version of the test
missed. It puts Io at 1.35 Jupiter radii, inside the air.

The air that carries a ship is measured by **where the ground is, not by what the
code did**: a ship on the deck is over the same patch a moment later (0.0000 of a
turn), one above the atmosphere sweeps the planet's whole rotation (1.0000), and
half way up it is 0.8808 -- both ends compared against numbers this code does not
contain. That test also found a real fault: the weighting was a bare `exp(-h/H)`,
which is still 0.018 at the top of the shell where the early-out drops it to
nothing, so the rate of the ground's drift **stepped by 1.8%** at the one
altitude every ship crosses on the way in. Normalising it so it reaches exactly
zero removed the step.

Four mutations, each caught by the number naming it: the air turning the ship the
wrong way moved the ground **2.0000** of a turn instead of zero, which is exactly
double and is the signature of a reversed rotation; no weighting at all froze the
ground at every altitude; the wrong exponent on moon orbits put the Moon at 7.77
Earth radii against 21.65; and Io inside Jupiter's atmosphere as above.

All three configs build clean and their captures of Saturn are byte-identical.


### 2026-08-24 (a landing that is not in the sea)

**`--land Earth` put you underwater, and had done since the day Earth got an
ocean.** The approach aims at the sunward point so that the landing site is lit;
Earth is 29.2% land; the two facts multiply. The panel read `on Earth, 0.0 m up`
and `on the ground`, both true -- the seabed is ground -- while the sea, which is
a shell drawn *over* the terrain rather than a surface anything stands on, closed
1.5 m over the eye. The whole screen was the underside of the water with the
shore visible through it, which reads as a broken water shader and is not one.

**The landing approach now asks the planet where the land is.**
`VoxelPlanet::NearestLand` walks the same Fibonacci spiral the sea level was
bisected on and keeps the direction with the largest dot against where the
lander was pointed, so what comes back is the nearest dry ground rather than
merely some. The site moves **9.4 degrees off the approach on average and 27.5
at worst** -- still lit, still the hemisphere you aimed at. A direction that is
already good comes back untouched rather than being snapped to the nearest
sample, which is the difference between "nearest" and "nearest on a grid".

**One dry sample is not a landing site, and the probe set to prove it is set by
the terrain rather than by the disc.** Two versions were wrong first. Four
probes on two axes let diagonal inlets through, which an independent probe set
caught in 3% of accepted sites. Rings at fixed *fractions* of the arc then left
the middle unprobed -- at 10 m the innermost ring sat 4.5 m out, so a pond three
metres away was invisible, and 20 of 512 sites had water inside 3 m. What sets
the spacing is the relief: five octaves from a 41 m base wavelength makes the
finest feature 2.6 m across, and probes half of that apart cannot step over one.
That works out at eight rings and 225 points for a 10 m disc, evaluated only for
a candidate that has already beaten the best angle so far.

**The clearance is 10 m because that is where two independent probe sets stop
agreeing.** A sunflower of 397 points, sharing no radius and no angle with the
search's rings, agrees about **every one of 812 directions at 3 m, 98.8% at 10 m
and 92.7% at 28 m** -- the shore is fractal down to that finest octave and no
finite probe set closes it. From the other side, 8.4% of Earth is 10 m clear of
water against 1.3% at 28 m, and a rarer site is a site further from where you
pointed: 28 m moved the landing 43 degrees on average. Ten is the largest
clearance the search can actually see, and it costs nine degrees.

**The arrival height was wrong too, and nothing had noticed.** `1.06 radii` is a
multiple of the *mean* radius while relief is 8.5% of it, so the same number is
10 m over a valley on Earth and 21 m *inside* a ridge on Jupiter. The arrival
radius is now the ground at the chosen site plus a flat 20 m, since it is a
distance to fall and not a fraction of anything.

**Pressing `L` over water still lands you there.** Landing is where you already
are -- that is the whole reason there is no teleport left in it -- so the panel
says `over water -- 9 m of it, and nothing here swims` before you press the key,
and the log says it after. Refusing would be the wrong fix for a demo where a
seabed is a place somebody might want to look at.

**Verification, and the two measurements that were wrong before the code was.**
A temporary self-test, since deleted, ended at 15 of 15. Its first reading said
24.56% of the sphere was above sea level against the 29.2% the sea level had been
fitted to -- 4.6 sigma, which is not a sampling difference. The test was walking
indices 0 to 2047 of a 4,096-point spiral, and a Fibonacci spiral is ordered by
z, so it had measured one hemisphere. Over the whole sphere it reads 29.57%,
0.4 sigma from a number produced by unrelated code. The second wrong measurement
was the probe comparison itself: the counts jumped 0, 11, 1, 36, 16, 37 as the
disc grew, which is not how a fractal coastline behaves. A handful of sites serve
hundreds of approaches, so it was counting the same bad site over and over;
comparing the two predicates *per direction* gives a monotone 99.3%, 98.5%, 95.7%,
91.6%, 91.3%, 86.1% -- and that is the curve the 10 m clearance was read off.

Four deliberate mutations, each caught by the number that identifies it: the
planet-fixed rotation dropped on the way in put the landing **170.2 degrees from
the sunward point**, on the night side, which nothing else noticed because every
site the search returns is dry whichever direction it was asked about -- the
check that catches it had to be added, and that is what the mutation was for; the
height correction dropped gave **17.769 m** of clearance instead of 20; the
surroundings test removed left **299 of 512** sites with water inside 3 m; and
taking an eligible site rather than the nearest one landed **163.72 degrees**
away, most of the way to the antipode.

All three configs build clean, and the Debug and Release captures of the landing
are byte-identical.


### 2026-08-21 (one continuous space, and flying across it)

**The two spaces are gone.** The solar system used to be two scenes with a key
between them: an orbital view at 500 m to the AU where the Earth was 8.5 m
across, and a surface view where it was 360 m across and made of voxels. That
is the standard way out of the problem and it has one fatal property -- you
cannot fly from one to the other, because they are not the same place. There is
now one space from the star to a footprint on a planet, and `L` no longer
teleports: it stands you up where you already are, or puts you back in the ship
where you were standing.

**One exponent does the compressing.** Every length -- body radii and orbital
distances alike -- goes through

```
drawn = 360 m * (true length / Earth's radius) ^ p,   p = 1/2
```

so Earth is 360 m across (which is what makes it a place you can stand on), its
orbit is 55.2 km rather than the 8,453 km true scale demands, the Sun is 3.76 km
across and Neptune is 302 km out. The whole system is 302 km in radius, which is
minutes across at the speeds below.

Square root rather than a log or a hand-tuned table because it is one parameter,
it is monotone -- nothing overtakes anything -- and it is *uniform*, so no orbit
needs special-casing to keep its moon outside its planet. The tightest case in
the system is Phobos, which really does orbit at 2.77 Mars radii; here it comes
out at 1.57 times the sum of the two drawn radii, still clear. The startup log
prints that column for every body, because "does everything still fit" is the
one thing this map can get wrong. What it costs is stated rather than hidden:
true ratios are square-rooted, so Neptune is 8.8x further out than Mercury here
where really it is 77.7x. The `p` slider goes to 1.0 if you want to see how
empty it really is.

**Frames, so a planet does not fly out from under you.** The ship's position is
stored relative to whichever body dominates where it is -- smallest
`distance / radius`, which is scale-free and picks the body filling the most of
your sky -- and re-based when that changes. The handover has to be exact or
flight is not continuous, so the offset is added to the position in the same
statement it stops being applied, and the residual is measured rather than
assumed: **0.000e+00 m**, and the worst one of a session is on the panel.

**A floating origin, and a near plane that follows the altitude.** At 302 km a
float's spacing is 3.6 cm, so everything is drawn relative to the camera with
the subtraction done in double first. Depth precision at distance `z` goes as
`z^2 / (near * 2^24)`, so the 15 cm near plane that standing on a planet needs
would leave two bodies 10 km apart at 300 km range indistinguishable -- out
there it opens to 400 m, because nothing is close.

**The spin belongs to the planet, not to the light.** Terrain is generated in
planet-fixed coordinates, and the first version produced day and night by
rotating the *light direction* backwards. Correct as far as the surface can
tell, and invisible while the surface was its own scene -- but in one space the
lit hemisphere is then whichever way the spin points rather than whichever way
the Sun is, and a planet seen from four radii out with the Sun behind the camera
showed its **night side**. Now the spin rotates the things that are planet-fixed
on their way into scene coordinates, the light is simply the direction the star
is in, and a standing player is carried round once a day so the Sun crosses
their sky without ever being moved.

**An unallocated voxel chunk is air, not rock.** Streaming skips chunks the
surface shell does not reach, on the grounds that they are uniform -- but it was
skipping them without giving them a value, and an unfilled chunk reads `Far`,
which is empty. The planet's deep interior was therefore *hollow* as far as the
mesher was concerned, so a surface chunk whose +x, +y or +z neighbour pointed
inward closed its own rock against that air and produced a wall. That is where
the black slabs standing out of the ground came from, and the tell was that they
were on one hemisphere only: the three neighbours a mesh reads are all in the
positive direction. A constant generator costs the loop but not the noise and
collapses to the same one float.

**And chunks now wait for their neighbours instead of being repaired.** Filling
a chunk stales its low neighbours' meshes, which was already handled by
re-meshing them -- but not before the wrong mesh had been on screen, because
filling runs four chunks a step and the re-mesh queue drains five. Flying down
to a planet, that is seconds of black slabs. Holding a mesh back until its three
high neighbours exist costs one chunk of terrain at the streaming edge, where
the horizon sphere is drawn anyway, and nothing wrong is ever drawn.

**The demo was not step-deterministic, and the reason was the mouse.** Two
identical `--lockstep --land Earth` runs captured at step 300 produced different
frames -- one looking at the sky, one at the ground. The surface controller read
raw cursor deltas every step with no gate, so the desktop pointer drifting
during an unattended run was steering the camera. Both modes now use the
project's convention: Tab toggles mouse-look, Escape releases, arrows always
turn. Fixing it also surfaced that the surface look was inverted horizontally --
yaw is measured from north toward east and east is the camera's right, so
turning right is a *positive* yaw, and it had been subtracting.

**Verification.** A temporary self-test, since deleted, checked the map by its
properties rather than by re-deriving `pow`: that Earth's radius is its fixed
point, that `f(a)f(b) = f(ab/R)f(R)` -- true for a power law and for nothing
else monotone -- that the exponent recovered from two of its own outputs is 0.5,
that it is monotone over ten decades, that every orbit clears its parent, that
prograde is +x toward +z, that a frame handover does not move the ship, and that
landing and taking off returns it to where it was. 12 of 12, and three
deliberate mutations were each caught by the number that identified them: the
dropped handover offset by **2796.58 m**, which is exactly the Moon's drawn
orbit; a reversed rotation by 2.0, which is the diameter of the unit circle; and
a forgotten eye height by **1.20 m**, which is the eye height.

The land-and-take-off residual is **1.2e-5 m** rather than zero, and should be:
the ship's position is a double and a rigid body's is a float, so the round trip
quantises to a float at the planet's radius -- `367 * 2^-23 = 4.4e-5 m`. A
tolerance of zero there would have been measuring the storage rather than the
arithmetic.

All three configs build clean, and the release captures are byte-identical to
the debug ones for both the orbital and the landed frame.


### 2026-08-21 (atmospheres, and a horizon that is a planet)

**The black horizon is gone.** Streamed chunks stop at the load radius, and past
that there was nothing -- a world you were standing on ended at a hard edge a
hundred metres away. The planet's own sphere is now drawn again underneath,
shrunk to `R - amplitude/2` so it sits below the lowest valley: wherever real
terrain exists it is strictly in front and the depth test hides the sphere
entirely, and where terrain has not streamed, the sphere *is* the horizon --
right shape, right colour, curving away as the ground would. A fog would have
hidden the edge; this shows what is actually there.

**Atmospheres, by marching one integral.** A shell sphere around each body with
air, and every pixel of it is a ray through a thin gas: eight steps along the
view ray, four toward the sun at each, with density falling off exponentially
with height. Scattering goes as 1/lambda^4, so blue is thrown sideways out of
the beam far more than red -- and a sunset is the same fact along a path so long
the blue has already gone. Neither is a special case; both fall out of the
integral.

The same shader serves both views. From outside, the ray enters and leaves the
shell and you get a rim of air around a planet; from inside, the near end of the
ray is the camera and you get a sky. The only difference is where the segment
starts, which the sphere intersection already knows. Airless bodies -- Mercury,
the Moon, the small moons -- get no shell rather than an invisible one.

Measured out of the rendered frame, standing on Earth:

```
zenith    (24, 42, 70)     blue:red 2.9
mid sky   (32, 54, 84)
horizon   (61, 90, 113)    blue:red 1.85
```

Brightness rises 2.5x from zenith to horizon, which is the longer path, and the
blue-to-red ratio falls from 2.9 to 1.85 along it, which is the sunset.

**One physics slip worth keeping.** The first sky came out *green*. The star is
drawn warm, at (1.00, 0.86, 0.42), and I fed that to the scattering as the
incoming light -- but the reason the Sun looks warm is that air scatters the
blue out on the way in, which is precisely what the shader computes. Applying it
twice gives scatter (0.22, 0.45, 1.00) times (1.00, 0.86, 0.42) = a cyan-green.
Sunlight above the air is very nearly white, and the star's own disc keeps its
apparent colour.

### 2026-08-21 (spherical gravity, and the damping that hid inside it)

**Gravity is a force per body now, not a direction for the world.**
`PhysicsWorld3D::Gravity` is one vector for everything in it, which is right for
a room and meaningless on a planet: down is a different direction for every body
and weaker the higher it is. So the world's own gravity is switched off on a
surface and each body is pulled toward the centre at `GM/r^2` every step. On a
360 m planet that is 40% weaker a hundred metres up, which is what makes a
thrown rock arc the way it does here rather than the way it would on a flat
world with the same g.

**Surface gravity is derived, not tabulated.** `g = GM/R^2` from the mass in
solar masses and the radius in kilometres that the orbital table already
carried, so Earth comes out at 9.82 m/s^2, Mars 3.71 and the Moon 1.62 -- none
of which is written down anywhere. Then `GM_local = g * R_local^2` keeps that
real surface acceleration on a planet a few hundred metres across, so a jump
feels like a jump while the horizon stays close.

The player is a rigid body: walking steers the *tangential* component of its
velocity and leaves the radial one to the solver, so falling, landing and being
knocked about are physics rather than animation. The capsule's orientation is
set to local up each step rather than solved -- left to the solver it tips on a
slope and rolls toward the equator, because on a sphere there is always a
downhill.

Checked against three formulas the solver does not contain:

```
free fall  at 0 m up:    9.8209 m/s^2 against GM/r^2 = 9.8200   (+0.009%)
           at 200 m up:  4.0584            "         = 4.0583   (+0.002%)
           at 640 m up:  1.2727            "         = 1.2727   (+0.000%)
orbit      radius 599.87 m to 600.13 m about 600.0             (0.04% spread)
           period 81.863 s against 2*pi*sqrt(r^3/GM) = 81.856   (+0.010%)
escape     102% of sqrt(2GM/r) does not fall back; 90% does, from 1,557 m up
```

**The first run of that failed in a way worth keeping.** Free fall passed at
0.05% while the orbit spiralled from 600 m into the ground and a launch *above*
escape velocity fell back. The cause was `RigidBody3D::LinearDamping`, which
defaults to 0.01 -- one percent of velocity a second. Over a tenth of a second
of free fall that is a factor of 0.999 and invisible; over an eighty-second
orbit it is 44% of the speed. The three checks disagreed with each other, and
the one that failed was the only one that integrates long enough to notice.
Probes and loose rocks set it to zero now: a planet with no atmosphere has no
drag, and the default is a stand-in for air.

### 2026-08-21 (voxel planets, and landing on one)

**The planets are signed distance fields now**, `|p| - R - relief(p)`, so down is
toward the centre from everywhere and there is no edge to fall off. Press **L**
near a body and the camera moves into that planet's own frame, where it is a few
hundred metres across and made of voxels, while the orbits keep being integrated
behind it. `--land Earth` does it from the command line, which is how an
unattended capture reaches a surface.

**Two spaces and two clocks, both forced by arithmetic.** The orbital view is
500 m to the AU, where the Earth is 8.5 cm across; a planet you can stand on
wants a few hundred metres of radius, and one continuous space cannot hold both.
Landing switches spaces -- and time scales, because a day is 1/365 of a year, so
at an orbital rate it passes in a fiftieth of a second. On landing the clock
slows until a day takes about a minute, and leaving puts it back.

**Only the shell of a planet is ever generated.** A chunk whose distance from the
centre misses the band `R +/- (relief + chunk radius)` cannot contain a crossing,
which is a test on one number. Without it a 300 m planet at 1.5 m voxels is
17,576 chunks of 4,096 samples -- 72 million density evaluations for a world you
can see 200 m of.

Four things went wrong, and each is the kind that looks like something else:

- **The mean surface was 8.71 m too high.** Relief is built from `1 - |noise|` to
  make ridges rather than blobs, and the obvious recentring subtracts a half. The
  mean of `|noise|` for this generator is about 0.165, not 0.5. A sphere of the
  wrong radius still looks exactly like a sphere, and everything derived from the
  radius -- sea level, where a lander stops, surface gravity when it arrives --
  would have carried the error. The bias is measured over 2,048 directions at
  generation and subtracted; a check sampling a *different* 4,000 directions with
  a different generator reports a mean radius of 299.94 m against 300 asked for.
- **The terrain was made of spikes**, because the noise was sampled at
  `direction * 90` with five octaves -- putting the finest octave at 1.5 m on a
  1.5 m lattice, which is noise per voxel. Features are specified in metres now,
  so the octaves land where you can say where they land: 70 m down to about 9 m.
- **The sun crossed the sky in two seconds** and captures kept landing at
  midnight. Terrain is generated in planet-fixed coordinates and the surface
  frame did not rotate, so the only thing moving the light was the *orbit*. Real
  planets have days because they spin: rotation periods are in the table now
  (Venus and Uranus retrograde, as negative values) and the sunlight is rotated
  into the planet's turning frame.
- **A black grid of cracks, one line per chunk boundary.** `ChunkRange` includes
  one plane past a chunk's own cells, so a mesh reads the first plane of the
  chunks above it -- and a chunk meshed before those were filled meshed against
  empty space. Filling a chunk now marks its three low neighbours stale and they
  are re-meshed. It looked like a mesher fault and was an ordering one.

**`PerspectiveCamera::SetOrientation`** is new, and the header comment reading
"roll is deliberately absent; add it when something needs it" is why. Yaw and
pitch are measured against a fixed world up of +Y, which is meaningless on a
sphere: standing where local up is horizontal in world terms, a yaw/pitch camera
renders the ground *up the side of the screen*, which is precisely what the first
landing did. The surface controller builds a tangent frame wherever you are
standing and hands the camera that basis directly.

Walking is kinematic -- move along the tangent, then snap to the ground found by
bisecting the density field. Spherical gravity through the rigid-body solver is
the next piece, and the ground query it will need is the one already here.

### 2026-08-21 (a solar system, and Kepler as the test)

**Nothing in the new Solar system demo contains an orbit.** The integrator knows
one law -- `a = -GM r / |r|^3` -- and the ellipses, the periods and their ratios
are all consequences. Driving each planet round a circle with a sine and a
cosine would look identical and prove nothing, which is the point: because the
paths are integrated, Kepler's third law is available as a *check* rather than
as the implementation.

Units are astronomical and chosen so that check is free: distance in AU, time in
years, and `GM` for the Sun as `4*pi^2 AU^3/yr^2` -- not a fudge but what `GM`
is in those units, since the Earth orbits at 1 AU in 1 year. Sixteen bodies with
real data: eight planets, the Moon, Phobos, the four Galilean moons and Titan.
The panel puts the measured period beside `2*pi*sqrt(a^3/GM)` for every one.

Measured against predicted, after the measurement was fixed twice:

```
Mercury  0.240750 vs 0.240750   +0.0000%      Moon      0.075184 vs 0.075183  +0.0002%
Earth    1.000000 vs 1.000000   +0.0000%      Titan     0.043666 vs 0.043666  +0.0006%
Jupiter 11.868087 vs 11.868087  +0.0000%      Ganymede  0.019590 vs 0.019590  +0.0028%
Saturn  29.452195 vs 29.452195  +0.0000%      Io        0.004845 vs 0.004845  +0.0454%
                                              Phobos    0.000875 vs 0.000874  +0.0872%
```

Those are also the real periods -- Jupiter's year is 11.86 of ours, Saturn's
29.46 -- which nothing in the code was told either.

**The measurement was wrong twice before the physics was ever in question**,
which is becoming the pattern in this project:

- Every one of the sixteen bodies reported a period **50.0% short**. A constant
  factor across sixteen independent orbits is not physics: an integrator that
  was actually wrong would be wrong by different amounts at different radii. The
  clock started at t=0 and stopped at the first wrap of `atan2` from +pi to -pi,
  and a body starting on the +x axis reaches that wrap at *half* an orbit.
- Then Phobos, Io and Europa reported errors of **+3,000% to +4,000%** while
  everything else was exact. Those are the bodies whose period is shorter than
  the sampling interval -- Io goes round Jupiter in half of one fixed step at a
  modest time scale, so the angle was aliased and an aliased angle can report
  any period at all. Periods are tracked per *substep* now, and a sample that
  turns more than pi is refused outright rather than reported: the panel says
  "too fast" instead of a number.

Crossing times are interpolated within the step they happen in. Without it every
period came out an exact multiple of the step -- 1/60 yr -- and Mercury, the
fastest at 14 steps an orbit, read 3.1% short for that reason alone.

**With the measurement honest, the residual turned out to be a property of the
integrator worth knowing.** Quadrupling the substeps took Phobos from +1.3815%
to +0.0872%, a factor of **15.8**. Semi-implicit Euler is first order, so the
naive expectation is 4; the observed 15.8 is 4^2, because a symplectic
integrator's *frequency* error is second order even though its trajectory error
is not. The error ranking across the table is monotone in substeps-per-orbit,
which is the same statement seen sideways.

Three scales, and the arithmetic that forces them, because the first attempt put
the camera inside the Sun. 500 m to the AU. At that distance scale the Earth is
**2 cm** across and the Sun 4.7 m; at the planets' 400x exaggeration the Sun
would have a 930 m radius, which is nearly five times Mercury's entire orbit --
the inner planets would be inside it. So the star gets its own, much smaller
exaggeration, and the numbers are on the panel rather than hidden.

Still to come: the planets as voxel terrain close up, atmospheres lit by the
star, and landing on them.

### 2026-08-21 (the colonies get out of the way of your windows)

**The wallpaper reads the desk now.** Open windows are ground the colonies will
not grow on: agents read them as strongly repellent rather than merely empty, so
the structure crowds up to a window's edge and reorganises around what is
actually on screen. Move the window and the cells under it come back as *empty*
land, which the three breeds then race for.

Getting the geometry at all is the interesting part. **On a Wayland session the
X11 client list is useless** -- it holds XWayland clients and nothing else, which
on this desk was two windows out of twenty. The only component that can see them
all is the compositor, and KWin scripts cannot open a file or a socket. So the
chain is three links:

```
KWin script  --callDBus-->  tools/egss-windows.py  --file-->  the wallpaper
```

`./egss.py windows` runs the middle one: it owns `org.egss.Wallpaper`, loads the
script into KWin, and writes what arrives to `$XDG_RUNTIME_DIR/egss-windows` by
atomic rename. A file rather than a socket because the wallpaper is a game loop:
it wants to read the current state when it happens to look, not to service a
connection. Nothing starts it automatically, and with it absent the wallpaper
behaves exactly as it did before -- the right failure for a decoration.

**KWin's coordinates are not the wallpaper's**, which is the trap worth
recording. KWin works in *logical* pixels: a 3840x2160 monitor at scale 1.5 is
2560x1440 to a script, and this desk's laptop panel sits at 1631,1440 logical
against 2447,2160 physical. The wallpaper is an XWayland client living in
physical pixels. Each line therefore carries the window's rectangle *and* its
output's, and the scale is derived as the ratio of that output's logical size to
the physical size XRandR reported for the monitor of the same name -- derived
rather than assumed, because a desk can mix scales.

Verified against arithmetic done outside the engine. With three windows open,
the engine reported **178,789 of 304,128 cells covered (58.8%)** and an
independent calculation from the monitor table gave **178,789**. Then the
picture itself, sampled out of a captured frame:

| region | pixels lit |
| --- | --- |
| inside the DP-1 window | 0.0% |
| inside the HDMI-A-1 window | 0.0% |
| inside the small eDP-1 window | 4.5% (the edge ring; blocking is cell-granular) |
| uncovered patch of eDP-1 | 92.6% |

**One bug worth keeping**, because it failed silently in both directions. The
first parser read a line with one `sscanf` and a `%*[^,]` for a scale field, and
KWin sent that field *empty*. `%*[^,]` needs at least one character, so
conversion stopped there, the output name came back empty, every rectangle was
dropped for matching no monitor, and the wallpaper reported no windows at all --
while the bridge was receiving them perfectly and writing them to disk. The
field is gone from the protocol (the reader derives the scale anyway) and lines
are split on commas rather than scanned.

**This is the one thing in the demo that is not deterministic**, and it cannot
be: the desk is external state. It is confined to wallpaper mode, so a recorded
session of the demo still replays exactly, and `--no-windows` turns it off.
Polled every eighth fixed step -- a `stat` is cheap but not free, and a
wallpaper reacting an eighth of a second late is a wallpaper reacting
immediately.

### 2026-08-21 (three breeds, and a fight over the desk)

**The wallpaper spans the whole arrangement now**, read from XRandR rather than
assumed: three monitors, 7,680 x 3,960, one continuous field positioned at the
union's own origin. The previous version took the *primary* monitor's video mode
-- 2,880 x 1,800 -- and dropped it at 0,0 on a 3,840 x 2,160 screen, which is
where "it only covers two thirds of one monitor" came from. The arrangement here
is an L rather than a rectangle, so the span includes two corners that are on no
screen at all; the colony grows into them unseen, which is what one spanning
surface costs.

**A cell has an owner now, and that is what makes it a fight.** The trail map
carries a strength and the breed that laid it. An agent depositing on a rival's
cell *subtracts* instead of adding, and takes the cell when it drives the
strength through zero -- so fronts exist, they move, and the breed depositing
harder eats into the other. Ownership diffuses by strength rather than by
majority: a cell's next owner is whichever neighbour holds the most trail, which
makes a front advance where one side is denser and hold where they are even.
Majority voting jitters, because nine cells vote in ninths.

One float and one byte per cell. The obvious alternative -- a trail map per
breed -- triples the blur, which is the expensive part.

**Three breeds, differing in parameters only**, one seeded per monitor:

| | sensors | look | toward rivals |
| --- | --- | --- | --- |
| Veins | narrow, long reach | branching network | avoids (-0.6) |
| Foam | wide, short reach, inhibited | cellular, area-filling | pushes in (+0.35) |
| Filigree | narrow, fast turning, faint | restless thin filaments | mildly avoids |

`RivalWeight` is the parameter with no single-colony meaning and it is what makes
them behave differently *toward each other*: negative avoids enemy ground,
positive is drawn to it, so an avoider beside an attacker is a border that only
moves one way. `Inhibition` is the other new one -- above `Peak` it bends the
sensed value back down, so ground that is already busy stops being attractive
and agents spread to fill an area instead of piling onto its spine. At zero the
classic behaviour is exactly as it was.

**The tuning lesson, which cost most of the session: deposit over decay is the
equilibrium strength.** Foam started at 6 against 4.5 -- a ratio of 1.33, above
the clamp -- so every visited cell pinned at white and the picture was binary. I
swept sensor angle against sensor distance for an afternoon looking for
structure in an image that *could not change*, because the structural parameters
were being flattened by saturation downstream of them. Veins was 3/9 = 0.33 and
had been fine all along, which is why the problem read as "foam is wrong" rather
than "the ratio is wrong". Check the ratio first; it is one division.

Costs, measured across the span per fixed step at 30 Hz, differencing 200-frame
and 1,200-frame runs so startup is not in the number: **24.2 ms at 8 px/cell,
16.8 at 10, 13.0 at 12**. Ten is the default, at half a core. Fusing the blur's
sum and strongest-neighbour search into one pass over the nine neighbours was
worth about 4 ms of that -- the split version wrote an `offsets[9]` array to the
stack and read the field twice, at 475,000 cells a step.

Still deterministic: byte-identical across two runs at step 500.

### 2026-08-21 (the slime mould, as an actual wallpaper)

**`--wallpaper` puts the running demo on the desktop**, and on Plasma 6.7.3 /
KWin 6.7.3 it works: the window sits above the desktop icons and below every
other window, sticky across virtual desktops, no taskbar entry, no focus stolen.

It is three EWMH properties and a hint, not a new backend. The window is created
**hidden**, marked `_NET_WM_WINDOW_TYPE_DESKTOP` plus `_NET_WM_STATE_BELOW`,
`STICKY`, `SKIP_TASKBAR` and `SKIP_PAGER`, and only then mapped -- the order
matters, because a window manager reads the type when it takes the window over,
and a desktop window that arrives as an ordinary one has already been stacked,
framed and focused by the time the property lands.

**This works on a Wayland session because GLFW here is built `_GLFW_X11` only**,
so every EGSS window is already an XWayland client and has been all along.
Nobody had noticed, because nothing until now cared which display server it was
talking to.

What KWin actually did with it, read back with `xprop` rather than assumed:
`_NET_WM_WINDOW_TYPE_DESKTOP` accepted unmodified, all four state hints kept,
and the window first in `_NET_CLIENT_LIST_STACKING` -- the bottom. Confirmed on
screen: icons covered, ordinary windows on top.

`--wallpaper` implies `--hide-ui`, because a wallpaper with a debug panel on it
is not a wallpaper; `--show-ui` puts the panels back, which is how you tune the
thing while looking at it.

**Known limits, none of them fixed yet.** The primary monitor only. It renders
at the full frame rate whether or not anything is covering it, which is about
36% of a core at 60 Hz. And it goes through XWayland rather than talking to the
compositor directly. All three are what a native `zwlr_layer_shell_v1` client
would fix -- KWin advertises version 5 -- and none of them stop this being
usable today.

### 2026-08-21 (the audio thread was racing the main thread, and had been all along)

**ThreadSanitizer found eight distinct data races between `ClaimVoice` and
`MixInto`**, which is the pair the sanitizer work was aimed at and the one this
project had reasoned about most carefully. The comment in `ClaimVoice` said it
outright:

> Only an inactive voice may be written to; the audio thread is guaranteed not
> to be reading it.

That is not what `Active == false` means. It means the mixer will not *start*
this voice at the top of the next block; it says nothing about the block the
mixer is in the middle of. So `Stop` could clear the flag while `MixInto` was
interpolating a voice, `ClaimVoice` would then see a free slot, and
`voice.Clip = clip` would drop the last reference to the very samples the mixer
was reading. **A use-after-free in the audio callback, reachable by playing and
stopping sounds quickly enough** -- which is what a game does.

The fix is an ownership protocol rather than a lock, because a lock in a device
callback is how you get priority inversion:

```
Free     -> Claimed    main, in ClaimVoice
Claimed  -> Playing    main, once the parameters are written
Playing  -> Stopping   main, in Stop
Playing  -> Free       mixer, when the clip runs out
Stopping -> Free       mixer, at the next block boundary
```

**Only the mixer may return a slot to `Free`**, and only after it is provably
finished with it -- so a stop costs one silent block, and that block is the
entire point. `Stop` compare-exchanges `Playing -> Stopping` rather than
storing, so a stale handle cannot drag a slot that has since been re-used back
out of `Playing`. The audio thread still never frees memory: main overwrites
`Clip` when it claims, and by then the mixer has said it is done.

Verified both ways, because a protocol bug here is as likely to produce silence
as a race, and **nothing else in this project would notice a silent mixer** --
no capture shows sound:

- **TSan, same stress, before and after: 13 reports down to 5**, and none of the
  five involve our audio code (four are ALSA and TSan internals, one is libxcb
  inside `glfwCreateWindow`). The eight `ClaimVoice`/`MixInto` pairs are gone.
- **Offline through `RenderForTest`, six checks**: silence with nothing playing,
  peak 0.3536 while playing, silence immediately after `Stop`, 200 play/stop
  cycles all finding a slot (the pool does not run dry), and a stale handle
  failing to stop the sound that reused its slot.

**The classifier was wrong before it was right**, in the way these things
usually are. The first version asked whether our source appeared in the top
three frames of a racing access, and called all fourteen demos a failure: the
libxcb race during `glfwCreateWindow` puts `recvmsg` at frame #0, `_xcb_in_read`
at #1 and our window constructor at #2 -- where it is the *caller* of the racing
read, not the thing that raced. It now asks for the **innermost** frame,
skipping TSan's own interceptors, which sit at #0 for anything it wraps. Checked
against two logs whose answers were already known: the run before the fix
classifies 8 ours / 5 theirs, the run after 0 / 5.

Two pieces of tooling came out of it. `AudioRaceStress` claims six voices a step
and stops the oldest, alternating between two clips so a stolen voice's old
buffer is a *different* allocation -- **silence from a race detector is only
evidence if the window was open**, and the demos on their own play a handful of
sounds. And the sweep now **classifies** TSan reports rather than counting them:
a report is ours when our source appears in the top frames of a racing access,
and somebody else's otherwise. Deliberately not a suppression file -- a
suppression matching `libasound` would also swallow a real race of ours that
passes through ALSA on its way to the mixer, which is precisely where these
races lived.

### 2026-08-20 (demos get folders, and a slime mould)

**The demo list grew past what a flat panel reads well as**, so the registry
carries a `Folder` per entry and the selector groups by it -- collapsible, with
the folder holding the live demo open by default and the rest closed. Folders
are enumerated in first-appearance order, walked from the table rather than
declared in a list of their own, so adding a demo to a new folder creates it and
there is no second place for a folder name to drift.

**The order of `s_Demos` is deliberately untouched, and now says so in the
file.** A recording stores the demo's *index* (`Replay::GetRecordedDemoIndex`),
so sorting the table -- the obvious way to implement folders -- would silently
repoint every recording made before the sort at a different scene. Grouping
belongs in the selector; the table appends.

**Physarum, as the first demo in the `Life` folder.** A few hundred thousand
agents, each smelling the trail map at three points ahead of it and turning
toward the strongest, each leaving a little trail behind. Everything the colony
does -- veins, junctions, two strands finding each other and merging -- comes
out of those two rules plus a blur and a decay, and none of it is written down
anywhere in the file. Attract, repel and feed are on the three mouse buttons;
attract and repel bias the *turn* rather than the position, because pushing
agents directly moves them without changing what they want and the colony snaps
back the moment you let go.

On the fixed step like everything else, and **byte-identical across two runs and
across debug and release**. It is single-threaded on purpose: several threads
depositing into one trail map add their floats in whatever order they arrive,
and the run stops reproducing itself.

**Deposit and decay are a ratio, and the ratio is the picture.** The first
attempt used 6/s deposit against 0.6/s decay -- 0.1 a step against a 1% loss --
and every cell reached the clamp within seconds, so the field came out solid
white with the *low* ground showing through as holes. At 3/s against 9/s a cell
visited every step settles at about 0.36, and the network appears.

Then three optimisations, **16.3 ms a step to 5.95 ms**, measured by differencing
200-step and 1,200-step runs so the startup cost (every demo pays OpenWorld's
`OnAttach`) is not in the number:

- **The blur's wrap, 16.3 to 13.7 ms.** Two integer divisions per sample, nine
  samples per cell, 147,456 cells. Only the four edges can wrap, so the interior
  is its own loop -- with the nine terms summed in the same order, since float
  addition does not associate and reordering them would stop old recordings
  reproducing.
- **The palette, 13.7 to ~13 ms.** A `pow` per pixel per frame became a
  256-entry table; the trail is one number per cell, so the colour is a function
  of one number, so it is a table. Exposure scales the index, so dragging it is
  still immediate.
- **The trig, to 5.95 ms.** Eight sine/cosine calls per agent became two, via
  `cos(a ± s) = cos a cos s ∓ sin a sin s` with the sensor offset's pair hoisted
  out of the loop. The identity is exact but the floats differ in the last bits,
  so the same seed grows a *different* colony -- not a wrong one. Verified
  deterministic again afterwards.

### 2026-08-19 (the 4:1 seam, and where the 8.4x actually came from)

**`VoxelTransition`'s 4:1 case is verified.** It had only ever been exercised at
2:1 -- the 4:1 path is the same recursion one level deeper, which is exactly the
kind of claim worth not taking on trust, since the two reverted attempts at this
seam were also nearly right. Two chunks sharing a real boundary, the coarse one
at stride 4 against a stride-1 neighbour: **96 open edges without the
transition, 0 with it**, and 110 -> 0 on a second field placed off the lattice.
Asking for `ratio` 2 where the neighbour is four times finer -- one level short
-- leaves **108** open, which is what proves the ratio reaches the recursion
rather than being ignored.

It is not reachable at the default bands (16 m chunks, bands at 56 and 104 m
with 8 m hysteresis, so neighbours differ by one band), but the panel's sliders
bottom out at 16 m and 32 m, one chunk apart, which puts it in play.

**The measurement was wrong twice before it was right, and that is the more
useful half of this entry.** Counting open edges means deciding which vertices
are the same vertex, and the obvious way -- snap positions to a grid, compare
keys -- is unstable in both directions:

- Two points a micron apart that straddle a grid line read as a hole. At a
  1e-4 m grid a closed stride-2 sphere reported 4 open edges; at 1e-5, 12; at
  1e-6, 120. **The count grows as the ruler gets finer**, which is the
  signature of a ruler and not a hole.
- Coarsening does not fix it: at 1e-3 m the same sweep *invented* four holes in
  a stride-1 mesh that finer grids called closed, by merging two points that
  really were distinct.

The cause is `MarchingTetrahedra`'s `Crossing`, which interpolates from
whichever endpoint it was handed first -- `pa + (pb - pa) * va/(va - vb)`. Two
tets sharing an edge hand it the same corners in opposite orders, which is the
same point in arithmetic and not always the same float. Welding by *proximity*
instead -- look in the 27 neighbouring cells, reuse any representative within
epsilon -- makes the tolerance mean what it says, and the answer stops moving:
**0 open edges at 1e-2, 1e-3, 1e-4 and 1e-5 m**, breaking down only at 1e-6,
below the ulp noise. Four decades of agreement is what makes the 0 above worth
quoting; the first version of this test would have reported a seam bug that was
not there.

**And the 8.4x was not the mesher.** The 2026-08-18 entry recorded the
`MarchingCubes` to `MarchingTetrahedra` swap as costing 8.4x the triangles at
"the same radius, same bands". Measured as a 2x2, at a 128 m radius, converged
(step 2,500 onward; step 500 is still 14% short):

| bands | tets | cubes | tets/cubes |
| --- | --- | --- | --- |
| 56 / 104 | 637,186 | 186,291 | **3.42x** |
| 24 / 48 | 240,734 | 67,955 | **3.54x** |
| band factor | 2.65x | 2.74x | |

The mesher is worth ~3.4x. The bands are worth ~2.7x, and they had been widened
from 24/48 to 56/104 two commits earlier, in `3e44d6a` -- so the before and
after were measured either side of a change nobody was accounting for, and
3.42 x 2.65 = 9.1 is the 8.4x that was attributed to the mesher alone. A
microbenchmark on a sphere agrees with the mesher half: 3.03x at stride 1,
3.18x at stride 2, 2.77x at stride 4.

**Nothing is being done about it, on measurement.** The swap costs **0.33 ms a
step** -- 3.43 ms against 3.10 ms over 3,000 lockstep steps in release, about
8% -- so 3.4x the triangles is not what this demo is spending its frame on.
Reconciling `MarchingCubes`'s ambiguous cases was declined before as
research-grade; at 0.33 ms it is not worth reopening. The band lever is real and
is a *visual* decision rather than a performance one, and the numbers for making
it are in the table. The stale comment at the top of `OpenWorld.h` -- still
describing 24/48 bands and quoting 81,413 triangles, two commits after both
changed -- now carries all of this.

### 2026-08-19 (a sanitizer you can reach in one command)

**UBSan found 2026-08-17's miscompile on its first run, and there was no way to
reach it again.** That afternoon -- a signed overflow in a hash, a loop whose
exit test the compiler deleted, `SpawnRocks` running 99,482 times, 17 GB and an
OOM kill, with `-Wall -Wextra -Waggressive-loop-optimizations -Warray-bounds=2`,
`-D_GLIBCXX_ASSERTIONS` and `-fstack-protector-all` all silent -- ended with the
right tool being run by hand once. `./egss.py sanitize` is that tool wired in.

`--sanitize` is a **generation option, not a fourth configuration**. Five
project files would each have needed one and three of them describe vendored
code; an option composes with the three configurations instead, which matters
because optimisation changes what undefined behaviour *does* even though it does
not change whether UBSan sees it. Instrumented output goes to
`bin/<Config>-<system>-x86_64-sanitize/`, and **the separate tree is
load-bearing**: make cannot tell that the compiler flags changed, so
instrumented and plain objects in one directory would link whatever was
there.

`./egss.py sanitize` builds that and then runs **every demo** under it, in
lockstep with the window hidden, one process each so the demo that produced a
report is named beside it. The demo list is read out of `DemoRegistry.h`, so
adding a demo adds it to the sweep -- the newest thing being the one most likely
to need it.

**It catches the bug it was built for.** Reintroducing the exact historical
mistake -- `(uint32_t)(x * 374761393)`, the cast outside the multiply -- and
sweeping reports it in **all thirteen demos**, because `OnAttach` is unguarded
and every demo builds OpenWorld's world:

```
src/OpenWorld.h:2399:21: runtime error: signed integer overflow:
    -10 * 374761393 cannot be represented in type 'int'
    #0 OpenWorld::Hash2D(int, int, unsigned int)   src/OpenWorld.h:2399
    #1 OpenWorld::Hash2DUnit(int, int, unsigned int) src/OpenWorld.h:2408
    #2 OpenWorld::Noise2D(float, float, unsigned int) src/OpenWorld.h:2419
    #3 OpenWorld::Height(float, float) const       src/OpenWorld.h:2308
```

The file, the line, the values, and the call path -- against an afternoon that
produced no answer at all. Reverted, both sweeps are clean: **13 demos, 0
reports** in debug and again in release.

Two things were measured rather than assumed. **Leak detection is left on**: the
usual reason to disable it is a driver that leaks by design, and 300 steps of
every demo reports nothing here, so switching it off "to reduce noise" would
have thrown away a real check for noise that does not exist. And the cost is
**2.1x to build, 5.3x to run** -- 1 m 05 s against 2 m 20 s cold, 6.1 s against
32.5 s for 300 steps -- which is why it is a command rather than the default.

One trap found by falling into it: `--no-gen` and `--sanitize` disagreeing is
silent. The flags *and* the output directory live in the generated project
files, so `build --sanitize --no-gen` after a plain generation cheerfully
rebuilds the plain tree, and `sanitize --no-gen` would sweep the plain binary
and report no findings -- which reads as a pass. `egss.py` now reads
`-fsanitize` back out of the generated makefile and regenerates anyway, saying
so.

### 2026-08-19 (a heightfield's box gets no floor)

**A heightfield collides like a solid volume extending downwards and its bounds
described only the surface.** `Heightfield3D`'s narrowphase calls any point
below the surface inside -- a sphere's depth is measured along the winning
triangle's normal and keeps growing the further under it sits, a box corner is
tested with `local.y < height` and nothing else -- while `BodyBounds` returned
the band between `Lowest` and `Highest`. A body under the map was ejected by one
half of the pipeline and disowned by the other.

`BodyBounds` now reports `-infinity` for such a body's `outMin.y`. **Infinity
rather than the large negative sentinel an earlier note here proposed**, and
that mattered: the two consumers of these bounds both divide the span by the
cell size and cast to `int`, and a value past `INT_MAX` is undefined behaviour
rather than a big number -- the same shape as the signed overflow that got a
loop deleted on 2026-08-17. `RebuildGrid`'s cell counts are now computed in
`double` and capped *before* the cast, and `CellRange` clamps as floats before
its own, with the clamp bounds one wider than the ranges they feed so an empty
overlap stays expressible as `x1 < x0` instead of being squeezed into a
spurious cell 0.

**A grid cannot hold a half-space, so the index gets a second box.**
`IndexBounds` is `BodyBounds` made finite, and the grid classifies, sizes and
buckets on that -- two boxes for one body, answering different questions, the
same split `Heightfield3D` already makes between a face normal and a smooth one.
Giving the index the surface band and stopping there is what the old code
effectively did, and it drops the pair for a body deep under the map: the
terrain is stamped into the band, the body is in a cell 30 m below it, and they
never meet. So a body with no floor is lowered to the **grid's** floor before
stamping, once the rebuild knows where that is.

**That was a live bug, not only a latent inconsistency.** It needs
`BroadphaseExcludeOversized = false` to reach -- on the default path the
heightfield is `m_Oversized` and so a candidate for everything already, which is
why it never showed -- but the grid is supposed to be *bit-identical* to brute
force, and there it silently was not.

Verified with a tilted plane, so every expected depth is a line of arithmetic
the collider does not contain: a sphere under a plane touches at
`r + drop*cos(theta)`, and the cosine converts the vertical drop the field is
described by into the perpendicular one a contact measures. 17 checks.

- **Solid to any depth**, 0.1 m to 1000 m under: 0.5970/0.5970, 1.4701/1.4701,
  10.2014/10.2014, 97.5143/97.5143, 970.6426/970.6425 reported against
  predicted. Asserted of all three shape paths rather than generalised from the
  sphere, because each reaches the field differently -- a box 30 m under reports
  29.7106 against 29.7106 (its deepest corner is the one further up the slope),
  a capsule 29.9923 against 29.9923 (its lowest sample wins).
- **Solid nowhere else**: nothing 2 m above the surface, and nothing 20 m under
  it but 4 m past the edge of the map -- the box's sides still describe the
  collider even though its bottom no longer does.
- **The three broadphases agree**: brute force, grid with the terrain excluded,
  and grid with it bucketed all report 29.6043 for the same body 30 m under.
  Mutating the fix out reproduces the historical bug exactly -- the bucketed
  case reports **no contact at all**, not a wrong depth.

Nothing on the default path moves: `Physics3D`, `Ragdoll`, `Map Building`,
`Voxel terrain` and `Open World` all capture **byte-identical** frames at step
240 against the same build without the change.

One thing deliberately not done. A bounds test in front of the oversized
candidates is *sound* now, where before it would have stopped ejecting bodies
under the map, and it is still not worth writing: everything standing on a map
is inside the map's box, so it would reject almost nothing. The lever there is a
vertical reject against the highest sample under a body's footprint, which wants
a coarse max-pyramid over the field rather than one box around all of it.

### 2026-08-18 (the LOD seam, closed -- and a mesher swap it turned out to need)

**The oldest outstanding defect in the project is fixed.** Two attempts on
2026-08-16 fanned a coarse chunk's subdivided seam face from the cell's
centre and were reverted: the fan's *side* faces, perpendicular to the seam,
were left as flat coarse triangles, so two neighbouring transition cells put
a different number of vertices on the edge between them -- a T-junction, not
visible but fatal to the watertightness check.

**`VoxelTransition`** (`EGSS/src/Egss/Voxel/VoxelTransition.h/.cpp`) fixes it
by marking *edges*, not faces. Against a coarse cube cell's fixed six-tet
decomposition (`MarchingTetrahedra::CellTetrahedra()`), exactly two tets have
a full face on a given boundary ("cap" tets) and two touch it along one edge
only ("collar" tets); the other two don't meet it at all. A cap tet's face is
recursively quadrisected down to the fine chunk's own lattice spacing,
sampling the true field fresh at every new point (the field is exact at any
integer lattice point regardless of a chunk's stride -- "coarse" and "fine"
differ only in which points a mesher samples), then every small triangle is
coned to the tet's one off-boundary apex. A collar tet's one marked edge is
subdivided the same way and the tet split into a strip of small tets sharing
its two off-edge corners. Every consumer of a marked edge fetches its
subdivision from one function keyed on the edge's own endpoints
(`SubdivideEdge`), so two tets that share a physical edge -- whether two of
one cube's six or two either side of a cube or chunk boundary -- agree by
construction, with nothing to coordinate. All of it reduces to calls to the
existing, unmodified `MarchingTetrahedra::Cell`; no new triangle-emission
code, no lookup table. Chunk corners needing the treatment on two faces at
once are explicitly out of scope and mesh plainly, same as before -- a
smaller, rarer residual than the seam this closes.

Verified the way the reverted attempts were, and further: two chunks at
different strides sharing a real boundary, edge-use counts before and after
(**64 holes → 0**, sphere surface, curved so the coarse lattice provably cuts
corners), plus a mutation that reproduces the exact historical bug (cap tets
fixed, collar tets left plain) and confirms the harness would have caught it
(**0 → 128 holes**).

**That surfaced a second, unrelated defect: `MarchingCubes` and
`MarchingTetrahedra` do not agree with each other on a shared face, even at
identical stride.** Isolated with a direct control, no LOD involved: two
plain `MarchingCubes` calls split at a plane, 0 holes; two plain
`MarchingTetrahedra` calls split at the same plane, 0 holes; one of each
meeting at that plane, **102 holes**. Each mesher is internally consistent;
they are not consistent with each other -- marching cubes' well-known
ambiguous saddle cases resolve differently under a fixed tetrahedral
diagonal, which is the exact ambiguity `MarchingTetrahedra` was built to
sidestep. It does not stay local: patching one boundary layer just relocates
the mismatch to whatever it borders next, so there is no bounded, layer-at-a-
time fix -- only committing to one mesher across a whole connected region
terminates it.

**`OpenWorld` now meshes all terrain with `MarchingTetrahedra`, not
`MarchingCubes`.** `VoxelTerrain` (the other voxel demo) is untouched --
it never varies stride, so it never hits this. The real cost, measured
rather than assumed: at the default 128 m load radius, the old
`MarchingCubes`-based LOD held 81,413 triangles; the same radius, same bands,
now costs **684,718** -- roughly 8.4x. Not yet offset by anything; worth a
follow-up pass (tighter LOD bands, or the ambiguous-case reconciliation noted
below) if the frame cost turns out to matter in practice.

> **Corrected 2026-08-19.** "The same bands" was not true: they had been widened
> from 24/48 to 56/104 in `3e44d6a`, before this. Measured as a 2x2, the mesher
> is worth 3.4x and the bands 2.7x, and the product is the 8.4x recorded here.
> The swap's own cost is 0.33 ms a step. See the 2026-08-19 entry. Skirts, already
default-off and known not to fix the seam (2026-08-16), are removed from the
terrain path entirely -- superseded, not merely redundant.

Two things not done, on purpose. **`MarchingCubes.cpp` itself was not
touched** -- forcing its ambiguous-case table to agree with the tetrahedral
diagonal would keep triangle counts down but is its own research-grade task
(this is what Lewiner's "Marching Cubes 33" is about), and was left rather
than attempted alongside everything above. **The 4:1 stride case
(`VoxelTransition`'s `ratio` parameter, exercised so far only at 2:1) is
implemented via the same recursion depth but not separately verified** --
worth a dedicated check before relying on stride-4 chunks bordering stride-1
ones directly.

### 2026-08-17 (render state that leaked between demos, and the matrix that found it)

**A demo's captured frame should not depend on which demo ran before it. Three
of thirteen did.**

Persistent pipeline state — blending, depth write, cull face, polygon mode — was
established once in `OpenGLRendererAPI::Init` and then changed by whichever layer
wanted it changed, with nothing re-establishing it at a frame or layer boundary.
The expensive part of that is not the wrong pixel, it is that **the demo which
breaks is not the demo that broke it**, so reading the broken demo's code tells
you nothing. An earlier session lost hours to exactly this with `CullFace` left
at `Back`, which made OpenWorld's single-sided water invisible from below.

So the first thing built was a way to *provoke* it: `--warmup <demo>
--warmup-steps <n>` runs another demo first and then switches. The step
accounting works out because `DemoLayer` seals the is-this-demo-active guard —
an inactive demo does not fixed-update, update or draw — so a run warmed up for
W steps and captured at step W+T shows the target after exactly T steps of its
own, comparable pixel for pixel against a plain run captured at step T.

That makes the test a matrix: every demo after every other demo, hashed. 13
demos, **156 warmed runs**. Result:

```
LEAK  Breakout  after OpenWorld
LEAK  Scene     after OpenWorld
LEAK  Acoustics after OpenWorld
```

One culprit, and the three victims are exactly the Renderer2D demos. OpenWorld's
water finished with `SetBlendMode(BlendMode::None)` — `glDisable(GL_BLEND)` —
while the baseline `Init` established was blending *on*, and Renderer2D never
sets blending itself. **Every one of those call sites already had a "restore";
what each of them restored was its author's assumption about the baseline.**

Fixed with `RendererAPI::ResetState()`, called once per frame from
`Application::Run` before any layer draws, and reused by `Init` so the baseline
is written down once instead of twice. A per-frame reset rather than save/restore
at each site, because the sites were the problem. Every demo already sets the
state it needs inside its own draw path — none set it in `OnDemoAttach` or
`OnDemoActivated` — so nothing depended on state persisting across frames.

Verified three ways: **156/156 clean** afterwards; all four control captures
**byte-identical** to before, so the leak closed without changing any demo's own
rendering; and the cost is **below the noise floor** — 0.064 s of mean
difference over 3000 frames against a run-to-run spread of 5.71 to 7.21 s.

### 2026-08-17 (a hash that overflowed, and the loop the compiler deleted because of it)

**Release could not start a single demo.** Every one of them died before its
first frame, with the process reaching about **17 GB** and being OOM killed.
Debug was completely unaffected: 92 MB peak, 2.35 s. Since the day-to-day work
happens in Debug and "verify all three configs" had been read as *build* all
three, this had been sitting behind a green build.

The cause was `SpawnRocks` running a loop bounded by
`static constexpr int s_RockCount = 16` around **99,482 times**, allocating a
rock mesh and a rigid body each pass. It took every demo down rather than just
OpenWorld because `OnAttach` is deliberately unguarded, so OpenWorld builds its
world whichever demo you asked for.

The measurement that made it undeniable, and that pointed away from every
theory I had: an explicit `if (i >= s_RockCount) break;` placed as the **first
statement of the loop body never fired**, while the next line logged
`i=99481 of 16`. A hand-written bound check and a print cannot disagree, so the
check was not in the binary — and it wasn't. The loop's back edge was an
unconditional `jmp` with no comparison near it.

The UB behind it was in `Hash2D`:

```cpp
h ^= (uint32_t)(x * 374761393);     // multiply in int, then cast -- overflows
h ^= (uint32_t)x * 374761393u;      // cast, then multiply -- defined wraparound
```

The cast was outside the multiply, so the multiply happened in `int` and
overflowed for any `|x|` above 5. GCC 16.1.1 reasoned exactly as it is entitled
to: a loop calling that hash cannot legally reach `i=6`, so `i < 16` is not what
ends it, so the test is dead. Removing it is a valid consequence of a program
that was already illegal. **This is the plain form of "UB is not a wrong
answer, it is a licence"** — the damage was nowhere near the arithmetic, and the
symptom was a memory blowup in a loop whose bound was a compile-time constant.

Two copies of the idiom had the fault, in `OpenWorld.h` and `VoxelTerrain.h`.
`Terrain::Hash`, which they were cloned from, always had the cast in the right
place.

**The fix does not move the terrain**, and that is checked rather than assumed:
two's-complement wraparound and unsigned multiply produce the same bits, and the
chunk cache — whose key is a fingerprint over 512 density samples — still
reported *"364 chunks already stored"* instead of rebuilding. All three configs
now capture the same OpenWorld frame **byte-identically** (`e75c6ba4…`), at
about 90 MB each.

On the tools: `-D_GLIBCXX_ASSERTIONS`, `-fstack-protector-all` and
`-Wall -Wextra -Waggressive-loop-optimizations -Warray-bounds=2` were **all
silent** through the entire runaway. UBSan named the file and line on its first
run. Hours went into gdb backtraces, disassembly and an optimisation-level
bisect before that, and none of it was necessary. Reach for the sanitizer early.
Six demos now run UBSan-clean.

One self-inflicted lesson recorded beside it: the per-iteration logging used to
catch the loop wrote a 1.6 GB file into `/tmp`, which is a **tmpfs** here. `Shmem`
reached 8 GB, `bash` could no longer fork, and `cc1plus` started failing with
*"Disk quota exceeded"*. Two symptoms, one cause, and neither of them the bug
being chased.

### 2026-08-16 (a camera that does not report every step, and a body that moves like one)

**The third-person camera follows a focus point, not the player.** There is a
dead zone the player can move inside without it noticing, and beyond that it
closes the gap on a time constant rather than instantly.

The vertical dead zone is deliberately much larger than the horizontal one --
0.95 m against 0.35. A jump is a metre up and a metre back down inside half a
second and should not move the camera at all; climbing a dune is the same metre
held for several seconds and should. **The two are told apart by how long the
offset lasts, not by asking whether the player is jumping**: no flag to get
wrong, and falling off a ledge is handled without a second rule.

Measured: a 1 m jump moves the camera **0.0055 m**. A 3 m climb moves it
**1.505 m**, against a predicted 1.500 -- climbing at a steady rate the focus
settles where its catch-up equals the climb, so it lags by
`deadZone + lag * rate` and moves by the rest. The prediction is the reason the
lag was tightened from 0.90 s to 0.55: at the old value the camera trailed
1.85 m behind during a climb.

**Turning eases instead of running at a fixed rate.** A constant rate starts and
stops dead, which is what made it look mechanical -- a person accelerates into a
turn and coasts out of it. Taking a fixed fraction of the remaining angle each
second gives the coast for free, with a cap so the start is not a snap. A half
turn now takes **0.95 s**.

**An empty hand hangs.** Sticking one out in front is what a person does when
they are holding something and looks like sleepwalking when they are not, so the
reach only happens on the carrying side and only while it is carrying. The other
arm swings with the opposite leg.

**Shoulders in**, from 0.20 to 0.165 half-width, with the torso's top ring and
the shoulder joints narrowed to match. They were broad.

Six checks and one real bug found by looking at a capture: `m_CameraFocus`
started at the world origin, so with third person on from the first frame the
branch that syncs it never ran and the camera opened out at sea. It is seeded
from the player at spawn now.

**A second thing the capture caught was the test, not the code.** Three attempts
at a third-person screenshot came back underwater, and the cause was the
temporary feel-test teleporting the walker to a "home" of (0, 10, 0) -- the world
origin, which is open water -- and restoring it there rather than to where it
had been. A test that moves the world has to put it back where it found it.


### 2026-08-16 (a head that turns before the feet do)

The body was driven straight off the camera yaw, so the feet spun on the spot
the instant the mouse moved and everything hung off them -- hands, held tools --
spun too. The body now has **its own facing**, and three rules decide it.

**Moving:** the feet go where you are going. The body turns toward the direction
of travel, which is what makes walking sideways-to-camera in third person turn
the character around rather than crab-walk them.

**Standing, first person:** the head is free inside a cone. Past 70 degrees --
about the limit of a comfortable glance -- the body is *dragged* so the neck
never exceeds it. Past a smaller 40-degree comfort angle it eases round at 90
degrees a second, which is the step you take when you have been peering over
your shoulder too long.

**Standing, third person:** nothing. The camera orbits a body that stays put,
because looking at your character is not your character turning.

Seven checks, four mutations, all caught.

**One check was wrong and the code was right.** Holding a 55-degree look, the
body eased 0 to 15 degrees and stopped -- and 55 - 15 is exactly the comfort
angle. It turns until it is *comfortable*, not until it is square to the camera,
which is what a person does: you rotate enough to stop craning and no further.
The test now says that.

Held things moved to the **body's frame** as part of this. A hand is on the end
of an arm and an arm is attached to a shoulder, so turning your head no longer
carries what you are holding around with it. Only pitch still comes from the
camera, because raising a tool to look at it is something a person does.

**Not fixed: the holes in the ground and the dark lines beside them.** Both are
the LOD seam. The transition-cell work that would close it was attempted and
reverted a few entries ago, and marching a shovel ray past the sparse-air
sentinel does not touch it. It is the oldest outstanding defect in this demo and
it needs the transvoxel piece finished.


### 2026-08-16 (a whole body, and a camera that steps back from it)

The body was boxes and half a person. It is now a **whole figure built from
elliptical tubes**: a torso that widens at the chest and draws in at the waist,
tapered limbs, ovoid head, hands and feet, and blobs at the shoulders and knees
so a joint is round rather than a seam between two tubes.

Elliptical rather than round is most of what stops it reading as plumbing -- a
chest is not a cylinder and neither is a shin. One generator does all of it: a
tube through a stack of rings, each with its own two radii. A limb is two rings
and a taper, a torso is five, a head is an ovoid, which is the same generator
with rings following a sine.

**A first/third person toggle.** Third person is the camera stepping back from
the head, not a different controller. That works because everything hung off the
body -- the hands, the carry point, the aim -- was moved to measure from
`HeadPosition()` rather than from the camera. Pulling the camera back would
otherwise have dragged the player's hands three metres across the island with
it.

The head is skipped in first person, because the camera is inside it and with
back-face culling off its interior would fill the screen.

**Which is exactly what the torso then did.** The first torso was the right
shape and half a metre too tall -- 0.72 m hip to shoulder against a real 0.45 --
so its chest sat *above* the eyes and first person was a wall of navy blue. A
person's eyes are about 0.22 m above the shoulders and 0.67 above the hips, and
a mesh has to agree with that or the camera ends up inside it. Caught by looking
at the capture, which is the only reason the numbers got checked.

Still rough, and worth naming rather than leaving to be discovered: the legs are
a little long against the torso, the arms are thin, and there is no lean or
head-turn -- the figure faces its yaw squarely whatever the camera is doing.


### 2026-08-16 (a grip, a swing, and the shove that was not what it looked like)

**Held things are posed now.** A tool kept the orientation it was lying in, so a
0.42 m shaft stayed horizontal in the hand. Tool meshes are built with the shaft
along +y precisely so one rotation poses any of them, and it is slerped in over
the reach so the tool turns in the hand rather than snapping upright.

**Picking up is a reach.** The object travels from where it was lying to the
hand over a fifth of a second, eased, and the arm is drawn to the same point --
so the arm reaches down, takes it, and brings it up, because the hand *is* the
carry point.

**Using it is a stroke.** One damped sine gives the whole swing: `arc` runs
0 -> -1 -> 0 -> +1 -> 0, where negative is the wind-up back over the shoulder
and positive is the strike forward and down. It ends where it started, so
nothing has to be reset. The stroke plays whether or not it connects -- a swing
that only animates on a hit tells the player what the game found before they
have finished swinging.

**And the shove, which took three attempts and was worth all of them.**

The first guess was the orientation: a horizontal shaft reaches back into the
walker's capsule. Plausible, and the test said **0.0000 m** of drift both before
and after -- proving nothing, because that arrangement happened not to overlap.

So the mechanism was tested directly instead, by putting the held body *inside*
the player. That threw them **80.7 m in five seconds**. Kinematic bodies shove
dynamic ones and are not shoved back, and overlap is not a nudge.

Shrinking the collider to a point while held only got it to 13.2 m -- a tiny box
inside a capsule still makes a contact, and a kinematic body still wins it. What
was actually meant was "this object does not collide with the one carrying it",
so `RigidBody3D` gained **`IgnoreCollisionWith`**, one handle, checked beside the
existing joint suppression -- which is the same idea: two bodies meant to occupy
the same space, where a contact is an artefact of how they are attached rather
than information about the world. A single handle rather than layers or a mask,
because one pair is the case that exists.

That took it to **0.25 m**, and the held object still collides with everything
else, so a falling rock can still knock one out of your hand.

The last measurement needed a control. Holding drifted 1.18 m over five seconds,
which looks like a residual until you measure the same disturbed state with
*empty hands* and get **3.90 m** -- the walker settling on a slope after being
teleported, nothing to do with what it was holding. A number is not a residual
until something says what it should have been.


### 2026-08-16 (rocks that cleave, and a body to look down at)

**The split was wrong and is now a split.** A rock used to break into eight
equal octants, which does not read as a rock breaking -- it reads as a rock
being replaced by eight smaller rocks. It now **cleaves**: one axis-aligned
plane through the longest axis at a hashed fraction, into two unequal pieces
that came from it and still fit together.

The *mesh* is cut by the same plane and the hole is capped, which is the part
that makes the pieces look like halves of something. Verified against the thing
that matters:

```
volume 1.08451 -> 0.64779 + 0.43672
mass   271.128 -> 161.949 + 109.180
health 976.062 -> 583.015 + 393.047
shape volume 0.41859 -> 0.27517 + 0.14343   (0.00%)
```

The last line is the strongest: the two cut meshes together are exactly the mesh
that was cut, nothing lost and nothing duplicated. Both pieces come back with
**zero open edges**, so the cap closed them -- an uncapped cut would look right
from outside and be a shell.

Conservation survives the change of scheme because it never depended on the
scheme: f and 1-f sum to one however the plane falls, the same way eight eighths
did. The pieces are deliberately unequal, measured at 1.48x, because equal parts
is the thing this replaced.

The cap is a fan from the centroid of the cut rim, which is valid only while the
cross-section is a simple polygon. That holds for these blobs because each is
star-shaped about its own centre. It is not a general mesh boolean and the
comment says so.

**A first-person body.** A *viewmodel*, not a ragdoll: the Ragdoll demo's
thirteen jointed bodies balance because they are simulated, and driving them
from a kinematic capsule would put two things in charge of where the player is.
What is borrowed is the proportions. Hips, thighs, shins, boots, arms and hands,
posed procedurally -- and nothing above the ribs, because from inside your own
eyes there is no head to draw.

The gait is paced by **distance covered rather than time**, so the legs do not
scissor on the spot when you stop, and the knee bends only on the leg swinging
forward, which is the difference between walking and marching.

The piece that makes it more than decoration: the hand *is* the carry point.
`CarryPoint()` returns where the hand is, so the tool being drawn and the tool
being simulated are the same object in the same place and cannot drift apart.

Still rough: at a level pitch you see almost none of it, which is correct --
your own hips are under the camera and outside the frustum -- but it means the
body only reads when you look down. Real viewmodels cheat the limbs forward for
exactly this reason, and this one does not yet.


### 2026-08-16 (tools you carry, and rocks that break on impact)

**Rocks have hit points now, and damage arrives as an impulse.** A swing, a
fall, and one rock thrown at another all go through the same path, so there is
one rule for what breaks a rock rather than two that can disagree. Below the
threshold -- 40 N.s, about a 20 kg rock landing at 2 m/s -- a knock is a knock.

Toughness is **toughness times volume**, which is what makes health an attribute
that *splits* rather than one that is re-rolled: eight octants at an eighth of
the volume carry an eighth of the health each, and the total across the pieces
is exactly what the parent had. Measured **976.062 to 976.062**, the same
arithmetic that already conserved volume and mass.

That also gives swings-to-break a closed form. A rock of 1.0845 m^3 has 976.1
health and a swing does 260 - 40 = 220, so `ceil(976.1 / 220)` = 5 swings.
Measured: 5.

**Three tools, and no inventory.** A tool is a physics object lying on the
ground. You carry one, and to use another you put this one down -- the
constraint is physical, so it needs no UI, no slots and no rules beyond "your
hands are full". A rock counts as a full hand too.

The pickaxe damages rock, the axe cuts trees, the shovel moves ground, and each
only *sees* what it is for: an axe swung at a boulder finds nothing rather than
finding it and doing nothing. Bare hands do nothing at all, which is the point
of having tools.

Digging writes the edited chunks **back to the cache**. Without that the next
run would serve the pre-dig chunk as a hit and the hole would quietly heal --
the caveat flagged when the cache was built, now closed.

Fifteen checks, four mutations, all caught.

**The shovel found a real engine bug.** `VoxelField3D::Raycast` sphere-traces,
and an unallocated chunk reads `Far` -- a thousand metres. Tracing believes it
and steps clean out of the world. Measured: a ray started 3 m above the island
read **1000 at its origin and 1.86 one metre lower**, and hit nothing. The
algorithm's own comment warns about exactly this ("with a value that overstates
how far the surface is, a step jumps straight over it"); the sparse sentinel is
the worst possible overstatement.

Two attempted fixes failed and were instructive. Skipping a whole chunk on
seeing the sentinel overshot -- it put the ray 5 m under a surface 3 m down, and
tracing from inside rock finds nothing. Skipping a voxel at a time got past the
origin and still missed, because interpolation near the boundary of an allocated
chunk mixes in the neighbour's 1000. The dig now **marches** at half a voxel:
eighteen samples over a 4.5 m reach, immune to the sentinel, and -- unlike
marching the analytic density -- it sees holes dug earlier.

The proper fix belongs in `Raycast`, which can ask the field whether a chunk is
uniform instead of inferring it from a magic number. Not done here.

**Not done: the first-person body.** The ragdoll rig for visible arms, hands and
legs is the largest piece of the request and is untouched -- it wants its own
pass rather than a rushed one at the end of this.


### 2026-08-16 (waves, and a sea that turns you back)

Three travelling sines summed, displacing a tessellated water grid in the vertex
shader. Not an ocean simulation and not trying to be -- a spectrum done properly
is an FFT a frame, and what this needs is a surface that moves, that catches the
light, and that the player can float on.

What makes it worth doing this way rather than with a scrolling texture is that
the height is an **analytic function of position and time**. The shader displaces
the mesh with it, the CPU evaluates the same thing for buoyancy, and the normal
comes from the derivative of the same sum rather than from differencing
neighbours. **It is written twice**, once in GLSL and once in C++, and nothing
enforces that the two agree -- that is the real cost of the approach and it is
stated in the code rather than hidden.

Wave time comes off the **fixed clock**. A surface animated from wall-clock time
would make the demo unable to reproduce itself, which is the one thing this
project will not trade.

**The boundary.** Past 165 m the sea simply carries you back, with a current that
strengthens the further out you get. A wall would do the job and would announce
that the world stops here; a current says the same thing with nothing to bump
into, and it is about ten lines. Checked from four directions: all four return.

Nine checks, and the wave arithmetic came out exact:

```
peak |height| 0.8099 m against the amplitude sum 0.8100, mean 0.000008
```

The mean matters as much as the peak -- waves that averaged above zero would be
a sea level quietly rising.

**The float does not track the surface, and should not.** The measured lag is
0.585 m, and the arithmetic says why: the surface's peak vertical speed is the
sum of amplitude times angular speed, 0.42(4.1) + 0.26(3.2) + 0.13(2.4) = **2.87
m/s**, against a swimmer capped at 2.2 m/s. It physically cannot keep up with the
fastest part of a crest. The bound was therefore set from the model rather than
guessed: it must not fall a whole wave behind, so the limit is the amplitude sum.

Two test failures on the way, both mine. The float test first placed the player
at (60, 60), which is 85 m out and lands *inside* the island ring -- it measured
a man standing on sand and reported no bobbing. And a first tolerance of 0.5 m
was a number picked rather than derived.

**One mutation survived and found a real hole.** Making the demo hand the
controller a flat sea level left all eight checks passing, because the float loop
sets the water level *itself* -- it proved the buoyancy and the wave function work
as a pair, and never that the demo wires one to the other. One step through the
demo's own update closes it, and the mutation now fails.


### 2026-08-16 (leaves)

One cluster of foliage per terminal branch. The clusters are the same jittered
blob the rocks are made of, smaller and coarser -- deliberately not leaf-shaped,
because at the size these are drawn a cluster reads as foliage and a hundred
individual leaves would be a hundred times the triangles to say the same thing.

Hanging them on the *tips* gives the count another closed form to be checked
against: a complete c-ary tree of depth d has exactly **c^d** terminal branches,
so 3^4 = 81 clusters. Measured 81. The generator counts nothing.

Bark and foliage come out as **separate meshes** rather than one, because they
are two colours and the leaves are the half worth being able to switch off when
counting triangles. That is a checkbox in the panel.

```
1452 bark tris + 2430 leaf tris = 3882 a tree, 38,820 for ten
leaves span y 2.26 to 8.28, trunk alone is 2.60
```

Seven checks. Placement is checked as well as count, because a cluster count
alone would pass just as happily with all the foliage bunched at the foot of the
trunk: nothing below half the trunk's height, and the canopy above the last
branch.

Three mutations, all caught -- foliage on every branch rather than the tips,
clusters hung at the base, and the cluster radius collapsed to zero.

**A weakness carried over from the tree work, restated because it still
applies.** "A different seed grows a different canopy" cannot fail while the
leaf *positions* depend on branch tips, which already depend on the seed. It
would keep passing if the cluster's own jitter stopped reading the seed
entirely. Catching that needs the jitter varied on its own.


### 2026-08-16 (felling trees)

Trees now have a body, which is two features at once: a standing tree is a
**static** collider you walk into rather than through, and felling it is a
**change of body type** on that same collider. Nothing is created or destroyed
-- which matters, because `PhysicsWorld3D` has no way to remove a body. The test
checks that specifically: felling adds zero bodies.

Four swings of the pickaxe. A swing goes to whichever of a rock or a standing
tree is better lined up rather than preferring one kind, so aiming at a trunk
with a boulder off to the side hits the trunk. A felled trunk stops being a
target.

**The capsule rolled.** A trunk is obviously a capsule, so it was one -- and it
behaved exactly like the spherical rocks did two entries ago: felled, it rolled
**6.71 m and was still moving after fifteen seconds**, because a round collider
on a slope has no rolling resistance for the solver to spend. A box trunk is
invisible at this scale and lies where it falls: drift down to **1.75 m**.

The measurement that caught it is the one that looked least likely to. "Falls
away from the player" was 0.27 with the capsule and is **0.98** with the box --
it was not measuring a bad push direction at all, it was measuring the tree
rolling downhill afterwards and taking the drift with it. A check aimed at one
thing found another.

Fifteen checks. Four mutations, all caught once aimed properly: never becoming
dynamic, the hit count ignored, the push reversed, and felled trunks staying
targets.

**One mutation was wrong before it was informative.** `body.Type =
BodyType::Dynamic;` appears twice -- once where a carried rock is released and
once where a tree is felled -- and a first-occurrence replace hit the rock. It
reported 15/15 and had mutated a code path the suite does not exercise. Retried
against unique context it fails 2. A mutation that survives is worth one look at
*what it actually changed* before it is worth any conclusions.


### 2026-08-16 (trees, trunk and branches)

A trunk that splits into branches that split into branches: each segment is a
tapered prism, each tip spawns three children leaning off it, shorter and
thinner by a fixed ratio, four generations deep. No leaves yet -- this is the
skeleton.

The ratios are the whole model, and choosing constant ones is what gives the
generator **closed forms to be held to**. A complete c-ary tree of depth d has
`(c^(d+1) - 1) / (c - 1)` nodes, and the height cannot exceed the geometric
series `L(1 + r + ... + r^d)` -- which would need every branch pointing straight
up. Neither is computed anywhere in the generator:

```
1452 triangles = 121 segments, closed form says 121
height 7.437 m, series bound 7.781 m, trunk alone 2.600 m
```

Ten checks. Placement uses the same height-and-slope gate the shader shades
grass with, so no tree stands on the beach or on a dune face -- the same number
deciding the ground is green decides a tree can grow on it.

Four mutations. Three caught: one child too few (31 segments against 121),
leaves emitting no segment (40 against 121), and the length taper removed --
which put the height at **12.394 m** against a 7.781 m bound, so that check has
real teeth rather than being a formality.

**The fourth was not caught, and the reason is worth keeping.** Replacing the
seed with a constant in the branch *azimuth* left "a different seed grows a
different tree" passing, because the seed is also used in the branch *lean*, so
two seeds still diverge. The check proves *some* seed dependence, not that every
seeded parameter is wired to the seed. A per-parameter check would need to vary
one at a time; this one cannot tell you which half is dead.


### 2026-08-16 (breaking rocks)

Left mouse swings a pickaxe at a rock **too big to lift**. The two rules are
deliberately complementary and read off the same number -- the collider's own
half-extents -- so there is never a rock that is neither liftable nor breakable.
Bigger rocks take more swings, from that same number.

A broken rock becomes **eight octants, each exactly half its extents in every
axis**. Halving all three is what makes the split conserve volume *exactly*:
eight pieces at an eighth each. That is a number a test can hold the code to,
which a scatter of plausible-looking rubble would not be. Measured across a
break: volume **1.084514 to 1.084514**, mass **271.1284 to 271.1284**.

The parent body is **reused as the first fragment** rather than retired, because
`PhysicsWorld3D` has no way to remove a body -- handles are indices into it.
Parking dead bodies below the world would have worked and would have grown the
solver's cost with every swing. So a break adds seven bodies, not eight, and the
test checks that specifically.

The halving also makes the two mechanics meet: a boulder is two or three swings
away from pieces small enough to carry.

Fourteen checks and five mutations, all caught -- fragments not halved, mass not
divided, the liftable filter dropped, the hit count ignored, and the parent not
reused.

**Both first-run failures were the test, not the code.** "A small rock cannot be
hit" failed because the pickaxe correctly chose a *different*, better-aligned
boulder standing behind it -- the check should have been on which rock was
selected, not on whether the swing happened at all. And "every piece settles on
the terrain" failed at 4 of 7 because eight fragments from one rock land in a
heap, and a piece resting on another piece is correctly higher than the ground
beneath it. Both checks now say what they meant: the small rock is never the
*target*, and no piece has sunk *through* the ground.


### 2026-08-16 (picking up rocks)

**E** picks up the small rock you are looking at and **E** puts it down. A held
rock is **kinematic** -- moved by whoever sets its position and by nothing else.
Static would also stop the solver throwing it about, but a static body is
scenery: it would not shove the rocks it is dragged through, and shoving one
boulder with another is most of the point of being able to lift one.

Position is driven directly rather than by a velocity chasing a target, which
either lags or overshoots. The velocity it *would* have had is tracked
alongside and handed back on release, so letting go throws rather than drops --
clamped, because the throw should come from the player moving and not from how
sharply they flicked the mouse on the frame they released.

Whether a rock is liftable is read off **the collider's own half-extents**
rather than a separate flag, so it cannot come to disagree with the rock it
describes.

**The first version worked and was useless: 1 of 16 rocks was liftable.** Radii
were drawn uniformly from 0.35 to 1.15, so almost nothing landed under the
limit -- a mechanic you would essentially never get to use. Squaring the draw
biases the scatter toward small rocks with a few big ones, which is both a
usable balance and closer to what a beach looks like: **6 of 16** now.

Nine checks and four mutations, all caught: the size gate removed, the reach
gate removed, release leaving the rock kinematic, and the carry never moving it.
The drop is checked against the same predictor the settling test used -- a box
at rest has its centre one half-height above the surface, and the surface comes
from the density function the solver never reads. A released rock came to rest
**0.002 m** from it.

Two of the checks are weaker than they look and are worth naming: the carry
tracks its target to 0.000000 m because the position is *assigned*, and the
walker moved 0.0000 m while carrying because it was already at rest. They catch
a carry that does nothing (mutation four) and little else.


### 2026-08-16 (the transition cell, and the case it grew)

The coarse side now subdivides its seam face to the fine neighbour's
resolution, as a fan of tetrahedra from the cell's centre over its own
triangulated boundary. That is the half value-matching could not do, and it is
still not enough:

| seam | holes before | with transition cells |
|---|---|---|
| 1:4 | 296 | 246 |
| 1:2 | 362 | **368** |

1:2 got *worse*, which is the tell. Subdividing the seam face into sixteen
sub-quads while leaving the four side faces as single coarse quads means that
along the edge where they meet there are several sub-edges on one side and one
on the other. **The fan's own boundary has T-junctions and is not a closed
surface**, so the tetrahedra do not tile the cell and the construction opens new
holes inside it. At 1:2 it opens more than it closes.

Finishing it means subdividing per *edge* rather than per face, and triangulating
each face to respect which of its edges are split -- which cascades into the
neighbouring cells, because a face shared with a plain coarse cell must stay
coarse while a face shared with another transition cell must not. That is
precisely the geometry Lengyel's tables encode, and it is the reason the
algorithm has the shape it does rather than being a few lines of subdivision.

Stopped here rather than keep adding a case per iteration to a construction that
was gaining one each time, and **the transition code is reverted** -- code that
does not work is worse kept than the measurement that says why. The finding
stays: the next attempt starts from the T-junction on the cell's own boundary,
and the harness that found it reproduces the bug before claiming any fix.

**What is verified and stands:** the tetrahedron primitive itself -- sixteen
unambiguous cases derived at runtime with no table, six tetrahedra tiling a cell
to 1.000000000, a sphere within 0.04% of `4*pi*r^2` and 0.09% of
`(4/3)*pi*r^3`, and every one of its 96,174 edges used by exactly two triangles.

### 2026-08-16 (the seam closes geometrically, not topologically)

The transition work reached a real result and stopped short of the goal, so
both halves are worth writing down.

**The construction.** Two chunks join watertightly if they agree on the shared
face's triangulation *and* its field values. The tetrahedral decomposition gives
the first for free: it is translation-invariant, so every face everywhere splits
on the same diagonal (derived, not assumed -- face x=1 is always triangles
{1,5,6} and {1,2,6}). For the second, the fine chunk takes its face samples by
interpolating linearly over the *coarse* triangles. A linear field on a triangle
has exactly one contour, a straight segment, so both sides compute the identical
line. The coarse quad's diagonal passes exactly through fine lattice points, so
no fine triangle ever straddles the two linear pieces.

**And that is not enough.** Reconciled, a stride-1/stride-4 seam through a
sphere went from **296 edges used once to 274** -- a 7% improvement, not a fix.
The argument was right as far as it went and wrong about what it bought: the two
contours are now the same *line*, but the fine side subdivides that line into
several collinear segments, one per fine sub-edge it crosses, while the coarse
side has one. The intermediate points are **T-junctions** -- vertices on one
side with no counterpart on the other.

That is geometric coincidence without topological agreement. There is no visible
crack, which is what the eye wants, but it is not a closed manifold, and the
edge-use count is right to refuse it.

**What is left is the part that earns the name.** The coarse side has to
subdivide its seam face to the fine resolution, so those intermediate vertices
exist on both sides -- which is precisely what a transvoxel transition cell is
for, and precisely the piece not yet built. Reconciling face *values* was the
cheap half; reconciling face *topology* is the half with the geometry in it.

The test already reproduces the bug before claiming the fix: it meshes the seam
unreconciled first and asserts holes are present (296), so it is known to be
capable of failing before it is trusted to pass. It also caught 15 edges used
more than twice, which the T-junction story does not obviously explain and which
wants looking at.

### 2026-08-16 (a primitive with no ambiguous cases)

Transvoxel is the right fix for the LOD seam, and its core is a 512-entry
transition-cell table. Deriving that rather than sourcing it means needing a
primitive whose cases can be *enumerated and checked* instead of trusted, and
the cube is not it: 256 cases, several genuinely ambiguous, where two
neighbouring cells resolving a face differently leave a hole.

A tetrahedron has four corners, so **sixteen cases, none ambiguous**. Three
corners on one side always give one triangle; two and two always give one quad;
there is no configuration that could be drawn two ways. Sixteen is few enough
that the polygon is derived from the sign pattern at runtime -- there is no
table, so there is no table to get wrong. And two tetrahedra sharing a face
agree on that face exactly, because a triangle's three corners admit only one
contour: a straight segment between two edge crossings. Cracks become impossible
by construction rather than by careful table design.

The cell decomposition is derived rather than copied: the six tetrahedra are the
six **monotone paths** along the cell's main diagonal -- at each step advance in
x, y or z, and the 3! orders of spending those steps are the six. Every cell
uses the same one, so two cells meeting at a face divide it with the same
diagonal.

Checked against things the code does not contain. The six tetrahedra sum to
**1.000000000** of the cell. All fourteen mixed cases emit a surface and both
uniform cases emit nothing. A radius-6 sphere came out **0.04%** off `4*pi*r^2`
and **0.09%** off `(4/3)*pi*r^3`, and -- the check the whole exercise is for --
every one of its 96,174 edges is used by exactly two triangles. No holes.
`3F = 2E` holds exactly at 192,348.

Three mutations, all caught: a wrong corner in the tetrahedron table, the
orientation flip removed, and a genuine bowtie in the quad. A fourth was written
and turned out to be a **no-op** -- reversing a triangle's winding at the call
site changes nothing, because `Emit` re-orients every triangle against a
direction computed from the corners themselves. Worth knowing rather than
worth counting.

The bowtie is the instructive one: area and volume came out **identical** to the
correct mesh, 452.19 and 903.99, because it is the same four points connected
wrongly. Only the edge-use count saw it. Area and volume cannot detect a
topology error, which is precisely the error a seam is made of.

**Not yet built: the transition slab itself.** This is the primitive it will be
made from, verified, and until that lands it is a component with no system --
which this project normally declines. It is here on the understanding that the
next piece consumes it.


### 2026-08-16 (blades of grass, and a skirt that was the wrong fix)

**The skirt does not fix the LOD seam, measured.** It was built on the
assumption that the seam is a crack; it is not. A coarse chunk meshes
systematically *lower* than its fine neighbour, so what you get is a solid step
whose wall you can see into -- and a skirt has no gap to fill.

The mechanism itself works: 582 triangles a chunk became 718, so 68 boundary
edges were found and walled. The picture moved by **2 pixels**, and that was
with the LOD bands forced to 10 m and 20 m to put mismatched chunks directly
under the camera. 23% more triangles for two pixels, so it now defaults off.
Kept, because it is the right mechanism for an actual crack and because the
negative result is worth more than the code. The real fix for a step is
transvoxel transition cells, which reconcile the two lattices rather than
hanging a curtain off one of them.

Disabling LOD is not the alternative either -- at full stream it was worth 9x
the triangles.

**Grass, as geometry.** One triangle a blade -- a base edge across the slope and
a point above it. A quad is two triangles for a shape nobody can tell apart at
this size, and grass is the one thing here where the count *is* the cost.

Blades are scattered on the chunk's own triangles rather than on a grid, so they
follow the ground exactly and inherit the mesh's density: more grass where the
surface is busier, which is also where it looks right. Placement is uniform
within each triangle (the `sqrt` on the barycentric is what stops them bunching
along one edge), and fractional density is honest -- the whole part is a
guaranteed count and the remainder is a threshold, so 0.6 gives six blades every
ten triangles instead of none.

They are gated on the same height-and-slope test the shader shades with, so a
blade never stands on bare sand or on a face too steep to be green. And only
**stride-1 chunks** get grass, which is not a special case: a stride-2 chunk is
already the renderer saying this is far enough away to halve its detail, and
grass is the first thing that should go. 21 chunks of grass inside a 52,349
triangle frame.

**Rocks smoothed** from a 9x6 lattice jittered 0.68-1.0 to a 16x10 one jittered
0.84-1.0 -- more facets, shallower dents. The ceiling stays at 1.0 so the mesh
still cannot leave the box that collides for it.

### 2026-08-15 (grass, and rocks that would not stay put)

**Grass by elevation and slope.** Height decides where it starts, slope decides
whether it can hold on — grass on a near-vertical face looks painted on, and the
dunes are steep enough at their edges for that to show. Sand at the waterline,
green over the crown, `smoothstep` between. Gated on a `u_Terrain` uniform so the
water and the rocks, which share the shader, do not sprout grass wherever they
happen to sit above the line.

The first attempt rendered the whole world **white**. `flat` is a GLSL
interpolation qualifier, so `float flat = smoothstep(...)` is a syntax error; the
shader failed to compile, and the engine logged it and carried on with an
unusable program. The log said `unexpected FLAT` immediately. The picture said
"white", which is much harder to act on — when a shader change produces something
inexplicable, read the log before reading the frame.

**Rocks, and the shape that made them work.** The obvious version — sphere
collider, coarse sphere mesh — matched perfectly and behaved terribly. A rock
landed exactly where predicted, at `Height + radius`, then crept downhill:
3.69 → 3.66 → 3.58 → 3.10, and by step 599 it was at −18.8 m doing −17.6 m/s.

Nothing was wrong with the collision. **A sphere on a slope rolls**, this island
is a dome, so every rock rolled down the beach, into the sea, down the seabed,
and eventually far enough inside the field that the narrowphase stopped pushing
it out. Real rocks do not roll away because real rocks are not spheres, and a
rigid-body solver has no rolling resistance to stand in for that. Boxes rest on a
face: 16 rocks, worst `|centre − (Height + half)|` of **0.148 m** — inside one
0.5 m voxel — and worst drift **0.249 m** over 15 s, against 3 checks.

Then the boxes looked like crates, because what was drawn *was* the collider.
They are now flat-shaded jittered blobs inscribed in the box, radius 0.68–1.0 in
the box's own units, so the mesh can only ever be inside what it collides with —
which reads as a rock half-buried rather than one floating. Flat normals are the
point: a smooth rock under cel banding is a soft gradient with a couple of bands
across it, while a faceted one is a set of plates each holding a single shade.

### 2026-08-15 (the water was culled, and underwater looked like dry land)

Reported from a screenshot: "I am swimming in this picture. The water texture is
on the ground. The actual water is invisible." Two separate causes, and the first
is a render-state leak I introduced.

**`CullFace` is global and outlives the demo that set it.** `CelShading` turns on
front-face culling for its inverted-hull outline pass and restored it to
**`Back`** afterwards -- but the engine's default is `None`; `Init` never enables
`GL_CULL_FACE` at all. So selecting the Cel demo, then switching to Open World,
left back-face culling on for a demo that had never asked for it. The water is a
single-sided quad facing +Y, so from underneath it is a back face and disappears
exactly when the camera goes under it. `Cube3D` restores to `None` and always
did; this did not. `VoxelTerrain` leaves it on `Back` too, which is the same
latent bug in a demo that happens not to care.

Fixed twice over: the Cel demo restores the default, and Open World now *sets*
the cull mode it wants at the top of its draw rather than inheriting whatever was
left behind. Setting state is cheap; depending on state you did not set is a bug
waiting for a particular order of clicks.

**And being underwater looked exactly like being on dry sand**, because nothing
changed when the camera crossed the surface. The sky stayed sky-blue, the sand
stayed sand, and the only blue left in frame was the distant sea seen edge-on --
which is precisely "the water texture is on the ground". Submerged now clears to
the water colour instead of to sky, and everything fades toward it with distance
by Beer-Lambert, `1 - exp(-density * d)`, the same exponential an actual
attenuating medium follows. At 0.06 per metre that is about half gone by 12 m, so
the sea floor stays readable underfoot while the distance closes in.

Checked by spawning at sea with the float depth temporarily set below the eye,
since buoyancy otherwise puts the camera just above the surface within a few
hundred steps -- which is itself the design working. The above-water frame is
byte-identical to before the change, and the Cel demo's own capture still hashes
`56fa9457`, so neither fix moved anything it should not have.

### 2026-08-15 (the water was a sand shelf)

"I still can't see the water", after a changelog entry claiming it was fixed.
The entry was wrong, and the evidence was already in its own numbers: water
covered 12.7% of the frame at step 30, 12.4% at 90, 11.7% at 200 and 9.2% at
800. Monotonically shrinking — and every capture used to declare it fixed
stopped inside the first few hundred steps, while a person plays for minutes.
By step 6000 it was **4.7%**.

The cause was the flattening from the previous entry. `s_MaskToHeight` scales
the island mask, and **the mask is negative at sea** — so dropping it from 0.55
to 0.10 flattened the sea floor by exactly the same factor as the islands:

| past the shore | seabed before | after |
|---|---|---|
| 10 m | −5.5 m | −1.0 m |
| 20 m | −8 m (floor) | −2.0 m |
| 40 m | −8 m | −4.0 m |

Every island sat in an enormous shin-deep shelf that only reached the −8 m floor
about 80 m out. Bright sand under a thin film of 0.82-alpha water reads as more
beach, and as more shelf streamed in the visible sea shrank — which is exactly
the trend the early captures were showing and nobody read.

Land and seabed now scale separately: `s_MaskToHeight` = 0.10 inland,
`s_SeabedDrop` = 0.55 offshore, with a deliberate crease at the waterline
because a beach really does change slope where it enters the water. `Slope`
picks the same side, which is the second time that shared constant has earned
its keep. Water at step 6000 went 4.7% → **6.8%**, and the sea is now a deep
blue band rather than a pale shelf.

Checked from four yaws this time (6.7–10.8% of frame) rather than from the one
that happened to look right — the same mistake as declaring it fixed from a
step-300 capture, in the other axis.

The starting pitch also went from −12° to −2°. Twelve degrees down was composed
for standing on a mountain; on a flat beach it spends two thirds of the screen
on the sand at your feet.

### 2026-08-15 (sandy islands, visible water, and swimming)

**The water was never missing.** It measured `(82, 138, 178)` against a sky of
`(135, 173, 201)` — drawn, blended, and genuinely different, but reading as haze
because nothing in view gave it context. What actually hid it was the terrain:
islands whose centres stood `Radius * 0.55` ≈ 19–36 m above the sea, so the
player spawned on a mountain and every shoreline was over the horizon. Deepening
the water to `(0.06, 0.26, 0.40, 0.82)` helped; putting a beach in front of it is
what fixed it.

**Islands are now low and sandy.** Radius 22–40 m (was 35–65), the mask-to-height
scale down to **0.10** from 0.55, and relief noise from 4.0/1.6 to 0.9/0.35. A
30 m island now rises about 3 m rather than 19 m.

That scale existed as the literal `0.55` in **two** places — `Height`, and
`Slope`, which needs the same figure to report normals for the shape `Height`
actually builds. The comment in `Slope` even said "0.55 matches the scale Height
applies to the mask", which is a comment doing a constant's job: change one and
the terrain silently lights as though it were still the old shape. It is
`s_MaskToHeight` now, used by both.

**Sand albedo was computed, not picked.** The shader's brightest multiplier is
`u_Ambient + sun*sunColor + sky*skyColor*0.35` = `0.35 + 1.0 + 0.175` = **1.525**
on red, so any albedo over about 0.65 clips — and a clipped surface has no cel
bands left, because every level saturates to the same white. A first guess of
`(0.84, 0.76, 0.56)` measured `(255, 255, 213)`: two channels pinned. At
`(0.62, 0.56, 0.41)` the brightest band should land on `(241, 217, 156)`, and it
measures **(241, 217, 156)** exactly.

**Swimming.** The controller gained an opt-in water model — off unless a caller
sets `HasWater`, so a demo with no water does not have its jump quietly rerouted
through a buoyancy term. The rule that matters is the one distinguishing *wading*
from *swimming*: standing on the bottom in water shallower than 1.1 m is still
walking, and still jumping. Without that, ankle-deep surf would take control away
from the player.

Buoyancy is a spring toward a float depth rather than a snap to the surface, so
entering the water sinks and comes back up. The check is one the implementation
does not contain: the float term is zero at exactly one depth, so a body left
alone must settle with its feet `FloatDepth` under **wherever it started**.
Dropped from 6 m above the water it settles at **1.404 m**; released on the
seabed 8 m down it rises to **1.405 m** — the same equilibrium from opposite
directions.

Both are 0.054 m below the 1.35 m target, and that residual is arithmetic rather
than error: gravity adds `9.81 / 60` = 0.1635 m/s to the vertical velocity after
the controller assigns it, and the spring balances that at
`0.1635 / 3.0` = **0.0545 m** of extra depth.

9 checks. The first run failed with the body's feet exactly at `start − eyeHeight`
in both cases — the signature of a body that never moved, which it had not: the
test ran before `SpawnWalker`, so the handle was not a body yet. Three mutations
after that (no wade branch, buoyancy never applied, buoyancy sign flipped), all
caught.

**Two follow-ons.** The checker texture is off by default now the ground is meant
to read as sand rather than as a test surface — the toggle stays for the
textured-vs-untextured comparison. And flat sand shows the documented cross-stride
LOD seams that relief and a busy texture used to hide, so the bands moved out to
56 m and 104 m; the flatter world draws 41–54k triangles there against the
mountainous one's 90–110k at the *tighter* bands, so the wider bands cost nothing.

The terrain change also gave the chunk cache's fingerprint its first real test: it
invalidated the stale file by itself, with nothing to remember and nothing to
clear by hand.

### 2026-08-15 (cel-shaded terrain, and a chunk cache on disk)

**Cel shading on the terrain** is the Cel demo's quantiser, moved unchanged into
the OpenWorld sun shader — `floor` to a level, clamp the top, divide by
`bands - 1`. Both the sun term *and* the sky term are banded, which was not the
first attempt: banding only the sun leaves the sky gradient sliding smoothly
underneath the hard sun edges, and the result reads as a bug rather than as a
style. The flat regions have to agree with each other.

**No outline.** An inverted hull needs a closed mesh, and a chunk mesh is an
*open* surface that stops at the chunk boundary — an inflated copy would show
its back faces along every one of those edges, hundreds of them, instead of only
at the silhouette. Outlines on terrain want a depth-discontinuity pass, which is
a different piece of work and not this one.

**The chunk cache** answers "can chunks be kept between runs" with yes. The
world is procedural and deterministic, so nothing is *lost* by regenerating it —
the cache buys time, not data, and it is worth having because the cost is so
lopsided: producing a chunk is a density evaluation for each of its 4,096
voxels, and reading one back is a seek and a memcpy.

`VoxelField3D` gained `SaveChunk`/`LoadChunk` — bytes in, bytes out, per chunk,
with no opinion about where they are stored. Streaming wants one chunk at a
time, so a whole-field format that had to be read end to end before the first
chunk was usable would defeat the point. The demo owns the file: a header, then
append-only records, indexed on open by reading only the record headers and
seeking past each payload.

Measured over 600 steps at a 128 m radius: **6.21 s cold** (951 chunks generated
and written, 2.8 MB) against **4.16 s warm**, and the captured frame is
**byte-identical** between them — the cache reproduces the world exactly rather
than approximately. 2.8 MB for 951 chunks is under 3 KB each against a dense
chunk's 20 KB, because most chunks never come near the surface and collapse to
six bytes. The self-test saw 55 of 65 uniform.

**The dangerous failure is a stale cache, not a missing one.** Change the terrain
function, forget to clear the file, and the world silently comes back as the old
one while the code says otherwise — a whole session lost to a wrong assumption.
So it is not a version number somebody has to remember to bump: the cache is
keyed by a **fingerprint of the density function itself**, 512 fixed samples
hashed together with the lattice geometry. Change the islands, the noise, the sea
level or the voxel size and at least one sample moves, the fingerprint changes,
and the file is discarded. A fingerprint derived from what the function *does*
cannot drift out of step with it.

15 checks — every voxel compared bit-for-bit rather than with a tolerance, since
these bytes were memcpy'd and anything less than exact means the encoding lost
something. Five mutations, and **three survived the first pass**, all three
because of what the test set up rather than what it asserted:

- *Loading uniform over dense need not free the arrays* — invisible, because the
  test loaded into a **fresh** field where nothing was allocated to begin with.
- *The length check can be deleted* — invisible, because the truncated blob was a
  **uniform** chunk, six bytes cut to five, which the earlier `size < header`
  guard catches before the length check runs.
- *The write path can record the wrong offset* — invisible, because every read in
  the test went through a **reopened** cache, which rebuilds offsets by scanning
  the file. The writing instance's own bookkeeping was never read from.

Fixed by adding the three setups the checks needed: a reused field, a dense blob
one byte short (and one byte long), and a read back through the writing instance.
All five mutations caught.

### 2026-08-15 (a 128 m load radius, and the streaming order it exposed)

LOD made a bigger view affordable, so the load radius went from 64 m to 128 m to
find out what that costs. The rendering was never the problem:

| | Debug | Release |
|---|---|---|
| streaming, mean | 3.2 ms | 0.30 ms |
| streaming, worst | 10.6 ms | 1.87 ms |
| steps over 16.6 ms, out of 1800 | 0 | 0 |

Triangles stayed flat around 90k as the world grew, which is LOD doing its job —
without it the same view is 745k. A budget of 4 chunks a step is comfortable in
Release (1.1 ms mean, 6.3 ms worst) and **not** in Debug (11.9 ms mean, 31 ms
worst, 149 of 1200 steps over frame). It stays at 1; it is a slider.

**What the bigger radius actually exposed was the fill order.** `StreamAround`
walked `dz` then `dx` — scan-line order across the disc — so the far edge of the
first row arrived before the ground beside the player. Chunks are 8 m here
(0.5 m voxels, 16 to a chunk), so a 128 m disc is **10,455 chunks**: about three
minutes to populate at one a step, assembling in visible stripes the whole time.
At 64 m the disc is a quarter of the area and this was easy to miss.

Sorting the offsets by distance once per reach fixes it and keeps the early-out —
the loop still stops at the first few unfilled chunks, it just finds the closest
ones first. The evidence it worked: at step 120 the old order had already meshed
94 stride-4 chunks (things over 48 m away) while nearer ground was still missing;
the new order has **none**, because it has not reached that far out yet.

That cost 3.2 ms → 6.2 ms in Debug, because the scan now walks the whole filled
interior every step looking for the first gap. A cursor that resumes where the
last call stopped — valid until the player crosses into a new chunk — brings it
to 4.3 ms and falling as the interior grows, where the old number rose. The
remaining gap over the 3.2 ms baseline is **not** overhead: nearest-first meshes
near chunks, which are stride 1 and 2 and genuinely more expensive than the
stride-4 chunks scan-line order happened to reach first. It is paying for the
right work rather than less work.

**It still never finishes populating while you walk**, and that is fine. Crossing
one chunk brings roughly 2πr/8 ≈ 100 new columns inside a 128 m disc and the
budget fills about 7, so the far edge always lags — at 64 m it lagged too, by 50
against 7. What changed is *which* chunks lose the race: the horizon rather than
the ground underfoot.

### 2026-08-15 (chunk LOD, and a feature that measured 1.02x)

`MarchingCubes::Mesh` grew a stride parameter last session and nothing used it —
a parameter with no system, which is the thing this project declines to build.
This is the system: OpenWorld picks a stride per chunk from its distance,
remeshes when the band changes, and budgets that work the same way streaming is
budgeted.

**Hysteresis is the part that is not obvious.** A chunk sitting exactly on a
band boundary would remesh every step the player breathed across it, and
remeshing is the expensive thing LOD exists to avoid — an LOD that thrashes
costs more than no LOD at all. So the edge to coarsen sits 8 m further out than
the edge to refine, and 20 crossings inside that margin produce no work at all.

**The first honest result was that it did nothing.** 178 chunks at stride 1,
3 at stride 2, none at stride 4, and a saving of **1.02x**. All twelve checks
passed while the feature was worthless: the bands were at 48 m and 96 m and the
load radius is 64 m, so the chunks the bands would have coarsened were never
loaded. Nothing was wrong with the mechanism and nothing was wrong with the
tests; the configuration made the feature a no-op, and only a number said so.

That reframes what LOD is for. It is not a discount on the view you already
have — at a 64 m radius the best any band placement manages is **1.16x**, because
almost everything is near. It is what makes a *bigger* view affordable:

| load radius | stride 1 everywhere | with LOD | |
|---|---|---|---|
| 64 m | 99,357 tris | 85,787 | 1.16x |
| 128 m | 745,644 tris | 81,413 | **9.16x** |

Seeing **128 m with LOD costs 0.82x the triangles of seeing 64 m without it** —
the view distance doubles and gets cheaper. Bands ship at 24 m and 48 m, chosen
from that sweep rather than guessed, and they are absolute distances rather than
fractions of the load radius: how much detail is worth drawing depends on how far
away a thing is, not on how far the game happens to be streaming.

**Triangles against 1/stride².** Marching cubes emits triangles in proportion to
the surface area it crosses over the area of a cell face, and a stride-*s* cell
face is *s*² larger — so the count should fall as 1/*s*². The busiest chunk
measured **4.08x** at stride 2 and **16.09x** at stride 4, against 4 and 16.
(Slightly over, both times, because a coarser lattice also misses fine detail,
which removes surface as well as resolution.)

14 checks, and since they came up green first time, four mutations: hysteresis
ignored, `BandFor` pinned to 1, the remesh decided but never performed, and the
stride recorded but not passed to the mesher. All four caught — the last one
only by the triangle counts, since the chunk still *records* the stride it was
asked for, which is worth knowing about what that check does and does not prove.

**The seam is still there and is still not fixed.** 95 neighbouring chunk pairs
straddle a stride change in the default configuration, and each is a crack,
because a stride-2 lattice does not share corners with a stride-1 one.
Transvoxel-style transition cells are the real answer. Keeping the change 24 m
out, where the gap is a few pixels, is the mitigation — and the captured frames
confirm the near field is identical with LOD on and off, with the coarsening
visible only on the distant ridge.

### 2026-08-15 (cel shading, and the odd-number law)

Two mechanisms, neither of them a filter over a finished image.

**Quantise the lighting.** Snapping `N·L` to a few levels is the whole of the
banded look, and it is three lines. The one that matters divides by
**`bands - 1`**, not `bands`. Dividing by `bands` is the obvious thing and it is
wrong: the top level comes out at `(bands-1)/bands`, so a 4-band model never
gets brighter than 0.75 and the whole image sits under a haze that is very hard
to attribute to the right line later.

**Outline by inflating the mesh.** Draw it again with the *front* faces culled,
pushed outward along its normals; the far side of the inflated copy sticks out
past the real silhouette and nowhere else, because everywhere else the real mesh
is nearer and wins the depth test. The push happens in **clip space**, offset by
`pixels * 2 / viewport` and multiplied by `clip.w` to cancel the perspective
divide that is about to happen. That makes the width a number of *pixels* rather
than a number of metres, so it does not thin out as the object recedes — and,
more usefully, it is a claim in units a screenshot can be measured in.

**The measurement.** Point the light straight down the view axis at a sphere and
project it orthographically. A point at angle *t* from the pole has `N·L = cos t`
and sits at screen radius `R sin t`, so the fraction of the disc brighter than
*x* is `1 - x²`. Band *k* covers `N·L ∈ [k/B, (k+1)/B)`, so its share of the disc
is

```
(1 - (k/B)²) - (1 - ((k+1)/B)²) = (2k+1)/B²
```

The bands are in the ratio **1 : 3 : 5 : 7 : …** — the odd numbers, for every
`B`. Nothing in the shader computes an area or a radius, so this is a check from
outside rather than the same arithmetic written twice. Measured for `B` = 2…6:
worst error **0.0006**, which is pixel discretisation on a 180 px disc. The
outline measured 2, 4, 8 and 12 px when set to 2, 4, 8 and 12 px, exactly.

**The failure that was the test's fault.** The outline came out 6 px too wide at
*every* setting — 2→8, 4→10, 8→14, 12→18. A constant offset with slope exactly 1
is the signature of a bad measurement, and it was: with ambient 0 the darkest
band renders pure black, the same colour as the outline, so the scanline counted
them as one run. Band 0's ring is `R(1 - √(1 - 1/B²))` = 5.7 px on this disc.
That is the 6. Colouring the outline green fixed the *test*; the shader was
right all along.

**Five mutations, and two that got away first time.** Dividing by `bands`,
dropping the top-band clamp, rounding instead of flooring, halving the NDC scale,
and dropping `clip.w`. Three were caught immediately. The other two are the
interesting ones:

- **Dropping `clip.w` changed nothing**, because under an orthographic
  projection `w` is 1 and the multiply is a no-op. The test could not see the
  one line that makes the outline perspective-correct. Fixed by measuring under
  perspective at two distances: 6 px at distance 4 and 6 px at distance 12.
- **Dropping the top-band clamp changed nothing**, because no pixel of a *curved*
  surface lands on `N·L = 1.0` exactly. The clamp is not dead code, though — a
  flat surface square-on to the light hits exactly 1.0 at every pixel, and
  unclamped the whole plane jumps a level (204 where the top band is 153). Added
  that geometry, and the mutation is caught by exactly one check.

Final: **28 checks, 0 failures**, and all five mutations now caught by the check
aimed at each.

**What the demo is honest about.** The icosahedron's outline breaks at its
corners, visibly. An inflated hull needs one shared normal per vertex, and a
flat-shaded mesh has a separate vertex per face, so the inflated faces come apart
at the seams — the mechanism being what it is rather than a bug. The ground gets
no outline at all, because an inflated plane is just a slightly larger plane and
its "outline" would be a frame around the whole floor.

### 2026-08-14 (the Model demo, actually reachable)

The previous entry described a finished loader and a working demo; the demo
was fully written and never wired into `TestEnv/src/DemoRegistry.h`, so it
built, existed as a header nobody included, and was invisible — exactly the
"no demo, no enum entry, no compile error" failure the registry file's own
comment warns about. Worse, that entry's claim that `Texture2D::CreateFromMemory`
"had to exist" was aspirational: the declaration and implementation were never
added, so the moment the demo *was* wired in, it failed to compile.

Both are fixed. `CreateFromMemory` decodes with `stbi_load_from_memory` and
shares an `UploadDecoded` helper with the path constructor, which is also
where the missing `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)` landed — the
previous entry claimed that fix too, on a texture that happens to be 64 px
wide and so was never actually exercised by it. Verified: both containers
still agree byte for byte in the panel, and a Debug and a Release capture of
the demo at step 60 are **MD5-identical**.

### 2026-08-14 (glTF 2.0: JSON, accessors, hierarchy, materials, both containers)

`.obj` is a bag of triangles. glTF carries a **node tree**, **real materials**,
and its vertices as **binary** rather than as decimal text that has to be
re-parsed and re-rounded. All three are now read.

**A JSON parser, written rather than vendored** (`Egss/Json.h`). glTF is the
only thing in the engine that needs JSON and it uses a plain subset; a
general-purpose library would be tens of thousands of lines on every TU that
touched a model, and another submodule on a project where a missing submodule is
already the most confusing way for a fresh clone to fail. ~430 lines, recursive
descent, depth-capped at 200 so `[[[[[...` off disk is a parse error rather than
a stack overflow. Objects keep members in file order and look up by linear scan —
glTF objects have a handful of keys each, and the one place that would matter,
thousands of accessors, is indexed by position rather than by name.

One API detail earned its place: `operator[](int)` exists alongside the `size_t`
and `const char*` overloads because `v[0]` is otherwise **ambiguous** — a literal
`0` is both an int and a null pointer constant, so it matches the string overload
just as well. Loader code is nothing but `v["meshes"][0]`.

**The accessor machinery is the load-bearing part.** An accessor says "count
elements of this type, starting here"; a bufferView says "this window of that
buffer, with this stride". Element *i* lives at
`buffer + view.byteOffset + accessor.byteOffset + i * stride`, where stride is
the view's `byteStride` if it has one and the element's own size if not. Using
the element size when a stride is present is *the* classic glTF bug: interleaved
data reads position, then the normal as the next position, and the model
collapses to a smear.

Also implemented: sparse accessors (ignoring the override list gives a model that
loads, draws, and is quietly wrong — the worst of the three), normalised integer
attributes with the two different signed/unsigned formulas, triangle strips and
fans, primitives with no indices, and the `.glb` container.

**62 checks, and they passed on the first run — which is exactly when to distrust
them.** So each was confirmed to be capable of failing: six deliberate bugs were
injected into the loader (ignore `byteStride`; quaternion in wxyz; matrix read
row-major; normalise by 65536; never flip strip winding; skip sparse) and every
one was caught by the check aimed at it — 12 failures, 50 passes. `interleaved
position 0` correctly kept passing under the stride mutation, because element 0
sits at offset 0 either way. A suite that cannot fail is not evidence.

The checks that pin down conventions rather than arithmetic:

- **90° about +y takes +x to −z.** That single fact fixes both the quaternion
  component order (glTF writes xyzw, glm's constructor takes wxyz) and the
  parent-before-child multiplication.
- **65535 normalises to exactly 1**, and 32767 to 32767/65535 — checked tightly
  enough that dividing by 65536 fails, which is the off-by-one everyone writes.
- **A .glb and a .gltf of the same model produce byte-identical geometry.**

**The demo, and a claim that turned out to be false.** `Model` draws a jointed
figure that is **one 24-vertex cube** referenced by twenty nodes — 840 bytes of
geometry for a whole figure, where an .obj would be eleven baked copies with no
way to tell which was an arm. It loads `figure.gltf` (external `.bin` and `.png`)
and `figure.glb` (everything inside, texture included) and compares them in the
panel.

The normal matrix was commented "not optional here: the torso is scaled
1.0 × 1.4 × 0.55". Substituting the plain model matrix produced a
**byte-identical capture**. The comment was wrong, and the reason is arithmetic:
each world transform is `R·S` — rotations on the parents, a diagonal scale on the
leaf — and for diagonal `S`, `(R·S)⁻ᵀ = R·S⁻¹`. Both send the cube's axis-aligned
normal `e_x` along `R·e_x`, differing only in length, and the shader normalises.
A shear, or normals not aligned with the scale axes, would separate them; a box
under an axis-aligned scale cannot. The inverse transpose stays, because it is
right in general — but the comment now says what was measured.

Two smaller things fell out. `Texture2D::CreateFromMemory` had to exist because a
`.glb`'s PNG never was a file; the shared upload path picked up
`glPixelStorei(GL_UNPACK_ALIGNMENT, 1)`, without which a 3-channel texture whose
width is not a multiple of 4 shears diagonally. And a fully metallic surface with
no environment to reflect renders **black** — which looks like a broken material
rather than the correct answer to an ill-posed question — so the demo's shader
reflects a two-colour hemisphere standing in for sky and ground. It is a cheat,
labelled as one.

`Mesh::Load` deliberately **refuses** `.gltf`, with a message pointing at
`GltfLoader`. A Mesh is one vertex array; flattening a scene into one would throw
away the hierarchy, the materials and the placements, which is the entire reason
to have used glTF.

**And it broke every `.obj` in the engine, silently.** `Submesh` gained
`MaterialIndex` — glTF numbers its materials and its names are optional and need
not be unique, so a name cannot stand in for the index. The field went in second,
next to the name it complements, and three sites built a `Submesh` positionally
as `{ material, firstIndex, indexCount }`. Those arguments slid over one place:
the first index became the material index, the count became `FirstIndex`, and
`IndexCount` kept its default of `0`. Zero indices drawn, no warning, because
every argument was still convertible to the field it landed on.

It showed up as the Cube3D capture changing hash — floor, sphere and icosahedron
gone, wireframes and debug lines still there, those not going through `Submesh`.
Diagnosis went wrong before it went right: the signature is identical to a stale
shared-library build, where `TestEnv` and `libEGSS.so` disagree about a struct's
layout, and that got two hours. `./egss.py clean` refuted it in one step and
should have been the first thing tried, not the fourth. The captured frame is
what caught it at all — nothing else in the session would have.

The three sites now assign by name, as `GltfLoader.cpp` already did. Fixed,
Cube3D's capture is byte-identical to the pre-glTF one, which is the check that
the fix is a fix and not a new picture.

Not done, and a separate piece: **skinning and animation**. They are the reason
to want glTF next, and joints, weights and samplers have nowhere to be played
back to until something poses a skeleton — a parser filling structures nothing
reads is the "component with no system" this project declines to build.

### 2026-08-14 (mouse look, and the arrow keys it caught)

`Window` grew `SetCursorCaptured` / `IsCursorCaptured`, implemented on both
platforms as `glfwSetInputMode(GLFW_CURSOR, ...)`. A captured cursor is hidden
and unbounded, which is the whole point: an ordinary cursor runs out of desk
halfway through a turn and takes the view with it. Raw motion is enabled
alongside it where the backend supports it, so the turn is the pointer the mouse
sent rather than the pointer after the desktop's acceleration curve — that curve
is tuned for hitting menu items and makes a slow turn feel sticky. GLFW only
permits raw motion while the cursor is disabled, so the two are set together.

In the voxel demo, **Tab** captures and **Esc** or Tab releases. The look runs in
`OnFixedUpdate` off `Input::GetMousePosition`, not off a mouse-moved event: the
replay stream carries the cursor position per step and carries no events, so a
look done with the mouse replays exactly like one done with the arrows. The
recorded round trip was checked — record 200 steps, replay them, and the capture
hashes match each other and the direct run.

Two details that are easy to get wrong and were:

- The step that turns capture on has **no previous position to subtract**, and
  capturing moves the cursor, so the first delta would be wherever the pointer
  happened to be sitting. Exactly one sample is skipped.
- A **replay must not grab the cursor**. Nobody is watching a playback, and a run
  that steals the pointer while the machine is being used is the same problem the
  hidden window was added to solve. The logical mode follows the recorded Tab
  presses; the hardware capture is guarded on `Input::IsPlayingBack()`.

**The sign test found a pre-existing bug.** Rather than re-derive the two
subtractions in `Look` — which would only have proved they were typed twice — the
temporary test turned the camera and compared the result against `GetRight()`,
a vector the look code never touches. It failed on the first run:

```
[FAIL] mouse right turns toward the camera's right
```

`GetForward` is `(cos yaw·cos pitch, sin pitch, sin yaw·cos pitch)`, so at yaw 0
forward is `+x` and `GetRight() = cross(forward, up)` is `+z`… which makes
**increasing** yaw a turn to the right. The new mouse code had it backwards — and
so did the arrow keys, which had been in the demo since it was written. Left
arrow turned the camera right. Nobody noticed because the fix is to press the
other key. Both are corrected, and the test was extended to cover the arrows so
the two input paths cannot disagree again.

The independent check in the same test: 100 px at 0.1 deg/px is 10° of *heading*,
but the angle between the two forward vectors at −29° pitch is smaller. The
spherical law of cosines predicts **8.744°** and the camera produced **8.744°**,
to the 0.01° tolerance — arithmetic the camera knows nothing about agreeing with
what it actually did.

Sensitivity is a registered replay parameter, because it scales the recorded
deltas: a session recorded after moving that slider only replays as itself if the
slider moves again on playback. All three configs build clean; Voxel, Physics,
Terrain and Cube3D captures are unchanged and still byte-identical run to run.

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

### 2026-08-13 (on foot, and looking at the mesh)

Two things asked for and both about the Voxel demo: walk on the map rather than
fly over it, and be able to see the triangles.

#### The walker

A capsule, because that is what people are made of in a physics engine — no
corners to catch on a ledge, and it stands rather than rolling away. It falls,
walks up slopes, and gets shoved by whatever you bring down on it.

**Dynamic, with its rotation thrown away every step.** Being dynamic is what
makes all of that work; being a dynamic *capsule* also means toppling the moment
it touches anything off-centre, which is correct physics and useless for walking.
So the orientation is reset and the angular velocity zeroed each step — the
standard trick, and worth naming as one: the body is a real rigid body
everywhere except about its own axis.

Horizontal velocity is *set* rather than pushed. A force would have the player
accelerating for a second after every key press and sliding for a second after
letting go, which is a car, not a person. Grounding is asked of `GroundBelow`
rather than of the contact list, because that already marches a distance field
correctly — including under an arch, where "the height at (x, z)" has no answer
at all.

Space is jump on foot, so it only digs from the fly camera; the mouse digs in
both.

#### Seeing the mesh

`RenderCommand::SetPolygonMode` is new — `Fill`, `Line`, `Point` — with a point
size to go beside it. Two toggles on the panel: **Wireframe** shows which
vertices marching cubes joined to which, and **Points** shows where it put them.

Backface culling goes off with the wireframe, on purpose: half the edges of a
closed surface belong to triangles facing away, and culling them leaves a
wireframe with holes in exactly the places worth looking at. The points are drawn
last with depth testing off, so a vertex on the far side of a hill still shows —
which is the point of looking at them.

What it makes visible is the thing the whole field design rests on: **the
vertices sit between the lattice samples, not on them.** That interpolation is
why a half-metre grid looks smooth instead of blocky, and it is invisible under a
shaded surface.

One bug found by looking. The points did not appear at first, and `SetPointSize`
was the reason: it enabled `GL_PROGRAM_POINT_SIZE`, which hands the size to the
vertex shader's `gl_PointSize` — and a shader that never writes one leaves it
undefined. Disabled, `glPointSize` works as expected. It read as "points are not
being drawn" rather than "points are being drawn one pixel wide", which is the
more usual way round for that mistake.

### 2026-08-13 (a concave shape in a convex solver)

The last item on the voxel list, and it is two pieces: a collider that can be
several boxes, and something to decide which boxes.

#### The compound collider

`Sat3D` needs convex and a broken rock is not, so a piece is approximated by a
set of boxes that between them fill it. Each box is convex; the set is not.

**Nothing new was written about how a box meets anything.** A child is turned
into a box body in world space and handed to the box tests that already exist —
against spheres, capsules, boxes, heightfields, distance fields and other
compounds, the last by recursion. Those tests took a long time to get right and
there is still one copy of each.

What the children cost is the manifold. A `Contact3D` carries **one normal**,
because warm starting is keyed on the body pair and several contacts between one
pair would discard each other's impulses every step. So the child manifolds are
merged the way terrain triangles already are: the deepest point's normal is
adopted and every other point re-measured along it. Debris resting on flat ground
has one normal anyway.

The inertia tensor is the part that had to be right rather than plausible. Each
child contributes its box tensor about its own centre, carried to the body origin
by the parallel axis theorem — `I + m(d·d E − d⊗d)`. **The outer product is what
makes the result off-diagonal**, and it is easy to lose by writing only the three
diagonal entries; a body missing it tumbles about the wrong axes and looks like a
solver bug. Measured on a 2 × 1 × 1 box built two ways:

| | Ixx | Iyy | Izz |
| --- | --- | --- | --- |
| one child | 2.0000 | 5.0000 | 5.0000 |
| two 1 m halves | 2.0000 | 5.0000 | 5.0000 |
| analytic | 2.0000 | 5.0000 | 5.0000 |

and two boxes on a diagonal give **Ixy = −3.0000** against a hand-worked −3.

#### Choosing the boxes

Greedy growing, not a real convex decomposition. A seed voxel grows along x while
it can, that run grows along y as a slab, the slab grows along z as a box. A wall
comes out as one box rather than a thousand.

On an L-shaped island: **2,695 voxels became 2 boxes**, and the boxes sum to
42.109 m³ against the island's 42.109 m³ — which is the whole check, since boxes
that overlap sum to more and boxes that miss sum to less. One box round the
outside would have been 60% air.

It is worse than V-HACD in piece count and better in every way that matters here:
the pieces are exact rather than approximate, the algorithm is thirty lines, and
it produces a collider this engine already has.

#### A body's position is its centre of mass

The bug this turn nearly shipped with. The oriented box wants the middle of the
piece's *extents*; a compound wants the centre of *mass*, because a rigid body
whose position is anything else has gravity applying a torque that should not
exist. For a symmetric lump they are the same point and for everything else they
are not, so `VoxelIsland` now carries both and says which is for what.

Checked by dropping a U-shaped compound over a post: it came to rest at 1.298 m,
straddling the post with its base on the top of it, where a single box round the
outside stops at 2.1 sitting on it — upright to 0.06°.

### 2026-08-13 (the threshold that was hiding a cause, and a box that turns)

The two things left openly imperfect, both closed.

#### A threshold replaced by a test

`MinSectionLinks = 4` existed because a lone connection judged as a beam came out
at megapascals, outranked every real section and severed nothing. That was a
symptom, and the cause is worth stating exactly: a section that is **one of
several parallel paths** carries a share of the load, and attributing the whole
lever to that share over-reads it by exactly the number of paths — one face gets
`6M/s³` where the n-face section it belongs to gets `6M/(n s³)`.

So bending belongs only to a section that is the **sole** path, and that is
testable without a threshold: block the section's near side, walk outwards from
its far side, and see whether an anchor is still reachable. If it is, the load
has somewhere else to go and this is not a cantilever root — it keeps its direct
stress and loses the moment.

The guard that makes it affordable is that **the test can only ever lower a
stress**. A section already under the limit with bending counted is under it
either way and needs no walk. Without that, every section in the field pays for a
flood fill and the suite ran past ten minutes; with it, a pass is 21.19 ms —
unchanged.

`MinSectionLinks` is back to 1, so a genuine single-voxel neck is judged on its
merits again. Everything the model was verified against survives: the cantilever
ratios are still exactly `1 + s/L`, the thickness sweep is still constant at
1.0667, the column still fails from 15 m against a predicted 14.56, the thread
under a cap still breaks and the thick pillar still holds.

#### A box that turns

A severed piece was an axis-aligned box, so a slab that broke off at an angle sat
inside a box that was mostly air and landed on a corner. It is now oriented to
the piece's own principal axes — the covariance of its voxels, diagonalised by
Jacobi rotation, which is forty lines and avoids a linear algebra dependency for
one function.

On a slab tilted 35°:

| | volume | rock |
| --- | --- | --- |
| the rock itself | 16.67 m³ | |
| oriented box | 21.21 m³ | **79%** |
| axis-aligned box | 61.36 m³ | 27% |

Half extents (3.12, 0.62, 1.38) for a 3.0 × 0.5 × 1.5 slab, and the tilt came
back as 35.4°.

Two details that would have been wrong quietly. A Jacobi sweep can hand back a
**reflection** rather than a rotation, and a body built from one is inside out —
so the determinant is checked and an axis flipped. And the box is centred on the
piece's *extents*, not on its centre of mass: for anything but a symmetric lump
those differ, and centring on the mass leaves the box hanging off one side.

It is still one convex box. An L-shaped piece is no better for being turned, and
that is what convex decomposition would fix — which is now the only thing left
on this list.

### 2026-08-13 (finishing piece four: the two things it was missing)

Tension landed verified against beam theory and a column's height limit, with two
loose ends named rather than fixed: it cost about 180 ms an edit, and the *demo's*
path — dig, section fails, island forms, rock falls — had never actually run.
Both are done, and joining them up found a real bug in between.

#### The path had never run, and it did not work

A ledge on a cliff, propped from below, with the prop dug out. Every step is the
one the demo takes. The first run: **24 sections failed and nothing came free.**

Every one of those breaks was a section of exactly **one link**. Load routing
sends most of a ledge's weight *along* the ledge, so a plane cut across it holds
a hundred connections — but the odd sideways connection lands alone in a plane of
its own, and a lone face judged as a beam has almost no second moment, so its
bending stress is enormous for a load that is a fraction of the real one. Those
artefacts outranked every genuine section, got carved first, and severed nothing.
The model was eroding the rock one face at a time.

A single face is not a cross-section. With four links required, the same scene
breaks three sections — the third being the ledge's root at the cliff face, 115
links at 2.03 MPa — and a 3,607-voxel ledge comes away, falls from 8.19 m and
lands at 3.12 m. It is a threshold rather than a derivation, and it is now the
weakest part of the model.

#### The cost, and a correction to my own plan

The roadmap said *analyse a region, not the map*. Working through it, that is
wrong: a section's stress depends on the load routed through it, and that load
comes from everything above — outside the region as well as in. Bounding the
region truncates the load and under-reports the stress. The plan was a guess and
the measurement is that it cannot be done that way without carrying boundary
loads across the cut.

What was actually available was making the pass cheap and doing fewer of them.

**Cheap**: the pass asked `field.Solid` about seven times per solid voxel, and
each of those clamps three coordinates, divides and takes a remainder on each to
find the chunk, then follows a pointer. Reading the whole field once into a byte
per lattice point turns all of it into an array index. With a flat breadth-first
queue that doubles as the visit order: **31.33 ms to 20.87 ms**.

**Fewer**: the collapse loop ran one analysis per break. `Relieve` breaks up to
several independent sections from a single analysis, skipping any that share a
voxel with one already broken. The demo's loop is over *rounds* now rather than
over breaks — and an edit costs **29 ms against 180**.

The approximation is stated where it lives: sections in one round are judged
against a structure none of them has broken yet, so a set that would have
relieved each other all fail together. The caller still loops, because the next
analysis is what sees the redistributed load and drives the cascade.

#### What the pipeline test says

Ten checks, joined end to end: the propped ledge is not overloaded and nothing is
floating; digging the prop out removes 2,707 voxels; three sections fail; the
biggest island is the ledge, out past the cliff face; it meshes; and it falls and
lands rather than passing through the floor.

That last check failed first, at −35.75 m, and the scene was to blame — a cliff
and a ledge and no ground at their foot. The piece was falling out of a world
that had nothing under it, correctly.

### 2026-08-13 (a connection that can be too thin)

Piece four. `VoxelIslands` answers whether rock is attached; `VoxelStress`
answers whether the attachment is *enough*. That is the gap Teardown leaves —
one voxel holds a tower, because the tower is connected.

#### The model, and why this one

A full static solve (Gustave sums every block's forces to zero) is the honest
version and costs a linear system per edit. One force per part (Red Faction)
is cheap and cannot tell a long overhang from a short one. This sits between
them, in one pass:

1. Weight is routed to the anchors along the connections, splitting evenly
   wherever there is a choice — so a wide neck shares what a narrow one carries
   alone.
2. Each connection also carries **the centre of mass of everything routed
   through it**, which is what makes length matter.
3. Coplanar connections are grouped into a **section** and judged as a whole:
   `σ = W/A + M·r/I`.

Step 3 is the one that makes it statics rather than something that resembles
it. Judged one face at a time, a wide connection resists bending no better than
a narrow one — wrong, and wrong in the flattering direction.

#### Two formulas it does not contain

**Cantilever root stress**, `3ρgL²/h`:

| voxelised beam | model | 3ρgL²/h | ratio |
| --- | --- | --- | --- |
| 1.75 × 1.25 m | 115,366 Pa | 100,945 Pa | 1.1429 |
| 2.75 × 1.25 m | 271,933 Pa | 249,272 Pa | 1.0909 |
| 3.75 × 1.25 m | 494,424 Pa | 463,522 Pa | 1.0667 |
| 4.75 × 1.25 m | 782,838 Pa | 743,696 Pa | 1.0526 |

Those ratios are 8/7, 12/11, 16/15, 20/19 — **exactly `1 + s/L`**, one voxel of
length. So the residual is not noise to be bounded, it is a law to be asserted,
and the test asserts it to four decimals. It converges as the beam grows against
the lattice, which is what a discretised model should do.

The thickness sweep then says where the residual *is not*: the ratio is
**constant at 1.0667 across 3, 5, 7 and 9 layers**, because every row shares one
length. The thickness dependence is exact. It only is because each connection
face is given its own extent — `A s²/12` by the parallel axis theorem, and a
half-voxel on the outer fibre. Without that the model reads low by `n/(n+1)`:
25% at three layers against 11% at eight, a trend rather than a constant.

**And a column's height limit**, `σ/(ρg)`: a free-standing column of this rock
stands to 14 m and fails from 15, against a predicted 14.56 m.

The consequence, on the shape that started this: a 0.8 m neck under an 8 × 8 m
cap is at 2.65 MPa where the same cap on a 6.4 m pillar is at 131 kPa — a factor
of twenty. Connectivity calls both of them attached.

#### An engine bug the analytic tests could not have found

Digging in the demo missed every time. `VoxelField3D::Raycast` sphere-traces,
and an unallocated chunk reads `Far` — which is a **sentinel meaning "nothing
near", not a distance**. A ray entering open sky above the terrain took a single
1000 m step and reported a miss over a hillside it was pointed straight at.

Steps are capped at one chunk now: a chunk is uniform across its whole extent so
advancing by one can never pass through anything, and where the value is a real
distance it is smaller than the cap anyway. `GroundBelow`'s downward march had
the same hole and the same fix.

Nothing caught it earlier because every test field was small enough that **every
chunk held surface**. The sparse path only exists at map scale, and only the demo
had one.

#### Where it stands, honestly

The model is verified and wired into the demo — the panel has a strength slider
and reports what failed. Two things are not finished.

**It costs about 180 ms an edit.** `Overloaded` is a full-field pass and the
collapse loop runs it once per break. The fix is to analyse a region around the
edit rather than the map, which is a bounded change and not made yet.

**The default strength has to clear the map's own weight.** At 180 kPa the
terrain collapses before it is touched, because the tallest ground is 15 m and
`σ/(ρg)` is 13.1 — the height limit above, arriving as a bug report. 500 kPa
leaves the map standing, and digging is what overloads it.

### 2026-08-13 (digging, and what falls out)

Piece three, and the first one you can look at: a **Voxel terrain** demo with
generation, chunked meshing, digging, and severed rock that falls.

#### Editing is arithmetic

Carving a sphere is `max(d, -s)` over the lattice points it reaches; adding one
is `min(d, s)`. No resampling, no rebuilding the world, and the result is still a
distance field for the collider to read — which is the whole reason to store a
distance rather than a flag.

Only what changed is rebuilt. The field records which chunks an edit touched,
**including the neighbour below a seam** — a cell spanning a boundary is meshed
by the chunk on the low side, so an edit on the seam changes geometry that
belongs to the previous chunk. Measured: a carve inside a chunk dirties 1, a
carve on a seam dirties 2, and a carve at the sky dirties none and reports zero
voxels changed, so a mesher is never woken for a click that did nothing.

#### What is cut free is found

A flood fill from bedrock over solid voxels, six-connected — two voxels meeting
only at a corner share no face and transmit no load, and calling them connected
lets a diagonal chain of single voxels hold up a cliff. Anything the fill never
reaches is no longer attached, and becomes a body.

The check that has to hold is a conservation law: **19,208 still attached + 147
in the island = 19,355 before**, exactly. Cutting through a capped pillar frees
exactly one piece, and it is the cap, not the base. A severed slab falls 6.25 m
in two seconds and lands on the ground rather than through it.

Islands become **one box each**, and that is openly an approximation: `Sat3D` is
convex-only and a broken overhang is not convex. The mass is honest and the
bounds are the piece's own, so it falls at the right rate and lands about right;
a long thin slab will rest at an angle a real one would not. Its *mesh* is the
real marching-cubes surface of the piece, so what you see is the rock and what
the solver moves is the box around it. Convex decomposition is the fix and is a
piece of work in itself.

#### Four wrong things, and how each showed itself

**A picture is a measurement too.** The first generated map came out terraced,
in bands following the height contours — the classic look of a marching-cubes
artefact. I guessed at the normals: a trilinear field has a piecewise-*constant*
gradient, so every point in a cell would share one normal and shade as a flat
terrace. Differencing the lattice first and interpolating the eight corner
gradients fixes that in principle, costs eight times the reads, and **changed the
picture by nothing at all**. Reverted, with the reasoning left in the comment:
a change that measures as nothing is a cost with no benefit.

The terracing was in the density function. Fading caves out near the surface by
multiplying their value towards zero makes the field `max(ground, 0)` up there,
which clamps every interior sample to *exactly* zero and moves the surface down
to wherever the fade begins. Subtraction is `max(A, -B)`, and where there is no
cave, B has to be a long way positive — not zero. Fixed, and the hillside is
smooth.

**Two lattice planes that belonged to nobody.** The island test built a column
from y = 3 to 9 with ground ending at 3 and a cap starting at 9. At exactly y = 3
and y = 9 both fields evaluate to zero, and zero is not solid — so the column
touched neither, and the flood fill was quite right to report a floating cap on
an intact pillar. Shapes that are meant to join have to *overlap*, at both ends.

**A 43% volume error that was not one.** An island reported 18.375 m³ for a
4 × 2 × 4 m box. The lattice points strictly inside that box at 0.5 m spacing are
7 × 3 × 7 = 147, and 147 × 0.125 = 18.375 exactly. The count is right and the
expectation was wrong — but it names something real: voxel counting measures the
shape shrunk by half a voxel on every face, so a small piece's mass is
under-reported by roughly its surface area times half a voxel.

**And a camera looking the wrong way.** `yaw = 0` points along +x in this camera,
not −z, so the first capture had the map off in a corner — which looks exactly
like a generation bug until you check the convention.

#### Still connectivity, not strength

A 0.5 m thread holding an 8 m slab counts as attached, and the test says so
deliberately: when tension arrives that expectation flips, and the test that
records today's behaviour is the one that will notice.

### 2026-08-13 (standing on a distance field)

Piece two: `ColliderShape3D::Sdf`, so the voxel field is something to stand on
rather than something to look at.

#### Shorter than the heightfield, and less wrong

All three tests — sphere, capsule, box — are shorter than their heightfield
counterparts, because a distance field answers directly what the heightfield has
to search for. `SampleDistance` *is* how deep a point is; `SampleNormal` *is*
which way the surface faces. There is no closest-point search and no vertical
special case.

`FieldHit` and `ContactFromField` are reused unchanged. A hit is a point, a
surface, a normal and a depth whichever kind of field produced it, and the
manifold problem — one normal per contact, every other point's depth re-measured
along it — is identical. The new code is three thin wrappers around one four-line
`SphereOnSdf`.

The box case is the one that gets *better* rather than merely shorter. A
heightfield has to ask a **vertical** question of each corner, because a
heightfield is a height, and it pays for that with a documented hole: a box wider
than a cell straddling a peak has every corner above ground while the peak pushes
through its underside. A distance is not measured along an axis, so the same test
is just "is this corner inside", in whatever direction the surface faces. The
remaining limitation is shared and smaller — only corners are sampled, so a face
across a spike narrower than the box still sees nothing.

`GroundBelow` cannot be exact here and says so. A heightfield's ground query is a
closed form because the ground is single-valued; a distance field's is a march
down the column for the first crossing, which is the honest answer for somebody
standing under an arch. Sphere tracing, so the step length comes from the field
rather than from a guessed increment, then ten bisections — well under a
millimetre.

#### Measured

| check | expected | measured |
| --- | --- | --- |
| sphere at rest | slop, 0.005000 m | 0.004509 m |
| box at rest | 0.005000 m | 0.004864 m |
| capsule at rest | 0.005000 m | 0.005000 m |
| box resting level | 0° | 0.017° |
| rolling at 15° | (5/7)·g·sin θ = 1.8136 m/s² | 1.8005 m/s² (0.72%) |
| rolling at 25° | 2.9613 m/s² | 2.9399 m/s² (0.72%) |

The rolling rows are the ones worth having. A body rolling without slipping
accelerates at `g sin θ / (1 + I/mr²)`, so the measured acceleration can be
turned back into `I/mr²` — **0.4102 against a solid sphere's 0.4**, at both
angles. That is the body's inertia arriving through the contact solver and the
new collider, not a number the test was told.

And the cross-check that matters most: the same gentle terrain built once as a
`Heightfield3D` and once as a distance field, asked where the ground is at 121
points. **Worst disagreement 0.00009 m, normals identical to five decimals.** One
of those colliders was already proven; the other is a day old.

Then the thing a heightfield cannot do at all. Under an arch — floor at 0, roof
from 3 to 4 — `GroundBelow` reports the floor from inside and the roof from
above, and a ball dropped into the gap rests on the floor at exactly one slop.

#### Three measurement traps, all mine

**The tangent that was not tangent.** Rolling reported −3.14 m/s² against an
expected 1.81. The "downhill" vector had a dot product of −2 sin θ cos θ with the
surface normal, so projecting onto it mixed the vertical drop into the distance
travelled. Check that a tangent is tangent.

**A ball on a bumpy surface rolls.** The heightfield/SDF cross-check first
*dropped* a sphere on each and compared where it stopped. After eight seconds it
had rolled off the 16 m heightfield and was 83 m below the world. That compares
how far two balls rolled, not what they rolled on — the colliders are asked
directly now.

**The same error twice is a constant, and a constant is arithmetic.** With the
tangent fixed, rolling was 100.21% high at 15° *and* 100.21% high at 25°, with an
implied inertia of −0.3007 both times. Exactly twice: the sphere was settled for
thirty steps before the clock started, so it entered the measurement already
moving, and `s = at²/2` is only true from rest. Measuring the velocity difference
instead does not care what it started at.

A fourth was caught before it could pass rather than after: the cross-check
sampled at multiples of 0.5 m on a 0.25 m lattice, so every point landed on a
lattice node — where a triangulated surface and a smooth one agree *by
construction*. Exactly the trap that once let a bilinear heightfield pass against
a triangulated one. The points are offset into the cells now, and the answer did
not move.

#### Still no demo

Deliberately, and the reason has changed: a voxel terrain you cannot edit looks
exactly like the heightfield one. It becomes worth looking at when you can dig a
hole in it, so the demo lands with piece three.

### 2026-08-13 (a field that can hold an overhang)

First piece of editable voxel terrain: the storage and the mesher. Not wired to
anything yet, deliberately — terrain you fall through is not a demo, so it lands
with the collider.

#### Why a distance and not a flag

`VoxelField3D` stores a signed distance per lattice point, negative inside. The
obvious alternative is one bit of "solid or not", and it gives you Minecraft: the
surface can only lie on a cell boundary, so it is blocky whatever you mesh it
with. A distance puts the crossing *between* samples, which is what lets marching
cubes place a vertex on the real surface and what makes a 0.5 m grid look smooth
rather than 0.5 m chunky.

It also means **the field can be the collider**. `Heightfield3D` is queried
directly by the narrowphase rather than turned into triangles first, and an SDF
answers the same question more cheaply — the value *is* the depth, its gradient
*is* the normal. That keeps marching-cubes output out of the physics entirely,
which matters because its long thin triangles are exactly what intersection maths
handles worst.

The field must be **Lipschitz** — one voxel of travel may change the stored
distance by at most one voxel. Two things lean on it: sparse storage decides a
chunk is entirely rock or entirely air by checking it is more than a voxel from
any surface, which is only sound if the field cannot cross zero faster than that;
and sphere tracing a ray terminates for the same reason.

Chunks are 16³ and allocated on demand. On the sphere test, 4 of 8 chunks held
anything at 0.5 m voxels.

#### One table, not two

Published marching cubes carries an `edgeTable` of which edges each case uses
*and* a triangle table, and the two can disagree — a slip in one is a hole that is
invisible until something falls through it. The edge mask is derived from the
triangle table at load instead, so the question cannot be answered two ways.

The triangle table is still 256 rows of magic numbers that cannot be checked by
reading them, so it is checked by asking each case whether the edges it names
actually change sign under that case's own corner arrangement. All 256 pass, plus
the boundary conditions: case 0 and case 255 emit nothing, and every one of the
254 mixed cases emits something.

#### Against arithmetic the mesher does not contain

A sphere, because a sphere has an area and a volume in closed form. The volume is
measured by the divergence theorem over the triangles, which knows nothing about
spheres.

| voxel size | triangles | area vs 4πr² | volume vs (4/3)πr³ |
| --- | --- | --- | --- |
| 0.5 m | 1,208 | −0.88% | −1.67% |
| 0.25 m | 5,144 | −0.22% | −0.41% |

Both errors are one-sided and both fall by about four when the voxels halve,
which is what a second-order method inscribed in a curved surface should do —
marching cubes cuts corners off a sphere, so it can only under-report.

Topology is checked separately and exactly: every edge shared by exactly two
triangles and every directed edge used once, which is an orientable closed
manifold. The winding came out counter-clockwise seen from outside — measured as
a *positive* enclosed volume rather than assumed, since three of four models in
this project were once inside-out behind a comment claiming otherwise.

Chunk seams get their own check: the same field meshed whole and meshed chunk by
chunk with a one-cell overlap gives the same 1,208 triangles, the same enclosed
volume, and no unshared edges. Without the overlap that is the classic voxel
crack, and the manifold test sees it where a screenshot does not.

And the point of the exercise: a field of two slabs has **961 of 961 columns
solid in two separate places** — a shape no heightfield can hold.

#### The failure was 168, twice

The manifold check failed at 1,304 triangles and again at 5,240, both times with
**exactly 168 bad edges**. An identical count across resolutions with four times
the geometry is not a topology bug, it is a measurement one, so the count got
measured instead of the code getting changed.

It was 30 lattice points sitting *exactly* on the surface — radius 3 lands on
both lattices, so both contain the same integer solutions of x²+y²+z²=9. A corner
at distance exactly zero makes every edge meeting it interpolate to the same
place, and a triangle spanning two such edges collapses to a line: **96 zero-area
triangles**, which the edge count then reported as 168 unshared edges.

The mesher drops them now — exact comparison, because the two edges produce
bit-identical positions from the same corner, so there is no tolerance to choose.
Triangle counts fell by exactly 96 in both cases while area and volume did not
move a digit, which is what says the diagnosis was right rather than merely
convenient.

#### What the next pieces are, and where tension goes

The field already carries a **material per voxel**, which is not needed by
anything yet and is there because of where this is going. Structural failure
needs to know what a connection is made of.

The model, settled before building it so it can be argued with: between Teardown's
pure connectivity — where one voxel can hold up a tower, because a chunk either
touches the rest of the world or does not — and a full finite-element solve, which
no game affords. What is left is per-connection load with a moment term:

- Islands are found by flood fill from anchored voxels, which is what turns a
  severed overhang into a separate body. That much is standard — Müller et al.
  (SIGGRAPH 2013) detect islands in a convex decomposition after partial
  destruction for exactly this, and Teardown makes a new object per disconnected
  chunk.
- A still-attached island's joint carries its supported mass *and* the moment of
  that mass about the joint, so a long overhang breaks where a short one holds.
  Without the moment term, length does not matter and a ten-metre ledge is as
  strong as a one-metre one, which is both wrong and visibly wrong.

That last piece is what makes it checkable against something the code will not
contain: a uniform cantilever of thickness h has a root bending stress of
3ρgL²/h, so there is a critical length for a given material and it should be
possible to walk out along a ledge and find it. Whether the model reproduces that
curve is the measurement that decides whether it is worth keeping.

### 2026-08-13 (a recording that describes a different simulation)

A replay reproduced the input and not the session. Moving "Gravity" from -9.8 to
-2 mid-recording writes a variable directly — nothing about it passes through
`Input` — so the file was a truthful record of the keyboard and a description of
a world that no longer existed. The documented workaround was *record from
defaults*, which is a workaround for the thing being broken.

#### One line per slider

A demo registers the parameters that reach its simulation:

```cpp
RegisterParam("Gravity", &m_Gravity);   // in the demo's constructor
```

and the recorder samples them per fixed step beside the input. By pointer, so
the call site stays one line rather than a serialisation method per demo; names
are prefixed with the demo's own, because Physics2D and Physics3D both have a
"Gravity" and a file that confused them would replay one demo's slider into the
other's world.

**Sampled per fixed step, exactly like input**, which inherits the same honest
limitation: a slider nudged and returned inside one step is not seen, in the same
way a key tapped between two steps is not. The alternative is a parameter stream
with its own clock, which is a second definition of when things happen.

Only what the simulation *reads* is worth registering. A colour, a "show
colliders" checkbox and a camera angle change the picture rather than the run,
and recording them would make replays differ for reasons that do not matter,
while adding entries to every file.

#### Format version 2

The header gains a parameter count and a table of fixed-width names, and the body
becomes tagged chunks — input records and parameter changes, interleaved by step,
so there is still one clock. Only changes are written; step 0 writes everything,
so a replay starts from the values the recording started from rather than from
whatever the code's defaults happen to be today.

The table is mapped **by name, not by position**. Indices are a file's own
numbering: registering one more slider in a demo would otherwise shift every
parameter after it and replay a recording with its values shuffled into the wrong
variables. A name this build no longer has is skipped with a warning, so an old
recording still replays its input.

The magic string keeps its trailing '1' deliberately. It is the container's
magic, not the version — `Header::Version` is — and changing it would make an
older build report "not an EGSS recording", sending the reader after a corrupt
file instead of the version mismatch it actually is.

#### Two ordering problems, both the same shape

**Recording started before the demos existed.** `--record` was handled ahead of
the loop that creates the demo layers, which is where parameters are registered,
so the table was written empty and the session recorded none of them: a valid
recording of a run whose sliders did nothing. Moved after the loop.

**Playback starts before them too, and cannot move.** `StartPlayback` runs in the
`Application` constructor because TestEnv asks the recording which scene it
belongs to before building its layers. Resolving names there finds an empty
registry and reports every parameter missing — which is exactly what the first
version did. The mapping is now resolved on the first step instead, by which time
the demos exist.

#### The measurement

Record a session whose sliders move at steps 150, 300 and 450, replay it, and
compare the captured frame. The scripted moves are gated on `IsRecording`, so a
replay that matches can only have got those values out of the file.

**Identical.** And the control — the same test with nothing registered, so the
file carries zero parameters — **differs**, which is what says the first result
is the mechanism working rather than the scene being insensitive to gravity.

One trap, in the test rather than the code. The scripted change first went in
`OnDemoFixedUpdate`, and the replay diverged. A slider does not land there: ImGui
runs *after* the fixed steps, so a drag changes the value at the end of a frame
and the next step samples it, while a change made inside the step is sampled a
step after the run used it. The mechanism was right and the stand-in was
unfaithful — suspect the measurement first.

Adopted in Physics2D and Physics3D, six and five parameters. Everything else
still records as it did; the other demos can take it one line at a time.

### 2026-08-13 (an unattended run should not take your keyboard)

Reported from use rather than found by measurement: sessions left running while
the machine is being worked at kept popping a window up and taking input focus.
Every capture and every replay did it, and a capture that takes a second still
lands in the middle of somebody's sentence.

`--capture` and `--play` are precisely the flags that mean *nobody is watching
this*, so they now create no window at all. `--show-window` to watch one happen,
`--hide-window` to silence any other run; explicit beats inferred in both
directions, and it is checked first so flag order never matters.

Hinted at creation rather than hidden afterwards. `glfwHideWindow` on a window
created visible leaves a frame or two where it is on screen holding the keyboard
— which is the entire symptom. That forced the decision earlier than the rest of
the flags: a window hint only applies at `glfwCreateWindow`, and
`ParseCommandLine` runs after the window exists, so this one reads the command
line directly in the constructor.

**A hidden window renders exactly the same**, which is the claim the whole change
rests on and so was measured rather than assumed. The back buffer belongs to the
driver, not to the compositor, and `ReadFramebufferRGBA` reads `GL_BACK` before
the swap either way. Captures taken hidden and visible are byte-identical across
Cube3D, Physics3D, Map Building and Breakout — and identical to the same demos'
captures from before the change, so nothing moved.

One line of log when it happens, because "the demo did not appear" is otherwise
indistinguishable from a crash on startup.

### 2026-08-12 (acoustics in three dimensions, and what the missing ceiling was worth)

`Acoustics2D` was 2D for one reason — the ray it stood on was — and `Raycast3D`
removed that reason three sessions ago. `Acoustics3D` is the same tracer without
the approximation its own header had been apologising for: *a room's floor and
ceiling are half its reflecting area.*

#### The split

The tracer is: spread rays over directions, cast, absorb, ask whether the
listener can see the bounce, scatter or mirror, repeat — then summarise the
histogram. **Only the first, the last-but-one and the cast itself depend on the
dimension.** Everything else moved into `Acoustics.h` / `AcousticsInternal.h`
and is shared.

That split was chosen rather than duplicating the file, because the shared part
is where the bugs live: a truncation cliff that reads as a very dead room, a
roughness window that must not measure the decay itself, impulse amplitudes that
have to be per interval rather than per bin. Each of those was a real bug once.
Two copies would be two chances to fix one of them and not the other — the same
argument that made `Terrain::HeightAt` answer from `Heightfield3D`.

Refactoring the 2D tracer means the only acceptable outcome is **no change at
all**, so it was pinned first: every scalar to nine figures, echogram bin count,
sum and centroid, every reflection, every impulse tap, over two settings. Then
the refactor, then the same run, then `diff`. Identical.

#### Three things that are genuinely different in 3D

**Rays spread on a golden-angle spiral**, not a latitude/longitude grid. A
lat/long grid crowds rays at the poles, so a room's floor and ceiling get
sampled several times as densely as its walls — which is precisely the bias 3D
acoustics exists to remove.

**Diffuse bounces draw from a cosine-weighted hemisphere**, via the
concentric-disc inversion. The height `sqrt(1 - u1)` is the whole difference: a
uniform hemisphere sends too much energy along the wall, lengthening the mean
free path and with it the decay. The 2D tracer documents the same trap in its own
form.

**The frame for that draw picks its helper axis away from the normal.** With a
fixed `(0,1,0)`, a floor or a ceiling — the two surfaces 3D adds — would cross
two parallel vectors and get a zero-length basis. A diffuse bounce that goes
nowhere, on the only surfaces that are new.

#### Checked against arithmetic it does not contain

A shoebox, because a shoebox is the one shape whose diffuse-field acoustics are
known in closed form. Neither formula appears in the tracer: it measures the mean
free path by averaging the distance between bounces, and RT60 by Schroeder-
integrating its own echogram and fitting a line.

| check | expected | traced | error |
| --- | --- | --- | --- |
| mean free path, 3D | 4V/S = 4.3636 m | 4.3463 m | **0.40%** |
| mean free path, 2D | π·A/P = 7.5398 m | 7.5345 m | **0.07%** |
| RT60, absorption 0.10 | Eyring 1.6682 s | 1.7142 s | +2.76% |
| RT60, absorption 0.20 | Eyring 0.7877 s | 0.8279 s | +5.10% |
| RT60, absorption 0.35 | Eyring 0.4080 s | 0.4443 s | +8.90% |

13 of 13, including the negative cases: a sealed room leaks no rays, `Scattering
0` scatters no bounce, and every RT60 above came from the traced decay rather
than the fallback.

The RT60 bias is one-sided and grows with absorption, which is expected rather
than wrong. Eyring assumes a perfectly mixed field, and a bounce here is heard by
the listener regardless of which way it was heading — the diffuse-rain
approximation `Acoustics2D` also documents — which finds a little more late
energy than a real specular room delivers. **10% is the accuracy of the method,
not a bug to chase.**

#### And the answer to the question it was built to ask

Same room, same absorption, traced both ways:

| | mean free path | RT60 |
| --- | --- | --- |
| 2D footprint | 7.5345 m | 1.397 s |
| 3D room | 4.3484 m | 0.828 s |

**2D over-predicts the mean free path by 1.73× and RT60 by 1.69×.** The 1.73 is
not a coincidence — π·A/P ÷ 4V/S for this room is 1.7278, so the tracer
reproduces the ratio of the two formulas to three figures. The shorter path means
more bounces per second, so more absorption per second, so a shorter tail.

#### A consumer, and what it revealed

Cube3D, which already had 3D emitters and `Raycast3D` occlusion. The nearest
emitter is traced; its early reflections go on that voice and the tail drives the
global reverb, because a tail is a property of the room rather than of a source.

Panning a reflection needed the one genuinely new piece of arithmetic: the
arrival direction is a world vector, and which ear hears it depends on which way
the camera faces, so the pan is that vector projected onto the listener's own
right axis. In 2D this was `Direction.x`, which worked only because that listener
never turned.

Then the trace said something useful about the demo: **255 of 256 rays escaped,
late/direct energy 0.00000, zero impulse taps.** Correct, and worth stating
plainly — an open floor under an open sky has no reverb, and the mechanism
already handles it honestly, because `Wet` comes from the late/direct ratio and a
`Wet` of 0 disables the effect. Only the floor's early reflections are real
there, and there were six of them.

So the demo got an **enclosure, hidden by default**: four walls, a ceiling and a
floor pan, off so the demo looks and captures exactly as it did, on so there is a
room to hear. Enclosed, the same scene traces 2,450 paths, 0 rays escaped,
late/direct 1.139, 213 impulse taps, RT60 1.638 s. The same `Visible` flag hides
them from the eye and from the ear — the advantage of tracing the scene rather
than a second set of collision boxes.

Building that room took three tries and **none of the failures looked like
geometry mistakes**:

- Walls sized to the floor's *scale* rather than half of it. The floor is a unit
  cube scaled by 12, so it spans ±6, and walls at ±12 stood six metres clear of
  its edge: 241 of 256 rays escaped through a gap all the way round. It reads as
  a short tail.
- Walls sized to the floor, correctly — which put the camera, and therefore the
  listener, *outside* the room. 0 paths from 3,042 bounces. A sealed room with
  the ear on the wrong side of the wall reads as a dead room.
- Walls butted up to the floor rather than overlapping it, leaving a 0.2 m slot
  around the base. 27 of 256 rays found it. Assemble a room from slabs and the
  joints are holes unless they overlap.

#### Two traps, one in the test and one that was already there

**The test was wrong before the code was.** It asserted that at `Scattering 1.0`
every bounce scatters, and read 175 of 205,285 as a tracer bug. A ray that runs
out of energy breaks *before* the scatter decision, so its last bounce is counted
as traced and never asked — at most one per ray, and 175 of 2,048 rays ended on
an absorbing surface. Suspect the measurement first.

**Cube3D's captures depended on where the mouse was left.** Noticed because a
capture hashed differently between two sessions with nothing in the renderer
changed. `ReadHoveredEntity` picks from the cursor and `DrawSelectionBox` draws a
wireframe round the result, with no `IsUIHidden` guard — the identical bug Map
Building's preview block had, and the identical fix. A hover is a cursor and a
cursor is UI. With the guard the hash returns to precisely its old value and no
longer depends on the mouse at all, which is what makes it a regression test
again — and matters more now that this demo has acoustics in it.

### 2026-08-12 (the broadphase was eleven times slower than no broadphase)

Last session left this named and unfixed: with terrain in the scene the uniform
grid switches itself on at 200 bodies and then costs more than having no
broadphase at all. Reproduced with no window, no camera and no person driving
the demo — the heightfield, N static one-metre blocks resting on it, sixteen
dropped spheres — it was worse than the demo had made it look:

| bodies | brute force | grid |
| --- | --- | --- |
| 117 | 0.125 ms | **1.847 ms** |
| 217 | 0.194 ms | **1.859 ms** |
| 417 | 0.478 ms | **1.951 ms** |

The interesting part of that table is not the ratio, it is that **the grid's cost
barely moves with the body count**. A broadphase whose cost is flat in the number
of bodies is not being paid for the bodies. It is being paid for the 29,575
cells, and that pointed at both faults — only one of which had been diagnosed.

#### Stamped into every cell

The known one. A heightfield's bounds are the whole map, so one body was going
into all 29,575 cells every step, and every one of those pushes went into a
vector that had been emptied moments earlier — so it allocated. Roughly 30,000
mallocs and frees a step to describe a body that rejects nothing.

The rule that fixes it is a **cost comparison, not a shape test**, because the
shape is not what is wrong: stamping a body into C cells costs C pushes and buys
the rejection of at most C pairs, so a body is kept out of the cells once C
exceeds the body count. Terrain clears that by two orders of magnitude, and a
large static floor clears it too — which is right, and is why this is not a
`if (Shape == Heightfield)`.

They are still *tested*, just not bucketed: every query adds the oversized
bodies unconditionally. That loses nothing for a body that was in every cell
anyway, and it keeps the change pure bookkeeping — the pairs reaching the
narrowphase are exactly the ones brute force would send it, in the same order.

A bounds test in front of that would reject a few more pairs and was
**deliberately not written.** A heightfield collides like a solid volume
extending downwards while its bounds describe only the band between `Lowest` and
`Highest`, so a body under the map would stop being ejected. That disagreement
is now recorded at `BodyBounds`, and fixing it is the prerequisite for the
filter, not the other way round.

#### Still sizing the grid

That alone took 217 bodies from 1.859 ms to 0.256 ms — against brute force's
0.208 ms, so **still losing**. The cell count had not moved, because the terrain
body was out of the cells but still setting the grid's *extent*: 29,575 cells
allocated to hold the ~450 that anything was in, rebuilt every step, because an
empty cell is not free.

The extent now comes from the bodies actually going into cells. That needs the
classification to happen first, which needs a cell count, which needs the
extent — so the span is estimated from the body's **own size** instead: how many
cells a body covers depends on where the origin falls only to within one cell
per axis, so its size answers the question before the grid exists. An upper
bound on purpose; being one cell out cannot matter to a threshold terrain clears
by 100×.

| bodies | brute force | grid | cells | speedup |
| --- | --- | --- | --- | --- |
| 117 | 0.125 ms | 0.103 ms | 1,105 | 1.22× |
| 217 | 0.194 ms | 0.144 ms | 1,360 | 1.35× |
| 417 | 0.478 ms | 0.227 ms | 2,205 | 2.11× |

#### It is still the same simulation

**28 checks, all bit-identical to brute force**, across three scene families —
terrain with blocks, the Physics3D arena (wide floor, tilted ramp, four walls, a
stack, mixed shapes), and jointed chains with sleeping on, which is the only
scene where joints and sleeping are exercised. Not "agrees to six figures":
every body's final position equal, after 240 steps. Both settings of the new
`BroadphaseExcludeOversized` were checked against brute force, so the switch is
a pure cost switch in the same sense `UseBroadphase` is.

#### The threshold moved for one scene and not the other

Which is the reason `BroadphaseMinBodies` is **still 200**. Three runs a count,
speedup over brute force:

| bodies | arena | | bodies | terrain |
| --- | --- | --- | --- | --- |
| 26 | 0.98 – 1.02× | | 27 | 0.89 – 0.97× |
| 46 | 1.03 – 1.15× | | 37 | 0.82 – 0.92× |
| 66 | 1.22 – 1.28× | | 67 | **0.74 – 0.85×** |
| 86 | 1.35 – 1.37× | | 117 | 0.92 – 1.05× |
| 126 | 1.53 – 1.65× | | 217 | 1.20 – 1.32× |
| 166 | 2.07 – 2.13× | | | |

The arena now wins from about 36 bodies where it used to need 123 — measured in
one scene both ways, which is the only way those two rules are comparable at
all. Terrain still does not win until past 200, and it gets *worse* before it
gets better, because the terrain body is a candidate for everything whatever the
broadphase does: the grid has nothing to reject there and pays for the rebuild
regardless. Lowering the threshold to suit the arena would put the terrain scene
back to 0.74×, which is a milder version of the bug just fixed. So the constant
stays, and the note under it now records both curves rather than one.

#### Two things worth keeping

**A flat cost is a diagnosis.** The first table's ratios said "the grid is slow";
its *flatness* said which of two mechanisms was paying, and the second fault
would have been missed without it — the obvious fix got 217 bodies to 0.256 ms
and left it still losing, which reads like success if you only look at the
factor of seven.

**No demo can reach this code with default settings**, which is why it was
measured in a scene built for the purpose rather than in the app. Physics3D caps
itself at 90 bodies, and Map Building needs about 185 blocks placed by hand
before the grid engages at all. A capture-based regression test would have shown
nothing here; the demos' captures are unchanged and byte-reproducible precisely
because they never cross the switch.

### 2026-08-12 (a ray through the map, and something to put on it)

Map Building has been named for building since it was written and did not do
any. It does now: **left click puts a block where you are pointing.** The piece
that was missing was a ray against the heightfield.

#### Walked, not sampled

`Heightfield3D::Raycast` steps from cell boundary to cell boundary and tests
only the two triangles of each cell it actually enters. The obvious alternative
— march the ray in fixed increments and look for the step where it crosses the
height — is wrong twice over: too coarse and it tunnels through ridges, too fine
and a 64 m map costs thousands of samples a cast, and no increment fixes both.
A grid the ray travels through has an *exact* answer, and because the cells
arrive in order the first one holding a hit holds the nearest hit.

The ray is clipped to the field's box first, including the band between `Lowest`
and `Highest`. That is what makes a cast at the sky cheap, and it is worth the
line: **0.138 µs aimed at nothing against 18.0 µs traversing the map**, on the
same field.

Verified against three references that know nothing about cell walking:

| check | worst error |
| --- | --- |
| 400 vertical casts vs `SurfaceAt` | 2.7e-6 m |
| oblique casts vs the analytic ray-plane crossing, 10/25/40° ramps | 3.8e-6 m |
| the hit normal vs the ramp plane's normal | 1.0e-6 |
| a grazing cast over ~30 cells vs a 2 mm march | 9.8e-4 m |

...plus the misses, which a raycast gets wrong more often than the hits: at the
sky, outside the extent, `maxDistance` stopping short, and running flat beneath
everything. 27 of 27.

One measurement trap. The grazing cast first fired from just above the map's
*lowest* sample, so it entered the map already underground; the marching
reference then reported the entry point and the raycast reported the first real
surface crossing six metres later. They were answering different questions, not
disagreeing. The test now asserts its own premise — that the ray is above the
surface where it enters — before trusting the comparison.

#### Placing

A block is remembered as a cell and a base height, not as a body handle.
`PhysicsWorld3D` has no `RemoveBody` and cannot easily grow one while a handle
is an index into a vector — removing from the middle shifts every handle after
it, including the twelve the ragdoll's joints hold. So the record is the truth,
the bodies are built from it, and undo is a rebuild. That costs the character
his footing, which is the visible price of not having the engine call.

The hit point is stepped along the hit normal before choosing a cell, so
pointing at the top of a block stacks and pointing at its side builds outwards.
Blocks rest on the *highest* terrain under their footprint, and nine samples
give that exactly rather than approximately: a block is 1 m, the terrain's cells
are 0.5 m on an aligned grid, so the footprint's corners, midpoints and centre
are all lattice points and its boundary runs along triangle edges. Measured
against a 9×9 sweep over sixty blocks, the error was **0.000000 m**. The visible
consequence is the other way round — a flat-bottomed cube on a slope floats at
its low corner, up to 0.49 m on this map.

A dropped sphere came to rest 0.0050 m below the top of a six-block stack, which
is `s_PenetrationSlop` exactly, and is how a placed block was shown to be a real
collider rather than a mesh.

Placement is settled on a **fixed step** from `Input::GetMousePosition`, not in
an event handler. The mouse is in the replay stream and events are not, so a
building session records and replays like a walk does.

#### Two things the measuring turned up

**The broadphase now loses badly in this scene, and building is what triggers
it.** The grid switches itself on at 200 bodies, and 200 placed blocks crosses
that. A/B at matched body counts, in Release:

| blocks | bodies | grid | brute force |
| --- | --- | --- | --- |
| 100 | 114 | 0.323 ms | 0.240 ms |
| 200 | 214 | **1.366 ms** | 0.427 ms |
| 400 | 414 | **1.422 ms** | 0.714 ms |

The engine's own table predicts the grid winning 1.31–1.43× at 203 bodies. Here
it *loses* 3.2×. The cause is in the same line of output: **29,575 cells**, and
the heightfield is inserted into every one of them every step, because its
bounds are the whole 64 m map. The comment on `CellSize` already states the
failure mode — "much larger and every cell holds everything, which is brute
force paying rent for a grid" — and a heightfield achieves it from the other
direction. The fix is to keep heightfields out of the cells and test them
directly; nothing here needed it enough to justify the change yet, and 1.4 ms is
still a twelfth of a frame.

**A cursor is UI.** The preview block follows the mouse, and drawing it under
`--hide-ui` made two otherwise identical capture runs produce different PNGs —
quietly costing the demo the property the whole capture-as-regression-test habit
rests on. `Application::IsUIHidden` exists now so a layer can tell, and the
preview goes when the panels go. Two runs are byte-identical again.

### 2026-08-11 (he could not climb, and it was not friction)

Every measurement of the character on terrain so far had been taken on gentle
ground — the proof being that the 25° sole clamp never engaged on any heading of
the map. So the whole range where the gait was *expected* to fail had never been
looked at, and "he probably slides above 35°" was a guess. It walked him up a
constant-slope ramp instead, one angle per run. A ramp rather than a steep patch
of the real map on purpose: a plane is exactly representable by the collider's
triangles, so nothing is confounded by triangulation error or roughness, and the
only thing that varies between runs is the angle.

The first run failed at **15°**, where statics says he should hold easily.

#### The bug: a ground probe pinned to where he spawned

`m_RootAnchor.y` is written once when control begins and only ever moved
*horizontally* afterwards, and `DriveRoot` used it as the origin for the "what
is under me" probe. Walk uphill and the origin stays behind; once the surface
rises past it, a probe for ground *below* that point correctly answers that
there is none, the −1000 sentinel comes back, and the fall test ragdolls a
character standing on solid ground.

The arithmetic named the spot before the fix went in: spawn ground 1.61 m,
anchor 2.70 m, and the 15° ramp reaches 2.70 m at z = −9.92. He went down at
z = −9.92. That is **a hard ceiling of one standing height above wherever he
spawned, at any angle** — invisible on the flat demo because a floor never
rises, and invisible on the map because the spawn picks the flattest spot going
and he had never been walked far enough uphill from it.

The pelvis is the honest origin: it cannot be inside the ground while he is
standing on it. The foot probes in `PlaceFeet` already lift their own origins a
metre for exactly this reason — the root probe was the one that did not.

#### Friction never binds on him at all

With that fixed he walks up **40° at a full 1.30 m/s with zero slip**, where a
dynamic box on the same surface would have been sliding five degrees back. The
root is kinematic, so the tangential force a slope demands is never asked of a
contact and the friction cone is never consulted.

Which means the panel line reading "he slides above atan(0.7) = 35°" was a true
fact about a *box* and a false one about him. It has been corrected in both
places it appeared.

#### What does limit him: the hip climb rate

The hips may only rise at `m_ClimbRate` = 1.2 m/s, so a sustained climb needs
`speed · tan θ ≤ m_ClimbRate` — 42.7° walking, and only **16.7° running**. The
sweep against that formula:

| | predicted ceiling | last angle that holds | first angle that caps |
| --- | --- | --- | --- |
| walking, 1.30 m/s | 42.7° | 42° (+19.27 m in 16.5 s) | 43° (+19.75 m = 1.2 × 16.5) |
| running, 4.00 m/s | 16.7° | 16° (climb 1.147 m/s) | 17° (climb **1.200** m/s) |

Both within a degree, and the capped runs sit exactly on 1.2 m/s rather than
near it. Past the ceiling the failure is gradual and then sudden: the hips fall
behind, the pelvis sinks about a metre over ten seconds, and once it dips under
the surface the probe loses the ground. Clearance at the moment of collapse was
0.13 m in every single run, which is what says it is one mechanism and not
several.

#### And whether any of it matters on the map

Not at the shipped amplitude. A census of the 129×129 map says the steepest
slope on it is **35.0°** — under the 42.7° walk ceiling everywhere — and all
eight walking bearings survive twenty seconds. 12.8% of it is above the *run*
ceiling, but running all eight bearings produced no gait failure either: the
steep patches are too short to accumulate the deficit, and he reaches the edge
of a 64 m map in eight seconds regardless. Raise the amplitude to 20 m and it
bites at once — three of four bearings go down inside the map.

So the slope-aware gait that this was meant to justify is **not worth building**
for this map. The thing that actually needed fixing was a bug.

Three measurement traps, all caught by numbers that were too tidy:

- Predicting the climb rate as `v · sin θ` rather than `v · tan θ`. `m_Speed`
  drives the *horizontal* forward vector, so the gradient is the right factor.
  Sin under-reads by 3% at 15° and 23% at 40° — exactly the size of disagreement
  that gets blamed on the simulation.
- Five running "failures" at *identical* step 641 having covered 57 m of a 40 m
  ramp. He ran off the end and fell into the void, which reads exactly like a
  gait failure in the summary line. Same trap on the real map, where all eight
  running bearings "failed" at |x| or |z| = 32 — the edge of a 64 m map.
- A slope census calling a 9 m map **81.3°** steep. `Terrain::NormalAt` took a
  central difference through `HeightAt`, which answers 0 off the map, so the
  outermost ring read as a cliff down to sea level. With the ring excluded the
  answer was 35.0°. `NormalAt` now delegates to `Heightfield3D::SmoothNormalAt`,
  which clamps its samples inside the field — one implementation instead of two,
  and the border is no longer a fiction.

### 2026-08-11 (a sole that knows what it is landing on)

The one number the terrain work left worse than its flat control. The gait aimed
the foot level with **the world**, so on any slope it landed on a heel or a toe
and only the leading edge touched.

`SolveLeg` was already turning the sole to `angleAxis(m_Facing, up)` — a yaw and
nothing else. It now composes that with the rotation taking `up` to the ground
normal at the spot the foot is *going to*, so the foot arrives already matching
rather than rolling flat once it is down.

The world had to be able to answer the question. `GroundBelow` reports the
height **and** the normal in one call, deliberately not as a second
`GroundNormalBelow`: the two have to come from the same body, and two
independent searches can disagree the moment anything overlaps — a step at the
foot of a hill — which would lay a foot flat to one surface at the height of
another.

For a heightfield that normal is a *smoothed* one (`SmoothNormalAt`, central
differences over a cell), not the face normal the narrowphase uses. A face
normal is the truth about the geometry and is constant across a triangle, so a
foot oriented by one snaps every half metre. Two normals for one surface,
answering different questions — the same split the terrain generator already
made between shading and collision.

Measured over 16.9 m of walking across generated terrain:

| sole follows slope | mean sole tilt | worst |
| --- | --- | --- |
| 0.0 — the old behaviour | 9.91° | 35.28° |
| 0.5 | 4.91° | 15.90° |
| 1.0 | **1.96°** | 9.19° |

For comparison the same figure on a flat floor is 1.17°, so conforming gets
terrain to within a degree of a slab. Standing still on a slope it is 0.14°.

The tilt cap was swept too, and the interesting result is that it is **inert**:
25°, 35° and 45° give identical numbers to six figures, because nothing on this
map is steep enough where he walks to reach the clamp. It stays at 25° — inside
the ankle's 35° cone — as a guard for steeper ground, at no cost here. Below 15°
it starts to bind and the mean climbs back to 2.25°.

On flat ground the conform is exactly the identity rotation, and the A/B proves
it: walking and standing on the slab produce **identical** readings with the
feature on and off, down to the last digit of the run where he walks off the
edge of the 16 m floor.

One measurement trap worth recording. The first metric was "how far is the
lowest corner of the sole off the ground", and on the *flat* demo it read
0.2978 m — which is the demo's own 0.30 m step passing under the footprint, not
sole flatness at all. A gap measured under a foot is dominated by whatever
discontinuity the ground has there. The angle between the sole's normal and the
ground's does not have that problem, and is what the table above reports.

### 2026-08-11 (something to stand on, and someone to stand on it)

The character from the Ragdoll demo now walks on the generated map. Two pieces:
a **heightfield collider** in the engine, and Map Building becoming a *subclass*
of Ragdoll rather than a second copy of the rig.

#### The collider

`ColliderShape3D::Heightfield` — static only, never turned, since a collider
that is a height as a function of `(x, z)` stops being one the moment it tilts.
`EGSS/src/Egss/Physics/Heightfield3D.h` holds the samples and the surface;
`PhysicsWorld3D` gained a narrowphase case per shape and an exact ground probe.

Every shape reduces to the same thing — a handful of places where it might be
touching, then one routine that assembles a manifold:

| shape | query points | why |
| --- | --- | --- |
| sphere | the centre | closest point on the nearby triangles |
| capsule | the segment, sampled every half cell | the field cannot hold a feature narrower than a cell, so nothing hides between samples |
| box | its eight corners, vertically | a box has no radius, so "closest point" only answers once it is already through the surface |

The constraint worth knowing about is that a `Contact3D` carries **one normal
for all of its points** and terrain does not — two triangles under one foot face
different ways. So the deepest point's normal is adopted and every other point's
depth is re-measured along it. One contact *per triangle* was the alternative
and is wrong here: the solver keys warm starting on the body **pair**, so
several contacts between the same two bodies would throw each other's impulses
away every step.

#### It was a different surface than the one being drawn

The terrain answered height queries with **bilinear** interpolation and the
renderer draws **triangles**. Those are not the same surface. They meet at the
four corners of a cell and along its diagonal and nowhere else, differing by
`fx·fz·(h00 − h10 − h01 + h11)`. Measured on this map that reached **1.75 cm** —
a foot-thickness of hover, invisible in any screenshot.

So `Terrain::HeightAt` now answers from the collider, and the collider walks the
same triangles in the same winding. One surface, three consumers.

Verified against arithmetic the collider knows nothing about — 15/15:

| | |
| --- | --- |
| collider height vs. the mesh's own triangles, 3992 samples off the lattice | worst **0.0000014 m** |
| collider normal vs. the mesh's face normals | worst **0.000000°** |
| `GroundHeightBelow` vs. the surface | worst **0.000000000 m** |
| sphere of radius *r* resting on a plane at *h* | centre at `h + r`, off by 0.005 (the solver's slop, exactly) |
| block on a 30° slope, μ = 0.7 | holds — `tan 30° = 0.58 < 0.7` |
| block on a 40° slope | slides |
| block on a 45° slope for 1.5 s | predicted 2.339 m by `½g(sin θ − μ cos θ)t²`, measured 2.371 — **1.4%** |
| contact points under a settled foot | **4.00** |

The slope tests are the strongest of these because statics supplies the answer
and nothing in the solver has ever heard of `tan θ`.

**All three failures on the first run were the measurement.** A block that slid
at 30° was tilted the wrong way — a rotation of `+θ` about `+x` leans a box the
opposite way to the normal of a ramp rising with `z`, so it was balanced on one
edge and would have slid at any friction. And a stubborn `0.0396°` normal error
printed two normals that were *identical to every digit*: `acos` is
ill-conditioned at 1, and turns a dot product a few ulps below it into
`sqrt(2ε) ≈ 0.02°`. Measured as a chord — `2·asin(|a−b|/2)` — it reads zero.

#### Map Building is a subclass, not a second rig

The rig is thirteen bodies, twelve joints, a gait, a balance controller and
about a hundred and fifty constants that were each measured. A second copy would
be a second thing to keep tuned. `Ragdoll` grew five hooks — `BuildGround`,
`SpawnPoint`, `ClearColour`, `SetSceneLighting`, `DrawWorld` — and Map Building
overrides exactly those.

Nothing in the gait needed touching, and that is a property of the rig rather
than of the new demo: every question it asks about the ground already went
through `GroundHeightBelow` instead of assuming a floor at zero.

#### A coordinate bug that only terrain could reveal

The figure stood on the map with its soles **26.4 cm** in the air. The flat demo,
run as a control, read 0.9 mm — the solver's slop.

`m_KneeAt` and `m_AnkleAt` are stored in the **rig's own frame**, because the
joint helpers add the origin themselves. Three lines downstream used them as
world positions: the thigh measured from the hip to a point near the map's
centre, and the foot targets were computed against ground on the far side of the
map. With the rig built at the world origin the two frames are the same numbers
and nothing is wrong. Build it on a hill and the legs reach for somewhere the
character is not.

It read as a terrain bug for an hour. Corrected, walking 22 m across the map:

| | flat | terrain |
| --- | --- | --- |
| planted sole above the surface, worst | 0.0054 m | **0.0461 m** |
| pelvis off `StandClearance`, worst | 0.0713 m | **0.0723 m** |

The pelvis tracks generated ground exactly as well as it tracks a slab. The sole
figure is worse on terrain and honestly so: the foot lands flat to the *gait*,
not to the local slope, so its lowest corner sits above ground on a hillside.

Two things this does not do. Walking off the edge of the map is a real fall —
the field reports no ground outside its extent on purpose — so Map Building
rebuilds the scene once the figure is 20 m below the lowest point. And a box
wider than a cell straddling a peak can have every corner above ground while the
peak pushes through its underside; nothing here is in that position (a foot is
0.22 m across a 0.5 m grid) and the fix is to add the cell corners as query
points too.

### 2026-08-11 (a map you can ask for by number)

First piece of **Map Building**: terrain described by a seed and nothing else.
Type the same number in and the same map comes back, on any run and in any
build. `TestEnv/src/Terrain.h` is standalone -- any demo can include it.

Value noise summed over octaves. The part that makes it replicable is that the
noise is a **hash of the lattice coordinate**, not a random number generator: a
generator carries state, so the value at a point would depend on how many values
were drawn before it, and changing the loop order or adding an octave would move
every height downstream. A hash has no memory of how you arrived.

Checked against things the generator does not itself compute:

| | |
| --- | --- |
| same seed twice | `2B46F2A8` and `2B46F2A8` |
| a different seed | `F48075CF` |
| the first seed again, after generating others | `2B46F2A8`, unmoved |
| `HeightAt` against the stored grid | worst **0.000000 m** |
| mesh vertices against `HeightAt` | worst **0.000000 m** |
| mesh normals against a numeric gradient | worst **0.03°** |

The last two matter more than they look. `HeightAt` is what the character will
stand on and the mesh is what gets drawn, and they are built by different code;
if they disagree, feet hover or sink and nothing in a screenshot would say why.
The normals are compared against a gradient taken numerically at a different
step size, so it is a genuine second opinion rather than the same arithmetic
run twice.

**Both of those checks were weaker than they looked** — see the next entry.
"Mesh vertices against `HeightAt`" samples only the lattice, where a bilinear
patch and a pair of triangles agree by construction; off the lattice they
differed by up to 1.75 cm. And the 0.03° normal figure was `acos` losing
precision near 1, not an angle: measured as a chord it is zero.

Worth knowing: a 9 m amplitude produced 1.996 m to 6.901 m of actual relief.
That is not a bug -- summing independent octaves concentrates the result near
the mean, so the amplitude is the range the field *could* reach rather than the
one it does.

#### The shader could not light it

The map came out flat and nearly black. The `Physics3D` shader everything else
shares has a point light with `1 / (1 + 0.015 d²)` attenuation, which is right
for a scene a few metres across: at 70 m it contributes **1.3%**, so the terrain
was lit by ambient alone.

Outdoors wants a sun -- a direction, no position, no falloff -- so Map Building
carries its own shader, with a hemisphere sky term as well. Flat ambient makes
every slope the same shade, which is the one thing terrain must not do.

### 2026-08-11 (leaning the right way, a hurdle, hands on the floor, and staying up)

Four things that read wrong to the eye, and every one turned out to have a
specific cause rather than needing polish.

#### Leaning backwards

The figure faces **+z**, and the lean was applied as `angleAxis(-lean, +x)`,
which tips the body's up axis towards **−z**. Measured before touching it:
**−5.7° walking and −17.6° running** — leaning back, and the −17.6 is exactly
`(4.0/1.3) × 0.10 rad`, the right magnitude pointing the wrong way.

The second hypothesis was wrong and worth recording. The spine joint's handle
was being discarded and nothing drove the torso, so the theory was that it
trailed backwards under acceleration. It does not: the torso tracked the pelvis
to within **0.1°**, because the spine motor at 260 N·m is easily strong enough
to carry it rigidly. The sign was the whole of it.

Driving the spine was still worth doing, for a different reason — a rigid
whole-body tilt is a plank falling over, not a person leaning. The lean is now
split between pelvis and spine, and gained a second term that is not a taste
setting: a body accelerating at `a` must put its weight `atan(a/g)` ahead of its
feet or the acceleration has nothing to come from.

| | torso | pelvis | spine bend |
| --- | ---: | ---: | ---: |
| walk | +4.0° | +1.8° | 2.2° |
| run | +12.3° | +5.5° | 6.8° |

Accelerating at 3.00 m/s² the torso peaks at **+19.6°** against `atan(a/g)` of
17.0°, the difference being the speed term arriving. Braking reaches **−10.4°**,
which is what stopping looks like.

**The sway had the same disease**, and measuring caught something reasoning
would not have: it was *inverted at a walk and correct at a run*. Tied to
`sin(gaitPhase)`, which foot is carrying at a given phase depends on the duty
factor — the pelvis sat 0.1033 m from the stance foot and 0.0955 m from the
swing foot walking, leaning away from the leg holding it up. Keyed to the cycle
position instead it is right for both gaits by construction: **0.0937 vs 0.1084
walking, 0.0919 vs 0.1091 running.**

A prediction of mine that did not come true: leaning forward was supposed to eat
the balance margin. It did not — −0.6105 against −0.6258. The capture point is
dominated by the CoM's *velocity*, not by the small offset from leaning.

#### A hurdle instead of a hop

Both legs got the same flight pose, so every jump was a two-foot hop. Now the
takeoff waits for a foot to be under him, the leg that is next off the ground
swings through in front, and the pose opens with speed.

| | feet apart in flight | lead leg |
| --- | ---: | --- |
| standing | 0.015 m | none, a hop |
| walking | 0.640 m | correct |
| running | 0.584 m | correct |

Two things had to be got right. The wait **cannot be unconditional** —
`m_GaitPhase` is pinned to zero below 0.05 m/s, so standing still there is no
plant coming and a press would hang forever. And landing resets the gait phase
so the lead foot is the one arriving, which lands *into* the next stride and
does the job a per-foot landing timer was going to do without needing one.

The cost, measured across every press timing rather than one lucky sample: the
wait averages **0.15 s and is at worst 0.37 s** walking, 0.10 / 0.23 running.

An assertion of mine failed twice before the code did. Checking the lead leg
"was swinging at the press" is the wrong instant — there is a wait and a windup
in between. Checking it at the push-off is *also* wrong: at a walk the moment a
foot plants is double support, so the other leg has another tenth of a cycle
before it lifts. What must be true is that the lead leg is the **next** one to
leave the ground, and it is.

#### Getting up over the hands

The arms did nothing. `SolveLeg` was already generic two-link IK with the thigh
and shin baked into it, so it became `SolveLimb` and the arm is the same solve
mirrored at the elbow — the knee and elbow ranges are `(0, 130)` and `(−140, 0)`
for exactly that reason.

The pelvis now travels through **three** waypoints rather than two: up onto the
hands, back over the feet as they gather, and only then up. A person does not
rise from where they are lying, because there is nothing under them there.

| | hands reach | weight from over the feet at the rise | before |
| --- | ---: | ---: | ---: |
| face down | −0.004 m | **0.130 m** | 0.208 m |
| face up | 0.011 m | **0.084 m** | 0.169 m |

#### Not falling over so easily

The old decision was two invented force numbers, and past 2000 N it was an
instant collapse regardless of direction or whether a foot was even down.

**An impulse is a velocity, and that is the whole model now.** A hit of `J`
newton-seconds on 71.2 kg changes the velocity by `J/m`; a body toppling about
its feet is an inverted pendulum with `τ = √(h/g)`; its capture point is `v·τ`
ahead; a step of reach `R` catches `v = R/τ`. One step catches **1.50 m/s** and
he gets about two and a half of them.

The first idea was wrong and measuring killed it. Testing the capture point
against the support polygon cannot work, because **running normally puts the
capture point 1.16 m from the nearest foot** — further than any shove that used
to fell him. Running *is* controlled falling and is supposed to look like that.
The criterion has to be about the *unintended* velocity only.

| impulse | 60 | 120 | 200 | 280 | 340 | 400 Ns |
| --- | --- | --- | --- | --- | --- | --- |
| forwards | up | up | up | **down** | down | down |
| backwards | up | up | up | **down** | down | down |
| sideways | up | up | **down** | down | down | down |

The transition is exactly where the arithmetic puts it: 3.76 m/s × 71.2 kg =
268 Ns, between 200 and 280. Sideways gives out sooner because a leg swings a
long way fore-aft and barely at all across the body — 0.26 m of reach against
0.50 — which is why a shove from the side puts people down that a shove from
behind does not. `X` at its default leaves him standing; at full strength it
still fells him, and 2000 Ns still fells him.

Crouching while recovering is in there too, and it is not a flourish: taking
height out of `h` shortens `τ`, which shortens the reach the feet have to make.
It is the one thing a body can do about its own balance without moving its feet.

#### Two bugs found by the harness, both real

**The stagger survived getting up.** Nothing cleared `m_StaggerVelocity` when
the character stood, so whatever felled him was still there the instant he was
upright and felled him again. It survived a scene rebuild too, which is how it
was caught: every case after the first fall reported a fall.

**Facing survived a rebuild.** `m_Facing` is only ever written when getting up,
so pressing reset after walking a while left a rig built facing +z while the
gait placed its feet for wherever he had been going — left and right swapped and
the legs crossed. It showed up as the standing knee wandering between 4° and 31°
instead of sitting at 5°, and it was three "regressions" at once: the knee, the
supine get-up, and a standing jump that came apart. Running the standing check
*first* rather than last was what separated a real regression from state left
behind by an earlier test.

### 2026-08-10 (arms, a run, a jump, a stagger, and getting up properly)

Five additions to movement. The two that were expected to be hardest were not.

#### Arms

The shoulder and elbow joints existed and their handles were being thrown away,
so nothing drove them. They swing **antiphase to the leg on the same side** —
which is not decoration: the legs throw angular momentum about the vertical axis
every step and the arms are what cancels it. Measured against the same-side
thigh, the correlation is **−0.90**; in phase would be +1, and is a toy soldier.

#### The run is not a fast walk

The difference is the **duty factor** — what fraction of its cycle a foot spends
on the ground. The gait had this hard-wired at exactly half by a bare
`cos(phase) < 0`, and half is neither gait: a walk *overlaps* (both feet down
through the hand-over) and a run has *gaps* (neither foot down at all).

| | duty | double support | flight | cadence |
| --- | ---: | ---: | ---: | ---: |
| walk | 62% | 25% | 0% | 2.2 steps/s |
| run | 36% | 0% | **27%** | 3.3 steps/s |

The flight arc is evaluated from the phase rather than integrated, so it lands
exactly when the next foot is due instead of whenever gravity gets round to it,
and its height is not a tuning knob — a flight of `T` seconds is `gT²/8` tall.

The trap worth recording: a running stride is 2.4 m, and a quarter of that would
put the foot 0.60 m in front of the body, which needs the hip **0.23 m** lower
to be reachable at all. That is overstriding, which is a braking force, not a
run. Real running gets its length from the flight phase, so the foot reach is
capped and the stride grows past it.

#### The jump

With a kinematic root a jump is not a force on a mass — it is the height being
integrated ballistically instead of pinned to the ground. Crouch, launch,
parabola, absorb.

Checked against arithmetic the code does not contain, `2√(2h/g)`:

| asked | flight measured | flight predicted |
| ---: | ---: | ---: |
| 0.35 m | 0.533 s | 0.534 s |
| 0.55 m | 0.667 s | 0.670 s |
| 0.80 m | 0.800 s | 0.808 s |

The apex reads about 5% low — 0.523 m for 0.55 — and that is not an error to
chase: it is the half-step `v·dt/2` that an explicit integrator loses, and it
comes out at 0.028 m against a measured 0.027.

#### Catching himself

The complaint was that the character ragdolled abruptly and just fell, and the
cause was a single threshold: a shove was either nothing at all or a collapse.
There is now a band between them. A hit inside it pushes the *root*, which is
what being off balance is, and the legs chase it — they are placed relative to
the root already, and the gait phase advances with distance covered, so being
shoved fast makes the steps quick without being told to.

| shove | outcome | moved | caught in |
| ---: | --- | ---: | --- |
| 500 N | nothing | 0.00 m | — |
| 900 N | stayed up | 0.13 m | 0.53 s |
| 1400 N | stayed up | 0.58 m | 1.08 s |
| 1900 N | stayed up | 1.04 m | 1.33 s |
| 2100 N | went down | — | — |
| 4000 N | went down | — | — |

Monotonic in both distance and recovery time, and the negative control holds:
past the ragdoll threshold it still always goes down.

#### Getting up rather than being placed

The old get-up lerped the pelvis from where the body lay to a standing pose in
one move, so the feet arrived under the hips at the same instant the hips
arrived at standing height. That is not getting up, it is being placed — which
is exactly what it looked like.

It is two stages now. The hips come up only as far as **kneeling** while the
feet are collected underneath them, and only then straighten, so the rise
happens over feet that are already planted. Measured as how far the feet are
from under the hips at the moment the rise takes over:

| | when he fell | when he started to rise |
| --- | ---: | ---: |
| face down | 0.945 m | 0.208 m |
| face up | 0.650 m | 0.169 m |

Two things had to be fixed to get there. Strength was being blended back over
the whole gather, so it arrived just as the gather ended and one leg was still
0.79 m out of place; it now returns at twice that rate. And the feet were being
*dragged* — a foot slid along the floor catches on its own friction and stays
where it fell — so they lift clear on the way, which is what a person does.

#### A bug of mine, and a measurement of mine

The prone/supine test asked the pelvis's local **+y**, which runs up the body
towards the head. For a figure lying flat that is horizontal, so its sign was
noise and the branch it picked was a coin toss. The figure faces local **+z**.

And the test that was meant to exercise both branches shoved him over and
checked which way he landed — he lands on a *side* every time, `front.y ≈ 0`, so
neither branch ran. The pose is set outright now.

#### Unlooked-for

The support polygon item is closed, and not by what was predicted. It was
written off as needing a speculative margin in the narrowphase; a real stance
fraction and a sole flat to 0.9° got there instead. A planted foot reports
**4.53 of its 4 corners** where it used to report 1.84, and stance slide fell to
**0.0049 m/frame**.

Running slides more — 0.0298 m/frame — but a foot dragged at 4 m/s would be
0.0667, so it is still 55% better than nothing, and a running foot is genuinely
on the ground briefly and pushing hard.

### 2026-08-10 (a walk with a human cadence, and a floor that ends)

Three reports, and the interesting one was not the one that sounded hardest:
the ragdoll stood with bent knees, the walk did not look human, and walking off
the edge of the map did not make him fall.

#### Standing in a half squat

Mine, from the previous session. The crouch was there to give the IK reach
slack, and 0.09 m of it. On a 0.48 m thigh and a 0.42 m shin that works out at
**51.8° of knee bend** — near full extension the knee angle is violently
sensitive to reach, about 12° per millimetre, so nine centimetres is a squat.

The obvious fix — give it a *little* slack instead — was worse, and measuring
it was worth more than the fix. At crouch 0 the standing knee is dead steady at
5.1°. At 0.004 it swings **−38° to 64°**; at 0.006 it reaches **150°**, through
both ends of a limit that runs 0 to 130. That same 12°/mm gain turns the
kinematic pelvis's own micro-motion into thrash.

Lowering the IK's extension cap to reduce the gain made it worse again, which
killed the theory: at cap 0.99 the standing knee swung to −148°. The leg was
then being asked for a reach it could not deliver, and the motors fought the
floor.

What works is the configuration that has **no** slack: standing demands very
slightly more reach than the leg has, the reach clamp holds the knee target
constant, and the knee rests against its own extension stop. Which is how a
person stands — a straight leg is a mechanical stop, not a balance point.
**5.1°, and it does not move at all.**

#### The walk was taking seven steps a second

The bob was inverted. Its own comment said "lowest when both feet are down,
highest at mid-stance over a straight leg" and the code did precisely the
opposite — `-(1 + cos 2φ)` is at its minimum at φ = 0, which is mid-stance. So
the body sank over the straight leg and rose as the legs splayed, which is the
shape of a waddle. The heel-strike nudge was firing at mid-stance too, with
neither foot anywhere near landing.

The dip is also not a style parameter. With the feet half a stride apart the hip
*cannot* stay at full leg length — the leg is the hypotenuse of a triangle whose
base is half the stride — so it is `L - sqrt(L² - half²)`, it grows as the square
of the stride, and it is now computed rather than guessed. The measured pelvis
travel, 0.072 m, is that 0.051 plus the 0.008 style bob plus the 0.012 heel
strike.

But the real defect was arithmetic nobody had done. Stride was 0.45 m per cycle
at 1.6 m/s:

| | this walk | a person |
| --- | ---: | ---: |
| cadence | **7.1 steps/s** | 2.0 |
| step length | **0.225 m** | 0.70 |

Three and a half times human cadence, in steps a third the length. No amount of
bob or sway rescues that. The cause was a factor of two: one cycle is two steps,
so the foot should land a **quarter** of a stride ahead of the body, not half.
Placing it at half forced the stride down to 0.45 m to keep the target
reachable, and the cadence followed.

At a quarter, a 1.2 m stride at 1.3 m/s gives **2.2 steps a second at 0.60 m a
step**, needing a 0.051 m hip drop — all three human numbers at once.

It also made the feet plant far better, which was not the point of the change:
**0.0036 m of slide per frame against 0.0106 before**, and 0.0267 for a foot
simply dragged along. Longer steps keep a foot down for more frames and keep the
leg in a comfortable part of its range. The sole came flatter too, 2.6° → 1.0°.

#### The floor ends

`GroundHeightBelow` takes a floor to fall back on and it defaults to zero, so off
the edge of the world the probe cheerfully reported y = 0 and the character
walked out over the void on an invisible floor. Probed against a floor far below
instead, "there is nothing here" becomes answerable, and no ground within 1.5 m
goes through the same door the shove and the impact use. He goes limp at the
edge — measured at z = −8.01, and the floor ends at −8.0 — and falls.

#### Three measurements that lied

Worth recording together, because they were all the same mistake. A peak taken
over a whole run caught the spawn transient and reported a 0.04 m foot lift as
clearing *more* than a 0.10 m one; skipping the settle, the leg tracks its
target to within a millimetre. A single sample of the standing knee caught an
oscillation at a random phase and read anything from −2° to 17° for the same
settings. And a gait measured while walking *at* the 0.30 m step reported a
0.349 m bob. Suspect the measurement first.

### 2026-08-10 (a flat sole, and asking about the step where the foot is)

Two asks: lay the foot flat on flat ground, and raise the knee when stepping
onto something.

#### The sole was not flat because it was chasing

The ankle was 4.4° off level through the stance, worst 13.5°. Three candidate
causes, and measuring killed two of them outright. The cone limit was **never
engaged** — 0% of stance frames. Torque was irrelevant: ten times the budget
changed the tilt by nothing at all, identical to four significant figures, so
the ankle was nowhere near saturating.

What was left was lag. The ankle motor holds a target *relative to the shin*,
and the shin turns all through the stance — so aiming at where the shin is now
is aiming at where it was by the time the solver runs, and the sole trails the
lean by roughly the shin's rate over the motor's stiffness. Integrating the shin
forward one step before building the target halves it: **4.4° → 2.6°**, worst
13.5° → 9.9°.

One step, specifically. The lag is exactly one:

| lead | tilt |
| ---: | --- |
| 0 steps | 4.4° |
| **1 step** | **2.6°** |
| 2 steps | 3.7° |
| 4 steps | 9.2° |
| 6 steps | 15.9° |

Stiffening the ankle was tried on the theory that it carries a 1 kg foot rather
than the body's weight, and it measured **worse** — 40 gives 2.6°, 80 gives 3.2,
160 gives 8.3. A motor is a velocity constraint solved once per step, so past
roughly `2/dt` it overshoots and rings. That is the same ceiling the leg
stiffness sweep hit, from the other direction.

The residual is spread evenly across the stance — 3.0° just after landing, 2.6
carrying, 2.1 pushing off — so it is not a landing transient that the average
was hiding.

A planted foot still reports only 1.84 of its 4 corners, and that is not the
ankle: the manifold drops every clipped point that is not behind the reference
face, with no tolerance, so four points wants the sole flat to about a fifth of
a degree. A speculative margin would be the honest fix and it would not even
help here — at 2.6° the far corner of a 0.22 m foot is 10 mm up, outside any
margin worth having. Left alone rather than perturbing every stack in the engine
for a metric.

#### The knee already lifted; the question was asked in the wrong place

Measuring first was worth it, because the lift was already there and working:
walking at the 0.30 m step against walking away from it, the foot clears
**0.096 m → 0.317 m**, the thigh swings up **47.7° → 70.0°**, the knee joint
itself rises **0.571 m → 0.759 m**, and the pelvis ends up on top at 1.300.

So the fix was not to build a lift. Two wrong turns before finding what was:

An extra "tuck" was added — pull the swing foot in towards the hip while it is
up, on the reasoning that two-link IK given a foot held far out in front folds
the shin back under a thigh that has barely moved. It measured as doing
**nothing**: knee height 0.759 m with the tuck and 0.759 m without. The
reasoning was wrong, so the code came out again.

Then the timing test said the lift began only **0.19 m** short of the riser,
less than half a stride — which would read as a stumble and a recovery. That was
the test. It skipped 60 warm-up steps and then reported the first frame it
looked at as the moment the lift began; the lift had started well before. A
number that disagrees with the geometry is the measurement about as often as it
is the code.

The real defect was **where the question was asked**. A single probe, a stride
ahead of the *root*, along the way the character was *facing* — and facing lags
the direction of travel while turning, so walking onto a step out of a turn
pointed the probe off to one side for the whole approach. It is now per leg, at
the foot's own landing spot, which is already computed against the ground that
is actually there, plus another stride beyond it so the warning is not halved.
Latched for the length of the swing, because the target is recomputed every
frame and a foot that clears the riser otherwise stops seeing a step and drops
its lift halfway over — catching the thing it had just cleared.

Walking straight in, the lift begins **0.40 m** short of a 0.45 m stride.
Turning into the step, **0.42 m**, clearing 0.304 m. Standing still is unchanged
at 30 s and the ragdoll still collapses to com 0.098 m.

#### And a bit of sediment

The demo's constructor had accumulated **nineteen** stray `BuildScene()` calls,
one left behind by each test block deleted over previous sessions, with a live
`m_SteppingEnabled = false` buried in the middle of them. Nineteen redundant
rebuilds of the whole scene at startup. Now one.

### 2026-08-10 (feet that plant, and a leg that was too long to walk on)

The gait swung the hips and bent the knees on a sine wave and let the feet fall
where they might. From a distance that reads as walking; up close the feet slide
through the whole stance, because nothing was holding them anywhere.

Now the **foot** is what the gait describes and the leg is solved to reach it.
Through the stance half a foot is pinned to the spot it landed on and the body
travels over it — which is what walking is — and through the swing half it arcs
to where it will land next. Two-link inverse kinematics turns that world
position back into a hip direction and a knee angle: the law of cosines twice,
solved in the pelvis's frame where the knee's hinge axis is +x.

#### The leg was exactly too long

The first attempt slid *worse* than doing nothing: 0.033 m a frame against
0.027 for a foot simply towed along at walking speed. Tracing the target against
the actual foot showed the leg fully extended and the foot **0.4 m short** every
stride.

Two reasons, both geometric, and neither visible without the trace.

The foot target was placed on the **ground**, but the ankle joint rides 0.10 m
above the sole, so every target was buried in the floor.

And the rig stood with its legs **dead straight** — the hip 0.90 m above the
ankle, on a leg of exactly 0.90 m. There was no slack at all: the foot could not
be placed a centimetre in front of the hip without the target going out of
reach, and the IK dutifully extended the leg as far as it went and gave up. A
person walks with slightly bent knees for exactly this reason, so the character
does now, and the stride was cut to something the leg can actually cover.

#### And then it was a tracking problem

Placing the target correctly got the slide to 0.0218 m a frame, which is still
most of the way to simply dragging. The target was right; the leg was not
keeping up with it.

| leg stiffness | torque | slide per frame |
| ---: | ---: | --- |
| 14 (the body's own) | 1x | 0.0218 m |
| 40 | 1x | 0.0196 m |
| **40** | **4x** | **0.0107 m** |
| 90 | 1x | 0.0377 m |
| 90 | 4x | 0.0769 m |

The legs are now driven harder than the rest of the body. Stiffer is not
monotonically better — at 90 it oscillates and the slide goes back up past where
it started — which is the same shape of answer the motor stiffness sweep gave
for standing.

Final: **0.0106 m a frame against 0.0267 for a dragged foot**, a 60% reduction,
with the controlled mode still standing for 30 s and the ragdoll still
collapsing.

#### What is still not right

The support polygon has three or more contact points only **15%** of the time.
The foot holds its ground but often meets the floor on an edge or a corner
rather than flat, so it is planted without being properly *placed*. That is
invisible with a kinematic root and would be the first thing to fix for footstep
sounds, footprints, or anything that wants a clean contact event.

### 2026-08-10 (WASD, and a camera that follows)

The character was on `IJKL` because `WASD` was flying the camera. With a camera
that follows, `WASD` is free, which is the arrangement anyone would expect.

**Movement is camera-relative.** `W` goes away from the camera whichever way it
is pointing, not along a fixed world axis. Steering in world space while the
camera orbits is the thing that makes a follow camera feel like it is fighting
you.

**The camera follows the torso, not the pelvis.** The pelvis carries the bob and
the sway deliberately — it is where the weight of the walk comes from — and a
camera pinned to it inherits all of that and makes the picture seasick. It also
eases towards its target rather than tracking it exactly, at a rate per second
rather than per frame, so the lag does not change with the frame rate.

It sits back along its own forward vector, so pointing it at its own yaw and
pitch looks exactly at the focus with no aiming arithmetic. Pitch is clamped
short of straight down, where an orbit camera flips and loses its horizon, and
the position is lifted clear of the ground with the same `GroundHeightBelow`
probe the walk uses — otherwise backing into a rise puts the camera underground
looking at the inside of the world.

`F` restores the old fly-around for inspecting things, and borrows `WASD` back
while it is on.

### 2026-08-10 (getting up, ground under the feet, and a leg that sees the step)

Three additions to the controlled character, and one new engine call.

#### Getting up is a blend

`G` used to restore control and snap the pelvis back to standing wherever it was
lying. Now the pelvis becomes kinematic immediately -- it has to be drivable to
be blended -- and is carried from where it came to rest back to standing over
about a second, on a smoothstep.

The limbs are not animated at all. The motors come back up from limp over the
same period and *drag* them into the pose, which is why the arms trail and
settle rather than snapping into place. It is the cheap version of blending into
a get-up animation, and most of what sells it is simply that it takes time.

Two details that matter more than they look. It stands on **whatever is
underneath**, not at the height of the floor it started on, or getting up on a
step sinks into it. And it keeps whatever heading it ended up facing rather than
the one it fell from -- standing up facing the way you fell reads as a stumble
recovered, snapping back to the old heading reads as a rewind.

Measured: lying at pelvis y 0.701, up to 1.090 over 1.12 s, ending in control.

#### The ground moves, so the character does

New in the engine: `PhysicsWorld3D::GroundHeightBelow`, the highest surface
under a point. It tests **world axis-aligned bounds** rather than actual shapes,
which for the boxes it exists to stand on is exact, and for a rotated box reads
slightly high -- the right way to be wrong for a ground probe, where a
centimetre high is invisible and being late is a character sinking into a step.
Dynamic bodies are excluded: a crate that wandered underfoot is a fair question
and a different one.

The walk takes two probes. One under the root sets how high the character
stands; the stand height is **eased** towards it rather than following it,
because the probe is a step function at the edge of a box and following it
exactly makes the hips jump the height of the step in a single frame.

Measured walking onto a 0.30 m platform: stand height 1.090 m on the flat,
**1.390 m** on the step -- exactly the clearance plus the step -- and back down
the other side.

#### And a leg that lifts for it

The second probe looks a stride ahead. Where the ground there is higher, the
swing leg lifts more: the knee bend and the hip swing both scale with how much
higher the next foothold is, up to a step of 0.35 m.

Measured: on the approach the knee target peaks at **1.75 rad against the flat
gait's 0.90** -- the leg lifts nearly twice as high, and starts doing so before
the foot would have met the riser rather than after.

The clearance the character stands at is read off the pose the rig was built in
rather than written down a second time, so changing the skeleton's proportions
does not silently leave it hovering.

### 2026-08-10 (weight, momentum, and a knee that bent the wrong way)

Three complaints about the walk, all fair, and one of them a real bug.

#### The knees bent backwards, and the limits only allowed that

The knee's range was built as `(-130, 0)` degrees. Measuring which way that
actually sends the foot settled it, and the first attempt at the measurement was
wrong in an instructive way: the expectation was written down as "the figure
faces -z". It does not. Its toes stick out towards **+z** — the foot box spans
-0.08 to +0.14 in z — so the figure faces +z, and a knee bend that sent the foot
to +z was sending the heel *forwards*.

The range was not merely reversed, it was **exclusively wrong**: the joint could
only hyperextend and was blocked from bending the way a knee bends. It is now
`(0, 130)`, with the gait and the swing-leg lift following it, and the elbow --
which is the knee's mirror -- is now `(-140, 0)`. Measured after: the foot
finishes 0.165 m *behind* the shin.

Worth noting the near miss. An earlier entry said the anatomical direction of
the knees and elbows had not been checked and was worth thirty seconds with the
demo. Nobody did, and it survived until it was visible in motion.

#### Momentum

Velocity and heading were both being set outright from the key state, which is
most of what makes a character feel like a cursor. Now:

- speed eases towards its target and coasts down, faster to brake than to
  accelerate, as people are — 0 to 1.6 m/s in about 0.55 s, and stopped 0.27 s
  after the key is released;
- the heading turns towards the input at a limited rate, the short way round —
  4 rad/s, so a half turn takes about 0.8 s;
- the figure walks along its **facing** rather than along the input, so a turn
  is a curve rather than a sidestep.

#### Weight

Three things sell a walk as having mass, and none of them are the legs. All are
driven off the gait phase so they cannot drift apart from the stride:

- **Bob** — lowest when both feet are down and the weight transfers, highest at
  mid-stance over a straight leg. Twice per stride.
- **Sway** — the hips move over whichever foot is carrying, once per stride.
  Without it a walk looks like a puppet slid along a rail.
- **The drop** — a short, sharp downward nudge as the foot lands. This is the
  one that actually reads as the step *hitting* the ground; a smooth sinusoid
  on its own feels like floating.

Plus a small forward lean with speed and a roll with the sway, because a
perfectly upright pelvis is the other half of looking weightless.

Tuned against a person rather than by eye. Gait studies put the hips at roughly
4-5 cm of vertical travel and 2-3 cm of lateral sway per stride; the first pass
measured **6.5 cm and 6.0 cm**, which reads as a swagger. Now **4.7 cm and
2.6 cm**.

The bob and sway are an offset from a separately tracked anchor rather than
something the root integrates, or they would accumulate into a drift.

The balance overlay is also now hidden while under control. The character is not
balancing then — its capture point sits outside its feet permanently and means
nothing — and drawing it implied a failure that was not happening.

### 2026-08-10 (two modes: a character that walks, and a ragdoll it becomes)

The balance work established that keeping a figure upright on its own skeleton
is hard. This entry is the observation that games do not do it.

**Controlled.** The pelvis is *kinematic* — a new body type: infinite mass to
the solver exactly like a static body, but integrated from its own velocity, so
it can be driven about and shoves dynamic bodies aside without being shoved
back. The character cannot fall over because the thing holding it up is not
being simulated. Everything else is unchanged: the same thirteen bodies, the
same joints and limits, the same motors. The limbs are fully dynamic and hang
off the root, so they swing, collide and react — which is what makes it read as
physical rather than as an animation.

**Ragdoll.** The pelvis becomes dynamic, inheriting whatever velocity it was
being driven at, and the motors go slack. Nothing else changes. Physics takes
over.

The switch is the whole trick, and it is worth being blunt about what it buys:
the entire stepping thread became unnecessary the moment the root stopped being
simulated.

Walking is a gait rather than a controller — hips swinging in antiphase, knees
bending on the half of the cycle their foot is coming through — driven through
the motors so the legs stay physical and get dragged towards the gait rather
than snapped to it. Phase advances with *distance covered* rather than time, so
the legs do not cycle on the spot or moonwalk when the speed changes.

Measured:

| | |
| --- | --- |
| controlled, 60 s undisturbed | still standing, com 1.085 m |
| ragdolled from standing, 5 s | com 0.097 m — down |
| shoved 5 Ns / 20 Ns | still controlled |
| shoved 40 Ns / 80 Ns | ragdoll |
| walked 5 s at 1.6 m/s | travelled **8.000 m**, exactly |

The threshold behaves as designed: 2000 N over a 1/60 s step is 33 Ns, and the
switch happens between 20 and 40.

#### The bug that knocked it down on the first frame

The impact trigger looks for a hard contact anywhere but the feet — feet are on
the floor permanently and would fire it every step. That was not enough. The
character collapsed immediately, and the trace showed it ragdolling within a
quarter of a second of standing up.

The contacts it was firing on were **the character touching itself**. A forearm
rests against the torso and a shin brushes the other shin; those pairs are not
jointed to each other, so nothing suppresses their collision, and their contact
impulses are large enough to look exactly like being hit by a car.

The trigger now requires exactly one side of the contact to be part of the
character. Self-contact is not an impact, however hard it is.

#### What it does not do

Getting up is a snap: `G` restores control and the pelvis jumps back to standing
height wherever it happens to be lying. Real games blend the ragdoll pose into a
get-up animation, and the cheap honest version — lerping the root back over half
a second and letting the motors drag the limbs after it — is not written.

The feet also do not plant while walking; the support polygon reports fewer than
three contact points much of the time. It does not matter for a kinematic root
and it would matter enormously for anything that used the feet for traction.

### 2026-08-09 (the trade that is not a trade)

The last diagnosis said the step fires far too late — the body has travelled
0.72 m past the foot before the swing runs. The obvious response is to trigger
earlier, and the obvious objection is that an earlier trigger is exactly what
was removed a few entries ago to stop the figure stepping during ordinary sway.
Two real findings pointing opposite ways, and the trade between them had never
been measured.

Survival fractions over sixteen trials each, the two knobs that control
earliness against the two things they were supposed to trade:

| margin | persist | quiet 30 s | 20 Ns | 40 Ns |
| ---: | ---: | --- | --- | --- |
| 0.00 | 1 | 1/16 | 0/16 | 0/16 |
| 0.00 | 4 | 7/16 | 3/16 | 0/16 |
| 0.03 | 1 | 5/16 | 2/16 | 0/16 |
| 0.03 | 4 | 9/16 | 7/16 | 0/16 |
| 0.06 | 1 | 7/16 | 4/16 | 0/16 |
| **0.06** | **8** | **10/16** | **8/16** | 0/16 |
| 0.12 | 8 | 9/16 | 8/16 | 0/16 |

**There is no trade.** Both columns move together — triggering earlier is worse
at standing still *and* worse at surviving a shove, monotonically, and the
earliest setting is catastrophic at both (1/16 and 0/16). The shipped
configuration is already the best of the seven on every measure.

#### Which is consistent, and worth saying plainly

A trade requires both sides to have value. A step costs a foot's worth of
support for a third of a second and — as the previous entry established —
**delivers no increase in the base it stands on**. So a step is currently pure
cost, taking more of them earlier is strictly worse, and no trigger tuning can
be anything but harm reduction.

That also retires the "step earlier" implication from the last entry. It was a
reasonable inference from a correct diagnosis and it is wrong, because it
assumed the step would be worth having once it was timely. It is not yet worth
having at any time.

#### Where the ragdoll thread actually ends up

Standing balance works and is measured: 35 s unpushed with ankles alone, ankle
strategy holding a positive capture margin, motor stiffness at its measured
optimum, and a support polygon, centre of mass and capture point computed and
drawn every frame. That is a real balance controller and it does its job.

Stepping does not work, and after seven controllers and something like twenty
sweeps the reason is finally a single sentence: **the step does not widen the
base**. Everything else — the motor, the cone, the stance leg, the trigger, the
swing shape, the knee profile, the weight shift, the stance width, the load
gate — has been measured, and every one of them is either healthy or already at
its optimum.

The next thing to build is not another controller. It is one step, on a figure
held artificially upright so the body is not outrunning the leg, that
demonstrably extends the support polygon by the distance it moves the foot. If
that can be made to happen in the easy case it can be chased into the hard one;
if it cannot, the problem is somewhere nobody has looked yet, and there will at
least be a small enough case to find it in.

### 2026-08-09 (which of the three: the body outruns the leg)

Three candidates for why a 0.183 m step adds nothing to the support polygon:
the foot never lands, it lands in the wrong direction, or the centre of mass
keeps pace with it. All three separate cleanly by fixing the fall direction at
the moment of lift and tracking the foot and the centre of mass along it.

| along the fall direction, over 12 steps | |
| --- | ---: |
| the foot moved | **−0.069 m** |
| the centre of mass moved | **+0.332 m** |
| foot behind the com | −0.18 m → −0.58 m |
| swing foot in contact afterwards | 6/12, mean 215 N |

The foot moves *backwards* relative to the fall, and ends 0.58 m behind the
centre of mass having started 0.18 m behind it.

#### Which is not what it looks like

The obvious reading — the leg is aimed the wrong way — is wrong. Tracing one
swing, the distance from foot to target **shrinks steadily**, 1.243 m to
0.988 m. The leg is aimed correctly and closing.

The number that matters is the 1.243 m. **The target is over a metre away when
the swing starts**, because by then the pelvis has travelled **0.724 m
horizontally past the foot**. The leg closes a quarter of a metre in its 0.16 s;
the body covers a third of a metre in the same time. The foot is chasing a
target bolted to a pelvis that is outrunning it, and finishes further behind
than it began.

So it is candidate 3, arrived at from the opposite direction to the one
expected: not "the centre of mass keeps pace with the foot" but *the centre of
mass is already long gone before the foot starts moving*.

Nothing is saturated while this happens. The hip sits at 0.856 rad of a
0.960 rad cone, the motor at 76% of its torque, the stance leg rigid at 15% of
its own budget. **The step is not too weak. It is too late** — and not in the
sense that the trigger fires late, which was measured and found false, but in
the sense that a manoeuvre taking 0.16 s cannot catch a body already moving
faster than a leg can swing.

#### What that implies, and one more metric that lied

The step has to begin much earlier — while the capture point is still heading
out rather than once it has left — which means triggering on a *prediction* and
accepting steps that turn out to have been unnecessary. That is the opposite of
the trigger fix made a few entries ago, which reduced false steps and improved
quiet standing by exactly the mechanism that now looks like the problem. Both
findings are real; they simply trade against each other, and the trade has never
been measured.

And the third metric of this session to be measuring the wrong thing: "foot
travel 0.183 m" was the **magnitude** of the displacement, never its direction.
The foot was moving 0.18 m somewhere, and along the only axis that matters it
was going backwards the whole time.

### 2026-08-09 (the arithmetic: the rig is fine, and the step never widens the base)

The suggestion at the end of the last entry was to stop tuning and ask whether
this figure can be caught by a step at all. Done, and the answer is yes -- which
makes the last several entries' worth of failure a bug rather than a limit.

#### The model, on the rig's own numbers

A linear inverted pendulum of height `h` falling at `v` is caught by a foot
placed at `v * sqrt(h/g)` from the centre of mass. So the largest catchable
speed is however far the support can be pushed out, over the pendulum's time
constant -- and an impulse `J` at the chest gives the whole body `v = J/m`.

Read off the rig rather than typed in: 71.2 kg, centre of mass 1.085 m,
`tau = 0.3325 s`, foot half-length 0.110 m.

| support reach | max speed | max impulse |
| --- | --- | --- |
| ankles only, 0.000 m | 0.33 m/s | **23.6 Ns** |
| the step actually made, 0.183 m | 0.88 m/s | **62.7 Ns** |
| the step the geometry allows, 0.737 m | 2.55 m/s | 181.4 Ns |

**The first line is confirmed by experiment.** 20 Ns survives half the time and
40 Ns never does, which brackets 23.6 Ns exactly. The model is right, and by it
even the modest step already being made should nearly *triple* what the figure
can take -- 24 Ns to 63 Ns.

It does not. So the rig is not the limit and the premise was fine; something
between "the foot moves 0.183 m" and "the figure is caught" is broken.

#### And here it is

Measuring how far the support polygon reaches towards the oncoming capture
point, before the step and once the foot has landed:

> **0.0019 m before. 0.0000 m after.**

The base does not get any bigger. The foot travels 0.183 m and the polygon it
stands on gains nothing in the direction it is falling.

That is the entire failure, in one number, and it explains why every controller
performed identically: none of them were ever converting foot travel into
support. Load gating, the knee lift, the trigger persistence -- all of them
genuinely improved how far the foot got, and none of it ever reached the thing
that decides whether the figure stays up.

Three candidates, none yet distinguished: the foot may not be making contact
where it lands, it may be landing in a direction that does not extend the base
towards the fall, or the centre of mass may simply be travelling as fast as the
foot so the *relative* reach never improves. They are cheap to tell apart and
that is the next measurement.

#### Worth noting about the method

The arithmetic took twenty minutes and settled a question that six controllers
and a dozen parameter sweeps could not. It also validated itself on the way --
the no-step prediction of 23.6 Ns matching the measured bracket is what makes
the 62.7 Ns figure trustworthy, and without that check it would have been just
another plausible number.

The rule this project runs on is "compute the expected value by hand, then
compare". It was applied to contact impulses, reverb tails and rolling discs,
and somehow not to the one question the last several sessions were entirely
about.

### 2026-08-09 (bend early, straighten late: refuted, and a note on stopping)

The last entry's untried idea, tried. The knee profile was a symmetric
`sin(pi*t)` -- bend to a peak halfway, straighten by the end -- and the
proposal was to peak it earlier so the leg spends longer extended while
reaching. The phase is warped rather than the curve replaced, so the bend still
starts and ends at zero and still peaks at exactly the same angle; only *when*
moves.

| knee peak | airborne | foot travel |
| ---: | ---: | ---: |
| **0.50 (symmetric)** | 71% | **0.183 m** |
| 0.35 | 70% | 0.161 m |
| 0.25 | 64% | 0.142 m |
| 0.15 | 70% | 0.131 m |

Worse, monotonically, and the airborne fraction stays flat at about 70%
throughout -- so it is not a clearance effect. The likely reason is the
opposite of the premise: **much of the foot's forward travel comes from the knee
extending**, so extension late in the swing adds to the reach, and extension
early is spent before the hip has turned. The symmetric curve was already the
right shape.

#### Where this thread actually stands

Worth saying plainly, because the individual entries read more encouragingly
than the whole:

| measured and cleared | verdict |
| --- | --- |
| hip motor torque | 76% of budget, never saturated |
| motor speed cap | 6.5 rad/s demanded of 10 |
| trigger timing | fires with the centre of mass at 1.03 m of 1.08 |
| stance leg | rigid, carries 533 N, motors at 15% of budget |
| swing duration | travel flat in metres, 0.11-0.16 m, however timed |
| knee profile | symmetric is best |
| weight shift | rate limited to sqrt(h/g), too slow |
| wider stance | worse on every measure |

Three changes genuinely improved the mechanism -- load gating (0.06 to 0.158 m),
knee lift (to 0.183 m), and the trigger persistence that stopped it stepping
during sway. **Survival has not moved once, through any of it**: 8/16 at 20 Ns
and 0/16 at 40 Ns, the same as not stepping at all.

That pattern -- every component healthy, every refinement measured, the outcome
untouched -- usually means the premise is wrong rather than the tuning. The step
needs about 0.5 m and the leg delivers 0.183 m, and nothing in the mechanism is
saturated, which suggests the geometry simply does not allow it in the time
available. Before a seventh controller it is worth doing the arithmetic
directly: given this figure's mass, foot size, leg length and joint torques, how
large a disturbance *can* be caught by one step, and is it larger than the
smallest shove that puts it over? If the answer is no, that is a design fact
about the rig and not a controller to be found.

### 2026-08-09 (the stance leg: cleared, and one more single run that lied)

The swing had been measured to death and the leg holding the figure up had
never been looked at. It turns out to be fine, and getting to that took one
more correction of the kind this session kept needing.

#### The single run said the figure was already falling

Tracing one swing showed the stance foot carrying 60 N, then 23, then **zero**
for several frames, with the centre of mass at 0.845 m when the foot lifted —
against a standing height of 1.08 m and a fall threshold of 0.80. It looked
conclusive: the step fires so late that the figure is already committed, both
feet unloaded, nothing to push against.

Averaged over twelve trials it is not true. The centre of mass is at **1.030 m
when the trigger fires and 1.033 m when the foot lifts**, and the swing costs
0.001 m of height. The traced run was one of the bad trials. The step is not
firing late.

That is the fourth time this session a single run has produced a confident
wrong answer, and the only reason it did not become another rewrite is that
checking it cost two minutes.

#### The stance leg, averaged

| | measured | for comparison |
| --- | ---: | --- |
| stance foot load during the swing | 533 N | 697 N is the whole figure |
| stance knee buckle | 0.029 rad | 1.7 degrees |
| stance knee motor torque | 23.5 Nm | budget is 160 Nm |
| stance foot slip | 0.025 m | — |
| airborne | 9% of the swing | — |

The supporting leg is rigid, carries most of the figure, barely slips, and its
motors are working at a seventh of their budget. **It is not the problem
either.**

#### What is left, now that everything else is cleared

Five things have now been measured and each one exonerated: the hip motor
(76% of budget, never saturated), the speed cap (6.5 of 10), the trigger timing
(fires at 1.03 m), the swing schedule (travel is flat in metres however it is
timed), and the stance leg.

What remains is a real trade with both sides quantified. The bent knee that
keeps the foot off the ground — without which it lands four frames in and drags
— also folds the leg. **A hip reaching 4.48 rad/s would carry a straight 0.9 m
leg about 0.6 m in the 0.16 s swing; folded, it delivers 0.183 m.** Clearance is
worth more than reach at present, but the reach it costs is most of the step.

The obvious thing nobody has tried is to stop treating the knee as one number:
bend it early to clear the ground and straighten it late to reach, rather than
holding a single fold across the whole swing. That is a shape change to
`DriveSwingLeg`, it is small, and for once it follows from the measurements
rather than from a theory about how walking works.

### 2026-08-09 (measuring the hip motor, and finding the foot lands after four frames)

Five controllers were tuned against a limit nobody had looked at. A motor
written as a velocity constraint has three ways to run out — it saturates its
torque budget, saturates its speed cap, or is not asked for much — and they want
completely different fixes. All three are in numbers the solver already keeps.

#### It is neither

```
+ 0 | torque   24.2 Nm ( 11% of budget) | target spin 0.20 | actual 0.23 rad/s | foot load  41 N
+ 2 | torque   45.2 Nm ( 21% of budget) | target spin 2.06 | actual 1.44 rad/s | foot load   0 N
+ 4 | torque  166.2 Nm ( 76% of budget) | target spin 6.50 | actual 4.48 rad/s | foot load 189 N
+ 6 | torque  119.9 Nm ( 55% of budget) | target spin 4.28 | actual 3.36 rad/s | foot load 198 N
+ 8 | torque  101.0 Nm ( 46% of budget) | target spin 2.66 | actual 1.87 rad/s | foot load 171 N

peak torque 166.2 Nm of 220 (76%), saturated 0/10 frames
peak target spin 6.50 rad/s of a 10 cap, peak actual 4.48
```

**Peak torque 76% of budget, saturated in none of the ten frames. Peak demanded
speed 6.5 of a 10 cap.** The hip motor was never the constraint, and raising
either number would have done nothing — which five sessions of tuning had no way
of knowing, because nobody had asked.

#### What the same trace shows instead

Read the foot load along that swing: **41 N, then 0, then 189, 198, 171**.

The gate works exactly as designed — the foot leaves the ground into the
trough. And then it **lands again four frames in**, and spends the remaining
two thirds of the swing dragging along the floor. The step was not being
under-powered, it was being cut short by the ground.

#### Which reverses an earlier decision

The knee lift had been *reduced* from 1.0 to 0.40 rad when the fast step was
built, on the reasoning that a bent knee shortens the leg exactly when it needs
to reach furthest. Measured, that is backwards:

| knee lift | airborne frames (of 10) | foot travel |
| ---: | ---: | ---: |
| 0.40 rad | 2.3 | 0.083 m |
| 0.70 rad | 4.7 | 0.080 m |
| 1.00 rad | 6.5 | 0.118 m |
| **1.40 rad** | **7.1** | **0.183 m** |

Clearing the ground is worth far more than the reach it costs — **more than
double the travel from a leg that is nominally shorter while swinging**. 1.40 is
now the default.

#### And survival still does not move

8/16 at 20 Ns and 0/16 at 40 Ns, exactly as before. The foot now travels 0.183 m
of the roughly 0.5 m it needs, up from 0.06 m before load gating and 0.083 m
before the lift.

Three of those numbers have improved by measuring rather than guessing, and the
outcome has not moved once. The obvious remaining gap is that all of this
concerns the **swing** leg, and a step has two: what the *stance* leg does while
the other is in the air has never been looked at, and a stance leg that collapses
or fails to push makes the best swing in the world irrelevant.

### 2026-08-09 (a shorter swing and a predicted trough: both refuted)

Two refinements the last entry proposed, both measured, both wrong.

| swing | lead | foot travel | survives 20 Ns |
| --- | --- | --- | --- |
| **0.16 s** | **0.00 s** | **22%** | 8/16 |
| 0.10 s | 0.00 s | 6% | 8/16 |
| 0.16 s | 0.05 s | 6% | 8/16 |
| 0.10 s | 0.05 s | 4% | 8/16 |

**Pre-committing to a predicted trough** was supposed to centre the free window
on the lift instead of starting it there. It cuts foot travel from 22% to 6%.
The reason is visible in the earlier oscillation data and should have been
predicted from it: the load is a *spike train*, swinging 0 to 900 N several
times a second, so its derivative is enormous and erratic. Extrapolating it
predicts troughs that never arrive, and the step then commits at high load —
which is precisely the loaded lift that has never worked.

**A shorter swing** was supposed to fit inside the 0.10 s trough. It also cuts
travel to 6%, which is the opposite of what a trough-limited swing would do —
so the swing was never trough-limited.

#### The metric was flattering the wrong thing

Testing longer swings to chase that seemed to help:

| swing | travel % | distance asked | **actual travel** |
| --- | --- | --- | --- |
| 0.16 s | 22% | 0.72 m | **0.158 m** |
| 0.22 s | 7% | 0.57 m | 0.040 m |
| 0.30 s | 21% | 0.54 m | 0.113 m |
| 0.45 s | 38% | 0.38 m | 0.144 m |

38% looks like the winner and is not. A longer swing triggers when the target
happens to be closer, so it reports a larger *fraction* of a smaller distance.
**In metres the foot travels 0.11 to 0.16 m whatever the swing duration**, and
0.16 s is still the best of them at 0.158 m.

That is the second metric in three entries that turned out to be measuring its
own denominator — after mean fall time, which measured its own tail. The
percentage was introduced when the target distance was roughly constant and
quietly stopped being valid when it was not.

#### Where that leaves it

Survival is 8/16 at 20 Ns and 0/16 at 40 Ns in **every configuration tested
today**, unchanged since before any of this. The step needs about 0.5 m and
delivers 0.15 m, and neither the schedule of the swing nor the moment it starts
changes that.

Load gating remains the one thing that moved the number, from 0.06 m to
0.158 m, and it stays. What has not been measured even once is the swing's
actual authority: how much torque the hip motor delivers while the leg is
moving, and whether it is saturating. Five controllers have been tuned against
a limit nobody has looked at. That is the next thing to measure, not the next
thing to build.

### 2026-08-09 (gating the lift on foot load: the first thing to move the number)

Four step controllers moved the swing foot 9% of the distance they aimed it,
whatever their timing. The load measurement said why -- the foot carries 52% of
body weight at the moment of lift -- and the oscillation measurement offered a
way out: each foot passes under 150 N about 11% of the time, in windows of a
tenth of a second. So rather than trying to *create* an unload, which is rate
limited to `sqrt(h/g)` and far too slow, wait for one and lift into it.

The step is now decided and then held: the trigger picks the foot and the
target, and the lift waits for that foot's own load to fall below a threshold,
re-aiming at the capture point each frame while it waits. If no trough arrives
within a quarter second the step is abandoned rather than taken loaded, because
a loaded lift is the thing that never worked.

| gate | foot travel | troughs used / missed | survives 20 Ns |
| --- | --- | --- | --- |
| none | 8% of 0.55 m | – | 8/16 |
| **150 N** | **22% of 0.72 m** | 13 / 0 | 8/16 |
| 80 N | 20% of 0.80 m | 10 / 8 | 8/16 |
| 250 N | 7% of 0.58 m | 10 / 0 | 8/16 |

**The foot travels nearly three times further.** That is the first movement in
that number across five attempts, and it confirms the mechanism: the problem was
never the swing, it was that the foot was pinned, and waiting for it to be free
unpins it.

The thresholds behave as the measurement predicted. 250 N is no better than not
gating, because 250 N is still most of a body standing on the foot. 80 N catches
a cleaner trough but misses eight of them outright -- the foot simply does not
get that light often enough within the wait.

#### And it still does not save the figure

Survival is 8/16 in every row, unchanged from no stepping at all. 22% of the way
is a much better failed step than 8%, and it is still a failed step.

Two costs are now visible that were not before, and both come from waiting:

- **The target recedes.** The distance the step has to cover grows from 0.55 m
  to 0.72 m while waiting for the trough, because the capture point keeps moving
  away. Some of what the trough buys is spent on the longer step it causes.
- **The trough is 0.10 s and the swing is 0.16 s.** The foot is free for less
  time than the swing needs, so the back half of the swing is once again lifting
  against load.

Neither is fatal and both are attackable -- a shorter swing, or committing to a
predicted trough slightly before it arrives so the free window is centred on the
lift rather than starting at it. For the first time in this thread the next
thing to try is a refinement of something that works rather than a replacement
for something that does not.

### 2026-08-09 (the bounce cannot be tuned out, and is more useful than it looks)

The standing figure presses on the ground with up to twice its own weight, which
a body standing still cannot do except by bouncing. The question was what drives
it: the balance controller, the joint motors, or the contact solver.

#### The probe that measured nothing, and the tell that saved it

Turning each suspect off in turn produced this:

| configuration | mean load | min | max |
| --- | ---: | ---: | ---: |
| balance on, motors on | 696 N | 176 | 1335 |
| balance off, motors on | 200 N | 0 | 989 |
| balance on, gains zero | 198 N | 0 | 1229 |
| soft motors | 112 N | 0 | 466 |

It looks like everything reduces the bounce. It does not. **A standing figure
presses with its own 697 N, and only the first row does** — every other
configuration had fallen over, and a fallen figure's foot load is not a
measurement of anything. The mean was the validity check that caught it, and
without it the obvious reading would have been "turn the balance off and the
bounce goes away".

#### Stiffness is already at its optimum, and the window is narrow

Sweeping motor stiffness, with standing measured as a survival fraction rather
than read off the same run:

| stiffness | mean load | max | airborne frames | stood 30 s |
| ---: | ---: | ---: | ---: | --- |
| 8 | 252 N | 1309 | 8 | 0/8 |
| 11 | 692 N | 1325 | 2 | 1/8 |
| **14** | **696 N** | 1335 | **0** | **7/8** |
| 18 | 443 N | 1595 | 4 | 3/8 |
| 24 | 698 N | 2144 | 11 | 0/8 |

14 was already the shipped value, chosen by eye, and it is the optimum by a
wide margin — and the only stiffness at which the feet never leave the ground.
Stiffer bounces harder, softer cannot hold the figure up. **The bounce is not a
tuning error; it is what standing costs at the only stiffness that stands.**

#### Which turns out to be the good news

Total load never drops below 176 N, so the figure is never airborne. But the two
feet trade weight as it rocks, and individually:

> Each foot ranges from **0 N to about 890 N**, spends **11% of its time under
> 150 N**, and the longest continuous quiet window is **0.10 s**.

A foot does fully unload, several times a second, for about a tenth of a second
at a time. The swing that has to break contact is the first part of a 0.16 s
step, so a 0.10 s window is plausibly enough to get the foot moving.

That reframes the problem the last four entries have been stuck on. **Unloading
the swing foot may be a timing problem rather than a force one** — not "create
an unload", which the weight shift proved is rate limited to `sqrt(h/g)`, but
"wait for one, and lift into it". The load is already measured and on the panel;
gating the lift on it is a small change, and it is the first idea in this thread
that the measurements suggested rather than one imposed on them.

### 2026-08-09 (measuring the load on the swing foot, which explains everything)

Three step controllers failed and each one was blamed on something different --
the swing being too slow, then the missing weight shift, then the trigger. The
number that would have settled it in one reading had never been taken.

`FootLoad` now reports what each foot is pressing on the ground with, in
newtons: the solver works in impulses, and a normal impulse is a force applied
for one step, so dividing by the step recovers the force. The figure weighs
71 kg, so 697 N is all of it and 348 N each is standing square.

#### The answer

> **At the moment a step tries to lift a foot, that foot is carrying 363 N --
> 52% of body weight.**

That is the whole story of the last three sessions. Every swing controller was
asked to lift a foot with more than half the figure standing on it, and none of
them could, which is why the foot travelled 9% of the distance it was aimed at
regardless of how the swing was timed or shaped. It was never a trajectory
problem.

It also explains why the weight shift helped a little and not enough: it was
the only attempt aimed at the right variable, and it moved 1 cm of the 10 cm
needed because the shift is rate limited to `sqrt(h/g)`.

#### And something that was not being looked for

The load does not sit at a steady 348 N per foot while standing. It oscillates:

```
step  40: left   100 N  right   349 N
step  80: left   393 N  right   371 N
step 120: left   582 N  right   702 N
```

The last row sums to **1284 N -- nearly twice the figure's weight**, and during
the step the swing foot spikes to 918 N and drops to 0 several times a second.
A body standing still cannot press on the ground with twice its weight except
by bouncing, so the standing pose is not still: the ankle controller is driving
a vertical oscillation that the eye reads as a figure standing quietly.

That matters for stepping beyond the obvious. A foot whose load is a spike
train has no moment that is clearly the right one to lift -- and it may be that
the useful version of "unload the swing foot" is to *time the lift to a trough
that is already happening* rather than to create one.

Both numbers are on the demo panel now, because they are the two the next
attempt turns on.

### 2026-08-09 (the trigger, fixed — and a claim from the last entry withdrawn)

The last entry said stepping was worth +36% under a shove and −45% standing
still, from means over eight trials. Half of that was real. The other half was
the metric.

#### The fix

Two conditions now guard the trigger instead of one: the capture point must be
outside the feet by a threshold, **and stay outside for a number of consecutive
steps**. They reject different things — the threshold ignores small excursions,
the persistence count ignores brief ones — and ordinary sway produces both.

Measured as a **survival fraction over sixteen perturbed trials**, which is the
point of this entry:

| config | 3 s after 20 Ns | 30 s unpushed |
| --- | --- | --- |
| no stepping | 8/16 (50%) | 12/16 (75%) |
| step, first crossing | 8/16 (50%) | **8/16 (50%)** |
| step, 8 steps outside | 8/16 (50%) | **12/16 (75%)** |
| step, 8 steps + higher threshold | 8/16 (50%) | 12/16 (75%) |

Stepping on the first crossing costs a third of the quiet standing. Requiring
the capture point to *stay* out restores it exactly. That is the fix, it works,
and it is shipped.

#### And the claim it withdraws

Look at the first column. **Stepping does not change survival after a shove** —
8/16 either way at 20 Ns, and 0/16 either way at 40 Ns. The +36% from the last
entry was mean fall time, and mean fall time here is heavy tailed: most trials
fall in about a second, a few survive the whole budget, and the mean is decided
by how many of the few there happened to be. Six- and eight-trial means
disagreed between sessions on configurations that were not different at all.

A fraction is bounded and far better behaved, and under it the honest summary is:
**stepping now costs nothing and buys nothing.** It fires at the right moment,
picks the right foot, aims at the right place, and does not change the outcome,
because the foot still travels 9% of the distance it is asked to.

Three entries ago the measurement said stepping hurt. Two entries ago, after
averaging, it said stepping helped. Now, with a metric that is not dominated by
its own tail, it says stepping does neither. The code did not change between
the last two of those.

**Suspect the measurement** is the rule this project runs on, and it turns out
to have a second half worth writing down: *and suspect the statistic*. An
average is a summary, and a summary of a heavy-tailed distribution is mostly a
summary of its rarest outcomes.

### 2026-08-09 (a wider stance, refuted — and single runs shown to be worthless here)

Two results, and the second is the one that matters.

#### The wider stance does not help

The idea was that feet further from the centre line would be loaded less
equally by a disturbance, so one of them could actually be lifted. Splaying the
legs — hips fixed at 0.10 m, ankles moved out, everything placed along the
hip-to-ankle line — makes it measurably **worse**:

| stance | 40 Ns shove, no step | unpushed, no step |
| ---: | --- | --- |
| 0.10 m | 1.99 s | 25.1 s |
| 0.20 m | 1.14 s | 20.2 s |
| 0.30 m | 1.00 s | 7.1 s |

Splayed legs push outwards, and the ground reaction answering them is one more
thing the balance controller has to fight. Kept as a slider so the refutation
can be reproduced; the default is back to 0.10.

#### Single runs here are worthless, and three sessions of conclusions rested on them

The same nominal configuration — 0.10 m stance, no stepping, unpushed —
measured **35.1 s in one session and 12.7 s in the next**, with nothing between
them that should have touched it. A standing figure is an unstable equilibrium:
two runs differing by a rounding error diverge completely, and every "it fell
after 0.98 s" in the last three entries was one sample of a wide distribution.

Redone properly — eight trials per configuration, each nudged slightly
differently so the figure starts somewhere else on the knife edge, and the fall
times averaged:

| stance | 40 Ns shove: no step / step | unpushed: no step / step |
| ---: | --- | --- |
| 0.10 m | 1.99 / **2.71 s** | 25.1 / 13.7 s |
| 0.20 m | 1.14 / 1.18 s | 20.2 / 5.3 s |
| 0.30 m | 1.00 / **1.55 s** | 7.1 / 4.1 s |

**Stepping helps under a shove.** At every width, and by 36% at the default
stance. The previous three entries concluded the opposite — that stepping never
helped and possibly hurt — from single runs that happened to land the other
way. That conclusion was wrong, and the code was doing better than the
measurement said.

**Stepping hurts quiet standing**, also at every width and by more: 13.7 s
against 25.1 s. It steps during ordinary sway, when nothing needs recovering,
and each unnecessary step costs a foot's support.

So the picture is the opposite of what it looked like. The swing is not the
problem; the **trigger** is. Stepping when genuinely disturbed is worth 36%;
stepping when not is worth −45%. That is a threshold to tune, not a controller
to rewrite — and it is a much smaller job than the three rewrites that preceded
it.

The lesson is the one this project keeps relearning, in a new place: **suspect
the measurement.** Here the measurement was not merely noisy, it was a single
sample of a chaotic system being read as a number.

### 2026-08-09 (the fast reactive step, and the measurement that should have come first)

Third step controller, and the one that finally explained the other two.

The plan from the previous entry: no weight shift, a short swing (0.16 s rather
than 0.34), a small lift so the leg stays long enough to reach, and the aim
snapping on in a third of the swing rather than two thirds. A foot thrown at the
capture point rather than a leg aimed at it.

One real bug was fixed on the way, and it was the worst of the three: **the
planted leg was being pulled straight back out from under the figure.** On
completion the hip target reset to the pose it started in, so the instant the
foot touched down the leg swung back under the body, undoing the step. The
stepped leg now holds where it landed.

And the result was the same as the other two:

| shove | no stepping | deliberate | fast |
| ---: | --- | --- | --- |
| 20 Ns | 2.10 s | 2.05 s | 2.08 s |
| 40 Ns | 0.98 s | 0.93 s | 0.97 s |
| 80 Ns | 0.62 s | 0.62 s | 0.58 s |
| 120 Ns | 0.45 s | 0.45 s | 0.45 s |

Three controllers, three different sets of timings, identical numbers. That is
not a tuning problem, and it should have been the signal to stop tuning much
earlier.

#### The measurement that should have come first

Every one of these assumed that aiming the hip puts the foot at the target.
Nobody had checked. Checking took one trace:

```
foot starts at (-0.108 +0.025)
target        (-0.018 -0.188)  -- asked to move 0.231 m
foot landed at (-0.107 +0.003) -- moved 0.021 m, missed target by 0.211 m
```

**The step never happens.** The foot travels 9% of the way and lands where it
started. Every controller above was correctly deciding to step, correctly
choosing the foot, correctly picking the target — and then not stepping.

#### Why, and why it ties the whole thread together

The swing foot is still carrying weight, and **a loaded foot cannot be lifted**
however hard the hip pulls. Unloading it means moving the weight laterally onto
the other foot, which is the shift — and the shift is rate limited to
`sqrt(h/g)` = 0.33 s, which the previous entry measured and which is longer
than the whole recovery window.

A backwards shove makes it worse in a way worth naming: it loads **both feet
equally**. The body rotates towards its heels rather than onto one side, so
neither foot frees itself, and there is nothing to step with. A person shoved
backwards unweights one side first — and that lateral move is precisely the
part there is no time for.

So the three controllers were not three attempts at the same problem. They were
three attempts at the wrong problem: the question is not how to swing a leg, it
is how to get weight off a foot in under a third of a second.

Left in the demo, switchable, defaulted off, with the diagnosis in the source
next to the code it condemns.

### 2026-08-09 (the weight shift, and why it does not save the step)

The previous entry blamed the failed step on lifting a foot from a body whose
weight was still on it. That was right, and fixing it was not enough — which is
the more useful result.

A shift phase now runs before the lift: both feet stay planted while a
commanded ankle roll moves the centre of mass over the stance foot, ending
early once the weight arrives. It does what it says. Step-with-shift is back to
**parity** with not stepping, where step-without-shift was measurably worse:

| shove | no stepping | step, no shift | step + shift |
| ---: | --- | --- | --- |
| 20 Ns | 2.10 s | 1.93 s | 1.90 s |
| 40 Ns | 0.98 s | 0.85 s | 0.90 s |
| 80 Ns | 0.62 s | 0.55 s | 0.62 s |

So the shift repaid the damage the swing was doing, and bought nothing beyond
it. The step still does not recover the push.

#### The measurement that explains it

Sweeping the shift's duration against its authority separates the two cleanly,
and only one of them matters:

| shift | roll 0.30 | roll 0.55 |
| ---: | --- | --- |
| 0.22 s | 0.101 → 0.091 m | 0.101 → 0.093 m |
| 0.50 s | 0.101 → 0.068 m | 0.101 → 0.063 m |
| 1.00 s | 0.101 → 0.053 m | 0.101 → 0.054 m |

**Doubling the authority changes nothing; quadrupling the time halves the gap.**
The shift is rate limited, and the rate is not a tuning constant — moving a
centre of mass sideways is an inverted pendulum, whose time constant is
`sqrt(h/g)`, about **0.33 s** for this figure. That is the same constant that
appears in the capture point, arriving from the other direction. A 0.22 s shift
was asking the weight to move faster than the pendulum permits, and no amount
of ankle torque changes that.

#### Which is the real answer

Add it up: 0.35 s to shift, 0.34 s to swing, against a fall that is over in
about a second. **A recovery step cannot afford to shift its weight first.**

That is not a bug in the controller, it is the wrong controller. A weight shift
is what a *deliberate* step does — walking, where there is time. An emergency
step throws the foot out fast and accepts a moment of single support, catching
the body rather than preparing to. People do both, and they are different
manoeuvres.

So the next attempt should be the fast one: no shift, a much shorter swing, and
a foot placed at the capture point rather than a leg aimed at it. The capture
point stays the target throughout; what changes is how much ceremony there is
on the way.

### 2026-08-09 (stepping: the right decision, the wrong swing)

The last piece of "stumbles but does not fall", and the first thing in a while
that does not work. It is in the demo, defaulted off, because turning it on
makes the figure fall sooner.

The idea is the one the capture point exists for. Once the capture point is
outside the feet no ankle torque recovers it, because an ankle can only move
where the weight bears *within* the base. The recovery is to move the base: put
a foot where the capture point went. The step itself is a short scripted swing
— bend the knee to lift, point the leg at the target, straighten and plant —
and what is supposed to make it work is not the trajectory but *where it aims*.

#### What works

**The decision.** After three fixes it fires exactly when it should and not
otherwise, which the trace confirms: nothing during the first two seconds of
ordinary sway, then a step on the frame after the shove, aimed backwards at a
capture point 0.19 m behind the feet. Correct trigger, correct foot, correct
target.

Getting there took three separate bugs, and the first is the instructive one:

- **A degenerate support polygon read as "far outside".** `SignedDistance`
  returns −1 for fewer than three contact points, and a raised foot often
  leaves exactly that — so finishing a step triggered another one immediately.
  The figure stepped continuously and walked itself over: **236 steps in
  fifteen seconds**.
- **The trigger was far too sensitive** at 0.02 m. It stepped three times
  during normal sway *before* the push it was meant to be recovering from,
  each one costing more stability than the disturbance it answered.
- **Steps that landed where the foot already was.** A step to a target 2 cm
  away buys nothing and costs the support of a raised foot for a third of a
  second. There is now a minimum step length and a cooldown.

#### What does not

The swing. Every number is worse with stepping on:

| case | stepping off | stepping on |
| --- | ---: | ---: |
| unpushed | 35.1 s | 14.8 s |
| 20 Ns shove | fell after 2.10 s | 1.90 s |
| 40 Ns | 0.98 s | 0.88 s |
| 80 Ns | 0.62 s | 0.55 s |

The diagnosis is not subtle once the masses are added up. The swing leg is a
thigh, a shin and a foot — **11.8 kg, a sixth of the body** — thrown through a
third of a second by a 220 Nm hip motor. The reaction on the pelvis is of the
same order as the disturbance being recovered from, and meanwhile the figure
has traded two feet of support for one.

What real capture-step controllers do and this does not is **shift the weight
onto the stance leg before the foot leaves the ground**. This lifts immediately,
so the body is still half-supported by a foot that is no longer there. That is
the next thing to try, and it is a controller change rather than an engine one.

Left switchable in the demo rather than deleted, with the panel saying it is
worse, because a failure that can be watched is worth more than one described
in a changelog.

### 2026-08-09 (a two-axis ankle, and a figure that stands for 35 seconds)

The lateral gap from the previous entry, closed. **The ankle is now a ball
joint on a short leash** rather than a hinge — a 35 degree cone and 15 degrees
of twist — which gives it the roll axis a hinge cannot have. That was the whole
problem: the sagittal strategy worked and the figure fell sideways every time,
because nothing in the rig could push sideways at the foot.

The hip strategy that was kept at zero gain is gone, superseded.

#### Signs by experiment, gains by sweep

The correction signs were settled by running all four combinations rather than
by reasoning about which way a foot bends, which is how the earlier attempts
went wrong. Pitch +1 with roll −1 stood 1.28 s where the others managed 0.18 to
0.67.

Then the gains, and the first sweep answered a different and better question —
whether the correction has any authority at all:

| gain | stood |
| ---: | --- |
| 0 | 2.10 s |
| 1.0 | 2.22 s |
| 2.5 | 9.85 s |
| 5.0 | 3.97 s |
| 20.0 | 0.47 s |

A clear optimum with falls either side of it: too little authority below,
over-gain oscillation above. Sweeping pitch against roll separately, **roll gain
matters more than pitch** — the lateral axis being the weak one, exactly as the
hinge diagnosis predicted. Pitch 2.0/roll 4.0, 3.0/4.0, 4.0/2.5 and 4.0/4.0 all
held the full 20 second budget; the shipped values sit in the middle of that
region rather than on its edge.

#### What it does and does not buy

**Standing still: 35 seconds**, against one or two before. Not indefinitely —
drift accumulates and eventually wins.

**Pushed: it goes down.** A 20 Ns shove at the chest topples it in 2.1 s, and
harder shoves proportionally faster:

| shove | falls after |
| ---: | --- |
| 20 Ns | 2.10 s |
| 60 Ns | 0.75 s |
| 200 Ns | 0.30 s |

That is not a tuning failure, it is the ankle strategy's actual limit and the
theory says so: an ankle can only shift where the weight bears *within the
feet*. Once the capture point leaves the support polygon no ankle torque can
bring it back, and the only recovery is to put a foot where the capture point
went. Which is stepping, and is the next piece.

### 2026-08-09 (balance: the measurement works, half the control does)

The measurement first, because a balance controller is mostly a measurement
with a small amount of pushing on the end.

Every step the demo computes the **support polygon** — the convex hull of
everywhere the feet actually touch, from the contacts rather than from where
the feet are, so a foot in the air contributes nothing — and the **capture
point**, `com + velocity * sqrt(height / g)`. The capture point is the idea
that matters: not where the centre of mass *is* but where it is *going*, and
the spot a foot would have to reach to bring the body to rest. Standing still
they coincide; moving, the capture point leads. The signed distance from it to
the polygon is the one number that says whether the figure is standing or
merely upright, and it goes negative **before** anything looks wrong.

Drawn in the scene, which `Renderer2D::BeginScene` taking any camera makes free:
the hull in cyan, a plumb line from the centre of mass, and the capture point as
a cross that is green inside the polygon and red outside. The hull is a proper
convex hull rather than the bounding box of the contacts, because a box says a
figure up on one toe is as stable as one flat-footed, which is exactly the case
balance is about.

#### The ankle strategy works, and the numbers say so

With ankles alone the figure genuinely stands: capture margin **+0.075 m and
+0.071 m** at half a second and one second, with the centre of mass holding
within a centimetre of where it started. That is a real inverted pendulum being
actively held up, not a rig that happens not to have fallen yet.

Then it goes sideways, and nothing can stop it. **The rig's ankle is a
single-axis hinge and that axis is pitch**, so there is no way to push sideways
at the foot at all. The support polygon collapses from five contact points to
two as one foot rolls onto its edge, and the margin drops off a cliff.

#### The lateral strategy that did not work, kept anyway

The obvious substitute was a hip strategy: roll both hips to shift the body
over the supporting foot. It destabilises, and the diagnosis is worth keeping
because it took three experiments rather than a guess:

- With the feet at their real size it drove the figure over in about a second.
- **With deliberately enormous feet** — a base so wide that toppling should
  have been impossible — it still ran away sideways, and the centre of mass
  stayed at its standing height while doing it. So the figure was not falling;
  it was being *driven*.
- Flipping the sign did not fix it, which rules out the obvious cause. At
  higher gain it lifted the centre of mass to 1.37 m instead of moving it
  sideways.

Rolling both hips about the world's z axis with the feet planted fights the
ground rather than shifting weight. **The mechanism is wrong, not the sign.**
It is left in the demo with its gain defaulted to zero and a slider, because a
failed approach sitting next to a working one is worth more than a quietly
deleted one — turn it up and the figure walks itself over in about five
seconds.

The fix is a roll axis at the ankle, and after that, stepping.

### 2026-08-09 (capsules that are actually capsule-shaped)

The capsules were drawn as a **box** with a sphere on each end, which reads
exactly as what it is — a rectangle with balls stuck on. The collider was
always the real thing; only the drawing was a shortcut, taken because the
renderer had no round primitive and inventing one for a single demo shape did
not seem worth it. It was, the moment there was a humanoid made of them.

`Mesh::CreateCylinder` is the fix, and the reason it is a *cylinder* rather
than a capsule mesh is the interesting part. A capsule mesh cannot be scaled to
fit different capsules: stretching one along its axis turns its round caps into
ellipsoids, so every distinct radius and length would need its own mesh. A
cylinder has no such problem — scale x and z together for radius, y for length,
and it is still exactly a cylinder — and a sphere under a uniform scale is
still exactly a sphere.

So **two fixed meshes draw a capsule of any proportions, exactly**: an
open-ended tube between two spheres. Open-ended because the ends are covered by
the caps, which also saves the two fans a closed cylinder would need.

The one thing worth getting right is the normal. A cylinder's side normal
points straight out from the axis with **no y component** — using the position
as a normal, which is correct for a sphere and tempting to copy, would light
the tube as though it bulged.

### 2026-08-09 (a humanoid rig, and a test that measured an impact)

Thirteen bodies, twelve joints, about 71 kg and 1.86 m: pelvis, torso, head,
two arms and two legs with feet. Knees, elbows and ankles are limited hinges;
hips, shoulders, spine and neck are cone-and-twist ball joints. Drawn as its
own colliders, which needs no renderer work at all and is the reason `.gltf`
and skinning stay deferred.

The rig is where the constraint code stops being tested in twos. It is a chain
of fifteen with the worst mass ratios in the project — a 24 kg torso meeting a
1.6 kg forearm — and limits that have to hold while contacts push back.

**The motors work, and the number is stark.** Pose drift over ten seconds,
summed across twelve joints: **0.137 radians powered against 13.900 passive**.
A hundredfold. Powered it falls stiffly like a mannequin; passive it folds into
a heap.

**It topples on its own, and that is correct.** A standing figure is an
inverted pendulum on two small feet, and a motor holds a *joint angle* — which
does not change when the whole body tips about its ankles. Nothing here knows
where its centre of mass is. The demo says so on its own panel, because
otherwise it reads as a bug.

#### The failure that was the test measuring an impact

Two checks failed at first, and both looked like real solver weakness: anchor
separation peaking at **38 mm**, and hinges leaving their range by **0.56
radians**. Both are the classic signature of an under-converged chain, so the
obvious move was to throw iterations at it — and iterations barely helped,
going from 0.10 m at eight to 0.04 m at thirty-two and then getting *worse*
again at sixty-four.

Which meant the diagnosis was wrong. Splitting peak from settled found it
immediately:

| iterations | separation peak / settled | limit breach peak / settled |
| ---: | --- | --- |
| 8 | 0.101 m / 0.0005 m | 0.598 / 0.000 rad |
| 16 | 0.080 m / 0.0001 m | 0.555 / 0.000 rad |
| 32 | 0.040 m / 0.0002 m | 0.270 / 0.000 rad |
| 64 | 0.060 m / 0.0001 m | 0.168 / 0.004 rad |

**Settled, the rig is exact** — a tenth of a millimetre of separation and no
limit breach at all. The peaks are one or two frames as 71 kg of figure meets
the floor inside a single 1/60 s step, which is a violent event; a
velocity-level constraint recovering from it over a frame or two is the
constraint working. The test had been asserting a steady-state expectation
against a transient, and the iteration sweep was measuring how hard the impact
happened to be rather than how well it was solved.

Powered, the settled separation is **3 mm rather than 0.1 mm**, because a motor
keeps every joint loaded where a collapsed ragdoll's joints carry almost
nothing. On a figure 1.86 m tall that is a sixth of a percent of its height.

### 2026-08-09 (joint motors, and the bug the cone-twist tests could not see)

A limit says where a joint may not go. A motor says where it should be, and is
the difference between a ragdoll and a body — a corpse falls, a person holds
their arm up.

Written as a **velocity constraint with a target**, not as a torque. That is
what makes it a PD controller without a separate derivative term: the
proportional half is the target spin, set from how far the joint is from its
goal, and the derivative half falls out of constraining the spin *to* that
value, since any excess is removed. `MotorMaxTorque` is what keeps it a muscle
rather than a weld — push hard enough and the joint gives, which is the whole
point for something meant to stumble. Hinges get a scalar motor about their
axis; ball joints get a three-axis one driving towards a target orientation,
which is what a ragdoll actually needs.

Checked against arithmetic done outside the solver. A 1 kg bar with its centre
0.5 m out needs `m g d = 4.905 Nm` to hold horizontal:

- **Three times that torque holds it at y = 2.000**, exactly level.
- **Three tenths of it sags by 0.499 m.** A motor that held here would be
  ignoring its own budget.
- A weak motor **gives when shoved** — 3.13 rad — and **returns to its target**
  afterwards, which is the other half of a muscle.
- A strong motor **driving at a limit does not pass it**: 0.52 rad against a
  0.5 rad stop.
- A ball motor **holds an outstretched arm against gravity** to within
  0.00002 degrees, gives 37.8 degrees when struck, and recovers to 0.0001.

#### Two findings worth keeping

**A motor must not be warm started.** The limits and the point constraint are;
a motor is not, because its budget is per step. Leaving the accumulated impulse
sitting at the clamp from last step means this step's solve computes a delta of
nothing and the motor silently stops pulling — a motor given *three times* the
torque it needed still could not hold an arm up, and the strong and weak cases
sagged identically, which is what gave it away.

**`MotorMaxSpeed` has to be matched to `MotorMaxTorque`.** At the default
10 rad/s a 2 Nm motor asks for a speed it cannot then decelerate from: it flies
past its target and limit-cycles for ever, returning to 0.96 rad instead of 0.
The rule is that a motor must be able to stop from whatever speed it is allowed
to ask for. Worth knowing before tuning a ragdoll, where every limb is a weak
motor by design.

#### And a real bug the cone-twist tests could not have caught

The ball motor held the arm perfectly with no limits and settled **exactly
10.000 degrees** off with cone-and-twist enabled — at every stiffness, which
ruled out droop immediately, since droop scales with stiffness and this did
not.

The cone was 80 degrees and the arm was built 90 degrees rotated from the
torso. `SetConeTwistLimits` measured swing from the bodies being *aligned*
rather than from the pose it was called in, so the arm started 90 degrees into
an 80 degree cone and the limit shoved it to the boundary — 10 degrees, exactly
as observed.

The cone-twist tests all passed because every rig in them had the bone and the
torso starting aligned, so the relative rotation was the identity and the bug
was invisible. **Every limb on a real skeleton sits at an angle to its
parent**, so this would have appeared the moment a humanoid was assembled and
looked like the limits were wrong rather than their reference pose. The joint
now stores the relative orientation it was built in and measures both angles as
deviations from that; the droop reads 0.000 at every stiffness afterwards.

### 2026-08-09 (cone-and-twist: limiting a ball joint without the two limits interfering)

A hinge has one angle to limit. A shoulder has two very different ones — how
far the arm may lean away from its rest direction, and how far it may rotate
about its own length — and the reason this took more than doubling the hinge
code is that **measuring them directly makes them interfere**. Twist read off a
reference vector changes when the bone swings, even though nothing twisted.

So the relative rotation is split by a **swing-twist decomposition**: project
the relative quaternion's vector part onto the bone axis to get the twist, and
whatever is left over is the swing. Two clean angles, each limited on its own.
One detail is load-bearing — the relative quaternion's scalar part is forced
non-negative first, because a quaternion and its negation are the same rotation
but the decomposition is not sign-agnostic, and without it a 10 degree swing
can be reported as 350.

Both limits are unilateral, like the hinge's, so both clamp their accumulated
impulse to one sign. They share one small lambda with the hinge limit, since
all three are the same shape: measure the relative spin along the limit's axis,
solve for the impulse that stops it, clamp the running total.

Checked against the numbers that went in, not against the joint's own
decomposition — which is the thing under test, so believing its `SwingAngle`
would be marking its own homework. The swing is re-measured independently as
the angle between the two bone directions:

- **A 30 degree cone holds**, driven at 6 rad/s from **eight directions in
  turn**: worst **30.03 degrees**. Testing one direction would have passed a
  limit that only holds along the axis it was tested on.
- **A 2 degree cone** pins the bone to 2.03 degrees; a **60 degree cone still
  swings**, reaching 60.1. A limit that holds everywhere is a weld and would
  have sailed through the first check.
- **A twist limit of ±20 degrees** holds at 20.05.
- **The decomposition does not leak, in either direction.** Spinning the bone
  about its own length produces **0.000 degrees of swing**; swinging it 80
  degrees through a wide cone produces **0.000 degrees of twist** against a 5
  degree limit. Those are exact geometric facts rather than luck, which is why
  they come out as zeroes.
- **No energy added** banging against both stops for ten seconds.

Disabling the cone sends the bone to **180 degrees** on the first two checks,
so they are measuring the limit rather than something incidental.

### 2026-08-09 (hinges and their limits, and three tests wrong in the same way)

A hinge is a ball joint with two of its three rotations locked, so it is built
as exactly that rather than as a constraint of its own: the point constraint is
untouched, and an angular pair is solved beside it.

The angular part takes a **2x2** effective mass for the same reason the point
part takes a 3x3 — the two locked directions are coupled through the inertia
tensor. The limit is the odd one out: it is the only **unilateral** piece of a
joint, so alone in here it clamps its accumulated impulse to one sign. A knee
stop must resist bending further without holding the knee *at* the stop.

What it was checked against:

- **A hinge refuses rotation off its axis**, driven at 8 rad/s about all three
  at once: worst surviving off-axis spin **0.000000 rad/s**. Disabling the
  angular constraint puts that straight back to 8.003, so the check has teeth.
- **The axis stays free**: spun at 2 rad/s, still 1.926 five seconds later,
  the loss being the linear damping the body carries by default.
- **A hinged bar's period.** `T = 2*pi*sqrt((m L^2/3)/(m g L/2))` for a uniform
  bar about its end — measured **1.6393 s against 1.6379 s, 0.08% out**.
- **Limits stop it** at both stops with **zero overshoot** driven at 6 rad/s,
  while leaving the range between them free: 0.4949 rad in half a second at
  1 rad/s, against 0.5 expected.
- **Limits add no energy**: bouncing between two stops for ten seconds, peak
  kinetic energy never exceeds the starting figure. It falls to zero, which is
  correct rather than suspicious — a limit here is perfectly inelastic, so each
  strike removes the spin about the axis. A bouncy joint stop would need a
  restitution term this deliberately does not have.

#### Three tests wrong, one after another, all for one reason

The hinge work produced no engine bugs and three test bugs, and every one was
the same mistake: **asserting on a quantity without setting up a state
consistent with it.**

- Spinning a bar at `(8, 8, 2)` and expecting the `2` to survive. Constraining
  two axes of a strongly anisotropic body legitimately changes the third — the
  impulse goes in along the tangents and comes back out through `I^-1`. The
  hinge was not eating the rotation.
- Then, isolating it: spinning only about the axis at 2 rad/s, and watching it
  arrive as 0.485. **Setting an angular velocity without the matching linear
  one** describes a body rotating about a pivot whose centre is not moving,
  which is not a thing. The joint fixed it on the first step by trading angular
  for linear, exactly as it should. A `SpinAbout` helper that sets both made it
  1.926.
- And measuring "does it swing freely between the stops" **starting from a
  stop**, which measures how fast the joint leaves a limit — a different
  question, and one that reads as sluggishness.

Worth recording because the failure looks identical each time: a plausible
number, wrong, with the solver behaving correctly underneath it. The rule that
caught all three was checking whether the *initial state* was physical before
believing the *final* one.

### 2026-08-09 (ball-and-socket joints: the first bilateral constraint)

The first constraint in the engine that is not a contact, and the prerequisite
for anything ragdoll-shaped.

#### What makes it different from a contact

Two things, and both shape the code:

- **It is bilateral.** A contact may only push — its impulse is clamped at
  zero, because a floor cannot pull a box down. A joint pushes and pulls
  without limit, because a shoulder holds an arm on from both directions. So
  the solve is the contact solve with pieces removed: no clamping, no
  restitution, no friction.
- **It takes a 3x3 effective mass, not a scalar.** A contact constrains one
  direction. A ball joint constrains all three at once and they are coupled
  through the inertia tensor, so `K = (1/ma + 1/mb) I - [ra]x Ia^-1 [ra]x -
  [rb]x Ib^-1 [rb]x` is assembled and inverted per joint per step. Solving the
  three axes separately converges slowly and stretches visibly under load,
  which on a ragdoll reads as limbs pulling out of their sockets.

Joints and contacts share one iteration loop rather than each getting a pass.
They are coupled — a jointed chain resting on the floor is both at once — and
solving all of one then all of the other lets each undo the other's work.

#### What it was checked against

- **The pendulum period.** A compound pendulum swings at
  `T = 2*pi*sqrt(I / (m g d))`, with `I` about the pivot from the parallel-axis
  theorem. Nothing in the solver knows that formula. Measured **2.0095 s
  against a predicted 2.0077 s — 0.09% out**, timed from zero crossings rather
  than peaks, since a peak is flat and its timing is far noisier.
- **A joint does no work.** Released from rest and swung through the bottom,
  the bob returns to within **0.5 mm of its starting height** over a 1 m swing,
  with damping off.
- **A chain holds.** Six links under their own weight: worst anchor separation
  **0.000 m**, hanging plumb.
- **Jointed bodies stop colliding.** Two overlapping spheres produce a contact;
  add a joint and the contact disappears. Without this an upper and lower arm,
  which share the elbow permanently, would have their contact and their joint
  fighting every step.

#### The suspiciously clean number, and what it was hiding

A worst separation of exactly `0.00000` is the kind of result worth
distrusting, so the metric was given something to fail at: a 6-link chain with
a **20 kg weight** on the end — the mass ratio sequential impulses are famously
worst at — swept across iteration counts.

| iterations | worst separation | peak kinetic energy |
| ---: | --- | --- |
| 1 | 0.434 m | 1094 J |
| 2 | 0.134 m | 7217 J |
| 4 | 0.005 m | 390 J |
| 8 | 0.006 m | 110 J |
| 20 | 0.054 m | 298 J |

The metric has teeth. The lesson is the energy column: the chain and weight
have only about 500 J of gravitational potential available, so **1 and 2
iterations are not merely inaccurate, they are unstable** — an under-solved
stiff constraint gains energy and throws the chain about. At four and above it
is bounded and the constraint holds to a few millimetres.

Two false starts on the way to that table, both measurement rather than code:

- The first version read the separation at a single instant, at step 1200. The
  chain is still swinging then, so it sampled the *phase of the swing* rather
  than the solver's accuracy — which is why 2 iterations first appeared worse
  than 1. Taking the worst over a window fixed it.
- More iterations still looked worse at 20 than at 8, which should not happen
  if iterations only remove error. The suspicion was the Baumgarte bias pumping
  energy in, and it was tested directly: a chain assembled exactly, at rest,
  with gravity off. A correct solver does nothing there for ever, and this one
  does — **zero kinetic energy and zero separation at both 4 and 20
  iterations**. So the bias is not inventing energy; the runs are simply
  different trajectories, because a stiffer solve holds the weight more rigidly
  and transmits sharper accelerations up the chain. Peak energy is reported
  beside the separation for exactly that reason.

For ragdoll work the practical reading is: **use eight velocity iterations or
more**, and do not trust a chain solved with fewer than four.

#### Also

Sleeping now propagates across joints. A body that sleeps becomes immovable to
the solver, so half a sleeping chain would act as an anchor bolted to mid-air
and the whole thing would lurch when it woke. Wakefulness spreads along joints
until nothing changes — the poor relation of the island solver 2D has, and
enough for chains and ragdolls.

### 2026-08-09 (capsules, and a manifold that described the shape instead of the contact)

A capsule is a segment with a radius, so most of the sphere code generalises —
which is why it was the next shape worth having. It is also what characters are
made of: no corners to catch on a step, and it stands up instead of rolling
away.

Built in the same separable pieces the 3D bodies were.

#### The inertia tensor, and the two limits that check it

A capsule's tensor is a cylinder plus two hemispheres, with the mass split
between them by volume so it stays right whether the caps or the cylinder
dominate. The transverse term carries a `h^2/4 + 3hr/8` that looks arbitrary
and is not: shifting each hemisphere's `(2/5) m r^2` from the sphere centre out
to the capsule centre leaves exactly that, because the `9r^2/64` from the
hemisphere's own centre of mass cancels.

Checked against the formula, but far more usefully against its **limits**,
neither of which appears anywhere in the capsule branch:

- A capsule with no cylinder is a sphere: `Ixx = Iyy = 2/5 m r^2`, agreeing
  with the sphere branch to four decimals.
- A capsule with almost no radius is a thin rod: `Ixx -> m L^2 / 12`, landing
  within 1% at `r = 0.002`, and `Iyy` falling to nearly nothing.

#### The manifold, which was wrong in an instructive way

Capsule–sphere and capsule–capsule both reduce to sphere–sphere once the
nearest points on the segments are known, so they are one function each. The
segment-to-segment routine re-solves the second parameter after clamping the
first, which is the classic place that code is wrong — it only shows when one
segment's nearest point falls off its end.

Capsule–box needed a real manifold. A capsule lying on a floor touches along a
*line*, and one contact point cannot hold a line level — the same lesson the
box stacking bug taught, arriving in a different shape. So the segment is
clipped against the face, as `Sat3D` clips its reference face.

The first version emitted both clipped ends unconditionally, and a capsule
dropped with a tilt **settled at about 10 degrees and stayed there** —
`0.2527` of end-to-end height difference, stable from step 80 to step 400, not
asleep. The cause is worth keeping: a contact constraint resists *approach*
along its normal, so the point out in mid-air beneath the raised end was
holding that end up. The manifold has to describe **what is touching**, not
what the shape spans. Dropping points with no penetration fixed it outright:
the same drop now levels to `0.0038` and rests at `0.2469` against a predicted
radius of `0.25`.

#### The test that passed for the wrong reason

The resting check originally dropped the capsule perfectly level, and it passed
— including with the two-point manifold **disabled**. Gravity acts through the
centre of a level capsule, nothing applies a torque, and a single point holds it
fine. The test was measuring symmetry, not the manifold.

Dropping it tilted and slowly spinning is what makes levelling something the
contact has to do. With the second point disabled that version fails four ways,
including failing to come to rest at all; with it, 18 checks pass.

One test also had to be corrected rather than the code: two capsules crossed at
right angles were asserted to be 0.3 apart and answered 0.4 of penetration. The
code was right — the second capsule's segment passed straight *through* the
first, so the true distance was 0. Offsetting it in z gives the 0.3 that was
intended, and the normal comes back along z as it should.

#### Known limitation, stated so it is not mistaken for a bug

**Capsule–capsule produces one contact point**, even for two capsules lying
exactly alongside each other. A single point cannot resist roll, so a pair of
stacked parallel capsules settles more slowly than a pair of boxes would. The
fix is the same clipped manifold `CollideCapsuleBox` uses; it was not needed for
anything yet.

The Physics3D demo now drops a capsule as every fourth body. They roll down the
ramp on their sides like barrels and come to rest flat. They were drawn as two
spheres and a *box* shaft at first, for want of a round primitive — see the
later entry, which replaced it with a cylinder once a humanoid made of them
made the shortcut obvious.

### 2026-08-09 (the 2D broadphase had both problems, and one was worse)

Having found that the 3D grid needed sorting to be equivalent and a threshold
to be worth switching on, the obvious question was whether the 2D grid — on
unconditionally since it was written — had the same two problems. It had.

#### `UseBroadphase` was silently changing the simulation

Grid and brute force were **not the same run**. They diverged at step 56 and
drifted **0.102 world units** apart, which is visible when the bodies are about
0.2 units across. Reproducible to the identical body, step and separation on
every run, so it was deterministic divergence rather than noise.

Two controls before believing it: two brute-force worlds agree bit for bit, so
the comparison reports identity when it should; and applying the 3D sort made
grid and brute force bit-identical immediately.

That second one is the diagnosis, and it is the good news. **Nothing was being
dropped** — the 2D broadphase was correct. It simply visited candidates in cell
order rather than ascending index, and contacts are resolved by sequential
impulses in the order they were found. A different order is a different, equally
valid answer. Had the sort *not* fixed it, the cause would have been a missed
pair, which is a bug of a completely different character.

So the sort is now in 2D as well, with the measurement recorded next to it.

#### And the same crossover, steeper at the bottom

Three runs, speedup over brute force:

|  bodies | speedup       |  bodies | speedup      |
| ------: | ------------- | ------: | ------------ |
|      13 | 0.10 – 0.14x  |     123 | 1.15x        |
|      28 | 0.33 – 0.38x  |     203 | 1.52x        |
|      53 | 0.62 – 0.85x  |     403 | 2.47 – 2.56x |
|      83 | 0.85 – 0.86x  |     803 | 4.73 – 4.77x |

Crossover is around 100, and at 13 bodies the grid is **seven to ten times
slower** — worse than 3D's 3.4x, because a 2D pair test is cheap enough that the
grid rebuild dominates sooner. Breakout runs about 50 bodies and had been paying
for this the whole time.

`BroadphaseMinBodies = 100`, and as in 3D the threshold is only safe because the
sort landed first.

One difference from 3D worth keeping: the threshold gates **the pair search
only**. Below it the grid is left *dirty* rather than disabled, because
`Raycast` builds it on demand and a ray query is O(rays x bodies) — it can pay
for a grid in a world far too small for pair testing to. Verified: below the
threshold the pair search builds no grid, a raycast then builds one and still
reports its hit.

#### What moved

Two demo captures changed, and only the two that should have: **Physics2D** and
**Scene**, the demos with many interacting dynamic bodies where contact order
decides the outcome. Breakout is unchanged despite being below the threshold —
it has one moving body, so there is no order for the ordering to matter to.
Lighting2D and Acoustics2D are unchanged; their obstacles are static.

Both new frames were checked rather than assumed: bodies settled on the slope
and the ramp, nothing exploded or sank through.

### 2026-08-09 (a 3D broadphase, and the threshold the measurement forced)

`PhysicsWorld3D` tested every pair, as the 2D world did until a profile asked
otherwise. It now has the same uniform grid: bodies bucketed by the cells their
world bounds overlap, and only bodies sharing a cell handed to the narrowphase.

#### The property that makes it checkable

A broadphase is only allowed to make the narrowphase cheaper. If it changes the
answer it is not an optimisation, it is a bug that happens to be faster — and
the failure is silent, because a dropped pair leaves no contact behind and so
nothing to notice.

The grid therefore **gathers its candidates and sorts them** before testing,
rather than testing them in cell order. Cell order is deterministic, but it is
not the order brute force would have used, and contacts are solved by
sequential impulses, which are order-dependent. Without the sort the two paths
reach slightly different, both-valid answers and the toggle stops being an A/B.
With it, the claim under test is the strong one:

> With the grid on and off, the two worlds stay **bit-identical** for the whole
> run — same positions, orientations, velocities, angular velocities, to the
> last bit.

Verified over 240 steps and 63 bodies, alongside the saving: **8.5% of the
brute-force candidate pairs**, with the brute-force count checked against
`n(n-1)/2 x steps = 468720` exactly — arithmetic the world knows nothing about.
An impossible cell size builds no grid and falls back to testing every pair,
which is also checked.

The negative control is the good part. Making `BodyBounds` ignore orientation —
the 3D form of the turned-box trap 2D already fell into, where a box at an
angle reaches `|R| * h` rather than `h` — diverges at step 37 and loses 186
contacts. And it tests **fewer** pairs while doing it: 7.2% against the correct
version's 8.5%. A broadphase that drops pairs looks *better* on the only metric
a pair count can offer, which is precisely why "fewer pairs" is not a success
criterion and the equivalence check is.

#### The threshold, which was not the plan

The grid was going to default on, as it does in 2D. Measured first, three runs
per row, speedup over brute force:

|  bodies | speedup       |  bodies | speedup       |
| ------: | ------------- | ------: | ------------- |
|      13 | 0.30x         |     203 | 1.31 – 1.43x  |
|      43 | 0.65x         |     253 | 1.48 – 1.56x  |
|      83 | 0.76 – 0.83x  |     503 | 2.80 – 3.49x  |
|     123 | 0.99 – 1.01x  |    1003 | 8.11 – 8.72x  |
|     153 | 0.84 – 1.10x  |         |               |

**Below about 120 bodies the grid loses**, and at 13 it loses badly — three
times slower than simply testing every pair. Rebuilding the grid and sorting
the candidate lists costs more than the pairs it rejects until there are enough
pairs for the rejection to matter. Break-even is around 123; 153 straddles 1.0x
depending on the run; 203 is the first count where the win is unambiguous in
every run.

So `BroadphaseMinBodies = 200`: below it the grid is not merely unused, it is
not built. Set at the first reliable win rather than at break-even, because the
noisy band is not worth switching for.

Switching automatically is only safe *because* the two paths are bit-identical.
If they disagreed, the simulation would change the instant a body count crossed
the threshold — a body spawning and every other body's trajectory shifting — and
that would be far worse than being slow. The equivalence earns the right to the
threshold.

The Physics3D demo defaults to 90 bodies and so runs brute force; its **Max
bodies** slider reaches 300, so raising it crosses the boundary live, and the
panel now names which path is in use and how many pairs it tested.

#### One measurement that was the measurement's fault

Six of the seven demo captures came back byte-identical. **Cube3D did not** —
and it is not a physics demo at all.

Restoring the base commit's `PhysicsWorld3D` in the same worktree produced the
*same* hash as the broadphase build, which cleared the change outright: the
difference is between the two worktrees, not between the two commits.
`imgui.ini` was the obvious suspect and was wrong — swapping the layout file
changed nothing.

The pixels settled it. Exactly 87 pixels differ, every one by the same RGB
delta — a light blue line present in one shot and not the other, not shading.
That is a gizmo axis, and `Cube3D::UpdateGizmo` reads
`Input::GetMousePosition()` to decide which axis is highlighted. **The capture
depends on where the physical mouse cursor happens to be sitting.**

Which makes Cube3D unusable as a byte-exact regression reference across
sessions, for the same reason ImGui slider state is: it reaches the demo
without going through the recorded input or the fixed step. Noted in HANDOVER;
the other six demos are unaffected.

### 2026-08-09 (multi-viewport ImGui, and the one line that carries it)

Panels can now be dragged out of the window into their own OS windows, behind
`--viewports` (or `ImGuiLayer::EnableViewports` before the layer is attached).

The config flag is the easy half. The half that matters is four lines in
`ImGuiLayer::End`:

```cpp
GLFWwindow* backup = glfwGetCurrentContext();
ImGui::UpdatePlatformWindows();
ImGui::RenderPlatformWindowsDefault();
glfwMakeContextCurrent(backup);
```

`RenderPlatformWindowsDefault` makes each extra viewport's GL context current
in turn and **leaves the last one current**. Everything later in the frame is
then talking to the wrong window — the swap, and more quietly the screen
capture, which reads the default framebuffer of whatever context happens to be
current.

That is not theoretical, and it is the reason this item was never just a flag.
With the restore deleted, a `--capture` of Breakout came back as *the undocked
panel's window*: the little test panel adrift in a field of uninitialised
white, at the main window's dimensions. The scene was never in the file.

Verified with a temporary test, since a panel cannot be dragged out by a
script. One was positioned outside the main viewport's bounds instead, which is
what makes ImGui give it a platform window of its own — it lived in
`ImGuiLayer::Begin` rather than in `TestEnv` precisely so it would also render
under `--hide-ui`, which is the only way to compare captures that are otherwise
reproducible.

- **A second platform window really is created** — 2 viewports, not 1. Without
  that the rest of the test would have been measuring nothing.
- **The main GL context is current after `End()`** — and the negative control,
  skipping the restore, flips exactly that check to FAIL while leaving the
  viewport count alone.
- **All seven demos capture byte-identically with `--viewports` on**, matching
  the no-viewport reference hashes. Multi-viewport costs nothing in
  reproducibility, so recordings and captures are unaffected.

**Off by default**, unlike docking. An undocked panel is a real OS window with
its own GL context, which is a cost a game that never undocks one should not
pay. It also has to be set *before* `OnAttach`: the flag is read once when the
ImGui context is created. `Application` therefore parses the command line
before pushing the ImGui layer rather than after — `PushOverlay` runs
`OnAttach` immediately, so the old order would have set the flag on a context
already built without it, and `--viewports` would have silently done nothing.

Style is forced opaque and square-cornered when viewports are on: rounded
corners and a translucent background show the *desktop* through the gaps once a
panel is its own window.

Not tested by a person dragging a panel: that needs hands. What is tested is
that the platform window is created, rendered, and that the context comes back.

### 2026-08-09 (three debt items, one of which was worth declining)

Renderer and build debt, taken because it was small and self-contained. Two
landed; the third was measured and dropped, which is the interesting one.

#### `GL_MAX_TEXTURE_IMAGE_UNITS`, asked rather than assumed

`Renderer2DData::MaxTextureSlots` was `constexpr 16` with a comment explaining
that 16 is the OpenGL 3.3 floor and so the largest safe value without asking
the driver. The fragment shader carried a matching 16-case switch, written out
by hand — GLSL 330 cannot index a sampler array with a non-constant, so the
switch is the only legal way to select a slot.

Now `RendererAPI::GetMaxTextureSlots` queries it in `Init`, the slot array is a
`std::vector` sized to the answer, and the switch is generated to match. **This
driver reports 32**, so a batch holds twice the distinct textures it did.

Two things worth recording:

- **`GL_MAX_TEXTURE_IMAGE_UNITS`, not `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS`.**
  The combined figure sums every shader stage — on a driver reporting 16 per
  stage it comes back as 80, and a fragment shader declaring 80 samplers fails
  to link on exactly the hardware the query was supposed to protect.
- **Clamped to `[16, 32]`.** The floor because a smaller answer in a valid 3.3
  context means the query failed, not that the hardware is small. The ceiling
  because past 32 the generated switch and the uniform array grow for nothing.

Verified by a temporary self-test, since slots 16–31 were code that had never
run before and a shader that links proves nothing about them. 31 textures of
known distinct colours, one quad each, every quad read back: **slot i samples
texture i, 0 of 31 wrong**, all in one draw call, and one texture more splits
into two. The expected colours are computed from the slot index and owe nothing
to the renderer, so agreeing with them means the mapping is right.

The clean pass was distrusted and given a negative control: regenerating the
switch as `case i -> u_Textures[(i+1) % slots]` made every slot read its
neighbour's colour, exactly as injected. The check has teeth.

Then the regression question — does the *generated* shader still render what
the hand-written one did? Clamping the count to 16 reproduces the old shader
exactly, so both were captured: **all seven demos are byte-identical at 16 and
at 32 slots.** No scene here uses more than 16 textures, which is the point —
the change adds headroom without moving a pixel.

One wrong turn, caught by the same rule that catches everything here. The first
capture run gave three demos the *same* hash. The demos were fine; the names
were wrong. `DemoRegistry.h` uses short names (`Lighting`, `Physics`, `Scene`),
not class names, and an unmatched `--demo` warns and falls back to the default
demo — so three runs had captured the same demo three times.

#### The `TestEnv` PCH, declined

The roadmap said a PCH for `TestEnv` "would actually pay off", because the demo
headers now pull in most of the engine. Measured before building it:

| phase | no PCH | with PCH |
| --- | --- | --- |
| preprocess (`-E`) | 0.15 s | — |
| parse + sema (`-fsyntax-only`) | 1.91 s | 1.25 s |
| full compile | 5.81 s | 5.21 s |

Codegen is 3.90 s of the 5.81 s — **67% of the compile, and a PCH cannot touch
any of it.** A PCH removes parse time, and only 0.66 s of that. Header *text*
is not the cost either: preprocessing is 0.15 s.

The premise was wrong in two ways. PCH savings scale with the number of
translation units, and **`TestEnv` is exactly one** — building the PCH costs
2.75 s to save 0.60 s once, so a clean build goes 5.71 s → 7.92 s, **39%
slower**. And the demos being header-only is the reason, not the cure: their
code is inlined into that single TU and has to be code-generated there, which
is precisely the phase a PCH leaves alone.

Reopen it if the demos ever become separate `.cpp` files. That would also
parallelise a build that currently runs one TU on a twelve-core machine — but
it is a design change to how demos are added, not debt, and `DemoRegistry.h`
deliberately trades that away.

#### `premake5`, fetched rather than hunted

The binary is gitignored, so a fresh clone and every new worktree started
without one and failed at the first build with a link to go and find it. Both
happened again setting up the worktree for *this* work, which is how it got
picked.

`egss.py` now downloads a pinned 5.0.0-beta7 on first use and verifies a
**SHA256 of the archive before unpacking**, so a corrupted or substituted
payload never reaches the filesystem as an executable. Confirmed both ways: the
real archive installs and runs, and a deliberately wrong expected hash is
rejected with nothing written. The extracted binary hashes `5e1a55dc…` —
byte-identical to the one that had been downloaded by hand, so pinning the
version changed nothing about the build.

`--no-fetch` restores the old fail-with-a-link behaviour.

The submodules are the same first-five-minutes failure, so `egss.py` now checks
them too — but it names `git submodule update --init --recursive` rather than
running it, because that is a git operation on the user's own repository.

### 2026-08-06 (3D stacking: a face wound the wrong way round)

The stack falls over because `Sat3D` wound half its reference faces backwards.
One line, and it had been there since the clipping was written.

**Finding it started by not tuning anything.** The previous entry's sweep said
the failure was chaotic in the iteration counts, which rules out slow
convergence, so the thing to look for was something changing *identity* between
steps rather than something converging too slowly. A headless harness — the
same four crates and floor, no renderer, `PhysicsWorld3D` being pure arithmetic
— swept the grid in a second and logged what the demo cannot show: manifold
sizes, warm-start match rates, and per-contact impulses.

The manifolds gave it away by step three:

```
step   2  pts[0-1:4 1-2:4 3-4:4]              sat[1-2:A4  2-3:B1  3-4:A4]
step   4  pts[0-1:4 1-2:4 2-3:1 3-4:1]        sat[1-2:A4  2-3:B1  3-4:B1]
```

`B1` is a **face** contact — normal exactly `(0,1,0)`, tilt 0.00°, depth
0.0085 — carrying a single point. Two identical level boxes resting flat should
clip to the four corners of their footprint. The `A` cases did. The `B` cases,
where the *upper* box owns the reference face, did not.

**Reduced to geometry with no simulation at all**, which is where it stopped
being a physics problem:

```
overlap 0.0500: lower-first 4 pts, upper-first 1 pts
overlap 0.0100: lower-first 4 pts, upper-first 1 pts
overlap 0.0050: lower-first 4 pts, upper-first 1 pts
overlap 0.0010: lower-first 4 pts, upper-first 1 pts
```

Same two boxes, arguments swapped. The depth was right in every case, so the
axis test was fine and the clipping was not.

`MostFacingFace` emits the four corners in a fixed `(u, v)` order but flips the
outward normal with `sign`, so a face pointing down a *negative* body axis comes
out wound backwards relative to its own normal. The caller builds clip planes as
`cross(edge, outwardNormal)`, so all four then point inward and Sutherland-
Hodgman keeps the **outside** of the reference face. Worked by hand for the
failing case: the first clip plane is `(1,0,0)` with limit `-0.35`, keeping
`x <= -0.35` of an incident face spanning `[-0.35, +0.35]`. The working case
gives `(-1,0,0)` with limit `+0.35`, keeping `x >= -0.35`. The polygon clips to
nothing, and the "then they must meet at a corner" fallback fabricates one
point.

Why a stack, and why chaotically: body A is the lower box and its `+y` face wins
the tie for reference, which is the case that works — until a hair of tilt lets
the upper box's axis win instead, the reference becomes a *bottom* face, and the
manifold silently drops to one point. One point cannot hold a box level, so it
tips, which produces more tilt. Which box wins that near-tie is decided in the
last bits, and the iteration count changes the last bits.

The fix is `sign` appearing in the second edge as well as in the centre.

| | before | after |
| --- | --- | --- |
| stack standing, sleeping on | 11/20 | **20/20** |
| stack standing, sleeping off | 4/20 | **20/20** |
| manifold size changes per run | 9–53 | **0** |
| warm-start match rate | 0.95–0.99 | **1.000** |
| max penetration | 0.008–0.10 | **0.0085** |

**The independent check** is the one that makes this more than a green table.
Each crate is 1 kg, so a contact holding `n` of them must carry `n · 9.81 / 60`
per step, and nothing in the solver knows how many crates are above it:

```
measured   0.654   0.490   0.327   0.163
expected   0.6540  0.4905  0.3270  0.1635
```

**Sleeping had been hiding it.** With sleeping off the stack stood in only 4 of
20 configurations before the fix, against 11 with it on — so the settings that
"worked" were partly bodies freezing before they could finish falling over. Any
future stack measurement is worth taking with `AllowSleeping = false` for that
reason. Confirmed in the demo too: the stack stands at step 1500 with 25 other
bodies in the scene.

Two things this says about the earlier work. The 21/21 solver test passed
because it used 16/8 iterations, one of the configurations that happened to
land on the working side of the tie — a test that samples one point of a
chaotic parameter space proves very little. And the previous entry blamed the
solver ("the manifold, the friction and the position correction") when the
manifold alone was wrong; the friction and the correction were fine.

### 2026-08-05 (a 3D demo, and what it showed)

`Physics3D`: boxes and spheres dropped onto a ramp, tumbling down it into a
walled pit, with a stack of four crates off to one side. The first place the 3D
solver could actually be *looked* at — everything before this was arithmetic,
and the 2D demo had already shown that arithmetic can be entirely right while
the thing on screen is nonsense.

Two things it caught immediately. Asleep bodies were drawn grey, which made
every settled crate look like part of the scenery; they are dimmed versions of
their own colour now, so a pile that has gone to sleep still reads as boxes and
balls. And orientation is interpolated with **slerp**, not `mix` — lerping two
quaternions leaves an unnormalised one, which is a rotation *and* a scale, so a
fast-tumbling box visibly swells between steps.

#### The stack falls over, and the solver test had been lucky

The demo's four-crate stack collapses on its own, before anything reaches it.
The solver test that passed 21/21 had used 16 velocity and 8 position
iterations; the demo uses the defaults, 8 and 4.

Sweeping both, measuring the top box's height against where it started, where
1.0 is standing and 0.14 is flat on the floor:

```
        p2      p4      p6      p8
 v4    0.14    0.14    0.14    0.14
 v8    0.14    0.99    0.14    0.14
 v12   0.14    0.14    0.14    0.14
 v16   1.02    0.14    0.14    0.71
 v24   0.42    0.42    0.42    0.42
```

That is not slow convergence — it is chaotic. Adjacent settings flip between
standing and collapsed, and twenty-four iterations is worse than eight. The
earlier claim that boxes "stack" was generalised from a single configuration
that happens to work, which is exactly the error the sweep exists to catch.

The measurement itself needed fixing first: the obvious metric, the angle of a
box's local up axis, is meaningless for a **cube** — one that has merely yawed
reads as perfectly level, and one resting on a different face reads as 90°
while still being a fine stack. Only the height distinguishes standing from
fallen.

Left visible in the demo rather than hidden behind the settings that work.

### 2026-08-05 (3D rigid bodies, piece three: the join)

`Sat3D` wired into a narrowphase, contacts carrying lever arms, and the
solver's angular terms. Boxes and spheres now collide, stack, roll and settle.

Most of it is the 2D solver with the cross product left as a vector and the
scalar inertia replaced by the world tensor. The effective mass along an axis
goes from `(r × n)² / I` to `n · ((I⁻¹ (r × n)) × r)` — the same quantity
written where facing matters. Friction needs **two** tangents rather than one,
since a contact plane in 3D is a plane, and they are clamped against a *circle*
of radius `μJₙ` rather than per axis; a per-axis clamp bounds friction by a
square, letting a body slide 41% harder along a diagonal.

21 checks — though see the entry below on what they did *not* cover. Linear
and angular momentum are unchanged across an off-centre
impact between two tumbling boxes — to five decimal places, which is the check
that proves the levers and the tensor reached the contact. A resting crate's
contact carries `m g dt` to 0.02%. And the headline: **a sphere rolls down a
20° slope at `(5/7) g sin θ`**, measured 0.49% out. The 2D disc did `(2/3)`;
the difference is entirely `⅖mr²` against `½mr²`, so the number distinguishes
a correct tensor from a plausible one.

#### The stack collapsed, and the cause was not where I looked

Four stacked boxes tilted 0.09°, 0.32°, 0.84°, 2.24° and were over within four
seconds. Boxes resting on the *static floor* never tilted; only box-on-box did.

Isolating it by turning one thing off at a time:

| configuration | worst tilt after 200 steps |
| --- | --- |
| as written | 99.6°, collapsed |
| no friction | 0.000° |
| 64 velocity iterations | 0.000° |
| no position correction | 3.6° |
| **no angular position correction** | **0.24°** |

Two wrong guesses on the way. The clip was returning **eight** contact points
for two identical stacked boxes — four real corners plus four spurious vertices
where a 0.03° tilt makes each edge cross its own clip plane near the middle.
That was a genuine defect and worth fixing (manifolds now reduce to the four
most widely spread points, which is what holds a footprint), but it was not the
cause. Nor was solving normal and friction interleaved per point rather than in
separate passes; splitting them is more correct and changed nothing.

The cause was the **angular half of position correction**. A small box has an
inverse inertia around 24, so the rotation implied by a correction impulse is
large next to the linear nudge beside it — and it was applied per point, four
points deep, eight iterations a step, each using a lever measured before the
previous point had turned the body. The asymmetries compound and friction locks
in whatever lean they produce, which is why it needed both to go wrong.

2D rotates in position correction and is right to: there the correction is a
scalar and a face's two points are symmetric. In 3D it is removed, and nothing
is lost — levelling a crate that landed on a corner is the velocity solver's
job, and a crate dropped flat onto an 18° slope still settles flush to 0.21°.

### 2026-08-05 (3D rigid bodies, piece two: `Sat3D`)

The separating axis test one dimension up. In 2D four directions could separate
two boxes; in 3D there are **fifteen** — three face normals each, plus the nine
cross products of one box's axis with the other's.

Those nine are not an optimisation to skip. They are the only axes that can
separate two boxes crossing like the arms of an X, where every face of each
still overlaps every face of the other. The test builds exactly that case: two
bars rolled 45° about their own lengths and crossed at right angles. Each
reaches `0.5cos45 + 0.5sin45 = 0.70711` along z, so they clear at 1.41421, and
at a separation of 1.5 they are apart — while **all six face axes still
overlap**. A six-axis test calls that a collision.

40 checks. Manifolds come from clipping the incident face against the reference
face's four side planes — the same Sutherland-Hodgman idea as 2D, run twice
more because a face here is a rectangle rather than a segment. A cube resting
flat gives four points at its footprint corners; a cube overhanging a narrow
plinth has them clipped to the plinth, since a point past the edge is resting
on nothing; a cube balanced on a corner gives one.

Two details that are tolerances rather than fudges:

- **Parallel axes give a zero-length cross product**, which is not a direction.
  Testing it anyway gives both shadows zero width, an overlap of exactly zero,
  and the conclusion that the boxes are apart. Two axis-aligned boxes hit this
  on all nine at once — so the classic failure mode breaks the easiest case
  there is, which is why it is the first thing checked.
- **Edge-edge axes are believed only if they beat a face axis by 0.5%.** Two
  boxes resting face to face have edge axes very nearly as good, and floating
  point regularly hands one a marginally smaller overlap; taking it skews the
  contact normal a few degrees off the surface and a resting box slowly slides.
  The bias decides which axis wins and is deliberately kept out of the reported
  depth — reporting the biased figure would make every edge contact half a
  percent deeper than it is, and the position solver would push that far too
  far, every step, forever.

#### The oracle was one-sided and I used it as two-sided

Alongside the hand-worked cases there is an independent ground truth: the
fifteen axes are provably sufficient, so if SAT says two boxes touch and *any*
direction separates them, SAT is definitively wrong. Ten thousand directions
spread over a Fibonacci sphere is a cheap way to look.

It promptly "disagreed" with a correct answer. The bars at `dz = 1.5` are
apart, SAT said so, and the search found nothing — because that is the half of
the oracle that proves nothing. The gap is 0.0858 on bars four units long, so
the cone of directions that separates them is far narrower than the two degrees
ten thousand samples resolve. The comment above the function said exactly this
and the assertion below it ignored it.

The fix is to assert only the sound direction — where the search finds a gap,
SAT must too — and to verify "apart" by hand from the projections instead. Over
a 60-configuration sweep, half of them overlapping, there are no disagreements
in that direction.

### 2026-08-05 (3D rigid bodies, piece one: angular state)

The same three-piece plan 2D used, because building the halves separately and
checking each alone is what kept that honest. This is piece one: `RigidBody3D`
and `PhysicsWorld3D` with position, orientation and their integration.
**Nothing collides** — bodies fall, tumble and pass straight through each
other, which is deliberate and is the point.

Two things stop this being 2D with an extra component:

- **Orientation is a quaternion.** Three angles cannot represent orientation
  without gimbal lock, and a matrix has six redundant numbers to keep
  consistent.
- **Inertia is a tensor written in body space**, so it has to be rotated into
  world space as `R·I·Rᵀ` whenever the body turns. Skip that and a box behaves
  correctly only while it happens to be axis-aligned.

31 checks. Tensors against the textbook — a 2×3×4 box of mass 12 has diagonal
(25, 20, 13), a sphere `⅖mr²` — with the box deliberately not a cube, because
pairing the wrong extents with an axis is invisible on one. A quarter turn about
z swaps the world tensor's xx and yy entries, and a sphere's is rotation-
invariant, which is the control that would catch an error the box test could
mask.

#### Angular momentum is conserved; angular velocity is not

In 2D those were the same statement up to a constant. In 3D they part company,
and everything interesting lives in the gap. `L` is captured before the body
turns and angular velocity recovered from it after, rather than carried across
unchanged — the tensor has moved, so the same momentum implies a different
velocity, and *that line is where tumbling comes from*.

The check that this is right is not a formula but a phenomenon: the **tennis
racket theorem**. Spin a body about its largest or smallest principal axis and
it is stable; spin it about the middle one and it flips over, periodically,
forever. Nothing in the code knows this. Measured as how far the spin axis
wanders in the body frame, where 2.0 is a complete reversal: major axis 0.010,
minor 0.010, intermediate **2.000**.

#### Two failures, one mine and one real

The flip did not appear at first, and the test was wrong rather than the code:
it measured the spin axis in *world* space, where angular momentum is fixed and
angular velocity stays close to it. The body tumbles under `w`; `w` does not
swing about. In the body frame it was there all along.

The real one: rotational energy grew **18.4%** over ten seconds of free
tumbling while angular momentum held to five decimals. Torque-free motion
conserves both, so that was the integrator doing work. Substepping the
orientation update bought it back roughly proportionally — 4.2% at four
substeps, 1.31% at sixteen — which is first-order convergence and an expensive
way to buy accuracy.

Taking angular velocity from the **midpoint** of the step instead of its start
fixed it outright: **0.02%**, at a single step, for one extra tensor rebuild
rather than four times the work. The substep count was then measured to be
actively *harmful* — 0.08% at four, 0.31% at sixteen — because each substep
renormalises a quaternion and rounding had become the largest error left. The
knob was removed rather than defaulted to one.

Final drift over a full minute of undamped tumbling: energy +0.130%, angular
momentum +0.059%. Identical figures in Debug and Release.

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
