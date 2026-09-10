#pragma once

// Browses the currently open project's files. No persistent tree state of
// our own -- walks std::filesystem fresh each frame. ImGui's own TreeNode
// does track each node's open/closed state by ID for the running session
// (in window->StateStorage), but that state is NOT serialized to
// imgui.ini (only window/table/dock layout is) -- tree expansion resets
// on the next launch. Either way, there's nothing here that needs to be
// kept in sync with the real filesystem by hand.

#include <GS.h>
#include <imgui.h>

#include "EditorProject.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

class FileTreePanel
{
public:
	// Returns the path of a file the user clicked this frame, or an empty
	// string if nothing was clicked.
	std::string OnImGuiRender()
	{
		std::string clicked;

		if (ImGui::Begin("Files"))
		{
			if (g_EditorProjectPath.empty())
				ImGui::TextDisabled("Open or create a project to browse its files.");
			else
				DrawDirectory(g_EditorProjectPath, clicked);
		}

		ImGui::End();
		return clicked;
	}

private:
	void DrawDirectory(const std::filesystem::path& dir, std::string& clicked)
	{
		struct Entry
		{
			std::filesystem::path Path;
			std::string Name;
			bool IsDir;
		};

		std::error_code ec;
		std::vector<Entry> entries;
		for (const auto& dirEntry : std::filesystem::directory_iterator(dir, ec))
		{
			std::string name = dirEntry.path().filename().string();
			if (name.empty() || name[0] == '.')
				continue;

			std::error_code typeEc;
			bool isDir = dirEntry.is_directory(typeEc); // noexcept overload -- false on error, never throws
			entries.push_back({ dirEntry.path(), name, isDir });
		}

		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
			if (a.IsDir != b.IsDir)
				return a.IsDir; // directories before files
			return a.Name < b.Name;
		});

		for (const Entry& entry : entries)
		{
			if (entry.IsDir)
			{
				if (ImGui::TreeNode(entry.Name.c_str()))
				{
					DrawDirectory(entry.Path, clicked);
					ImGui::TreePop();
				}
			}
			else
			{
				if (ImGui::Selectable(entry.Name.c_str()))
					clicked = entry.Path.string();
			}
		}
	}
};
