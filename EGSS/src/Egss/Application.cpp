#include "egsspch.h"
#include "Application.h"

#include "Window.h"
#include"Events/ApplicationEvent.h"
#include"Log.h"

#include "Egss/Renderer/Renderer.h"
#include "Egss/Debug/Instrumentor.h"
#include "Egss/Debug/ScreenCapture.h"
#include "Egss/Debug/Replay.h"
#include "Egss/Audio/AudioEngine.h"

namespace Egss {

	Application* Application::s_Instance = nullptr;

	// Stashed by EntryPoint, read by the constructor. A free static rather than
	// a member because it has to exist before any Application does.
	static std::vector<std::string> s_CommandLine;

	// Monotonic seconds since process start, used to derive the timestep.
	static float GetTime()
	{
		static const auto start = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		return std::chrono::duration<float>(now - start).count();
	}

	Application::Application()
	{
		EGSS_CORE_ASSERT(!s_Instance, "Application already exists");
		s_Instance = this;

		// Visibility has to be decided *here*, before the window exists, which
		// is why it is read straight off the command line rather than waiting
		// for ParseCommandLine below. A window hint only applies at creation,
		// and creating it visible to hide it a moment later is a window that
		// still flashed up and still took the keyboard.
		WindowProps props;
		props.Visible = !WantsHiddenWindow();

		// Wallpaper mode is a window that is meant to be seen and never
		// focused, so it overrides the hidden-by-default inference rather than
		// being subject to it.
		for (const std::string& argument : s_CommandLine)
			props.Wallpaper = props.Wallpaper || argument == "--wallpaper";

		m_Wallpaper = props.Wallpaper;

		if (props.Wallpaper)
			props.Visible = true;

		// Said out loud, because "the demo did not appear" is otherwise
		// indistinguishable from a crash on startup.
		if (!props.Visible)
			EGSS_CORE_INFO("No window: unattended run (--show-window to watch it)");

		m_Window = std::unique_ptr<Window>(Window::Create(props));
		m_Window->SetEventCallback(EGSS_BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();
		AudioEngine::Init();

		// Parsed before the ImGui layer is pushed, not after. PushOverlay runs
		// OnAttach immediately, and OnAttach is where the ImGui context is
		// created and its config flags read -- so --viewports arriving
		// afterwards would be set on a context that had already been built
		// without it, and would silently do nothing.
		ParseCommandLine();

		// Owned by the layer stack, but kept as a pointer so Run can bracket
		// the per-layer ImGui rendering.
		m_ImGuiLayer = new ImGuiLayer();
		m_ImGuiLayer->EnableViewports(m_Viewports);
		PushOverlay(m_ImGuiLayer);
	}

	void Application::SetCommandLine(int argc, char** argv)
	{
		s_CommandLine.assign(argv, argv + argc);
	}

	const std::vector<std::string>& Application::GetCommandLine()
	{
		return s_CommandLine;
	}

	// Whether this run should put a window on the screen.
	//
	// **An unattended run is hidden by default.** `--capture` and `--play` are
	// the two flags that mean "nobody is watching this": one is a screenshot
	// script, the other a replay. Both used to map a window and seize the
	// keyboard, which is merely untidy on an idle machine and genuinely
	// disruptive on one somebody is working at -- a capture that takes a second
	// still lands in the middle of a sentence.
	//
	// A hidden window renders exactly the same: the back buffer belongs to the
	// driver, not to the compositor, and `ReadFramebufferRGBA` reads GL_BACK
	// before the swap either way. Verified rather than assumed -- captures taken
	// hidden and visible hash identically.
	//
	// Both directions are overridable, because the default is a guess about
	// intent: `--show-window` to watch a capture happen, `--hide-window` to run
	// anything else quietly.
	bool Application::WantsHiddenWindow()
	{
		bool automated = false;

		for (size_t i = 1; i < s_CommandLine.size(); i++)
		{
			const std::string& argument = s_CommandLine[i];

			// Explicit wins over inferred, in both directions, and is checked
			// first so the order of the flags never matters.
			if (argument == "--show-window")
				return false;
			if (argument == "--hide-window")
				return true;

			// The value-taking flags: a bare `--capture` with nothing after it
			// captures nothing, so it should not hide anything either.
			if ((argument == "--capture" || argument == "--play")
				&& i + 1 < s_CommandLine.size())
				automated = true;
		}

		return automated;
	}

	void Application::ParseCommandLine()
	{
		// A flag taking a value, or an empty string if it is absent or was
		// given with nothing after it.
		auto valueOf = [](const std::string& flag) -> std::string
		{
			for (size_t i = 1; i + 1 < s_CommandLine.size(); i++)
			{
				if (s_CommandLine[i] == flag)
					return s_CommandLine[i + 1];
			}
			return {};
		};

		m_CapturePath = valueOf("--capture");

		std::string frame = valueOf("--capture-frame");
		if (!frame.empty())
			m_CaptureFrame = std::strtoull(frame.c_str(), nullptr, 10);

		std::string step = valueOf("--capture-step");
		if (!step.empty())
			m_CaptureStep = std::strtoull(step.c_str(), nullptr, 10);

		std::string exitAfter = valueOf("--exit-after");
		if (!exitAfter.empty())
		{
			m_ExitAfterFrame = std::strtoull(exitAfter.c_str(), nullptr, 10);
			m_ExitAfterExplicit = true;
		}

		bool wallpaper = false;

		for (const std::string& argument : s_CommandLine)
		{
			m_Lockstep = m_Lockstep || argument == "--lockstep";
			m_HideUI = m_HideUI || argument == "--hide-ui";
			m_Viewports = m_Viewports || argument == "--viewports";
			wallpaper = wallpaper || argument == "--wallpaper";
		}

		// A wallpaper with a debug panel on it is not a wallpaper. Implied
		// rather than required, and still overridable the other way: --show-ui
		// puts the panels back, which is how you tune the thing you are
		// looking at without stopping it.
		if (wallpaper)
		{
			m_HideUI = true;

			for (const std::string& argument : s_CommandLine)
				if (argument == "--show-ui")
					m_HideUI = false;
		}

		// --play <file> / --record <file>. Playback starts here, in the
		// constructor, because TestEnv asks the recording which scene it
		// belongs to when it builds its layers -- which happens next.
		std::string play = valueOf("--play");
		if (!play.empty() && Replay::StartPlayback(play))
		{
			// A replay driven by wall-clock time is not a replay: how much has
			// been simulated by a given moment would depend on the machine.
			// Forced rather than merely defaulted, since the alternative
			// silently produces a different run.
			m_Lockstep = true;
			m_Window->SetVSync(false);

			m_ExitWhenReplayEnds = !m_ExitAfterExplicit;
		}

		// --record is deliberately *not* handled here. Starting a recording
		// needs the scene index to stamp into the header, and which scene is
		// which is the sandbox's business -- the engine has no idea what a
		// demo is. TestEnv starts it once it has chosen one.

		if (m_Lockstep)
		{
			// The swap would otherwise pace the run to the display, and a
			// lockstep run is not being watched -- there is nothing to pace
			// it for. This is most of why a 240-frame capture takes about a
			// second instead of four.
			m_Window->SetVSync(false);
			EGSS_CORE_INFO("Lockstep: one fixed step per frame, VSync off");
		}

		if (!m_CapturePath.empty())
		{
			// Frame 60 by default: the first frames are still loading assets
			// and settling the layout, and a shot of a half-built scene looks
			// exactly like a rendering bug.
			if (m_CaptureFrame == 0 && m_CaptureStep == 0)
				m_CaptureFrame = 60;

			// An unattended capture that leaves the window open forever is a
			// hung script. --exit-after overrides this; a step-scheduled shot
			// gets a generous frame budget instead, since how many frames a
			// given number of steps takes is exactly what is not fixed.
			if (m_ExitAfterFrame == 0)
			{
				m_ExitAfterFrame = m_CaptureStep != 0
					? m_CaptureStep + 600
					: m_CaptureFrame + 1;
			}

			if (m_CaptureStep != 0)
				EGSS_CORE_INFO("Capture scheduled: '{0}' at step {1}", m_CapturePath, m_CaptureStep);
			else
				EGSS_CORE_INFO("Capture scheduled: '{0}' at frame {1}", m_CapturePath, m_CaptureFrame);
		}
	}

	void Application::CaptureFrame(const std::string& path)
	{
		m_PendingCapturePath = path;
	}

	Application::~Application()
	{
		// Before anything else: a recording is only valid once its header has
		// been patched with the step count, and that happens here.
		Replay::Stop();

		AudioEngine::Shutdown();
		Renderer::Shutdown();
	}

	void Application::PushLayer(Layer* layer)
	{
		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* overlay)
	{
		m_LayerStack.PushOverlay(overlay);
		overlay->OnAttach();
	}

	void Application::OnEvent(Event& e)
	{
		// While replaying, the keyboard and mouse belong to the recording.
		// A stray keypress from whoever is watching would otherwise be seen by
		// the layers and desynchronise the run from the file -- which looks
		// like the replay being wrong rather than like interference.
		//
		// Window events are let through regardless: closing, resizing and
		// minimising are the host's business, not the recording's.
		if (Replay::IsPlaying() && !Replay::IsDispatchingSyntheticEvent())
		{
			bool isInput = e.IsInCategory(EventCategoryKeyboard)
				|| e.IsInCategory(EventCategoryMouse)
				|| e.IsInCategory(EventCategoryMouseButton);

			if (isInput)
				return;
		}

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(EGSS_BIND_EVENT_FN(Application::OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(EGSS_BIND_EVENT_FN(Application::OnWindowResize));

		// Top-down: overlays get first refusal, and a handled event stops here.
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (e.IsHandled())
				break;
			(*it)->OnEvent(e);
		}
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		if (e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			m_Minimized = true;
			return false;
		}

		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	// Longest real interval fed into the accumulator in one go. Without this,
	// a pause at a breakpoint or a dragged window queues up thousands of
	// simulation steps, which take longer to run than the time they represent,
	// so the loop falls further behind every frame and never recovers -- the
	// "spiral of death". Clamping means the simulation simply loses that time.
	static const float s_MaxFrameTime = 0.25f;

	void Application::Run()
	{
		while (m_Running)
		{
			// Rolls last frame's totals over before anything is timed.
			Instrumentor::NextFrame();
			EGSS_PROFILE_SCOPE("Frame");

			float frameTime;

			if (m_Lockstep)
			{
				// Simulation time, not wall-clock: exactly one fixed step per
				// frame, so frame N *is* step N.
				//
				// Normally the accumulator is fed real elapsed time, which is
				// right for an interactive app and fatal for a reproducible
				// one -- how many steps have run by frame 240 then depends on
				// how fast the machine happened to be, and two identical runs
				// captured at the same frame come back different. Measured:
				// they did.
				frameTime = m_FixedTimestep;
			}
			else
			{
				float time = GetTime();
				frameTime = time - m_LastFrameTime;
				m_LastFrameTime = time;

				if (frameTime > s_MaxFrameTime)
					frameTime = s_MaxFrameTime;
			}

			if (!m_Minimized)
			{
				// Bank the real time, then spend it in fixed-size pieces. The
				// remainder stays in the accumulator for next frame, which is
				// what keeps the simulation rate independent of the frame rate.
				m_Accumulator += frameTime;
				m_FixedStepsLastFrame = 0;

				while (m_Accumulator >= m_FixedTimestep)
				{
					EGSS_PROFILE_SCOPE("Layer::OnFixedUpdate");

					// Input is sampled and replayed here, on the simulation's
					// clock, rather than per frame. A recording indexed by
					// frames would replay differently on a machine that draws
					// them at a different rate.
					Replay::BeginStep(m_StepCount);

					for (Layer* layer : m_LayerStack)
						layer->OnFixedUpdate(m_FixedTimestep);

					m_Accumulator -= m_FixedTimestep;
					m_FixedStepsLastFrame++;
					m_StepCount++;
				}

				// Whatever is left over, as a fraction of one step. Layers use
				// it to render between the last two simulation states rather
				// than snapping to the most recent one.
				m_InterpolationAlpha = m_Accumulator / m_FixedTimestep;

				{
					EGSS_PROFILE_SCOPE("Layer::OnUpdate");

					// Persistent pipeline state back to the baseline before any
					// layer draws, so a layer that leaves it altered cannot
					// reconfigure the next frame -- or the next demo. See
					// RendererAPI::ResetState for what leaked and how it was
					// measured.
					RenderCommand::ResetState();

					// Bottom-up, so later layers draw over earlier ones.
					for (Layer* layer : m_LayerStack)
						layer->OnUpdate(frameTime);
				}

				{
					EGSS_PROFILE_SCOPE("ImGui");

					// The frame is still begun and ended when the UI is hidden,
					// so ImGui's own state stays consistent -- only the panels
					// are skipped. They print frame times in milliseconds,
					// which makes an otherwise reproducible capture differ
					// every run for reasons that have nothing to do with what
					// is being tested.
					m_ImGuiLayer->Begin();

					if (!m_HideUI)
					{
						for (Layer* layer : m_LayerStack)
							layer->OnImGuiRender();
					}

					m_ImGuiLayer->End();
				}
			}
			else
			{
				// Nothing is simulating while minimized, so drop the banked
				// time instead of releasing it in a burst on restore.
				m_Accumulator = 0.0f;
			}

			// The scheduled shot. By simulation step where one was asked for,
			// by frame otherwise -- and >= rather than == because several
			// steps can run in one frame, so the exact number may be stepped
			// straight over.
			if (!m_CapturePath.empty() && !m_Captured)
			{
				bool due = m_CaptureStep != 0
					? m_StepCount >= m_CaptureStep
					: m_FrameCount >= m_CaptureFrame;

				if (due)
				{
					CaptureFrame(m_CapturePath);
					m_Captured = true;
				}
			}

			// The only moment a finished frame exists. After the swap the back
			// buffer's contents are undefined, so a capture taken there reads
			// whatever the driver left behind -- which is the "two probes came
			// back byte-identical" failure wearing a different hat.
			if (!m_PendingCapturePath.empty() && !m_Minimized)
			{
				EGSS_PROFILE_SCOPE("ScreenCapture");

				EGSS_CORE_INFO("Capturing at frame {0}, step {1}", m_FrameCount, m_StepCount);

				ScreenCapture::SaveFrame(m_PendingCapturePath,
					m_Window->GetWidth(), m_Window->GetHeight());

				m_PendingCapturePath.clear();

				// The shot was the whole errand unless told otherwise.
				if (!m_ExitAfterExplicit)
					m_ExitAfterFrame = m_FrameCount + 1;
			}

			{
				// Mostly the VSync wait: the swap blocks until the display is
				// ready. A large number here is the GPU idling, not work.
				EGSS_PROFILE_SCOPE("Window::OnUpdate (swap + poll)");
				m_Window->OnUpdate();
			}

			m_FrameCount++;

			if (m_ExitAfterFrame != 0 && m_FrameCount >= m_ExitAfterFrame)
			{
				EGSS_CORE_INFO("Exiting after {0} frames as requested", m_FrameCount);
				m_Running = false;
			}

			// A replay that has run out has nothing left to drive it, and
			// carrying on would show the simulation continuing from recorded
			// state under no input at all -- which is a different thing from
			// what was recorded and would be easy to mistake for it.
			if (m_ExitWhenReplayEnds && Replay::PlaybackFinished())
			{
				EGSS_CORE_INFO("Replay finished at step {0}", m_StepCount);

				// A capture scheduled past the end of the recording never
				// fires, and the run exits looking entirely successful with no
				// file written. Say so -- an empty output directory is a
				// miserable thing to debug from.
				if (!m_CapturePath.empty() && !m_Captured)
				{
					EGSS_CORE_ERROR("Capture at step {0} never happened: the replay is only {1} steps",
						m_CaptureStep, Replay::GetTotalSteps());
				}

				m_Running = false;
			}
		}
		EGSS_CORE_INFO("Application run loop exiting");
	}
}
