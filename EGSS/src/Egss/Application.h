#pragma once
#include "egsspch.h"
#include "Core.h"

namespace Egss {

	class EGSS_API Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();
	};

	// To be defined in the client
	Application* CreateApplication();
}

