#pragma once

#if defined(GS_PLATFORM_WINDOWS) || defined(GS_PLATFORM_LINUX)

extern GS::Application* GS::CreateApplication();

int main(int argc, char** argv)
{
	GS::Log::Init();

	// Before CreateApplication, because the Application constructor reads its
	// own flags out of this.
	GS::Application::SetCommandLine(argc, argv);

	auto app = GS::CreateApplication();
	app->Run();
	delete app;
}
#endif