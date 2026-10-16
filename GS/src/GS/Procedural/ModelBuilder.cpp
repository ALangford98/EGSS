#include "gspch.h"
#include "GS/Procedural/ModelBuilder.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GS {

	ModelBuilder::ModelBuilder()
	{
		m_Stack.push_back(glm::mat4(1.0f));
		m_Groups.push_back({ "default", {}, {} });
	}

	void ModelBuilder::Material(const std::string& name)
	{
		for (size_t i = 0; i < m_Groups.size(); i++)
			if (m_Groups[i].Name == name)
			{
				m_Current = (int)i;
				return;
			}
		m_Groups.push_back({ name, {}, {} });
		m_Current = (int)m_Groups.size() - 1;
	}

	void ModelBuilder::Push(const glm::mat4& transform) { m_Stack.push_back(m_Stack.back() * transform); }
	void ModelBuilder::Pop() { if (m_Stack.size() > 1) m_Stack.pop_back(); }

	glm::mat4 ModelBuilder::At(const glm::vec3& position) { return glm::translate(glm::mat4(1.0f), position); }

	glm::mat4 ModelBuilder::At(const glm::vec3& position, const glm::vec3& eulerDegrees)
	{
		glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
		m = glm::rotate(m, glm::radians(eulerDegrees.y), glm::vec3(0, 1, 0));
		m = glm::rotate(m, glm::radians(eulerDegrees.x), glm::vec3(1, 0, 0));
		m = glm::rotate(m, glm::radians(eulerDegrees.z), glm::vec3(0, 0, 1));
		return m;
	}

	ModelBuilder::Group& ModelBuilder::Current() { return m_Groups[m_Current]; }

	unsigned int ModelBuilder::Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& uv)
	{
		const glm::mat4& m = m_Stack.back();
		glm::vec3 p = glm::vec3(m * glm::vec4(position, 1.0f));
		// The inverse transpose, so a squashed part keeps normals that are
		// still perpendicular to its surface.
		glm::vec3 n = glm::mat3(glm::transpose(glm::inverse(m))) * normal;
		float length = glm::length(n);
		n = length > 1e-12f ? n / length : glm::vec3(0.0f, 1.0f, 0.0f);
		Group& group = Current();
		group.Vertices.push_back({ p, n, uv });
		return (unsigned int)group.Vertices.size() - 1;
	}

	unsigned int ModelBuilder::Vertex(const glm::vec3& position, const glm::vec3& normal)
	{
		// Box projection, by whichever axis the surface faces most: enough
		// for a procedural material to tile across a part at a constant
		// scale, which is all these models ask of a UV.
		glm::vec3 a = glm::abs(normal);
		glm::vec2 uv = a.x >= a.y && a.x >= a.z ? glm::vec2(position.z, position.y)
			: a.y >= a.z ? glm::vec2(position.x, position.z) : glm::vec2(position.x, position.y);
		return Vertex(position, normal, uv);
	}

	void ModelBuilder::Triangle(unsigned int a, unsigned int b, unsigned int c)
	{
		// A mirroring transform turns the winding inside out; undone here so
		// a part built once can be placed on the left and the right.
		Group& group = Current();
		if (glm::determinant(glm::mat3(m_Stack.back())) < 0.0f)
			std::swap(b, c);
		group.Indices.insert(group.Indices.end(), { a, b, c });
	}

	void ModelBuilder::QuadIndices(unsigned int a, unsigned int b, unsigned int c, unsigned int d)
	{
		Triangle(a, b, c);
		Triangle(a, c, d);
	}

	void ModelBuilder::RoundedBox(const glm::vec3& half, float radius, int segments)
	{
		radius = std::max(0.0f, std::min(radius, std::min({ half.x, half.y, half.z })));
		segments = std::max(1, segments);
		glm::vec3 inner = half - glm::vec3(radius);

		// Along one axis: the rounded band from -h to -inner, the flat
		// stretch, then the band to +h. The band is sampled at equal angles
		// -- p = inner + r tan(theta), theta up to 45 degrees on this face, the
		// other 45 belonging to the neighbouring one -- so the curve is even.
		auto coordinates = [&](int axis)
		{
			std::vector<float> list;
			for (int j = 0; j <= segments; j++)
				list.push_back(-(inner[axis] + radius * std::tan(glm::quarter_pi<float>() * (float)(segments - j) / segments)));
			for (int j = 0; j <= segments; j++)
				list.push_back(inner[axis] + radius * std::tan(glm::quarter_pi<float>() * (float)j / segments));
			return list;
		};

		for (int a = 0; a < 3; a++)
			for (float sign : { 1.0f, -1.0f })
			{
				int u = (a + 1) % 3, v = (a + 2) % 3;
				if (sign < 0.0f)
					std::swap(u, v);   // keeps u x v along the face's normal
				std::vector<float> us = coordinates(u), vs = coordinates(v);
				unsigned int first = (unsigned int)Current().Vertices.size();
				for (float pv : vs)
					for (float pu : us)
					{
						glm::vec3 p;
						p[a] = sign * half[a];
						p[u] = pu;
						p[v] = pv;
						glm::vec3 q = glm::clamp(p, -inner, inner);
						glm::vec3 d = p - q;
						float length = glm::length(d);
						glm::vec3 n = length > 1e-9f ? d / length : glm::vec3(0.0f);
						if (length <= 1e-9f)
							n[a] = sign;
						Vertex(q + n * radius, n);
					}
				unsigned int row = (unsigned int)us.size();
				for (unsigned int j = 0; j + 1 < vs.size(); j++)
					for (unsigned int i = 0; i + 1 < row; i++)
					{
						unsigned int i0 = first + j * row + i;
						QuadIndices(i0, i0 + 1, i0 + row + 1, i0 + row);
					}
			}
	}

	void ModelBuilder::Lathe(const std::vector<glm::vec2>& profile, int segments, float creaseDegrees, float sweepDegrees)
	{
		if (profile.size() < 2)
			return;
		segments = std::max(3, segments);
		float sweep = glm::radians(sweepDegrees);
		float creaseCos = std::cos(glm::radians(creaseDegrees));

		// Each profile segment's outward normal in the (r, y) plane: the
		// tangent turned clockwise.
		std::vector<glm::vec2> normals;
		for (size_t k = 0; k + 1 < profile.size(); k++)
		{
			glm::vec2 t = profile[k + 1] - profile[k];
			float length = glm::length(t);
			normals.push_back(length > 1e-9f ? glm::vec2(t.y, -t.x) / length : glm::vec2(1.0f, 0.0f));
		}

		float along = 0.0f;
		for (size_t k = 0; k + 1 < profile.size(); k++)
		{
			// An end shares the neighbouring segment's normal, averaged, unless
			// the profile turns there by more than the crease.
			auto endNormal = [&](size_t at, int neighbour)
			{
				glm::vec2 own = normals[k];
				if (neighbour < 0 || neighbour >= (int)normals.size())
					return own;
				glm::vec2 other = normals[(size_t)neighbour];
				if (glm::dot(own, other) < creaseCos)
					return own;
				return glm::normalize(own + other);
			};
			glm::vec2 n0 = endNormal(k, (int)k - 1);
			glm::vec2 n1 = endNormal(k + 1, (int)k + 1);
			float length = glm::length(profile[k + 1] - profile[k]);

			unsigned int first = (unsigned int)Current().Vertices.size();
			for (int end = 0; end < 2; end++)
			{
				const glm::vec2& p = profile[k + end];
				const glm::vec2& n = end == 0 ? n0 : n1;
				for (int s = 0; s <= segments; s++)
				{
					float phi = sweep * (float)s / segments;
					float sn = std::sin(phi), cs = std::cos(phi);
					Vertex(glm::vec3(p.x * sn, p.y, p.x * cs), glm::vec3(n.x * sn, n.y, n.x * cs),
						glm::vec2((float)s / segments, along + (end ? length : 0.0f)));
				}
			}
			unsigned int row = (unsigned int)segments + 1;
			for (unsigned int s = 0; s < (unsigned int)segments; s++)
				QuadIndices(first + s, first + s + 1, first + row + s + 1, first + row + s);
			along += length;
		}
	}

	void ModelBuilder::Cylinder(float radius, float halfHeight, int segments, float chamfer)
	{
		chamfer = std::min(chamfer, std::min(radius, halfHeight) * 0.9f);
		if (chamfer > 0.0f)
			Lathe({ { 0.0f, -halfHeight }, { radius - chamfer, -halfHeight }, { radius, -halfHeight + chamfer },
				{ radius, halfHeight - chamfer }, { radius - chamfer, halfHeight }, { 0.0f, halfHeight } }, segments, 30.0f);
		else
			Lathe({ { 0.0f, -halfHeight }, { radius, -halfHeight }, { radius, halfHeight }, { 0.0f, halfHeight } }, segments, 30.0f);
	}

	void ModelBuilder::Sphere(float radius, int segments, int rings)
	{
		std::vector<glm::vec2> profile;
		for (int k = 0; k <= rings; k++)
		{
			float theta = glm::pi<float>() * (float)k / rings;
			profile.push_back({ radius * std::sin(theta), -radius * std::cos(theta) });
		}
		Lathe(profile, segments, 180.0f);
	}

	void ModelBuilder::Torus(float major, float minor, int majorSegments, int minorSegments)
	{
		std::vector<glm::vec2> profile;
		for (int k = 0; k <= minorSegments; k++)
		{
			float a = -glm::pi<float>() + glm::two_pi<float>() * (float)k / minorSegments;
			profile.push_back({ major + minor * std::cos(a), minor * std::sin(a) });
		}
		Lathe(profile, majorSegments, 180.0f);
	}

	void ModelBuilder::Tube(const std::vector<glm::vec3>& path, float radius, int segments, bool caps)
	{
		if (path.size() < 2)
			return;
		segments = std::max(3, segments);
		size_t count = path.size();
		std::vector<glm::vec3> tangents(count);
		for (size_t k = 0; k < count; k++)
		{
			glm::vec3 a = path[k > 0 ? k - 1 : k], b = path[k + 1 < count ? k + 1 : k];
			tangents[k] = glm::normalize(b - a);
		}

		// A first normal perpendicular to the first tangent, then carried
		// along: each one turned by the rotation that takes the last tangent
		// to the next -- parallel transport, so the tube does not corkscrew.
		glm::vec3 up = std::abs(tangents[0].y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
		glm::vec3 normal = glm::normalize(glm::cross(glm::cross(tangents[0], up), tangents[0]));
		std::vector<glm::vec3> normals(count);
		normals[0] = normal;
		for (size_t k = 1; k < count; k++)
		{
			glm::vec3 axis = glm::cross(tangents[k - 1], tangents[k]);
			float s = glm::length(axis);
			if (s > 1e-6f)
			{
				float angle = std::atan2(s, glm::dot(tangents[k - 1], tangents[k]));
				normal = glm::angleAxis(angle, axis / s) * normal;
			}
			normal = glm::normalize(normal - tangents[k] * glm::dot(normal, tangents[k]));
			normals[k] = normal;
		}

		unsigned int first = (unsigned int)Current().Vertices.size();
		unsigned int row = (unsigned int)segments + 1;
		float along = 0.0f;
		for (size_t k = 0; k < count; k++)
		{
			if (k > 0)
				along += glm::length(path[k] - path[k - 1]);
			glm::vec3 b = glm::cross(tangents[k], normals[k]);
			for (int s = 0; s <= segments; s++)
			{
				float theta = glm::two_pi<float>() * (float)s / segments;
				glm::vec3 n = normals[k] * std::cos(theta) + b * std::sin(theta);
				Vertex(path[k] + n * radius, n, glm::vec2((float)s / segments, along));
			}
		}
		for (unsigned int k = 0; k + 1 < (unsigned int)count; k++)
			for (unsigned int s = 0; s < (unsigned int)segments; s++)
			{
				unsigned int i0 = first + k * row + s;
				QuadIndices(i0, i0 + 1, i0 + row + 1, i0 + row);
			}

		if (!caps)
			return;
		for (int end = 0; end < 2; end++)
		{
			size_t k = end == 0 ? 0 : count - 1;
			glm::vec3 n = end == 0 ? -tangents[k] : tangents[k];
			glm::vec3 b = glm::cross(tangents[k], normals[k]);
			unsigned int centre = Vertex(path[k], n);
			unsigned int ring = (unsigned int)Current().Vertices.size();
			for (int s = 0; s <= segments; s++)
			{
				float theta = glm::two_pi<float>() * (float)s / segments;
				Vertex(path[k] + (normals[k] * std::cos(theta) + b * std::sin(theta)) * radius, n);
			}
			for (unsigned int s = 0; s < (unsigned int)segments; s++)
				if (end == 0)
					Triangle(centre, ring + s + 1, ring + s);
				else
					Triangle(centre, ring + s, ring + s + 1);
		}
	}

	void ModelBuilder::Extrude(const std::vector<glm::vec2>& outline, float depth)
	{
		size_t count = outline.size();
		if (count < 3)
			return;
		float d = depth * 0.5f;
		for (size_t i = 0; i < count; i++)
		{
			glm::vec2 a = outline[i], b = outline[(i + 1) % count];
			glm::vec2 e = b - a;
			float length = glm::length(e);
			if (length < 1e-9f)
				continue;
			glm::vec3 n(e.y / length, -e.x / length, 0.0f);
			unsigned int v0 = Vertex({ a, -d }, n), v1 = Vertex({ b, -d }, n);
			unsigned int v2 = Vertex({ b, d }, n), v3 = Vertex({ a, d }, n);
			QuadIndices(v0, v1, v2, v3);
		}
		// The caps fan from the outline's centroid rather than its first
		// point, so a profile that is only star-shaped -- a crash barrier's,
		// which pinches in above its foot -- still caps without overlaps.
		glm::vec2 centroid(0.0f);
		for (const glm::vec2& p : outline)
			centroid += p;
		centroid /= (float)count;
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 n(0.0f, 0.0f, side == 0 ? 1.0f : -1.0f);
			float z = side == 0 ? d : -d;
			unsigned int centre = Vertex({ centroid, z }, n);
			unsigned int first = (unsigned int)Current().Vertices.size();
			for (const glm::vec2& p : outline)
				Vertex({ p, z }, n);
			for (unsigned int i = 0; i < (unsigned int)count; i++)
			{
				unsigned int a = first + i, b = first + (i + 1) % (unsigned int)count;
				if (side == 0)
					Triangle(centre, a, b);
				else
					Triangle(centre, b, a);
			}
		}
	}

	void ModelBuilder::Sector(float radius, float fromDegrees, float toDegrees, int segments)
	{
		segments = std::max(1, segments);
		glm::vec3 n(0.0f, 0.0f, 1.0f);
		unsigned int centre = Vertex(glm::vec3(0.0f), n, glm::vec2(0.5f));
		unsigned int first = (unsigned int)Current().Vertices.size();
		for (int s = 0; s <= segments; s++)
		{
			float a = glm::radians(fromDegrees + (toDegrees - fromDegrees) * (float)s / segments);
			Vertex(glm::vec3(std::cos(a), std::sin(a), 0.0f) * radius, n,
				glm::vec2(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)));
		}
		for (unsigned int s = 0; s < (unsigned int)segments; s++)
			Triangle(centre, first + s, first + s + 1);
	}

	void ModelBuilder::Quad(const glm::vec2& half)
	{
		glm::vec3 n(0.0f, 0.0f, 1.0f);
		unsigned int a = Vertex({ -half.x, -half.y, 0.0f }, n, { 0.0f, 0.0f });
		unsigned int b = Vertex({ half.x, -half.y, 0.0f }, n, { 1.0f, 0.0f });
		unsigned int c = Vertex({ half.x, half.y, 0.0f }, n, { 1.0f, 1.0f });
		unsigned int d = Vertex({ -half.x, half.y, 0.0f }, n, { 0.0f, 1.0f });
		QuadIndices(a, b, c, d);
	}

	MeshData ModelBuilder::Build() const
	{
		MeshData data;
		for (const Group& group : m_Groups)
		{
			if (group.Indices.empty())
				continue;
			unsigned int base = (unsigned int)data.Vertices.size();
			Submesh submesh;
			submesh.Material = group.Name;
			submesh.FirstIndex = (unsigned int)data.Indices.size();
			submesh.IndexCount = (unsigned int)group.Indices.size();
			data.Vertices.insert(data.Vertices.end(), group.Vertices.begin(), group.Vertices.end());
			for (unsigned int index : group.Indices)
				data.Indices.push_back(base + index);
			data.Submeshes.push_back(submesh);
		}
		data.RecalculateBounds();
		return data;
	}

}
