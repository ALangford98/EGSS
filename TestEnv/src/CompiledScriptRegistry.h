#pragma once

// The list of GSS scripts that have graduated from interpreted to compiled.
// PlayMode checks this before running a ScriptComponent: if the entity's
// script matches a name here, its native GeneratedScripts::<Name> class runs
// directly and the matching .gss file is never read or interpreted for that
// entity again.
//
// Deliberately *not* self-registering, same reasoning as DemoRegistry.h: a
// generated .cpp defines its class entirely inline (every method body lives
// inside the class, the shape ScriptEngine::TranspileToCpp always emits), so
// #include-ing it below is enough to link it in -- but forgetting the
// #include should be a script that quietly keeps interpreting its .gss
// (still correct, just not what "optimizing" it was for), not a silent
// no-op that looks like the graduation never happened.
//
// To graduate a script once its generated <name>.cpp has been reviewed (and
// optionally hand-optimized):
//   1. #include its path below.
//   2. Add one line to s_CompiledScripts:
//        MakeCompiledScriptEntry<GeneratedScripts::<Name>>("<Name>"),
//      where <Name> matches ScriptEngine::ClassNameFromPath's PascalCase
//      naming for that script's file stem (paddle.gss -> "Paddle").

#include "CompiledScript.h"

#include "../assets/demos/BreakoutRecreation/paddle.cpp"
#include "../assets/demos/BreakoutRecreation/ball.cpp"

#include <string>
#include <vector>

inline const std::vector<CompiledScriptEntry> s_CompiledScripts = {
	MakeCompiledScriptEntry<GeneratedScripts::Paddle>("Paddle"),
	MakeCompiledScriptEntry<GeneratedScripts::Ball>("Ball"),
};

inline const CompiledScriptEntry* FindCompiledScript(const std::string& className)
{
	for (const CompiledScriptEntry& entry : s_CompiledScripts)
	{
		if (className == entry.ClassName)
			return &entry;
	}
	return nullptr;
}
