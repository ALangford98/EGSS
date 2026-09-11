#pragma once

// Glues TextBuffer + CellGrid into the editor's "Editor" panel.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "EditorTheme.h"
#include "ScriptEngine.h"
#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// A single-line tokenizer: keywords, "..."/'...' strings and // comments
// get their own color, everything else stays theme.Foreground. Line-local
// only -- a construct that spans lines (/* */, a template literal) isn't
// tracked across calls, so it just renders as plain text instead of being
// colored wrong. Good enough for the small, single-purpose scripts this
// editor actually opens (see TestEnv/assets/demos/*/*.gss); a real
// multi-line lexer is more machinery than that use case has ever needed.
inline std::vector<ImU32> TokenizeLineColors(const std::string& line, const EditorTheme& theme)
{
	static const char* keywords[] = {
		"const", "let", "var", "function", "return", "if", "else", "for", "while", "do",
		"class", "interface", "extends", "implements", "new", "this", "typeof", "true",
		"false", "null", "undefined", "import", "export", "from", "as", "of", "in",
		"break", "continue", "switch", "case", "default", "try", "catch", "finally",
		"throw", "void", "number", "string", "boolean", "any", "static", "public",
		"private", "readonly", "instanceof"
	};

	std::vector<ImU32> colors(line.size(), theme.Foreground);
	size_t i = 0;
	while (i < line.size())
	{
		if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')
		{
			for (size_t j = i; j < line.size(); j++)
				colors[j] = theme.Comment;
			break;
		}

		if (line[i] == '"' || line[i] == '\'')
		{
			char quote = line[i];
			colors[i] = theme.StringLiteral;
			i++;
			while (i < line.size() && line[i] != quote)
			{
				colors[i] = theme.StringLiteral;
				// Skip one escaped character so a `\"` inside the literal
				// doesn't end it early.
				if (line[i] == '\\' && i + 1 < line.size())
				{
					i++;
					colors[i] = theme.StringLiteral;
				}
				i++;
			}
			if (i < line.size())
			{
				colors[i] = theme.StringLiteral;
				i++;
			}
			continue;
		}

		if (std::isalpha((unsigned char)line[i]) || line[i] == '_')
		{
			size_t start = i;
			while (i < line.size() && (std::isalnum((unsigned char)line[i]) || line[i] == '_'))
				i++;
			std::string word = line.substr(start, i - start);
			for (const char* keyword : keywords)
			{
				if (word == keyword)
				{
					for (size_t j = start; j < i; j++)
						colors[j] = theme.Keyword;
					break;
				}
			}
			continue;
		}

		i++;
	}
	return colors;
}

inline bool IsInSelection(int row, int col, int startRow, int startCol, int endRow, int endCol)
{
	if (row < startRow || row > endRow)
		return false;
	if (startRow == endRow)
		return col >= startCol && col < endCol;
	if (row == startRow)
		return col >= startCol;
	if (row == endRow)
		return col < endCol;
	return true;
}

// Copies the buffer's visible window into the CellGrid: a right-aligned
// line-number gutter (current line highlighted), then the line text. A
// free function (not a TextEditorPanel method) so a self-test can
// exercise this glue directly, without a live ImGui frame -- same pattern
// TerminalPanel's CopyVTermScreenToGrid already established.
inline void RenderBufferToGrid(const TextBuffer& buffer, CellGrid& grid, const EditorTheme& theme,
	int scrollRow, int gutterDigits, int gutterCols, int textCols, int rows)
{
	bool hasSelection = buffer.HasSelection();
	int selStartRow = 0, selStartCol = 0, selEndRow = 0, selEndCol = 0;
	if (hasSelection)
		buffer.GetSelectionRange(selStartRow, selStartCol, selEndRow, selEndCol);

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
		std::vector<ImU32> lineColors = line ? TokenizeLineColors(*line, theme) : std::vector<ImU32>();

		for (int col = 0; col < textCols; col++)
		{
			Cell& cell = grid.At(gutterCols + col, screenRow);
			bool selected = hasSelection && IsInSelection(bufferRow, col, selStartRow, selStartCol, selEndRow, selEndCol);
			cell.Bg = selected ? theme.SelectionBg : theme.Background;
			cell.Fg = (line && col < (int)line->size()) ? lineColors[(size_t)col] : theme.Foreground;
			cell.Codepoint = (line && col < (int)line->size()) ? (char32_t)(unsigned char)(*line)[col] : U' ';
		}
	}

	int cursorScreenRow = buffer.CursorRow() - scrollRow;
	// Clamped, not scrolled -- there is no horizontal scroll offset yet
	// (a real feature, deferred; see docs/STATE.md). This just keeps the
	// cursor visible at the right edge instead of vanishing past it.
	int cursorScreenCol = std::min(buffer.CursorCol(), textCols - 1);
	grid.SetCursor(gutterCols + std::max(0, cursorScreenCol), cursorScreenRow,
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

	void SetScriptEngine(ScriptEngine* engine) { m_ScriptEngine = engine; }

	const std::string& LastRunOutput() const { return m_LastRunOutput; }
	const std::string& LastRunError() const { return m_LastRunError; }

	bool OpenFile(const std::string& path)
	{
		// `path` is a distinct object from m_PathBuffer (never itself a view
		// into it), so this strncpy is never a self-copy -- relevant because
		// the Open button passes m_PathBuffer by value into this by-reference
		// parameter, constructing a temporary std::string first.
		strncpy(m_PathBuffer, path.c_str(), sizeof(m_PathBuffer) - 1);
		m_PathBuffer[sizeof(m_PathBuffer) - 1] = '\0';
		bool ok = m_Buffer.LoadFromFile(path);
		m_StatusMessage = ok ? "Opened." : "Could not open file.";
		return ok;
	}

private:
	void DrawFileBar()
	{
		ImGui::PushItemWidth(300.0f);
		ImGui::InputText("##editorpath", m_PathBuffer, sizeof(m_PathBuffer));
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGui::Button("Open"))
			OpenFile(m_PathBuffer);
		ImGui::SameLine();
		if (ImGui::Button("Save"))
			m_StatusMessage = m_Buffer.SaveToFile(m_PathBuffer) ? "Saved." : "Could not save file.";
		ImGui::SameLine();
		if (ImGui::Button("Run"))
		{
			if (m_ScriptEngine)
			{
				ScriptEngine::ScriptResult result = m_ScriptEngine->RunScript(m_Buffer.FullText());
				m_LastRunOutput = result.Output;
				m_LastRunError = result.Ok ? "" : result.Error;
			}
			else
			{
				m_LastRunError = "No script engine.";
			}
		}
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

		bool shift = io.KeyShift;
		bool ctrl = io.KeyCtrl;

		// Clipboard/undo shortcuts first: Ctrl held changes what a letter
		// key does entirely, so these must not also fall through to
		// InputQueueCharacters below (Ctrl+C etc. don't produce printable
		// characters anyway, but this keeps the two blocks unambiguous).
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, true))
		{
			if (shift) m_Buffer.Redo();
			else m_Buffer.Undo();
		}
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, true)) m_Buffer.Redo();
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, true)) m_Buffer.SelectAll();
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, true) && m_Buffer.HasSelection())
			ImGui::SetClipboardText(m_Buffer.GetSelectedText().c_str());
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, true) && m_Buffer.HasSelection())
		{
			ImGui::SetClipboardText(m_Buffer.GetSelectedText().c_str());
			m_Buffer.DeleteSelection();
		}
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, true))
		{
			const char* clipboard = ImGui::GetClipboardText();
			if (clipboard)
				m_Buffer.InsertText(clipboard);
		}

		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			ImWchar c = io.InputQueueCharacters[i];
			if (c >= 32 && c < 127) // printable ASCII only, matching CellGrid's own cut
				m_Buffer.InsertCharWithPairing((char)c);
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Enter, true)) m_Buffer.InsertNewline();
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) m_Buffer.Backspace();
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) m_Buffer.Delete();
		if (ImGui::IsKeyPressed(ImGuiKey_Tab, true)) m_Buffer.InsertTab();
		if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) m_Buffer.MoveLeft(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) m_Buffer.MoveRight(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) m_Buffer.MoveUp(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) m_Buffer.MoveDown(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) m_Buffer.MoveHome(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_End, true)) m_Buffer.MoveEnd(shift);
		if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) m_Buffer.MovePageUp(visibleRows, shift);
		if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) m_Buffer.MovePageDown(visibleRows, shift);
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
	ScriptEngine* m_ScriptEngine = nullptr; // not owned -- set by EditorShell
	std::string m_LastRunOutput;
	std::string m_LastRunError;
};
