#pragma once
#include "GS/Window.h"

#include <EGL/egl.h>

namespace GS {

	// The window on Android is the activity's surface, and it is not ours to
	// keep: the system takes it away whenever the game goes to the
	// background and hands a new one back on return. So the EGL *context*
	// lives as long as this object -- every texture, buffer and shader with
	// it -- while the *surface* is made and unmade with the native window.
	// Between the two, the window reports 0x0, which Application already
	// reads as minimised: no simulation, nothing drawn, and OnUpdate sleeps
	// in the event queue rather than spinning.
	class AndroidWindow : public Window
	{
	public:
		AndroidWindow(const WindowProps& props);
		~AndroidWindow() override;

		void OnUpdate() override;

		unsigned int GetWidth() const override { return m_Width; }
		unsigned int GetHeight() const override { return m_Height; }

		void SetEventCallback(const EventCallbackFn& callback) override { m_Callback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override { return m_VSync; }

		// No title bar and no cursor on a phone.
		void SetTitle(const std::string&) override {}
		void SetCursorCaptured(bool) override {}
		bool IsCursorCaptured() const override { return false; }

		void* GetNativeWindow() const override;

		// From the event pump (AndroidPlatform.cpp), on this same thread.
		void OnCommand(int command);
		bool OnInput(const void* inputEvent);

		static AndroidWindow* Get() { return s_Instance; }

		// The pointer, as AndroidInput reports it: the first finger down,
		// tracked by id until it lifts. A second finger is ignored rather than
		// teleporting the pointer -- one pointer is what GSS offers.
		bool PointerDown() const { return m_PointerDown; }
		float PointerX() const { return m_PointerX; }
		float PointerY() const { return m_PointerY; }

	private:
		void CreateSurface();
		void DestroySurface();
		void RefreshSize();
		void Emit(Event& e) { if (m_Callback) m_Callback(e); }

		static AndroidWindow* s_Instance;

		EGLDisplay m_Display = EGL_NO_DISPLAY;
		EGLConfig m_Config = nullptr;
		EGLContext m_Context = EGL_NO_CONTEXT;
		EGLSurface m_Surface = EGL_NO_SURFACE;

		unsigned int m_Width = 0, m_Height = 0;
		bool m_VSync = true;
		bool m_CloseSent = false;
		EventCallbackFn m_Callback;

		int m_PointerId = -1;
		bool m_PointerDown = false;
		float m_PointerX = 0.0f, m_PointerY = 0.0f;
	};
}
