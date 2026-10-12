#include "gspch.h"
#include "GS/Input.h"
#include "Platform/Android/AndroidWindow.h"

namespace GS {

	// No keyboard to poll -- a phone game reads the pointer (Input.h). The
	// pointer is button 0 and the mouse position, as the window tracks it.
	class AndroidInput : public Input
	{
	protected:
		bool IsKeyPressedImpl(int) override { return false; }

		bool IsMouseButtonPressedImpl(int button) override
		{
			AndroidWindow* window = AndroidWindow::Get();
			return button == 0 && window && window->PointerDown();
		}

		std::pair<float, float> GetMousePositionImpl() override
		{
			AndroidWindow* window = AndroidWindow::Get();
			return window ? std::pair<float, float>{ window->PointerX(), window->PointerY() }
						  : std::pair<float, float>{ 0.0f, 0.0f };
		}

		float GetMouseXImpl() override { return GetMousePositionImpl().first; }
		float GetMouseYImpl() override { return GetMousePositionImpl().second; }
	};

	Input* Input::s_Instance = new AndroidInput();
}
