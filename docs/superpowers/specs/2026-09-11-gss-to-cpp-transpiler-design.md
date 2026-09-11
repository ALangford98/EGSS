# GSS-to-C++ transpiler — design

TypeScript (`.gss`, "GameStart Script") as a macro layer over hand-written
C++: press Ctrl+S on a `.gss` file, see the equivalent C++ generated and
compiler-checked live, then optionally hand-optimize the generated file,
which becomes that entity's real source from that point on. Builds on
entity-scripting (`ScriptEngine.h`) and cross-file imports (see
`2026-09-11-script-imports-design.md`, whose dependency manifest this is
the first real consumer of).

## Goal, and what "done" means for v1

**This version does not wire compiled code into Play at all.** Today, every
scripted entity runs through one interpreter (QuickJS) uniformly; once a
compiled entity can exist, that stops being true, and *how* it runs is a
real fork:

- **Static linking** — a generated `.cpp` joins `TestEnv/src/` for real,
  built into the executable like anything else here. No new
  infrastructure, but testing a compiled entity means rebuilding **and
  restarting** the editor, not clicking Play.
- **Dynamic loading** — compile to a `.so`, `dlopen()` it at Play time.
  Fast iteration, matching what interpreted scripts already have, but
  needs real plugin infrastructure (symbol conventions, ABI stability
  against this engine's own headers, safe unloading) this codebase has
  never built.

Deciding between them is deferred. **v1 produces real, compiler-verified
C++ and the live diff panel; it does not run the result.** That's a
complete, useful thing on its own — a reviewable, correct starting point
for hand-optimization — without first solving an execution model that
half of this spec's own design (the "optimize-lock", the dependency
fan-out) doesn't actually depend on.

## The language subset

Scoped against what `.gss` scripts actually use today (paddle/ball
recreation, `mathutils`-style modules), not guessed. The transpiler is
**mechanical, not semantic** — it is not a type-inferring compiler.
Wherever it needs a C++ type, the script must have said so explicitly; if
it hasn't, that construct is unsupported for compilation (the script still
runs fine interpreted — this only affects the Ctrl+S compile path).

**Supported:**
- `let`/`const` with an explicit type annotation (`: number`, `: boolean`,
  `: string`) → `double`/`bool`/`std::string`.
- Arithmetic, `===`/`!==` (not bare `==`/`!=` — JS loose equality has no
  honest C++ translation, so it's rejected rather than silently
  mistranslated), comparison, logical operators, the ternary operator.
- `if`/`else if`/`else`, C-style `for`, `while`.
- Calls to the native API (`entity.get/setPosition/Rotation/Scale`,
  `input.isKeyDown`, `scene.findByTag/spawn/destroy`), to user-defined
  top-level functions and imported module functions (same explicit-typing
  requirement on their parameters/return type), and `Math.*` mapped to a
  fixed `std::*` table (`Math.abs`→`std::abs`, `Math.min/max`→
  `std::min/max`, `Math.sqrt`→`std::sqrt`, ...).
- Arrow functions — purely syntactic; map to a C++ lambda, or a plain
  function when assigned to a top-level `const`.
- Template literals — `` `x=${x}` `` becomes string concatenation,
  stringifying each `${...}` per its operand's declared type
  (`std::to_string` for numbers, as-is for strings).
- Simple classes: typed fields, methods, constructors, single
  inheritance (`extends`) → C++ class, members, methods, public
  inheritance. Not interfaces-as-mixins, abstract classes, decorators, or
  static blocks.
- Simple generics: unconstrained or simply-constrained
  (`<T extends number>`) generic functions → C++ function templates. Not
  conditional types, mapped types, or `infer` — none of those have a
  mechanical C++ equivalent.

**Explicitly not supported, and why (not just "too hard"):**
- **Destructuring, at all** (array or object) — dropped by request rather
  than partially supported. This means every script written so far (they
  all use `let [x, y, z] = entity.getPosition()`) is **not**, as written,
  compilable — it would need rewriting to index access instead
  (`const pos = entity.getPosition(); pos[0] += 1; ...`), which itself
  *is* supported (plain array indexing isn't destructuring). Worth
  knowing before anyone reaches for Ctrl+S expecting today's scripts to
  just compile.
- **`try`/`catch`, `async`/`await`** — not a difficulty call. This engine
  builds with exceptions enabled, but has exactly zero real `throw`/
  `catch` anywhere in `GS/` or `TestEnv/` — every fallible operation
  returns `bool` + an error string, and the native script API never
  throws, so there'd be nothing for a generated `catch` to catch.
  `async`/`await` has no runtime to target at all: no event loop, no
  Promise equivalent, no coroutine system — this is a synchronous,
  fixed-step engine. A "wait N seconds, then do X" scripting need is a
  real, separate feature (a coroutine/timer system for entity scripts),
  not something this transpiler produces as a byproduct.
- **`globalThis`-based cross-entity state** (the trick `ball.gss` uses
  today to read `paddle.gss`'s position) — no shared-QuickJS-context
  equivalent exists in generated C++, and it doesn't need one:
  `scene.findByTag(...)` is the real, already-built replacement. A script
  wanting to compile should use that instead.
- Untyped declarations (per the mechanical-not-semantic rule above).

## Generated shape

One C++ class per script file, name from the file's stem in PascalCase
(`paddle.gss` → `Paddle`), in a `GeneratedScripts` namespace to keep clear
of hand-written code:

```cpp
// GS-GENERATED: <hash of everything below this line>
// Generated from paddle.gss -- see docs/superpowers/specs/
// 2026-09-11-gss-to-cpp-transpiler-design.md. Hand-editing this file is
// fine; doing so is what "optimizing" it means. A future rebuild from
// paddle.gss will ask before overwriting a file whose content no longer
// matches this hash.
#include <GS.h>

namespace GeneratedScripts {

class Paddle
{
public:
	void OnStart(GS::Entity entity, GS::Scene& scene) {}

	void OnUpdate(GS::Entity entity, GS::Scene& scene, double dt)
	{
		auto* transform = entity.Get<GS::TransformComponent>();
		// ...
	}
};

}
```

`input` is not a parameter here (unlike the interpreted API) --
`GS::Input::IsKeyPressed` is already a static call; the *interpreted*
path only threads `input` through as a parameter to keep two entities'
QuickJS closures from colliding, a problem that doesn't exist in C++.
`scene` stays a parameter because `GS::Entity` has no accessor back to its
owning `GS::Scene*` (private, unexposed) -- adding one is a plausible
small engine change but out of scope here.

## Pipeline

1. **Parse**, using the same TS-compiler AST approach `AnalyzeModule`
   (imports design) already established -- walk top-level statements,
   this time emitting C++ text per statement/expression instead of
   splicing JS.
2. **Reject up front**, with a specific, named error (which construct,
   where), anything outside the supported subset above -- never silently
   approximate.
3. **Emit** the `GeneratedScripts::<Name>` class shown above into
   `<same directory>/<name>.cpp`, stamped with the `GS-GENERATED` hash
   line.
4. **Syntax-check**, not link: shell out to `g++ -std=c++17 -fsyntax-only`
   against the real engine include paths. This is what actually proves
   the generated code means something -- a transpiler that never runs a
   real compiler over its own output is just guessing that it's valid
   C++.

Step 4 only runs on Ctrl+S (a real compiler process per keystroke would be
slow and constant); steps 1-3 (transpile only, no compiler invocation) run
on a short idle debounce while typing, driving the live preview pane.

## The live panel

A `.gss` file open in the "Editor" tab gets a second, read-only pane
beside it (split within the same tab, not a separate one) showing the
current transpile output, refreshed ~500ms after typing stops. Ctrl+S
saves the `.gss` file, then runs the full pipeline including the
syntax-check compile; its diagnostics surface in the existing "Build
Output" panel (`TextEditorPanel::LastRunError`/`LastRunOutput`, already
wired to that panel for the "Run" button -- the compile path reuses the
same display, doesn't invent a second one).

## The optimize-lock

The `GS-GENERATED: <hash>` comment is a hash of the generated file's
content *below that line*. Before overwriting a `.cpp` on a Ctrl+S
rebuild: recompute that hash against the file's current content. Equal →
never hand-edited, overwrite freely. Different → hand-edited (has
"graduated"), and this is where the three-way prompt (Cancel / Overwrite /
Save a copy) fires -- "Save a copy" writes the freshly generated content
to `<name>.generated.cpp` instead, leaving the hand-optimized `<name>.cpp`
untouched.

## Dependency-manifest fan-out

Editing a module (not the script directly) needs the same protection: if
`mathutils.gss` changes, every script that imports it (transitively) is
now potentially stale. Saving a module:

1. Reads `.gs-dependencies.txt` (imports design) to find every script
   depending on it.
2. For each, applies the same hash check above.
3. If **any** are hand-edited, show **one consolidated dialog** listing
   all of them (not one popup per file) -- pick per-script: skip,
   overwrite, or save a copy.
4. Scripts that were never hand-edited just get silently rebuilt --
   nothing to lose there.

## Explicitly deferred

Execution wiring (static or dynamic -- see "Goal" above), destructuring,
`try`/`catch`, `async`/`await`, a coroutine/timer system for scripts,
advanced generics/classes, adding a `GS::Entity::GetScene()` accessor.

## Testing

A temporary self-test per pipeline stage rather than one large one: the
supported-subset constructs each produce the expected C++ text (arrow
function, template literal, a simple class, a simple generic, the native
API calls); each unsupported construct (bare `==`, destructuring,
`try`/`catch`, `async`, untyped declaration) is rejected with a specific
error naming the construct, not a generic failure; the syntax-check step
actually invokes `g++ -fsyntax-only` and correctly distinguishes a clean
generated file from one with a deliberately broken include; the hash
marker round-trips (generate, don't touch, rebuild → no prompt; generate,
hand-edit, rebuild → prompt fires); and the dependency-manifest fan-out
correctly finds multiple dependents of one module and reports which are
hand-edited versus safe to silently rebuild.
