#pragma once

// The arithmetic behind the editor's rotate and scale gizmos, apart from
// mouse and ray handling so it can be checked on its own. EditorSceneView
// owns the interaction; this owns what a drag does to a transform.

#include <GS.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace GizmoMath {

	// The composition TransformComponent::GetTransform uses: Rx * Ry * Rz.
	inline glm::mat3 RotationFromEuler(const glm::vec3& degrees)
	{
		return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(degrees.x), glm::vec3(1, 0, 0))
			* glm::rotate(glm::mat4(1.0f), glm::radians(degrees.y), glm::vec3(0, 1, 0))
			* glm::rotate(glm::mat4(1.0f), glm::radians(degrees.z), glm::vec3(0, 0, 1)));
	}

	// Its inverse. R = Rx(a) Ry(b) Rz(c) has first row (cb cc, -cb sc, sb)
	// and last column (sb, -sa cb, ca cb), so b, then a and c, fall out of
	// those (row 1 at c = 0 gives a when cb is 0); glm stores columns, so R(row, col) is m[col][row]. Of the two
	// angle triples that give any rotation -- (a, b, c) and
	// (a+180, 180-b, c+180) -- the one nearer `near` is kept, each angle
	// within 180 degrees of it, so the Inspector's numbers move smoothly
	// through a drag instead of jumping between equivalent spellings.
	inline glm::vec3 EulerFromRotation(const glm::mat3& m, const glm::vec3& near)
	{
		// b from atan2 against cos b = |(R00, R01)|, not asin(R02): near
		// +-90 degrees asin is badly conditioned -- one float step in R02
		// (~6e-8) is sqrt(2 * 6e-8) = 3.5e-4 rad of angle, which is exactly
		// the error the first self-test measured at gimbal lock.
		float sb = m[2][0];
		float cb = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
		float a, b = std::atan2(sb, cb), c;
		if (cb > 1e-6f)
		{
			a = std::atan2(-m[2][1], m[2][2]);
			c = std::atan2(-m[1][0], m[0][0]);
		}
		else
		{
			// Gimbal lock: only a combination of a and c is fixed; c = 0
			// gives it all to a.
			a = std::atan2(m[1][2], m[1][1]);
			c = 0.0f;
		}
		glm::vec3 first = glm::degrees(glm::vec3(a, b, c));
		glm::vec3 second(first.x + 180.0f, 180.0f - first.y, first.z + 180.0f);

		auto wrapNear = [&near](glm::vec3 angles)
		{
			for (int i = 0; i < 3; i++)
				angles[i] = near[i] + std::remainder(angles[i] - near[i], 360.0f);
			return angles;
		};
		auto distance = [&near](const glm::vec3& angles)
		{
			return std::abs(angles.x - near.x) + std::abs(angles.y - near.y) + std::abs(angles.z - near.z);
		};
		first = wrapNear(first);
		second = wrapNear(second);
		return distance(first) <= distance(second) ? first : second;
	}

	// Degrees from `from` to `to` (unit vectors in the plane with `normal`),
	// signed by the right-hand rule about the normal -- the same sense
	// glm::rotate turns by, so dragging round a ring turns the object the
	// way the cursor went.
	inline float SignedAngle(const glm::vec3& from, const glm::vec3& to, const glm::vec3& normal)
	{
		return glm::degrees(std::atan2(glm::dot(glm::cross(from, to), normal), glm::dot(from, to)));
	}

	// `start` turned by `degrees` about world axis `axis` (0, 1, 2) through
	// `pivot`: its orientation turns, and its position orbits the pivot
	// (staying put when the pivot is its own position).
	inline GS::TransformComponent Rotated(const GS::TransformComponent& start, const glm::vec3& pivot, int axis, float degrees)
	{
		glm::vec3 normal(0.0f);
		normal[axis] = 1.0f;
		glm::mat3 turn = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(degrees), normal));
		GS::TransformComponent result = start;
		result.Rotation = EulerFromRotation(turn * RotationFromEuler(start.Rotation), start.Rotation);
		result.Position = pivot + turn * (start.Position - pivot);
		return result;
	}

	// `start` scaled in place: along its own axis `handle` (0, 1, 2) or,
	// for 3, uniformly. Never below 1%: a zero scale cannot be dragged back.
	inline GS::TransformComponent Scaled(const GS::TransformComponent& start, int handle, float factor)
	{
		factor = std::max(factor, 0.01f);
		GS::TransformComponent result = start;
		if (handle == 3)
			result.Scale *= factor;
		else
			result.Scale[handle] *= factor;
		return result;
	}
}
