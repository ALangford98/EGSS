#pragma once

// The list of demos. This is the only file to touch when adding one.
//
// Name, short name and constructor sit on a single line, so there is no second
// place for them to drift out of step with -- which is what the enum, the
// names array and the PushLayer calls kept doing.
//
// Deliberately *not* self-registering. A `static Registration<T> reg;` in each
// demo header would remove this file, but these demos are header-only: the
// initialiser only runs if some translation unit includes the header, so
// forgetting the include would silently produce no demo, no enum entry, and no
// compile error. Five obvious lines beat one invisible failure.

#include "Demo.h"

#include "Breakout.h"
#include "Cube3D.h"
#include "Physics2D.h"
#include "Lighting2D.h"

struct DemoEntry
{
	const char* Name;       // shown in the dropdown
	const char* ShortName;  // shown on the quick-select buttons
	DemoLayer* (*Create)();
};

// Add a line here and you are done.
inline const DemoEntry s_Demos[] =
{
	{ "Breakout (2D, batched quads)", "Breakout",  []() -> DemoLayer* { return new Breakout(); } },
	{ "Cube3D (3D, lit meshes)",      "Cube3D",    []() -> DemoLayer* { return new Cube3D(); } },
	{ "Physics2D (rigid bodies)",     "Physics",   []() -> DemoLayer* { return new Physics2D(); } },
	{ "Lighting2D (visibility)",      "Lighting",  []() -> DemoLayer* { return new Lighting2D(); } }
};

inline constexpr int s_DemoCount = (int)(sizeof(s_Demos) / sizeof(s_Demos[0]));

// Creates every demo, hands each its index, and pushes it. Called once from
// TestApp -- so adding a demo needs no change there either.
inline void PushAllDemos(Egss::Application& app)
{
	for (int i = 0; i < s_DemoCount; i++)
	{
		DemoLayer* demo = s_Demos[i].Create();
		demo->SetDemoId(i);
		app.PushLayer(demo);
	}
}
