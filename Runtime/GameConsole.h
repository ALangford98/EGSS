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

#include <algorithm>
#include <cctype>
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

	// A line as if typed; run at the next fixed step.
	void Submit(const std::string& line) { m_Queue.push_back(line); }

	// Runs whatever was typed since the last fixed step.
	void RunQueued()
	{
		std::vector<std::string> queue;
		queue.swap(m_Queue);
		for (const std::string& line : queue)
			Execute(line);
	}

	void Execute(const std::string& line)
	{
		Args words = Split(line);
		if (words.empty())
			return;
		Print("> " + line, Tone::Echo);
		std::string name = Lower(words[0]);
		words.erase(words.begin());
		auto found = m_Commands.find(name);
		if (found == m_Commands.end())
		{
			Bad("unknown command '" + name + "' -- type help");
			return;
		}
		found->second.Run(words);
	}

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

		if (!m_Open)
		{
			DrawStatus(x, y, width, height);
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
			if (m_FocusInput)
			{
				ImGui::SetKeyboardFocusHere();
				m_FocusInput = false;
			}
			ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
				| ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter;
			if (ImGui::InputText("##ConsoleInput", m_Input, sizeof(m_Input), inputFlags, &GameConsole::OnInput, this))
			{
				std::string line = m_Input;
				m_Input[0] = '\0';
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
		ImGui::End();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);
	}

private:
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
	std::vector<std::string> m_Queue;
	std::vector<std::string> m_History;
	int m_HistoryCursor = -1;
	char m_Input[256] = {};
	bool m_Open = false;
	bool m_FocusInput = false;
	bool m_InputActive = false;
	bool m_ScrollToEnd = false;
	std::string m_Status;
};
