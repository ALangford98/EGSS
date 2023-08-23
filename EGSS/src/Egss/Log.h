#pragma once

#include<memory>

#include "Core.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include"spdlog/fmt/ostr.h"

namespace Egss {
	class EGSS_API Log
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


#define EGSS_CORE_TRACE(...) ::Egss::Log::GetCoreLogger()->trace (__VA_ARGS__)
#define EGSS_CORE_INFO(...)  ::Egss::Log::GetCoreLogger()->info  (__VA_ARGS__)
#define EGSS_CORE_WARN(...)  ::Egss::Log::GetCoreLogger()->warn  (__VA_ARGS__)
#define EGSS_CORE_ERROR(...) ::Egss::Log::GetCoreLogger()->error (__VA_ARGS__)
#define EGSS_CORE_FATAL(...) ::Egss::Log::GetCoreLogger()->fatal (__VA_ARGS__)

#define EGSS_TRACE(...) ::Egss::Log::GetClientLogger()->trace (__VA_ARGS__)
#define EGSS_INFO(...)  ::Egss::Log::GetClientLogger()->info  (__VA_ARGS__)
#define EGSS_WARN(...)  ::Egss::Log::GetClientLogger()->warn  (__VA_ARGS__)
#define EGSS_ERROR(...) ::Egss::Log::GetClientLogger()->error (__VA_ARGS__)
#define EGSS_FATAL(...) ::Egss::Log::GetClientLogger()->fatal (__VA_ARGS__)

