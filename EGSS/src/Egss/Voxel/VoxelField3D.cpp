#include "egsspch.h"
#include "Egss/Voxel/VoxelField3D.h"

namespace Egss {

	void VoxelField3D::Create(const glm::ivec3& size, float voxelSize, const glm::vec3& origin)
	{
		m_Size = glm::max(size, glm::ivec3(0));
		m_VoxelSize = glm::max(voxelSize, 1e-4f);
		m_Origin = origin;

		// Rounded up, so the last chunk is partly outside the field. Those
		// samples are never read -- every accessor clamps to Size first -- and
		// the alternative is a field whose dimensions have to be multiples of
		// the chunk size, which pushes the constraint onto every caller.
		m_Chunks = glm::ivec3(
			(m_Size.x + ChunkSize - 1) / ChunkSize,
			(m_Size.y + ChunkSize - 1) / ChunkSize,
			(m_Size.z + ChunkSize - 1) / ChunkSize);

		// Nothing is allocated here any more. The map fills as chunks are
		// written to, which is what lets the lattice be a planet across.
		m_Storage.clear();
	}

	const VoxelField3D::Chunk VoxelField3D::s_Absent;

	// **The const path must not create.** The non-const one below does, which
	// is why the two are written out separately rather than one delegating to
	// the other through a `const_cast` -- that older shape would have handed a
	// writer a reference to the shared absent chunk and let it scribble on
	// every unwritten chunk in the field at once.
	const VoxelField3D::Chunk& VoxelField3D::ChunkAt(int cx, int cy, int cz) const
	{
		auto it = m_Storage.find(ChunkIndexOf(cx, cy, cz));

		return it == m_Storage.end() ? s_Absent : it->second;
	}

	VoxelField3D::Chunk& VoxelField3D::ChunkFor(int x, int y, int z,
		int& outLocalX, int& outLocalY, int& outLocalZ)
	{
		outLocalX = x % ChunkSize;
		outLocalY = y % ChunkSize;
		outLocalZ = z % ChunkSize;

		// Creates the entry if it is not there, which is what a writer wants
		// and exactly what the const path above must never do.
		return m_Storage[ChunkIndexOf(x / ChunkSize, y / ChunkSize, z / ChunkSize)];
	}

	const VoxelField3D::Chunk& VoxelField3D::ChunkFor(int x, int y, int z,
		int& outLocalX, int& outLocalY, int& outLocalZ) const
	{
		outLocalX = x % ChunkSize;
		outLocalY = y % ChunkSize;
		outLocalZ = z % ChunkSize;

		return ChunkAt(x / ChunkSize, y / ChunkSize, z / ChunkSize);
	}

	float VoxelField3D::DistanceAt(int x, int y, int z) const
	{
		if (m_Chunks.x <= 0)
			return Far;

		x = glm::clamp(x, 0, m_Size.x - 1);
		y = glm::clamp(y, 0, m_Size.y - 1);
		z = glm::clamp(z, 0, m_Size.z - 1);

		int lx, ly, lz;
		const Chunk& chunk = ChunkFor(x, y, z, lx, ly, lz);

		return chunk.Distance.empty()
			? chunk.Uniform
			: chunk.Distance[IndexInChunk(lx, ly, lz)];
	}

	unsigned char VoxelField3D::MaterialAt(int x, int y, int z) const
	{
		if (m_Chunks.x <= 0)
			return 0;

		x = glm::clamp(x, 0, m_Size.x - 1);
		y = glm::clamp(y, 0, m_Size.y - 1);
		z = glm::clamp(z, 0, m_Size.z - 1);

		int lx, ly, lz;
		const Chunk& chunk = ChunkFor(x, y, z, lx, ly, lz);

		return chunk.Material.empty()
			? chunk.UniformMaterial
			: chunk.Material[IndexInChunk(lx, ly, lz)];
	}

	void VoxelField3D::Set(int x, int y, int z, float distance, unsigned char material)
	{
		if (m_Chunks.x <= 0 || !Contains(x, y, z))
			return;

		int lx, ly, lz;
		Chunk& chunk = ChunkFor(x, y, z, lx, ly, lz);

		// Materialising a uniform chunk keeps the value it was standing for, so
		// writing one voxel does not quietly zero the other 4,095.
		if (chunk.Distance.empty())
		{
			const size_t count = (size_t)ChunkSize * ChunkSize * ChunkSize;
			chunk.Distance.assign(count, chunk.Uniform);
			chunk.Material.assign(count, chunk.UniformMaterial);
		}

		size_t index = IndexInChunk(lx, ly, lz);
		chunk.Distance[index] = distance;
		chunk.Material[index] = material;
	}

	void VoxelField3D::SetDistance(int x, int y, int z, float distance)
	{
		Set(x, y, z, distance, MaterialAt(x, y, z));
	}

	float VoxelField3D::SampleDistance(const glm::vec3& world) const
	{
		if (Empty())
			return Far;

		glm::vec3 local = (world - m_Origin) / m_VoxelSize;

		// floor, not truncation: a negative coordinate truncates towards zero
		// and would read the cell on the wrong side of the origin.
		glm::vec3 base = glm::floor(local);
		glm::vec3 t = local - base;

		int x = (int)base.x, y = (int)base.y, z = (int)base.z;

		float d000 = DistanceAt(x, y, z);
		float d100 = DistanceAt(x + 1, y, z);
		float d010 = DistanceAt(x, y + 1, z);
		float d110 = DistanceAt(x + 1, y + 1, z);
		float d001 = DistanceAt(x, y, z + 1);
		float d101 = DistanceAt(x + 1, y, z + 1);
		float d011 = DistanceAt(x, y + 1, z + 1);
		float d111 = DistanceAt(x + 1, y + 1, z + 1);

		float d00 = glm::mix(d000, d100, t.x);
		float d10 = glm::mix(d010, d110, t.x);
		float d01 = glm::mix(d001, d101, t.x);
		float d11 = glm::mix(d011, d111, t.x);

		return glm::mix(glm::mix(d00, d10, t.y), glm::mix(d01, d11, t.y), t.z);
	}

	glm::vec3 VoxelField3D::SampleNormal(const glm::vec3& world) const
	{
		// A central difference one voxel apart, on the interpolated field.
		//
		// Differencing the *lattice* first and interpolating the eight corner
		// gradients was tried, on the theory that a trilinear field has a
		// piecewise-constant gradient and would shade in flat cell-sized
		// terraces. It costs eight times the reads and the picture did not
		// change by a pixel -- the terracing that prompted it was a bug in the
		// caller's density function, not in the normals. Reverted rather than
		// kept: a change that measures as nothing is a cost with no benefit.
		const float h = m_VoxelSize;

		glm::vec3 gradient(
			SampleDistance(world + glm::vec3(h, 0.0f, 0.0f)) - SampleDistance(world - glm::vec3(h, 0.0f, 0.0f)),
			SampleDistance(world + glm::vec3(0.0f, h, 0.0f)) - SampleDistance(world - glm::vec3(0.0f, h, 0.0f)),
			SampleDistance(world + glm::vec3(0.0f, 0.0f, h)) - SampleDistance(world - glm::vec3(0.0f, 0.0f, h)));

		float length = glm::length(gradient);
		return length > 1e-12f ? gradient / length : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	void VoxelField3D::FillOneChunk(int cx, int cy, int cz,
		const std::function<float(const glm::vec3&)>& sdf, unsigned char material)
	{
		const float band = SparseBandVoxels * m_VoxelSize;

		Chunk& chunk = m_Storage[ChunkIndexOf(cx, cy, cz)];

		// Evaluated once into a scratch buffer, because deciding whether the
		// chunk is worth allocating needs every sample anyway -- and calling
		// the generator twice for the same point is the sort of thing that
		// is free until the generator is five octaves of noise.
		const size_t count = (size_t)ChunkSize * ChunkSize * ChunkSize;
		std::vector<float> values(count);

		bool allSolid = true;
		bool allAir = true;

		for (int lz = 0; lz < ChunkSize; lz++)
		{
			for (int ly = 0; ly < ChunkSize; ly++)
			{
				for (int lx = 0; lx < ChunkSize; lx++)
				{
					int x = cx * ChunkSize + lx;
					int y = cy * ChunkSize + ly;
					int z = cz * ChunkSize + lz;

					// Outside the field: clamped, so the padding in the last
					// chunk repeats the border rather than asking the
					// generator about somewhere that is not part of the
					// world.
					glm::vec3 position = PositionOf(
						glm::min(x, m_Size.x - 1),
						glm::min(y, m_Size.y - 1),
						glm::min(z, m_Size.z - 1));

					float distance = sdf(position);
					values[IndexInChunk(lx, ly, lz)] = distance;

					allSolid = allSolid && distance < -band;
					allAir = allAir && distance > band;
				}
			}
		}

		if (allAir)
		{
			chunk.Distance.clear();
			chunk.Material.clear();
			chunk.Uniform = Far;
			chunk.UniformMaterial = 0;
			return;
		}

		if (allSolid)
		{
			chunk.Distance.clear();
			chunk.Material.clear();
			chunk.Uniform = -Far;
			chunk.UniformMaterial = material;
			return;
		}

		chunk.Distance = std::move(values);
		chunk.Material.assign(count, 0);

		for (size_t i = 0; i < count; i++)
			chunk.Material[i] = chunk.Distance[i] < 0.0f ? material : 0;
	}

	void VoxelField3D::Fill(const std::function<float(const glm::vec3&)>& sdf,
		unsigned char material)
	{
		if (m_Chunks.x <= 0 || !sdf)
			return;

		for (int cz = 0; cz < m_Chunks.z; cz++)
			for (int cy = 0; cy < m_Chunks.y; cy++)
				for (int cx = 0; cx < m_Chunks.x; cx++)
					FillOneChunk(cx, cy, cz, sdf, material);
	}

	void VoxelField3D::FillChunk(const glm::ivec3& chunk,
		const std::function<float(const glm::vec3&)>& sdf, unsigned char material)
	{
		if (m_Chunks.x <= 0 || !sdf)
			return;

		if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
			|| chunk.x >= m_Chunks.x || chunk.y >= m_Chunks.y || chunk.z >= m_Chunks.z)
			return;

		FillOneChunk(chunk.x, chunk.y, chunk.z, sdf, material);
	}

	bool VoxelField3D::Raycast(const glm::vec3& origin, const glm::vec3& direction,
		float maxDistance, float& outDistance, glm::vec3& outPoint,
		glm::vec3& outNormal) const
	{
		if (Empty())
			return false;

		// The field's box, so a ray aimed at the sky leaves immediately instead
		// of stepping its whole budget through empty space.
		glm::vec3 low = m_Origin;
		glm::vec3 high = m_Origin + glm::vec3(m_Size - glm::ivec3(1)) * m_VoxelSize;

		float near = 0.0f;
		float far = maxDistance;

		for (int axis = 0; axis < 3; axis++)
		{
			if (std::fabs(direction[axis]) < 1e-8f)
			{
				if (origin[axis] < low[axis] || origin[axis] > high[axis])
					return false;
				continue;
			}

			float inverse = 1.0f / direction[axis];
			float t1 = (low[axis] - origin[axis]) * inverse;
			float t2 = (high[axis] - origin[axis]) * inverse;

			if (t1 > t2)
				std::swap(t1, t2);

			near = glm::max(near, t1);
			far = glm::min(far, t2);

			if (near > far)
				return false;
		}

		// A hair inside, so a ray starting exactly on the boundary samples the
		// field rather than the clamped border.
		float travelled = near + m_VoxelSize * 0.01f;

		// A ray that starts inside the rock has already hit, at its origin.
		if (SampleDistance(origin + direction * travelled) < 0.0f)
		{
			outDistance = travelled;
			outPoint = origin + direction * travelled;
			outNormal = SampleNormal(outPoint);
			return true;
		}

		// The step floor stops a ray running almost parallel to a surface from
		// inching forward forever without ever touching it.
		const float minimumStep = m_VoxelSize * 0.05f;
		const float surface = m_VoxelSize * 0.01f;

		// **And the ceiling stops it leaving the map in one bound.** An
		// unallocated chunk reads `Far`, which is a sentinel meaning "nothing
		// near", not a distance -- so a ray entering open sky above the terrain
		// takes a single 1000 m step and reports a miss over a hillside it was
		// pointing straight at. A chunk is uniform across its whole extent, so
		// advancing by one chunk can never pass through anything; and where the
		// value *is* a real distance it is smaller than this anyway, so the cap
		// costs those rays nothing.
		//
		// Missed by every test until a demo built a sparse field: the analytic
		// spheres and planes are small enough that every chunk holds surface.
		const float maximumStep = (float)ChunkSize * m_VoxelSize;

		for (int step = 0; step < 512 && travelled <= far; step++)
		{
			glm::vec3 at = origin + direction * travelled;
			float distance = SampleDistance(at);

			if (distance < surface)
			{
				outDistance = travelled;
				outPoint = at;
				outNormal = SampleNormal(at);
				return true;
			}

			travelled += glm::clamp(distance, minimumStep, maximumStep);
		}

		return false;
	}

	void VoxelField3D::MarkDirty(int x, int y, int z)
	{
		if (!Contains(x, y, z))
			return;

		glm::ivec3 chunk = ChunkOf(x, y, z);

		// The chunk holding the point, and the one below it on any axis where
		// the point sits on the chunk's low edge. A cell spanning a boundary is
		// meshed by the chunk on the low side, so an edit on the seam changes a
		// cell that belongs to the previous chunk.
		for (int dz = -1; dz <= 0; dz++)
		{
			for (int dy = -1; dy <= 0; dy++)
			{
				for (int dx = -1; dx <= 0; dx++)
				{
					if ((dx < 0 && x % ChunkSize != 0)
						|| (dy < 0 && y % ChunkSize != 0)
						|| (dz < 0 && z % ChunkSize != 0))
						continue;

					glm::ivec3 neighbour = chunk + glm::ivec3(dx, dy, dz);

					if (neighbour.x < 0 || neighbour.y < 0 || neighbour.z < 0)
						continue;

					if (std::find(m_Dirty.begin(), m_Dirty.end(), neighbour) == m_Dirty.end())
						m_Dirty.push_back(neighbour);
				}
			}
		}
	}

	void VoxelField3D::MarkAllDirty()
	{
		m_Dirty.clear();

		for (int z = 0; z < m_Chunks.z; z++)
			for (int y = 0; y < m_Chunks.y; y++)
				for (int x = 0; x < m_Chunks.x; x++)
					m_Dirty.push_back({ x, y, z });
	}

	int VoxelField3D::EditSphere(const glm::vec3& centre, float radius, bool add)
	{
		if (m_Chunks.x <= 0 || radius <= 0.0f)
			return 0;

		// Only the lattice points the sphere can reach, plus a margin so the
		// band of near-surface values either side stays correct.
		const float margin = SparseBandVoxels * m_VoxelSize + m_VoxelSize;

		glm::vec3 low = (centre - glm::vec3(radius + margin) - m_Origin) / m_VoxelSize;
		glm::vec3 high = (centre + glm::vec3(radius + margin) - m_Origin) / m_VoxelSize;

		glm::ivec3 from = glm::max(glm::ivec3(glm::floor(low)), glm::ivec3(0));
		glm::ivec3 to = glm::min(glm::ivec3(glm::ceil(high)), m_Size - glm::ivec3(1));

		int changed = 0;

		for (int z = from.z; z <= to.z; z++)
		{
			for (int y = from.y; y <= to.y; y++)
			{
				for (int x = from.x; x <= to.x; x++)
				{
					float before = DistanceAt(x, y, z);
					float sphere = glm::length(PositionOf(x, y, z) - centre) - radius;

					// Union to add, subtraction to carve. Both are exact at the
					// surface, which is the only place the mesher and the
					// narrowphase read.
					float after = add
						? glm::min(before, sphere)
						: glm::max(before, -sphere);

					if (after == before)
						continue;

					// Material follows the sign: carved voxels become air, added
					// ones take the material of whatever is around them. Kept
					// simple on purpose -- a per-edit material belongs to
					// whatever is doing the editing, not to the field.
					unsigned char material = after < 0.0f
						? (MaterialAt(x, y, z) ? MaterialAt(x, y, z) : (unsigned char)1)
						: (unsigned char)0;

					if ((before < 0.0f) != (after < 0.0f))
						changed++;

					Set(x, y, z, after, material);
					MarkDirty(x, y, z);
				}
			}
		}

		return changed;
	}

	void VoxelField3D::SetUniform(const glm::ivec3& chunk, float distance,
		unsigned char material)
	{
		if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
			|| chunk.x >= m_Chunks.x || chunk.y >= m_Chunks.y || chunk.z >= m_Chunks.z)
			return;

		Chunk& c = m_Storage[ChunkIndexOf(chunk.x, chunk.y, chunk.z)];

		c.Uniform = distance;
		c.UniformMaterial = material;

		// Whatever it used to hold is gone, and the point is to hold nothing.
		c.Distance.clear();
		c.Distance.shrink_to_fit();
		c.Material.clear();
		c.Material.shrink_to_fit();
	}

	void VoxelField3D::ClearChunk(const glm::ivec3& chunk)
	{
		if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
			|| chunk.x >= m_Chunks.x || chunk.y >= m_Chunks.y || chunk.z >= m_Chunks.z)
			return;

		m_Storage.erase(ChunkIndexOf(chunk.x, chunk.y, chunk.z));
	}

	size_t VoxelField3D::AllocatedChunks() const
	{
		size_t count = 0;
		for (const auto& [key, chunk] : m_Storage)
			count += chunk.Distance.empty() ? 0 : 1;

		return count;
	}

	// Layout: a flag byte, then the uniform value and material, then the dense
	// arrays if there are any. A uniform chunk is six bytes; a dense one is
	// 4096 floats and 4096 bytes, which is the whole reason the flag is there.
	//
	// No endianness or float-format conversion. This is a cache next to the
	// executable that regenerates when it does not match, not an interchange
	// format -- paying for portability would be paying for a property nothing
	// asks of it.
	namespace {
		constexpr size_t s_VoxelsPerChunk =
			(size_t)VoxelField3D::ChunkSize * VoxelField3D::ChunkSize * VoxelField3D::ChunkSize;
		constexpr size_t s_HeaderBytes = 1 + sizeof(float) + 1;
		constexpr size_t s_DenseBytes =
			s_HeaderBytes + s_VoxelsPerChunk * sizeof(float) + s_VoxelsPerChunk;
	}

	void VoxelField3D::SaveChunk(const glm::ivec3& chunk, std::vector<unsigned char>& out) const
	{
		out.clear();

		if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
			|| chunk.x >= m_Chunks.x || chunk.y >= m_Chunks.y || chunk.z >= m_Chunks.z)
			return;

		const Chunk& c = ChunkAt(chunk.x, chunk.y, chunk.z);
		bool dense = !c.Distance.empty();

		out.resize(dense ? s_DenseBytes : s_HeaderBytes);

		size_t at = 0;
		out[at++] = dense ? 1u : 0u;
		std::memcpy(&out[at], &c.Uniform, sizeof(float));
		at += sizeof(float);
		out[at++] = c.UniformMaterial;

		if (!dense)
			return;

		std::memcpy(&out[at], c.Distance.data(), s_VoxelsPerChunk * sizeof(float));
		at += s_VoxelsPerChunk * sizeof(float);
		std::memcpy(&out[at], c.Material.data(), s_VoxelsPerChunk);
	}

	bool VoxelField3D::LoadChunk(const glm::ivec3& chunk, const unsigned char* data, size_t size)
	{
		if (!data || size < s_HeaderBytes)
			return false;

		if (chunk.x < 0 || chunk.y < 0 || chunk.z < 0
			|| chunk.x >= m_Chunks.x || chunk.y >= m_Chunks.y || chunk.z >= m_Chunks.z)
			return false;

		bool dense = data[0] != 0;
		if (size != (dense ? s_DenseBytes : s_HeaderBytes))
			return false;

		Chunk& c = m_Storage[ChunkIndexOf(chunk.x, chunk.y, chunk.z)];

		size_t at = 1;
		std::memcpy(&c.Uniform, &data[at], sizeof(float));
		at += sizeof(float);
		c.UniformMaterial = data[at++];

		if (!dense)
		{
			// Releasing the vectors rather than leaving them is what makes a
			// round trip through the cache produce the same *storage* and not
			// merely the same values -- AllocatedChunks would otherwise differ
			// between a generated field and a loaded one.
			c.Distance.clear();
			c.Distance.shrink_to_fit();
			c.Material.clear();
			c.Material.shrink_to_fit();
			return true;
		}

		c.Distance.resize(s_VoxelsPerChunk);
		c.Material.resize(s_VoxelsPerChunk);

		std::memcpy(c.Distance.data(), &data[at], s_VoxelsPerChunk * sizeof(float));
		at += s_VoxelsPerChunk * sizeof(float);
		std::memcpy(c.Material.data(), &data[at], s_VoxelsPerChunk);

		return true;
	}

	size_t VoxelField3D::AllocatedBytes() const
	{
		size_t bytes = m_Storage.size() * sizeof(Chunk);

		for (const auto& [key, chunk] : m_Storage)
		{
			bytes += chunk.Distance.size() * sizeof(float);
			bytes += chunk.Material.size() * sizeof(unsigned char);
		}

		return bytes;
	}

}
