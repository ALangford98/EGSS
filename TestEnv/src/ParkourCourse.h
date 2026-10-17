#pragma once

// The parkour course: the crash test's dummy on his own, in a yard with a
// station for each thing he can do -- two rows of lanes either side of a
// walkway, each lane one station, its sign at the walkway end:
//
//    north (+z)   1 walking    2 vaults     3 climbing   4 crates      5 hanging      6 drops         13 slopes
//    south (-z)   7 wall run   8 corner     9 tic-tac   10 cat leap   11 precision   12 knock-downs  14 dodging
//
// You are him from the start (E lets go and flies the camera; walk up to
// him and E takes him again). The panel at the top right says what to do
// in the station he is in; `station 7` in the terminal (`) puts him at the
// start of station 7, and `station` lists them.
//
// Like the crash test it is a scene rather than code of its own --
// everything that happens is the runtime's (GameSession, GameModes, the
// humanoid, the launcher) -- so it opens in the editor too (File > Add
// Demo Scene). What only this layer adds is the panel, `station`, and
// `ghost`: a recorded person (TestEnv/assets/mocap, MotionClip.h) walking,
// running or turning beside him on his own bodies, to see where he is wrong.
//
// Each lane is laid out in its own frame (along it, across it), so a
// station is a list of blocks at distances from its start mark. The
// numbers are the rig's own: a vault is a wall up to 1.15 m and no deeper
// than 0.75, a step on is up to 0.8, a climb up to 2.6, a hang needs an
// edge out of reach standing (2.35 here), a wall run a wall taller than he
// is within a metre of him (Humanoid::Sense, SenseSideWall).

#include <GS.h>
#include <imgui.h>

#include "CrashTest.h"
#include "Gunnery.h"
#include "MotionClip.h"

#include <cctype>
#include <string>
#include <vector>

namespace ParkourCourseScene
{
	using CrashTestScene::Box;
	using CrashTestScene::Mesh;
	using CrashTestScene::Static;

	struct Station
	{
		const char* Name;      // on its sign and the panel
		const char* Word;      // what `station` answers to besides its number
		const char* How[3];    // what to do there, keys as {keys.<action>}
		glm::vec3 Start;       // the mark at the start of its lane
		float Yaw;             // degrees: the lane runs along (sin, 0, cos)
	};

	// Lanes 7 m wide, their marks either side of the walkway (z = 0): the
	// north row runs +z from z = 3, the south row -z from z = -3.
	inline const float LaneHalfWidth = 3.5f;
	inline const float LaneLength = 19.0f;

	inline const std::vector<Station>& Stations()
	{
		static const std::vector<Station> stations = []()
		{
			const float xs[7] = { -17.5f, -10.5f, -3.5f, 3.5f, 10.5f, 17.5f, 24.5f };
			auto north = [&](int i) { return glm::vec3(xs[i], 0.0f, 3.0f); };
			auto south = [&](int i) { return glm::vec3(xs[i], 0.0f, -3.0f); };
			return std::vector<Station>{
				{ "WALKING", "walk", { "{keys.forward}{keys.left}{keys.back}{keys.right} walk, hold {keys.sprint} to run",
					"a kerb, stairs up and down, a big step:", "the walk takes them; a wall stops him" }, north(0), 0.0f },
				{ "VAULTS", "vault", { "walk up and {keys.jump}: over on both hands",
					"run at it ({keys.sprint}): over by itself, fast one-handed", "two walls in a row: one vault into the next" }, north(1), 0.0f },
				{ "CLIMBING", "climb", { "{keys.jump} at the box: a foot up and on",
					"{keys.jump} at the ledge: climb it", "the high wall: jump, catch, hang; {keys.forward} pulls up" }, north(2), 0.0f },
				{ "CRATES", "crates", { "loose crates, heavy enough to stand on:",
					"{keys.jump} at one to climb or step on it", "running at the small ones vaults them" }, north(3), 0.0f },
				{ "HANGING", "hang", { "{keys.jump} at the high ledge: hang from it",
					"{keys.left} {keys.right} along it, round its corners too", "{keys.forward} or {keys.jump} climbs, {keys.back} lets go" }, north(4), 0.0f },
				{ "DROPS", "drop", { "climb up ({keys.jump}) to the top of the tower",
					"walk off the far edge: down into a hang", "run off it: drop, and roll out of it" }, north(5), 0.0f },
				{ "WALL RUN", "wallrun", { "run beside the wall ({keys.sprint}) and {keys.jump}:",
					"along the wall on his feet", "{keys.jump} again kicks off it" }, south(0), 180.0f },
				{ "CORNER", "corner", { "run along the wall and {keys.jump} near its end:",
					"round the inside corner", "and on along the next wall" }, south(1), 180.0f },
				{ "TIC-TAC", "tictac", { "run at the slanted wall ({keys.sprint})",
					"and {keys.jump} at it: a step off it", "and away the other way" }, south(2), 180.0f },
				{ "CAT LEAP", "catleap", { "run at the high wall ({keys.sprint})",
					"{keys.jump} at the line, two metres short:", "a leap to the edge, caught in a hang" }, south(3), 180.0f },
				{ "PRECISION", "precision", { "climb the block, stand at its edge and {keys.jump}",
					"(not running): a two-footed jump, top to top", "three pillars, then the landing" }, south(4), 180.0f },
				{ "KNOCK-DOWNS", "knock", { "{keys.fire} fires the cannon at him",
					"face it: down on his back; turn away: on his front", "he gets up by himself" }, south(5), 180.0f },
				// Added after the twelve, at the east end of each row, so
				// the numbers people know stayed where they were.
				{ "SLOPES", "slope", { "up the ramp, walking or running ({keys.sprint})",
					"and down the far side: his feet follow the slope", "the last is 30 degrees: steep, but a walk" }, north(6), 0.0f },
				{ "DODGING", "dodge", { "the cannon hisses and blinks, then fires: watch it",
					"{keys.dodge} with a direction: a sidestep, a cut or a dive", "hold {keys.focus} to look at what is coming" }, south(6), 180.0f },
			};
		}();
		return stations;
	}

	// A lane's frame: along it from the start mark, and across it to the
	// right of someone on the mark looking down it.
	struct Lane
	{
		glm::vec3 O{ 0.0f };
		float Yaw = 0.0f;
		glm::vec3 F{ 0.0f, 0.0f, 1.0f }, R{ -1.0f, 0.0f, 0.0f };
		explicit Lane(const Station& s) : O(s.Start), Yaw(s.Yaw)
		{
			float y = glm::radians(s.Yaw);
			F = glm::vec3(std::sin(y), 0.0f, std::cos(y));
			R = glm::cross(F, glm::vec3(0.0f, 1.0f, 0.0f));
		}
		glm::vec3 At(float along, float across, float y) const { return glm::vec3(O.x, y, O.z) + F * along + R * across; }
	};

	// Which station a point is in, or -1 on the walkway.
	inline int StationAt(const glm::vec3& p)
	{
		const std::vector<Station>& stations = Stations();
		for (size_t i = 0; i < stations.size(); i++)
		{
			Lane lane(stations[i]);
			glm::vec3 d = p - lane.O;
			float along = glm::dot(d, lane.F), across = glm::dot(d, lane.R);
			if (along >= -0.6f && along <= LaneLength && std::fabs(across) <= LaneHalfWidth)
				return (int)i;
		}
		return -1;
	}

	inline const glm::vec4 Concrete(0.62f, 0.6f, 0.56f, 1.0f), Painted(0.85f, 0.55f, 0.15f, 1.0f),
		WallGrey(0.55f, 0.57f, 0.6f, 1.0f), Darker(0.48f, 0.5f, 0.53f, 1.0f);

	// A fixed block in a lane: `from` to `to` along it, `width` across it
	// centred on `across`, up to `top`.
	inline GS::Entity Block(GS::Scene& scene, const char* name, const Lane& lane, float from, float to, float across, float width,
		float top, const glm::vec4& colour = Concrete)
	{
		GS::Entity entity = Box(scene, name, lane.At(0.5f * (from + to), across, 0.5f * top), { width, top, to - from }, colour, 0.7f);
		entity.Get<GS::TransformComponent>()->Rotation = { 0.0f, lane.Yaw, 0.0f };
		Static(entity);
		return entity;
	}

	// A ramp: a plank `thickness` deep whose top runs from height `h0` at
	// `from` to `h1` at `to` along the lane, `width` across it. Turned
	// first about the lane's across axis and then to the lane's yaw, which
	// the transform's X-then-Y-then-Z angles can only say for a lane along
	// an axis -- so the angles are read back off the turn itself, as
	// GameSession writes a body's.
	inline GS::Entity Ramp(GS::Scene& scene, const char* name, const Lane& lane, float from, float to, float h0, float h1,
		float across, float width, float thickness = 0.4f, const glm::vec4& colour = Concrete)
	{
		float run = to - from, rise = h1 - h0;
		float slope = std::atan2(rise, run);
		float length = std::sqrt(run * run + rise * rise);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 normal = up * std::cos(slope) - lane.F * std::sin(slope);
		glm::vec3 centre = lane.At(0.5f * (from + to), across, 0.5f * (h0 + h1)) - normal * (0.5f * thickness);
		GS::Entity entity = Box(scene, name, centre, { width, thickness, length }, colour, 0.7f);
		glm::quat turn = glm::angleAxis(glm::radians(lane.Yaw), up) * glm::angleAxis(-slope, glm::vec3(1.0f, 0.0f, 0.0f));
		float x, y, z;
		glm::extractEulerAngleXYZ(glm::mat4_cast(turn), x, y, z);
		entity.Get<GS::TransformComponent>()->Rotation = glm::degrees(glm::vec3(x, y, z));
		Static(entity);
		return entity;
	}

	// Paint on the ground: drawn, not collided with, and too thin to be a
	// step if it were.
	inline void Paint(GS::Scene& scene, const char* name, const Lane& lane, float from, float to, float across, float width,
		const glm::vec4& colour)
	{
		GS::Entity entity = Box(scene, name, lane.At(0.5f * (from + to), across, 0.003f), { width, 0.006f, to - from }, colour, 0.85f);
		entity.Get<GS::TransformComponent>()->Rotation = { 0.0f, lane.Yaw, 0.0f };
	}

	// A crate, loose: the crash test's, at `size` metres and `mass` kg.
	inline void Crate(GS::Scene& scene, const glm::vec3& at, float size, float mass, float yaw)
	{
		GS::Entity crate = Mesh(scene, "Crate", "model:crate", at + glm::vec3(0.0f, 0.5f * size, 0.0f), { 0.0f, yaw, 0.0f }, glm::vec3(size));
		GS::PhysicsComponent body;
		body.Mass = mass;
		body.Friction = 0.7f;
		crate.Add<GS::PhysicsComponent>(body);
	}

	// Station `index`'s sign, a .gsui screen like any HUD: its number, its
	// name, what to do. The keys are the project's (`keys.jump` is whatever
	// Jump is bound to), filled in when it is drawn.
	inline std::string SignScreen(int index)
	{
		const Station& s = Stations()[(size_t)index];
		std::string text = R"json({
			"theme": { "accent": "#ffc21a", "text": "#f2f4f6", "dim": "#b4bcc4" },
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "panel", "stretch": "both", "pos": [14, 14], "padding": 0 },
				{ "type": "panel", "anchor": "center", "layout": "vertical", "style": "clear", "padding": 0, "gap": 6, "children": [
					{ "type": "label", "text": "@N", "style": "accent", "fontSize": 250, "align": "center" },
					{ "type": "label", "text": "@NAME", "style": "big", "fontSize": 78, "align": "center" },
					{ "type": "label", "text": "@0", "fontSize": 68, "align": "center" },
					{ "type": "label", "text": "@1", "fontSize": 68, "align": "center" },
					{ "type": "label", "text": "@2", "style": "dim", "fontSize": 68, "align": "center" } ] }
			] } })json";
		auto put = [&](const std::string& key, const std::string& value)
		{
			size_t at = text.find(key);
			if (at != std::string::npos)
				text.replace(at, key.size(), value);
		};
		put("@NAME", s.Name);
		put("@N", std::to_string(index + 1));
		for (int k = 0; k < 3; k++)
			put("@" + std::to_string(k), s.How[k]);
		return text;
	}

	inline std::string SignName(int index) { return "course-sign-" + std::to_string(index + 1); }

	// The panel at the top right: the station he is in, or how to find one.
	inline const char* CourseScreen()
	{
		return R"json({
			"root": { "type": "panel", "style": "clear", "stretch": "both", "padding": 0, "children": [
				{ "type": "panel", "anchor": "top-right", "pos": [28, 28], "layout": "vertical", "style": "accent",
				  "padding": 14, "gap": 3, "visible": "course.in", "children": [
					{ "type": "label", "text": "{course.title}", "style": "title" },
					{ "type": "label", "text": "{course.how0}" },
					{ "type": "label", "text": "{course.how1}" },
					{ "type": "label", "text": "{course.how2}", "style": "dim" } ] },
				{ "type": "panel", "anchor": "top-right", "pos": [28, 28], "layout": "vertical", "style": "accent",
				  "padding": 14, "gap": 3, "visible": "!course.in", "children": [
					{ "type": "label", "text": "PARKOUR COURSE", "style": "title" },
					{ "type": "label", "text": "fourteen stations: walk into one" },
					{ "type": "label", "text": "or {keys.terminal} and  station 1 .. station 14", "style": "dim" } ] }
			] } })json";
	}

	inline void BuildStation(GS::Scene& scene, int index)
	{
		const Lane lane(Stations()[(size_t)index]);
		switch (index)
		{
			case 0:   // walking
			{
				// A kerb (12 cm), five stairs up of 17 cm to a platform, four
				// down, a 33 cm step -- just under the 35 the walk takes --
				// and a wall at the end, too tall for anything but stopping.
				Block(scene, "Kerb", lane, 2.0f, 2.3f, 0.0f, 3.0f, 0.12f);
				for (int k = 0; k < 5; k++)
					Block(scene, "Stair", lane, 4.0f + 0.32f * k, 4.32f + 0.32f * k, 0.0f, 3.0f, 0.17f * (k + 1));
				Block(scene, "Platform", lane, 5.6f, 8.6f, 0.0f, 3.0f, 0.85f);
				for (int k = 0; k < 4; k++)
					Block(scene, "Stair", lane, 8.6f + 0.32f * k, 8.92f + 0.32f * k, 0.0f, 3.0f, 0.68f - 0.17f * k);
				Block(scene, "Big Step", lane, 11.5f, 12.5f, 0.0f, 3.0f, 0.33f);
				Block(scene, "End Wall", lane, 16.0f, 16.4f, 0.0f, 4.0f, 3.0f, WallGrey);
				break;
			}
			case 1:   // vaults
				// Two waist-high walls 4.2 m apart, so a run vaults the one
				// into the other, and a lower rail.
				Block(scene, "Vault Wall", lane, 4.0f, 4.3f, 0.0f, 3.0f, 1.0f, Painted);
				Block(scene, "Vault Wall", lane, 8.5f, 8.8f, 0.0f, 3.0f, 1.0f, Painted);
				Block(scene, "Rail", lane, 13.0f, 13.2f, 0.0f, 3.0f, 0.9f, Painted);
				break;
			case 2:   // climbing
				// Knee high and deep, a ledge at 1.6, and a wall at 2.35:
				// out of reach standing, so a jump to a hang first.
				Block(scene, "Step Box", lane, 3.5f, 4.9f, 0.0f, 3.0f, 0.6f);
				Block(scene, "Ledge", lane, 8.0f, 10.4f, 0.0f, 3.0f, 1.6f);
				Block(scene, "High Wall", lane, 14.0f, 15.2f, 0.0f, 3.0f, 2.35f, Darker);
				break;
			case 3:   // crates
			{
				// Loose crates: one of 1 m and 40 kg, two small ones (60 cm,
				// 15 kg) side by side, a stack of three big ones, a pile of
				// small ones.
				Crate(scene, lane.At(4.5f, 0.0f, 0.0f), 1.0f, 40.0f, lane.Yaw);
				Crate(scene, lane.At(8.3f, -0.32f, 0.0f), 0.6f, 15.0f, lane.Yaw);
				Crate(scene, lane.At(8.3f, 0.32f, 0.0f), 0.6f, 15.0f, lane.Yaw);
				Crate(scene, lane.At(12.5f, -0.52f, 0.0f), 1.0f, 40.0f, lane.Yaw);
				Crate(scene, lane.At(12.5f, 0.52f, 0.0f), 1.0f, 40.0f, lane.Yaw);
				Crate(scene, lane.At(12.5f, 0.0f, 1.0f), 1.0f, 40.0f, lane.Yaw);
				// And the crash test's pile behind the dummy's mark: three,
				// two and one of the small ones, side by side across the lane.
				for (int row = 0; row < 3; row++)
					for (int k = 0; k < 3 - row; k++)
						Crate(scene, lane.At(16.8f, (k - 0.5f * (2 - row)) * 0.62f, 0.6f * row), 0.6f, 15.0f, lane.Yaw);
				break;
			}
			case 4:   // hanging
				// A block 4 m wide with its top at 2.35: an edge on every
				// side, and a corner at each end to go round.
				Block(scene, "Hang Block", lane, 5.0f, 6.2f, 0.0f, 4.0f, 2.35f, Darker);
				break;
			case 5:   // drops
				// The ledge, and a 3 m tower on its far half to climb to from
				// it; its far side is the drop, onto open ground with room
				// for a roll.
				Block(scene, "Ledge", lane, 4.0f, 6.4f, 0.0f, 3.0f, 1.6f);
				Block(scene, "Tower", lane, 6.4f, 9.2f, 0.0f, 3.0f, 3.0f, Darker);
				break;
			case 6:   // wall run
				// 9 m of wall, 3.5 high, its face 0.7 m to the left of the
				// line from his mark: within the metre he looks for one.
				Block(scene, "Run Wall", lane, 3.0f, 12.0f, -0.9f, 0.4f, 3.5f, WallGrey);
				break;
			case 7:   // corner
				// A shorter wall on the left and one across its end: an
				// inside corner to run round.
				Block(scene, "Run Wall", lane, 3.0f, 8.0f, -0.9f, 0.4f, 3.5f, WallGrey);
				Block(scene, "Corner Wall", lane, 8.0f, 8.4f, 0.6f, 3.4f, 3.5f, WallGrey);
				break;
			case 8:   // tic-tac
			{
				// A tall wall across the lane at 50 degrees: met running
				// straight down the lane, at a slant.
				GS::Entity slant = Box(scene, "Slant Wall", lane.At(6.0f, 0.4f, 1.75f), { 3.0f, 3.5f, 0.4f }, WallGrey, 0.7f);
				slant.Get<GS::TransformComponent>()->Rotation = { 0.0f, lane.Yaw + 50.0f, 0.0f };
				Static(slant);
				break;
			}
			case 9:   // cat leap
				// The high wall with a run-up, and a line on the ground two
				// metres short of it where the leap leaves.
				Block(scene, "High Wall", lane, 9.0f, 10.2f, 0.0f, 3.0f, 2.35f, Darker);
				Paint(scene, "Take-off", lane, 6.75f, 7.0f, 0.0f, 3.0f, Painted);
				break;
			case 10:  // precision
				// A block to climb, three pillars as high, gaps of 0.85 and 1.1
				// m, and a block to land on. All a metre: a precision jump
				// wants the gap more than 0.8 m deep, or the floor in it reads
				// as the nearer top -- from the crash test's 50 cm step none
				// would go.
				Block(scene, "Pillar Step", lane, 2.5f, 3.5f, 0.0f, 1.2f, 1.0f);
				for (float at : { 4.6f, 6.2f, 7.8f })
					Block(scene, "Pillar", lane, at - 0.25f, at + 0.25f, 0.0f, 0.5f, 1.0f, Painted);
				Block(scene, "Landing", lane, 9.15f, 10.65f, 0.0f, 2.0f, 1.0f);
				break;
			case 11:  // knock-downs
			{
				// A mat to fall on, and the crash test's cannon 13 m down
				// the lane, turned to face his mark and aimed at him.
				Paint(scene, "Mat", lane, 1.0f, 6.0f, 0.0f, 3.2f, { 0.2f, 0.32f, 0.55f, 1.0f });
				const glm::vec3 pivot = lane.At(13.0f, 0.0f, 1.25f);
				const glm::vec3 turned(0.0f, lane.Yaw + 180.0f, 0.0f);
				Mesh(scene, "Cannon", "model:cannon.base", pivot, turned);
				Mesh(scene, "Launcher Yoke", "model:cannon.yoke", pivot, turned);
				GS::Entity launcher = Mesh(scene, "Launcher", "model:cannon.barrel", pivot, turned);
				// 12 kg at 16 m/s, 192 N s: enough to put him down, where the
				// default 5 kg ball only staggers him.
				GS::LauncherComponent fire;
				fire.Target = "Runner";
				fire.Mass = 12.0f;
				fire.Speed = 16.0f;
				fire.Size = glm::vec3(0.34f);
				launcher.Add<GS::LauncherComponent>(fire);
				CrashTestScene::Interact(launcher, "operate", "Operate the cannon", 3.2f);
				CrashTestScene::Collider(scene, "Cannon Collider", { pivot.x, 0.06f, pivot.z }, { 1.6f, 0.12f, 1.6f });
				CrashTestScene::Collider(scene, "Cannon Collider", { pivot.x, 0.45f, pivot.z }, { 0.6f, 0.8f, 0.6f });
				break;
			}
			case 12:  // slopes
				// 15 degrees up to a platform at 1.2 m, 25 down the far side,
				// and a 30 degree ramp to a block at 0.9 -- the ankles tilt the
				// soles 25 degrees at most, so on the last the feet stand on
				// their toes and heels.
				Ramp(scene, "Ramp", lane, 3.0f, 3.0f + 1.2f / std::tan(glm::radians(15.0f)), 0.0f, 1.2f, 0.0f, 3.0f);
				Block(scene, "Ramp Top", lane, 3.0f + 1.2f / std::tan(glm::radians(15.0f)), 10.0f, 0.0f, 3.0f, 1.2f);
				Ramp(scene, "Ramp", lane, 10.0f, 10.0f + 1.2f / std::tan(glm::radians(25.0f)), 1.2f, 0.0f, 0.0f, 3.0f);
				Ramp(scene, "Steep Ramp", lane, 14.0f, 14.0f + 0.9f / std::tan(glm::radians(30.0f)), 0.0f, 0.9f, 0.0f, 3.0f, 0.4f, Darker);
				Block(scene, "Steep Top", lane, 14.0f + 0.9f / std::tan(glm::radians(30.0f)), 18.0f, 0.0f, 3.0f, 0.9f, Darker);
				break;
			default:
				break;
		}

		// The start mark, the lane's edges, and the sign beside the start,
		// turned to face the walkway.
		const glm::vec4 line(0.86f, 0.86f, 0.82f, 1.0f);
		Paint(scene, "Mark", lane, -0.06f, 0.06f, 0.0f, 0.9f, Painted);
		for (float edge : { -LaneHalfWidth, LaneHalfWidth })
			Paint(scene, "Lane Line", lane, -0.5f, LaneLength, edge, 0.08f, line);
		GS::Entity sign = Mesh(scene, "Sign", "model:screen", lane.At(-0.45f, 2.55f, 1.95f), { 0.0f, lane.Yaw + 180.0f, 0.0f },
			{ 2.4f, 1.35f, 1.0f });
		GS::ScreenComponent screen;
		screen.Screen = SignName(index);
		screen.Width = 1280;
		screen.Height = 720;
		// Brighter than the crash test's, which hangs indoors: in full sun
		// the same screen read as unlit.
		screen.Emission = 2.6f;
		sign.Add<GS::ScreenComponent>(screen);
		Block(scene, "Sign Post", lane, -0.51f, -0.39f, 2.55f, 0.12f, 1.28f, Darker);
	}

	inline void Build(GS::Scene& scene)
	{
		// Outdoors: a high sun, a clear sky, a little haze.
		GS::Entity environment = scene.CreateEntity("Environment");
		GS::EnvironmentComponent env;
		env.SunAzimuth = 35.0f;
		env.SunElevation = 52.0f;
		env.SunIntensity = 3.2f;
		env.AmbientIntensity = 0.55f;
		env.FogDensity = 0.004f;
		env.ShadowExtent = 24.0f;
		environment.Add<GS::EnvironmentComponent>(env);

		// The ground, far wider than the yard so nothing walks off it, and
		// the yard's paving over it: 44 by 46 m, the walkway down its middle.
		Static(Box(scene, "Ground", { 0.0f, -0.5f, 0.0f }, { 160.0f, 1.0f, 160.0f }, { 0.36f, 0.41f, 0.31f, 1.0f }, 0.9f));
		Box(scene, "Yard", { 3.5f, 0.002f, 0.0f }, { 51.0f, 0.004f, 46.0f }, { 0.5f, 0.51f, 0.52f, 1.0f }, 0.45f);
		Box(scene, "Walkway", { 3.5f, 0.004f, 0.0f }, { 51.0f, 0.004f, 4.4f }, { 0.4f, 0.42f, 0.45f, 1.0f }, 0.5f);

		for (int i = 0; i < (int)Stations().size(); i++)
			BuildStation(scene, i);

		// The dummy, on the first station's mark.
		const Station& first = Stations()[0];
		CrashTestScene::Dummy(scene, "Runner", first.Start, first.Yaw, { 0.15f, 0.75f, 0.45f, 1.0f }, 0);

		// The camera you fly when you let go of him (and the one that
		// follows him until then), and one over the whole yard.
		CrashTestScene::Camera(scene, "Fly", first.Start + glm::vec3(0.0f, 2.2f, -3.6f), first.Start + glm::vec3(0.0f, 1.0f, 3.0f), true, true);
		CrashTestScene::Camera(scene, "Overview", { 0.0f, 26.0f, -32.0f }, { 0.0f, 0.0f, 2.0f }, false);
	}

	// The screens this scene uses, into a project's ui/ folder, unless it
	// already has them (they may have been edited).
	inline void WriteScreens(const std::string& projectPath)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path ui = fs::path(projectPath) / "ui";
		fs::create_directories(ui, ec);
		auto write = [&](const std::string& name, const std::string& text)
		{
			fs::path file = ui / (name + ".gsui");
			if (fs::exists(file))
				return;
			std::ofstream out(file, std::ios::binary);
			out << text << "\n";
		};
		for (int i = 0; i < (int)Stations().size(); i++)
			write(SignName(i), SignScreen(i));
		write("cannon", GameModes::CannonScreen());
		write("hud", GameModes::HudScreen());
	}
}

class ParkourCourse : public DemoLayer
{
public:
	ParkourCourse() : DemoLayer("ParkourCourse") {}

	void OnDemoAttach() override
	{
		m_Renderer.Init();
	}

	void OnDemoActivated() override
	{
		m_Scene = std::make_unique<GS::Scene>();
		ParkourCourseScene::Build(*m_Scene);
		// The dodging lane's cannon is the cannon run's (Gunnery), not a
		// launcher: it charges, leads him and holds its fire as those do.
		{
			const ParkourCourseScene::Station& s = ParkourCourseScene::Stations()[13];
			ParkourCourseScene::Lane lane(s);
			Gunnery::Settings settings;
			settings.Speed = 16.0f;
			settings.Reload = 2.4f;
			settings.Charge = 0.9f;
			settings.MinRange = 5.0f;
			m_Guns.AddCannon(*m_Scene, "Dodge Cannon", lane.At(16.0f, 0.0f, 1.25f), lane.Yaw + 180.0f, settings);
		}
		m_Session = std::make_unique<GameSession>(*m_Scene, PlayMode::s_ScriptEngine);
		m_Session->Start();
		m_Guns.Attach(m_Session.get());
		m_Session->Modes().SetMouseLook(true);
		GameUI::Context& ui = m_Session->UI();
		for (int i = 0; i < (int)ParkourCourseScene::Stations().size(); i++)
			ui.RegisterBuiltin(ParkourCourseScene::SignName(i), ParkourCourseScene::SignScreen(i));
		ui.RegisterBuiltin("course", ParkourCourseScene::CourseScreen());
		ui.Show("course");
		m_Session->Console().Register("station", "[number or name]", "put him at the start of a station; no number lists them",
			[this](const GameConsole::Args& a) { Station(a); });
		m_Session->Console().Register("ghost", "[clip | off]", "a recorded person doing it beside him, on his bodies; no clip lists them",
			[this](const GameConsole::Args& a) { Ghost(a); });
		m_Ghost = false;
		// As pressing E on him does.
		m_Session->Console().Execute("possess Runner", false);
		m_Shown = -2;
	}

	void OnDemoDeactivated() override
	{
		Capture(false);
		m_Guns.Clear(m_Scene.get());
		m_Guns.Attach(nullptr);
		m_Session.reset();
		m_Scene.reset();
	}

	void OnDemoFixedUpdate(GS::Timestep step) override
	{
		if (!m_Session)
			return;
		m_Session->FixedUpdate(step);
		if (m_Ghost)
		{
			// Looped, with half a second standing at the end so where it
			// stopped can be seen before it starts again.
			m_GhostTime += step.GetSeconds();
			if (m_GhostTime > m_GhostClip.Duration() + 0.5f)
				m_GhostTime = 0.0f;
		}
		// The dodging lane's cannon, live while he is in that lane.
		Humanoid* rig = m_Session->Rig("Runner");
		bool inLane = rig && ParkourCourseScene::StationAt(m_Session->World().GetBody(rig->PelvisBody()).Position) == 13;
		m_Guns.Step(step, *m_Session, *m_Scene, rig, inLane);
		glm::vec3 focus(0.0f);
		bool has = m_Guns.Focus(focus);
		m_Session->Characters().SetFocusTarget(focus, has);
		m_Session->Characters().SetDodgeAssist(m_Guns.Evade());
		PublishStation();
	}

	void OnDemoUpdate(GS::Timestep) override
	{
		if (!m_Session)
			return;

		ViewportRect view = DemoViewport();
		GS::RenderCommand::SetViewport((unsigned int)view.X, (unsigned int)view.Y, (unsigned int)view.Width, (unsigned int)view.Height);
		GS::RenderCommand::SetClearColor({ 0.62f, 0.74f, 0.88f, 1.0f });
		GS::RenderCommand::Clear();

		float aspect = (float)view.Width / (float)std::max(view.Height, 1);
		GS::PerspectiveCamera* camera = &m_Camera;
		if (!SceneRenderer::FindActiveCamera(*m_Scene, aspect, m_Camera))
		{
			m_Fallback.SetProjection(50.0f, aspect, 0.1f, 500.0f);
			camera = &m_Fallback;
		}
		GS::RenderCommand::SetCullFace(GS::CullFace::Back);
		std::vector<ExtraDraw> humanoids;
		AppendHumanoidDraws(*m_Scene, m_Session.get(), humanoids);
		m_Guns.AppendDraws(humanoids);
		AppendGhost(humanoids);
		m_Renderer.Render(*m_Scene, *camera, &humanoids);
		GS::RenderCommand::SetCullFace(GS::CullFace::None);
		Capture(m_Session->Modes().WantsCursorCaptured());

		GS::AudioListener listener;
		listener.Position = camera->GetPosition();
		listener.Forward = camera->GetForward();
		listener.Up = camera->GetUp();
		GS::AudioEngine::SetListener(listener);
	}

	void OnDemoGameOverlay() override
	{
		if (!m_Session)
			return;
		ViewportRect view = DemoViewport();
		float displayHeight = ImGui::GetIO().DisplaySize.y;
		m_Session->DrawOverlay((float)view.X, displayHeight - (float)(view.Y + view.Height), (float)view.Width, (float)view.Height);
	}

	void OnExportToScene(GS::Scene& scene) override
	{
		ParkourCourseScene::Build(scene);
	}

	void OnExportFiles(const std::string& projectPath) override
	{
		ParkourCourseScene::WriteScreens(projectPath);
	}

private:
	// `station 4`, `station crates`: him on that station's mark, facing
	// down its lane, the camera behind him.
	void Station(const GameConsole::Args& a)
	{
		const std::vector<ParkourCourseScene::Station>& stations = ParkourCourseScene::Stations();
		GameConsole& console = m_Session->Console();
		if (a.empty())
		{
			console.Print("stations", GameConsole::Tone::Title);
			for (size_t i = 0; i < stations.size(); i++)
				console.Print("  " + std::to_string(i + 1) + "  " + GameConsole::Lower(stations[i].Name) + "   (" + stations[i].Word + ")");
			return;
		}
		std::string want = GameConsole::Lower(a[0]);
		int index = -1;
		if (std::isdigit((unsigned char)want[0]))
			index = std::atoi(want.c_str()) - 1;
		else
			for (size_t i = 0; i < stations.size(); i++)
				if (want == stations[i].Word || want == GameConsole::Lower(stations[i].Name))
					index = (int)i;
		Humanoid* rig = m_Session->Rig("Runner");
		if (index < 0 || index >= (int)stations.size() || !rig)
		{
			console.Bad("no station '" + a[0] + "' -- station lists them");
			return;
		}
		const ParkourCourseScene::Station& s = stations[(size_t)index];
		rig->Respawn(s.Start, glm::radians(s.Yaw));
		// Taking him again takes the way the camera looks for his
		// (CharacterController::Possess), so it is turned down the lane
		// first: a camera's yaw is half a turn from a figure's.
		for (GS::EntityId id : m_Scene->GetEntities())
			if (auto* tag = m_Scene->GetComponent<GS::TagComponent>(id); tag && tag->Name == "Fly")
				if (auto* t = m_Scene->GetComponent<GS::TransformComponent>(id))
					t->Rotation = RotationLookingAlong(LookDirection(-12.0f, s.Yaw + 180.0f));
		console.Execute("possess Runner", false);
		console.Good("station " + std::to_string(index + 1) + ": " + GameConsole::Lower(s.Name));
	}

	// `ghost walk`: the clip on his bodies a metre to his side, facing the
	// way he faces, from where he stands -- so walking beside it, the two
	// can be watched step for step. `ghost off` stops it.
	void Ghost(const GameConsole::Args& a)
	{
		GameConsole& console = m_Session->Console();
		const std::string folder = "assets/mocap/cmu";
		std::vector<MotionClip::Entry> clips = MotionClip::ReadIndex(folder);
		if (a.empty())
		{
			console.Print("clips (CMU motion capture library, mocap.cs.cmu.edu)", GameConsole::Tone::Title);
			for (const MotionClip::Entry& e : clips)
				console.Print("  " + e.Name + "   " + e.About);
			return;
		}
		std::string want = GameConsole::Lower(a[0]);
		if (want == "off")
		{
			m_Ghost = false;
			console.Good("ghost off");
			return;
		}
		Humanoid* rig = m_Session->Rig("Runner");
		for (const MotionClip::Entry& e : clips)
		{
			if (e.Name != want)
				continue;
			std::string error;
			if (!rig || !m_GhostClip.Load(folder + "/" + e.Skeleton, folder + "/" + e.Frames, &error)
				|| !m_GhostMap.Setup(m_GhostClip, *rig))
			{
				console.Bad("could not load " + e.Name + (error.empty() ? "" : ": " + error));
				m_Ghost = false;
				return;
			}
			// Where the clip starts and which way its hips face then, so it
			// can be turned and moved to start beside him.
			glm::vec3 position[ClipRetarget::BodyCount];
			glm::quat orientation[ClipRetarget::BodyCount];
			m_GhostMap.Pose(0.0f, position, orientation);
			glm::vec3 f = orientation[0] * glm::vec3(0.0f, 0.0f, 1.0f);
			m_GhostFrom = glm::vec3(position[0].x, 0.0f, position[0].z);
			float clipYaw = std::atan2(f.x, f.z);
			const GS::RigidBody3D& pelvis = m_Session->World().GetBody(rig->PelvisBody());
			float yaw = rig->FacingRadians();
			glm::vec3 side(std::cos(yaw), 0.0f, -std::sin(yaw));
			float ground = m_Session->World().GroundHeightBelow(pelvis.Position, rig->Bodies(), -1000.0f);
			m_GhostAt = glm::vec3(pelvis.Position.x, ground, pelvis.Position.z) + side * 1.0f;
			m_GhostTurn = glm::angleAxis(yaw - clipYaw, glm::vec3(0.0f, 1.0f, 0.0f));
			m_GhostTime = 0.0f;
			m_Ghost = true;
			console.Good("ghost: " + e.Name + " (" + e.About + "), " + std::to_string(m_GhostClip.Duration()).substr(0, 4) + " s, looped");
			return;
		}
		console.Bad("no clip '" + a[0] + "' -- ghost lists them");
	}

	void AppendGhost(std::vector<ExtraDraw>& out) const
	{
		Humanoid* rig = m_Ghost ? m_Session->Rig("Runner") : nullptr;
		if (!rig)
			return;
		glm::vec3 position[ClipRetarget::BodyCount];
		glm::quat orientation[ClipRetarget::BodyCount];
		m_GhostMap.Pose(std::min(m_GhostTime, m_GhostClip.Duration()), position, orientation);
		const glm::vec4 colour(1.0f, 0.72f, 0.32f, 1.0f);
		const auto& bodies = rig->Bodies();
		for (int i = 0; i < ClipRetarget::BodyCount && i < (int)bodies.size(); i++)
		{
			const GS::RigidBody3D& body = m_Session->World().GetBody(bodies[(size_t)i]);
			glm::vec3 at = m_GhostAt + m_GhostTurn * (position[i] - m_GhostFrom);
			glm::quat turn = glm::normalize(m_GhostTurn * orientation[i]);
			Mannequin::AppendBody(body, at, turn, Mannequin::PartColour(*rig, bodies[(size_t)i], colour), GS::InvalidEntity, out);
		}
	}

	// The panel's text for the station he is in, when that changes.
	void PublishStation()
	{
		Humanoid* rig = m_Session->Rig("Runner");
		int in = rig ? ParkourCourseScene::StationAt(m_Session->World().GetBody(rig->PelvisBody()).Position) : -1;
		if (in == m_Shown)
			return;
		m_Shown = in;
		GameUI::Context& ui = m_Session->UI();
		ui.Set("course.in", in >= 0 ? "1" : "0");
		if (in < 0)
			return;
		const ParkourCourseScene::Station& s = ParkourCourseScene::Stations()[(size_t)in];
		ui.Set("course.title", std::to_string(in + 1) + "   " + s.Name);
		// Expanded here, once: the panel's own {course.how0} is one level of
		// substitution, and these hold {keys.jump} and the like.
		for (int k = 0; k < 3; k++)
			ui.Set("course.how" + std::to_string(k), ui.Expand(s.How[k]));
	}

	ViewportRect DemoViewport() const
	{
		if (g_Viewport.Valid())
			return g_Viewport;
		GS::Window& window = GS::Application::Get().GetWindow();
		return { 0, 0, (int)window.GetWidth(), (int)window.GetHeight() };
	}

	// The pointer is the camera's while he is being walked, and the
	// desktop's otherwise -- flying, at the terminal, at the cannon.
	void Capture(bool on)
	{
		if (on == m_Captured)
			return;
		m_Captured = on;
		GS::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	std::unique_ptr<GS::Scene> m_Scene;
	std::unique_ptr<GameSession> m_Session;
	Gunnery m_Guns;
	bool m_Captured = false;
	SceneRenderer m_Renderer;
	GS::PerspectiveCamera m_Camera{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };
	GS::PerspectiveCamera m_Fallback{ 50.0f, 16.0f / 9.0f, 0.1f, 500.0f };
	int m_Shown = -2;   // the station the panel shows; -2 before the first

	// `ghost`: the clip, put on his bodies, and where it plays.
	MotionClip m_GhostClip;
	ClipRetarget m_GhostMap;
	bool m_Ghost = false;
	float m_GhostTime = 0.0f;
	glm::vec3 m_GhostAt{ 0.0f }, m_GhostFrom{ 0.0f };
	glm::quat m_GhostTurn{ 1.0f, 0.0f, 0.0f, 0.0f };
};
