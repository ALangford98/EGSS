#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Voxel/VoxelField3D.h"
#include "Egss/Physics/RigidBody3D.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Egss {

	// What is left holding a piece of terrain up, and what is not.
	//
	// Dig away the base of a pillar and the top of it is still sitting there,
	// solid, connected to nothing. Finding those pieces is a **connected
	// component labelling** over the solid voxels: flood fill outwards from
	// everything anchored, and whatever the fill never reaches is no longer
	// attached to the world.
	//
	// This is the standard approach rather than a clever one. Teardown separates
	// blasted objects into disconnected chunks and makes a new body per chunk;
	// Müller et al. (SIGGRAPH 2013) detect islands in a convex decomposition
	// after partial destruction for the same purpose. The interesting part is
	// not the algorithm, it is what you do with the answer.
	//
	// **Connectivity alone is not structural integrity**, and this deliberately
	// only answers the connectivity half. A pillar attached to the world by a
	// single voxel is, to this code, attached -- so a mountain can hang from a
	// thread. Making that break is the *next* piece, and it needs a load and a
	// strength per connection rather than a yes or no. What lives here is the
	// part that has to happen either way: once something is severed, find it.
	struct EGSS_API VoxelIsland
	{
		// Lattice coordinates of every solid voxel in the piece.
		std::vector<glm::ivec3> Voxels;

		// Inclusive lattice bounds, and the same in world space.
		glm::ivec3 Min = { 0, 0, 0 };
		glm::ivec3 Max = { 0, 0, 0 };

		glm::vec3 WorldMin = { 0.0f, 0.0f, 0.0f };
		glm::vec3 WorldMax = { 0.0f, 0.0f, 0.0f };

		// Centre of the voxels, weighted equally -- which is the centre of mass
		// for a piece of uniform material, and where a body made from it should
		// be placed.
		glm::vec3 Centre = { 0.0f, 0.0f, 0.0f };

		// Voxels times the volume of one, so a body can be given an honest mass
		// rather than a guessed one.
		float Volume = 0.0f;

		// --- The box to hand the solver ---------------------------------
		//
		// `Sat3D` is convex-only, so a severed piece has to be approximated by
		// something convex, and a box is what this engine has. **Oriented, not
		// axis-aligned**: a slab that broke off a sloping cliff is not aligned
		// with the world, and an axis-aligned box around it is mostly air --
		// which lands the rock on a corner and rests it at an angle nothing
		// about the rock justifies.
		//
		// The axes are the piece's own principal axes, from the covariance of
		// its voxels: the direction the material is most spread along, then the
		// most spread of what is left. For a slab that recovers the slab.
		//
		// It is still one convex box. An L-shaped piece is not improved by
		// orienting the box around it, and that is what convex decomposition
		// would fix.
		glm::quat Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 HalfExtents = glm::vec3(0.0f);

		// Where that box sits, which is **not** `Centre`.
		//
		// `Centre` is the centre of mass and is where a *compound* body goes,
		// because a rigid body's position has to be its centre of mass -- put it
		// anywhere else and gravity applies a torque that should not exist. The
		// box's own centre is the middle of the piece's extents, and for
		// anything but a symmetric lump the two are different points.
		glm::vec3 BoxCentre = glm::vec3(0.0f);
	};

	class EGSS_API VoxelIslands
	{
	public:
		// Every solid voxel not connected to an anchored one, grouped.
		//
		// `anchorHeight` is the lattice y at or below which solid voxels count
		// as attached to the world -- bedrock. Everything else is anchored only
		// by being connected, through solid voxels, to that.
		//
		// **Six-connected, not twenty-six.** Two voxels that meet only at a
		// corner share no face, transmit no load, and calling them connected
		// makes a diagonal chain of single voxels hold up a cliff. Face
		// adjacency is also what a stress model will need, so the two agree.
		//
		// `minVoxels` drops pieces too small to be worth simulating. A voxel
		// world can shed thousands of single-voxel crumbs from one edit, and the
		// broadphase measurements in this project say a few hundred loose bodies
		// is where it starts to cost -- so the small ones are better deleted
		// than dropped. They are still removed from the field by `Extract`.
		static std::vector<VoxelIsland> Find(const VoxelField3D& field,
			int anchorHeight = 0, size_t minVoxels = 1);

		// Removes an island's voxels from `field` and returns them as a field of
		// their own, sized to the island plus a one-voxel skirt of air.
		//
		// The skirt matters: marching cubes needs a sign change to place a
		// vertex, so a piece whose solid voxels run to the very edge of its field
		// meshes as an open shell rather than a closed lump. The clamped reads at
		// the border would repeat the solid value outwards forever.
		//
		// The returned field's `Origin` is in the same world frame as the
		// source's, so a body made from it sits where the rock was.
		static VoxelField3D Extract(VoxelField3D& field, const VoxelIsland& island);

		// The island as a set of boxes that between them fill exactly its
		// voxels, for a `Compound` collider.
		//
		// **Greedy growing, not a real convex decomposition.** A proper one
		// (V-HACD, or the volumetric approximate convex decomposition Müller et
		// al. fracture with) finds few convex pieces of arbitrary shape. This
		// finds many axis-aligned boxes, which is worse in count and better in
		// every other way for voxels: the pieces are exact rather than
		// approximate, the algorithm is thirty lines, and the collider it
		// produces is one this engine already has.
		//
		// A seed voxel is grown along x while it can, then that run is grown
		// along y as a slab, then along z as a box -- so a wall comes out as one
		// box rather than a thousand. The boxes tile the island: every solid
		// voxel is in exactly one, and none overlap.
		//
		// Offsets are relative to `island.Centre` and turned into the island's
		// own frame, so the result drops straight into `MakeCompound` beside
		// `island.Orientation`.
		static std::vector<CompoundChild> Decompose(const VoxelField3D& field,
			const VoxelIsland& island);
	};

}
