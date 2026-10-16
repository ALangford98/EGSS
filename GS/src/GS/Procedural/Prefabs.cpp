#include "gspch.h"
#include "GS/Procedural/Prefabs.h"
#include "GS/Procedural/ModelBuilder.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <sstream>
#include <unordered_map>

namespace GS::Prefabs {

	using M = ModelBuilder;

	const Surface* FindSurface(const std::string& material)
	{
		// Colours are sRGB, as everything a scene names is; roughness and
		// metalness are the usual 0..1. Kept few and reused, the way a real
		// lab's are: one paint, one steel, one rubber.
		static const std::unordered_map<std::string, Surface> surfaces =
		{
			// The dummy. Skin takes the entity's colour -- crash-test yellow
			// in the demo -- so a scene can have a red one beside it.
			{ "dummy.skin",      { { 1.0f, 1.0f, 1.0f, 1.0f }, 0.42f, 0.0f, 0.0f, true } },
			{ "dummy.skin.dark", { { 0.78f, 0.78f, 0.78f, 1.0f }, 0.5f, 0.0f, 0.0f, true } },
			{ "dummy.rubber",    { { 0.06f, 0.06f, 0.065f, 1.0f }, 0.82f } },
			{ "dummy.joint",     { { 0.32f, 0.33f, 0.35f, 1.0f }, 0.3f, 0.9f } },
			{ "dummy.target.a",  { { 0.04f, 0.04f, 0.04f, 1.0f }, 0.6f } },
			{ "dummy.target.b",  { { 1.0f, 0.86f, 0.1f, 1.0f }, 0.45f } },
			{ "dummy.label",     { { 0.9f, 0.9f, 0.88f, 1.0f }, 0.55f } },

			// The cannon: safety-orange paint over steel.
			{ "cannon.paint",    { { 0.93f, 0.38f, 0.07f, 1.0f }, 0.38f, 0.15f } },
			{ "cannon.dark",     { { 0.13f, 0.14f, 0.15f, 1.0f }, 0.45f, 0.5f } },
			{ "cannon.steel",    { { 0.62f, 0.63f, 0.65f, 1.0f }, 0.24f, 1.0f } },
			{ "cannon.bore",     { { 0.02f, 0.02f, 0.02f, 1.0f }, 0.7f, 0.6f } },
			{ "cannon.tank",     { { 0.86f, 0.87f, 0.88f, 1.0f }, 0.3f, 0.1f } },
			{ "cannon.hose",     { { 0.07f, 0.07f, 0.08f, 1.0f }, 0.75f } },
			{ "hazard.yellow",   { { 0.98f, 0.78f, 0.06f, 1.0f }, 0.5f } },
			{ "hazard.black",    { { 0.05f, 0.05f, 0.05f, 1.0f }, 0.6f } },

			// The computer.
			{ "computer.case",   { { 0.2f, 0.21f, 0.23f, 1.0f }, 0.35f, 0.3f } },
			{ "computer.bezel",  { { 0.05f, 0.05f, 0.055f, 1.0f }, 0.3f, 0.1f } },
			{ "computer.key",    { { 0.16f, 0.17f, 0.18f, 1.0f }, 0.55f } },
			{ "computer.keycap", { { 0.85f, 0.42f, 0.1f, 1.0f }, 0.5f } },
			{ "computer.led",    { { 0.3f, 1.0f, 0.5f, 1.0f }, 0.3f, 0.0f, 6.0f } },
			{ "computer.led.red",{ { 1.0f, 0.2f, 0.15f, 1.0f }, 0.3f, 0.0f, 6.0f } },

			// The hall.
			{ "wood",            { { 0.62f, 0.44f, 0.25f, 1.0f }, 0.7f } },
			{ "wood.dark",       { { 0.42f, 0.28f, 0.15f, 1.0f }, 0.75f } },
			{ "concrete",        { { 0.62f, 0.62f, 0.6f, 1.0f }, 0.85f } },
			{ "concrete.floor",  { { 0.5f, 0.51f, 0.52f, 1.0f }, 0.35f } },
			{ "floor.seam",      { { 0.3f, 0.31f, 0.32f, 1.0f }, 0.6f } },
			{ "paint.wall",      { { 0.78f, 0.8f, 0.82f, 1.0f }, 0.6f } },
			{ "paint.wall.dark", { { 0.22f, 0.25f, 0.3f, 1.0f }, 0.55f } },
			{ "paint.blue",      { { 0.12f, 0.32f, 0.62f, 1.0f }, 0.45f, 0.1f } },
			{ "steel.beam",      { { 0.36f, 0.38f, 0.42f, 1.0f }, 0.4f, 0.85f } },
			{ "pad",             { { 0.1f, 0.24f, 0.55f, 1.0f }, 0.9f } },
			{ "pad.frame",       { { 0.15f, 0.16f, 0.18f, 1.0f }, 0.5f, 0.6f } },
			{ "light.housing",   { { 0.85f, 0.86f, 0.88f, 1.0f }, 0.4f, 0.5f } },
			{ "light.panel",     { { 1.0f, 0.97f, 0.9f, 1.0f }, 0.2f, 0.0f, 5.0f } },
			{ "lane.white",      { { 0.92f, 0.92f, 0.9f, 1.0f }, 0.5f } },
			{ "glass.sky",       { { 0.75f, 0.85f, 0.95f, 1.0f }, 0.1f, 0.0f, 1.2f } },
			{ "screen",          { { 1.0f, 1.0f, 1.0f, 1.0f }, 0.25f } },
		};
		auto found = surfaces.find(material);
		return found == surfaces.end() ? nullptr : &found->second;
	}

	void CannonScreen(glm::vec3& centre, glm::vec2& half, glm::vec3& facing)
	{
		float tilt = glm::radians(10.0f);
		facing = glm::vec3(0.0f, std::sin(tilt), -std::cos(tilt));
		centre = glm::vec3(-1.05f, 1.36f, -0.66f) + facing * 0.033f;
		half = glm::vec2(0.285f, 0.178f);
	}

	// ---- the crash-test dummy ------------------------------------------------
	//
	// Built over the ragdoll's own colliders, part for part, in each body's
	// frame -- +y up the limb, +z his front -- so what is drawn is what hits
	// things, only better looking. The calibration targets are the black and
	// yellow quartered discs real dummies carry, for the cameras that track
	// them.

	static void Target(M& b, const glm::mat4& at, float radius)
	{
		b.Push(at);
		for (int q = 0; q < 4; q++)
		{
			b.Material(q % 2 == 0 ? "dummy.target.a" : "dummy.target.b");
			b.Sector(radius, 90.0f * q, 90.0f * (q + 1), 6);
		}
		b.Material("dummy.target.a");
		b.Push(M::At({ 0.0f, 0.0f, -0.002f }));
		b.Sector(radius * 1.12f, 0.0f, 360.0f, 24);   // the dark rim
		b.Pop();
		b.Pop();
	}

	static void DummyHead(M& b)
	{
		// An egg, slightly taller than wide, over the 0.12 m sphere.
		b.Material("dummy.skin");
		std::vector<glm::vec2> skull;
		for (int k = 0; k <= 16; k++)
		{
			float t = glm::pi<float>() * (float)k / 16;
			float wide = 0.108f * std::sin(t) * (1.0f + 0.06f * std::sin(t * 2.0f));
			skull.push_back({ wide, -0.122f * std::cos(t) + 0.004f });
		}
		b.Lathe(skull, 28, 180.0f);
		// The face: a plate, a nose ridge and a chin, and the brow.
		b.Push(M::At({ 0.0f, -0.005f, 0.078f }, { -6.0f, 0.0f, 0.0f }));
		b.RoundedBox({ 0.07f, 0.075f, 0.028f }, 0.022f, 3);
		b.Pop();
		b.Push(M::At({ 0.0f, -0.002f, 0.107f }));
		b.RoundedBox({ 0.011f, 0.03f, 0.014f }, 0.009f, 2);
		b.Pop();
		b.Push(M::At({ 0.0f, -0.085f, 0.055f }));
		b.RoundedBox({ 0.052f, 0.03f, 0.04f }, 0.02f, 3);
		b.Pop();
		b.Material("dummy.skin.dark");
		b.Push(M::At({ 0.0f, 0.045f, 0.095f }));
		b.RoundedBox({ 0.068f, 0.012f, 0.012f }, 0.01f, 2);
		b.Pop();
		// Eyes: two dark dots, not a face to read emotion in.
		b.Material("dummy.rubber");
		for (float x : { -0.035f, 0.035f })
		{
			b.Push(M::At({ x, 0.022f, 0.104f }, { -6.0f, 0.0f, 0.0f }));
			b.Sector(0.011f, 0.0f, 360.0f, 12);
			b.Pop();
		}
		// Ear bolts and the side targets.
		b.Material("dummy.joint");
		for (float side : { -1.0f, 1.0f })
		{
			b.Push(M::At({ side * 0.104f, 0.0f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.022f, 0.012f, 12, 0.004f);
			b.Pop();
			Target(b, M::At({ side * 0.1f, 0.06f, -0.03f }, { 0.0f, side * 90.0f, 0.0f }) , 0.026f);
		}
		Target(b, M::At({ 0.0f, 0.1f, 0.03f }, { -70.0f, 0.0f, 0.0f }), 0.024f);
	}

	static void DummyTorso(M& b)
	{
		// The ribcage block over the 0.17 x 0.22 x 0.11 box.
		b.Material("dummy.skin");
		b.Push(M::At({ 0.0f, 0.01f, 0.0f }));
		b.RoundedBox({ 0.162f, 0.2f, 0.104f }, 0.06f, 4);
		b.Pop();
		// The chest plate and its ribs.
		b.Material("dummy.rubber");
		b.Push(M::At({ 0.0f, 0.03f, 0.096f }));
		b.RoundedBox({ 0.115f, 0.125f, 0.018f }, 0.014f, 3);
		b.Pop();
		b.Material("dummy.skin.dark");
		for (int k = 0; k < 4; k++)
		{
			b.Push(M::At({ 0.0f, -0.055f + 0.05f * k, 0.114f }));
			b.RoundedBox({ 0.1f, 0.007f, 0.006f }, 0.005f, 2);
			b.Pop();
		}
		// A label plate and the spine.
		b.Material("dummy.label");
		b.Push(M::At({ 0.0f, -0.145f, 0.1f }));
		b.RoundedBox({ 0.07f, 0.02f, 0.006f }, 0.004f, 2);
		b.Pop();
		b.Material("dummy.joint");
		b.Push(M::At({ 0.0f, 0.0f, -0.1f }));
		b.RoundedBox({ 0.05f, 0.16f, 0.012f }, 0.01f, 2);
		b.Pop();
		for (int k = 0; k < 4; k++)
			for (float x : { -0.032f, 0.032f })
			{
				b.Push(M::At({ x, -0.12f + 0.08f * k, -0.113f }, { 90.0f, 0.0f, 0.0f }));
				b.Cylinder(0.008f, 0.004f, 8);
				b.Pop();
			}
		// Shoulder pucks, and the neck: rubber discs between steel.
		for (float side : { -1.0f, 1.0f })
		{
			b.Material("dummy.joint");
			b.Push(M::At({ side * 0.168f, 0.155f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.058f, 0.018f, 20, 0.006f);
			b.Pop();
			Target(b, M::At({ side * 0.163f, 0.0f, 0.0f }, { 0.0f, side * 90.0f, 0.0f }), 0.042f);
		}
		for (int k = 0; k < 4; k++)
		{
			b.Material(k % 2 == 0 ? "dummy.rubber" : "dummy.joint");
			b.Push(M::At({ 0.0f, 0.205f + 0.016f * k, -0.01f }));
			b.Cylinder(k % 2 == 0 ? 0.044f : 0.036f, 0.008f, 16, 0.003f);
			b.Pop();
		}
	}

	static void DummyPelvis(M& b)
	{
		b.Material("dummy.skin");
		b.RoundedBox({ 0.132f, 0.084f, 0.094f }, 0.045f, 4);
		b.Material("dummy.rubber");
		b.Push(M::At({ 0.0f, 0.062f, 0.0f }));
		b.RoundedBox({ 0.138f, 0.018f, 0.1f }, 0.014f, 3);
		b.Pop();
		b.Material("dummy.joint");
		b.Push(M::At({ 0.0f, 0.062f, 0.101f }));
		b.RoundedBox({ 0.026f, 0.02f, 0.006f }, 0.004f, 2);   // the buckle
		b.Pop();
		for (float side : { -1.0f, 1.0f })
		{
			b.Material("dummy.joint");
			b.Push(M::At({ side * 0.1f, -0.07f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.05f, 0.035f, 18, 0.006f);
			b.Pop();
			Target(b, M::At({ side * 0.133f, 0.0f, 0.0f }, { 0.0f, side * 90.0f, 0.0f }), 0.034f);
		}
	}

	static void DummyFoot(M& b)
	{
		// A boot over the 0.06 x 0.05 x 0.11 box: upper, sole, toe cap.
		b.Material("dummy.rubber");
		b.Push(M::At({ 0.0f, 0.006f, 0.0f }));
		b.RoundedBox({ 0.056f, 0.042f, 0.104f }, 0.03f, 3);
		b.Pop();
		b.Material("dummy.skin.dark");
		b.Push(M::At({ 0.0f, -0.043f, 0.002f }));
		b.RoundedBox({ 0.06f, 0.008f, 0.11f }, 0.006f, 2);
		b.Pop();
		b.Material("dummy.joint");
		b.Push(M::At({ 0.0f, 0.03f, -0.07f }));
		b.Cylinder(0.04f, 0.02f, 16, 0.006f);   // the ankle socket
		b.Pop();
	}

	// A segment of limb over a capsule of `r` and `h` along y: a tapered
	// shell between two steel joint balls, with what is particular to each
	// limb -- a knee pad, a hand.
	static void DummyLimb(M& b, const std::string& kind, float r, float h)
	{
		float topScale = kind == "thigh" ? 1.0f : kind == "shin" ? 0.95f : kind == "upperarm" ? 1.0f : 0.95f;
		float bottomScale = kind == "thigh" ? 0.82f : kind == "shin" ? 0.72f : kind == "upperarm" ? 0.85f : 0.75f;
		b.Material("dummy.skin");
		std::vector<glm::vec2> shell =
		{
			{ r * 0.5f * bottomScale, -h - r * 0.42f },
			{ r * 0.86f * bottomScale, -h - r * 0.1f },
			{ r * 0.97f * bottomScale, -h + r * 0.3f },
			{ r * (bottomScale + topScale) * 0.5f, 0.0f },
			{ r * 0.98f * topScale, h - r * 0.3f },
			{ r * 0.88f * topScale, h + r * 0.1f },
			{ r * 0.5f * topScale, h + r * 0.42f },
		};
		b.Lathe(shell, 22, 180.0f);
		// The seam rings a moulded limb has.
		b.Material("dummy.skin.dark");
		for (float y : { -h * 0.45f, h * 0.45f })
		{
			b.Push(M::At({ 0.0f, y, 0.0f }));
			float scale = y < 0.0f ? bottomScale : topScale;
			b.Torus(r * 0.95f * (scale + 1.0f) * 0.5f, r * 0.05f, 22, 6);
			b.Pop();
		}
		b.Material("dummy.joint");
		for (float y : { -h, h })
		{
			b.Push(M::At({ 0.0f, y, 0.0f }));
			b.Sphere(r * 0.72f, 16, 10);
			b.Pop();
		}
		if (kind == "shin")
		{
			b.Material("dummy.rubber");
			b.Push(M::At({ 0.0f, h - r * 0.2f, r * 0.62f }, { -8.0f, 0.0f, 0.0f }));
			b.RoundedBox({ r * 0.55f, r * 0.75f, r * 0.3f }, r * 0.25f, 3);
			b.Pop();
		}
		if (kind == "forearm")
		{
			// A mitten hand at the wrist, palm in, thumb forward.
			b.Material("dummy.skin");
			b.Push(M::At({ 0.0f, -h - r * 2.0f, 0.0f }));
			b.RoundedBox({ r * 0.48f, r * 1.25f, r * 0.85f }, r * 0.4f, 3);
			b.Pop();
			b.Push(M::At({ 0.0f, -h - r * 1.4f, r * 0.85f }, { 30.0f, 0.0f, 0.0f }));
			b.RoundedBox({ r * 0.3f, r * 0.6f, r * 0.3f }, r * 0.25f, 2);
			b.Pop();
			b.Material("dummy.joint");
			b.Push(M::At({ 0.0f, -h - r * 0.7f, 0.0f }));
			b.Cylinder(r * 0.6f, r * 0.2f, 14, r * 0.05f);
			b.Pop();
		}
	}


	// The feminine build's trunk: the same plates and fittings, over a
	// chest that narrows to a waist, with a bust, and a pelvis that is
	// rounder at the hips. Sized for the default colliders like the rest;
	// the look scales them to the body it is drawn over.
	static void DummyTorsoFeminine(M& b)
	{
		b.Material("dummy.skin");
		b.Push(M::At({ 0.0f, 0.075f, 0.0f }));
		b.RoundedBox({ 0.158f, 0.13f, 0.1f }, 0.06f, 4);
		b.Pop();
		b.Push(M::At({ 0.0f, -0.11f, -0.005f }));
		b.RoundedBox({ 0.128f, 0.1f, 0.088f }, 0.05f, 4);   // the waist
		b.Pop();
		for (float x : { -0.062f, 0.062f })
		{
			b.Push(M::At({ x, 0.045f, 0.072f }) * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.9f, 0.75f)));
			b.Sphere(0.058f, 18, 12);
			b.Pop();
		}
		b.Material("dummy.rubber");
		b.Push(M::At({ 0.0f, -0.07f, 0.083f }));
		b.RoundedBox({ 0.085f, 0.05f, 0.012f }, 0.01f, 2);   // the abdomen insert
		b.Pop();
		b.Material("dummy.label");
		b.Push(M::At({ 0.0f, -0.16f, 0.083f }));
		b.RoundedBox({ 0.06f, 0.017f, 0.006f }, 0.004f, 2);
		b.Pop();
		b.Material("dummy.joint");
		b.Push(M::At({ 0.0f, 0.0f, -0.096f }));
		b.RoundedBox({ 0.045f, 0.16f, 0.012f }, 0.01f, 2);
		b.Pop();
		for (float side : { -1.0f, 1.0f })
		{
			b.Material("dummy.joint");
			b.Push(M::At({ side * 0.163f, 0.155f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.052f, 0.016f, 20, 0.006f);
			b.Pop();
			Target(b, M::At({ side * 0.155f, 0.06f, 0.0f }, { 0.0f, side * 90.0f, 0.0f }), 0.036f);
		}
		for (int k = 0; k < 4; k++)
		{
			b.Material(k % 2 == 0 ? "dummy.rubber" : "dummy.joint");
			b.Push(M::At({ 0.0f, 0.205f + 0.016f * k, -0.01f }));
			b.Cylinder(k % 2 == 0 ? 0.04f : 0.033f, 0.008f, 16, 0.003f);
			b.Pop();
		}
	}

	static void DummyPelvisFeminine(M& b)
	{
		b.Material("dummy.skin");
		b.Push(M::At({ 0.0f, 0.02f, 0.0f }));
		b.RoundedBox({ 0.118f, 0.066f, 0.09f }, 0.045f, 4);
		b.Pop();
		// The hips' curve: two rounded pads low on the sides.
		for (float x : { -0.085f, 0.085f })
		{
			b.Push(M::At({ x, -0.025f, -0.005f }) * glm::scale(glm::mat4(1.0f), glm::vec3(0.85f, 1.0f, 1.05f)));
			b.Sphere(0.075f, 18, 12);
			b.Pop();
		}
		b.Material("dummy.rubber");
		b.Push(M::At({ 0.0f, 0.066f, 0.0f }));
		b.RoundedBox({ 0.124f, 0.016f, 0.094f }, 0.012f, 3);
		b.Pop();
		for (float side : { -1.0f, 1.0f })
		{
			b.Material("dummy.joint");
			b.Push(M::At({ side * 0.1f, -0.07f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.046f, 0.03f, 18, 0.006f);
			b.Pop();
			Target(b, M::At({ side * 0.152f, -0.015f, 0.0f }, { 0.0f, side * 90.0f, 0.0f }), 0.03f);
		}
	}

	// ---- the cannon -----------------------------------------------------------
	//
	// Three parts, because two of them move: the base never does (and
	// carries the computer), the yoke turns about up, and the barrel turns
	// with it and tips about the trunnions. All three are built with their
	// own origin at the trunnion pivot's place in the base's frame --
	// 1.25 m up -- so the launcher only has to rotate, never offset. +z is
	// the way it fires.

	constexpr float Pivot = 1.25f;

	static void HazardEdge(M& b, const glm::vec3& from, const glm::vec3& to, float width, float height, int stripes)
	{
		glm::vec3 d = to - from;
		float length = glm::length(d);
		glm::vec3 along = d / length;
		float yaw = glm::degrees(std::atan2(along.x, along.z));
		for (int k = 0; k < stripes; k++)
		{
			b.Material(k % 2 == 0 ? "hazard.yellow" : "hazard.black");
			glm::vec3 at = from + along * (length * (k + 0.5f) / stripes);
			b.Push(M::At(at, { 0.0f, yaw, 0.0f }));
			b.RoundedBox({ width * 0.5f, height * 0.5f, length / stripes * 0.5f }, 0.004f, 1);
			b.Pop();
		}
	}

	static void CannonBase(M& b)
	{
		b.Push(M::At({ 0.0f, -Pivot, 0.0f }));

		// The floor plate, with its hazard edge and anchor bolts.
		b.Material("cannon.dark");
		b.Push(M::At({ 0.0f, 0.05f, 0.0f }));
		b.RoundedBox({ 0.8f, 0.05f, 0.8f }, 0.025f, 2);
		b.Pop();
		for (int side = 0; side < 4; side++)
		{
			float a = glm::half_pi<float>() * side;
			glm::vec3 c0(std::cos(a) * 0.79f - std::sin(a) * 0.79f, 0.104f, std::sin(a) * 0.79f + std::cos(a) * 0.79f);
			glm::vec3 c1(std::cos(a) * 0.79f + std::sin(a) * 0.79f, 0.104f, std::sin(a) * 0.79f - std::cos(a) * 0.79f);
			HazardEdge(b, c0, c1, 0.06f, 0.012f, 9);
		}
		b.Material("cannon.steel");
		for (float x : { -0.66f, 0.66f })
			for (float z : { -0.66f, 0.66f })
			{
				b.Push(M::At({ x, 0.115f, z }));
				b.Cylinder(0.035f, 0.016f, 6, 0.004f);
				b.Pop();
			}

		// The pedestal: a turned column, flared at the foot, banded.
		b.Material("cannon.paint");
		b.Lathe({ { 0.0f, 0.1f }, { 0.44f, 0.1f }, { 0.44f, 0.14f }, { 0.36f, 0.2f }, { 0.25f, 0.32f },
			{ 0.22f, 0.5f }, { 0.22f, 0.7f }, { 0.26f, 0.76f }, { 0.26f, 0.8f }, { 0.0f, 0.8f } }, 40, 25.0f);
		b.Material("cannon.dark");
		for (float y : { 0.42f, 0.62f })
		{
			b.Push(M::At({ 0.0f, y, 0.0f }));
			b.Torus(0.222f, 0.012f, 40, 8);
			b.Pop();
		}
		b.Material("cannon.steel");
		b.Push(M::At({ 0.0f, 0.82f, 0.0f }));
		b.Cylinder(0.3f, 0.02f, 40, 0.006f);   // the bearing race the yoke turns on
		b.Pop();

		// The air tank on its cradle, and the hose up to the pedestal.
		b.Material("cannon.tank");
		b.Push(M::At({ 0.5f, 0.32f, -0.15f }, { 90.0f, 0.0f, 0.0f }));
		b.Lathe({ { 0.0f, -0.42f }, { 0.08f, -0.41f }, { 0.13f, -0.37f }, { 0.15f, -0.3f }, { 0.15f, 0.3f },
			{ 0.13f, 0.37f }, { 0.08f, 0.41f }, { 0.0f, 0.42f } }, 28, 180.0f);
		b.Pop();
		b.Material("hazard.yellow");
		for (float z : { -0.35f, 0.05f })
		{
			b.Push(M::At({ 0.5f, 0.32f, z }, { 90.0f, 0.0f, 0.0f }));
			b.Torus(0.152f, 0.008f, 28, 6);
			b.Pop();
		}
		b.Material("cannon.dark");
		for (float z : { -0.38f, 0.08f })
		{
			b.Push(M::At({ 0.5f, 0.17f, z }));
			b.RoundedBox({ 0.13f, 0.06f, 0.03f }, 0.01f, 2);
			b.Pop();
		}
		b.Material("cannon.hose");
		b.Tube({ { 0.5f, 0.32f, 0.27f }, { 0.5f, 0.36f, 0.42f }, { 0.4f, 0.52f, 0.38f }, { 0.26f, 0.62f, 0.2f }, { 0.2f, 0.7f, 0.08f } }, 0.022f, 10);

		// The computer, on an arm from the plate's corner: a desk, a
		// keyboard, a monitor and the tower under it.
		b.Material("cannon.dark");
		b.Tube({ { -0.62f, 0.1f, -0.58f }, { -0.66f, 0.5f, -0.6f }, { -0.82f, 0.8f, -0.66f }, { -1.0f, 0.93f, -0.74f } }, 0.035f, 12);
		b.Material("computer.case");
		b.Push(M::At({ -1.05f, 0.95f, -0.8f }));
		b.RoundedBox({ 0.34f, 0.018f, 0.24f }, 0.012f, 2);   // the desk
		b.Pop();
		b.Push(M::At({ -1.05f, 0.982f, -0.9f }, { 6.0f, 0.0f, 0.0f }));
		b.RoundedBox({ 0.21f, 0.012f, 0.075f }, 0.008f, 2);   // the keyboard
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 13; col++)
			{
				bool accent = (row == 3 && col == 12) || (row == 0 && col == 0);
				b.Material(accent ? "computer.keycap" : "computer.key");
				float width = row == 3 && col == 6 ? 0.0f : 0.0125f;
				if (width == 0.0f)
					continue;
				b.Push(M::At({ -0.18f + col * 0.03f, 0.016f, -0.045f + row * 0.03f }));
				b.RoundedBox({ width, 0.006f, 0.0125f }, 0.004f, 1);
				b.Pop();
			}
		b.Material("computer.key");
		b.Push(M::At({ 0.0f, 0.016f, 0.045f }));
		b.RoundedBox({ 0.08f, 0.006f, 0.0125f }, 0.004f, 1);   // the space bar
		b.Pop();
		b.Pop();
		// A big red button beside it: FIRE.
		b.Material("cannon.dark");
		b.Push(M::At({ -0.78f, 0.985f, -0.92f }));
		b.Cylinder(0.04f, 0.018f, 20, 0.004f);
		b.Pop();
		b.Material("computer.led.red");
		b.Push(M::At({ -0.78f, 1.01f, -0.92f }));
		b.Cylinder(0.03f, 0.012f, 20, 0.008f);
		b.Pop();

		// The monitor: stand, bezel, and the glass the terminal is shown on
		// (the glass itself is the screen entity's; CannonScreen says where).
		b.Material("computer.case");
		b.Push(M::At({ -1.05f, 1.06f, -0.66f }));
		b.Cylinder(0.025f, 0.1f, 12);
		b.Pop();
		b.Push(M::At({ -1.05f, 0.975f, -0.66f }));
		b.Cylinder(0.11f, 0.008f, 24, 0.003f);
		b.Pop();
		b.Material("computer.bezel");
		b.Push(M::At({ -1.05f, 1.36f, -0.66f }, { -10.0f, 180.0f, 0.0f }));
		b.RoundedBox({ 0.31f, 0.2f, 0.028f }, 0.014f, 3);
		b.Push(M::At({ 0.0f, 0.0f, -0.035f }));
		b.RoundedBox({ 0.2f, 0.12f, 0.02f }, 0.02f, 2);   // its back
		b.Pop();
		b.Material("computer.led");
		b.Push(M::At({ 0.26f, -0.185f, 0.029f }));
		b.RoundedBox({ 0.006f, 0.003f, 0.002f }, 0.0015f, 1);
		b.Pop();
		b.Pop();

		// The tower, on the floor beside the plate, glowing a little.
		b.Material("computer.case");
		b.Push(M::At({ -1.32f, 0.24f, -0.42f }));
		b.RoundedBox({ 0.1f, 0.22f, 0.22f }, 0.012f, 2);
		b.Material("computer.bezel");
		for (int k = 0; k < 6; k++)
		{
			b.Push(M::At({ -0.101f, 0.05f + 0.025f * k, 0.0f }));
			b.RoundedBox({ 0.002f, 0.006f, 0.15f }, 0.002f, 1);
			b.Pop();
		}
		b.Material("computer.led");
		b.Push(M::At({ -0.101f, -0.12f, 0.12f }));
		b.RoundedBox({ 0.003f, 0.08f, 0.004f }, 0.002f, 1);
		b.Pop();
		b.Pop();
		b.Material("cannon.hose");
		b.Tube({ { -1.3f, 0.46f, -0.5f }, { -1.25f, 0.7f, -0.62f }, { -1.12f, 0.95f, -0.62f }, { -1.06f, 1.2f, -0.62f } }, 0.009f, 8, false);

		b.Pop();
	}

	static void CannonYoke(M& b)
	{
		// The turntable and the two arms holding the trunnions.
		b.Material("cannon.steel");
		b.Push(M::At({ 0.0f, -0.39f, 0.0f }));
		b.Cylinder(0.32f, 0.025f, 40, 0.008f);
		b.Pop();
		b.Material("cannon.paint");
		b.Push(M::At({ 0.0f, -0.33f, 0.0f }));
		b.RoundedBox({ 0.31f, 0.04f, 0.16f }, 0.02f, 3);
		b.Pop();
		for (float side : { -1.0f, 1.0f })
		{
			b.Material("cannon.paint");
			b.Push(M::At({ side * 0.27f, -0.14f, 0.0f }));
			b.RoundedBox({ 0.045f, 0.2f, 0.13f }, 0.03f, 3);
			b.Pop();
			b.Material("cannon.steel");
			b.Push(M::At({ side * 0.32f, 0.0f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.085f, 0.02f, 24, 0.006f);
			b.Pop();
			b.Material("cannon.dark");
			b.Push(M::At({ side * 0.345f, 0.0f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
			b.Cylinder(0.04f, 0.012f, 6, 0.003f);   // the trunnion nut
			b.Pop();
		}
		// A hydraulic ram on the right arm, for the look of it.
		b.Material("cannon.steel");
		b.Tube({ { 0.33f, -0.3f, -0.1f }, { 0.33f, -0.1f, -0.05f } }, 0.022f, 12);
		b.Material("cannon.dark");
		b.Tube({ { 0.33f, -0.36f, -0.115f }, { 0.33f, -0.22f, -0.08f } }, 0.03f, 12);
	}

	static void CannonBarrel(M& b)
	{
		// A turned barrel along +z: breech, chamber, reinforcing bands, a
		// tapering chase and a flared muzzle with its bore showing.
		b.Material("cannon.steel");
		b.Push(M::At({ 0.0f, 0.0f, 0.0f }, { 90.0f, 0.0f, 0.0f }));
		// Rotated so the lathe's +y is the barrel's +z: X by +90 takes
		// (0, 1, 0) to (0, 0, 1).
		b.Lathe({ { 0.0f, -0.56f }, { 0.12f, -0.56f }, { 0.16f, -0.52f }, { 0.165f, -0.3f }, { 0.15f, -0.2f },
			{ 0.15f, 0.1f }, { 0.135f, 0.14f }, { 0.122f, 0.6f }, { 0.11f, 0.9f }, { 0.122f, 0.94f },
			{ 0.135f, 1.02f }, { 0.13f, CannonMuzzle }, { 0.075f, CannonMuzzle } }, 40, 25.0f);
		b.Material("cannon.bore");
		b.Lathe({ { 0.075f, CannonMuzzle }, { 0.072f, CannonMuzzle - 0.02f }, { 0.072f, 0.6f }, { 0.0f, 0.55f } }, 24, 60.0f);
		b.Material("cannon.paint");
		for (float y : { -0.12f, 0.35f, 0.72f })
		{
			b.Push(M::At({ 0.0f, y, 0.0f }));
			b.Cylinder(0.165f - y * 0.03f, 0.03f, 40, 0.01f);
			b.Pop();
		}
		b.Pop();
		// The breech block, its handle, the trunnion pin through the middle.
		b.Material("cannon.dark");
		b.Push(M::At({ 0.0f, 0.0f, -0.6f }));
		b.RoundedBox({ 0.15f, 0.15f, 0.06f }, 0.025f, 3);
		b.Pop();
		b.Material("cannon.steel");
		b.Tube({ { 0.08f, 0.06f, -0.66f }, { 0.08f, 0.06f, -0.74f }, { -0.08f, 0.06f, -0.74f }, { -0.08f, 0.06f, -0.66f } }, 0.012f, 10);
		b.Push(M::At({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 90.0f }));
		b.Cylinder(0.055f, 0.3f, 20, 0.006f);
		b.Pop();
		// A sight on top, and a hazard band at the muzzle.
		b.Material("cannon.dark");
		b.Push(M::At({ 0.0f, 0.18f, -0.15f }));
		b.RoundedBox({ 0.022f, 0.03f, 0.08f }, 0.008f, 2);
		b.Pop();
		b.Material("computer.led.red");
		b.Push(M::At({ 0.0f, 0.212f, -0.08f }));
		b.RoundedBox({ 0.006f, 0.004f, 0.006f }, 0.003f, 1);
		b.Pop();
		b.Push(M::At({ 0.0f, 0.0f, 0.0f }, { 90.0f, 0.0f, 0.0f }));
		for (int k = 0; k < 8; k++)
		{
			b.Material(k % 2 == 0 ? "hazard.yellow" : "hazard.black");
			b.Push(M::At({ 0.0f, 0.98f, 0.0f }));
			b.Lathe({ { 0.137f, -0.025f }, { 0.138f, 0.025f } }, 6, 30.0f, 45.0f);
			b.Pop();
			b.Push(M::At({ 0.0f, 0.0f, 0.0f }, { 0.0f, 45.0f, 0.0f }));
		}
		for (int k = 0; k < 8; k++)
			b.Pop();
		b.Pop();
	}

	// ---- the hall -------------------------------------------------------------

	static void Crate(M& b)
	{
		// A 1 m crate, everything inside +-0.5 so its collider is its bounds:
		// boards, a frame on every edge, a brace across each face.
		b.Material("wood");
		b.RoundedBox(glm::vec3(0.47f), 0.01f, 1);
		b.Material("wood.dark");
		for (int a = 0; a < 3; a++)
			for (float s1 : { -1.0f, 1.0f })
				for (float s2 : { -1.0f, 1.0f })
				{
					glm::vec3 at(0.0f), half(0.05f);
					int u = (a + 1) % 3, v = (a + 2) % 3;
					at[u] = s1 * 0.45f;
					at[v] = s2 * 0.45f;
					half[a] = 0.5f;
					b.Push(M::At(at));
					b.RoundedBox(half, 0.012f, 1);
					b.Pop();
				}
		for (int a = 0; a < 3; a++)
			for (float s : { -1.0f, 1.0f })
			{
				glm::vec3 at(0.0f);
				at[a] = s * 0.48f;
				glm::vec3 euler(0.0f);
				glm::vec3 half(0.035f, 0.6f, 0.035f);
				// Lay the brace in the face's plane, on its diagonal.
				if (a == 0) { euler = { 45.0f, 0.0f, 0.0f }; half = { 0.02f, 0.6f, 0.05f }; }
				if (a == 1) { euler = { 0.0f, 45.0f, 90.0f }; half = { 0.02f, 0.6f, 0.05f }; }
				if (a == 2) { euler = { 0.0f, 0.0f, 45.0f }; half = { 0.05f, 0.6f, 0.02f }; }
				b.Push(M::At(at, euler));
				b.RoundedBox(half, 0.01f, 1);
				b.Pop();
			}
	}

	static void Barrier(M& b)
	{
		// A 2 m concrete crash barrier: the Jersey profile, extruded, with
		// a hazard band painted along the top.
		b.Material("concrete");
		std::vector<glm::vec2> profile = { { 0.3f, 0.0f }, { 0.3f, 0.08f }, { 0.16f, 0.25f }, { 0.08f, 0.8f },
			{ -0.08f, 0.8f }, { -0.16f, 0.25f }, { -0.3f, 0.08f }, { -0.3f, 0.0f } };
		b.Push(M::At({ 0.0f, -0.4f, 0.0f }, { 0.0f, 90.0f, 0.0f }));
		b.Extrude(profile, 2.0f);
		b.Pop();
		for (int k = 0; k < 8; k++)
		{
			b.Material(k % 2 == 0 ? "hazard.yellow" : "hazard.black");
			b.Push(M::At({ -0.875f + 0.25f * k, 0.3f, 0.0f }));
			b.RoundedBox({ 0.125f, 0.06f, 0.115f }, 0.004f, 1);
			b.Pop();
		}
	}

	static void PadWall(M& b)
	{
		// An impact wall 4 m wide and 2.4 m tall: a steel frame holding a
		// grid of padded cushions, centred, facing +z.
		b.Material("pad.frame");
		b.RoundedBox({ 2.0f, 1.2f, 0.06f }, 0.02f, 2);
		for (float x : { -2.0f, 2.0f })
		{
			b.Push(M::At({ x, 0.0f, 0.0f }));
			b.RoundedBox({ 0.08f, 1.2f, 0.15f }, 0.02f, 2);
			b.Pop();
		}
		b.Material("pad");
		for (int row = 0; row < 5; row++)
			for (int col = 0; col < 8; col++)
			{
				b.Push(M::At({ -1.75f + col * 0.5f, -0.96f + row * 0.48f, 0.12f }));
				b.RoundedBox({ 0.235f, 0.225f, 0.07f }, 0.06f, 3);
				b.Pop();
			}
		b.Material("hazard.yellow");
		b.Push(M::At({ 0.0f, 1.22f, 0.05f }));
		b.RoundedBox({ 2.0f, 0.03f, 0.1f }, 0.01f, 1);
		b.Pop();
	}

	static void LightPanel(M& b)
	{
		// A ceiling fixture 1.2 x 0.4 m: housing with the glowing diffuser
		// facing down.
		b.Material("light.housing");
		b.RoundedBox({ 0.62f, 0.05f, 0.22f }, 0.02f, 2);
		b.Material("light.panel");
		b.Push(M::At({ 0.0f, -0.052f, 0.0f }, { 90.0f, 0.0f, 0.0f }));
		b.Quad({ 0.57f, 0.17f });
		b.Pop();
		b.Material("steel.beam");
		for (float x : { -0.45f, 0.45f })
			b.Tube({ { x, 0.05f, 0.0f }, { x, 0.8f, 0.0f } }, 0.006f, 6, false);
	}

	static void Truss(M& b)
	{
		// A 10 m roof truss along x: two chords and a zigzag of struts.
		b.Material("steel.beam");
		const float length = 10.0f, depth = 0.6f;
		b.Push(M::At({ 0.0f, depth * 0.5f, 0.0f }));
		b.RoundedBox({ length * 0.5f, 0.05f, 0.08f }, 0.01f, 1);
		b.Pop();
		b.Push(M::At({ 0.0f, -depth * 0.5f, 0.0f }));
		b.RoundedBox({ length * 0.5f, 0.05f, 0.08f }, 0.01f, 1);
		b.Pop();
		int bays = 16;
		for (int k = 0; k < bays; k++)
		{
			float x0 = -length * 0.5f + length * k / bays, x1 = -length * 0.5f + length * (k + 1) / bays;
			float y0 = k % 2 == 0 ? -depth * 0.5f : depth * 0.5f;
			b.Tube({ { x0, y0, 0.0f }, { x1, -y0, 0.0f } }, 0.025f, 8, false);
		}
	}

	static void WallPanel(M& b)
	{
		// A 4 m x 6 m wall bay facing +z: painted panel, a dado band, ribs.
		b.Material("paint.wall");
		b.Push(M::At({ 0.0f, 3.0f, 0.0f }));
		b.RoundedBox({ 2.0f, 3.0f, 0.1f }, 0.01f, 1);
		b.Pop();
		b.Material("paint.wall.dark");
		b.Push(M::At({ 0.0f, 0.6f, 0.02f }));
		b.RoundedBox({ 2.0f, 0.6f, 0.1f }, 0.01f, 1);
		b.Pop();
		b.Material("hazard.yellow");
		b.Push(M::At({ 0.0f, 1.22f, 0.03f }));
		b.RoundedBox({ 2.0f, 0.025f, 0.1f }, 0.005f, 1);
		b.Pop();
		b.Material("steel.beam");
		for (float x : { -1.96f, 1.96f })
		{
			b.Push(M::At({ x, 3.0f, 0.1f }));
			b.RoundedBox({ 0.04f, 3.0f, 0.06f }, 0.01f, 1);
			b.Pop();
		}
		b.Material("paint.wall.dark");
		for (float y : { 2.6f, 4.1f })
		{
			b.Push(M::At({ 0.0f, y, 0.1f }));
			b.RoundedBox({ 1.92f, 0.012f, 0.01f }, 0.004f, 1);
			b.Pop();
		}
	}

	static void Pillar(M& b)
	{
		// A 6 m steel column, I-section, on a base plate with bolts.
		b.Material("steel.beam");
		for (float z : { -0.12f, 0.12f })
		{
			b.Push(M::At({ 0.0f, 3.0f, z }));
			b.RoundedBox({ 0.13f, 3.0f, 0.015f }, 0.004f, 1);
			b.Pop();
		}
		b.Push(M::At({ 0.0f, 3.0f, 0.0f }));
		b.RoundedBox({ 0.012f, 3.0f, 0.12f }, 0.004f, 1);
		b.Pop();
		b.Material("hazard.yellow");
		for (int k = 0; k < 4; k++)
		{
			b.Material(k % 2 == 0 ? "hazard.yellow" : "hazard.black");
			b.Push(M::At({ 0.0f, 0.2f + 0.3f * k, 0.0f }));
			b.RoundedBox({ 0.14f, 0.15f, 0.14f }, 0.006f, 1);
			b.Pop();
		}
		b.Material("cannon.dark");
		b.Push(M::At({ 0.0f, 0.02f, 0.0f }));
		b.RoundedBox({ 0.25f, 0.02f, 0.25f }, 0.005f, 1);
		b.Pop();
	}

	static void FloorGrid(M& b)
	{
		// Saw-cut joints every 2 m across a 30 x 40 m slab, as one model.
		b.Material("floor.seam");
		for (int i = -7; i <= 7; i++)
		{
			b.Push(M::At({ i * 2.0f, 0.002f, 0.0f }));
			b.RoundedBox({ 0.012f, 0.002f, 20.0f }, 0.0f, 1);
			b.Pop();
		}
		for (int k = -10; k <= 10; k++)
		{
			b.Push(M::At({ 0.0f, 0.002f, k * 2.0f }));
			b.RoundedBox({ 15.0f, 0.002f, 0.012f }, 0.0f, 1);
			b.Pop();
		}
	}

	static void Lane(M& b)
	{
		// The test lane, 3 m wide and 14 m long from the dummy's mark (z = 0)
		// to the cannon: hazard-striped edges, distance bars every metre,
		// and the mark itself -- a quartered circle to stand on.
		for (float x : { -1.5f, 1.5f })
			for (int k = 0; k < 28; k++)
			{
				b.Material(k % 2 == 0 ? "hazard.yellow" : "hazard.black");
				b.Push(M::At({ x, 0.004f, -1.0f + 0.5f * k + 0.25f }));
				b.RoundedBox({ 0.06f, 0.003f, 0.25f }, 0.0f, 1);
				b.Pop();
			}
		b.Material("lane.white");
		for (int k = 1; k <= 10; k++)
		{
			float width = k % 5 == 0 ? 0.5f : 0.25f;
			for (float x : { -1.3f, 1.3f })
			{
				b.Push(M::At({ x + (x < 0.0f ? width * 0.5f : -width * 0.5f), 0.004f, (float)k }));
				b.RoundedBox({ width * 0.5f, 0.003f, 0.025f }, 0.0f, 1);
				b.Pop();
			}
		}
		b.Push(M::At({ 0.0f, 0.005f, 0.0f }, { -90.0f, 0.0f, 0.0f }));
		for (int q = 0; q < 4; q++)
		{
			b.Material(q % 2 == 0 ? "hazard.yellow" : "hazard.black");
			b.Sector(0.5f, 90.0f * q, 90.0f * (q + 1), 10);
		}
		b.Pop();
		b.Material("lane.white");
		b.Push(M::At({ 0.0f, 0.0045f, 0.0f }));
		b.Torus(0.56f, 0.035f, 40, 3);
		b.Pop();
	}

	static void Roof(M& b)
	{
		// The roof over a 30 x 40 m hall: deck panels with three long
		// skylights down the length, the glass glowing faintly from the sky
		// above. Built facing down; the skylights are gaps the sun comes
		// through.
		b.Material("paint.wall.dark");
		const float halfX = 15.0f, halfZ = 20.0f;
		float lights[3] = { -7.0f, 0.0f, 7.0f };
		float x = -halfX;
		for (int k = 0; k <= 3; k++)
		{
			float end = k < 3 ? lights[k] - 1.0f : halfX;
			b.Push(M::At({ (x + end) * 0.5f, 0.0f, 0.0f }));
			b.RoundedBox({ (end - x) * 0.5f, 0.1f, halfZ }, 0.0f, 1);
			b.Pop();
			if (k < 3)
				x = lights[k] + 1.0f;
		}
		b.Material("steel.beam");
		for (float l : lights)
			for (float side : { -1.0f, 1.0f })
			{
				b.Push(M::At({ l + side * 1.0f, -0.15f, 0.0f }));
				b.RoundedBox({ 0.06f, 0.08f, halfZ }, 0.01f, 1);
				b.Pop();
			}
	}

	bool Build(const std::string& name, MeshData& out)
	{
		ModelBuilder b;
		if (name == "dummy.head") DummyHead(b);
		else if (name == "dummy.torso") DummyTorso(b);
		else if (name == "dummy.pelvis") DummyPelvis(b);
		else if (name == "dummy.foot") DummyFoot(b);
		else if (name == "dummy.torso.f") DummyTorsoFeminine(b);
		else if (name == "dummy.pelvis.f") DummyPelvisFeminine(b);
		else if (name.rfind("dummy.limb:", 0) == 0)
		{
			// dummy.limb:<kind>:<radius>:<halfHeight>
			std::string rest = name.substr(11);
			std::replace(rest.begin(), rest.end(), ':', ' ');
			std::istringstream fields(rest);
			std::string kind;
			float r = 0.0f, h = 0.0f;
			if (!(fields >> kind >> r >> h) || r <= 0.0f)
				return false;
			DummyLimb(b, kind, r, h);
		}
		else if (name == "cannon.base") CannonBase(b);
		else if (name == "cannon.yoke") CannonYoke(b);
		else if (name == "cannon.barrel") CannonBarrel(b);
		else if (name == "screen") { b.Material("screen"); b.Quad({ 0.5f, 0.5f }); }
		else if (name == "crate") Crate(b);
		else if (name == "barrier") Barrier(b);
		else if (name == "padwall") PadWall(b);
		else if (name == "lightpanel") LightPanel(b);
		else if (name == "truss") Truss(b);
		else if (name == "wallpanel") WallPanel(b);
		else if (name == "pillar") Pillar(b);
		else if (name == "floorgrid") FloorGrid(b);
		else if (name == "lane") Lane(b);
		else if (name == "roof") Roof(b);
		else
			return false;
		out = b.Build();
		return !out.Indices.empty();
	}

}
