#include "egsspch.h"

#include "Egss/Voxel/VoxelTransition.h"
#include "Egss/Voxel/MarchingTetrahedra.h"

namespace Egss {

	namespace {

		// The same corner numbering MarchingCubes and MarchingTetrahedra use.
		const glm::ivec3 s_Corners[8] =
		{
			{ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
			{ 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 }
		};

		// Which corners bound each of the cube's six faces. Membership, not
		// order, is what a caller reads out of this -- the order a tet's
		// corners land in `shared` below comes from the tet table, not this.
		const unsigned int s_FaceCorners[6] =
		{
			(1u << 1) | (1u << 2) | (1u << 5) | (1u << 6),   // PosX
			(1u << 0) | (1u << 3) | (1u << 4) | (1u << 7),   // NegX
			(1u << 4) | (1u << 5) | (1u << 6) | (1u << 7),   // PosY
			(1u << 0) | (1u << 1) | (1u << 2) | (1u << 3),   // NegY
			(1u << 2) | (1u << 3) | (1u << 6) | (1u << 7),   // PosZ
			(1u << 0) | (1u << 1) | (1u << 4) | (1u << 5),   // NegZ
		};

		VoxelTransition::LatticePoint SampleAt(const VoxelField3D& field,
			const glm::ivec3& lattice, const glm::ivec3* about)
		{
			VoxelTransition::LatticePoint p;
			p.Lattice = lattice;
			p.Position = about
				? field.PositionFrom(lattice.x, lattice.y, lattice.z, *about)
				: field.PositionOf(lattice.x, lattice.y, lattice.z);
			p.Value = field.DistanceAt(lattice.x, lattice.y, lattice.z);
			return p;
		}

	}

	void VoxelTransition::SubdivideEdge(const VoxelField3D& field,
		const glm::ivec3& a, const glm::ivec3& b, int depth,
		std::vector<LatticePoint>& out, const glm::ivec3* about)
	{
		int n = 1 << depth;
		out.resize(n + 1);

		// b - a divides evenly by n on every axis whenever depth came from
		// log2(coarseStride / fineStride), which is the only way this is
		// ever called -- integer division is therefore exact, not truncating.
		glm::ivec3 step = (b - a) / n;

		for (int i = 0; i <= n; i++)
			out[i] = SampleAt(field, a + step * i, about);
	}

	void VoxelTransition::SubdivideFace(const VoxelField3D& field,
		const glm::ivec3& a, const glm::ivec3& b, const glm::ivec3& c,
		int depth, std::vector<LatticePoint>& out, const glm::ivec3* about)
	{
		int n = 1 << depth;
		out.resize((n + 1) * (n + 2) / 2);

		// The three boundary rows, each a direct call to SubdivideEdge so a
		// neighbouring tet subdividing the same physical edge -- as its own
		// marked collar edge, or as one boundary of its own quadrisected cap
		// face -- reads bit-identical points, not merely arithmetically
		// equivalent ones.
		std::vector<LatticePoint> edgeAB, edgeAC, edgeBC;
		SubdivideEdge(field, a, b, depth, edgeAB, about);   // v = 0
		SubdivideEdge(field, a, c, depth, edgeAC, about);   // u = 0
		SubdivideEdge(field, b, c, depth, edgeBC, about);   // u + v = n, by v

		glm::ivec3 stepU = (b - a) / n;
		glm::ivec3 stepV = (c - a) / n;

		for (int v = 0; v <= n; v++)
		{
			for (int u = 0; u <= n - v; u++)
			{
				LatticePoint p;

				if (v == 0)
					p = edgeAB[u];
				else if (u == 0)
					p = edgeAC[v];
				else if (u + v == n)
					p = edgeBC[v];
				else
					p = SampleAt(field, a + stepU * u + stepV * v, about);

				out[FaceIndex(u, v, n)] = p;
			}
		}
	}

	void VoxelTransition::Cell(const VoxelField3D& field, const glm::ivec3& origin,
		int stride, unsigned int boundaryMask, int ratio, MeshData& data,
		const glm::ivec3* about)
	{
		glm::ivec3 lattice[8];
		glm::vec3 pos[8];
		float val[8];

		for (int c = 0; c < 8; c++)
		{
			lattice[c] = origin + s_Corners[c] * stride;
			pos[c] = about
				? field.PositionFrom(lattice[c].x, lattice[c].y, lattice[c].z, *about)
				: field.PositionOf(lattice[c].x, lattice[c].y, lattice[c].z);
			val[c] = field.DistanceAt(lattice[c].x, lattice[c].y, lattice[c].z);
		}

		const int (*tets)[4] = MarchingTetrahedra::CellTetrahedra();

		// Exactly one bit set, or fall back to a plain cell -- see the class
		// comment's "out of scope" note for why zero or several is not
		// handled here.
		bool onePlane = boundaryMask != 0 && (boundaryMask & (boundaryMask - 1)) == 0;

		int face = -1;
		if (onePlane)
		{
			for (int b = 0; b < 6; b++)
			{
				if (boundaryMask & (1u << b))
				{
					face = b;
					break;
				}
			}
		}

		if (face < 0)
		{
			for (int t = 0; t < 6; t++)
			{
				glm::vec3 p[4] = { pos[tets[t][0]], pos[tets[t][1]], pos[tets[t][2]], pos[tets[t][3]] };
				float v[4] = { val[tets[t][0]], val[tets[t][1]], val[tets[t][2]], val[tets[t][3]] };
				MarchingTetrahedra::Cell(p, v, data);
			}
			return;
		}

		unsigned int faceCorners = s_FaceCorners[face];

		int depth = 0;
		while ((1 << depth) < ratio)
			depth++;

		int n = 1 << depth;

		for (int t = 0; t < 6; t++)
		{
			int shared[4], sharedCount = 0;
			int other[4], otherCount = 0;

			for (int k = 0; k < 4; k++)
			{
				int corner = tets[t][k];
				if (faceCorners & (1u << corner))
					shared[sharedCount++] = corner;
				else
					other[otherCount++] = corner;
			}

			if (sharedCount == 3)
			{
				// Cap tet: `shared` is the face lying on the boundary,
				// `other[0]` its one apex.
				int apex = other[0];

				std::vector<LatticePoint> facePoints;
				SubdivideFace(field, lattice[shared[0]], lattice[shared[1]],
					lattice[shared[2]], depth, facePoints, about);

				for (int v = 0; v < n; v++)
				{
					for (int u = 0; u < n - v; u++)
					{
						const LatticePoint& p00 = facePoints[FaceIndex(u, v, n)];
						const LatticePoint& p10 = facePoints[FaceIndex(u + 1, v, n)];
						const LatticePoint& p01 = facePoints[FaceIndex(u, v + 1, n)];

						glm::vec3 tp[4] = { pos[apex], p00.Position, p10.Position, p01.Position };
						float tv[4] = { val[apex], p00.Value, p10.Value, p01.Value };
						MarchingTetrahedra::Cell(tp, tv, data);

						if (u + v < n - 1)
						{
							const LatticePoint& p11 = facePoints[FaceIndex(u + 1, v + 1, n)];

							glm::vec3 tp2[4] = { pos[apex], p10.Position, p11.Position, p01.Position };
							float tv2[4] = { val[apex], p10.Value, p11.Value, p01.Value };
							MarchingTetrahedra::Cell(tp2, tv2, data);
						}
					}
				}
			}
			else if (sharedCount == 2)
			{
				// Collar tet: `shared` is the one edge lying on the
				// boundary, `other` its two off-edge corners.
				std::vector<LatticePoint> edgePoints;
				SubdivideEdge(field, lattice[shared[0]], lattice[shared[1]],
					depth, edgePoints, about);

				for (size_t i = 0; i + 1 < edgePoints.size(); i++)
				{
					glm::vec3 tp[4] = { pos[other[0]], pos[other[1]], edgePoints[i].Position, edgePoints[i + 1].Position };
					float tv[4] = { val[other[0]], val[other[1]], edgePoints[i].Value, edgePoints[i + 1].Value };
					MarchingTetrahedra::Cell(tp, tv, data);
				}
			}
			else
			{
				// Untouched: meets the boundary face at one corner at most.
				glm::vec3 p[4] = { pos[tets[t][0]], pos[tets[t][1]], pos[tets[t][2]], pos[tets[t][3]] };
				float v[4] = { val[tets[t][0]], val[tets[t][1]], val[tets[t][2]], val[tets[t][3]] };
				MarchingTetrahedra::Cell(p, v, data);
			}
		}
	}

	void VoxelTransition::MeshBoundaryLayer(const VoxelField3D& field,
		const glm::ivec3& min, const glm::ivec3& max,
		int stride, unsigned int boundaryMask, int ratio, MeshData& data,
		const glm::ivec3* about)
	{
		glm::ivec3 from = glm::max(min, glm::ivec3(0));
		glm::ivec3 to = glm::min(max, field.Size() - glm::ivec3(1));

		for (int z = from.z; z < to.z; z += stride)
			for (int y = from.y; y < to.y; y += stride)
				for (int x = from.x; x < to.x; x += stride)
					Cell(field, { x, y, z }, stride, boundaryMask, ratio, data, about);
	}

}
