#pragma once


#ifdef EGSS_PLATFORM_WINDOWS
	#ifdef EGSS_BUILD_DLL
		#define EGSS_API __declspec(dllexport)
	#else
		#define EGSS_API __declspec(dllimport)
	#endif
#else
	#error Unsupported Platform.
#endif

#ifdef EGSS_ENABLE_ASSERTS
	#define EGSS_ASSERT(x, ...){if(!(x)) {HZ_ERROR("Assertion failed: {0}", __VAARGS__); __debugbreak(); } }
	#define EGSS_CORE_ASSERT(x, ...){if(!(x)) {HZ_ERROR("Assertion failed: {0}", __VAARGS__); __debugbreak(); } }
#else
	#define EGSS_ASSERT(x, ...)
	#define EGSS_CORE_ASSERT(x, ...)
#endif


#define BIT(x) (1 << x)