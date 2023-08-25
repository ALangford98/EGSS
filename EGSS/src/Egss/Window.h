#pragma once

#include "egsspch.h"

#include "Egss/Core.h"
#include "Egss/Events/Event.h"

namespace Egss {
	struct WindowProps
	{
		std::string Title;
		unsigned int Width;
		unsigned int Height;
		
		WindowProps(const std::string& t = "Every Game Starts Somewhere",
			unsigned int w = 1280,
			unsigned int h = 720)
			: Title(t), Width(w), Height(h)
		{
		}
	};

	// Inteface window
	class EGSS_API Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;
		virtual ~Window() {}
		virtual void OnUpdate() = 0;

		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;

		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;

		static Window* Create(const WindowProps& props = WindowProps());
	};
}