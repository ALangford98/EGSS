# Scripting runtime — brief spec

Follows the embedded text editor. Kept short per the owner's request.

## Goal

Run a TypeScript file from the editor and see its output. v1 is the bare
pipeline end to end: edit -> transpile -> execute -> see output. No
entity/scene bindings yet -- that's its own later module once there's a
real API surface to design carefully, not something to bolt on here.

**Confirmed via a throwaway spike before committing to this design**:
TypeScript's own npm-distributed compiler bundle (`typescript.js`, the
real, unmodified `lib/typescript.js` from the `typescript` npm package)
loads and runs correctly inside QuickJS with zero Node polyfills needed.
Measured: 0.27s to load the 8.9MB bundle once, ~5ms per
`ts.transpileModule()` call on a small file. This settles the
architecture -- no second toolchain (no Node, no Go/esbuild binary), just
JS doing double duty as both the runtime and (via TypeScript's own
compiler, running inside it) the type-stripper.

**Explicitly deferred**: entity/component script attachment ("MonoBehaviour"
-style OnStart/OnUpdate hooks), any engine API surface exposed to scripts
(no Transform/Scene bindings in v1 -- console.log is the only I/O), module
imports between multiple script files, a script picker/library beyond "the
file currently open in the text editor," debugging/breakpoints.

## Design

- **`GS/vendor/quickjs`** (new git submodule) -- `quickjs-ng/quickjs`, the
  actively maintained fork (the original bellard/quickjs is less so).
  MIT licensed.
- **`GS/vendor/quickjs_premake5.lua`** (new) -- same convention as
  `libvterm_premake5.lua`: compiles the core sources (`quickjs.c`,
  `libregexp.c`, `libunicode.c`, `dtoa.c`) into a static lib. Deliberately
  **excludes** `quickjs-libc.c` (the CLI's OS/file/exec/signal bindings) --
  a sandboxed script host has no business giving scripts direct
  file/process access by default, and skipping it avoids that file's
  Linux-signal-handling code entirely.
- **`TestEnv/assets/typescript.js`** (new, vendored as a checked-in data
  asset, not a buildable submodule) -- the real `lib/typescript.js` from
  the `typescript` npm package, unmodified. A comment at the top of
  `ScriptEngine.h` records the exact npm version this came from and the
  one-line command to refresh it (`npm pack typescript@X.Y.Z`, extract
  `package/lib/typescript.js`). This is a deliberate exception to "vendor
  everything as buildable-from-source" -- the TypeScript compiler's own
  build toolchain is exactly the Node-based heaviness this whole approach
  exists to avoid; checking in the already-built bundle is the same
  reasoning that already justifies vendoring `stb_image.h` as a single
  file rather than its build process.
- **`TestEnv/src/ScriptEngine.h`** (new) -- owns one `JSRuntime`/`JSContext`
  for the app's lifetime. Lazily creates both and loads+evaluates
  `typescript.js` on the *first* `RunScript()` call, not at startup (most
  sessions won't touch scripting at all; paying 0.27s only when actually
  used). `RunScript(const std::string& tsSource)` returns the captured
  console output and any error (transpile diagnostic or a runtime
  exception's message + stack), as plain strings. A native `console.log`
  binding appends to an internal string buffer rather than printing to
  stdout. One global JS scope persists across `RunScript()` calls within
  a session -- a script's top-level `let`/`function` declarations remain
  visible on the next run, REPL-like. This is a known, simple design
  choice, not an oversight; per-run isolation (a fresh context each time)
  is easy to add later if it turns out to matter.
- **`TestEnv/src/TextEditorPanel.h`** (modified) -- a "Run" button added to
  the existing file bar, calling `ScriptEngine::RunScript()` on the
  buffer's current content and routing the result to `EditorShell.h`'s
  existing "Build Output" panel (currently a stub -- exactly the reserved
  slot this output was always meant to land in, per the menu-bar plan's
  own layout design).
- **`TestEnv/src/EditorShell.h`** (modified) -- "Build Output" stub
  replaced with a plain scrolling text panel showing `ScriptEngine`'s
  captured output/error from the last run (owns the `ScriptEngine`
  instance itself, same ownership pattern as `m_Terminal`/`m_TextEditor`).

## Testing

`ScriptEngine::RunScript()` is pure logic wrapped around QuickJS/TS state
-- no ImGui dependency -- so it gets a temporary self-test the same way
`TextBuffer` did: run a small known TS snippet (typed function, an
interface, a template literal), assert the captured output matches by
hand-computed expectation, assert a deliberately-broken script (a runtime
`throw`) produces a captured error rather than crashing the process.
