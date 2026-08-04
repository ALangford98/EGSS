#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	// Writes what the GPU just drew to a PNG.
	//
	// Deliberately in-engine rather than a screenshot tool driven from
	// outside. The compositor route was tried and abandoned: `import` hangs
	// against XWayland, window matching picks up stale windows, and two probes
	// coming back byte-identical meant the *capture* had gone stale rather than
	// the app having stopped. None of that can happen here, because there is no
	// compositor in the loop -- this reads the buffer the frame was rendered
	// into, on the thread that rendered it.
	//
	// It also works the same on Wayland, on X11, and on a machine with no
	// session at all, which the outside-in route never will.
	class EGSS_API ScreenCapture
	{
	public:
		// Captures the framebuffer **currently bound** and writes it as a PNG,
		// creating parent directories as needed. Returns false and logs on
		// failure rather than throwing.
		//
		// Timing is the whole game: this must be called with a finished frame
		// still in the buffer -- after the last draw, before the swap. Once
		// swapped, the back buffer's contents are undefined, and reading it
		// there gives you whatever the driver happened to leave behind. Prefer
		// Application::CaptureFrame, which defers to exactly that moment.
		//
		// Because it reads whatever is bound, binding an offscreen Framebuffer
		// first captures *that* instead -- which is how you'd photograph a
		// picking or light-map pass rather than the composited result.
		static bool SaveFrame(const std::string& path, unsigned int width, unsigned int height);
	};

}
