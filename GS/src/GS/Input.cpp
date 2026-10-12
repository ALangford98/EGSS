#include "gspch.h"
#include "GS/Input.h"

#include "GS/Application.h"
#include "GS/KeyCodes.h"

namespace GS {

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
		// released and nothing ever sets them. The GS codes are GLFW's own
		// numbers, spelled without GLFW so a build with no GLFW (Android) can
		// compile this.
		for (int key = GS_KEY_SPACE; key <= GS_KEY_MENU; key++)
			snapshot.SetKey(key, s_Instance->IsKeyPressedImpl(key));

		for (int button = 0; button < InputSnapshot::MaxMouseButtons; button++)
			snapshot.SetMouseButton(button, s_Instance->IsMouseButtonPressedImpl(button));

		auto [x, y] = s_Instance->GetMousePositionImpl();
		snapshot.MouseX = x;
		snapshot.MouseY = y;

		return snapshot;
	}

	// Divided by the window as it is now. A replayed run reads recorded
	// pixels against the current window, so a recording replays its pointer
	// faithfully only at the size it was made -- the same as raw mouse
	// positions always have.
	float Input::GetPointerX()
	{
		unsigned int width = Application::Get().GetWindow().GetWidth();
		return width > 0 ? GetMouseX() / (float)width : 0.0f;
	}

	float Input::GetPointerY()
	{
		unsigned int height = Application::Get().GetWindow().GetHeight();
		return height > 0 ? GetMouseY() / (float)height : 0.0f;
	}

}
