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

#define BIT(x) (1 << x)