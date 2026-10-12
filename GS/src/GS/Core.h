#pragma once

// GLES 3.0 rather than desktop GL: WebGL2 in a browser, OpenGL ES on Android
// and iOS. The renderer's dialect differences (shader version line, clears,
// the calls GLES lacks) key off this; platform matters (main loop, window)
// do not.
#if (defined(GS_PLATFORM_WEB) || defined(GS_PLATFORM_ANDROID) || defined(GS_PLATFORM_IOS)) && !defined(GS_GLES)
	#define GS_GLES
#endif


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
#elif defined(GS_PLATFORM_WEB) || defined(GS_PLATFORM_ANDROID) || defined(GS_PLATFORM_IOS)
	// The web, Android and iOS builds link the engine statically into one
	// module (the wasm, the app's single .so, the app binary): nothing to
	// export, no import side.
	#define GS_API
	#define GS_DEBUGBREAK() __builtin_trap()
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