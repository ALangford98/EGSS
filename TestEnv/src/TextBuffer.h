#pragma once

// Pure text-buffer data and edit operations for the embedded text editor.
// No ImGui, no rendering -- TextEditorPanel (Task 3) copies this into a
// CellGrid. ASCII-only, byte-indexed columns: inherits CellGrid's existing
// ASCII rendering limit (a loaded file's non-ASCII bytes will show as
// blank cells, same disclosed cut the embedded terminal already made).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class TextBuffer
{
public:
	TextBuffer() : m_Lines(1) {}

	int LineCount() const { return (int)m_Lines.size(); }
	const std::string& Line(int row) const { return m_Lines[(size_t)row]; }

	std::string FullText() const
	{
		std::string result;
		for (const std::string& line : m_Lines)
			result += line + '\n';
		return result;
	}

	int CursorRow() const { return m_CursorRow; }
	int CursorCol() const { return m_CursorCol; }

	// --- Selection ------------------------------------------------------
	// Shift+move (keyboard) and MoveTo with extend=true (a mouse click or
	// an in-progress drag, see TextEditorPanel::HandleInput) both extend
	// it through the same BeginOrExtendSelection path below -- one
	// mechanism, two ways of driving it.
	bool HasSelection() const { return m_SelectionAnchorRow >= 0; }
	void ClearSelection() { m_SelectionAnchorRow = -1; m_SelectionAnchorCol = -1; }

	void SelectAll()
	{
		m_SelectionAnchorRow = 0;
		m_SelectionAnchorCol = 0;
		m_CursorRow = LineCount() - 1;
		m_CursorCol = (int)m_Lines.back().size();
		m_DesiredCol = m_CursorCol;
	}

	// Normalized so `start` is always the earlier point in the document,
	// regardless of which direction the selection was dragged.
	void GetSelectionRange(int& startRow, int& startCol, int& endRow, int& endCol) const
	{
		if (m_SelectionAnchorRow < m_CursorRow
			|| (m_SelectionAnchorRow == m_CursorRow && m_SelectionAnchorCol < m_CursorCol))
		{
			startRow = m_SelectionAnchorRow; startCol = m_SelectionAnchorCol;
			endRow = m_CursorRow; endCol = m_CursorCol;
		}
		else
		{
			startRow = m_CursorRow; startCol = m_CursorCol;
			endRow = m_SelectionAnchorRow; endCol = m_SelectionAnchorCol;
		}
	}

	std::string GetSelectedText() const
	{
		if (!HasSelection())
			return "";

		int startRow, startCol, endRow, endCol;
		GetSelectionRange(startRow, startCol, endRow, endCol);

		if (startRow == endRow)
			return m_Lines[(size_t)startRow].substr((size_t)startCol, (size_t)(endCol - startCol));

		std::string result = m_Lines[(size_t)startRow].substr((size_t)startCol) + "\n";
		for (int row = startRow + 1; row < endRow; row++)
			result += m_Lines[(size_t)row] + "\n";
		result += m_Lines[(size_t)endRow].substr(0, (size_t)endCol);
		return result;
	}

	void DeleteSelection()
	{
		if (!HasSelection())
			return;

		int startRow, startCol, endRow, endCol;
		GetSelectionRange(startRow, startCol, endRow, endCol);

		PushUndoIfNeeded(EditKind::Other);

		if (startRow == endRow)
		{
			m_Lines[(size_t)startRow].erase((size_t)startCol, (size_t)(endCol - startCol));
		}
		else
		{
			std::string tail = m_Lines[(size_t)endRow].substr((size_t)endCol);
			m_Lines[(size_t)startRow].erase((size_t)startCol);
			m_Lines[(size_t)startRow] += tail;
			m_Lines.erase(m_Lines.begin() + startRow + 1, m_Lines.begin() + endRow + 1);
		}

		m_CursorRow = startRow;
		m_CursorCol = startCol;
		m_DesiredCol = startCol;
		ClearSelection();
	}

	void InsertChar(char c)
	{
		if (HasSelection())
			DeleteSelection();
		PushUndoIfNeeded(EditKind::Insert);
		m_Lines[(size_t)m_CursorRow].insert((size_t)m_CursorCol, 1, c);
		m_CursorCol++;
		m_DesiredCol = m_CursorCol;
	}

	// Auto-closing pairs: typing an opener inserts its closer too, cursor
	// left between them; typing a closer that's already sitting right
	// where the cursor is steps over it instead of inserting a second one;
	// typing an opener while text is selected wraps the selection instead
	// of replacing it. Only TextEditorPanel's own typed-character path
	// calls this -- InsertTab's spaces, and pasted text via InsertText,
	// go through plain InsertChar/InsertText so balanced pasted code isn't
	// paired a second time on top of itself.
	//
	// No lexical awareness (a `(` typed inside a string or comment pairs
	// just the same as anywhere else) -- the same simple-mechanical scope
	// cut RenderBufferToGrid's own single-line tokenizer already makes for
	// syntax highlighting, not a new one.
	void InsertCharWithPairing(char c)
	{
		if (HasSelection() && IsOpener(c))
		{
			char closing = ClosingFor(c);
			int startRow, startCol, endRow, endCol;
			GetSelectionRange(startRow, startCol, endRow, endCol);

			PushUndoIfNeeded(EditKind::Other);
			m_LastEditKind = EditKind::None;

			// The closer goes in first -- it's at the later position, so
			// the opener's insert (an earlier position on an earlier or
			// the same line) never needs to account for an offset the
			// closer's own insert would otherwise have introduced.
			m_Lines[(size_t)endRow].insert((size_t)endCol, 1, closing);
			m_Lines[(size_t)startRow].insert((size_t)startCol, 1, c);

			// Cursor lands right after the newly-wrapped text rather than
			// preserving the selection -- simpler, and typing usually
			// continues right there anyway.
			m_CursorRow = endRow;
			m_CursorCol = endCol + (startRow == endRow ? 2 : 1);
			m_DesiredCol = m_CursorCol;
			ClearSelection();
			return;
		}

		if (!HasSelection() && IsCloser(c))
		{
			const std::string& line = m_Lines[(size_t)m_CursorRow];
			if (m_CursorCol < (int)line.size() && line[(size_t)m_CursorCol] == c)
			{
				m_CursorCol++;
				m_DesiredCol = m_CursorCol;
				return;
			}
		}

		InsertChar(c);

		char closing = ClosingFor(c);
		if (closing != 0)
			m_Lines[(size_t)m_CursorRow].insert((size_t)m_CursorCol, 1, closing);
	}

	// Splits the current line at the cursor. The new line inherits the
	// current line's leading whitespace, so pressing Enter mid-block keeps
	// the same indent -- the "line indenting" this editor exists to have.
	void InsertNewline()
	{
		if (HasSelection())
			DeleteSelection();
		PushUndoIfNeeded(EditKind::Other);

		std::string& current = m_Lines[(size_t)m_CursorRow];
		std::string indent = LeadingWhitespace(current);
		std::string rest = current.substr((size_t)m_CursorCol);
		current.erase((size_t)m_CursorCol);

		m_Lines.insert(m_Lines.begin() + m_CursorRow + 1, indent + rest);
		m_CursorRow++;
		m_CursorCol = (int)indent.size();
		m_DesiredCol = m_CursorCol;
	}

	// Pasted text is spliced in verbatim, deliberately not routed through
	// InsertNewline -- its leading-whitespace inheritance is right for
	// pressing Enter mid-block, but would add unwanted extra indent on top
	// of whatever indentation the pasted text already carries.
	void InsertText(const std::string& text)
	{
		if (text.empty())
			return;
		if (HasSelection())
			DeleteSelection();
		PushUndoIfNeeded(EditKind::Other);

		std::vector<std::string> pieces;
		size_t start = 0;
		for (size_t i = 0; i <= text.size(); i++)
		{
			if (i == text.size() || text[i] == '\n')
			{
				pieces.push_back(text.substr(start, i - start));
				start = i + 1;
			}
		}

		std::string& current = m_Lines[(size_t)m_CursorRow];
		std::string tail = current.substr((size_t)m_CursorCol);
		current.erase((size_t)m_CursorCol);
		current += pieces[0];

		int insertRow = m_CursorRow;
		for (size_t p = 1; p < pieces.size(); p++)
		{
			insertRow++;
			m_Lines.insert(m_Lines.begin() + insertRow, pieces[p]);
		}
		m_Lines[(size_t)insertRow] += tail;

		m_CursorRow = insertRow;
		m_CursorCol = (int)pieces.back().size();
		m_DesiredCol = m_CursorCol;
	}

	void Backspace()
	{
		if (HasSelection())
		{
			DeleteSelection();
			return;
		}

		// Backspacing in the middle of an empty auto-inserted pair removes
		// both characters together -- otherwise deleting what auto-pairing
		// just inserted leaves a dangling, unbalanced closer behind.
		if (m_CursorCol > 0 && m_CursorCol < (int)m_Lines[(size_t)m_CursorRow].size())
		{
			char before = m_Lines[(size_t)m_CursorRow][(size_t)m_CursorCol - 1];
			char after = m_Lines[(size_t)m_CursorRow][(size_t)m_CursorCol];
			if (ClosingFor(before) == after)
			{
				PushUndoIfNeeded(EditKind::Backspace);
				m_Lines[(size_t)m_CursorRow].erase((size_t)m_CursorCol - 1, 2);
				m_CursorCol--;
				m_DesiredCol = m_CursorCol;
				return;
			}
		}

		PushUndoIfNeeded(EditKind::Backspace);

		if (m_CursorCol > 0)
		{
			m_Lines[(size_t)m_CursorRow].erase((size_t)m_CursorCol - 1, 1);
			m_CursorCol--;
		}
		else if (m_CursorRow > 0)
		{
			int prevLen = (int)m_Lines[(size_t)m_CursorRow - 1].size();
			m_Lines[(size_t)m_CursorRow - 1] += m_Lines[(size_t)m_CursorRow];
			m_Lines.erase(m_Lines.begin() + m_CursorRow);
			m_CursorRow--;
			m_CursorCol = prevLen;
		}
		m_DesiredCol = m_CursorCol;
	}

	void Delete()
	{
		if (HasSelection())
		{
			DeleteSelection();
			return;
		}
		PushUndoIfNeeded(EditKind::Delete);

		std::string& current = m_Lines[(size_t)m_CursorRow];
		if (m_CursorCol < (int)current.size())
		{
			current.erase((size_t)m_CursorCol, 1);
		}
		else if (m_CursorRow + 1 < LineCount())
		{
			current += m_Lines[(size_t)m_CursorRow + 1];
			m_Lines.erase(m_Lines.begin() + m_CursorRow + 1);
		}
	}

	void InsertTab()
	{
		for (int i = 0; i < 4; i++)
			InsertChar(' ');
	}

	void MoveLeft(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		if (m_CursorCol > 0)
			m_CursorCol--;
		else if (m_CursorRow > 0)
		{
			m_CursorRow--;
			m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveRight(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		if (m_CursorCol < (int)m_Lines[(size_t)m_CursorRow].size())
			m_CursorCol++;
		else if (m_CursorRow + 1 < LineCount())
		{
			m_CursorRow++;
			m_CursorCol = 0;
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveUp(bool extend = false) { BeginOrExtendSelection(extend); MoveVertical(-1); }
	void MoveDown(bool extend = false) { BeginOrExtendSelection(extend); MoveVertical(1); }

	void MoveHome(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		m_CursorCol = 0;
		m_DesiredCol = m_CursorCol;
	}

	void MoveEnd(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		m_DesiredCol = m_CursorCol;
	}

	void MovePageUp(int rows, bool extend = false) { BeginOrExtendSelection(extend); MoveVertical(-rows); }
	void MovePageDown(int rows, bool extend = false) { BeginOrExtendSelection(extend); MoveVertical(rows); }

	void MoveDocumentStart(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		m_CursorRow = 0;
		m_CursorCol = 0;
		m_DesiredCol = 0;
	}

	void MoveDocumentEnd(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		m_CursorRow = LineCount() - 1;
		m_CursorCol = (int)m_Lines.back().size();
		m_DesiredCol = m_CursorCol;
	}

	// A mouse click's entry point: clamps into range rather than assuming
	// the caller already did (a click can land past the last line, or past
	// a short line's own end), then behaves exactly like every keyboard
	// Move* above -- `extend` shared-click (or an in-progress drag)
	// extends the selection through the same BeginOrExtendSelection path,
	// a plain click drops it and moves the cursor there instead.
	void MoveTo(int row, int col, bool extend = false)
	{
		row = std::clamp(row, 0, LineCount() - 1);
		col = std::clamp(col, 0, (int)m_Lines[(size_t)row].size());
		BeginOrExtendSelection(extend);
		m_CursorRow = row;
		m_CursorCol = col;
		m_DesiredCol = col;
	}

	// Skips a run of whitespace, then a run of "the same category" of
	// character (word chars vs. everything else) -- the usual Ctrl+Left/
	// Right model. At a line's start/end, steps to the previous/next
	// line's end/start instead, so it never gets stuck.
	void MoveWordLeft(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		if (m_CursorCol == 0)
		{
			if (m_CursorRow > 0)
			{
				m_CursorRow--;
				m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
			}
		}
		else
		{
			const std::string& line = m_Lines[(size_t)m_CursorRow];
			int col = m_CursorCol;
			while (col > 0 && std::isspace((unsigned char)line[(size_t)col - 1]))
				col--;
			if (col > 0)
			{
				bool wordChar = IsWordChar(line[(size_t)col - 1]);
				while (col > 0 && IsWordChar(line[(size_t)col - 1]) == wordChar)
					col--;
			}
			m_CursorCol = col;
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveWordRight(bool extend = false)
	{
		BeginOrExtendSelection(extend);
		const std::string& line = m_Lines[(size_t)m_CursorRow];
		int len = (int)line.size();
		if (m_CursorCol >= len)
		{
			if (m_CursorRow + 1 < LineCount())
			{
				m_CursorRow++;
				m_CursorCol = 0;
			}
		}
		else
		{
			int col = m_CursorCol;
			bool wordChar = IsWordChar(line[(size_t)col]);
			while (col < len && IsWordChar(line[(size_t)col]) == wordChar)
				col++;
			while (col < len && std::isspace((unsigned char)line[(size_t)col]))
				col++;
			m_CursorCol = col;
		}
		m_DesiredCol = m_CursorCol;
	}

	// Ctrl+Alt+Up/Down: duplicates the selection (or, with none, just the
	// current line) in the given direction. The cursor/selection ends up
	// on the *new* copy either way, so pressing the shortcut again stacks
	// another one in the same direction -- matching the editors these
	// bindings are borrowed from.
	void DuplicateLines(bool below)
	{
		bool hadSelection = HasSelection();
		int startRow = m_CursorRow, startCol = 0, endRow = m_CursorRow, endCol = 0;
		if (hadSelection)
			GetSelectionRange(startRow, startCol, endRow, endCol);

		PushUndoIfNeeded(EditKind::Other);

		std::vector<std::string> block(m_Lines.begin() + startRow, m_Lines.begin() + endRow + 1);
		int insertAt = below ? endRow + 1 : startRow;
		m_Lines.insert(m_Lines.begin() + insertAt, block.begin(), block.end());

		int shiftAmount = below ? (int)block.size() : 0;
		if (hadSelection)
		{
			m_SelectionAnchorRow = startRow + shiftAmount;
			m_SelectionAnchorCol = startCol;
			m_CursorRow = endRow + shiftAmount;
			m_CursorCol = endCol;
		}
		else
		{
			m_CursorRow = startRow + shiftAmount;
		}
		m_DesiredCol = m_CursorCol;
	}

	// Ctrl+Enter / Shift+Enter: a blank line below or above the current
	// one, indented to match it -- unlike a plain Enter, the current
	// line's own text is never split, and any selection is dropped
	// (not deleted) rather than being treated as something to type over.
	void InsertLineBelow()
	{
		PushUndoIfNeeded(EditKind::Other);
		std::string indent = LeadingWhitespace(m_Lines[(size_t)m_CursorRow]);
		m_Lines.insert(m_Lines.begin() + m_CursorRow + 1, indent);
		m_CursorRow++;
		m_CursorCol = (int)indent.size();
		m_DesiredCol = m_CursorCol;
		ClearSelection();
	}

	void InsertLineAbove()
	{
		PushUndoIfNeeded(EditKind::Other);
		std::string indent = LeadingWhitespace(m_Lines[(size_t)m_CursorRow]);
		// Inserting AT the cursor's row pushes the old line down to
		// row+1, so the new blank line lands at the row the cursor
		// already names -- no row adjustment needed.
		m_Lines.insert(m_Lines.begin() + m_CursorRow, indent);
		m_CursorCol = (int)indent.size();
		m_DesiredCol = m_CursorCol;
		ClearSelection();
	}

	// Alt+Up/Down: moves the current line, or every line the selection
	// touches, one row in the given direction -- a block move, not a
	// per-line one, so a multi-line selection travels together.
	void MoveLinesUp()
	{
		int startRow, startCol, endRow, endCol;
		bool hadSelection = HasSelection();
		if (hadSelection)
			GetSelectionRange(startRow, startCol, endRow, endCol);
		else
			startRow = endRow = m_CursorRow;

		if (startRow == 0)
			return;

		PushUndoIfNeeded(EditKind::Other);
		std::string above = m_Lines[(size_t)(startRow - 1)];
		m_Lines.erase(m_Lines.begin() + (startRow - 1));
		m_Lines.insert(m_Lines.begin() + endRow, above);

		m_CursorRow--;
		if (hadSelection)
			m_SelectionAnchorRow--;
		m_DesiredCol = m_CursorCol;
	}

	void MoveLinesDown()
	{
		int startRow, startCol, endRow, endCol;
		bool hadSelection = HasSelection();
		if (hadSelection)
			GetSelectionRange(startRow, startCol, endRow, endCol);
		else
			startRow = endRow = m_CursorRow;

		if (endRow + 1 >= LineCount())
			return;

		PushUndoIfNeeded(EditKind::Other);
		std::string below = m_Lines[(size_t)(endRow + 1)];
		m_Lines.erase(m_Lines.begin() + (endRow + 1));
		m_Lines.insert(m_Lines.begin() + startRow, below);

		m_CursorRow++;
		if (hadSelection)
			m_SelectionAnchorRow++;
		m_DesiredCol = m_CursorCol;
	}

	// Tab with no selection still just inserts a literal tab (InsertTab);
	// with one, every touched line gets indented instead of the selection
	// being replaced by a tab character -- the usual "select a block,
	// press Tab" editor behaviour. Both ends of the selection shift by
	// the same fixed 4 columns since every affected line grows by exactly
	// that much at its start.
	void IndentSelection()
	{
		int startRow, startCol, endRow, endCol;
		GetSelectionRange(startRow, startCol, endRow, endCol);

		PushUndoIfNeeded(EditKind::Other);
		for (int row = startRow; row <= endRow; row++)
			m_Lines[(size_t)row].insert(0, "    ");

		m_SelectionAnchorCol += 4;
		m_CursorCol += 4;
		m_DesiredCol = m_CursorCol;
	}

	// Shift+Tab: removes up to 4 leading spaces (or one leading tab) from
	// the current line (no selection) or every line the selection
	// touches. Each affected line can lose a different amount (a
	// shallowly-indented line has less to remove than a deep one), so
	// the two selection endpoints are corrected independently rather than
	// by one shared amount the way IndentSelection's uniform +4 can be.
	void OutdentSelection()
	{
		if (!HasSelection())
		{
			int removed = OutdentLine(m_CursorRow);
			m_CursorCol = std::max(0, m_CursorCol - removed);
			m_DesiredCol = m_CursorCol;
			return;
		}

		int startRow, startCol, endRow, endCol;
		GetSelectionRange(startRow, startCol, endRow, endCol);

		PushUndoIfNeeded(EditKind::Other);
		int removedAtStart = 0, removedAtEnd = 0;
		for (int row = startRow; row <= endRow; row++)
		{
			int removed = OutdentLine(row);
			if (row == startRow) removedAtStart = removed;
			if (row == endRow) removedAtEnd = removed;
		}

		// GetSelectionRange normalizes by document order, not by which of
		// anchor/cursor the user actually dragged from -- correct each
		// against whichever one it really is rather than assuming anchor
		// is always "start".
		if (m_SelectionAnchorRow == startRow)
		{
			m_SelectionAnchorCol = std::max(0, m_SelectionAnchorCol - removedAtStart);
			m_CursorCol = std::max(0, m_CursorCol - removedAtEnd);
		}
		else
		{
			m_SelectionAnchorCol = std::max(0, m_SelectionAnchorCol - removedAtEnd);
			m_CursorCol = std::max(0, m_CursorCol - removedAtStart);
		}
		m_DesiredCol = m_CursorCol;
	}

	// --- Find/replace ----------------------------------------------------
	// Wrap-around, case-insensitive by default. A match becomes the
	// selection (anchor at its start, cursor at its end) so repeated
	// FindNext/FindPrevious presses step through the document instead of
	// re-finding the same spot, and so a Replace right afterward is just
	// "the current selection is the text to replace".
	bool FindNext(const std::string& query, bool caseSensitive = false) { return FindImpl(query, caseSensitive, true); }
	bool FindPrevious(const std::string& query, bool caseSensitive = false) { return FindImpl(query, caseSensitive, false); }

	// One forward-only pass from the document's start -- deliberately
	// never wraps, unlike FindNext/FindPrevious, so it terminates cleanly
	// even when `replacement` itself contains `query` (each replacement is
	// skipped past, never re-scanned, so it can't regrow forever). Returns
	// how many replacements were made.
	int ReplaceAll(const std::string& query, const std::string& replacement, bool caseSensitive = false)
	{
		if (query.empty())
			return 0;

		PushUndoIfNeeded(EditKind::Other);
		m_LastEditKind = EditKind::None; // its own undo step, not coalesced with whatever ran before or after

		std::string needle = caseSensitive ? query : Lowercase(query);
		int count = 0;
		int row = 0;
		size_t col = 0;

		while (row < LineCount())
		{
			std::string line = caseSensitive ? m_Lines[(size_t)row] : Lowercase(m_Lines[(size_t)row]);
			size_t pos = line.find(needle, col);
			if (pos == std::string::npos)
			{
				row++;
				col = 0;
				continue;
			}

			m_Lines[(size_t)row].erase(pos, needle.size());
			m_Lines[(size_t)row].insert(pos, replacement);
			col = pos + replacement.size();
			count++;
		}

		ClearSelection();
		m_CursorCol = std::min(m_CursorCol, (int)m_Lines[(size_t)m_CursorRow].size());
		m_DesiredCol = m_CursorCol;
		return count;
	}

	// --- Undo/redo --------------------------------------------------------
	// Snapshot-based, not operation-based: a full copy of every line is a
	// lot more memory per step than a diff, but this editor's own files are
	// small scripts, not novels, and "copy the whole thing" is the one undo
	// implementation that cannot itself have a replay bug. Runs of the same
	// edit kind coalesce into one snapshot (see PushUndoIfNeeded) so typing
	// "hello" is one undo, not five.
	bool CanUndo() const { return !m_UndoStack.empty(); }
	bool CanRedo() const { return !m_RedoStack.empty(); }

	void Undo()
	{
		if (m_UndoStack.empty())
			return;
		m_RedoStack.push_back({ m_Lines, m_CursorRow, m_CursorCol });
		UndoState state = m_UndoStack.back();
		m_UndoStack.pop_back();
		m_Lines = state.Lines;
		m_CursorRow = state.CursorRow;
		m_CursorCol = state.CursorCol;
		m_DesiredCol = m_CursorCol;
		ClearSelection();
		m_LastEditKind = EditKind::None;
	}

	void Redo()
	{
		if (m_RedoStack.empty())
			return;
		m_UndoStack.push_back({ m_Lines, m_CursorRow, m_CursorCol });
		UndoState state = m_RedoStack.back();
		m_RedoStack.pop_back();
		m_Lines = state.Lines;
		m_CursorRow = state.CursorRow;
		m_CursorCol = state.CursorCol;
		m_DesiredCol = m_CursorCol;
		ClearSelection();
		m_LastEditKind = EditKind::None;
	}

	bool LoadFromFile(const std::string& path)
	{
		if (!std::filesystem::is_regular_file(path))
			return false;

		std::ifstream file(path);
		if (!file.is_open())
			return false;

		m_Lines.clear();
		std::string line;
		while (std::getline(file, line))
			m_Lines.push_back(line);
		if (m_Lines.empty())
			m_Lines.push_back("");

		m_CursorRow = 0;
		m_CursorCol = 0;
		m_DesiredCol = 0;
		ClearSelection();
		m_UndoStack.clear();
		m_RedoStack.clear();
		m_LastEditKind = EditKind::None;
		return true;
	}

	bool SaveToFile(const std::string& path) const
	{
		std::ofstream file(path);
		if (!file.is_open())
			return false;

		for (const std::string& line : m_Lines)
			file << line << '\n';
		return true;
	}

private:
	// 0 for anything that isn't a pairable opener -- quotes are their own
	// closer, which is what lets IsOpener/IsCloser both be true for them.
	static char ClosingFor(char c)
	{
		switch (c)
		{
			case '(': return ')';
			case '[': return ']';
			case '{': return '}';
			case '"': return '"';
			case '\'': return '\'';
			default: return 0;
		}
	}
	static bool IsOpener(char c) { return ClosingFor(c) != 0; }
	static bool IsCloser(char c) { return c == ')' || c == ']' || c == '}' || c == '"' || c == '\''; }

	static std::string LeadingWhitespace(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		return line.substr(0, i);
	}

	static bool IsWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

	static std::string Lowercase(const std::string& s)
	{
		std::string result = s;
		for (char& c : result)
			c = (char)std::tolower((unsigned char)c);
		return result;
	}

	// Removes up to 4 leading spaces, or one leading tab, from `row`.
	// Returns how many characters were actually removed, since a
	// shallowly-indented line has less to give up than a deeply-indented
	// one -- callers need that to keep the cursor/selection's column
	// correct afterward.
	int OutdentLine(int row)
	{
		std::string& line = m_Lines[(size_t)row];
		int remove = 0;
		while (remove < 4 && remove < (int)line.size() && line[(size_t)remove] == ' ')
			remove++;
		if (remove == 0 && !line.empty() && line[0] == '\t')
			remove = 1;
		line.erase(0, (size_t)remove);
		return remove;
	}

	void SelectRange(int startRow, int startCol, int endRow, int endCol)
	{
		m_SelectionAnchorRow = startRow;
		m_SelectionAnchorCol = startCol;
		m_CursorRow = endRow;
		m_CursorCol = endCol;
		m_DesiredCol = endCol;
	}

	bool FindImpl(const std::string& query, bool caseSensitive, bool forward)
	{
		if (query.empty())
			return false;

		std::string needle = caseSensitive ? query : Lowercase(query);
		int lineCount = LineCount();

		int row, col;
		if (HasSelection())
		{
			int sr, sc, er, ec;
			GetSelectionRange(sr, sc, er, ec);
			row = forward ? er : sr;
			col = forward ? ec : sc;
		}
		else
		{
			row = m_CursorRow;
			col = m_CursorCol;
		}

		// lineCount+1 passes: the extra one lets a single-line document (or
		// any search that starts past every match on its own line) wrap
		// all the way back around to find a match earlier on the very row
		// it started from.
		for (int steps = 0; steps <= lineCount; steps++)
		{
			bool constrained = (steps == 0);
			std::string line = caseSensitive ? m_Lines[(size_t)row] : Lowercase(m_Lines[(size_t)row]);

			size_t pos;
			if (forward)
				pos = line.find(needle, constrained ? (size_t)col : 0);
			else if (constrained && col == 0)
				pos = std::string::npos;
			else
				pos = line.rfind(needle, constrained ? (size_t)(col - 1) : std::string::npos);

			if (pos != std::string::npos)
			{
				SelectRange(row, (int)pos, row, (int)(pos + needle.size()));
				return true;
			}

			row = forward ? (row + 1) % lineCount : (row - 1 + lineCount) % lineCount;
		}

		return false;
	}

	void MoveVertical(int delta)
	{
		int target = m_CursorRow + delta;
		if (target < 0)
			target = 0;
		if (target >= LineCount())
			target = LineCount() - 1;
		m_CursorRow = target;
		m_CursorCol = std::min(m_DesiredCol, (int)m_Lines[(size_t)m_CursorRow].size());
	}

	// Shared by every Move* method: extends the existing selection (or
	// starts one at the pre-move position) when Shift is held, otherwise
	// drops any selection -- an ordinary, unshifted cursor move deselects,
	// same as every other editor. Also breaks undo-run coalescing, so
	// "type, move away, move back, type" is two undo steps instead of one
	// that silently spans the movement in between.
	void BeginOrExtendSelection(bool extend)
	{
		if (extend)
		{
			if (!HasSelection())
			{
				m_SelectionAnchorRow = m_CursorRow;
				m_SelectionAnchorCol = m_CursorCol;
			}
		}
		else
		{
			ClearSelection();
		}
		m_LastEditKind = EditKind::None;
	}

	enum class EditKind { None, Insert, Backspace, Delete, Other };

	struct UndoState
	{
		std::vector<std::string> Lines;
		int CursorRow;
		int CursorCol;
	};

	// Pushes the pre-edit state onto the undo stack only when this edit's
	// kind differs from the last one recorded -- consecutive InsertChar
	// calls (ordinary typing) share one snapshot instead of one per
	// keystroke. Any push invalidates the redo stack, same as every other
	// editor: redo only makes sense immediately after an undo, not after a
	// fresh edit branches off from it.
	void PushUndoIfNeeded(EditKind kind)
	{
		if (kind == m_LastEditKind)
			return;
		m_UndoStack.push_back({ m_Lines, m_CursorRow, m_CursorCol });
		if (m_UndoStack.size() > 200)
			m_UndoStack.erase(m_UndoStack.begin());
		m_RedoStack.clear();
		m_LastEditKind = kind;
	}

	std::vector<std::string> m_Lines;
	int m_CursorRow = 0;
	int m_CursorCol = 0;
	int m_DesiredCol = 0; // preserved across vertical moves through shorter lines

	int m_SelectionAnchorRow = -1;
	int m_SelectionAnchorCol = -1;

	std::vector<UndoState> m_UndoStack;
	std::vector<UndoState> m_RedoStack;
	EditKind m_LastEditKind = EditKind::None;
};
