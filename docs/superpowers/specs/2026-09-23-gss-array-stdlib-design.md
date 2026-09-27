# GSS stdlib: console.log, number[], .length, .map, .filter, .toArray

**Status:** approved, ready for planning
**Scope:** `editor_roadmap.md` wishlist #3 ("GSS stdlib functions"), narrowed during
brainstorming — see "Explicitly out of scope" below for what wishlist #3 named
that this spec does not cover, and why.

## Problem

The GSS-to-C++ transpiler (`TestEnv/src/ScriptEngine.h`'s embedded codegen JS)
only recognizes a fixed set of calls: `entity.*`/`input.*`/`scene.*`/`Math.*`
member calls, and top-level function/local-variable calls. A script that
calls `console.log(...)` — already available in the **interpreted** path,
where `console` is bound on the shared QuickJS context (`ScriptEngine.h:1246-1250`)
— fails at transpile time with "unsupported method call 'log'", because
`emitCall`'s `PropertyAccessExpression` branch never checks for `console`.
This blocks debugging a script once it's graduated to compiled C++, the
same workflow `paddle.gss`/`ball.gss` already went through.

More broadly, no GSS script — interpreted or compiled — has ever had a
dynamic, variable-length array to work with. The only array-shaped value
any script can obtain is the fixed 3-element result of `getPosition()`/
`getRotation()`/`getScale()` (a `glm::vec3`, returned to the interpreted
path as a 3-element JS array via `JS_NewArray` + `JS_SetPropertyUint32`,
and to the compiled path as a raw `glm::vec3`). `.map`/`.filter`/`.find`
have nothing real to operate on without one.

## Decisions made during brainstorming

These were genuine forks, not implementation detail — recorded so a later
reader doesn't have to re-derive them:

1. **No new array-*producing* native call in this pass.** `map`/`filter`
   are scoped to operate on the vec3-derived case (via `.toArray()`), not
   on a new `scene.findAllByTag()`-style collection. Adding a real
   collection-returning native call is a separate, later feature — this
   pass only makes the vec3 case (which already exists) usable as an array.
2. **A real `std::vector<double>` codegen type, not a workaround.** Once
   `.filter()` is in scope, its result can have 0–3 elements — nothing
   fixed-size can hold that. Introducing `std::vector<double>` (GSS
   spelling: `number[]`) as a second codegen array type, alongside the
   existing fixed `glm::vec3`, was chosen over declining `.filter`/`.map`
   or faking variable length inside a 3-slot type.
3. **Uniform dispatch via an explicit `.toArray()` bridge, not dual
   dispatch.** `.length`/`.map`/`.filter` are defined *only* on
   `std::vector<double>`. `glm::vec3` gains exactly one new capability,
   `.toArray()`, plus a compile-time `.length` (see below). This was
   chosen over also letting `.map` run directly on a `vec3` (which would
   need its own branch, its own “returns vec3 vs. returns array”
   distinction, and doubles the number of type-dispatch sites for a
   convenience that only saves typing `.toArray()`).
4. **`.find` is out of scope for this pass.** Its no-match case needs
   either a sentinel value (a silent footgun) or `std::optional<double>`
   (a second new type in the same pass, the same complexity category that
   justified `std::vector<double>` in the first place). Deferred rather
   than resolved with a shortcut.
5. **`number[]` maps to `std::vector<double>`, not `std::vector<float>`.**
   Found during design, not assumed: `typeToCpp` already maps the scalar
   `number` to `double` (`ScriptEngine.h:1516`), so the array form follows
   the same element type for consistency. `.toArray()` narrows each
   `float` vec3 component to `double` on conversion — implicit and
   lossless enough for this engine's existing precision conventions
   (every other GSS numeric value is already `double`).

## Design

### 1. `console.log` / `.error` / `.warn` in compiled scripts

`emitCall`'s `PropertyAccessExpression` branch gets a new
`isNamed(obj, "console")` case, alongside the existing `entity`/`input`/
`scene`/`Math` cases, mapping via a flat table:

```js
var consoleFns = { log: "GS_TRACE", error: "GS_ERROR", warn: "GS_WARN" };
```

— the engine's own logging macros, since a compiled script runs as real
C++ with no QuickJS involved once graduated. Anything else under
`console.*` hits the existing `fail(node, "unsupported console.* call '" +
member + "'")` path.

### 2. `number[]` as a real type

`typeToCpp` gains one literal check before its passthrough:

```js
if (t === "number[]") return "std::vector<double>";
```

This makes `number[]` valid anywhere a type annotation already is: a
class member (`let`/top-level `const`, where an annotation is mandatory,
same as `number`/`string`/`boolean` today) or a local declaration (where
it's optional — a `.toArray()`/`.map()`/`.filter()`-initialized local
falls back to `auto`, same as every other typed local already does).

### 3. `glm::vec3.length` and `.toArray()`

Both are recognized **only** when the transpiler can already prove the
callee expression is a `vec3` — the same mechanical-not-general-inference
limit already documented for template-literal interpolation, not new
inference machinery:

- an identifier whose tracked type (`currentVarTypes`) is `vec3` /
  `glm::vec3`, or
- a direct chain off `entity.getPosition()` / `.getRotation()` /
  `.getScale()`, or the same called on an identifier already known to
  hold a native `Entity` (the existing "any identifier holding a native
  GS::Entity value" mechanism `emitCall` already has for
  `getPosition`/etc., `ScriptEngine.h:1655-1678`).

`.length` is a **bare property access** (`emitExpr`'s
`PropertyAccessExpression` case, not `emitCall`) that emits the literal
`3` — a compile-time fact about a fixed-size type, not a runtime read.
`.toArray()` is a call, handled in `emitCall`, emitting:

```cpp
std::vector<double>{ EXPR.x, EXPR.y, EXPR.z }
```

Any other bare property access, or any other zero-arg call, on a
`vec3`-typed expression still falls through to the existing `fail()`
paths in `emitExpr`/`emitCall` unchanged.

### 4. `number[].map(cb)` / `.filter(cb)`

Recognized when the callee object's tracked type is
`std::vector<double>` — itself the result of `.toArray()`, a declared
`number[]`, or a prior `.map`/`.filter` (chaining falls out of this for
free, since every intermediate result is itself tracked as
`std::vector<double>`). The callback argument is emitted through the
**existing, unmodified** `emitArrowFunction` (already produces a
`[&](params) {...}` reference-capturing lambda — no new lambda-emission
path needed) and spliced into an IIFE, the same expression-position
pattern `scene.findByTag` already uses (`ScriptEngine.h:1637-1640`):

```cpp
// .map
([&]{ std::vector<double> __r; __r.reserve(SRC.size());
      for (auto __e : SRC) __r.push_back((LAMBDA)(__e));
      return __r; }())

// .filter
([&]{ std::vector<double> __r;
      for (auto __e : SRC) if ((LAMBDA)(__e)) __r.push_back(__e);
      return __r; }())
```

`SRC` is the emitted callee expression (evaluated once, bound into the
IIFE — not re-evaluated per element). `LAMBDA` is `emitArrowFunction`'s
unmodified output.

### Error handling

Everything not covered above — `.map`/`.filter`/`.length`/`.toArray()`
on an untracked or non-array/non-vec3 expression, `console.*` with an
unrecognized member, `number[][]` or any other unrecognized type string,
`.find` (explicitly, with a message naming it as not yet implemented
rather than falling into a generic "unsupported method" error) — hits
the existing `fail(node, ...)` path with a specific, named message. This
file's whole codegen discipline is "mechanical and total: a recognized
form emits correct code, everything else is a named error, nothing in
between" — this feature adds no exception to that.

## Explicitly out of scope (named, not silently dropped)

- **`.find`** — see decision 4 above. A natural follow-up once
  `std::optional<double>` (or an equivalent) is independently justified.
- **A new array-*producing* native call** (e.g. `scene.findAllByTag`
  returning a collection of entities) — see decision 1. `map`/`filter`
  only ever start from a `vec3`-via-`.toArray()` source in this pass.
- **Array literals** (`[1, 2, 3]`) — `ArrayLiteralExpression` stays
  explicitly rejected in `emitExpr` (`ScriptEngine.h:1755-1757`) for this
  pass. A `number[]` value can only be *produced* by `.toArray()`,
  `.map()`, or `.filter()` here, never constructed from a literal.
- **`.map`/`.filter` directly on `vec3`** — see decision 3. Always goes
  through `.toArray()` first.
- **Any change to the interpreted path.** `console.log`/`.error`/`.warn`
  already work there; QuickJS arrays are already dynamic, so nothing
  about `.length`/`.map`/`.filter`/`.toArray()` is a gap in interpreted
  scripts — this whole spec is compiled-path-only.

## Testing plan

A temporary self-test (per the self-test pattern in `CLAUDE.md`),
transpiling small GSS snippets through `TranspileToCpp` and round-tripping
each through a real `g++ -fsyntax-only`, covering:

- `console.log`/`.error`/`.warn` inside a compiled `OnUpdate`, syntax-checks
- A `number[]` class member with an explicit annotation, syntax-checks
- `.length` on a `vec3` (`entity.getPosition().length` or an identifier
  chain) emits the literal `3`
- `.length` on a `number[]` compiles and (via a runtime check, not just
  syntax-check) returns the real element count
- `.toArray()` on a tracked `vec3` local produces a correct 3-element
  `std::vector<double>`, checked by value, not just by compiling
- `.map` doubling a `.toArray()`'d position, checked against hand-computed
  expected values
- `.filter` on a `number[]` keeping only positive values, checked by value
- A chained `.toArray().filter(...).map(...)` on one line
- Each documented failure case produces its specific named error, not a
  crash or a generic message: `.map`/`.filter`/`.length`/`.toArray()` on
  an untracked identifier; an unrecognized `console.*` member; `.find`
  called at all

Plus a clean three-config build and a byte-identical Cube3D `--hide-ui`
capture (this feature touches only the transpiler's codegen JS string,
no renderer or demo-layer path).
