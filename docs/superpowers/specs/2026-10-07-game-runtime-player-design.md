# Game runtime and standalone player (export piece 1)

**Status:** approved 2026-10-07
**Scope:** piece 1 of the export roadmap below. Each later piece gets its own
spec.

## Why

A game made in the editor only runs inside `TestEnv` — one binary holding the
editor, eighteen demos and Play mode. Nothing can be handed to someone else.
The owner wants games exportable to Linux, Windows and the web, and later
Android and iOS. Every one of those needs the same first thing: a **runtime**
that runs a project with no editor in it.

## Export roadmap (decomposition agreed in brainstorming)

1. **Runtime + player** — this spec.
2. **Packager + Linux export** — scan what a game uses, pre-strip scripts to
   plain JS (no 8.9 MB TypeScript compiler shipped), generate the project's
   compiled-script registry and build the player for it, output a folder /
   `.tar.gz`, then an AppImage.
3. **Editor "Export Game…" dialog.**
4. **Windows** — cross-compile from Linux with a fetched toolchain (as
   `gs.py` fetches premake); port the little POSIX code the runtime holds,
   and split the editor-only transpiler/syntax-check out of `ScriptEngine`.
5. **Web** — Emscripten + WebGL2; the asset seam redirects to a virtual FS.
6. **Android** — NDK, buildable entirely on Linux; assets from the APK via
   the seam.
7. **iOS** — exported as an Xcode project to finish on a Mac; building or
   signing for iOS needs macOS and cannot happen on this machine.

macOS desktop is out (Apple SDK, deprecated OpenGL).

## Decisions

1. **A separate player executable** (`GSPlayer`), not the editor in a no-UI
   mode: the player must carry only what a shipped game needs, because every
   later port builds on it.
2. **A shared, header-only `Runtime/` folder** on the include path of both
   `TestEnv` and `GSPlayer`, matching `TestEnv`'s header-only style. Not a
   library: nothing in it needs separate compilation, and both executables
   already link QuickJS and GS.
3. **One renderer and one session for both.** The editor stops owning the
   scene-rendering and Play code and *uses* the runtime's, so an editor Play
   and a player run are the same code — which is what lets them be compared
   byte for byte.
4. **An asset seam now, even with one platform.** `GS::Assets::Resolve(path)`
   is where the runtime turns a stored path into a file to open. Web and
   Android redirect it later instead of every caller being found again.
   Resolution: absolute paths and `primitive:` keys pass through; otherwise
   `<project root>/<path>` if that exists, else `<path>` unchanged (relative to
   the working directory). The fallback is required, not a convenience:
   existing projects store working-directory-relative paths
   (BreakoutRecreation's scripts are `assets/demos/...`).
5. **Editor overlays are hidden during Play.** Selection box, camera rays,
   light gizmos and the transform gizmo drew over the game while playing;
   Play now shows what a player sees (the Unity "Game view" convention). A
   visible behaviour change in the editor, and what makes editor Play and the
   player comparable.
6. **Deferred, deliberately:** splitting `ScriptEngine` (piece 4 — its run and
   codegen halves share one JS context, and only a port that can't compile the
   g++-shelling tooling needs the split); generating a per-project
   compiled-script registry (piece 2 — it belongs to "build the player for
   this project"). In piece 1 the player includes the same
   `CompiledScriptRegistry.h` the editor does.

## Design

### `Runtime/`

| File | From | Holds |
| --- | --- | --- |
| `ScriptEngine.h` | `TestEnv/src`, moved whole | unchanged |
| `CompiledScript.h`, `CompiledScriptRegistry.h` | `TestEnv/src`, moved | unchanged (registry path to Breakout's `.cpp` adjusted) |
| `MaterialLibrary.h` | `TestEnv/src`, moved | file loads go through `GS::Assets::Resolve` |
| `SceneRenderer.h` | extracted from `EditorSceneView` | lit shader (albedo/normal/roughness), per-mesh material binding, scene lights, ambient, clear colour, and the active-`CameraComponent` camera choice |
| `GameSession.h` | extracted from `PlayMode` | physics world + bodies, the `GS::ScriptPhysics` backend, prepared interpreted and compiled scripts, the tick; operates on a `GS::Scene&` it is given |
| `ProjectManifest.h` | extracted from `EditorProject` | reading a `.gsproj`: name, scenes, which is active |

`PlayMode` becomes the editor's wrapper: snapshot the scene, run a
`GameSession` over `g_EditorScene`, restore on Stop. `EditorSceneView` keeps
its framebuffer, picking, gizmos and blit, and calls `SceneRenderer` for the
meshes and the Play camera.

### `GS` additions

- `GS::Assets::SetRoot(path)` / `Resolve(path)` (`GS/src/GS/Assets.h`), used
  by `MeshCache` for file paths, `ScriptEngine` for script and module files,
  `MaterialLibrary` for `.gsmat`. Root empty (the editor) = today's behaviour
  exactly.
- `Window::SetTitle(const std::string&)`.

### `GSPlayer`

New premake project (`Player/src/PlayerApp.cpp`), linking GS + ImGui (the
engine's layer stack expects it) + QuickJS; `$ORIGIN` rpath; assets copied
beside the binary like `TestEnv`. Usage:

```
GSPlayer <project folder> [engine flags: --lockstep --capture ... ]
```

It reads the folder's `.gsproj`, titles the window with the project name,
sets the asset root to the folder, loads the active scene, and runs a
`GameSession` from the first fixed step: render through `SceneRenderer` into
a framebuffer the window's size, blit, repeat. No ImGui windows. A missing or
unreadable project prints why and exits non-zero.

### Editor `--start-play`

Starts Play right after the editor loads its scene, before the first fixed
step — the flag that makes Play captures possible. (`--play` is taken: it
replays an input recording.)

## Verification

Baselines taken before any change:

- Captures (`--hide-ui --lockstep --capture-step`) of an editor scene with
  meshes and a light, of Cube3D, and of a scene with a linked material: all
  byte-identical after every task.
- BreakoutRecreation in Play for 300 fixed steps, transforms dumped:
  byte-identical after the `GameSession` extraction.

New:

- `GS::Assets::Resolve`: absolute, `primitive:`, root hit, root miss falling
  back, empty root — each against the expected path.
- **The headline check:** BreakoutRecreation captured at the same simulation
  step from `TestEnv --scene … --start-play` and from `GSPlayer` — the ball has
  moved, and the two PNGs are byte-identical.
- A physics project run in the player (a box falling onto a floor):
  transforms match the same project in editor Play.
- `GSPlayer` on a missing folder exits non-zero with a message.
- All three configs build.

## Out of scope

Packaging, export UI, other platforms (pieces 2–7); audio in the player
beyond what the engine already initialises; settings such as resolution or
fullscreen in the manifest (piece 2 or 3, when there is a dialog to set them).
