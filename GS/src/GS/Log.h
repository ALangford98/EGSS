#pragma once

#include "Core.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include"spdlog/fmt/ostr.h"

namespace GS {
	class GS_API Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};

}


#define GS_CORE_TRACE(...) ::GS::Log::GetCoreLogger()->trace (__VA_ARGS__)
#define GS_CORE_INFO(...)  ::GS::Log::GetCoreLogger()->info  (__VA_ARGS__)
#define GS_CORE_WARN(...)  ::GS::Log::GetCoreLogger()->warn  (__VA_ARGS__)
#define GS_CORE_ERROR(...) ::GS::Log::GetCoreLogger()->error (__VA_ARGS__)
#define GS_CORE_CRITICAL(...) ::GS::Log::GetCoreLogger()->critical (__VA_ARGS__)

#define GS_TRACE(...) ::GS::Log::GetClientLogger()->trace (__VA_ARGS__)
#define GS_INFO(...)  ::GS::Log::GetClientLogger()->info  (__VA_ARGS__)
#define GS_WARN(...)  ::GS::Log::GetClientLogger()->warn  (__VA_ARGS__)
#define GS_ERROR(...) ::GS::Log::GetClientLogger()->error (__VA_ARGS__)
#define GS_CRITICAL(...) ::GS::Log::GetClientLogger()->critical (__VA_ARGS__)

