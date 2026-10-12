#pragma once

// Every platform the engine builds for -- the web included. Leaving one out
// is silent: the web build once linked with no main at all, Emscripten said
// nothing, and the .wasm came out at 172 KB with the whole engine
// dead-stripped from it.
#if defined(GS_PLATFORM_WINDOWS) || defined(GS_PLATFORM_LINUX) || defined(GS_PLATFORM_WEB)

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