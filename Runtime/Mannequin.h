#pragma once

// What a humanoid looks like: its own bodies, each drawn as the shape that
// collides -- a capsule as a tube and two balls (so the caps stay round at
// any length), a box as a box. The Ragdoll demo has always drawn it so, and
// it is honest: what you see is exactly what hits things. A smooth skinned
// figure needs skeletal skinning, which the engine does not have yet.
//
// In the editor, a humanoid entity is drawn in the rig's rest pose where it
// stands; while a game runs, its live bodies are drawn.

#include <GS.h>

#include "Humanoid.h"
#include "SceneRenderer.h"

#include <cstdio>
#include <vector>

namespace Mannequin {

	inline glm::vec4 Shade(const glm::vec4& colour, float factor)
	{
		return glm::vec4(glm::clamp(glm::vec3(colour) * factor, 0.0f, 1.0f), colour.a);
	}

	// The crash-test dummy's part over one body (GS/Procedural/Prefabs):
	// the model whose frame is the body's, sized from its collider.
	inline std::shared_ptr<GS::Mesh> DummyPart(Humanoid::Part part, const GS::RigidBody3D& body)
	{
		auto limb = [&](const char* kind)
		{
			char key[96];
			std::snprintf(key, sizeof key, "model:dummy.limb:%s:%.3f:%.3f", kind, body.Radius, body.HalfHeight);
			return GS::MeshCache::Get(key);
		};
		switch (part)
		{
			case Humanoid::Part::Head: return GS::MeshCache::Get("model:dummy.head");
			case Humanoid::Part::Torso: return GS::MeshCache::Get("model:dummy.torso");
			case Humanoid::Part::Pelvis: return GS::MeshCache::Get("model:dummy.pelvis");
			case Humanoid::Part::Foot: return GS::MeshCache::Get("model:dummy.foot");
			case Humanoid::Part::Thigh: return limb("thigh");
			case Humanoid::Part::Shin: return limb("shin");
			case Humanoid::Part::UpperArm: return limb("upperarm");
			case Humanoid::Part::Forearm: return limb("forearm");
			default: return nullptr;
		}
	}

	inline void AppendDummy(Humanoid::Part part, const GS::RigidBody3D& body, const glm::vec3& position, const glm::quat& orientation,
		const glm::vec4& colour, GS::EntityId entity, std::vector<ExtraDraw>& out)
	{
		std::shared_ptr<GS::Mesh> mesh = DummyPart(part, body);
		if (!mesh)
			return;
		ExtraDraw item;
		item.Mesh = mesh;
		item.Model = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation);
		item.Color = colour;
		item.Entity = entity;
		item.AllSubmeshes = true;
		out.push_back(item);
	}

	// One body's shape at a pose, as draw items.
	inline void AppendBody(const GS::RigidBody3D& body, const glm::vec3& position, const glm::quat& orientation,
		const glm::vec4& colour, GS::EntityId entity, std::vector<ExtraDraw>& out)
	{
		glm::mat4 frame = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation);
		switch (body.Shape)
		{
			case GS::ColliderShape3D::Capsule:
			{
				float r = body.Radius, h = body.HalfHeight;
				out.push_back({ GS::MeshCache::Get("primitive:cylinder"),
					frame * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f * r, 2.0f * h, 2.0f * r)), colour, entity });
				for (float end : { -h, h })
					out.push_back({ GS::MeshCache::Get("primitive:sphere"),
						frame * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, end, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f * r)),
						colour, entity });
				break;
			}
			case GS::ColliderShape3D::Box:
				out.push_back({ GS::MeshCache::Get("primitive:cube"),
					frame * glm::scale(glm::mat4(1.0f), body.HalfExtents * 2.0f), colour, entity });
				break;
			case GS::ColliderShape3D::Sphere:
				out.push_back({ GS::MeshCache::Get("primitive:sphere"),
					frame * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f * body.Radius)), colour, entity });
				break;
			default:
				break;
		}
	}

	// Head lighter and feet darker than the body's colour, so the figure
	// reads at a glance which way up and which way round it is.
	inline glm::vec4 PartColour(const Humanoid& rig, GS::PhysicsWorld3D::BodyHandle handle, const glm::vec4& colour)
	{
		if (handle == rig.HeadBody())
			return Shade(colour, 1.45f);
		if (rig.IsFoot(handle))
			return Shade(colour, 0.55f);
		return colour;
	}

	// The rig's rest pose, built once into a scratch world standing at the
	// origin facing +z, and kept as each body's shape and pose.
	struct RestBody
	{
		GS::RigidBody3D Body;
		glm::vec4 Shade{ 1.0f };   // multiplier: head, feet
		Humanoid::Part Part = Humanoid::Part::Other;
	};

	inline const std::vector<RestBody>& RestPose()
	{
		static std::vector<RestBody> pose = []()
		{
			std::vector<RestBody> bodies;
			GS::PhysicsWorld3D world;
			Humanoid rig(world);
			rig.Spawn(glm::vec3(0.0f), 0.0f);
			for (GS::PhysicsWorld3D::BodyHandle handle : rig.Bodies())
			{
				RestBody rest;
				rest.Body = world.GetBody(handle);
				float factor = handle == rig.HeadBody() ? 1.45f : rig.IsFoot(handle) ? 0.55f : 1.0f;
				rest.Shade = glm::vec4(factor, factor, factor, 1.0f);
				rest.Part = rig.PartOf(handle);
				bodies.push_back(rest);
			}
			return bodies;
		}();
		return pose;
	}

	// A box round the rest pose, in its own frame (feet at the origin): each
	// body's centre grown by its farthest reach -- loose for a turned box,
	// which is fine for a selection outline.
	inline void RestBounds(glm::vec3& lo, glm::vec3& hi)
	{
		lo = glm::vec3(1e9f);
		hi = glm::vec3(-1e9f);
		for (const RestBody& rest : RestPose())
		{
			const GS::RigidBody3D& b = rest.Body;
			float reach = b.Shape == GS::ColliderShape3D::Capsule ? b.Radius + b.HalfHeight
				: b.Shape == GS::ColliderShape3D::Box ? glm::length(b.HalfExtents) : b.Radius;
			lo = glm::min(lo, b.Position - glm::vec3(reach));
			hi = glm::max(hi, b.Position + glm::vec3(reach));
		}
		lo.y = std::max(lo.y, 0.0f);
	}

	// The rest pose standing where `transform` says -- its position is the
	// ground under the feet, its yaw the way the figure faces.
	inline void AppendRestPose(const GS::TransformComponent& transform, const glm::vec4& colour, GS::EntityId entity,
		std::vector<ExtraDraw>& out, int look = 0)
	{
		glm::quat turn = glm::angleAxis(glm::radians(transform.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
		for (const RestBody& rest : RestPose())
		{
			glm::vec3 at = transform.Position + turn * rest.Body.Position;
			glm::quat facing = turn * rest.Body.Orientation;
			if (look == 1)
				AppendDummy(rest.Part, rest.Body, at, facing, colour, entity, out);
			else
				AppendBody(rest.Body, at, facing, Shade(colour, rest.Shade.x), entity, out);
		}
	}

	inline void AppendLive(const Humanoid& rig, const GS::PhysicsWorld3D& world, const glm::vec4& colour,
		GS::EntityId entity, std::vector<ExtraDraw>& out, int look = 0)
	{
		for (GS::PhysicsWorld3D::BodyHandle handle : rig.Bodies())
		{
			const GS::RigidBody3D& body = world.GetBody(handle);
			if (look == 1)
				AppendDummy(rig.PartOf(handle), body, body.Position, body.Orientation, colour, entity, out);
			else
				AppendBody(body, body.Position, body.Orientation, PartColour(rig, handle, colour), entity, out);
		}
	}

	inline int LookOf(GS::Scene& scene, GS::EntityId entity)
	{
		if (auto* player = scene.GetComponent<GS::PlayerComponent>(entity))
			return player->Look;
		if (auto* npc = scene.GetComponent<GS::NpcComponent>(entity))
			return npc->Look;
		return 0;
	}

	inline glm::vec4 ColourOf(GS::Scene& scene, GS::EntityId entity)
	{
		auto* mesh = scene.GetComponent<GS::MeshComponent>(entity);
		return mesh ? mesh->Color : glm::vec4(0.7f, 0.7f, 0.7f, 1.0f);
	}
}
