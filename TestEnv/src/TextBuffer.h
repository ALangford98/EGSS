#pragma once

// Pure text-buffer data and edit operations for the embedded text editor.
// No ImGui, no rendering -- TextEditorPanel (Task 3) copies this into a
// CellGrid. ASCII-only, byte-indexed columns: inherits CellGrid's existing
// ASCII rendering limit (a loaded file's non-ASCII bytes will show as
// blank cells, same disclosed cut the embedded terminal already made).

#include <algorithm>
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

	void InsertChar(char c)
	{
		m_Lines[(size_t)m_CursorRow].insert((size_t)m_CursorCol, 1, c);
		m_CursorCol++;
		m_DesiredCol = m_CursorCol;
	}

	// Splits the current line at the cursor. The new line inherits the
	// current line's leading whitespace, so pressing Enter mid-block keeps
	// the same indent -- the "line indenting" this editor exists to have.
	void InsertNewline()
	{
		std::string& current = m_Lines[(size_t)m_CursorRow];
		std::string indent = LeadingWhitespace(current);
		std::string rest = current.substr((size_t)m_CursorCol);
		current.erase((size_t)m_CursorCol);

		m_Lines.insert(m_Lines.begin() + m_CursorRow + 1, indent + rest);
		m_CursorRow++;
		m_CursorCol = (int)indent.size();
		m_DesiredCol = m_CursorCol;
	}

	void Backspace()
	{
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

	void MoveLeft()
	{
		if (m_CursorCol > 0)
			m_CursorCol--;
		else if (m_CursorRow > 0)
		{
			m_CursorRow--;
			m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveRight()
	{
		if (m_CursorCol < (int)m_Lines[(size_t)m_CursorRow].size())
			m_CursorCol++;
		else if (m_CursorRow + 1 < LineCount())
		{
			m_CursorRow++;
			m_CursorCol = 0;
		}
		m_DesiredCol = m_CursorCol;
	}

	void MoveUp() { MoveVertical(-1); }
	void MoveDown() { MoveVertical(1); }

	void MoveHome()
	{
		m_CursorCol = 0;
		m_DesiredCol = m_CursorCol;
	}

	void MoveEnd()
	{
		m_CursorCol = (int)m_Lines[(size_t)m_CursorRow].size();
		m_DesiredCol = m_CursorCol;
	}

	void MovePageUp(int rows) { MoveVertical(-rows); }
	void MovePageDown(int rows) { MoveVertical(rows); }

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
	static std::string LeadingWhitespace(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		return line.substr(0, i);
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

	std::vector<std::string> m_Lines;
	int m_CursorRow = 0;
	int m_CursorCol = 0;
	int m_DesiredCol = 0; // preserved across vertical moves through shorter lines
};
