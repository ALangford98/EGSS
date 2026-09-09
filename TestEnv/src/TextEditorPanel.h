#pragma once

// Glues TextBuffer + CellGrid into the editor's "Editor" panel.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "EditorTheme.h"
#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>
#include <string>

// Copies the buffer's visible window into the CellGrid: a right-aligned
// line-number gutter (current line highlighted), then the line text. A
// free function (not a TextEditorPanel method) so a self-test can
// exercise this glue directly, without a live ImGui frame -- same pattern
// TerminalPanel's CopyVTermScreenToGrid already established.
inline void RenderBufferToGrid(const TextBuffer& buffer, CellGrid& grid, const EditorTheme& theme,
	int scrollRow, int gutterDigits, int gutterCols, int textCols, int rows)
{
	for (int screenRow = 0; screenRow < rows; screenRow++)
	{
		int bufferRow = scrollRow + screenRow;
		bool isCurrentLine = (bufferRow == buffer.CursorRow());

		for (int col = 0; col < gutterCols; col++)
		{
			Cell& cell = grid.At(col, screenRow);
			cell.Bg = theme.Background;
			cell.Fg = isCurrentLine ? theme.CurrentLineNumberFg : theme.LineNumberFg;
			cell.Codepoint = U' ';
		}

		bool hasLine = bufferRow < buffer.LineCount();
		if (hasLine)
		{
			char numberText[16];
			snprintf(numberText, sizeof(numberText), "%*d", gutterDigits, bufferRow + 1);
			for (int i = 0; i < gutterDigits; i++)
				grid.At(i, screenRow).Codepoint = (char32_t)(unsigned char)numberText[i];
		}

		const std::string* line = hasLine ? &buffer.Line(bufferRow) : nullptr;
		for (int col = 0; col < textCols; col++)
		{
			Cell& cell = grid.At(gutterCols + col, screenRow);
			cell.Bg = theme.Background;
			cell.Fg = theme.Foreground;
			cell.Codepoint = (line && col < (int)line->size()) ? (char32_t)(unsigned char)(*line)[col] : U' ';
		}
	}

	int cursorScreenRow = buffer.CursorRow() - scrollRow;
	grid.SetCursor(gutterCols + buffer.CursorCol(), cursorScreenRow,
		cursorScreenRow >= 0 && cursorScreenRow < rows);
}

class TextEditorPanel
{
public:
	void OnImGuiRender()
	{
		bool visible = ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar);

		if (ImGui::IsWindowFocused())
			ImGui::GetIO().WantCaptureKeyboard = true;

		if (!visible)
		{
			ImGui::End();
			return;
		}

		DrawFileBar();

		ImVec2 avail = ImGui::GetContentRegionAvail();
		float cellWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "M").x;
		float cellHeight = ImGui::GetTextLineHeight();

		int gutterDigits = std::max(2, (int)std::to_string(m_Buffer.LineCount()).size());
		int gutterCols = gutterDigits + 1; // one column of spacing after the number

		int totalCols = std::max(gutterCols + 8, (int)(avail.x / cellWidth));
		int textCols = totalCols - gutterCols;
		int rows = std::max(2, (int)(avail.y / cellHeight));

		HandleInput(rows);
		ScrollToCursor(rows);

		m_Grid.Resize(totalCols, rows);
		RenderBufferToGrid(m_Buffer, m_Grid, m_Theme, m_ScrollRow, gutterDigits, gutterCols, textCols, rows);
		m_Grid.Render();

		ImGui::End();
	}

private:
	void DrawFileBar()
	{
		ImGui::PushItemWidth(300.0f);
		ImGui::InputText("##editorpath", m_PathBuffer, sizeof(m_PathBuffer));
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGui::Button("Open"))
			m_StatusMessage = m_Buffer.LoadFromFile(m_PathBuffer) ? "Opened." : "Could not open file.";
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			m_StatusMessage = m_Buffer.SaveToFile(m_PathBuffer) ? "Saved." : "Could not save file.";
		if (!m_StatusMessage.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_StatusMessage.c_str());
		}
	}

	// Skips buffer-editing keys entirely while the path field (or any other
	// text widget) is the one capturing keyboard text this frame -- both it
	// and this function would otherwise read the same io.InputQueueCharacters
	// this frame, double-handling every typed character.
	void HandleInput(int visibleRows)
	{
		// "Editor" and "Terminal" are different dock nodes and can both be
		// visible/rendering at once (unlike "Scene"/"Editor", which share a
		// tab group and are mutually exclusive) -- without this, typing into
		// a focused Terminal (which has no InputText widget, so
		// io.WantTextInput never goes true) would silently also land in the
		// visible-but-unfocused editor buffer. Same pattern
		// TerminalPanel::HandleInput() already uses for the same reason.
		if (!ImGui::IsWindowFocused())
			return;

		ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput)
			return;

		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			ImWchar c = io.InputQueueCharacters[i];
			if (c >= 32 && c < 127) // printable ASCII only, matching CellGrid's own cut
				m_Buffer.InsertChar((char)c);
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Enter, true)) m_Buffer.InsertNewline();
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) m_Buffer.Backspace();
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) m_Buffer.Delete();
		if (ImGui::IsKeyPressed(ImGuiKey_Tab, true)) m_Buffer.InsertTab();
		if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) m_Buffer.MoveLeft();
		if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) m_Buffer.MoveRight();
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) m_Buffer.MoveUp();
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) m_Buffer.MoveDown();
		if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) m_Buffer.MoveHome();
		if (ImGui::IsKeyPressed(ImGuiKey_End, true)) m_Buffer.MoveEnd();
		if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) m_Buffer.MovePageUp(visibleRows);
		if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) m_Buffer.MovePageDown(visibleRows);
	}

	void ScrollToCursor(int visibleRows)
	{
		if (m_Buffer.CursorRow() < m_ScrollRow)
			m_ScrollRow = m_Buffer.CursorRow();
		else if (m_Buffer.CursorRow() >= m_ScrollRow + visibleRows)
			m_ScrollRow = m_Buffer.CursorRow() - visibleRows + 1;
		if (m_ScrollRow < 0)
			m_ScrollRow = 0;
	}

	TextBuffer m_Buffer;
	CellGrid m_Grid;
	EditorTheme m_Theme = EverforestDark();
	char m_PathBuffer[512] = "";
	std::string m_StatusMessage;
	int m_ScrollRow = 0;
};
