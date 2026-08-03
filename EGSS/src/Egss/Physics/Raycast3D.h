#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Scene/Scene.h"

#include <glm/glm.hpp>

namespace Egss {

	// An axis-aligned box, in whatever space the caller is working in.
	struct EGSS_API Aabb
	{
		glm::vec3 Min = glm::vec3(0.0f);
		glm::vec3 Max = glm::vec3(0.0f);

		glm::vec3 Centre() const { return (Min + Max) * 0.5f; }
		glm::vec3 Size() const { return Max - Min; }
	};

	// What a 3D ray found. The 2D counterpart in PhysicsWorld2D reports the
	// same things, deliberately -- the two are different dimensions of one idea
	// and callers should not have to learn two shapes.
	struct EGSS_API RaycastHit3D
	{
		bool Hit = false;
		EntityId Entity = InvalidEntity;

		glm::vec3 Point = glm::vec3(0.0f);
		// Surface normal at the hit, pointing back towards the ray's origin.
		glm::vec3 Normal = glm::vec3(0.0f);

		float Distance = 0.0f;
		// Where along the ray, 0..1 of maxDistance.
		float Fraction = 0.0f;
	};

	// Ray casting in three dimensions.
	//
	// This exists because `PhysicsWorld2D::Raycast` is 2D, and two things that
	// wanted it are not: 3D audio emitters cannot be occluded, and `Acoustics2D`
	// is 2D purely because the ray it stands on is. Neither needs a full 3D
	// rigid-body world -- they need to know what is in the way.
	//
	// Boxes only, and deliberately: a mesh's bounds are already computed, an
	// AABB test is a handful of divides, and per-triangle accuracy buys almost
	// nothing for occlusion. What it does buy is cost -- a torus would go from
	// one test to 2,304.
	class EGSS_API Raycast3D
	{
	public:
		// The slab method: clip the ray against each pair of parallel planes in
		// turn and see whether an interval survives. `direction` need not be
		// normalised, but `maxDistance` is measured in units of its length if
		// it is not, so normalise it unless you know why you are not.
		//
		// Returns the near hit. A ray starting *inside* the box reports a hit
		// at distance 0 with a normal facing back down the ray -- there is no
		// correct answer there, and this at least never hands back a
		// zero-length normal.
		static bool AgainstAabb(const glm::vec3& origin, const glm::vec3& direction,
			float maxDistance, const Aabb& box, float& distance, glm::vec3& normal);

		// Every entity with a mesh and a transform, nearest hit wins.
		//
		// The box is the mesh's own bounds, and the ray is transformed into
		// each object's local space rather than the box into world space. That
		// is what makes rotation work: a rotated box is not an AABB in world
		// space, but the ray is still a ray in any space you put it in.
		static RaycastHit3D Against(const Scene& scene, const glm::vec3& origin,
			const glm::vec3& direction, float maxDistance,
			EntityId ignore = InvalidEntity);

		// How blocked the straight line from `from` to `to` is, 0 clear to 1
		// fully blocked.
		//
		// Graded rather than yes/no, by casting several rays spread across a
		// disc facing the line. A source half behind a pillar should not flip
		// between silent and clear as it moves a millimetre -- the same reason
		// the 2D audio occlusion grades its answer.
		static float Occlusion(const Scene& scene, const glm::vec3& from,
			const glm::vec3& to, float spread = 0.35f, int rays = 5,
			EntityId ignore = InvalidEntity);
	};

}
