#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Mesh.h"
#include "Egss/Voxel/VoxelField3D.h"

#include <glm/glm.hpp>

namespace Egss {

	// Marching cubes: the isosurface of a `VoxelField3D`, as triangles.
	//
	// The algorithm is a lookup. Each cell has eight corners, each corner is
	// inside or outside, so there are 2^8 = 256 arrangements, and for each one
	// a table says which of the cell's twelve edges the surface crosses and how
	// to join those crossings into triangles. Where along an edge the crossing
	// goes is linear interpolation between the two corner distances -- which is
	// the entire reason the field stores a distance rather than a flag, and the
	// difference between a smooth hillside and a staircase.
	//
	// **There is one table here, not the usual two.** Published implementations
	// carry an `edgeTable` of which edges each case uses alongside the triangle
	// table, and the two can disagree -- a transcription slip in one produces a
	// hole that is invisible until something falls through it. The edge mask is
	// derived from the triangle table at startup instead, so the question
	// cannot be answered two ways.
	//
	// **Chunks are meshed with a one-cell overlap.** A chunk's last cell needs
	// the first lattice plane of its neighbour, and meshing without it leaves a
	// crack at every chunk boundary -- the classic voxel seam. `Mesh` takes a
	// lattice range and reads whatever the field gives it, so an overlapping
	// range is the whole fix.
	class EGSS_API MarchingCubes
	{
	public:
		// The isosurface over `[min, max)` cells of the field. Pass the whole
		// field for one mesh, or a chunk's range plus one for a seamless piece
		// of one.
		//
		// Normals come from the field's gradient rather than from the triangle,
		// for the same reason `Heightfield3D` has both a face normal and a
		// smooth one: a face normal is constant across a triangle and makes a
		// smooth surface look faceted at exactly the scale of the lattice.
		//
		// `stride` reads every `stride`-th lattice point instead of every one,
		// so a cell spans `stride` voxels and the mesh comes out coarser --
		// the same field, fewer triangles, for a chunk far enough away that
		// the difference is not worth the vertices. Corner reads still go
		// through `DistanceAt`, which clamps, so a stride that does not
		// evenly divide the range just clips the last partial cell rather
		// than reading outside the field.
		//
		// **Chunks of different strides do not stitch.** A stride-2 chunk's
		// lattice does not share corners with a stride-1 neighbour's, so
		// there is a seam at that boundary -- proper transition cells
		// (transvoxel-style) would close it and are not implemented. Keep a
		// stride change away from the camera, where the gap is a few pixels
		// wide at most, and treat this as a known limitation rather than a
		// bug: see the OpenWorld demo's LOD banding for how it is kept out
		// of view in practice.
		// `skirtDepth` (in world units, 0 to disable) extrudes the mesh's open
		// boundary straight down by that much.
		//
		// This is the mitigation for the seam described above, not a fix for
		// it. Two chunks on different strides genuinely disagree about where
		// the surface is -- marching cubes on a coarser lattice cuts corners,
		// so on a convex dome the coarse chunk meshes *systematically lower*
		// than its fine neighbour, and what you get is not a hairline crack
		// but a trench you can see into. A skirt hangs a wall off the edge of
		// every chunk so the gap is closed by geometry: the step in the
		// surface is still there, but it is a step rather than a hole.
		//
		// The depth wants to cover the worst vertical disagreement, which is
		// about the terrain's slope times one coarse cell.
		static MeshData Mesh(const VoxelField3D& field,
			const glm::ivec3& min, const glm::ivec3& max, int stride = 1,
			float skirtDepth = 0.0f);

		static MeshData Mesh(const VoxelField3D& field)
		{
			return Mesh(field, glm::ivec3(0), field.Size() - glm::ivec3(1));
		}

		// Which edges case `index` uses, as a 12-bit mask. Derived from the
		// triangle table; exposed so the table can be checked rather than
		// trusted.
		static unsigned int EdgeMask(int index);

		// The triangle table row for a case: edge indices in threes, terminated
		// by -1. Exposed for the same reason.
		static const signed char* Triangles(int index);

		// The two corners an edge joins, 0-7.
		static void EdgeCorners(int edge, int& outA, int& outB);

		// A corner's offset within the cell, in lattice steps.
		static glm::ivec3 CornerOffset(int corner);
	};

}
