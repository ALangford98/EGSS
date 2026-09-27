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

	inline void Run()
	{
		g_Pass = 0;
		g_Fail = 0;
		GS_TRACE("GssArrayStdlibTest: starting");
		RunConsoleTests();
		RunNumberArrayTypeTest();
		GS_TRACE("GssArrayStdlibTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
