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

#include <chrono>
#include <cstring>
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

	ScriptEngine() = default;
	ScriptEngine(const ScriptEngine&) = delete;
	ScriptEngine& operator=(const ScriptEngine&) = delete;

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

		// Set after Initialize() so the one-time typescript.js load (up to
		// ~0.65s in Debug) doesn't eat into the script's own time budget.
		m_Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

		m_CapturedOutput.clear();
		m_OutputTruncated = false;

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

	// The transpiled code runs inside its own IIFE so each Run gets a fresh
	// function scope -- without it, QuickJS's global-lexical-declaration check
	// throws "SyntaxError: redeclaration of '<name>'" the second time a
	// top-level const/let/class name is evaluated at global scope (standard
	// ECMAScript behavior, same as a browser console), which broke the
	// edit-run-edit-run loop on the second click of any idiomatic
	// const/class-heavy script. This trades away context persistence across
	// runs -- a deliberate design choice originally -- because that turned out
	// net-negative next to just letting every run start clean.
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
			return { ok: true, error: null, code: '(function(){' + result.outputText + '\n})();' };
		})
	)JS";

	JSRuntime* m_Runtime = nullptr;
	JSContext* m_Context = nullptr;
	bool m_Ready = false;
	bool m_InitFailed = false;
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
