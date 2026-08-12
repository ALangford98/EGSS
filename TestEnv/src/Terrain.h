#pragma once

#include <Egss.h>

// Seeded terrain: a heightmap, the mesh for it, and the queries a character
// needs to walk on it.
//
// **The same seed always produces the same map.** That is the whole point of
// generating it this way rather than with `rand()`: the noise is a pure
// function of the lattice coordinate and the seed, so nothing depends on how
// many samples were taken before, what order they came in, or what else the
// program did first. A map can be described by one number and rebuilt exactly,
// which is what makes a generated world something you can go back to.
//
// The noise is value noise summed over octaves -- fractional Brownian motion.
// Each octave is the same lattice at twice the frequency and half the
// amplitude, so the first octave gives the hills and the later ones the
// roughness on top of them.
class Terrain
{
public:
	// --- Shape of the map -------------------------------------------------
	//
	// Resolution is samples per side, so the mesh has (Resolution-1)^2 cells
	// and twice that many triangles. 129 is 32768 triangles, one draw call,
	// and about a metre between samples across a 64 m map.
	int Resolution = 129;
	float Extent = 64.0f;       // metres across, centred on the origin
	float Amplitude = 9.0f;     // lowest point to highest

	// How many times the lattice repeats across the map at the first octave.
	// Larger is busier terrain rather than bigger terrain.
	float Frequency = 2.5f;
	int Octaves = 5;
	float Lacunarity = 2.0f;    // frequency multiplier per octave
	float Gain = 0.5f;          // amplitude multiplier per octave

	uint32_t Seed = 1337;

	void Generate()
	{
		m_Heights.assign((size_t)Resolution * Resolution, 0.0f);

		// The amplitudes are normalised by their own sum, so the result lands
		// in [0,1] whatever the octave count is -- otherwise adding an octave
		// would quietly raise the whole map.
		float total = 0.0f;
		float amplitude = 1.0f;
		for (int o = 0; o < Octaves; o++)
		{
			total += amplitude;
			amplitude *= Gain;
		}
		if (total <= 0.0f)
			total = 1.0f;

		for (int z = 0; z < Resolution; z++)
		{
			for (int x = 0; x < Resolution; x++)
			{
				// Position across the map in [0,1], then in lattice units.
				float u = (float)x / (float)(Resolution - 1);
				float v = (float)z / (float)(Resolution - 1);

				float sum = 0.0f;
				float frequency = Frequency;
				float weight = 1.0f;

				for (int o = 0; o < Octaves; o++)
				{
					// Each octave gets its own slice of the hash space, so
					// they are independent fields rather than the same field
					// sampled twice.
					sum += weight * ValueNoise(u * frequency, v * frequency,
						Seed + (uint32_t)o * 0x9E3779B9u);

					frequency *= Lacunarity;
					weight *= Gain;
				}

				m_Heights[(size_t)z * Resolution + x] = (sum / total) * Amplitude;
			}
		}

		// The collider is built from the same samples, and every height query
		// below goes through it. That is deliberate: the alternative is this
		// file answering "how high is the ground" one way and the physics
		// answering it another, and the two disagreeing by a centimetre is
		// exactly the bug that shows up as feet hovering with nothing in a
		// screenshot to explain it.
		m_Field = std::make_shared<Egss::Heightfield3D>();
		m_Field->SetHeights(Resolution, Extent, m_Heights);
	}

	// --- Queries ----------------------------------------------------------

	// Height of the surface under a world position, from the collider -- so it
	// is the triangulated surface the mesh draws, not a bilinear patch through
	// the same corners.
	//
	// Those are genuinely different surfaces. They meet at the four corners of
	// a cell and along its diagonal, and between them they differ by
	// `fx*fz*(h00 - h10 - h01 + h11)`. This started out bilinear on the
	// assumption that it *was* the mesh's surface; measured on this terrain the
	// gap reached 1.9 cm, which is a foot-thickness of hover.
	float HeightAt(float x, float z) const
	{
		if (!m_Field)
			return 0.0f;

		float height;
		glm::vec3 normal;
		if (!m_Field->SurfaceAt(x, z, height, normal))
			return 0.0f;

		return height;
	}

	// The surface normal for *shading*, from the slope of the height field.
	//
	// Central differences over one cell rather than one triangle's face
	// normal, and the distinction matters now that the collider has face
	// normals of its own. A face normal is the truth about the geometry and is
	// what a contact wants; it is also constant across a triangle, so using it
	// per vertex would light the map as 32768 flat facets. These two normals
	// describe the same surface and answer different questions.
	// Delegated rather than differenced here, and that fixed a real edge case.
	// This used to take its own central difference through `HeightAt`, which
	// answers 0 for a sample off the map -- so on the outermost ring one of the
	// two samples was sea level and the whole border came out as a cliff. It is
	// invisible in a screenshot, because a rim lit as a steep face is exactly
	// what the edge of a floating slab of terrain looks like anyway; it showed
	// up when a census of slopes called this 9 m map 81.3 degrees steep, and the
	// real answer with the ring excluded was 35.0. `SmoothNormalAt` clamps its
	// samples inside the field, which is the fix, and having one implementation
	// is the rest of it.
	glm::vec3 NormalAt(float x, float z) const
	{
		if (!m_Field)
			return { 0.0f, 1.0f, 0.0f };

		return m_Field->SmoothNormalAt(x, z);
	}

	bool Contains(float x, float z) const
	{
		float half = Extent * 0.5f;
		return x >= -half && x <= half && z >= -half && z <= half;
	}

	float Lowest() const
	{
		float lowest = 0.0f;
		for (size_t i = 0; i < m_Heights.size(); i++)
			lowest = i == 0 ? m_Heights[i] : std::min(lowest, m_Heights[i]);
		return lowest;
	}

	float Highest() const
	{
		float highest = 0.0f;
		for (size_t i = 0; i < m_Heights.size(); i++)
			highest = i == 0 ? m_Heights[i] : std::max(highest, m_Heights[i]);
		return highest;
	}

	// A number that changes if any height changes. What "the same seed gives
	// the same map" is checked with.
	uint32_t Checksum() const
	{
		uint32_t hash = 2166136261u;   // FNV-1a
		for (float height : m_Heights)
		{
			uint32_t bits;
			std::memcpy(&bits, &height, sizeof(bits));
			for (int byte = 0; byte < 4; byte++)
			{
				hash ^= (bits >> (byte * 8)) & 0xFFu;
				hash *= 16777619u;
			}
		}
		return hash;
	}

	// --- Geometry ---------------------------------------------------------

	Egss::MeshData BuildMesh() const
	{
		Egss::MeshData data;
		if (m_Heights.empty())
			return data;

		float half = Extent * 0.5f;
		float step = Extent / (float)(Resolution - 1);

		data.Vertices.reserve((size_t)Resolution * Resolution);
		for (int z = 0; z < Resolution; z++)
		{
			for (int x = 0; x < Resolution; x++)
			{
				float wx = -half + (float)x * step;
				float wz = -half + (float)z * step;

				Egss::MeshVertex vertex;
				vertex.Position = { wx, At(x, z), wz };
				vertex.Normal = NormalAt(wx, wz);

				// Tiled rather than stretched, so the texture density does not
				// depend on how big the map is.
				vertex.TexCoord = { (float)x * 0.25f, (float)z * 0.25f };

				data.Vertices.push_back(vertex);
			}
		}

		data.Indices.reserve((size_t)(Resolution - 1) * (Resolution - 1) * 6);
		for (int z = 0; z + 1 < Resolution; z++)
		{
			for (int x = 0; x + 1 < Resolution; x++)
			{
				unsigned int i00 = (unsigned int)(z * Resolution + x);
				unsigned int i10 = i00 + 1;
				unsigned int i01 = i00 + (unsigned int)Resolution;
				unsigned int i11 = i01 + 1;

				// Wound counter-clockwise seen from above, which is what the
				// renderer treats as front-facing. Getting this backwards on
				// a heightfield is invisible until something is lit.
				data.Indices.push_back(i00);
				data.Indices.push_back(i01);
				data.Indices.push_back(i10);

				data.Indices.push_back(i10);
				data.Indices.push_back(i01);
				data.Indices.push_back(i11);
			}
		}

		data.Submeshes.push_back({ "", 0, (unsigned int)data.Indices.size() });
		data.RecalculateBounds();
		return data;
	}

	const std::vector<float>& Heights() const { return m_Heights; }

	// The collider, for `RigidBody3D::MakeHeightfield`. Shared, so regenerating
	// while a body holds the old one is safe -- though the body wants
	// rebuilding too, or it will be standing on the previous map.
	const std::shared_ptr<Egss::Heightfield3D>& Field() const { return m_Field; }

	// Grid sample, for tests that want the stored value rather than the
	// interpolated one.
	float At(int x, int z) const
	{
		x = glm::clamp(x, 0, Resolution - 1);
		z = glm::clamp(z, 0, Resolution - 1);
		return m_Heights[(size_t)z * Resolution + x];
	}

private:
	// --- Noise ------------------------------------------------------------

	// An integer hash, not a random number generator.
	//
	// This is what makes the map replicable. A generator carries state, so the
	// value at a point would depend on how many values were drawn before it --
	// change the loop order, or generate one extra octave, and every height
	// downstream moves. A hash of the coordinate has no such memory: the value
	// at a lattice point is the same however you arrive at it.
	static uint32_t Hash(int x, int y, uint32_t seed)
	{
		uint32_t h = seed;
		h += (uint32_t)x * 0xC2B2AE35u;
		h += (uint32_t)y * 0x27D4EB2Fu;
		h ^= h >> 15;
		h *= 0x2545F491u;
		h ^= h >> 13;
		h *= 0x85EBCA6Bu;
		h ^= h >> 16;
		return h;
	}

	static float HashUnit(int x, int y, uint32_t seed)
	{
		// Divided by 2^32 rather than by 2^32-1, so the range is [0,1) and the
		// top of it is never quite reached -- which keeps the sum of octaves
		// off its own ceiling.
		return (float)Hash(x, y, seed) * (1.0f / 4294967296.0f);
	}

	// Value noise: the lattice carries a value per corner, and the cell is
	// interpolated between them. Smoothstep rather than linear, so the field
	// has no creases along the lattice lines -- linear interpolation is
	// continuous but its slope is not, and that shows up as a grid of ridges.
	static float ValueNoise(float x, float y, uint32_t seed)
	{
		int xi = (int)std::floor(x);
		int yi = (int)std::floor(y);

		float fx = x - (float)xi;
		float fy = y - (float)yi;

		float sx = fx * fx * (3.0f - 2.0f * fx);
		float sy = fy * fy * (3.0f - 2.0f * fy);

		float n00 = HashUnit(xi, yi, seed);
		float n10 = HashUnit(xi + 1, yi, seed);
		float n01 = HashUnit(xi, yi + 1, seed);
		float n11 = HashUnit(xi + 1, yi + 1, seed);

		return glm::mix(glm::mix(n00, n10, sx), glm::mix(n01, n11, sx), sy);
	}

	std::vector<float> m_Heights;
	std::shared_ptr<Egss::Heightfield3D> m_Field;
};
