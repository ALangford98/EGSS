# GSS-to-C++ Transpiler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Given a `.gss` file, generate real, compiler-verified C++ implementing the same behavior, shown live beside the source, with a hand-edited generated file protected from being silently overwritten.

**Architecture:** A new JS driver (loaded into the existing `ScriptEngine`'s QuickJS context, reusing the already-loaded TypeScript compiler) walks a script's AST and emits C++ text directly, statement by statement — mechanical translation, not semantic inference. C++ produced this way is then handed to a real `g++ -fsyntax-only` invocation to prove it's valid. A hash stamped in each generated file's first line is what "has this been hand-optimized" checks against.

**Tech Stack:** Existing `ScriptEngine`/QuickJS/bundled TypeScript compiler; `g++` invoked as a subprocess for syntax-checking; ImGui for the live panel.

**Spec:** `docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md`

## Global Constraints

- Mechanical translation only — no type inference. Anything needing a C++ type that wasn't explicitly annotated in the `.gss` source is rejected with a specific, named error, not approximated.
- `===`/`!==` only; bare `==`/`!=` is rejected, not translated.
- No execution wiring in this plan — the generated `.cpp` is never compiled into a running behavior, only syntax-checked. See spec's "Goal" section.
- Destructuring is unsupported entirely (per owner decision) — including the `let [x,y,z] = entity.getPosition()` pattern every existing `.gss` script uses. Plain array indexing (`const pos = entity.getPosition(); pos[0]`) is supported and is the documented workaround.
- `try`/`catch`, `async`/`await`, `globalThis` are unsupported — see spec for why each is a real mismatch, not a difficulty call.
- One class per script file in a `GeneratedScripts` namespace; `OnStart`/`OnUpdate` take `(GS::Entity entity, GS::Scene& scene[, double dt])` — no `input` parameter (`GS::Input::IsKeyPressed` is already static).

---

### Task 1: Core statement/expression codegen + native API mapping

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h` (add the codegen driver and its C++-side glue, alongside the existing `s_ModuleAnalysisSrc`/`AnalyzeModule` pattern)
- Test: temporary, per this repo's established self-test convention

**Interfaces:**
- Produces: `bool ScriptEngine::TranspileToCpp(const std::string& source, const std::string& className, std::string& error, std::string& outCpp)` — `source` is one script's raw, **unresolved** text (as actually built: imports are *not* yet spliced in first — calling an imported function fails the same "not a recognized top-level function" way any other unrecognized call would; wiring `AnalyzeModule`/`ResolveModule` in first is a clearly-scoped follow-up, not done in this task); `className` is the PascalCase name for the generated class; on success `outCpp` holds the complete generated file text *including* the `GS-GENERATED: <hash>` header line (hash computed over everything after that line); on failure `error` names the specific unsupported construct and, where possible, the line it's on.

This task covers exactly the subset named in the spec's "Supported" list **except** arrow functions, template literals, classes, and generics (Tasks 2-4): typed `let`/`const`, arithmetic/`===`/`!==`/comparison/logical/ternary, `if`/`for`/`while`, calls to `entity`/`input`/`scene` native API and to user/imported functions, `Math.*` → `std::*`, plain array indexing on `entity.get*()` results.

- [x] **Step 1: Write the driver as a JS string constant**, `s_CodegenSrc`, analogous to `s_ModuleAnalysisSrc` — a recursive-descent emitter walking `ts.createSourceFile`'s AST, producing C++ text for each supported node kind and returning `{ ok: false, error: "...", construct: "..." }` the moment it meets anything outside the supported list (never partially emit past an unsupported construct).

- [x] **Step 2: Write `ScriptEngine::TranspileToCpp`** following the exact `JS_Eval`/`JS_Call`/`GetStringProperty` pattern `AnalyzeModule` already uses, computing the `GS-GENERATED` hash (reuse whatever hashing this codebase already has access to, or a simple FNV-1a if not — this doesn't need cryptographic strength, only to detect "did this file change"). Used the codebase's own existing FNV-1a convention (`Terrain.h::Checksum`, `OpenWorld.h`).

- [x] **Step 3: Temporary self-test** covering: a script using every construct in this task's subset produces the expected C++ text; a bare `==` is rejected naming that construct; an untyped `let x = 5` is rejected naming the missing annotation; a call to an unresolvable function is rejected; the `GS-GENERATED` hash is present and stable (transpiling the same source twice produces an identical hash). **Design correction found during this step**: requiring an explicit annotation on *every* declaration turned out to be unworkable — `entity.getPosition()` has no `.d.ts`, so there's no correct annotation to write for `const pos = entity.getPosition()` at all. Fixed by falling back to C++'s own `auto` for an initialized local with no annotation (still zero inference on the transpiler's own part — `auto` is deduced by the compiler at the syntax-check step); an annotation is still required only when there's neither a type nor an initializer. Class member variables (top-level `let`/`const`) still always require an explicit annotation — C++ doesn't allow `auto` on a non-static data member at all, a language rule, not a design choice. Extended the self-test with this real g++ invocation, ahead of Task 5's own syntax-check integration: `g++ -std=c++17 -fsyntax-only` (with TestEnv's actual Linux/Debug defines) against the generated output confirmed it's genuinely valid C++, not just plausible-looking text -- caught one real bug this way (a missing deadline on the driver's `JS_Call`, which hung the process outright on the first run; fixed to match every other JS execution point's `m_Deadline` convention).

- [x] **Step 4: Run the self-test, confirm it passes, delete it. `git grep TEMPORARY` clean.** 15/15, clean three-config build.

### Task 2: Arrow functions and template literals

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h` (`s_CodegenSrc`)

**Interfaces:**
- Consumes: `TranspileToCpp` from Task 1 (extends its supported-construct handling; signature unchanged).

- [x] **Step 1: Arrow functions** — a `const f = (params): ReturnType => body` (block or expression body) at top level becomes a named C++ method (entity/scene prepended, exactly like a `function` declaration); a local `const f = (...) => ...` becomes a genuine `[&]` C++ lambda, captured by reference so it can reach entity/scene/locals from its enclosing method without anything threaded through explicitly. **Bug found by the self-test, not anticipated in the design**: calling a local lambda variable isn't calling a "top-level function," so the existing entity/scene-prepending rule rejected it outright ("not a recognized top-level function"). Fixed: a call to an identifier now checks the top-level-function set first (prepend), then a lightweight local-variable-type table (call directly, no prepending — a captured lambda needs nothing threaded through), and only fails if neither recognizes the name.
- [x] **Step 2: Template literals** — `` `text${expr}more` `` becomes `std::string` concatenation, stringifying each `${expr}` per its operand's type (`std::to_string` for a `number`-typed identifier or numeric literal, as-is for `string`). Scoped narrower than "any expression": interpolation is limited to a plain identifier (looked up in the same lightweight, never-reset, non-lexically-scoped type table Task 2 introduced) or a literal — a general expression inside `${...}` is rejected by name, since deciding how to stringify an arbitrary expression would mean tracking types through binary/call/property-access forms too, which is real inference, not the mechanical lookup this is meant to stay.
- [x] **Step 3: Temporary self-test**: a top-level arrow function call produces the same entity/scene-prepended shape as an equivalent `function` declaration; a local arrow function becomes a real lambda and is callable; a template literal mixing a `string` and a `number` variable stringifies correctly; interpolating a general expression (`entity.getPosition()`) is rejected by name. Every generated case additionally verified against a real `g++ -fsyntax-only` invocation, not just string-matched.
- [x] **Step 4: Run, confirm, delete. `git grep TEMPORARY` clean.** 11/11.

### Task 3: Simple classes

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h` (`s_CodegenSrc`)

**Interfaces:**
- Consumes: `TranspileToCpp` from Task 1 (extends it; signature unchanged).

- [x] **Step 1:** Typed fields, methods, a constructor, and single inheritance (`extends`) on a TS class → a C++ class with matching members/methods/constructor/public inheritance, emitted as its own top-level class alongside (not nested in) the entity's generated class -- a class's methods do *not* get entity/scene prepended, since a class has no relation to the entity-script closure model that exists to preserve. Everything comes out `public:` (access modifiers/`readonly` aren't translated -- a disclosed simplification, not a correctness issue). Reject (naming the construct): `implements`, multiple inheritance, abstract classes, static members, computed/destructured member names, getters/setters, decorators. **Scope note found while implementing, not in the original plan text**: `super(...)` calls aren't a recognized expression form -- a derived class's constructor can't call its base constructor. Real fields/methods/single-inheritance still work; a derived class needing to pass constructor arguments up to its base doesn't, in this task.
- [x] **Step 2: Temporary self-test**: a class with a typed field + constructor + method produces a compiling C++ class with `this.x` mapped to `this->x`; `extends` produces public inheritance; an abstract class and a static member are each rejected by name. All confirmed against real `g++ -fsyntax-only`.
- [x] **Step 3: Run, confirm, delete. `git grep TEMPORARY` clean.** 12/12.

### Task 4: Simple generics

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h` (`s_CodegenSrc`)

**Interfaces:**
- Consumes: `TranspileToCpp` from Task 1 (extends it; signature unchanged).

- [x] **Step 1:** An unconstrained (`<T>`) or simply-constrained (`<T extends number>`) generic function → a C++ `template<typename T>` function, applied to top-level `function` declarations and top-level const-assigned arrow functions (entity/scene are still prepended -- a generic top-level function is still a top-level function). **Simplified from the plan's original wording, not a partial implementation**: rather than detecting and separately rejecting conditional types/mapped types/`infer` specifically, every constraint -- simple or complex -- is uniformly dropped, never translated. C++17 has no `concept` keyword to express any of them with, so a "simple" `T extends number` and a "complex" conditional-type constraint are equally unenforceable in the output; there was no correctness reason to tell them apart, only to drop both the same way. Generic *classes* are explicitly rejected by name (not silently mistranslated) -- out of this task's scope, which was functions only.
- [x] **Step 2: Temporary self-test**: an unconstrained generic function transpiles to a matching `template<typename T>` function with entity/scene still prepended; a simply-constrained one transpiles with the constraint silently dropped (confirmed against real `g++`); a generic class is rejected by name.
- [x] **Step 3: Run, confirm, delete. `git grep TEMPORARY` clean.** 8/8.

### Task 5: Syntax-check compile + live panel UI

**Files:**
- Modify: `TestEnv/src/TextEditorPanel.h` (split pane for `.gss` files; Ctrl+S triggers save + full pipeline; `Build Output` panel shows compile diagnostics via the existing `LastRunOutput`/`LastRunError` wiring)
- Modify: `TestEnv/src/ScriptEngine.h` if a small `RunSyntaxCheck(const std::string& cppPath, std::string& diagnostics)` helper belongs there (shells out to `g++ -std=c++17 -fsyntax-only -I<GS include paths>`, captures stderr)

**Interfaces:**
- Consumes: `ScriptEngine::TranspileToCpp` (Tasks 1-4).
- Produces: the live preview pane's content (refreshed on a ~500ms idle debounce after typing, transpile-only, no compiler invocation) and the Ctrl+S path (save `.gss` → `TranspileToCpp` → write `.cpp` → `RunSyntaxCheck` → diagnostics into `Build Output`).

- [ ] **Step 1:** Determine, once, the actual include-path arguments `g++ -fsyntax-only` needs against this project's own `GS/src` (and whatever `GS.h` itself pulls in) — read `premake5.lua`'s existing include-dir configuration rather than guessing a parallel path list.
- [ ] **Step 2:** Wire the split pane: only appears for a `.gss`/`.ts` file; left pane unchanged `TextEditorPanel` content, right pane read-only `CellGrid` (or plain `ImGui::TextUnformatted`, simpler, revisit if it's unreadable) showing the last successful transpile.
- [ ] **Step 3:** Ctrl+S handling: save (existing `SaveToFile`), transpile, write the `.cpp` **only if this is a first-time generation or the optimize-lock (Task 6) says it's safe** — Task 5 can stub the lock check as "always safe" and let Task 6 wire the real one in, since Task 6 needs Task 5's write-path to exist first.
- [ ] **Step 4: Temporary self-test** for `RunSyntaxCheck` alone (no ImGui needed): a known-good generated `.cpp` reports no diagnostics; a deliberately broken one (bad include, or hand-corrupted after a real `TranspileToCpp` call) reports a real `g++` error string. The debounce/split-pane UI itself needs the disclosed manual click-through every other UI feature in this editor has needed (no GUI automation tool exists in this environment).
- [ ] **Step 5: Run the non-UI self-test, confirm, delete. `git grep TEMPORARY` clean.** Manually verify the split pane and Ctrl+S flow via `--capture`/a code-driven scripted run standing in for the click-through, matching this editor's established verification pattern for UI-interaction features.

### Task 6: Optimize-lock + dependency-manifest fan-out

**Files:**
- Modify: `TestEnv/src/TextEditorPanel.h` (the real lock check Task 5 stubbed, plus the three-way Cancel/Overwrite/Save-a-copy prompt)
- New: a small helper for reading `.gs-dependencies.txt` (imports plan) and finding a module's dependents — likely belongs beside `PlayMode::WriteDependencyManifest` in `PlayMode.h`, as its natural counterpart

**Interfaces:**
- Consumes: the `GS-GENERATED: <hash>` line `TranspileToCpp` (Task 1) already writes; `.gs-dependencies.txt`'s format (imports plan spec).
- Produces: `bool IsGeneratedFileHandEdited(const std::string& cppPath)` (recompute the hash over current content, compare to the stamped one); `std::vector<std::string> FindDependentScripts(const std::string& modulePath)`.

- [ ] **Step 1:** `IsGeneratedFileHandEdited` — read the file, extract the stamped hash from its first line, recompute over the rest, compare.
- [ ] **Step 2:** Wire it into Task 5's Ctrl+S path: equal → overwrite; different → the three-way prompt (Cancel / Overwrite / Save a copy to `<name>.generated.cpp`).
- [ ] **Step 3:** `FindDependentScripts` — parse `.gs-dependencies.txt`, return every `script` entry whose `import` list contains the given module path.
- [ ] **Step 4:** When a `.gss` module (not a leaf entity script) is saved: call `FindDependentScripts`, apply Step 1's check to each dependent's generated `.cpp`; if any are hand-edited, one consolidated dialog listing all of them with a per-script skip/overwrite/save-copy choice, not one popup per file.
- [ ] **Step 5: Temporary self-test**: an unedited generated file reports not-hand-edited; a hand-edited one reports hand-edited; `FindDependentScripts` against a hand-built manifest returns the right set; a module with three dependents, two hand-edited, produces the correct per-script classification for the consolidated dialog (UI itself needs the disclosed manual click-through).
- [ ] **Step 6: Run the non-UI self-test, confirm, delete. `git grep TEMPORARY` clean.**

---

## Notes for whoever picks this up

Tasks 1-4 are pure codegen (`ScriptEngine.h` only) and can each be verified with a self-test alone, no UI involved — the highest-value, lowest-risk place to make incremental progress across sessions. Tasks 5-6 need Tasks 1-4's `TranspileToCpp` to exist first, and touch real UI (`TextEditorPanel`), which is where the manual-click-through disclosure applies. `docs/STATE.md` names which of these are done as of any given session.
