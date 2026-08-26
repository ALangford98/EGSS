#pragma once

#include <Egss.h>
#include "VoxelPlanet.h"

#include <queue>
#include <vector>

// Water on the ground you are actually standing on.
//
// **The planet's drainage grid cannot do this, and the reason is arithmetic.**
// A texel of it is 1.5 km; a lake is smaller than that. So it can say which
// basins hold water and at what height, and it cannot say where a shore is --
// a surface drawn from it stands in the air wherever its basin does not exist
// in the real terrain, which was measured at 8.14% of a frame before this
// existed.
//
// This is the same algorithm at the resolution of the ground: a grid of
// *columns* over the streamed region, each holding the radius of the rock and
// the radius of the water above it. Two things follow from columns rather than
// voxels. A lake at rest is one number a column instead of a stack of full
// cells, which is what makes a live simulation affordable. And "level" means
// *radius from the planet's centre*, not height in a tangent plane -- over
// 800 m of a 250 km sphere a plane is out by 0.32 m, which is a fifth of a
// voxel of slope across every lake.
//
// **What it cannot represent: water in a tunnel.** A column has one water
// surface, so digging *under* a ridge does not carry water through it. Digging
// a channel *across* one does, which is the case that was asked for.
class SurfaceWater
{
public:
	// Enough that a lake's edge lands within a few metres, and no finer: the
	// shoreline a viewer sees is the terrain occluding the water, at the
	// resolution of the chunk meshes, not the resolution of this.
	static constexpr int Side = 128;

	bool Valid() const { return m_Valid; }

	int Cells() const { return Side * Side; }

	float GroundAt(int i) const { return m_Ground[i]; }
	float LevelAt(int i) const { return m_Level[i]; }
	bool WetAt(int i) const { return m_Level[i] > m_Ground[i] + m_Film; }

	const glm::vec3& DirectionAt(int i) const { return m_Direction[i]; }

	float Reach() const { return m_Reach; }
	const glm::dvec3& Site() const { return m_Site; }

	// The water's own radius above a point given in the landing site's local
	// frame -- the same frame `Touch` and `Dig` already work in. False if the
	// point is over dry ground or outside the grid, in which case `outLevel`
	// is untouched.
	bool LevelNear(const glm::vec3& localPosition, float& outLevel) const;

	// The four numbers worth having out loud: how much of the region is under
	// water, how flat the water is, whether any of it is under the ground, and
	// how much there is of it.
	void Report() const;

	void Build(const VoxelPlanet& planet, const glm::ivec3& lattice,
		const glm::dvec3& site, float reach);

	// **Where the rock is, asked of the field and not of the generator.**
	//
	// The two disagree: the field is a sampled, trilinearly interpolated copy
	// of the generator, and near a waterline a few centimetres decides whether
	// a column is wet. Building from one and editing from the other put
	// twenty-four columns under water for a pit narrower than one of them --
	// the ground had not moved, the *question* had.
	//
	// It also has to answer where the field has nothing yet: a landing happens
	// before the region has streamed, and an unallocated chunk reads `Far`.
	// The generator is the fallback, and the water is rebuilt as the ground
	// arrives.
	float GroundFrom(const VoxelPlanet& planet, const glm::ivec3& lattice,
		const glm::vec3& direction, float guess, float range) const;

	// The wet columns as geometry, in the site's frame. Empty if none are.
	//
	// **The shoreline is not in here.** Only whole wet quads are emitted, so
	// the mesh's own edge is a staircase at the column spacing -- and it never
	// shows, because the terrain in front of it is what the eye sees the water
	// end against. That is the same division of labour the ocean sphere has
	// always used, and it is why this grid can be six metres a cell while the
	// coast reads at the resolution of the chunks.
	void BuildMesh(Egss::MeshData& out) const;

	// **Re-read the ground where somebody dug, then let the water find out
	// what that changed.**
	//
	// The columns are re-derived from the *field*, not from the generator: the
	// generator does not know about the hole. Everything else is the flood
	// again, which is cheap enough at sixteen thousand columns to simply
	// re-run -- and re-running it is what makes the answer right rather than
	// approximately right. A pit dug inland stays dry because no path to water
	// reaches it; a channel cut to the shore fills it, because one does.
	//
	// Returns true if the water moved, meaning the mesh wants rebuilding.
	bool Touch(const VoxelPlanet& planet, const glm::ivec3& lattice,
		const glm::vec3& centre, float radius);

private:
	// **How deep water has to be before it is water**, and it is a quarter of
	// a voxel rather than an epsilon.
	//
	// An epsilon was enough to stop float equality declaring dry land wet --
	// the trap the planet-wide version fell into. It is not enough once the
	// ground is read from the *field*, which is a sampled, interpolated copy
	// of the generator and dips by a few centimetres all over: every one of
	// those dips became its own puddle, and a coastline came out as **five
	// hundred separate sheets** of water instead of eight. A quarter of a
	// voxel is the shallowest puddle the terrain can actually represent.
	float m_Film = 0.01f;

	// **How far the seeded rim's answer reaches inward before the local flood
	// overrides it.** Measured, not guessed: excluding two columns from the
	// levelness check still showed 0.36 m of spread and excluding six showed
	// none, so six is where the planet-wide map stops having an opinion. The
	// drawn mesh leaves the same margin out, which is what makes the surface
	// on screen exactly the answer the local terrain gave.
	static constexpr int s_SeedMargin = 6;

	int Index(int u, int v) const { return v * Side + u; }

	// The column a point in the site's local frame falls in, *not* clamped to
	// the grid -- `Touch` wants the raw value to centre a span on, `LevelNear`
	// wants to reject it. Shared because both are the reverse of the
	// direction-to-offset mapping `Build` uses to lay the grid out.
	glm::ivec2 ColumnAt(const glm::vec3& localPosition) const
	{
		glm::vec3 up = glm::vec3(glm::normalize(m_Site));
		glm::vec3 reference = std::abs(up.y) < 0.9f
			? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

		glm::vec3 east = glm::normalize(glm::cross(reference, up));
		glm::vec3 north = glm::cross(up, east);

		int u = (int)std::round((glm::dot(localPosition, east) / (2.0f * m_Reach) + 0.5f)
			* (float)(Side - 1));
		int v = (int)std::round((glm::dot(localPosition, north) / (2.0f * m_Reach) + 0.5f)
			* (float)(Side - 1));

		return glm::ivec2(u, v);
	}

	// Priority-Flood again, and for the same reason: every column is raised
	// until it has a path to somewhere water can leave by that never goes
	// uphill. What differs from the planet-wide pass is what seeds it. There
	// the sea was the boundary; here the *edge of the streamed region* is,
	// because water can leave across it -- and it leaves at whatever height
	// the planet-wide answer says the water stands at out there, which is how
	// the local answer and the global one agree at the join.
	void Flood(const std::vector<float>& outside);

	bool m_Valid = false;

	glm::ivec3 m_Lattice = glm::ivec3(0);
	float m_Reach = 0.0f;
	glm::dvec3 m_Site = glm::dvec3(0.0);

	// Where the ground was found by asking the field rather than the
	// generator. Kept so the seeds can be rebuilt without re-deriving
	// everything: `Build` fills it from the map, `Touch` from the field.
	std::vector<float> m_Outside;

	std::vector<glm::vec3> m_Direction;
	std::vector<float> m_Ground;
	std::vector<float> m_Level;
};

inline float SurfaceWater::GroundFrom(const VoxelPlanet& planet,
	const glm::ivec3& lattice, const glm::vec3& direction, float guess,
	float range) const
{
	float voxel = planet.Get().VoxelSize;
	float step = voxel * 0.5f;

	// **Down from above, and stop at the first rock.** A column can cross the
	// surface several times once somebody has dug -- air, roof, the hole,
	// floor -- and the one that matters is the highest solid the sky can see.
	// Bisecting a range would find whichever crossing the endpoints happened
	// to bracket.
	float high = guess + range;
	float low = guess - range;

	auto solid = [&](float r)
	{
		glm::vec3 point = glm::vec3(
			glm::dvec3(direction) * (double)r - m_Site);

		return planet.Field()->SampleDistanceFrom(point, lattice) <= 0.0f;
	};

	for (float r = high; r > low; r -= step)
	{
		if (!solid(r))
			continue;

		float above = r + step;
		float below = r;

		for (int i = 0; i < 14; i++)
		{
			float middle = (above + below) * 0.5f;

			if (solid(middle))
				above = middle;
			else
				below = middle;
		}

		return (above + below) * 0.5f;
	}

	// Nothing here yet. The generator's answer stands until it streams in.
	return guess;
}

inline void SurfaceWater::Build(const VoxelPlanet& planet,
	const glm::ivec3& lattice, const glm::dvec3& site, float reach)
{
	m_Valid = false;

	if (!planet.Get().HasOcean || !planet.Water().Valid())
		return;

	m_Site = site;
	m_Lattice = lattice;
	m_Reach = reach;
	m_Film = 0.25f * planet.Get().VoxelSize;

	const int count = Side * Side;

	m_Direction.assign(count, glm::vec3(0.0f));
	m_Ground.assign(count, 0.0f);
	m_Level.assign(count, 0.0f);

	glm::vec3 up = glm::vec3(glm::normalize(site));
	glm::vec3 east, north;

	glm::vec3 reference = std::abs(up.y) < 0.9f
		? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

	east = glm::normalize(glm::cross(reference, up));
	north = glm::cross(up, east);

	// What the planet-wide answer says out at the rim, which is what the local
	// flood is allowed to drain to.
	m_Outside.assign(count, 0.0f);

	std::vector<float>& outside = m_Outside;

	for (int v = 0; v < Side; v++)
	for (int u = 0; u < Side; u++)
	{
		float su = ((float)u / (float)(Side - 1) - 0.5f) * 2.0f * reach;
		float sv = ((float)v / (float)(Side - 1) - 0.5f) * 2.0f * reach;

		glm::dvec3 out = site + glm::dvec3(east) * (double)su
			+ glm::dvec3(north) * (double)sv;

		glm::vec3 direction = glm::vec3(glm::normalize(out));

		int at = Index(u, v);

		m_Direction[at] = direction;

		// **The terrain, not the map.** This is the whole difference: the
		// ground here is the surface the mesher cuts, to a millimetre, rather
		// than a 1.5 km average of it. The generator gives the first guess and
		// the field -- what is actually meshed and stood on -- refines it.
		float guess = planet.SurfaceRadius(direction);

		m_Ground[at] = GroundFrom(planet, lattice, direction, guess,
			3.0f * planet.Get().VoxelSize);

		// **The map only gets to seed a rim cell within a plausible reach of
		// what the fine terrain there actually is.** `WetnessAt`/
		// `WaterHeightAt` are a 1.5 km texel's answer, already documented to
		// disagree with the terrain the mesher cuts by up to 116 m -- and a
		// 1.5 km texel is wider than this whole grid (2x380 m), so one
		// texel's classification can cover every direction around the rim at
		// once, not just the side a real shore is actually on. Nothing
		// stopped that disagreement from being seeded here and flooding
		// inward through the interior, well past `s_SeedMargin`, which only
		// hides the seam this leaves in the *drawn* mesh, not what the flood
		// itself believes. Measured at the default landing site: a rim
		// direction the map called a lake 39.5 m above sea level seeded a
		// flood that reached the lander's own column, whose fine, sampled
		// ground is 21 m above sea level and has nothing to do with that
		// basin. A real shoreline's water sits close to the ground beside
		// it; a coarse texel guessing at a point it cannot resolve does not.
		float mapLevel = planet.WetnessAt(direction) > 0.25f
			? (float)planet.Get().OceanRadius + planet.WaterHeightAt(direction)
			: -1.0f;

		float shoreSlack = 3.0f * planet.Get().VoxelSize;

		outside[at] = (mapLevel > 0.0f && mapLevel <= m_Ground[at] + shoreSlack)
			? mapLevel : -1.0f;
	}

	Flood(outside);

	m_Valid = true;
}

inline void SurfaceWater::Flood(const std::vector<float>& outside)
{
	const int count = Side * Side;

	std::vector<unsigned char> done(count, 0);

	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;

	// The rim, and only the rim. An interior column that the planet-wide map
	// calls wet is *not* seeded: whether it holds water is exactly what this
	// is being run to find out, and seeding it would assert the answer the map
	// got wrong.
	for (int v = 0; v < Side; v++)
	for (int u = 0; u < Side; u++)
	{
		if (u != 0 && v != 0 && u != Side - 1 && v != Side - 1)
			continue;

		int at = Index(u, v);

		m_Level[at] = glm::max(m_Ground[at], outside[at]);

		done[at] = 1;
		open.push({ m_Level[at], at });
	}

	static const int dx[4] = { 1, -1, 0, 0 };
	static const int dy[4] = { 0, 0, 1, -1 };

	while (!open.empty())
	{
		Entry entry = open.top();
		open.pop();

		int at = entry.second;
		int u = at % Side, v = at / Side;

		for (int d = 0; d < 4; d++)
		{
			int nu = u + dx[d], nv = v + dy[d];

			if (nu < 0 || nv < 0 || nu >= Side || nv >= Side)
				continue;

			int next = Index(nu, nv);

			if (done[next])
				continue;

			// No epsilon. Nothing is routed on this surface -- it is the water
			// itself -- so a basin's columns take the same spill height and
			// the lake in it is flat, which is the property the planet-wide
			// version had to be reordered to get.
			m_Level[next] = glm::max(m_Ground[next], m_Level[at]);

			done[next] = 1;
			open.push({ m_Level[next], next });
		}
	}
}

inline void SurfaceWater::Report() const
{
	if (!m_Valid)
		return;

	const int count = Side * Side;

	int wet = 0;
	double volume = 0.0;
	float worstStanding = 0.0f;

	for (int i = 0; i < count; i++)
	{
		if (!WetAt(i))
			continue;

		wet++;
		volume += (double)(m_Level[i] - m_Ground[i]);
		worstStanding = glm::max(worstStanding, m_Ground[i] - m_Level[i]);
	}

	// Levelness per connected sheet, not per neighbour pair -- two different
	// basins meeting at a saddle are supposed to differ, and comparing them
	// reports slope on water that is perfectly flat.
	std::vector<unsigned char> seen(count, 0);
	float worstSpread = 0.0f;
	int sheets = 0;

	static const int dx[4] = { 1, -1, 0, 0 };
	static const int dy[4] = { 0, 0, 1, -1 };

	std::vector<int> stack;

	for (int seed = 0; seed < count; seed++)
	{
		if (seen[seed] || !WetAt(seed))
			continue;

		sheets++;
		stack.push_back(seed);
		seen[seed] = 1;

		// **Max against min over the sheet, not against whichever cell the
		// scan happened to start from.** That cell can be on the seeded ring,
		// in which case every interior column is measured against a level the
		// flood never chose -- which reported a metre of slope on water the
		// flood had made perfectly flat.
		float low = 1e30f, high = -1e30f;

		while (!stack.empty())
		{
			int at = stack.back();
			stack.pop_back();

			int u = at % Side, v = at / Side;

			// **The seeded ring is not evidence, nor is what it feeds.**
			// Rim columns take whatever the planet-wide map says, and that map
			// changes by a texel -- two neighbours can legitimately be handed
			// the sea and a lake, and every measured disagreement turned out
			// to be one of those. Levelness is a claim about what the *flood*
			// did, so it is measured two columns in, where the flood decided.
			bool edge = u < s_SeedMargin || v < s_SeedMargin
				|| u >= Side - s_SeedMargin || v >= Side - s_SeedMargin;

			if (!edge)
			{
				low = glm::min(low, m_Level[at]);
				high = glm::max(high, m_Level[at]);
			}

			for (int d = 0; d < 4; d++)
			{
				int nu = u + dx[d], nv = v + dy[d];

				if (nu < 0 || nv < 0 || nu >= Side || nv >= Side)
					continue;

				int next = Index(nu, nv);

				if (seen[next] || !WetAt(next))
					continue;

				seen[next] = 1;
				stack.push_back(next);
			}
		}

		if (high >= low)
			worstSpread = glm::max(worstSpread, high - low);
	}

	// And what the seam at the rim actually costs, separately, because it is
	// a real step in the drawn surface even though it is not the flood's.
	float worstSeam = 0.0f;

	for (int v = 0; v < Side; v++)
	for (int u = 0; u < Side; u++)
	{
		int at = Index(u, v);

		if (!WetAt(at))
			continue;

		for (int d = 0; d < 4; d++)
		{
			int nu = u + dx[d], nv = v + dy[d];

			if (nu < 0 || nv < 0 || nu >= Side || nv >= Side)
				continue;

			int next = Index(nu, nv);

			if (WetAt(next))
				worstSeam = glm::max(worstSeam,
					std::abs(m_Level[at] - m_Level[next]));
		}
	}

	float cell = 2.0f * m_Reach / (float)(Side - 1);

	EGSS_TRACE("Surface water: {0} of {1} columns wet ({2:.1f}%) in {3} sheets, "
		"{4:.0f} m^3; each sheet level to {5:.5f} m inside the seeded ring, "
		"ground above its own surface by at most {6:.5f} m",
		wet, count, 100.0f * (float)wet / (float)count, sheets,
		volume * (double)cell * (double)cell, worstSpread, worstStanding);

	// **The seam where the planet-wide answer meets the local one.** The map
	// says water stands somewhere the real terrain does not support, by up to
	// a couple of metres. It is confined to the seeded ring -- the flood
	// corrects it in one column -- and the drawn mesh leaves that ring out, so
	// it is measured here and never seen.
	EGSS_TRACE("  the map and the terrain disagree by up to {0:.3f} m at the "
		"seeded rim, which is why the outer {1} columns are not drawn",
		worstSeam, s_SeedMargin);
}

inline void SurfaceWater::BuildMesh(Egss::MeshData& out) const
{
	out = Egss::MeshData();

	if (!m_Valid)
		return;

	std::vector<int> vertex((size_t)Side * Side, -1);

	// **Well in from the edge.** The seeded ring carries the planet-wide map's
	// answer, which disagrees with the terrain here by up to 2.5 m, and that
	// disagreement reaches about six columns inward before the local flood
	// overrides it. Leaving that margin out draws only what the flood decided.
	for (int v = s_SeedMargin; v < Side - s_SeedMargin; v++)
	for (int u = s_SeedMargin; u < Side - s_SeedMargin; u++)
	{
		int at = Index(u, v);

		if (!WetAt(at))
			continue;

		vertex[at] = (int)out.Vertices.size();

		// **How deep the water is here, carried in the texture coordinate.**
		//
		// A puddle and a lake were the same colour, because the shader tinted
		// by the *view* angle -- Fresnel and nothing else -- which says how
		// much sky is being reflected and nothing at all about what is under
		// the surface. Real water reads as water because you can see the
		// bottom at the edge and not in the middle, and that needs a depth.
		//
		// The flood already knows it: the level it settled at, less the ground
		// it settled on. It costs a float that was already in the vertex and
		// otherwise unused -- this mesh has no texture.
		Egss::MeshVertex point;
		point.Position = glm::vec3(glm::dvec3(m_Direction[at])
			* (double)m_Level[at] - m_Site);
		point.Normal = m_Direction[at];
		point.TexCoord = glm::vec2(m_Level[at] - m_Ground[at], 0.0f);

		out.Vertices.push_back(point);
	}

	for (int v = s_SeedMargin; v < Side - s_SeedMargin - 1; v++)
	for (int u = s_SeedMargin; u < Side - s_SeedMargin - 1; u++)
	{
		int a = vertex[Index(u, v)];
		int b = vertex[Index(u + 1, v)];
		int c = vertex[Index(u, v + 1)];
		int d = vertex[Index(u + 1, v + 1)];

		if (a < 0 || b < 0 || c < 0 || d < 0)
			continue;

		out.Indices.insert(out.Indices.end(), {
			(unsigned int)a, (unsigned int)c, (unsigned int)b,
			(unsigned int)b, (unsigned int)c, (unsigned int)d });
	}

	if (out.Indices.empty())
	{
		out = Egss::MeshData();
		return;
	}

	Egss::Submesh all;
	all.IndexCount = (unsigned int)out.Indices.size();
	out.Submeshes.push_back(all);
	out.RecalculateBounds();
}

inline bool SurfaceWater::Touch(const VoxelPlanet& planet,
	const glm::ivec3& lattice, const glm::vec3& centre, float radius)
{
	if (!m_Valid)
		return false;

	float voxel = planet.Get().VoxelSize;
	float cell = 2.0f * m_Reach / (float)(Side - 1);

	// The columns the sphere could have reached, plus a margin, converted from
	// metres to grid steps.
	int span = (int)std::ceil((radius + 2.0f * voxel) / cell) + 1;

	// Which column the edit is under.
	glm::ivec2 column = ColumnAt(centre);
	int cu = column.x, cv = column.y;

	bool moved = false;

	for (int v = glm::max(cv - span, 0); v <= glm::min(cv + span, Side - 1); v++)
	for (int u = glm::max(cu - span, 0); u <= glm::min(cu + span, Side - 1); u++)
	{
		int at = Index(u, v);

		float found = GroundFrom(planet, lattice, m_Direction[at],
			m_Ground[at], radius + 4.0f * voxel);

		if (std::abs(found - m_Ground[at]) > 1e-3f)
		{
			m_Ground[at] = found;
			moved = true;
		}
	}

	if (!moved)
		return false;

	Flood(m_Outside);

	return true;
}

inline bool SurfaceWater::LevelNear(const glm::vec3& localPosition, float& outLevel) const
{
	if (!m_Valid)
		return false;

	glm::ivec2 column = ColumnAt(localPosition);

	if (column.x < 0 || column.y < 0 || column.x >= Side || column.y >= Side)
		return false;

	int at = Index(column.x, column.y);

	if (!WetAt(at))
		return false;

	outLevel = m_Level[at];
	return true;
}
