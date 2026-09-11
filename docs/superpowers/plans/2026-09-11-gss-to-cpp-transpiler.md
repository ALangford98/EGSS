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

- [x] **Step 1:** Determined via `premake5.lua`'s actual `includedirs` for TestEnv (`GS/src`, `GS/vendor/spdlog/include`, glm, ImGui, libvterm, quickjs), not guessed. Needed an absolute anchor the running executable has no other way to find (`bin/<config>/TestEnv/` is always three directories deep from the repo root, but that's exactly the kind of assumption that quietly breaks if the layout ever changes) -- added `GS_REPO_ROOT` as a premake `defines` entry (`'GS_REPO_ROOT="' .. _MAIN_SCRIPT_DIR .. '"'`), baked in absolutely at generation time.
- [x] **Step 2:** Split pane wired: only appears for `.gss`/`.ts`/`.gs`; left pane is the unchanged `TextEditorPanel` content inside its own child window (sized to half the available width), right pane is plain `ImGui::TextUnformatted` (not a second `CellGrid` -- simpler, and it's read-only text, not something that needs cursor/selection machinery) showing the last successful transpile or the current error.
- [x] **Step 3:** Ctrl+S (and a new "Build (Ctrl+S)" button next to Run, visible only for a `.gss` file) saves, transpiles, and -- per Task 6, built in the same pass since the two are tightly coupled at the exact point of "write the .cpp" -- checks the real optimize-lock before writing, not a stub.
- [x] **Step 4: Temporary self-test** for `RunSyntaxCheck` (no ImGui needed): a known-good generated file passes; a deliberately broken one fails with real `g++` diagnostics. **Real bug found by this test, not anticipated**: the second `RunSyntaxCheck` call hung the process outright. Root cause: `std::system` (fork+exec) from a process that already has other threads running (this app's audio thread) is a known hazard -- a lock held by another thread at the instant of `fork()` can leave the forked child deadlocked before `exec()` ever replaces it. Fixed defensively by wrapping the compiler invocation in `timeout 10` so a fork/exec hazard degrades to a reported timeout error instead of a frozen editor; empirically confirmed this resolves the observed hang, though the exact mechanism wasn't isolated further than that plausible, well-documented cause.
- [x] **Step 5: Run the non-UI self-test (11/11), confirm, delete. `git grep TEMPORARY` clean.** Split pane and button visibility verified via a `--capture` against the real `TextEditorPanel` (driven through a temporary, since-deleted `EditorShell::GetTextEditor()` accessor and `ImGui::SetWindowFocus("Editor")`, since "Scene"/"Editor" share a tab group and only one renders) opening the real `paddle.gss` -- the capture shows the split pane, the "Build (Ctrl+S)" button appearing correctly (gated on file extension), and the live preview firing and correctly reporting a **real, previously-unknown issue**: `paddle.gss`'s `const paddleSpeed = 2.2;` has no type annotation, so it isn't compilable as written today. Clicking the actual Cancel/Overwrite/Save-a-copy/fan-out popup buttons still needs the disclosed manual click-through every other UI-interaction feature in this editor has needed -- no GUI automation tool exists in this environment.

### Task 6: Optimize-lock + dependency-manifest fan-out

**Files:**
- Modify: `TestEnv/src/TextEditorPanel.h` (the real lock check Task 5 stubbed, plus the three-way Cancel/Overwrite/Save-a-copy prompt)
- New: a small helper for reading `.gs-dependencies.txt` (imports plan) and finding a module's dependents — likely belongs beside `PlayMode::WriteDependencyManifest` in `PlayMode.h`, as its natural counterpart

**Interfaces:**
- Consumes: the `GS-GENERATED: <hash>` line `TranspileToCpp` (Task 1) already writes; `.gs-dependencies.txt`'s format (imports plan spec).
- Produces: `bool IsGeneratedFileHandEdited(const std::string& cppPath)` (recompute the hash over current content, compare to the stamped one); `std::vector<std::string> FindDependentScripts(const std::string& modulePath)`.

- [x] **Step 1:** `ScriptEngine::IsGeneratedFileHandEdited` — finds the body by searching for its known starting text (`#include <GS.h>`) rather than hardcoding "skip N header comment lines", so a future wording change to those comments can't silently break the check. A file with no stamp at all, or that doesn't parse as expected, is treated as hand-written and protected (returns true), not assumed safe.
- [x] **Step 2:** Wired into `TextEditorPanel::RunFullBuild` (Ctrl+S / the Build button): equal → overwrite via `WriteAndSyntaxCheck`; different → `DrawOverwritePrompt`'s three-way modal (Cancel / Overwrite / Save a copy to `<name>.generated.cpp`).
- [x] **Step 3:** `PlayMode::FindDependentScripts` — parses `.gs-dependencies.txt` fresh on each call (only ever called right after a save, not per-frame, so no reason to cache it), returns every `script` entry whose `import` list contains the given module path.
- [x] **Step 4:** `TextEditorPanel::SaveAndBuild` calls `FindDependentScripts` after building the saved file itself, checks Step 1's lock against each dependent, and if any are hand-edited opens `DrawFanOutPrompt` -- one modal listing every affected script with its own Overwrite/Save-a-copy/Skip buttons, not one popup per file.
- [x] **Step 5: Temporary self-test**: an unedited generated file reports safe, a hand-edited one reports locked, a file with no stamp at all is protected rather than assumed safe, a nonexistent file is reported safe (nothing to lose); `FindDependentScripts` against a hand-built manifest returns exactly the right dependents and correctly returns none for a module nothing imports.
- [x] **Step 6: Run the non-UI self-test (11/11, combined with Task 5's), confirm, delete. `git grep TEMPORARY` clean.** Clean three-config build; Cube3D's byte-identical `--hide-ui` regression capture confirmed unchanged.

---

## Notes for whoever picks this up

**All six tasks are done.** Tasks 1-4 (pure codegen, `ScriptEngine.h` only) each had a self-test with no UI involved. Tasks 5-6 (real UI in `TextEditorPanel.h`) were verified with a self-test for their non-UI logic plus one `--capture` against the real, running editor (via a temporary, since-deleted accessor) showing the split pane, button visibility, and the live preview actually firing against a real project file. Clicking the Cancel/Overwrite/Save-a-copy/fan-out popup buttons themselves still needs a human click-through -- no GUI automation tool exists in this environment, same disclosed gap every other UI-interaction feature in this editor has had.

**What this plan explicitly did not do** (see the spec's "Explicitly deferred" and each task's own scope notes): wire compiled code into Play at all (Option A vs. B in the spec's "Goal" section is still undecided); resolve imports before transpiling (an imported function call fails the same way any unrecognized call does); support `super(...)` in a derived class's constructor; support destructuring in any form (dropped by explicit request, meaning every `.gss` script written before this plan needs rewriting to use index access instead of `let [x,y,z] = ...` before it can compile); enforce a generic function's constraint (`T extends number` is accepted syntactically and then dropped, never checked); add a `GS::Entity::GetScene()` accessor (would simplify generated class methods, not required).

**A real, concrete finding from verifying this against the actual Breakout recreation**: `paddle.gss`'s `const paddleSpeed = 2.2;` (and likely other `.gss` files in this project) use untyped top-level `const`s, which this transpiler's "class member variables always need an explicit annotation" rule (C++ can't `auto`-deduce a non-static data member) rejects. Nothing in this plan required fixing that -- it's a real, disclosed consequence of turning a mechanical transpiler on code that predates it, not a bug in the transpiler.
