#pragma once
// A running game: the physics world built from a scene's PhysicsComponents,
// the GS::ScriptPhysics backend scripts reach it through, every
// ScriptComponent'd entity's script (interpreted through ScriptEngine, or a
// graduated GeneratedScripts::<Name> class driven directly), and the fixed
// tick that advances all of it. It runs on whatever GS::Scene it is given --
// the editor's (PlayMode wraps it with a snapshot and a revert) or the one
// GSPlayer loaded. See
// docs/superpowers/specs/2026-10-07-game-runtime-player-design.md.
//
// Extracted from the editor's PlayMode, whose comments on each step it
// keeps; what stayed behind there is only what is editor-specific.
#include <GS.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "CompiledScriptRegistry.h"
#include "ScriptEngine.h"

#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class GameSession
{
public:
	using Dependencies = std::unordered_map<std::string, std::set<std::string>>;

	// The engine is borrowed, not owned: it is expensive to initialise (the
	// TypeScript compiler loads into it), so the editor keeps one alive
	// across every Play/Stop.
	GameSession(GS::Scene& scene, ScriptEngine& engine) : m_Scene(scene), m_Engine(engine) {}
	~GameSession() { Stop(); }

	GameSession(const GameSession&) = delete;
	GameSession& operator=(const GameSession&) = delete;

	bool IsRunning() const { return m_Running; }

	// Builds every body, installs the physics backend, then prepares and
	// starts every script. `dependencies`, when given, receives each
	// interpreted script's resolved imports (the editor records them).
	void Start(Dependencies* dependencies = nullptr)
	{
		if (m_Running)
			return;

		// Scoped to this one session, not the engine's lifetime (which spans
		// every Play/Stop cycle for the whole app run) -- otherwise editing a
		// module file and pressing Play again would keep running the stale
		// cached copy forever.
		m_Engine.ClearModuleCache();

		// Every body exists before any script starts: an OnStart that pushes
		// *another* entity must find its body whichever order the scene lists
		// them in -- so two passes, bodies then scripts.
		for (GS::EntityId id : m_Scene.GetEntities())
		{
			if (auto* transform = m_Scene.GetComponent<GS::TransformComponent>(id))
				if (auto* physics = m_Scene.GetComponent<GS::PhysicsComponent>(id))
				{
					glm::vec3 halfExtents;
					if (auto* mesh = m_Scene.GetComponent<GS::MeshComponent>(id); mesh && mesh->Geometry)
						halfExtents = (mesh->Geometry->GetBoundsMax() - mesh->Geometry->GetBoundsMin()) * 0.5f * transform->Scale;
					else
						halfExtents = transform->Scale * 0.5f;

					GS::RigidBody3D body = (physics->Type == GS::BodyType::Dynamic)
						? GS::RigidBody3D::MakeBox(transform->Position, halfExtents, physics->Mass)
						: GS::RigidBody3D::MakeStaticBox(transform->Position, halfExtents);
					if (physics->Type == GS::BodyType::Kinematic)
						body.Type = GS::BodyType::Kinematic;

					// Matches TransformComponent::GetTransform()'s own
					// rotation composition (Rx * Ry * Rz, degrees) exactly,
					// so a physics body starts at the same orientation the
					// object was already rendering at.
					body.Orientation = glm::quat_cast(glm::mat3(
						glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.x), glm::vec3(1, 0, 0)) *
						glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.y), glm::vec3(0, 1, 0)) *
						glm::rotate(glm::mat4(1.0f), glm::radians(transform->Rotation.z), glm::vec3(0, 0, 1))));
					body.Friction = physics->Friction;
					body.Restitution = physics->Restitution;

					GS::PhysicsWorld3D::BodyHandle handle = m_World.AddBody(body);
					m_Bodies[id] = handle;
					m_BodyOwners[handle] = id;
					m_LastSynced[id] = transform->Position;
				}
		}
		GS::ScriptPhysics::SetBackend(&m_Backend);

		for (GS::EntityId id : m_Scene.GetEntities())
		{
			if (!m_Scene.HasComponent<GS::ScriptComponent>(id))
				continue;

			auto* script = m_Scene.GetComponent<GS::ScriptComponent>(id);
			if (script->ScriptPath.empty())
				continue;

			// Graduated scripts never touch the .gss file at all -- that's
			// the point of graduating: the native class is the real behavior
			// from here on, and a stale or deleted .gss no longer matters for
			// this entity.
			if (const CompiledScriptEntry* entry = FindCompiledScript(ScriptEngine::ClassNameFromPath(script->ScriptPath)))
			{
				CompiledInstance compiled;
				compiled.Entry = entry;
				compiled.Instance = entry->Create();
				entry->CallOnStart(compiled.Instance.get(), GS::Entity(&m_Scene, id), m_Scene);
				m_Compiled[id] = compiled;
				continue;
			}

			std::ifstream file(GS::Assets::Resolve(script->ScriptPath), std::ios::in | std::ios::binary);
			if (!file.is_open())
			{
				GS_ERROR("GameSession: could not open script '{0}' for entity {1}", script->ScriptPath, id);
				continue;
			}
			std::stringstream buffer;
			buffer << file.rdbuf();

			ScriptEngine::PreparedScript prepared;
			std::string error;
			std::set<std::string> imports;
			if (!m_Engine.PrepareEntityScript(&m_Scene, id, script->ScriptPath, buffer.str(), error, prepared, &imports))
			{
				GS_ERROR("GameSession: entity {0}'s script failed to prepare: {1}", id, error);
				continue;
			}
			if (dependencies && !imports.empty())
				(*dependencies)[script->ScriptPath] = imports;

			m_Engine.CallOnStart(prepared);
			m_Prepared[id] = prepared;
		}

		m_Running = true;
	}

	// Releases every script and removes the physics backend. Leaves the
	// scene as the game left it -- reverting it is the editor's business.
	void Stop()
	{
		if (!m_Running)
			return;

		GS::ScriptPhysics::SetBackend(nullptr);

		for (auto& pair : m_Prepared)
			m_Engine.ReleasePreparedScript(pair.second);
		m_Prepared.clear();

		// No JSValues to release here -- a shared_ptr<void>'s deleter (set
		// up inside MakeCompiledScriptEntry's Create) already knows how to
		// destroy the real T, so clear() alone is enough.
		m_Compiled.clear();

		m_Running = false;
	}

	void FixedUpdate(float dt)
	{
		if (!m_Running)
			return;

		// A transform that no longer matches what the last tick wrote was
		// moved by something else -- a script's setPosition -- so the body
		// is teleported there rather than the copy-back undoing the move.
		for (auto& [id, handle] : m_Bodies)
		{
			if (!m_Scene.IsValid(id))
				continue;
			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(id);
			if (!transform || transform->Position == m_LastSynced[id])
				continue;
			GS::RigidBody3D& body = m_World.GetBody(handle);
			body.Position = transform->Position;
			body.PreviousPosition = transform->Position;
			body.Awake = true;
			body.SleepTimer = 0.0f;
		}

		m_World.Step(dt);

		for (auto& [id, handle] : m_Bodies)
		{
			if (!m_Scene.IsValid(id))
				continue;   // a script could destroy a physics entity mid-game

			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(id);
			if (!transform)
				continue;

			const GS::RigidBody3D& body = m_World.GetBody(handle);
			transform->Position = body.Position;
			m_LastSynced[id] = body.Position;

			float x, y, z;
			glm::extractEulerAngleXYZ(glm::mat4_cast(body.Orientation), x, y, z);
			transform->Rotation = glm::degrees(glm::vec3(x, y, z));
		}

		// scene.destroy() can remove any entity, including one with its own
		// running script -- self-destruction, or another entity destroying
		// it (a ball clearing the brick it just hit). Skip ticking one whose
		// id is no longer valid, and drop it (releasing its JSValues) so it
		// isn't checked forever. Checked before each call rather than once
		// up front, so an entity destroyed earlier in *this* tick doesn't
		// also get ticked once more before its removal is noticed.
		std::vector<GS::EntityId> stale;
		for (auto& pair : m_Prepared)
		{
			if (!m_Scene.IsValid(pair.first))
			{
				stale.push_back(pair.first);
				continue;
			}
			m_Engine.CallOnUpdate(pair.second, dt);
		}
		for (GS::EntityId id : stale)
		{
			m_Engine.ReleasePreparedScript(m_Prepared[id]);
			m_Prepared.erase(id);
		}

		// The same for compiled scripts, which can call scene.destroy() just
		// as an interpreted one can.
		std::vector<GS::EntityId> staleCompiled;
		for (auto& pair : m_Compiled)
		{
			if (!m_Scene.IsValid(pair.first))
			{
				staleCompiled.push_back(pair.first);
				continue;
			}
			pair.second.Entry->CallOnUpdate(pair.second.Instance.get(), GS::Entity(&m_Scene, pair.first), m_Scene, (double)dt);
		}
		for (GS::EntityId id : staleCompiled)
			m_Compiled.erase(id);
	}

private:
	struct CompiledInstance
	{
		const CompiledScriptEntry* Entry = nullptr;
		std::shared_ptr<void> Instance;
	};

	// Scripts' applyImpulse/applyForce/get/setVelocity/isTouching land here,
	// through GS::ScriptPhysics, installed for exactly the life of a session.
	struct PhysicsBackend : GS::ScriptPhysics::Backend
	{
		GameSession& Session;
		explicit PhysicsBackend(GameSession& session) : Session(session) {}

		GS::RigidBody3D* Body(GS::EntityId entity, GS::PhysicsWorld3D::BodyHandle* handle = nullptr)
		{
			auto found = Session.m_Bodies.find(entity);
			if (found == Session.m_Bodies.end() || !Session.m_Scene.IsValid(entity))
				return nullptr;
			if (handle)
				*handle = found->second;
			return &Session.m_World.GetBody(found->second);
		}

		bool ApplyImpulse(GS::EntityId entity, const glm::vec3& impulse) override
		{
			GS::PhysicsWorld3D::BodyHandle handle;
			if (!Body(entity, &handle))
				return false;
			Session.m_World.ApplyImpulse(handle, impulse);   // ignores static bodies, wakes the rest
			return true;
		}

		bool ApplyForce(GS::EntityId entity, const glm::vec3& force) override
		{
			GS::PhysicsWorld3D::BodyHandle handle;
			if (!Body(entity, &handle))
				return false;
			Session.m_World.ApplyForce(handle, force);
			return true;
		}

		bool GetVelocity(GS::EntityId entity, glm::vec3& out) override
		{
			GS::RigidBody3D* body = Body(entity);
			if (!body)
				return false;
			out = body->Velocity;
			return true;
		}

		bool SetVelocity(GS::EntityId entity, const glm::vec3& velocity) override
		{
			GS::RigidBody3D* body = Body(entity);
			if (!body)
				return false;
			if (body->Type == GS::BodyType::Static)
				return true;
			body->Velocity = velocity;
			// A sleeping body is skipped by integration entirely, so without
			// this a box at rest would ignore the call.
			body->Awake = true;
			body->SleepTimer = 0.0f;
			return true;
		}

		bool IsTouching(GS::EntityId entity, const std::string& tag, bool& out) override
		{
			GS::PhysicsWorld3D::BodyHandle handle;
			if (!Body(entity, &handle))
				return false;
			out = false;
			for (const GS::Contact3D& contact : Session.m_World.GetContacts())
			{
				if (contact.A != handle && contact.B != handle)
					continue;
				auto owner = Session.m_BodyOwners.find(contact.A == handle ? contact.B : contact.A);
				if (owner == Session.m_BodyOwners.end() || !Session.m_Scene.IsValid(owner->second))
					continue;
				const GS::TagComponent* other = Session.m_Scene.GetComponent<GS::TagComponent>(owner->second);
				if (other && other->Name == tag)
				{
					out = true;
					break;
				}
			}
			return true;
		}
	};

	GS::Scene& m_Scene;
	ScriptEngine& m_Engine;
	bool m_Running = false;

	GS::PhysicsWorld3D m_World;
	std::unordered_map<GS::EntityId, GS::PhysicsWorld3D::BodyHandle> m_Bodies;
	// Body index -> owning entity, for turning a contact back into a tag.
	std::unordered_map<GS::PhysicsWorld3D::BodyHandle, GS::EntityId> m_BodyOwners;
	// The position the last tick wrote into each physics entity's transform.
	std::unordered_map<GS::EntityId, glm::vec3> m_LastSynced;

	std::unordered_map<GS::EntityId, ScriptEngine::PreparedScript> m_Prepared;
	std::unordered_map<GS::EntityId, CompiledInstance> m_Compiled;

	PhysicsBackend m_Backend{ *this };
};
