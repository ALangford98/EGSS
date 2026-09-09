#pragma once

// A generic grid of styled character cells, plus an ImGui renderer for it.
// Deliberately knows nothing about PTYs, shells, or libvterm -- the
// embedded-Neovim sub-project that follows this one fills the same `Cell`
// grid from its own msgpack-RPC UI events instead of from a terminal.
//
// v1 renders ASCII only (codepoints >= 128 are left blank, not garbage --
// ImGui's default compiled-in font only covers ASCII, which is also what
// keeps a fixed-pitch character grid simple to compute: every glyph this
// pass draws is exactly one cell wide).

#include <GS.h>
#include <imgui.h>
#include <vector>

struct Cell
{
	char32_t Codepoint = U' ';
	ImU32 Fg = IM_COL32(220, 220, 220, 255);
	ImU32 Bg = IM_COL32(0, 0, 0, 0); // alpha 0 == "paint no background"
	bool Bold = false;
};

class CellGrid
{
public:
	void Resize(int cols, int rows)
	{
		if (cols == m_Cols && rows == m_Rows)
			return;
		m_Cols = cols;
		m_Rows = rows;
		m_Cells.assign((size_t)cols * (size_t)rows, Cell{});
	}

	int Cols() const { return m_Cols; }
	int Rows() const { return m_Rows; }

	Cell& At(int col, int row)
	{
		GS_ASSERT(col >= 0 && col < m_Cols && row >= 0 && row < m_Rows,
			"CellGrid::At out of range");
		return m_Cells[(size_t)row * (size_t)m_Cols + (size_t)col];
	}

	void SetCursor(int col, int row, bool visible)
	{
		m_CursorCol = col;
		m_CursorRow = row;
		m_CursorVisible = visible;
	}

	// Draws at the current ImGui cursor position, using the current font's
	// own advance width as the cell pitch -- a fixed grid, not ImGui's
	// word-wrapped text flow.
	void Render()
	{
		if (m_Cols == 0 || m_Rows == 0)
			return;

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 origin = ImGui::GetCursorScreenPos();
		ImFont* font = ImGui::GetFont();
		float fontSize = ImGui::GetFontSize();
		float cellWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();

		for (int row = 0; row < m_Rows; row++)
		{
			for (int col = 0; col < m_Cols; col++)
			{
				const Cell& cell = m_Cells[(size_t)row * (size_t)m_Cols + (size_t)col];
				ImVec2 cellMin(origin.x + col * cellWidth, origin.y + row * cellHeight);
				ImVec2 cellMax(cellMin.x + cellWidth, cellMin.y + cellHeight);

				if ((cell.Bg & IM_COL32_A_MASK) != 0)
					drawList->AddRectFilled(cellMin, cellMax, cell.Bg);

				if (cell.Codepoint > U' ' && cell.Codepoint < 128)
				{
					char ch = (char)cell.Codepoint;
					ImU32 fg = cell.Bold ? Brighten(cell.Fg) : cell.Fg;
					drawList->AddText(font, fontSize, cellMin, fg, &ch, &ch + 1);
				}
			}
		}

		if (m_CursorVisible && m_CursorCol >= 0 && m_CursorCol < m_Cols &&
			m_CursorRow >= 0 && m_CursorRow < m_Rows)
		{
			ImVec2 cursorMin(origin.x + m_CursorCol * cellWidth, origin.y + m_CursorRow * cellHeight);
			ImVec2 cursorMax(cursorMin.x + cellWidth, cursorMin.y + cellHeight);
			drawList->AddRectFilled(cursorMin, cursorMax, IM_COL32(200, 200, 200, 120));
		}

		// Reserve the layout space ImGui itself doesn't know about, since
		// everything above was drawn directly with the draw list.
		ImGui::Dummy(ImVec2(m_Cols * cellWidth, m_Rows * cellHeight));
	}

private:
	static ImU32 Brighten(ImU32 color)
	{
		auto lift = [](ImU32 channel) { return channel + (255 - channel) * 2 / 5; };
		ImU32 r = lift((color >> IM_COL32_R_SHIFT) & 0xFF);
		ImU32 g = lift((color >> IM_COL32_G_SHIFT) & 0xFF);
		ImU32 b = lift((color >> IM_COL32_B_SHIFT) & 0xFF);
		ImU32 a = (color >> IM_COL32_A_SHIFT) & 0xFF;
		return IM_COL32(r, g, b, a);
	}

	std::vector<Cell> m_Cells;
	int m_Cols = 0;
	int m_Rows = 0;
	int m_CursorCol = -1;
	int m_CursorRow = -1;
	bool m_CursorVisible = false;
};
