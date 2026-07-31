#pragma once

#if defined(EGSS_PLATFORM_WINDOWS) || defined(EGSS_PLATFORM_LINUX)

extern Egss::Application* Egss::CreateApplication();

int main(int argc, char** argv)
{
	Egss::Log::Init();
	


	auto app = Egss::CreateApplication();
	app->Run();
	delete app;
}
#endif