#pragma once

#ifdef EGSS_PLATFORM_WINDOWS

extern Egss::Application* Egss::CreateApplication();

int main(int argc, char** argv)
{
	Egss::Log::Init();
	EGSS_CORE_TRACE("This is a Trace Log");
	Egss::Log::GetCoreLogger()->info("This is an info Log");
	EGSS_CORE_WARN("This is a warning");
	EGSS_CORE_ERROR("This is an error");


	auto app = Egss::CreateApplication();
	app->Run();
	delete app;
}
#endif