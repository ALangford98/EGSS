#pragma once

// Glues Pty + libvterm + CellGrid into the editor's "Terminal" panel.
//
// Once per frame: read whatever the shell produced (non-blocking) and feed
// it to libvterm's parser; copy libvterm's *entire* current screen state
// into the CellGrid (a terminal is ~80x24 = ~2000 cells -- cheap enough to
// copy in full every frame, which avoids wiring up libvterm's damage-region
// callbacks for no real benefit at this size); render; forward keyboard
// input and panel-resize the other way.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "Pty.h"

#include <vterm.h>

#include <algorithm>
#include <cstdint>

// Copies libvterm's current screen state into a CellGrid. A free function
// (not a TerminalPanel method) so a self-test can exercise this glue by
// feeding vterm_input_write() directly, without spawning a real shell.
inline void CopyVTermScreenToGrid(VTerm* vt, VTermScreen* screen, CellGrid& grid, int cols, int rows)
{
	for (int row = 0; row < rows; row++)
	{
		for (int col = 0; col < cols; col++)
		{
			VTermScreenCell cell{};
			VTermPos pos{ row, col };
			if (!vterm_screen_get_cell(screen, pos, &cell))
			{
				grid.At(col, row) = Cell{};
				continue;
			}

			// Resolves indexed/default colors to concrete RGB via the
			// screen's own palette -- after this, .fg.rgb/.bg.rgb are
			// always valid regardless of what kind of color it started as.
			vterm_screen_convert_color_to_rgb(screen, &cell.fg);
			vterm_screen_convert_color_to_rgb(screen, &cell.bg);

			Cell& out = grid.At(col, row);
			out.Codepoint = (cell.chars[0] == 0 || cell.chars[0] == (uint32_t)-1) ? U' ' : (char32_t)cell.chars[0];
			out.Fg = IM_COL32(cell.fg.rgb.red, cell.fg.rgb.green, cell.fg.rgb.blue, 255);
			out.Bg = IM_COL32(cell.bg.rgb.red, cell.bg.rgb.green, cell.bg.rgb.blue, 255);
			out.Bold = cell.attrs.bold != 0;
		}
	}

	VTermState* state = vterm_obtain_state(vt);
	VTermPos cursor;
	vterm_state_get_cursorpos(state, &cursor);
	grid.SetCursor(cursor.col, cursor.row, true);
}

class TerminalPanel
{
public:
	~TerminalPanel()
	{
		if (m_Vt)
			vterm_free(m_Vt); // frees the VTermScreen/VTermState it owns too
	}

	void OnImGuiRender()
	{
		bool visible = ImGui::Begin("Terminal");

		if (ImGui::IsWindowFocused())
			ImGui::GetIO().WantCaptureKeyboard = true;

		ImVec2 avail = ImGui::GetContentRegionAvail();
		float cellWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();
		int cols = std::max(8, (int)(avail.x / cellWidth));
		int rows = std::max(2, (int)(avail.y / cellHeight));

		if (!m_Vt)
		{
			if (!visible)
			{
				ImGui::End();
				return; // nobody has opened this tab yet -- don't fork a shell
			}

			if (!m_SpawnFailed)
			{
				Spawn(cols, rows);
				m_SpawnFailed = (m_Vt == nullptr);
			}
			if (!m_Vt)
			{
				ImGui::TextDisabled("Terminal unavailable: could not start a shell.");
				if (ImGui::Button("Retry"))
					m_SpawnFailed = false;
				ImGui::End();
				return;
			}
		}
		else if (!m_Pty.IsAlive())
		{
			ImGui::TextDisabled("Shell exited. Press any key to restart.");
			ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsWindowFocused() &&
				(io.InputQueueCharacters.Size > 0 || ImGui::IsKeyPressed(ImGuiKey_Enter)))
			{
				vterm_free(m_Vt);
				m_Vt = nullptr;
				m_Screen = nullptr;
				m_SpawnFailed = false;
			}
			ImGui::End();
			return;
		}
		else if (visible && (cols != m_Cols || rows != m_Rows))
		{
			Resize(cols, rows);
		}

		Pump(); // always drain, even hidden, so the shell never blocks on a full pty buffer

		if (visible)
		{
			HandleInput();
			m_Grid.Render();
		}

		ImGui::End();
	}

private:
	void Spawn(int cols, int rows)
	{
		if (!m_Pty.Open(cols, rows))
			return;

		m_Vt = vterm_new(rows, cols);
		vterm_set_utf8(m_Vt, 1);
		m_Screen = vterm_obtain_screen(m_Vt);
		vterm_screen_reset(m_Screen, 1);
		vterm_output_set_callback(m_Vt, &TerminalPanel::OnVtermOutput, this);

		m_Cols = cols;
		m_Rows = rows;
		m_Grid.Resize(cols, rows);
	}

	void Resize(int cols, int rows)
	{
		m_Cols = cols;
		m_Rows = rows;
		vterm_set_size(m_Vt, rows, cols);
		m_Pty.Resize(cols, rows);
		m_Grid.Resize(cols, rows);
	}

	// libvterm invokes this synchronously, from inside vterm_keyboard_key()/
	// vterm_keyboard_unichar() (see HandleInput()), with the exact bytes a
	// real terminal would send a program for that key. We just forward them.
	static void OnVtermOutput(const char* s, size_t len, void* user)
	{
		static_cast<TerminalPanel*>(user)->m_Pty.Write(s, (int)len);
	}

	void Pump()
	{
		char buf[4096];
		int budget = 256 * 1024; // ~64 reads; enough to clear a frame's burst without stalling the frame loop on a runaway producer
		while (budget > 0)
		{
			int n = m_Pty.Read(buf, sizeof(buf));
			if (n <= 0) // 0 = nothing left this frame, -1 = child gone
				break;
			vterm_input_write(m_Vt, buf, (size_t)n);
			budget -= n;
		}

		CopyVTermScreenToGrid(m_Vt, m_Screen, m_Grid, m_Cols, m_Rows);
	}

	void HandleInput()
	{
		if (!ImGui::IsWindowFocused())
			return;

		ImGuiIO& io = ImGui::GetIO();

		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			ImWchar c = io.InputQueueCharacters[i];
			if (c > 0 && c < 0x10000)
				vterm_keyboard_unichar(m_Vt, (uint32_t)c, VTERM_MOD_NONE);
		}

		struct KeyMap { ImGuiKey Key; VTermKey VKey; };
		static const KeyMap keys[] = {
			{ ImGuiKey_Enter, VTERM_KEY_ENTER },
			{ ImGuiKey_KeypadEnter, VTERM_KEY_ENTER },
			{ ImGuiKey_Backspace, VTERM_KEY_BACKSPACE },
			{ ImGuiKey_Tab, VTERM_KEY_TAB },
			{ ImGuiKey_Escape, VTERM_KEY_ESCAPE },
			{ ImGuiKey_UpArrow, VTERM_KEY_UP },
			{ ImGuiKey_DownArrow, VTERM_KEY_DOWN },
			{ ImGuiKey_LeftArrow, VTERM_KEY_LEFT },
			{ ImGuiKey_RightArrow, VTERM_KEY_RIGHT },
			{ ImGuiKey_Insert, VTERM_KEY_INS },
			{ ImGuiKey_Delete, VTERM_KEY_DEL },
			{ ImGuiKey_Home, VTERM_KEY_HOME },
			{ ImGuiKey_End, VTERM_KEY_END },
			{ ImGuiKey_PageUp, VTERM_KEY_PAGEUP },
			{ ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN },
		};
		for (const KeyMap& k : keys)
			if (ImGui::IsKeyPressed(k.Key, true))
				vterm_keyboard_key(m_Vt, k.VKey, VTERM_MOD_NONE);

		if (io.KeyCtrl)
			for (int letter = 0; letter < 26; letter++)
				if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + letter), false))
					vterm_keyboard_unichar(m_Vt, (uint32_t)('a' + letter), VTERM_MOD_CTRL);
	}

	Pty m_Pty;
	VTerm* m_Vt = nullptr;
	VTermScreen* m_Screen = nullptr;
	CellGrid m_Grid;
	int m_Cols = 0;
	int m_Rows = 0;
	bool m_SpawnFailed = false;
};
