# Cross-file script imports — design

Lets an entity script `import` named functions/consts from another `.gs`/`.ts`
file. Builds on the entity-scripting system (`ScriptEngine.h`, `PlayMode.h`);
doesn't touch the not-yet-built TS→C++ transpiler, but records data that
transpiler will need later (see "Dependency manifest").

## Goal

```ts
// mathutils.gs
export function clamp(x: number, lo: number, hi: number) {
	return Math.max(lo, Math.min(hi, x));
}

// paddle.gs
import { clamp } from "./mathutils";

function OnUpdate(dt: number) {
	let [x, y, z] = entity.getPosition();
	x = clamp(x + 1, -1.6, 1.6);
	entity.setPosition(x, y, z);
}
```

## Design

### Why not real ES modules

QuickJS supports them natively, but a real module's top-level code only sees
`globalThis` — it can't take function parameters. The entity-scripting design
deliberately passes `entity`/`input`/`scene` as parameters to a fresh
per-entity closure specifically so two entities running the same script file
never share state (Task 2 of the entity-scripting plan). Making an entity's
own script a real ES module would mean putting `entity`/`input`/`scene` back
on `globalThis`, reopening that exact bug. Giving each entity its own
synthetic native module to import them through would fix that, but needs
QuickJS module-loader C API (`JS_SetModuleLoaderFunc`, `JS_NewCModule`) this
codebase has never used. Not worth it for what this actually needs.

### Approach: transpile-time inlining

An entity's own script stays exactly what it is today — wrapped fresh per
entity, `entity`/`input`/`scene` as parameters. `import`/`export` are handled
entirely in `PrepareEntityScript`, before the existing `ts.transpileModule`
call:

1. Parse the script's source with `ts.createSourceFile` (not the transpiled
   output — type-only imports need to be visible before stripping) and
   collect its top-level `ImportDeclaration` nodes: specifier + imported
   names, with `as` aliases.
2. For each specifier, resolve it relative to the *importing file's own
   directory* (`./`, `../` only — no bare specifiers, no node_modules, no
   project-root-relative paths). Try the specifier literally first, then
   with `.gs` and `.ts` appended if it has no extension.
3. Read, parse, and recursively resolve that module's own imports the same
   way (one module can import another). A module in progress of being
   resolved that gets asked for again is a cycle — fail with a clear error
   naming the cycle, don't loop.
4. Strip `export` off the module's exported functions/consts, transpile the
   result with the existing `ts.transpileModule` call, and cache the JS body
   plus its export names by resolved absolute path — for this Play session
   only (see "Cache lifetime").
5. Splice each imported module in as its own IIFE, destructured into local
   `const`s matching the import (and any alias):
   ```js
   const { clamp } = (function() { <mathutils.gs's transpiled body>
     return { clamp }; })();
   ```
   Each module gets its own IIFE scope, so two modules exporting the same
   name never collide, and a module's own internal (non-exported) helpers
   never leak into the entity script.
6. Concatenate the spliced modules ahead of the entity's own code (with its
   `import` lines removed) and continue exactly as today: wrap in
   `(function(entity, input, scene) {...})`, eval, extract `OnStart`/
   `OnUpdate`.

**Only named exports** — `export function`/`export const` at a module's top
level. No default exports, no `export * from`, no re-exports. Keeps export
resolution to "walk top-level statements, collect the ones marked `export`,"
no separate re-export graph to chase.

### Cache lifetime: one Play session, not the engine's lifetime

`PlayMode::s_ScriptEngine` is one long-lived instance reused across every
Play/Stop cycle for the life of the app. A module cache tied to that
instance's lifetime would mean editing a module file and pressing Play again
still runs the *old* content forever, silently. The cache is cleared at the
start of every `PlayMode::Play()` instead — same freshness guarantee the
entity's own script already has today (nothing about scripts survives a
Stop; every Play() re-reads and re-parses from disk). Five entities
importing the same module within one session still only cause one read +
parse of it (the cache holds each module's own analyzed, import-stripped
TS text) -- the final type-stripping transpile still runs once per entity
over its assembled text (its own code plus whichever modules it pulled in),
the same single `ts.transpileModule` call `PrepareEntityScript` already
made before this feature existed, just over more text. Not caching that
last step too was a deliberate simplification: it would mean transpiling
each module in isolation and splicing pre-transpiled JS instead of TS,
for a savings too small to matter at the size these scripts actually are.

This does **not** cover hot-reloading a module while Play is still running
(same already-accepted gap as an entity's own script — see the
entity-scripting spec's deferred list). Stop, edit, Play again to see a
module change.

### Because entities get their own inlined copy, module state isn't shared

A real ES module is a singleton — every importer sees the same instance,
including any top-level mutable state. Because this design splices a fresh
copy of a module's transpiled body into *each entity's own closure*, a
`let count = 0` at a module's top level would **not** be shared across
entities that import it — every entity effectively gets a private instance.
This is a real, deliberate deviation from how imports behave in real
JS/TS, worth surfacing because it will surprise anyone who knows ES modules:
this is not that. Modules should be written as stateless helpers (pure
functions, constants) — which is what the actual use case (shared movement/
collision math) needs anyway.

### Dependency manifest

Recorded as a byproduct of step 2 above, purely for a future consumer:
**nothing reads this file yet.** The not-yet-built TS→C++ transpiler will
need to know, per script, which module files it was built from — a module
edit after a script has been compiled-and-hand-optimized needs to trigger
the same overwrite-conflict handling a direct script edit would, and
possibly fan out to several affected scripts sharing one module. That
design belongs to the transpiler spec, not this one, but the transpiler
can't be designed against data that doesn't exist — so this feature writes
it now.

Format, written fresh (fully overwritten, not incrementally updated) to
`<project folder>/.gs-dependencies.txt` at the end of every `PlayMode::Play()`
call that resolved at least one import:

```
gs-script-deps 1
script assets/demos/BreakoutRecreation/paddle.ts
  import assets/demos/BreakoutRecreation/mathutils.gs
script assets/demos/BreakoutRecreation/ball.ts
  import assets/demos/BreakoutRecreation/mathutils.gs
```

One `script` line per entity script that has at least one resolved import,
followed by one indented `import` line per module it (transitively)
depends on. A script with no imports gets no entry at all — an empty
manifest for a project using no modules yet.

## Explicitly deferred

Real ES-module semantics (shared singleton state), default exports,
re-exports, bare/absolute-path specifiers, circular imports, hot-reloading a
module while Play is active, and any consumer of the dependency manifest
(that's the transpiler's job, later). The text editor's "Run" button
(`ScriptEngine::RunScript`) also doesn't gain import support -- it runs
whatever's in the buffer with no associated file path, and relative import
resolution needs a real path to resolve against. Only `ScriptComponent`
scripts (which always have a `ScriptPath`) can import.

## Testing

A temporary self-test: two entities importing the same module (confirms one
module is resolved once per session and each entity's calls to it behave
independently — no shared state leaking between them); a module importing
another module (confirms recursive resolution); two modules exporting the
same name, imported with different aliases into one script (confirms no
collision); a missing import path and a deliberately circular import (both
must fail with a clear, named error, not a crash or a silent wrong result);
re-Play after editing a module file on disk (confirms the cache doesn't
survive Stop, matching the entity-script-edit precedent); and the
dependency-manifest file's content after a Play() with imports present.
