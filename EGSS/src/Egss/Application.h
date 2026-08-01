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

		static Application* s_Instance;
	};

	// To be defined in the client
	Application* CreateApplication();
}
