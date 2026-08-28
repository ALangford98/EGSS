#pragma once

// **Blades of grass, as geometry, scattered over a mesh that already exists.**
//
// Lifted out of `OpenWorld` so the planet can have grass too. That move is most
// of the work: the original assumed `+Y` was up, in four separate places, which
// is true on a flat world and false everywhere on a sphere. Nothing here knows
// which way up is -- it asks.
//
// **Why this is a template and not a pair of `std::function`s.** The two
// callbacks are invoked once per terrain triangle, which for a stride-1 chunk
// is a few thousand times per chunk and tens of thousands of times a second
// while chunks are streaming in. A `std::function` is an indirect call through
// a pointer the optimiser cannot see, so it cannot inline the body and cannot
// hoist anything out of the loop around it. A template parameter is resolved at
// compile time, so `up(p)` on a flat world compiles to loading a constant and
// the call disappears entirely.
//
// The cost of that choice is real and worth naming: a template has to live in a
// header, so every translation unit that uses it compiles its own copy, and
// changing this file recompiles all of them. That is a build-time cost, not a
// runtime one. The rule of thumb this project can use: **things called in a
// loop are worth a template; things called once are worth a `.cpp`.**

#include <Egss.h>

#include "Vegetation.h"

#include <glm/glm.hpp>

#include <cmath>

namespace Grass {

	struct Settings
	{
		// Blades per qualifying triangle. Fractional, and honestly so -- see
		// the note at the count below.
		float Density = 0.6f;

		float Height = 0.42f;
		float Width = 0.045f;

		// How far the tip leans off vertical, as a share of the blade's
		// height. Upright blades read as spikes and a field of them looks like
		// a bed of nails.
		float Lean = 0.35f;

		// The slope a blade will still stand on, as the cosine between the
		// face and up. Ramped rather than a cutoff so a hillside thins out
		// instead of ending on a line.
		float FlatLow = 0.55f;
		float FlatHigh = 0.88f;

		unsigned int Seed = 977u;
	};

	// `up` returns the local vertical at a point, in the mesh's own frame.
	// `allow` returns 0..1 for how much grass belongs at a point, given the
	// point and its face normal -- the caller's biome test, whatever that is.
	//
	// One triangle a blade. A quad would be two triangles for a shape nobody
	// can distinguish at the size these are drawn, and grass is the one thing
	// here where the count is the cost.
	//
	// Placed on the terrain's own triangles rather than on a grid, so blades
	// follow the ground exactly and inherit the mesh's density -- more
	// triangles where the surface is busier is also where more grass looks
	// right.
	template <typename Up, typename Allow>
	Egss::MeshData Build(const Egss::MeshData& terrain, const Settings& settings,
		unsigned int chunkSeed, Up up, Allow allow)
	{
		Egss::MeshData grass;

		if (settings.Density <= 0.0f || terrain.Indices.size() < 3)
			return grass;

		size_t triangles = terrain.Indices.size() / 3;
		unsigned int seed = settings.Seed + chunkSeed;

		for (size_t t = 0; t < triangles; t++)
		{
			const glm::vec3& a = terrain.Vertices[terrain.Indices[t * 3 + 0]].Position;
			const glm::vec3& b = terrain.Vertices[terrain.Indices[t * 3 + 1]].Position;
			const glm::vec3& c = terrain.Vertices[terrain.Indices[t * 3 + 2]].Position;

			glm::vec3 centre = (a + b + c) / 3.0f;

			glm::vec3 face = glm::cross(b - a, c - a);
			float area2 = glm::length(face);

			if (area2 < 1e-8f)
				continue;

			glm::vec3 n = face / area2;
			glm::vec3 vertical = up(centre);

			// A face pointing into the ground is the mesher's winding, not a
			// cliff -- take the side that agrees with up.
			if (glm::dot(n, vertical) < 0.0f)
				n = -n;

			float flatness = glm::smoothstep(settings.FlatLow, settings.FlatHigh,
				glm::dot(n, vertical));

			float chance = allow(centre, n) * flatness * settings.Density;

			if (chance <= 0.001f)
				continue;

			// Fractional density done honestly: the whole part is a guaranteed
			// count and the remainder is a threshold, so 0.3 gives roughly
			// three blades every ten triangles rather than none.
			int count = (int)chance;

			if (Veg::Hash2DUnit((int)t, 0, seed) < chance - (float)count)
				count++;

			for (int i = 0; i < count; i++)
			{
				// Uniform inside the triangle: the sqrt is what stops the
				// points bunching along one edge.
				float u = Veg::Hash2DUnit((int)t, i * 3 + 1, seed);
				float v = Veg::Hash2DUnit((int)t, i * 3 + 2, seed);
				float su = std::sqrt(u);

				glm::vec3 base = a + (b - a) * (su * (1.0f - v))
					+ (c - a) * (su * v);

				// **A frame on the ground, not in the world.** The original
				// built the blade's width along world x and z, which is only
				// the ground plane if up is +Y. Two axes across the local
				// vertical work anywhere, and reduce to the old ones exactly
				// when the vertical is +Y.
				glm::vec3 vert = up(base);

				glm::vec3 reference = std::abs(vert.y) < 0.9f
					? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

				glm::vec3 east = glm::normalize(glm::cross(reference, vert));
				glm::vec3 north = glm::cross(vert, east);

				float angle = Veg::Hash2DUnit((int)t, i * 3 + 3, seed) * 6.2831853f;

				float height = settings.Height
					* (0.65f + Veg::Hash2DUnit((int)t, i * 3 + 4, seed) * 0.7f);

				glm::vec3 side = (east * std::cos(angle) + north * std::sin(angle))
					* settings.Width;

				// Leaning, and leaning the same way per blade.
				glm::vec3 lean = (east * std::cos(angle + 1.57f)
					+ north * std::sin(angle + 1.57f)) * (height * settings.Lean);

				glm::vec3 tip = base + vert * height + lean;

				// Facing the lean, so a blade catches the light on its face
				// rather than edge-on.
				glm::vec3 bladeNormal = glm::normalize(
					glm::cross(side * 2.0f, tip - (base - side)));

				if (glm::dot(bladeNormal, vert) < 0.0f)
					bladeNormal = -bladeNormal;

				unsigned int at = (unsigned int)grass.Vertices.size();

				// **The texture coordinate carries height up the blade**, 0 at
				// the root and 1 at the tip, which is what lets the shader bend
				// it in the wind without knowing anything about where the root
				// is. Same trick the trees use, one dimension smaller.
				grass.Vertices.push_back({ base - side, bladeNormal, { 0.0f, 0.0f } });
				grass.Vertices.push_back({ base + side, bladeNormal, { 1.0f, 0.0f } });
				grass.Vertices.push_back({ tip,         bladeNormal, { 0.5f, 1.0f } });

				grass.Indices.push_back(at);
				grass.Indices.push_back(at + 1);
				grass.Indices.push_back(at + 2);
			}
		}

		if (grass.Indices.empty())
			return grass;

		Egss::Submesh all;
		all.IndexCount = (unsigned int)grass.Indices.size();

		grass.Submeshes.push_back(all);
		grass.RecalculateBounds();

		return grass;
	}

}
