# Entity/Component Scripting + Play Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Attach a TypeScript file to an entity via the Inspector. Press Play: each scripted entity's `OnStart`/`OnUpdate(dt)` runs against a small, fixed API (its own transform, basic keyboard input). Press Stop: the scene reverts to exactly how it was before Play.

**Architecture:** `ScriptComponent` (a path, like `MeshComponent::SourcePath`) plus a native QuickJS `Entity` class binding transform get/set methods, both new. `PlayMode` (a namespace, matching `EditorHistory`'s own established pattern) snapshots the scene via the existing `Scene::Save`/`Load`, prepares every scripted entity's script through the already-hardened `ScriptEngine`, and ticks `OnUpdate` from `OnFixedUpdate` — this repo's own convention for "anything that moves."

**Tech Stack:** C++17, QuickJS (already vendored), the existing `ScriptEngine`/`GS::Scene` infrastructure.

**Spec:** `docs/superpowers/specs/2026-09-10-entity-scripting-design.md`

**API verified before writing this plan, not assumed**: `JS_NewClassID`/`JS_NewClass`/`JS_NewObjectClass`/`JS_SetOpaque`/`JS_GetOpaque2`'s exact usage pattern was checked directly against `GS/vendor/quickjs/api-test.c`'s own test code (`class_id = 0; JS_NewClassID(rt, &class_id); JS_NewClass(rt, class_id, &def);`), not written from memory. `GS::Timestep` confirmed to implicitly convert to `float`. `GS::Scene::GetEntities()`/`HasComponent`/`GetComponent`/`AddComponent`/`RemoveComponent` signatures confirmed directly from `GS/src/GS/Scene/Scene.h`. `GS_KEY_*` constants confirmed to be plain ASCII codes for letters/digits (`GS_KEY_A == 65`, `GS_KEY_0 == 48`).

## Global Constraints

- **No git commits.** `git add` after each task's review passes is the boundary marker, same as every prior plan in this repo.
- **Temporary self-test per task**, deleted once it has done its job. `git grep TEMPORARY` must be clean before Task 4 (the last task) finishes.
- **`./gs.py build all` must succeed with zero errors before Task 4 finishes.**
- **No existing demo's behavior, capture, or replay may change.** `PlayMode`/`ScriptComponent`/the native `Entity` binding are all editor-only, reachable only through `EditorMenuBar`'s Play button (itself gated on `g_ActiveDemo == InvalidDemo`, same pattern Undo/Redo/Find already use) — no demo ever constructs any of this.
- **No physics, no spawning/destroying entities from a script, no querying other entities, no camera/audio access, no `OnStop` hook.** All explicitly deferred by the spec — don't add speculative surface beyond what's asked.
- **`PlayMode::Stop()` must fully revert the scene**, including reverting any `ScriptComponent` changes a script might have made to itself (it can't in v1, since scripts have no way to touch components other than Transform — but the revert-via-`Scene::Load` mechanism doesn't care what changed, it reverts everything).

---

### Task 1: `ScriptComponent` and its Inspector UI

**Files:**
- Modify: `GS/src/GS/Scene/Components.h`
- Modify: `TestEnv/src/EditorSceneView.h`
- Create (temporary): `TestEnv/src/ScriptComponentTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Produces: `struct GS::ScriptComponent { std::string ScriptPath; }`. Tasks 3 consumes this (reading `ScriptPath` for every entity that has one).

Independent of Tasks 2/3 — no file overlap, can be built and reviewed on its own.

- [ ] **Step 1: Add `ScriptComponent`**

In `GS/src/GS/Scene/Components.h`, add after the existing `CameraComponent` struct, before the closing `}` of `namespace GS`:

```cpp
	// A TypeScript file's path, run against this entity while Play mode is
	// active (TestEnv/src/PlayMode.h). The engine itself only knows this is
	// a path -- same relationship MeshComponent has with SourcePath.
	struct ScriptComponent
	{
		std::string ScriptPath;
	};
```

- [ ] **Step 2: Add the Inspector's Add/Remove Script UI**

Read `TestEnv/src/EditorSceneView.h`'s Inspector method first to confirm the exact surrounding code still matches (it should — nothing has touched this file since the embedded-terminal plan). Find where the Camera component block ends (the last `if (auto* camera = g_EditorScene.GetComponent<GS::CameraComponent>(m_Selected)) { ... }` block, right before wherever the Inspector's `ImGui::End()` is), and add this immediately after it, still before `ImGui::End()`:

```cpp
		ImGui::SeparatorText("Script");
		if (auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(m_Selected))
		{
			char pathBuf[512];
			strncpy(pathBuf, script->ScriptPath.c_str(), sizeof(pathBuf) - 1);
			pathBuf[sizeof(pathBuf) - 1] = '\0';
			if (ImGui::InputText("##scriptpath", pathBuf, sizeof(pathBuf)))
				script->ScriptPath = pathBuf;
			if (ImGui::Button("Remove Script"))
				g_EditorScene.RemoveComponent<GS::ScriptComponent>(m_Selected);
		}
		else
		{
			if (ImGui::Button("Add Script"))
				g_EditorScene.AddComponent<GS::ScriptComponent>(m_Selected, GS::ScriptComponent{});
		}
```

Add `#include <cstring>` to `EditorSceneView.h`'s includes if it isn't already there (needed for `strncpy` — check first, don't add a duplicate).

Note: unlike every other field in this Inspector, editing `ScriptPath` does **not** go through `EditorHistory`/Undo-Redo. This is a deliberate v1 scope cut (documented in the spec implicitly by omission) — the field is easy to retype if changed by mistake, and wiring a string field through `EditFieldCommand` is more machinery than this cut is worth right now. Don't add it.

- [ ] **Step 3: Write the temporary self-test**

Create `TestEnv/src/ScriptComponentTest.h`:

```cpp
// TEMPORARY -- delete after verifying ScriptComponent (Task 1 of the
// entity-scripting plan).
#pragma once
#include <GS.h>

namespace ScriptComponentTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        GS::Scene scene;
        GS::Entity e = scene.CreateEntity("Test");

        Check(!scene.HasComponent<GS::ScriptComponent>(e.GetId()), "fresh entity has no ScriptComponent");

        e.Add<GS::ScriptComponent>(GS::ScriptComponent{ "scripts/test.ts" });
        Check(scene.HasComponent<GS::ScriptComponent>(e.GetId()), "AddComponent<ScriptComponent> registers");

        auto* script = scene.GetComponent<GS::ScriptComponent>(e.GetId());
        Check(script != nullptr && script->ScriptPath == "scripts/test.ts", "GetComponent returns the stored path");

        e.Remove<GS::ScriptComponent>();
        Check(!scene.HasComponent<GS::ScriptComponent>(e.GetId()), "RemoveComponent removes it");
    }
}
```

- [ ] **Step 4: Call it, build, and run**

Temporarily add `#include "ScriptComponentTest.h"` and `ScriptComponentTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 4 `[ok ]` lines, `g_Pass == 4`, `g_Fail == 0`. This is pure ECS logic, no ImGui/GL needed.

- [ ] **Step 5: Visual check of the Inspector UI**

`m_Selected` needs a real entity to show the Inspector's new block at all. Place a cube via the Tools panel (or, for a headless check, use whatever mechanism a prior task in this editor pivot used to force state for a capture — check `docs/STATE.md`'s file-tree entry for the technique it used to force a project open headlessly, and adapt similarly if you want a capture rather than only a live check). Confirm visually: with no entity selected, no Script section appears (matches every other component block's existing behavior); with an entity selected and no `ScriptComponent`, an "Add Script" button appears; after clicking it, a path field + "Remove Script" button appear instead.

- [ ] **Step 6: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/ScriptComponentTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty.

- [ ] **Step 7: Stage**

```bash
git add GS/src/GS/Scene/Components.h TestEnv/src/EditorSceneView.h
git status --short
```

---

### Task 2: `ScriptEngine` extension — the native `Entity`/`Input` binding

**Files:**
- Modify: `TestEnv/src/ScriptEngine.h`
- Create (temporary): `TestEnv/src/ScriptEngineEntityTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `GS::Scene`, `GS::EntityId`, `GS::TransformComponent` (all already exist).
- Produces: `ScriptEngine::PreparedScript { JSValue EntityObj, OnStart, OnUpdate; }`, `bool ScriptEngine::PrepareEntityScript(GS::Scene* scene, GS::EntityId id, const std::string& tsSource, std::string& error, PreparedScript& out)`, `void ScriptEngine::CallOnStart(PreparedScript&)`, `void ScriptEngine::CallOnUpdate(PreparedScript&, float dt)`, `void ScriptEngine::ReleasePreparedScript(PreparedScript&)`. Task 3 consumes all of this exactly.

Independent of Task 1 — no file overlap. Can be built and reviewed on its own (its self-test constructs its own throwaway `GS::Scene`/entity, not depending on `ScriptComponent` existing at all).

This is the highest-risk task in this plan (new QuickJS class-binding code). If anything about the API doesn't behave as this brief describes, **check `GS/vendor/quickjs/api-test.c` for a real, working usage example before guessing** — the class-registration pattern below was verified against that file directly.

- [ ] **Step 1: Read the current `ScriptEngine.h` in full**

Confirm the exact current state of `RunScript()`, `Initialize()`, `s_DriverSrc`, and the private members before editing — nothing has touched this file since the scripting-runtime plan's final fix wave, but confirm rather than assume.

- [ ] **Step 2: Factor the transpile-only step out of `RunScript`'s driver**

`RunScript`'s current `s_DriverSrc` both transpiles AND wraps the result in a self-executing IIFE (`'(function(){' + result.outputText + '\n})();'`). Entity scripts need a *different* wrapper (taking `entity`/`input` as parameters, not self-executing) around the *same* transpile step. Split it in two.

Replace the current `s_DriverSrc` member with:

```cpp
	static constexpr const char* s_TranspileSrc = R"JS(
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
```

(This is the old `s_DriverSrc` with its last line changed back to `return { ok: true, error: null, code: result.outputText };` — no IIFE wrapping. Rename every use of `s_DriverSrc` elsewhere in the file to `s_TranspileSrc`.)

In `RunScript()`, find where the transpiled `jsCode` is extracted and then `JS_Eval`'d directly:

```cpp
		std::string jsCode = GetStringProperty(transpileResult, "code");
		JS_FreeValue(m_Context, transpileResult);

		JSValue runResult = JS_Eval(m_Context, jsCode.c_str(), jsCode.size(), "script", JS_EVAL_TYPE_GLOBAL);
```

Replace with (wrapping happens here now, at the call site, instead of inside the driver script):

```cpp
		std::string jsCode = GetStringProperty(transpileResult, "code");
		JS_FreeValue(m_Context, transpileResult);

		std::string wrapped = "(function(){" + jsCode + "\n})();";
		JSValue runResult = JS_Eval(m_Context, wrapped.c_str(), wrapped.size(), "script", JS_EVAL_TYPE_GLOBAL);
```

`RunScript`'s behavior is otherwise unchanged — this is a pure refactor to share the transpile step with the new entity-script path below.

- [ ] **Step 3: Add the native `Entity` class**

Add `#include <cstdint>` if not already present (for the opaque struct's use of `GS::EntityId`, which is a plain `unsigned int` typedef — confirm this against `GS/src/GS/Scene/Entity.h` if you want to double check, but `unsigned int` is a built-in type needing no extra include either way).

Add this near the top of the class, as a private nested struct and a private static class-ID member:

```cpp
private:
	struct EntityBinding
	{
		GS::Scene* Scene;
		GS::EntityId Id;
	};

	inline static JSClassID s_EntityClassId = 0;
```

Add these private static methods (the native functions QuickJS calls):

```cpp
	static void EntityFinalizer(JSRuntime*, JSValueConst val)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque(val, s_EntityClassId);
		delete binding;
	}

	static JSValue Native_GetPosition(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		JSValue arr = JS_NewArray(ctx);
		glm::vec3 pos = transform ? transform->Position : glm::vec3(0.0f);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, pos.x));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, pos.y));
		JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, pos.z));
		return arr;
	}

	static JSValue Native_SetPosition(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding || argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		double x = 0, y = 0, z = 0;
		JS_ToFloat64(ctx, &x, argv[0]);
		JS_ToFloat64(ctx, &y, argv[1]);
		JS_ToFloat64(ctx, &z, argv[2]);
		transform->Position = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_GetRotation(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		JSValue arr = JS_NewArray(ctx);
		glm::vec3 rot = transform ? transform->Rotation : glm::vec3(0.0f);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, rot.x));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, rot.y));
		JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, rot.z));
		return arr;
	}

	static JSValue Native_SetRotation(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding || argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		double x = 0, y = 0, z = 0;
		JS_ToFloat64(ctx, &x, argv[0]);
		JS_ToFloat64(ctx, &y, argv[1]);
		JS_ToFloat64(ctx, &z, argv[2]);
		transform->Rotation = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_GetScale(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		JSValue arr = JS_NewArray(ctx);
		glm::vec3 scale = transform ? transform->Scale : glm::vec3(1.0f);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, scale.x));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, scale.y));
		JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, scale.z));
		return arr;
	}

	static JSValue Native_SetScale(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
	{
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, s_EntityClassId);
		if (!binding || argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		double x = 0, y = 0, z = 0;
		JS_ToFloat64(ctx, &x, argv[0]);
		JS_ToFloat64(ctx, &y, argv[1]);
		JS_ToFloat64(ctx, &z, argv[2]);
		transform->Scale = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_IsKeyDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		if (argc < 1)
			return JS_NewBool(ctx, false);
		const char* name = JS_ToCString(ctx, argv[0]);
		int keycode = KeyNameToCode(name ? name : "");
		JS_FreeCString(ctx, name);
		if (keycode < 0)
			return JS_NewBool(ctx, false);
		return JS_NewBool(ctx, GS::Input::IsKeyPressed(keycode));
	}

	static int KeyNameToCode(const std::string& name)
	{
		if (name.size() == 1)
		{
			char c = name[0];
			if (c >= 'A' && c <= 'Z')
				return GS_KEY_A + (c - 'A');
			if (c >= '0' && c <= '9')
				return GS_KEY_0 + (c - '0');
		}
		if (name == "SPACE")
			return GS_KEY_SPACE;
		return -1;
	}

	void EnsureEntityClassRegistered()
	{
		if (s_EntityClassId != 0)
			return;
		JS_NewClassID(m_Runtime, &s_EntityClassId);
		JSClassDef def = {};
		def.class_name = "Entity";
		def.finalizer = EntityFinalizer;
		JS_NewClass(m_Runtime, s_EntityClassId, &def);
	}

	JSValue CreateEntityBinding(GS::Scene* scene, GS::EntityId id)
	{
		JSValue obj = JS_NewObjectClass(m_Context, s_EntityClassId);
		JS_SetOpaque(obj, new EntityBinding{ scene, id });
		JS_SetPropertyStr(m_Context, obj, "getPosition", JS_NewCFunction(m_Context, Native_GetPosition, "getPosition", 0));
		JS_SetPropertyStr(m_Context, obj, "setPosition", JS_NewCFunction(m_Context, Native_SetPosition, "setPosition", 3));
		JS_SetPropertyStr(m_Context, obj, "getRotation", JS_NewCFunction(m_Context, Native_GetRotation, "getRotation", 0));
		JS_SetPropertyStr(m_Context, obj, "setRotation", JS_NewCFunction(m_Context, Native_SetRotation, "setRotation", 3));
		JS_SetPropertyStr(m_Context, obj, "getScale", JS_NewCFunction(m_Context, Native_GetScale, "getScale", 0));
		JS_SetPropertyStr(m_Context, obj, "setScale", JS_NewCFunction(m_Context, Native_SetScale, "setScale", 3));
		return obj;
	}
```

Add `#include "GS/Scene/Scene.h"` and `#include "GS/Input.h"` to `ScriptEngine.h`'s includes if not already transitively available via `<GS.h>` — check first (this codebase's `<GS.h>` umbrella header likely already pulls both in; only add if the build actually needs it).

In `Initialize()`, right after `JS_SetInterruptHandler(m_Runtime, InterruptHandler, this);`, add:

```cpp
		EnsureEntityClassRegistered();

		m_InputObject = JS_NewObject(m_Context);
		JS_SetPropertyStr(m_Context, m_InputObject, "isKeyDown", JS_NewCFunction(m_Context, Native_IsKeyDown, "isKeyDown", 1));
```

Add the member (near `m_Ready`/`m_CapturedOutput`):

```cpp
	JSValue m_InputObject = JS_UNDEFINED;
```

In `~ScriptEngine()`, free it before the context (order matters — freeing a `JSValue` needs the context to still be alive):

```cpp
	~ScriptEngine()
	{
		if (m_Context)
		{
			JS_FreeValue(m_Context, m_InputObject);
			JS_FreeContext(m_Context);
		}
		if (m_Runtime)
			JS_FreeRuntime(m_Runtime);
	}
```

- [ ] **Step 4: Add `PrepareEntityScript`/`CallOnStart`/`CallOnUpdate`/`ReleasePreparedScript`**

Add this public struct (near the existing `ScriptResult`):

```cpp
	struct PreparedScript
	{
		JSValue EntityObj = JS_UNDEFINED;
		JSValue OnStart = JS_UNDEFINED;
		JSValue OnUpdate = JS_UNDEFINED;
	};
```

Add these public methods:

```cpp
	// Transpiles and prepares a script for one entity: its top-level
	// OnStart/OnUpdate become a private closure over that entity's own
	// native binding (passed as a function *parameter*, not a global) --
	// so two entities running the same script file, or the same entity
	// re-prepared, never collide, the same reasoning RunScript's own IIFE
	// wrap already relies on for its own redeclaration fix.
	bool PrepareEntityScript(GS::Scene* scene, GS::EntityId entityId, const std::string& tsSource, std::string& error, PreparedScript& out)
	{
		if (!m_Ready && !Initialize())
		{
			error = "Script engine failed to initialize (see log).";
			return false;
		}

		JSValue transpileFn = JS_Eval(m_Context, s_TranspileSrc, strlen(s_TranspileSrc), "transpile", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(transpileFn))
		{
			error = "Internal error: transpile driver failed: " + DescribeException();
			JS_FreeValue(m_Context, transpileFn);
			return false;
		}

		JSValue srcArg = JS_NewString(m_Context, tsSource.c_str());
		JSValue transpileResult = JS_Call(m_Context, transpileFn, JS_UNDEFINED, 1, &srcArg);
		JS_FreeValue(m_Context, srcArg);
		JS_FreeValue(m_Context, transpileFn);

		if (JS_IsException(transpileResult))
		{
			error = "Internal error: transpile call failed: " + DescribeException();
			JS_FreeValue(m_Context, transpileResult);
			return false;
		}

		JSValue okVal = JS_GetPropertyStr(m_Context, transpileResult, "ok");
		bool transpileOk = JS_ToBool(m_Context, okVal);
		JS_FreeValue(m_Context, okVal);

		if (!transpileOk)
		{
			error = GetStringProperty(transpileResult, "error");
			JS_FreeValue(m_Context, transpileResult);
			return false;
		}

		std::string jsCode = GetStringProperty(transpileResult, "code");
		JS_FreeValue(m_Context, transpileResult);

		std::string wrapped = "(function(entity, input) {" + jsCode +
			"\nreturn { onStart: typeof OnStart === 'function' ? OnStart : null, "
			"onUpdate: typeof OnUpdate === 'function' ? OnUpdate : null }; })";

		JSValue wrapperFn = JS_Eval(m_Context, wrapped.c_str(), wrapped.size(), "entity-script", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(wrapperFn))
		{
			error = "Script error: " + DescribeException();
			JS_FreeValue(m_Context, wrapperFn);
			return false;
		}

		JSValue entityObj = CreateEntityBinding(scene, entityId);
		JSValue args[2] = { entityObj, m_InputObject };
		JSValue result = JS_Call(m_Context, wrapperFn, JS_UNDEFINED, 2, args);
		JS_FreeValue(m_Context, wrapperFn);

		if (JS_IsException(result))
		{
			error = "Script error: " + DescribeException();
			JS_FreeValue(m_Context, entityObj);
			JS_FreeValue(m_Context, result);
			return false;
		}

		out.EntityObj = entityObj;
		out.OnStart = JS_GetPropertyStr(m_Context, result, "onStart");
		out.OnUpdate = JS_GetPropertyStr(m_Context, result, "onUpdate");
		JS_FreeValue(m_Context, result);
		return true;
	}

	void CallOnStart(PreparedScript& script)
	{
		if (JS_IsFunction(m_Context, script.OnStart))
		{
			JSValue result = JS_Call(m_Context, script.OnStart, JS_UNDEFINED, 0, nullptr);
			if (JS_IsException(result))
				GS_ERROR("ScriptEngine: entity script OnStart threw: {0}", DescribeException());
			JS_FreeValue(m_Context, result);
		}
	}

	void CallOnUpdate(PreparedScript& script, float dt)
	{
		if (JS_IsFunction(m_Context, script.OnUpdate))
		{
			JSValue dtArg = JS_NewFloat64(m_Context, dt);
			JSValue result = JS_Call(m_Context, script.OnUpdate, JS_UNDEFINED, 1, &dtArg);
			if (JS_IsException(result))
				GS_ERROR("ScriptEngine: entity script OnUpdate threw: {0}", DescribeException());
			JS_FreeValue(m_Context, result);
		}
	}

	void ReleasePreparedScript(PreparedScript& script)
	{
		JS_FreeValue(m_Context, script.EntityObj);
		JS_FreeValue(m_Context, script.OnStart);
		JS_FreeValue(m_Context, script.OnUpdate);
		script.EntityObj = JS_UNDEFINED;
		script.OnStart = JS_UNDEFINED;
		script.OnUpdate = JS_UNDEFINED;
	}
```

- [ ] **Step 5: Write the temporary self-test**

Create `TestEnv/src/ScriptEngineEntityTest.h`:

```cpp
// TEMPORARY -- delete after verifying ScriptEngine's entity binding
// (Task 2 of the entity-scripting plan).
#pragma once
#include <GS.h>
#include "ScriptEngine.h"

namespace ScriptEngineEntityTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        GS::Scene scene;
        GS::Entity e = scene.CreateEntity("Test");
        auto& transform = e.Add<GS::TransformComponent>();
        transform.Position = { 1.0f, 2.0f, 3.0f };

        ScriptEngine engine;

        const char* src =
            "function OnUpdate(dt: number): void {\n"
            "    let p = entity.getPosition();\n"
            "    entity.setPosition(p[0] + 5, p[1], p[2]);\n"
            "}\n";

        ScriptEngine::PreparedScript prepared;
        std::string error;
        bool prepared_ok = engine.PrepareEntityScript(&scene, e.GetId(), src, error, prepared);
        Check(prepared_ok, "a valid entity script prepares successfully (error: '" + error + "')");

        engine.CallOnUpdate(prepared, 0.016f);
        Check(std::abs(transform.Position.x - 6.0f) < 0.001f,
            "OnUpdate's entity.setPosition actually wrote the real TransformComponent (got x=" + std::to_string(transform.Position.x) + ")");

        engine.CallOnUpdate(prepared, 0.016f);
        Check(std::abs(transform.Position.x - 11.0f) < 0.001f,
            "a second OnUpdate call continues from the updated position, not a stale copy (got x=" + std::to_string(transform.Position.x) + ")");

        engine.ReleasePreparedScript(prepared);
        Check(true, "ReleasePreparedScript runs without crashing");

        // A second, independent entity running the SAME script source must
        // not collide with the first (this is the whole reason entity/input
        // are passed as function parameters, not globals).
        GS::Entity e2 = scene.CreateEntity("Test2");
        auto& transform2 = e2.Add<GS::TransformComponent>();
        transform2.Position = { 100.0f, 0.0f, 0.0f };

        ScriptEngine::PreparedScript prepared2;
        bool prepared2_ok = engine.PrepareEntityScript(&scene, e2.GetId(), src, error, prepared2);
        Check(prepared2_ok, "preparing the SAME script source for a second entity succeeds (error: '" + error + "')");

        engine.CallOnUpdate(prepared2, 0.016f);
        Check(std::abs(transform2.Position.x - 105.0f) < 0.001f,
            "the second entity's own transform moved independently (got x=" + std::to_string(transform2.Position.x) + ")");
        Check(std::abs(transform.Position.x - 11.0f) < 0.001f,
            "the first entity's transform is untouched by the second entity's update (got x=" + std::to_string(transform.Position.x) + ")");

        engine.ReleasePreparedScript(prepared2);
    }
}
```

- [ ] **Step 6: Call it, build, and run**

Temporarily add `#include "ScriptEngineEntityTest.h"` and `ScriptEngineEntityTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 7 `[ok ]` lines, `g_Pass == 7`, `g_Fail == 0`. This is pure logic — no ImGui/GL needed, and doesn't touch `assets/typescript.js`'s load path any differently than `RunScript` already does.

- [ ] **Step 7: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/ScriptEngineEntityTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty.

- [ ] **Step 8: Stage**

```bash
git add TestEnv/src/ScriptEngine.h
git status --short
```

---

### Task 3: `PlayMode.h` — Play/Stop lifecycle

**Files:**
- Create: `TestEnv/src/PlayMode.h`
- Create (temporary): `TestEnv/src/PlayModeTest.h`
- Modify (temporary, reverted at the end of this task): `TestEnv/src/TestApp.cpp`

**Interfaces:**
- Consumes: `GS::ScriptComponent` (Task 1), `ScriptEngine`/`ScriptEngine::PreparedScript`/`PrepareEntityScript`/`CallOnStart`/`CallOnUpdate`/`ReleasePreparedScript` (Task 2), `g_EditorScene` (already exists, `EditorProject.h`).
- Produces: `PlayMode::Play()`, `PlayMode::Stop()`, `PlayMode::OnFixedUpdate(float dt)`, `PlayMode::IsPlaying() -> bool`. Task 4 consumes all of this exactly.

- [ ] **Step 1: Write `PlayMode.h`**

```cpp
#pragma once

// Play/Stop for the editor. Snapshots g_EditorScene before Play (via the
// same GS::Scene::Save/Load this editor already uses for real project
// files), prepares every ScriptComponent'd entity's script through
// ScriptEngine, runs OnUpdate from OnFixedUpdate while playing, and
// reverts the whole scene back to its pre-Play snapshot on Stop --
// discarding anything the scripts did. A namespace with inline state,
// matching EditorHistory.h's own established pattern for exactly this
// kind of scene-wide, singleton concern.

#include <GS.h>

#include "EditorProject.h"
#include "ScriptEngine.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace PlayMode {

	inline bool s_Playing = false;
	inline ScriptEngine s_ScriptEngine;
	inline std::unordered_map<GS::EntityId, ScriptEngine::PreparedScript> s_Prepared;

	// Outside g_EditorProjectPath on purpose -- a scratch file, not a
	// project asset, so it never shows up in the file tree.
	inline const char* SnapshotPath() { return "play_snapshot.tmp"; }

	inline bool IsPlaying() { return s_Playing; }

	inline void Play()
	{
		if (s_Playing)
			return;

		if (!g_EditorScene.Save(SnapshotPath()))
		{
			GS_ERROR("PlayMode::Play: could not write scratch snapshot, not entering play mode");
			return;
		}

		for (GS::EntityId id : g_EditorScene.GetEntities())
		{
			if (!g_EditorScene.HasComponent<GS::ScriptComponent>(id))
				continue;

			auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(id);
			if (script->ScriptPath.empty())
				continue;

			std::ifstream file(script->ScriptPath, std::ios::in | std::ios::binary);
			if (!file.is_open())
			{
				GS_ERROR("PlayMode::Play: could not open script '{0}' for entity {1}", script->ScriptPath, id);
				continue;
			}
			std::stringstream buffer;
			buffer << file.rdbuf();

			ScriptEngine::PreparedScript prepared;
			std::string error;
			if (!s_ScriptEngine.PrepareEntityScript(&g_EditorScene, id, buffer.str(), error, prepared))
			{
				GS_ERROR("PlayMode::Play: entity {0}'s script failed to prepare: {1}", id, error);
				continue;
			}

			s_ScriptEngine.CallOnStart(prepared);
			s_Prepared[id] = prepared;
		}

		s_Playing = true;
	}

	inline void Stop()
	{
		if (!s_Playing)
			return;

		for (auto& pair : s_Prepared)
			s_ScriptEngine.ReleasePreparedScript(pair.second);
		s_Prepared.clear();

		g_EditorScene.Load(SnapshotPath());

		s_Playing = false;
	}

	inline void OnFixedUpdate(float dt)
	{
		if (!s_Playing)
			return;

		for (auto& pair : s_Prepared)
			s_ScriptEngine.CallOnUpdate(pair.second, dt);
	}

}
```

- [ ] **Step 2: Write the temporary self-test**

Create `TestEnv/src/PlayModeTest.h`:

```cpp
// TEMPORARY -- delete after verifying PlayMode (Task 3 of the
// entity-scripting plan).
#pragma once
#include <GS.h>
#include "PlayMode.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace PlayModeTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }

    inline void Run() {
        const char* scriptPath = "playmode_test_script.ts";
        std::ofstream scriptFile(scriptPath);
        scriptFile <<
            "function OnUpdate(dt: number): void {\n"
            "    let pos = entity.getPosition();\n"
            "    entity.setPosition(pos[0] + 10 * dt, pos[1], pos[2]);\n"
            "}\n";
        scriptFile.close();

        g_EditorScene.Clear();
        GS::Entity e = g_EditorScene.CreateEntity("Mover");
        auto& transform = e.Add<GS::TransformComponent>();
        transform.Position = { 0.0f, 0.0f, 0.0f };
        e.Add<GS::ScriptComponent>(GS::ScriptComponent{ scriptPath });

        PlayMode::Play();
        Check(PlayMode::IsPlaying(), "Play() enters play mode");

        for (int i = 0; i < 10; i++)
            PlayMode::OnFixedUpdate(0.1f); // 10 ticks x 0.1s = 1.0s simulated

        auto* duringTransform = g_EditorScene.GetComponent<GS::TransformComponent>(e.GetId());
        Check(duringTransform != nullptr, "entity survives through Play");
        Check(duringTransform && std::abs(duringTransform->Position.x - 10.0f) < 0.01f,
            "10 ticks of 0.1s at 10 units/sec moved the entity ~10 units on X (got " +
            std::to_string(duringTransform ? duringTransform->Position.x : -999.0f) + ")");

        PlayMode::Stop();
        Check(!PlayMode::IsPlaying(), "Stop() exits play mode");

        auto* afterTransform = g_EditorScene.GetComponent<GS::TransformComponent>(e.GetId());
        Check(afterTransform != nullptr && std::abs(afterTransform->Position.x) < 0.001f,
            "Stop() reverted the entity's position back to before Play (got " +
            std::to_string(afterTransform ? afterTransform->Position.x : -999.0f) + ")");

        g_EditorScene.Clear();
        std::remove(scriptPath);
        std::remove(PlayMode::SnapshotPath());
    }
}
```

- [ ] **Step 3: Call it, build, and run**

Temporarily add `#include "PlayModeTest.h"` and `PlayModeTest::Run();` to the `TestEnv` constructor in `TestApp.cpp`.

```bash
./gs.py build
./gs.py run
```

Expected: 5 `[ok ]` lines, `g_Pass == 5`, `g_Fail == 0`. Confirm from the binary's own working directory (`bin/Debug-linux-x86_64/TestEnv/`) so the relative script/snapshot paths resolve — `./gs.py run` already handles this.

- [ ] **Step 4: Revert the temporary insertion and delete the test file**

Remove the include/call from `TestApp.cpp`; delete `TestEnv/src/PlayModeTest.h`. Confirm `git diff -- TestEnv/src/TestApp.cpp` is empty, and confirm no `playmode_test_script.ts`/`play_snapshot.tmp` files were left behind in the binary's working directory (the test removes them itself — check anyway).

- [ ] **Step 5: Stage**

```bash
git add TestEnv/src/PlayMode.h
git status --short
```

---

### Task 4: Wire Play/Stop into `EditorMenuBar.h`

**Files:**
- Modify: `TestEnv/src/EditorMenuBar.h`
- Modify: `docs/STATE.md`

**Interfaces:**
- Consumes: `PlayMode::Play/Stop/IsPlaying/OnFixedUpdate` (Task 3).
- Produces: nothing further — this is the last task.

- [ ] **Step 1: Add the Play/Stop buttons and the `OnFixedUpdate` tick**

Read `TestEnv/src/EditorMenuBar.h` in full first to confirm the exact current state of `OnImGuiRender()`'s `BeginMainMenuBar`/`EndMainMenuBar` block and whether `OnFixedUpdate` already exists (it shouldn't yet).

Add `#include "PlayMode.h"` to the include block.

In `OnImGuiRender()`, find:

```cpp
		if (ImGui::BeginMainMenuBar())
		{
			DrawFileMenu();
			DrawEditMenu();
			DrawViewMenu();
			DrawHelpMenu();
			ImGui::EndMainMenuBar();
		}
```

Replace with:

```cpp
		if (ImGui::BeginMainMenuBar())
		{
			DrawFileMenu();
			DrawEditMenu();
			DrawViewMenu();
			DrawHelpMenu();
			DrawPlayControls();
			ImGui::EndMainMenuBar();
		}
```

Add the new private method (near `DrawHelpMenu()`):

```cpp
	// Plain buttons in the menu bar strip itself, not a dropdown -- always
	// visible, matching how a game engine editor's play button typically
	// works. Gated on g_ActiveDemo, same pattern Undo/Redo/Find already use:
	// Play mode operates on g_EditorScene, meaningless while looking at a
	// hardcoded C++ demo.
	void DrawPlayControls()
	{
		if (g_ActiveDemo != InvalidDemo)
			return;

		ImGui::Separator();
		if (!PlayMode::IsPlaying())
		{
			if (ImGui::Button("Play"))
				PlayMode::Play();
		}
		else
		{
			if (ImGui::Button("Stop"))
				PlayMode::Stop();
		}
	}
```

Add a new `OnFixedUpdate` override (`EditorMenuBar` is already a `GS::Layer`; this is a new override, not modifying an existing one):

```cpp
	void OnFixedUpdate(GS::Timestep step) override
	{
		if (g_ActiveDemo != InvalidDemo)
			return;
		PlayMode::OnFixedUpdate(step);
	}
```

- [ ] **Step 2: Build all three configs and verify no existing demo is affected**

```bash
./gs.py build all
```

Expected: exit 0, zero "error" occurrences, Debug/Release/Dist.

```bash
./TestEnv --demo Cube3D --hide-window --lockstep --hide-ui --capture /tmp/entityscript_hideui.png --capture-step 5
```

Compare against a pre-this-plan capture — must be byte-identical. `PlayMode::OnFixedUpdate` early-returns whenever `g_ActiveDemo != InvalidDemo`, and `DrawPlayControls()` is only ever called from `EditorMenuBar::OnImGuiRender`, which `--hide-ui` prevents from running.

- [ ] **Step 3: Manual verification (disclosed, not faked)**

```bash
./gs.py run
```

Place a cube (Tools panel), select it, add a Script via the Inspector, point it at a small `.ts` file on disk that moves the entity (e.g. reusing Task 3's own test script shape), click "Play" in the menu bar, and confirm the cube visibly moves in the "Scene" tab. Click "Stop" and confirm it snaps back to its pre-Play position. This cannot be driven headlessly in this environment (no GUI-automation tool) — report the actual result of clicking through it, not an assumption, same disclosed-limitation pattern as every other UI-interaction check in this editor pivot. If a live click-through genuinely can't be attempted safely in this session (per this repo's own established `xdotool`/window-mapping cautions), say so plainly and describe what alternative verification you did instead (e.g. a headless capture forcing play state via whatever technique the file-tree/scripting-runtime plans already established for this class of problem).

- [ ] **Step 4: Clean up every temporary test**

```bash
git grep TEMPORARY
```

Expected: no output.

- [ ] **Step 5: Update `docs/STATE.md`**

Add a "Last landed" bullet ahead of the file-tree entry, in the same style as every prior sub-project's landing entry (name the new files, the Play/Stop mechanism, the native `Entity`/`Input` binding, the self-test counts, the byte-identical `--hide-ui` capture, and the manual/alternative verification result). This is sub-project 2 of 4 toward "recreate any existing demo inside the editor" — say so. Update "What is next" accordingly.

- [ ] **Step 6: Stage**

```bash
git add TestEnv/src/EditorMenuBar.h docs/STATE.md
git status --short
```

---

## Self-Review

**Spec coverage:** `ScriptComponent` + Inspector Add/Remove UI (Task 1) ✓. Native `Entity` class (get/setPosition/Rotation/Scale) + shared `Input.isKeyDown` (Task 2) ✓. `entity`/`input` passed as function parameters, not globals, specifically to avoid cross-entity closure collision (Task 2, verified by its own self-test's two-entity scenario) ✓. Play/Stop via `Scene::Save`/`Load` snapshot-and-revert, outside the project folder (Task 3) ✓. `OnFixedUpdate`, not `OnUpdate` (Task 3/4) ✓. Editor camera navigation untouched during Play (nothing in this plan touches `EditorSceneView::MoveCamera`) ✓. Play/Stop as plain menu-bar buttons, gated on `g_ActiveDemo` (Task 4) ✓. No physics/spawning/querying-other-entities/camera-audio/`OnStop` anywhere in this plan ✓. Deterministic self-tests for every piece that doesn't strictly require a live window (Tasks 1-3), manual/disclosed verification for what does (Task 4) ✓.

**Placeholder scan:** none — every step has complete, concrete code, verified against the real vendored QuickJS API (`api-test.c`) and the real `GS::Scene`/`GS::Timestep`/`GS_KEY_*` interfaces rather than assumed.

**Type consistency:** `ScriptEngine::PreparedScript{EntityObj, OnStart, OnUpdate}` and `PrepareEntityScript`/`CallOnStart`/`CallOnUpdate`/`ReleasePreparedScript`'s signatures are used identically across Task 2's own self-test and Task 3's `PlayMode.h`. `GS::EntityId` (the map key in `PlayMode::s_Prepared`, the `EntityBinding::Id` field, the loop variable in `Play()`) is used consistently as the plain `unsigned int`-typedef'd type it already is everywhere in this codebase — no new alias or wrapper introduced. `entity.getPosition()`/`setPosition(x,y,z)` (and Rotation/Scale) argument order and array-index convention (`[0]=x, [1]=y, [2]=z`) match between Task 2's native implementation and every script written against it in Tasks 2/3/4's own test scripts.
