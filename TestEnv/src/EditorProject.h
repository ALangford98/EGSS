#pragma once

// The project the editor has open: one GS::Scene, a path (empty if it has
// never been saved), and enough bookkeeping to reopen the same one next run.
//
// Kept separate from EditorShell.h on purpose -- that file lays out panels,
// this one owns what they show. A layout change should never risk the scene.

#include <GS.h>
#include <fstream>

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
