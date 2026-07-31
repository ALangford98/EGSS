# EGSS

Every Game Starts Somewhere, Why not here?

A game engine built from scratch, following along with TheCherno's Hazel
series. `EGSS` is the engine itself, built as a shared library; `TestEnv` is a
sandbox application that links against it.

**Current state:** a window with an OpenGL 3.3 core context, a working event
system, a layer stack, polled input, frame timing, a renderer abstraction
(vertex/index buffers, declarative vertex layouts, shaders, textures, an
orthographic camera), and ImGui as a debug overlay. `TestEnv` draws a textured
quad and a colored triangle, moves the camera with the arrow keys, and exposes
frame time and camera state in an ImGui panel. See [Roadmap](#roadmap) for
what's left.

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

Four projects build in dependency order: **GLFW** (static lib) → **Glad**
(static lib) → **EGSS** (shared lib) → **TestEnv** (executable). The two vendor
projects have their own `premake5.lua` files, pulled in by `include` directives
at the top of the root script.

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

You should see a 1280x720 window with a colored triangle, and:

```
[20:37:54] EGSS: Creating Window Every Game Starts Somewhere (1280, 720)
[20:37:54] EGSS: OpenGL 4.6 (Core Profile) Mesa 26.1.5 | Mesa Intel(R) Iris(R) Xe Graphics (RPL-U)
```

Arrow keys move the camera; `A` and `D` rotate it. Closing the window exits
cleanly.

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

All of this is in `Application`'s constructor. It's temporary — it belongs in a
renderer, and the roadmap tracks moving it there.

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

There is no frame timing yet, so nothing can be animated at a rate independent
of framerate. Delta time is on the roadmap.

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

`glGetError` is not called anywhere yet. A debug-context callback
(`GL_DEBUG_OUTPUT` with `glDebugMessageCallback`) is a much better answer and
is on the roadmap — it turns silent failures into log lines automatically.

[**RenderDoc**](https://renderdoc.org/) is the tool worth installing when the
above isn't enough. It captures a frame and lets you inspect every draw call,
the bound state, buffer contents, and the shaders as they were compiled.

---

# Roadmap

Groups 1-5 from the original plan are done. What follows is what remains.

## Still outstanding

- [ ] **Batching in `Renderer`.** `Submit` draws immediately;
      `BeginScene`/`EndScene` exist to make batching possible but don't batch
      yet. This is the single biggest performance item
- [ ] **A `ShaderLibrary`**, so shaders are looked up by name rather than
      passed around as `shared_ptr`s
- [ ] **A profiler** — instrumentation timers and scope macros, ideally
      feeding a Chrome-tracing JSON
- [ ] **Verify the Windows build.** Nothing since the Linux port has been
      compiled there. The platform code is mirrored and the premake filters
      are symmetric, but it is unverified
- [ ] **A PCH for `TestEnv`.** Only the engine has one
- [ ] **Vendor a `premake5` binary per platform**, or script fetching it
- [ ] **A 2D renderer layer** (`Renderer2D::DrawQuad`) over the current
      primitives, which is where the Hazel series goes next
- [ ] **Framebuffers**, needed before an editor viewport is possible

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

---

# Changelog

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
