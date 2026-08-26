#include "egsspch.h"

#include "Egss/Voxel/MarchingTetrahedra.h"

namespace Egss {

	namespace {

		// The same corner numbering MarchingCubes uses, so the two can be read
		// against each other. y is up; 0-3 are the bottom face, 4-7 the top.
		const glm::ivec3 s_Corners[8] =
		{
			{ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
			{ 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 }
		};

		// Six tetrahedra, all sharing the cell's main diagonal from corner 0
		// (0,0,0) to corner 6 (1,1,1).
		//
		// These are the six *monotone paths* from one end of that diagonal to
		// the other -- at each step you may advance in x, y or z, and the 3! = 6
		// orders in which you can spend those three steps are the six tets. That
		// derivation is why the set is exactly right: every path visits four
		// distinct corners, no two paths visit the same set, and together they
		// account for all six of the cell's volume without overlap.
		//
		// It is also why neighbouring cells agree. The decomposition is defined
		// by the diagonal direction alone, which every cell shares, so two cells
		// meeting at a face divide that face with the same diagonal.
		const int s_Tetrahedra[6][4] =
		{
			{ 0, 1, 5, 6 },   // x, y, z
			{ 0, 1, 2, 6 },   // x, z, y
			{ 0, 4, 5, 6 },   // y, x, z
			{ 0, 4, 7, 6 },   // y, z, x
			{ 0, 3, 2, 6 },   // z, x, y
			{ 0, 3, 7, 6 },   // z, y, x
		};

		glm::vec3 Crossing(const glm::vec3& pa, float va, const glm::vec3& pb, float vb)
		{
			float d = va - vb;
			float t = std::fabs(d) > 1e-12f ? va / d : 0.5f;

			return pa + (pb - pa) * glm::clamp(t, 0.0f, 1.0f);
		}

		void Emit(MeshData& data, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
			const glm::vec3& outward)
		{
			glm::vec3 n = glm::cross(b - a, c - a);

			float length = glm::length(n);
			if (length < 1e-12f)
				return;   // degenerate: a crossing landed exactly on a corner

			n /= length;

			// Wound so the normal points out of the solid. Which way that is
			// comes from the corners themselves -- from the negative ones toward
			// the positive ones -- rather than from an assumption about the
			// order the crossings came out in.
			glm::vec3 first = a, second = b, third = c;
			if (glm::dot(n, outward) < 0.0f)
			{
				std::swap(second, third);
				n = -n;
			}

			unsigned int at = (unsigned int)data.Vertices.size();
			data.Vertices.push_back({ first,  n, { 0.0f, 0.0f } });
			data.Vertices.push_back({ second, n, { 1.0f, 0.0f } });
			data.Vertices.push_back({ third,  n, { 0.5f, 1.0f } });

			data.Indices.push_back(at);
			data.Indices.push_back(at + 1);
			data.Indices.push_back(at + 2);
		}

	}

	const int (*MarchingTetrahedra::CellTetrahedra())[4]
	{
		return s_Tetrahedra;
	}

	void MarchingTetrahedra::Cell(const glm::vec3 position[4], const float value[4], MeshData& data)
	{
		// Solid is negative, the same as VoxelField3D::Solid.
		int inside[4], outside[4];
		int insideCount = 0, outsideCount = 0;

		for (int i = 0; i < 4; i++)
		{
			if (value[i] < 0.0f)
				inside[insideCount++] = i;
			else
				outside[outsideCount++] = i;
		}

		if (insideCount == 0 || insideCount == 4)
			return;

		// Which way is out, from the corners rather than from a convention
		// about the order the crossings are generated in.
		glm::vec3 insideCentre(0.0f), outsideCentre(0.0f);
		for (int i = 0; i < insideCount; i++)
			insideCentre += position[inside[i]];
		for (int i = 0; i < outsideCount; i++)
			outsideCentre += position[outside[i]];

		insideCentre /= (float)insideCount;
		outsideCentre /= (float)outsideCount;

		glm::vec3 outward = outsideCentre - insideCentre;

		// One corner alone on its side: the surface cuts the three edges that
		// meet at it, and one triangle joins those three crossings. There is no
		// other way to separate one corner from three, which is why this case
		// cannot be ambiguous.
		if (insideCount == 1 || outsideCount == 1)
		{
			int lone = (insideCount == 1) ? inside[0] : outside[0];

			const int* others = (insideCount == 1) ? outside : inside;
			int otherCount = (insideCount == 1) ? outsideCount : insideCount;

			glm::vec3 p[3];
			for (int i = 0; i < otherCount; i++)
				p[i] = Crossing(position[lone], value[lone],
					position[others[i]], value[others[i]]);

			Emit(data, p[0], p[1], p[2], outward);
			return;
		}

		// Two and two: the surface cuts the four edges joining an inside corner
		// to an outside one, and those four crossings form a quad.
		//
		// The order matters -- taken in the wrong order the four points make a
		// bowtie rather than a quad. Walking i0-o0, i0-o1, i1-o1, i1-o0 gives a
		// closed loop, because consecutive entries always share a corner and so
		// lie on a common face of the tetrahedron.
		glm::vec3 q0 = Crossing(position[inside[0]], value[inside[0]],
			position[outside[0]], value[outside[0]]);
		glm::vec3 q1 = Crossing(position[inside[0]], value[inside[0]],
			position[outside[1]], value[outside[1]]);
		glm::vec3 q2 = Crossing(position[inside[1]], value[inside[1]],
			position[outside[1]], value[outside[1]]);
		glm::vec3 q3 = Crossing(position[inside[1]], value[inside[1]],
			position[outside[0]], value[outside[0]]);

		Emit(data, q0, q1, q2, outward);
		Emit(data, q0, q2, q3, outward);
	}

	MeshData MarchingTetrahedra::Mesh(const VoxelField3D& field,
		const glm::ivec3& min, const glm::ivec3& max, int stride,
		const glm::ivec3* about)
	{
		MeshData data;

		if (field.Empty())
			return data;

		stride = glm::max(stride, 1);

		glm::ivec3 from = glm::max(min, glm::ivec3(0));
		glm::ivec3 to = glm::min(max, field.Size() - glm::ivec3(1));

		for (int z = from.z; z + stride <= to.z; z += stride)
		{
			for (int y = from.y; y + stride <= to.y; y += stride)
			{
				for (int x = from.x; x + stride <= to.x; x += stride)
				{
					glm::vec3 corner[8];
					float value[8];

					for (int c = 0; c < 8; c++)
					{
						glm::ivec3 at = glm::ivec3(x, y, z) + s_Corners[c] * stride;

						corner[c] = about
							? field.PositionFrom(at.x, at.y, at.z, *about)
							: field.PositionOf(at.x, at.y, at.z);
						value[c] = field.DistanceAt(at.x, at.y, at.z);
					}

					for (const int* tet : s_Tetrahedra)
					{
						glm::vec3 p[4] = { corner[tet[0]], corner[tet[1]], corner[tet[2]], corner[tet[3]] };
						float v[4] = { value[tet[0]], value[tet[1]], value[tet[2]], value[tet[3]] };

						Cell(p, v, data);
					}
				}
			}
		}

		if (!data.Indices.empty())
		{
			Submesh all;
			all.IndexCount = (unsigned int)data.Indices.size();
			data.Submeshes.push_back(all);
			data.RecalculateBounds();
		}

		return data;
	}

}
