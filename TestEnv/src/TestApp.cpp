// The sandbox. Two demo layers are pushed here, plus a selector panel that
// chooses between them (or F1 to cycle).
//
//   Breakout.h     -- a playable 2D game on Renderer2D's batched quads
//   Cube3D.h       -- lit, textured cubes on the perspective camera and
//                     Renderer::Submit
//   Physics2D.h    -- rigid bodies falling, stacking and bouncing
//   Lighting2D.h   -- visibility-polygon lighting from raycasts
//   SceneDemo.h    -- entities, components and pixel-exact picking
//   Acoustics2DDemo.h -- ray-traced room acoustics driving the mixer
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

#include "DemoRegistry.h"
#include "DemoSelector.h"
#include "DemoWarmup.h"
#include "ProfilerPanel.h"
#include "AudioRaceStress.h"

class TestEnv : public Egss::Application
{
public:
	TestEnv()
	{
		// Before the demos, so that on the step it hands over, the demo it
		// hands over *to* is already the active one when the demos are walked.
		// Pushed unconditionally; without --warmup it does nothing at all.
		PushLayer(new DemoWarmup());

		// Every demo in DemoRegistry.h, pushed and numbered. Adding one needs
		// no change here.
		PushAllDemos(*this);

		// Order matters only for event handling: layers pushed later sit
		// higher in the stack and see events first, so the selector goes
		// above the demos and consumes F1 before any of them.
		PushLayer(new DemoSelector());
		PushLayer(new ProfilerPanel());
		// Inert without --audio-stress. Pushed unconditionally so
		// `./egss.py sanitize --thread` can turn it on from the command line --
		// see the note in AudioRaceStress.h for why a race sweep needs it.
		PushLayer(new AudioRaceStress());
	}
};

// The one function the engine requires of you.
Egss::Application* Egss::CreateApplication()
{
	return new TestEnv();
}
