#pragma once


#ifdef GS_PLATFORM_WINDOWS
	#ifdef GS_BUILD_DLL
		#define GS_API __declspec(dllexport)
	#else
		#define GS_API __declspec(dllimport)
	#endif
	#define GS_DEBUGBREAK() __debugbreak()
#elif defined(GS_PLATFORM_LINUX)
	// ELF has no import side: default visibility on the exporting library is enough.
	#ifdef GS_BUILD_DLL
		#define GS_API __attribute__((visibility("default")))
	#else
		#define GS_API
	#endif
	#include <signal.h>
	#define GS_DEBUGBREAK() raise(SIGTRAP)
#else
	#error Unsupported Platform.
#endif

#ifdef GS_ENABLE_ASSERTS
	#define GS_ASSERT(x, ...){if(!(x)) {GS_ERROR("Assertion failed: {0}", __VA_ARGS__); GS_DEBUGBREAK(); } }
	#define GS_CORE_ASSERT(x, ...){if(!(x)) {GS_CORE_ERROR("Assertion failed: {0}", __VA_ARGS__); GS_DEBUGBREAK(); } }
#else
	#define GS_ASSERT(x, ...)
	#define GS_CORE_ASSERT(x, ...)
#endif


#define BIT(x) (1 << x)

// Binds a member function as an event handler: GS_BIND_EVENT_FN(OnWindowClose)
#define GS_BIND_EVENT_FN(fn) std::bind(&fn, this, std::placeholders::_1)