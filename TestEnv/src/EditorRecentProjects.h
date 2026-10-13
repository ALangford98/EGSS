#pragma once

// The projects the editor has had open, most recent first -- what the
// Projects window and File > Open Recent list, and what a plain launch
// reopens. docs/superpowers/specs/2026-10-13-project-save-load-design.md.
//
// Per user, not per build: $XDG_CONFIG_HOME/gs-editor/recent-projects.txt
// (~/.config when that is unset). Beside the binary, where imgui.ini lives,
// Debug and Release would each keep a list of their own, and a project
// opened in one would be missing from the other.
//
// An unattended run (--capture, --play, --export) neither reads nor writes
// it: a test run must not reopen the owner's last project behind a capture,
// or rewrite the owner's list with scratch folders.

#include <GS.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace RecentProjects {

	constexpr size_t MaxEntries = 10;

	inline bool IsUnattendedRun()
	{
		for (const std::string& argument : GS::Application::GetCommandLine())
			if (argument == "--capture" || argument == "--play" || argument == "--export")
				return true;
		return false;
	}

	inline std::filesystem::path FilePath()
	{
		const char* config = std::getenv("XDG_CONFIG_HOME");
		std::filesystem::path base;
		if (config && config[0] != '\0')
			base = config;
		else if (const char* home = std::getenv("HOME"))
			base = std::filesystem::path(home) / ".config";
		else
			return {};
		return base / "gs-editor" / "recent-projects.txt";
	}

	inline std::vector<std::string> Load()
	{
		std::vector<std::string> paths;
		if (IsUnattendedRun())
			return paths;
		std::ifstream in(FilePath());
		std::string line;
		while (std::getline(in, line) && paths.size() < MaxEntries)
			if (!line.empty() && std::find(paths.begin(), paths.end(), line) == paths.end())
				paths.push_back(line);
		return paths;
	}

	inline void Save(const std::vector<std::string>& paths)
	{
		if (IsUnattendedRun())
			return;
		std::filesystem::path file = FilePath();
		if (file.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::ofstream out(file);
		for (size_t i = 0; i < paths.size() && i < MaxEntries; i++)
			out << paths[i] << "\n";
	}

	// To the front, once.
	inline void Touch(const std::string& projectFolder)
	{
		std::vector<std::string> paths = Load();
		paths.erase(std::remove(paths.begin(), paths.end(), projectFolder), paths.end());
		paths.insert(paths.begin(), projectFolder);
		Save(paths);
	}

	inline void Remove(const std::string& projectFolder)
	{
		std::vector<std::string> paths = Load();
		paths.erase(std::remove(paths.begin(), paths.end(), projectFolder), paths.end());
		Save(paths);
	}
}
