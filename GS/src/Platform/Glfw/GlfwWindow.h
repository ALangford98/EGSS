#pragma once
#include "GS/Window.h"
#include "GS/Log.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace GS {
	class GlfwWindow : public Window
	{
	public:
		GlfwWindow(const WindowProps&);
		virtual ~GlfwWindow();

		void OnUpdate() override;

		inline unsigned int GetWidth() const override { return m_Data.Width; }
		inline unsigned int GetHeight() const override { return m_Data.Height; }

		//Window attributes

		inline void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		void SetTitle(const std::string& title) override { m_Data.Title = title; glfwSetWindowTitle(m_Window, title.c_str()); }
		bool IsVSync() const override;

		void SetCursorCaptured(bool captured) override;
		inline bool IsCursorCaptured() const override { return m_Data.CursorCaptured; }
		void SetFullscreen(bool fullscreen) override;
		inline bool IsFullscreen() const override { return m_Fullscreen; }

		inline void* GetNativeWindow() const override { return m_Window; }
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
		void SetGLFWCallbacks();
		void MakeDesktopWindow(bool show);

		// Top-left of the union of every monitor, which is where a wallpaper
		// window goes. Not always (0, 0).
		int m_WallpaperX = 0;
		int m_WallpaperY = 0;
	private:
		bool m_Fullscreen = false;
		int m_WindowedX = 0, m_WindowedY = 0, m_WindowedWidth = 0, m_WindowedHeight = 0;
		GLFWwindow* m_Window;

		struct WindowData
		{
			std::string Title;
			unsigned int Width, Height;
			bool VSync;
			bool CursorCaptured = false;

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};
}
