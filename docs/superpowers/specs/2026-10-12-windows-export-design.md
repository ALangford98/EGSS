# Windows export (export piece 4)

**Status:** built 2026-10-12. The owner asked for every remaining export
piece without stopping between them, so the design decisions below were
made during the work and are recorded here for review.

## Decisions

1. **Cross-compiled from Linux with llvm-mingw** (Clang + MinGW-w64 + UCRT,
   `20260922`), fetched by `gs.py fetch llvm-mingw` with its published
   SHA-256. Chosen over GCC MinGW (a system package, not fetchable the same
   way) and `zig cc` (less complete C++17 library on Windows).
2. **premake's own Windows filters, driven cross:** `gs.py build <config>
   --target windows` generates with `--os=windows`, runs `make GSPlayer`
   with `CC/CXX/AR/RC` set to the cross tools, then regenerates the Linux
   project files. Composes with `--player-scripts`.
3. **One GLFW window for both desktops.** `Platform/Linux/LinuxWindow` was
   GLFW all along; it became `Platform/Glfw/GlfwWindow` (and `GlfwInput`),
   with only `--wallpaper`'s X11 code platform-specific. The stale
   `Platform/Windows/` copy (233 lines against 397) was deleted.
4. **Ship libc++.dll and libunwind.dll** beside the game rather than link the
   C++ runtime statically: a static copy in both the exe and `GS.dll` would
   give each its own runtime, and strings, shared pointers and exceptions
   cross that boundary. Everything else the game imports is a system DLL or
   the UCRT (Windows 10+).
5. **Dist is a GUI-subsystem app** (`WindowedApp`): no console window
   behind a shipped game. Debug and Release keep the console for logs.
6. **The `ScriptEngine` split planned for this piece was not needed**: its
   only shell-out (`RunSyntaxCheck`) is already compiled out without
   `GS_REPO_ROOT`, which the player never defines.
7. **Package layout:** `<out>/windows/<Game>/` with `<game>.exe`, `GS.dll`,
   the two runtime DLLs and `game/`; archived as `.zip` (stdlib `zipfile`;
   no `zip` here). Linux packages moved to `<out>/linux/<Game>/` in the same
   change, so one output folder holds both.
8. **Deferred:** an embedded `.ico` (needs an icon conversion and a
   `windres` step in the per-game build); signing.

## What porting took

- `<Windows.h>` -> `<windows.h>` (MinGW is case-sensitive on Linux).
- `NOMINMAX`, `WIN32_LEAN_AND_MEAN`; `#undef near/far` after `windows.h`
  (16-bit-era empty macros that turned `float near = 0.0f` into
  `float = 0.0f`).
- A shim header for libc++'s removed private `<__std_stream>`, which the
  fmt bundled in the vendored spdlog includes on Windows + libc++ (see the
  shim's comment) -- the submodule is untouched.
- `Socket.cpp` on Winsock: `WSAStartup` once, `closesocket`, `ioctlsocket`
  for non-blocking, `intptr_t` handles; a UDP `WSAECONNRESET` (Windows
  reporting an ICMP "port unreachable" for an earlier send) is "nothing to
  read", not an error.
- `GS::ImGuiLayer::WantsKeyboard()`: `ScriptEngine` asked ImGui directly, but
  a Windows DLL exports only what it marks, so the runtime could not link
  ImGui; `Runtime/` no longer includes `imgui.h`.
- The player finds its folder with `GetModuleFileNameW`.
- premake's `{COPY}` expands for the target's `cmd` while make runs the
  host's `/bin/sh`: Windows-target copies spell `mkdir -p`/`cp` on a
  non-Windows host.

## Verification

A portable Wine (11.18, fetched, verification only) runs the exported games
on this machine with OpenGL passed through to the real driver. BreakoutRecreation
(compiled scripts, cross-built registry) and the falling-box game
(interpreted script as JS), each exported for Windows, unzipped fresh and
run from `/` with no arguments: captures **byte-identical** to editor Play
on Linux at step 150. The Dist exe is subsystem Windows GUI. Never run on
real Windows hardware -- Wine is the evidence.
