#pragma once

// The editor's own per-user preferences: key = value lines in
// $XDG_CONFIG_HOME/gs-editor/settings.txt, beside the recent-projects list
// (EditorRecentProjects.h) and for the same reason -- per user, not per
// build folder. Unattended runs (--capture, --play, --export) read nothing
// and write nothing: a test must not depend on, or change, how the owner
// has the editor set up.

#include <GS.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "EditorRecentProjects.h"

namespace EditorSettings {

	inline std::filesystem::path FilePath()
	{
		std::filesystem::path recent = RecentProjects::FilePath();
		return recent.empty() ? recent : recent.parent_path() / "settings.txt";
	}

	inline std::map<std::string, std::string> LoadAll()
	{
		std::map<std::string, std::string> values;
		if (RecentProjects::IsUnattendedRun())
			return values;
		std::ifstream in(FilePath());
		std::string line;
		while (std::getline(in, line))
		{
			size_t equals = line.find('=');
			if (equals != std::string::npos)
				values[line.substr(0, equals)] = line.substr(equals + 1);
		}
		return values;
	}

	inline std::string Get(const std::string& key, const std::string& fallback = "")
	{
		auto values = LoadAll();
		auto found = values.find(key);
		return found == values.end() ? fallback : found->second;
	}

	inline void Set(const std::string& key, const std::string& value)
	{
		if (RecentProjects::IsUnattendedRun() || FilePath().empty())
			return;
		auto values = LoadAll();
		if (value.empty())
			values.erase(key);
		else
			values[key] = value;
		std::error_code ec;
		std::filesystem::create_directories(FilePath().parent_path(), ec);
		std::ofstream out(FilePath());
		for (const auto& [k, v] : values)
			out << k << "=" << v << "\n";
	}
}
