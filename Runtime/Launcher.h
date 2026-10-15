#pragma once

// The launcher: fires a physics object -- a ball or a box, of a size, a
// mass and a speed -- from a LauncherComponent's entity at its target.
// Driven from the game console (Runtime/GameConsole.h) and the F key; made
// for the crash-test demo, where what it is aimed at is a humanoid.
//
// Aiming. With Arc on, the launch angle is the low solution of the
// projectile equation, so gravity brings the shot onto the aim point
// instead of under it:
//
//     tan(theta) = (v^2 - sqrt(v^4 - g (g x^2 + 2 y v^2))) / (g x)
//
// for a target x away and y above. Too slow to reach, the root is
// imaginary; it then fires at 45 degrees -- its longest reach -- and says
// so.
//
// The physics world never removes a body (handles are indices), so the
// projectiles are a pool: past m_MaxShots the oldest is retired -- its body
// switched off (RigidBody3D::Enabled), its entity destroyed -- and its body
// reused for the next shot.

#include <GS.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "GameConsole.h"

#include <cstdio>
#include <string>
#include <vector>

// What the launcher needs of the game it runs in.
struct LauncherHost
{
	virtual ~LauncherHost() = default;
	virtual GS::Scene& HostScene() = 0;
	virtual GS::PhysicsWorld3D& HostWorld() = 0;
	// Gives `entity` a body; into `reuse`'s slot when it is a retired one.
	virtual GS::PhysicsWorld3D::BodyHandle AttachBody(GS::EntityId entity, const GS::RigidBody3D& body, int reuse) = 0;
	// Switches the body off and forgets the entity (destroying it is the
	// caller's business).
	virtual void RetireBody(GS::EntityId entity) = 0;
};

class LauncherSystem
{
public:
	struct Preset
	{
		const char* Name;
		const char* About;
		int Shape;
		glm::vec3 Size;
		float Mass, Speed;
		glm::vec4 Color;
		float Aim = -1.0f;   // where on the target, if this object wants it; -1 leaves it be
	};

	// Real objects at real speeds, so the numbers mean something.
	static const std::vector<Preset>& Presets()
	{
		static const std::vector<Preset> presets =
		{
			{ "tennis",   "a tennis ball, served",                 0, glm::vec3(0.067f), 0.058f, 45.0f, { 0.85f, 0.95f, 0.2f, 1.0f } },
			{ "football", "a football, struck",                    0, glm::vec3(0.22f), 0.43f, 25.0f, { 0.95f, 0.95f, 0.95f, 1.0f } },
			{ "bowling",  "a bowling ball, bowled",                0, glm::vec3(0.218f), 7.0f, 9.0f, { 0.15f, 0.2f, 0.55f, 1.0f } },
			{ "brick",    "a house brick, thrown",                 1, glm::vec3(0.215f, 0.065f, 0.1025f), 2.7f, 12.0f, { 0.7f, 0.3f, 0.2f, 1.0f } },
			{ "crate",    "a wooden crate, flung",                 1, glm::vec3(0.6f), 20.0f, 8.0f, { 0.65f, 0.45f, 0.25f, 1.0f } },
			{ "beam",     "a 2 m timber beam, swung end-on",       1, glm::vec3(0.2f, 0.2f, 2.0f), 40.0f, 10.0f, { 0.6f, 0.5f, 0.35f, 1.0f } },
			{ "wrecking", "a wrecking ball",                       0, glm::vec3(0.9f), 500.0f, 7.0f, { 0.25f, 0.25f, 0.28f, 1.0f }, 1.2f },
			// Aimed so its middle meets him at its own half-height: a car
			// arcing in at his chest was a car flying.
			{ "car",      "a car-sized box at 50 km/h",            1, glm::vec3(1.8f, 1.4f, 4.2f), 1200.0f, 13.9f, { 0.8f, 0.1f, 0.12f, 1.0f }, 0.72f },
		};
		return presets;
	}

	void Start(LauncherHost& host)
	{
		m_Host = &host;
		m_Shots.clear();
		m_Launcher = GS::InvalidEntity;
		GS::Scene& scene = host.HostScene();
		auto& launchers = scene.View<GS::LauncherComponent>();
		if (launchers.Size() > 0)
			m_Launcher = launchers.Owner(0);
	}

	bool Active() const { return m_Host && m_Host->HostScene().IsValid(m_Launcher); }

	// The projectiles, which an editor keeping Play's changes should drop.
	std::vector<GS::EntityId> Projectiles() const
	{
		std::vector<GS::EntityId> out;
		for (const Shot& shot : m_Shots)
			out.push_back(shot.Entity);
		return out;
	}

	void RegisterCommands(GameConsole& console)
	{
		m_Console = &console;
		if (!Active())
			return;

		console.Register("fire", "[count]", "launch one, or a volley (F does it too)", [this](const GameConsole::Args& a)
		{
			float n = 1.0f;
			if (!a.empty() && !m_Console->Number(a[0], n))
				return;
			int count = std::clamp((int)n, 1, 20);
			m_Volley += count;
		});
		console.Register("shape", "ball|box", "what it fires", [this](const GameConsole::Args& a)
		{
			if (a.empty()) { m_Console->Bad("shape ball, or shape box"); return; }
			std::string s = GameConsole::Lower(a[0]);
			if (s == "ball" || s == "sphere") Settings()->Shape = 0;
			else if (s == "box" || s == "cube") Settings()->Shape = 1;
			else { m_Console->Bad("shape ball, or shape box"); return; }
			PrintSettings();
		});
		console.Register("size", "<diameter> | <x> <y> <z>", "metres: a ball's diameter, or a box's sides", [this](const GameConsole::Args& a)
		{
			float x, y, z;
			if (a.size() == 1 && m_Console->Number(a[0], x))
				Settings()->Size = glm::vec3(std::clamp(x, 0.02f, 6.0f));
			else if (a.size() == 3 && m_Console->Number(a[0], x) && m_Console->Number(a[1], y) && m_Console->Number(a[2], z))
				Settings()->Size = glm::clamp(glm::vec3(x, y, z), glm::vec3(0.02f), glm::vec3(6.0f));
			else
			{
				m_Console->Bad("size 0.3, or size 0.6 0.4 1.2");
				return;
			}
			PrintSettings();
		});
		console.Register("mass", "<kg>", "how heavy", [this](const GameConsole::Args& a)
		{
			float kg;
			if (a.empty() || !m_Console->Number(a[0], kg)) { m_Console->Bad("mass 5"); return; }
			Settings()->Mass = std::clamp(kg, 0.01f, 5000.0f);
			PrintSettings();
		});
		console.Register("speed", "<m/s>", "how fast it leaves (14 m/s is 50 km/h)", [this](const GameConsole::Args& a)
		{
			float v;
			if (a.empty() || !m_Console->Number(a[0], v)) { m_Console->Bad("speed 15"); return; }
			Settings()->Speed = std::clamp(v, 0.1f, 120.0f);
			PrintSettings();
		});
		console.Register("spin", "<rad/s>", "topspin; negative for backspin", [this](const GameConsole::Args& a)
		{
			float w;
			if (a.empty() || !m_Console->Number(a[0], w)) { m_Console->Bad("spin 10"); return; }
			Settings()->Spin = std::clamp(w, -200.0f, 200.0f);
			PrintSettings();
		});
		console.Register("aim", "head|chest|hips|legs|<metres>", "where on the target, as a height above its feet", [this](const GameConsole::Args& a)
		{
			if (a.empty()) { m_Console->Bad("aim chest, or aim 1.1"); return; }
			std::string s = GameConsole::Lower(a[0]);
			float h;
			if (s == "head") h = 1.7f;
			else if (s == "chest") h = 1.3f;
			else if (s == "hips") h = 1.0f;
			else if (s == "legs") h = 0.5f;
			else if (!m_Console->Number(a[0], h)) return;
			Settings()->AimHeight = std::clamp(h, -2.0f, 10.0f);
			PrintSettings();
		});
		console.Register("arc", "on|off", "aim for gravity, or fire straight at the aim point", [this](const GameConsole::Args& a)
		{
			if (a.empty()) { m_Console->Bad("arc on, or arc off"); return; }
			Settings()->Arc = GameConsole::Lower(a[0]) == "on";
			PrintSettings();
		});
		console.Register("target", "<name>", "aim at another entity, by its name", [this](const GameConsole::Args& a)
		{
			if (a.empty()) { m_Console->Bad("target Dummy"); return; }
			std::string name;
			for (const std::string& w : a)
				name += (name.empty() ? "" : " ") + w;
			if (Find(name) == GS::InvalidEntity) { m_Console->Bad("nothing is called '" + name + "'"); return; }
			Settings()->Target = name;
			PrintSettings();
		});
		console.Register("auto", "<shots per second>|off", "keep firing", [this](const GameConsole::Args& a)
		{
			float rate = 0.0f;
			if (!a.empty() && GameConsole::Lower(a[0]) != "off" && !m_Console->Number(a[0], rate))
				return;
			m_AutoRate = std::clamp(rate, 0.0f, 20.0f);
			if (m_AutoRate == 0.0f)
				m_Volley = 0;   // and nothing still queued from it
			m_Console->Good(m_AutoRate > 0.0f ? "auto fire: " + Format(m_AutoRate) + " a second" : "auto fire off");
		});
		console.Register("preset", "[name]", "load a real object; no name lists them", [this](const GameConsole::Args& a)
		{
			if (a.empty())
			{
				m_Console->Print("presets", GameConsole::Tone::Title);
				for (const Preset& p : Presets())
					m_Console->Print("  " + std::string(p.Name) + "  -  " + p.About + " (" + Format(p.Mass) + " kg, " + Format(p.Speed) + " m/s)");
				return;
			}
			for (const Preset& p : Presets())
				if (GameConsole::Lower(a[0]) == p.Name)
				{
					GS::LauncherComponent* s = Settings();
					s->Shape = p.Shape;
					s->Size = p.Size;
					s->Mass = p.Mass;
					s->Speed = p.Speed;
					s->Color = p.Color;
					if (p.Aim >= 0.0f)
						s->AimHeight = p.Aim;
					m_Console->Good(std::string("loaded ") + p.About);
					PrintSettings();
					return;
				}
			m_Console->Bad("no preset '" + a[0] + "' -- preset lists them");
		});
		console.Register("status", "", "what the launcher is set to", [this](const GameConsole::Args&) { PrintSettings(); });
	}

	// Projectiles off and gone.
	int Sweep()
	{
		m_Volley = 0;   // shots still queued would refill what was just cleared
		int count = (int)m_Shots.size();
		for (Shot& shot : m_Shots)
		{
			Retire(shot);
			m_Free.push_back((int)shot.Body);
		}
		m_Shots.clear();
		return count;
	}

	// One fixed step. `dt` is the simulated step (slow motion included),
	// `real` the wall step the input is read on.
	void Step(float dt, float real, bool keyboardFree)
	{
		if (!Active())
			return;

		bool fireKey = keyboardFree && GS::Input::IsKeyPressed(GS_KEY_F);
		if (fireKey && !m_FireKeyWas)
			m_Volley++;
		m_FireKeyWas = fireKey;

		if (m_AutoRate > 0.0f)
		{
			m_AutoClock += dt;
			if (m_AutoClock >= 1.0f / m_AutoRate)
			{
				m_AutoClock = 0.0f;
				m_Volley++;
			}
		}

		// A volley goes one shot at a time, a tenth of a second apart.
		m_VolleyClock -= dt;
		if (m_Volley > 0 && m_VolleyClock <= 0.0f)
		{
			FireOne();
			m_Volley--;
			m_VolleyClock = 0.1f;
		}
		(void)real;

		AimLauncherMesh();
		UpdateStatus();
	}

private:
	struct Shot
	{
		GS::EntityId Entity = GS::InvalidEntity;
		GS::PhysicsWorld3D::BodyHandle Body = 0;
	};

	GS::LauncherComponent* Settings()
	{
		return m_Host->HostScene().GetComponent<GS::LauncherComponent>(m_Launcher);
	}

	GS::EntityId Find(const std::string& name)
	{
		GS::Scene& scene = m_Host->HostScene();
		for (GS::EntityId id : scene.GetEntities())
			if (auto* tag = scene.GetComponent<GS::TagComponent>(id); tag && tag->Name == name)
				return id;
		return GS::InvalidEntity;
	}

	static std::string Format(float value)
	{
		char buffer[32];
		snprintf(buffer, sizeof buffer, value >= 100.0f ? "%.0f" : value >= 10.0f ? "%.1f" : "%.2f", value);
		return buffer;
	}

	// Where the shot leaves from and which way; false with nowhere to aim.
	bool Aim(glm::vec3& from, glm::vec3& direction, bool& outOfRange)
	{
		GS::Scene& scene = m_Host->HostScene();
		GS::LauncherComponent* s = Settings();
		auto* transform = scene.GetComponent<GS::TransformComponent>(m_Launcher);
		if (!s || !transform)
			return false;
		from = transform->Position;
		outOfRange = false;

		GS::EntityId target = s->Target.empty() ? GS::InvalidEntity : Find(s->Target);
		auto* targetTransform = target != GS::InvalidEntity ? scene.GetComponent<GS::TransformComponent>(target) : nullptr;
		if (!targetTransform)
		{
			glm::mat4 r = glm::eulerAngleXYZ(glm::radians(transform->Rotation.x), glm::radians(transform->Rotation.y),
				glm::radians(transform->Rotation.z));
			direction = glm::normalize(glm::vec3(r * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
			return true;
		}

		glm::vec3 to = targetTransform->Position + glm::vec3(0.0f, s->AimHeight, 0.0f) - from;
		glm::vec2 flat(to.x, to.z);
		float x = glm::length(flat);
		if (x < 1e-3f)
		{
			direction = glm::normalize(to);
			return true;
		}
		glm::vec2 across = flat / x;
		if (!s->Arc)
		{
			direction = glm::normalize(to);
			return true;
		}

		const float g = 9.81f;
		float v = s->Speed, y = to.y;
		float root = v * v * v * v - g * (g * x * x + 2.0f * y * v * v);
		float angle;
		if (root < 0.0f)
		{
			angle = glm::radians(45.0f);
			outOfRange = true;
		}
		else
			angle = std::atan((v * v - std::sqrt(root)) / (g * x));
		direction = glm::vec3(across.x * std::cos(angle), std::sin(angle), across.y * std::cos(angle));
		return true;
	}

	void FireOne()
	{
		glm::vec3 from, direction;
		bool outOfRange = false;
		GS::LauncherComponent* s = Settings();
		if (!s || !Aim(from, direction, outOfRange))
			return;

		// Clear of the launcher's own mesh before it is a body at all.
		float reach = 0.5f * std::max({ s->Size.x, s->Size.y, s->Size.z });
		glm::vec3 at = from + direction * (reach + 0.45f);

		// Turned to face where it is going, so a beam flies end-on.
		float yaw = std::atan2(direction.x, direction.z);
		float pitch = std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
		glm::quat facing = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(-pitch, glm::vec3(1.0f, 0.0f, 0.0f));

		GS::RigidBody3D body = s->Shape == 0
			? GS::RigidBody3D::MakeSphere(at, 0.5f * s->Size.x, s->Mass)
			: GS::RigidBody3D::MakeBox(at, 0.5f * s->Size, s->Mass);
		body.Orientation = facing;
		body.PreviousOrientation = facing;
		body.UpdateInertiaWorld();
		body.Friction = 0.6f;
		body.Restitution = 0.15f;
		body.Velocity = direction * s->Speed;
		glm::vec3 sideways = glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f));
		if (glm::length(sideways) > 1e-4f)
			body.AngularVelocity = glm::normalize(sideways) * -s->Spin;

		// Past the pool's size the oldest goes, and its body is reused.
		int reuse = -1;
		if ((int)m_Shots.size() >= m_MaxShots)
		{
			Shot oldest = m_Shots.front();
			m_Shots.erase(m_Shots.begin());
			Retire(oldest);
			reuse = (int)oldest.Body;
		}
		else if (!m_Free.empty())
		{
			reuse = m_Free.back();
			m_Free.pop_back();
		}

		GS::Scene& scene = m_Host->HostScene();
		GS::Entity entity = scene.CreateEntity("Projectile");
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		float x, y, z;
		glm::extractEulerAngleXYZ(glm::mat4_cast(facing), x, y, z);
		transform->Rotation = glm::degrees(glm::vec3(x, y, z));
		transform->Scale = s->Shape == 0 ? glm::vec3(s->Size.x) : s->Size;
		GS::MeshComponent mesh;
		mesh.SourcePath = s->Shape == 0 ? "primitive:sphere" : "primitive:cube";
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Color = s->Color;
		entity.Add<GS::MeshComponent>(mesh);

		Shot shot;
		shot.Entity = entity.GetId();
		shot.Body = m_Host->AttachBody(entity.GetId(), body, reuse);
		m_Shots.push_back(shot);

		// What it carries, in the terms that decide what it does to him.
		float momentum = s->Mass * s->Speed;
		float energy = 0.5f * s->Mass * s->Speed * s->Speed;
		if (m_Console)
		{
			std::string what = std::string(s->Shape == 0 ? "ball" : "box") + " " + Format(s->Mass) + " kg at " + Format(s->Speed)
				+ " m/s  -  " + Format(momentum) + " N s, " + Format(energy) + " J";
			m_Console->Print("fired " + what, GameConsole::Tone::Dim);
			if (outOfRange)
				m_Console->Bad("too slow to reach the target on an arc -- fired at 45 degrees");
		}
	}

	void Retire(Shot& shot)
	{
		GS::Scene& scene = m_Host->HostScene();
		m_Host->RetireBody(shot.Entity);
		if (scene.IsValid(shot.Entity))
			scene.DestroyEntity(shot.Entity);
	}

	// The launcher's own mesh points where the next shot will go.
	void AimLauncherMesh()
	{
		glm::vec3 from, direction;
		bool outOfRange;
		GS::Scene& scene = m_Host->HostScene();
		auto* transform = scene.GetComponent<GS::TransformComponent>(m_Launcher);
		GS::LauncherComponent* s = Settings();
		if (!transform || !s || s->Target.empty() || !Aim(from, direction, outOfRange))
			return;
		float yaw = std::atan2(direction.x, direction.z);
		float pitch = std::asin(glm::clamp(direction.y, -1.0f, 1.0f));
		glm::quat facing = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(-pitch, glm::vec3(1.0f, 0.0f, 0.0f));
		float x, y, z;
		glm::extractEulerAngleXYZ(glm::mat4_cast(facing), x, y, z);
		transform->Rotation = glm::degrees(glm::vec3(x, y, z));
	}

	void UpdateStatus()
	{
		GS::LauncherComponent* s = Settings();
		if (!s || !m_Console)
			return;
		std::string shape = s->Shape == 0 ? "ball " + Format(s->Size.x) + " m"
			: "box " + Format(s->Size.x) + " x " + Format(s->Size.y) + " x " + Format(s->Size.z) + " m";
		m_Console->SetStatus(shape + "  " + Format(s->Mass) + " kg  " + Format(s->Speed) + " m/s"
			+ (s->Target.empty() ? std::string() : "  at " + s->Target) + "      F  fire");
	}

	void PrintSettings()
	{
		GS::LauncherComponent* s = Settings();
		if (!s || !m_Console)
			return;
		m_Console->Print("launcher", GameConsole::Tone::Title);
		m_Console->Print(std::string("  shape   ") + (s->Shape == 0 ? "ball" : "box"));
		m_Console->Print("  size    " + (s->Shape == 0 ? Format(s->Size.x) + " m across"
			: Format(s->Size.x) + " x " + Format(s->Size.y) + " x " + Format(s->Size.z) + " m"));
		m_Console->Print("  mass    " + Format(s->Mass) + " kg");
		m_Console->Print("  speed   " + Format(s->Speed) + " m/s (" + Format(s->Speed * 3.6f) + " km/h)");
		m_Console->Print("  spin    " + Format(s->Spin) + " rad/s");
		m_Console->Print("  aim     " + Format(s->AimHeight) + " m up " + (s->Target.empty() ? std::string("(no target: straight ahead)") : "on " + s->Target)
			+ (s->Arc ? ", arcing for gravity" : ", straight"));
		m_Console->Print("  carries " + Format(s->Mass * s->Speed) + " N s, " + Format(0.5f * s->Mass * s->Speed * s->Speed) + " J", GameConsole::Tone::Dim);
	}

	LauncherHost* m_Host = nullptr;
	GameConsole* m_Console = nullptr;
	GS::EntityId m_Launcher = GS::InvalidEntity;
	std::vector<Shot> m_Shots;
	std::vector<int> m_Free;          // retired bodies waiting to be reused
	int m_MaxShots = 40;
	int m_Volley = 0;
	float m_VolleyClock = 0.0f;
	float m_AutoRate = 0.0f;
	float m_AutoClock = 0.0f;
	bool m_FireKeyWas = false;
};
