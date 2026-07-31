#pragma once


#ifdef EGSS_PLATFORM_WINDOWS
	#ifdef EGSS_BUILD_DLL
		#define EGSS_API __declspec(dllexport)
	#else
		#define EGSS_API __declspec(dllimport)
	#endif
	#define EGSS_DEBUGBREAK() __debugbreak()
#elif defined(EGSS_PLATFORM_LINUX)
	// ELF has no import side: default visibility on the exporting library is enough.
	#ifdef EGSS_BUILD_DLL
		#define EGSS_API __attribute__((visibility("default")))
	#else
		#define EGSS_API
	#endif
	#include <signal.h>
	#define EGSS_DEBUGBREAK() raise(SIGTRAP)
#else
	#error Unsupported Platform.
#endif

#ifdef EGSS_ENABLE_ASSERTS
	#define EGSS_ASSERT(x, ...){if(!(x)) {EGSS_ERROR("Assertion failed: {0}", __VA_ARGS__); EGSS_DEBUGBREAK(); } }
	#define EGSS_CORE_ASSERT(x, ...){if(!(x)) {EGSS_CORE_ERROR("Assertion failed: {0}", __VA_ARGS__); EGSS_DEBUGBREAK(); } }
#else
	#define EGSS_ASSERT(x, ...)
	#define EGSS_CORE_ASSERT(x, ...)
#endif


#define BIT(x) (1 << x)