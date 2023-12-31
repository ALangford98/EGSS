#include "egsspch.h"
#include "Application.h"

#include "Window.h"
#include"Events/ApplicationEvent.h"
#include"Log.h"

namespace Egss {
	Application::Application()
	{
		m_Window = std::unique_ptr<Window>(Window::Create());
	}

	Application::~Application()
	{
	}

	void Application::Run()
	{
		
		while (m_Running)
		{
			m_Window->OnUpdate();
		}
	}
}
