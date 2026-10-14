#include "gspch.h"
#include "GS/Procedural/TerrainMesh.h"

#include "GS/Procedural/Noise.h"
#include "GS/Renderer/MeshCache.h"

#include <glm/gtc/constants.hpp>

#include <cstdio>
#include <unordered_map>

namespace GS::TerrainMesh {

	namespace {
		// The primitives' own sizes (Mesh::Create*): 1 across.
		constexpr float s_Half = 0.5f;

		int Clamped(int resolution) { return std::clamp(resolution, 1, 512); }

		// A grid of (n+1)^2 vertices over the quad c0 c1 c2 c3 (counter-
		// clockwise from outside, as Mesh's cube faces and plane are),
		// wound the same way.
		void AppendGrid(MeshData& data, const glm::vec3& c0, const glm::vec3& c1, const glm::vec3& c3,
			const glm::vec3& normal, int n)
		{
			unsigned int base = (unsigned int)data.Vertices.size();
			for (int j = 0; j <= n; j++)
				for (int i = 0; i <= n; i++)
				{
					float s = (float)i / n, t = (float)j / n;
					data.Vertices.push_back({ c0 + s * (c1 - c0) + t * (c3 - c0), normal, { s, t } });
				}
			unsigned int stride = (unsigned int)n + 1;
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++)
				{
					unsigned int v00 = base + (unsigned int)j * stride + (unsigned int)i;
					unsigned int v10 = v00 + 1, v01 = v00 + stride, v11 = v01 + 1;
					data.Indices.insert(data.Indices.end(), { v00, v10, v11, v11, v01, v00 });
				}
		}

		MeshData Plane(int n)
		{
			MeshData data;
			AppendGrid(data, { -s_Half, 0, s_Half }, { s_Half, 0, s_Half }, { -s_Half, 0, -s_Half }, { 0, 1, 0 }, n);
			return data;
		}

		MeshData Cube(int n)
		{
			const float h = s_Half;
			// Mesh::CreateCubeData's faces: corners 0, 1 and 3 of each.
			const glm::vec3 faces[6][3] = {
				{ {-h,-h, h}, { h,-h, h}, {-h, h, h} },
				{ { h,-h,-h}, {-h,-h,-h}, { h, h,-h} },
				{ { h,-h, h}, { h,-h,-h}, { h, h, h} },
				{ {-h,-h,-h}, {-h,-h, h}, {-h, h,-h} },
				{ {-h, h, h}, { h, h, h}, {-h, h,-h} },
				{ {-h,-h,-h}, { h,-h,-h}, {-h,-h, h} } };
			const glm::vec3 normals[6] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
			MeshData data;
			for (int f = 0; f < 6; f++)
				AppendGrid(data, faces[f][0], faces[f][1], faces[f][2], normals[f], n);
			return data;
		}

		// A sphere-like grid of rows (top to bottom) by `segments`, wound as
		// Mesh::CreateSphereData is. `rows` gives each row's polar angle and
		// a height offset -- which is all that separates a capsule from a
		// sphere: the equator row twice, once at +halfHeight and once at
		// -halfHeight, with the tube between them.
		MeshData Rows(const std::vector<std::pair<float, float>>& rows, int segments, float radius)
		{
			MeshData data;
			for (size_t r = 0; r < rows.size(); r++)
			{
				float phi = rows[r].first;
				for (int s = 0; s <= segments; s++)
				{
					float u = (float)s / segments, theta = u * glm::two_pi<float>();
					glm::vec3 unit(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
					data.Vertices.push_back({ unit * radius + glm::vec3(0, rows[r].second, 0), unit,
						{ u, 1.0f - (float)r / (float)(rows.size() - 1) } });
				}
			}
			unsigned int stride = (unsigned int)segments + 1;
			for (size_t r = 0; r + 1 < rows.size(); r++)
				for (int s = 0; s < segments; s++)
				{
					unsigned int a = (unsigned int)r * stride + (unsigned int)s, b = a + stride;
					bool topPole = std::sin(rows[r].first) < 1e-6f;
					bool bottomPole = std::sin(rows[r + 1].first) < 1e-6f;
					if (!topPole)
						data.Indices.insert(data.Indices.end(), { a, a + 1, b });
					if (!bottomPole)
						data.Indices.insert(data.Indices.end(), { a + 1, b + 1, b });
				}
			return data;
		}

		// An open tube about y, `bands` high, wound counter-clockwise from
		// outside as Mesh::CreateCylinderData's is.
		MeshData Tube(int segments, int bands)
		{
			MeshData data;
			for (int k = 0; k <= bands; k++)
			{
				float y = -s_Half + (float)k / bands;
				for (int s = 0; s <= segments; s++)
				{
					float u = (float)s / segments, theta = u * glm::two_pi<float>();
					glm::vec3 outward(std::cos(theta), 0.0f, std::sin(theta));
					data.Vertices.push_back({ { outward.x * s_Half, y, outward.z * s_Half }, outward, { u, (float)k / bands } });
				}
			}
			unsigned int stride = (unsigned int)segments + 1;
			for (int k = 0; k < bands; k++)
				for (int s = 0; s < segments; s++)
				{
					unsigned int bottom = (unsigned int)k * stride + (unsigned int)s, top = bottom + stride;
					data.Indices.insert(data.Indices.end(), { bottom, top, bottom + 1, bottom + 1, top, top + 1 });
				}
			return data;
		}

		// Area-weighted normals, then averaged across every copy of the same
		// point -- seam and edge duplicates included -- so the displaced
		// surface shades as one piece rather than showing where the shape
		// was stitched.
		void WeldedNormals(MeshData& data)
		{
			std::vector<glm::vec3> accumulated(data.Vertices.size(), glm::vec3(0.0f));
			for (size_t i = 0; i + 2 < data.Indices.size(); i += 3)
			{
				const glm::vec3& a = data.Vertices[data.Indices[i]].Position;
				const glm::vec3& b = data.Vertices[data.Indices[i + 1]].Position;
				const glm::vec3& c = data.Vertices[data.Indices[i + 2]].Position;
				glm::vec3 face = glm::cross(b - a, c - a);
				for (int k = 0; k < 3; k++)
					accumulated[data.Indices[i + k]] += face;
			}

			struct KeyHash
			{
				size_t operator()(const glm::ivec3& k) const
				{
					return ((size_t)(uint32_t)k.x * 73856093u) ^ ((size_t)(uint32_t)k.y * 19349663u) ^ ((size_t)(uint32_t)k.z * 83492791u);
				}
			};
			std::unordered_map<glm::ivec3, glm::vec3, KeyHash> welded;
			auto key = [](const glm::vec3& p) { return glm::ivec3(glm::round(p * 100000.0f)); };
			for (size_t i = 0; i < data.Vertices.size(); i++)
				welded[key(data.Vertices[i].Position)] += accumulated[i];
			for (MeshVertex& vertex : data.Vertices)
			{
				glm::vec3 n = welded[key(vertex.Position)];
				float length = glm::length(n);
				vertex.Normal = length > 1e-12f ? n / length : glm::vec3(0, 1, 0);
			}
		}
	}

	bool Supports(const std::string& sourcePath)
	{
		return sourcePath == "primitive:plane" || sourcePath == "primitive:cube" || sourcePath == "primitive:sphere"
			|| sourcePath == "primitive:cylinder" || sourcePath == "primitive:capsule";
	}

	std::string Key(const std::string& sourcePath, const TerrainComponent& terrain)
	{
		char buffer[256];
		std::snprintf(buffer, sizeof(buffer), "terrain:%s|%d|%.9g|%.9g|%d|%.9g|%d|%d", sourcePath.c_str(),
			Clamped(terrain.Resolution), terrain.Height, terrain.Frequency, terrain.Octaves, terrain.Roughness,
			terrain.Seed, terrain.Ridged ? 1 : 0);
		return buffer;
	}

	float Displacement(const TerrainComponent& terrain, const glm::vec3& point)
	{
		return terrain.Height * Noise::Fbm3(point * terrain.Frequency, std::clamp(terrain.Octaves, 1, 10),
			terrain.Roughness, (uint32_t)terrain.Seed, terrain.Ridged);
	}

	MeshData BuildData(const std::string& sourcePath, const TerrainComponent& terrain)
	{
		int n = Clamped(terrain.Resolution);
		MeshData data;
		// Which way each shape is pushed: see the header.
		enum class Push { Up, Centre, Axis } push = Push::Centre;
		float axisHalf = 0.0f;

		if (sourcePath == "primitive:plane")
		{
			data = Plane(n);
			push = Push::Up;
		}
		else if (sourcePath == "primitive:cube")
			data = Cube(n);
		else if (sourcePath == "primitive:sphere")
			data = Mesh::CreateSphereData(s_Half, (unsigned int)(2 * n), (unsigned int)std::max(2, n));
		else if (sourcePath == "primitive:cylinder")
		{
			data = Tube(std::max(3, 2 * n), n);
			push = Push::Axis;
			axisHalf = s_Half;
		}
		else if (sourcePath == "primitive:capsule")
		{
			// Mesh::CreateCapsuleData's proportions: radius 0.5, tube 1 high.
			int half = std::max(1, n / 2);
			std::vector<std::pair<float, float>> rows;
			for (int r = 0; r <= half; r++)
				rows.push_back({ glm::half_pi<float>() * r / half, s_Half });
			for (int r = 0; r <= half; r++)
				rows.push_back({ glm::half_pi<float>() + glm::half_pi<float>() * r / half, -s_Half });
			data = Rows(rows, std::max(3, 2 * n), s_Half);
			push = Push::Axis;
			axisHalf = s_Half;
		}
		else
			return data;

		for (MeshVertex& vertex : data.Vertices)
		{
			glm::vec3 p = vertex.Position;
			glm::vec3 direction;
			if (push == Push::Up)
				direction = glm::vec3(0, 1, 0);
			else if (push == Push::Axis)
				direction = p - glm::vec3(0.0f, glm::clamp(p.y, -axisHalf, axisHalf), 0.0f);
			else
				direction = p;
			float length = glm::length(direction);
			if (length < 1e-6f)
				continue;
			vertex.Position = p + (direction / length) * Displacement(terrain, p);
		}

		WeldedNormals(data);
		Submesh all;
		all.FirstIndex = 0;
		all.IndexCount = (unsigned int)data.Indices.size();
		data.Submeshes = { all };
		data.RecalculateBounds();
		return data;
	}

	std::shared_ptr<Mesh> Get(const std::string& sourcePath, const TerrainComponent& terrain)
	{
		if (!Supports(sourcePath))
			return MeshCache::Get(sourcePath);

		// Shared like MeshCache's primitives. Bounded, since every drag of a
		// slider makes a new key: past a few dozen the cache starts over,
		// and anything still in use is kept alive by the entity holding it.
		static std::unordered_map<std::string, std::shared_ptr<Mesh>> cache;
		std::string key = Key(sourcePath, terrain);
		auto found = cache.find(key);
		if (found != cache.end())
			return found->second;
		if (cache.size() > 48)
			cache.clear();
		std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>(BuildData(sourcePath, terrain), "Terrain");
		cache[key] = mesh;
		return mesh;
	}
}
