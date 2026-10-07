#pragma once

#include "GS/Core.h"

#include <string>

namespace GS::Assets {

	// Where a running game turns a stored path into a file to open. Today
	// that is a directory join; on the web it will be a virtual filesystem
	// and on Android the APK, and those ports redirect this one function
	// rather than finding every file open again. See
	// docs/superpowers/specs/2026-10-07-game-runtime-player-design.md.
	//
	// Rules, in order: empty, absolute, and "primitive:" keys pass through;
	// with a root set, <root>/<path> if that file exists; otherwise the path
	// unchanged, relative to the working directory. The fallback is not a
	// convenience -- projects store working-directory-relative paths today
	// (BreakoutRecreation's scripts are "assets/demos/..."), and engine files
	// such as the TypeScript compiler live beside the executable.
	//
	// The editor sets no root, and every path then resolves to itself.
	GS_API void SetRoot(const std::string& root);
	GS_API const std::string& GetRoot();
	GS_API std::string Resolve(const std::string& path);

}
