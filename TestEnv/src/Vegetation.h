#pragma once

// Procedural trees: a trunk that splits into branches that split into
// branches, and the lump of foliage each tip carries.
//
// **Moved here out of `OpenWorld.h` rather than written a second time.** The
// solar system's Earth wanted trees, and this project has a documented habit
// of the same idea diverging in three copies -- `FirstPersonController` exists
// because forward-from-yaw had been written three times with different sign
// conventions. The move is a move: not a line of the generator changed, which
// is checkable, and was checked -- OpenWorld's capture is byte-identical
// across it.
//
// `OpenWorld` keeps one-line forwarders under the old names so its forty-odd
// call sites did not have to be rewritten in the same change that proves the
// change did nothing.

#include <Egss.h>

namespace Veg {

	// A 2D integer hash. **The cast is inside the multiply on purpose** -- see
	// the 2026-08-17 changelog entry for what `(uint32_t)(x * 374761393)` does
	// to a release build when x overflows an int.
	inline uint32_t Hash2D(int x, int y, uint32_t seed)
	{
		uint32_t h = seed;
		h ^= (uint32_t)x * 374761393u;
		h ^= (uint32_t)y * 668265263u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return h;
	}

	inline float Hash2DUnit(int x, int y, uint32_t seed)
	{
		return (float)(Hash2D(x, y, seed) & 0xFFFFFF) / (float)0xFFFFFF;
	}

	// The ratios are the whole model. Length and radius shrinking by a
	// constant factor per generation is what makes the thing read as a tree
	// rather than as a bundle of sticks, and it also means the result has
	// closed forms to be checked against: the segment count is a geometric
	// series in the branching factor, and the height is bounded by one in the
	// length ratio. Neither is computed anywhere in the generator.
	struct TreeParams
	{
		int Depth = 4;
		int Children = 3;
		int Sides = 6;

		float Length = 2.6f;
		float Radius = 0.20f;

		float LengthRatio = 0.74f;
		float RadiusRatio = 0.62f;

		float Spread = 36.0f;     // degrees a child leans off its parent

		// Foliage. One cluster per terminal branch, which makes the leaf count
		// a closed form too: a complete c-ary tree of depth d has c^d tips.
		float LeafRadius = 0.55f;
		int LeafSegments = 5;
		int LeafRings = 3;
	};

	// A ring of `sides` points around `centre`, in the plane perpendicular to
	// `dir`.
	inline void Ring(const glm::vec3& centre, const glm::vec3& dir, float radius,
		int sides, const glm::vec3& u, const glm::vec3& v, std::vector<glm::vec3>& out)
	{
		out.clear();
		for (int i = 0; i < sides; i++)
		{
			float a = (float)i / (float)sides * 6.2831853f;
			out.push_back(centre + (u * std::cos(a) + v * std::sin(a)) * radius);
		}
	}

	// Any two vectors perpendicular to `dir` and to each other. Picked from
	// whichever axis `dir` is least aligned with, so the cross product never
	// collapses.
	inline void Basis(const glm::vec3& dir, glm::vec3& outU, glm::vec3& outV)
	{
		glm::vec3 away = std::fabs(dir.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

		outU = glm::normalize(glm::cross(dir, away));
		outV = glm::normalize(glm::cross(dir, outU));
	}

	// A lump of foliage: the same jittered blob the rocks are made of, smaller
	// and coarser. Deliberately not leaf-shaped -- at the size these are drawn
	// a cluster reads as foliage and a hundred individual leaves would be a
	// hundred times the triangles to say the same thing.
	inline void LeafCluster(Egss::MeshData& data, const glm::vec3& centre, float radius,
		int segments, int rings, unsigned int seed, int path)
	{
		auto point = [&](int i, int j)
		{
			int wrapped = i % segments;

			float u = (float)wrapped / (float)segments * 6.2831853f;
			float v = (float)j / (float)rings * 3.14159265f;

			float r = radius * (0.72f + Hash2DUnit(path * 31 + wrapped, j, seed) * 0.5f);

			return centre + glm::vec3(std::sin(v) * std::cos(u), std::cos(v),
				std::sin(v) * std::sin(u)) * r;
		};

		// **Radial normals, not the triangle's own.**
		//
		// This used one flat normal per face, and that is the whole reason a
		// leaf cluster read as *blocky*: it is a low-polygon sphere with the
		// radius jittered per point, so flat shading draws every one of those
		// facets as its own flat patch of colour and the lumpiness that was
		// meant to make it look organic instead outlines each triangle.
		//
		// A cluster of leaves is a blob. The normal a blob has at a point is
		// the direction from its centre to that point -- which is free here,
		// because the centre is already known, and which shades the whole
		// cluster as one rounded mass while leaving the silhouette exactly as
		// jagged as it was. The lumpy radius then reads as what it is, a rough
		// surface, rather than as a modelling error.
		auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			glm::vec3 flat = glm::cross(b - a, c - a);
			if (glm::length(flat) < 1e-9f)
				return;

			auto outward = [&](const glm::vec3& p)
			{
				glm::vec3 out = p - centre;
				float length = glm::length(out);

				// Degenerate only if a point landed on the centre, which the
				// radius jitter cannot produce -- but a normalize of zero is
				// a NaN that spreads, so it is worth the branch.
				return length > 1e-6f ? out / length : glm::normalize(flat);
			};

			unsigned int at = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ a, outward(a), { 0.0f, 0.0f } });
			data.Vertices.push_back({ b, outward(b), { 1.0f, 0.0f } });
			data.Vertices.push_back({ c, outward(c), { 0.5f, 1.0f } });
			data.Indices.push_back(at);
			data.Indices.push_back(at + 1);
			data.Indices.push_back(at + 2);
		};

		for (int j = 0; j < rings; j++)
			for (int i = 0; i < segments; i++)
			{
				glm::vec3 a = point(i, j), b = point(i + 1, j);
				glm::vec3 c = point(i + 1, j + 1), d = point(i, j + 1);

				face(a, b, c);
				face(a, c, d);
			}
	}

	inline void Segment(Egss::MeshData& data, const glm::vec3& base, const glm::vec3& tip,
		float baseRadius, float tipRadius, int sides)
	{
		glm::vec3 dir = tip - base;
		float length = glm::length(dir);
		if (length < 1e-5f)
			return;

		dir /= length;

		glm::vec3 u, v;
		Basis(dir, u, v);

		std::vector<glm::vec3> lower, upper;
		Ring(base, dir, baseRadius, sides, u, v, lower);
		Ring(tip, dir, tipRadius, sides, u, v, upper);

		for (int i = 0; i < sides; i++)
		{
			int j = (i + 1) % sides;

			// Flat per face, which suits the banded shading the rest of the
			// world uses -- a smooth trunk under four bands is two stripes.
			glm::vec3 n = glm::cross(lower[j] - lower[i], upper[i] - lower[i]);
			if (glm::length(n) < 1e-8f)
				continue;

			n = glm::normalize(n);

			unsigned int at = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ lower[i], n, { 0.0f, 0.0f } });
			data.Vertices.push_back({ lower[j], n, { 1.0f, 0.0f } });
			data.Vertices.push_back({ upper[j], n, { 1.0f, 1.0f } });
			data.Vertices.push_back({ upper[i], n, { 0.0f, 1.0f } });

			data.Indices.push_back(at);
			data.Indices.push_back(at + 1);
			data.Indices.push_back(at + 2);
			data.Indices.push_back(at);
			data.Indices.push_back(at + 2);
			data.Indices.push_back(at + 3);
		}
	}

	inline void Branch(Egss::MeshData& bark, Egss::MeshData& leaves, const TreeParams& tree,
		const glm::vec3& base, const glm::vec3& dir, float length, float radius,
		int depth, unsigned int seed, int path)
	{
		glm::vec3 tip = base + dir * length;

		Segment(bark, base, tip, radius, radius * tree.RadiusRatio, tree.Sides);

		if (depth <= 0)
		{
			// A terminal branch carries the foliage, set a little past the tip
			// so the twig disappears into it rather than poking out the far
			// side.
			LeafCluster(leaves, tip + dir * (tree.LeafRadius * 0.35f),
				tree.LeafRadius * (0.75f + Hash2DUnit(path, 9, seed) * 0.5f),
				tree.LeafSegments, tree.LeafRings, seed, path);
			return;
		}

		glm::vec3 u, v;
		Basis(dir, u, v);

		for (int i = 0; i < tree.Children; i++)
		{
			// Spread evenly around the parent, then jittered -- evenly spaced
			// children look like a lamp, and unjittered ones repeat visibly at
			// every level because every node uses the same angles.
			float around = ((float)i / (float)tree.Children) * 6.2831853f
				+ Hash2DUnit(path, i * 3 + depth, seed) * 1.7f;

			float lean = glm::radians(tree.Spread
				* (0.65f + Hash2DUnit(path, i * 3 + 1 + depth, seed) * 0.7f));

			glm::vec3 side = u * std::cos(around) + v * std::sin(around);
			glm::vec3 childDir = glm::normalize(dir * std::cos(lean) + side * std::sin(lean));

			Branch(bark, leaves, tree, tip, childDir, length * tree.LengthRatio,
				radius * tree.RadiusRatio, depth - 1, seed, path * tree.Children + i + 1);
		}
	}

	inline void Finish(Egss::MeshData& data)
	{
		if (data.Indices.empty())
			return;

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();
	}

	// Bark and foliage come out as separate meshes rather than one, because
	// they are two different colours and the leaves are the half worth being
	// able to turn off when counting triangles.
	inline void MakeTreeMesh(unsigned int seed, const TreeParams& tree,
		Egss::MeshData& outBark, Egss::MeshData& outLeaves)
	{
		Branch(outBark, outLeaves, tree, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
			tree.Length, tree.Radius, tree.Depth, seed, 0);

		Finish(outBark);
		Finish(outLeaves);
	}

}
