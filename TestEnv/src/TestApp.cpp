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

#include <GS.h>

#include "DemoRegistry.h"
#include "EditorMenuBar.h"
#include "EditorShell.h"
#include "EditorSceneView.h"
#include "EditorHistory.h"
#include "DemoSelector.h"
#include "DemoWarmup.h"
#include "EditorProject.h"
#include "ProfilerPanel.h"
#include "AudioRaceStress.h"
#include "PlayMode.h"

class TestEnv : public GS::Application
{
public:
	TestEnv()
	{
		// Before the demos, so that on the step it hands over, the demo it
		// hands over *to* is already the active one when the demos are walked.
		// Pushed unconditionally; without --warmup it does nothing at all.
		PushLayer(new DemoWarmup());

		// **Before the demos, because ImGui runs in layer order.** The shell
		// publishes the dock a demo's panel should open into, and a demo that
		// opened its window first would already have been placed by the time
		// the shell said where -- and `FirstUseEver` only fires once. It
		// handles no events, so sitting at the bottom of the stack costs
		// nothing.
		PushLayer(new EditorMenuBar());
		PushLayer(new EditorShell());
		PushLayer(new EditorSceneView());

		// Load the editor's project state before demos, so if a demo's warmup
		// or attach logic wants to know the editor's state, it is already set.
		LoadEditorProjectFromCommandLine();

		// Every demo in DemoRegistry.h, pushed and numbered. Adding one needs
		// no change here.
		PushAllDemos(*this);

		// Order matters only for event handling: layers pushed later sit
		// higher in the stack and see events first, so the selector goes
		// above the demos and consumes F1 before any of them.
		PushLayer(new DemoSelector());
		PushLayer(new ProfilerPanel());
		// Inert without --audio-stress. Pushed unconditionally so
		// `./gs.py sanitize --thread` can turn it on from the command line --
		// see the note in AudioRaceStress.h for why a race sweep needs it.
		PushLayer(new AudioRaceStress());
	}

	// g_EditorScene is a global with static storage duration, so without this
	// it is destroyed after main() returns -- after the GL context is gone
	// (~Application, and the glfwDestroyWindow inside it, already ran). Its
	// MeshComponents hold VertexArray/VertexBuffer objects whose destructors
	// call glDeleteVertexArrays/glDeleteBuffers, which is undefined behavior
	// against a dead context. Clearing it here, while TestEnv (and therefore
	// the context) is still alive, avoids that.
	~TestEnv() override
	{
		// If the window closes while Play is still active (Play clicked,
		// Stop never was -- the single most ordinary way to leave this
		// feature), PlayMode::s_Prepared is still holding onto QuickJS
		// JSValues that only Stop()'s own ReleasePreparedScript loop frees.
		// Left alone, those outlive this function, and PlayMode::s_ScriptEngine's
		// destructor -- running later, at static-destruction time -- calls
		// JS_FreeRuntime into a runtime that still has live objects, which
		// is a hard assertion inside QuickJS itself
		// (quickjs.c: "JS_FreeRuntime: Assertion `list_empty(&rt->gc_obj_list)'
		// failed"), not a recoverable error. Must run before g_EditorScene.Clear()
		// below: Stop() reverts g_EditorScene from the Play-time snapshot via
		// Scene::Load, which needs the scene in a normal, not-yet-torn-down
		// state to load into.
		if (PlayMode::IsPlaying())
			PlayMode::Stop();

		// EditorHistory::s_Commands can also hold shared_ptr<Mesh> (a
		// Place/DeleteEntityCommand's captured MeshComponent) -- a third
		// owner alongside g_EditorScene and MeshCache that needs clearing
		// before the GL context goes away, for the same reason as the line
		// below.
		EditorHistory::Clear();
		g_EditorScene.Clear();
	}
};

// The one function the engine requires of you.
GS::Application* GS::CreateApplication()
{
	// Before constructing TestEnv (and therefore its GS::Application base,
	// which constructs and attaches ImGuiLayer before TestEnv's own
	// constructor body ever runs) -- this is the only point early enough
	// for ImGuiLayer::OnAttach to see it. "assets/fonts/..." is a plain
	// cwd-relative path, same convention every other TestEnv asset load
	// already uses (e.g. "assets/models/figure.gltf") -- the executable's
	// working directory is bin/<Config>-linux-x86_64/TestEnv, where
	// premake's postbuild step already copies TestEnv/assets.
	GS::ImGuiLayer::SetFontPath("assets/fonts/DejaVuSansMono.ttf");
	return new TestEnv();
}
