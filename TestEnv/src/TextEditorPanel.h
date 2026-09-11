#pragma once

// Glues TextBuffer + CellGrid into the editor's "Editor" panel.

#include <GS.h>
#include <imgui.h>

#include "CellGrid.h"
#include "EditorTheme.h"
#include "PlayMode.h"
#include "ScriptEngine.h"
#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

// Whether Ctrl+S on this file should also run the GSS-to-C++ transpiler
// pipeline (see docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-
// design.md), rather than just saving the buffer. `.gs` was floated once
// and never actually used anywhere; kept here only so a leftover file
// from that naming still gets picked up.
inline bool IsGssFile(const std::string& path)
{
	auto endsWith = [&](const char* ext) {
		size_t len = strlen(ext);
		return path.size() >= len && path.compare(path.size() - len, len, ext) == 0;
	};
	return endsWith(".gss") || endsWith(".ts") || endsWith(".gs");
}

// "paddle.gss" -> "Paddle", "my_cool-script.gss" -> "MyCoolScript" -- the
// generated class's name, matching TranspileToCpp's own className
// parameter.
inline std::string ClassNameFromPath(const std::string& path)
{
	std::string stem = std::filesystem::path(path).stem().string();
	std::string result;
	bool capitalizeNext = true;
	for (char c : stem)
	{
		if (c == '_' || c == '-')
		{
			capitalizeNext = true;
			continue;
		}
		result += capitalizeNext ? (char)std::toupper((unsigned char)c) : c;
		capitalizeNext = false;
	}
	return result.empty() ? "Script" : result;
}

inline std::string CppPathFor(const std::string& gssPath)
{
	std::filesystem::path p(gssPath);
	p.replace_extension(".cpp");
	return p.string();
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

		bool showPreview = IsGssFile(m_PathBuffer);
		if (showPreview)
			UpdatePreviewIfNeeded();

		ImVec2 avail = ImGui::GetContentRegionAvail();
		float editorWidth = showPreview ? avail.x * 0.5f - 4.0f : avail.x;

		if (showPreview)
			ImGui::BeginChild("##EditorSource", ImVec2(editorWidth, 0.0f), false);

		{
			ImVec2 paneAvail = ImGui::GetContentRegionAvail();
			float cellWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "M").x;
			float cellHeight = ImGui::GetTextLineHeight();

			int gutterDigits = std::max(2, (int)std::to_string(m_Buffer.LineCount()).size());
			int gutterCols = gutterDigits + 1; // one column of spacing after the number

			int totalCols = std::max(gutterCols + 8, (int)(paneAvail.x / cellWidth));
			int textCols = totalCols - gutterCols;
			int rows = std::max(2, (int)(paneAvail.y / cellHeight));

			HandleInput(rows);
			ScrollToCursor(rows);

			m_Grid.Resize(totalCols, rows);
			RenderBufferToGrid(m_Buffer, m_Grid, m_Theme, m_ScrollRow, gutterDigits, gutterCols, textCols, rows);
			m_Grid.Render();
		}

		if (showPreview)
		{
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##EditorCppPreview", ImVec2(0.0f, 0.0f), true);
			DrawCppPreview();
			ImGui::EndChild();
		}

		ImGui::End();

		DrawOverwritePrompt();
		DrawFanOutPrompt();
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
		if (IsGssFile(m_PathBuffer))
		{
			ImGui::SameLine();
			if (ImGui::Button("Build (Ctrl+S)"))
				SaveAndBuild();
		}
		if (!m_StatusMessage.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_StatusMessage.c_str());
		}
	}

	// The live preview pane: transpile-only (no compiler invocation -- that
	// only happens on an actual build, see SaveAndBuild), refreshed a short
	// idle period after the buffer stops changing so it doesn't re-run
	// against invalid, half-typed syntax on every keystroke.
	void UpdatePreviewIfNeeded()
	{
		std::string currentText = m_Buffer.FullText();
		if (currentText != m_LastSeenText)
		{
			m_LastSeenText = currentText;
			m_IdleSeconds = 0.0f;
			return;
		}

		m_IdleSeconds += ImGui::GetIO().DeltaTime;
		if (m_IdleSeconds < 0.5f || currentText == m_PreviewSourceSeen)
			return;

		m_PreviewSourceSeen = currentText;
		if (!m_ScriptEngine)
		{
			m_PreviewError = "No script engine.";
			m_PreviewCpp.clear();
			return;
		}

		std::string error, cpp;
		if (m_ScriptEngine->TranspileToCpp(currentText, ClassNameFromPath(m_PathBuffer), error, cpp))
		{
			m_PreviewCpp = cpp;
			m_PreviewError.clear();
		}
		else
		{
			m_PreviewError = error;
			m_PreviewCpp.clear();
		}
	}

	void DrawCppPreview()
	{
		if (!m_PreviewError.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_PreviewError.c_str());
		else if (!m_PreviewCpp.empty())
			ImGui::TextUnformatted(m_PreviewCpp.c_str());
		else
			ImGui::TextDisabled("Stop typing for a moment to see the generated C++.");
	}

	// Ctrl+S (or the Build button): save the .gss, then run the actual
	// pipeline -- transpile, check the optimize-lock, write + syntax-check
	// the .cpp. Also checks whether any *other* script imports this file
	// as a module and, if so, whether saving it has now made any of their
	// already-hand-optimized .cpp files stale -- the dependency-manifest
	// fan-out (Task 6). Only reached for a .gss/.ts file; a plain file's
	// Ctrl+S (not wired up here at all) would just be an ordinary save,
	// which the "Save" button already covers.
	void SaveAndBuild()
	{
		m_StatusMessage = m_Buffer.SaveToFile(m_PathBuffer) ? "Saved." : "Could not save file.";
		if (m_StatusMessage != "Saved.")
			return;

		RunFullBuild(m_PathBuffer, m_Buffer.FullText());

		std::vector<std::string> dependents = PlayMode::FindDependentScripts(m_PathBuffer);
		m_PendingFanOut.clear();
		for (const std::string& dependent : dependents)
			if (ScriptEngine::IsGeneratedFileHandEdited(CppPathFor(dependent)))
				m_PendingFanOut.push_back(dependent);
		if (!m_PendingFanOut.empty())
			m_ShowFanOutPrompt = true;
	}

	// Transpiles `gssPath`'s current `source` and either writes its .cpp
	// straight away (nothing to lose) or, if the existing .cpp has been
	// hand-edited since it was last generated, defers to the Cancel/
	// Overwrite/Save-a-copy prompt instead of overwriting silently.
	void RunFullBuild(const std::string& gssPath, const std::string& source)
	{
		if (!m_ScriptEngine)
		{
			m_LastRunError = "No script engine.";
			return;
		}

		std::string error, cpp;
		if (!m_ScriptEngine->TranspileToCpp(source, ClassNameFromPath(gssPath), error, cpp))
		{
			m_LastRunOutput.clear();
			m_LastRunError = error;
			return;
		}

		std::string cppPath = CppPathFor(gssPath);
		if (ScriptEngine::IsGeneratedFileHandEdited(cppPath))
		{
			m_PendingOverwrite = { cppPath, cpp };
			m_ShowOverwritePrompt = true;
			return;
		}

		WriteAndSyntaxCheck(cppPath, cpp);
	}

	void WriteAndSyntaxCheck(const std::string& cppPath, const std::string& cpp)
	{
		std::ofstream out(cppPath);
		out << cpp;
		out.close();

		std::string diagnostics;
		bool ok = ScriptEngine::RunSyntaxCheck(cppPath, diagnostics);
		m_LastRunOutput = ok ? ("Compiled OK: " + cppPath) : "";
		m_LastRunError = ok ? "" : diagnostics;
	}

	void DrawOverwritePrompt()
	{
		if (m_ShowOverwritePrompt)
		{
			ImGui::OpenPopup("Overwrite hand-edited file?");
			m_ShowOverwritePrompt = false;
		}
		if (ImGui::BeginPopupModal("Overwrite hand-edited file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s has been hand-edited since it was generated.", m_PendingOverwrite.CppPath.c_str());
			ImGui::TextDisabled("Rebuilding it from the .gss source would discard those edits.");
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("Overwrite"))
			{
				WriteAndSyntaxCheck(m_PendingOverwrite.CppPath, m_PendingOverwrite.Cpp);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Save a copy"))
			{
				WriteAndSyntaxCheck(GeneratedCopyPath(m_PendingOverwrite.CppPath), m_PendingOverwrite.Cpp);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// One dialog for every affected dependent, not one popup per file --
	// see docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md's
	// "Dependency-manifest fan-out" section for why that matters once a
	// widely-shared module has several hand-optimized dependents.
	void DrawFanOutPrompt()
	{
		if (m_ShowFanOutPrompt)
		{
			ImGui::OpenPopup("Dependent scripts affected");
			m_ShowFanOutPrompt = false;
		}
		if (ImGui::BeginPopupModal("Dependent scripts affected", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("These scripts import the module you just saved and have already been hand-optimized:");
			for (size_t i = 0; i < m_PendingFanOut.size(); i++)
			{
				ImGui::PushID((int)i);
				ImGui::BulletText("%s", m_PendingFanOut[i].c_str());
				ImGui::SameLine();
				bool handled = false;
				if (ImGui::SmallButton("Overwrite"))
				{
					RebuildScriptFile(m_PendingFanOut[i], false);
					handled = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Save a copy"))
				{
					RebuildScriptFile(m_PendingFanOut[i], true);
					handled = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Skip"))
					handled = true;
				ImGui::PopID();
				if (handled)
				{
					m_PendingFanOut.erase(m_PendingFanOut.begin() + i);
					break;   // re-enter next frame against the shrunk list
				}
			}
			ImGui::Separator();
			if (m_PendingFanOut.empty() || ImGui::Button("Done"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	static std::string GeneratedCopyPath(const std::string& cppPath)
	{
		std::filesystem::path p(cppPath);
		return p.replace_extension("").string() + ".generated.cpp";
	}

	// Rebuilds a *different* script than the one currently open (a fan-out
	// dependent) -- reads it fresh from disk into a throwaway buffer
	// rather than assuming it happens to match whatever's in m_Buffer.
	void RebuildScriptFile(const std::string& gssPath, bool asCopy)
	{
		if (!m_ScriptEngine)
			return;
		TextBuffer temp;
		if (!temp.LoadFromFile(gssPath))
			return;

		std::string error, cpp;
		if (!m_ScriptEngine->TranspileToCpp(temp.FullText(), ClassNameFromPath(gssPath), error, cpp))
			return;

		std::string cppPath = asCopy ? GeneratedCopyPath(CppPathFor(gssPath)) : CppPathFor(gssPath);
		WriteAndSyntaxCheck(cppPath, cpp);
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
		// A plain file's Ctrl+S isn't handled at all here -- only a .gss/.ts
		// one runs the transpile pipeline; every other file type still only
		// saves through the "Save" button, unchanged from before this.
		if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, true) && IsGssFile(m_PathBuffer))
			SaveAndBuild();

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

	// --- GSS-to-C++ transpiler UI (Tasks 5-6) -------------------------
	std::string m_LastSeenText;      // what the buffer held as of the last UpdatePreviewIfNeeded call
	std::string m_PreviewSourceSeen; // what m_PreviewCpp/m_PreviewError currently correspond to
	float m_IdleSeconds = 0.0f;
	std::string m_PreviewCpp;
	std::string m_PreviewError;

	struct PendingOverwrite { std::string CppPath; std::string Cpp; };
	PendingOverwrite m_PendingOverwrite;
	bool m_ShowOverwritePrompt = false;

	std::vector<std::string> m_PendingFanOut;
	bool m_ShowFanOutPrompt = false;
};
