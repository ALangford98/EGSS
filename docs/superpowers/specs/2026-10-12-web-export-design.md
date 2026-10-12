# Web export (export piece 5)

**Status:** built 2026-10-12, under the owner's "keep going with everything"
-- decisions made during the work, recorded here for review.

## Decisions

1. **Emscripten + WebGL2** (GLES 3.0), emsdk `6.0.11` cloned at its pinned
   commit by `gs.py fetch emsdk`, which then installs and activates that
   release (emsdk checks its own downloads against its manifest).
2. **A `--web` premake option, not a new OS.** Generated on the Linux host;
   `GS_PLATFORM_WEB` replaces `GS_PLATFORM_LINUX` (each first-party
   `system:linux` block stands aside under `--web`); outputs get a `-web`
   suffix; the engine links statically (one wasm module); Emscripten's own
   GLFW (`-sUSE_GLFW=3`) replaces the vendored one, and a glad shim maps onto
   its GLES3 headers.
3. **The main loop runs per frame**: `Application::Run` was split into
   `RunFrame`, which `Run` loops on desktop and `emscripten_set_main_loop`
   calls on the web (a browser cannot run a blocking loop).
4. **No threads on the web.** Browsers allow them only on pages served with
   cross-origin-isolation headers, which a game host may not set;
   `ParallelRows` runs single-threaded there (output identical anyway).
5. **WebAssembly exceptions** (`-fwasm-exceptions`): Emscripten's default
   aborts on any throw, and the engine and spdlog throw and catch.
6. **The game is preloaded into the in-memory filesystem at `/game`**, where
   `GS::Assets` and `std::ifstream` work unchanged -- the piece-1 seam paid off.
7. **Package:** `index.html` (titled with the game), `GSPlayer.js/.wasm/.data`
   and a README saying it must be served (browsers refuse `.wasm` from
   `file://`), zipped -- the shape itch.io and similar hosts take.

## What the port took

- Shaders: GLSL 3.30 core rewritten to GLSL ES 3.00 in one place
  (`OpenGLShader`): version line, highp default precisions -- and *nothing
  before the version line*: the engine's raw-string shaders start with a
  newline, which desktop GLSL accepts and ES rejects (the compiler then fell
  back to ES 1.00, where `isampler2D` does not exist).
- Desktop-only GL skipped: debug output, `GL_MULTISAMPLE`, polygon mode,
  `glPointSize`, timer queries; `glDrawBuffer` -> `glDrawBuffers`.
- **`glClear` with an integer colour attachment in the draw buffers is
  `INVALID_OPERATION` in WebGL2** (the editor-style framebuffer's entity-id
  target): the colour is cleared with `glClearBufferfv` on draw buffer 0.
- **WebGL fixes a buffer's target at its first bind.** The index buffer was
  uploaded through `GL_ARRAY_BUFFER` (a desktop idiom to avoid clobbering the
  bound VAO's element binding), so it could never be bound as an element
  buffer: every draw failed and the frame was black. On the web it is bound
  as an element buffer, with VAO 0 bound meanwhile.
- `EntryPoint.h` defined `main` only for Windows and Linux. The first web
  build therefore had no `main` at all; Emscripten linked it without a
  word, and the `.wasm` came out at 172 KB with the engine dead-stripped.
- `ScreenCapture` writes into the in-memory filesystem; a page hook
  (`Module.onGsCapture`) can hand the file out -- the test harness's way in.

## How it was debugged

Each failure above was found by measurement, not guessed: the 172 KB wasm
was the first clue; a page shell that, under `?harness=1`, forwards the
engine log, page errors and status to the test server showed "Running..."
and then silence (no `main`); per-stage `glGetError` checks showed
`INVALID_OPERATION` on the clear, the scene draw and the blit; and wrapping
every WebGL2 method to call `getError` after it (kept as `?glcheck=1`)
named `bindBuffer(ELEMENT_ARRAY_BUFFER)` as the failing call.

## Verification

Headless Firefox (throwaway profile), served by a small Python server that
accepts `POST /capture`: BreakoutRecreation (compiled scripts, built into the
wasm) and the falling-box game (an interpreted script exported as JS),
exported for the web, unzipped fresh, run with `--lockstep --capture-step
150` passed through the page URL: captures **byte-identical** to editor Play
on Linux -- Firefox's WebGL2 runs on the same NVIDIA driver.

The second web export first shipped the *first* game: `gs.py` cleared the
export build folder named after the host system, not the target's
(`-windows-`, `-web`), so the web link and its `.data` were reused. Fixed for
both targets, and checked by exporting in the order that exposed it.
