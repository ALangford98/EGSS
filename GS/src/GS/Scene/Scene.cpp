#include "gspch.h"
#include "GS/Scene/Scene.h"

#include "GS/Debug/Instrumentor.h"
#include "GS/Renderer/MeshCache.h"
#include "GS/Log.h"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace GS {

	Entity Scene::CreateEntity(const std::string& name)
	{
		unsigned int index;

		// Reuse a freed slot if there is one, so the generation array does not
		// grow without bound in a scene that churns entities.
		if (!m_FreeSlots.empty())
		{
			index = m_FreeSlots.back();
			m_FreeSlots.pop_back();
		}
		else
		{
			index = (unsigned int)m_Generations.size();
			// Generation starts at 1, so a valid handle is never 0 and
			// InvalidEntity stays unambiguous.
			m_Generations.push_back(1);
		}

		EntityId id = EntityIds::Make(index, m_Generations[index]);

		m_Live.push_back(id);
		m_LiveCount++;

		AddComponent<TagComponent>(id, { name });
		AddComponent<TransformComponent>(id, {});

		return Entity(this, id);
	}

	void Scene::DestroyEntity(EntityId entity)
	{
		if (!IsValid(entity))
			return;

		unsigned int index = EntityIds::Index(entity);

		// Bumping the generation is what makes every outstanding handle to
		// this entity stale, including ones the caller has stored elsewhere.
		m_Generations[index]++;
		if (m_Generations[index] > EntityIds::MaxGeneration)
			m_Generations[index] = 1;   // wrap; aliasing is now possible but 4096 reuses away

		m_FreeSlots.push_back(index);

		// Every store, without knowing what any of them hold -- the reason
		// IComponentStore exists.
		for (auto& [type, store] : m_Stores)
			store->Remove(entity);

		m_Live.erase(std::remove(m_Live.begin(), m_Live.end(), entity), m_Live.end());
		m_LiveCount--;
	}

	void Scene::Clear()
	{
		m_Stores.clear();
		m_Generations.clear();
		m_FreeSlots.clear();
		m_Live.clear();
		m_LiveCount = 0;
		m_Physics.Clear();
	}

	bool Scene::IsValid(EntityId entity) const
	{
		if (entity == InvalidEntity)
			return false;

		unsigned int index = EntityIds::Index(entity);
		if (index >= m_Generations.size())
			return false;

		// The slot exists, but is this the entity that currently owns it?
		return m_Generations[index] == EntityIds::Generation(entity);
	}

	Entity Scene::Wrap(EntityId entity)
	{
		return Entity(this, entity);
	}

	EntityId Scene::EntityAtIndex(unsigned int index) const
	{
		if (index >= m_Generations.size())
			return InvalidEntity;

		EntityId candidate = EntityIds::Make(index, m_Generations[index]);

		// The slot may have been freed without being reused, in which case the
		// generation is valid but no entity is live there.
		return IsValid(candidate) ? candidate : InvalidEntity;
	}

	void Scene::StepPhysics(float fixedStep)
	{
		GS_PROFILE_SCOPE("Scene::StepPhysics");

		ComponentStore<RigidBody2DComponent>& bodies = View<RigidBody2DComponent>();

		// Transforms that drive their body are pushed in *before* the step, or
		// the step would immediately overwrite them.
		for (size_t i = 0; i < bodies.Size(); i++)
		{
			RigidBody2DComponent& link = bodies.Components()[i];
			if (link.DrivenByPhysics)
				continue;

			TransformComponent* transform = GetComponent<TransformComponent>(bodies.Owner(i));
			if (!transform || link.Body >= m_Physics.GetBodyCount())
				continue;

			m_Physics.GetBody(link.Body).Position = glm::vec2(transform->Position);
		}

		m_Physics.Step(fixedStep);

		// ...and physics-driven bodies are read back out afterwards.
		for (size_t i = 0; i < bodies.Size(); i++)
		{
			RigidBody2DComponent& link = bodies.Components()[i];
			if (!link.DrivenByPhysics)
				continue;

			TransformComponent* transform = GetComponent<TransformComponent>(bodies.Owner(i));
			if (!transform || link.Body >= m_Physics.GetBodyCount())
				continue;

			const RigidBody2D& body = m_Physics.GetBody(link.Body);
			transform->Position = glm::vec3(body.Position, transform->Position.z);
		}
	}

	static void WriteFloats(std::ofstream& out, std::initializer_list<float> values)
	{
		for (float v : values)
			out << ' ' << v;
	}

	bool Scene::Save(const std::string& path) const
	{
		std::ofstream out(path);
		if (!out)
		{
			GS_WARN("Scene::Save: could not write '{0}'", path);
			return false;
		}

		out << "gs-scene 1\n";

		// A float needs 9 significant digits (max_digits10) to round-trip
		// exactly; the default stream precision is 6, which silently drops
		// bits for anything that isn't a round number. Set once, before any
		// float is written, rather than per-value in WriteFloats.
		out.precision(9);

		Scene* self = const_cast<Scene*>(this);

		for (EntityId entity : m_Live)
		{
			out << "entity\n";

			if (TagComponent* tag = self->GetComponent<TagComponent>(entity))
				out << "tag " << tag->Name << "\n";

			if (TransformComponent* t = self->GetComponent<TransformComponent>(entity))
			{
				out << "transform";
				WriteFloats(out, { t->Position.x, t->Position.y, t->Position.z,
					t->Rotation.x, t->Rotation.y, t->Rotation.z,
					t->Scale.x, t->Scale.y, t->Scale.z });
				out << "\n";
			}

			// A mesh with no SourcePath was built ad hoc and has nothing to
			// reload from -- silently dropped rather than written as a
			// reference nothing can follow.
			if (MeshComponent* mesh = self->GetComponent<MeshComponent>(entity))
			{
				if (!mesh->SourcePath.empty())
				{
					out << "mesh " << mesh->SourcePath;
					WriteFloats(out, { mesh->Color.r, mesh->Color.g, mesh->Color.b, mesh->Color.a });
					out << ' ' << (mesh->Visible ? 1 : 0) << "\n";
				}
			}

			if (CameraComponent* camera = self->GetComponent<CameraComponent>(entity))
			{
				out << "camera";
				WriteFloats(out, { camera->Fov, camera->NearClip, camera->FarClip });
				out << ' ' << (camera->Active ? 1 : 0) << "\n";
			}

			// A script with no path was added but not yet pointed at a file --
			// silently dropped, same reasoning the mesh block already uses for
			// an empty SourcePath.
			if (ScriptComponent* script = self->GetComponent<ScriptComponent>(entity))
			{
				if (!script->ScriptPath.empty())
					out << "script " << script->ScriptPath << "\n";
			}

			if (LightComponent* light = self->GetComponent<LightComponent>(entity))
			{
				out << "light";
				WriteFloats(out, { light->Color.r, light->Color.g, light->Color.b, light->Color.a, light->Radius });
				out << ' ' << (light->Enabled ? 1 : 0) << "\n";
			}
		}

		return true;
	}

	bool Scene::Load(const std::string& path)
	{
		std::ifstream in(path);
		if (!in)
		{
			GS_WARN("Scene::Load: could not read '{0}'", path);
			return false;
		}

		std::string tag;
		int version = 0;
		in >> tag >> version;

		if (tag != "gs-scene")
		{
			// A version number would be misleading here -- a file that isn't a
			// gs-scene file at all can still parse a "1" into `version` by
			// coincidence, which would print "is version 1; this build reads 1"
			// and read as a version problem rather than the wrong file.
			GS_WARN("Scene::Load: '{0}' is not a gs-scene file (found tag '{1}')", path, tag);
			Clear();
			return false;
		}

		if (version != 1)
		{
			GS_WARN("Scene::Load: '{0}' is version {1}; this build reads 1", path, version);
			Clear();
			return false;
		}

		std::string line;
		std::getline(in, line);   // rest of the header line

		Clear();
		Entity current;

		while (std::getline(in, line))
		{
			std::istringstream fields(line);
			std::string kind;
			fields >> kind;

			if (kind == "entity")
			{
				current = CreateEntity();
			}
			else if (kind == "tag" && current)
			{
				std::string name;
				std::getline(fields, name);
				size_t from = name.find_first_not_of(' ');
				current.Get<TagComponent>()->Name = from == std::string::npos ? "" : name.substr(from);
			}
			else if (kind == "transform" && current)
			{
				TransformComponent t;
				fields >> t.Position.x >> t.Position.y >> t.Position.z
					>> t.Rotation.x >> t.Rotation.y >> t.Rotation.z
					>> t.Scale.x >> t.Scale.y >> t.Scale.z;
				*current.Get<TransformComponent>() = t;
			}
			else if (kind == "mesh" && current)
			{
				std::string sourcePath;
				fields >> sourcePath;

				MeshComponent mesh;
				mesh.SourcePath = sourcePath;
				fields >> mesh.Color.r >> mesh.Color.g >> mesh.Color.b >> mesh.Color.a;
				int visible = 1;
				fields >> visible;
				mesh.Visible = visible != 0;
				mesh.Geometry = MeshCache::Get(sourcePath);

				current.Add<MeshComponent>(mesh);
			}
			else if (kind == "camera" && current)
			{
				CameraComponent camera;
				int active = 0;
				fields >> camera.Fov >> camera.NearClip >> camera.FarClip >> active;
				camera.Active = active != 0;
				current.Add<CameraComponent>(camera);
			}
			else if (kind == "script" && current)
			{
				std::string path;
				fields >> path;
				current.Add<ScriptComponent>(ScriptComponent{ path });
			}
			else if (kind == "light" && current)
			{
				LightComponent light;
				int enabled = 1;
				fields >> light.Color.r >> light.Color.g >> light.Color.b >> light.Color.a
					>> light.Radius >> enabled;
				light.Enabled = enabled != 0;
				current.Add<LightComponent>(light);
			}
		}

		return true;
	}

}
