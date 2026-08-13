#include "egsspch.h"
#include "Egss/Voxel/VoxelIslands.h"

namespace Egss {

	namespace {

		const glm::ivec3 s_Neighbours[6] =
		{
			{ 1, 0, 0 }, { -1, 0, 0 },
			{ 0, 1, 0 }, { 0, -1, 0 },
			{ 0, 0, 1 }, { 0, 0, -1 }
		};

		// The principal axes of a set of points, by Jacobi rotation of their
		// covariance matrix.
		//
		// A 3x3 symmetric matrix is diagonalised by repeatedly rotating away its
		// largest off-diagonal entry; the accumulated rotation's columns are the
		// eigenvectors. Six sweeps is far past convergence for a matrix this
		// small, and the whole thing is forty lines -- which is why this is here
		// rather than pulling in a linear algebra library for one function.
		glm::mat3 PrincipalAxes(const glm::dmat3& covariance)
		{
			glm::dmat3 a = covariance;
			glm::dmat3 v(1.0);

			for (int sweep = 0; sweep < 24; sweep++)
			{
				// The largest off-diagonal entry, which is the one worth zeroing.
				int p = 0, q = 1;
				double largest = std::fabs(a[1][0]);

				if (std::fabs(a[2][0]) > largest) { largest = std::fabs(a[2][0]); p = 0; q = 2; }
				if (std::fabs(a[2][1]) > largest) { largest = std::fabs(a[2][1]); p = 1; q = 2; }

				if (largest < 1e-12)
					break;

				double theta = (a[q][q] - a[p][p]) / (2.0 * a[q][p]);
				double t = (theta >= 0.0 ? 1.0 : -1.0)
					/ (std::fabs(theta) + std::sqrt(theta * theta + 1.0));

				double c = 1.0 / std::sqrt(t * t + 1.0);
				double s = t * c;

				glm::dmat3 rotation(1.0);
				rotation[p][p] = c;  rotation[q][q] = c;
				rotation[q][p] = s;  rotation[p][q] = -s;

				a = glm::transpose(rotation) * a * rotation;
				v = v * rotation;
			}

			return glm::mat3(v);
		}

	}

	std::vector<VoxelIsland> VoxelIslands::Find(const VoxelField3D& field,
		int anchorHeight, size_t minVoxels)
	{
		std::vector<VoxelIsland> islands;

		if (field.Empty())
			return islands;

		const glm::ivec3 size = field.Size();
		const size_t count = (size_t)size.x * size.y * size.z;

		auto indexOf = [&](int x, int y, int z)
		{
			return ((size_t)z * size.y + y) * size.x + x;
		};

		// One byte per lattice point rather than a set: a 128^3 field is 2 MB
		// here and the alternative is a hash lookup per neighbour test, six
		// times per voxel.
		std::vector<unsigned char> visited(count, 0);
		std::vector<glm::ivec3> stack;

		// --- Anchored first ------------------------------------------------
		// Everything reachable from bedrock, marked and then never looked at
		// again. What remains unmarked and solid is, by definition, floating.
		for (int z = 0; z < size.z; z++)
		{
			for (int y = 0; y <= glm::min(anchorHeight, size.y - 1); y++)
			{
				for (int x = 0; x < size.x; x++)
				{
					if (!field.Solid(x, y, z) || visited[indexOf(x, y, z)])
						continue;

					visited[indexOf(x, y, z)] = 1;
					stack.push_back({ x, y, z });

					while (!stack.empty())
					{
						glm::ivec3 at = stack.back();
						stack.pop_back();

						for (const glm::ivec3& offset : s_Neighbours)
						{
							glm::ivec3 next = at + offset;

							if (!field.Contains(next.x, next.y, next.z))
								continue;
							if (visited[indexOf(next.x, next.y, next.z)])
								continue;
							if (!field.Solid(next.x, next.y, next.z))
								continue;

							visited[indexOf(next.x, next.y, next.z)] = 1;
							stack.push_back(next);
						}
					}
				}
			}
		}

		// --- Then whatever is left ------------------------------------------
		const float voxel = field.VoxelSize();
		const float volumePerVoxel = voxel * voxel * voxel;

		for (int z = 0; z < size.z; z++)
		{
			for (int y = 0; y < size.y; y++)
			{
				for (int x = 0; x < size.x; x++)
				{
					if (!field.Solid(x, y, z) || visited[indexOf(x, y, z)])
						continue;

					VoxelIsland island;
					island.Min = { x, y, z };
					island.Max = { x, y, z };

					visited[indexOf(x, y, z)] = 1;
					stack.push_back({ x, y, z });

					glm::dvec3 sum(0.0);

					while (!stack.empty())
					{
						glm::ivec3 at = stack.back();
						stack.pop_back();

						island.Voxels.push_back(at);
						island.Min = glm::min(island.Min, at);
						island.Max = glm::max(island.Max, at);
						sum += glm::dvec3(field.PositionOf(at.x, at.y, at.z));

						for (const glm::ivec3& offset : s_Neighbours)
						{
							glm::ivec3 next = at + offset;

							if (!field.Contains(next.x, next.y, next.z))
								continue;
							if (visited[indexOf(next.x, next.y, next.z)])
								continue;
							if (!field.Solid(next.x, next.y, next.z))
								continue;

							visited[indexOf(next.x, next.y, next.z)] = 1;
							stack.push_back(next);
						}
					}

					if (island.Voxels.size() < minVoxels)
						continue;

					island.Centre = glm::vec3(sum / (double)island.Voxels.size());
					island.Volume = (float)island.Voxels.size() * volumePerVoxel;

					// The solid *cells*, not the lattice points: a voxel at a
					// lattice point stands for the material around it, so the
					// piece reaches half a voxel past its outermost sample.
					island.WorldMin = field.PositionOf(island.Min.x, island.Min.y, island.Min.z)
						- glm::vec3(voxel * 0.5f);
					island.WorldMax = field.PositionOf(island.Max.x, island.Max.y, island.Max.z)
						+ glm::vec3(voxel * 0.5f);

					// --- The oriented box -----------------------------------
					//
					// Covariance of the voxel positions about their centre, then
					// its principal axes. The rotation is orthonormal by
					// construction, so it is a valid orientation without any
					// fixing up -- except for handedness, since a Jacobi sweep
					// can hand back a reflection, and a body built from one is
					// inside out.
					glm::dmat3 covariance(0.0);

					for (const glm::ivec3& at : island.Voxels)
					{
						glm::dvec3 offset = glm::dvec3(field.PositionOf(at.x, at.y, at.z))
							- glm::dvec3(island.Centre);

						for (int r = 0; r < 3; r++)
							for (int c = 0; c < 3; c++)
								covariance[c][r] += offset[r] * offset[c];
					}

					covariance /= (double)island.Voxels.size();

					glm::mat3 axes = PrincipalAxes(covariance);

					if (glm::determinant(axes) < 0.0f)
						axes[2] = -axes[2];

					// Extents along those axes, plus the half voxel each sample
					// stands for -- the same half-voxel skin the world bounds get.
					glm::vec3 low(std::numeric_limits<float>::max());
					glm::vec3 high(-std::numeric_limits<float>::max());

					for (const glm::ivec3& at : island.Voxels)
					{
						glm::vec3 offset = field.PositionOf(at.x, at.y, at.z) - island.Centre;
						glm::vec3 local = glm::transpose(axes) * offset;

						low = glm::min(low, local);
						high = glm::max(high, local);
					}

					// The box is centred on the *extents*; `Centre` stays the
					// centre of mass. For anything but a symmetric piece those are
					// different points, and they are wanted for different things:
					// a single box body goes at the box centre, a compound goes at
					// the centre of mass with its children offset from there.
					glm::vec3 middle = (low + high) * 0.5f;

					island.Orientation = glm::quat_cast(axes);
					island.HalfExtents = (high - low) * 0.5f + glm::vec3(voxel * 0.5f);
					island.BoxCentre = island.Centre + axes * middle;

					islands.push_back(std::move(island));
				}
			}
		}

		return islands;
	}

	std::vector<CompoundChild> VoxelIslands::Decompose(const VoxelField3D& field,
		const VoxelIsland& island)
	{
		std::vector<CompoundChild> boxes;

		if (island.Voxels.empty())
			return boxes;

		const float voxel = field.VoxelSize();

		glm::ivec3 min = island.Min;
		glm::ivec3 size = island.Max - island.Min + glm::ivec3(1);

		auto indexOf = [&](int x, int y, int z)
		{
			return ((size_t)z * size.y + y) * size.x + x;
		};

		// The island's own voxels, marked in a local box -- not `field.Solid`,
		// because a second island touching this one's bounding box is not part
		// of it and must not be swallowed into a shared slab.
		std::vector<unsigned char> present((size_t)size.x * size.y * size.z, 0);

		for (const glm::ivec3& at : island.Voxels)
		{
			glm::ivec3 local = at - min;
			present[indexOf(local.x, local.y, local.z)] = 1;
		}

		std::vector<unsigned char> used(present.size(), 0);

		glm::mat3 toLocal = glm::transpose(glm::mat3_cast(island.Orientation));

		for (int z = 0; z < size.z; z++)
		{
			for (int y = 0; y < size.y; y++)
			{
				for (int x = 0; x < size.x; x++)
				{
					if (!present[indexOf(x, y, z)] || used[indexOf(x, y, z)])
						continue;

					// Along x while the run continues.
					int spanX = 0;
					while (x + spanX < size.x
						&& present[indexOf(x + spanX, y, z)]
						&& !used[indexOf(x + spanX, y, z)])
						spanX++;

					// Then that run upwards, while every voxel of the next row
					// is available -- a partial row would leave a hole.
					int spanY = 1;
					while (y + spanY < size.y)
					{
						bool whole = true;
						for (int i = 0; i < spanX && whole; i++)
							whole = present[indexOf(x + i, y + spanY, z)]
								&& !used[indexOf(x + i, y + spanY, z)];

						if (!whole)
							break;

						spanY++;
					}

					// Then the slab along z, on the same terms.
					int spanZ = 1;
					while (z + spanZ < size.z)
					{
						bool whole = true;
						for (int j = 0; j < spanY && whole; j++)
							for (int i = 0; i < spanX && whole; i++)
								whole = present[indexOf(x + i, y + j, z + spanZ)]
									&& !used[indexOf(x + i, y + j, z + spanZ)];

						if (!whole)
							break;

						spanZ++;
					}

					for (int k = 0; k < spanZ; k++)
						for (int j = 0; j < spanY; j++)
							for (int i = 0; i < spanX; i++)
								used[indexOf(x + i, y + j, z + k)] = 1;

					// The box in world space, then into the island's frame.
					glm::vec3 low = field.PositionOf(min.x + x, min.y + y, min.z + z)
						- glm::vec3(voxel * 0.5f);
					glm::vec3 high = field.PositionOf(min.x + x + spanX - 1,
						min.y + y + spanY - 1, min.z + z + spanZ - 1)
						+ glm::vec3(voxel * 0.5f);

					CompoundChild box;
					box.HalfExtents = (high - low) * 0.5f;
					box.Offset = toLocal * ((low + high) * 0.5f - island.Centre);

					boxes.push_back(box);
				}
			}
		}

		return boxes;
	}

	VoxelField3D VoxelIslands::Extract(VoxelField3D& field, const VoxelIsland& island)
	{
		VoxelField3D piece;

		if (island.Voxels.empty())
			return piece;

		const float voxel = field.VoxelSize();

		// One voxel of air all round, so the surface has somewhere to close.
		const glm::ivec3 skirt(1);
		glm::ivec3 min = island.Min - skirt;
		glm::ivec3 size = (island.Max + skirt) - min + glm::ivec3(1);

		piece.Create(size, voxel, field.PositionOf(min.x, min.y, min.z));

		// Copied before anything is removed, or the distances written into the
		// piece are the ones left behind by the carving.
		for (const glm::ivec3& at : island.Voxels)
		{
			glm::ivec3 local = at - min;
			piece.Set(local.x, local.y, local.z,
				field.DistanceAt(at.x, at.y, at.z),
				field.MaterialAt(at.x, at.y, at.z));
		}

		// The piece's air. Taken from the source field where it exists, so the
		// surface between solid and air lands in the same place it did before
		// the piece was cut out -- a flat +Far skirt would drag every boundary
		// vertex onto the last solid sample and shrink the rock.
		for (int z = 0; z < size.z; z++)
		{
			for (int y = 0; y < size.y; y++)
			{
				for (int x = 0; x < size.x; x++)
				{
					glm::ivec3 source = min + glm::ivec3(x, y, z);

					if (!field.Contains(source.x, source.y, source.z))
						continue;
					if (field.Solid(source.x, source.y, source.z))
						continue;

					piece.Set(x, y, z, field.DistanceAt(source.x, source.y, source.z),
						field.MaterialAt(source.x, source.y, source.z));
				}
			}
		}

		// Now take it out of the world. Written as air a voxel deep rather than
		// as +Far, so the field the character stands on stays a distance field
		// near its new surface.
		for (const glm::ivec3& at : island.Voxels)
		{
			field.Set(at.x, at.y, at.z, voxel, 0);
			field.MarkDirtyAt(at.x, at.y, at.z);
		}

		return piece;
	}

}
