#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Texture.h"
#include "Egss/Renderer/Mesh.h"
#include "Egss/Renderer/Material.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Egss {

	// The components the engine itself knows about. Game-specific ones live in
	// the game -- any struct can be a component, there is nothing to register.

	struct TagComponent
	{
		std::string Name;
	};

	// Where a thing is. Almost every other system reads this one, which is why
	// it is stored as parts rather than a matrix: a matrix is easy to build
	// from position/rotation/scale and painful to pull them back out of.
	struct TransformComponent
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		// Degrees, applied X then Y then Z.
		glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		glm::mat4 GetTransform() const
		{
			// Right to left: scale, then rotate, then translate.
			return glm::translate(glm::mat4(1.0f), Position)
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.x), glm::vec3(1, 0, 0))
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.y), glm::vec3(0, 1, 0))
				* glm::rotate(glm::mat4(1.0f), glm::radians(Rotation.z), glm::vec3(0, 0, 1))
				* glm::scale(glm::mat4(1.0f), Scale);
		}
	};

	struct SpriteComponent
	{
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::shared_ptr<Texture2D> Texture;
		float TilingFactor = 1.0f;
	};

	// Links an entity to a body in a PhysicsWorld2D.
	//
	// The body is *not* stored here. The physics world owns its bodies and
	// knows nothing about entities -- this is only the handle that joins the
	// two, which is what lets the physics world stay a standalone thing that
	// could be swapped for Box2D without the scene noticing.
	struct RigidBody2DComponent
	{
		unsigned int Body = ~0u;
		// When false the transform drives the body; otherwise the body drives
		// the transform. Static scenery usually wants the former.
		bool DrivenByPhysics = true;
	};

	// 3D geometry. The mesh is shared rather than owned: a hundred entities
	// pointing at one Mesh cost one copy of the geometry on the GPU. That is
	// the whole reason the transform lives on the entity and not in the mesh.
	struct MeshComponent
	{
		// Named Geometry, not Mesh -- a member cannot share its name with the
		// type it is declared from inside the same struct.
		std::shared_ptr<Egss::Mesh> Geometry;

		// One material per submesh, in the same order as Geometry's. A model
		// whose file switched material partway through needs one each, so this
		// is a vector rather than the single material it started as -- a mesh
		// with one submesh simply has one entry.
		//
		// May be shorter than the submesh count, or empty: the renderer fills
		// the gap. An entity with geometry and no material yet is a normal
		// thing to have while a scene is being built.
		std::vector<std::shared_ptr<Egss::Material>> Materials;

		// Whether the materials above came from the model's own .mtl. When they
		// did, the renderer must leave their colour alone -- overwriting it
		// with Color would throw away the thing the file was loaded for. When
		// they did not, Color drives them, which is what lets the panel recolour
		// a primitive.
		bool MaterialsFromFile = false;

		// Kept alongside the materials rather than folded into them. Colour is
		// the one thing every scene wants per object, and requiring a material
		// for it would mean building one before anything could be seen at all.
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool Visible = true;
	};

	struct LightComponent
	{
		glm::vec4 Color = { 1.0f, 0.95f, 0.8f, 1.0f };
		float Radius = 2.0f;
		bool Enabled = true;
	};

}
