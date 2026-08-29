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
		// **Blades per square metre.** A real lawn is thousands; the eye stops
		// counting somewhere in the low hundreds, and every one of them has to
		// be built, uploaded and transformed. This started at the equivalent of
		// two or three a square metre, which is stubble, and the number that
		// makes it read as a field is nearer sixty.
		//
		// It is worth knowing what this costs before turning it up: each blade
		// is five vertices and three triangles, so sixty a square metre over a
		// fifty-metre circle is half a million triangles. That is why the
		// radius grass is built over is the first thing to trade, not this.
		float Density = 60.0f;

		float Height = 0.42f;

		// **Half-width at the root, in metres, and it wants to be small.**
		//
		// This was 0.045 -- a blade 9 cm across. Real grass is four or five
		// millimetres, so every blade was twenty times too wide, and standing
		// in it you were not looking at grass but at a heap of flat green
		// shards the size of dinner plates. That is what the "geometry
		// artifacts" in the near field were: correct geometry at an absurd
		// scale.
		//
		// Narrow blades only work if there are a great many of them, which is
		// what the LOD below is for. The two numbers move together and always
		// have -- a blade you can see individually has to be thin, and a field
		// of thin blades has to be dense or it is bare ground with hairs on.
		float Width = 0.006f;

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
	// Three triangles a blade -- see the note where they are built for why one
	// is not enough. Placed on the terrain's own triangles rather than on a grid, so blades
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

			// **Blades per square metre, not per triangle.** Per triangle is a
			// number that means nothing on its own: it depends on how finely
			// the terrain happens to be meshed, so the same setting gives a
			// lawn on one body and a stubble field on another. The triangle's
			// own area turns it into a density anyone can reason about, and
			// makes the cost of a change predictable -- doubling it doubles
			// the geometry, wherever it is.
			float area = 0.5f * area2;

			float chance = allow(centre, n) * flatness * settings.Density * area;

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

				// **A blade with a waist, not a spike.**
				//
				// This was one triangle: two corners at the root and a point
				// at the tip. That is a *needle*, and a field of them looks
				// like one -- the shape has no length along which anything can
				// happen, so it cannot curve, and the only way to make it read
				// as grass is to make it fat, which makes it read as a leaf.
				//
				// Three triangles and a middle pair of corners costs five
				// vertices instead of three and buys the thing that actually
				// matters: the blade can *bend along itself*. The shader leans
				// each vertex by its own height up the blade, so with a
				// mid-point at 55% the blade curves over instead of pivoting
				// rigidly, which is what grass in wind does and what a single
				// triangle can never do at any width.
				//
				// The waist is 70% of the root's width and the tip is a point,
				// so the silhouette tapers the way a real blade does.
				//
				// **The texture coordinate carries height up the blade**, 0 at
				// the root and 1 at the tip, which is what lets the shader bend
				// it without knowing anything about where the root is. Same
				// trick the trees use, one dimension smaller.
				const float waist = 0.55f;

				glm::vec3 middle = base + vert * (height * waist)
					+ lean * (waist * waist);

				// **`TexCoord.x` is a per-blade lottery ticket, not a
				// coordinate.** The shader needs one number that is the same
				// at all five vertices of a blade and different between
				// blades, so it can drop a fixed share of the field with
				// distance and have whole blades vanish rather than corners
				// of them. There is nowhere else to put it -- `Normal` is
				// doing lighting and `TexCoord.y` is the height up the blade
				// the bend depends on -- and the across-the-blade coordinate
				// it replaces was never read by anything.
				// **The texture coordinate carries height up the blade**, 0 at
				// the root and 1 at the tip, which is what lets the shader
				// bend it without knowing where the root is.
				//
				// `x` is a per-blade lottery ticket, the same at all five
				// vertices; `y` is the height up the blade. The across-blade
				// coordinate it replaces was never read by anything.
				//
				// The ticket is what the level of detail thresholds against.
				// It has to be identical across a blade or the blade tears in
				// half, and the number it is compared with has to be identical
				// too -- which is why that one is a uniform per chunk rather
				// than anything computed from a vertex. Both halves of that
				// were learned the hard way; see the changelog.
				float ticket = Veg::Hash2DUnit((int)t, i * 3 + 5, seed);

				grass.Vertices.push_back({ base - side,          bladeNormal, { ticket, 0.0f } });
				grass.Vertices.push_back({ base + side,          bladeNormal, { ticket, 0.0f } });
				grass.Vertices.push_back({ middle - side * 0.7f, bladeNormal, { ticket, waist } });
				grass.Vertices.push_back({ middle + side * 0.7f, bladeNormal, { ticket, waist } });
				grass.Vertices.push_back({ tip,                  bladeNormal, { ticket, 1.0f } });

				grass.Indices.insert(grass.Indices.end(), {
					at,     at + 1, at + 3,
					at,     at + 3, at + 2,
					at + 2, at + 3, at + 4 });
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
