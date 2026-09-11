#pragma once

// Play/Stop for the editor. Snapshots g_EditorScene before Play (via the
// same GS::Scene::Save/Load this editor already uses for real project
// files), prepares every ScriptComponent'd entity's script through
// ScriptEngine, runs OnUpdate from OnFixedUpdate while playing, and
// reverts the whole scene back to its pre-Play snapshot on Stop --
// discarding anything the scripts did. A namespace with inline state,
// matching EditorHistory.h's own established pattern for exactly this
// kind of scene-wide, singleton concern.

#include <GS.h>

#include "EditorProject.h"
#include "ScriptEngine.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace PlayMode {

	inline bool s_Playing = false;
	inline ScriptEngine s_ScriptEngine;
	inline std::unordered_map<GS::EntityId, ScriptEngine::PreparedScript> s_Prepared;

	// Outside g_EditorProjectPath on purpose -- a scratch file, not a
	// project asset, so it never shows up in the file tree.
	inline const char* SnapshotPath() { return "play_snapshot.tmp"; }

	inline bool IsPlaying() { return s_Playing; }

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

	inline void Play()
	{
		if (s_Playing)
			return;

		if (!g_EditorScene.Save(SnapshotPath()))
		{
			GS_ERROR("PlayMode::Play: could not write scratch snapshot, not entering play mode");
			return;
		}

		// Scoped to this one session, not s_ScriptEngine's lifetime (which
		// spans every Play/Stop cycle for the whole app run) -- otherwise
		// editing a module file and pressing Play again would keep running
		// the stale cached copy forever.
		s_ScriptEngine.ClearModuleCache();
		std::unordered_map<std::string, std::set<std::string>> allDependencies;

		for (GS::EntityId id : g_EditorScene.GetEntities())
		{
			if (!g_EditorScene.HasComponent<GS::ScriptComponent>(id))
				continue;

			auto* script = g_EditorScene.GetComponent<GS::ScriptComponent>(id);
			if (script->ScriptPath.empty())
				continue;

			std::ifstream file(script->ScriptPath, std::ios::in | std::ios::binary);
			if (!file.is_open())
			{
				GS_ERROR("PlayMode::Play: could not open script '{0}' for entity {1}", script->ScriptPath, id);
				continue;
			}
			std::stringstream buffer;
			buffer << file.rdbuf();

			ScriptEngine::PreparedScript prepared;
			std::string error;
			std::set<std::string> dependencies;
			if (!s_ScriptEngine.PrepareEntityScript(&g_EditorScene, id, script->ScriptPath, buffer.str(), error, prepared, &dependencies))
			{
				GS_ERROR("PlayMode::Play: entity {0}'s script failed to prepare: {1}", id, error);
				continue;
			}
			if (!dependencies.empty())
				allDependencies[script->ScriptPath] = dependencies;

			s_ScriptEngine.CallOnStart(prepared);
			s_Prepared[id] = prepared;
		}

		WriteDependencyManifest(allDependencies);

		s_Playing = true;
	}

	inline void Stop()
	{
		if (!s_Playing)
			return;

		for (auto& pair : s_Prepared)
			s_ScriptEngine.ReleasePreparedScript(pair.second);
		s_Prepared.clear();

		if (!g_EditorScene.Load(SnapshotPath()))
			GS_ERROR("PlayMode::Stop: failed to revert scene from '{0}' -- scene may be in an unexpected state", SnapshotPath());

		s_Playing = false;
	}

	inline void OnFixedUpdate(float dt)
	{
		if (!s_Playing)
			return;

		// scene.destroy() (ScriptEngine.h) can remove any entity, including
		// one with its own running script -- self-destruction, or another
		// entity destroying it (a ball clearing the brick it just hit).
		// Skip ticking one whose id is no longer valid, and drop it from
		// s_Prepared (releasing its JSValues) so it isn't checked forever.
		// Checked before each call rather than only once up front, so an
		// entity destroyed earlier in *this* tick doesn't also get ticked
		// once more before its removal is noticed.
		std::vector<GS::EntityId> stale;
		for (auto& pair : s_Prepared)
		{
			if (!g_EditorScene.IsValid(pair.first))
			{
				stale.push_back(pair.first);
				continue;
			}
			s_ScriptEngine.CallOnUpdate(pair.second, dt);
		}
		for (GS::EntityId id : stale)
		{
			s_ScriptEngine.ReleasePreparedScript(s_Prepared[id]);
			s_Prepared.erase(id);
		}
	}

}
