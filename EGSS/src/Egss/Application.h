#pragma once
#include "egsspch.h"
#include "Core.h"
#include "Window.h"
#include "LayerStack.h"
#include "ImGui/ImGuiLayer.h"
#include "Events/Event.h"
#include "Events/ApplicationEvent.h"

namespace Egss {

	class EGSS_API Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		inline Window& GetWindow() { return *m_Window; }
		inline ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }
		inline static Application& Get() { return *s_Instance; }

		// Length of one simulation step, in seconds. Shorter is more accurate
		// and more expensive; 1/60 is the usual compromise.
		inline float GetFixedTimestep() const { return m_FixedTimestep; }
		inline void SetFixedTimestep(float seconds) { m_FixedTimestep = seconds; }

		// How far the current frame sits between the last simulation step and
		// the next, in 0..1. Rendering a body at
		// mix(previousPosition, currentPosition, alpha) is what stops motion
		// juddering when the frame rate isn't a multiple of the step rate.
		inline float GetInterpolationAlpha() const { return m_InterpolationAlpha; }

		// Simulation steps run in the frame just gone. Useful for spotting the
		// loop falling behind: consistently above 1 means the step is too
		// short or the simulation too slow.
		inline unsigned int GetFixedStepsLastFrame() const { return m_FixedStepsLastFrame; }

		// Whether `--hide-ui` was given. The engine already uses it to skip the
		// ImGui pass; a layer needs it for anything else that is an *affordance*
		// rather than part of the scene.
		//
		// The case that asked for this: Map Building draws a preview block under
		// the cursor, and the cursor is wherever the mouse happens to be. That
		// made two otherwise identical capture runs produce different PNGs, which
		// quietly costs a demo the property the whole capture-as-regression-test
		// habit depends on. A cursor is UI. It goes when the panels go.
		inline bool IsUIHidden() const { return m_HideUI; }

		// --- Capture ---------------------------------------------------------

		// Write the frame currently being drawn to a PNG.
		//
		// Safe to call from anywhere inside a frame -- a layer's OnUpdate, an
		// ImGui button, a key handler -- because it only *requests* the shot.
		// The read happens at the one instant a finished frame exists, between
		// the last draw and the swap. Calling it twice in a frame keeps the
		// last path; there is one back buffer to photograph.
		void CaptureFrame(const std::string& path);

		// Frames completed since startup. The capture scheduling is built on
		// this, and it is the honest clock for "let it settle then look":
		// wall-clock time says nothing about how much has been drawn.
		inline unsigned long long GetFrameCount() const { return m_FrameCount; }

		// Fixed steps run since startup. This -- not the frame count -- is the
		// simulation's clock, and the only index a reproducible capture can
		// use: frames get skipped while the window is being mapped or
		// minimised, so the same frame number is not the same simulation state
		// twice.
		inline unsigned long long GetStepCount() const { return m_StepCount; }

		// Stashed by EntryPoint before the application is constructed, so the
		// constructor can read its own flags.
		static void SetCommandLine(int argc, char** argv);
		static const std::vector<std::string>& GetCommandLine();
	private:
		// --capture <path>        write a PNG, by default at frame 60
		// --capture-frame <n>     which frame to write it at
		// --exit-after <n>        quit once n frames have been drawn
		// --capture-step <n>      ...or at simulation step n, which is the
		//                         reproducible one: frames get skipped while
		//                         the window is mapping, steps do not
		// --lockstep              one fixed step per frame, ignoring real time
		// --hide-ui               no demo panels, so nothing in shot is timing
		//
		// Together these make an unattended run produce one image and stop,
		// which is the whole point: a capture nobody has to watch for. Add
		// --lockstep and the image is the *same* image every run, which is
		// what turns it into a test rather than an anecdote.
		void ParseCommandLine();
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		std::unique_ptr<Window> m_Window;
		bool m_Running = true;
		bool m_Minimized = false;
		LayerStack m_LayerStack;
		ImGuiLayer* m_ImGuiLayer;
		float m_LastFrameTime = 0.0f;

		float m_FixedTimestep = 1.0f / 60.0f;
		// Unspent real time carried between frames, always < m_FixedTimestep
		// once the step loop has run.
		float m_Accumulator = 0.0f;
		float m_InterpolationAlpha = 0.0f;
		unsigned int m_FixedStepsLastFrame = 0;

		unsigned long long m_FrameCount = 0;

		// Drives the loop by simulation steps rather than by the clock. The
		// prerequisite for a recorded demo replaying the same way twice, and
		// on its own enough to make a captured frame reproducible.
		bool m_Lockstep = false;

		// Skips the demo panels. They print frame times, so a capture
		// including them differs every run however deterministic the
		// simulation underneath is.
		bool m_HideUI = false;

		// Lets panels be dragged out into their own OS windows. Off unless
		// --viewports asks, and read before the ImGui layer is attached.
		bool m_Viewports = false;

		// Quit when the recording runs out, unless --exit-after said otherwise.
		bool m_ExitWhenReplayEnds = false;

		// Empty when nothing is pending. Cleared as soon as it is written, so
		// a request never survives into the next frame.
		std::string m_PendingCapturePath;

		// The scheduled shot, from --capture. Zero means none.
		std::string m_CapturePath;
		unsigned long long m_CaptureFrame = 0;
		unsigned long long m_CaptureStep = 0;
		unsigned long long m_ExitAfterFrame = 0;
		bool m_ExitAfterExplicit = false;
		bool m_Captured = false;

		unsigned long long m_StepCount = 0;

		static Application* s_Instance;
	};

	// To be defined in the client
	Application* CreateApplication();
}
