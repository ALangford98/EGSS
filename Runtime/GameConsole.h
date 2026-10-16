#pragma once

// The game console: a terminal over a running game, for typing commands
// into it. Part of the runtime, so it is there in the editor's Play, in an
// exported game and in a TestEnv demo alike; the game registers what it
// understands (GameSession's own commands, the launcher's), the host only
// says where to draw it.
//
// ` (backtick) opens and closes it. While its input has the keyboard, ImGui
// says so, and the game's own input -- which already checks
// ImGuiLayer::WantsKeyboard -- stands aside: typing "speed 20" does not walk
// anyone forwards.
//
// Commands typed are queued and run at the start of the next fixed step,
// not in the middle of drawing: they change the simulation, and the
// simulation changes only on its own steps.

#include <GS.h>
#include <imgui.h>

#include "GameUI.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

class GameConsole
{
public:
	using Args = std::vector<std::string>;
	using Handler = std::function<void(const Args&)>;

	enum class Tone { Plain, Echo, Good, Bad, Title, Dim };

	void Register(const std::string& name, const std::string& usage, const std::string& help, Handler handler)
	{
		m_Commands[name] = { usage, help, std::move(handler) };
	}

	void Print(const std::string& text, Tone tone = Tone::Plain)
	{
		// A command from the HUD runs quietly: moving a slider should not
		// fill the terminal with its every position. What went wrong still
		// says so.
		if (m_Quiet && tone != Tone::Bad)
			return;
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
			m_Lines.push_back({ line, tone });
		if (m_Lines.size() > 400)
			m_Lines.erase(m_Lines.begin(), m_Lines.begin() + (m_Lines.size() - 400));
		m_ScrollToEnd = true;
	}
	void Good(const std::string& text) { Print(text, Tone::Good); }
	void Bad(const std::string& text) { Print(text, Tone::Bad); }

	void ClearText() { m_Lines.clear(); }

	// A line as if typed; run at the next fixed step. `echo` false is a
	// line from the game's own UI, run without printing what it says.
	void Submit(const std::string& line, bool echo = true) { m_Queue.push_back({ line, echo }); }

	// Runs whatever was typed since the last fixed step.
	void RunQueued()
	{
		std::vector<Queued> queue;
		queue.swap(m_Queue);
		for (const Queued& q : queue)
			Execute(q.Line, q.Echo);
	}

	void Execute(const std::string& line, bool echo = true)
	{
		Args words = Split(line);
		if (words.empty())
			return;
		m_Quiet = !echo;
		Print("> " + line, Tone::Echo);
		std::string name = Lower(words[0]);
		words.erase(words.begin());
		auto found = m_Commands.find(name);
		if (found == m_Commands.end())
		{
			Bad("unknown command '" + name + "' -- type help");
			m_Quiet = false;
			return;
		}
		found->second.Run(words);
		m_Quiet = false;
	}

	bool Knows(const std::string& name) const { return m_Commands.count(Lower(name)) > 0; }

	// Every command, for "help".
	void PrintHelp()
	{
		Print("commands", Tone::Title);
		for (auto& [name, command] : m_Commands)
		{
			std::string usage = command.Usage.empty() ? name : name + " " + command.Usage;
			Print("  " + usage);
			Print("      " + command.Help, Tone::Dim);
		}
	}

	bool IsOpen() const { return m_Open; }

	// The terminal is a thing in the world -- a computer's screen, drawn by
	// DrawTerminal into its texture -- rather than a window over the game.
	// Open, it still takes the keyboard through an ImGui text field, which
	// keeps history, completion and editing exactly as they are and tells
	// the editor someone is typing; the field is simply not drawn.
	void SetEmbedded(bool embedded) { m_Embedded = embedded; }
	bool IsEmbedded() const { return m_Embedded; }
	void SetOpen(bool open) { m_Open = open; m_FocusInput = open; }

	// One line shown along the bottom of the view while the terminal is
	// closed: what is set up, and how to act on it.
	void SetStatus(const std::string& status) { m_Status = status; }

	// Draws the terminal (or, closed, the status line) inside the given
	// rectangle, in ImGui's screen coordinates: the game's viewport.
	void Draw(float x, float y, float width, float height)
	{
		if (width <= 0.0f || height <= 0.0f)
			return;

		// The toggle -- not while some other text field has the keyboard.
		ImGuiIO& io = ImGui::GetIO();
		bool typingElsewhere = io.WantTextInput && !m_InputActive;
		if (!typingElsewhere && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
			SetOpen(!m_Open);

		if (m_Open && !typingElsewhere && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			SetOpen(false);

		if (!m_Open)
		{
			if (!m_Embedded)
				DrawStatus(x, y, width, height);
			m_InputActive = false;
			return;
		}

		if (m_Embedded)
		{
			DrawHiddenInput(x, y);
			return;
		}

		float w = std::clamp(width * 0.55f, std::min(420.0f, width), width);
		float h = std::clamp(height * 0.45f, std::min(220.0f, height), height);
		ImGui::SetNextWindowPos(ImVec2(x + 8.0f, y + height - h - 8.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(w - 16.0f, h), ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.05f, 0.04f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.75f, 0.45f, 0.6f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.6f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 1.0f, 0.7f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##GameConsole", nullptr, flags))
		{
			ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.5f, 1.0f), "TERMINAL");
			ImGui::SameLine();
			ImGui::TextDisabled("  `  to close   help  for commands   up/down history   tab completes");
			ImGui::Separator();

			float inputHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
			ImGui::BeginChild("##ConsoleLines", ImVec2(0.0f, -inputHeight), false);
			for (const Line& line : m_Lines)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ColourOf(line.Kind));
				ImGui::TextWrapped("%s", line.Text.c_str());
				ImGui::PopStyleColor();
			}
			if (m_ScrollToEnd)
				ImGui::SetScrollHereY(1.0f);
			m_ScrollToEnd = false;
			ImGui::EndChild();

			ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1.0f), ">");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1.0f);
			InputField();
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);
	}

	// The terminal as a screen: a CRT's worth of green on black, the
	// scrollback, the line being typed with its cursor, and a header. Drawn
	// into a texture `width` x `height` (ImGui's coordinates, top left
	// origin). `focused` is whether someone is at it; `time` blinks the
	// cursor.
	void DrawTerminal(ImDrawList* list, float width, float height, bool focused, float time, const std::string& title)
	{
		float s = height / 640.0f;
		// Large: it is read on a monitor across a desk, at a slant, from a
		// texture the screen shrinks.
		float font = 27.0f * s;
		ImU32 green = IM_COL32(120, 255, 160, 255);
		list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height),
			IM_COL32(4, 20, 14, 255), IM_COL32(4, 20, 14, 255), IM_COL32(2, 10, 8, 255), IM_COL32(2, 10, 8, 255));

		// The header bar.
		float header = 46.0f * s;
		list->AddRectFilled(ImVec2(0, 0), ImVec2(width, header), IM_COL32(18, 70, 44, 255));
		GameUI::AddText(list, GameUI::Font(true), font, ImVec2(18.0f * s, (header - font) * 0.5f), IM_COL32(200, 255, 220, 255), title.c_str());
		const char* state = focused ? "ONLINE  //  ESC TO STEP BACK" : "STANDBY  //  [E] TO USE";
		ImVec2 stateSize = GameUI::Font(true)->CalcTextSizeA(font * 0.8f, FLT_MAX, 0.0f, state);
		GameUI::AddText(list, GameUI::Font(true), font * 0.8f, ImVec2(width - stateSize.x - 18.0f * s, (header - font * 0.8f) * 0.5f),
			focused ? IM_COL32(255, 220, 120, 255) : IM_COL32(120, 200, 150, 255), state);

		// The scrollback, from the bottom up, wrapped to the width.
		float left = 18.0f * s, right = width - 18.0f * s;
		float bottom = height - 56.0f * s;
		float line = font * 1.25f;
		float y = bottom - line;
		for (int i = (int)m_Lines.size() - 1; i >= 0 && y > header + 4.0f * s; i--)
		{
			const Line& l = m_Lines[(size_t)i];
			ImVec4 c = ColourOf(l.Kind);
			std::vector<std::string> wrapped = Wrap(l.Text, font, right - left);
			for (int k = (int)wrapped.size() - 1; k >= 0 && y > header + 4.0f * s; k--)
			{
				GameUI::AddText(list, GameUI::Font(true), font, ImVec2(left, y), ImGui::ColorConvertFloat4ToU32(c), wrapped[(size_t)k].c_str());
				y -= line;
			}
		}

		// The input line.
		list->AddLine(ImVec2(left, bottom + 6.0f * s), ImVec2(right, bottom + 6.0f * s), IM_COL32(60, 160, 100, 160), 1.5f * s);
		float inputY = bottom + 16.0f * s;
		GameUI::AddText(list, GameUI::Font(true), font, ImVec2(left, inputY), IM_COL32(255, 200, 90, 255), ">");
		float textX = left + 22.0f * s;
		std::string typed = focused ? std::string(m_Input) : std::string("type  help  for commands");
		GameUI::AddText(list, GameUI::Font(true), font, ImVec2(textX, inputY), focused ? green : IM_COL32(70, 140, 100, 255), typed.c_str());
		if (focused && std::fmod(time, 1.0f) < 0.6f)
		{
			std::string before = std::string(m_Input).substr(0, (size_t)std::clamp(m_Cursor, 0, (int)std::strlen(m_Input)));
			float cx = textX + GameUI::Font(true)->CalcTextSizeA(font, FLT_MAX, 0.0f, before.c_str()).x;
			list->AddRectFilled(ImVec2(cx, inputY), ImVec2(cx + 11.0f * s, inputY + font), green);
		}

		// Scanlines and a vignette, so it reads as a screen and not a panel.
		for (float sy = 0.0f; sy < height; sy += 4.0f * s)
			list->AddRectFilled(ImVec2(0, sy), ImVec2(width, sy + 1.2f * s), IM_COL32(0, 0, 0, 46));
		list->AddRect(ImVec2(0, 0), ImVec2(width, height), IM_COL32(0, 0, 0, 120), 0.0f, 0, 26.0f * s);
	}

private:
	struct Queued
	{
		std::string Line;
		bool Echo = true;
	};

	// The text field, shared by the window and the hidden in-world input.
	void InputField()
	{
		ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
			| ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways;
		if (m_FocusInput)
		{
			ImGui::SetKeyboardFocusHere();
			m_FocusInput = false;
		}
		if (ImGui::InputText("##ConsoleInput", m_Input, sizeof(m_Input), inputFlags, &GameConsole::OnInput, this))
		{
			std::string line = m_Input;
			m_Input[0] = '\0';
			m_Cursor = 0;
			if (!Trim(line).empty())
			{
				m_History.push_back(line);
				m_HistoryCursor = -1;
				Submit(line);
			}
			m_FocusInput = true;   // straight back for the next command
		}
		m_InputActive = ImGui::IsItemActive();
	}

	// Takes the keyboard while drawing nothing: the screen in the world
	// shows what is typed. Not by a zero style alpha -- ImGui skips the items
	// of a window begun at alpha 0, the text field with them, which is how
	// the first version of this took no keys at all -- but by every colour
	// the field draws in being transparent.
	void DrawHiddenInput(float x, float y)
	{
		ImGui::SetNextWindowPos(ImVec2(x + 4.0f, y + 4.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(300.0f, 40.0f), ImGuiCond_Always);
		const ImGuiCol clear[] = { ImGuiCol_Text, ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
			ImGuiCol_TextSelectedBg, ImGuiCol_InputTextCursor, ImGuiCol_Border, ImGuiCol_NavCursor };
		for (ImGuiCol colour : clear)
			ImGui::PushStyleColor(colour, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
		if (ImGui::Begin("##GameConsoleHidden", nullptr, flags & ~ImGuiWindowFlags_NoInputs))
		{
			ImGui::SetNextItemWidth(280.0f);
			if (!m_InputActive)
				m_FocusInput = true;   // a click elsewhere took it: the terminal is still the one in use
			InputField();
		}
		ImGui::End();
		ImGui::PopStyleColor((int)(sizeof(clear) / sizeof(clear[0])));
	}

	// A line broken at spaces to fit `width` at `font` pixels.
	static std::vector<std::string> Wrap(const std::string& text, float font, float width)
	{
		std::vector<std::string> out;
		std::string current;
		std::istringstream words(text);
		std::string leading;
		for (char c : text)
		{
			if (c != ' ')
				break;
			leading += ' ';
		}
		std::string word;
		current = leading;
		while (words >> word)
		{
			std::string trial = current.size() > leading.size() ? current + " " + word : current + word;
			if (GameUI::Font(true)->CalcTextSizeA(font, FLT_MAX, 0.0f, trial.c_str()).x > width && current.size() > leading.size())
			{
				out.push_back(current);
				current = leading + "  " + word;
			}
			else
				current = trial;
		}
		out.push_back(current);
		return out;
	}

	struct Command
	{
		std::string Usage, Help;
		Handler Run;
	};
	struct Line
	{
		std::string Text;
		Tone Kind = Tone::Plain;
	};

	// Only for a game that has something to say: a status line in every
	// exported game -- Breakout, say -- would be a hint nobody asked for,
	// and a frame the editor's Play does not draw.
	void DrawStatus(float x, float y, float width, float height)
	{
		if (m_Status.empty())
			return;
		std::string text = m_Status + "      `  terminal";
		ImVec2 size = ImGui::CalcTextSize(text.c_str());
		ImDrawList* draw = ImGui::GetForegroundDrawList();
		ImVec2 at(x + 10.0f, y + height - size.y - 14.0f);
		draw->AddRectFilled(ImVec2(at.x - 6.0f, at.y - 4.0f), ImVec2(at.x + size.x + 6.0f, at.y + size.y + 4.0f),
			IM_COL32(4, 12, 9, 190), 4.0f);
		draw->AddText(at, IM_COL32(150, 255, 175, 255), text.c_str());
	}

	static ImVec4 ColourOf(Tone tone)
	{
		switch (tone)
		{
			case Tone::Echo:  return ImVec4(1.0f, 0.82f, 0.4f, 1.0f);
			case Tone::Good:  return ImVec4(0.55f, 1.0f, 0.65f, 1.0f);
			case Tone::Bad:   return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
			case Tone::Title: return ImVec4(0.9f, 0.97f, 1.0f, 1.0f);
			case Tone::Dim:   return ImVec4(0.42f, 0.62f, 0.5f, 1.0f);
			default:          return ImVec4(0.68f, 0.92f, 0.74f, 1.0f);
		}
	}

	static int OnInput(ImGuiInputTextCallbackData* data)
	{
		GameConsole& self = *(GameConsole*)data->UserData;
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
			return data->EventChar == '`' ? 1 : 0;   // the toggle, not text
		if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			self.m_Cursor = data->CursorPos;
			return 0;
		}

		if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !self.m_History.empty())
		{
			int last = (int)self.m_History.size() - 1;
			if (data->EventKey == ImGuiKey_UpArrow)
				self.m_HistoryCursor = self.m_HistoryCursor < 0 ? last : std::max(0, self.m_HistoryCursor - 1);
			else if (data->EventKey == ImGuiKey_DownArrow && self.m_HistoryCursor >= 0)
				self.m_HistoryCursor = self.m_HistoryCursor >= last ? -1 : self.m_HistoryCursor + 1;
			data->DeleteChars(0, data->BufTextLen);
			if (self.m_HistoryCursor >= 0)
				data->InsertChars(0, self.m_History[self.m_HistoryCursor].c_str());
		}

		// Completes the command name: the one that fits, or the list of
		// those that might.
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			std::string typed(data->Buf, data->BufTextLen);
			if (typed.find(' ') != std::string::npos)
				return 0;
			std::vector<std::string> matches;
			for (auto& [name, command] : self.m_Commands)
				if (name.rfind(Lower(typed), 0) == 0)
					matches.push_back(name);
			if (matches.size() == 1)
			{
				data->DeleteChars(0, data->BufTextLen);
				data->InsertChars(0, (matches[0] + " ").c_str());
			}
			else if (matches.size() > 1)
			{
				std::string list;
				for (const std::string& m : matches)
					list += m + "  ";
				self.Print(list, Tone::Dim);
			}
		}
		return 0;
	}

	static Args Split(const std::string& line)
	{
		Args words;
		std::istringstream in(line);
		std::string word;
		while (in >> word)
			words.push_back(word);
		return words;
	}

	static std::string Trim(const std::string& text)
	{
		size_t from = text.find_first_not_of(" \t");
		if (from == std::string::npos)
			return "";
		return text.substr(from, text.find_last_not_of(" \t") - from + 1);
	}

public:
	static std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return text;
	}

	// A number from an argument, or false (and the reason printed).
	bool Number(const std::string& word, float& out)
	{
		char* end = nullptr;
		out = std::strtof(word.c_str(), &end);
		if (end == word.c_str() || *end != '\0')
		{
			Bad("'" + word + "' is not a number");
			return false;
		}
		return true;
	}

private:
	std::map<std::string, Command> m_Commands;
	std::vector<Line> m_Lines;
	std::vector<Queued> m_Queue;
	std::vector<std::string> m_History;
	int m_HistoryCursor = -1;
	char m_Input[256] = {};
	bool m_Open = false;
	bool m_FocusInput = false;
	bool m_InputActive = false;
	bool m_ScrollToEnd = false;
	bool m_Embedded = false;
	bool m_Quiet = false;
	int m_Cursor = 0;
	std::string m_Status;
};
