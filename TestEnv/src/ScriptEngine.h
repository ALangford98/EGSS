#pragma once

// Runs TypeScript by transpiling it with TypeScript's own compiler bundle
// (vendored as TestEnv/assets/typescript.js -- the real, unmodified
// lib/typescript.js from typescript@5.6.3, refreshed by re-running the
// `npm pack typescript@<version>` + extract steps this plan's Task 2
// used originally) running inside our own embedded QuickJS, then
// evaluating the resulting JS in the same engine. No file/process/OS
// access is exposed to scripts. console.log is still the only I/O surface
// for RunScript's free-standing scripts; per-entity scripts prepared via
// PrepareEntityScript additionally get a native `entity` (position/
// rotation/scale), `input` (isKeyDown), and `scene` (findByTag/spawn/
// destroy) binding, passed in as function parameters rather than globals
// so two entities running the same script source never share state.

#include <GS.h>
#include <quickjs.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class ScriptEngine
{
public:
	struct ScriptResult
	{
		bool Ok = false;
		std::string Output;
		std::string Error;
	};

	struct PreparedScript
	{
		JSValue EntityObj = JS_UNDEFINED;
		JSValue OnStart = JS_UNDEFINED;
		JSValue OnUpdate = JS_UNDEFINED;
	};

	ScriptEngine() = default;
	ScriptEngine(const ScriptEngine&) = delete;
	ScriptEngine& operator=(const ScriptEngine&) = delete;

	~ScriptEngine()
	{
		if (m_Context)
		{
			JS_FreeValue(m_Context, m_InputObject);
			JS_FreeValue(m_Context, m_SceneObject);
			JS_FreeContext(m_Context);
		}
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

		// Set after Initialize() so the one-time typescript.js load (up to
		// ~0.65s in Debug) doesn't eat into the script's own time budget.
		m_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

		m_CapturedOutput.clear();
		m_OutputTruncated = false;

		JSValue transpileFn = JS_Eval(m_Context, s_TranspileSrc, strlen(s_TranspileSrc), "transpile", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(transpileFn))
		{
			ScriptResult failure;
			failure.Error = "Internal error: driver script failed: " + DescribeException();
			JS_FreeValue(m_Context, transpileFn);
			return failure;
		}

		JSValue srcArg = JS_NewString(m_Context, tsSource.c_str());
		JSValue transpileResult = JS_Call(m_Context, transpileFn, JS_UNDEFINED, 1, &srcArg);
		JS_FreeValue(m_Context, srcArg);
		JS_FreeValue(m_Context, transpileFn);

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

		std::string wrapped = "(function(){" + jsCode + "\n})();";
		JSValue runResult = JS_Eval(m_Context, wrapped.c_str(), wrapped.size(), "script", JS_EVAL_TYPE_GLOBAL);

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

	// Transpiles and prepares a script for one entity: its top-level
	// OnStart/OnUpdate become a private closure over that entity's own
	// native binding (passed as a function *parameter*, not a global) --
	// so two entities running the same script file, or the same entity
	// re-prepared, never collide, the same reasoning RunScript's own IIFE
	// wrap already relies on for its own redeclaration fix.
	//
	// `scriptPath` anchors relative import resolution (the entity's own
	// script may `import` from a sibling file) -- this is why RunScript's
	// ad-hoc, possibly-unsaved buffer doesn't get import support, only a
	// real ScriptComponent with a path does. `outDependencies`, if given,
	// collects every module (transitively) imported, purely so PlayMode
	// can write the dependency manifest -- see
	// docs/superpowers/specs/2026-09-11-script-imports-design.md.
	bool PrepareEntityScript(GS::Scene* scene, GS::EntityId entityId, const std::string& scriptPath,
		const std::string& tsSource, std::string& error, PreparedScript& out,
		std::set<std::string>* outDependencies = nullptr)
	{
		if (!m_Ready && !Initialize())
		{
			error = "Script engine failed to initialize (see log).";
			return false;
		}

		ModuleAnalysis analysis;
		if (!AnalyzeModule(tsSource, error, analysis))
			return false;

		std::string assembled;
		std::set<std::string> stack;
		std::set<std::string> dependencies;
		for (const ImportInfo& imp : analysis.Imports)
		{
			std::string resolvedPath = ResolveImportPath(scriptPath, imp.Specifier);
			if (resolvedPath.empty())
			{
				error = "could not resolve import '" + imp.Specifier + "' from '" + scriptPath + "'";
				return false;
			}
			dependencies.insert(resolvedPath);

			ResolvedModule* module = nullptr;
			if (!ResolveModule(resolvedPath, stack, dependencies, error, module))
				return false;

			for (const ImportName& name : imp.Names)
			{
				if (std::find(module->ExportNames.begin(), module->ExportNames.end(), name.Imported) == module->ExportNames.end())
				{
					error = "'" + name.Imported + "' is not exported by the module imported from '" + imp.Specifier + "' (in '" + scriptPath + "')";
					return false;
				}
			}

			assembled += SpliceImport(imp, *module);
		}
		assembled += analysis.Body;

		if (outDependencies)
			*outDependencies = dependencies;

		JSValue transpileFn = JS_Eval(m_Context, s_TranspileSrc, strlen(s_TranspileSrc), "transpile", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(transpileFn))
		{
			error = "Internal error: transpile driver failed: " + DescribeException();
			JS_FreeValue(m_Context, transpileFn);
			return false;
		}

		JSValue srcArg = JS_NewString(m_Context, assembled.c_str());
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

		std::string wrapped = "(function(entity, input, scene) {" + jsCode +
			"\nreturn { onStart: typeof OnStart === 'function' ? OnStart : null, "
			"onUpdate: typeof OnUpdate === 'function' ? OnUpdate : null }; })";

		JSValue wrapperFn = JS_Eval(m_Context, wrapped.c_str(), wrapped.size(), "entity-script", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(wrapperFn))
		{
			error = "Script error: " + DescribeException();
			JS_FreeValue(m_Context, wrapperFn);
			return false;
		}

		// scene.findByTag/spawn/destroy operate on whichever GS::Scene* this
		// entity belongs to -- set here rather than baked into m_SceneObject
		// at construction, since m_SceneObject is created once in Initialize()
		// before any scene exists yet, and is shared (not per-entity, unlike
		// the entity binding) across every PrepareEntityScript call.
		m_CurrentScene = scene;

		JSValue entityObj = CreateEntityBinding(scene, entityId);
		JSValue args[3] = { entityObj, m_InputObject, m_SceneObject };
		// One-time call, at Play time -- same 5s budget as RunScript's own
		// top-level eval, since this is where a script's own module-level
		// (non-OnUpdate) code actually runs.
		m_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		JSValue result = JS_Call(m_Context, wrapperFn, JS_UNDEFINED, 3, args);
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
			// One-time call, at Play time -- same budget as PrepareEntityScript's.
			m_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
			// Runs every fixed step, unlike OnStart/PrepareEntityScript's
			// one-time calls -- a much tighter budget so a runaway OnUpdate
			// doesn't freeze every single frame while playing.
			m_Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
			JSValue dtArg = JS_NewFloat64(m_Context, dt);
			JSValue result = JS_Call(m_Context, script.OnUpdate, JS_UNDEFINED, 1, &dtArg);
			if (JS_IsException(result))
				GS_ERROR("ScriptEngine: entity script OnUpdate threw: {0}", DescribeException());
			JS_FreeValue(m_Context, result);
		}
	}

	// Scoped to one Play session -- see PlayMode::Play(), which calls this
	// before preparing any entity's script.
	void ClearModuleCache() { m_ModuleCache.clear(); }

	void ReleasePreparedScript(PreparedScript& script)
	{
		JS_FreeValue(m_Context, script.EntityObj);
		JS_FreeValue(m_Context, script.OnStart);
		JS_FreeValue(m_Context, script.OnUpdate);
		script.EntityObj = JS_UNDEFINED;
		script.OnStart = JS_UNDEFINED;
		script.OnUpdate = JS_UNDEFINED;
	}

	// FNV-1a, matching the convention this codebase already uses
	// elsewhere (Terrain.h::Checksum, OpenWorld.h) for "did this content
	// change" hashing -- not cryptographic, doesn't need to be.
	static std::string Fnv1aHex(const std::string& text)
	{
		uint32_t hash = 2166136261u;
		for (unsigned char byte : text)
		{
			hash ^= byte;
			hash *= 16777619u;
		}
		char hex[9];
		snprintf(hex, sizeof(hex), "%08x", hash);
		return std::string(hex);
	}

	// The optimize-lock (Task 6 of the transpiler plan): true if a
	// generated file's current content no longer matches the hash
	// TranspileToCpp stamped it with -- i.e. it's been hand-edited
	// ("graduated") since it was last generated, and rebuilding it needs
	// the owner's say-so rather than a silent overwrite. True for a file
	// with no stamp at all (hand-written, or from before this existed) --
	// unstamped means "protect it," not "assume it's safe." False (safe
	// to overwrite freely) for a file that doesn't exist yet.
	//
	// Finds the body by searching for its known starting text rather than
	// hardcoding "skip N header comment lines", so a future wording change
	// to those comments can't silently break this check.
	static bool IsGeneratedFileHandEdited(const std::string& cppPath)
	{
		std::ifstream file(cppPath);
		if (!file)
			return false;

		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		const std::string marker = "// GS-GENERATED: ";
		if (content.rfind(marker, 0) != 0)
			return true;

		size_t hashStart = marker.size();
		size_t hashEnd = content.find('\n', hashStart);
		if (hashEnd == std::string::npos)
			return true;
		std::string storedHash = content.substr(hashStart, hashEnd - hashStart);

		size_t bodyStart = content.find("#include <GS.h>");
		if (bodyStart == std::string::npos)
			return true;

		return Fnv1aHex(content.substr(bodyStart)) != storedHash;
	}

	// Shells out to a real compiler against this project's own actual
	// include paths (GS_REPO_ROOT, baked in absolutely by premake5.lua at
	// generation time -- the running executable has no other reliable way
	// to find GS/src from wherever its own bin/<config>/TestEnv/ happens
	// to sit) and the same platform/config defines *this build* was
	// compiled with, read via the same preprocessor guards rather than
	// hardcoded, so a Release or Dist build's syntax-check doesn't quietly
	// check against Debug's defines instead.
	static bool RunSyntaxCheck(const std::string& cppPath, std::string& diagnostics)
	{
#ifndef GS_REPO_ROOT
		diagnostics = "Syntax-checking isn't implemented on this platform yet (no GS_REPO_ROOT).";
		return false;
#else
		std::string root = GS_REPO_ROOT;
		std::string defines = "-DGS_PLATFORM_LINUX ";
#ifdef GS_DEBUG
		defines += "-DGS_DEBUG -DGS_ENABLE_ASSERTS ";
#endif
#ifdef GS_RELEASE
		defines += "-DGS_RELEASE ";
#endif
#ifdef GS_DIST
		defines += "-DGS_DIST ";
#endif
#ifdef GS_PROFILE
		defines += "-DGS_PROFILE ";
#endif

		std::string errPath = cppPath + ".synerr";
		// `timeout` wraps the whole thing: calling std::system (fork+exec)
		// from a process that already has other threads running (audio,
		// in this app) is a known hazard -- a lock held by another thread
		// at the instant of fork() can leave the forked child deadlocked
		// before exec() ever replaces it. A bound here turns "the editor
		// hangs forever" into "the syntax-check reports a timeout error",
		// which is what an unrelated bug in this fork/exec path should
		// degrade to, not a frozen UI.
		std::string command = "timeout 10 g++ -std=c++17 -fsyntax-only " + defines
			+ "-I\"" + root + "/GS/vendor/spdlog/include\" "
			+ "-I\"" + root + "/GS/src\" "
			+ "-I\"" + root + "/GS/vendor/glm\" "
			+ "-I\"" + root + "/GS/vendor/imgui\" "
			+ "-I\"" + root + "/GS/vendor/libvterm/include\" "
			+ "-I\"" + root + "/GS/vendor/quickjs\" "
			+ "\"" + cppPath + "\" 2> \"" + errPath + "\"";

		int result = std::system(command.c_str());

		std::ifstream errFile(errPath);
		diagnostics.assign((std::istreambuf_iterator<char>(errFile)), std::istreambuf_iterator<char>());
		errFile.close();
		std::remove(errPath.c_str());

		return result == 0;
#endif
	}

	// GSS-to-C++ codegen, Task 1 of the transpiler plan: the subset named
	// in docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md
	// minus arrow functions/template literals/classes/generics (Tasks
	// 2-4) and imports (not yet wired into codegen at all -- calling an
	// imported function fails with a named "not a recognized top-level
	// function" error, same as calling anything else codegen doesn't
	// understand). `source` is one script's raw text, unmodified --
	// unlike PrepareEntityScript, this does not resolve imports first.
	//
	// On success, `outCpp` is the complete generated file text, stamped
	// with a `GS-GENERATED: <hash>` first line (the hash covers everything
	// after that line) -- see docs/superpowers/specs/
	// 2026-09-11-gss-to-cpp-transpiler-design.md's "optimize-lock" section
	// for what reads that stamp later (not built in this task).
	bool TranspileToCpp(const std::string& source, const std::string& className, std::string& error, std::string& outCpp)
	{
		if (!m_Ready && !Initialize())
		{
			error = "Script engine failed to initialize (see log).";
			return false;
		}

		JSValue fn = JS_Eval(m_Context, s_CodegenSrc, strlen(s_CodegenSrc), "codegen", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(fn))
		{
			error = "Internal error: codegen driver failed: " + DescribeException();
			JS_FreeValue(m_Context, fn);
			return false;
		}

		JSValue srcArg = JS_NewString(m_Context, source.c_str());
		// Same 5s budget as PrepareEntityScript's own one-time calls --
		// forgetting this (as the first version of this method did) means
		// InterruptHandler's `now() > deadline` never trips against
		// m_Deadline's very-far-future default, so a genuine infinite
		// loop anywhere in s_CodegenSrc hangs the whole process instead of
		// failing cleanly.
		m_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		JSValue result = JS_Call(m_Context, fn, JS_UNDEFINED, 1, &srcArg);
		JS_FreeValue(m_Context, srcArg);
		JS_FreeValue(m_Context, fn);

		if (JS_IsException(result))
		{
			error = "Internal error: codegen call failed: " + DescribeException();
			JS_FreeValue(m_Context, result);
			return false;
		}

		JSValue okVal = JS_GetPropertyStr(m_Context, result, "ok");
		bool ok = JS_ToBool(m_Context, okVal);
		JS_FreeValue(m_Context, okVal);

		if (!ok)
		{
			error = GetStringProperty(result, "error");
			JS_FreeValue(m_Context, result);
			return false;
		}

		std::string membersText;
		JSValue membersArr = JS_GetPropertyStr(m_Context, result, "members");
		uint32_t memberCount = 0;
		JS_ToUint32(m_Context, &memberCount, JS_GetPropertyStr(m_Context, membersArr, "length"));
		for (uint32_t i = 0; i < memberCount; i++)
		{
			JSValue member = JS_GetPropertyUint32(m_Context, membersArr, i);
			std::string type = GetStringProperty(member, "type");
			std::string name = GetStringProperty(member, "name");
			JSValue initVal = JS_GetPropertyStr(m_Context, member, "init");
			std::string init = JS_IsNull(initVal) ? "" : (" = " + GetStringProperty(member, "init"));
			JS_FreeValue(m_Context, initVal);
			membersText += "\t" + type + " " + name + init + ";\n";
			JS_FreeValue(m_Context, member);
		}
		JS_FreeValue(m_Context, membersArr);

		std::string publicMethods, privateMethods;
		JSValue methodsArr = JS_GetPropertyStr(m_Context, result, "methods");
		uint32_t methodCount = 0;
		JS_ToUint32(m_Context, &methodCount, JS_GetPropertyStr(m_Context, methodsArr, "length"));
		for (uint32_t i = 0; i < methodCount; i++)
		{
			JSValue method = JS_GetPropertyUint32(m_Context, methodsArr, i);
			JSValue isPublicVal = JS_GetPropertyStr(m_Context, method, "isPublic");
			bool isPublic = JS_ToBool(m_Context, isPublicVal);
			JS_FreeValue(m_Context, isPublicVal);

			// A template<> clause on a *member function* (not a free
			// function) still needs to sit directly against the class
			// body, indented the same as the method itself -- hence the
			// leading "\t" here too, not just on the signature line.
			std::string signature = "\t" + GetStringProperty(method, "templateClause")
				+ GetStringProperty(method, "returnType") + " " + GetStringProperty(method, "name")
				+ "(" + GetStringProperty(method, "params") + ")\n" + GetStringProperty(method, "body") + "\n\n";
			(isPublic ? publicMethods : privateMethods) += signature;
			JS_FreeValue(m_Context, method);
		}
		JS_FreeValue(m_Context, methodsArr);

		// Top-level classes: independent siblings of the entity's own
		// class in the same namespace, not nested inside it -- see
		// s_CodegenSrc's emitClass for why (a class has no relation to
		// the entity-script closure model entity/scene-prepending exists
		// to preserve). Emitted before the entity class so a forward
		// reference from the entity class to a helper class (using one as
		// a member's type, say) doesn't need a separate declaration pass.
		std::string classesText;
		JSValue classesArr = JS_GetPropertyStr(m_Context, result, "classes");
		uint32_t classCount = 0;
		JS_ToUint32(m_Context, &classCount, JS_GetPropertyStr(m_Context, classesArr, "length"));
		for (uint32_t i = 0; i < classCount; i++)
		{
			JSValue cls = JS_GetPropertyUint32(m_Context, classesArr, i);
			std::string clsName = GetStringProperty(cls, "name");
			std::string baseClause = GetStringProperty(cls, "baseClause");

			std::string fieldsText;
			JSValue fieldsArr = JS_GetPropertyStr(m_Context, cls, "fields");
			uint32_t fieldCount = 0;
			JS_ToUint32(m_Context, &fieldCount, JS_GetPropertyStr(m_Context, fieldsArr, "length"));
			for (uint32_t f = 0; f < fieldCount; f++)
			{
				JSValue fieldVal = JS_GetPropertyUint32(m_Context, fieldsArr, f);
				const char* fieldStr = JS_ToCString(m_Context, fieldVal);
				fieldsText += "\t" + std::string(fieldStr ? fieldStr : "") + "\n";
				JS_FreeCString(m_Context, fieldStr);
				JS_FreeValue(m_Context, fieldVal);
			}
			JS_FreeValue(m_Context, fieldsArr);

			std::string ctorText;
			JSValue ctorVal = JS_GetPropertyStr(m_Context, cls, "ctor");
			if (!JS_IsNull(ctorVal))
			{
				ctorText = "\t" + clsName + "(" + GetStringProperty(ctorVal, "params") + ")\n"
					+ GetStringProperty(ctorVal, "body") + "\n\n";
			}
			JS_FreeValue(m_Context, ctorVal);

			std::string classMethodsText;
			JSValue classMethodsArr = JS_GetPropertyStr(m_Context, cls, "methods");
			uint32_t classMethodCount = 0;
			JS_ToUint32(m_Context, &classMethodCount, JS_GetPropertyStr(m_Context, classMethodsArr, "length"));
			for (uint32_t m = 0; m < classMethodCount; m++)
			{
				JSValue classMethod = JS_GetPropertyUint32(m_Context, classMethodsArr, m);
				classMethodsText += "\t" + GetStringProperty(classMethod, "returnType") + " " + GetStringProperty(classMethod, "name")
					+ "(" + GetStringProperty(classMethod, "params") + ")\n" + GetStringProperty(classMethod, "body") + "\n\n";
				JS_FreeValue(m_Context, classMethod);
			}
			JS_FreeValue(m_Context, classMethodsArr);

			classesText += "class " + clsName + baseClause + "\n{\npublic:\n" + fieldsText + "\n"
				+ ctorText + classMethodsText + "};\n\n";
			JS_FreeValue(m_Context, cls);
		}
		JS_FreeValue(m_Context, classesArr);
		JS_FreeValue(m_Context, result);

		std::string body = "#include <GS.h>\n\nnamespace GeneratedScripts {\n\n" + classesText
			+ "class " + className + "\n{\npublic:\n"
			+ publicMethods + "private:\n" + privateMethods + membersText + "};\n\n}\n";

		outCpp = "// GS-GENERATED: " + Fnv1aHex(body) +
			"\n// Generated from a .gss file -- see docs/superpowers/specs/"
			"2026-09-11-gss-to-cpp-transpiler-design.md. Hand-editing this file is\n"
			"// fine; doing so is what \"optimizing\" it means. A future rebuild from\n"
			"// the .gss source will ask before overwriting a file whose content no\n"
			"// longer matches the hash above.\n" + body;
		return true;
	}

private:
	struct EntityBinding
	{
		GS::Scene* Scene;
		GS::EntityId Id;
	};

	// Per-instance, not per-process: an `inline static` class ID was tried
	// first and was wrong -- JS_NewClass() registers a class ID into one
	// specific JSRuntime's class table (confirmed against
	// GS/vendor/quickjs/api-test.c's own JS_NewClassID/JS_NewClass/
	// JS_NewObjectClass sequence), but EnsureEntityClassRegistered() only
	// runs JS_NewClass on the *first* ScriptEngine ever constructed, since a
	// shared static already reads as "registered" for every instance after
	// that. A second ScriptEngine -- its own runtime, e.g. Task 3's
	// PlayMode::s_ScriptEngine alongside EditorShell's existing one -- would
	// then call JS_NewObjectClass with a class ID never registered on *its*
	// runtime: undefined behavior. Caught by a two-instance self-test before
	// this shipped.
	JSClassID m_EntityClassId = 0;

	// A JSClassDef::finalizer only gets the JSRuntime* and the value being
	// finalized -- no `this` -- so it can't read an instance's
	// m_EntityClassId directly. It recovers the owning ScriptEngine through
	// JS_GetRuntimeOpaque(rt), the same association Initialize() sets up via
	// JS_SetRuntimeOpaque(m_Runtime, this) and NativeConsoleLog already
	// relies on below.
	static void EntityFinalizer(JSRuntime* rt, JSValueConst val)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(rt);
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque(val, self->m_EntityClassId);
		delete binding;
	}

	static JSValue Native_GetPosition(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
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
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		if (argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		// JS_ToFloat64 returns -1 (and leaves a pending exception) on
		// failure, e.g. a script passing a string -- propagate that instead
		// of silently writing whatever half-converted (typically zero) value
		// came out, matching the getters' JS_EXCEPTION convention.
		double x = 0, y = 0, z = 0;
		if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 || JS_ToFloat64(ctx, &z, argv[2]) < 0)
			return JS_EXCEPTION;
		transform->Position = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_GetRotation(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
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
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		if (argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		double x = 0, y = 0, z = 0;
		if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 || JS_ToFloat64(ctx, &z, argv[2]) < 0)
			return JS_EXCEPTION;
		transform->Rotation = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_GetScale(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
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
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, thisVal, self->m_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;
		if (argc < 3)
			return JS_UNDEFINED;
		auto* transform = binding->Scene->GetComponent<GS::TransformComponent>(binding->Id);
		if (!transform)
			return JS_UNDEFINED;
		double x = 0, y = 0, z = 0;
		if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 || JS_ToFloat64(ctx, &z, argv[2]) < 0)
			return JS_EXCEPTION;
		transform->Scale = { (float)x, (float)y, (float)z };
		return JS_UNDEFINED;
	}

	static JSValue Native_IsKeyDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		// Same guard as EditorSceneView's fly camera, and for the same
		// reason: this view sits beside the Inspector/Assets/Find text
		// fields, and typing in one of those shouldn't also drive whatever
		// script is running under Play.
		if (ImGui::GetIO().WantCaptureKeyboard)
			return JS_NewBool(ctx, false);
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
		// Named rather than single-character, same reasoning as SPACE --
		// revealed as missing while recreating Breakout's paddle, which
		// binds movement to both WASD and the arrow keys.
		if (name == "LEFT") return GS_KEY_LEFT;
		if (name == "RIGHT") return GS_KEY_RIGHT;
		if (name == "UP") return GS_KEY_UP;
		if (name == "DOWN") return GS_KEY_DOWN;
		return -1;
	}

	// Reads obj[key] as a 3-element array into `out`; leaves `out`
	// untouched and returns false if the property isn't an array (missing,
	// wrong type, or a script that just didn't pass it -- every spawn()
	// option is optional).
	static bool ReadVec3(JSContext* ctx, JSValueConst obj, const char* key, glm::vec3& out)
	{
		JSValue arr = JS_GetPropertyStr(ctx, obj, key);
		bool isArray = JS_IsArray(arr);
		if (isArray)
		{
			double x = 0, y = 0, z = 0;
			JSValue xv = JS_GetPropertyUint32(ctx, arr, 0);
			JSValue yv = JS_GetPropertyUint32(ctx, arr, 1);
			JSValue zv = JS_GetPropertyUint32(ctx, arr, 2);
			JS_ToFloat64(ctx, &x, xv);
			JS_ToFloat64(ctx, &y, yv);
			JS_ToFloat64(ctx, &z, zv);
			JS_FreeValue(ctx, xv);
			JS_FreeValue(ctx, yv);
			JS_FreeValue(ctx, zv);
			out = glm::vec3((float)x, (float)y, (float)z);
		}
		JS_FreeValue(ctx, arr);
		return isArray;
	}

	static bool ReadVec4(JSContext* ctx, JSValueConst obj, const char* key, glm::vec4& out)
	{
		JSValue arr = JS_GetPropertyStr(ctx, obj, key);
		bool isArray = JS_IsArray(arr);
		if (isArray)
		{
			double x = 0, y = 0, z = 0, w = 1;
			JSValue xv = JS_GetPropertyUint32(ctx, arr, 0);
			JSValue yv = JS_GetPropertyUint32(ctx, arr, 1);
			JSValue zv = JS_GetPropertyUint32(ctx, arr, 2);
			JSValue wv = JS_GetPropertyUint32(ctx, arr, 3);
			JS_ToFloat64(ctx, &x, xv);
			JS_ToFloat64(ctx, &y, yv);
			JS_ToFloat64(ctx, &z, zv);
			if (!JS_IsUndefined(wv)) JS_ToFloat64(ctx, &w, wv);
			JS_FreeValue(ctx, xv);
			JS_FreeValue(ctx, yv);
			JS_FreeValue(ctx, zv);
			JS_FreeValue(ctx, wv);
			out = glm::vec4((float)x, (float)y, (float)z, (float)w);
		}
		JS_FreeValue(ctx, arr);
		return isArray;
	}

	// scene.findByTag(name) -- a linear scan, not an index: entity counts
	// in this editor's own scenes are in the tens, not the thousands, and
	// this is the one place "querying other entities" (the entity-scripting
	// spec's own deferred item) actually needs to happen.
	static JSValue Native_SceneFindByTag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		if (!self->m_CurrentScene || argc < 1)
			return JS_NULL;

		const char* name = JS_ToCString(ctx, argv[0]);
		std::string wanted = name ? name : "";
		JS_FreeCString(ctx, name);

		for (GS::EntityId id : self->m_CurrentScene->GetEntities())
		{
			auto* tag = self->m_CurrentScene->GetComponent<GS::TagComponent>(id);
			if (tag && tag->Name == wanted)
				return self->CreateEntityBinding(self->m_CurrentScene, id);
		}
		return JS_NULL;
	}

	// scene.destroy(entity) -- any entity, not just the caller's own,
	// which is what makes e.g. a ball script clearing a brick it just hit
	// possible at all.
	static JSValue Native_SceneDestroy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		if (!self->m_CurrentScene || argc < 1)
			return JS_UNDEFINED;

		EntityBinding* binding = (EntityBinding*)JS_GetOpaque2(ctx, argv[0], self->m_EntityClassId);
		if (!binding)
			return JS_EXCEPTION;

		self->m_CurrentScene->DestroyEntity(binding->Id);
		return JS_UNDEFINED;
	}

	// scene.spawn({ position?, scale?, mesh?, color?, tag? }) -- Transform
	// and, if `mesh` names a MeshCache key (e.g. "primitive:cube"), a
	// MeshComponent. Deliberately no `script` option: a spawned entity
	// carrying its own running script would need PlayMode's own
	// s_Prepared bookkeeping reachable from here, and ScriptEngine has no
	// dependency on PlayMode.h today (PlayMode.h depends on this file, not
	// the other way around) -- not worth a layering change for a use case
	// (bricks) that only needs to exist and be destroyed, never to act.
	static JSValue Native_SceneSpawn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
		if (!self->m_CurrentScene || argc < 1)
			return JS_EXCEPTION;

		JSValueConst options = argv[0];

		std::string tag = "Spawned";
		JSValue tagVal = JS_GetPropertyStr(ctx, options, "tag");
		if (!JS_IsUndefined(tagVal))
		{
			const char* s = JS_ToCString(ctx, tagVal);
			if (s) tag = s;
			JS_FreeCString(ctx, s);
		}
		JS_FreeValue(ctx, tagVal);

		GS::Entity entity = self->m_CurrentScene->CreateEntity(tag);

		glm::vec3 position;
		if (ReadVec3(ctx, options, "position", position))
			entity.Get<GS::TransformComponent>()->Position = position;

		glm::vec3 scale;
		if (ReadVec3(ctx, options, "scale", scale))
			entity.Get<GS::TransformComponent>()->Scale = scale;

		JSValue meshVal = JS_GetPropertyStr(ctx, options, "mesh");
		if (!JS_IsUndefined(meshVal))
		{
			const char* meshPath = JS_ToCString(ctx, meshVal);
			if (meshPath)
			{
				GS::MeshComponent mesh;
				mesh.SourcePath = meshPath;
				mesh.Geometry = GS::MeshCache::Get(meshPath);

				glm::vec4 color;
				if (ReadVec4(ctx, options, "color", color))
					mesh.Color = color;

				entity.Add<GS::MeshComponent>(mesh);
				JS_FreeCString(ctx, meshPath);
			}
		}
		JS_FreeValue(ctx, meshVal);

		return self->CreateEntityBinding(self->m_CurrentScene, entity.GetId());
	}

	void EnsureEntityClassRegistered()
	{
		if (m_EntityClassId != 0)
			return;
		JS_NewClassID(m_Runtime, &m_EntityClassId);
		JSClassDef def = {};
		def.class_name = "Entity";
		def.finalizer = EntityFinalizer;
		JS_NewClass(m_Runtime, m_EntityClassId, &def);
	}

	JSValue CreateEntityBinding(GS::Scene* scene, GS::EntityId id)
	{
		JSValue obj = JS_NewObjectClass(m_Context, m_EntityClassId);
		JS_SetOpaque(obj, new EntityBinding{ scene, id });
		JS_SetPropertyStr(m_Context, obj, "getPosition", JS_NewCFunction(m_Context, Native_GetPosition, "getPosition", 0));
		JS_SetPropertyStr(m_Context, obj, "setPosition", JS_NewCFunction(m_Context, Native_SetPosition, "setPosition", 3));
		JS_SetPropertyStr(m_Context, obj, "getRotation", JS_NewCFunction(m_Context, Native_GetRotation, "getRotation", 0));
		JS_SetPropertyStr(m_Context, obj, "setRotation", JS_NewCFunction(m_Context, Native_SetRotation, "setRotation", 3));
		JS_SetPropertyStr(m_Context, obj, "getScale", JS_NewCFunction(m_Context, Native_GetScale, "getScale", 0));
		JS_SetPropertyStr(m_Context, obj, "setScale", JS_NewCFunction(m_Context, Native_SetScale, "setScale", 3));
		return obj;
	}

	// --- Cross-file imports -------------------------------------------
	// See docs/superpowers/specs/2026-09-11-script-imports-design.md for
	// the design this implements: transpile-time inlining, not real ES
	// modules -- entity/input/scene stay function parameters, and each
	// entity that imports a module gets its own private copy of it.

	struct ImportName { std::string Imported, Local; };
	struct ImportInfo { std::string Specifier; std::vector<ImportName> Names; };
	struct ModuleAnalysis { std::vector<ImportInfo> Imports; std::vector<std::string> ExportNames; std::string Body; };

	// A module's own imports are already spliced ahead of Body here, so a
	// cache hit hands back something ready to splice directly -- callers
	// never need to re-walk a cached module's own dependency list.
	struct ResolvedModule { std::string Body; std::vector<std::string> ExportNames; };

	// Cleared at the start of every PlayMode::Play() (see PlayMode.h) --
	// scoped to one Play session, not this (long-lived) engine instance,
	// so editing a module and pressing Play again picks up the change.
	std::unordered_map<std::string, ResolvedModule> m_ModuleCache;

	bool AnalyzeModule(const std::string& source, std::string& error, ModuleAnalysis& out)
	{
		JSValue fn = JS_Eval(m_Context, s_ModuleAnalysisSrc, strlen(s_ModuleAnalysisSrc), "module-analysis", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(fn))
		{
			error = "Internal error: module analysis driver failed: " + DescribeException();
			JS_FreeValue(m_Context, fn);
			return false;
		}

		JSValue srcArg = JS_NewString(m_Context, source.c_str());
		JSValue result = JS_Call(m_Context, fn, JS_UNDEFINED, 1, &srcArg);
		JS_FreeValue(m_Context, srcArg);
		JS_FreeValue(m_Context, fn);

		if (JS_IsException(result))
		{
			error = "Internal error: module analysis call failed: " + DescribeException();
			JS_FreeValue(m_Context, result);
			return false;
		}

		JSValue okVal = JS_GetPropertyStr(m_Context, result, "ok");
		bool ok = JS_ToBool(m_Context, okVal);
		JS_FreeValue(m_Context, okVal);

		if (!ok)
		{
			error = GetStringProperty(result, "error");
			JS_FreeValue(m_Context, result);
			return false;
		}

		out.Body = GetStringProperty(result, "body");

		JSValue importsArr = JS_GetPropertyStr(m_Context, result, "imports");
		uint32_t importCount = 0;
		JS_ToUint32(m_Context, &importCount, JS_GetPropertyStr(m_Context, importsArr, "length"));
		for (uint32_t i = 0; i < importCount; i++)
		{
			JSValue imp = JS_GetPropertyUint32(m_Context, importsArr, i);
			ImportInfo info;
			JSValue specVal = JS_GetPropertyStr(m_Context, imp, "specifier");
			const char* spec = JS_ToCString(m_Context, specVal);
			info.Specifier = spec ? spec : "";
			JS_FreeCString(m_Context, spec);
			JS_FreeValue(m_Context, specVal);

			JSValue namesArr = JS_GetPropertyStr(m_Context, imp, "names");
			uint32_t nameCount = 0;
			JS_ToUint32(m_Context, &nameCount, JS_GetPropertyStr(m_Context, namesArr, "length"));
			for (uint32_t n = 0; n < nameCount; n++)
			{
				JSValue nameObj = JS_GetPropertyUint32(m_Context, namesArr, n);
				ImportName name;
				JSValue importedVal = JS_GetPropertyStr(m_Context, nameObj, "imported");
				JSValue localVal = JS_GetPropertyStr(m_Context, nameObj, "local");
				const char* importedStr = JS_ToCString(m_Context, importedVal);
				const char* localStr = JS_ToCString(m_Context, localVal);
				name.Imported = importedStr ? importedStr : "";
				name.Local = localStr ? localStr : "";
				JS_FreeCString(m_Context, importedStr);
				JS_FreeCString(m_Context, localStr);
				JS_FreeValue(m_Context, importedVal);
				JS_FreeValue(m_Context, localVal);
				info.Names.push_back(name);
				JS_FreeValue(m_Context, nameObj);
			}
			JS_FreeValue(m_Context, namesArr);

			out.Imports.push_back(info);
			JS_FreeValue(m_Context, imp);
		}
		JS_FreeValue(m_Context, importsArr);

		JSValue exportsArr = JS_GetPropertyStr(m_Context, result, "exportNames");
		uint32_t exportCount = 0;
		JS_ToUint32(m_Context, &exportCount, JS_GetPropertyStr(m_Context, exportsArr, "length"));
		for (uint32_t i = 0; i < exportCount; i++)
		{
			JSValue nameVal = JS_GetPropertyUint32(m_Context, exportsArr, i);
			const char* name = JS_ToCString(m_Context, nameVal);
			out.ExportNames.push_back(name ? name : "");
			JS_FreeCString(m_Context, name);
			JS_FreeValue(m_Context, nameVal);
		}
		JS_FreeValue(m_Context, exportsArr);

		JS_FreeValue(m_Context, result);
		return true;
	}

	// Tries the specifier literally first, then with .gss and .ts appended
	// -- .gss (GameStart Script) first, since that's this project's own
	// name for the language going forward, but nothing stops a script
	// naming a .ts file explicitly (e.g. an older file not yet renamed).
	// Relative to the *importing file's* own directory only -- no bare
	// specifiers, no project-root-relative paths.
	static std::string ResolveImportPath(const std::string& fromFile, const std::string& specifier)
	{
		std::filesystem::path base = std::filesystem::path(fromFile).parent_path();
		std::filesystem::path candidate = base / specifier;

		if (std::filesystem::is_regular_file(candidate))
			return candidate.lexically_normal().string();

		for (const char* ext : { ".gss", ".ts" })
		{
			std::filesystem::path withExt = candidate;
			withExt += ext;
			if (std::filesystem::is_regular_file(withExt))
				return withExt.lexically_normal().string();
		}
		return "";
	}

	std::string SpliceImport(const ImportInfo& imp, const ResolvedModule& module)
	{
		std::string destructure;
		for (size_t i = 0; i < imp.Names.size(); i++)
		{
			if (i > 0) destructure += ", ";
			destructure += (imp.Names[i].Imported == imp.Names[i].Local)
				? imp.Names[i].Local
				: (imp.Names[i].Imported + ": " + imp.Names[i].Local);
		}

		std::string returned;
		for (size_t i = 0; i < module.ExportNames.size(); i++)
		{
			if (i > 0) returned += ", ";
			returned += module.ExportNames[i];
		}

		// Each import gets its own IIFE scope, so two modules exporting the
		// same name never collide and a module's own non-exported helpers
		// never leak into the importing script.
		return "const { " + destructure + " } = (function() {\n" + module.Body +
			"\nreturn { " + returned + " };\n})();\n";
	}

	// Recursively resolves `path`'s own imports (a module may import
	// another module), filling `dependencies` with every module path
	// touched anywhere in the chain -- the byproduct PlayMode writes to
	// the dependency manifest. `stack` is the set of paths currently being
	// resolved; a path already in it is a cycle, reported as an error
	// rather than looped on forever.
	bool ResolveModule(const std::string& path, std::set<std::string>& stack,
		std::set<std::string>& dependencies, std::string& error, ResolvedModule*& out)
	{
		auto cached = m_ModuleCache.find(path);
		if (cached != m_ModuleCache.end())
		{
			out = &cached->second;
			return true;
		}

		if (stack.count(path))
		{
			error = "circular import involving '" + path + "'";
			return false;
		}
		stack.insert(path);

		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file.is_open())
		{
			error = "could not open imported module '" + path + "'";
			stack.erase(path);
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();

		ModuleAnalysis analysis;
		if (!AnalyzeModule(buffer.str(), error, analysis))
		{
			stack.erase(path);
			return false;
		}

		std::string assembled;
		for (const ImportInfo& imp : analysis.Imports)
		{
			std::string resolvedPath = ResolveImportPath(path, imp.Specifier);
			if (resolvedPath.empty())
			{
				error = "could not resolve import '" + imp.Specifier + "' from '" + path + "'";
				stack.erase(path);
				return false;
			}
			dependencies.insert(resolvedPath);

			ResolvedModule* nested = nullptr;
			if (!ResolveModule(resolvedPath, stack, dependencies, error, nested))
			{
				stack.erase(path);
				return false;
			}

			for (const ImportName& name : imp.Names)
			{
				if (std::find(nested->ExportNames.begin(), nested->ExportNames.end(), name.Imported) == nested->ExportNames.end())
				{
					error = "'" + name.Imported + "' is not exported by the module imported from '" + imp.Specifier + "' (in '" + path + "')";
					stack.erase(path);
					return false;
				}
			}

			assembled += SpliceImport(imp, *nested);
		}
		assembled += analysis.Body;

		stack.erase(path);

		ResolvedModule resolved;
		resolved.Body = assembled;
		resolved.ExportNames = analysis.ExportNames;
		auto inserted = m_ModuleCache.emplace(path, std::move(resolved));
		out = &inserted.first->second;
		return true;
	}

private:
	bool Initialize()
	{
		if (m_InitFailed)
			return false;

		m_Runtime = JS_NewRuntime();
		if (!m_Runtime)
		{
			m_InitFailed = true;
			return false;
		}
		JS_SetMemoryLimit(m_Runtime, (size_t)512 * 1024 * 1024);
		JS_SetRuntimeOpaque(m_Runtime, this);
		JS_SetInterruptHandler(m_Runtime, InterruptHandler, this);
		EnsureEntityClassRegistered();

		m_Context = JS_NewContext(m_Runtime);
		if (!m_Context)
		{
			JS_FreeRuntime(m_Runtime);
			m_Runtime = nullptr;
			m_InitFailed = true;
			return false;
		}

		JSValue global = JS_GetGlobalObject(m_Context);
		JSValue consoleObj = JS_NewObject(m_Context);
		JS_SetPropertyStr(m_Context, consoleObj, "log", JS_NewCFunction(m_Context, NativeConsoleLog, "log", 1));
		JS_SetPropertyStr(m_Context, consoleObj, "error", JS_NewCFunction(m_Context, NativeConsoleLog, "error", 1));
		JS_SetPropertyStr(m_Context, consoleObj, "warn", JS_NewCFunction(m_Context, NativeConsoleLog, "warn", 1));
		JS_SetPropertyStr(m_Context, global, "console", consoleObj);
		JS_FreeValue(m_Context, global);

		std::ifstream tsFile("assets/typescript.js", std::ios::in | std::ios::binary);
		if (!tsFile.is_open())
		{
			GS_ERROR("ScriptEngine: could not open assets/typescript.js");
			JS_FreeContext(m_Context);
			m_Context = nullptr;
			JS_FreeRuntime(m_Runtime);
			m_Runtime = nullptr;
			m_InitFailed = true;
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
			JS_FreeContext(m_Context);
			m_Context = nullptr;
			JS_FreeRuntime(m_Runtime);
			m_Runtime = nullptr;
			m_InitFailed = true;
			return false;
		}
		JS_FreeValue(m_Context, loadResult);

		// Created only here, past every failure path above -- m_InputObject
		// used to be set up right after JS_NewContext, which left it on the
		// runtime's GC object list on both the missing-typescript.js and
		// failed-eval paths below, each of which frees the context/runtime
		// without ever freeing m_InputObject first. JS_FreeRuntime asserts
		// (quickjs.c:2704) if any object is still alive when it runs -- the
		// same crash class Task 4 already fixed at a different site. Once
		// initialization can no longer fail past this point, there's nothing
		// left for it to leak against.
		m_InputObject = JS_NewObject(m_Context);
		JS_SetPropertyStr(m_Context, m_InputObject, "isKeyDown", JS_NewCFunction(m_Context, Native_IsKeyDown, "isKeyDown", 1));

		m_SceneObject = JS_NewObject(m_Context);
		JS_SetPropertyStr(m_Context, m_SceneObject, "findByTag", JS_NewCFunction(m_Context, Native_SceneFindByTag, "findByTag", 1));
		JS_SetPropertyStr(m_Context, m_SceneObject, "spawn", JS_NewCFunction(m_Context, Native_SceneSpawn, "spawn", 1));
		JS_SetPropertyStr(m_Context, m_SceneObject, "destroy", JS_NewCFunction(m_Context, Native_SceneDestroy, "destroy", 1));

		m_Ready = true;
		return true;
	}

	static int InterruptHandler(JSRuntime*, void* opaque)
	{
		ScriptEngine* self = (ScriptEngine*)opaque;
		return std::chrono::steady_clock::now() > self->m_Deadline ? 1 : 0;
	}

	static JSValue NativeConsoleLog(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv)
	{
		ScriptEngine* self = (ScriptEngine*)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));

		if (self->m_CapturedOutput.size() >= kOutputCap)
		{
			if (!self->m_OutputTruncated)
			{
				self->m_CapturedOutput += "\n[output truncated at 4 MB]\n";
				self->m_OutputTruncated = true;
			}
			return JS_UNDEFINED;
		}

		for (int i = 0; i < argc; i++)
		{
			if (i > 0)
				self->m_CapturedOutput += " ";
			const char* s = JS_ToCString(ctx, argv[i]);
			if (!s)
			{
				self->m_CapturedOutput += "[unprintable]";
				continue;
			}
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

	// Transpile-only: turns TypeScript source into a { ok, error, code }
	// result and nothing more. The IIFE wrap used to live in this string's
	// last line, but entity scripts need a different wrapper around the same
	// transpiled code -- one that takes (entity, input) as parameters instead
	// of self-executing -- so the wrap moved out to each call site instead
	// (RunScript wraps in a bare self-executing IIFE; PrepareEntityScript
	// wraps in a function that takes entity/input as parameters and returns
	// the script's OnStart/OnUpdate). Either wrapper gives each Run/Prepare
	// its own fresh function scope -- without one, QuickJS's
	// global-lexical-declaration check throws "SyntaxError: redeclaration of
	// '<name>'" the second time a top-level const/let/class name is evaluated
	// at global scope (standard ECMAScript behavior, same as a browser
	// console), which broke the edit-run-edit-run loop on the second click of
	// any idiomatic const/class-heavy script. This trades away context
	// persistence across runs -- a deliberate design choice originally --
	// because that turned out net-negative next to just letting every run
	// start clean.
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

	// Finds a source's top-level `import`/`export` statements using the
	// same bundled TypeScript compiler s_TranspileSrc already loads --
	// its real parser, not a hand-rolled scan that would mis-handle a
	// string literal or comment containing the word "import". Runs
	// *before* transpiling: type-stripping isn't needed yet, and running
	// it after would have already thrown on real `import`/`export` syntax,
	// which plain-script `ts.transpileModule` (module: ts.ModuleKind.None)
	// has nothing to lower it into.
	//
	// Only `import { a, b as c } from "./relative/path"` and top-level
	// `export function`/`export const` are understood -- anything else
	// (default exports, `import * as`, destructured export declarations,
	// a specifier that isn't a plain string literal) is reported as an
	// unsupported form rather than silently doing the wrong thing.
	static constexpr const char* s_ModuleAnalysisSrc = R"JS(
		(function(source) {
			try {
				var sourceFile = ts.createSourceFile("module.ts", source, ts.ScriptTarget.ES2020, true);
				var imports = [];
				var exportNames = [];
				var edits = [];

				function stripKeyword(node) {
					var start = node.getStart(sourceFile);
					var end = node.end;
					while (end < source.length && (source[end] === ' ' || source[end] === '\t'))
						end++;
					edits.push({ start: start, end: end, replacement: "" });
				}

				var statements = sourceFile.statements;
				for (var i = 0; i < statements.length; i++) {
					var stmt = statements[i];

					if (stmt.kind === ts.SyntaxKind.ImportDeclaration) {
						if (!stmt.moduleSpecifier || typeof stmt.moduleSpecifier.text !== "string")
							return { ok: false, error: "unsupported import form -- only `import { a, b } from \"...\"` is understood" };

						var bindings = stmt.importClause && stmt.importClause.namedBindings;
						if (!bindings || !bindings.elements)
							return { ok: false, error: "unsupported import form -- only named imports are understood, e.g. `import { a } from \"./b\"`" };

						var names = [];
						for (var e = 0; e < bindings.elements.length; e++) {
							var el = bindings.elements[e];
							names.push({
								imported: el.propertyName ? el.propertyName.text : el.name.text,
								local: el.name.text
							});
						}
						imports.push({ specifier: stmt.moduleSpecifier.text, names: names });
						edits.push({ start: stmt.getStart(sourceFile), end: stmt.end, replacement: "" });
						continue;
					}

					var exportKeyword = null;
					if (stmt.modifiers) {
						for (var m = 0; m < stmt.modifiers.length; m++) {
							if (stmt.modifiers[m].kind === ts.SyntaxKind.ExportKeyword) { exportKeyword = stmt.modifiers[m]; break; }
						}
					}
					if (!exportKeyword)
						continue;

					if (stmt.kind === ts.SyntaxKind.FunctionDeclaration && stmt.name) {
						exportNames.push(stmt.name.text);
					} else if (stmt.kind === ts.SyntaxKind.VariableStatement) {
						var decls = stmt.declarationList.declarations;
						for (var d = 0; d < decls.length; d++) {
							if (!decls[d].name || decls[d].name.kind !== ts.SyntaxKind.Identifier)
								return { ok: false, error: "unsupported export form -- only a plain name may be exported, not a destructuring pattern" };
							exportNames.push(decls[d].name.text);
						}
					} else {
						return { ok: false, error: "unsupported export form -- only `export function` and `export const`/`let` are understood" };
					}
					stripKeyword(exportKeyword);
				}

				edits.sort(function(a, b) { return b.start - a.start; });
				var body = source;
				for (var k = 0; k < edits.length; k++)
					body = body.slice(0, edits[k].start) + edits[k].replacement + body.slice(edits[k].end);

				return { ok: true, error: null, imports: imports, exportNames: exportNames, body: body };
			} catch (e) {
				return { ok: false, error: "module analysis failed: " + (e && e.message ? e.message : String(e)) };
			}
		})
	)JS";

	// GSS-to-C++ codegen (Task 1 of the transpiler plan -- see
	// docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md).
	// Mechanical, not semantic: every construct below is a direct,
	// context-free syntax mapping. Nothing here infers a type that wasn't
	// explicitly annotated, and nothing here approximates a construct it
	// doesn't recognize -- nodes outside this list fail with a specific,
	// named error instead.
	//
	// Every top-level function (OnStart/OnUpdate and any other
	// script-defined helper) gets `GS::Entity entity, GS::Scene& scene`
	// prepended to its real parameter list, and every call to one of them
	// gets `entity, scene` prepended as arguments -- this is what
	// preserves GSS's closure semantics (every top-level function shares
	// the same entity/input/scene in the interpreted world) once each
	// becomes its own separate C++ method with no implicit shared scope.
	static constexpr const char* s_CodegenSrc = R"JS(
		(function(source) {
			function fail(node, message) {
				var lc = sourceFile.getLineAndCharacterOfPosition(node.getStart ? node.getStart(sourceFile) : node.pos);
				var err = new Error(message + " (line " + (lc.line + 1) + ")");
				err.isCodegenFailure = true;
				throw err;
			}

			function typeToCpp(typeNode, ctxNode, ctxWhat) {
				if (!typeNode) fail(ctxNode, "missing type annotation on " + ctxWhat);
				var t = typeNode.getText(sourceFile);
				if (t === "number") return "double";
				if (t === "boolean") return "bool";
				if (t === "string") return "std::string";
				if (t === "void") return "void";
				return t;
			}

			var mathFns = {
				abs: "std::abs", min: "std::min", max: "std::max", sqrt: "std::sqrt",
				floor: "std::floor", ceil: "std::ceil", round: "std::round", pow: "std::pow",
				sin: "std::sin", cos: "std::cos", tan: "std::tan"
			};

			function keyNameToMacro(name) {
				if (name.length === 1) {
					if (name >= "A" && name <= "Z") return "GS_KEY_" + name;
					if (name >= "0" && name <= "9") return "GS_KEY_" + name;
				}
				if (name === "SPACE" || name === "LEFT" || name === "RIGHT" || name === "UP" || name === "DOWN")
					return "GS_KEY_" + name;
				return null;
			}

			var topLevelFunctionNames = {};

			// A best-effort, never-reset symbol table (name -> its C++
			// type, e.g. "double"/"std::string"/"bool", or "auto" when
			// unknown) used only to decide how to stringify a template
			// literal's ${...} interpolation. It doesn't model real
			// lexical scoping -- a nested arrow function's parameters are
			// merged into the same table as their enclosing function's
			// locals, so a name reused across nested scopes can shadow
			// incorrectly here. Low-stakes: the only consequence is a
			// template literal being rejected (or, rarely, stringified
			// using a stale type) in an edge case, not a wrong program.
			var currentVarTypes = {};

			function isNamed(node, name) {
				return node.kind === ts.SyntaxKind.Identifier && node.text === name;
			}

			var binOps = {};
			binOps[ts.SyntaxKind.PlusToken] = "+";
			binOps[ts.SyntaxKind.MinusToken] = "-";
			binOps[ts.SyntaxKind.AsteriskToken] = "*";
			binOps[ts.SyntaxKind.SlashToken] = "/";
			binOps[ts.SyntaxKind.PercentToken] = "%";
			binOps[ts.SyntaxKind.LessThanToken] = "<";
			binOps[ts.SyntaxKind.GreaterThanToken] = ">";
			binOps[ts.SyntaxKind.LessThanEqualsToken] = "<=";
			binOps[ts.SyntaxKind.GreaterThanEqualsToken] = ">=";
			binOps[ts.SyntaxKind.AmpersandAmpersandToken] = "&&";
			binOps[ts.SyntaxKind.BarBarToken] = "||";
			binOps[ts.SyntaxKind.EqualsToken] = "=";
			binOps[ts.SyntaxKind.PlusEqualsToken] = "+=";
			binOps[ts.SyntaxKind.MinusEqualsToken] = "-=";
			binOps[ts.SyntaxKind.AsteriskEqualsToken] = "*=";
			binOps[ts.SyntaxKind.SlashEqualsToken] = "/=";

			function emitBinary(node) {
				var opKind = node.operatorToken.kind;
				if (opKind === ts.SyntaxKind.EqualsEqualsToken || opKind === ts.SyntaxKind.ExclamationEqualsToken)
					fail(node, "'==' / '!=' are unsupported -- use '===' / '!==' (JS loose equality has no honest C++ translation)");
				if (opKind === ts.SyntaxKind.EqualsEqualsEqualsToken)
					return emitExpr(node.left) + " == " + emitExpr(node.right);
				if (opKind === ts.SyntaxKind.ExclamationEqualsEqualsToken)
					return emitExpr(node.left) + " != " + emitExpr(node.right);
				var op = binOps[opKind];
				if (!op) fail(node, "unsupported operator");
				return emitExpr(node.left) + " " + op + " " + emitExpr(node.right);
			}

			var transformGetter = { getPosition: "Position", getRotation: "Rotation", getScale: "Scale" };
			var transformSetter = { setPosition: "Position", setRotation: "Rotation", setScale: "Scale" };

			function emitArgs(args) { return args.map(emitExpr).join(", "); }

			function emitCall(node) {
				var callee = node.expression;
				var args = node.arguments;

				if (callee.kind === ts.SyntaxKind.PropertyAccessExpression) {
					var obj = callee.expression;
					var member = callee.name.text;

					if (obj.kind === ts.SyntaxKind.ThisKeyword)
						return "this->" + member + "(" + emitArgs(args) + ")";

					if (isNamed(obj, "entity")) {
						if (transformGetter[member]) {
							if (args.length !== 0) fail(node, member + " takes no arguments");
							return "entity.Get<GS::TransformComponent>()->" + transformGetter[member];
						}
						if (transformSetter[member]) {
							if (args.length !== 3) fail(node, member + " needs exactly 3 arguments");
							return "entity.Get<GS::TransformComponent>()->" + transformSetter[member] +
								" = glm::vec3(" + emitArgs(args) + ")";
						}
						fail(node, "unsupported entity.* call '" + member + "'");
					}

					if (isNamed(obj, "input")) {
						if (member === "isKeyDown") {
							if (args.length !== 1 || args[0].kind !== ts.SyntaxKind.StringLiteral)
								fail(node, "input.isKeyDown needs a literal key name, e.g. input.isKeyDown(\"A\")");
							var macro = keyNameToMacro(args[0].text);
							if (!macro) fail(node, "unrecognized key name '" + args[0].text + "'");
							return "GS::Input::IsKeyPressed(" + macro + ")";
						}
						fail(node, "unsupported input.* call '" + member + "'");
					}

					if (isNamed(obj, "scene")) {
						// Reproduces Native_SceneFindByTag/Native_SceneDestroy's
						// own logic rather than calling a shared helper -- no
						// such helper exists yet (both are small, inlined
						// native functions in this same file). Extracting one
						// so the interpreted and compiled paths can't drift is
						// a good follow-up, not done here.
						if (member === "findByTag") {
							if (args.length !== 1) fail(node, "scene.findByTag needs exactly 1 argument");
							return "([&]() -> GS::Entity { for (GS::EntityId id : scene.GetEntities()) { "
								+ "auto* tag = scene.GetComponent<GS::TagComponent>(id); "
								+ "if (tag && tag->Name == (" + emitExpr(args[0]) + ")) return scene.Wrap(id); } "
								+ "return GS::Entity(); }())";
						}
						if (member === "destroy") {
							if (args.length !== 1) fail(node, "scene.destroy needs exactly 1 argument");
							return "scene.DestroyEntity((" + emitExpr(args[0]) + ").GetId())";
						}
						fail(node, "unsupported scene.* call '" + member + "' (scene.spawn isn't implemented in the transpiler yet)");
					}

					if (isNamed(obj, "Math")) {
						var cppFn = mathFns[member];
						if (!cppFn) fail(node, "unsupported Math." + member);
						return cppFn + "(" + emitArgs(args) + ")";
					}

					fail(node, "unsupported method call '" + member + "'");
				}

				if (callee.kind === ts.SyntaxKind.Identifier) {
					// A known top-level function (or top-level
					// const-assigned arrow) gets entity/scene prepended,
					// same as its own declaration does -- that's what
					// preserves GSS's shared-closure semantics once it
					// becomes a separate C++ method with no implicit
					// shared scope. Anything else reaching here is assumed
					// to be a local variable holding a callable (a `[&]`
					// lambda, which already captures its enclosing scope
					// and needs nothing threaded through) -- called
					// directly, with no prefix. If it isn't actually
					// callable, g++'s syntax-check catches that, not this.
					if (topLevelFunctionNames[callee.text]) {
						var prefix = "entity, scene" + (args.length ? ", " : "");
						return callee.text + "(" + prefix + emitArgs(args) + ")";
					}
					if (currentVarTypes[callee.text] !== undefined)
						return callee.text + "(" + emitArgs(args) + ")";
					fail(node, "calling '" + callee.text + "', which isn't a recognized top-level function, local variable, or the "
						+ "native API (imported functions aren't supported by codegen yet)");
				}

				fail(node, "unsupported call form");
			}

			function emitExpr(node) {
				switch (node.kind) {
					case ts.SyntaxKind.NumericLiteral: return node.text;
					case ts.SyntaxKind.StringLiteral: return JSON.stringify(node.text);
					case ts.SyntaxKind.TrueKeyword: return "true";
					case ts.SyntaxKind.FalseKeyword: return "false";
					case ts.SyntaxKind.Identifier: return node.text;
					case ts.SyntaxKind.ParenthesizedExpression: return "(" + emitExpr(node.expression) + ")";
					case ts.SyntaxKind.PrefixUnaryExpression: {
						if (node.operator === ts.SyntaxKind.ExclamationToken) return "!" + emitExpr(node.operand);
						if (node.operator === ts.SyntaxKind.MinusToken) return "-" + emitExpr(node.operand);
						if (node.operator === ts.SyntaxKind.PlusPlusToken) return "++" + emitExpr(node.operand);
						if (node.operator === ts.SyntaxKind.MinusMinusToken) return "--" + emitExpr(node.operand);
						fail(node, "unsupported prefix operator");
						break;
					}
					case ts.SyntaxKind.PostfixUnaryExpression: {
						if (node.operator === ts.SyntaxKind.PlusPlusToken) return emitExpr(node.operand) + "++";
						if (node.operator === ts.SyntaxKind.MinusMinusToken) return emitExpr(node.operand) + "--";
						fail(node, "unsupported postfix operator");
						break;
					}
					case ts.SyntaxKind.BinaryExpression: return emitBinary(node);
					case ts.SyntaxKind.ConditionalExpression:
						return emitExpr(node.condition) + " ? " + emitExpr(node.whenTrue) + " : " + emitExpr(node.whenFalse);
					case ts.SyntaxKind.ElementAccessExpression:
						return emitExpr(node.expression) + "[" + emitExpr(node.argumentExpression) + "]";
					case ts.SyntaxKind.ThisKeyword: return "this";
					case ts.SyntaxKind.PropertyAccessExpression: {
						// Only `this.field` -- a bare (non-call) property
						// access on anything else (entity.foo without a
						// call, a class instance's field from outside its
						// own methods) isn't recognized. Calls are handled
						// separately in emitCall, which is reached first
						// for a CallExpression regardless of what its
						// callee looks like.
						if (node.expression.kind === ts.SyntaxKind.ThisKeyword)
							return "this->" + node.name.text;
						fail(node, "unsupported property access '" + node.name.text + "' outside of a recognized call");
						break;
					}
					case ts.SyntaxKind.CallExpression: return emitCall(node);
					case ts.SyntaxKind.NoSubstitutionTemplateLiteral:
					case ts.SyntaxKind.TemplateExpression:
						return emitTemplateLiteral(node);
					case ts.SyntaxKind.ArrowFunction:
						return emitArrowFunction(node);
					case ts.SyntaxKind.ArrayLiteralExpression:
						fail(node, "array literals are unsupported (index into an existing array-like value instead)");
						break;
					default:
						fail(node, "unsupported expression form (kind " + node.kind + ")");
				}
			}

			// For a *local* declaration (this function; never used for a
			// top-level one, which becomes a class member and can't use
			// `auto` in C++), a missing annotation falls back to `auto`
			// when there's an initializer to deduce from -- e.g.
			// `const pos = entity.getPosition();` has no TS declaration
			// file to write a correct annotation against at all, since the
			// native API isn't typed. This still isn't type inference on
			// this transpiler's own part: `auto` is deduced by the C++
			// compiler at the syntax-check step, not reasoned about here.
			// An annotation is still required when there's no initializer
			// (`let x: number;`) -- nothing for `auto` to deduce from.
			function emitVarDecl(decl, isConst) {
				if (decl.name.kind !== ts.SyntaxKind.Identifier)
					fail(decl, "destructuring is unsupported -- declare a plain name and index into it instead");
				var cppType = decl.type
					? typeToCpp(decl.type, decl, "'" + decl.name.text + "'")
					: (decl.initializer ? "auto" : fail(decl, "missing type annotation on '" + decl.name.text + "' (no initializer to deduce it from)"));
				currentVarTypes[decl.name.text] = cppType;
				var init = decl.initializer ? (" = " + emitExpr(decl.initializer)) : "";
				return (isConst ? "const " : "") + cppType + " " + decl.name.text + init;
			}

			function emitStmtAsBlock(node, indent) {
				if (node.kind === ts.SyntaxKind.Block) return emitBlockStmt(node, indent);
				return indent + "{\n" + emitStmt(node, indent + "\t") + "\n" + indent + "}";
			}

			function emitBlockStmt(block, indent) {
				var inner = indent + "\t";
				var lines = block.statements.map(function(s) { return emitStmt(s, inner); });
				return indent + "{\n" + lines.join("\n") + "\n" + indent + "}";
			}

			function emitStmt(node, indent) {
				switch (node.kind) {
					case ts.SyntaxKind.VariableStatement: {
						var isConst = (node.declarationList.flags & ts.NodeFlags.Const) !== 0;
						var decls = node.declarationList.declarations.map(function(d) { return emitVarDecl(d, isConst); });
						return indent + decls.join(", ") + ";";
					}
					case ts.SyntaxKind.ExpressionStatement:
						return indent + emitExpr(node.expression) + ";";
					case ts.SyntaxKind.ReturnStatement:
						return indent + "return" + (node.expression ? (" " + emitExpr(node.expression)) : "") + ";";
					case ts.SyntaxKind.IfStatement: {
						var s = indent + "if (" + emitExpr(node.expression) + ")\n" + emitStmtAsBlock(node.thenStatement, indent);
						if (node.elseStatement)
							s += "\n" + indent + "else\n" + emitStmtAsBlock(node.elseStatement, indent);
						return s;
					}
					case ts.SyntaxKind.WhileStatement:
						return indent + "while (" + emitExpr(node.expression) + ")\n" + emitStmtAsBlock(node.statement, indent);
					case ts.SyntaxKind.ForStatement: {
						var init = "";
						if (node.initializer) {
							init = (node.initializer.kind === ts.SyntaxKind.VariableDeclarationList)
								? node.initializer.declarations.map(function(d) { return emitVarDecl(d, false); }).join(", ")
								: emitExpr(node.initializer);
						}
						var cond = node.condition ? emitExpr(node.condition) : "";
						var incr = node.incrementor ? emitExpr(node.incrementor) : "";
						return indent + "for (" + init + "; " + cond + "; " + incr + ")\n" + emitStmtAsBlock(node.statement, indent);
					}
					case ts.SyntaxKind.Block:
						return emitBlockStmt(node, indent);
					default:
						fail(node, "unsupported statement form (kind " + node.kind + ")");
				}
			}

			function paramList(params) {
				return params.map(function(p) {
					if (p.name.kind !== ts.SyntaxKind.Identifier)
						fail(p, "destructured parameters are unsupported");
					var cppType = typeToCpp(p.type, p, "parameter '" + p.name.text + "'");
					currentVarTypes[p.name.text] = cppType;
					return cppType + " " + p.name.text;
				}).join(", ");
			}

			// Template-literal interpolation is scoped to a plain
			// identifier or literal with a known type -- not a general
			// expression -- since deciding how to stringify an arbitrary
			// expression would mean tracking types through the same
			// binary/call/property-access forms emitExpr already handles,
			// which is real type inference, not the mechanical lookup this
			// is meant to stay.
			function stringifyForTemplate(node) {
				if (node.kind === ts.SyntaxKind.StringLiteral) return JSON.stringify(node.text);
				if (node.kind === ts.SyntaxKind.NumericLiteral) return "std::to_string(" + node.text + ")";
				if (node.kind === ts.SyntaxKind.Identifier) {
					var t = currentVarTypes[node.text];
					if (t === "std::string") return node.text;
					if (t === "double") return "std::to_string(" + node.text + ")";
					if (t === "bool") return "(" + node.text + " ? \"true\" : \"false\")";
					fail(node, "cannot interpolate '" + node.text + "' in a template literal -- its type isn't known to the transpiler "
						+ "(give it an explicit number/string/boolean annotation)");
				}
				fail(node, "template literal interpolation only supports a plain identifier or literal, not a general expression");
			}

			function emitTemplateLiteral(node) {
				if (node.kind === ts.SyntaxKind.NoSubstitutionTemplateLiteral)
					return JSON.stringify(node.text);

				var parts = [JSON.stringify(node.head.text)];
				for (var i = 0; i < node.templateSpans.length; i++) {
					parts.push(stringifyForTemplate(node.templateSpans[i].expression));
					parts.push(JSON.stringify(node.templateSpans[i].literal.text));
				}
				// The head is always wrapped in std::string(...) so the
				// rest of the chain is guaranteed std::string + ...
				// concatenation regardless of how many parts follow --
				// two bare C-string literals can't be added with operator+
				// in C++, but std::string + anything string-like can.
				var result = "std::string(" + parts[0] + ")";
				for (var j = 1; j < parts.length; j++)
					result += " + " + parts[j];
				return result;
			}

			// [&]: captures its enclosing scope by reference, which is
			// what lets an inline lambda reach entity/scene/local variables
			// from the function it's written inside -- unlike a top-level
			// arrow function (handled separately, below, as a named
			// method), which has no enclosing C++ scope to capture at all.
			function emitArrowFunction(node) {
				var params = paramList(node.parameters);
				if (node.body.kind === ts.SyntaxKind.Block)
					return "[&](" + params + ") " + emitBlockStmt(node.body, "\t");
				return "[&](" + params + ") { return " + emitExpr(node.body) + "; }";
			}

			function hasModifier(node, kind) {
				return !!(node.modifiers && node.modifiers.some(function(m) { return m.kind === kind; }));
			}

			// `<T>` and `<T extends number>` both become plain
			// `template<typename T>` -- any constraint is dropped, not
			// translated, regardless of how simple or complex it is.
			// C++17 (this project's dialect) has no `concept` keyword to
			// express one with anyway; enforcing "T extends number" would
			// need real machinery (SFINAE, static_assert against a trait)
			// this mechanical transpiler doesn't build. A disclosed gap,
			// not a silent one: the constraint simply isn't enforced in
			// the generated code.
			function typeParamClause(typeParameters) {
				if (!typeParameters || typeParameters.length === 0) return "";
				return "template<" + typeParameters.map(function(tp) { return "typename " + tp.name.text; }).join(", ") + ">\n";
			}

			// A top-level class is its own independent C++ class, a
			// sibling of the entity's own generated class in the same
			// GeneratedScripts namespace -- not nested inside it, and its
			// methods do *not* get entity/scene prepended the way
			// top-level functions do. A class is a general-purpose data
			// structure (a Timer, a small math type); it has no relation
			// to the entity-script closure model that prepending exists
			// to preserve. If a method genuinely needs entity/scene, they
			// have to be declared as ordinary parameters, same as any
			// other value the caller wants to pass in.
			//
			// Everything comes out `public:` -- access modifiers and
			// `readonly` aren't translated. A human refining the generated
			// code by hand can add real encapsulation; the transpiler's
			// job here is a correct starting point, not a finished one.
			function emitClass(node) {
				if (hasModifier(node, ts.SyntaxKind.AbstractKeyword))
					fail(node, "abstract classes are unsupported");
				if (!node.name) fail(node, "an unnamed class is unsupported");
				// Generic functions are in scope (see typeParamClause);
				// generic classes are not -- rejected explicitly here
				// rather than silently emitting `T` as a field type with
				// no enclosing `template<>` to declare it, which would
				// fail at the syntax-check step with a confusing error
				// instead of this clear, named one.
				if (node.typeParameters && node.typeParameters.length)
					fail(node, "generic classes are unsupported");

				var baseClause = "";
				if (node.heritageClauses) {
					for (var h = 0; h < node.heritageClauses.length; h++) {
						var clause = node.heritageClauses[h];
						if (clause.token === ts.SyntaxKind.ImplementsKeyword)
							fail(node, "'implements' is unsupported -- only single 'extends' inheritance is");
						if (clause.token === ts.SyntaxKind.ExtendsKeyword) {
							if (clause.types.length !== 1) fail(node, "multiple inheritance is unsupported");
							baseClause = " : public " + clause.types[0].expression.getText(sourceFile);
						}
					}
				}

				var fields = [];
				var ctor = null;
				var methods = [];

				for (var m = 0; m < node.members.length; m++) {
					var member = node.members[m];
					if (hasModifier(member, ts.SyntaxKind.StaticKeyword))
						fail(member, "static class members are unsupported");

					if (member.kind === ts.SyntaxKind.PropertyDeclaration) {
						if (member.name.kind !== ts.SyntaxKind.Identifier)
							fail(member, "a computed or destructured field name is unsupported");
						var fieldType = typeToCpp(member.type, member, "field '" + member.name.text + "'");
						currentVarTypes[member.name.text] = fieldType;
						var fieldInit = member.initializer ? (" = " + emitExpr(member.initializer)) : "";
						fields.push(fieldType + " " + member.name.text + fieldInit + ";");
						continue;
					}

					if (member.kind === ts.SyntaxKind.Constructor) {
						if (ctor) fail(member, "multiple constructors are unsupported");
						if (!member.body) fail(member, "a constructor needs a body");
						ctor = { params: paramList(member.parameters), body: emitBlockStmt(member.body, "\t\t") };
						continue;
					}

					if (member.kind === ts.SyntaxKind.MethodDeclaration) {
						if (!member.body) fail(member, "a method needs a body");
						if (member.name.kind !== ts.SyntaxKind.Identifier)
							fail(member, "a computed method name is unsupported");
						var methodReturnType = member.type
							? typeToCpp(member.type, member, "'" + member.name.text + "' return value")
							: "auto";
						methods.push({
							returnType: methodReturnType,
							name: member.name.text,
							params: paramList(member.parameters),
							body: emitBlockStmt(member.body, "\t\t")
						});
						continue;
					}

					fail(member, "unsupported class member form (getters/setters and decorators aren't supported)");
				}

				return { name: node.name.text, baseClause: baseClause, fields: fields, ctor: ctor, methods: methods };
			}

			try {
				var sourceFile = ts.createSourceFile("script.ts", source, ts.ScriptTarget.ES2020, true);
				var statements = sourceFile.statements;

				for (var pass = 0; pass < statements.length; pass++) {
					var passStmt = statements[pass];
					if (passStmt.kind === ts.SyntaxKind.FunctionDeclaration && passStmt.name) {
						topLevelFunctionNames[passStmt.name.text] = true;
					} else if (passStmt.kind === ts.SyntaxKind.VariableStatement) {
						for (var pd = 0; pd < passStmt.declarationList.declarations.length; pd++) {
							var pdecl = passStmt.declarationList.declarations[pd];
							if (pdecl.name.kind === ts.SyntaxKind.Identifier && pdecl.initializer
								&& pdecl.initializer.kind === ts.SyntaxKind.ArrowFunction)
								topLevelFunctionNames[pdecl.name.text] = true;
						}
					}
				}

				var members = [];
				var methods = [];
				var classes = [];

				for (var i = 0; i < statements.length; i++) {
					var stmt = statements[i];

					if (stmt.kind === ts.SyntaxKind.FunctionDeclaration) {
						if (!stmt.body) fail(stmt, "a function declaration needs a body");
						var name = stmt.name.text;
						var isEntryPoint = (name === "OnStart" || name === "OnUpdate");
						if (isEntryPoint && stmt.typeParameters && stmt.typeParameters.length)
							fail(stmt, "OnStart/OnUpdate cannot be generic");
						var returnType = stmt.type ? typeToCpp(stmt.type, stmt, "'" + name + "' return value") : "void";
						var ownParams = paramList(stmt.parameters);
						methods.push({
							isPublic: isEntryPoint,
							templateClause: typeParamClause(stmt.typeParameters),
							returnType: returnType,
							name: name,
							params: "GS::Entity entity, GS::Scene& scene" + (ownParams ? ", " + ownParams : ""),
							body: emitBlockStmt(stmt.body, "\t\t")
						});
						continue;
					}

					if (stmt.kind === ts.SyntaxKind.VariableStatement) {
						for (var d = 0; d < stmt.declarationList.declarations.length; d++) {
							var decl = stmt.declarationList.declarations[d];
							if (decl.name.kind !== ts.SyntaxKind.Identifier)
								fail(decl, "destructuring is unsupported at top level either");

							// `const helper = (x: number) => ...` is a named
							// function, not data -- treated exactly like a
							// `function helper(...)` declaration, entity/scene
							// prepended the same way.
							if (decl.initializer && decl.initializer.kind === ts.SyntaxKind.ArrowFunction) {
								var arrowName = decl.name.text;
								var arrow = decl.initializer;
								var arrowIsEntryPoint = (arrowName === "OnStart" || arrowName === "OnUpdate");
								if (arrowIsEntryPoint && arrow.typeParameters && arrow.typeParameters.length)
									fail(arrow, "OnStart/OnUpdate cannot be generic");
								var arrowReturnType = arrow.type
									? typeToCpp(arrow.type, arrow, "'" + arrowName + "' return value")
									: "auto";   // deduced from its return statement(s), or void if it has none
								var arrowOwnParams = paramList(arrow.parameters);
								var arrowBody = (arrow.body.kind === ts.SyntaxKind.Block)
									? emitBlockStmt(arrow.body, "\t\t")
									: "\t\t{\n\t\t\treturn " + emitExpr(arrow.body) + ";\n\t\t}";
								methods.push({
									isPublic: arrowIsEntryPoint,
									templateClause: typeParamClause(arrow.typeParameters),
									returnType: arrowReturnType,
									name: arrowName,
									params: "GS::Entity entity, GS::Scene& scene" + (arrowOwnParams ? ", " + arrowOwnParams : ""),
									body: arrowBody
								});
								continue;
							}

							members.push({
								type: typeToCpp(decl.type, decl, "'" + decl.name.text + "'"),
								name: decl.name.text,
								init: decl.initializer ? emitExpr(decl.initializer) : null
							});
						}
						continue;
					}

					if (stmt.kind === ts.SyntaxKind.ClassDeclaration) {
						classes.push(emitClass(stmt));
						continue;
					}

					fail(stmt, "unsupported top-level statement form (kind " + stmt.kind + ")");
				}

				return { ok: true, error: null, members: members, methods: methods, classes: classes };
			} catch (e) {
				if (e && e.isCodegenFailure) return { ok: false, error: e.message, members: [], methods: [], classes: [] };
				return { ok: false, error: "codegen failed: " + (e && e.message ? e.message : String(e)), members: [], methods: [], classes: [] };
			}
		})
	)JS";

	JSRuntime* m_Runtime = nullptr;
	JSContext* m_Context = nullptr;
	bool m_Ready = false;
	bool m_InitFailed = false;
	JSValue m_InputObject = JS_UNDEFINED;
	JSValue m_SceneObject = JS_UNDEFINED;
	// Which GS::Scene the native findByTag/spawn/destroy functions operate
	// on. Set fresh in PrepareEntityScript, not at construction -- there is
	// no scene yet when m_SceneObject itself is created in Initialize().
	GS::Scene* m_CurrentScene = nullptr;
	std::string m_CapturedOutput;
	// Defaulted far in the future, not to time_point{} (epoch) -- the interrupt
	// handler is registered inside Initialize() itself (so it can also bound a
	// hung typescript.js load), which runs before RunScript ever sets a real
	// deadline. An epoch-zero default made InterruptHandler's `now() > deadline`
	// true immediately, so the very first typescript.js eval was interrupted
	// before it could finish -- Initialize() always failed, permanently
	// (m_InitFailed latches), and the engine never ran a script. Confirmed by
	// toggling this default during the fix wave's own verification: identical
	// build, only this line differed, and it was the difference between 0/8
	// and 8/8 on the Fix 1 + Fix 2 self-test.
	std::chrono::steady_clock::time_point m_Deadline = std::chrono::steady_clock::time_point::max();

	static constexpr size_t kOutputCap = 4 * 1024 * 1024;
	bool m_OutputTruncated = false;
};
