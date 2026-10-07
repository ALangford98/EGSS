#pragma once
// A project's manifest (project.gsproj): its name and its scenes. Read by
// the editor when it opens a project and by GSPlayer when it runs one.
#include <GS.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// A project is a folder holding a manifest (name, which scene is active)
// wrapping today's single g_EditorScene. Deliberately one scene per
// project for now -- a project that can hold several is a real feature for
// when something actually needs it, not built speculatively here.
struct SceneEntry
{
	std::string Name;
	std::string RelativePath;
};

struct ProjectManifest
{
	std::string Name;
	std::string SceneRelativePath;
	std::vector<SceneEntry> Scenes;
};

inline const char* ProjectManifestFilename() { return "project.gsproj"; }

// Shared by OpenEditorProject and RenameEditorProject so the manifest's
// tagged-text format is parsed in exactly one place.
inline bool ReadProjectManifest(const std::string& folderPath, ProjectManifest& out)
{
	std::filesystem::path manifestPath = std::filesystem::path(folderPath) / ProjectManifestFilename();

	std::ifstream in(manifestPath.string());
	if (!in)
		return false;

	std::string tag;
	int version = 0;
	in >> tag >> version;

	if (tag != "gs-project" || version != 1)
		return false;

	std::string line;
	std::getline(in, line);   // rest of the header line

	while (std::getline(in, line))
	{
		std::istringstream fields(line);
		std::string key;
		fields >> key;

		if (key == "name")
		{
			std::string name;
			std::getline(fields, name);
			size_t from = name.find_first_not_of(' ');
			out.Name = from == std::string::npos ? "" : name.substr(from);
		}
		else if (key == "scene")
		{
			fields >> out.SceneRelativePath;
		}
		else if (key == "sceneentry")
		{
			SceneEntry entry;
			fields >> entry.Name >> entry.RelativePath;
			out.Scenes.push_back(entry);
		}
	}

	// Old-format manifest (no sceneentry lines): synthesize the one scene
	// it already names, so every caller of ReadProjectManifest can rely on
	// Scenes being non-empty for any manifest with a valid `scene` line,
	// without needing to know the format's history.
	if (out.Scenes.empty() && !out.SceneRelativePath.empty())
	{
		std::string stem = std::filesystem::path(out.SceneRelativePath).stem().string();
		out.Scenes.push_back({ stem, out.SceneRelativePath });
	}

	return !out.SceneRelativePath.empty();
}
