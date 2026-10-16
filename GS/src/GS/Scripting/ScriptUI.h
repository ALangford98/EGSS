#pragma once

#include "GS/Core.h"

#include <string>

namespace GS::ScriptUI {

	// What a GSS script may do to the game's UI (Runtime/GameUI.h): show and
	// hide its screens, set and read the values they display, and send the
	// game a command -- the words the game console and every button speak.
	// Both ways a script runs come through here: the interpreted path's
	// natives call these, the transpiler emits calls to them. A running game
	// (GameSession) installs the backend; without one each call does nothing
	// and Get reads empty.
	class GS_API Backend
	{
	public:
		virtual ~Backend() = default;
		virtual void Show(const std::string& screen, bool shown) = 0;
		virtual void Set(const std::string& name, const std::string& value) = 0;
		virtual std::string Get(const std::string& name) = 0;
		virtual void Command(const std::string& line) = 0;
	};

	GS_API void SetBackend(Backend* backend);

	GS_API void Show(const std::string& screen, bool shown = true);
	GS_API void Hide(const std::string& screen);
	GS_API void Set(const std::string& name, const std::string& value);
	GS_API void Set(const std::string& name, double value);
	inline void Set(const std::string& name, const char* value) { Set(name, std::string(value)); }
	GS_API std::string Get(const std::string& name);
	GS_API void Command(const std::string& line);

}
