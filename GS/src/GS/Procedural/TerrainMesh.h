#pragma once

// Terrain on a primitive shape (TerrainComponent): the shape rebuilt with as
// many vertices as asked for, then its surface pushed in and out by fractal
// noise. A plane rises along +Y -- hills -- and every other shape pushes
// outward from its centre (from its axis, for a cylinder or capsule): a
// sphere becomes a small planet, a cube a rock.
//
// Outward-from-the-centre rather than along each vertex's normal is what
// keeps the surface whole. A cube's edge and a sphere's seam are made of
// duplicated vertices -- one point, two normals -- and displacing along the
// normal would move the two copies apart and crack the shape open. The
// direction from the centre is the same for both copies, and so is the
// noise, which is sampled at the point itself.

#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"
#include "GS/Scene/Components.h"

#include <memory>
#include <string>

namespace GS::TerrainMesh {

	// The primitives terrain applies to. A loaded model is not one of them:
	// its vertex count is the file's, and there is no surface to rebuild.
	GS_API bool Supports(const std::string& sourcePath);

	// Everything the geometry depends on, as text: a cache key, and what
	// TerrainComponent::BuiltKey is compared against.
	GS_API std::string Key(const std::string& sourcePath, const TerrainComponent& terrain);

	// The displacement at a point in the shape's own space: Height times
	// normalised fractal noise, so within [-Height, Height]. For a plane
	// that is the height above y = 0 at (x, z) -- what the physics
	// heightfield samples.
	GS_API float Displacement(const TerrainComponent& terrain, const glm::vec3& point);

	GS_API MeshData BuildData(const std::string& sourcePath, const TerrainComponent& terrain);

	// Built once per distinct key and shared, as MeshCache shares
	// primitives. An unsupported source gives MeshCache's plain mesh.
	GS_API std::shared_ptr<Mesh> Get(const std::string& sourcePath, const TerrainComponent& terrain);
}
