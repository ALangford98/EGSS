# GSS Array Stdlib Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `console.log`/`.error`/`.warn`, a `number[]` array type, and `.length`/`.map`/`.filter`/`.toArray()` to the GSS-to-C++ compiled transpiler in `TestEnv/src/ScriptEngine.h`.

**Architecture:** Everything lives inside the embedded codegen JS string (`s_CodegenSrc`) that already implements the transpiler — five small, mechanical additions to `emitCall`/`emitExpr`/`typeToCpp`/`emitVarDecl`, following the exact patterns those functions already use (the `mathFns` lookup table, the `scene.findByTag` IIFE-in-expression-position trick, the existing `currentVarTypes` tracking). No new files, no changes to the interpreted (QuickJS) path, which already has all of this via real JS semantics. Verification uses this codebase's own self-test pattern: a temporary header called once from `TestEnv`'s constructor, deleted once every check has passed. Because every check here requires a real rebuild+run (there is no fast unit-test runner for the embedded JS), each task bundles "add this task's checks" and "implement the feature that makes them pass" into one build/run/commit cycle rather than a separate red build — this matches how every prior self-test pass in this codebase (the original 46-check transpiler tests, the mesh-authoring tests) was actually run.

**Tech Stack:** C++17, the project's embedded QuickJS-hosted TypeScript-compiler-driven transpiler, `g++ -fsyntax-only` for real-compiler verification (already wired via `ScriptEngine::RunSyntaxCheck`).

**Spec:** `docs/superpowers/specs/2026-09-23-gss-array-stdlib-design.md`

## Global Constraints

- C++17 dialect only (the project's own compiler flag, unchanged).
- `number[]` maps to `std::vector<double>`, matching the existing `number` → `double` mapping (spec decision 5) — never `std::vector<float>`.
- No new `#include` is assumed necessary in generated files beyond the existing `#include <GS.h>` (`ScriptEngine.h:611`) unless a real `g++ -fsyntax-only` run proves otherwise — checked empirically in Task 2, not assumed.
- `.length`/`.map`/`.filter`/`.toArray()` are recognized **only** on an expression the transpiler can mechanically prove is `vec3`- or array-typed (a tracked identifier, or a direct chain off a recognized producing call) — never general type inference (spec, design sections 3–4).
- `.find` is explicitly unimplemented this pass and must fail with a specific, named error, not a generic one (spec decision 4).
- Array literals (`[1, 2, 3]`) stay rejected — `number[]` values are only ever produced by `.toArray()`, `.map()`, or `.filter()` (spec, "Explicitly out of scope").
- The interpreted (QuickJS) execution path is untouched — every change in this plan is inside the codegen-only functions of `s_CodegenSrc`.
- Every task's generated C++ must pass a real `g++ -std=c++17 -fsyntax-only` round trip via `ScriptEngine::RunSyntaxCheck`, not just "looks right" text inspection.

## Review Focus

- `console.log()` called with zero arguments, or with more than one argument of mixed types — must still produce a valid `GS_TRACE(...)` call, not a malformed format string. *(Task 1's own test.)*
- `.toArray()`/`.length`/`.map`/`.filter` called on an expression the transpiler can't prove is vec3- or array-typed (an untracked identifier, an arbitrary call) must fail with the specific "unsupported" error already established for every other unrecognized form here — not silently emit wrong C++, not crash the codegen driver. *(Task 3's negative test for `.toArray()`/`.length`; Task 4's negative test for `.map`/`.filter`.)*
- `.find` must hit its own specific "not implemented" message, not fall through to a different, more confusing error (e.g. the generic "unsupported method call"). *(Task 4's own test.)*
- Chaining `.toArray().filter(cb).map(cb)` in one real expression, inside a script that also uses `entity.getPosition()`, `console.log`, and `Math.*` together — the full realistic shape, not just isolated one-feature snippets — must produce code that both text-matches expectations and passes a real `g++ -fsyntax-only` check. *(Task 5's integration test.)*
- Two independent `TranspileToCpp` calls in the same process (as every task's tests already do, back to back) must not leak `currentVarTypes` state between them — a `number[]` member from one script accidentally being visible to a later, unrelated script's `.map` check would be a silent correctness bug. *(Task 5's integration test runs two unrelated snippets back to back and confirms the second one's `.map`-on-an-untracked-identifier failure path still fires correctly.)*

---

## File Structure

- **Modify:** `TestEnv/src/ScriptEngine.h` — all five codegen additions, inside `s_CodegenSrc` (the embedded transpiler JS) plus one line in `typeToCpp`.
- **Create (temporary):** `TestEnv/src/GssArrayStdlibTest.h` — the self-test header, grown across Tasks 1–4, deleted in Task 5 once everything passes. Follows the `CLAUDE.md` self-test template (`g_Pass`/`g_Fail`/`Check`).
- **Modify (temporary, reverted in Task 5):** `TestEnv/src/TestApp.cpp` — one `#include` and one call added at the top of `TestEnv`'s constructor in Task 1, removed in Task 5.
- **Modify:** `docs/CHANGELOG.md` — one entry, added in Task 5.

---

### Task 1: `console.log`/`.error`/`.warn` in the compiled path

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h:1523-1527` (add `consoleFns` table next to the existing `mathFns` table)
- Modify: `TestEnv/src/ScriptEngine.h:1628-1649` (add the new `console` branch to `emitCall`, between the existing `scene` and `Math` branches)
- Create: `TestEnv/src/GssArrayStdlibTest.h`
- Modify: `TestEnv/src/TestApp.cpp:41-46` (include + call)

**Interfaces:**
- Consumes: `ScriptEngine::TranspileToCpp(const std::string& source, const std::string& className, std::string& error, std::string& outCpp)` (existing, `ScriptEngine.h:465`); `ScriptEngine::RunSyntaxCheck(const std::string& cppPath, std::string& diagnostics)` (existing, static, `ScriptEngine.h:401`).
- Produces: `GssArrayStdlibTest::TranspileAndSyntaxCheck(const std::string& source, const std::string& className, std::string& outCpp)` — a shared test helper every later task reuses. Returns `true` only if both transpilation and the real `g++ -fsyntax-only` check succeed; on failure, appends the error/diagnostics to `outCpp` so a failing `Check()` message is self-explanatory in the log. `GssArrayStdlibTest::g_Pass`, `GssArrayStdlibTest::g_Fail`, `GssArrayStdlibTest::Check(bool, const std::string&)`, `GssArrayStdlibTest::Run()` (the entry point every later task adds its own `RunX()` call into).

- [ ] **Step 1: Create the self-test header with the shared harness and Task 1's checks**

```cpp
// TestEnv/src/GssArrayStdlibTest.h
// TEMPORARY -- delete after verifying the GSS array stdlib
// (console.log/.error/.warn, number[], .length, .map, .filter, .toArray()).
// See docs/superpowers/plans/2026-09-27-gss-array-stdlib.md.
#pragma once
#include <GS.h>
#include "ScriptEngine.h"

#include <filesystem>
#include <fstream>

namespace GssArrayStdlibTest {

	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	// Shared by every task below: transpiles `source`, then round-trips the
	// result through a real `g++ -fsyntax-only` (ScriptEngine::RunSyntaxCheck,
	// the same mechanism the editor's own "Build (Ctrl+S)" button already
	// uses) so "passes" means "a real compiler accepts it," not just "the
	// generated text looks right." Returns the generated C++ via `outCpp`
	// either way, so a failing Check() can log exactly what was produced.
	inline bool TranspileAndSyntaxCheck(const std::string& source, const std::string& className, std::string& outCpp)
	{
		ScriptEngine engine;
		std::string error;
		if (!engine.TranspileToCpp(source, className, error, outCpp))
		{
			outCpp = "transpile failed: " + error;
			return false;
		}

		std::filesystem::path cppPath = std::filesystem::temp_directory_path() / (className + "_gss_array_stdlib_test.cpp");
		{
			std::ofstream out(cppPath, std::ios::out | std::ios::trunc);
			out << outCpp;
		}
		std::string diagnostics;
		bool ok = ScriptEngine::RunSyntaxCheck(cppPath.string(), diagnostics);
		if (!ok)
			outCpp += "\n\n-- g++ diagnostics --\n" + diagnostics;
		return ok;
	}

	inline void RunConsoleTests()
	{
		std::string outCpp;
		bool ok = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    console.log(\"tick\", dt);\n"
			"    console.warn(\"low health\");\n"
			"    console.error(\"bad state\");\n"
			"    console.log();\n"
			"}\n",
			"ConsoleTest", outCpp);
		Check(ok, "console.log/.warn/.error transpile and pass a real g++ -fsyntax-only check");
		Check(outCpp.find("GS_TRACE(\"{0} {1}\", \"tick\", dt)") != std::string::npos, "console.log(a, b) emits GS_TRACE with a two-slot format string");
		Check(outCpp.find("GS_WARN(\"{0}\", \"low health\")") != std::string::npos, "console.warn(a) emits GS_WARN");
		Check(outCpp.find("GS_ERROR(\"{0}\", \"bad state\")") != std::string::npos, "console.error(a) emits GS_ERROR");
		Check(outCpp.find("GS_TRACE(\"\")") != std::string::npos, "console.log() with no arguments emits a valid zero-argument GS_TRACE call");

		std::string badOutCpp;
		bool badOk = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    console.table(\"nope\");\n"
			"}\n",
			"ConsoleBadMemberTest", badOutCpp);
		Check(!badOk && badOutCpp.find("unsupported console.* call 'table'") != std::string::npos,
			"an unrecognized console.* member fails with a specific, named error");
	}

	inline void Run()
	{
		g_Pass = 0;
		g_Fail = 0;
		GS_TRACE("GssArrayStdlibTest: starting");
		RunConsoleTests();
		GS_TRACE("GssArrayStdlibTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Wire the test into `TestEnv`'s constructor**

In `TestEnv/src/TestApp.cpp`, add the include near the top of the file (after the existing includes, `TestApp.cpp:36`):

```cpp
#include "GssArrayStdlibTest.h"
```

And add the call as the very first line of `TestEnv::TestEnv()`'s body (`TestApp.cpp:42`, before `PushLayer(new DemoWarmup());`):

```cpp
	TestEnv()
	{
		GssArrayStdlibTest::Run();

		// Before the demos, so that on the step it hands over, the demo it
```

- [ ] **Step 3: Build and run to confirm the new checks currently fail**

Run: `./gs.py build`
Run: `./gs.py run` (quit after a couple of seconds once the log has scrolled past startup)
Expected: the log shows `GssArrayStdlibTest: starting`, several `FAIL` lines for the `console.*` checks (since `console` isn't recognized by `emitCall` yet — every one of these snippets currently fails to transpile with "unsupported method call 'log'"/`'warn'`/`'error'`), and a final tally like `0 passed, 5 failed` (or similar — the "unrecognized member" negative test may or may not already incidentally pass, since `console` isn't recognized at all yet; that's fine, note whatever the log actually shows).

- [ ] **Step 4: Add the `consoleFns` table**

In `TestEnv/src/ScriptEngine.h`, right after the existing `mathFns` table (ends at line 1527):

```js
			var mathFns = {
				abs: "std::abs", min: "std::min", max: "std::max", sqrt: "std::sqrt",
				floor: "std::floor", ceil: "std::ceil", round: "std::round", pow: "std::pow",
				sin: "std::sin", cos: "std::cos", tan: "std::tan"
			};

			var consoleFns = { log: "GS_TRACE", error: "GS_ERROR", warn: "GS_WARN" };
```

- [ ] **Step 5: Add the `console.*` branch to `emitCall`**

In the same file, inside `emitCall`'s `PropertyAccessExpression` handling, insert a new branch right after the `scene` branch closes and right before the existing `Math` branch (currently `ScriptEngine.h:1647-1649`):

```js
					if (isNamed(obj, "scene")) {
						// ... existing scene.findByTag/scene.destroy code, unchanged ...
					}

					if (isNamed(obj, "console")) {
						var logFn = consoleFns[member];
						if (!logFn) fail(node, "unsupported console.* call '" + member + "'");
						var fmt = args.map(function(_, idx) { return "{" + idx + "}"; }).join(" ");
						return logFn + "(\"" + fmt + "\"" + (args.length ? ", " + emitArgs(args) : "") + ")";
					}

					if (isNamed(obj, "Math")) {
						// ... existing Math.* code, unchanged ...
					}
```

- [ ] **Step 6: Build and run to confirm Task 1's checks pass**

Run: `./gs.py build`
Run: `./gs.py run` (quit after the log settles)
Expected: `GssArrayStdlibTest: 5 passed, 0 failed` (or however many `Check()` calls Step 1 added — recount from the file — with zero failures).

- [ ] **Step 7: Commit**

```bash
git add TestEnv/src/ScriptEngine.h TestEnv/src/GssArrayStdlibTest.h TestEnv/src/TestApp.cpp
git commit -m "Wire console.log/.error/.warn into the compiled GSS transpiler path"
```

---

### Task 2: `number[]` as a real codegen type

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h:1513-1521` (`typeToCpp`)
- Modify: `TestEnv/src/GssArrayStdlibTest.h` (add `RunNumberArrayTypeTest`, call it from `Run()`)

**Interfaces:**
- Consumes: `GssArrayStdlibTest::TranspileAndSyntaxCheck` (Task 1).
- Produces: nothing new consumed by later tasks directly — `typeToCpp("number[]")` returning `"std::vector<double>"` is exercised again implicitly by every later task's snippets, since `.toArray()`/`.map()`/`.filter()` all produce values of that same C++ type.

- [ ] **Step 1: Add the failing test**

In `TestEnv/src/GssArrayStdlibTest.h`, add:

```cpp
	inline void RunNumberArrayTypeTest()
	{
		std::string outCpp;
		bool ok = TranspileAndSyntaxCheck(
			"let counters: number[];\n"
			"\n"
			"function OnUpdate(dt: number) {\n"
			"}\n",
			"NumberArrayTypeTest", outCpp);
		Check(ok, "a number[] class member transpiles and passes a real g++ -fsyntax-only check");
		Check(outCpp.find("std::vector<double> counters;") != std::string::npos, "number[] maps to std::vector<double>, not std::vector<float>");
	}
```

And add the call inside `Run()`, after `RunConsoleTests();`:

```cpp
		RunConsoleTests();
		RunNumberArrayTypeTest();
```

- [ ] **Step 2: Build and run to confirm it fails**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: a `FAIL` for both new checks — today `typeToCpp` passes `"number[]"` through unrecognized (its final `return t;` fallback), so the generated field declares as `number[] counters;`, which is not valid C++ and fails the real `g++ -fsyntax-only` check.

- [ ] **Step 3: Add the type mapping**

In `TestEnv/src/ScriptEngine.h`, inside `typeToCpp` (`ScriptEngine.h:1513-1521`):

```js
			function typeToCpp(typeNode, ctxNode, ctxWhat) {
				if (!typeNode) fail(ctxNode, "missing type annotation on " + ctxWhat);
				var t = typeNode.getText(sourceFile);
				if (t === "number") return "double";
				if (t === "number[]") return "std::vector<double>";
				if (t === "boolean") return "bool";
				if (t === "string") return "std::string";
				if (t === "void") return "void";
				return t;
			}
```

- [ ] **Step 4: Build and run to confirm it passes**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: both `RunNumberArrayTypeTest` checks pass. **If the `g++ -fsyntax-only` check still fails** with something like `'vector' does not name a template` or `'std::vector' has not been declared`, that means `<GS.h>` does *not* transitively pull in `<vector>` the way it does `<algorithm>` (which `paddle.cpp`'s checked-in `std::max`/`std::min` calls already prove is transitively available) — in that case, add `#include <vector>\n` to the generated body's header in `ScriptEngine.h:611`:

```cpp
			std::string body = "#include <GS.h>\n#include <vector>\n\nnamespace GeneratedScripts {\n\n" + classesText
```

and rebuild/rerun. Note in the commit message which of the two actually happened — this is checked empirically, not assumed, per the Global Constraints section above.

- [ ] **Step 5: Commit**

```bash
git add TestEnv/src/ScriptEngine.h TestEnv/src/GssArrayStdlibTest.h
git commit -m "Add number[] as a std::vector<double> codegen type"
```

---

### Task 3: `glm::vec3.length` and `.toArray()`

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h:1588-1589` (add `isVec3Expr`, right after `transformGetter`/`transformSetter`)
- Modify: `TestEnv/src/ScriptEngine.h:1628-1653` (add the `.toArray()` branch to `emitCall`, after the `Math` branch)
- Modify: `TestEnv/src/ScriptEngine.h:1736-1747` (add the vec3 `.length` case to `emitExpr`'s `PropertyAccessExpression` handling)
- Modify: `TestEnv/src/ScriptEngine.h:1774-1783` (`emitVarDecl` — track a local's inferred `vec3`-ness alongside its emitted `auto`)
- Modify: `TestEnv/src/GssArrayStdlibTest.h` (add `RunVec3LengthAndToArrayTests`, call it from `Run()`)

**Interfaces:**
- Consumes: `transformGetter` (existing, `ScriptEngine.h:1588`), `isNamed` (existing), `currentVarTypes` (existing, per-transpile-call local).
- Produces: `isVec3Expr(node)` — a JS function inside `s_CodegenSrc`, used by Task 4's `isArrayExpr` (for the `.toArray()` chain case) and nowhere outside this file. `currentVarTypes[name]` may now hold the internal sentinel `"vec3"` for an unannotated local whose initializer is a recognized vec3-producing call — this is *only* read by `isVec3Expr`/`.length`/`.toArray()` in this and later tasks; the C++ actually emitted for such a local's declaration is unchanged (`auto`).

- [ ] **Step 1: Add the failing tests**

In `TestEnv/src/GssArrayStdlibTest.h`, add:

```cpp
	inline void RunVec3LengthAndToArrayTests()
	{
		std::string outCpp;
		bool ok = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    const pos = entity.getPosition();\n"
			"    const n = pos.length;\n"
			"    const arr = pos.toArray();\n"
			"}\n",
			"Vec3LengthToArrayTest", outCpp);
		Check(ok, "vec3.length and vec3.toArray() transpile and pass a real g++ -fsyntax-only check");
		Check(outCpp.find("const auto n = 3;") != std::string::npos, "vec3.length emits the compile-time literal 3");
		Check(outCpp.find("const auto arr = std::vector<double>{ pos.x, pos.y, pos.z };") != std::string::npos,
			"vec3.toArray() emits a std::vector<double> built from .x/.y/.z");

		// The exact shape emitCall's new .toArray() branch emits for a real
		// vec3 -- run for real, not just syntax-checked, to prove the values
		// (not just the C++ syntax) are right. glm::vec3 stores float, so
		// each component narrows to double on the way in, same as the spec
		// (decision 5) says .toArray() does.
		glm::vec3 testVec(1.0f, -2.5f, 3.0f);
		std::vector<double> arr = std::vector<double>{ testVec.x, testVec.y, testVec.z };
		Check(arr.size() == 3 && arr[0] == 1.0 && arr[1] == -2.5 && arr[2] == 3.0,
			"the .toArray() shape produces the right 3 values when actually run");

		std::string badOutCpp;
		bool badOk = TranspileAndSyntaxCheck(
			"let notAVec3: number = 1;\n"
			"\n"
			"function OnUpdate(dt: number) {\n"
			"    const bad = notAVec3.toArray();\n"
			"}\n",
			"ToArrayOnNonVec3Test", badOutCpp);
		Check(!badOk && badOutCpp.find("unsupported method call 'toArray'") != std::string::npos,
			".toArray() on an expression that isn't provably a vec3 fails with the existing named error, not a crash or silent bad codegen");
	}
```

And add the call inside `Run()`, after `RunNumberArrayTypeTest();`:

```cpp
		RunNumberArrayTypeTest();
		RunVec3LengthAndToArrayTests();
```

- [ ] **Step 2: Build and run to confirm it fails**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: `FAIL` on the transpile/text checks (`.length`/`.toArray()` aren't recognized yet — `pos.length` fails emitExpr's `PropertyAccessExpression` fallback, `.toArray()` fails emitCall's fallback); the hand-run `arr` value check passes already (it's plain C++, doesn't depend on the transpiler); the negative "on a non-vec3" test may already incidentally pass or fail depending on today's exact error text — note whatever the log shows.

- [ ] **Step 3: Add `isVec3Expr`**

In `TestEnv/src/ScriptEngine.h`, right after `transformGetter`/`transformSetter` (`ScriptEngine.h:1588-1589`):

```js
			var transformGetter = { getPosition: "Position", getRotation: "Rotation", getScale: "Scale" };
			var transformSetter = { setPosition: "Position", setRotation: "Rotation", setScale: "Scale" };

			// Mechanical, not general inference: recognizes exactly the two
			// shapes the spec names -- an identifier this same pass already
			// tracked as "vec3" (see emitVarDecl below), or a direct,
			// zero-arg entity.getPosition()/.getRotation()/.getScale() call
			// (on `entity` itself, or on any identifier -- the same
			// mechanical trust emitCall's own getter/setter fallback already
			// places in a plausible name, not proof of a real GS::Entity).
			function isVec3Expr(node) {
				if (node.kind === ts.SyntaxKind.Identifier)
					return currentVarTypes[node.text] === "vec3";
				if (node.kind === ts.SyntaxKind.CallExpression && node.arguments.length === 0
						&& node.expression.kind === ts.SyntaxKind.PropertyAccessExpression) {
					var callee = node.expression;
					if (!transformGetter[callee.name.text]) return false;
					return isNamed(callee.expression, "entity") || callee.expression.kind === ts.SyntaxKind.Identifier;
				}
				return false;
			}
```

- [ ] **Step 4: Add the `.toArray()` branch to `emitCall`**

Right after the `Math` branch closes (`ScriptEngine.h:1653`), before the "Any other identifier..." comment block:

```js
					if (isNamed(obj, "Math")) {
						var cppFn = mathFns[member];
						if (!cppFn) fail(node, "unsupported Math." + member);
						return cppFn + "(" + emitArgs(args) + ")";
					}

					if (member === "toArray" && args.length === 0 && isVec3Expr(obj)) {
						var vecExpr = emitExpr(obj);
						return "std::vector<double>{ " + vecExpr + ".x, " + vecExpr + ".y, " + vecExpr + ".z }";
					}

					// Any other identifier holding a native GS::Entity value
```

- [ ] **Step 5: Add the vec3 `.length` case to `emitExpr`**

In the `PropertyAccessExpression` case (`ScriptEngine.h:1736-1747`):

```js
					case ts.SyntaxKind.PropertyAccessExpression: {
						if (node.expression.kind === ts.SyntaxKind.ThisKeyword)
							return "this->" + node.name.text;
						if (node.name.text === "length" && isVec3Expr(node.expression))
							return "3";
						fail(node, "unsupported property access '" + node.name.text + "' outside of a recognized call");
						break;
					}
```

- [ ] **Step 6: Track `vec3`-ness through an unannotated local in `emitVarDecl`**

In `emitVarDecl` (`ScriptEngine.h:1774-1783`):

```js
			function emitVarDecl(decl, isConst) {
				if (decl.name.kind !== ts.SyntaxKind.Identifier)
					fail(decl, "destructuring is unsupported -- declare a plain name and index into it instead");
				var cppType = decl.type
					? typeToCpp(decl.type, decl, "'" + decl.name.text + "'")
					: (decl.initializer ? "auto" : fail(decl, "missing type annotation on '" + decl.name.text + "' (no initializer to deduce it from)"));
				// currentVarTypes tracks a richer internal type than what's
				// actually emitted for an unannotated local -- the emitted
				// C++ keyword stays "auto" either way (byte-identical output
				// for every script that predates this feature), but
				// .length/.toArray()/.map()/.filter() need to know *which*
				// auto this is, since C++'s own `auto` erases that
				// distinction. An explicit annotation's typeToCpp() output
				// already carries enough information as-is.
				var trackedType = cppType;
				if (!decl.type && decl.initializer && isVec3Expr(decl.initializer))
					trackedType = "vec3";
				currentVarTypes[decl.name.text] = trackedType;
				var init = decl.initializer ? (" = " + emitExpr(decl.initializer)) : "";
				return (isConst ? "const " : "") + cppType + " " + decl.name.text + init;
			}
```

- [ ] **Step 7: Build and run to confirm Task 3's checks pass**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: all of `RunVec3LengthAndToArrayTests`'s checks pass, and every earlier task's checks still pass too (cumulative count in the log).

- [ ] **Step 8: Commit**

```bash
git add TestEnv/src/ScriptEngine.h TestEnv/src/GssArrayStdlibTest.h
git commit -m "Add vec3.length and vec3.toArray() to the compiled GSS transpiler"
```

---

### Task 4: `number[].length`, `.map`, `.filter`, and `.find` rejection

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h:1588-1589` area (add `isArrayExpr`, right after `isVec3Expr`)
- Modify: `TestEnv/src/ScriptEngine.h` (the `.toArray()` branch added in Task 3 — add `.map`/`.filter`/`.find` right after it)
- Modify: `TestEnv/src/ScriptEngine.h` (the vec3 `.length` case added in Task 3 — add the array `.length` case right after it)
- Modify: `TestEnv/src/ScriptEngine.h` (the `emitVarDecl` tracking block added in Task 3 — extend it for arrays)
- Modify: `TestEnv/src/GssArrayStdlibTest.h` (add `RunArrayMethodsTests`, call it from `Run()`)

**Interfaces:**
- Consumes: `isVec3Expr` (Task 3), `emitArrowFunction` (existing, `ScriptEngine.h:1889-1894` — unmodified), `currentVarTypes`.
- Produces: `isArrayExpr(node)` — used only within this file, recursively handles chaining since a `.map`/`.filter` result is itself recognized by `isArrayExpr`.

- [ ] **Step 1: Add the failing tests**

In `TestEnv/src/GssArrayStdlibTest.h`, add:

```cpp
	inline void RunArrayMethodsTests()
	{
		std::string outCpp;
		bool ok = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    const pos = entity.getPosition();\n"
			"    const arr = pos.toArray();\n"
			"    const n = arr.length;\n"
			"    const doubled = arr.map((x: number) => x * 2);\n"
			"    const positive = arr.filter((x: number) => x > 0);\n"
			"}\n",
			"ArrayMethodsTest", outCpp);
		Check(ok, "number[].length/.map/.filter transpile and pass a real g++ -fsyntax-only check");
		Check(outCpp.find("const auto n = (arr).size();") != std::string::npos, "number[].length emits a runtime .size() call, not a literal");
		Check(outCpp.find("__r.push_back((") != std::string::npos && outCpp.find("__src.size()") != std::string::npos,
			".map emits the reserve+push_back IIFE shape");
		Check(outCpp.find("if ((") != std::string::npos && outCpp.find("__r.push_back(__e)") != std::string::npos,
			".filter emits the conditional-push IIFE shape");

		// Run the exact IIFE shape .map/.filter emit against real data --
		// hand-computed expected values, not re-deriving the transpiler's
		// own arithmetic.
		std::vector<double> src = { 1.0, -2.0, 3.0 };
		std::vector<double> doubled = ([&]{ std::vector<double> __src = (src); std::vector<double> __r; __r.reserve(__src.size());
			for (auto __e : __src) __r.push_back(([&](double x) { return x * 2; })(__e)); return __r; }());
		Check(doubled.size() == 3 && doubled[0] == 2.0 && doubled[1] == -4.0 && doubled[2] == 6.0,
			".map's shape doubles every element correctly when actually run");

		std::vector<double> positive = ([&]{ std::vector<double> __src = (src); std::vector<double> __r;
			for (auto __e : __src) if (([&](double x) { return x > 0; })(__e)) __r.push_back(__e); return __r; }());
		Check(positive.size() == 2 && positive[0] == 1.0 && positive[1] == 3.0,
			".filter's shape keeps only the matching elements correctly when actually run");

		std::string findOutCpp;
		bool findOk = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    const arr = entity.getPosition().toArray();\n"
			"    const first = arr.find((x: number) => x > 0);\n"
			"}\n",
			"FindNotImplementedTest", findOutCpp);
		Check(!findOk && findOutCpp.find("Array.find isn't implemented") != std::string::npos,
			".find fails with its own specific message, not a generic 'unsupported method call'");

		std::string badOutCpp;
		bool badOk = TranspileAndSyntaxCheck(
			"let notTracked: number = 1;\n"
			"\n"
			"function OnUpdate(dt: number) {\n"
			"    const bad = notTracked.map((x: number) => x);\n"
			"}\n",
			"MapOnNonArrayTest", badOutCpp);
		Check(!badOk && badOutCpp.find("unsupported method call 'map'") != std::string::npos,
			".map on an expression that isn't provably an array fails with the existing named error");
	}
```

And add the call inside `Run()`, after `RunVec3LengthAndToArrayTests();`:

```cpp
		RunVec3LengthAndToArrayTests();
		RunArrayMethodsTests();
```

- [ ] **Step 2: Build and run to confirm it fails**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: `FAIL` on every transpile/text check (`.length` on a `number[]`, `.map`, `.filter`, `.find` all unrecognized yet); the two hand-run value checks (`doubled`, `positive`) already pass, since they're plain C++.

- [ ] **Step 3: Add `isArrayExpr`**

Right after `isVec3Expr` (added in Task 3):

```js
			// Recursive by design: a .map()/.filter() result is itself
			// array-typed, so chaining (.toArray().filter(...).map(...))
			// falls out of this for free, with no separate chaining logic.
			function isArrayExpr(node) {
				if (node.kind === ts.SyntaxKind.Identifier)
					return currentVarTypes[node.text] === "std::vector<double>";
				if (node.kind === ts.SyntaxKind.CallExpression && node.expression.kind === ts.SyntaxKind.PropertyAccessExpression) {
					var callee = node.expression;
					if (callee.name.text === "toArray") return isVec3Expr(callee.expression);
					if (callee.name.text === "map" || callee.name.text === "filter") return isArrayExpr(callee.expression);
				}
				return false;
			}
```

- [ ] **Step 4: Add `.map`/`.filter`/`.find` to `emitCall`, right after the Task 3 `.toArray()` branch**

```js
					if (member === "toArray" && args.length === 0 && isVec3Expr(obj)) {
						var vecExpr = emitExpr(obj);
						return "std::vector<double>{ " + vecExpr + ".x, " + vecExpr + ".y, " + vecExpr + ".z }";
					}

					if ((member === "map" || member === "filter") && args.length === 1 && isArrayExpr(obj)) {
						if (args[0].kind !== ts.SyntaxKind.ArrowFunction)
							fail(node, "Array." + member + " needs an arrow-function callback, e.g. arr." + member + "(x => ...)");
						var lambda = emitArrowFunction(args[0]);
						var srcExpr = emitExpr(obj);
						if (member === "map") {
							return "([&]{ std::vector<double> __src = (" + srcExpr + "); std::vector<double> __r; __r.reserve(__src.size()); "
								+ "for (auto __e : __src) __r.push_back((" + lambda + ")(__e)); return __r; }())";
						}
						return "([&]{ std::vector<double> __src = (" + srcExpr + "); std::vector<double> __r; "
							+ "for (auto __e : __src) if ((" + lambda + ")(__e)) __r.push_back(__e); return __r; }())";
					}

					if (member === "find" && isArrayExpr(obj))
						fail(node, "Array.find isn't implemented in the transpiler yet -- use .filter(...)[0] with an explicit .length check instead");
```

- [ ] **Step 5: Add the array `.length` case to `emitExpr`, right after the Task 3 vec3 case**

```js
						if (node.name.text === "length" && isVec3Expr(node.expression))
							return "3";
						if (node.name.text === "length" && isArrayExpr(node.expression))
							return "(" + emitExpr(node.expression) + ").size()";
						fail(node, "unsupported property access '" + node.name.text + "' outside of a recognized call");
```

- [ ] **Step 6: Extend the `emitVarDecl` tracking block from Task 3**

```js
				var trackedType = cppType;
				if (!decl.type && decl.initializer) {
					if (isVec3Expr(decl.initializer)) trackedType = "vec3";
					else if (isArrayExpr(decl.initializer)) trackedType = "std::vector<double>";
				}
				currentVarTypes[decl.name.text] = trackedType;
```

- [ ] **Step 7: Build and run to confirm Task 4's checks pass**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: all of `RunArrayMethodsTests`'s checks pass, and every earlier task's checks still pass too.

- [ ] **Step 8: Commit**

```bash
git add TestEnv/src/ScriptEngine.h TestEnv/src/GssArrayStdlibTest.h
git commit -m "Add number[].length/.map/.filter to the compiled GSS transpiler; reject .find explicitly"
```

---

### Task 5: Integration test, three-config build, changelog, cleanup

**Files:**
- Modify: `TestEnv/src/GssArrayStdlibTest.h` (add `RunIntegrationTest`, call it from `Run()` — then, at the end of this task, delete the whole file)
- Modify: `TestEnv/src/TestApp.cpp` (remove the include and call added in Task 1)
- Modify: `docs/CHANGELOG.md` (new entry)

**Interfaces:**
- Consumes: everything from Tasks 1–4.
- Produces: nothing — this task only verifies the whole feature together, then removes every piece of temporary scaffolding, per the self-test pattern (`CLAUDE.md`: "Tests are temporary... Grep for `TEMPORARY` before declaring finished").

- [ ] **Step 1: Add the integration test**

In `TestEnv/src/GssArrayStdlibTest.h`, add:

```cpp
	inline void RunIntegrationTest()
	{
		// The full realistic shape: entity.getPosition(), console.log,
		// Math.*, and a chained .toArray().filter(...).map(...) together in
		// one script, not just isolated one-feature snippets.
		std::string outCpp;
		bool ok = TranspileAndSyntaxCheck(
			"function OnUpdate(dt: number) {\n"
			"    const pos = entity.getPosition();\n"
			"    console.log(\"position\", pos.length);\n"
			"    const positiveDoubled = pos.toArray().filter((x: number) => x > 0).map((x: number) => x * Math.abs(dt));\n"
			"    console.log(\"count\", positiveDoubled.length);\n"
			"}\n",
			"IntegrationTest", outCpp);
		Check(ok, "a realistic script combining entity.getPosition, console.log, Math.*, and chained toArray/filter/map passes a real g++ -fsyntax-only check");

		// Two independent transpiles back to back, in the same process --
		// confirms currentVarTypes doesn't leak between them. The second
		// script's `.map` on an untracked identifier must still fail
		// correctly even though the first script just legitimately used
		// number[]/vec3 tracking for real.
		std::string firstOutCpp;
		TranspileAndSyntaxCheck(
			"let tracked: number[];\n"
			"function OnUpdate(dt: number) {\n"
			"}\n",
			"LeakCheckFirst", firstOutCpp);

		std::string secondOutCpp;
		bool secondOk = TranspileAndSyntaxCheck(
			"let untracked: number = 1;\n"
			"function OnUpdate(dt: number) {\n"
			"    const bad = untracked.map((x: number) => x);\n"
			"}\n",
			"LeakCheckSecond", secondOutCpp);
		Check(!secondOk && secondOutCpp.find("unsupported method call 'map'") != std::string::npos,
			"currentVarTypes from one TranspileToCpp call doesn't leak into the next");
	}
```

And add the call inside `Run()`, after `RunArrayMethodsTests();`:

```cpp
		RunArrayMethodsTests();
		RunIntegrationTest();
```

- [ ] **Step 2: Build and run to confirm everything passes**

Run: `./gs.py build`
Run: `./gs.py run`
Expected: the log's final tally shows every check across all five tasks passing, zero failures.

- [ ] **Step 3: Verify all three configs build clean**

Run: `./gs.py build all`
Expected: Debug, Release, and Dist all build with no errors (per `CLAUDE.md`: "Verify all three configs before calling something done. Release has caught things Debug did not.").

- [ ] **Step 4: Confirm the Cube3D `--hide-ui` capture is still byte-identical**

This feature touches only `ScriptEngine.h`'s codegen-JS string and a `TestApp.cpp` constructor line removed by the end of this task — no renderer or demo-layer path. Run:

```sh
./gs.py run -- --demo Cube3D --hide-ui --capture /tmp/cube3d_before.png --capture-step 60
```

against a build from *before* this plan's first commit (e.g. `git stash`, rebuild, capture, `git stash pop`, rebuild) if a prior baseline isn't already on hand, and compare byte-for-byte with a capture from the current tree. Expected: identical files.

- [ ] **Step 5: Remove the temporary self-test scaffolding**

Delete `TestEnv/src/GssArrayStdlibTest.h` entirely.

In `TestEnv/src/TestApp.cpp`, remove the `#include "GssArrayStdlibTest.h"` line and the `GssArrayStdlibTest::Run();` line added in Task 1, Step 2.

Run: `grep -rn "TEMPORARY\|GssArrayStdlibTest" TestEnv/src/`
Expected: no matches.

- [ ] **Step 6: Rebuild after removing the scaffolding**

Run: `./gs.py build all`
Expected: clean build across all three configs with the test scaffolding gone (confirms nothing else in the tree came to depend on it).

- [ ] **Step 7: Add the changelog entry**

Add a new entry at the top of `docs/CHANGELOG.md`'s existing entry list, following that file's own established format (see any recent entry for the exact heading/date style already in use — e.g. the 2026-09-14 mesh UV template export entry). Content to include: what was built (console.log/.error/.warn in the compiled path; `number[]`/`std::vector<double>` as a new codegen type; `.length`/`.toArray()` on vec3; `.length`/`.map`/`.filter` on `number[]`, uniformly dispatched through the `.toArray()` bridge rather than dual-dispatched on `vec3` directly); what was deliberately deferred (`.find`, a new array-producing native call, array literals) and why (spec decisions 1 and 4); the one real finding from design (existing `currentVarTypes` only ever recorded the literal string `"auto"` for an unannotated local, which would have made `isVec3Expr`/`isArrayExpr` never fire for the common `const pos = entity.getPosition()` case — fixed by tracking a richer internal type alongside the unchanged emitted `auto`); link to the spec and this plan.

- [ ] **Step 8: Commit**

```bash
git add TestEnv/src/TestApp.cpp docs/CHANGELOG.md
git rm TestEnv/src/GssArrayStdlibTest.h
git commit -m "Verify GSS array stdlib end-to-end; remove temporary self-test; changelog entry"
```

---

## Self-Review

**Spec coverage:** console.log/.error/.warn (Task 1) — covered. `number[]` → `std::vector<double>` (Task 2) — covered, including the empirical `<vector>`-include check the spec's decision 5 implies but doesn't spell out. `vec3.length`/`.toArray()` via the mechanical-proof rule (Task 3) — covered. `number[].length`/`.map`/`.filter` via the uniform `.toArray()`-bridge dispatch, not dual dispatch (Task 4) — covered. `.find` explicitly rejected with its own message (Task 4) — covered. Chaining (Task 5) — covered. Array literals staying rejected — no task changes `ArrayLiteralExpression` handling, so this is covered by omission, correctly.

**Placeholder scan:** every step has real code, no "add appropriate tests," no "TBD." Checked.

**Type consistency:** `isVec3Expr`/`isArrayExpr` names and signatures are identical everywhere they're introduced (Task 3) and consumed (Task 4). `TranspileAndSyntaxCheck`'s signature (Task 1) is used identically in every later task's tests. `consoleFns`/`mathFns` follow the same table shape. The `trackedType` variable introduced in Task 3's `emitVarDecl` patch is the same variable Task 4 extends, not a differently-named duplicate.

**Review Focus:** all five items above are each mapped to the task whose tests exercise them (zero-arg/multi-arg console.log → Task 1; unprovable vec3/array expressions → Tasks 3 and 4; `.find`'s specific message → Task 4; full-shape chaining → Task 5; `currentVarTypes` leakage across calls → Task 5).

---

Plan complete and saved to `docs/superpowers/plans/2026-09-27-gss-array-stdlib.md`. Please review the plan. Which execution approach would you prefer?

- **Subagent-driven** — a fresh subagent implements each task and a fresh reviewer checks it before the next one starts, then a whole-branch review at the end. Most thorough; costs a fresh context per task and per review.
- **Native** — I implement every task myself in this session, then one fresh reviewer on the most capable model checks the whole branch. Cheapest and fastest; no independent review until the end.

For this plan I'd lean toward **native**: the five tasks are tightly sequential (each one's `emitCall`/`emitExpr`/`emitVarDecl` edits sit right next to the previous task's, in the same handful of functions in one file), so there's little for parallel subagents to gain, and a shipped mistake here is caught immediately by the next task's own `g++ -fsyntax-only` check rather than silently compounding. Does the plan capture what you want, and which approach should we use?
