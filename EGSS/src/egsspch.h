#pragma once

#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>
#include <chrono>

#include <string>
#include <sstream>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>

#include <cstring>

#ifdef EGSS_PLATFORM_WINDOWS
	// Windows.h defines min/max as macros, which collide with std::min and
	// std::max -- glm uses both, so this must be set before the include.
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	// Trims rarely-used headers, and avoids some of the macro pollution that
	// clashes with GL headers.
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <Windows.h>
#endif


