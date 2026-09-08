#pragma once

// The project the editor has open: one GS::Scene, a path (empty if it has
// never been saved), and enough bookkeeping to reopen the same one next run.
//
// Kept separate from EditorShell.h on purpose -- that file lays out panels,
// this one owns what they show. A layout change should never risk the scene.

#include <GS.h>
#include <fstream>
#include <filesystem>
#include <sstream>

inline GS::Scene g_EditorScene;
inline std::string g_EditorScenePath;

// Not checked in -- generated beside the executable, the same way imgui.ini
// and profile.json are. Remembers the last scene across runs so relaunching
// the editor picks up where you left off rather than opening blank.
inline const char* EditorLastScenePath() { return "editor_last_scene.txt"; }

inline bool OpenEditorScene(const std::string& path)
{
	if (!g_EditorScene.Load(path))
		return false;

	g_EditorScenePath = path;

	std::ofstream last(EditorLastScenePath());
	if (last)
		last << path;

	return true;
}

inline bool SaveEditorScene(const std::string& path)
{
	if (!g_EditorScene.Save(path))
		return false;

	g_EditorScenePath = path;

	std::ofstream last(EditorLastScenePath());
	if (last)
		last << path;

	return true;
}

// --demo/--scene are mutually exclusive by construction: SelectDemoFromCommandLine
// (DemoRegistry.h) only ever moves g_ActiveDemo off InvalidDemo when --demo is
// given, and this function only runs the editor's own boot logic, so whichever
// flag is present decides which of the two ever does anything observable.
inline void LoadEditorProjectFromCommandLine()
{
	const std::vector<std::string>& arguments = GS::Application::GetCommandLine();

	for (size_t i = 1; i + 1 < arguments.size(); i++)
	{
		if (arguments[i] != "--scene")
			continue;

		if (!OpenEditorScene(arguments[i + 1]))
			GS_WARN("--scene '{0}' could not be opened; starting blank", arguments[i + 1]);

		return;
	}

	// No --scene: reopen whatever was open last, if anything. A missing or
	// unreadable last-scene file is not a warning -- the very first run has
	// neither, and that is not a fault.
	std::ifstream last(EditorLastScenePath());
	std::string path;
	if (last && std::getline(last, path) && !path.empty())
		OpenEditorScene(path);
}

// A project is a folder holding a manifest (name, which scene is active)
// wrapping today's single g_EditorScene. Deliberately one scene per
// project for now -- a project that can hold several is a real feature for
// when something actually needs it, not built speculatively here.
struct ProjectManifest
{
	std::string Name;
	std::string SceneRelativePath;
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
	}

	return !out.SceneRelativePath.empty();
}

inline bool WriteProjectManifest(const std::string& folderPath, const ProjectManifest& manifest)
{
	std::filesystem::path manifestPath = std::filesystem::path(folderPath) / ProjectManifestFilename();

	std::ofstream out(manifestPath.string());
	if (!out)
		return false;

	out << "gs-project 1\n";
	out << "name " << manifest.Name << "\n";
	out << "scene " << manifest.SceneRelativePath << "\n";
	return true;
}

// The project's own name, distinct from its folder path -- empty when no
// project is open (a bare --scene session, exactly today's behaviour).
inline std::string g_EditorProjectPath;
inline std::string g_EditorProjectName;

// Creates a new, blank project: the folder, a fresh empty scene inside it,
// and a manifest naming both. Clears g_EditorScene first -- "new" means
// starting over, unlike SaveEditorProjectAs below.
inline bool CreateEditorProject(const std::string& folderPath, const std::string& name)
{
	std::error_code ec;
	std::filesystem::create_directories(folderPath, ec);
	if (ec)
	{
		GS_WARN("Could not create project folder '{0}': {1}", folderPath, ec.message());
		return false;
	}

	ProjectManifest manifest;
	manifest.Name = name;
	manifest.SceneRelativePath = "scene.txt";

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();

	// Save a blank scene to disk *before* clearing the live one -- a failed
	// write here (disk full, a permissions quirk create_directories didn't
	// itself catch) must not cost whatever scene was already open.
	GS::Scene blank;
	if (!blank.Save(scenePath))
	{
		GS_WARN("Could not create the new project's scene file at '{0}'", scenePath);
		return false;
	}

	if (!WriteProjectManifest(folderPath, manifest))
	{
		GS_WARN("Could not write a project manifest in '{0}'", folderPath);
		return false;
	}

	g_EditorScene.Clear();
	g_EditorProjectPath = folderPath;
	g_EditorProjectName = name;
	g_EditorScenePath = scenePath;
	return true;
}

// Writes whatever scene is *currently open* into a new project folder,
// without touching it first -- the opposite of CreateEditorProject, which
// is why the two are separate functions rather than one with a flag.
inline bool SaveEditorProjectAs(const std::string& folderPath, const std::string& name)
{
	std::error_code ec;
	std::filesystem::create_directories(folderPath, ec);
	if (ec)
	{
		GS_WARN("Could not create project folder '{0}': {1}", folderPath, ec.message());
		return false;
	}

	ProjectManifest manifest;
	manifest.Name = name;
	manifest.SceneRelativePath = "scene.txt";

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();
	if (!g_EditorScene.Save(scenePath))
	{
		GS_WARN("Could not save the current scene to '{0}'", scenePath);
		return false;
	}

	if (!WriteProjectManifest(folderPath, manifest))
	{
		GS_WARN("Could not write a project manifest in '{0}'", folderPath);
		return false;
	}

	g_EditorProjectPath = folderPath;
	g_EditorProjectName = name;
	g_EditorScenePath = scenePath;
	return true;
}

inline bool OpenEditorProject(const std::string& folderPath)
{
	ProjectManifest manifest;
	if (!ReadProjectManifest(folderPath, manifest))
	{
		GS_WARN("'{0}' has no readable {1}", folderPath, ProjectManifestFilename());
		return false;
	}

	std::string scenePath = (std::filesystem::path(folderPath) / manifest.SceneRelativePath).string();
	if (!OpenEditorScene(scenePath))
	{
		GS_WARN("Project '{0}' names scene '{1}', which could not be opened", folderPath, scenePath);
		return false;
	}

	g_EditorProjectPath = folderPath;
	g_EditorProjectName = manifest.Name;
	return true;
}

// Writes the current scene back to wherever the open project's manifest
// says it lives. False if no project is open.
inline bool SaveEditorProject()
{
	if (g_EditorProjectPath.empty() || g_EditorScenePath.empty())
		return false;

	return SaveEditorScene(g_EditorScenePath);
}

// Changes only the manifest's name field. The folder and the scene file it
// names are untouched, so a rename never breaks anything that held the old
// path.
inline bool RenameEditorProject(const std::string& newName)
{
	if (g_EditorProjectPath.empty())
		return false;

	ProjectManifest manifest;
	if (!ReadProjectManifest(g_EditorProjectPath, manifest))
		return false;

	manifest.Name = newName;
	if (!WriteProjectManifest(g_EditorProjectPath, manifest))
		return false;

	g_EditorProjectName = newName;
	return true;
}
