#include "egsspch.h"
#include "LinuxWindow.h"

// The native handle, for wallpaper mode. GLFW here is built `_GLFW_X11` only,
// so this is the X11 window even inside a Wayland session -- the session runs
// it through XWayland, and XWayland windows are managed by the compositor like
// any other X11 client.
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>

#include "Egss/Events/ApplicationEvent.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

namespace Egss {
	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char* description)
	{
		EGSS_CORE_ERROR("GLFW error ({0}): {1}", error, description);
	}

	static void EnsureGLFW()
	{
		if (s_GLFWInitialized)
			return;

		//TODO: glfwTerminate guard clause
		int success = glfwInit();
		EGSS_CORE_ASSERT(success, "Could not initialize GLFW");
		glfwSetErrorCallback(GLFWErrorCallback);
		s_GLFWInitialized = true;
	}

	Window* Window::Create(const WindowProps& props)
	{
		return new LinuxWindow(props);
	}

	std::vector<MonitorInfo> Window::GetMonitors()
	{
		// Callable before the first window, which is the point: the wallpaper
		// needs the arrangement to decide how big the window should be.
		EnsureGLFW();

		std::vector<MonitorInfo> monitors;

		int count = 0;
		GLFWmonitor** handles = glfwGetMonitors(&count);
		GLFWmonitor* primary = glfwGetPrimaryMonitor();

		for (int i = 0; i < count; i++)
		{
			const GLFWvidmode* mode = glfwGetVideoMode(handles[i]);
			if (!mode)
				continue;

			MonitorInfo info;
			glfwGetMonitorPos(handles[i], &info.X, &info.Y);

			info.Width = (unsigned int)mode->width;
			info.Height = (unsigned int)mode->height;
			info.Primary = handles[i] == primary;

			if (const char* name = glfwGetMonitorName(handles[i]))
				info.Name = name;

			monitors.push_back(info);
		}

		return monitors;
	}

	LinuxWindow::LinuxWindow(const WindowProps& props)
	{
		Init(props);
	}

	LinuxWindow::~LinuxWindow()
	{
		Shutdown();
	}

	void LinuxWindow::Init(const WindowProps& props)
	{
		m_Data.Title = props.Title;
		m_Data.Width = props.Width;
		m_Data.Height = props.Height;

		EGSS_CORE_INFO("Creating Window {0} ({1}, {2})", props.Title, props.Width, props.Height);

		EnsureGLFW();

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef EGSS_DEBUG
		// Required for glDebugMessageCallback to receive anything.
		glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

		// Hinted rather than hidden afterwards, so an unattended run never maps
		// the window at all. Creating it visible and calling glfwHideWindow
		// leaves a frame or two where it is on screen and holding the keyboard,
		// which is exactly the flicker this exists to remove.
		glfwWindowHint(GLFW_VISIBLE, props.Visible ? GLFW_TRUE : GLFW_FALSE);

		unsigned int width = props.Width;
		unsigned int height = props.Height;

		if (props.Wallpaper)
		{
			// Created hidden whatever was asked for, because the window type
			// has to be set *before* the window is mapped: a window manager
			// reads `_NET_WM_WINDOW_TYPE` when it takes the window over, and a
			// desktop window that arrives as an ordinary one has already been
			// stacked, focused and given a frame by then.
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
			glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
			glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
			glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

			// **Every screen, as one window.** The first version took the
			// primary monitor's video mode, which on a three-monitor desk meant
			// a 2880x1800 window sitting on a 3840x2160 screen -- covering
			// three quarters of one display and none of the other two.
			//
			// The union can start left of or above the origin, so the origin is
			// carried and applied after the window exists: GLFW sizes at
			// creation and positions afterwards.
			std::vector<MonitorInfo> monitors = GetMonitors();

			if (!monitors.empty())
			{
				int minX = monitors[0].X, minY = monitors[0].Y;
				int maxX = minX, maxY = minY;

				for (const MonitorInfo& monitor : monitors)
				{
					minX = std::min(minX, monitor.X);
					minY = std::min(minY, monitor.Y);
					maxX = std::max(maxX, monitor.X + (int)monitor.Width);
					maxY = std::max(maxY, monitor.Y + (int)monitor.Height);
				}

				m_WallpaperX = minX;
				m_WallpaperY = minY;

				width = (unsigned int)(maxX - minX);
				height = (unsigned int)(maxY - minY);

				EGSS_CORE_INFO("Wallpaper: {0} monitors spanning {1}x{2} at ({3}, {4})",
					monitors.size(), width, height, minX, minY);
			}

			m_Data.Width = width;
			m_Data.Height = height;
		}

		m_Window = glfwCreateWindow((int)width, (int)height, m_Data.Title.c_str(), nullptr, nullptr);

		if (props.Wallpaper)
			MakeDesktopWindow(props.Visible);

		glfwMakeContextCurrent(m_Window);

		int gladStatus = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		EGSS_CORE_ASSERT(gladStatus, "Could not initialize Glad");
		EGSS_CORE_INFO("OpenGL {0} | {1}", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

		glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVSync(true);

		SetGLFWCallbacks();
	}

	// Ask the window manager to treat this window as the desktop background.
	//
	// `_NET_WM_WINDOW_TYPE_DESKTOP` is the EWMH way of saying "wallpaper": the
	// window is stacked at the bottom, gets no frame, no taskbar entry, and no
	// focus. `xwinwrap` and every animated-wallpaper tool on X11 works this way.
	//
	// The state hints beside it are for window managers that honour the states
	// but not the type; setting both costs three atoms and removes a whole class
	// of "it works on mine".
	//
	// **Whether it is honoured is the compositor's decision, not ours.** On a
	// Wayland session this arrives through XWayland, and KWin may well stack it
	// as an ordinary window instead -- in which case what you get is a
	// borderless fullscreen window rather than a wallpaper, and the answer is a
	// native layer-shell surface instead. That is the experiment.
	void LinuxWindow::MakeDesktopWindow(bool show)
	{
		Display* display = glfwGetX11Display();
		::Window handle = glfwGetX11Window(m_Window);

		if (!display || !handle)
		{
			EGSS_CORE_WARN("--wallpaper: no X11 window to mark (not an X11 session?)");
			return;
		}

		Atom typeProperty = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
		Atom desktop = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);

		XChangeProperty(display, handle, typeProperty, XA_ATOM, 32,
			PropModeReplace, (const unsigned char*)&desktop, 1);

		Atom states[] =
		{
			XInternAtom(display, "_NET_WM_STATE_BELOW", False),
			XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
			XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False),
			XInternAtom(display, "_NET_WM_STATE_STICKY", False)
		};

		XChangeProperty(display, handle, XInternAtom(display, "_NET_WM_STATE", False),
			XA_ATOM, 32, PropModeReplace, (const unsigned char*)states, 4);

		XFlush(display);

		EGSS_CORE_INFO("Wallpaper mode: window 0x{0:x} marked as desktop", (unsigned long)handle);

		if (show)
		{
			glfwShowWindow(m_Window);

			// Positioned after mapping, and at the union's own origin rather
			// than at 0,0 -- an arrangement with a monitor left of or above the
			// primary starts at negative coordinates.
			glfwSetWindowPos(m_Window, m_WallpaperX, m_WallpaperY);
		}
	}

	// Each callback recovers the WindowData from the user pointer, builds the
	// matching Event, and hands it to whatever set the callback. GLFW is C, so
	// these have to be captureless lambdas.
	void LinuxWindow::SetGLFWCallbacks()
	{
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;

			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			WindowCloseEvent event;
			data.EventCallback(event);
		});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(key, 0);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(key);
					data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					// GLFW doesn't count repeats, so this reports 1 rather
					// than an accumulating total.
					KeyPressedEvent event(key, 1);
					data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int keycode)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			KeyTypedEvent event(keycode);
			data.EventCallback(event);
		});

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			switch (action)
			{
				case GLFW_PRESS:
				{
					MouseButtonPressedEvent event(button);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					MouseButtonReleasedEvent event(button);
					data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseScrolledEvent event((float)xOffset, (float)yOffset);
			data.EventCallback(event);
		});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xPos, double yPos)
		{
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);

			MouseMovedEvent event((float)xPos, (float)yPos);
			data.EventCallback(event);
		});
	}

	void LinuxWindow::Shutdown()
	{
		glfwDestroyWindow(m_Window);
	}

	void LinuxWindow::OnUpdate()
	{
		glfwPollEvents();
		glfwSwapBuffers(m_Window);
	}

	void LinuxWindow::SetVSync(bool enabled)
	{
		if (enabled)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);

		m_Data.VSync = enabled;
	}

	bool LinuxWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

	void LinuxWindow::SetCursorCaptured(bool captured)
	{
		if (m_Data.CursorCaptured == captured)
			return;

		glfwSetInputMode(m_Window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

		// Raw motion is the pointer the mouse actually sent, before the desktop
		// applied its acceleration curve. Wanted here because that curve is
		// tuned for hitting menu items, and it makes a slow turn feel sticky.
		// Only legal while the cursor is disabled, and not supported on every
		// backend -- both of which GLFW enforces, so ask first.
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);

		m_Data.CursorCaptured = captured;
	}

}
