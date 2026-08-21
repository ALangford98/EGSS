#pragma once

#include "egsspch.h"

#include "Egss/Core.h"
#include "Egss/Events/Event.h"

namespace Egss {
	// One display, in the desktop's own coordinate space: where its top-left
	// corner sits and how many pixels it holds.
	//
	// The coordinates are the *arrangement's*, not each screen's own -- a second
	// monitor to the right of a 3840-wide one starts at x = 3840 -- which is
	// what makes a window spanning several of them expressible as one rectangle.
	struct MonitorInfo
	{
		int X = 0, Y = 0;
		unsigned int Width = 0, Height = 0;
		bool Primary = false;
		std::string Name;
	};

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

		// Ask the window manager to treat this as the desktop background.
		//
		// Undecorated, screen-sized, and marked `_NET_WM_WINDOW_TYPE_DESKTOP`,
		// which is the EWMH way of saying "this is the wallpaper". Whether a
		// given window manager honours it is the window manager's business --
		// see LinuxWindow::Init.
		bool Wallpaper = false;

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

		// Hide the cursor and stop it hitting the edges of the screen, which is
		// what a first-person look needs: an ordinary cursor runs out of desk
		// halfway through a turn and the view stops with it. Captured, the
		// position reported by Input becomes an unbounded virtual coordinate,
		// so a delta between two samples is a delta however far you turn.
		//
		// Deliberately not something a demo turns on for itself and leaves on.
		// ImGui reads the same cursor, so a captured cursor means no panels and
		// no way to click anything -- it has to be under the user's thumb.
		virtual void SetCursorCaptured(bool captured) = 0;
		virtual bool IsCursorCaptured() const = 0;

		// Escape hatch for platform code (input polling, ImGui backends).
		virtual void* GetNativeWindow() const = 0;

		static Window* Create(const WindowProps& props = WindowProps());

		// Every connected display, in arrangement coordinates. Safe to call
		// before a window exists.
		//
		// **The union of these is not always a rectangle.** Three monitors in
		// an L -- two 4K side by side with a laptop screen below and between
		// them -- span a box with two corners that are not on any screen at
		// all. Anything sizing itself to "all the monitors" gets that box and
		// has to accept that parts of it are never seen.
		static std::vector<MonitorInfo> GetMonitors();
	};
}