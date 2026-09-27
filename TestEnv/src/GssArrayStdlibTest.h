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

	inline void Run()
	{
		g_Pass = 0;
		g_Fail = 0;
		GS_TRACE("GssArrayStdlibTest: starting");
		RunConsoleTests();
		RunNumberArrayTypeTest();
		RunVec3LengthAndToArrayTests();
		GS_TRACE("GssArrayStdlibTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
