// TEMPORARY -- delete after verifying dodges and slopes.
#pragma once
#include <GS.h>

#include "GameSession.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace DodgeDriver
{
	struct Phase
	{
		int Steps;
		std::vector<int> Keys;
		bool Dodge = false;    // the dodge button on this phase's first step
	};
	struct Scenario
	{
		std::string Name;
		glm::vec3 Start;
		float Yaw;             // degrees, the rig's (faces (sin, 0, cos))
		std::vector<Phase> Phases;
	};

	inline std::vector<Scenario> g_Scenarios;
	inline size_t g_Index = 0;
	inline size_t g_Phase = 0;
	inline int g_InPhase = -1;
	inline int g_Step = 0;
	inline GS::InputSnapshot g_Snapshot;
	inline bool g_Done = false;

	// Per scenario.
	inline glm::vec3 g_PressAt{ 0.0f }, g_PressVelocity{ 0.0f };
	inline int g_PressStep = -1;
	inline std::vector<glm::vec3> g_Track;
	inline float g_Deviation[3] = { 0, 0, 0 };
	inline bool g_Fell = false;
	inline std::string g_Kind, g_LastMove;
	inline float g_MaxSpin = 0.0f, g_MaxDepth = 0.0f, g_MaxPelvisGap = -1e9f, g_MinPelvisGap = 1e9f;
	inline int g_DepthPart = -1, g_DepthStep = -1, g_EndStep = -1;
	inline int g_DodgeSteps = 0;
	inline glm::vec3 g_DodgeEnd{ 0.0f };
	inline bool g_DodgeEnded = false;
	inline float g_ExitSpeed = 0.0f;

	inline void Setup()
	{
		const int W = GS_KEY_W, A = GS_KEY_A, S = GS_KEY_S, D = GS_KEY_D, Sh = GS_KEY_LEFT_SHIFT;
		const glm::vec3 lane(32.0f, 0.0f, 0.0f);   // east of the yard: open ground
		auto stand = [&](const char* name, std::vector<int> keys)
		{
			g_Scenarios.push_back({ name, lane, 90.0f, { { 40, {} }, { 1, keys, true }, { 150, {} } } });
		};
		stand("stand left", { A });
		stand("stand right", { D });
		stand("stand back", { S });
		stand("stand fwd", { W });
		stand("stand none", {});
		auto walk = [&](const char* name, std::vector<int> keys, std::vector<int> after)
		{
			g_Scenarios.push_back({ name, lane, 90.0f, { { 150, { W } }, { 1, keys, true }, { 150, after } } });
		};
		walk("walk W+A", { W, A }, { W });
		walk("walk A", { A }, { W });
		walk("walk back", { S }, {});
		walk("walk fwd", { W }, { W });
		auto run = [&](const char* name, std::vector<int> keys, std::vector<int> after)
		{
			g_Scenarios.push_back({ name, lane, 90.0f, { { 170, { W, Sh } }, { 1, keys, true }, { 150, after } } });
		};
		if (std::getenv("DIVE_ONLY")) g_Scenarios.clear();
		run("run cut A", { W, A }, { W });
		run("run cut D", { W, D }, { W });
		run("run brake", { S }, {});
		run("sprint dive A", { W, A, Sh }, { W, Sh });
		run("sprint dive fwd", { W, Sh }, { W, Sh });
		run("sprint back", { S, Sh }, {});
		// The drops station's roll, for comparison: run off the 3 m tower.
		g_Scenarios.push_back({ "tower dive-roll", { 17.5f, 3.0f, 10.0f }, 0.0f, { { 1, {} }, { 1, { W, Sh }, false }, { 260, { W, Sh } } } });
		if (const char* only = std::getenv("ONLY")) { std::vector<Scenario> keep; for (auto& x : g_Scenarios) if (x.Name == only) keep.push_back(x); g_Scenarios = keep; return; }
		if (std::getenv("SLOPE_ONLY")) { g_Scenarios.clear(); g_Scenarios.push_back({ "slope walk", { 24.5f, 0.0f, 3.0f }, 0.0f, { { 900, { W } } } }); g_Scenarios.push_back({ "slope run", { 24.5f, 0.0f, 3.0f }, 0.0f, { { 420, { W, Sh } } } }); return; }
		if (std::getenv("DIVE_ONLY")) { std::vector<Scenario> keep; for (auto& x : g_Scenarios) if (x.Name.find("dive") != std::string::npos) keep.push_back(x); g_Scenarios = keep; return; }
		// Slopes: station 13's lane, walked and run end to end.
		g_Scenarios.push_back({ "slope walk", { 24.5f, 0.0f, 3.0f }, 0.0f, { { 900, { W } } } });
		g_Scenarios.push_back({ "slope run", { 24.5f, 0.0f, 3.0f }, 0.0f, { { 420, { W, Sh } } } });
	}

	inline void Begin(GameSession& session, GS::Scene& scene, const Scenario& sc)
	{
		Humanoid* rig = session.Rig("Runner");
		rig->Respawn(sc.Start, glm::radians(sc.Yaw));
		for (GS::EntityId id : scene.GetEntities())
			if (auto* tag = scene.GetComponent<GS::TagComponent>(id); tag && tag->Name == "Fly")
				if (auto* t = scene.GetComponent<GS::TransformComponent>(id))
					t->Rotation = RotationLookingAlong(LookDirection(-12.0f, sc.Yaw + 180.0f));
		session.Console().Execute("possess Runner", false);
		g_PressStep = sc.Name == "tower dive-roll" ? 0 : -1;
		g_Track.clear();
		g_Deviation[0] = g_Deviation[1] = g_Deviation[2] = 0.0f;
		g_Fell = false;
		g_Kind.clear();
		g_LastMove.clear();
		g_MaxSpin = g_MaxDepth = 0.0f;
		g_DepthPart = g_DepthStep = -1;
		g_MaxPelvisGap = -1e9f;
		g_MinPelvisGap = 1e9f;
		g_DodgeSteps = 0;
		g_DodgeEnded = false;
		g_ExitSpeed = 0.0f;
	}

	// How far a body is into the ground: each box corner (or each end of a
	// capsule, a sphere's bottom) against the ground under that point --
	// on a slope the ground under the centre is not the ground under a toe.
	inline float Depth(const GS::PhysicsWorld3D& world, const GS::RigidBody3D& b, const std::vector<GS::PhysicsWorld3D::BodyHandle>& own)
	{
		auto below = [&](const glm::vec3& p, float radius)
		{
			float ground = world.GroundHeightBelow(glm::vec3(p.x, b.Position.y + 2.0f, p.z), own, -1000.0f);
			return ground - (p.y - radius);
		};
		float worst = -1.0f;
		if (b.Shape == GS::ColliderShape3D::Box)
		{
			for (int k = 0; k < 8; k++)
			{
				glm::vec3 c((k & 1) ? b.HalfExtents.x : -b.HalfExtents.x, (k & 2) ? b.HalfExtents.y : -b.HalfExtents.y, (k & 4) ? b.HalfExtents.z : -b.HalfExtents.z);
				worst = std::max(worst, below(b.Position + b.Orientation * c, 0.0f));
			}
		}
		else if (b.Shape == GS::ColliderShape3D::Capsule)
		{
			glm::vec3 axis = b.Orientation * glm::vec3(0.0f, b.HalfHeight, 0.0f);
			worst = std::max(below(b.Position + axis, b.Radius), below(b.Position - axis, b.Radius));
		}
		else
			worst = below(b.Position, b.Radius);
		return worst;
	}

	inline void Report(const Scenario& sc)
	{
		glm::vec3 end = g_Track.empty() ? glm::vec3(0.0f) : g_Track.back();
		std::printf("[dodge] %-16s kind=%-8s dev(.3/.5/.7s)=%.2f/%.2f/%.2f m  disp=%.2f m  steps=%d exit=%.2f m/s fell=%d spin=%.0f deg/s depth=%.1f cm (part %d @%d)  pelvis-gap=[%.2f,%.2f] end=(%.2f,%.2f,%.2f)\n",
			sc.Name.c_str(), g_Kind.empty() ? "-" : g_Kind.c_str(), g_Deviation[0], g_Deviation[1], g_Deviation[2],
			g_DodgeEnded ? glm::length(glm::vec2(g_DodgeEnd.x - g_PressAt.x, g_DodgeEnd.z - g_PressAt.z)) : 0.0f,
			g_DodgeSteps, g_ExitSpeed, g_Fell ? 1 : 0, glm::degrees(g_MaxSpin), g_MaxDepth * 100.0f, g_DepthPart, g_DepthStep,
			g_MinPelvisGap, g_MaxPelvisGap, end.x, end.y, end.z);
		std::fflush(stdout);
	}

	// After the session's step: a camera beside him, for a side view of a
	// move (SIDE_CAMERA=1), over whatever the follow camera wrote.
	inline void PostStep(GameSession& session, GS::Scene& scene)
	{
		if (!std::getenv("SIDE_CAMERA") || g_Done)
			return;
		Humanoid* rig = session.Rig("Runner");
		glm::vec3 p = session.World().GetBody(rig->PelvisBody()).Position;
		float f = rig->FacingRadians();
		glm::vec3 F(std::sin(f), 0.0f, std::cos(f)), R(std::cos(f), 0.0f, -std::sin(f));
		static glm::vec3 eye = p;
		glm::vec3 want = glm::vec3(p.x, 0.9f, p.z) + R * 4.5f + glm::vec3(0.0f, 0.4f, 0.0f);
		eye += (want - eye) * 0.2f;
		for (GS::EntityId id : scene.GetEntities())
			if (auto* tag = scene.GetComponent<GS::TagComponent>(id); tag && tag->Name == "Fly")
				if (auto* t = scene.GetComponent<GS::TransformComponent>(id))
				{
					t->Position = eye;
					t->Rotation = RotationLookingAlong(glm::vec3(p.x, 0.7f, p.z) - eye);
				}
	}

	// Before the session's step: this step's keys.
	inline void Step(GameSession& session, GS::Scene& scene)
	{
		if (g_Done)
			return;
		if (g_Scenarios.empty())
			Setup();
		if (g_Index >= g_Scenarios.size())
		{
			GS::Input::SetPlaybackSnapshot(nullptr);
			g_Done = true;
			std::printf("[dodge] done\n");
			std::fflush(stdout);
			return;
		}
		Scenario& sc = g_Scenarios[g_Index];
		if (g_InPhase < 0)
		{
			Begin(session, scene, sc);
			g_Phase = 0;
			g_InPhase = 0;
			g_Step = 0;
		}
		const Phase& ph = sc.Phases[g_Phase];
		g_Snapshot = GS::InputSnapshot{};
		for (int k : ph.Keys)
			g_Snapshot.SetKey(k, true);
		if (ph.Dodge && g_InPhase == 0)
			g_Snapshot.SetMouseButton(GS_MOUSE_BUTTON_LEFT, true);
		GS::Input::SetPlaybackSnapshot(&g_Snapshot);

		Humanoid* rig = session.Rig("Runner");
		const GS::PhysicsWorld3D& world = session.World();
		glm::vec3 p = world.GetBody(rig->PelvisBody()).Position;
		g_Track.push_back(p);
		if (ph.Dodge && g_InPhase == 0)
		{
			g_PressStep = g_Step;
			g_PressAt = p;
			size_t n = g_Track.size();
			glm::vec3 back = n > 7 ? g_Track[n - 7] : p;
			g_PressVelocity = (p - back) * (60.0f / 6.0f);
			g_PressVelocity.y = 0.0f;
		}
		if (g_PressStep >= 0)
		{
			float t = (g_Step - g_PressStep) / 60.0f;
			glm::vec3 expected = g_PressAt + g_PressVelocity * t;
			float dev = glm::length(glm::vec2(p.x - expected.x, p.z - expected.z));
			for (int k = 0; k < 3; k++)
				if (std::fabs(t - (0.3f + 0.2f * k)) < 0.5f / 60.0f)
					g_Deviation[k] = dev;
			std::string move = rig->MoveName();
			if (rig->IsDodging() || (sc.Name == "tower dive-roll" && rig->IsMoving()))
			{
				g_Kind = rig->DodgeName();
				g_DodgeSteps++;
				if (sc.Name.find("dive") != std::string::npos)
				{
					float worst = 0.0f;
					int part = -1;
					for (GS::PhysicsWorld3D::BodyHandle h : rig->Bodies())
					{
						float w = glm::length(world.GetBody(h).AngularVelocity);
						if (w > worst) { worst = w; part = (int)rig->PartOf(h); }
					}
					const GS::RigidBody3D& pv = world.GetBody(rig->PelvisBody());
					glm::vec3 up = pv.Orientation * glm::vec3(0.0f, 1.0f, 0.0f);
					std::printf("[dive] %s step %d pelvis=(%.2f,%.2f,%.2f) up=(%.2f,%.2f,%.2f) spin=%.0f part=%d\n", sc.Name.c_str(), g_Step - g_PressStep,
						pv.Position.x, pv.Position.y, pv.Position.z, up.x, up.y, up.z, glm::degrees(worst), part);
				}
				for (GS::PhysicsWorld3D::BodyHandle h : rig->Bodies())
				{
					const GS::RigidBody3D& b = world.GetBody(h);
					g_MaxSpin = std::max(g_MaxSpin, glm::length(b.AngularVelocity));
					float dd = Depth(world, b, rig->Bodies());
					if (dd > g_MaxDepth) { g_MaxDepth = dd; g_DepthPart = (int)rig->PartOf(h); g_DepthStep = g_Step - g_PressStep; }
				}
			}
			else if (!g_Kind.empty() && !g_DodgeEnded)
			{
				g_DodgeEnded = true;
				g_DodgeEnd = p;
				g_EndStep = g_Step;
			}
			if (g_DodgeEnded && g_Step == g_EndStep + 8)
			{
				glm::vec3 v = (p - g_Track[g_Track.size() - 7]) * 10.0f;   // over the last 6 steps
				g_ExitSpeed = glm::length(glm::vec2(v.x, v.z));
			}
		}
		if (g_PressStep >= 0 && std::getenv("DIVE_ONLY") && !rig->IsDown())
		{
			auto mine = [&](unsigned int b) { for (auto h : rig->Bodies()) if (h == b) return true; return false; };
			for (const GS::Contact3D& c : world.GetContacts())
			{
				bool a = mine(c.A), bb = mine(c.B);
				if (a == bb) continue;
				unsigned int m = a ? c.A : c.B, o = a ? c.B : c.A;
				float f = c.TotalNormalImpulse() * 60.0f;
				if (f > 500.0f)
					std::printf("[contact] %s step %d part=%d other=%u (type %d) force=%.0f moving=%d move=%s\n", sc.Name.c_str(), g_Step - g_PressStep,
						(int)rig->PartOf(m), o, (int)world.GetBody(o).Type, f, rig->IsMoving() ? 1 : 0, rig->MoveName());
			}
		}
		if (rig->IsDown() && !g_Fell)
		{
			g_Fell = true;
			std::printf("[down] %s step %d (%d after the press): %s\n", sc.Name.c_str(), g_Step, g_Step - g_PressStep, rig->DownReason());
		}
		if (sc.Name.rfind("slope", 0) == 0 && !rig->IsDown() && !rig->IsMoving())
		{
			float ground = world.GroundHeightBelow(p, rig->Bodies(), -1000.0f);
			g_MinPelvisGap = std::min(g_MinPelvisGap, p.y - ground);
			g_MaxPelvisGap = std::max(g_MaxPelvisGap, p.y - ground);
			for (GS::PhysicsWorld3D::BodyHandle h : rig->Bodies())
			{
				float dd = Depth(world, world.GetBody(h), rig->Bodies());
				if (dd > 0.04f)
					std::printf("[sink] %s step %d part=%d depth=%.1f cm at (%.2f,%.2f,%.2f)\n", sc.Name.c_str(), g_Step, (int)rig->PartOf(h), dd * 100.0f,
						world.GetBody(h).Position.x, world.GetBody(h).Position.y, world.GetBody(h).Position.z);
				if (dd > g_MaxDepth) { g_MaxDepth = dd; g_DepthPart = (int)rig->PartOf(h); g_DepthStep = g_Step; }
			}
			std::string move = rig->MoveName();
			if (g_Step % 60 == 0)
			{
				std::printf("[slope] %s t=%.1f pos=(%.2f,%.2f,%.2f) gap=%.3f ground=%.3f stumbling=%d\n", sc.Name.c_str(), g_Step / 60.0f, p.x, p.y, p.z,
					p.y - ground, ground, rig->IsStumbling() ? 1 : 0);
				std::fflush(stdout);
			}
		}
		if (std::string(rig->MoveName()) != g_LastMove)
		{
			g_LastMove = rig->MoveName();
			if (!g_LastMove.empty())
				std::printf("[move] %s step %d: %s\n", sc.Name.c_str(), g_Step, g_LastMove.c_str());
		}

		g_Step++;
		g_InPhase++;
		if (g_InPhase >= ph.Steps)
		{
			g_InPhase = 0;
			g_Phase++;
			if (g_Phase >= sc.Phases.size())
			{
				Report(sc);
				g_Index++;
				g_InPhase = -1;
			}
		}
	}
}
