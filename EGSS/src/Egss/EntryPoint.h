#pragma once

#if defined(EGSS_PLATFORM_WINDOWS) || defined(EGSS_PLATFORM_LINUX)

extern Egss::Application* Egss::CreateApplication();

int main(int argc, char** argv)
{
	Egss::Log::Init();

	// Before CreateApplication, because the Application constructor reads its
	// own flags out of this.
	Egss::Application::SetCommandLine(argc, argv);

	auto app = Egss::CreateApplication();
	app->Run();
	delete app;
}
#endif