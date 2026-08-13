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

		// Whether the window is mapped at all.
		//
		// A hidden window still has a GL context and still renders -- the back
		// buffer belongs to the driver, not to the compositor -- so a capture
		// taken from one is byte-identical to a capture taken from a visible
		// window. What it does not do is appear on somebody's screen and take
		// their keyboard, which is the entire point: an automated run happening
		// while the machine is being used for something else should not steal
		// focus mid-sentence. See `Application::WantsHiddenWindow`.
		bool Visible = true;

		WindowProps(const std::string& t = "Every Game Starts Somewhere",
			unsigned int w = 1280,
			unsigned int h = 720,
			bool visible = true)
			: Title(t), Width(w), Height(h), Visible(visible)
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

		// Escape hatch for platform code (input polling, ImGui backends).
		virtual void* GetNativeWindow() const = 0;

		static Window* Create(const WindowProps& props = WindowProps());
	};
}