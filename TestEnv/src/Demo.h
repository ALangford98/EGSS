#pragma once

// Which demo is live. Every demo layer is pushed at startup and skips its own
// work when it isn't selected, so they can be compared without restarting.
//
// This is a sandbox shortcut, not an engine pattern -- a real application
// would push and pop layers, or hold a scene. It is a global here only so the
// demo headers can share it without dragging in a third class.
//
// Adding a demo: add an enumerator, add its name to s_DemoNames in the same
// order, and push the layer in TestApp.cpp. DemoSelector picks it up with no
// further changes.
enum class Demo
{
	Breakout = 0,
	Cube3D,
	Physics2D,

	Count
};

inline Demo g_ActiveDemo = Demo::Physics2D;

// Order must match the enum -- the selector indexes straight into this.
inline const char* s_DemoNames[] =
{
	"Breakout (2D, batched quads)",
	"Cube3D (3D, lit meshes)",
	"Physics2D (rigid bodies)"
};

static_assert(sizeof(s_DemoNames) / sizeof(s_DemoNames[0]) == (size_t)Demo::Count,
	"s_DemoNames is out of step with the Demo enum");
