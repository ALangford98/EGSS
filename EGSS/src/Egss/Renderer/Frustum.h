#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/Raycast3D.h"

#include <glm/glm.hpp>

namespace Egss {

	// Six half-spaces bounding what a camera can see, extracted straight from
	// its view-projection matrix (Gribb/Hartmann) rather than rebuilt from
	// FOV/aspect/near/far. The matrix already carries whatever projection
	// produced it -- perspective or orthographic -- so one extraction works
	// for both camera kinds Renderer::BeginScene accepts, and there is no
	// second place for a frustum shape to disagree with the one actually
	// drawing.
	//
	// This is geometry and nothing else, for the same reason
	// Heightfield3D/VoxelField3D/Sat3D are: checkable against hand-placed
	// boxes with no GL context and no camera involved.
	struct EGSS_API Frustum
	{
		// Left, Right, Bottom, Top, Near, Far, each (a, b, c, d) with
		// a*x + b*y + c*z + d >= 0 meaning "on the inside of this plane".
		// Normalised, so the same planes double as signed distances later --
		// a distance-banded LOD is the reason this exists.
		glm::vec4 Planes[6];

		static Frustum FromViewProjection(const glm::mat4& viewProjection);

		// False only once some plane rejects every one of the box's eight
		// corners. A box that straddles a frustum edge without any single
		// plane clearing it is reported as visible -- the standard
		// conservative test: it can false-positive near a frustum corner and
		// never false-negative, which is the safe direction for culling
		// (worst case, something offscreen still gets drawn; never the
		// reverse).
		bool Intersects(const Aabb& box) const;
	};

}
