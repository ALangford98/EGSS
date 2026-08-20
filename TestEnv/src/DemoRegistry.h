#pragma once

// The list of demos. This is the only file to touch when adding one.
//
// Name, short name and constructor sit on a single line, so there is no second
// place for them to drift out of step with -- which is what the enum, the
// names array and the PushLayer calls kept doing.
//
// Deliberately *not* self-registering. A `static Registration<T> reg;` in each
// demo header would remove this file, but these demos are header-only: the
// initialiser only runs if some translation unit includes the header, so
// forgetting the include would silently produce no demo, no enum entry, and no
// compile error. Five obvious lines beat one invisible failure.

#include "Demo.h"

#include "Breakout.h"
#include "Cube3D.h"
#include "Physics2D.h"
#include "Physics3D.h"
#include "Lighting2D.h"
#include "SceneDemo.h"
#include "Acoustics2DDemo.h"
#include "Ragdoll.h"
#include "MapBuilding.h"
#include "VoxelTerrain.h"
#include "ModelDemo.h"
#include "OpenWorld.h"
#include "CelShading.h"

struct DemoEntry
{
	const char* Folder;     // the group it files under, in the selector
	const char* Name;       // shown in the dropdown
	const char* ShortName;  // shown on the quick-select buttons
	DemoLayer* (*Create)();
};

// Add a line here and you are done.
//
// **The order of this array is part of the file format.** A recording stores
// the demo's *index* (see `Replay::GetRecordedDemoIndex`), so moving a line
// silently repoints every recording made before the move at a different scene.
// Folders are a field rather than an arrangement for exactly that reason:
// grouping happens in the selector, which reads the folder and leaves the order
// alone. Append new demos at the end.
inline const DemoEntry s_Demos[] =
{
	{ "Engine",   "Breakout (2D, batched quads)", "Breakout",  []() -> DemoLayer* { return new Breakout(); } },
	{ "Engine",   "Cube3D (3D, lit meshes)",      "Cube3D",    []() -> DemoLayer* { return new Cube3D(); } },
	{ "Engine",   "Physics2D (rigid bodies)",     "Physics",   []() -> DemoLayer* { return new Physics2D(); } },
	{ "Engine",   "Physics3D (boxes and spheres)","Physics3D", []() -> DemoLayer* { return new Physics3D(); } },
	{ "Engine",   "Lighting2D (visibility)",      "Lighting",  []() -> DemoLayer* { return new Lighting2D(); } },
	{ "Engine",   "Scene (entities + components)","Scene",     []() -> DemoLayer* { return new SceneDemo(); } },
	{ "Engine",   "Acoustics2D (ray-traced sound)","Acoustics", []() -> DemoLayer* { return new Acoustics2DDemo(); } },
	{ "Engine",   "Ragdoll (jointed humanoid)",   "Ragdoll",   []() -> DemoLayer* { return new Ragdoll(); } },
	{ "Engine",   "Map Building (seeded terrain)","MapBuild",  []() -> DemoLayer* { return new MapBuilding(); } },
	{ "Engine",   "Voxel terrain (dig it out)",   "Voxel",     []() -> DemoLayer* { return new VoxelTerrain(); } },
	{ "Engine",   "Model (glTF hierarchy + textures)", "Model", []() -> DemoLayer* { return new ModelDemo(); } },
	{ "Engine",   "Open World (streaming islands)", "OpenWorld", []() -> DemoLayer* { return new OpenWorld(); } },
	{ "Engine",   "Cel shading (bands + outlines)", "Cel",      []() -> DemoLayer* { return new CelShading(); } }
};

inline constexpr int s_DemoCount = (int)(sizeof(s_Demos) / sizeof(s_Demos[0]));

// The folders, in the order they first appear above -- so a folder's position
// is decided by where its first demo was added, and adding a demo to an
// existing folder never moves anything. Walked rather than declared, because a
// second list of folder names is a second thing to forget to update.
inline int DemoFolderCount()
{
	int count = 0;

	for (int i = 0; i < s_DemoCount; i++)
	{
		bool seen = false;
		for (int j = 0; j < i; j++)
			seen = seen || std::strcmp(s_Demos[i].Folder, s_Demos[j].Folder) == 0;

		if (!seen)
			count++;
	}

	return count;
}

// The `index`-th distinct folder, in first-appearance order.
inline const char* DemoFolder(int index)
{
	int count = 0;

	for (int i = 0; i < s_DemoCount; i++)
	{
		bool seen = false;
		for (int j = 0; j < i; j++)
			seen = seen || std::strcmp(s_Demos[i].Folder, s_Demos[j].Folder) == 0;

		if (seen)
			continue;

		if (count == index)
			return s_Demos[i].Folder;

		count++;
	}

	return "";
}

// `--demo <index|shortname>`, so a run can be pointed at one demo without
// editing g_ActiveDemo and rebuilding.
//
// That dance -- change the default, rebuild, look, revert -- was the standing
// advice for exercising a non-default path, and it is a poor one: the revert
// is easy to forget and the rebuild makes "capture each demo in turn" a chore
// rather than a loop. Pairs with --capture.
inline void SelectDemoFromCommandLine()
{
	const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();

	// A recording knows which scene it belongs to, and it outranks --demo:
	// replaying Breakout's input into Physics2D would produce something, and
	// that something would look like a broken replay rather than a mistake.
	if (Egss::Replay::IsPlaying())
	{
		int recorded = Egss::Replay::GetRecordedDemoIndex();
		if (recorded >= 0 && recorded < s_DemoCount)
		{
			g_ActiveDemo = recorded;
			return;
		}

		EGSS_WARN("Replay names demo {0}, which does not exist here", recorded);
	}

	for (size_t i = 1; i + 1 < arguments.size(); i++)
	{
		if (arguments[i] != "--demo")
			continue;

		const std::string& wanted = arguments[i + 1];

		// A bare number is an index.
		if (!wanted.empty() && std::isdigit((unsigned char)wanted[0]))
		{
			int index = std::atoi(wanted.c_str());
			if (index >= 0 && index < s_DemoCount)
				g_ActiveDemo = index;
			else
				EGSS_WARN("--demo {0} is out of range; {1} demos exist", wanted, s_DemoCount);

			return;
		}

		// Otherwise a short name, case-insensitively -- "physics" should work
		// as well as "Physics".
		auto equalsIgnoringCase = [](const std::string& a, const std::string& b)
		{
			if (a.size() != b.size())
				return false;

			for (size_t c = 0; c < a.size(); c++)
			{
				if (std::tolower((unsigned char)a[c]) != std::tolower((unsigned char)b[c]))
					return false;
			}
			return true;
		};

		for (int d = 0; d < s_DemoCount; d++)
		{
			if (equalsIgnoringCase(wanted, s_Demos[d].ShortName))
			{
				g_ActiveDemo = d;
				return;
			}
		}

		EGSS_WARN("--demo '{0}' matches no demo", wanted);
		return;
	}
}

// Creates every demo, hands each its index, and pushes it. Called once from
// TestApp -- so adding a demo needs no change there either.
inline void PushAllDemos(Egss::Application& app)
{
	SelectDemoFromCommandLine();

	for (int i = 0; i < s_DemoCount; i++)
	{
		DemoLayer* demo = s_Demos[i].Create();
		demo->SetDemoId(i);
		app.PushLayer(demo);
	}

	// Recording starts here rather than in the engine, because the header
	// stamps which scene is being recorded and only the sandbox knows that.
	// After selection, so the number written is the one actually used.
	//
	// And **after the demos are built**, which used not to matter and now does:
	// a recording writes a table of the parameters it carries, and a demo
	// registers those from its constructor. Starting the recording first wrote a
	// file that named no parameters and then recorded none of them -- a valid
	// recording of a session whose sliders did nothing.
	const std::vector<std::string>& arguments = Egss::Application::GetCommandLine();
	for (size_t i = 1; i + 1 < arguments.size(); i++)
	{
		if (arguments[i] == "--record")
		{
			Egss::Replay::StartRecording(arguments[i + 1], g_ActiveDemo, app.GetFixedTimestep());
			break;
		}
	}
}
