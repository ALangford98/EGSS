#include "egsspch.h"
#include "Egss/Physics/Raycast3D.h"

#include "Egss/Scene/Components.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace Egss {

	bool Raycast3D::AgainstAabb(const glm::vec3& origin, const glm::vec3& direction,
		float maxDistance, const Aabb& box, float& distance, glm::vec3& normal)
	{
		// The interval of the ray still inside the box, narrowed one axis at a
		// time. It starts as the whole ray and can only shrink; if it ever
		// inverts, the slabs have no common stretch and the ray misses.
		float near = 0.0f;
		float far = maxDistance;

		int nearAxis = 0;
		float nearSign = -1.0f;

		for (int axis = 0; axis < 3; axis++)
		{
			// A ray parallel to this pair of planes either never leaves the
			// slab or was never in it. Dividing by zero would give infinities
			// that happen to work, but only by accident and not on every
			// platform, so it is worth saying out loud.
			if (std::fabs(direction[axis]) < 1e-8f)
			{
				if (origin[axis] < box.Min[axis] || origin[axis] > box.Max[axis])
					return false;
				continue;
			}

			float inverse = 1.0f / direction[axis];
			float t1 = (box.Min[axis] - origin[axis]) * inverse;
			float t2 = (box.Max[axis] - origin[axis]) * inverse;

			// t1 is the near plane only if the ray points up this axis.
			float sign = -1.0f;
			if (t1 > t2)
			{
				std::swap(t1, t2);
				sign = 1.0f;
			}

			// Track *which* plane last pushed the near edge along: that is the
			// face the ray enters through, and so the one whose normal to
			// report.
			if (t1 > near)
			{
				near = t1;
				nearAxis = axis;
				nearSign = sign;
			}

			far = std::min(far, t2);

			if (near > far)
				return false;
		}

		distance = near;

		normal = glm::vec3(0.0f);
		normal[nearAxis] = nearSign;

		// Started inside: `near` never moved off zero, so no face was chosen.
		// Face back down the ray rather than returning nothing.
		if (near <= 0.0f)
		{
			float length = glm::length(direction);
			normal = (length > 0.0f) ? -direction / length : glm::vec3(0.0f, 1.0f, 0.0f);
		}

		return true;
	}

	RaycastHit3D Raycast3D::Against(const Scene& scene, const glm::vec3& origin,
		const glm::vec3& direction, float maxDistance, EntityId ignore)
	{
		RaycastHit3D result;

		float closest = maxDistance;

		for (EntityId entity : scene.GetEntities())
		{
			if (entity == ignore)
				continue;

			const auto* mesh = const_cast<Scene&>(scene).GetComponent<MeshComponent>(entity);
			const auto* transform = const_cast<Scene&>(scene).GetComponent<TransformComponent>(entity);
			if (!mesh || !transform || !mesh->Geometry || !mesh->Visible)
				continue;

			glm::mat4 toWorld = transform->GetTransform();
			glm::mat4 toLocal = glm::inverse(toWorld);

			// Into the object's own space, where its bounds *are* axis aligned
			// however it has been rotated or scaled.
			glm::vec3 localOrigin = glm::vec3(toLocal * glm::vec4(origin, 1.0f));
			// A direction, so w is 0 -- translation must not apply to it.
			glm::vec3 localDirection = glm::vec3(toLocal * glm::vec4(direction, 0.0f));

			// Scaling changes how long the direction is, and with it what
			// `maxDistance` means. Rather than correct for that, test against a
			// generous local distance and measure the real one in world space
			// from the hit point.
			float localLength = glm::length(localDirection);
			if (localLength < 1e-8f)
				continue;

			Aabb box;
			box.Min = mesh->Geometry->GetBoundsMin();
			box.Max = mesh->Geometry->GetBoundsMax();

			float localDistance = 0.0f;
			glm::vec3 localNormal(0.0f);
			if (!AgainstAabb(localOrigin, localDirection / localLength,
				maxDistance * localLength, box, localDistance, localNormal))
				continue;

			glm::vec3 localPoint = localOrigin + (localDirection / localLength) * localDistance;
			glm::vec3 worldPoint = glm::vec3(toWorld * glm::vec4(localPoint, 1.0f));

			float worldDistance = glm::length(worldPoint - origin);
			if (worldDistance > closest)
				continue;

			closest = worldDistance;

			result.Hit = true;
			result.Entity = entity;
			result.Point = worldPoint;
			// Normals do not transform like directions under non-uniform
			// scale -- squashing a box along x tilts its diagonal faces the
			// other way. The inverse transpose is what accounts for that.
			result.Normal = glm::normalize(glm::inverseTranspose(glm::mat3(toWorld)) * localNormal);
			result.Distance = worldDistance;
			result.Fraction = (maxDistance > 0.0f) ? worldDistance / maxDistance : 0.0f;
		}

		return result;
	}

	float Raycast3D::Occlusion(const Scene& scene, const glm::vec3& from,
		const glm::vec3& to, float spread, int rays, EntityId ignore)
	{
		glm::vec3 along = to - from;
		float distance = glm::length(along);
		if (distance < 1e-6f || rays <= 0)
			return 0.0f;

		glm::vec3 direction = along / distance;

		// Two axes across the line, to spread the extra rays over. Any vector
		// not parallel to the line will do to start; up is only a bad choice
		// when the line is vertical, hence the fallback.
		glm::vec3 reference = (std::fabs(direction.y) > 0.99f)
			? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 right = glm::normalize(glm::cross(direction, reference));
		glm::vec3 up = glm::cross(right, direction);

		int blocked = 0;
		for (int i = 0; i < rays; i++)
		{
			glm::vec3 offset(0.0f);
			if (i > 0)
			{
				// The first ray is the line itself; the rest ring around it, so
				// a source just clipping a corner reads as partly blocked.
				float angle = glm::two_pi<float>() * (float)(i - 1) / (float)(rays - 1);
				offset = (right * std::cos(angle) + up * std::sin(angle)) * spread;
			}

			// Both ends move, so the ray stays parallel to the line rather than
			// fanning out and hitting things the source cannot see past.
			glm::vec3 start = from + offset;
			glm::vec3 end = to + offset;
			glm::vec3 ray = end - start;
			float rayLength = glm::length(ray);

			if (rayLength < 1e-6f)
				continue;

			if (Against(scene, start, ray / rayLength, rayLength, ignore).Hit)
				blocked++;
		}

		return (float)blocked / (float)rays;
	}

}
