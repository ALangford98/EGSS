#include "egsspch.h"
#include "Egss/Renderer/Frustum.h"

namespace Egss {

	Frustum Frustum::FromViewProjection(const glm::mat4& m)
	{
		Frustum frustum;

		// glm stores column-major, so m[col][row] -- row i of the
		// mathematical matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
		glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
		glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
		glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
		glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

		// Clip space keeps a point when -w <= x,y,z <= w, which is six
		// inequalities of the form (row3 +/- rowN).v >= 0 -- the standard
		// Gribb/Hartmann result for a column-vector (OpenGL-style) matrix.
		frustum.Planes[0] = row3 + row0; // Left:   x_c + w_c >= 0
		frustum.Planes[1] = row3 - row0; // Right:  w_c - x_c >= 0
		frustum.Planes[2] = row3 + row1; // Bottom: y_c + w_c >= 0
		frustum.Planes[3] = row3 - row1; // Top:    w_c - y_c >= 0
		frustum.Planes[4] = row3 + row2; // Near:   z_c + w_c >= 0
		frustum.Planes[5] = row3 - row2; // Far:    w_c - z_c >= 0

		for (glm::vec4& plane : frustum.Planes)
		{
			float length = glm::length(glm::vec3(plane));
			if (length > 1e-8f)
				plane /= length;
		}

		return frustum;
	}

	bool Frustum::Intersects(const Aabb& box) const
	{
		for (const glm::vec4& plane : Planes)
		{
			// The corner furthest in the plane's positive direction -- if
			// even that one fails the plane, all eight do.
			glm::vec3 positive(
				plane.x >= 0.0f ? box.Max.x : box.Min.x,
				plane.y >= 0.0f ? box.Max.y : box.Min.y,
				plane.z >= 0.0f ? box.Max.z : box.Min.z);

			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}

		return true;
	}

}
