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

#include "CharacterController.h"
#include "GameConsole.h"
#include "GameModes.h"
#include "GameUI.h"
#include "Launcher.h"
#include "SceneRenderer.h"
#include "CompiledScriptRegistry.h"
#include "ScriptEngine.h"

#include <GS/Procedural/TerrainMesh.h>

#include <filesystem>
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

	// The running humanoids, as draw items for SceneRenderer -- and what
	// the game draws that is not an entity: the cannon's trajectory.
	void AppendCharacterDraws(std::vector<ExtraDraw>& out) const
	{
		m_Characters.AppendDraws(m_Scene, m_World, out);
		if (m_Running)
			m_Modes.AppendDraws(out);
	}

	bool IsRunningCharacter(GS::EntityId entity) const { return m_Characters.IsRunning(entity); }

	// Entities the game made for itself (the follow camera), which an editor
	// keeping the scene as Play left it should not keep.
	std::vector<GS::EntityId> CreatedEntities() const
	{
		std::vector<GS::EntityId> created;
		if (m_Characters.CreatedCamera())
			created.push_back(m_Characters.CameraEntity());
		for (GS::EntityId projectile : m_Launchers.Projectiles())
			created.push_back(projectile);
		return created;
	}

	// The game console, drawn by the host over the game's view, in ImGui's
	// screen coordinates. Called inside the host's ImGui frame.
	void DrawOverlay(float x, float y, float width, float height)
	{
		if (!m_Running)
			return;
		m_Modes.Frame(x, y, width, height);
		m_Console.Draw(x, y, width, height);
		GS::ImGuiLayer::UploadTextures();
	}

	GameConsole& Console() { return m_Console; }
	GameModes& Modes() { return m_Modes; }
	LauncherSystem& Launchers() { return m_Launchers; }
	GS::PhysicsWorld3D& World() { return m_World; }
	GameUI::Context& UI() { return m_Modes.UI(); }
	// A running humanoid by its entity's name, or null: for a host with
	// commands of its own about one (the parkour course's `station`).
	Humanoid* Rig(const std::string& name) { return FindRig(name); }

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
			auto* transform = m_Scene.GetComponent<GS::TransformComponent>(id);
			if (!transform)
				continue;
			auto* physics = m_Scene.GetComponent<GS::PhysicsComponent>(id);
			bool character = m_Scene.HasComponent<GS::PlayerComponent>(id) || m_Scene.HasComponent<GS::NpcComponent>(id);
			if (!physics && !character)
				continue;

			// A humanoid is thirteen bodies and twelve joints, built by its
			// own class into this world, not one body made here.
			if (character && CharacterController::BodyOf(m_Scene, id) == CharacterController::HumanoidBody)
			{
				m_World.VelocityIterations = std::max(m_World.VelocityIterations, Humanoid::VelocityIterations);
				m_World.PositionIterations = std::max(m_World.PositionIterations, Humanoid::PositionIterations);
				m_Characters.AddHumanoid(m_Scene, id, m_World);
				continue;
			}

			GS::RigidBody3D body;
			if (character)
				body = CharacterController::MakeBody(*transform, physics, CharacterController::BodyOf(m_Scene, id));
			else if (!MakeTerrainBody(id, *transform, *physics, body))
			{
				glm::vec3 halfExtents;
				if (auto* mesh = m_Scene.GetComponent<GS::MeshComponent>(id); mesh && mesh->Geometry)
					halfExtents = (mesh->Geometry->GetBoundsMax() - mesh->Geometry->GetBoundsMin()) * 0.5f * transform->Scale;
				else
					halfExtents = transform->Scale * 0.5f;

				// A sphere collides as a sphere -- it used to be its bounding
				// box, which does not roll -- when it is scaled evenly enough
				// for one radius to describe it.
				auto* sphereMesh = m_Scene.GetComponent<GS::MeshComponent>(id);
				glm::vec3 scale = transform->Scale;
				bool sphere = sphereMesh && sphereMesh->SourcePath == "primitive:sphere"
					&& std::max({ scale.x, scale.y, scale.z }) <= 1.05f * std::min({ scale.x, scale.y, scale.z });
				float radius = 0.5f * std::max({ scale.x, scale.y, scale.z });
				if (sphere)
					body = physics->Type == GS::BodyType::Dynamic
						? GS::RigidBody3D::MakeSphere(transform->Position, radius, physics->Mass)
						: GS::RigidBody3D::MakeStaticSphere(transform->Position, radius);
				else
					body = (physics->Type == GS::BodyType::Dynamic)
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
			}

			// Set before the first step, so the launch is the body's state at
			// t = 0 and not a kick part-way through it.
			if (physics && body.Type == GS::BodyType::Dynamic)
				body.Velocity = physics->LaunchVelocity;

			GS::PhysicsWorld3D::BodyHandle handle = m_World.AddBody(body);
			m_Bodies[id] = handle;
			m_BodyOwners[handle] = id;
			m_LastSynced[id] = transform->Position;
			if (character)
				m_Characters.Add(m_Scene, id, handle);
		}
		m_Characters.Start(m_Scene);

		// The project's controls, when it has saved some: what every
		// action below reads. Without the file the map is left as it is --
		// defaults in a player, whatever the editor's Controls panel holds
		// in the editor.
		{
			std::string controls = GS::Assets::Resolve(GS::InputMap::FileName());
			if (std::filesystem::exists(controls))
				GS::InputMap::Active().Load(controls);
		}

		m_Launchers.Start(m_LauncherHost);
		RegisterCommands();
		m_Launchers.RegisterCommands(m_Console);
		m_Modes.Start();
		m_TimeScale = 1.0f;
		GS::ScriptPhysics::SetBackend(&m_Backend);
		GS::ScriptUI::SetBackend(&m_UIBackend);

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
			// A .js script was compiled at export (imports already spliced
			// in) and runs without the TypeScript compiler; anything else is
			// GSS source, compiled here.
			bool isJs = std::filesystem::path(script->ScriptPath).extension() == ".js";
			bool prepared_ok = isJs
				? m_Engine.PrepareEntityScriptFromJS(&m_Scene, id, buffer.str(), error, prepared)
				: m_Engine.PrepareEntityScript(&m_Scene, id, script->ScriptPath, buffer.str(), error, prepared, &imports);
			if (!prepared_ok)
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
		GS::ScriptUI::SetBackend(nullptr);
		m_Modes.Stop();

		for (auto& pair : m_Prepared)
			m_Engine.ReleasePreparedScript(pair.second);
		m_Prepared.clear();

		// No JSValues to release here -- a shared_ptr<void>'s deleter (set
		// up inside MakeCompiledScriptEntry's Create) already knows how to
		// destroy the real T, so clear() alone is enough.
		m_Compiled.clear();
		m_Characters.Clear();

		m_Running = false;
	}

	void FixedUpdate(float dt)
	{
		if (!m_Running)
			return;

		// What was typed since the last step, then the launcher -- on the
		// step, so a shot leaves at a step boundary like everything else.
		// Slow motion is a shorter simulated step for the same real one.
		m_Console.RunQueued();
		float real = dt;
		dt *= m_TimeScale;
		// The camera, the modes and walking up to things go at real speed:
		// slow motion slows the world, not the person watching it.
		m_Modes.Step(real);
		m_Launchers.Step(dt, real, !GS::ImGuiLayer::WantsKeyboard() && m_Modes.KeyboardFree());
		if (dt <= 0.0f)
			return;   // paused

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

		// Split when something is fast. A step moves a body v dt, and once
		// that is more than the two things meeting are thick the contact is
		// found too late: measured, a 7 kg ball at 13.8 m/s (0.23 m a step)
		// entered the dummy's 0.22 m chest, the contact normal flipped as its
		// centre passed the middle, and it was pushed out through his back at
		// 7 m/s, his body hardly moved. So nothing moves more than
		// m_MaxTravel in one substep; the characters run on every substep as
		// a whole tick of their own, which is what their dt already means.
		float fastest = 0.0f;
		for (const GS::RigidBody3D& body : m_World.GetBodies())
			if (body.Type == GS::BodyType::Dynamic && body.Enabled && body.Awake)
				fastest = std::max(fastest, glm::length(body.Velocity));
		int substeps = std::clamp((int)std::ceil(fastest * dt / m_MaxTravel), 1, 8);
		float sub = dt / (float)substeps;
		for (int i = 0; i < substeps; i++)
		{
			m_Characters.BeforeStep(m_Scene, m_World, sub);
			m_World.Step(sub);
			if (i + 1 < substeps)
				m_Characters.AfterStep(m_Scene, m_World);
		}

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
		m_Characters.AfterStep(m_Scene, m_World);

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
	// The session's own commands; the launcher adds its own.
	void RegisterCommands()
	{
		GameConsole& c = m_Console;
		c.ClearText();
		c.Register("help", "", "this list", [this](const GameConsole::Args&) { m_Console.PrintHelp(); });
		c.Register("clear", "", "empty the terminal", [this](const GameConsole::Args&) { m_Console.ClearText(); });
		c.Register("reset", "", "everyone back on their feet where they started, projectiles gone", [this](const GameConsole::Args&)
		{
			int swept = m_Launchers.Sweep();
			int stood = m_Characters.ResetHumanoids();
			m_Console.Good("reset: " + std::to_string(stood) + " stood up, " + std::to_string(swept) + " projectiles cleared");
		});
		c.Register("getup", "", "the fallen get up where they lie", [this](const GameConsole::Args&)
		{
			int n = m_Characters.GetUpHumanoids();
			m_Console.Good(n ? std::to_string(n) + " getting up" : "nobody is down");
		});
		c.Register("sweep", "", "clear away the projectiles", [this](const GameConsole::Args&)
		{
			m_Console.Good(std::to_string(m_Launchers.Sweep()) + " projectiles cleared");
		});
		c.Register("slowmo", "<factor>", "1 is real time, 0.25 a quarter speed, 0 paused", [this](const GameConsole::Args& a)
		{
			float f;
			if (a.empty() || !m_Console.Number(a[0], f)) { m_Console.Bad("slowmo 0.25"); return; }
			m_TimeScale = std::clamp(f, 0.0f, 2.0f);
			m_Console.Good(m_TimeScale == 0.0f ? "paused -- slowmo 1 to go on" : "time at " + std::to_string((int)std::lround(m_TimeScale * 100.0f)) + "%");
		});
		c.Register("pause", "", "stop time, or start it again", [this](const GameConsole::Args&)
		{
			if (m_TimeScale > 0.0f) { m_PausedFrom = m_TimeScale; m_TimeScale = 0.0f; m_Console.Good("paused"); }
			else { m_TimeScale = m_PausedFrom > 0.0f ? m_PausedFrom : 1.0f; m_Console.Good("running"); }
		});
		c.Register("shove", "<name> <N s> [front|back|left|right]", "push a humanoid at the chest (from behind by default)", [this](const GameConsole::Args& a)
		{
			if (a.size() < 2) { m_Console.Bad("shove Dummy 120 back"); return; }
			float strength;
			// The name may hold spaces: the number is the first word that is one.
			size_t numberAt = 1;
			char* end = nullptr;
			while (numberAt < a.size() && (std::strtof(a[numberAt].c_str(), &end), end == a[numberAt].c_str() || *end))
				numberAt++;
			if (numberAt >= a.size() || !m_Console.Number(a[numberAt], strength)) { m_Console.Bad("shove Dummy 120 back"); return; }
			std::string name;
			for (size_t i = 0; i < numberAt; i++)
				name += (i ? " " : "") + a[i];
			Humanoid* rig = FindRig(name);
			if (!rig) { m_Console.Bad("no humanoid called '" + name + "'"); return; }
			std::string from = numberAt + 1 < a.size() ? GameConsole::Lower(a[numberAt + 1]) : "back";
			float f = rig->FacingRadians();
			glm::vec3 forward(std::sin(f), 0.0f, std::cos(f)), right(std::cos(f), 0.0f, -std::sin(f));
			// Pushed *from* that side: from behind sends him forwards.
			glm::vec3 direction = from == "front" ? -forward : from == "left" ? right : from == "right" ? -right : forward;
			rig->Shove(strength, direction);
			m_Console.Good("shoved " + name + " with " + a[numberAt] + " N s from the " + from);
		});
		c.Register("walkstyle", "<name> [preset]", "how a humanoid walks; no preset lists them", [this](const GameConsole::Args& a)
		{
			const auto& presets = Humanoid::GaitPresets();
			if (a.empty())
			{
				std::string list;
				for (const GS::GaitSettings& g : presets)
					list += (list.empty() ? "" : "  ") + g.Preset;
				m_Console.Print("styles: " + list, GameConsole::Tone::Dim);
				return;
			}
			// The last word is the style when it names one; the rest, the humanoid.
			std::string style = GameConsole::Lower(a.back());
			const GS::GaitSettings* chosen = nullptr;
			for (const GS::GaitSettings& g : presets)
				if (GameConsole::Lower(g.Preset) == style)
					chosen = &g;
			std::string name;
			for (size_t i = 0; i + (chosen ? 1 : 0) < a.size(); i++)
				name += (i ? " " : "") + a[i];
			Humanoid* rig = FindRig(name);
			if (!rig) { m_Console.Bad("no humanoid called '" + name + "'"); return; }
			if (!chosen)
			{
				m_Console.Print(name + " walks " + rig->GetGait().Preset, GameConsole::Tone::Dim);
				return;
			}
			rig->SetGait(*chosen);
			m_Console.Good(name + " now walks " + chosen->Preset);
		});
		c.Register("camera", "[name]", "look through another camera; no name lists them", [this](const GameConsole::Args& a)
		{
			auto& cameras = m_Scene.View<GS::CameraComponent>();
			std::string want;
			for (const std::string& w : a)
				want += (want.empty() ? "" : " ") + w;
			if (want.empty())
			{
				m_Console.Print("cameras", GameConsole::Tone::Title);
				for (size_t i = 0; i < cameras.Size(); i++)
				{
					auto* tag = m_Scene.GetComponent<GS::TagComponent>(cameras.Owner(i));
					m_Console.Print(std::string("  ") + (tag ? tag->Name : "?") + (cameras.Components()[i].Active ? "   (on)" : ""));
				}
				return;
			}
			int found = -1;
			for (size_t i = 0; i < cameras.Size(); i++)
			{
				auto* tag = m_Scene.GetComponent<GS::TagComponent>(cameras.Owner(i));
				if (tag && GameConsole::Lower(tag->Name) == GameConsole::Lower(want))
					found = (int)i;
			}
			if (found < 0) { m_Console.Bad("no camera '" + want + "' -- camera lists them"); return; }
			for (size_t i = 0; i < cameras.Size(); i++)
				cameras.Components()[i].Active = (int)i == found;
			m_Console.Good("camera: " + want);
		});
		c.Print("GS game terminal -- type help", GameConsole::Tone::Title);
		if (m_Launchers.Active())
			c.Print("a launcher is ready: " + GS::InputMap::Active().Keys("fire") + " fires it; try  preset car, fire  or  slowmo 0.25", GameConsole::Tone::Dim);
	}

	// A humanoid by its entity's name.
	Humanoid* FindRig(const std::string& name)
	{
		for (GS::EntityId id : m_Scene.GetEntities())
			if (auto* tag = m_Scene.GetComponent<GS::TagComponent>(id); tag && GameConsole::Lower(tag->Name) == GameConsole::Lower(name))
				return m_Characters.RigOf(id);
		return nullptr;
	}

	// What the launcher needs of the session: the world, and bodies made
	// and switched off mid-game. Registered like any body, so its entity's
	// transform follows it and scripts can ask about it.
	struct SessionLauncherHost : LauncherHost
	{
		GameSession& Session;
		explicit SessionLauncherHost(GameSession& session) : Session(session) {}
		GS::Scene& HostScene() override { return Session.m_Scene; }
		GS::PhysicsWorld3D& HostWorld() override { return Session.m_World; }
		GS::PhysicsWorld3D::BodyHandle AttachBody(GS::EntityId entity, const GS::RigidBody3D& body, int reuse) override
		{
			GS::PhysicsWorld3D::BodyHandle handle;
			if (reuse >= 0)
			{
				handle = (GS::PhysicsWorld3D::BodyHandle)reuse;
				Session.m_World.GetBody(handle) = body;
				Session.m_World.GetBody(handle).Enabled = true;
			}
			else
				handle = Session.m_World.AddBody(body);
			Session.m_Bodies[entity] = handle;
			Session.m_BodyOwners[handle] = entity;
			Session.m_LastSynced[entity] = body.Position;
			return handle;
		}
		void RetireBody(GS::EntityId entity) override
		{
			auto found = Session.m_Bodies.find(entity);
			if (found == Session.m_Bodies.end())
				return;
			GS::RigidBody3D& body = Session.m_World.GetBody(found->second);
			body.Enabled = false;
			body.Velocity = glm::vec3(0.0f);
			body.AngularVelocity = glm::vec3(0.0f);
			Session.m_BodyOwners.erase(found->second);
			Session.m_LastSynced.erase(entity);
			Session.m_Bodies.erase(found);
		}
	};

	// A static terrain plane collides as what it looks like: a heightfield
	// sampled from the same displacement its mesh was built from, at the
	// mesh's own vertices -- so the ground a character stands on is the
	// ground drawn. Only for a plane that is level (a heightfield cannot be
	// turned) and square (it has one extent); anything else falls back to
	// its bounding box, which GameSession says so about.
	bool MakeTerrainBody(GS::EntityId id, const GS::TransformComponent& transform, const GS::PhysicsComponent& physics,
		GS::RigidBody3D& out)
	{
		auto* terrain = m_Scene.GetComponent<GS::TerrainComponent>(id);
		auto* mesh = m_Scene.GetComponent<GS::MeshComponent>(id);
		if (!terrain || !mesh || mesh->SourcePath != "primitive:plane")
			return false;
		if (physics.Type != GS::BodyType::Static || transform.Rotation != glm::vec3(0.0f)
			|| std::abs(transform.Scale.x - transform.Scale.z) > 1e-4f * std::max(1.0f, transform.Scale.x))
		{
			GS_WARN("GameSession: terrain '{0}' collides as a box -- a terrain collider needs a static, unrotated plane "
				"scaled the same in x and z", id);
			return false;
		}

		int n = std::clamp(terrain->Resolution, 1, 512);
		std::vector<float> heights((size_t)(n + 1) * (n + 1));
		for (int k = 0; k <= n; k++)
			for (int i = 0; i <= n; i++)
			{
				glm::vec3 local(-0.5f + (float)i / n, 0.0f, -0.5f + (float)k / n);
				heights[(size_t)k * (n + 1) + i] = GS::TerrainMesh::Displacement(*terrain, local) * transform.Scale.y;
			}
		auto field = std::make_shared<GS::Heightfield3D>();
		field->SetHeights(n + 1, transform.Scale.x, heights);
		out = GS::RigidBody3D::MakeHeightfield(transform.Position, field);
		out.Friction = physics.Friction;
		out.Restitution = physics.Restitution;
		return true;
	}

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

	// Scripts' ui.show/hide/set/get/command, through GS::ScriptUI.
	struct UIBackend : GS::ScriptUI::Backend
	{
		GameSession& Session;
		explicit UIBackend(GameSession& session) : Session(session) {}
		void Show(const std::string& screen, bool shown) override { Session.UI().Show(screen, shown); }
		void Set(const std::string& name, const std::string& value) override { Session.UI().Set(name, value); }
		std::string Get(const std::string& name) override { return Session.UI().Get(name); }
		void Command(const std::string& line) override { Session.m_Console.Submit(line, false); }
	};

	PhysicsBackend m_Backend{ *this };
	UIBackend m_UIBackend{ *this };
	CharacterController m_Characters;
	GameConsole m_Console;
	LauncherSystem m_Launchers;
	GameModes m_Modes{ m_Scene, m_World, m_Characters, m_Launchers, m_Console };
	SessionLauncherHost m_LauncherHost{ *this };
	float m_TimeScale = 1.0f;
	float m_PausedFrom = 1.0f;
	float m_MaxTravel = 0.08f;   // m a body may move in one physics substep
};

// Every humanoid in the scene as draw items: live bodies for those a running
// session is driving, the rest pose for the others -- which, in the editor
// outside Play, is all of them.
inline void AppendHumanoidDraws(GS::Scene& scene, const GameSession* session, std::vector<ExtraDraw>& out)
{
	if (session)
		session->AppendCharacterDraws(out);
	auto addRest = [&](GS::EntityId entity)
	{
		if (!IsHumanoidEntity(scene, entity) || (session && session->IsRunningCharacter(entity)))
			return;
		auto* transform = scene.GetComponent<GS::TransformComponent>(entity);
		auto* mesh = scene.GetComponent<GS::MeshComponent>(entity);
		if (transform && (!mesh || mesh->Visible))
			Mannequin::AppendRestPose(*transform, Mannequin::ColourOf(scene, entity), entity, out, Mannequin::LookOf(scene, entity),
				Mannequin::PhysiqueOf(scene, entity));
	};
	auto& players = scene.View<GS::PlayerComponent>();
	for (size_t i = 0; i < players.Size(); i++)
		addRest(players.Owner(i));
	auto& npcs = scene.View<GS::NpcComponent>();
	for (size_t i = 0; i < npcs.Size(); i++)
		addRest(npcs.Owner(i));
}
