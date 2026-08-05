#include "egsspch.h"
#include "Egss/Input.h"

#include <GLFW/glfw3.h>

namespace Egss {

	const InputSnapshot* Input::s_Playback = nullptr;

	InputSnapshot Input::CaptureSnapshot()
	{
		// Playback is already a snapshot; handing back a copy of it is what
		// makes record-while-replaying round-trip exactly.
		if (s_Playback)
			return *s_Playback;

		InputSnapshot snapshot;

		// GLFW rejects anything outside [GLFW_KEY_SPACE, GLFW_KEY_LAST] with an
		// error rather than returning "not pressed", so the sweep starts at 32
		// rather than 0. The gaps inside the range are harmless -- they read as
		// released and nothing ever sets them.
		for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; key++)
			snapshot.SetKey(key, s_Instance->IsKeyPressedImpl(key));

		for (int button = 0; button < InputSnapshot::MaxMouseButtons; button++)
			snapshot.SetMouseButton(button, s_Instance->IsMouseButtonPressedImpl(button));

		auto [x, y] = s_Instance->GetMousePositionImpl();
		snapshot.MouseX = x;
		snapshot.MouseY = y;

		return snapshot;
	}

}
