#include "egsspch.h"
#include "Egss/Renderer/Mesh.h"
#include "Egss/Renderer/ObjLoader.h"
#include "Egss/Renderer/Buffer.h"
#include "Egss/Log.h"

#include <glm/gtc/constants.hpp>

namespace Egss {

	void MeshData::RecalculateBounds()
	{
		if (Vertices.empty())
		{
			BoundsMin = BoundsMax = glm::vec3(0.0f);
			return;
		}

		BoundsMin = BoundsMax = Vertices[0].Position;

		for (const MeshVertex& vertex : Vertices)
		{
			BoundsMin = glm::min(BoundsMin, vertex.Position);
			BoundsMax = glm::max(BoundsMax, vertex.Position);
		}
	}

	void MeshData::RecalculateNormals(const std::vector<bool>* keep)
	{
		auto keeps = [keep](size_t i)
		{
			return keep && i < keep->size() && (*keep)[i];
		};

		for (size_t i = 0; i < Vertices.size(); i++)
			if (!keeps(i))
				Vertices[i].Normal = glm::vec3(0.0f);

		for (size_t i = 0; i + 2 < Indices.size(); i += 3)
		{
			MeshVertex& a = Vertices[Indices[i]];
			MeshVertex& b = Vertices[Indices[i + 1]];
			MeshVertex& c = Vertices[Indices[i + 2]];

			// The cross product's *length* is twice the triangle's area, so
			// adding it un-normalised weights each face by its size for free.
			glm::vec3 faceNormal = glm::cross(b.Position - a.Position,
				c.Position - a.Position);

			if (!keeps(Indices[i]))     a.Normal += faceNormal;
			if (!keeps(Indices[i + 1])) b.Normal += faceNormal;
			if (!keeps(Indices[i + 2])) c.Normal += faceNormal;
		}

		for (size_t i = 0; i < Vertices.size(); i++)
		{
			if (keeps(i))
				continue;

			MeshVertex& vertex = Vertices[i];
			float length = glm::length(vertex.Normal);
			// A degenerate triangle contributes a zero cross product, and a
			// vertex touched only by those has nothing to normalise. Up is as
			// good a guess as any, and beats a NaN reaching the shader.
			vertex.Normal = (length > 1e-8f)
				? vertex.Normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
		}
	}

	Mesh::Mesh(const MeshData& data, const std::string& name)
		: m_Name(name),
		  m_Submeshes(data.Submeshes),
		  m_MaterialLibraries(data.MaterialLibraries),
		  m_VertexCount(data.Vertices.size()),
		  m_TriangleCount(data.TriangleCount()),
		  m_BoundsMin(data.BoundsMin),
		  m_BoundsMax(data.BoundsMax)
	{
		// A built primitive names no materials, and neither does an .obj that
		// never says `usemtl`. Giving those one range covering everything is
		// what lets a caller loop over submeshes without asking whether there
		// are any.
		if (m_Submeshes.empty() && !data.Indices.empty())
			m_Submeshes.push_back({ std::string(), 0u, (unsigned int)data.Indices.size() });

		m_VertexArray.reset(VertexArray::Create());

		std::shared_ptr<VertexBuffer> vertexBuffer;
		vertexBuffer.reset(VertexBuffer::Create((float*)data.Vertices.data(),
			(unsigned int)(data.Vertices.size() * sizeof(MeshVertex))));

		// The layout is the only thing telling GL how to read those bytes, and
		// it must match MeshVertex field for field.
		vertexBuffer->SetLayout({
			{ ShaderDataType::Float3, "a_Position" },
			{ ShaderDataType::Float3, "a_Normal"   },
			{ ShaderDataType::Float2, "a_TexCoord" }
		});
		m_VertexArray->AddVertexBuffer(vertexBuffer);

		std::shared_ptr<IndexBuffer> indexBuffer;
		indexBuffer.reset(IndexBuffer::Create((unsigned int*)data.Indices.data(),
			(unsigned int)data.Indices.size()));
		m_VertexArray->SetIndexBuffer(indexBuffer);
	}

	Mesh* Mesh::Load(const std::string& path)
	{
		MeshData data;
		std::string error;

		if (!ObjLoader::Load(path, data, error))
		{
			EGSS_CORE_ERROR("Mesh::Load failed: {0}", error);
			return nullptr;
		}

		// The file name alone, so a panel showing it stays readable.
		size_t slash = path.find_last_of("/\\");
		std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

		EGSS_CORE_INFO("Loaded '{0}': {1} vertices, {2} triangles",
			name, data.Vertices.size(), data.TriangleCount());

		return new Mesh(data, name);
	}

	Mesh* Mesh::CreateCube(float size)
	{
		float h = size * 0.5f;

		const glm::vec3 faceNormals[6] = {
			{  0,  0,  1 }, {  0,  0, -1 },
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 }
		};

		// Each face as four corners, wound counter-clockwise seen from outside.
		// Consistent winding is what makes back-face culling safe to switch on.
		const glm::vec3 faceCorners[6][4] = {
			{ {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} }, // +Z
			{ { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} }, // -Z
			{ { h,-h, h}, { h,-h,-h}, { h, h,-h}, { h, h, h} }, // +X
			{ {-h,-h,-h}, {-h,-h, h}, {-h, h, h}, {-h, h,-h} }, // -X
			{ {-h, h, h}, { h, h, h}, { h, h,-h}, {-h, h,-h} }, // +Y
			{ {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h} }  // -Y
		};

		const glm::vec2 uvs[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

		MeshData data;
		data.Vertices.reserve(24);
		data.Indices.reserve(36);

		for (int face = 0; face < 6; face++)
		{
			unsigned int base = (unsigned int)data.Vertices.size();

			for (int corner = 0; corner < 4; corner++)
				data.Vertices.push_back({ faceCorners[face][corner], faceNormals[face], uvs[corner] });

			data.Indices.insert(data.Indices.end(), {
				base + 0, base + 1, base + 2,
				base + 2, base + 3, base + 0
			});
		}

		data.RecalculateBounds();
		return new Mesh(data, "Cube");
	}

	Mesh* Mesh::CreatePlane(float size)
	{
		float h = size * 0.5f;

		MeshData data;
		data.Vertices = {
			{ {-h, 0.0f,  h }, { 0, 1, 0 }, { 0, 0 } },
			{ { h, 0.0f,  h }, { 0, 1, 0 }, { 1, 0 } },
			{ { h, 0.0f, -h }, { 0, 1, 0 }, { 1, 1 } },
			{ {-h, 0.0f, -h }, { 0, 1, 0 }, { 0, 1 } }
		};
		data.Indices = { 0, 1, 2, 2, 3, 0 };

		data.RecalculateBounds();
		return new Mesh(data, "Plane");
	}

	Mesh* Mesh::CreateSphere(float radius, unsigned int segments, unsigned int rings)
	{
		if (segments < 3) segments = 3;
		if (rings < 2) rings = 2;

		MeshData data;
		data.Vertices.reserve((size_t)(segments + 1) * (rings + 1));
		data.Indices.reserve((size_t)segments * rings * 6);

		// The seam gets a duplicated column of vertices: the same point on the
		// sphere needs u = 0 on one side and u = 1 on the other, and a vertex
		// carries one texture coordinate. Sharing it wraps the whole texture
		// backwards across the last column.
		for (unsigned int ring = 0; ring <= rings; ring++)
		{
			float v = (float)ring / (float)rings;
			float phi = v * glm::pi<float>();          // 0 at the top pole

			float y = std::cos(phi);
			float ringRadius = std::sin(phi);

			for (unsigned int segment = 0; segment <= segments; segment++)
			{
				float u = (float)segment / (float)segments;
				float theta = u * glm::two_pi<float>();

				glm::vec3 unit(ringRadius * std::cos(theta), y, ringRadius * std::sin(theta));

				// On a unit sphere centred at the origin the normal *is* the
				// position, which is the one case where no maths is needed.
				data.Vertices.push_back({ unit * radius, unit, { u, 1.0f - v } });
			}
		}

		unsigned int stride = segments + 1;
		for (unsigned int ring = 0; ring < rings; ring++)
		{
			for (unsigned int segment = 0; segment < segments; segment++)
			{
				unsigned int a = ring * stride + segment;
				unsigned int b = a + stride;

				// The polar rings are triangles, not quads -- every vertex in
				// the top row is the same point. Emitting the degenerate half
				// would cost draw work for zero pixels.
				if (ring != 0)
					data.Indices.insert(data.Indices.end(), { a, b, a + 1 });
				if (ring != rings - 1)
					data.Indices.insert(data.Indices.end(), { a + 1, b, b + 1 });
			}
		}

		data.RecalculateBounds();
		return new Mesh(data, "Sphere");
	}

}
