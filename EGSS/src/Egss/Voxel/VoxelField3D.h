#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

#include <functional>
#include <unordered_map>

namespace Egss {

	// A signed distance field on a regular grid, chunked so an edit costs one
	// chunk rather than a world.
	//
	// This is geometry and nothing else -- no mass, no velocity, no solver --
	// for the same reason `Heightfield3D` and `Sat3D` are: it can then be
	// checked against hand-worked answers with no simulation running.
	//
	// **A distance, not an occupancy flag.** Storing "solid or not" is the
	// obvious thing and gives you Minecraft: the surface can only ever lie on a
	// cell boundary, so it is blocky whatever you mesh it with. Storing how far
	// each lattice point is from the surface puts the crossing *between*
	// samples, which is what lets marching cubes place a vertex at the real
	// surface and what makes a 0.5 m grid look smooth rather than 0.5 m chunky.
	//
	// It also means **the field is the collider**. A heightfield is queried
	// directly by the narrowphase rather than being turned into triangles first
	// (see `Heightfield3D::ClosestPoint`), and an SDF answers the same question
	// more cheaply: the value *is* the depth and its gradient *is* the normal.
	// Meshing is then a rendering concern only, which is the arrangement that
	// avoids ever collidng against marching-cubes output -- long thin triangles
	// that intersection maths handles badly.
	//
	// **Negative inside, positive outside**, which is the usual convention and
	// the one that makes `Distance < 0` read as "solid".
	//
	// **The field has to be Lipschitz** -- moving one voxel may change the
	// stored distance by at most one voxel, which a real distance obeys and an
	// arbitrary density function does not. Two things depend on it: the sparse
	// storage below decides a chunk is entirely solid or entirely air by
	// checking that it is more than a voxel from any surface, which is only
	// sound if the field cannot cross zero faster than that; and sphere tracing
	// a ray through it terminates for the same reason.
	class EGSS_API VoxelField3D
	{
	public:
		// Voxels per side of a chunk. 16 is 4,096 lattice points, which is a
		// remesh small enough to hide inside a frame and large enough that the
		// per-chunk bookkeeping is not most of the cost.
		static constexpr int ChunkSize = 16;

		// What an unwritten chunk reads as. Large enough that no interpolation
		// against it can produce a crossing, which is what makes an unallocated
		// chunk indistinguishable from one filled with air.
		static constexpr float Far = 1000.0f;

		// A chunk is kept sparse only when every sample in it is further than
		// this from the surface, in voxels. One would do for a Lipschitz field;
		// two is a margin, and costs a handful of chunks around the surface.
		static constexpr float SparseBandVoxels = 2.0f;

		// `size` is lattice points per axis, so a field of N points spans N-1
		// cells and (N-1) * voxelSize metres. `origin` is the world position of
		// lattice point (0,0,0).
		void Create(const glm::ivec3& size, float voxelSize, const glm::vec3& origin);

		bool Empty() const { return m_Size.x < 2 || m_Size.y < 2 || m_Size.z < 2; }

		const glm::ivec3& Size() const { return m_Size; }
		float VoxelSize() const { return m_VoxelSize; }
		const glm::vec3& Origin() const { return m_Origin; }
		glm::ivec3 ChunkCount() const { return m_Chunks; }

		glm::vec3 PositionOf(int x, int y, int z) const
		{
			return m_Origin + glm::vec3((float)x, (float)y, (float)z) * m_VoxelSize;
		}

		bool Contains(int x, int y, int z) const
		{
			return x >= 0 && y >= 0 && z >= 0
				&& x < m_Size.x && y < m_Size.y && z < m_Size.z;
		}

		// Clamped at the border rather than wrapped or asserted: a mesher walks
		// cells and would otherwise need a special case at every face, and a
		// clamped read is the same answer the border sample already gives.
		float DistanceAt(int x, int y, int z) const;
		unsigned char MaterialAt(int x, int y, int z) const;

		bool Solid(int x, int y, int z) const { return DistanceAt(x, y, z) < 0.0f; }

		void Set(int x, int y, int z, float distance, unsigned char material);
		void SetDistance(int x, int y, int z, float distance);

		// Trilinear, in world space. This is what the narrowphase will ask, and
		// it is continuous across cell boundaries -- a nearest-sample read is
		// not, and a contact normal that jumps at every cell face is a body that
		// buzzes.
		float SampleDistance(const glm::vec3& world) const;

		// The field's gradient, normalised: the surface normal, from central
		// differences one voxel apart. For a true distance field the gradient
		// has unit length already, so a normalise here is cheap insurance
		// rather than a correction.
		glm::vec3 SampleNormal(const glm::vec3& world) const;

		// Where a ray first meets the surface, by **sphere tracing**: the field
		// says how far it is safe to travel, so each step is the largest one
		// that cannot pass through anything. That is the whole algorithm, and it
		// is why the field has to be a real distance -- with a value that
		// overstates how far the surface is, a step jumps straight over it.
		//
		// Cheaper than the cell walk `Heightfield3D::Raycast` does, and much
		// cheaper than marching by a fixed increment: across open air the steps
		// are metres long and near a surface they shorten by themselves, so
		// nothing has to choose a step size.
		//
		// `direction` must be normalised. False if nothing is hit inside
		// `maxDistance` or the ray never enters the field.
		bool Raycast(const glm::vec3& origin, const glm::vec3& direction,
			float maxDistance, float& outDistance, glm::vec3& outPoint,
			glm::vec3& outNormal) const;

		// Writes every voxel from `sdf`, which is given a world position and
		// returns a signed distance in metres.
		//
		// Chunks entirely beyond the band are left unallocated, which is most of
		// a terrain: rock well underground and air well above it are both a
		// single float. Measured on the test field, that is the difference
		// between the whole lattice and about a tenth of it.
		void Fill(const std::function<float(const glm::vec3&)>& sdf,
			unsigned char material = 1);

		// The same per-voxel logic as Fill, scoped to one chunk -- what makes
		// lazy, distance-driven generation possible without evaluating (or
		// even declaring the content of) the whole field upfront. A
		// coordinate outside ChunkCount() is a no-op.
		//
		// Marks nothing dirty, the same as Fill does not -- a demo that calls
		// Fill still has to call MarkAllDirty itself before the first mesh.
		// One thing Fill does not have to account for: this chunk's low-x/
		// low-y/low-z neighbours, if they are already meshed, each read one
		// plane of *this* chunk for their own seam overlap (see ChunkRange).
		// Filling a chunk that was previously unallocated changes that whole
		// plane, so a caller streaming chunks in has to mark this chunk *and*
		// those three neighbours dirty, not just this one, or the seam keeps
		// the old (empty) surface until something else disturbs it.
		void FillChunk(const glm::ivec3& chunk,
			const std::function<float(const glm::vec3&)>& sdf,
			unsigned char material = 1);

		// **One value for the whole chunk, without visiting its voxels.**
		//
		// A caller that already knows a chunk is solid rock or open sky can
		// say so directly. Going through `FillChunk` with a constant generator
		// gets the same answer by allocating a 16 KB scratch buffer, writing
		// 4,096 identical floats into it and then discovering they are all the
		// same -- which is most of the cost of a chunk that has nothing in it,
		// and on a planet most chunks have nothing in them.
		void SetUniform(const glm::ivec3& chunk, float distance,
			unsigned char material = 1);

		// Allocated chunks, and the total the lattice could hold. For checking
		// that the sparse storage is actually sparse rather than assuming it.
		size_t AllocatedChunks() const;
		size_t TotalChunks() const
		{
			return (size_t)glm::max(m_Chunks.x, 0) * glm::max(m_Chunks.y, 0)
				* glm::max(m_Chunks.z, 0);
		}
		// Chunks the map is actually holding, uniform ones included.
		size_t StoredChunks() const { return m_Storage.size(); }
		size_t AllocatedBytes() const;

		// **Forget one chunk entirely**, so it goes back to reading as `Far`.
		//
		// A streamer that walks a body of any size has to give storage back or
		// it accumulates every chunk it ever touched. On a small field that is
		// a rounding error; on a planet it is the whole planet.
		void ClearChunk(const glm::ivec3& chunk);

		// --- Persistence ---------------------------------------------------
		//
		// One chunk's contents as bytes, and back. A field is expensive to
		// *produce* -- a density evaluation per voxel, 4,096 of them a chunk --
		// and cheap to store, because a chunk that never comes near the surface
		// is uniform and collapses to two numbers.
		//
		// Deliberately per chunk rather than a whole-field Save/Load: streaming
		// wants one chunk at a time, and a format that has to be read end to end
		// before the first chunk is usable would defeat the point of streaming.
		// What the bytes are *stored in* -- one file, many files, a database --
		// is the caller's business and is not decided here.
		void SaveChunk(const glm::ivec3& chunk, std::vector<unsigned char>& out) const;

		// Returns false and changes nothing if the bytes are not the right
		// length for this field's chunk size, which is the one corruption that
		// can be detected locally. Everything else -- whether these bytes came
		// from the same *world* -- is the caller's to check, and OpenWorld's
		// chunk cache does it by fingerprinting the density function.
		bool LoadChunk(const glm::ivec3& chunk, const unsigned char* data, size_t size);

		// --- Editing ------------------------------------------------------
		//
		// Digging is a min or a max against another distance field, which is
		// the whole reason to store a distance rather than a flag: the union of
		// two shapes is the smaller of their two distances, and subtracting one
		// from the other is the larger of `d` and `-other`. No special cases, no
		// resampling, and the result is still a distance field.
		//
		// It is only *approximately* a distance field afterwards -- min and max
		// are exact at the surface and can leave the interior reading further
		// from the surface than it is. That is the safe direction for sphere
		// tracing (a step that is too short never overshoots) but the wrong one
		// for the sparse-chunk test, so an edited chunk stays allocated.

		// Carves a sphere out of the field, or adds one. Returns the number of
		// lattice points whose sign changed, which is what "did this edit do
		// anything" means.
		int EditSphere(const glm::vec3& centre, float radius, bool add);

		// Chunks whose contents changed since the last call, as chunk
		// coordinates. Taking them clears the list -- a mesher asks once per
		// frame and rebuilds exactly those.
		//
		// Neighbours are included when an edit touches a chunk's edge: a cell
		// spanning a boundary is meshed by the chunk on the low side, so the
		// chunk *below* an edited boundary has to be rebuilt too or the seam
		// keeps the old surface.
		const std::vector<glm::ivec3>& DirtyChunks() const { return m_Dirty; }
		void ClearDirtyChunks() { m_Dirty.clear(); }
		void MarkAllDirty();

		// For anything that writes through `Set` directly rather than through
		// `EditSphere` -- island extraction does, and a mesher that is never
		// told will keep drawing rock that is no longer there.
		void MarkDirtyAt(int x, int y, int z) { MarkDirty(x, y, z); }

		// The chunk a lattice point belongs to, and the lattice range a chunk
		// covers -- including the one-cell overlap a seamless mesh needs.
		glm::ivec3 ChunkOf(int x, int y, int z) const
		{
			return { x / ChunkSize, y / ChunkSize, z / ChunkSize };
		}

		void ChunkRange(const glm::ivec3& chunk, glm::ivec3& outMin, glm::ivec3& outMax) const
		{
			outMin = chunk * ChunkSize;
			outMax = glm::min(outMin + glm::ivec3(ChunkSize), m_Size - glm::ivec3(1));
		}
	private:
		// **Sparse, keyed by the packed chunk index.**
		//
		// It was a dense `std::vector<Chunk>`, one entry per chunk of the
		// lattice whether anything was ever written to it or not. That is 56
		// bytes a chunk, which is nothing for a field a few hundred voxels
		// across and is the reason a planet could not be bigger than about
		// 20 km: Earth at its own radius and 1.5 m voxels is 5.8e5 chunks a
		// side, which is 1.9e17 of them and 1.1e10 GB of empty structs. The
		// map holds only what has been filled, which is what the streamer
		// keeps near the player and nothing else.
		struct Chunk
		{
			// Empty while the chunk is uniform. Allocated on the first write,
			// filled with Uniform so the values that were implied stay implied.
			std::vector<float> Distance;
			std::vector<unsigned char> Material;
			float Uniform = Far;
			unsigned char UniformMaterial = 0;
		};

		size_t ChunkIndexOf(int cx, int cy, int cz) const
		{
			return ((size_t)cz * m_Chunks.y + cy) * m_Chunks.x + cx;
		}

		static size_t IndexInChunk(int x, int y, int z)
		{
			return ((size_t)z * ChunkSize + y) * ChunkSize + x;
		}

		// Whole-chunk lookup that never creates. The read paths go through it.
		const Chunk& ChunkAt(int cx, int cy, int cz) const;

		Chunk& ChunkFor(int x, int y, int z, int& outLocalX, int& outLocalY, int& outLocalZ);
		const Chunk& ChunkFor(int x, int y, int z, int& outLocalX, int& outLocalY, int& outLocalZ) const;

		// The body of Fill's triple loop, shared with FillChunk.
		void FillOneChunk(int cx, int cy, int cz,
			const std::function<float(const glm::vec3&)>& sdf, unsigned char material);

		glm::ivec3 m_Size = { 0, 0, 0 };
		glm::ivec3 m_Chunks = { 0, 0, 0 };
		float m_VoxelSize = 1.0f;
		glm::vec3 m_Origin = { 0.0f, 0.0f, 0.0f };

		void MarkDirty(int x, int y, int z);

		std::unordered_map<size_t, Chunk> m_Storage;

		// Returned for any chunk the map has never been given, so a read of
		// untouched space is the same `Far` it always was.
		static const Chunk s_Absent;
		std::vector<glm::ivec3> m_Dirty;
	};

}
