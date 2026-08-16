#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Mesh.h"
#include "Egss/Voxel/VoxelField3D.h"

namespace Egss {

	// Surface extraction from tetrahedra, intended as the foundation for
	// stitching two lattices of different resolutions together.
	//
	// **Nothing consumes this yet.** The transition cell it was built for was
	// attempted and reverted -- subdividing a coarse cell's seam face while
	// leaving its side faces coarse puts T-junctions on the cell's own
	// boundary, which opened more holes than it closed on a 1:2 seam. Doing it
	// properly needs subdivision per *edge* with each face triangulated to
	// match, cascading into neighbours, which is the geometry transvoxel's
	// tables encode. See the changelog.
	//
	// What is here is verified on its own terms and is kept for that: six
	// tetrahedra tiling a cell to 1.000000000, a sphere within 0.04% of
	// 4*pi*r^2 and 0.09% of (4/3)*pi*r^3, and every edge of it used by exactly
	// two triangles.
	//
	// **Why tetrahedra, when the chunks are cubes.** Marching cubes has 256
	// cases and several of them are ambiguous -- a face with two diagonal
	// corners inside can be resolved two ways, and two neighbouring cells that
	// choose differently leave a hole. Transvoxel handles the LOD seam with a
	// second table of 512 transition cases, which is the correct answer and is
	// a table you either have or you do not.
	//
	// A tetrahedron has **four corners, so sixteen cases**, and none of them is
	// ambiguous: three corners on one side of the surface always give one
	// triangle, two-and-two always give one quad, and there is no configuration
	// where the contour could be drawn two ways. Sixteen cases is few enough
	// that the polygon can be *derived from the sign pattern at runtime* rather
	// than looked up -- which means there is no table to get wrong, and the
	// whole thing can be checked by exhausting all sixteen.
	//
	// The property that makes this the right primitive for a seam: two tetrahedra
	// sharing a face agree on that face's contour exactly, because a triangular
	// face has three corners and a straight segment between two edge crossings
	// is the only contour three corners can produce. Cracks between cells are
	// impossible by construction rather than by careful table design.
	class EGSS_API MarchingTetrahedra
	{
	public:
		// One tetrahedron. `value` is the field at each corner, negative inside
		// the solid -- the same convention `VoxelField3D::Solid` uses. Appends
		// to `data` rather than returning, so a caller can accumulate a whole
		// region without joining meshes afterwards.
		//
		// Emits nothing when all four corners are on the same side.
		static void Cell(const glm::vec3 position[4], const float value[4], MeshData& data);

		// A whole region, by splitting every cell into six tetrahedra.
		//
		// The split is the same for every cell (all six tets share the cell's
		// main diagonal), which is what makes neighbouring cells agree on their
		// shared face: both see it divided by the same diagonal. A split chosen
		// per cell would reintroduce exactly the cracks this exists to avoid.
		static MeshData Mesh(const VoxelField3D& field,
			const glm::ivec3& min, const glm::ivec3& max, int stride = 1);

		// The six tetrahedra of a unit cell, as corner indices into the usual
		// 0-7 cube numbering. Exposed so the decomposition can be checked --
		// it must use every cell exactly once and leave no gaps.
		static const int (*CellTetrahedra())[4];
	};

}
