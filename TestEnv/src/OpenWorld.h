#pragma once

// A large play area: islands separated by water, streamed in around the
// player instead of baked upfront at load, and frustum-culled at
// submission. Built to answer several questions the roadmap needed a real
// scene for: does a big VoxelField3D world load in performantly with
// per-chunk generation, does culling actually reduce draw calls, and is a
// first-person controller built once and shared "up to scratch". A
// textured-vs-untextured compute comparison is deliberately not in this
// piece -- it needs this demo to exist first as the thing to measure.
//
// **Distant-chunk LOD is in**, on top of `MarchingCubes::Mesh`'s stride:
// chunks past 24 m mesh on a stride-2 lattice and past 48 m on stride-4,
// with an 8 m hysteresis band so a chunk sitting on a boundary does not
// remesh every step. It buys little at the default 64 m load radius, where
// almost everything is near -- the point of it is that a *bigger* radius
// becomes affordable: at 128 m it is 745,644 triangles down to 81,413, and
// seeing 128 m with LOD costs 0.82x what seeing 64 m without it did. The load
// radius now defaults to 128 m on the strength of that.
//
// Chunks are filled **nearest first** rather than in scan-line order, which
// only started to matter at the larger radius -- a 128 m disc is 10,455 chunks
// and takes minutes to populate at one a step, so the order in which it
// assembles is something you watch happen.
//
// **The field's fixed extent is not the world's extent.** VoxelField3D
// declares its whole lattice at Create() time (400 x 100 x 400 m here), but
// nothing is generated or meshed until a chunk falls within the load
// radius of the player -- see StreamChunks. Turning that into a genuinely
// unbounded world would mean reworking the field's storage from a flat
// indexed array to one keyed by coordinate, which would ripple into
// VoxelIslands/VoxelStress; a large fixed bound gets the practical result
// (nothing reachable in a session) without that risk.

#include <Egss.h>
#include <imgui.h>

#include "Demo.h"
#include "FirstPersonController.h"
#include "ChunkCache.h"

#include <unordered_set>
#include <climits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

class OpenWorld : public DemoLayer
{
public:
	OpenWorld()
		: DemoLayer("OpenWorld"), m_Camera(70.0f, 16.0f / 9.0f, 0.1f, 500.0f),
		  m_Controller(m_Camera, -90.0f, -2.0f)
	{
		RegisterParam("Walk speed", &m_Controller.Cfg.WalkSpeed);
		RegisterParam("Sensitivity", &m_Controller.Cfg.MouseSensitivity);
		RegisterParam("Load radius", &m_LoadRadius);
		RegisterParam("Culling", &m_Culling);
		RegisterParam("First person", &m_FirstPerson);
		RegisterParam("Chunks per step", &m_ChunksPerStep);
		RegisterParam("Textured", &m_Textured);
	}

	void OnDemoAttach() override
	{
		BuildShader();
		BuildIslands();

		m_Field = std::make_shared<Egss::VoxelField3D>();
		m_Field->Create({ s_SideX, s_SideY, s_SideZ }, s_Voxel,
			{ -0.5f * (s_SideX - 1) * s_Voxel, s_OriginY, -0.5f * (s_SideZ - 1) * s_Voxel });

		Egss::RigidBody3D ground = Egss::RigidBody3D::MakeSdf({ 0.0f, 0.0f, 0.0f }, m_Field);
		ground.Friction = 0.8f;
		ground.Restitution = 0.0f;
		m_World.Gravity = { 0.0f, -9.81f, 0.0f };
		m_World.AddBody(ground);

		BuildWater();

		// The spawn island's chunks have to exist before GroundBelow has
		// anything to answer -- unlike the steady-state stream, this one
		// call is synchronous and unbudgeted, the same one-time cost
		// VoxelTerrain's whole-map Fill pays at attach, just over a much
		// smaller area.
		// Before any streaming, so the very first chunks can come from it.
		m_Cache.Open("openworld.chunks", FingerprintDensity());
		if (m_Cache.Rebuilt())
			EGSS_INFO("Chunk cache: starting fresh (absent, or a different world)");
		else
			EGSS_INFO("Chunk cache: {0} chunks already stored", m_Cache.Entries());

		m_Controller.Cfg.HasWater = true;
		m_Controller.Cfg.WaterLevel = s_SeaLevel;

		// Just above the island rather than 40 m over it: the islands are now
		// a few metres tall, so the old spawn height was most of a minute of
		// falling before the demo started.
		glm::vec2 centre = m_Islands[0].Centre;
		glm::vec3 spawn(centre.x, Height(centre.x, centre.y) + 4.0f, centre.y);

		StreamAround(spawn, s_ChunkWorld * 3.0f, 100000);

		SpawnWalker(spawn);
		SpawnRocks(m_Islands[0]);
	}

	void OnDemoDeactivated() override
	{
		m_Controller.SetMouseLook(false);
	}

	// --- Update ---------------------------------------------------------

	void OnDemoFixedUpdate(Egss::Timestep step) override
	{
		float dt = step;

		m_Controller.UpdateLook(dt);

		if (m_FirstPerson)
			m_Grounded = m_Controller.UpdateWalk(m_World, m_Walker, s_EyeHeight, dt);
		else
			m_Controller.UpdateFly(dt);

		StreamChunks();

		m_World.Step(step);
	}

	// --- Streaming --------------------------------------------------------

	// Where streaming is centred: the walker's feet in first person (so the
	// world ahead of a turn loads before the camera swings onto it as much
	// as behind), the camera itself flying free.
	glm::vec3 StreamFocus() const
	{
		return m_FirstPerson ? m_World.GetBody(m_Walker).Position : m_Camera.GetPosition();
	}

	void StreamChunks()
	{
		StreamAround(StreamFocus(), m_LoadRadius, m_ChunksPerStep);
		UpdateLod(StreamFocus(), m_LodPerStep);
		EvictDistantMeshes(StreamFocus());
	}

	// Fills and meshes whatever chunks within `radius` of `focus` are not
	// already resident, up to `budget` chunks this call -- the budget is
	// what keeps a burst of newly-entered terrain from spiking a frame.
	// Vertical range is not distance-limited: the field is only 13 chunks
	// tall, so filling a whole column once its horizontal distance qualifies
	// costs little and keeps the query simple.
	void StreamAround(const glm::vec3& focus, float radius, int budget)
	{
		glm::ivec3 chunkCount = m_Field->ChunkCount();
		glm::vec3 local = focus - m_Field->Origin();
		glm::ivec2 centre = glm::ivec2(glm::floor(glm::vec2(local.x, local.z) / s_ChunkWorld));

		int reach = (int)std::ceil(radius / s_ChunkWorld) + 1;

		auto sdf = [this](const glm::vec3& p) { return Density(p); };

		// **Nearest first.** This used to walk dz then dx, which fills a disc
		// in scan-line order: the far edge of the first row arrives before the
		// ground the player is standing next to. At a 64 m radius that is hard
		// to notice; at 128 m the disc is four times the area and the world
		// visibly assembles in stripes.
		//
		// The offsets are pre-sorted by distance once per reach rather than
		// sorted per step, which keeps the early-out on `budget` -- the loop
		// still stops at the first few unfilled chunks it finds, it just finds
		// the *closest* ones first. Same cost, useful order.
		const std::vector<glm::ivec2>& ring = RingOffsets(reach);

		// Resume where the last call stopped. Everything before the cursor was
		// either filled or permanently skipped, and neither changes while the
		// centre stays put -- without this the scan walks the whole filled
		// interior every step looking for the first gap, which measured 3.2 ms
		// to 6.2 ms in Debug as the interior grew. The cursor resets when the
		// player crosses into a new chunk, which costs one full scan and then
		// amortises away again.
		if (centre != m_RingCentre)
		{
			m_RingCentre = centre;
			m_RingCursor = 0;
		}

		size_t i = m_RingCursor;

		while (i < ring.size() && budget > 0)
		{
			const glm::ivec2& offset = ring[i];
			{
				int cx = centre.x + offset.x;
				int cz = centre.y + offset.y;
				if (cx < 0 || cz < 0 || cx >= chunkCount.x || cz >= chunkCount.z)
				{
					i++;
					continue;
				}

				glm::vec2 chunkCentreXZ = glm::vec2(m_Field->Origin().x, m_Field->Origin().z)
					+ (glm::vec2(cx, cz) + 0.5f) * s_ChunkWorld;
				if (glm::length(chunkCentreXZ - glm::vec2(focus.x, focus.z)) > radius)
				{
					i++;
					continue;
				}

				// The cursor may only pass a column once every chunk in it is
				// filled; running out of budget half way leaves it here.
				bool columnComplete = true;

				for (int cy = 0; cy < chunkCount.y; cy++)
				{
					glm::ivec3 chunk(cx, cy, cz);
					size_t key = ChunkKey(chunk);

					if (m_Filled.count(key))
						continue;

					if (budget <= 0)
					{
						columnComplete = false;
						break;
					}

					// Cache first. A hit is a seek and a memcpy; a miss is a
					// density evaluation for every one of the chunk's 4,096
					// voxels, and then the bytes go back for next time.
					bool loaded = false;
					if (m_UseCache && m_Cache.Read(chunk, m_ChunkBytes))
						loaded = m_Field->LoadChunk(chunk, m_ChunkBytes.data(), m_ChunkBytes.size());

					if (!loaded)
					{
						m_Field->FillChunk(chunk, sdf, 1);

						if (m_UseCache)
						{
							m_Field->SaveChunk(chunk, m_ChunkBytes);
							m_Cache.Write(chunk, m_ChunkBytes);
						}
					}

					m_Filled.insert(key);

					// The chunk itself, plus its low-x/y/z neighbours if
					// already meshed -- see the note on FillChunk in
					// VoxelField3D.h. Marking dirty at the chunk's own
					// (0,0,0) corner would do this in one call, but it also
					// marks all four *diagonal* combinations (low-x-and-y,
					// low-x-and-z, low-y-and-z, low-x-y-z) that MarkDirty's
					// point-based rule cannot help firing when every
					// coordinate sits on a boundary at once -- correct, since
					// those chunks are not actually stale, but it was
					// measured remeshing 16 chunks for 3 newly-filled ones.
					// Three calls, each with exactly one coordinate on the
					// boundary, hit only that one axis's low neighbour (plus
					// this chunk, redundantly-but-harmlessly, each time).
					int lx = cx * Egss::VoxelField3D::ChunkSize;
					int ly = cy * Egss::VoxelField3D::ChunkSize;
					int lz = cz * Egss::VoxelField3D::ChunkSize;
					const int mid = Egss::VoxelField3D::ChunkSize / 2;

					m_Field->MarkDirtyAt(lx, ly + mid, lz + mid);
					m_Field->MarkDirtyAt(lx + mid, ly, lz + mid);
					m_Field->MarkDirtyAt(lx + mid, ly + mid, lz);

					budget--;
				}

				if (!columnComplete)
					break;

				i++;
			}
		}

		m_RingCursor = i;

		RebuildDirtyMeshes(focus);
	}

	// Chunk-column offsets within `reach`, sorted nearest first. Rebuilt only
	// when the reach changes, which happens when the load-radius slider moves.
	const std::vector<glm::ivec2>& RingOffsets(int reach)
	{
		if (reach == m_RingReach)
			return m_Ring;

		m_Ring.clear();
		m_Ring.reserve((size_t)(2 * reach + 1) * (2 * reach + 1));

		for (int dz = -reach; dz <= reach; dz++)
			for (int dx = -reach; dx <= reach; dx++)
				m_Ring.push_back({ dx, dz });

		std::sort(m_Ring.begin(), m_Ring.end(),
			[](const glm::ivec2& a, const glm::ivec2& b)
			{
				int da = a.x * a.x + a.y * a.y;
				int db = b.x * b.x + b.y * b.y;
				// Ties broken on the coordinates so the order is total, and so
				// two runs fill the same chunks in the same sequence -- the
				// demo has to replay identically.
				if (da != db) return da < db;
				if (a.x != b.x) return a.x < b.x;
				return a.y < b.y;
			});

		// A different reach is a different list, so the cursor into the old one
		// means nothing.
		m_RingReach = reach;
		m_RingCursor = 0;
		return m_Ring;
	}

	void RebuildDirtyMeshes(const glm::vec3& focus)
	{
		const std::vector<glm::ivec3>& dirty = m_Field->DirtyChunks();

		for (const glm::ivec3& chunk : dirty)
		{
			// An edit does not change which band a chunk is in, so a resident
			// chunk is remeshed at the stride it already had. A chunk being
			// meshed for the first time has no previous stride and takes the
			// band outright, with no hysteresis to apply.
			auto it = m_Chunks.find(ChunkKey(chunk));
			int stride = (it != m_Chunks.end())
				? it->second.Stride
				: BandFor(FocusDistance(ChunkCentre(chunk), focus));

			MeshChunk(chunk, stride);
		}

		m_Field->ClearDirtyChunks();
	}

	// Meshes one chunk on a lattice `stride` voxels wide and stores the result.
	void MeshChunk(const glm::ivec3& chunk, int stride)
	{
		glm::ivec3 min, max;
		m_Field->ChunkRange(chunk, min, max);

		// Skirt depth scales with the stride, because the disagreement it is
		// covering does: a coarse cell spans `stride` voxels, and the surface
		// can differ by roughly the terrain's slope across that span. The
		// seabed falls at 0.55 m per metre, so 1.5 voxels of stride is
		// comfortably more than the worst case.
		float skirt = m_Skirts ? (float)stride * s_Voxel * 1.5f : 0.0f;

		Egss::MeshData data = Egss::MarchingCubes::Mesh(*m_Field, min, max, stride, skirt);
		size_t key = ChunkKey(chunk);

		if (data.Indices.empty())
		{
			m_Chunks.erase(key);
			return;
		}

		ChunkEntry entry;
		entry.MeshPtr = std::make_shared<Egss::Mesh>(data, "OpenWorldChunk");
		entry.Stride = stride;
		entry.Coord = chunk;

		if (m_Grass && stride == 1)
		{
			Egss::MeshData blades = BuildGrass(data, chunk);
			if (!blades.Indices.empty())
				entry.GrassPtr = std::make_shared<Egss::Mesh>(blades, "OpenWorldGrass");
		}

		// The bounds describe the chunk's extent in the field, not its
		// triangles, so they are the same at every stride -- which is what
		// keeps frustum culling unaffected by an LOD change.
		glm::vec3 chunkOrigin = m_Field->Origin() + glm::vec3(min) * s_Voxel;
		glm::vec3 chunkExtent = glm::vec3(max - min) * s_Voxel;
		entry.Bounds = { chunkOrigin, chunkOrigin + chunkExtent };

		m_Chunks[key] = entry;
	}

	// Blades of grass, as geometry, built from the chunk's own triangles.
	//
	// One triangle a blade: a base edge across the slope and a point above it.
	// A quad would be two triangles for a shape nobody can distinguish at the
	// size these are drawn, and grass is the one thing here where the count is
	// the cost.
	//
	// Placed on the terrain surface rather than on a grid, so blades follow
	// the ground exactly and inherit the mesh's own density -- more triangles
	// where the surface is busier is also where more grass looks right.
	//
	// Only stride-1 chunks get grass. That is not a special case bolted on: a
	// stride-2 chunk is already the renderer saying this is far enough away to
	// halve its detail, and grass is the first thing that should go.
	Egss::MeshData BuildGrass(const Egss::MeshData& terrain, const glm::ivec3& chunk) const
	{
		Egss::MeshData grass;

		if (m_GrassDensity <= 0.0f)
			return grass;

		size_t triangles = terrain.Indices.size() / 3;
		unsigned int seed = 977u + (unsigned int)(chunk.x * 73 + chunk.y * 19 + chunk.z * 131);

		for (size_t t = 0; t < triangles; t++)
		{
			const glm::vec3& a = terrain.Vertices[terrain.Indices[t * 3 + 0]].Position;
			const glm::vec3& b = terrain.Vertices[terrain.Indices[t * 3 + 1]].Position;
			const glm::vec3& c = terrain.Vertices[terrain.Indices[t * 3 + 2]].Position;

			glm::vec3 centre = (a + b + c) / 3.0f;

			glm::vec3 face = glm::cross(b - a, c - a);
			float area2 = glm::length(face);
			if (area2 < 1e-8f)
				continue;

			glm::vec3 n = face / area2;

			// The same test the shader shades with, so a blade never appears on
			// bare sand or on a face too steep to be green.
			float high = glm::smoothstep(m_GrassLow, m_GrassHigh, centre.y);
			float flatness = glm::smoothstep(0.55f, 0.88f, n.y);
			float chance = high * flatness * m_GrassDensity;

			if (chance <= 0.001f)
				continue;

			// Fractional density done honestly: the whole part is a guaranteed
			// count and the remainder is a threshold, so 0.3 gives roughly
			// three blades every ten triangles rather than none.
			int count = (int)chance;
			if (Hash2DUnit((int)t, 0, seed) < chance - (float)count)
				count++;

			for (int i = 0; i < count; i++)
			{
				// Uniform inside the triangle: the sqrt is what stops the
				// points bunching along one edge.
				float u = Hash2DUnit((int)t, i * 3 + 1, seed);
				float v = Hash2DUnit((int)t, i * 3 + 2, seed);
				float su = std::sqrt(u);

				glm::vec3 base = a + (b - a) * (su * (1.0f - v)) + (c - a) * (su * v);

				float angle = Hash2DUnit((int)t, i * 3 + 3, seed) * 6.2831853f;
				float height = m_GrassHeight * (0.65f + Hash2DUnit((int)t, i * 3 + 4, seed) * 0.7f);

				glm::vec3 side(std::cos(angle) * m_GrassWidth, 0.0f, std::sin(angle) * m_GrassWidth);

				// Leaning, and leaning the same way per blade: upright blades
				// read as spikes, and a whole field of them looks like a bed of
				// nails rather than grass.
				glm::vec3 lean = glm::vec3(std::cos(angle + 1.57f), 0.0f, std::sin(angle + 1.57f))
					* (height * 0.35f);

				glm::vec3 tip = base + n * height + lean;

				// Facing the lean, so a blade catches the light on its face
				// rather than edge-on.
				glm::vec3 bladeNormal = glm::normalize(glm::cross(side * 2.0f, tip - (base - side)));
				if (glm::dot(bladeNormal, glm::vec3(0.0f, 1.0f, 0.0f)) < 0.0f)
					bladeNormal = -bladeNormal;

				unsigned int at = (unsigned int)grass.Vertices.size();
				grass.Vertices.push_back({ base - side, bladeNormal, { 0.0f, 0.0f } });
				grass.Vertices.push_back({ base + side, bladeNormal, { 1.0f, 0.0f } });
				grass.Vertices.push_back({ tip,         bladeNormal, { 0.5f, 1.0f } });

				grass.Indices.push_back(at);
				grass.Indices.push_back(at + 1);
				grass.Indices.push_back(at + 2);
			}
		}

		if (grass.Indices.empty())
			return grass;

		Egss::Submesh all;
		all.IndexCount = (unsigned int)grass.Indices.size();
		grass.Submeshes.push_back(all);
		grass.RecalculateBounds();

		return grass;
	}

	// --- Level of detail --------------------------------------------------
	//
	// Distance is measured in the horizontal plane only, the same as streaming
	// and eviction: the field is 13 chunks tall and a column directly overhead
	// is not meaningfully further away than the one underfoot.
	static float FocusDistance(const glm::vec3& chunkCentre, const glm::vec3& focus)
	{
		return glm::length(glm::vec2(chunkCentre.x, chunkCentre.z)
			- glm::vec2(focus.x, focus.z));
	}

	glm::vec3 ChunkCentre(const glm::ivec3& chunk) const
	{
		glm::ivec3 min, max;
		m_Field->ChunkRange(chunk, min, max);

		glm::vec3 origin = m_Field->Origin() + glm::vec3(min) * s_Voxel;
		return origin + glm::vec3(max - min) * s_Voxel * 0.5f;
	}

	// Which stride a chunk at this distance belongs on, ignoring where it is
	// now. Powers of two only: the mesher's stride divides the 16-voxel chunk
	// exactly at 1, 2 and 4, and a stride that does not divide it evenly
	// clips a partial cell and widens the seam it already has.
	int BandFor(float distance) const
	{
		if (!m_Lod)
			return 1;
		if (distance > m_LodFar)
			return 4;
		if (distance > m_LodNear)
			return 2;
		return 1;
	}

	// The band, with a margin that must be crossed before a chunk actually
	// changes. Without it a chunk parked on a boundary remeshes every step
	// the player breathes across it -- and remeshing is the expensive thing
	// LOD exists to avoid, so an LOD that thrashes costs more than none.
	int DesiredStride(float distance, int current) const
	{
		if (!m_Lod)
			return 1;

		int band = BandFor(distance);
		if (band == current)
			return current;

		// Coarsening is judged against the edge the chunk is leaving;
		// refining against the edge it is coming back inside.
		float edge = (band > current)
			? ((current == 1) ? m_LodNear : m_LodFar)
			: ((band == 1) ? m_LodNear : m_LodFar);

		if (band > current && distance < edge + m_LodHysteresis)
			return current;
		if (band < current && distance > edge - m_LodHysteresis)
			return current;

		return band;
	}

	// Remeshes up to `budget` chunks whose band no longer matches their mesh.
	// Budgeted for the same reason streaming is: the cost is marching cubes on
	// the CPU, and doing every stale chunk in one step is exactly the spike
	// the budget exists to prevent.
	void UpdateLod(const glm::vec3& focus, int budget)
	{
		m_LodRemeshes = 0;

		// Collected first rather than remeshed in place: MeshChunk can erase
		// its entry when a coarser lattice finds no surface at all, and that
		// invalidates the iterator standing on it.
		std::vector<std::pair<glm::ivec3, int>> work;

		for (const auto& [key, entry] : m_Chunks)
		{
			if ((int)work.size() >= budget)
				break;

			int want = DesiredStride(FocusDistance(entry.Bounds.Centre(), focus), entry.Stride);
			if (want != entry.Stride)
				work.push_back({ entry.Coord, want });
		}

		for (const auto& [coord, stride] : work)
			MeshChunk(coord, stride);

		m_LodRemeshes = (int)work.size();
	}

	// Drops the GPU mesh for anything well outside the load radius. Field
	// data is left alone -- re-filling later would cost the same as filling
	// fresh, so there is nothing to save by discarding it, and keeping it
	// means a chunk re-entering range meshes from data that is already
	// there.
	void EvictDistantMeshes(const glm::vec3& focus)
	{
		float evictRadius = m_LoadRadius * 1.6f;

		for (auto it = m_Chunks.begin(); it != m_Chunks.end(); )
		{
			glm::vec3 centre = it->second.Bounds.Centre();
			float d = glm::length(glm::vec2(centre.x, centre.z) - glm::vec2(focus.x, focus.z));

			if (d > evictRadius)
				it = m_Chunks.erase(it);
			else
				++it;
		}
	}

	static size_t ChunkKey(const glm::ivec3& chunk)
	{
		return ((size_t)chunk.z * 1024 + chunk.y) * 1024 + chunk.x;
	}

	// --- Terrain shape: islands, and water in between --------------------

	struct Island
	{
		glm::vec2 Centre;
		float Radius;
	};

	void BuildIslands()
	{
		m_Islands.clear();

		for (int i = 0; i < s_IslandCount; i++)
		{
			float angle = ((float)i / (float)s_IslandCount) * 6.2831853f
				+ Hash2DUnit(i, 0, 7u) * 1.5f;
			float distance = 55.0f + Hash2DUnit(i, 1, 7u) * 85.0f;
			float radius = s_IslandRadiusMin
				+ Hash2DUnit(i, 2, 7u) * (s_IslandRadiusMax - s_IslandRadiusMin);

			m_Islands.push_back({
				{ std::cos(angle) * distance, std::sin(angle) * distance }, radius });
		}
	}

	// The largest of every island's own radial falloff: +Radius at an
	// island's centre, crossing zero at its edge, and increasingly negative
	// (deep water) wherever no island reaches. Also returns which island
	// won, for Slope -- computing the mask and its gradient in separate
	// passes over the same island list is one of the two redundant costs
	// Slope used to pay; see the note there.
	float IslandMask(float x, float z, const Island** outWinner = nullptr) const
	{
		float best = -80.0f;
		const Island* winner = nullptr;

		for (const Island& island : m_Islands)
		{
			float d = glm::length(glm::vec2(x, z) - island.Centre);
			float local = island.Radius - d;

			if (local > best)
			{
				best = local;
				winner = &island;
			}
		}

		if (outWinner)
			*outWinner = winner;

		return best;
	}

	float Height(float x, float z) const
	{
		float mask = IslandMask(x, z);

		// Relief only matters near or above the coast -- hill noise applied
		// to the seafloor too would look like the islands sit over jagged
		// undersea mountains rather than in open water.
		float coastFade = glm::clamp(mask / 15.0f + 0.5f, 0.0f, 1.0f);

		float relief = s_ReliefBroad * Noise2D(x * 0.02f, z * 0.02f, 401u)
			+ s_ReliefFine * Noise2D(x * 0.05f, z * 0.05f, 402u);

		// s_MaskToHeight: the mask is metres of falloff per metre of distance
		// from an island's edge, which reads as a cliff at 1:1. Scaled right
		// down, a coastline slopes like a beach and the middle of an island is
		// a low sandy rise rather than a mountain -- at 0.10 a 30 m island
		// peaks about 3 m above the sea instead of 19 m.
		// The crease at the waterline is deliberate: a beach really does
		// change slope where it enters the water.
		float base = mask > 0.0f ? mask * s_MaskToHeight : mask * s_SeabedDrop;

		return base + relief * coastFade;
	}

	// Analytic, not finite-difference: the winning island's own mask is
	// `Radius - |p - centre|`, whose gradient is `-(p - centre) / |p -
	// centre|` -- cheap, and exact everywhere except the measure-zero
	// boundary where two islands' masks are equal, which a true max() is
	// not differentiable at either. Relief's own gradient is dropped, which
	// under-reports slope on the hilliest ground by up to relief's
	// amplitude against the mask's much larger one -- a small, known
	// approximation, kept because the alternative (finite-differencing
	// Height, as VoxelField3D::SampleNormal and
	// Heightfield3D::SmoothNormalAt do for exactly this kind of function)
	// measured at 7.8 ms per 4,096-voxel chunk: four extra Height() calls
	// per voxel, each re-running both noise octaves and the island loop.
	// This version measures at 1.7 ms/chunk for the same field -- see the
	// changelog entry.
	glm::vec2 Slope(float x, float z) const
	{
		const Island* winner = nullptr;
		IslandMask(x, z, &winner);

		if (!winner)
			return glm::vec2(0.0f);

		glm::vec2 toCentre = glm::vec2(x, z) - winner->Centre;
		float d = glm::length(toCentre);

		if (d < 1e-4f)
			return glm::vec2(0.0f);

		// **The same constants Height uses, and the same branch.** These were
		// two separate 0.55 literals; shared constants are the only thing
		// stopping a change to the terrain's height from silently leaving the
		// normals describing the old shape -- and now that land and seabed
		// scale differently, the side has to be picked here too.
		float mask = winner->Radius - d;
		float scale = mask > 0.0f ? s_MaskToHeight : s_SeabedDrop;

		return -(toCentre / d) * scale;
	}

	// Same technique VoxelTerrain::Density uses: a height function's
	// gradient has length sqrt(1 + |grad h|^2), which changes faster than a
	// distance does, so dividing by that length gives the first-order true
	// distance -- without it, sphere tracing (and the sparse-chunk test,
	// which relies on the same Lipschitz property) would step past thin
	// slopes.
	float Density(const glm::vec3& p) const
	{
		float h = Height(p.x, p.z);
		glm::vec2 slope = Slope(p.x, p.z);
		return (p.y - h) / std::sqrt(1.0f + glm::dot(slope, slope));
	}

	// Value noise: an integer hash per lattice corner, smoothstep-
	// interpolated. The same idiom as Terrain::ValueNoise and
	// VoxelTerrain::Noise3D -- a hash of the coordinate rather than a
	// generator with state, so the map is a pure function of x, z and seed
	// and does not depend on what was sampled before it. Written fresh
	// rather than reused because both existing versions are private to
	// their own class.
	static uint32_t Hash2D(int x, int y, uint32_t seed)
	{
		uint32_t h = seed;
		h ^= (uint32_t)(x * 374761393);
		h ^= (uint32_t)(y * 668265263);
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return h;
	}

	static float Hash2DUnit(int x, int y, uint32_t seed)
	{
		return (float)(Hash2D(x, y, seed) & 0xFFFFFF) / (float)0xFFFFFF;
	}

	static float Noise2D(float x, float y, uint32_t seed)
	{
		float xi = std::floor(x), yi = std::floor(y);
		float fx = x - xi, fy = y - yi;
		float sx = fx * fx * (3.0f - 2.0f * fx);
		float sy = fy * fy * (3.0f - 2.0f * fy);

		int xii = (int)xi, yii = (int)yi;
		float n00 = Hash2DUnit(xii, yii, seed);
		float n10 = Hash2DUnit(xii + 1, yii, seed);
		float n01 = Hash2DUnit(xii, yii + 1, seed);
		float n11 = Hash2DUnit(xii + 1, yii + 1, seed);

		// Centred on zero rather than [0,1), so it can push the mask either
		// way instead of only ever raising it.
		return glm::mix(glm::mix(n00, n10, sx), glm::mix(n01, n11, sx), sy) - 0.5f;
	}

	// --- The walker ---------------------------------------------------------

	static constexpr float s_WalkerRadius = 0.35f;
	static constexpr float s_WalkerHalfHeight = 0.55f;
	static constexpr float s_EyeHeight = 0.75f;

	// A lumpy, flat-shaded blob on the unit sphere: a lattice of points whose
	// radius is pushed in and out by the same hash the rest of the world uses,
	// emitted as independent triangles so every face gets its own normal.
	//
	// Flat normals are the point. A smooth-shaded rock under cel banding is a
	// soft gradient with a couple of bands crossing it; a faceted one is a set
	// of flat plates, each a single shade, which is what makes it read as
	// stone in this style at all.
	static Egss::MeshData MakeRockMesh(unsigned int seed)
	{
		const int segments = 16, rings = 10;

		auto point = [&](int i, int j)
		{
			// Wrap the seam so the last column is literally the first.
			int wrapped = i % segments;

			float u = (float)wrapped / (float)segments * 6.2831853f;
			float v = (float)j / (float)rings * 3.14159265f;

			// 0.84..1.0. Was 0.68..1.0 on a 9x6 lattice, which read as a lump
			// of coal -- more facets and a shallower jitter give a boulder that
			// is still faceted but no longer jagged. The ceiling stays at 1.0
			// so the blob cannot leave the box that collides for it.
			float radius = 0.84f + Hash2DUnit(wrapped, j, seed) * 0.16f;

			// Poles pulled in a little, or a jittered pole spikes.
			if (j == 0 || j == rings)
				radius = 0.86f + Hash2DUnit(0, j, seed) * 0.10f;

			return glm::vec3(
				std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u)) * radius;
		};

		Egss::MeshData data;

		auto face = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			glm::vec3 n = glm::cross(b - a, c - a);
			if (glm::length(n) < 1e-8f)
				return;

			n = glm::normalize(n);

			unsigned int base = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ a, n, { 0.0f, 0.0f } });
			data.Vertices.push_back({ b, n, { 1.0f, 0.0f } });
			data.Vertices.push_back({ c, n, { 0.5f, 1.0f } });
			data.Indices.push_back(base);
			data.Indices.push_back(base + 1);
			data.Indices.push_back(base + 2);
		};

		for (int j = 0; j < rings; j++)
		{
			for (int i = 0; i < segments; i++)
			{
				glm::vec3 a = point(i, j), b = point(i + 1, j);
				glm::vec3 c = point(i + 1, j + 1), d = point(i, j + 1);

				// Degenerate at the poles, where the whole ring is one point --
				// `face` drops those on the zero-area test.
				face(a, b, c);
				face(a, c, d);
			}
		}

		Egss::Submesh all;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes.push_back(all);
		data.RecalculateBounds();

		return data;
	}

	// Grey rocks, scattered on the spawn island and left to the solver.
	//
	// **Boxes, not spheres.** The first version used sphere colliders with a
	// coarse sphere mesh, which matched perfectly and behaved terribly: a
	// sphere on a slope rolls, this island is a dome, and every rock rolled
	// down the beach, into the sea, down the seabed, and eventually far enough
	// inside the field that the narrowphase stopped pushing it out and it fell
	// forever. Measured: a rock landed correctly at Height + radius, then crept
	// 3.69 -> 3.58 -> 3.10 and was at -18.8 m doing -17.6 m/s by step 599.
	//
	// Nothing was wrong with the collision. Real rocks do not roll away because
	// real rocks are not spheres, and a rigid-body solver has no rolling
	// resistance to stand in for that. A box rests on a face.
	//
	// The mesh is a **jittered sphere inscribed in the box**, not the box
	// itself. Drawing the collider is the honest thing and it looked like a
	// crate; a rock has to look like a rock. The mesh never leaves the box --
	// its radius is at most 1 in the box's own units -- so it can only ever be
	// *inside* what it collides with, which is the direction that reads as a
	// rock half-buried rather than as one floating.
	void SpawnRocks(const Island& island)
	{
		// A handful of distinct shapes, cycled, so sixteen rocks are not one
		// rock sixteen times.
		for (int i = 0; i < s_RockShapes; i++)
			m_RockMeshes[i].reset(new Egss::Mesh(MakeRockMesh(101u + (unsigned int)i * 37u), "Rock"));

		// Kept inside the radius the attach-time stream already filled. A rock
		// dropped over a chunk that does not exist yet has nothing to land on,
		// falls, and then has to be ejected once the ground appears under it.
		float scatter = glm::min(island.Radius * 0.8f, s_ChunkWorld * 2.2f);

		for (int i = 0; i < s_RockCount; i++)
		{
			float angle = Hash2DUnit(i, 11, 31u) * 6.2831853f;
			float distance = std::sqrt(Hash2DUnit(i, 12, 31u)) * scatter;

			// sqrt on the radius, or every rock bunches at the centre: the
			// area of a ring grows with r, so a uniform radius does not give a
			// uniform scatter.
			glm::vec2 at = island.Centre + glm::vec2(std::cos(angle), std::sin(angle)) * distance;

			float radius = s_RockMinRadius
				+ Hash2DUnit(i, 13, 31u) * (s_RockMaxRadius - s_RockMinRadius);

			// Squashed a little differently on each axis, so sixteen rocks are
			// not sixteen cubes. Kept modest: a very flat box on a slope slides
			// rather than resting, which is the same problem in another shape.
			glm::vec3 half(
				radius * (0.75f + Hash2DUnit(i, 14, 31u) * 0.5f),
				radius * (0.60f + Hash2DUnit(i, 15, 31u) * 0.4f),
				radius * (0.75f + Hash2DUnit(i, 16, 31u) * 0.5f));

			// Dropped from just clear of the surface so it settles onto the
			// ground rather than starting interpenetrating it.
			float y = Height(at.x, at.y) + half.y + 0.30f;

			// Mass from volume, so the big ones behave like big ones: a 0.4 m
			// rock lands near 20 kg and a 1.1 m one near 400.
			float mass = 2000.0f * half.x * half.y * half.z;

			Egss::RigidBody3D rock = Egss::RigidBody3D::MakeBox({ at.x, y, at.y }, half, mass);

			// Turned about the vertical only. A box tipped onto a corner has to
			// fall over before it rests, which is a second of every rock
			// wobbling at startup for no gain.
			rock.Orientation = glm::angleAxis(
				Hash2DUnit(i, 17, 31u) * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
			rock.UpdateInertiaWorld();

			rock.Friction = 0.9f;
			rock.Restitution = 0.0f;
			rock.LinearDamping = 0.2f;
			rock.AngularDamping = 0.4f;

			m_Rocks.push_back({ m_World.AddBody(rock), half, i % s_RockShapes });
		}
	}

	void SpawnWalker(const glm::vec3& near)
	{
		float ground = 0.0f;
		glm::vec3 normal(0.0f);
		m_World.GroundBelow(near, ground, normal);

		Egss::RigidBody3D body = Egss::RigidBody3D::MakeCapsule(
			{ near.x, ground + s_EyeHeight + 1.0f, near.z }, s_WalkerRadius,
			s_WalkerHalfHeight, 75.0f);

		body.Friction = 0.4f;
		body.Restitution = 0.0f;
		body.LinearDamping = 0.05f;

		m_Walker = m_World.AddBody(body);
	}

	// --- Draw ---------------------------------------------------------------

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		(void)ts;

		// **Set the state rather than inherit it.** Cull mode is global and
		// outlives whichever demo last touched it -- a demo that assumes the
		// default is at the mercy of the one selected before it, which is
		// exactly how this demo's water came to be culled from below.
		Egss::RenderCommand::SetCullFace(Egss::CullFace::None);

		// Under the surface the sky is not the sky. Clearing to the water
		// colour is most of what makes being submerged *look* like being
		// submerged; without it, swimming renders identically to standing on
		// dry sand and the only blue left is the distant sea seen edge-on.
		m_Underwater = m_Camera.GetPosition().y < s_SeaLevel;

		Egss::RenderCommand::SetClearColor(m_Underwater
			? glm::vec4(m_Deep, 1.0f)
			: glm::vec4(0.53f, 0.68f, 0.79f, 1.0f));
		Egss::RenderCommand::Clear();

		Egss::Renderer::ResetStats();

		Egss::Frustum frustum = Egss::Frustum::FromViewProjection(m_Camera.GetViewProjectionMatrix());

		Egss::Renderer::BeginScene(m_Camera);

		m_Material->Set("u_SunDirection", glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)));
		m_Material->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_Material->Set("u_SkyColor", glm::vec3(0.5f, 0.6f, 0.75f));
		m_Material->Set("u_Ambient", 0.35f);
		m_Material->Set("u_Color", m_SandColour);
		m_Material->Set("u_Textured", m_Textured ? 1 : 0);
		m_Material->Set("u_Bands", m_Bands);
		m_Material->Set("u_CameraPosition", m_Camera.GetPosition());
		m_Material->Set("u_Underwater", m_Underwater ? 1 : 0);
		m_Material->Set("u_Deep", m_Deep);
		m_Material->Set("u_FogDensity", m_FogDensity);
		m_Material->Set("u_Grass", m_GrassColour);
		m_Material->Set("u_GrassLow", m_GrassLow);
		m_Material->Set("u_GrassHigh", m_GrassHigh);
		m_Material->Set("u_Terrain", 1);
		m_Material->Set("u_Quantise", m_Cel ? 1.0f : 0.0f);

		// Opt-in and blocking -- see RendererAPI::EndGpuTimerMs. Only around
		// the terrain pass, since that is the one whose cost the texture
		// toggle actually changes.
		if (m_MeasureGpu)
			Egss::RenderCommand::BeginGpuTimer();

		m_ChunksDrawn = 0;
		for (const auto& [key, entry] : m_Chunks)
		{
			if (m_Culling && !frustum.Intersects(entry.Bounds))
				continue;

			Egss::Renderer::Submit(m_Material, entry.MeshPtr, glm::mat4(1.0f));
			m_ChunksDrawn++;
		}

		if (m_MeasureGpu)
			m_LastGpuMs = Egss::RenderCommand::EndGpuTimerMs();

		// Its own colour, a shade off the ground's, so the blades read against
		// what they are standing in rather than disappearing into it.
		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);
		m_Material->Set("u_Color", m_BladeColour);

		m_GrassDrawn = 0;
		for (const auto& [key, entry] : m_Chunks)
		{
			if (!entry.GrassPtr)
				continue;
			if (m_Culling && !frustum.Intersects(entry.Bounds))
				continue;

			Egss::Renderer::Submit(m_Material, entry.GrassPtr, glm::mat4(1.0f));
			m_GrassDrawn++;
		}

		// Before the water, so a rock sitting in the shallows gets the water
		// blended over it rather than punched through it.
		DrawRocks();

		// Water: tested against the depth already in the buffer (so terrain
		// above sea level still occludes it) but not written to it, so it
		// does not wrongly reject whatever renders behind it -- there is
		// nothing else transparent here yet, but the ocean is one surface
		// and does not need to sort against itself.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Color", m_WaterColour);
		m_Material->Set("u_Textured", 0);
		Egss::Renderer::Submit(m_Material, m_Water, glm::mat4(1.0f));

		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);

		Egss::Renderer::EndScene();

		m_Stats = Egss::Renderer::GetStats();
	}

	// Identifies the world the cache belongs to, by **sampling the terrain
	// function** rather than by a version number somebody has to remember to
	// bump. Change the islands, the noise, the sea level or the voxel size and
	// at least one of these samples moves, the fingerprint changes, and the
	// stale file is discarded instead of quietly serving the old world.
	//
	// 512 samples is about a tenth of one chunk's worth of evaluation, paid
	// once at attach.
	unsigned long long FingerprintDensity() const
	{
		unsigned long long hash = 1469598103934665603ull;   // FNV-1a, 64-bit

		auto mix = [&hash](const void* data, size_t n)
		{
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < n; i++)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};

		// The lattice geometry matters too: the same density function sampled
		// at a different voxel size is a different set of stored chunks.
		const int dims[3] = { s_SideX, s_SideY, s_SideZ };
		mix(dims, sizeof(dims));
		mix(&s_Voxel, sizeof(s_Voxel));

		// Fixed, arbitrary, and spread across the whole field: fixed so two
		// runs of one build agree, spread so a change anywhere is likely to
		// move at least one sample.
		for (int i = 0; i < 512; i++)
		{
			float t = (float)i;
			glm::vec3 at(
				std::fmod(t * 37.0f, 380.0f) - 190.0f,
				std::fmod(t * 11.0f, 90.0f),
				std::fmod(t * 53.0f, 380.0f) - 190.0f);

			float d = Density(at);
			mix(&d, sizeof(d));
		}

		return hash;
	}

	void DrawRocks()
	{
		if (!m_RockMeshes[0])
			return;

		m_Material->Set("u_Terrain", 0);
		m_Material->Set("u_Textured", 0);
		m_Material->Set("u_Color", m_RockColour);

		for (const Rock& rock : m_Rocks)
		{
			const Egss::RigidBody3D& body = m_World.GetBody(rock.Handle);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), body.Position)
				* glm::mat4_cast(body.Orientation)
				// The mesh spans -1..1, so the half-extents scale it directly.
				* glm::scale(glm::mat4(1.0f), rock.HalfExtents);

			Egss::Renderer::Submit(m_Material, m_RockMeshes[rock.Shape], transform);
		}
	}

	// --- Water --------------------------------------------------------------

	void BuildWater()
	{
		float half = 0.5f * (s_SideX - 1) * s_Voxel;

		Egss::MeshData data;
		data.Vertices = {
			{ { -half, s_SeaLevel, -half }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
			{ {  half, s_SeaLevel, -half }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
			{ {  half, s_SeaLevel,  half }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
			{ { -half, s_SeaLevel,  half }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
		};
		data.Indices = { 0, 1, 2, 0, 2, 3 };
		data.Submeshes.push_back({ "", -1, 0, (unsigned int)data.Indices.size() });

		m_Water = std::make_shared<Egss::Mesh>(data, "Water");
	}

	// --- Shader ---------------------------------------------------------

	void BuildShader()
	{
		// A sun, not a point light -- the map is 400 m across, and a point
		// light's falloff leaves almost all of it lit by ambient alone. See
		// the same note in VoxelTerrain and the HANDOVER entry it cites.
		std::string vertexSrc = R"(
			#version 330 core
			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_Normal;
			out vec2 v_TexCoord;
			out vec3 v_WorldPosition;

			void main()
			{
				v_WorldPosition = (u_Transform * vec4(a_Position, 1.0)).xyz;
				v_Normal = mat3(u_Transform) * a_Normal;
				// World-space planar, from MarchingCubes -- see the comment
				// where it is generated. Spatially varying rather than
				// per-vertex-fixed is what makes a texture-cost comparison
				// honest: constant UVs would sample the same texel over
				// and over, which the cache makes artificially cheap.
				v_TexCoord = a_TexCoord;
				gl_Position = u_ViewProjection * u_Transform * vec4(a_Position, 1.0);
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core
			layout(location = 0) out vec4 color;

			in vec3 v_Normal;
			in vec2 v_TexCoord;
			in vec3 v_WorldPosition;

			uniform vec3 u_CameraPosition;
			uniform int u_Underwater;
			uniform vec3 u_Deep;
			uniform float u_FogDensity;

			// Terrain gets the sand/grass blend; water and rocks do not, which
			// is what this gate is for -- without it the sea would sprout grass
			// wherever the quad happened to sit above the line.
			uniform int u_Terrain;
			uniform vec3 u_Grass;
			uniform float u_GrassLow;
			uniform float u_GrassHigh;

			uniform vec4 u_Color;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunColor;
			uniform vec3 u_SkyColor;
			uniform float u_Ambient;
			uniform int u_Textured;
			uniform sampler2D u_BaseColourMap;

			uniform int u_Bands;
			uniform float u_Quantise;

			// The Cel demo's quantiser, unchanged: floor to a level, clamp the
			// point facing the light into the top band, then divide by
			// **bands - 1** so the levels span 0..1 inclusive. Dividing by
			// bands instead caps the brightest band at (bands-1)/bands and
			// lays a haze over everything.
			float Quantise(float x)
			{
				float steps = float(max(u_Bands, 2));
				float level = min(floor(x * steps), steps - 1.0);
				return mix(x, level / (steps - 1.0), u_Quantise);
			}

			void main()
			{
				vec4 base = u_Color;
				if (u_Textured == 1)
					base *= texture(u_BaseColourMap, v_TexCoord);

				vec3 n = normalize(v_Normal);

				if (u_Terrain == 1)
				{
					// Height decides where grass starts; slope decides whether
					// it can hold on. Grass on a near-vertical face looks
					// painted on, and the dunes are steep enough at their edges
					// for that to show.
					// `flatness`, not `flat` -- `flat` is a GLSL interpolation
					// qualifier, and using it as a variable is a syntax error
					// that takes the whole shader out. The engine logs the
					// failure and carries on with an unusable program, which
					// renders white; the log said so immediately and the
					// picture did not.
					float high = smoothstep(u_GrassLow, u_GrassHigh, v_WorldPosition.y);
					float flatness = smoothstep(0.55, 0.88, n.y);

					base.rgb = mix(base.rgb, u_Grass, high * flatness);
				}

				// Both terms are banded, not only the sun. Banding just the
				// sun leaves the sky gradient sliding smoothly underneath the
				// hard sun edges, which reads as a bug rather than as a style
				// -- the flat regions have to agree with each other.
				float sun = Quantise(max(dot(n, -u_SunDirection), 0.0));
				float sky = Quantise(0.5 + 0.5 * n.y);

				vec3 lit = base.rgb * (u_Ambient + sun * u_SunColor + sky * u_SkyColor * 0.35);

				// Beer-Lambert, the same exponential a real attenuating medium
				// follows: what survives over distance d is exp(-density*d), so
				// what the water has replaced is one minus that. Applied to the
				// terrain and to the water surface alike, so the surface seen
				// from below fades into the distance with everything else.
				if (u_Underwater == 1)
				{
					float d = length(v_WorldPosition - u_CameraPosition);
					lit = mix(lit, u_Deep, 1.0 - exp(-u_FogDensity * d));
				}

				color = vec4(lit, base.a);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("OpenWorldSun", vertexSrc, fragmentSrc));
		m_Material = Egss::Material::Create(m_Shader);

		m_GroundTexture.reset(Egss::Texture2D::Create("assets/models/checker.png"));
		m_Material->SetTexture("u_BaseColourMap", m_GroundTexture);
	}

	// --- Panel ------------------------------------------------------------

	void OnDemoImGui() override
	{
		ImGui::Begin("Open World");

		ImGui::Checkbox("First person", &m_FirstPerson);
		if (m_FirstPerson)
		{
			ImGui::Text("%s", FirstPersonController::MotionName(m_Controller.GetMotion()));
			ImGui::TextDisabled("Space swims up / jumps, Shift dives");
		}

		m_Controller.MouseLookHelp();

		ImGui::Separator();
		ImGui::SliderFloat("Load radius", &m_LoadRadius, 16.0f, 160.0f);
		ImGui::SliderInt("Chunks per step", &m_ChunksPerStep, 1, 8);
		ImGui::Checkbox("Frustum culling", &m_Culling);

		ImGui::Separator();
		ImGui::Checkbox("Chunk LOD", &m_Lod);
		ImGui::SameLine();
		ImGui::Checkbox("Skirts", &m_Skirts);
		ImGui::SliderFloat("Stride 2 beyond", &m_LodNear, 16.0f, 200.0f, "%.0f m");
		ImGui::SliderFloat("Stride 4 beyond", &m_LodFar, 32.0f, 300.0f, "%.0f m");
		ImGui::SliderFloat("Hysteresis", &m_LodHysteresis, 0.0f, 32.0f, "%.0f m");
		ImGui::SliderInt("LOD remeshes per step", &m_LodPerStep, 1, 8);

		{
			int perStride[3] = { 0, 0, 0 };
			unsigned int trianglesPerStride[3] = { 0, 0, 0 };
			for (const auto& [key, entry] : m_Chunks)
			{
				int slot = entry.Stride == 1 ? 0 : (entry.Stride == 2 ? 1 : 2);
				perStride[slot]++;
				trianglesPerStride[slot] += entry.MeshPtr->GetTriangleCount();
			}

			ImGui::Text("stride 1: %d chunks, %u tris", perStride[0], trianglesPerStride[0]);
			ImGui::Text("stride 2: %d chunks, %u tris", perStride[1], trianglesPerStride[1]);
			ImGui::Text("stride 4: %d chunks, %u tris", perStride[2], trianglesPerStride[2]);
			ImGui::Text("%d remeshed for LOD last step", m_LodRemeshes);
		}

		ImGui::Separator();
		ImGui::Text("%zu / %zu chunks filled", m_Filled.size(), (size_t)
			m_Field->ChunkCount().x * m_Field->ChunkCount().y * m_Field->ChunkCount().z);
		ImGui::Text("%zu chunks meshed, %d drawn this frame", m_Chunks.size(), m_ChunksDrawn);
		ImGui::Text("%u draw calls, %u triangles", m_Stats.DrawCalls, m_Stats.TriangleCount);

		ImGui::Separator();
		ImGui::Checkbox("Chunk cache", &m_UseCache);
		ImGui::Text("%zu stored, %u hits, %u generated (%.1f MB on disk)",
			m_Cache.Entries(), m_Cache.Hits(), m_Cache.Written(),
			m_Cache.BytesOnDisk() / (1024.0 * 1024.0));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Beside the executable, keyed by a fingerprint of the\n"
				"density function -- change the terrain and it rebuilds itself.");

		ImGui::Separator();
		ImGui::ColorEdit3("Sand", &m_SandColour.x);
		ImGui::ColorEdit3("Grass", &m_GrassColour.x);
		ImGui::SliderFloat("Grass from", &m_GrassLow, 0.0f, 4.0f, "%.1f m");
		ImGui::SliderFloat("Grass by", &m_GrassHigh, 0.0f, 6.0f, "%.1f m");
		ImGui::Checkbox("Grass blades", &m_Grass);
		ImGui::ColorEdit3("Blade", &m_BladeColour.x);
		ImGui::SliderFloat("Blades per triangle", &m_GrassDensity, 0.0f, 2.0f);
		ImGui::SliderFloat("Blade height", &m_GrassHeight, 0.1f, 1.0f, "%.2f m");
		ImGui::TextDisabled("%d chunks of grass drawn (stride 1 only)", m_GrassDrawn);
		ImGui::ColorEdit4("Water", &m_WaterColour.x);
		ImGui::ColorEdit3("Underwater", &m_Deep.x);
		ImGui::SliderFloat("Underwater fog", &m_FogDensity, 0.0f, 0.25f, "%.3f /m");
		ImGui::Checkbox("Cel shading", &m_Cel);
		ImGui::SliderInt("Bands", &m_Bands, 2, 8);
		ImGui::Checkbox("Textured ground", &m_Textured);
		ImGui::Checkbox("Measure terrain GPU time", &m_MeasureGpu);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Blocks the CPU on the GPU's result every frame --\n"
				"a diagnostic for A/B comparison, not something to leave on.");
		if (m_MeasureGpu)
			ImGui::Text("terrain pass: %.3f ms GPU", m_LastGpuMs);

		ImGui::End();
	}

	// --- State ------------------------------------------------------------

	static constexpr float s_Voxel = 0.5f;
	static constexpr int s_SideX = 800;
	static constexpr int s_SideY = 200;
	static constexpr int s_SideZ = 800;
	static constexpr float s_OriginY = -25.0f;
	static constexpr int s_RockCount = 16;
	static constexpr int s_RockShapes = 5;
	static constexpr float s_RockMinRadius = 0.35f;
	static constexpr float s_RockMaxRadius = 1.15f;

	static constexpr float s_SeaLevel = 0.0f;

	// --- Island shape ---
	//
	// Low, small and sandy rather than mountainous. Height at an island's
	// centre is Radius * s_MaskToHeight, so these numbers say "a 22-40 m
	// island rising 2.2-4.0 m above the water", with another metre or so of
	// dune noise on top. Raising s_MaskToHeight is what makes mountains; it
	// is shared with Slope, which needs the same figure for its normals.
	static constexpr float s_IslandRadiusMin = 22.0f;
	static constexpr float s_IslandRadiusMax = 40.0f;

	// **Two scales, not one.** The mask is positive inland and negative at
	// sea, so a single constant shapes the island and the sea floor together
	// -- and flattening the islands with one flattened the seabed with them.
	// At 0.10 the bottom only reached its -80 m mask floor about 80 m out, so
	// every island sat in a huge shin-deep shelf: bright sand under a thin
	// film of water, which reads as more beach rather than as sea. It is why
	// the water looked like it had gone.
	//
	// Land stays flat. Water drops away at the old rate, so the bottom is
	// 5.5 m down within 10 m of the shore and at its floor by about 15 m.
	static constexpr float s_MaskToHeight = 0.10f;
	static constexpr float s_SeabedDrop = 0.55f;
	static constexpr float s_ReliefBroad = 0.9f;
	static constexpr float s_ReliefFine = 0.35f;
	static constexpr int s_IslandCount = 5;

	// Egss::VoxelField3D::ChunkSize is a runtime constant expression too,
	// but this keeps the arithmetic in one place at the top of the file.
	static constexpr float s_ChunkWorld = 16.0f * s_Voxel;

	Egss::PerspectiveCamera m_Camera;
	FirstPersonController m_Controller;

	bool m_FirstPerson = true;
	bool m_Grounded = false;
	bool m_Culling = true;
	float m_LoadRadius = 128.0f;
	// Measured, not guessed: 3 filled + their remesh cascade cost 23-105 ms
	// of CPU time in a single fixed step on this machine's desktop CPU
	// (density evaluation and marching cubes are both CPU-side; the GPU is
	// not involved). 1 keeps the worst case closer to a 16 ms frame, at the
	// cost of the world taking three times as many steps to finish
	// populating around the player. See the changelog entry for the numbers
	// this was tuned against.
	int m_ChunksPerStep = 1;

	// --- LOD ---
	//
	// Deliberately *not* registered as replay parameters, unlike the load
	// radius above. The load radius decides which chunks get **filled**, and
	// the field is what the physics collides against, so moving it changes the
	// run. LOD only decides how finely a chunk that is already filled gets
	// **meshed**, and nothing collides with the mesh -- so it changes the
	// picture and the triangle count, and not the simulation.
	bool m_Lod = true;

	// **Default off, on measurement.** Skirts were built to close the LOD
	// seam and they do not, because the seam is not a hole: a coarse chunk
	// meshes systematically *lower* than its fine neighbour, and the result is
	// a solid step whose wall you can see. There is no gap for a skirt to
	// fill. Generated correctly (582 tris a chunk became 718, so 68 boundary
	// edges found), and the picture moved by **2 pixels** even with the bands
	// forced to 10 m and 20 m to put mismatched chunks right under the camera.
	// 23% more triangles for two pixels is not a trade worth making.
	//
	// Kept because the mechanism is right for an actual crack, and because the
	// measurement is the useful part: the fix for the step is transition cells
	// (transvoxel), which reconcile the two lattices instead of hanging a
	// curtain off one of them.
	bool m_Skirts = false;
	float m_LodNear = 56.0f;    // beyond this, stride 2
	float m_LodFar = 104.0f;     // beyond this, stride 4
	float m_LodHysteresis = 8.0f;
	int m_LodPerStep = 2;
	int m_LodRemeshes = 0;

	std::vector<Island> m_Islands;

	std::shared_ptr<Egss::VoxelField3D> m_Field;

	struct ChunkEntry
	{
		std::shared_ptr<Egss::Mesh> MeshPtr;
		Egss::Aabb Bounds;

		// Which lattice this mesh was built on. Kept per chunk rather than
		// recomputed from the distance, because the distance is what the
		// chunk *wants* and this is what it currently *is* -- the difference
		// between the two is the whole of the LOD update, and hysteresis
		// needs both.
		int Stride = 1;
		glm::ivec3 Coord{ 0 };

		// Null on a coarse chunk, or on one with no grass-worthy ground.
		std::shared_ptr<Egss::Mesh> GrassPtr;
	};
	std::map<size_t, ChunkEntry> m_Chunks;
	std::unordered_set<size_t> m_Filled;

	ChunkCache m_Cache;
	bool m_UseCache = true;
	std::vector<unsigned char> m_ChunkBytes;   // reused, so streaming does not allocate

	std::vector<glm::ivec2> m_Ring;
	int m_RingReach = -1;
	glm::ivec2 m_RingCentre{ INT_MIN };
	size_t m_RingCursor = 0;
	int m_ChunksDrawn = 0;

	std::shared_ptr<Egss::Mesh> m_Water;

	struct Rock
	{
		Egss::PhysicsWorld3D::BodyHandle Handle;
		glm::vec3 HalfExtents;
		int Shape;
	};
	std::vector<Rock> m_Rocks;
	std::shared_ptr<Egss::Mesh> m_RockMeshes[s_RockShapes];

	// Grey, and under the 1.525 exposure ceiling like everything else here.
	glm::vec4 m_RockColour{ 0.30f, 0.30f, 0.33f, 1.0f };

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;
	std::shared_ptr<Egss::Texture2D> m_GroundTexture;
	// Off by default now the terrain is meant to read as sand rather than as
	// a test surface -- the checker is a debug texture. The toggle stays,
	// because the textured-vs-untextured GPU comparison still wants it.
	bool m_Textured = false;

	// Chosen by arithmetic, not by eye. The shader's brightest possible
	// multiplier is u_Ambient + sun*sunColor + sky*skyColor*0.35, which is
	// 0.35 + 1.0 + 0.175 = 1.525 on the red channel -- so any albedo above
	// about 0.65 clips, and a clipped surface has no bands left because every
	// level saturates to the same white. A first attempt at (0.84, 0.76, 0.56)
	// measured (255, 255, 213): two channels pinned.
	//
	// At this albedo the brightest band should land on
	// (0.62*1.525, 0.56*1.52, 0.41*1.4925) = (0.945, 0.851, 0.612), or
	// (241, 217, 156).
	glm::vec4 m_SandColour{ 0.62f, 0.56f, 0.41f, 1.0f };

	// Was (0.10, 0.28, 0.42, 0.55), which blended over the sky to (82,138,178)
	// against a sky of (135,173,201) -- measurably different and still readable
	// as haze rather than as sea. Deeper and much more opaque, so the horizon
	// is a line between two clearly different things.
	glm::vec4 m_WaterColour{ 0.06f, 0.26f, 0.40f, 0.82f };

	// What everything fades toward while submerged, and how fast. 0.06 per
	// metre puts the fade at roughly half over 12 m, so the sea floor stays
	// readable underfoot while the distance closes in.
	// Grass takes over as the ground rises. The islands stand 2.2-4.0 m above
	// the sea, so the band sits in the middle of that: beach at the waterline,
	// green over the crown, and no hard line between them.
	//
	// Albedo picked under the same ceiling as the sand -- the brightest
	// multiplier is 1.525, so this peaks at (117, 174, 76) rather than
	// clipping.
	glm::vec3 m_GrassColour{ 0.30f, 0.45f, 0.20f };

	// --- Grass as geometry ---
	bool m_Grass = true;
	float m_GrassDensity = 0.6f;    // blades per qualifying terrain triangle
	float m_GrassHeight = 0.42f;
	float m_GrassWidth = 0.045f;
	int m_GrassDrawn = 0;
	glm::vec4 m_BladeColour{ 0.26f, 0.44f, 0.15f, 1.0f };
	float m_GrassLow = 1.1f;
	float m_GrassHigh = 2.6f;

	glm::vec3 m_Deep{ 0.05f, 0.20f, 0.32f };
	float m_FogDensity = 0.06f;
	bool m_Underwater = false;

	// Cel shading, the same quantiser as the Cel demo. No inverted-hull
	// outline here: a chunk mesh is an *open* surface that stops at the
	// chunk boundary, so an inflated copy would show its back faces along
	// every one of those edges -- hundreds of them -- rather than only at
	// the silhouette. Outlines want a closed mesh or a depth-discontinuity
	// pass, and the second is a different piece of work.
	bool m_Cel = true;
	int m_Bands = 4;

	bool m_MeasureGpu = false;
	double m_LastGpuMs = 0.0;

	Egss::PhysicsWorld3D m_World;
	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;

	Egss::Renderer::Statistics m_Stats;
};
