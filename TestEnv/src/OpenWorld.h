#pragma once

// A large play area: islands separated by water, streamed in around the
// player instead of baked upfront at load, and frustum-culled at
// submission. Built to answer several questions the roadmap needed a real
// scene for: does a big VoxelField3D world load in performantly with
// per-chunk generation, does culling actually reduce draw calls, and is a
// first-person controller built once and shared "up to scratch". Distant-
// chunk LOD and a textured-vs-untextured compute comparison are
// deliberately not in this piece -- both need this demo to exist first as
// the thing to measure, and are the next pass.
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

#include <unordered_set>

class OpenWorld : public DemoLayer
{
public:
	OpenWorld()
		: DemoLayer("OpenWorld"), m_Camera(70.0f, 16.0f / 9.0f, 0.1f, 500.0f),
		  m_Controller(m_Camera, -90.0f, -12.0f)
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
		glm::vec3 spawn(m_Islands[0].Centre.x, 40.0f, m_Islands[0].Centre.y);
		StreamAround(spawn, s_ChunkWorld * 3.0f, 100000);

		SpawnWalker(spawn);
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

		for (int dz = -reach; dz <= reach && budget > 0; dz++)
		{
			for (int dx = -reach; dx <= reach && budget > 0; dx++)
			{
				int cx = centre.x + dx;
				int cz = centre.y + dz;
				if (cx < 0 || cz < 0 || cx >= chunkCount.x || cz >= chunkCount.z)
					continue;

				glm::vec2 chunkCentreXZ = glm::vec2(m_Field->Origin().x, m_Field->Origin().z)
					+ (glm::vec2(cx, cz) + 0.5f) * s_ChunkWorld;
				if (glm::length(chunkCentreXZ - glm::vec2(focus.x, focus.z)) > radius)
					continue;

				for (int cy = 0; cy < chunkCount.y && budget > 0; cy++)
				{
					glm::ivec3 chunk(cx, cy, cz);
					size_t key = ChunkKey(chunk);

					if (m_Filled.count(key))
						continue;

					m_Field->FillChunk(chunk, sdf, 1);
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
			}
		}

		RebuildDirtyMeshes();
	}

	void RebuildDirtyMeshes()
	{
		const std::vector<glm::ivec3>& dirty = m_Field->DirtyChunks();

		for (const glm::ivec3& chunk : dirty)
		{
			glm::ivec3 min, max;
			m_Field->ChunkRange(chunk, min, max);

			Egss::MeshData data = Egss::MarchingCubes::Mesh(*m_Field, min, max);
			size_t key = ChunkKey(chunk);

			if (data.Indices.empty())
			{
				m_Chunks.erase(key);
				continue;
			}

			ChunkEntry entry;
			entry.MeshPtr = std::make_shared<Egss::Mesh>(data, "OpenWorldChunk");

			glm::vec3 chunkOrigin = m_Field->Origin() + glm::vec3(min) * s_Voxel;
			glm::vec3 chunkExtent = glm::vec3(max - min) * s_Voxel;
			entry.Bounds = { chunkOrigin, chunkOrigin + chunkExtent };

			m_Chunks[key] = entry;
		}

		m_Field->ClearDirtyChunks();
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
			float distance = 60.0f + Hash2DUnit(i, 1, 7u) * 90.0f;
			float radius = 35.0f + Hash2DUnit(i, 2, 7u) * 30.0f;

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

		float relief = 4.0f * Noise2D(x * 0.02f, z * 0.02f, 401u)
			+ 1.6f * Noise2D(x * 0.05f, z * 0.05f, 402u);

		// 0.55: the mask is metres of falloff per metre of distance from an
		// island's edge, which read as a cliff at 1:1. Scaled down, a
		// coastline slopes like a beach instead.
		return mask * 0.55f + relief * coastFade;
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

		// 0.55 matches the scale Height applies to the mask.
		return -(toCentre / d) * 0.55f;
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

		Egss::RenderCommand::SetClearColor({ 0.53f, 0.68f, 0.79f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer::ResetStats();

		Egss::Frustum frustum = Egss::Frustum::FromViewProjection(m_Camera.GetViewProjectionMatrix());

		Egss::Renderer::BeginScene(m_Camera);

		m_Material->Set("u_SunDirection", glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)));
		m_Material->Set("u_SunColor", glm::vec3(1.0f, 0.96f, 0.88f));
		m_Material->Set("u_SkyColor", glm::vec3(0.5f, 0.6f, 0.75f));
		m_Material->Set("u_Ambient", 0.35f);
		m_Material->Set("u_Color", glm::vec4(0.42f, 0.46f, 0.34f, 1.0f));
		m_Material->Set("u_Textured", m_Textured ? 1 : 0);

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

		// Water: tested against the depth already in the buffer (so terrain
		// above sea level still occludes it) but not written to it, so it
		// does not wrongly reject whatever renders behind it -- there is
		// nothing else transparent here yet, but the ocean is one surface
		// and does not need to sort against itself.
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::Alpha);
		Egss::RenderCommand::SetDepthWrite(false);

		m_Material->Set("u_Color", glm::vec4(0.10f, 0.28f, 0.42f, 0.55f));
		m_Material->Set("u_Textured", 0);
		Egss::Renderer::Submit(m_Material, m_Water, glm::mat4(1.0f));

		Egss::RenderCommand::SetDepthWrite(true);
		Egss::RenderCommand::SetBlendMode(Egss::BlendMode::None);

		Egss::Renderer::EndScene();

		m_Stats = Egss::Renderer::GetStats();
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

			void main()
			{
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

			uniform vec4 u_Color;
			uniform vec3 u_SunDirection;
			uniform vec3 u_SunColor;
			uniform vec3 u_SkyColor;
			uniform float u_Ambient;
			uniform int u_Textured;
			uniform sampler2D u_BaseColourMap;

			void main()
			{
				vec4 base = u_Color;
				if (u_Textured == 1)
					base *= texture(u_BaseColourMap, v_TexCoord);

				vec3 n = normalize(v_Normal);

				float sun = max(dot(n, -u_SunDirection), 0.0);
				float sky = 0.5 + 0.5 * n.y;

				vec3 lit = base.rgb * (u_Ambient + sun * u_SunColor + sky * u_SkyColor * 0.35);
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
			ImGui::Text(m_Grounded ? "grounded" : "airborne");

		m_Controller.MouseLookHelp();

		ImGui::Separator();
		ImGui::SliderFloat("Load radius", &m_LoadRadius, 16.0f, 160.0f);
		ImGui::SliderInt("Chunks per step", &m_ChunksPerStep, 1, 8);
		ImGui::Checkbox("Frustum culling", &m_Culling);

		ImGui::Separator();
		ImGui::Text("%zu / %zu chunks filled", m_Filled.size(), (size_t)
			m_Field->ChunkCount().x * m_Field->ChunkCount().y * m_Field->ChunkCount().z);
		ImGui::Text("%zu chunks meshed, %d drawn this frame", m_Chunks.size(), m_ChunksDrawn);
		ImGui::Text("%u draw calls, %u triangles", m_Stats.DrawCalls, m_Stats.TriangleCount);

		ImGui::Separator();
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
	static constexpr float s_SeaLevel = 0.0f;
	static constexpr int s_IslandCount = 5;

	// Egss::VoxelField3D::ChunkSize is a runtime constant expression too,
	// but this keeps the arithmetic in one place at the top of the file.
	static constexpr float s_ChunkWorld = 16.0f * s_Voxel;

	Egss::PerspectiveCamera m_Camera;
	FirstPersonController m_Controller;

	bool m_FirstPerson = true;
	bool m_Grounded = false;
	bool m_Culling = true;
	float m_LoadRadius = 64.0f;
	// Measured, not guessed: 3 filled + their remesh cascade cost 23-105 ms
	// of CPU time in a single fixed step on this machine's desktop CPU
	// (density evaluation and marching cubes are both CPU-side; the GPU is
	// not involved). 1 keeps the worst case closer to a 16 ms frame, at the
	// cost of the world taking three times as many steps to finish
	// populating around the player. See the changelog entry for the numbers
	// this was tuned against.
	int m_ChunksPerStep = 1;

	std::vector<Island> m_Islands;

	std::shared_ptr<Egss::VoxelField3D> m_Field;

	struct ChunkEntry
	{
		std::shared_ptr<Egss::Mesh> MeshPtr;
		Egss::Aabb Bounds;
	};
	std::map<size_t, ChunkEntry> m_Chunks;
	std::unordered_set<size_t> m_Filled;
	int m_ChunksDrawn = 0;

	std::shared_ptr<Egss::Mesh> m_Water;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_Material;
	std::shared_ptr<Egss::Texture2D> m_GroundTexture;
	bool m_Textured = true;

	bool m_MeasureGpu = false;
	double m_LastGpuMs = 0.0;

	Egss::PhysicsWorld3D m_World;
	Egss::PhysicsWorld3D::BodyHandle m_Walker = 0;

	Egss::Renderer::Statistics m_Stats;
};
