#include "egsspch.h"
#include "Application.h"

#include "Window.h"
#include"Events/ApplicationEvent.h"
#include"Log.h"

#include "Egss/Renderer/Renderer.h"

namespace Egss {

	Application* Application::s_Instance = nullptr;

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

		m_Window = std::unique_ptr<Window>(Window::Create());
		m_Window->SetEventCallback(EGSS_BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();

		// Owned by the layer stack, but kept as a pointer so Run can bracket
		// the per-layer ImGui rendering.
		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	Application::~Application()
	{
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

	void Application::Run()
	{
		while (m_Running)
		{
			float time = GetTime();
			Timestep timestep = time - m_LastFrameTime;
			m_LastFrameTime = time;

			if (!m_Minimized)
			{
				// Bottom-up, so later layers draw over earlier ones.
				for (Layer* layer : m_LayerStack)
					layer->OnUpdate(timestep);

				m_ImGuiLayer->Begin();
				for (Layer* layer : m_LayerStack)
					layer->OnImGuiRender();
				m_ImGuiLayer->End();
			}

			m_Window->OnUpdate();
		}
	}
}
