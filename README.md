# EGSS

Every Game Starts Somewhere, Why not here?

A game engine built from scratch, following along with TheCherno's Hazel
series. `EGSS` is the engine itself, built as a shared library; `TestEnv` is a
sandbox application that links against it.

Current state: a window opens, logging works, and the event classes are
defined. Events are not dispatched to the application yet.

## Layout

| Path | What it is |
| --- | --- |
| `EGSS/src/Egss/` | Engine core — application loop, logging, event types |
| `EGSS/src/Platform/` | Per-platform backends (`Windows/`, `Linux/`) |
| `EGSS/vendor/` | Submodules: GLFW, spdlog |
| `TestEnv/src/` | Sandbox app that consumes the engine |
| `premake5.lua` | Build definition — the source of truth for both platforms |

Project/solution files are generated from `premake5.lua`. Don't hand-edit them.

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

**Windows** — Visual Studio with the C++ desktop workload.

A `premake5` binary is expected at `vendor/bin/premake/` (`premake5` on Linux,
`premake5.exe` on Windows). It is gitignored, so grab it from
[premake.github.io](https://premake.github.io/download) if it's missing.

## Building

**Linux:**

```sh
./BuildProject.sh          # generates makefiles from premake5.lua
make config=debug          # or: config=release, config=dist
```

**Windows:**

```
BuildProject.bat
```

then open `EGSS.sln` and build.

Regenerate whenever `premake5.lua` changes or you add source files — the file
globs are expanded at generation time, not build time.

## Running

```sh
./bin/Debug-linux-x86_64/TestEnv/TestEnv
```

On Windows, `bin\Debug-windows-x86_64\TestEnv\TestEnv.exe`. The engine library
is copied next to the executable as a post-build step, and on Linux the binary
carries an `$ORIGIN` rpath so it resolves there.

A 1280x720 window opens. **Closing the window does not exit the program yet** —
`m_Running` is never cleared, because window events aren't wired to the
application loop. Kill it from the terminal.

## Changelog

### 2026-07-31

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
