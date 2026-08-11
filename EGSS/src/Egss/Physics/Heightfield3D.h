#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	// A grid of heights, and the surface those heights describe.
	//
	// This is geometry and nothing else -- no mass, no velocity, no solver --
	// for the same reason `Sat3D` is: it can then be checked against
	// hand-worked answers with no simulation running, which is how every claim
	// below was verified.
	//
	// **The surface is triangles, not a bilinear patch**, and the difference is
	// not cosmetic. A cell has four corner heights, and there are two obvious
	// ways to fill in between them:
	//
	//   * bilinear -- `h00 + fx*(h10-h00) + fz*(h01-h00) + fx*fz*D`, where
	//     `D = h00 - h10 - h01 + h11` is the cell's twist. A smooth saddle.
	//   * two triangles -- the same expression **without** the twist term on
	//     one side of the diagonal, and its mirror on the other.
	//
	// They agree at the four corners and along the shared diagonal, and nowhere
	// else: the gap is `fx*fz*D`, which peaks at a quarter of the twist. A
	// renderer draws triangles, so a physics surface that is bilinear is a
	// surface the character stands on and nobody can see. Triangles here, and
	// the generator that feeds this should answer height queries from it rather
	// than keeping its own.
	//
	// The lattice is laid out to match a mesh wound counter-clockwise seen from
	// above, split along the anti-diagonal: triangle A is
	// `(x0,z0) (x0,z1) (x1,z0)` and triangle B is `(x1,z0) (x0,z1) (x1,z1)`.
	// Change that winding and the normals invert, which is invisible until
	// something is lit -- or until a character is pushed *into* the ground.
	//
	// Coordinates here are **local**: the field is centred on the origin in x
	// and z, spans `Extent` metres each way, and heights are measured from
	// zero. A body places it by adding its own position.
	struct EGSS_API Heightfield3D
	{
		// Samples per side, so the field has (Resolution-1)^2 cells.
		int Resolution = 0;
		float Extent = 0.0f;

		// Row-major by z: `Heights[z * Resolution + x]`.
		std::vector<float> Heights;

		// Cached by SetHeights, because the world bounds are asked for every
		// step and scanning 16k samples to answer is not free.
		float Lowest = 0.0f;
		float Highest = 0.0f;

		// `heights` must hold resolution*resolution samples.
		void SetHeights(int resolution, float extent, const std::vector<float>& heights);

		float CellSize() const
		{
			return Resolution > 1 ? Extent / (float)(Resolution - 1) : Extent;
		}

		float HalfExtent() const { return Extent * 0.5f; }

		bool Empty() const { return Resolution < 2 || Heights.empty(); }

		// A stored sample, clamped to the grid.
		float At(int x, int z) const;

		bool Contains(float localX, float localZ) const
		{
			float half = HalfExtent();
			return localX >= -half && localX <= half
				&& localZ >= -half && localZ <= half;
		}

		// The surface directly above or below (localX, localZ): its height and
		// the normal of the triangle it belongs to. False outside the field --
		// which is what makes walking off the edge a fall rather than a
		// clamped-height ledge stretching to infinity.
		bool SurfaceAt(float localX, float localZ, float& outHeight, glm::vec3& outNormal) const;

		// The nearest point of the surface to `local`, searching only the cells
		// within `searchRadius` horizontally.
		//
		// `outDepth` is the distance from the surface **measured along the
		// winning triangle's normal**, so it is negative when `local` is under
		// the surface and a caller can subtract a radius from it without a
		// second sign test. That is the projected distance rather than the
		// straight-line one: they are the same over a face and the projection
		// is smaller near an edge, which is the right way round -- it treats
		// the contact as if the triangle's plane extended a little, and a plane
		// contact is what the solver is stable on.
		bool ClosestPoint(const glm::vec3& local, float searchRadius,
			glm::vec3& outPoint, glm::vec3& outNormal, float& outDepth) const;
	};

}
