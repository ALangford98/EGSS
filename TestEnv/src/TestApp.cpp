// The sandbox. Two demo layers are pushed here; F1 switches between them.
//
//   Breakout.h  -- a playable 2D game on Renderer2D's batched quads
//   Cube3D.h    -- lit, textured cubes on the perspective camera and
//                  Renderer::Submit
//
// Reading both side by side is the point: they share the window, the events,
// the input, the layer stack, the shaders, the buffers and the textures. What
// differs is the camera, and which renderer path the geometry takes.
//
// See docs/ENGINE.md for how the pieces fit together.

#include <Egss.h>

#include "Demo.h"
#include "Breakout.h"
#include "Cube3D.h"

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		// Order matters only for event handling: layers pushed later sit
		// higher in the stack and see events first.
		PushLayer(new Breakout());
		PushLayer(new Cube3D());
	}
};

// The one function the engine requires of you.
Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
