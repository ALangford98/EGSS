#pragma once

// **A boulder, as a jittered sphere.**
//
// Lifted out of `OpenWorld` so the terrain lab can have rocks too, and left
// exactly as it was found -- this one needed no generalising, because a rock is
// the same shape whichever way up the world is.
//
// The one number worth knowing is the jitter range. It was 0.68..1.0 on a 9x6
// lattice, which read as a lump of coal; more facets and a shallower jitter
// give a boulder that is still faceted but no longer jagged. The ceiling stays
// at 1.0 so the blob cannot leave the unit box that collides for it -- the
// collider is a box and the mesh is what you see, and the mesh has to stay
// inside the thing that stops it.

#include <Egss.h>

#include "Vegetation.h"

#include <glm/glm.hpp>

#include <cmath>

namespace Boulder {

inline Egss::MeshData Build(unsigned int seed)
{
	const int segments = 16, rings = 10;

	auto point = [&](int i, int j)
	{
		// Wrap the seam so the last column is literally the first.
		int wrapped = i % segments;

		float u = (float)wrapped / (float)segments * 6.2831853f;
		float v = (float)j / (float)rings * 3.14159265f;

		// 0.84..1.0. Was 0.68..1.0 on a 9x6 lattice, which read as a lump
		// of coal -- more facets and a shallower jitter give a boulder that
		// is still faceted but no longer jagged. The ceiling stays at 1.0
		// so the blob cannot leave the box that collides for it.
		float radius = 0.84f + Veg::Hash2DUnit(wrapped, j, seed) * 0.16f;

		// Poles pulled in a little, or a jittered pole spikes.
		if (j == 0 || j == rings)
			radius = 0.86f + Veg::Hash2DUnit(0, j, seed) * 0.10f;

		return glm::vec3(
			std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)) * radius;
	};

	Egss::MeshData data;

	auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		glm::vec3 n = glm::cross(b - a, c - a);
		if (glm::length(n) < 1e-8f)
			return;

		n = glm::normalize(n);

		unsigned int base = (unsigned int)data.Vertices.size();
		data.Vertices.push_back({ a, n, { 0.0f, 0.0f } });
		data.Vertices.push_back({ b, n, { 1.0f, 0.0f } });
		data.Vertices.push_back({ c, n, { 0.5f, 1.0f } });
		data.Indices.push_back(base);
		data.Indices.push_back(base + 1);
		data.Indices.push_back(base + 2);
	};

	for (int j = 0; j < rings; j++)
	{
		for (int i = 0; i < segments; i++)
		{
			glm::vec3 a = point(i, j), b = point(i + 1, j);
			glm::vec3 c = point(i + 1, j + 1), d = point(i, j + 1);

			// Degenerate at the poles, where the whole ring is one point --
			// `face` drops those on the zero-area test.
			face(a, b, c);
			face(a, c, d);
		}
	}

	Egss::Submesh all;
	all.IndexCount = (unsigned int)data.Indices.size();
	data.Submeshes.push_back(all);
	data.RecalculateBounds();

	return data;
}

}
