// The sandbox. Two demo layers are pushed here, plus a selector panel that
// chooses between them (or F1 to cycle).
//
//   Breakout.h     -- a playable 2D game on Renderer2D's batched quads
//   Cube3D.h       -- lit, textured cubes on the perspective camera and
//                     Renderer::Submit
//   Physics2D.h    -- rigid bodies falling, stacking and bouncing
//   DemoSelector.h -- the "Demos" panel; owns switching so the demo layers
//                     don't have to agree about it
//   ProfilerPanel.h -- live per-scope timings, and Chrome trace capture
//
// Reading both side by side is the point: they share the window, the events,
// the input, the layer stack, the shaders, the buffers and the textures. What
// differs is the camera, and which renderer path the geometry takes.
//
// See docs/ENGINE.md for how the pieces fit together.

#include <Egss.h>

#include "Demo.h"
#include "DemoSelector.h"
#include "Breakout.h"
#include "Cube3D.h"
#include "Physics2D.h"
#include "ProfilerPanel.h"

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		// Order matters only for event handling: layers pushed later sit
		// higher in the stack and see events first. The selector goes last so
		// it consumes F1 before either demo does.
		PushLayer(new Breakout());
		PushLayer(new Cube3D());
		PushLayer(new Physics2D());
		PushLayer(new DemoSelector());
		PushLayer(new ProfilerPanel());
	}
};

// The one function the engine requires of you.
Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
