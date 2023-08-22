#pragma once

#ifdef EGSS_PLATFORM_WINDOWS

extern Egss::Application* Egss::CreateApplication();

int main(int argc, char** argv)
{
	auto app = Egss::CreateApplication();
	app->Run();
	delete app;
}
#endif