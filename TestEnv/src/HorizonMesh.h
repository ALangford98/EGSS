#pragma once

#include <Egss.h>

#include "VoxelPlanet.h"

#include <cmath>
#include <memory>
#include <vector>

// **Past the streaming radius, the world was a smooth ball.**
//
// Voxel chunks are generated within `m_LoadRadius` of the camera -- four
// hundred metres, because a chunk is 4,096 evaluations of the density and the
// count goes as the cube of the reach. Everything beyond that was drawn as the
// planet's stand-in sphere, which is exactly what its name says: a sphere. It
// is inset below the deepest valley so real terrain always wins the depth test
// where real terrain exists, and with the landscape layer added that inset is
// **879 m**. So the ground you stand on ran out at four hundred metres and the
// horizon was a smooth ball most of a kilometre below your feet.
//
// That is why the first landings read as flat however much relief was put into
// the generator. A mountain 300 m high is visible from
// `sqrt(2 R h) = 12.2 km` away on this planet -- forty times further than
// anything was being drawn. The relief existed; there was no geometry to carry
// it.
//
// So: one mesh, a polar grid about the landing site, from just inside the
// streamed region out to fourteen kilometres, displaced by the same `Relief`
// the voxels are generated from. It is not a level of detail *of* the chunks --
// it does not replace them, and where they exist they are in front of it. It
// is the rest of the world.
//
// **Why a polar grid and not a square one.** The thing being drawn is a disc
// around the viewer, and what matters is the angle a triangle subtends, not
// the ground it covers. Rings spaced geometrically put 16 m between samples at
// the near edge and 665 m at the far one, which is the same handful of
// arcminutes at both -- so the near ground is as detailed as the chunks beside
// it and the far ground costs nothing. A square grid fine enough for the near
// edge would be four hundred times the vertices to cover the same disc.
//
// **The droop.** Where the mesh and the chunks overlap they are drawing the
// same surface from the same function, so they z-fight. Rather than trimming
// the overlap -- which is a moving, ragged boundary, because the streamed
// region is a sphere cutting a landscape -- the horizon is sunk by four
// millimetres per metre of distance from the inner edge. That is 1.2 m under
// the chunks at the seam, where a metre is 0.17 degrees, and 55 m at fourteen
// kilometres, where it is 0.22 degrees. Neither is visible; both guarantee the
// real geometry wins.
class HorizonMesh
{
public:
	bool Valid() const { return m_Mesh != nullptr; }

	const std::shared_ptr<Egss::Mesh>& Mesh() const { return m_Mesh; }

	// The site the grid is centred on, in the planet's own frame. Every vertex
	// is stored relative to it, so nothing in the buffer is a planet-sized
	// float.
	const glm::dvec3& Site() const { return m_Site; }

	float Outer() const { return m_Outer; }

	void Build(const VoxelPlanet& planet, const glm::dvec3& site,
		float inner, float outer);

	void Report() const;

private:
	// 96 rings by 192 spokes: 18,432 vertices and 36,480 triangles, which is
	// about three chunks' worth of geometry for the other thirteen and a half
	// kilometres of the world.
	static constexpr int s_Rings = 96;
	static constexpr int s_Spokes = 192;

	// Metres of sink per metre out from the inner edge. See the note above.
	static constexpr float s_Droop = 0.004f;

	std::shared_ptr<Egss::Mesh> m_Mesh;
	glm::dvec3 m_Site { 0.0 };

	float m_Inner = 0.0f;
	float m_Outer = 0.0f;

	// Measured in Build, reported by Report: what the mesh says the ground is
	// against what the generator says, at the seam where the chunks end.
	float m_SeamError = 0.0f;
	float m_SeamDroop = 0.0f;
	int m_Vertices = 0;
	int m_Triangles = 0;
};

inline void HorizonMesh::Build(const VoxelPlanet& planet, const glm::dvec3& site,
	float inner, float outer)
{
	m_Mesh = nullptr;
	m_Site = site;
	m_Inner = inner;
	m_Outer = outer;

	double length = glm::length(site);

	if (length < 1e-3 || outer <= inner)
		return;

	const VoxelPlanet::Settings& settings = planet.Get();

	float radius = glm::max(settings.Radius, 1.0f);

	glm::vec3 n = glm::vec3(site / length);

	glm::vec3 pick = std::abs(n.y) < 0.9f
		? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

	glm::vec3 t = glm::normalize(glm::cross(pick, n));
	glm::vec3 b = glm::cross(n, t);

	Egss::MeshData data;

	data.Vertices.reserve((size_t)s_Rings * s_Spokes);
	data.Indices.reserve((size_t)(s_Rings - 1) * s_Spokes * 6);

	// Geometric, so a ring is a constant fraction further out than the one
	// inside it and every quad subtends about the same angle.
	float growth = std::pow(outer / inner, 1.0f / (float)(s_Rings - 1));

	std::vector<glm::dvec3> position((size_t)s_Rings * s_Spokes);

	for (int ring = 0; ring < s_Rings; ring++)
	{
		float ground = inner * std::pow(growth, (float)ring);

		// The ground distance is an arc, not a chord: at fourteen kilometres
		// on a 250 km planet the two differ by 3.7 m, which is more than the
		// droop that keeps the chunks in front.
		float angle = ground / radius;

		float cosine = std::cos(angle);
		float sine = std::sin(angle);

		float droop = s_Droop * (ground - inner);

		for (int spoke = 0; spoke < s_Spokes; spoke++)
		{
			float azimuth = 6.283185307179586f * (float)spoke / (float)s_Spokes;

			glm::vec3 out = t * std::cos(azimuth) + b * std::sin(azimuth);

			glm::vec3 direction = glm::normalize(n * cosine + out * sine);

			double height = (double)radius + (double)planet.Relief(direction)
				- (double)droop;

			position[(size_t)ring * s_Spokes + spoke] =
				glm::dvec3(direction) * height;
		}
	}

	// Normals from the grid rather than from the density: a difference of two
	// neighbours is the surface this mesh actually has, and the gradient of the
	// field is the surface the chunks have. Where they disagree -- and at 665 m
	// between samples on the far rings they disagree a great deal -- the one
	// that shades correctly is the one that matches the triangles.
	for (int ring = 0; ring < s_Rings; ring++)
	{
		for (int spoke = 0; spoke < s_Spokes; spoke++)
		{
			size_t here = (size_t)ring * s_Spokes + spoke;

			size_t along = (size_t)ring * s_Spokes
				+ (spoke + 1) % s_Spokes;

			size_t outward = (size_t)glm::min(ring + 1, s_Rings - 1) * s_Spokes
				+ spoke;

			size_t inward = (size_t)glm::max(ring - 1, 0) * s_Spokes + spoke;

			glm::vec3 da = glm::vec3(position[along] - position[here]);
			glm::vec3 dr = glm::vec3(position[outward] - position[inward]);

			glm::vec3 normal = glm::cross(dr, da);

			float size = glm::length(normal);

			// Degenerate only where the two differences are parallel, which
			// happens on the innermost and outermost rings if the terrain is
			// exactly flat. Radially outward is the right answer there.
			normal = size > 1e-8f
				? normal / size
				: glm::vec3(glm::normalize(position[here]));

			// Outward, not inward: which way `cross` points depends on the
			// handedness of the frame, and this is cheaper than reasoning
			// about it.
			if (glm::dot(normal, glm::vec3(glm::normalize(position[here]))) < 0.0f)
				normal = -normal;

			Egss::MeshVertex vertex;
			vertex.Position = glm::vec3(position[here] - site);
			vertex.Normal = normal;
			vertex.TexCoord = glm::vec2(0.0f);

			data.Vertices.push_back(vertex);
		}
	}

	for (int ring = 0; ring + 1 < s_Rings; ring++)
	{
		for (int spoke = 0; spoke < s_Spokes; spoke++)
		{
			unsigned int a = (unsigned int)(ring * s_Spokes + spoke);
			unsigned int b2 = (unsigned int)(ring * s_Spokes + (spoke + 1) % s_Spokes);
			unsigned int c = (unsigned int)((ring + 1) * s_Spokes + spoke);
			unsigned int d = (unsigned int)((ring + 1) * s_Spokes
				+ (spoke + 1) % s_Spokes);

			data.Indices.push_back(a);
			data.Indices.push_back(c);
			data.Indices.push_back(b2);

			data.Indices.push_back(b2);
			data.Indices.push_back(c);
			data.Indices.push_back(d);
		}
	}

	m_Vertices = (int)data.Vertices.size();
	m_Triangles = (int)(data.Indices.size() / 3);

	// **The check.** On the innermost ring the droop is zero, so the mesh
	// should agree with the generator exactly -- any difference here is an
	// error in the parameterisation rather than a deliberate offset. On the
	// outermost ring it should be exactly the droop.
	m_SeamError = 0.0f;

	for (int spoke = 0; spoke < s_Spokes; spoke++)
	{
		glm::dvec3 at = position[(size_t)spoke];

		double have = glm::length(at);

		glm::vec3 direction = glm::vec3(at / have);

		double want = (double)radius + (double)planet.Relief(direction);

		m_SeamError = glm::max(m_SeamError, (float)std::abs(want - have));
	}

	m_SeamDroop = s_Droop * (outer - inner);

	m_Mesh = std::make_shared<Egss::Mesh>(data, "HorizonTerrain");
}

inline void HorizonMesh::Report() const
{
	if (!m_Mesh)
	{
		EGSS_TRACE("Horizon: none");
		return;
	}

	EGSS_TRACE("Horizon: {0} m to {1} m, {2} vertices, {3} triangles; "
		"seam agrees with the generator to {4:.4f} m, far edge sunk {5:.1f} m",
		(int)m_Inner, (int)m_Outer, m_Vertices, m_Triangles,
		m_SeamError, m_SeamDroop);
}
