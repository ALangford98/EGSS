#include "gspch.h"
#include "GS/Scripting/ScriptUI.h"

#include <cstdio>

namespace GS::ScriptUI {

	namespace {
		Backend* s_Backend = nullptr;
	}

	void SetBackend(Backend* backend) { s_Backend = backend; }

	void Show(const std::string& screen, bool shown) { if (s_Backend) s_Backend->Show(screen, shown); }
	void Hide(const std::string& screen) { Show(screen, false); }
	void Set(const std::string& name, const std::string& value) { if (s_Backend) s_Backend->Set(name, value); }

	void Set(const std::string& name, double value)
	{
		// The shortest spelling that reads back as the same float, so a
		// label showing it is not "0.30000001".
		char buffer[32];
		std::snprintf(buffer, sizeof buffer, "%g", value);
		Set(name, std::string(buffer));
	}

	std::string Get(const std::string& name) { return s_Backend ? s_Backend->Get(name) : std::string(); }
	void Command(const std::string& line) { if (s_Backend) s_Backend->Command(line); }

}
