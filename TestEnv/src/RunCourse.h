#pragma once

// The cannon run's courses: a path of pieces, each a stretch of track with
// what is on it -- walls to vault, a ledge to climb, a ramp up to a platform
// and a drop off its end, crates, cover, pillars to jump between, a corner
// -- and the cannons that cover it.
//
// Every piece says where it commits him (a no-fire zone: the run-up to a
// vault and the landing, the climb, the pillars, the drop) and where he is
// in the open; its cannons stand off to the side of the open stretches. So
// a course is fair by construction -- the cannons cannot see the stretches
// where he could not get out of the way, mostly, and Gunnery's own rules
// hold their fire on the rest.
//
// A course is a Layout: the pieces in order, each with its variant and the
// side its cannon is on. The first is made by hand (Gauntlet); any other is
// made from a seed (Generate) -- the same seed, the same course, so a time
// on "course 4711" means the same thing to anyone who runs it.

#include <GS.h>

#include "CrashTest.h"
#include "Gunnery.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace RunCourse
{
	enum class Kind { Start, Sprint, Vaults, RampDrop, Climb, Crates, Cover, Precision, Corner, Finish };

	inline const char* KindName(Kind k)
	{
		switch (k)
		{
			case Kind::Start: return "start";
			case Kind::Sprint: return "sprint";
			case Kind::Vaults: return "vaults";
			case Kind::RampDrop: return "ramp and drop";
			case Kind::Climb: return "climb";
			case Kind::Crates: return "crates";
			case Kind::Cover: return "cover";
			case Kind::Precision: return "pillars";
			case Kind::Corner: return "corner";
			case Kind::Finish: return "finish";
		}
		return "";
	}

	struct Piece
	{
		Kind Type = Kind::Sprint;
		int Variant = 0;
		int Side = 1;          // which side its cannon stands: -1 left, +1 right, 0 none; a corner's turn
	};

	struct Layout
	{
		uint32_t Seed = 0;     // 0 the one made by hand
		std::vector<Piece> Pieces;
	};

	// A gate across the track: crossed when the hips pass its line, inside
	// its width, going its way.
	struct Gate
	{
		glm::vec3 At{ 0.0f }, Forward{ 0.0f, 0.0f, 1.0f };
		float HalfWidth = 4.0f;
		std::string Name;
	};

	struct Built
	{
		std::vector<Gate> Gates;   // [0] the start line, back() the finish
		glm::vec3 Spawn{ 0.0f };
		float SpawnYaw = 0.0f;     // radians, the rig's
		float Length = 0.0f;       // m of track
		glm::vec3 Min{ 1e9f }, Max{ -1e9f };   // the ground it covers
	};

	// A piece's frame: along the track from where it starts, and across it
	// to the right of someone running it.
	struct Frame
	{
		glm::vec3 O{ 0.0f };
		float Yaw = 0.0f;          // degrees: the track runs along (sin, 0, cos)
		glm::vec3 F{ 0.0f, 0.0f, 1.0f }, R{ -1.0f, 0.0f, 0.0f };
		Frame() = default;
		Frame(const glm::vec3& o, float yaw) : O(o), Yaw(yaw)
		{
			float y = glm::radians(yaw);
			F = glm::vec3(std::sin(y), 0.0f, std::cos(y));
			R = glm::cross(F, glm::vec3(0.0f, 1.0f, 0.0f));
		}
		glm::vec3 At(float along, float across, float y) const { return glm::vec3(O.x, y, O.z) + F * along + R * across; }
	};

	inline const float Width = 6.0f;   // of the track

	// The colours of the place: a training ground in late summer -- grass,
	// a terracotta running track, warm concrete, the obstacles painted.
	namespace Palette
	{
		inline const glm::vec4 Grass(0.33f, 0.43f, 0.22f, 1.0f), Track(0.6f, 0.26f, 0.17f, 1.0f), Line(0.93f, 0.91f, 0.86f, 1.0f),
			Concrete(0.74f, 0.7f, 0.63f, 1.0f), ConcreteDark(0.56f, 0.53f, 0.49f, 1.0f), Vault(0.96f, 0.66f, 0.1f, 1.0f),
			Stripe(0.12f, 0.12f, 0.13f, 1.0f), Climb(0.33f, 0.41f, 0.52f, 1.0f), Cover(0.47f, 0.52f, 0.5f, 1.0f),
			Steel(0.2f, 0.22f, 0.25f, 1.0f), Pillar(0.85f, 0.42f, 0.2f, 1.0f), Bark(0.3f, 0.22f, 0.15f, 1.0f),
			Leaves(0.2f, 0.33f, 0.14f, 1.0f), LeavesLight(0.32f, 0.44f, 0.18f, 1.0f);
	}

	using CrashTestScene::Box;
	using CrashTestScene::Mesh;
	using CrashTestScene::Static;

	inline GS::Entity Block(GS::Scene& scene, const char* name, const Frame& f, float from, float to, float across, float width,
		float bottom, float top, const glm::vec4& colour, float roughness = 0.8f)
	{
		GS::Entity e = Box(scene, name, f.At(0.5f * (from + to), across, 0.5f * (bottom + top)), { width, top - bottom, to - from }, colour, roughness);
		e.Get<GS::TransformComponent>()->Rotation = { 0.0f, f.Yaw, 0.0f };
		Static(e);
		return e;
	}

	// Drawn on the ground, not collided with.
	inline void Paint(GS::Scene& scene, const char* name, const Frame& f, float from, float to, float across, float width, const glm::vec4& colour,
		float lift = 0.004f, float roughness = 0.85f)
	{
		GS::Entity e = Box(scene, name, f.At(0.5f * (from + to), across, lift), { width, 2.0f * lift, to - from }, colour, roughness);
		e.Get<GS::TransformComponent>()->Rotation = { 0.0f, f.Yaw, 0.0f };
	}

	// A plank ramp from height h0 at `from` to h1 at `to` (ParkourCourseScene::Ramp's arithmetic).
	inline void Ramp(GS::Scene& scene, const Frame& f, float from, float to, float h0, float h1, float width, const glm::vec4& colour)
	{
		float run = to - from, rise = h1 - h0, slope = std::atan2(rise, run), length = std::sqrt(run * run + rise * rise);
		const float thickness = 0.4f;
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 normal = up * std::cos(slope) - f.F * std::sin(slope);
		glm::vec3 centre = f.At(0.5f * (from + to), 0.0f, 0.5f * (h0 + h1)) - normal * (0.5f * thickness);
		GS::Entity e = Box(scene, "Ramp", centre, { width, thickness, length }, colour, 0.8f);
		glm::quat turn = glm::angleAxis(glm::radians(f.Yaw), up) * glm::angleAxis(-slope, glm::vec3(1.0f, 0.0f, 0.0f));
		float x, y, z;
		glm::extractEulerAngleXYZ(glm::mat4_cast(turn), x, y, z);
		e.Get<GS::TransformComponent>()->Rotation = glm::degrees(glm::vec3(x, y, z));
		Static(e);
	}

	inline void Crate(GS::Scene& scene, const glm::vec3& at, float size, float mass, float yaw)
	{
		GS::Entity crate = Mesh(scene, "Crate", "model:crate", at + glm::vec3(0.0f, 0.5f * size, 0.0f), { 0.0f, yaw, 0.0f }, glm::vec3(size));
		GS::PhysicsComponent body;
		body.Mass = mass;
		body.Friction = 0.7f;
		crate.Add<GS::PhysicsComponent>(body);
	}

	// A gate: two steel posts and a lit bar across the track. Not solid
	// overhead (the bar is high), the posts are.
	inline void GateArch(GS::Scene& scene, const Frame& f, float along, const glm::vec4& light, bool chequer)
	{
		for (float side : { -1.0f, 1.0f })
		{
			Block(scene, "Gate Post", f, along - 0.15f, along + 0.15f, side * (0.5f * Width + 0.45f), 0.3f, 0.0f, 3.3f, Palette::Steel, 0.4f)
				.Get<GS::MeshComponent>()->Metallic = 0.7f;
		}
		GS::Entity bar = Box(scene, "Gate Bar", f.At(along, 0.0f, 3.15f), { Width + 1.2f, 0.42f, 0.24f }, Palette::Steel, 0.4f);
		bar.Get<GS::TransformComponent>()->Rotation = { 0.0f, f.Yaw, 0.0f };
		bar.Get<GS::MeshComponent>()->Metallic = 0.7f;
		int cells = chequer ? 12 : 1;
		for (int k = 0; k < cells; k++)
		{
			float w = (Width + 0.9f) / (float)cells;
			float across = -0.5f * (Width + 0.9f) + w * ((float)k + 0.5f);
			bool dark = chequer && (k % 2 == 1);
			for (float face : { -1.0f, 1.0f })
			{
				GS::Entity panel = Box(scene, "Gate Light", f.At(along + face * 0.125f, across, 3.15f), { w, 0.3f, 0.02f },
					dark ? glm::vec4(0.05f, 0.05f, 0.06f, 1.0f) : light, 0.3f);
				panel.Get<GS::TransformComponent>()->Rotation = { 0.0f, f.Yaw, 0.0f };
				panel.Get<GS::MeshComponent>()->Emission = dark ? 0.0f : 2.2f;
			}
		}
	}

	// A tree, of two primitives: a trunk and a crown or two.
	inline void Tree(GS::Scene& scene, const glm::vec3& at, float height, uint32_t h)
	{
		auto r = [&](int n) { return (float)((h >> (n * 4)) & 15u) / 15.0f; };
		Box(scene, "Trunk", at + glm::vec3(0.0f, 0.5f * height * 0.45f, 0.0f), { 0.28f, height * 0.45f, 0.28f }, Palette::Bark, 0.9f);
		float crown = height * (0.36f + 0.1f * r(1));
		GS::Entity top = Mesh(scene, "Crown", "primitive:sphere", at + glm::vec3(0.0f, height * 0.62f, 0.0f), glm::vec3(0.0f),
			glm::vec3(crown * 1.3f, crown * 1.1f, crown * 1.3f), r(2) > 0.5f ? Palette::Leaves : Palette::LeavesLight);
		top.Get<GS::MeshComponent>()->Roughness = 0.95f;
		if (r(3) > 0.4f)
		{
			GS::Entity second = Mesh(scene, "Crown", "primitive:sphere", at + glm::vec3(0.3f * (r(4) - 0.5f) * height, height * 0.8f, 0.3f * (r(5) - 0.5f) * height),
				glm::vec3(0.0f), glm::vec3(crown * 0.9f), r(6) > 0.5f ? Palette::LeavesLight : Palette::Leaves);
			second.Get<GS::MeshComponent>()->Roughness = 0.95f;
		}
	}

	// The cannons' settings, varied a little per cannon so a battery is not
	// a metronome.
	inline Gunnery::Settings CannonSettings(int index)
	{
		Gunnery::Settings s;
		s.Reload = 2.8f + 0.35f * (float)(index % 3);
		s.Speed = 18.5f + 0.75f * (float)(index % 2);
		s.Phase = 0.6f * (float)(index % 4);
		return s;
	}

	// A cannon standing `across` m off the track at `along`, on a tower
	// `raise` m high, turned to face the point `aimAlong` on the track.
	inline void PlaceCannon(GS::Scene& scene, Gunnery& guns, const Frame& f, float along, float across, float raise, float aimAlong, int& count)
	{
		glm::vec3 base = f.At(along, across, 0.0f);
		if (raise > 0.0f)
			Static(Box(scene, "Cannon Tower", base + glm::vec3(0.0f, 0.5f * raise, 0.0f), { 2.2f, raise, 2.2f }, Palette::ConcreteDark, 0.85f));
		glm::vec3 pivot = base + glm::vec3(0.0f, raise + 1.25f, 0.0f);
		glm::vec3 to = f.At(aimAlong, 0.0f, 0.0f) - base;
		float yaw = glm::degrees(std::atan2(to.x, to.z));
		guns.AddCannon(scene, "Cannon " + std::to_string(count + 1), pivot, yaw, CannonSettings(count));
		count++;
	}

	inline void Zone(Gunnery& guns, const Frame& f, float from, float to, float halfWidth = 4.0f, float top = 4.0f)
	{
		glm::vec3 a = f.At(from, -halfWidth, -1.0f), b = f.At(to, halfWidth, top);
		glm::vec3 c = f.At(from, halfWidth, -1.0f), d = f.At(to, -halfWidth, top);
		glm::vec3 mn = glm::min(glm::min(a, b), glm::min(c, d)), mx = glm::max(glm::max(a, b), glm::max(c, d));
		guns.AddNoFireZone(mn, mx);
	}

	inline float PieceLength(const Piece& p)
	{
		switch (p.Type)
		{
			case Kind::Start: return 8.0f;
			case Kind::Sprint: return 20.0f;
			case Kind::Vaults: return 16.0f;
			case Kind::RampDrop: return 24.0f;
			case Kind::Climb: return 12.0f;
			case Kind::Crates: return 18.0f;
			case Kind::Cover: return 26.0f;
			case Kind::Precision: return 13.0f;
			case Kind::Corner: return 8.0f;
			case Kind::Finish: return 12.0f;
		}
		return 10.0f;
	}

	// The frame the next piece starts in.
	inline Frame Next(const Frame& f, const Piece& p)
	{
		if (p.Type == Kind::Corner)
		{
			// Out of the plaza's side, half way along it: right turns the
			// heading to R, which is a yaw 90 degrees less.
			int turn = p.Side >= 0 ? 1 : -1;
			return Frame(f.At(4.0f, 4.0f * (float)turn, 0.0f), f.Yaw - 90.0f * (float)turn);
		}
		return Frame(f.At(PieceLength(p), 0.0f, 0.0f), f.Yaw);
	}

	// Lays one piece down in frame `f`.
	inline void BuildPiece(GS::Scene& scene, Gunnery& guns, const Frame& f, const Piece& p, Built& out, int& cannons)
	{
		using namespace Palette;
		float L = PieceLength(p);
		float c = (float)p.Side;
		const float half = 0.5f * Width;

		// The track under it, its edges, and the line where it begins.
		if (p.Type == Kind::Corner)
			Paint(scene, "Track", f, 0.0f, 8.0f, 0.0f, 8.0f, Track);
		else
		{
			Paint(scene, "Track", f, 0.0f, L, 0.0f, Width, Track);
			for (float side : { -1.0f, 1.0f })
				Paint(scene, "Track Edge", f, 0.0f, L, side * (half - 0.06f), 0.1f, Line, 0.006f);
		}

		switch (p.Type)
		{
			case Kind::Start:
			{
				out.Spawn = f.At(1.6f, 0.0f, 0.0f);
				out.SpawnYaw = glm::radians(f.Yaw);
				Paint(scene, "Start Line", f, 4.4f, 4.6f, 0.0f, Width, Line, 0.007f);
				GateArch(scene, f, 4.5f, glm::vec4(0.5f, 1.0f, 0.62f, 1.0f), false);
				out.Gates.push_back(Gate{ f.At(4.5f, 0.0f, 0.0f), f.F, half + 0.4f, "start" });
				break;
			}
			case Kind::Sprint:
			{
				if (p.Variant == 1 && c != 0.0f)
				{
					// Cover on the cannon's side: the track's edge out of its sight.
					for (float at : { 5.0f, 14.0f })
						Block(scene, "Cover Wall", f, at - 1.2f, at + 1.2f, c * (half + 0.55f), 0.5f, 0.0f, 2.0f, Cover);
				}
				if (c != 0.0f)
					PlaceCannon(scene, guns, f, 12.0f, c * 13.0f, 0.0f, 9.0f, cannons);
				break;
			}
			case Kind::Vaults:
			{
				for (float at : { 4.0f, 8.5f, 13.0f })
				{
					Block(scene, "Vault Wall", f, at - 0.15f, at + 0.15f, 0.0f, Width, 0.0f, 1.0f, Vault, 0.6f);
					// Black chevrons along its top edge.
					for (int k = 0; k < 6; k++)
						Block(scene, "Vault Stripe", f, at - 0.16f, at + 0.16f, -half + 0.5f + 1.0f * (float)k, 0.42f, 0.86f, 1.001f, Stripe, 0.6f);
				}
				Zone(guns, f, 1.2f, 15.6f);
				break;
			}
			case Kind::RampDrop:
			{
				float h = 1.5f, foot = 2.0f, top = 2.0f + h / std::tan(glm::radians(14.0f));
				Ramp(scene, f, foot, top, 0.0f, h, Width, Concrete);
				Block(scene, "Platform", f, top, 17.0f, 0.0f, Width, 0.0f, h, Concrete);
				Paint(scene, "Platform Track", f, top, 17.0f, 0.0f, Width - 0.4f, Track, h + 0.004f);
				Block(scene, "Edge Stripe", f, 16.75f, 17.0f, 0.0f, Width, h, h + 0.01f, Vault, 0.6f);
				if (c != 0.0f)
					PlaceCannon(scene, guns, f, 12.5f, c * 12.0f, h, 12.0f, cannons);
				Zone(guns, f, 15.0f, 21.0f);
				break;
			}
			case Kind::Climb:
			{
				Block(scene, "Ledge", f, 5.0f, 8.2f, 0.0f, Width, 0.0f, 1.6f, Climb);
				Block(scene, "Ledge Lip", f, 4.98f, 5.1f, 0.0f, Width, 1.48f, 1.6f, Vault, 0.6f);
				Zone(guns, f, 2.5f, 11.0f);
				break;
			}
			case Kind::Crates:
			{
				const float big[5][2] = { { 4.0f, -1.2f }, { 6.5f, 1.5f }, { 9.0f, -0.4f }, { 12.0f, 1.8f }, { 13.5f, -1.8f } };
				const float small[3][2] = { { 8.0f, 2.4f }, { 10.5f, 0.9f }, { 15.0f, 0.3f } };
				float flip = p.Variant == 1 ? -1.0f : 1.0f;
				for (const auto& b : big)
					Crate(scene, f.At(b[0], flip * b[1], 0.0f), 1.0f, 40.0f, f.Yaw + 9.0f * b[1]);
				for (const auto& b : small)
					Crate(scene, f.At(b[0], flip * b[1], 0.0f), 0.6f, 15.0f, f.Yaw - 13.0f * b[1]);
				if (c != 0.0f)
					PlaceCannon(scene, guns, f, 10.0f, c * 14.0f, 0.0f, 9.0f, cannons);
				break;
			}
			case Kind::Cover:
			{
				int k = 0;
				for (float at : { 4.0f, 8.5f, 13.0f, 17.5f, 22.0f })
				{
					float side = (k++ % 2 == 0 ? -1.0f : 1.0f) * (p.Variant == 1 ? -1.0f : 1.0f);
					Block(scene, "Cover Block", f, at - 0.45f, at + 0.45f, side * 1.5f, 1.4f, 0.0f, 2.0f, Cover);
					Block(scene, "Cover Cap", f, at - 0.47f, at + 0.47f, side * 1.5f, 1.44f, 2.0f, 2.08f, ConcreteDark);
				}
				PlaceCannon(scene, guns, f, 19.0f, 12.0f, 0.0f, 9.0f, cannons);
				PlaceCannon(scene, guns, f, 19.0f, -12.0f, 0.0f, 9.0f, cannons);
				break;
			}
			case Kind::Precision:
			{
				Block(scene, "Pillar Step", f, 1.5f, 2.5f, 0.0f, 1.4f, 0.0f, 1.0f, Concrete);
				for (float at : { 3.6f, 5.2f, 6.8f })
					Block(scene, "Pillar", f, at - 0.25f, at + 0.25f, 0.0f, 0.5f, 0.0f, 1.0f, Pillar, 0.6f);
				Block(scene, "Pillar Landing", f, 8.15f, 9.65f, 0.0f, 2.0f, 0.0f, 1.0f, Concrete);
				// A way round for anyone who would rather not: slower, open.
				Zone(guns, f, 0.5f, 11.0f, 1.6f);
				break;
			}
			case Kind::Corner:
			{
				int turn = p.Side >= 0 ? 1 : -1;
				// A cannon out beyond the corner's far side, looking back
				// down both arms of it.
				if (p.Variant == 1)
					PlaceCannon(scene, guns, f, 14.0f, -(float)turn * 9.0f, 0.0f, 2.0f, cannons);
				break;
			}
			case Kind::Finish:
			{
				Paint(scene, "Finish Line", f, 2.9f, 3.1f, 0.0f, Width, Line, 0.007f);
				GateArch(scene, f, 3.0f, glm::vec4(1.0f, 0.95f, 0.85f, 1.0f), true);
				out.Gates.push_back(Gate{ f.At(3.0f, 0.0f, 0.0f), f.F, half + 0.4f, "finish" });
				break;
			}
		}

		// A checkpoint where each piece after the start begins.
		if (p.Type != Kind::Start && p.Type != Kind::Finish && p.Type != Kind::Corner)
		{
			out.Gates.push_back(Gate{ f.At(0.2f, 0.0f, 0.0f), f.F, half + 0.4f, KindName(p.Type) });
			for (float side : { -1.0f, 1.0f })
			{
				GS::Entity post = Box(scene, "Checkpoint Post", f.At(0.2f, side * (half + 0.3f), 0.75f), { 0.12f, 1.5f, 0.12f }, Steel, 0.4f);
				GS::Entity cap = Box(scene, "Checkpoint Light", f.At(0.2f, side * (half + 0.3f), 1.58f), { 0.2f, 0.16f, 0.2f },
					glm::vec4(0.35f, 0.85f, 1.0f, 1.0f), 0.3f);
				cap.Get<GS::MeshComponent>()->Emission = 2.5f;
				(void)post;
			}
		}

		out.Length += p.Type == Kind::Corner ? 8.0f : L;
		for (float along : { 0.0f, L })
			for (float across : { -14.0f, 14.0f })
			{
				glm::vec3 q = f.At(along, across, 0.0f);
				out.Min = glm::min(out.Min, q);
				out.Max = glm::max(out.Max, q);
			}
	}

	// The ground a piece and its cannons take up, flat, for keeping a
	// course from running into itself.
	inline void Footprint(const Frame& f, const Piece& p, glm::vec2& mn, glm::vec2& mx)
	{
		float L = PieceLength(p);
		mn = glm::vec2(1e9f);
		mx = glm::vec2(-1e9f);
		for (float along : { 0.0f, L })
			for (float across : { -15.0f, 15.0f })
			{
				glm::vec3 q = f.At(along, across, 0.0f);
				mn = glm::min(mn, glm::vec2(q.x, q.z));
				mx = glm::max(mx, glm::vec2(q.x, q.z));
			}
	}

	// The one made by hand: the moves in an order that teaches them --
	// sprint under fire, vaults, round a corner, up a ramp and off the
	// end, a gauntlet between cover, crates, a climb, a last sprint home.
	inline Layout Gauntlet()
	{
		Layout l;
		l.Seed = 0;
		l.Pieces = {
			{ Kind::Start, 0, 0 },
			{ Kind::Sprint, 0, 1 },
			{ Kind::Vaults, 0, 0 },
			{ Kind::Corner, 1, -1 },
			{ Kind::RampDrop, 0, 1 },
			{ Kind::Cover, 0, 0 },
			{ Kind::Corner, 0, 1 },
			{ Kind::Crates, 0, -1 },
			{ Kind::Climb, 0, 0 },
			{ Kind::Sprint, 1, -1 },
			{ Kind::Finish, 0, 0 },
		};
		return l;
	}

	// A course from a seed: eight to eleven pieces between the start and the
	// finish, drawn by weight, never the same twice running, never two
	// committing pieces back to back without open ground between (where
	// the cannons are), turning where a corner comes up -- whichever way
	// keeps the track off itself.
	inline Layout Generate(uint32_t seed)
	{
		Layout l;
		l.Seed = seed;
		uint32_t state = seed * 2654435761u + 0x9e3779b9u;
		auto next = [&]()
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return state;
		};
		auto unit = [&]() { return (float)(next() & 0xffffffu) / (float)0x1000000; };
		struct Weighted { Kind K; float W; bool Committing; };
		const Weighted pool[] = {
			{ Kind::Sprint, 3.0f, false }, { Kind::Vaults, 2.0f, true }, { Kind::RampDrop, 2.0f, true }, { Kind::Climb, 1.5f, true },
			{ Kind::Crates, 2.0f, false }, { Kind::Cover, 2.0f, false }, { Kind::Precision, 0.8f, true }, { Kind::Corner, 2.0f, false },
		};
		int count = 8 + (int)(unit() * 4.0f);
		l.Pieces.push_back({ Kind::Start, 0, 0 });
		std::vector<std::pair<glm::vec2, glm::vec2>> taken;
		Frame f(glm::vec3(0.0f), 0.0f);
		f = Next(f, l.Pieces.back());
		{
			glm::vec2 mn, mx;
			Footprint(Frame(glm::vec3(0.0f), 0.0f), l.Pieces.back(), mn, mx);
			taken.push_back({ mn, mx });
		}
		auto overlaps = [&](const Frame& at, const Piece& p)
		{
			glm::vec2 mn, mx;
			Footprint(at, p, mn, mx);
			// Not against the two pieces just before, which it touches (and,
			// after a corner, sits beside), and shrunk a little.
			mn += glm::vec2(1.0f);
			mx -= glm::vec2(1.0f);
			for (size_t i = 0; i + 2 < taken.size(); i++)
				if (mn.x < taken[i].second.x && mx.x > taken[i].first.x && mn.y < taken[i].second.y && mx.y > taken[i].first.y)
					return true;
			return false;
		};
		Kind last = Kind::Start;
		bool lastCommitting = true;
		for (int i = 0; i < count; i++)
		{
			Piece p;
			for (int attempt = 0; attempt < 24; attempt++)
			{
				float total = 0.0f;
				for (const Weighted& w : pool)
					total += w.W;
				float pick = unit() * total;
				const Weighted* chosen = &pool[0];
				for (const Weighted& w : pool)
				{
					if (pick < w.W) { chosen = &w; break; }
					pick -= w.W;
				}
				if (chosen->K == last || (chosen->Committing && lastCommitting) || (chosen->K == Kind::Corner && (i == 0 || i == count - 1)))
					continue;
				p.Type = chosen->K;
				p.Variant = (int)(unit() * 2.0f);
				p.Side = unit() < 0.5f ? -1 : 1;
				if (p.Type == Kind::Vaults || p.Type == Kind::Climb || p.Type == Kind::Precision)
					p.Side = 0;
				if (p.Type == Kind::Corner)
				{
					// Whichever turn keeps clear of what is laid already.
					Piece probe = p;
					Frame after = Next(f, probe);
					Piece ahead{ Kind::Sprint, 0, 0 };
					if (overlaps(after, ahead))
					{
						probe.Side = -probe.Side;
						after = Next(f, probe);
						if (overlaps(after, ahead))
							continue;
					}
					p.Side = probe.Side;
				}
				if (overlaps(f, p))
					continue;
				break;
			}
			if (overlaps(f, p))
				break;   // boxed in: finish here
			l.Pieces.push_back(p);
			glm::vec2 mn, mx;
			Footprint(f, p, mn, mx);
			taken.push_back({ mn, mx });
			f = Next(f, p);
			last = p.Type;
			lastCommitting = false;
			for (const Weighted& w : pool)
				if (w.K == p.Type)
					lastCommitting = w.Committing;
		}
		l.Pieces.push_back({ Kind::Finish, 0, 0 });
		return l;
	}

	// The whole place: the field, the course, the cannons, trees about it,
	// the sky.
	inline Built Build(GS::Scene& scene, Gunnery& guns, const Layout& layout)
	{
		using namespace Palette;
		Built out;

		GS::Entity environment = scene.CreateEntity("Environment");
		GS::EnvironmentComponent env;
		env.SunAzimuth = 218.0f;
		env.SunElevation = 33.0f;
		env.SunColor = glm::vec3(1.0f, 0.9f, 0.76f);
		env.SunIntensity = 3.5f;
		env.SkyColor = glm::vec3(0.29f, 0.49f, 0.84f);
		env.HorizonColor = glm::vec3(0.87f, 0.85f, 0.8f);
		env.GroundColor = glm::vec3(0.3f, 0.32f, 0.22f);
		env.AmbientIntensity = 0.55f;
		env.FogDensity = 0.0055f;
		env.FogFalloff = 0.07f;
		env.Exposure = 1.05f;
		env.ShadowExtent = 28.0f;
		environment.Add<GS::EnvironmentComponent>(env);

		int cannons = 0;
		Frame f(glm::vec3(0.0f), 0.0f);
		for (const Piece& p : layout.Pieces)
		{
			BuildPiece(scene, guns, f, p, out, cannons);
			f = Next(f, p);
		}

		// The field, well past everything.
		glm::vec3 centre = 0.5f * (out.Min + out.Max);
		glm::vec3 span = out.Max - out.Min;
		float size = std::max(span.x, span.z) + 260.0f;
		Static(Box(scene, "Ground", { centre.x, -0.5f, centre.z }, { size, 1.0f, size }, Grass, 0.95f));

		// Trees, scattered about the course but never on it: anywhere in
		// a ring around its bounds that is more than 18 m from the track.
		std::vector<std::pair<Frame, float>> track;
		{
			Frame g(glm::vec3(0.0f), 0.0f);
			for (const Piece& p : layout.Pieces)
			{
				track.push_back({ g, PieceLength(p) });
				g = Next(g, p);
			}
		}
		auto nearTrack = [&](const glm::vec3& q)
		{
			for (const auto& t : track)
			{
				glm::vec3 d = q - t.first.O;
				float along = glm::dot(d, t.first.F), across = glm::dot(d, t.first.R);
				if (along > -18.0f && along < t.second + 18.0f && std::fabs(across) < 18.0f)
					return true;
			}
			return false;
		};
		uint32_t h = 0x51ed27u + layout.Seed * 7919u;
		int planted = 0;
		for (int k = 0; k < 900 && planted < 140; k++)
		{
			h = h * 1664525u + 1013904223u;
			float x = out.Min.x - 40.0f + (float)((h >> 8) & 0xffff) / 65535.0f * (span.x + 80.0f);
			h = h * 1664525u + 1013904223u;
			float z = out.Min.z - 40.0f + (float)((h >> 8) & 0xffff) / 65535.0f * (span.z + 80.0f);
			glm::vec3 q(x, 0.0f, z);
			if (nearTrack(q))
				continue;
			h = h * 1664525u + 1013904223u;
			Tree(scene, q, 5.0f + 4.0f * (float)((h >> 12) & 255u) / 255.0f, h);
			planted++;
		}
		return out;
	}
}
