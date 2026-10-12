#include "gspch.h"
#include "Log.h"

#ifdef GS_PLATFORM_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif

namespace GS {
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	void Log::Init() 
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");

#ifdef GS_PLATFORM_ANDROID
		// An app's stdout goes nowhere on Android; logcat is where a
		// developer looks (adb logcat -s GS APPLICATION).
		s_CoreLogger = spdlog::android_logger_mt("GS", "GS");
		s_CoreLogger->set_level(spdlog::level::trace);
		s_ClientLogger = spdlog::android_logger_mt("APPLICATION", "APPLICATION");
#else
		s_CoreLogger = spdlog::stdout_color_mt("GS");
		s_CoreLogger->set_level(spdlog::level::trace);
		s_ClientLogger = spdlog::stdout_color_mt("APPLICATION");
#endif
		s_ClientLogger->set_level(spdlog::level::trace);

	}

}