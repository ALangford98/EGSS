# GS

Every Game Starts Somewhere, Why not here?

> **An LLM agent, or otherwise starting here with no prior context?** Read
> **[CLAUDE.md](CLAUDE.md)** before anything else in this repo, this file
> included. It's a tiered reading order — ~3k tokens to correctly answer
> "what are we working on," each tier past that read only on a specific,
> named trigger — built because reading this README and the project's other
> docs end to end has, measured, cost hundreds of thousands of tokens for a
> confidently wrong answer. Come back to the rest of this file once
> `CLAUDE.md` itself says to.

A game engine built from scratch, following along with TheCherno's Hazel
series but diverging where it made sense. `GS` is the engine itself, built as
a shared library; `TestEnv` is a sandbox application that links against it.

**Current state:** an OpenGL 3.3 core context, an event system, a layer
stack, polled input, and a **fixed-timestep** loop with render interpolation.
A **batched 2D renderer** (quads, lines, triangles, circles, sprite sheets)
and a **3D renderer** (lit/textured meshes, instancing, glTF and Wavefront
`.obj` loading) sit side by side; a **scene layer** with generational entity
handles and dense component stores backs both. **Physics** covers 2D
(rotation, oriented SAT manifolds, warm-started impulses, island sleeping, a
uniform-grid broadphase) and a considerably more complete 3D world (boxes,
spheres, capsules, and compound multi-box shapes; several joint types with
limits and motors; a humanoid ragdoll that balances and walks). Procedural
**voxel terrain** runs from a single chunk to a full solar system: chunked
signed-distance fields, marching cubes/tetrahedra meshing, LOD with seam
welding, structural stress simulation, drainage-based biomes, and
planet-scale atmosphere/orbital mechanics. A lock-free **audio engine** does
positional sound, occlusion and ray-traced acoustics in 2D and 3D; an
instrumenting **profiler** exports Chrome traces; and ImGui (docking,
multi-viewport) is both a debug overlay and the basis for...

**...a real in-process editor**, not just a demo harness: scene composition
with undo/redo, a project format, an embedded terminal and text editor, a
file tree, mesh authoring (vertex/face editing on placed meshes plus UV
template export), and **GameStart Script (`.gss`)** — TypeScript that runs
live in an embedded QuickJS runtime, or transpiles to real, statically-linked
C++ once a script is ready to "graduate." A UDP **multiplayer foundation**
(a custom reliable-ordered channel, host-as-server replication, RPCs)
rounds out the networking side.

`TestEnv` holds eighteen demos, from `Breakout` up through a solar system you
can fly a lander down onto and dig into with the voxel terrain tools. See
[Still outstanding](#still-outstanding) below for lower-level engine gaps,
and **[editor_roadmap.md](editor_roadmap.md)** for what's next on the
editor/game-building side.

> **New to the codebase?** Start with **[docs/ENGINE.md](docs/ENGINE.md)** — the
> frame's call path, the decisions that explain the rest, and the whole API
> you'd use day to day. Then read `TestEnv/src/Breakout.h`, a commented worked
> example of building a game on it.
>
> **Picking this up after a break?** **[docs/STATE.md](docs/STATE.md)** has
> the current working context — where the work is *this week*, kept short
> and cheap on purpose (git always wins where they disagree).
> **[docs/HANDOVER.md](docs/HANDOVER.md)** has the trap index — specific
> things that have cost time more than once — and how work gets verified here.
>
> **Want to build something?** **[docs/LIGHTING_EXERCISE.md](docs/LIGHTING_EXERCISE.md)**
> is a staged walkthrough of writing a 2D lighting system, kept in the order it
> was actually built — including the wrong turns, which are the useful part.

## Layout

| Path | What it is |
| --- | --- |
| `GS/src/GS/` | Engine core — application loop, layers, events, input, logging |
| `GS/src/GS/Renderer/` | Backend-agnostic renderer interfaces |
| `GS/src/GS/Scene/` | `Scene`, `Entity`, `ComponentStore`, `Components` |
| `GS/src/GS/Physics/` | `PhysicsWorld2D`, `RigidBody2D`, raycasts, broadphase |
| `GS/src/GS/Audio/` | `AudioEngine`, `AudioClip` — mixer, positional sound, reverb; `Acoustics2D` — ray-traced room response |
| `GS/src/GS/Debug/` | `Instrumentor` — scope timers and Chrome-trace capture |
| `GS/src/Platform/` | Backends: `Windows/`, `Linux/`, and `OpenGL/` |
| `GS/vendor/` | GLFW, spdlog, glm, imgui (submodules); Glad, stb_image and miniaudio (checked in) |
| `TestEnv/src/` | Sandbox app that consumes the engine |
| `TestEnv/assets/` | Sample models; copied next to the executable on build |
| `premake5.lua` | Build definition — the source of truth for both platforms |
| `gs.py` | Build/run wrapper; always regenerates, so new files are never missed |
| `editor_roadmap.md` | The editor/game-building wishlist — what's next, ordered easiest to hardest |
| `docs/` | `ENGINE.md` (orientation), `STATE.md` (current work, cheap and volatile on purpose), `HANDOVER.md` (trap index + how work is verified), `CHANGELOG.md` (the full history, moved out of this file), `LIGHTING_EXERCISE.md` (worked build) |
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
premake5.lua  ──[ BuildProject.sh ]──>  Makefile, GS/Makefile, TestEnv/Makefile
              ──[ BuildProject.bat ]──> GS.sln, *.vcxproj
```

Five projects build in dependency order: **GLFW**, **Glad**, and **ImGui**
(static libs) → **GS** (shared lib) → **TestEnv** (executable). GLFW and Glad
carry their own `premake5.lua`; ImGui ships none, so GS supplies one at
`GS/vendor/imgui_premake5.lua` — deliberately outside the submodule, since a
file added inside it would be lost on re-clone and would leave the submodule
permanently dirty. All three are pulled in by `include` directives at the top
of the root script.

**Either platform**, via the wrapper script:

```sh
./gs.py                  # build debug and run it
./gs.py build release    # or debug (default), dist, all
./gs.py run release      # build, then launch from beside the binary
./gs.py clean all
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

then open `GS.sln` and build.

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
| `debug` | `GS_DEBUG`, `GS_ENABLE_ASSERTS` | yes | no |
| `release` | `GS_RELEASE` | yes | `On` |
| `dist` | `GS_DIST` | no | `Full` |

`debug` is the only config with asserts compiled in, and the only one that
requests an OpenGL debug context.

Platform defines are `GS_PLATFORM_LINUX` or `GS_PLATFORM_WINDOWS`, set by
the `system:` filters. `Core.h` `#error`s on any other platform.

### Sanitizers

```sh
./gs.py sanitize              # build instrumented, run every demo under it
./gs.py sanitize release      # the config where UB actually bites
./gs.py build --sanitize      # just the build
./gs.py run --sanitize -- --demo OpenWorld
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
./gs.py run release -- --demo Slime --wallpaper
./gs.py windows        # in another terminal: react to open windows
```

`--wallpaper` marks the window `_NET_WM_WINDOW_TYPE_DESKTOP` and sizes it to the
union of every monitor. `--wallpaper-scale N` and `--wallpaper-density F` trade
detail against cost; `--show-ui` puts the panels back so the breeds can be tuned
while it runs; `--no-windows` ignores the desk even when the bridge is up.

`./gs.py windows` is the KWin bridge described in the 2026-08-21 changelog
entry. It runs until interrupted and unloads its KWin script on the way out. It
is not required: without it the colonies simply do not know about your windows.

### Output layout

```
bin/<Config>-<system>-x86_64/
  ├── GS/       libGS.so
  └── TestEnv/    TestEnv + a copy of libGS.so
bin-int/          object files and precompiled headers
```

Vendor static libraries build into their own directories rather than the
top-level `bin/`, because each vendor `premake5.lua` resolves `targetdir`
relative to itself — for example
`GS/vendor/glfw/bin/<Config>-<system>-x86_64/GLFW/libGLFW.a`. They are linked
into `libGS.so`, so nothing needs to ship them.

The engine library is copied next to the executable by a post-build step. On
Linux `TestEnv` is also linked with an `$ORIGIN` rpath, so it resolves
`libGS.so` from its own directory rather than the system path — you can move
the folder anywhere and it still runs.

## Running

```sh
./bin/Debug-linux-x86_64/TestEnv/TestEnv
```

On Windows, `bin\Debug-windows-x86_64\TestEnv\TestEnv.exe`.

You should see a 1280x720 window running one of the demos, plus:

```
[22:00:24] GS: Creating Window Every Game Starts Somewhere (1280, 720)
[22:00:24] GS: OpenGL 4.6 (Core Profile) Mesa 26.1.5 | Mesa Intel(R) Iris(R) Xe Graphics (RPL-U)
[22:00:24] GS: GL: 32 fragment texture units reported, using 32
[22:00:24] GS: Renderer2D initialized (10000 quads/batch, 32 texture slots)
[22:00:24] GS: Audio PulseAudio | 48000 Hz, 2 ch, 32 voices
[22:00:24] GS: ImGui 1.92.9b initialized
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
rpath resolves `libGS.so`. Changing it will break the launch with a
library-not-found error.

Breakpoints inside `libGS.so` work normally; GDB resolves them once the
shared library loads. If a breakpoint stays hollow, confirm you built the
`debug` config.

For GL problems specifically, a stepping debugger is often the wrong tool —
see [Debugging rendering](#debugging-rendering) below.

## Adding source files

Engine code goes under `GS/src/GS/`; platform-specific code goes under
`GS/src/Platform/<Platform>/`. The per-platform directories are mutually
exclusive at build time — each `system:` filter `removefiles` the other's
subtree — so a Linux backend never has to `#ifdef` around Windows code.

Anything you add to the engine's public surface needs `GS_API` on the class
so it's exported from the shared library:

```cpp
class GS_API Thing { ... };
```

Then regenerate. New engine headers should be reachable from `GS/src`, which
is on both projects' include path.

The engine uses a precompiled header, `gspch.h`, holding the common standard
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
function pointer and fills them in from a loader callback. `GS/vendor/Glad/`
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
driver's message via `GS_CORE_ERROR` on failure; linking checks
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
      `gs.py` now fetches a pinned, checksummed premake on first use
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
- [x] **Merge or LOD the chunk meshes.** Distinct geometry per chunk, so
      instancing does not apply — fixed 3x3x3 chunk groups instead, one GPU
      mesh each. Landed on TerrainLab first (93 draws down to 9 in the
      default view, see the changelog, 2026-09-04), then `VoxelPlanet.h`
      (1,459 meshed chunks down to 167 groups on a landed Earth, see the
      changelog, 2026-09-07) — the harder case, since chunks stream and
      already carry a stride-based LOD, though it turned out concatenation
      does not care what stride a member chunk was meshed at
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
driver messages to the log by severity. `GS_ENABLE_ASSERTS` defined in the
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

## Changelog

Moved to **[docs/CHANGELOG.md](docs/CHANGELOG.md)** (2026-09-15) — newest-first,
~170k tokens over hundreds of entries, meant to be grepped rather than read
whole. See `docs/STATE.md` for what's current; the changelog is the historical
record of how it got there.

