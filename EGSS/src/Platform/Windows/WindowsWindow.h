#pragma once
#include "Egss/Window.h"
#include "Egss/Log.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace Egss {
	class WindowsWindow : public Window
	{
	public:
		WindowsWindow(const WindowProps&);
		virtual ~WindowsWindow();

		void OnUpdate() override;

		inline unsigned int GetWidth() const override { return m_Data.Width; }
		inline unsigned int GetHeight() const override { return m_Data.Height; }

		//Window attributes

		inline void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override;

		void SetCursorCaptured(bool captured) override;
		inline bool IsCursorCaptured() const override { return m_Data.CursorCaptured; }

		inline void* GetNativeWindow() const override { return m_Window; }
	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
		void SetGLFWCallbacks();
	private:
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

