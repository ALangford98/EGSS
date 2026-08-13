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
		static MeshData Mesh(const VoxelField3D& field,
			const glm::ivec3& min, const glm::ivec3& max);

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
