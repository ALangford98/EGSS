#pragma once

// Cannons that shoot at the runner -- fairly. The crash test's pneumatic
// launcher, several of them, each on its own:
//
//   * it charges before it fires, and you can tell: a pressure hiss from
//     where it stands, rising, and a lamp on the barrel blinking faster --
//     m_Settings.Charge seconds of warning before the shot;
//   * it leads you: aimed where you will be when the ball gets there if
//     you keep doing what you are doing, so carrying on is what gets you
//     hit and changing what you do (a dodge, a stop, a turn) is what does
//     not;
//   * it only shoots when you could get out of the way. Not while you are
//     down, getting up, or in a move (Humanoid::CanReact); not into a
//     no-fire zone -- the stretches a course commits you to, a vault, a
//     climb, a jump -- anywhere on your way there; not through anything
//     (the ball's own arc is traced, so cover is cover); and never so that
//     two shots arrive within m_Spacing of each other. A shot charging
//     when one of those stops being true is held, not fired;
//   * and it is heard: the charge, a deep thump, the ball's whistle --
//     rising as it comes at you, the Doppler shift of anything that does
//     -- and a thud where it lands.
//
// What it knows of the game is the runner (a Humanoid) and the session it
// makes its balls in; what the game asks of it is what to look at (Focus)
// and which way is away (Evade), for the camera and an unaimed dodge.
//
// And it can be fought (2026-10-22). An armed runner (Runtime/Armament.h)
// is asked about every ball about to reach him (Humanoid::WardOff): blocked,
// it drops in front of him; parried, it glances away; struck, it is sent
// back at the cannon that fired it, on a ballistic arc solved as the cannon
// solves its own. Each cannon has Settings::Health: a ball returned into it
// takes kReturnDamage, a strike what its weapon does (StrikeDamage) -- a
// staff's casts too: a bolt flies at a ball coming at him or the nearest
// cannon ahead, a nova throws every ball near him back and hurts a cannon
// beside him. At none it is wrecked -- a blast, the barrel dropped onto its
// carriage, smoke -- and fires no more. A bar over each says how it is.

#include <GS.h>

#include "GameSession.h"
#include "SceneRenderer.h"

#include <GS/Procedural/Prefabs.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Gunnery
{
public:
	struct Settings
	{
		float Speed = 19.0f;        // m/s at the muzzle
		float Mass = 12.0f;         // kg: 228 N s at 19 m/s puts him down
		float Size = 0.34f;         // m across
		float Reload = 3.0f;        // s from a shot to the next charge
		float Charge = 0.8f;        // s of warning
		float MinRange = 9.0f, MaxRange = 36.0f;
		float Arc = 80.0f;          // degrees it turns either side of its yaw
		float Phase = 0.0f;         // s on its first reload, so a battery does not fire as one
		float Health = 100.0f;      // two returned balls, or four cuts
	};

	// What hurts a cannon, out of its Health.
	static constexpr float kReturnDamage = 50.0f;

	struct Zone { glm::vec3 Min{ 0.0f }, Max{ 0.0f }; };

	// What is coming at him, for a HUD: a charging cannon, or a ball.
	struct Threat
	{
		glm::vec3 Position{ 0.0f };
		float Eta = 0.0f;           // s until it fires, or until it reaches him
		bool Ball = false;
	};

	// The sounds, made once: nothing is loaded from a file.
	struct Sounds
	{
		std::shared_ptr<GS::AudioClip> Charge, Thump, Whistle, Thud, Hit, Clang, Boom;
	};

	~Gunnery() { Silence(); }

	void Clear(GS::Scene* scene = nullptr)
	{
		Sweep(scene);
		Silence();
		m_Cannons.clear();
		m_Zones.clear();
		m_Puffs.clear();
		m_Hits = 0;
		m_Shots = 0;
		m_Held = 0;
		m_Blocks = m_Parries = m_Returns = m_Wrecked = m_Downed = 0;
		m_Bolts.clear();
		m_Clock = 0.0f;
	}

	// A cannon at `pivot` (its trunnions, 1.25 m over its base), facing
	// `yaw` degrees about up (0 along +z), drawn as the crash test's.
	void AddCannon(GS::Scene& scene, const std::string& name, const glm::vec3& pivot, float yaw, const Settings& settings)
	{
		Cannon c;
		c.Name = name;
		c.Pivot = pivot;
		c.Yaw = yaw;
		c.AimYaw = yaw;
		c.AimPitch = 4.0f;
		c.S = settings;
		c.Health = settings.Health;
		c.Clock = settings.Reload * 0.5f + settings.Phase;
		const glm::vec3 turned(0.0f, yaw, 0.0f);
		c.Base = Mesh(scene, (name + " Base").c_str(), "model:cannon.base", pivot, turned);
		c.Yoke = Mesh(scene, (name + " Yoke").c_str(), "model:cannon.yoke", pivot, turned);
		c.Barrel = Mesh(scene, (name + " Barrel").c_str(), "model:cannon.barrel", pivot, turned);
		// Its footprint and its body, so he cannot walk through it.
		Collider(scene, glm::vec3(pivot.x, pivot.y - 1.19f, pivot.z), glm::vec3(1.6f, 0.12f, 1.6f));
		Collider(scene, glm::vec3(pivot.x, pivot.y - 0.8f, pivot.z), glm::vec3(0.6f, 0.8f, 0.6f));
		m_Cannons.push_back(c);
		AimMesh(scene, m_Cannons.back());
	}

	// Where a course commits him: no shot arrives here, nor on his way.
	void AddNoFireZone(const glm::vec3& min, const glm::vec3& max) { m_Zones.push_back({ glm::min(min, max), glm::max(min, max) }); }

	size_t CannonCount() const { return m_Cannons.size(); }
	int Hits() const { return m_Hits; }
	int Shots() const { return m_Shots; }
	// Shots held back as unfair -- charging when he went into a move, say.
	int Held() const { return m_Held; }
	// What the sword and shield did: balls blocked, parried, struck back;
	// cannons wrecked.
	int Blocks() const { return m_Blocks; }
	int Parries() const { return m_Parries; }
	int Returns() const { return m_Returns; }
	int Wrecked() const { return m_Wrecked; }
	// Balls a staff's bolt shot down.
	int Downed() const { return m_Downed; }

	// Every ball gone and every cannon reloading from the start, as for a
	// new run.
	void Reset(GS::Scene* scene)
	{
		Sweep(scene);
		Silence();
		m_Puffs.clear();
		for (Cannon& c : m_Cannons)
		{
			c.Clock = c.S.Reload * 0.5f + c.S.Phase;
			c.Charging = -1.0f;
			c.Health = c.S.Health;
			c.Wrecked = false;
		}
		m_Hits = 0;
		m_Shots = 0;
		m_Held = 0;
		m_Blocks = m_Parries = m_Returns = m_Wrecked = m_Downed = 0;
		m_Bolts.clear();
		m_SinceDown = 0.0f;
	}

	// One fixed step: aim, charge, fire, and the balls in the air. `armed`
	// off holds every cannon (before the start, after the finish).
	void Step(float dt, GameSession& session, GS::Scene& scene, Humanoid* rig, bool armed)
	{
		m_Clock += dt;
		GS::PhysicsWorld3D& world = session.World();
		if (rig)
		{
			const GS::RigidBody3D& pelvis = world.GetBody(rig->PelvisBody());
			glm::vec3 v(pelvis.Velocity.x, 0.0f, pelvis.Velocity.z);
			// Smoothed: the walk's bob and sway are in the hips' own velocity.
			float k = 1.0f - std::exp(-dt / 0.25f);
			m_RunnerVelocity += (v - m_RunnerVelocity) * k;
			m_Runner = pelvis.Position;
			m_SinceDown = rig->CanReact() ? m_SinceDown + dt : 0.0f;
		}
		StepBalls(dt, session, scene, rig);
		StepCasts(dt, session, scene, rig);
		StepPuffs(dt);
		StepSword(scene, rig);
		for (Cannon& c : m_Cannons)
			StepCannon(dt, c, session, scene, rig, armed);
	}

	// What the camera should turn to: the ball that will reach him first
	// (if it will come near), else a cannon charging at him.
	bool Focus(glm::vec3& point) const
	{
		float best = 1e9f;
		bool found = false;
		for (const Ball& b : m_Balls)
		{
			float eta, miss;
			if (b.Landed || !Approach(b, eta, miss) || miss > 2.5f)
				continue;
			if (eta < best)
			{
				best = eta;
				point = b.Position;
				found = true;
			}
		}
		if (found)
			return true;
		for (const Cannon& c : m_Cannons)
			if (c.Charging >= 0.0f && c.Charging < best)
			{
				best = c.Charging;
				point = Muzzle(c);
				found = true;
			}
		if (found)
			return true;
		// Nothing coming: a cannon within a sword's reach, so a swing (and
		// Focus) turns to it -- too close to fire, it never charges, and a
		// swing at the camera's way beside it cut the air.
		float nearest = 3.0f;
		for (const Cannon& c : m_Cannons)
		{
			float distance = glm::length(glm::vec2(c.Pivot.x - m_Runner.x, c.Pivot.z - m_Runner.z));
			if (!c.Wrecked && distance < nearest)
			{
				nearest = distance;
				point = c.Pivot - glm::vec3(0.0f, 0.3f, 0.0f);
				found = true;
			}
		}
		return found;
	}

	// The way out of the most urgent ball's path: across it, to the side he
	// is already on. Zero when nothing is coming.
	glm::vec3 Evade() const
	{
		const Ball* urgent = nullptr;
		float best = 1e9f;
		for (const Ball& b : m_Balls)
		{
			float eta, miss;
			if (!b.Landed && Approach(b, eta, miss) && miss < 1.5f && eta < best)
			{
				best = eta;
				urgent = &b;
			}
		}
		if (!urgent)
			return glm::vec3(0.0f);
		glm::vec3 along(urgent->Velocity.x, 0.0f, urgent->Velocity.z);
		if (glm::length(along) < 0.1f)
			return glm::vec3(0.0f);
		along = glm::normalize(along);
		glm::vec3 across(-along.z, 0.0f, along.x);
		glm::vec3 at = urgent->Position + urgent->Velocity * best;
		glm::vec3 him = m_Runner + m_RunnerVelocity * best;
		return glm::dot(him - at, across) >= 0.0f ? across : -across;
	}

	std::vector<Threat> Threats() const
	{
		std::vector<Threat> out;
		for (const Cannon& c : m_Cannons)
			if (c.Charging >= 0.0f)
				out.push_back({ Muzzle(c), c.Charging, false });
		for (const Ball& b : m_Balls)
		{
			float eta, miss;
			if (!b.Landed && Approach(b, eta, miss) && miss < 2.5f)
				out.push_back({ b.Position, eta, true });
		}
		return out;
	}

	// The lamps, the muzzle flashes and the smoke, as draw items.
	void AppendDraws(std::vector<ExtraDraw>& out) const
	{
		std::shared_ptr<GS::Mesh> sphere = GS::MeshCache::Get("primitive:sphere");
		for (const Cannon& c : m_Cannons)
		{
			// The lamp on the barrel: dim amber at rest; charging, blinking
			// faster and faster and red at the end.
			glm::vec3 lamp = c.Pivot + BarrelTurn(c) * glm::vec3(0.0f, 0.24f, -0.05f);
			float glow = c.Wrecked ? 0.0f : 0.4f;
			glm::vec4 colour = c.Wrecked ? glm::vec4(0.15f, 0.12f, 0.1f, 1.0f) : glm::vec4(1.0f, 0.55f, 0.1f, 1.0f);
			if (c.Charging >= 0.0f)
			{
				float done = 1.0f - c.Charging / std::max(c.S.Charge, 0.01f);
				float rate = 3.0f + 14.0f * done;
				float phase = std::fmod(c.Lit * rate, 1.0f);
				glow = phase < 0.5f ? 6.0f : 0.6f;
				colour = glm::mix(glm::vec4(1.0f, 0.6f, 0.1f, 1.0f), glm::vec4(1.0f, 0.12f, 0.05f, 1.0f), done);
			}
			ExtraDraw d;
			d.Mesh = sphere;
			d.Model = glm::translate(glm::mat4(1.0f), lamp) * glm::scale(glm::mat4(1.0f), glm::vec3(0.07f));
			d.Color = colour;
			d.Emission = glow;
			d.CastShadow = false;
			out.push_back(d);

			// Its health, over it: a dark bar, filled green to red by what is
			// left -- and flashing white for a moment when it is hurt. Across
			// its own facing, so the course it covers sees it broadside.
			if (!c.Wrecked)
			{
				std::shared_ptr<GS::Mesh> cube = GS::MeshCache::Get("primitive:cube");
				float left = glm::clamp(c.Health / std::max(c.S.Health, 1.0f), 0.0f, 1.0f);
				glm::quat facing = glm::angleAxis(glm::radians(c.Yaw), glm::vec3(0.0f, 1.0f, 0.0f));
				glm::vec3 across = facing * glm::vec3(1.0f, 0.0f, 0.0f);
				glm::vec3 over = c.Pivot + glm::vec3(0.0f, 0.95f, 0.0f);
				const float length = 1.0f;
				ExtraDraw back;
				back.Mesh = cube;
				back.Model = glm::translate(glm::mat4(1.0f), over) * glm::mat4_cast(facing) * glm::scale(glm::mat4(1.0f), glm::vec3(length + 0.04f, 0.12f, 0.06f));
				back.Color = glm::vec4(0.05f, 0.05f, 0.06f, 1.0f);
				back.CastShadow = false;
				out.push_back(back);
				ExtraDraw fill;
				fill.Mesh = cube;
				glm::vec3 middle = over + across * (-0.5f * length * (1.0f - left));
				fill.Model = glm::translate(glm::mat4(1.0f), middle) * glm::mat4_cast(facing) * glm::scale(glm::mat4(1.0f), glm::vec3(length * left, 0.08f, 0.08f));
				fill.Color = c.Flash > 0.0f ? glm::vec4(1.0f) : glm::mix(glm::vec4(0.95f, 0.18f, 0.1f, 1.0f), glm::vec4(0.3f, 0.9f, 0.35f, 1.0f), left);
				fill.Emission = c.Flash > 0.0f ? 4.0f : 1.2f;
				fill.CastShadow = false;
				out.push_back(fill);
			}
		}
		// A staff's bolts: bright blue, a little bigger than they are.
		for (const Bolt& b : m_Bolts)
		{
			ExtraDraw d;
			d.Mesh = sphere;
			d.Model = glm::translate(glm::mat4(1.0f), b.Position) * glm::scale(glm::mat4(1.0f), glm::vec3(0.22f));
			d.Color = glm::vec4(0.45f, 0.75f, 1.0f, 1.0f);
			d.Emission = 7.0f;
			d.CastShadow = false;
			out.push_back(d);
		}
		for (const Puff& p : m_Puffs)
		{
			float u = p.Age / p.Life;
			float size = p.Size * (0.4f + 1.6f * std::sqrt(u)) * (u > 0.7f ? 1.0f - (u - 0.7f) / 0.3f : 1.0f);
			if (size <= 0.0f)
				continue;
			ExtraDraw d;
			d.Mesh = sphere;
			d.Model = glm::translate(glm::mat4(1.0f), p.Position) * glm::scale(glm::mat4(1.0f), glm::vec3(size));
			d.Color = p.Color;
			d.Emission = p.Emission * std::max(0.0f, 1.0f - 3.0f * u);
			d.Roughness = 0.95f;
			d.CastShadow = false;
			out.push_back(d);
		}
	}

	// The sounds, synthesised the first time they are wanted. Mono, at the
	// engine's rate; noise from a hash, so a clip is the same every run.
	static const Sounds& Clips()
	{
		static Sounds sounds = []()
		{
			Sounds s;
			const float rate = (float)GS::AudioEngine::GetSampleRate();
			auto noise = [](uint32_t i)
			{
				uint32_t x = i * 747796405u + 2891336453u;
				x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
				x = (x >> 22u) ^ x;
				return (float)(x & 0xffffffu) / (float)0x800000 - 1.0f;
			};
			auto make = [&](float seconds, const std::function<float(float, uint32_t)>& f)
			{
				unsigned int n = (unsigned int)(seconds * rate);
				std::vector<float> out(n);
				for (unsigned int i = 0; i < n; i++)
					out[i] = f((float)i / rate, i);
				return GS::AudioClip::CreateFromSamples(std::move(out), 1);
			};
			// The charge: air hissing into the tank, pitch and loudness rising
			// over the warning (0.8 s; a longer charge just plays it slower).
			{
				float lp = 0.0f;
				s.Charge = make(0.8f, [&](float t, uint32_t i)
				{
					float u = t / 0.8f;
					float n = noise(i);
					float cut = 0.08f + 0.35f * u;          // one-pole low-pass, opening up
					lp += (n - lp) * cut;
					float tone = 0.25f * std::sin(glm::two_pi<float>() * (420.0f + 900.0f * u * u) * t);
					return (0.55f * lp + tone * u) * (0.25f + 0.75f * u) * 0.6f;
				});
			}
			// The thump: a pressure wave -- a sine falling from 110 to 38 Hz,
			// a burst of low noise, a click at the front.
			{
				float lp = 0.0f, phase = 0.0f;
				s.Thump = make(1.1f, [&](float t, uint32_t i)
				{
					float f = 38.0f + 72.0f * std::exp(-t * 9.0f);
					phase += glm::two_pi<float>() * f / rate;
					lp += (noise(i) - lp) * 0.06f;
					float body = std::sin(phase) * std::exp(-t * 4.5f);
					float air = lp * 2.5f * std::exp(-t * 7.0f);
					float click = t < 0.004f ? noise(i) * (1.0f - t / 0.004f) : 0.0f;
					return std::tanh(1.6f * (0.9f * body + 0.6f * air + 0.5f * click)) * 0.9f;
				});
			}
			// The whistle: a ball spinning through the air -- a breathy
			// tone near 1 kHz, wavering a little. Looped while it flies; the
			// mixer's Doppler shift does the rest.
			{
				float bp1 = 0.0f, bp2 = 0.0f, phase = 0.0f;
				s.Whistle = make(1.0f, [&](float t, uint32_t i)
				{
					float f = 980.0f + 25.0f * std::sin(glm::two_pi<float>() * 6.0f * t);
					phase += glm::two_pi<float>() * f / rate;
					// Noise through a resonator at the tone, for the breath in it.
					float w = glm::two_pi<float>() * 1000.0f / rate, r = 0.985f;
					float y = noise(i) + 2.0f * r * std::cos(w) * bp1 - r * r * bp2;
					bp2 = bp1;
					bp1 = y;
					return (0.22f * std::sin(phase) + 0.03f * y) * 0.7f;
				});
			}
			// The thud where it lands, and the heavier one when it is him.
			auto thud = [&](float base, float length, float weight)
			{
				float lp = 0.0f, phase = 0.0f;
				return make(length, [=, &lp, &phase](float t, uint32_t i) mutable
				{
					float f = base * (1.0f + 0.8f * std::exp(-t * 30.0f));
					phase += glm::two_pi<float>() * f / rate;
					lp += (noise(i + 99991u) - lp) * 0.12f;
					return std::tanh(weight * (std::sin(phase) * std::exp(-t * 14.0f) + lp * 1.8f * std::exp(-t * 22.0f))) * 0.8f;
				});
			};
			s.Thud = thud(70.0f, 0.45f, 1.4f);
			s.Hit = thud(95.0f, 0.5f, 2.2f);
			// The clang of iron on steel: a bell's inharmonic partials, the
			// high ones dying first, over a click. Played higher for a parry
			// and a cut, lower for a block.
			s.Clang = make(0.9f, [&](float t, uint32_t i)
			{
				const float ratio[4] = { 1.0f, 2.32f, 4.25f, 6.63f }, decay[4] = { 5.0f, 8.0f, 13.0f, 19.0f }, level[4] = { 0.5f, 0.35f, 0.25f, 0.15f };
				float ring = 0.0f;
				for (int k = 0; k < 4; k++)
					ring += level[k] * std::sin(glm::two_pi<float>() * 610.0f * ratio[k] * t) * std::exp(-t * decay[k]);
				float click = t < 0.006f ? noise(i + 7u) * (1.0f - t / 0.006f) : 0.0f;
				return std::tanh(1.4f * (ring + 0.8f * click)) * 0.75f;
			});
			// A cannon wrecked: a blast -- a falling thump under a long roar of
			// low noise.
			{
				float lp = 0.0f, lp2 = 0.0f, phase = 0.0f;
				s.Boom = make(1.8f, [&](float t, uint32_t i)
				{
					float f = 30.0f + 70.0f * std::exp(-t * 6.0f);
					phase += glm::two_pi<float>() * f / rate;
					lp += (noise(i + 31337u) - lp) * 0.05f;
					lp2 += (lp - lp2) * 0.2f;
					float body = std::sin(phase) * std::exp(-t * 3.0f);
					float roar = lp2 * 6.0f * std::exp(-t * 2.2f);
					float crack = t < 0.01f ? noise(i) * (1.0f - t / 0.01f) : 0.0f;
					return std::tanh(2.2f * (body + roar + 0.7f * crack)) * 0.95f;
				});
			}
			return s;
		}();
		return sounds;
	}

private:
	struct Cannon
	{
		std::string Name;
		glm::vec3 Pivot{ 0.0f };
		float Yaw = 0.0f;
		Settings S;
		float AimYaw = 0.0f, AimPitch = 0.0f;   // where the barrel points now, degrees
		float Clock = 0.0f;                     // reloading: s until it may charge
		float Charging = -1.0f;                 // s left of a charge, or -1
		float Lit = 0.0f;                       // s since the charge began, for the lamp
		float PlannedArrival = 0.0f;            // when this charge's shot would reach him
		float Health = 100.0f;
		bool Wrecked = false;
		float Flash = 0.0f;                     // s left of the bar's flash, after a hurt
		float Smoke = 0.0f;                     // s until the next puff from a hurt cannon
		uint32_t LastSwing = 0;                 // the sword's swing that last cut it: once a swing
		GS::VoiceHandle ChargeVoice = GS::InvalidVoice;
		GS::EntityId Base = GS::InvalidEntity, Yoke = GS::InvalidEntity, Barrel = GS::InvalidEntity;
	};
	struct Ball
	{
		GS::EntityId Entity = GS::InvalidEntity;
		GS::PhysicsWorld3D::BodyHandle Body = 0;
		glm::vec3 Position{ 0.0f }, Velocity{ 0.0f };
		float Age = 0.0f, SinceLanded = -1.0f;
		float Arrival = 0.0f;                   // when it was aimed to reach him (m_Clock)
		float Radius = 0.17f, Mass = 12.0f;
		int From = -1;                          // the cannon that fired it
		bool Landed = false, Struck = false;
		bool Warded = false;                    // met by the sword or the shield
		bool Returned = false;                  // struck back, and not yet into anything
		GS::VoiceHandle Whistle = GS::InvalidVoice;
	};
	struct Puff
	{
		glm::vec3 Position{ 0.0f }, Velocity{ 0.0f };
		float Age = 0.0f, Life = 1.0f, Size = 0.2f, Emission = 0.0f;
		glm::vec4 Color{ 1.0f };
	};
	struct Bolt
	{
		glm::vec3 Position{ 0.0f }, Velocity{ 0.0f };
		float Age = 0.0f, Damage = 30.0f;
	};
	struct Aim
	{
		glm::vec3 Point{ 0.0f };
		float Yaw = 0.0f, Pitch = 0.0f, Flight = 0.0f;   // degrees, s
		bool Ok = false;
	};

	static GS::EntityId Mesh(GS::Scene& scene, const char* name, const char* source, const glm::vec3& at, const glm::vec3& rotation)
	{
		GS::Entity entity = scene.CreateEntity(name);
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Rotation = rotation;
		GS::MeshComponent mesh;
		mesh.SourcePath = source;
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		entity.Add<GS::MeshComponent>(mesh);
		return entity.GetId();
	}

	static void Collider(GS::Scene& scene, const glm::vec3& at, const glm::vec3& size)
	{
		GS::Entity entity = scene.CreateEntity("Cannon Collider");
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Scale = size;
		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:cube";
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Visible = false;
		entity.Add<GS::MeshComponent>(mesh);
		GS::PhysicsComponent body;
		body.Type = GS::BodyType::Static;
		entity.Add<GS::PhysicsComponent>(body);
	}

	static glm::quat Turn(float yawDegrees, float pitchDegrees)
	{
		return glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(-glm::radians(pitchDegrees), glm::vec3(1.0f, 0.0f, 0.0f));
	}
	static glm::quat BarrelTurn(const Cannon& c) { return Turn(c.AimYaw, c.AimPitch); }
	static glm::vec3 Muzzle(const Cannon& c) { return c.Pivot + BarrelTurn(c) * glm::vec3(0.0f, 0.0f, GS::Prefabs::CannonMuzzle); }

	static glm::vec3 Degrees(const glm::quat& q)
	{
		float x, y, z;
		glm::extractEulerAngleXYZ(glm::mat4_cast(q), x, y, z);
		return glm::degrees(glm::vec3(x, y, z));
	}

	void AimMesh(GS::Scene& scene, const Cannon& c) const
	{
		if (auto* t = scene.GetComponent<GS::TransformComponent>(c.Barrel))
			t->Rotation = Degrees(BarrelTurn(c));
		if (auto* t = scene.GetComponent<GS::TransformComponent>(c.Yoke))
			t->Rotation = glm::vec3(0.0f, c.AimYaw, 0.0f);
	}

	// The low root of the projectile equation for a target `x` away and `y`
	// up, from the muzzle -- which moves with the pitch, so solved again from
	// where the last guess put it (as LauncherSystem::SolvePitch does).
	bool SolvePitch(const Cannon& c, float yawDegrees, const glm::vec3& point, float& pitchDegrees, float& flight) const
	{
		const float g = 9.81f, v = c.S.Speed;
		float pitch = 0.0f;
		for (int i = 0; i < 4; i++)
		{
			glm::vec3 from = c.Pivot + Turn(yawDegrees, glm::degrees(pitch)) * glm::vec3(0.0f, 0.0f, GS::Prefabs::CannonMuzzle);
			glm::vec3 to = point - from;
			float x = glm::length(glm::vec2(to.x, to.z)), y = to.y;
			float root = v * v * v * v - g * (g * x * x + 2.0f * y * v * v);
			if (root < 0.0f || x < 1e-3f)
				return false;
			pitch = std::atan((v * v - std::sqrt(root)) / (g * x));
			flight = x / std::max(v * std::cos(pitch), 1e-3f);
		}
		pitchDegrees = glm::degrees(pitch);
		return pitchDegrees > -25.0f && pitchDegrees < 45.0f;
	}

	// Where to aim to meet him: where his chest will be after the flight, if
	// he keeps his velocity -- the flight depending on where that is, so
	// worked out a few times over.
	Aim Solve(const Cannon& c, const Humanoid& rig) const
	{
		Aim a;
		glm::vec3 him = m_Runner;
		float chest = 0.28f + rig.RestPelvisHeight();
		float flight = glm::length(him - c.Pivot) / std::max(c.S.Speed, 1.0f);
		for (int i = 0; i < 4; i++)
		{
			glm::vec3 p = him + m_RunnerVelocity * flight;
			float ground = rig.GroundY(glm::vec3(p.x, him.y + 0.5f, p.z), -1000.0f);
			p.y = ground + chest;
			glm::vec3 to = p - c.Pivot;
			float yaw = glm::degrees(std::atan2(to.x, to.z));
			float pitch, t;
			if (!SolvePitch(c, yaw, p, pitch, t))
				return a;
			a.Point = p;
			a.Yaw = yaw;
			a.Pitch = pitch;
			a.Flight = t;
			flight = t;
		}
		glm::vec3 flat = a.Point - c.Pivot;
		float range = glm::length(glm::vec2(flat.x, flat.z));
		float off = a.Yaw - c.Yaw;
		while (off > 180.0f) off -= 360.0f;
		while (off < -180.0f) off += 360.0f;
		a.Ok = range >= c.S.MinRange && range <= c.S.MaxRange && std::fabs(off) <= c.S.Arc;
		return a;
	}

	static bool InZone(const Zone& z, const glm::vec3& p)
	{
		return p.x >= z.Min.x && p.x <= z.Max.x && p.y >= z.Min.y && p.y <= z.Max.y && p.z >= z.Min.z && p.z <= z.Max.z;
	}

	// Whether a shot along `a` would be fair now: he can react, his way for
	// the whole flight keeps out of the no-fire zones, the ball's own arc
	// reaches him without meeting anything first, and no other shot arrives
	// within m_Spacing of it.
	bool Fair(const Cannon& c, const Aim& a, float arrival, const GS::PhysicsWorld3D& world, const Humanoid& rig) const
	{
		if (!a.Ok || !rig.CanReact() || m_SinceDown < m_GraceAfterDown || a.Flight < m_MinFlight)
			return false;
		for (float t = 0.0f; t <= a.Flight + 0.3f; t += 0.1f)
		{
			glm::vec3 p = m_Runner + m_RunnerVelocity * t;
			for (const Zone& z : m_Zones)
				if (InZone(z, p))
					return false;
		}
		for (const Ball& b : m_Balls)
			if (!b.Landed && std::fabs(b.Arrival - arrival) < m_Spacing)
				return false;
		for (const Cannon& other : m_Cannons)
			if (&other != &c && other.Charging >= 0.0f && std::fabs(other.PlannedArrival - arrival) < m_Spacing)
				return false;
		// The arc, step by step: nothing solid before it reaches him.
		std::vector<GS::PhysicsWorld3D::BodyHandle> ignore = rig.Bodies();
		for (const Ball& b : m_Balls)
			ignore.push_back(b.Body);
		glm::vec3 d = Turn(a.Yaw, a.Pitch) * glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 p = c.Pivot + d * (GS::Prefabs::CannonMuzzle + 0.5f * c.S.Size);
		glm::vec3 v = d * c.S.Speed;
		const float step = 1.0f / 60.0f;
		for (float t = 0.0f; t < a.Flight - 0.05f; t += step)
		{
			glm::vec3 next = p + v * step + glm::vec3(0.0f, -0.5f * 9.81f * step * step, 0.0f);
			GS::PhysicsWorld3D::RayHit hit = world.Raycast(p, next - p, glm::length(next - p) + 0.5f * c.S.Size, ignore);
			if (hit.Hit)
				return false;
			v.y -= 9.81f * step;
			p = next;
		}
		return true;
	}

	void StepCannon(float dt, Cannon& c, GameSession& session, GS::Scene& scene, Humanoid* rig, bool armed)
	{
		c.Flash = std::max(c.Flash - dt, 0.0f);
		// Hurt, it smokes -- the more the worse it is; wrecked, it pours.
		if (c.Wrecked || c.Health < c.S.Health)
		{
			c.Smoke -= dt;
			if (c.Smoke <= 0.0f)
			{
				float left = c.Wrecked ? 0.0f : c.Health / std::max(c.S.Health, 1.0f);
				c.Smoke = c.Wrecked ? 0.18f : 0.3f + 1.2f * left;
				uint32_t h = (m_PuffCount++ + 1u) * 2654435761u;
				auto r = [&](int n) { return (float)((h >> (n * 5)) & 31u) / 31.0f - 0.5f; };
				Puff smoke;
				smoke.Position = c.Pivot + glm::vec3(r(0) * 0.4f, 0.1f, r(1) * 0.4f);
				smoke.Velocity = glm::vec3(r(2) * 0.4f, 1.2f + 0.4f * r(3), r(4) * 0.4f);
				smoke.Life = 1.6f + 0.4f * r(5);
				smoke.Size = c.Wrecked ? 0.32f : 0.2f;
				float grey = c.Wrecked ? 0.16f : 0.4f;
				smoke.Color = glm::vec4(grey, grey, grey * 1.05f, 1.0f);
				m_Puffs.push_back(smoke);
			}
		}
		if (c.Wrecked)
			return;
		GS::PhysicsWorld3D& world = session.World();
		Aim a;
		if (rig)
			a = Solve(c, *rig);
		// The barrel follows its aim at up to 140 degrees a second -- on
		// him while it could shoot, back to its rest when it could not.
		float wantYaw = a.Ok ? a.Yaw : c.Yaw, wantPitch = a.Ok ? a.Pitch : 4.0f;
		float dy = wantYaw - c.AimYaw;
		while (dy > 180.0f) dy -= 360.0f;
		while (dy < -180.0f) dy += 360.0f;
		float turn = 140.0f * dt;
		c.AimYaw += glm::clamp(dy, -turn, turn);
		c.AimPitch += glm::clamp(wantPitch - c.AimPitch, -turn, turn);
		AimMesh(scene, c);

		if (c.Charging >= 0.0f)
		{
			c.Lit += dt;
			c.Charging -= dt;
			c.PlannedArrival = m_Clock + std::max(c.Charging, 0.0f) + a.Flight;
			if (!armed || !rig || !Fair(c, a, c.PlannedArrival, world, *rig))
			{
				// Held: what made it fair has gone. It vents and reloads, a
				// little sooner than after a shot.
				c.Charging = -1.0f;
				c.Clock = 0.9f;
				m_Held++;
				StopVoice(c.ChargeVoice);
				return;
			}
			if (c.Charging <= 0.0f)
			{
				c.Charging = -1.0f;
				StopVoice(c.ChargeVoice);
				// On the aim exactly: the barrel has had the whole charge to
				// come round, and is drawn there.
				c.AimYaw = a.Yaw;
				c.AimPitch = a.Pitch;
				AimMesh(scene, c);
				Fire(c, a, session, scene);
				c.Clock = c.S.Reload;
			}
			return;
		}
		c.Clock -= dt;
		if (c.Clock > 0.0f || !armed || !rig)
			return;
		// Barrel on him (near enough), and fair as things stand: charge.
		float off = a.Yaw - c.AimYaw;
		while (off > 180.0f) off -= 360.0f;
		while (off < -180.0f) off += 360.0f;
		if (!a.Ok || std::fabs(off) > 25.0f)
			return;
		if (!Fair(c, a, m_Clock + c.S.Charge + a.Flight, world, *rig))
			return;
		c.Charging = c.S.Charge;
		c.Lit = 0.0f;
		c.PlannedArrival = m_Clock + c.S.Charge + a.Flight;
		if (GS::AudioEngine::IsAvailable())
		{
			GS::Audio3DParams p;
			p.Position = Muzzle(c);
			p.Volume = 0.9f;
			p.Pitch = 0.8f / std::max(c.S.Charge, 0.2f);
			p.MinDistance = 4.0f;
			p.MaxDistance = 60.0f;
			p.DopplerFactor = 0.0f;
			c.ChargeVoice = GS::AudioEngine::PlayAt(Clips().Charge, p);
		}
	}

	void Fire(Cannon& c, const Aim& a, GameSession& session, GS::Scene& scene)
	{
		glm::vec3 d = Turn(a.Yaw, a.Pitch) * glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 at = c.Pivot + d * (GS::Prefabs::CannonMuzzle + 0.5f * c.S.Size + 0.02f);
		GS::RigidBody3D body = GS::RigidBody3D::MakeSphere(at, 0.5f * c.S.Size, c.S.Mass);
		body.Friction = 0.6f;
		body.Restitution = 0.2f;
		body.LinearDamping = 0.0f;
		body.Velocity = d * c.S.Speed;

		Ball b;
		GS::Entity entity = scene.CreateEntity("Cannonball");
		GS::TransformComponent* transform = entity.Get<GS::TransformComponent>();
		transform->Position = at;
		transform->Scale = glm::vec3(c.S.Size);
		GS::MeshComponent mesh;
		mesh.SourcePath = "primitive:sphere";
		mesh.Geometry = GS::MeshCache::Get(mesh.SourcePath);
		mesh.Color = glm::vec4(0.09f, 0.09f, 0.1f, 1.0f);
		mesh.Roughness = 0.3f;
		mesh.Metallic = 0.85f;
		entity.Add<GS::MeshComponent>(mesh);
		int reuse = -1;
		if (!m_Free.empty())
		{
			reuse = (int)m_Free.back();
			m_Free.pop_back();
		}
		b.Entity = entity.GetId();
		b.Body = session.BodyHost().AttachBody(b.Entity, body, reuse);
		b.Position = at;
		b.Velocity = body.Velocity;
		b.Arrival = m_Clock + a.Flight;
		b.Radius = 0.5f * c.S.Size;
		b.Mass = c.S.Mass;
		b.From = (int)(&c - m_Cannons.data());
		m_Shots++;

		if (GS::AudioEngine::IsAvailable())
		{
			GS::Audio3DParams p;
			p.Position = at;
			p.Volume = 1.0f;
			p.MinDistance = 7.0f;
			p.MaxDistance = 140.0f;
			p.DopplerFactor = 0.0f;
			GS::AudioEngine::PlayAt(Clips().Thump, p);
			GS::Audio3DParams w;
			w.Position = at;
			w.Velocity = b.Velocity;
			w.Volume = 0.55f;
			w.Loop = true;
			w.MinDistance = 2.5f;
			w.MaxDistance = 45.0f;
			w.DopplerFactor = 1.0f;
			b.Whistle = GS::AudioEngine::PlayAt(Clips().Whistle, w);
		}
		m_Balls.push_back(b);

		// The flash, and the blast of air and dust out of the muzzle.
		Puff flash;
		flash.Position = at;
		flash.Life = 0.12f;
		flash.Size = 0.32f;
		flash.Color = glm::vec4(1.0f, 0.85f, 0.6f, 1.0f);
		flash.Emission = 8.0f;
		m_Puffs.push_back(flash);
		for (int k = 0; k < 7; k++)
		{
			uint32_t h = (uint32_t)(m_Shots * 7 + k) * 2654435761u;
			auto r = [&](int n) { return (float)((h >> (n * 5)) & 31u) / 31.0f - 0.5f; };
			Puff smoke;
			smoke.Position = at;
			smoke.Velocity = d * (3.0f + 2.0f * r(0)) + glm::vec3(r(1), r(2) + 0.6f, r(3)) * 1.6f;
			smoke.Life = 0.9f + 0.4f * r(4);
			smoke.Size = 0.16f + 0.05f * r(5);
			smoke.Color = glm::vec4(0.82f, 0.8f, 0.76f, 1.0f);
			smoke.Emission = 0.3f;
			m_Puffs.push_back(smoke);
		}
	}

	// The time until a ball is nearest him, and how near: both moving on as
	// they are, his chest against its centre.
	bool Approach(const Ball& b, float& eta, float& miss) const
	{
		glm::vec3 him = m_Runner + glm::vec3(0.0f, 0.3f, 0.0f);
		glm::vec3 rel = b.Position - him, vel = b.Velocity - m_RunnerVelocity;
		float speed2 = glm::dot(vel, vel);
		if (speed2 < 1e-4f)
			return false;
		eta = -glm::dot(rel, vel) / speed2;
		if (eta < 0.0f || eta > 3.0f)
			return false;
		miss = glm::length(rel + vel * eta);
		return true;
	}

	void StepBalls(float dt, GameSession& session, GS::Scene& scene, Humanoid* rig)
	{
		GS::PhysicsWorld3D& world = session.World();
		for (Ball& b : m_Balls)
		{
			GS::RigidBody3D& body = world.GetBody(b.Body);
			glm::vec3 before = b.Velocity;
			b.Position = body.Position;
			b.Velocity = body.Velocity;
			b.Age += dt;
			if (b.SinceLanded >= 0.0f)
				b.SinceLanded += dt;
			if (b.Whistle != GS::InvalidVoice)
				GS::AudioEngine::SetVoicePosition(b.Whistle, b.Position, b.Velocity);

			// About to reach a runner with a sword and shield: his to answer,
			// before the next step can carry it into him.
			if (rig && rig->Armed() && !b.Warded && !b.Landed)
			{
				Humanoid::WardResult ward = rig->WardOff(b.Position, b.Velocity, b.Radius, b.Mass, dt);
				if (ward.Kind != Humanoid::Ward::None)
				{
					glm::vec3 v = ward.Kind == Humanoid::Ward::Struck ? ReturnVelocity(b, ward.Velocity) : ward.Velocity;
					body.Velocity = v;
					body.AngularVelocity = glm::vec3(0.0f);
					body.Awake = true;
					body.SleepTimer = 0.0f;
					b.Velocity = v;
					b.Warded = true;
					b.Struck = true;   // not a hit: nothing after this counts against him
					b.Returned = ward.Kind == Humanoid::Ward::Struck;
					if (ward.Kind == Humanoid::Ward::Blocked) m_Blocks++;
					else if (ward.Kind == Humanoid::Ward::Parried) m_Parries++;
					else m_Returns++;
					if (!b.Returned)
						StopVoice(b.Whistle);
					PlayOnce(Clips().Clang, ward.Point, 1.0f, ward.Kind == Humanoid::Ward::Blocked ? 0.75f : 1.15f);
					Sparks(ward.Point, 8);
					continue;
				}
			}
			// Struck back: into a cannon, it hurts it.
			if (b.Returned)
				for (Cannon& c : m_Cannons)
					if (!c.Wrecked && InCannon(c, b.Position, b.Radius))
					{
						b.Returned = false;
						Hurt(c, kReturnDamage, scene, b.Position);
						break;
					}
			// A blow can come and go between two of GameSession's substeps,
			// and only the last one's contacts are kept: near enough to his
			// trunk counts as one too.
			if (rig && !b.Struck)
				for (GS::PhysicsWorld3D::BodyHandle h : { rig->PelvisBody(), rig->HeadBody() })
					if (glm::length(world.GetBody(h).Position - b.Position) < b.Radius + 0.15f)
					{
						b.Struck = true;
						m_Hits++;
						PlayOnce(Clips().Hit, b.Position, 1.0f);
					}
			// What it touched this step: him, or anything else.
			for (const GS::Contact3D& contact : world.GetContacts())
			{
				if (contact.A != b.Body && contact.B != b.Body)
					continue;
				unsigned int other = contact.A == b.Body ? contact.B : contact.A;
				bool him = false;
				if (rig)
					for (GS::PhysicsWorld3D::BodyHandle h : rig->Bodies())
						him = him || h == other;
				if (him && !b.Struck)
				{
					b.Struck = true;
					m_Hits++;
					PlayOnce(Clips().Hit, b.Position, 1.0f);
				}
				if (!b.Landed)
				{
					b.Landed = true;
					b.SinceLanded = 0.0f;
					StopVoice(b.Whistle);
					if (!him && glm::length(before) > 3.0f)
						PlayOnce(Clips().Thud, b.Position, glm::clamp(glm::length(before) / 18.0f, 0.3f, 1.0f));
					for (int k = 0; k < 5; k++)
					{
						uint32_t h = (uint32_t)(m_Shots * 13 + k + (uint32_t)(b.Age * 1000.0f)) * 2246822519u;
						auto r = [&](int n) { return (float)((h >> (n * 5)) & 31u) / 31.0f - 0.5f; };
						Puff dust;
						dust.Position = b.Position - glm::vec3(0.0f, 0.1f, 0.0f);
						dust.Velocity = glm::vec3(r(0) * 2.5f, 0.8f + r(1), r(2) * 2.5f);
						dust.Life = 0.7f + 0.3f * r(3);
						dust.Size = 0.12f;
						dust.Color = glm::vec4(0.62f, 0.56f, 0.46f, 1.0f);
						m_Puffs.push_back(dust);
					}
				}
			}
		}
		// Gone two seconds after it lands, or six after it was fired, or off
		// the world: a ball left rolling about the course is a hazard no
		// cannon chose, and kept every step split (GameSession's substeps).
		for (size_t i = 0; i < m_Balls.size();)
		{
			Ball& b = m_Balls[i];
			if (b.SinceLanded > 2.0f || b.Age > 6.0f || b.Position.y < -5.0f)
			{
				Retire(b, session, scene);
				m_Balls.erase(m_Balls.begin() + (long)i);
			}
			else
				i++;
		}
	}

	// Which way to send a ball struck back: at the cannon that fired it (or,
	// that one wrecked, the nearest still standing within 60 degrees of where
	// the strike sent it), on the low arc that reaches it at kReturnSpeed --
	// the projectile equation from where the ball is, as SolvePitch solves it
	// from a muzzle. Out of reach, the strike's own way.
	glm::vec3 ReturnVelocity(const Ball& b, const glm::vec3& struck) const
	{
		const Cannon* target = nullptr;
		if (b.From >= 0 && b.From < (int)m_Cannons.size() && !m_Cannons[(size_t)b.From].Wrecked)
			target = &m_Cannons[(size_t)b.From];
		if (!target && glm::length(struck) > 1e-3f)
		{
			float best = 60.0f;
			glm::vec3 way = glm::normalize(struck);
			for (const Cannon& c : m_Cannons)
			{
				glm::vec3 to = c.Pivot - b.Position;
				float distance = glm::length(to);
				if (c.Wrecked || distance < 1e-3f || distance > best || glm::dot(to / distance, way) < 0.5f)
					continue;
				best = distance;
				target = &c;
			}
		}
		if (!target)
			return struck;
		const float g = 9.81f, v = kReturnSpeed;
		glm::vec3 to = (target->Pivot - glm::vec3(0.0f, 0.35f, 0.0f)) - b.Position;
		float x = glm::length(glm::vec2(to.x, to.z)), y = to.y;
		float root = v * v * v * v - g * (g * x * x + 2.0f * y * v * v);
		if (root < 0.0f || x < 1e-3f)
			return struck;
		float pitch = std::atan((v * v - std::sqrt(root)) / (g * x));
		glm::vec3 flat(to.x / x, 0.0f, to.z / x);
		return flat * (v * std::cos(pitch)) + glm::vec3(0.0f, v * std::sin(pitch), 0.0f);
	}

	// Inside what a cannon is, with `pad` to spare: the carriage and yoke up
	// to the top of the barrel, a little over a metre across -- and the
	// barrel itself out to the muzzle, which is what sticks out at him: with
	// the carriage alone, a swing from where he could stand stopped 10 cm short.
	static bool InCannon(const Cannon& c, const glm::vec3& p, float pad)
	{
		glm::vec3 d = p - c.Pivot;
		if (std::fabs(d.x) <= 0.55f + pad && std::fabs(d.z) <= 0.55f + pad && d.y >= -1.25f - pad && d.y <= 0.45f + pad)
			return true;
		glm::vec3 muzzle = Muzzle(c) - c.Pivot;
		float along = glm::clamp(glm::dot(d, muzzle) / std::max(glm::dot(muzzle, muzzle), 1e-6f), 0.0f, 1.0f);
		return glm::length(d - muzzle * along) <= 0.2f + pad;
	}

	// A strike against the cannons: anywhere along an edge inside one hurts
	// it, by what the strike does, once a strike.
	void StepSword(GS::Scene& scene, Humanoid* rig)
	{
		if (!rig || !rig->Armed() || !rig->SwordCutting())
			return;
		glm::vec3 hilt[2], tip[2];
		int edges = rig->StrikeEdges(hilt, tip);
		for (Cannon& c : m_Cannons)
		{
			if (c.Wrecked || c.LastSwing == rig->SwingId())
				continue;
			bool struck = false;
			for (int e = 0; e < edges && !struck; e++)
				for (int k = 0; k <= 6; k++)
				{
					glm::vec3 p = glm::mix(hilt[e], tip[e], (float)k / 6.0f);
					if (InCannon(c, p, 0.05f))
					{
						c.LastSwing = rig->SwingId();
						PlayOnce(Clips().Clang, p, 0.9f, 0.9f);
						Hurt(c, rig->StrikeDamage(), scene, p);
						struck = true;
						break;
					}
				}
		}
	}

	// A staff's casts made real. A bolt flies straight at 32 m/s for 1.5 s --
	// at a ball coming at him within 40 degrees of where he faced, else at the
	// nearest standing cannon within 30 degrees (aimed by the body's facing
	// alone it missed: forgiving, as the guard is), else straight on --
	// hurting the first cannon it is inside, or shooting down the first ball
	// it meets. A nova, about him:
	// every ball within 3.5 m struck back at its cannon, every cannon within
	// it hurt.
	void StepCasts(float dt, GameSession& session, GS::Scene& scene, Humanoid* rig)
	{
		std::vector<Humanoid::Cast> casts;
		if (rig)
			rig->TakeCasts(casts);
		GS::PhysicsWorld3D& world = session.World();
		for (const Humanoid::Cast& cast : casts)
		{
			if (cast.Nova)
			{
				for (Ball& b : m_Balls)
					if (!b.Landed && glm::length(b.Position - cast.At) < 3.5f)
					{
						glm::vec3 v = ReturnVelocity(b, glm::normalize(b.Position - cast.At + glm::vec3(0.0f, 0.3f, 0.0f)) * 20.0f);
						GS::RigidBody3D& body = world.GetBody(b.Body);
						body.Velocity = v;
						body.Awake = true;
						body.SleepTimer = 0.0f;
						b.Velocity = v;
						b.Warded = b.Struck = b.Returned = true;
						m_Returns++;
					}
				for (Cannon& c : m_Cannons)
					if (!c.Wrecked && glm::length(glm::vec2(c.Pivot.x - cast.At.x, c.Pivot.z - cast.At.z)) < 3.5f)
						Hurt(c, cast.Damage, scene, c.Pivot);
				for (int k = 0; k < 18; k++)
				{
					float a = glm::two_pi<float>() * (float)k / 18.0f;
					Puff ring;
					ring.Position = cast.At + glm::vec3(0.0f, -0.6f, 0.0f);
					ring.Velocity = glm::vec3(std::cos(a), 0.15f, std::sin(a)) * 9.0f;
					ring.Life = 0.35f;
					ring.Size = 0.14f;
					ring.Color = glm::vec4(0.45f, 0.75f, 1.0f, 1.0f);
					ring.Emission = 6.0f;
					m_Puffs.push_back(ring);
				}
				PlayOnce(Clips().Thump, cast.At, 0.8f, 1.6f);
				continue;
			}
			Bolt bolt;
			bolt.Position = cast.At;
			bolt.Damage = cast.Damage;
			glm::vec3 way = glm::length(cast.Direction) > 1e-4f ? glm::normalize(cast.Direction) : glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 aim = way;
			float best = 45.0f;
			// A ball coming at him ahead first: aimed where it will be when
			// the bolt gets there (|B + V t - O| = S t, then its fall over t).
			// Only at the cannon, the bolt crossed a ball's arc 40-60 cm
			// above or below it and the ball came on.
			bool atBall = false;
			for (const Ball& b : m_Balls)
			{
				glm::vec3 rel = b.Position - cast.At;
				glm::vec3 flat(rel.x, 0.0f, rel.z);
				if (b.Landed || b.Returned || glm::dot(b.Velocity, rel) >= 0.0f || glm::length(flat) < 1e-3f
					|| glm::dot(glm::normalize(flat), way) < std::cos(glm::radians(40.0f)) || glm::length(rel) > 30.0f)
					continue;
				const float S = 32.0f;
				float qa = glm::dot(b.Velocity, b.Velocity) - S * S, qb = 2.0f * glm::dot(rel, b.Velocity), qc = glm::dot(rel, rel);
				float disc = qb * qb - 4.0f * qa * qc;
				if (disc < 0.0f || std::fabs(qa) < 1e-4f)
					continue;
				float t = (-qb - std::sqrt(disc)) / (2.0f * qa);
				if (t < 0.0f)
					t = (-qb + std::sqrt(disc)) / (2.0f * qa);
				if (t < 0.0f)
					continue;
				glm::vec3 meet = b.Position + b.Velocity * t + glm::vec3(0.0f, -4.905f * t * t, 0.0f);
				aim = glm::normalize(meet - cast.At);
				atBall = true;
				break;
			}
			for (const Cannon& c : m_Cannons)
			{
				glm::vec3 to = c.Pivot - glm::vec3(0.0f, 0.3f, 0.0f) - cast.At;
				float distance = glm::length(to);
				glm::vec3 flat(to.x, 0.0f, to.z);
				if (atBall || c.Wrecked || distance > best || glm::length(flat) < 1e-3f || glm::dot(glm::normalize(flat), way) < std::cos(glm::radians(30.0f)))
					continue;
				best = distance;
				aim = to / distance;
			}
			bolt.Velocity = aim * 32.0f;
			m_Bolts.push_back(bolt);
			PlayOnce(Clips().Whistle, cast.At, 0.5f, 1.8f);
		}

		for (Bolt& bolt : m_Bolts)
		{
			glm::vec3 from = bolt.Position;
			bolt.Position += bolt.Velocity * dt;
			bolt.Age += dt;
			// The way it went this step, in a few samples: at 32 m/s it moves
			// 53 cm a step, more than a ball is across.
			for (int k = 1; k <= 3 && bolt.Age < 9.0f; k++)
			{
				glm::vec3 p = glm::mix(from, bolt.Position, (float)k / 3.0f);
				for (Cannon& c : m_Cannons)
					if (!c.Wrecked && InCannon(c, p, 0.12f))
					{
						Hurt(c, bolt.Damage, scene, p);
						bolt.Age = 9.0f;
						break;
					}
				for (Ball& b : m_Balls)
					if (bolt.Age < 9.0f && !b.Landed && !b.Returned && glm::length(b.Position - p) < b.Radius + 0.25f)
					{
						// Shot down: it bursts, and is gone.
						Sparks(b.Position, 14);
						PlayOnce(Clips().Clang, b.Position, 0.9f, 1.4f);
						b.Landed = true;
						b.SinceLanded = 10.0f;
						b.Struck = true;
						StopVoice(b.Whistle);
						m_Downed++;
						bolt.Age = 9.0f;
					}
			}
			Puff trail;
			trail.Position = bolt.Position;
			trail.Life = 0.18f;
			trail.Size = 0.10f;
			trail.Color = glm::vec4(0.45f, 0.75f, 1.0f, 1.0f);
			trail.Emission = 4.0f;
			m_Puffs.push_back(trail);
		}
		m_Bolts.erase(std::remove_if(m_Bolts.begin(), m_Bolts.end(), [](const Bolt& b) { return b.Age > 1.5f; }), m_Bolts.end());
	}

	void Hurt(Cannon& c, float damage, GS::Scene& scene, const glm::vec3& at)
	{
		c.Health = std::max(c.Health - damage, 0.0f);
		c.Flash = 0.2f;
		c.Smoke = std::min(c.Smoke, 0.1f);
		Sparks(at, 10);
		PlayOnce(Clips().Hit, at, 1.0f);
		if (c.Health > 0.0f)
			return;
		// Wrecked: it stops, its barrel drops onto the carriage, and it goes
		// up -- a flash, and a column of dark smoke.
		c.Wrecked = true;
		c.Charging = -1.0f;
		StopVoice(c.ChargeVoice);
		c.AimPitch = -28.0f;
		AimMesh(scene, c);
		m_Wrecked++;
		PlayOnce(Clips().Boom, c.Pivot, 1.0f);
		Puff flash;
		flash.Position = c.Pivot;
		// Small and short: puffs swell to twice their size, and at 1.1 m the
		// flash filled the picture as an opaque ball 2 m across.
		flash.Life = 0.15f;
		flash.Size = 0.45f;
		flash.Color = glm::vec4(1.0f, 0.7f, 0.35f, 1.0f);
		flash.Emission = 10.0f;
		m_Puffs.push_back(flash);
		for (int k = 0; k < 14; k++)
		{
			uint32_t h = (m_PuffCount++ + 1u) * 2246822519u;
			auto r = [&](int n) { return (float)((h >> (n * 5)) & 31u) / 31.0f - 0.5f; };
			Puff smoke;
			smoke.Position = c.Pivot + glm::vec3(r(0), r(1) * 0.5f, r(2)) * 0.5f;
			smoke.Velocity = glm::vec3(r(3) * 4.0f, 2.0f + 2.0f * r(4), r(5) * 4.0f);
			smoke.Life = 1.4f + 0.5f * r(1);
			smoke.Size = 0.35f;
			smoke.Color = glm::vec4(0.2f, 0.19f, 0.18f, 1.0f);
			smoke.Emission = 0.2f;
			m_Puffs.push_back(smoke);
		}
		Sparks(c.Pivot, 16);
	}

	// Sparks: hot specks flung out from a strike, gone in a fifth of a second.
	void Sparks(const glm::vec3& at, int count)
	{
		for (int k = 0; k < count; k++)
		{
			uint32_t h = (m_PuffCount++ + 1u) * 3266489917u;
			auto r = [&](int n) { return (float)((h >> (n * 5)) & 31u) / 31.0f - 0.5f; };
			Puff spark;
			spark.Position = at;
			spark.Velocity = glm::vec3(r(0), r(1) + 0.4f, r(2)) * 9.0f;
			spark.Life = 0.15f + 0.15f * (r(3) + 0.5f);
			spark.Size = 0.035f;
			spark.Color = glm::vec4(1.0f, 0.75f, 0.35f, 1.0f);
			spark.Emission = 9.0f;
			m_Puffs.push_back(spark);
		}
	}

	void StepPuffs(float dt)
	{
		for (Puff& p : m_Puffs)
		{
			p.Age += dt;
			p.Position += p.Velocity * dt;
			p.Velocity *= std::exp(-3.0f * dt);
			p.Velocity.y += 0.4f * dt;
		}
		m_Puffs.erase(std::remove_if(m_Puffs.begin(), m_Puffs.end(), [](const Puff& p) { return p.Age >= p.Life; }), m_Puffs.end());
	}

	void Retire(Ball& b, GameSession& session, GS::Scene& scene)
	{
		StopVoice(b.Whistle);
		session.BodyHost().RetireBody(b.Entity);
		if (scene.IsValid(b.Entity))
			scene.DestroyEntity(b.Entity);
		m_Free.push_back(b.Body);
	}

	void Sweep(GS::Scene* scene)
	{
		for (Ball& b : m_Balls)
		{
			StopVoice(b.Whistle);
			if (m_Session && scene)
			{
				m_Session->BodyHost().RetireBody(b.Entity);
				if (scene->IsValid(b.Entity))
					scene->DestroyEntity(b.Entity);
				m_Free.push_back(b.Body);
			}
		}
		m_Balls.clear();
	}

public:
	// The session the balls are made in, for Reset and Clear to sweep with.
	void Attach(GameSession* session) { m_Session = session; m_Free.clear(); }

private:
	static void StopVoice(GS::VoiceHandle& voice)
	{
		if (voice != GS::InvalidVoice)
			GS::AudioEngine::Stop(voice);
		voice = GS::InvalidVoice;
	}

	void Silence()
	{
		for (Cannon& c : m_Cannons)
			StopVoice(c.ChargeVoice);
		for (Ball& b : m_Balls)
			StopVoice(b.Whistle);
	}

	static void PlayOnce(const std::shared_ptr<GS::AudioClip>& clip, const glm::vec3& at, float volume, float pitch = 1.0f)
	{
		if (!GS::AudioEngine::IsAvailable())
			return;
		GS::Audio3DParams p;
		p.Position = at;
		p.Volume = volume;
		p.Pitch = pitch;
		p.MinDistance = 3.0f;
		p.MaxDistance = 60.0f;
		p.DopplerFactor = 0.0f;
		GS::AudioEngine::PlayAt(clip, p);
	}

	std::vector<Cannon> m_Cannons;
	std::vector<Ball> m_Balls;
	std::vector<Puff> m_Puffs;
	std::vector<Zone> m_Zones;
	std::vector<GS::PhysicsWorld3D::BodyHandle> m_Free;   // retired ball bodies, reused
	GameSession* m_Session = nullptr;
	glm::vec3 m_Runner{ 0.0f }, m_RunnerVelocity{ 0.0f };
	float m_Clock = 0.0f;
	float m_SinceDown = 0.0f;
	int m_Hits = 0, m_Shots = 0, m_Held = 0;
	int m_Blocks = 0, m_Parries = 0, m_Returns = 0, m_Wrecked = 0, m_Downed = 0;
	std::vector<Bolt> m_Bolts;
	uint32_t m_PuffCount = 0;          // the puffs' hash stream: the same smoke every replay
	static constexpr float kReturnSpeed = 24.0f;   // m/s, a ball struck back
	// The rules of fair, in seconds.
	float m_Spacing = 1.2f;          // between two shots' arrivals
	float m_MinFlight = 0.55f;       // no shot quicker than this from the muzzle -- the charge is the warning
	float m_GraceAfterDown = 2.0f;   // on his feet this long after a fall before anything charges
};
