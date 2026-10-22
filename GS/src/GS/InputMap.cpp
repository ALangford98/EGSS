#include "gspch.h"
#include "GS/InputMap.h"
#include "GS/Input.h"
#include "GS/KeyCodes.h"
#include "GS/MouseButtonCodes.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace GS {

	namespace {
		Binding Key(int code) { return { false, code }; }
		Binding Mouse(int button) { return { true, button }; }

		struct Named
		{
			int Code;
			const char* Long;
			const char* Short;
		};

		// The keys with names of more than one character. Letters, digits and
		// F-keys are spelled out by rule below.
		const std::vector<Named>& NamedKeys()
		{
			static const std::vector<Named> keys =
			{
				{ GS_KEY_SPACE, "Space", "Space" }, { GS_KEY_ESCAPE, "Escape", "Esc" }, { GS_KEY_ENTER, "Enter", "Enter" },
				{ GS_KEY_TAB, "Tab", "Tab" }, { GS_KEY_BACKSPACE, "Backspace", "Bksp" }, { GS_KEY_INSERT, "Insert", "Ins" },
				{ GS_KEY_DELETE, "Delete", "Del" }, { GS_KEY_RIGHT, "Right", "Right" }, { GS_KEY_LEFT, "Left", "Left" },
				{ GS_KEY_DOWN, "Down", "Down" }, { GS_KEY_UP, "Up", "Up" }, { GS_KEY_PAGE_UP, "Page Up", "PgUp" },
				{ GS_KEY_PAGE_DOWN, "Page Down", "PgDn" }, { GS_KEY_HOME, "Home", "Home" }, { GS_KEY_END, "End", "End" },
				{ GS_KEY_CAPS_LOCK, "Caps Lock", "Caps" }, { GS_KEY_LEFT_SHIFT, "Left Shift", "Shift" },
				{ GS_KEY_RIGHT_SHIFT, "Right Shift", "RShift" }, { GS_KEY_LEFT_CONTROL, "Left Ctrl", "Ctrl" },
				{ GS_KEY_RIGHT_CONTROL, "Right Ctrl", "RCtrl" }, { GS_KEY_LEFT_ALT, "Left Alt", "Alt" },
				{ GS_KEY_RIGHT_ALT, "Right Alt", "RAlt" }, { GS_KEY_GRAVE_ACCENT, "`", "`" }, { GS_KEY_MINUS, "-", "-" },
				{ GS_KEY_EQUAL, "=", "=" }, { GS_KEY_LEFT_BRACKET, "[", "[" }, { GS_KEY_RIGHT_BRACKET, "]", "]" },
				{ GS_KEY_BACKSLASH, "\\", "\\" }, { GS_KEY_SEMICOLON, ";", ";" }, { GS_KEY_APOSTROPHE, "'", "'" },
				{ GS_KEY_COMMA, ",", "," }, { GS_KEY_PERIOD, ".", "." }, { GS_KEY_SLASH, "/", "/" },
			};
			return keys;
		}

		const std::vector<Named>& NamedButtons()
		{
			static const std::vector<Named> buttons =
			{
				{ GS_MOUSE_BUTTON_LEFT, "Mouse Left", "LMB" }, { GS_MOUSE_BUTTON_RIGHT, "Mouse Right", "RMB" },
				{ GS_MOUSE_BUTTON_MIDDLE, "Mouse Middle", "MMB" }, { GS_MOUSE_BUTTON_4, "Mouse 4", "M4" },
				{ GS_MOUSE_BUTTON_5, "Mouse 5", "M5" },
			};
			return buttons;
		}

		std::string Trim(const std::string& text)
		{
			size_t from = text.find_first_not_of(" \t\r");
			if (from == std::string::npos)
				return "";
			return text.substr(from, text.find_last_not_of(" \t\r") - from + 1);
		}

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			return text;
		}
	}

	InputMap::InputMap()
	{
		// What a game written before this existed already did, as defaults:
		// the keys CharacterController, GameModes and the launcher read.
		m_Actions =
		{
			{ "forward",  "Forward",                  "Moving", {}, { Key(GS_KEY_W) } },
			{ "back",     "Back",                     "Moving", {}, { Key(GS_KEY_S) } },
			{ "left",     "Left",                     "Moving", {}, { Key(GS_KEY_A) } },
			{ "right",    "Right",                    "Moving", {}, { Key(GS_KEY_D) } },
			{ "jump",     "Jump / fly up",            "Moving", {}, { Key(GS_KEY_SPACE) } },
			{ "down",     "Fly down",                 "Moving", {}, { Key(GS_KEY_LEFT_CONTROL), Key(GS_KEY_C) } },
			{ "sprint",   "Run / fly faster",         "Moving", {}, { Key(GS_KEY_LEFT_SHIFT), Key(GS_KEY_RIGHT_SHIFT) } },
			// The buttons, while a character is played with the mouse
			// looking (GameModes' mouse look): the index finger reacts
			// fastest, and a dodge is a reaction. Left Alt was the first
			// thought -- the thumb beside Space -- but on KDE Alt and a
			// right drag resizes the window.
			{ "dodge",    "Dodge (with a direction)", "Moving", {}, { Mouse(GS_MOUSE_BUTTON_LEFT), Key(GS_KEY_Q) } },
			// The sword and shield (Runtime/Armament.h), where a character
			// carries them: the index finger swings -- a ball is struck back
			// on the timing of the press -- and the guard is held on the
			// button that also looks at what is coming, which is what a
			// guard is for. Armed, the dodge gives up its left button to the
			// swing and keeps Q.
			{ "attack",   "Swing the sword",          "Fighting", {}, { Mouse(GS_MOUSE_BUTTON_LEFT), Key(GS_KEY_F) } },
			{ "block",    "Shield up (hold)",         "Fighting", {}, { Mouse(GS_MOUSE_BUTTON_RIGHT), Key(GS_KEY_V) } },
			{ "look",     "Look (hold)",              "Camera", {}, { Mouse(GS_MOUSE_BUTTON_RIGHT) } },
			{ "focus",    "Look at what is coming (hold)", "Camera", {}, { Mouse(GS_MOUSE_BUTTON_RIGHT), Key(GS_KEY_TAB) } },
			{ "turnleft", "Turn left",                "Camera", {}, { Key(GS_KEY_LEFT) } },
			{ "turnright","Turn right",               "Camera", {}, { Key(GS_KEY_RIGHT) } },
			{ "turnup",   "Look up",                  "Camera", {}, { Key(GS_KEY_UP) } },
			{ "turndown", "Look down",                "Camera", {}, { Key(GS_KEY_DOWN) } },
			{ "interact", "Use / take control",       "Using things", {}, { Key(GS_KEY_E) } },
			{ "cancel",   "Step back / let go",       "Using things", {}, { Key(GS_KEY_ESCAPE) } },
			{ "shoot",    "Fire (at the cannon)", "Using things", {}, { Mouse(GS_MOUSE_BUTTON_LEFT) } },
			{ "fire",     "Fire the launcher",        "Using things", {}, { Key(GS_KEY_F) } },
			{ "terminal", "Open the terminal",        "Using things", {}, { Key(GS_KEY_GRAVE_ACCENT) } },
			{ "restart",  "Start again",              "Playing", {}, { Key(GS_KEY_R) } },
			{ "next",     "Next / a new one",         "Playing", {}, { Key(GS_KEY_N) } },
		};
		ResetToDefaults();
	}

	InputMap& InputMap::Active()
	{
		static InputMap map;
		return map;
	}

	InputMap::Action* InputMap::Find(const std::string& id)
	{
		for (Action& action : m_Actions)
			if (action.Id == id)
				return &action;
		return nullptr;
	}

	const InputMap::Action* InputMap::Find(const std::string& id) const
	{
		return const_cast<InputMap*>(this)->Find(id);
	}

	bool InputMap::Down(const std::string& id) const
	{
		const Action* action = Find(id);
		if (!action)
			return false;
		for (const Binding& binding : action->Bindings)
			if (binding.Mouse ? Input::IsMouseButtonPressed(binding.Code) : Input::IsKeyPressed(binding.Code))
				return true;
		return false;
	}

	std::string InputMap::Keys(const std::string& id, bool all) const
	{
		const Action* action = Find(id);
		if (!action || action->Bindings.empty())
			return all ? "(none)" : "?";
		if (!all)
			return Name(action->Bindings.front(), true);
		std::string out;
		for (const Binding& binding : action->Bindings)
			out += (out.empty() ? "" : " / ") + Name(binding, true);
		return out;
	}

	void InputMap::ResetToDefaults()
	{
		for (Action& action : m_Actions)
			action.Bindings = action.Defaults;
	}

	std::string InputMap::Serialize() const
	{
		std::ostringstream out;
		out << "gs-controls 1\n";
		for (const Action& action : m_Actions)
		{
			out << action.Id;
			for (size_t i = 0; i < action.Bindings.size(); i++)
				out << (i == 0 ? " " : ", ") << Name(action.Bindings[i]);
			out << "\n";
		}
		return out.str();
	}

	bool InputMap::Deserialize(const std::string& text)
	{
		ResetToDefaults();
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			line = Trim(line);
			if (line.empty() || line.rfind("gs-controls", 0) == 0 || line[0] == '#')
				continue;
			size_t space = line.find(' ');
			std::string id = line.substr(0, space);
			Action* action = Find(id);
			if (!action)
				continue;   // an action this build does not have
			action->Bindings.clear();
			if (space == std::string::npos)
				continue;   // named with nothing: deliberately unbound
			std::istringstream parts(line.substr(space + 1));
			std::string part;
			while (std::getline(parts, part, ','))
			{
				Binding binding;
				if (Parse(Trim(part), binding))
					action->Bindings.push_back(binding);
			}
		}
		return true;
	}

	bool InputMap::Load(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			ResetToDefaults();
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();
		return Deserialize(buffer.str());
	}

	bool InputMap::Save(const std::string& path) const
	{
		std::ofstream file(path, std::ios::binary);
		return (bool)(file << Serialize());
	}

	std::string InputMap::Name(const Binding& binding, bool shortName)
	{
		if (binding.Mouse)
		{
			for (const Named& button : NamedButtons())
				if (button.Code == binding.Code)
					return shortName ? button.Short : button.Long;
			return "Mouse " + std::to_string(binding.Code + 1);
		}
		int code = binding.Code;
		if ((code >= GS_KEY_A && code <= GS_KEY_Z) || (code >= GS_KEY_0 && code <= GS_KEY_9))
			return std::string(1, (char)code);
		if (code >= GS_KEY_F1 && code <= GS_KEY_F12)
			return "F" + std::to_string(code - GS_KEY_F1 + 1);
		if (code >= GS_KEY_KP_0 && code <= GS_KEY_KP_9)
			return (shortName ? "Num" : "Keypad ") + std::to_string(code - GS_KEY_KP_0);
		for (const Named& key : NamedKeys())
			if (key.Code == code)
				return shortName ? key.Short : key.Long;
		return "Key " + std::to_string(code);
	}

	bool InputMap::Parse(const std::string& name, Binding& out)
	{
		std::string text = Trim(name);
		if (text.empty())
			return false;
		if (text.size() == 1 && std::isalnum((unsigned char)text[0]))
		{
			out = Key(std::toupper((unsigned char)text[0]));
			return true;
		}
		std::string lower = Lower(text);
		if (lower.size() >= 2 && lower[0] == 'f' && std::all_of(lower.begin() + 1, lower.end(), ::isdigit))
		{
			int n = std::stoi(lower.substr(1));
			if (n >= 1 && n <= 12)
			{
				out = Key(GS_KEY_F1 + n - 1);
				return true;
			}
		}
		// "Keypad 4", or the key cap's "Num4".
		for (const char* prefix : { "keypad ", "num" })
		{
			size_t n = std::strlen(prefix);
			if (lower.rfind(prefix, 0) == 0 && lower.size() == n + 1 && std::isdigit((unsigned char)lower[n]))
			{
				out = Key(GS_KEY_KP_0 + (lower[n] - '0'));
				return true;
			}
		}
		for (const Named& key : NamedKeys())
			if (Lower(key.Long) == lower || Lower(key.Short) == lower)
			{
				out = Key(key.Code);
				return true;
			}
		for (const Named& button : NamedButtons())
			if (Lower(button.Long) == lower || Lower(button.Short) == lower)
			{
				out = Mouse(button.Code);
				return true;
			}
		if (lower.rfind("key ", 0) == 0)
		{
			out = Key(std::atoi(lower.c_str() + 4));
			return out.Code > 0;
		}
		return false;
	}

	const std::vector<Binding>& InputMap::Bindable()
	{
		static const std::vector<Binding> all = []()
		{
			std::vector<Binding> list;
			for (int c = GS_KEY_A; c <= GS_KEY_Z; c++) list.push_back(Key(c));
			for (int c = GS_KEY_0; c <= GS_KEY_9; c++) list.push_back(Key(c));
			for (int c = GS_KEY_F1; c <= GS_KEY_F12; c++) list.push_back(Key(c));
			for (int c = GS_KEY_KP_0; c <= GS_KEY_KP_9; c++) list.push_back(Key(c));
			for (const Named& key : NamedKeys()) list.push_back(Key(key.Code));
			for (const Named& button : NamedButtons()) list.push_back(Mouse(button.Code));
			return list;
		}();
		return all;
	}

}
