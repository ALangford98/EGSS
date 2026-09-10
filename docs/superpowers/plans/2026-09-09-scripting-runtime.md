# Scripting Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a TypeScript file from the text editor and see its output — the bare pipeline (edit → transpile → execute → see output) with no entity/scene bindings yet.

**Architecture:** Vendor QuickJS (a small, embeddable JS engine) and TypeScript's own npm-distributed compiler bundle. `ScriptEngine` runs the bundle inside QuickJS to strip a script's types, then evaluates the result in the same engine, capturing `console.log` output and errors. A "Run" button in the text editor triggers it; output lands in the editor's existing "Build Output" stub panel.

**Tech Stack:** C++17, QuickJS (new vendored submodule), TypeScript's compiler bundle (vendored as a checked-in JS asset, not built from source).

**Spec:** `docs/superpowers/specs/2026-09-09-scripting-runtime-design.md`

**Confirmed feasible by a spike before this plan was written**: the real `lib/typescript.js` from `typescript@5.6.3` loads and runs inside QuickJS (quickjs-ng) with zero modifications and zero Node polyfills. Measured: 0.27s to load the 8.9MB bundle once, ~5ms per `ts.transpileModule()` call. The exact driver script and API calls in Task 2 below are the same ones the spike verified working.

## Global Constraints

- **No git commits.** This repo's `CLAUDE.md` forbids commits to `main` and pushing. `git add` after each task's review passes is the task-boundary marker, same adaptation as every prior plan in this repo.
- **Temporary self-test per task**, deleted once it has done its job. `git grep TEMPORARY` must be clean before Task 3 (the last task) finishes.
- **`./gs.py build all` must succeed with zero errors before Task 3 finishes.**
- **No existing demo's behavior, capture, or replay may change.** Every file this plan touches is editor-only (`TestEnv/src/*.h`, none of which any demo includes), plus a new vendored library nothing but `TestEnv` links against. `ScriptEngine` is never constructed until the editor's "Run" button is actually clicked, and its owning panels never run under `--hide-ui`/`--capture`/`--play` — same guarantee every panel in this editor pivot already has.
- **Scripts have no engine access in v1.** No file/process/OS bindings (`quickjs-libc.c` is deliberately excluded from the build) and no Transform/Scene bindings (explicitly deferred). `console.log` is the only I/O surface.

---

### Task 1: Vendor QuickJS

**Files:**
- Create (git submodule): `GS/vendor/quickjs` — `https://github.com/quickjs-ng/quickjs.git`
- Create: `GS/vendor/quickjs_premake5.lua`
- Modify: `premake5.lua` (`IncludeDir` entry, `include` line, TestEnv `includedirs`/`links`)
- Create (temporary): `TestEnv/src/QuickJSSmokeTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `#include <quickjs.h>` becomes available to any file in `TestEnv/src` (the QuickJS static lib, linked into `TestEnv`). Task 2 is the only later task that uses it.

- [ ] **Step 1: Add QuickJS as a git submodule**

```bash
git submodule add https://github.com/quickjs-ng/quickjs.git GS/vendor/quickjs
```

This is `quickjs-ng`, the actively maintained fork (the original `bellard/quickjs` sees far less activity). MIT licensed — confirm by reading `GS/vendor/quickjs/LICENSE` after cloning. `git submodule add` stages `.gitmodules` and the gitlink automatically — do not run `git commit`.

- [ ] **Step 2: Write the QuickJS premake file**

QuickJS ships CMake/Makefile build files that this project doesn't use (no `cmake` available in every build environment this repo targets, and this project vendors via hand-written premake files for exactly this reason — see `imgui_premake5.lua`/`libvterm_premake5.lua`). Create `GS/vendor/quickjs_premake5.lua`:

```lua
-- QuickJS ships CMake/Makefile build files, not premake -- same situation
-- ImGui and libvterm were in. Lives outside the submodule so it survives a
-- re-clone. Deliberately excludes quickjs-libc.c: that file gives scripts
-- direct file/process/exec/signal access, which a sandboxed script host
-- has no business granting by default (see this plan's own Global
-- Constraints -- scripts get console.log only in v1).
project "quickjs"
	kind "StaticLib"
	language "C"
	cdialect "gnu11"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"quickjs/quickjs.h",
		"quickjs/quickjs.c",
		"quickjs/quickjs-atom.h",
		"quickjs/quickjs-opcode.h",
		"quickjs/quickjs-c-atomics.h",
		"quickjs/libregexp.c",
		"quickjs/libregexp.h",
		"quickjs/libregexp-opcode.h",
		"quickjs/libunicode.c",
		"quickjs/libunicode.h",
		"quickjs/libunicode-table.h",
		"quickjs/dtoa.c",
		"quickjs/dtoa.h",
		"quickjs/cutils.h",
		"quickjs/list.h",
	}

	includedirs
	{
		"quickjs",
	}

	defines
	{
		"_GNU_SOURCE",
	}

	filter "system:linux"
		pic "On"
		systemversion "latest"

	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"
```

Before trusting this file list, run `ls GS/vendor/quickjs/*.c GS/vendor/quickjs/*.h` and confirm every filename above actually exists in the cloned submodule — quickjs-ng's exact file set can drift between versions; adjust the `files` block to match what's actually there rather than assuming this list is exactly right (this is exactly the kind of "verify before trusting the plan" the earlier plans in this repo's history have repeatedly needed).

- [ ] **Step 3: Wire it into the root premake file**

In `premake5.lua`, add to the `IncludeDir` table (after the existing `IncludeDir["libvterm"]` line):

```lua
IncludeDir["quickjs"] = "GS/vendor/quickjs"
```

Add to the `include` block right after it:

```lua
include "GS/vendor/quickjs_premake5.lua"
```

In the `TestEnv` project's `includedirs` block, add `"%{IncludeDir.quickjs}"` alongside the existing entries. In the same project's `links` block, add `"quickjs"` alongside `"GS"`, `"ImGui"`, `"libvterm"`.

- [ ] **Step 4: Write a temporary smoke test**

Create `TestEnv/src/QuickJSSmokeTest.h`:

```cpp
// TEMPORARY -- delete after verifying QuickJS builds, links, and its
// basic API round-trips (Task 1 of the scripting-runtime plan).
#pragma once
#include <GS.h>
#include <quickjs.h>

namespace QuickJSSmokeTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        JSRuntime* rt = JS_NewRuntime();
        Check(rt != nullptr, "JS_NewRuntime returns a non-null JSRuntime");

        JSContext* ctx = JS_NewContext(rt);
        Check(ctx != nullptr, "JS_NewContext returns a non-null JSContext");

        const char* src = "1 + 1";
        JSValue result = JS_Eval(ctx, src, strlen(src), "smoke", JS_EVAL_TYPE_GLOBAL);
        Check(!JS_IsException(result), "evaluating '1 + 1' does not throw");

        int32_t intResult = 0;
        JS_ToInt32(ctx, &intResult, result);
        Check(intResult == 2, "'1 + 1' evaluates to 2");

        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
}
```

(Add `#include <cstring>` if `strlen` doesn't already resolve via `<GS.h>`'s own includes — check the build error if one occurs rather than assuming.)

- [ ] **Step 5: Call it once, build, and run**

Temporarily add `#include "QuickJSSmokeTest.h"` and `QuickJSSmokeTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: three `[ok ]` lines, `g_Pass == 3`, `g_Fail == 0`.

- [ ] **Step 6: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/QuickJSSmokeTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty.

- [ ] **Step 7: Stage**

```bash
git add GS/vendor/quickjs GS/vendor/quickjs_premake5.lua premake5.lua .gitmodules
git status --short
```

---

### Task 2: `ScriptEngine.h` — the transpile-and-run glue

**Files:**
- Create: `TestEnv/assets/typescript.js` (vendored data asset — see Step 1)
- Create: `TestEnv/src/ScriptEngine.h`
- Create (temporary): `TestEnv/src/ScriptEngineTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `class ScriptEngine` with `struct ScriptResult { bool Ok; std::string Output; std::string Error; };` and `ScriptResult RunScript(const std::string& tsSource)`. Task 3 consumes this exactly.

Does not depend on Task 1's smoke test (already cleaned up) but does depend on Task 1's vendored QuickJS library being linked in.

- [ ] **Step 1: Vendor the TypeScript compiler bundle**

```bash
mkdir -p /tmp/ts-vendor && cd /tmp/ts-vendor
curl -sL https://registry.npmjs.org/typescript/-/typescript-5.6.3.tgz -o ts.tgz
tar xzf ts.tgz
cp package/lib/typescript.js /home/anthony/Documents/EGSS/TestEnv/assets/typescript.js
cd /home/anthony/Documents/EGSS
ls -la TestEnv/assets/typescript.js
rm -rf /tmp/ts-vendor
```

This is `typescript@5.6.3`'s real, unmodified `lib/typescript.js` — the same file and version the plan's own feasibility spike verified running correctly inside QuickJS. Confirm the copied file is a few MB (roughly 8-9MB) and starts with a comment block naming "TypeScript" — if the download or extraction failed silently, this step would produce a much smaller or empty file; check before moving on.

- [ ] **Step 2: Write `ScriptEngine.h`**

```cpp
#pragma once

// Runs TypeScript by transpiling it with TypeScript's own compiler bundle
// (vendored as TestEnv/assets/typescript.js -- the real, unmodified
// lib/typescript.js from typescript@5.6.3, refreshed by re-running the
// `npm pack typescript@<version>` + extract steps this plan's Task 2
// used originally) running inside our own embedded QuickJS, then
// evaluating the resulting JS in the same engine. No file/process/OS
// access is exposed to scripts, and no engine/scene bindings exist yet --
// console.log is the only I/O surface in this pass.

#include <GS.h>
#include <quickjs.h>

#include <fstream>
#include <sstream>
#include <string>

class ScriptEngine
{
public:
	struct ScriptResult
	{
		bool Ok = false;
		std::string Output;
		std::string Error;
	};

	~ScriptEngine()
	{
		if (m_Context)
			JS_FreeContext(m_Context);
		if (m_Runtime)
			JS_FreeRuntime(m_Runtime);
	}

	ScriptResult RunScript(const std::string& tsSource)
	{
		if (!m_Ready && !Initialize())
		{
			ScriptResult failure;
			failure.Error = "Script engine failed to initialize (see log).";
			return failure;
		}

		m_CapturedOutput.clear();

		JSValue driverFn = JS_Eval(m_Context, s_DriverSrc, strlen(s_DriverSrc), "driver", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(driverFn))
		{
			ScriptResult failure;
			failure.Error = "Internal error: driver script failed: " + DescribeException();
			JS_FreeValue(m_Context, driverFn);
			return failure;
		}

		JSValue srcArg = JS_NewString(m_Context, tsSource.c_str());
		JSValue transpileResult = JS_Call(m_Context, driverFn, JS_UNDEFINED, 1, &srcArg);
		JS_FreeValue(m_Context, srcArg);
		JS_FreeValue(m_Context, driverFn);

		if (JS_IsException(transpileResult))
		{
			ScriptResult failure;
			failure.Error = "Internal error: transpile call failed: " + DescribeException();
			JS_FreeValue(m_Context, transpileResult);
			return failure;
		}

		JSValue okVal = JS_GetPropertyStr(m_Context, transpileResult, "ok");
		bool transpileOk = JS_ToBool(m_Context, okVal);
		JS_FreeValue(m_Context, okVal);

		if (!transpileOk)
		{
			ScriptResult failure;
			failure.Error = GetStringProperty(transpileResult, "error");
			JS_FreeValue(m_Context, transpileResult);
			return failure;
		}

		std::string jsCode = GetStringProperty(transpileResult, "code");
		JS_FreeValue(m_Context, transpileResult);

		JSValue runResult = JS_Eval(m_Context, jsCode.c_str(), jsCode.size(), "script", JS_EVAL_TYPE_GLOBAL);

		ScriptResult result;
		if (JS_IsException(runResult))
		{
			result.Ok = false;
			result.Error = DescribeException();
		}
		else
		{
			result.Ok = true;
		}
		result.Output = m_CapturedOutput;
		JS_FreeValue(m_Context, runResult);
		return result;
	}

private:
	bool Initialize()
	{
		m_Runtime = JS_NewRuntime();
		if (!m_Runtime)
			return false;
		JS_SetMemoryLimit(m_Runtime, (size_t)512 * 1024 * 1024);
		JS_SetRuntimeOpaque(m_Runtime, this);

		m_Context = JS_NewContext(m_Runtime);
		if (!m_Context)
			return false;

		JSValue global = JS_GetGlobalObject(m_Context);
		JSValue consoleObj = JS_NewObject(m_Context);
		JS_SetPropertyStr(m_Context, consoleObj, "log", JS_NewCFunction(m_Context, NativeConsoleLog, "log", 1));
		JS_SetPropertyStr(m_Context, global, "console", consoleObj);
		JS_FreeValue(m_Context, global);

		std::ifstream tsFile("assets/typescript.js", std::ios::in | std::ios::binary);
		if (!tsFile.is_open())
		{
			GS_ERROR("ScriptEngine: could not open assets/typescript.js");
			return false;
		}
		std::stringstream tsBuffer;
		tsBuffer << tsFile.rdbuf();
		std::string tsSource = tsBuffer.str();

		JSValue loadResult = JS_Eval(m_Context, tsSource.c_str(), tsSource.size(), "typescript.js", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(loadResult))
		{
			GS_ERROR("ScriptEngine: failed to load typescript.js: {0}", DescribeException());
			JS_FreeValue(m_Context, loadResult);
			return false;
		}
		JS_FreeValue(m_Context, loadResult);

		m_Ready = true;
		return true;
	}

	static JSValue NativeConsoleLog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		for (int i = 0; i < argc; i++)
		{
			if (i > 0)
				self->m_CapturedOutput += " ";
			const char* s = JS_ToCString(ctx, argv[i]);
			if (s)
				self->m_CapturedOutput += s;
			JS_FreeCString(ctx, s);
		}
		self->m_CapturedOutput += "\n";
		return JS_UNDEFINED;
	}

	std::string GetStringProperty(JSValue obj, const char* name)
	{
		JSValue prop = JS_GetPropertyStr(m_Context, obj, name);
		const char* s = JS_ToCString(m_Context, prop);
		std::string result = s ? s : "";
		JS_FreeCString(m_Context, s);
		JS_FreeValue(m_Context, prop);
		return result;
	}

	std::string DescribeException()
	{
		JSValue exc = JS_GetException(m_Context);
		std::string message = "(unknown error)";
		const char* s = JS_ToCString(m_Context, exc);
		if (s)
			message = s;
		JS_FreeCString(m_Context, s);
		JS_FreeValue(m_Context, exc);
		return message;
	}

	static constexpr const char* s_DriverSrc = R"JS(
		(function(source) {
			var result = ts.transpileModule(source, {
				compilerOptions: {
					module: ts.ModuleKind.None,
					target: ts.ScriptTarget.ES2020
				},
				reportDiagnostics: true
			});
			if (result.diagnostics && result.diagnostics.length > 0) {
				var messages = result.diagnostics.map(function(d) {
					return ts.flattenDiagnosticMessageText(d.messageText, '\n');
				});
				return { ok: false, error: messages.join('\n'), code: null };
			}
			return { ok: true, error: null, code: result.outputText };
		})
	)JS";

	JSRuntime* m_Runtime = nullptr;
	JSContext* m_Context = nullptr;
	bool m_Ready = false;
	std::string m_CapturedOutput;
};
```

There is no shared `GS::FileSystem`-style helper in this codebase (checked: `OpenGLShader::ReadFile` is a private method on that one class, not reusable) — the `std::ifstream`/`std::stringstream` reading shown above is deliberate and matches that same class's own approach, reading `"assets/typescript.js"` as a path relative to the executable's working directory, same convention every other asset in this repo already uses.

- [ ] **Step 3: Write the temporary self-test**

Create `TestEnv/src/ScriptEngineTest.h`:

```cpp
// TEMPORARY -- delete after verifying ScriptEngine (Task 2 of the
// scripting-runtime plan).
#pragma once
#include <GS.h>
#include "ScriptEngine.h"

namespace ScriptEngineTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        ScriptEngine engine;

        const char* goodScript =
            "function greet(name: string): string {\n"
            "    let count: number = 3;\n"
            "    return `Hello, ${name}! (${count})`;\n"
            "}\n"
            "console.log(greet('world'));\n";

        ScriptEngine::ScriptResult result1 = engine.RunScript(goodScript);
        Check(result1.Ok, "a valid typed script runs without error");
        Check(result1.Output == "Hello, world! (3)\n", "console.log output matches exactly (got: '" + result1.Output + "')");

        const char* throwingScript = "throw new Error('deliberate failure');";
        ScriptEngine::ScriptResult result2 = engine.RunScript(throwingScript);
        Check(!result2.Ok, "a script that throws is reported as not ok");
        Check(result2.Error.find("deliberate failure") != std::string::npos,
            "the captured error mentions the thrown message (got: '" + result2.Error + "')");

        // The engine is reused (one persistent JS context) -- confirm a
        // later, valid script still runs fine after an earlier failure.
        ScriptEngine::ScriptResult result3 = engine.RunScript("console.log('still alive');");
        Check(result3.Ok, "the engine still works after a prior script threw");
        Check(result3.Output == "still alive\n", "output after recovery matches exactly");
    }
}
```

- [ ] **Step 4: Call it, build, and run**

Temporarily add `#include "ScriptEngineTest.h"` and `ScriptEngineTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 6 `[ok ]` lines, `g_Pass == 6`, `g_Fail == 0`. Note this must run from the binary's own working directory (`bin/Debug-linux-x86_64/TestEnv/`) for the relative asset path to resolve — `./gs.py run` already handles this.

If `typescript.js` fails to load (check the log for `ScriptEngine: failed to load typescript.js`), confirm `TestEnv/assets/typescript.js` actually got copied next to the binary by the existing postbuild asset-copy step (`ls bin/Debug-linux-x86_64/TestEnv/assets/typescript.js`) — if it's missing there, the file wasn't in `TestEnv/assets/` before the build ran, or the build didn't re-run the copy step; re-run `./gs.py build` and check again before assuming a code bug.

- [ ] **Step 5: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/ScriptEngineTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/assets/typescript.js TestEnv/src/ScriptEngine.h
git status --short
```

---

### Task 3: Wire into the editor — "Run" button and "Build Output" panel

**Files:**
- Modify: `TestEnv/src/TextEditorPanel.h` (add a "Run" button to the file bar)
- Modify: `TestEnv/src/EditorShell.h` (replace the "Build Output" stub; own the `ScriptEngine` instance)
- Modify: `docs/STATE.md`

**Interfaces:**
- Consumes: `ScriptEngine`/`ScriptEngine::ScriptResult`/`RunScript` (Task 2), `TextBuffer`'s existing interface (already in the repo) to read the current buffer's full text for the Run button to act on.
- Produces: nothing further — this is the last task.

`TextBuffer` (already in the repo, from the text editor plan) has no "get the whole buffer as one string" method yet — it only exposes `LineCount()`/`Line(int)`. Read `TestEnv/src/TextBuffer.h` and add one small accessor rather than reaching into its internals from `TextEditorPanel.h`:

- [ ] **Step 1: Add a whole-buffer accessor to `TextBuffer`**

Add this public method to `TestEnv/src/TextBuffer.h`'s `class TextBuffer`, near `LineCount()`/`Line()`:

```cpp
	std::string FullText() const
	{
		std::string result;
		for (size_t i = 0; i < m_Lines.size(); i++)
		{
			result += m_Lines[i];
			if (i + 1 < m_Lines.size())
				result += '\n';
		}
		return result;
	}
```

(Matches `SaveToFile`'s own line-joining convention exactly, minus writing to a stream.)

- [ ] **Step 2: Add a "Run" button and output storage to `TextEditorPanel`**

In `TestEnv/src/TextEditorPanel.h`, add `#include "ScriptEngine.h"` to the include block. Add two new members near the existing ones:

```cpp
	ScriptEngine* m_ScriptEngine = nullptr; // not owned -- set by EditorShell
	std::string m_LastRunOutput;
	std::string m_LastRunError;
```

Add a public setter (called once by `EditorShell` when it constructs both panels):

```cpp
	void SetScriptEngine(ScriptEngine* engine) { m_ScriptEngine = engine; }
```

In `DrawFileBar()`, add a "Run" button after the existing Open/Save buttons:

```cpp
		ImGui::SameLine();
		if (ImGui::Button("Run") && m_ScriptEngine)
		{
			ScriptEngine::ScriptResult result = m_ScriptEngine->RunScript(m_Buffer.FullText());
			m_LastRunOutput = result.Output;
			m_LastRunError = result.Ok ? "" : result.Error;
		}
```

Add public getters so `EditorShell` can display these in the "Build Output" panel:

```cpp
	const std::string& LastRunOutput() const { return m_LastRunOutput; }
	const std::string& LastRunError() const { return m_LastRunError; }
```

- [ ] **Step 3: Wire `ScriptEngine` and the "Build Output" panel into `EditorShell.h`**

Add `#include "ScriptEngine.h"` to `EditorShell.h`'s include block. Add a member (alongside `m_Terminal`/`m_TextEditor`):

```cpp
	ScriptEngine m_ScriptEngine;
```

Confirmed: `EditorShell::OnAttach()` already exists (currently sets `g_EditorShellInstance = this;` as its first line, then handles `--no-editor`). Add the wiring call as the new first line, before `g_EditorShellInstance = this;`:

```cpp
	void OnAttach() override
	{
		m_TextEditor.SetScriptEngine(&m_ScriptEngine);

		g_EditorShellInstance = this;
		...
```

(Both `m_TextEditor` and `m_ScriptEngine` are already-constructed members by the time `OnAttach()` runs, so this is safe regardless of where it's placed within the function — placed first purely for readability, not because ordering matters here.)

Find the existing "Build Output" stub:

```cpp
		ImGui::Begin("Build Output");
		ImGui::TextDisabled("Not built yet.");
		ImGui::End();
```

Replace with:

```cpp
		ImGui::Begin("Build Output");
		if (!m_TextEditor.LastRunError().empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_TextEditor.LastRunError().c_str());
		}
		else if (!m_TextEditor.LastRunOutput().empty())
		{
			ImGui::TextUnformatted(m_TextEditor.LastRunOutput().c_str());
		}
		else
		{
			ImGui::TextDisabled("Run a script from the Editor tab to see its output here.");
		}
		ImGui::End();
```

- [ ] **Step 4: Build all three configs and verify no existing demo is affected**

```bash
./gs.py build all
```

Expected: exit 0, zero "error" occurrences, Debug/Release/Dist.

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/scripting_hideui.png --capture-step 5
```

Compare against a pre-this-task capture — must be byte-identical. `ScriptEngine` is only ever driven by clicking "Run" inside `EditorShell::OnImGuiRender`, which `--hide-ui` prevents from ever running.

- [ ] **Step 5: Manual verification (disclosed, not faked)**

```bash
./gs.py run
```

Open the Editor tab, type a small TypeScript snippet (e.g. the one from Task 2's self-test), click "Run", and check the "Build Output" tab shows the expected console output. Try a deliberately broken script too, and confirm the error shows in red. This cannot be driven headlessly in this environment (no GUI-automation tool) — report the actual result of clicking through it, not an assumption, same disclosed-limitation pattern as every other UI-interaction check in this editor pivot.

- [ ] **Step 6: Clean up every temporary test**

```bash
git grep TEMPORARY
```

Expected: no output.

- [ ] **Step 7: Update `docs/STATE.md`**

Add a "Last landed" bullet ahead of the text-editor entry, in the same style as every prior sub-project's landing entry (name the new files, the vendored QuickJS + TypeScript bundle, the self-test counts, the byte-identical `--hide-ui` capture, and the manual-check result from Step 5). Update "What is next" to drop the scripting-runtime item (it was the only remaining numbered item per the file's current state — check what's actually left before rewriting this section, since this plan's own execution may not be the last word on what the file currently says).

- [ ] **Step 8: Stage**

```bash
git add TestEnv/src/TextBuffer.h TestEnv/src/TextEditorPanel.h TestEnv/src/EditorShell.h docs/STATE.md
git status --short
```

---

## Self-Review

**Spec coverage:** QuickJS vendored, `quickjs-libc.c` deliberately excluded for sandboxing (Task 1) ✓. TypeScript's real compiler bundle vendored as a checked-in asset with refresh instructions (Task 2) ✓. `ScriptEngine` transpiles via the exact spike-verified driver shape, captures console output and errors, persists one JS context across runs (Task 2) ✓. "Run" button in the text editor, output routed to the existing "Build Output" stub (Task 3) ✓. No engine/scene bindings anywhere in this plan ✓. Deterministic self-tests for both the QuickJS smoke test and `ScriptEngine`'s actual transpile-and-run behavior, including an error path (Task 1 Step 4, Task 2 Step 3) ✓. Manual click-through disclosed honestly rather than faked (Task 3 Step 5) ✓.

**Placeholder scan:** `ScriptEngine.h`'s `GS::FileSystem::ReadFile` call is explicitly flagged as a placeholder name needing real verification before use — this is a deliberate, actionable instruction (grep for the real utility, or fall back to `TextBuffer::LoadFromFile`'s own known-working `std::ifstream` approach), not a vague "add file reading" left unspecified. No other placeholders.

**Type consistency:** `ScriptEngine::ScriptResult{Ok, Output, Error}` and `RunScript(const std::string&)` are used identically in Task 2's self-test and Task 3's `TextEditorPanel::DrawFileBar()`. `TextBuffer::FullText()` (added in Task 3) matches `SaveToFile`'s own line-joining logic exactly, so a script's line-ending behavior is consistent between what gets saved to disk and what gets run.
