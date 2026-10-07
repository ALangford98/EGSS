#pragma once

// Play/Stop for the editor: snapshots g_EditorScene before Play (via the
// same GS::Scene::Save/Load this editor already uses for real project
// files), runs a GameSession over it while playing, and reverts the whole
// scene to its pre-Play snapshot on Stop -- discarding anything the game
// did. The game itself -- physics, scripts, the tick -- is
// Runtime/GameSession.h, shared with GSPlayer; what is here is only what is
// editor-specific. A namespace with inline state, matching EditorHistory.h's
// own pattern for this kind of scene-wide, singleton concern.

#include <GS.h>

#include "EditorProject.h"
#include "GameSession.h"
#include "ScriptEngine.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace PlayMode {

	// One engine for the whole app run: initialising it loads the TypeScript
	// compiler, which is too slow to repeat on every Play.
	inline ScriptEngine s_ScriptEngine;
	inline std::unique_ptr<GameSession> s_Session;

	// Outside g_EditorProjectPath on purpose -- a scratch file, not a
	// project asset, so it never shows up in the file tree.
	inline const char* SnapshotPath() { return "play_snapshot.tmp"; }

	inline bool IsPlaying() { return s_Session != nullptr; }

	// Written fresh (fully overwritten) at the end of every Play() that
	// resolved at least one import -- nothing reads this yet. It exists so
	// the not-yet-built TS-to-C++ transpiler can later know, per script,
	// which module files it was built from (a module edit after a script
	// has been compiled-and-hand-optimized needs the same overwrite-
	// conflict handling a direct script edit would). See
	// docs/superpowers/specs/2026-09-11-script-imports-design.md.
	inline const char* DependencyManifestPath() { return ".gs-dependencies.txt"; }

	inline void WriteDependencyManifest(const std::unordered_map<std::string, std::set<std::string>>& dependencies)
	{
		if (g_EditorProjectPath.empty())
			return;   // a bare --scene session has no project folder to place it in

		std::string path = (std::filesystem::path(g_EditorProjectPath) / DependencyManifestPath()).string();
		std::ofstream out(path);
		if (!out)
		{
			GS_WARN("PlayMode: could not write dependency manifest '{0}'", path);
			return;
		}

		out << "gs-script-deps 1\n";
		for (auto& [scriptPath, modules] : dependencies)
		{
			if (modules.empty())
				continue;
			out << "script " << scriptPath << "\n";
			for (const std::string& module : modules)
				out << "  import " << module << "\n";
		}
	}

	// The transpiler's dependency-manifest fan-out (Task 6): every script
	// this manifest records as depending (transitively) on `modulePath`.
	// Reads the manifest fresh each call rather than caching it -- this is
	// only ever called right after a save, not per-frame, so there's no
	// performance reason to hold it in memory between calls.
	inline std::vector<std::string> FindDependentScripts(const std::string& modulePath)
	{
		std::vector<std::string> dependents;
		if (g_EditorProjectPath.empty())
			return dependents;

		std::string path = (std::filesystem::path(g_EditorProjectPath) / DependencyManifestPath()).string();
		std::ifstream in(path);
		if (!in)
			return dependents;

		std::string line, currentScript;
		while (std::getline(in, line))
		{
			std::istringstream fields(line);
			std::string kind;
			fields >> kind;

			if (kind == "script")
			{
				fields >> currentScript;
			}
			else if (kind == "import" && !currentScript.empty())
			{
				std::string module;
				fields >> module;
				if (module == modulePath)
					dependents.push_back(currentScript);
			}
		}
		return dependents;
	}

	inline void Play()
	{
		if (s_Session)
			return;

		if (!g_EditorScene.Save(SnapshotPath()))
		{
			GS_ERROR("PlayMode::Play: could not write scratch snapshot, not entering play mode");
			return;
		}

		s_Session = std::make_unique<GameSession>(g_EditorScene, s_ScriptEngine);
		GameSession::Dependencies dependencies;
		s_Session->Start(&dependencies);
		WriteDependencyManifest(dependencies);
	}

	inline void Stop()
	{
		if (!s_Session)
			return;

		s_Session->Stop();
		s_Session.reset();

		if (!g_EditorScene.Load(SnapshotPath()))
			GS_ERROR("PlayMode::Stop: failed to revert scene from '{0}' -- scene may be in an unexpected state", SnapshotPath());
	}

	inline void OnFixedUpdate(float dt)
	{
		if (s_Session)
			s_Session->FixedUpdate(dt);
	}

}
