#include "egsspch.h"
#include "WindowsWindow.h"

#include "Egss/Events/ApplicationEvent.h"
#include "Egss/Events/KeyEvent.h"
#include "Egss/Events/MouseEvent.h"

namespace Egss {
	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char* description)
	{
		EGSS_CORE_ERROR("GLFW error ({0}): {1}", error, description);
	}

	Window* Window::Create(const WindowProps& props)
	{
		return new WindowsWindow(props);
	}

	WindowsWindow::WindowsWindow(const WindowProps& props)
	{
		Init(props);
	}

	WindowsWindow::~WindowsWindow()
	{
		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps& props)
	{
		m_Data.Title = props.Title;
		m_Data.Width = props.Width;
		m_Data.Height = props.Height;

		EGSS_CORE_INFO("Creating Window {0} ({1}, {2})", props.Title, props.Width, props.Height);

		if (!s_GLFWInitialized)
		{
			//TODO: glfwTerminate guard clause
			int success = glfwInit();
			EGSS_CORE_ASSERT(success, "Could not initialize GLFW");
			glfwSetErrorCallback(GLFWErrorCallback);
			s_GLFWInitialized = true;
		}

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

		// **Four samples, because this engine draws a great deal of geometry
		// that is thinner than a pixel.**
		//
		// A blade of grass is six millimetres across and a tree's twigs are
		// finer, so past a couple of metres their coverage of a pixel is a
		// coin-flip: either the sample point is inside the triangle or it is
		// not, and there is no partial answer. That is the grainy sparkle over
		// a grass field and the crawling on a distant branch, and no amount of
		// smoothing the shading removes it -- it is the *sampling* that is too
		// coarse.
		//
		// Multisampling is the direct fix: four coverage samples a pixel
		// instead of one, resolved at the end. It costs memory bandwidth on
		// the colour buffer and nothing in the shaders, since the fragment
		// still runs once per pixel. Four rather than eight because the return
		// falls off sharply -- four already turns a binary edge into five
		// levels -- and because this has to run on integrated graphics.
		glfwWindowHint(GLFW_SAMPLES, 4);
#ifdef EGSS_DEBUG
		// Required for glDebugMessageCallback to receive anything.
		glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

		// Hinted rather than hidden afterwards, so an unattended run never maps
		// the window at all. Creating it visible and calling glfwHideWindow
		// leaves a frame or two where it is on screen and holding the keyboard,
		// which is exactly the flicker this exists to remove.
		glfwWindowHint(GLFW_VISIBLE, props.Visible ? GLFW_TRUE : GLFW_FALSE);

		m_Window = glfwCreateWindow((int)props.Width, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
		glfwMakeContextCurrent(m_Window);

		int gladStatus = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		EGSS_CORE_ASSERT(gladStatus, "Could not initialize Glad");
		EGSS_CORE_INFO("OpenGL {0} | {1}", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

		glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVSync(true);

		SetGLFWCallbacks();
	}

	// Each callback recovers the WindowData from the user pointer, builds the
	// matching Event, and hands it to whatever set the callback. GLFW is C, so
	// these have to be captureless lambdas.
	void WindowsWindow::SetGLFWCallbacks()
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

	void WindowsWindow::Shutdown()
	{
		glfwDestroyWindow(m_Window);
	}

	void WindowsWindow::OnUpdate()
	{
		glfwPollEvents();
		glfwSwapBuffers(m_Window);
	}

	void WindowsWindow::SetVSync(bool enabled)
	{
		if (enabled)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);

		m_Data.VSync = enabled;
	}

	bool WindowsWindow::IsVSync() const
	{
		return m_Data.VSync;
	}

	void WindowsWindow::SetCursorCaptured(bool captured)
	{
		if (m_Data.CursorCaptured == captured)
			return;

		glfwSetInputMode(m_Window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

		// See LinuxWindow: raw motion is the mouse before the desktop's
		// acceleration curve, and is only legal while the cursor is disabled.
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);

		m_Data.CursorCaptured = captured;
	}

}
