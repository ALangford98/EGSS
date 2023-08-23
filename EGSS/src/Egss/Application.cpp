#include "Application.h"

#include"Events/ApplicationEvent.h"
#include"Log.h"

namespace Egss {
	Application::Application()
	{
	}

	Application::~Application()
	{
	}

	void Application::Run()
	{
		WindowResizeEvent e(1280, 720);
		EGSS_TRACE(e);

		while (true);
	}
}
