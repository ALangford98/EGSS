#pragma once

// The engine's built-in models, made with ModelBuilder, and what their
// materials look like. Reached through MeshCache as "model:<name>", so a
// scene stores one the way it stores "primitive:cube" and loads it the same.
//
//   dummy.head / torso / pelvis / foot          a crash-test dummy, one part
//   dummy.limb:<kind>:<radius>:<halfHeight>      per ragdoll body, each sized
//                                                from that body's collider
//   cannon.base / cannon.yoke / cannon.barrel    the launcher, in its three
//                                                moving parts, and its desk
//   screen                                       a 1 x 1 quad facing +z
//   crate, barrier, padwall, lightpanel, truss,  a test hall
//   wallpanel, pillar, floorgrid, lane, roof
//
// Every material name a model uses has a Surface here: its colour (or a
// tint of the entity's own, for the parts a scene recolours -- the dummy's
// skin), roughness, metalness and glow.

#include "GS/Core.h"
#include "GS/Renderer/Mesh.h"

#include <glm/glm.hpp>
#include <string>

namespace GS::Prefabs {

	struct Surface
	{
		glm::vec4 Color{ 1.0f };
		float Roughness = 0.5f;
		float Metallic = 0.0f;
		float Emission = 0.0f;
		bool Tint = false;     // multiply by the entity's MeshComponent::Color
	};

	// False for a name it does not know.
	GS_API bool Build(const std::string& name, MeshData& out);

	// The surface a submesh material names, or null for one this does not
	// define (a model loaded from a file).
	GS_API const Surface* FindSurface(const std::string& material);

	// The muzzle, measured from the barrel's pivot along its +z.
	constexpr float CannonMuzzle = 1.05f;
	// Where the computer's screen is on cannon.base, in the base's frame:
	// the centre of the glass, its half size, and the way it faces.
	GS_API void CannonScreen(glm::vec3& centre, glm::vec2& half, glm::vec3& facing);

}
