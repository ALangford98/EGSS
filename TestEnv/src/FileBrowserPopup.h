#pragma once

// A reusable in-editor file/folder browser, opened as an ImGui modal popup.
//
// Not a native OS dialog. Checked before writing this: no CLI dialog helper
// (kdialog/zenity/yad) is installed on this machine to shell out to, the way
// RunSyntaxCheck already shells out to g++. A real one means either hand-
// rolling xdg-desktop-portal's FileChooser interface -- an async D-Bus
// request/response exchange (the method call returns a request handle
// immediately; the actual result arrives later as a signal on that handle)
// that is awkward and fragile to drive by shelling out to gdbus/busctl and
// parsing their text output -- or vendoring a library that already does
// this (nativefiledialog-extended), which pulls in a new build dependency
// (its Linux backend links GTK3) for a project that has otherwise built
// every other editor amenity itself (the text editor, the terminal) rather
// than reach for an external one. This is that same choice again, and
// reuses FileTreePanel.h's own recursive TreeNode walk (directories first,
// alphabetical, dotfiles skipped) rather than inventing a second file-list
// convention.
#include <GS.h>
#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

class FileBrowserPopup
{
public:
	enum class Mode { PickFolder, PickFile };

	// `rootDir` is where browsing starts -- the caller's best guess at
	// where the user probably wants to be (the open project's folder, or
	// the working directory). `filterExtension` (e.g. ".gss") only applies
	// in PickFile mode; empty shows every file.
	void Open(const std::string& title, Mode mode, const std::string& rootDir, const std::string& filterExtension = "")
	{
		m_Title = title;
		m_Mode = mode;
		m_FilterExtension = filterExtension;
		m_HasResult = false;
		m_Result.clear();

		std::error_code ec;
		std::filesystem::path root = rootDir.empty() ? std::filesystem::current_path(ec) : std::filesystem::path(rootDir);
		if (!std::filesystem::is_directory(root, ec))
			root = std::filesystem::current_path(ec);

		m_Root = root.string();
		// PickFolder's implicit default is the root itself -- browsing in
		// further narrows it, never leaves nothing selected the way a file
		// pick starts with nothing to confirm.
		m_Selected = (mode == Mode::PickFolder) ? m_Root : std::string();
		m_ShouldOpen = true;
	}

	// Call every frame, whether or not Open() was just called -- same
	// always-drawn shape EditorMenuBar's own popup modals already use.
	void Draw()
	{
		if (m_ShouldOpen)
		{
			ImGui::OpenPopup(m_Title.c_str());
			m_ShouldOpen = false;
		}

		ImGui::SetNextWindowSize(ImVec2(560.0f, 440.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::BeginPopupModal(m_Title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextDisabled("%s", m_Root.c_str());
		ImGui::BeginChild("##browserentries", ImVec2(0.0f, -32.0f), true);
		DrawDirectory(m_Root);
		ImGui::EndChild();

		bool canConfirm = !m_Selected.empty();
		if (!canConfirm)
			ImGui::BeginDisabled();
		if (ImGui::Button(m_Mode == Mode::PickFolder ? "Choose Folder" : "Choose File"))
		{
			m_Result = m_Selected;
			m_HasResult = true;
			ImGui::CloseCurrentPopup();
		}
		if (!canConfirm)
			ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();

		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_Selected.empty() ? "(nothing selected)" : m_Selected.c_str());

		ImGui::EndPopup();
	}

	bool HasResult() const { return m_HasResult; }
	// Clears the pending result -- this is drawn every frame regardless of
	// whether it's open, so a caller that checked without clearing would
	// otherwise see the same confirmed path forever.
	std::string TakeResult()
	{
		m_HasResult = false;
		return m_Result;
	}

private:
	struct Entry
	{
		std::filesystem::path Path;
		std::string Name;
		bool IsDir;
	};

	void DrawDirectory(const std::filesystem::path& dir)
	{
		std::error_code ec;
		std::vector<Entry> entries;
		for (const auto& dirEntry : std::filesystem::directory_iterator(dir, ec))
		{
			std::string name = dirEntry.path().filename().string();
			if (name.empty() || name[0] == '.')
				continue;

			std::error_code typeEc;
			bool isDir = dirEntry.is_directory(typeEc); // noexcept overload -- false on error, never throws
			if (!isDir && m_Mode == Mode::PickFile && !MatchesFilter(dirEntry.path()))
				continue;

			entries.push_back({ dirEntry.path(), name, isDir });
		}

		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
			if (a.IsDir != b.IsDir)
				return a.IsDir; // directories before files
			return a.Name < b.Name;
		});

		for (const Entry& entry : entries)
		{
			std::string pathStr = entry.Path.string();

			if (entry.IsDir)
			{
				bool isSelected = (m_Mode == Mode::PickFolder && m_Selected == pathStr);
				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
					| (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

				bool open = ImGui::TreeNodeEx(entry.Name.c_str(), flags);
				// The arrow toggles open/closed; the label picks the
				// folder -- IsItemToggledOpen distinguishes "this click
				// was the arrow" from "this click was the label" so the
				// two don't fight over what one click means.
				if (m_Mode == Mode::PickFolder && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
					m_Selected = pathStr;

				if (open)
				{
					DrawDirectory(entry.Path);
					ImGui::TreePop();
				}
			}
			else
			{
				bool isSelected = (m_Selected == pathStr);
				if (ImGui::Selectable(entry.Name.c_str(), isSelected))
					m_Selected = pathStr;
			}
		}
	}

	bool MatchesFilter(const std::filesystem::path& path) const
	{
		return m_FilterExtension.empty() || path.extension().string() == m_FilterExtension;
	}

private:
	std::string m_Title;
	Mode m_Mode = Mode::PickFolder;
	std::string m_FilterExtension;
	std::string m_Root;
	std::string m_Selected;
	bool m_ShouldOpen = false;
	bool m_HasResult = false;
	std::string m_Result;
};
