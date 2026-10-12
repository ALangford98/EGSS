#include "gspch.h"
#include "Platform/Android/AndroidWindow.h"
#include "Platform/Android/AndroidPlatform.h"

#include "GS/Events/ApplicationEvent.h"
#include "GS/Events/MouseEvent.h"
#include "GS/Log.h"

#include <android_native_app_glue.h>
#include <glad/glad.h>

namespace GS {

	AndroidWindow* AndroidWindow::s_Instance = nullptr;

	Window* Window::Create(const WindowProps& props)
	{
		return new AndroidWindow(props);
	}

	// One screen, the size of the window. Only wallpaper mode asks, and a
	// phone has no desktop to put one on.
	std::vector<MonitorInfo> Window::GetMonitors()
	{
		std::vector<MonitorInfo> monitors;
		if (AndroidWindow* window = AndroidWindow::Get())
		{
			MonitorInfo info;
			info.Width = window->GetWidth();
			info.Height = window->GetHeight();
			info.Primary = true;
			info.Name = "Android";
			monitors.push_back(info);
		}
		return monitors;
	}

	AndroidWindow::AndroidWindow(const WindowProps&)
	{
		s_Instance = this;

		m_Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglInitialize(m_Display, nullptr, nullptr);

		// The same attachments the desktop window asks GLFW for: 8-bit colour,
		// 24-bit depth, 8-bit stencil. No multisampling -- the scene is drawn
		// into the engine's own framebuffer and only blitted here.
		const EGLint attributes[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
			EGL_NONE
		};
		EGLint count = 0;
		eglChooseConfig(m_Display, attributes, &m_Config, 1, &count);
		GS_CORE_ASSERT(count > 0, "No EGL config with GLES 3, RGBA8, depth 24, stencil 8");

		const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		m_Context = eglCreateContext(m_Display, m_Config, EGL_NO_CONTEXT, contextAttributes);
		GS_CORE_ASSERT(m_Context != EGL_NO_CONTEXT, "Could not create a GLES 3 context");

		// Android::Main waited for the native window before creating the
		// application, so there is one now: the renderer initialises right
		// after this returns and needs a current context.
		CreateSurface();
		GS_CORE_INFO("Window: Android {0}x{1}, GLES {2}", m_Width, m_Height,
			(const char*)glGetString(GL_VERSION));
	}

	AndroidWindow::~AndroidWindow()
	{
		DestroySurface();
		if (m_Context != EGL_NO_CONTEXT)
			eglDestroyContext(m_Display, m_Context);
		eglTerminate(m_Display);
		s_Instance = nullptr;
	}

	void* AndroidWindow::GetNativeWindow() const
	{
		return Android::GetApp()->window;
	}

	void AndroidWindow::SetVSync(bool enabled)
	{
		m_VSync = enabled;
		if (m_Surface != EGL_NO_SURFACE)
			eglSwapInterval(m_Display, enabled ? 1 : 0);
	}

	void AndroidWindow::CreateSurface()
	{
		android_app* app = Android::GetApp();
		if (m_Surface != EGL_NO_SURFACE || !app->window)
			return;

		m_Surface = eglCreateWindowSurface(m_Display, m_Config, app->window, nullptr);
		if (m_Surface == EGL_NO_SURFACE || !eglMakeCurrent(m_Display, m_Surface, m_Surface, m_Context))
		{
			// EGL_CONTEXT_LOST is the one that matters: some drivers drop the
			// context while the game is in the background, and every GL object
			// with it. Rebuilding them all is not something the engine can do
			// yet, so this says so plainly rather than drawing garbage.
			GS_CORE_ERROR("Window: could not make the surface current (EGL error 0x{0:x})", eglGetError());
			return;
		}
		eglSwapInterval(m_Display, m_VSync ? 1 : 0);
		RefreshSize();
	}

	void AndroidWindow::DestroySurface()
	{
		if (m_Surface == EGL_NO_SURFACE)
			return;
		// The context is released with the surface, not destroyed: it, and
		// everything uploaded through it, outlives the window.
		eglMakeCurrent(m_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(m_Display, m_Surface);
		m_Surface = EGL_NO_SURFACE;
	}

	// The surface's size is the truth -- a rotation changes it without the
	// window being recreated. A change is announced as the resize event the
	// desktop window sends, and 0x0 (no surface) as the minimise it means.
	void AndroidWindow::RefreshSize()
	{
		EGLint width = 0, height = 0;
		if (m_Surface != EGL_NO_SURFACE)
		{
			eglQuerySurface(m_Display, m_Surface, EGL_WIDTH, &width);
			eglQuerySurface(m_Display, m_Surface, EGL_HEIGHT, &height);
		}
		if ((unsigned int)width == m_Width && (unsigned int)height == m_Height)
			return;
		m_Width = (unsigned int)width;
		m_Height = (unsigned int)height;
		WindowResizeEvent event(m_Width, m_Height);
		Emit(event);
	}

	void AndroidWindow::OnUpdate()
	{
		if (m_Surface != EGL_NO_SURFACE)
			eglSwapBuffers(m_Display, m_Surface);

		Android::PumpEvents(false);

		// In the background: sleep in the event queue until the window is
		// back or the activity is going away, instead of running frames
		// nobody can see.
		android_app* app = Android::GetApp();
		while (m_Surface == EGL_NO_SURFACE && !app->destroyRequested)
			Android::PumpEvents(true);

		if (app->destroyRequested && !m_CloseSent)
		{
			m_CloseSent = true;
			WindowCloseEvent event;
			Emit(event);
		}

		RefreshSize();
	}

	void AndroidWindow::OnCommand(int command)
	{
		switch (command)
		{
			case APP_CMD_INIT_WINDOW:
				CreateSurface();
				break;
			case APP_CMD_TERM_WINDOW:
				DestroySurface();
				RefreshSize();
				break;
			case APP_CMD_WINDOW_RESIZED:
			case APP_CMD_CONFIG_CHANGED:
				RefreshSize();
				break;
			default:
				break;
		}
	}

	// Touches become the mouse -- button 0 at the finger -- because that is
	// what the desktop already reads, the ImGui layer included, and what
	// GSS's pointer is defined as. Returns whether the event was consumed;
	// keys are not, so Back keeps its system meaning and closes the game.
	bool AndroidWindow::OnInput(const void* inputEvent)
	{
		const AInputEvent* event = (const AInputEvent*)inputEvent;
		if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
			return false;

		int32_t action = AMotionEvent_getAction(event);
		int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
		size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
			>> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

		auto moveTo = [this](float x, float y) {
			m_PointerX = x;
			m_PointerY = y;
			MouseMovedEvent moved(x, y);
			Emit(moved);
		};

		switch (masked)
		{
			case AMOTION_EVENT_ACTION_DOWN:
			case AMOTION_EVENT_ACTION_POINTER_DOWN:
				if (m_PointerId < 0)
				{
					m_PointerId = AMotionEvent_getPointerId(event, index);
					// Moved before pressed, so the press lands where the
					// finger is rather than where the last one lifted.
					moveTo(AMotionEvent_getX(event, index), AMotionEvent_getY(event, index));
					m_PointerDown = true;
					MouseButtonPressedEvent pressed(0);
					Emit(pressed);
				}
				break;

			case AMOTION_EVENT_ACTION_MOVE:
				for (size_t i = 0; i < AMotionEvent_getPointerCount(event); i++)
					if (AMotionEvent_getPointerId(event, i) == m_PointerId)
						moveTo(AMotionEvent_getX(event, i), AMotionEvent_getY(event, i));
				break;

			case AMOTION_EVENT_ACTION_UP:
			case AMOTION_EVENT_ACTION_POINTER_UP:
			case AMOTION_EVENT_ACTION_CANCEL:
				if (masked == AMOTION_EVENT_ACTION_CANCEL || masked == AMOTION_EVENT_ACTION_UP
					|| AMotionEvent_getPointerId(event, index) == m_PointerId)
				{
					if (m_PointerDown)
					{
						m_PointerDown = false;
						MouseButtonReleasedEvent released(0);
						Emit(released);
					}
					m_PointerId = -1;
				}
				break;

			default:
				break;
		}
		return true;
	}
}
