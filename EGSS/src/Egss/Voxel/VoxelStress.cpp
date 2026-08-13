#include "egsspch.h"
#include "Egss/Voxel/VoxelStress.h"

#include <queue>
#include <unordered_map>
#include <set>

namespace Egss {

	namespace {

		const glm::ivec3 s_Neighbours[6] =
		{
			{ 1, 0, 0 }, { -1, 0, 0 },
			{ 0, 1, 0 }, { 0, -1, 0 },
			{ 0, 0, 1 }, { 0, 0, -1 }
		};

		// What travels along one connection: how much weight, and where that
		// weight is. The position is carried as mass times position, so it can
		// be added up along the way and divided out once at the end -- summing
		// centres of mass directly would weight a grain the same as a boulder.
		struct Flow
		{
			double Mass = 0.0;
			glm::dvec3 Weighted = glm::dvec3(0.0);
		};

		struct Grid
		{
			glm::ivec3 Size = glm::ivec3(0);

			size_t Index(int x, int y, int z) const
			{
				return ((size_t)z * Size.y + y) * Size.x + x;
			}

			size_t Index(const glm::ivec3& at) const { return Index(at.x, at.y, at.z); }
		};

	}

	std::vector<VoxelSection> VoxelStress::Overloaded(const VoxelField3D& field,
		const VoxelStressSettings& settings)
	{
		std::vector<VoxelSection> failing;

		if (field.Empty())
			return failing;

		Grid grid;
		grid.Size = field.Size();

		const size_t count = (size_t)grid.Size.x * grid.Size.y * grid.Size.z;
		const float voxel = field.VoxelSize();
		const float faceArea = voxel * voxel;
		const float weightPerVoxel = settings.Density * settings.Gravity
			* voxel * voxel * voxel;

		// --- 0. Solidity, once, into a flat array ---------------------------
		//
		// **This is most of what the pass used to cost.** `field.Solid` clamps
		// three coordinates, divides and takes a remainder on each to find the
		// chunk, then follows a pointer into it -- and the sweeps below ask it
		// about seven times per solid voxel. Reading the whole field once into a
		// byte per lattice point turns all of that into an array index, and the
		// array is a fraction of the field's own memory.
		std::vector<unsigned char> solid(count, 0);

		{
			size_t index = 0;
			for (int z = 0; z < grid.Size.z; z++)
				for (int y = 0; y < grid.Size.y; y++)
					for (int x = 0; x < grid.Size.x; x++, index++)
						solid[index] = field.Solid(x, y, z) ? 1 : 0;
		}

		// Stepping to a neighbour is adding one of these to a flat index, so the
		// inner loops never rebuild a coordinate. The bounds still have to be
		// checked in coordinates, or a step off the +x face silently lands on
		// the -x face of the next row.
		const int stride[6] = { 1, -1, grid.Size.x, -grid.Size.x,
			grid.Size.x * grid.Size.y, -grid.Size.x * grid.Size.y };

		// --- 1. How far each voxel is from being held up ---------------------
		//
		// A breadth-first sweep out from the anchors. The depth is not used as a
		// distance; it is used as an *order*, so that "towards the anchor" has a
		// meaning at every voxel and load has somewhere to go.
		std::vector<int> depth(count, -1);

		// The queue is the visit order, and the visit order is wanted afterwards
		// anyway -- so one vector serves as both, read with a cursor rather than
		// popped. A std::queue would allocate deque blocks for the same thing and
		// then throw the order away.
		std::vector<int> order;
		order.reserve(count / 4);

		for (int z = 0; z < grid.Size.z; z++)
		{
			for (int y = 0; y <= glm::min(settings.AnchorHeight, grid.Size.y - 1); y++)
			{
				for (int x = 0; x < grid.Size.x; x++)
				{
					size_t index = grid.Index(x, y, z);
					if (!solid[index])
						continue;

					depth[index] = 0;
					order.push_back((int)index);
				}
			}
		}

		for (size_t cursor = 0; cursor < order.size(); cursor++)
		{
			int at = order[cursor];

			int x = at % grid.Size.x;
			int y = (at / grid.Size.x) % grid.Size.y;
			int z = at / (grid.Size.x * grid.Size.y);

			for (int k = 0; k < 6; k++)
			{
				int nx = x + s_Neighbours[k].x;
				int ny = y + s_Neighbours[k].y;
				int nz = z + s_Neighbours[k].z;

				if (nx < 0 || ny < 0 || nz < 0
					|| nx >= grid.Size.x || ny >= grid.Size.y || nz >= grid.Size.z)
					continue;

				int next = at + stride[k];

				if (!solid[next] || depth[next] >= 0)
					continue;

				depth[next] = depth[at] + 1;
				order.push_back(next);
			}
		}

		// --- 2. Route the load inwards --------------------------------------
		//
		// Deepest first, so everything a voxel is holding up has already been
		// added to it by the time it passes anything on. Split evenly between
		// the neighbours nearer the anchor: that is what makes a wide neck share
		// a load a narrow one has to carry alone, and it is the cheap stand-in
		// for solving the whole structure at once.
		std::vector<Flow> carried(count);

		auto positionOf = [&](int index)
		{
			int x = index % grid.Size.x;
			int y = (index / grid.Size.x) % grid.Size.y;
			int z = index / (grid.Size.x * grid.Size.y);

			return field.PositionOf(x, y, z);
		};

		for (int at : order)
		{
			Flow& flow = carried[at];
			flow.Mass += weightPerVoxel;
			flow.Weighted += glm::dvec3(positionOf(at)) * (double)weightPerVoxel;
		}

		// Links are keyed by the far voxel and the direction the load leaves in,
		// which is enough to identify the face and to find it again below.
		struct Link
		{
			glm::ivec3 Far;
			int Axis = 0;
			int Plane = 0;       // lattice coordinate of the *near* voxel
			Flow Through;
		};

		std::vector<Link> links;
		links.reserve(order.size());

		for (size_t i = order.size(); i-- > 0; )
		{
			int at = order[i];
			int here = depth[at];

			if (here == 0)
				continue;   // an anchor; the world takes it from here

			int x = at % grid.Size.x;
			int y = (at / grid.Size.x) % grid.Size.y;
			int z = at / (grid.Size.x * grid.Size.y);

			int downhill[6];
			int downhillAxis[6];
			int downhillPlane[6];
			int downhillCount = 0;

			for (int k = 0; k < 6; k++)
			{
				int nx = x + s_Neighbours[k].x;
				int ny = y + s_Neighbours[k].y;
				int nz = z + s_Neighbours[k].z;

				if (nx < 0 || ny < 0 || nz < 0
					|| nx >= grid.Size.x || ny >= grid.Size.y || nz >= grid.Size.z)
					continue;

				int next = at + stride[k];

				if (!solid[next] || depth[next] >= here)
					continue;

				int axis = s_Neighbours[k].x != 0 ? 0 : (s_Neighbours[k].y != 0 ? 1 : 2);

				downhill[downhillCount] = next;
				downhillAxis[downhillCount] = axis;
				downhillPlane[downhillCount] = axis == 0 ? nx : (axis == 1 ? ny : nz);
				downhillCount++;
			}

			if (downhillCount == 0)
				continue;

			const Flow flow = carried[at];
			const double share = 1.0 / (double)downhillCount;

			for (int k = 0; k < downhillCount; k++)
			{
				Flow part;
				part.Mass = flow.Mass * share;
				part.Weighted = flow.Weighted * share;

				carried[downhill[k]].Mass += part.Mass;
				carried[downhill[k]].Weighted += part.Weighted;

				Link link;
				link.Far = { x, y, z };
				link.Axis = downhillAxis[k];
				link.Plane = downhillPlane[k];
				link.Through = part;

				links.push_back(link);
			}
		}

		// --- 3. Group coplanar links into sections ---------------------------
		//
		// A neck is not a set of independent faces, it is a beam: judged one face
		// at a time, a wide connection resists bending no better than a narrow
		// one, which is wrong and wrong in the flattering direction.
		//
		// Grouped by the plane the links cross and then by which of them touch,
		// so two separate legs standing on the same floor are two sections rather
		// than one.
		std::unordered_map<long long, std::vector<size_t>> byPlane;

		// Keyed by a packed integer rather than a tuple in an ordered map: this
		// runs over every connection in the field, and the constant factor is
		// most of what the pass costs.
		for (size_t i = 0; i < links.size(); i++)
			byPlane[(long long)links[i].Axis * 100000 + links[i].Plane].push_back(i);

		// One buffer for the whole pass, stamped rather than cleared -- the same
		// trick the broadphase uses. Allocating a flag array per plane is one
		// allocation the size of *every* link for each of a few hundred planes,
		// which is quadratic dressed up as bookkeeping.
		std::vector<unsigned int> stamp(links.size(), 0);
		unsigned int generation = 0;

		std::unordered_map<long long, size_t> lookup;
		std::vector<size_t> cluster;
		std::vector<size_t> stack;

		// The cut test's own buffers, stamped by the same generation counter so
		// nothing has to be cleared between sections.
		std::vector<unsigned int> cellStamp(count, 0);
		std::vector<int> cutFill;

		for (const auto& entry : byPlane)
		{
			const std::vector<size_t>& inPlane = entry.second;

			// Which links in this plane are adjacent to which, by the position of
			// their far voxels within the plane.
			lookup.clear();
			const int axis = links[inPlane[0]].Axis;

			int u = (axis + 1) % 3;
			int v = (axis + 2) % 3;

			for (size_t index : inPlane)
				lookup[(long long)links[index].Far[u] * 100000 + links[index].Far[v]] = index;

			generation++;

			for (size_t seed : inPlane)
			{
				if (stamp[seed] == generation)
					continue;

				cluster.clear();
				stack.clear();
				stack.push_back(seed);
				stamp[seed] = generation;

				while (!stack.empty())
				{
					size_t index = stack.back();
					stack.pop_back();
					cluster.push_back(index);

					const int cu = links[index].Far[u];
					const int cv = links[index].Far[v];

					const int steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

					for (const auto& step : steps)
					{
						auto found = lookup.find((long long)(cu + step[0]) * 100000
							+ (cv + step[1]));
						if (found == lookup.end() || stamp[found->second] == generation)
							continue;

						stamp[found->second] = generation;
						stack.push_back(found->second);
					}
				}

				if ((int)cluster.size() < settings.MinSectionLinks)
					continue;   // now 1 by default; the cut test does the work

				// --- The section's own arithmetic ---
				VoxelSection section;
				section.Axis = axis;
				section.Area = (float)cluster.size() * faceArea;

				Flow total;
				glm::dvec3 centroid(0.0);

				for (size_t index : cluster)
				{
					total.Mass += links[index].Through.Mass;
					total.Weighted += links[index].Through.Weighted;

					centroid += glm::dvec3(field.PositionOf(links[index].Far.x,
						links[index].Far.y, links[index].Far.z));

					section.Far.push_back(links[index].Far);
				}

				centroid /= (double)cluster.size();
				section.Centroid = glm::vec3(centroid);
				section.Load = (float)total.Mass;

				if (total.Mass <= 0.0)
					continue;

				glm::dvec3 loadCentre = total.Weighted / total.Mass;

				// Second moment of the connection about its own centroid, and how
				// far the outermost part of it is from that centre. Both are taken
				// perpendicular to gravity's lever: a section is bent about the
				// horizontal axis across the overhang.
				//
				// The lever arm is the *horizontal* offset from the section to the
				// weight it holds -- straight down through a section is a push,
				// not a bend.
				glm::dvec3 lever = loadCentre - centroid;
				lever.y = 0.0;

				double armLength = glm::length(lever);
				section.Moment = (float)(total.Mass * armLength);

				double second = 0.0;
				double furthest = 0.0;

				if (armLength > 1e-9)
				{
					// Bending happens about the axis across the lever, so the
					// distance that matters is measured *along* it.
					glm::dvec3 along = lever / armLength;

					for (size_t index : cluster)
					{
						glm::dvec3 offset = glm::dvec3(field.PositionOf(
							links[index].Far.x, links[index].Far.y,
							links[index].Far.z)) - centroid;

						// A vertical section (one crossing a horizontal plane) is
						// bent about a horizontal axis, so the fibres in tension
						// are the ones furthest along the lever. A horizontal
						// section -- an overhang meeting a cliff face -- is bent
						// about the same axis, and its fibres are spread
						// vertically.
						double r = axis == 1
							? glm::dot(offset, along)
							: offset.y;

						// **A face is not a point.** Its own second moment about
						// its own centre is `A s^2 / 12`, added by the parallel
						// axis theorem, and its outermost fibre is half a voxel
						// past its centre.
						//
						// Without both corrections the model reads systematically
						// low, by exactly `n / (n + 1)` for a section n voxels
						// thick -- 14% at six layers, and only reaching beam
						// theory in the limit. With them the discrete sum is
						// *identically* `w h^3 / 12` and `h / 2` for any n, so a
						// two-voxel neck is judged by the same arithmetic as a
						// twenty-voxel one.
						second += (r * r + (double)(voxel * voxel) / 12.0)
							* (double)faceArea;
						furthest = glm::max(furthest, std::fabs(r) + voxel * 0.5);
					}
				}

				// --- Is this section the only way down? -----------------------
				//
				// **Bending only belongs to a section that is the sole path.** A
				// connection that is one of many parallel routes carries a share
				// of the load, and attributing the *whole* lever to that share
				// over-reads its stress by exactly the number of parallel routes:
				// one face gets `6M/s^3` where the n-face section it belongs to
				// gets `6M/(n s^3)`.
				//
				// That is what made lone sideways connections outrank every real
				// section, get carved first and sever nothing -- which a minimum
				// face count papered over rather than fixed.
				//
				// The test is exact and needs no threshold: block the section's
				// near side, walk outwards from its far side, and see whether an
				// anchor is still reachable. If it is, the load has somewhere else
				// to go and this is not a cantilever root. The early-out makes the
				// artefact case cheap -- a lone link reaches an anchor in a step
				// or two -- while a genuine cut pays for one walk of the piece it
				// is holding up.
				// **Only asked when the answer can change anything.** The cut test
				// can only ever *lower* a stress -- it removes the bending term --
				// so a section already under the limit with bending counted is
				// under it either way and needs no walk. Without that guard every
				// section in the field pays for a flood fill, and the suite ran
				// past ten minutes.
				float optimistic = section.Load / section.Area
					+ (second > 1e-12
						? (float)((double)section.Moment * furthest / second)
						: 0.0f);

				bool solePath = true;

				if (optimistic > settings.Strength)
				{
					generation++;

					const int plane = links[cluster[0]].Plane;

					// The near side of every link in the section: the same voxel
					// with its coordinate on this axis moved to the plane. Blocked
					// so the walk cannot cross the section it is testing.
					for (const glm::ivec3& at : section.Far)
					{
						glm::ivec3 near = at;
						near[axis] = plane;
						cellStamp[grid.Index(near)] = generation;
					}

					cutFill.clear();

					for (const glm::ivec3& at : section.Far)
					{
						size_t index = grid.Index(at);
						if (cellStamp[index] == generation)
							continue;

						cellStamp[index] = generation;
						cutFill.push_back((int)index);
					}

					// A walk that gets this far without finding the ground is
					// hanging off something large, and the rest of it proves
					// nothing the first few thousand voxels did not.
					const size_t fillCap = 8192;

					for (size_t cursor = 0; cursor < cutFill.size() && solePath
						&& cursor < fillCap; cursor++)
					{
						int at = cutFill[cursor];

						int cx = at % grid.Size.x;
						int cy = (at / grid.Size.x) % grid.Size.y;
						int cz = at / (grid.Size.x * grid.Size.y);

						for (int k = 0; k < 6; k++)
						{
							int nx = cx + s_Neighbours[k].x;
							int ny = cy + s_Neighbours[k].y;
							int nz = cz + s_Neighbours[k].z;

							if (nx < 0 || ny < 0 || nz < 0
								|| nx >= grid.Size.x || ny >= grid.Size.y
								|| nz >= grid.Size.z)
								continue;

							int next = at + stride[k];

							if (!solid[next] || cellStamp[next] == generation)
								continue;

							if (depth[next] == 0)
							{
								// Reached the ground another way.
								solePath = false;
								break;
							}

							cellStamp[next] = generation;
							cutFill.push_back(next);
						}
					}
				}

				if (!solePath)
				{
					// It still carries its share; it just does not carry the
					// moment on its own.
					second = 0.0;
					section.Moment = 0.0f;
				}

				section.DirectStress = section.Load / section.Area;
				section.BendingStress = second > 1e-12
					? (float)((double)section.Moment * furthest / second)
					: 0.0f;

				section.Stress = section.DirectStress + section.BendingStress;

				if (section.Stress > settings.Strength)
					failing.push_back(std::move(section));
			}
		}

		std::sort(failing.begin(), failing.end(),
			[](const VoxelSection& a, const VoxelSection& b) { return a.Stress > b.Stress; });

		return failing;
	}

	int VoxelStress::Relieve(VoxelField3D& field, const VoxelStressSettings& settings,
		int maxSections)
	{
		std::vector<VoxelSection> failing = Overloaded(field, settings);

		if (failing.empty())
			return 0;

		const float voxel = field.VoxelSize();

		// Which voxels have already been carved this round, so two overlapping
		// sections do not both claim the same rock -- the second would be
		// carving air and reporting a break that did nothing.
		std::set<std::tuple<int, int, int>> taken;
		int broken = 0;

		for (const VoxelSection& section : failing)
		{
			if (broken >= maxSections)
				break;

			bool overlaps = false;
			for (const glm::ivec3& at : section.Far)
			{
				if (taken.count({ at.x, at.y, at.z }))
				{
					overlaps = true;
					break;
				}
			}

			if (overlaps)
				continue;

			for (const glm::ivec3& at : section.Far)
			{
				taken.insert({ at.x, at.y, at.z });

				field.Set(at.x, at.y, at.z, voxel, 0);
				field.MarkDirtyAt(at.x, at.y, at.z);
			}

			broken++;
		}

		return broken;
	}

	bool VoxelStress::BreakWorst(VoxelField3D& field, const VoxelStressSettings& settings)
	{
		std::vector<VoxelSection> failing = Overloaded(field, settings);

		if (failing.empty())
			return false;

		const float voxel = field.VoxelSize();

		for (const glm::ivec3& at : failing[0].Far)
		{
			// Air, one voxel deep, rather than +Far: the field either side of the
			// tear stays a distance field, which is what the collider and the
			// mesher both read.
			field.Set(at.x, at.y, at.z, voxel, 0);
			field.MarkDirtyAt(at.x, at.y, at.z);
		}

		return true;
	}

}
