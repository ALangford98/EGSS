#pragma once

// Which demo is live. Both layers are pushed at startup and each skips its own
// work when it isn't selected, so the two can be compared without restarting.
//
// This is a sandbox shortcut, not an engine pattern -- a real application
// would push and pop layers, or hold a scene. It is a global here only so the
// two demo headers can share it without dragging in a third class.
enum class Demo
{
	Breakout,
	Cube3D
};

inline Demo g_ActiveDemo = Demo::Cube3D;
