#include "egsspch.h"
#include "Egss/Physics/Heightfield3D.h"

namespace Egss {

	void Heightfield3D::SetHeights(int resolution, float extent,
		const std::vector<float>& heights)
	{
		Resolution = resolution;
		Extent = extent;
		Heights = heights;

		Lowest = 0.0f;
		Highest = 0.0f;

		for (size_t i = 0; i < Heights.size(); i++)
		{
			Lowest = i == 0 ? Heights[i] : std::min(Lowest, Heights[i]);
			Highest = i == 0 ? Heights[i] : std::max(Highest, Heights[i]);
		}
	}

	float Heightfield3D::At(int x, int z) const
	{
		if (Empty())
			return 0.0f;

		x = glm::clamp(x, 0, Resolution - 1);
		z = glm::clamp(z, 0, Resolution - 1);
		return Heights[(size_t)z * Resolution + x];
	}

	// Which cell a local coordinate falls in, and where inside it.
	//
	// Clamped to Resolution-2 rather than Resolution-1 because a *cell* is
	// named by its low corner and the last sample starts no cell. Off by one
	// here and the far edge of the map reads the row beyond the grid, which
	// At() clamps into being a flat strip a metre wide -- visible only as a
	// character walking onto a ledge at the boundary.
	static void CellOf(const Heightfield3D& field, float localX, float localZ,
		int& outX, int& outZ, float& outFx, float& outFz)
	{
		float half = field.HalfExtent();
		float step = field.CellSize();

		float gx = (localX + half) / step;
		float gz = (localZ + half) / step;

		outX = glm::clamp((int)std::floor(gx), 0, field.Resolution - 2);
		outZ = glm::clamp((int)std::floor(gz), 0, field.Resolution - 2);

		outFx = gx - (float)outX;
		outFz = gz - (float)outZ;
	}

	bool Heightfield3D::SurfaceAt(float localX, float localZ,
		float& outHeight, glm::vec3& outNormal) const
	{
		if (Empty() || !Contains(localX, localZ))
			return false;

		int x0, z0;
		float fx, fz;
		CellOf(*this, localX, localZ, x0, z0, fx, fz);

		float step = CellSize();
		float h00 = At(x0, z0), h10 = At(x0 + 1, z0);
		float h01 = At(x0, z0 + 1), h11 = At(x0 + 1, z0 + 1);

		// The anti-diagonal runs from (x0,z1) to (x1,z0), so fx+fz == 1 is the
		// seam and either triangle gives the same answer on it.
		if (fx + fz <= 1.0f)
		{
			outHeight = h00 + fx * (h10 - h00) + fz * (h01 - h00);

			// The plane's gradient is ((h10-h00)/step, (h01-h00)/step) in
			// (x, z); a surface normal is that negated with a unit vertical
			// part, scaled here by `step` so no division is needed.
			outNormal = glm::normalize(glm::vec3(-(h10 - h00), step, -(h01 - h00)));
		}
		else
		{
			outHeight = h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
			outNormal = glm::normalize(glm::vec3(h01 - h11, step, h10 - h11));
		}

		return true;
	}

	glm::vec3 Heightfield3D::SmoothNormalAt(float localX, float localZ) const
	{
		if (Empty())
			return { 0.0f, 1.0f, 0.0f };

		float step = CellSize();
		float half = HalfExtent();

		// Sampled a cell either side, clamped inside the map so the edge slopes
		// with the ground rather than with the clamp.
		auto height = [&](float x, float z)
		{
			x = glm::clamp(x, -half, half);
			z = glm::clamp(z, -half, half);

			float value = 0.0f;
			glm::vec3 ignored;
			SurfaceAt(x, z, value, ignored);
			return value;
		};

		float dx = height(localX + step, localZ) - height(localX - step, localZ);
		float dz = height(localX, localZ + step) - height(localX, localZ - step);

		// The gradient is (dh/dx, dh/dz) over 2*step; the normal is that negated
		// with a unit vertical part, scaled through by 2*step.
		return glm::normalize(glm::vec3(-dx, 2.0f * step, -dz));
	}

	// The point of triangle abc nearest p, by Voronoi region: the seven cases
	// are the three vertices, the three edges, and the interior. Written out
	// rather than solved as a constrained least squares because each case is a
	// couple of dot products and the branch is what makes it cheap.
	static glm::vec3 ClosestOnTriangle(const glm::vec3& p,
		const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		glm::vec3 ab = b - a;
		glm::vec3 ac = c - a;
		glm::vec3 ap = p - a;

		float d1 = glm::dot(ab, ap);
		float d2 = glm::dot(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f)
			return a;

		glm::vec3 bp = p - b;
		float d3 = glm::dot(ab, bp);
		float d4 = glm::dot(ac, bp);
		if (d3 >= 0.0f && d4 <= d3)
			return b;

		float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
			return a + ab * (d1 / (d1 - d3));

		glm::vec3 cp = p - c;
		float d5 = glm::dot(ab, cp);
		float d6 = glm::dot(ac, cp);
		if (d6 >= 0.0f && d5 <= d6)
			return c;

		float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
			return a + ac * (d2 / (d2 - d6));

		float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

		float denominator = 1.0f / (va + vb + vc);
		return a + ab * (vb * denominator) + ac * (vc * denominator);
	}

	bool Heightfield3D::ClosestPoint(const glm::vec3& local, float searchRadius,
		glm::vec3& outPoint, glm::vec3& outNormal, float& outDepth) const
	{
		if (Empty())
			return false;

		float half = HalfExtent();
		float step = CellSize();

		// Wholly beyond the map in x or z: nothing to be near.
		if (local.x + searchRadius < -half || local.x - searchRadius > half)
			return false;
		if (local.z + searchRadius < -half || local.z - searchRadius > half)
			return false;

		auto cellIndex = [&](float coordinate)
		{
			return glm::clamp((int)std::floor((coordinate + half) / step),
				0, Resolution - 2);
		};

		int x0 = cellIndex(local.x - searchRadius);
		int x1 = cellIndex(local.x + searchRadius);
		int z0 = cellIndex(local.z - searchRadius);
		int z1 = cellIndex(local.z + searchRadius);

		float bestDistanceSquared = std::numeric_limits<float>::max();
		bool found = false;

		for (int cz = z0; cz <= z1; cz++)
		{
			float zLow = -half + (float)cz * step;

			for (int cx = x0; cx <= x1; cx++)
			{
				float xLow = -half + (float)cx * step;

				float h00 = At(cx, cz), h10 = At(cx + 1, cz);
				float h01 = At(cx, cz + 1), h11 = At(cx + 1, cz + 1);

				glm::vec3 p00(xLow, h00, zLow);
				glm::vec3 p10(xLow + step, h10, zLow);
				glm::vec3 p01(xLow, h01, zLow + step);
				glm::vec3 p11(xLow + step, h11, zLow + step);

				// The same two triangles SurfaceAt splits the cell into, in the
				// same winding. They must agree: one is what a contact is
				// generated against and the other is what the ground probe
				// reports, and a character standing where those two disagree
				// hovers or sinks with nothing in a screenshot to say why.
				const glm::vec3 corners[2][3] = {
					{ p00, p01, p10 },
					{ p10, p01, p11 }
				};

				const glm::vec3 normals[2] = {
					glm::normalize(glm::vec3(-(h10 - h00), step, -(h01 - h00))),
					glm::normalize(glm::vec3(h01 - h11, step, h10 - h11))
				};

				for (int t = 0; t < 2; t++)
				{
					glm::vec3 closest = ClosestOnTriangle(local,
						corners[t][0], corners[t][1], corners[t][2]);

					glm::vec3 away = local - closest;
					float distanceSquared = glm::dot(away, away);

					if (distanceSquared >= bestDistanceSquared)
						continue;

					bestDistanceSquared = distanceSquared;
					outPoint = closest;
					outNormal = normals[t];
					outDepth = glm::dot(away, normals[t]);
					found = true;
				}
			}
		}

		return found;
	}

	// Moller-Trumbore. Solves origin + t*d = a + u*ab + v*ac for (t, u, v) by
	// Cramer's rule, which is three cross/dot products and one divide -- and,
	// more usefully, never builds the triangle's plane, so a degenerate triangle
	// falls out as a near-zero determinant instead of a normalised zero vector.
	//
	// Back faces are **not** culled. A heightfield cell viewed from underneath is
	// still a surface, and a caller probing upwards from inside the terrain
	// wants to be told where the roof is rather than nothing at all.
	static bool RayTriangle(const glm::vec3& origin, const glm::vec3& direction,
		const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& outT)
	{
		const float epsilon = 1e-8f;

		glm::vec3 ab = b - a;
		glm::vec3 ac = c - a;

		glm::vec3 pv = glm::cross(direction, ac);
		float determinant = glm::dot(ab, pv);

		// Parallel to the triangle's plane, or the triangle has no area.
		if (std::abs(determinant) < epsilon)
			return false;

		float inverse = 1.0f / determinant;

		glm::vec3 tv = origin - a;
		float u = glm::dot(tv, pv) * inverse;
		if (u < 0.0f || u > 1.0f)
			return false;

		glm::vec3 qv = glm::cross(tv, ab);
		float v = glm::dot(direction, qv) * inverse;
		if (v < 0.0f || u + v > 1.0f)
			return false;

		outT = glm::dot(ac, qv) * inverse;
		return true;
	}

	bool Heightfield3D::Raycast(const glm::vec3& origin, const glm::vec3& direction,
		float maxDistance, float& outDistance,
		glm::vec3& outPoint, glm::vec3& outNormal) const
	{
		if (Empty())
			return false;

		float half = HalfExtent();
		float step = CellSize();

		// --- clip the ray to the field's box ---------------------------------
		//
		// Including the vertical band, which is the half that earns its keep: a
		// ray aimed above the terrain leaves here rather than walking the whole
		// map to discover there was never anything to hit. Padded a little in y
		// so a cast that grazes the highest sample is not rejected by the last
		// bit of the float.
		float enter = 0.0f;
		float exit = maxDistance;

		const float pad = 1e-3f;
		glm::vec3 boxMin(-half, Lowest - pad, -half);
		glm::vec3 boxMax(half, Highest + pad, half);

		for (int axis = 0; axis < 3; axis++)
		{
			if (std::abs(direction[axis]) < 1e-8f)
			{
				// Running parallel to this pair of planes: either always inside
				// them or never.
				if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis])
					return false;

				continue;
			}

			float inverse = 1.0f / direction[axis];
			float t1 = (boxMin[axis] - origin[axis]) * inverse;
			float t2 = (boxMax[axis] - origin[axis]) * inverse;

			if (t1 > t2)
				std::swap(t1, t2);

			enter = std::max(enter, t1);
			exit = std::min(exit, t2);

			if (enter > exit)
				return false;
		}

		// --- walk the cells the ray crosses ----------------------------------

		glm::vec3 at = origin + direction * enter;

		int cx = glm::clamp((int)std::floor((at.x + half) / step), 0, Resolution - 2);
		int cz = glm::clamp((int)std::floor((at.z + half) / step), 0, Resolution - 2);

		int stepX = direction.x >= 0.0f ? 1 : -1;
		int stepZ = direction.z >= 0.0f ? 1 : -1;

		const float infinity = std::numeric_limits<float>::max();

		// How far along the ray the next cell boundary is, and how far apart
		// consecutive ones are. Both are absolute distances from the *origin*,
		// not from the entry point, so they stay comparable with `exit`.
		auto boundary = [&](int cell, int direction_)
		{
			return -half + (float)(cell + (direction_ > 0 ? 1 : 0)) * step;
		};

		float nextX = std::abs(direction.x) < 1e-8f ? infinity
			: (boundary(cx, stepX) - origin.x) / direction.x;
		float nextZ = std::abs(direction.z) < 1e-8f ? infinity
			: (boundary(cz, stepZ) - origin.z) / direction.z;

		float deltaX = std::abs(direction.x) < 1e-8f ? infinity
			: step / std::abs(direction.x);
		float deltaZ = std::abs(direction.z) < 1e-8f ? infinity
			: step / std::abs(direction.z);

		float travelled = enter;

		while (travelled <= exit)
		{
			float xLow = -half + (float)cx * step;
			float zLow = -half + (float)cz * step;

			float h00 = At(cx, cz), h10 = At(cx + 1, cz);
			float h01 = At(cx, cz + 1), h11 = At(cx + 1, cz + 1);

			glm::vec3 p00(xLow, h00, zLow);
			glm::vec3 p10(xLow + step, h10, zLow);
			glm::vec3 p01(xLow, h01, zLow + step);
			glm::vec3 p11(xLow + step, h11, zLow + step);

			const glm::vec3 corners[2][3] = {
				{ p00, p01, p10 },
				{ p10, p01, p11 }
			};

			const glm::vec3 normals[2] = {
				glm::normalize(glm::vec3(-(h10 - h00), step, -(h01 - h00))),
				glm::normalize(glm::vec3(h01 - h11, step, h10 - h11))
			};

			// Both triangles, nearest wins -- the cell order settles which cell,
			// but a cell's own two halves have to be compared to each other.
			float bestT = infinity;
			int bestTriangle = -1;

			for (int t = 0; t < 2; t++)
			{
				float hit = 0.0f;
				if (!RayTriangle(origin, direction,
					corners[t][0], corners[t][1], corners[t][2], hit))
					continue;

				if (hit < 0.0f || hit > exit || hit >= bestT)
					continue;

				bestT = hit;
				bestTriangle = t;
			}

			if (bestTriangle >= 0)
			{
				outDistance = bestT;
				outPoint = origin + direction * bestT;

				// Facing the ray. For the overhead cast that placement does this
				// is the up-facing normal; for one fired from under the terrain
				// it is the other one, which is the honest answer to "what did I
				// hit" in both cases.
				outNormal = glm::dot(normals[bestTriangle], direction) > 0.0f
					? -normals[bestTriangle] : normals[bestTriangle];

				return true;
			}

			// On to the next cell: whichever boundary the ray reaches first.
			if (nextX < nextZ)
			{
				travelled = nextX;
				nextX += deltaX;
				cx += stepX;
			}
			else
			{
				travelled = nextZ;
				nextZ += deltaZ;
				cz += stepZ;
			}

			// Off the grid. A vertical ray gets here at once, both boundaries
			// being infinitely far away, which is why the loop cannot spin.
			if (cx < 0 || cx > Resolution - 2 || cz < 0 || cz > Resolution - 2)
				break;
		}

		return false;
	}

}
