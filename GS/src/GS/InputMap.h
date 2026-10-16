#pragma once

// A game's controls as named actions -- "jump", "interact", "fire" -- each
// bound to keys or mouse buttons, rather than keys written into the code
// that reads them. The runtime asks InputMap::Active().Down("jump"); the
// editor's Controls panel rebinds; the binding is saved with the project
// (controls.txt) and shipped with an export; on-screen hints ask Keys() for
// the key's name, so a prompt says "[F]" the moment Interact is moved to F.
//
// Down() reads GS::Input, which a recording replays, so a rebound action
// replays as itself: every key and mouse button is in the recorded state.
//
// The file is lines of `action binding, binding`:
//
//   gs-controls 1
//   interact E
//   fire F
//   shoot Mouse Left
//
// An action the file does not name keeps its default, so a project saved
// before an action existed still gets it.

#include "GS/Core.h"

#include <string>
#include <vector>

namespace GS {

	struct GS_API Binding
	{
		bool Mouse = false;
		int Code = -1;      // a GS_KEY_* or GS_MOUSE_BUTTON_*

		bool operator==(const Binding& other) const { return Mouse == other.Mouse && Code == other.Code; }
		bool operator!=(const Binding& other) const { return !(*this == other); }
	};

	class GS_API InputMap
	{
	public:
		struct Action
		{
			std::string Id;       // what code asks for: "jump"
			std::string Label;    // what the panel shows: "Jump / fly up"
			std::string Group;    // the panel's heading: "Moving"
			std::vector<Binding> Bindings;
			std::vector<Binding> Defaults;
		};

		InputMap();

		// The map a running game reads. GameSession loads the project's into
		// it when play starts; the editor's Controls panel edits it.
		static InputMap& Active();

		std::vector<Action>& Actions() { return m_Actions; }
		const std::vector<Action>& Actions() const { return m_Actions; }
		Action* Find(const std::string& id);
		const Action* Find(const std::string& id) const;

		// Whether any of the action's bindings is held. Callers decide
		// whether the keyboard is the game's (ImGuiLayer::WantsKeyboard).
		bool Down(const std::string& id) const;

		// Its first binding's short name, for a key cap -- "E", "Space",
		// "RMB" -- or every binding joined by " / " when `all`.
		std::string Keys(const std::string& id, bool all = false) const;

		void ResetToDefaults();
		std::string Serialize() const;
		// Sets every action the text names; the rest keep their defaults.
		bool Deserialize(const std::string& text);
		bool Load(const std::string& path);
		bool Save(const std::string& path) const;

		// "Left Shift", "Mouse Right" -- and back. `shortName` is the key-cap
		// spelling ("Shift", "RMB").
		static std::string Name(const Binding& binding, bool shortName = false);
		static bool Parse(const std::string& name, Binding& out);

		// Every key and button the panel can listen for when rebinding.
		static const std::vector<Binding>& Bindable();

		// The project file's name, beside project.gsproj.
		static const char* FileName() { return "controls.txt"; }

	private:
		std::vector<Action> m_Actions;
	};

}
