#pragma once

// The project the editor has open, and everything that saves or loads it.
// docs/superpowers/specs/2026-10-13-project-save-load-design.md.
//
// The editor works inside a project: a folder holding project.gsproj, its
// scenes, and the assets they use. One scene of it is open (g_EditorScene).
// Asset paths in a scene are stored relative to the project folder, which
// is GS::Assets' root while the project is open -- the same seam GSPlayer
// resolves through, so a project can be moved or copied whole and the
// editor, the player and the exporter all read it the same way.
//
// The one way to have a scene without a project is `--scene <file>` on the
// command line: a loose scene, kept for the capture harness and automation.
//
// Kept separate from EditorShell.h on purpose -- that file lays out panels,
// this one owns what they show. A layout change should never risk the scene.
//
// Undo history is not reset here (EditorHistory.h includes this file, so
// this one cannot include it): every caller that swaps the scene goes
// through SceneSwapGuard, which resets history and selection.

#include <GS.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>
#include <cctype>

#include "EditorRecentProjects.h"
#include "ProjectManifest.h"
#include "ThemeManager.h"

inline GS::Scene g_EditorScene;

// The open scene's file, absolute. Empty when nothing is open.
inline std::string g_EditorScenePath;

// The open project's folder (absolute, normalised) and display name. Empty
// when no project is open -- the Projects window shows then.
inline std::string g_EditorProjectPath;
inline std::string g_EditorProjectName;

// Every scene the open project knows about (the manifest's sceneentry
// lines). The scene CRUD below keeps it and the manifest in step.
inline std::vector<SceneEntry> g_ProjectScenes;

// Unsaved changes that are not in the undo history: asset paths rewritten
// to the project-relative form when an older project was opened. Cleared
// by a save and by opening another scene.
inline bool g_SceneNeedsSave = false;

// Mirrors the last-scene pattern above: read editor_last_theme.txt (also
// not checked in), and fall back to default.json when it's missing or
// names a file that's gone -- not a warning, since the very first run has
// neither.
inline void LoadEditorThemeFromLastRun()
{
	std::ifstream last(ThemeManager::LastThemePath());
	std::string filename;
	if (last && std::getline(last, filename) && !filename.empty()
		&& std::filesystem::exists(std::filesystem::path(ThemeManager::ThemesFolder()) / filename))
	{
		ThemeManager::ApplyByFilename(filename);
		return;
	}

	ThemeManager::ApplyByFilename("default.json");
}

// --- Paths -----------------------------------------------------------------

inline std::string NormalisedFolder(const std::filesystem::path& folder)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(folder, ec), ec);
	std::string result = absolute.lexically_normal().string();
	while (result.size() > 1 && (result.back() == '/' || result.back() == '\\'))
		result.pop_back();
	return result;
}

// Whether `path` (absolute) lies inside `folder` (absolute), and if so the
// path relative to it.
inline bool RelativeInside(const std::filesystem::path& path, const std::filesystem::path& folder, std::string& relative)
{
	std::filesystem::path rel = path.lexically_normal().lexically_relative(folder.lexically_normal());
	if (rel.empty())
		return false;
	auto first = rel.begin();
	if (first != rel.end() && *first == "..")
		return false;
	relative = rel.generic_string();
	return true;
}

// How a path picked in a file browser (absolute) is stored in the scene:
// relative to the project when it is inside it, absolute when it is not --
// never relative to the editor's working directory, which is the build
// folder and moves with the configuration. A loose scene (no project) keeps
// the working-directory form it always had.
inline std::string StoreAssetPath(const std::string& picked)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::weakly_canonical(std::filesystem::absolute(picked, ec), ec);
	if (!g_EditorProjectPath.empty())
	{
		std::string relative;
		if (RelativeInside(absolute, g_EditorProjectPath, relative))
			return relative;
		// The editor's own assets (assets/materials/...) are beside every
		// build of it, so they keep the editor-relative form -- absolute
		// would name one build folder and break in the others.
		if (RelativeInside(absolute, std::filesystem::current_path(ec), relative))
			return relative;
		return absolute.generic_string();
	}
	std::filesystem::path relative = std::filesystem::relative(absolute, std::filesystem::current_path(ec), ec);
	return (ec || relative.empty()) ? picked : relative.generic_string();
}

// A stored asset path that will not travel with the project: absolute, and
// a project is open. The Inspector says so beside it.
inline bool IsOutsideProject(const std::string& stored)
{
	if (g_EditorProjectPath.empty() || stored.empty() || stored.rfind("primitive:", 0) == 0)
		return false;
	return std::filesystem::path(stored).is_absolute();
}

// Where a stored asset path is on disk, for writing. Assets::Resolve finds
// files that exist; a file about to be created has to be placed by rule:
// a relative path belongs to the open project.
inline std::string AssetDiskPath(const std::string& stored)
{
	if (g_EditorProjectPath.empty() || stored.empty() || std::filesystem::path(stored).is_absolute())
		return stored;
	return (std::filesystem::path(g_EditorProjectPath) / stored).string();
}

// The stored path for an asset the editor writes (an edited mesh, a UV
// template): <project>/<folder>/<name><suffix> in a project -- inside it, so
// it travels with it -- or assets/ beside the editor for a loose scene, as
// before. `keep` is the entity's current path: re-exporting overwrites its
// own earlier file instead of counting up Cube1, Cube2, ...
inline std::string NewAssetPath(const std::string& folder, const std::string& name, const std::string& extension,
	const std::string& keep = "")
{
	std::string prefix = g_EditorProjectPath.empty() ? std::string("assets/") : folder + "/";
	std::string candidate = prefix + name + extension;
	for (int suffix = 1; candidate != keep && std::filesystem::exists(AssetDiskPath(candidate)); suffix++)
		candidate = prefix + name + std::to_string(suffix) + extension;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(AssetDiskPath(candidate)).parent_path(), ec);
	return candidate;
}

// Rewrites one stored path to the project-relative convention. Projects
// saved before it stored paths relative to the editor's working directory
// (bin/<Config>/TestEnv/): such a path does not resolve under the project,
// but does from there. Returns whether it changed.
inline bool MigrateAssetPath(std::string& stored, const std::filesystem::path& projectRoot)
{
	if (stored.empty() || stored.rfind("primitive:", 0) == 0)
		return false;

	std::error_code ec;
	std::filesystem::path path(stored);
	std::filesystem::path cwd = std::filesystem::current_path(ec);

	// A path that climbs out with ".." is never a project-relative one, even
	// when it happens to resolve from the project too -- "/.." is "/", so
	// enough ".." reaches the same file from any folder. The first version
	// of this check kept such paths; the self-test's legacy project caught it.
	bool climbs = false;
	for (const auto& part : path)
		climbs = climbs || part == "..";

	std::filesystem::path absolute;
	if (path.is_absolute())
		absolute = path;
	else if (!climbs && std::filesystem::exists(projectRoot / path, ec))
		return false;   // already project-relative
	else if (std::filesystem::exists(cwd / path, ec))
		absolute = std::filesystem::weakly_canonical(cwd / path, ec);   // the old, editor-relative convention
	else if (std::filesystem::exists(projectRoot / path, ec))
		absolute = std::filesystem::weakly_canonical(projectRoot / path, ec);
	else
		return false;   // missing either way: left as it is, and the Inspector shows it broken

	std::string relative;
	std::string updated;
	if (RelativeInside(absolute, projectRoot, relative))
		updated = relative;
	else if (!path.is_absolute() && RelativeInside(absolute, std::filesystem::current_path(ec), relative))
		return false;   // one of the editor's own assets: editor-relative is right (see StoreAssetPath)
	else
		updated = absolute.generic_string();
	if (updated == stored)
		return false;
	stored = updated;
	return true;
}

inline int MigrateScenePaths(GS::Scene& scene, const std::filesystem::path& projectRoot)
{
	int changed = 0;
	for (GS::EntityId id : scene.GetEntities())
	{
		if (auto* script = scene.GetComponent<GS::ScriptComponent>(id))
			changed += MigrateAssetPath(script->ScriptPath, projectRoot);
		if (auto* mesh = scene.GetComponent<GS::MeshComponent>(id))
		{
			changed += MigrateAssetPath(mesh->SourcePath, projectRoot);
			changed += MigrateAssetPath(mesh->MaterialPath, projectRoot);
		}
	}
	return changed;
}

// --- Manifest --------------------------------------------------------------

// The manifest types and ReadProjectManifest live in Runtime/ProjectManifest.h,
// shared with GSPlayer; writing one is editor-only and stays here.
inline bool WriteProjectManifest(const std::string& folderPath, const ProjectManifest& manifest)
{
	std::filesystem::path manifestPath = std::filesystem::path(folderPath) / ProjectManifestFilename();

	// Callers build the manifest from editor state, which has no icon in it;
	// carry forward the one already on disk rather than silently drop it.
	std::string icon = manifest.Icon;
	if (icon.empty())
	{
		ProjectManifest existing;
		if (ReadProjectManifest(folderPath, existing))
			icon = existing.Icon;
	}

	std::ofstream out(manifestPath.string());
	if (!out)
		return false;

	out << "gs-project 1\n";
	out << "name " << manifest.Name << "\n";
	out << "scene " << manifest.SceneRelativePath << "\n";
	for (const SceneEntry& entry : manifest.Scenes)
		out << "sceneentry " << entry.Name << " " << entry.RelativePath << "\n";
	if (!icon.empty())
		out << "icon " << icon << "\n";
	return true;
}

// Rewrites project.gsproj from g_ProjectScenes, naming the open scene as
// the one the project opens with -- derived here rather than stored
// separately, so the two can never drift apart.
inline void PersistProjectScenes()
{
	if (g_EditorProjectPath.empty())
		return;

	ProjectManifest manifest;
	manifest.Name = g_EditorProjectName;
	manifest.Scenes = g_ProjectScenes;

	for (const SceneEntry& entry : g_ProjectScenes)
	{
		std::string joined = (std::filesystem::path(g_EditorProjectPath) / entry.RelativePath).lexically_normal().string();
		if (joined == g_EditorScenePath)
		{
			manifest.SceneRelativePath = entry.RelativePath;
			break;
		}
	}
	if (manifest.SceneRelativePath.empty() && !g_ProjectScenes.empty())
		manifest.SceneRelativePath = g_ProjectScenes.front().RelativePath;

	if (!WriteProjectManifest(g_EditorProjectPath, manifest))
		GS_WARN("Could not write the project manifest in '{0}'", g_EditorProjectPath);
}

// --- Opening and closing ---------------------------------------------------

// Loads one scene file as the open scene, and migrates its asset paths when
// a project is open. Not itself a project operation; the ones below are.
inline bool LoadSceneFile(const std::string& path, std::string& error)
{
	if (!std::filesystem::is_regular_file(path))
	{
		error = "Scene file '" + path + "' does not exist.";
		return false;
	}
	if (!g_EditorScene.Load(path))
	{
		error = "'" + path + "' is not a readable GS scene.";
		g_EditorScenePath.clear();
		return false;
	}

	g_EditorScenePath = std::filesystem::path(path).lexically_normal().string();
	g_SceneNeedsSave = false;

	if (!g_EditorProjectPath.empty())
	{
		int migrated = MigrateScenePaths(g_EditorScene, g_EditorProjectPath);
		if (migrated > 0)
		{
			GS_INFO("Rewrote {0} asset path(s) in '{1}' to be relative to the project -- save the scene to keep them",
				migrated, path);
			g_SceneNeedsSave = true;
		}
	}
	return true;
}

inline void CloseEditorProject()
{
	g_EditorScene.Clear();
	g_EditorScenePath.clear();
	g_EditorProjectPath.clear();
	g_EditorProjectName.clear();
	g_ProjectScenes.clear();
	g_SceneNeedsSave = false;
	GS::Assets::SetRoot("");
}

// `path` is the project folder or its project.gsproj.
inline bool OpenEditorProject(const std::string& path, std::string& error)
{
	std::error_code ec;
	std::filesystem::path folder(path);
	if (folder.filename() == ProjectManifestFilename())
		folder = folder.parent_path();
	std::string normalised = NormalisedFolder(folder);

	if (!std::filesystem::is_regular_file(std::filesystem::path(normalised) / ProjectManifestFilename(), ec))
	{
		error = "There is no " + std::string(ProjectManifestFilename()) + " in '" + normalised + "'.";
		return false;
	}
	ProjectManifest manifest;
	if (!ReadProjectManifest(normalised, manifest))
	{
		error = "'" + normalised + "/" + ProjectManifestFilename() + "' is not a GS project file, or names no scene.";
		return false;
	}

	// Checked before anything is replaced: a project that cannot open must
	// not cost the one already open.
	std::string scenePath = (std::filesystem::path(normalised) / manifest.SceneRelativePath).lexically_normal().string();
	if (!std::filesystem::is_regular_file(scenePath, ec))
	{
		error = "The project's scene '" + manifest.SceneRelativePath + "' is missing.";
		return false;
	}

	CloseEditorProject();
	g_EditorProjectPath = normalised;
	g_EditorProjectName = manifest.Name.empty() ? folder.filename().string() : manifest.Name;
	g_ProjectScenes = manifest.Scenes;
	GS::Assets::SetRoot(normalised);

	if (!LoadSceneFile(scenePath, error))
	{
		CloseEditorProject();
		return false;
	}

	RecentProjects::Touch(normalised);
	GS_INFO("Opened project '{0}' ({1}), scene {2}", g_EditorProjectName, normalised, manifest.SceneRelativePath);
	return true;
}

// A scene file with no project around it: --scene, for automation.
inline bool OpenLooseScene(const std::string& path, std::string& error)
{
	CloseEditorProject();
	return LoadSceneFile(path, error);
}

// A project name that is also a folder name.
inline bool IsValidProjectName(const std::string& name, std::string& error)
{
	if (name.empty() || name.find_first_not_of(' ') == std::string::npos)
		error = "Give the project a name.";
	else if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		error = "A project name cannot contain '/' or '\\'.";
	else if (name == "." || name == ".." || name.front() == ' ' || name.back() == ' ')
		error = "A project name cannot be '.' or '..', or start or end with a space.";
	else
		return true;
	return false;
}

// A folder a project may be created in: absent, or present and empty.
inline bool IsFreeFolder(const std::filesystem::path& folder, std::string& error)
{
	std::error_code ec;
	if (!std::filesystem::exists(folder, ec))
		return true;
	if (!std::filesystem::is_directory(folder, ec))
	{
		error = "'" + folder.string() + "' exists and is a file.";
		return false;
	}
	if (!std::filesystem::is_empty(folder, ec))
	{
		error = "'" + folder.string() + "' already exists and is not empty.";
		return false;
	}
	return true;
}

inline std::filesystem::path NewProjectFolder(const std::string& location, const std::string& name)
{
	return std::filesystem::path(location) / name;
}

// A new project in <location>/<name>/: the folder, scenes/Main.txt (empty)
// and the manifest. Opened once it exists.
inline bool CreateEditorProject(const std::string& location, const std::string& name, std::string& error)
{
	if (!IsValidProjectName(name, error))
		return false;
	if (location.empty())
	{
		error = "Choose where the project folder should go.";
		return false;
	}
	std::filesystem::path folder = NewProjectFolder(location, name);
	if (!IsFreeFolder(folder, error))
		return false;

	std::error_code ec;
	std::filesystem::create_directories(folder / "scenes", ec);
	if (ec)
	{
		error = "Could not create '" + folder.string() + "': " + ec.message();
		return false;
	}

	ProjectManifest manifest;
	manifest.Name = name;
	manifest.SceneRelativePath = "scenes/Main.txt";
	manifest.Scenes = { { "Main", "scenes/Main.txt" } };

	GS::Scene blank;
	if (!blank.Save((folder / manifest.SceneRelativePath).string()) || !WriteProjectManifest(folder.string(), manifest))
	{
		error = "Could not write the new project's files in '" + folder.string() + "'.";
		return false;
	}
	return OpenEditorProject(folder.string(), error);
}

// --- Saving ----------------------------------------------------------------

// The open scene back to its file, and the manifest naming it as the
// project's opening scene.
inline bool SaveEditorScene(std::string& error)
{
	if (g_EditorScenePath.empty())
	{
		error = "No scene is open.";
		return false;
	}
	if (!g_EditorScene.Save(g_EditorScenePath))
	{
		error = "Could not write '" + g_EditorScenePath + "'.";
		return false;
	}
	PersistProjectScenes();
	g_SceneNeedsSave = false;
	return true;
}

// Changes only the manifest's name field. The folder and the scene file it
// names are untouched, so a rename never breaks anything that held the old
// path.
inline bool RenameEditorProject(const std::string& newName, std::string& error)
{
	if (g_EditorProjectPath.empty())
	{
		error = "No project is open.";
		return false;
	}
	if (newName.empty())
	{
		error = "Give the project a name.";
		return false;
	}
	g_EditorProjectName = newName;
	PersistProjectScenes();
	return true;
}

// Copies the open project's folder, as saved, to <location>/<name>/,
// renames it there and opens the copy. Unsaved changes must be dealt with
// first (the menu routes this through the unsaved-changes guard).
inline bool DuplicateEditorProject(const std::string& location, const std::string& name, std::string& error)
{
	if (g_EditorProjectPath.empty())
	{
		error = "No project is open.";
		return false;
	}
	if (!IsValidProjectName(name, error))
		return false;
	std::filesystem::path destination = std::filesystem::path(NormalisedFolder(location)) / name;
	std::string inside;
	if (destination.string() == g_EditorProjectPath || RelativeInside(destination, g_EditorProjectPath, inside))
	{
		error = "The copy cannot go inside the project it copies.";
		return false;
	}
	if (!IsFreeFolder(destination, error))
		return false;

	std::error_code ec;
	std::filesystem::create_directories(destination, ec);
	std::filesystem::copy(g_EditorProjectPath, destination, std::filesystem::copy_options::recursive, ec);
	if (ec)
	{
		error = "Could not copy the project to '" + destination.string() + "': " + ec.message();
		return false;
	}

	// Scenes of an older project hold paths relative to the editor's
	// working directory that point into the *original*: rewritten here,
	// against the original, to the project-relative form -- which then
	// means the copy's own files.
	std::string source = g_EditorProjectPath;
	ProjectManifest manifest;
	ReadProjectManifest(destination.string(), manifest);
	for (const SceneEntry& entry : manifest.Scenes)
	{
		GS::Scene scene;
		std::string path = (destination / entry.RelativePath).string();
		if (scene.Load(path) && MigrateScenePaths(scene, source) > 0)
			scene.Save(path);
	}
	manifest.Name = name;
	WriteProjectManifest(destination.string(), manifest);

	return OpenEditorProject(destination.string(), error);
}

// --- Scenes within the project ----------------------------------------------

// Non-empty, no spaces (the manifest's fields are space-separated), no path
// separators, and not already used by another scene in this project
// (`ignoreExisting` lets a rename to the name it already has succeed).
inline bool IsValidSceneName(const std::string& name, const std::string& ignoreExisting = "")
{
	if (name.empty())
		return false;
	for (char c : name)
		if (std::isspace((unsigned char)c) || c == '/' || c == '\\')
			return false;
	for (const SceneEntry& entry : g_ProjectScenes)
		if (entry.Name == name && entry.Name != ignoreExisting)
			return false;
	return true;
}

inline std::string SceneFileFor(const std::string& name)
{
	return "scenes/" + name + ".txt";
}

inline const SceneEntry* FindProjectScene(const std::string& name)
{
	for (const SceneEntry& entry : g_ProjectScenes)
		if (entry.Name == name)
			return &entry;
	return nullptr;
}

inline std::string ProjectScenePath(const SceneEntry& entry)
{
	return (std::filesystem::path(g_EditorProjectPath) / entry.RelativePath).lexically_normal().string();
}

// Makes another of the project's scenes the open one, and the one the
// project opens with next time.
inline bool OpenProjectScene(const std::string& name, std::string& error)
{
	const SceneEntry* entry = FindProjectScene(name);
	if (!entry)
	{
		error = "The project has no scene named '" + name + "'.";
		return false;
	}
	if (!LoadSceneFile(ProjectScenePath(*entry), error))
		return false;
	PersistProjectScenes();
	return true;
}

inline bool CreateSceneInProject(const std::string& name, std::string& error)
{
	if (g_EditorProjectPath.empty())
	{
		error = "No project is open.";
		return false;
	}
	if (!IsValidSceneName(name))
	{
		error = "Scene names need no spaces or slashes, and must not repeat one the project has.";
		return false;
	}
	std::filesystem::path path = std::filesystem::path(g_EditorProjectPath) / SceneFileFor(name);
	if (std::filesystem::exists(path))
	{
		error = "'" + path.string() + "' already exists.";
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	GS::Scene blank;
	if (!blank.Save(path.string()))
	{
		error = "Could not write '" + path.string() + "'.";
		return false;
	}
	g_ProjectScenes.push_back({ name, SceneFileFor(name) });
	PersistProjectScenes();
	return true;
}

// The open scene, as it is now, written as a new scene of the project --
// which becomes the open one. The scene it came from keeps what it had on
// disk.
inline bool SaveSceneAs(const std::string& name, std::string& error)
{
	if (g_EditorProjectPath.empty())
	{
		error = "No project is open.";
		return false;
	}
	if (!IsValidSceneName(name))
	{
		error = "Scene names need no spaces or slashes, and must not repeat one the project has.";
		return false;
	}
	std::filesystem::path path = std::filesystem::path(g_EditorProjectPath) / SceneFileFor(name);
	if (std::filesystem::exists(path))
	{
		error = "'" + path.string() + "' already exists.";
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (!g_EditorScene.Save(path.string()))
	{
		error = "Could not write '" + path.string() + "'.";
		return false;
	}
	g_ProjectScenes.push_back({ name, SceneFileFor(name) });
	g_EditorScenePath = path.lexically_normal().string();
	g_SceneNeedsSave = false;
	PersistProjectScenes();
	return true;
}

inline bool DuplicateSceneInProject(const std::string& sourceName, const std::string& newName, std::string& error)
{
	const SceneEntry* source = FindProjectScene(sourceName);
	if (g_EditorProjectPath.empty() || !source)
	{
		error = "No such scene.";
		return false;
	}
	if (!IsValidSceneName(newName))
	{
		error = "Scene names need no spaces or slashes, and must not repeat one the project has.";
		return false;
	}
	std::filesystem::path destination = std::filesystem::path(g_EditorProjectPath) / SceneFileFor(newName);
	std::error_code ec;
	std::filesystem::create_directories(destination.parent_path(), ec);
	std::filesystem::copy_file(ProjectScenePath(*source), destination, ec);
	if (ec)
	{
		error = "Could not copy the scene: " + ec.message();
		return false;
	}
	g_ProjectScenes.push_back({ newName, SceneFileFor(newName) });
	PersistProjectScenes();
	return true;
}

// The name only; the file keeps its path, so nothing that refers to it
// breaks.
inline bool RenameSceneInProject(const std::string& oldName, const std::string& newName, std::string& error)
{
	if (!IsValidSceneName(newName, oldName))
	{
		error = "Scene names need no spaces or slashes, and must not repeat one the project has.";
		return false;
	}
	for (SceneEntry& entry : g_ProjectScenes)
		if (entry.Name == oldName)
		{
			entry.Name = newName;
			PersistProjectScenes();
			return true;
		}
	error = "No such scene.";
	return false;
}

// Refuses the open scene (switch away first) and the project's last
// remaining scene (a project with zero scenes has nothing to open).
inline bool DeleteSceneFromProject(const std::string& name, std::string& error)
{
	for (size_t i = 0; i < g_ProjectScenes.size(); i++)
	{
		if (g_ProjectScenes[i].Name != name)
			continue;
		std::string path = ProjectScenePath(g_ProjectScenes[i]);
		if (path == g_EditorScenePath || g_ProjectScenes.size() <= 1)
		{
			error = "The open scene and a project's only scene cannot be deleted.";
			return false;
		}
		std::error_code ec;
		std::filesystem::remove(path, ec);
		g_ProjectScenes.erase(g_ProjectScenes.begin() + i);
		PersistProjectScenes();
		return true;
	}
	error = "No such scene.";
	return false;
}

// A scene name not yet used in the project: `base`, or base_2, base_3...
inline std::string UniqueSceneName(std::string base)
{
	for (char& c : base)
		if (std::isspace((unsigned char)c) || c == '/' || c == '\\')
			c = '_';
	std::string name = base;
	for (int i = 2; !IsValidSceneName(name) || std::filesystem::exists(std::filesystem::path(g_EditorProjectPath) / SceneFileFor(name)); i++)
		name = base + "_" + std::to_string(i);
	return name;
}

// --- Startup ---------------------------------------------------------------

// --project <folder> opens that project; --scene <file> a loose scene (for
// automation); otherwise the most recent project, if it still exists. With
// none of those the Projects window shows. --demo/--scene are mutually
// exclusive by construction: SelectDemoFromCommandLine (DemoRegistry.h)
// only moves g_ActiveDemo when --demo is given.
inline void LoadEditorProjectFromCommandLine()
{
	const std::vector<std::string>& arguments = GS::Application::GetCommandLine();
	std::string error;

	for (size_t i = 1; i + 1 < arguments.size(); i++)
		if (arguments[i] == "--project")
		{
			if (!OpenEditorProject(arguments[i + 1], error))
				GS_WARN("--project: {0}", error);
			return;
		}

	for (size_t i = 1; i + 1 < arguments.size(); i++)
		if (arguments[i] == "--scene")
		{
			if (!OpenLooseScene(arguments[i + 1], error))
				GS_WARN("--scene: {0} Starting blank.", error);
			return;
		}

	std::vector<std::string> recent = RecentProjects::Load();
	if (!recent.empty() && !OpenEditorProject(recent.front(), error))
		GS_INFO("Last project not reopened: {0}", error);
}
