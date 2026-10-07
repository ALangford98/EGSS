# Game Runtime and Player Implementation Plan

> Executed natively, committed on `main`.

**Spec:** `docs/superpowers/specs/2026-10-07-game-runtime-player-design.md`

## Global Constraints

- Editor behaviour byte-identical through every extraction (captures and the
  Breakout Play dump), except overlays hidden during Play (spec decision 5).
- `GS::Assets` with an empty root resolves every path to itself.
- `--play` is the engine's replay flag; the editor's is `--start-play`.
- Temporary checks in a `TEMPORARY` header, deleted at the end.

## Review Focus

1. A project path given with or without a trailing slash, or as `.` — the
   player finds the `.gsproj` either way.
2. A project whose `.gsproj` names no scene, or a scene file that fails to
   load — message and non-zero exit, not a blank window.
3. Editor Play/Stop/Play after the extraction — the second session starts
   clean (bodies, scripts, warned-once set).
4. Window resized in the player — framebuffer and camera aspect follow.

### Task 0: Baselines
- [ ] Captures: editor scene with meshes + light; Cube3D; linked material.
- [ ] Breakout 300-step Play dump.

### Task 1: `GS::Assets` and `Window::SetTitle`
- [ ] Checks for every `Resolve` rule; implement; wire into `MeshCache`.
- [ ] Captures identical; commit.

### Task 2: Move to `Runtime/`
- [ ] Move `ScriptEngine.h`, `CompiledScript*.h`, `MaterialLibrary.h`;
      include path in both projects; `Resolve` in script/material loads.
- [ ] Build, captures + dump identical; commit.

### Task 3: `SceneRenderer`
- [ ] Extract shader, mesh/material binding, lights, Play camera.
- [ ] `EditorSceneView` uses it; captures identical; commit.

### Task 4: `GameSession` and `ProjectManifest`
- [ ] Extract from `PlayMode`/`EditorProject`; `PlayMode` wraps it.
- [ ] Breakout dump identical; Play/Stop/Play clean; commit.

### Task 5: `--start-play`, overlays hidden in Play
- [ ] Flag; overlay guard; capture Breakout in editor Play; commit.

### Task 6: `GSPlayer`
- [ ] Premake project, `PlayerApp.cpp`, title, asset root, render loop.
- [ ] Player vs editor `--start-play` capture byte-identical; physics
      project transforms match; missing folder exits non-zero; three
      configs; commit.

### Task 7: Finish
- [ ] Delete temporary checks; changelog, STATE, roadmap; commit.
