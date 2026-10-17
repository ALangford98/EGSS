#pragma once

// Players and NPCs (PlayerComponent, NpcComponent) while a game runs --
// editor Play and GSPlayer alike, owned by GameSession. See
// docs/superpowers/specs/2026-10-14-tools-characters-terrain-design.md.
//
// Each character is an upright capsule in the physics world: its rotational
// inertia removed so contacts can never tip it over, kept awake, and its
// horizontal velocity *set* every step from what it wants to do rather than
// pushed by forces -- a character stops when its key is released, not after
// sliding to a halt. Gravity, jumps, slopes and bumping into boxes are
// left to the physics.
//
// Everything runs per fixed step from polled input, including the mouse
// (as a difference between steps), so a recorded session replays exactly.

#include <GS.h>
#include <GS/Procedural/Noise.h>

#include "Humanoid.h"
#include "Mannequin.h"

#include <memory>

#include <cmath>
#include <unordered_map>
#include <vector>

class CharacterController
{
public:
	// Default capsule mass: a person, not a crate -- a character walking
	// into a 1 kg box should move it.
	static constexpr float s_DefaultMass = 70.0f;

	// The capsule a character's transform describes: the capsule mesh is 1
	// wide and 2 tall, so its radius is half the wider of x and z scale and
	// its half-height (without the caps) whatever of y's remains.
	static void CapsuleFor(const GS::TransformComponent& transform, float& radius, float& halfHeight)
	{
		radius = 0.5f * std::max(transform.Scale.x, transform.Scale.z);
		halfHeight = std::max(0.0f, transform.Scale.y - radius);
	}

	enum BodyKind { Upright = 0, Ball = 1, HumanoidBody = 2 };

	static int BodyOf(GS::Scene& scene, GS::EntityId entity)
	{
		if (auto* player = scene.GetComponent<GS::PlayerComponent>(entity))
			return player->Body;
		if (auto* npc = scene.GetComponent<GS::NpcComponent>(entity))
			return npc->Body;
		return Upright;
	}

	// A ball that rolls: a real sphere, its inertia kept, gripping the
	// ground so that turning it moves it. Radius from the largest scale --
	// the sphere mesh is 1 across.
	static GS::RigidBody3D MakeBall(const GS::TransformComponent& transform, const GS::PhysicsComponent* physics)
	{
		float radius = 0.5f * std::max(transform.Scale.x, std::max(transform.Scale.y, transform.Scale.z));
		GS::RigidBody3D body = GS::RigidBody3D::MakeSphere(transform.Position, radius, physics ? physics->Mass : 10.0f);
		body.Friction = 0.9f;
		body.Restitution = physics ? physics->Restitution : 0.1f;
		body.LinearDamping = 0.01f;
		body.AngularDamping = 0.02f;
		body.Orientation = glm::quat_cast(glm::mat3(
			glm::rotate(glm::mat4(1.0f), glm::radians(transform.Rotation.x), glm::vec3(1, 0, 0)) *
			glm::rotate(glm::mat4(1.0f), glm::radians(transform.Rotation.y), glm::vec3(0, 1, 0)) *
			glm::rotate(glm::mat4(1.0f), glm::radians(transform.Rotation.z), glm::vec3(0, 0, 1))));
		return body;
	}

	static GS::RigidBody3D MakeBody(const GS::TransformComponent& transform, const GS::PhysicsComponent* physics, int kind)
	{
		if (kind == Ball)
			return MakeBall(transform, physics);
		return MakeUpright(transform, physics);
	}

	static GS::RigidBody3D MakeUpright(const GS::TransformComponent& transform, const GS::PhysicsComponent* physics)
	{
		float radius, halfHeight;
		CapsuleFor(transform, radius, halfHeight);
		GS::RigidBody3D body = GS::RigidBody3D::MakeCapsule(transform.Position, radius, halfHeight,
			physics ? physics->Mass : s_DefaultMass);
		// Upright for good: no inverse inertia, so no contact can turn it.
		body.InverseInertiaLocal = glm::mat3(0.0f);
		body.InverseInertiaWorld = glm::mat3(0.0f);
		// Its velocity is set, not slid to: friction would only fight that.
		body.Friction = 0.0f;
		body.Restitution = 0.0f;
		body.LinearDamping = 0.0f;
		return body;
	}

	// Where the camera looks for this controller's pitch and yaw: a head's
	// (SceneRenderer's LookDirection), written to the camera as the
	// rotation that looks that way. It used to *be* the rotation (pitch,
	// yaw, 0), which the renderer reads as a tip about the world's x after
	// the turn: behind a figure facing +z the camera looked up when the
	// mouse asked it to look down, and facing along x it could not look up
	// or down at all.
	static glm::vec3 Forward(float pitchDegrees, float yawDegrees)
	{
		return LookDirection(pitchDegrees, yawDegrees);
	}

	// Yaw (degrees) whose forward, level, is `direction` -- the inverse of
	// Forward at pitch 0.
	static float YawOf(const glm::vec3& direction)
	{
		return glm::degrees(std::atan2(-direction.x, -direction.z));
	}

	// A humanoid: the active-ragdoll rig built standing at the entity's
	// position (the ground under its feet), facing its yaw. Its pelvis is
	// what the rest of the controller follows -- the camera, an NPC
	// chasing the player.
	void AddHumanoid(GS::Scene& scene, GS::EntityId entity, GS::PhysicsWorld3D& world)
	{
		auto* transform = scene.GetComponent<GS::TransformComponent>(entity);
		if (!transform)
			return;
		auto rig = std::make_unique<Humanoid>(world);
		// Its body and its walk, from whichever component made it a humanoid.
		if (auto* npc = scene.GetComponent<GS::NpcComponent>(entity))
		{
			rig->SetPhysique(npc->Physique);
			rig->SetGait(npc->Gait);
		}
		else if (auto* player = scene.GetComponent<GS::PlayerComponent>(entity))
		{
			rig->SetPhysique(player->Physique);
			rig->SetGait(player->Gait);
		}
		// Seeded by the entity, so every humanoid in a scene walks a little
		// differently, and the same one the same way every time it is played.
		rig->Spawn(transform->Position, glm::radians(transform->Rotation.y), (uint32_t)entity * 2654435761u + 1u);
		Add(scene, entity, rig->PelvisBody());
		Character& c = m_Characters.back();
		c.Kind = HumanoidBody;
		// The controller's yaw looks along (-sin, -cos); the rig faces
		// (sin, cos) at its yaw -- half a turn apart, so the camera starts
		// behind it.
		c.Facing = transform->Rotation.y + 180.0f;
		c.StartYaw = glm::radians(transform->Rotation.y);
		c.Rig = std::move(rig);
	}

	bool HasHumanoids() const
	{
		for (const Character& c : m_Characters)
			if (c.Rig)
				return true;
		return false;
	}

	// Each running humanoid's bodies, as draw items.
	void AppendDraws(GS::Scene& scene, const GS::PhysicsWorld3D& world, std::vector<ExtraDraw>& out) const
	{
		for (const Character& c : m_Characters)
			if (c.Rig && scene.IsValid(c.Entity))
			{
				auto* mesh = scene.GetComponent<GS::MeshComponent>(c.Entity);
				if (mesh && !mesh->Visible)
					continue;
				Mannequin::AppendLive(*c.Rig, world, Mannequin::ColourOf(scene, c.Entity), c.Entity, out,
					Mannequin::LookOf(scene, c.Entity));
			}
	}

	// The rig an entity runs as, or null.
	Humanoid* RigOf(GS::EntityId entity)
	{
		for (Character& c : m_Characters)
			if (c.Entity == entity && c.Rig)
				return c.Rig.get();
		return nullptr;
	}

	bool IsRunning(GS::EntityId entity) const
	{
		for (const Character& c : m_Characters)
			if (c.Entity == entity && c.Rig)
				return true;
		return false;
	}

	// Every humanoid on its feet again, where and as it started.
	int ResetHumanoids()
	{
		int count = 0;
		for (Character& c : m_Characters)
			if (c.Rig)
			{
				c.Rig->Respawn(c.Home, c.StartYaw);
				count++;
			}
		return count;
	}

	// Every fallen humanoid getting up where it lies.
	int GetUpHumanoids()
	{
		int count = 0;
		for (Character& c : m_Characters)
			if (c.Rig && c.Rig->IsDown())
			{
				c.Rig->GetUp();
				count++;
			}
		return count;
	}

	void Clear()
	{
		m_Characters.clear();

		m_Player = -1;
		m_Camera = GS::InvalidEntity;
		m_CreatedCamera = false;
		m_Possessing = false;
		m_HeldPlayer = -1;
		m_HeldCamera = GS::InvalidEntity;
	}

	// One character, its body already in the world.
	void Add(GS::Scene& scene, GS::EntityId entity, GS::PhysicsWorld3D::BodyHandle handle)
	{
		Character c;
		c.Entity = entity;
		c.Handle = handle;
		auto* transform = scene.GetComponent<GS::TransformComponent>(entity);
		c.Home = transform ? transform->Position : glm::vec3(0.0f);
		c.Facing = transform ? transform->Rotation.y : 0.0f;
		c.IsPlayer = scene.GetComponent<GS::PlayerComponent>(entity) != nullptr;
		c.Kind = BodyOf(scene, entity);
		c.Index = (uint32_t)m_Characters.size();
		m_Characters.push_back(c);
		if (c.IsPlayer && m_Player < 0)
			m_Player = (int)m_Characters.size() - 1;
	}

	// The camera the first player drives, when its mode asks for one: the
	// scene's active camera, or a new one made for it. Made here, while the
	// game starts -- the editor's Play restores the scene afterwards.
	void Start(GS::Scene& scene)
	{
		if (m_Player < 0)
			return;
		Character& player = m_Characters[m_Player];
		auto* settings = scene.GetComponent<GS::PlayerComponent>(player.Entity);
		if (!settings || settings->CameraMode == 2)
			return;

		auto& cameras = scene.View<GS::CameraComponent>();
		for (size_t i = 0; i < cameras.Size(); i++)
			if (cameras.Components()[i].Active)
			{
				m_Camera = cameras.Owner(i);
				break;
			}
		if (m_Camera == GS::InvalidEntity)
		{
			GS::Entity camera = scene.CreateEntity("Player Camera");
			GS::CameraComponent component;
			component.Active = true;   // default-constructed cameras are not, and the renderer looks only through an active one
			camera.Add<GS::CameraComponent>(component);
			m_Camera = camera.GetId();
			m_CreatedCamera = true;
		}
		// Behind the player, looking the way it faces, a little down.
		m_Yaw = player.Facing;
		m_Pitch = -15.0f;
	}

	// Before the physics step: what each character wants to do, as a
	// velocity.
	void BeforeStep(GS::Scene& scene, GS::PhysicsWorld3D& world, float dt)
	{
		m_Step++;
		glm::vec3 playerPosition(0.0f);
		bool havePlayer = m_Player >= 0 && scene.IsValid(m_Characters[m_Player].Entity);
		if (havePlayer)
			playerPosition = world.GetBody(m_Characters[m_Player].Handle).Position;

		ReadInput(dt);

		for (Character& c : m_Characters)
		{
			if (!scene.IsValid(c.Entity))
				continue;
			GS::RigidBody3D& body = world.GetBody(c.Handle);
			body.Awake = true;
			body.SleepTimer = 0.0f;

			float feet = body.Position.y - (c.Kind == Ball ? body.Radius : body.HalfHeight + body.Radius);
			float groundHeight = 0.0f;
			glm::vec3 groundNormal;
			c.Grounded = world.GroundBelow(body.Position, groundHeight, groundNormal, c.Handle, -1e9f)
				&& feet - groundHeight < 0.08f && body.Velocity.y <= 0.5f;

			glm::vec3 wish(0.0f);
			float speed = 0.0f;
			bool jump = false;

			if (c.Rig)
			{
				DriveHumanoid(scene, world, c, havePlayer, playerPosition, dt);
				continue;
			}

			if (IsControlled(c))
			{
				const GS::PlayerComponent* settings = SettingsOf(scene, c);
				// Relative to where the camera looks, flattened.
				glm::vec3 forward(-std::sin(glm::radians(m_Yaw)), 0.0f, -std::cos(glm::radians(m_Yaw)));
				glm::vec3 right(std::cos(glm::radians(m_Yaw)), 0.0f, -std::sin(glm::radians(m_Yaw)));
				wish = forward * m_MoveInput.y + right * m_MoveInput.x;
				speed = settings->Speed * (m_Sprint ? settings->SprintMultiplier : 1.0f);
				jump = m_JumpPressed;
				if (jump && c.Grounded)
				{
					// v^2 = 2 g h: the speed that rises exactly JumpHeight.
					body.Velocity.y = std::sqrt(2.0f * std::abs(world.Gravity.y) * std::max(0.0f, settings->JumpHeight));
				}
			}
			else if (auto* npc = scene.GetComponent<GS::NpcComponent>(c.Entity))
			{
				speed = npc->Speed;
				wish = NpcWish(c, *npc, body.Position, havePlayer, playerPosition, dt);
			}

			float length = glm::length(wish);
			glm::vec3 direction = length > 1e-4f ? wish / std::max(1.0f, length) : glm::vec3(0.0f);
			if (c.Kind == Ball)
			{
				DriveBall(body, c.Grounded, direction, speed, std::abs(world.Gravity.y));
				continue;
			}
			glm::vec3 velocity = direction * speed;
			body.Velocity.x = velocity.x;
			body.Velocity.z = velocity.z;
			if (length > 1e-4f)
				c.Facing = YawOf(wish);
		}
	}

	// After the step, once GameSession has copied bodies to transforms:
	// characters face where they walk (their bodies never turn), and the
	// player's camera follows.
	void AfterStep(GS::Scene& scene, GS::PhysicsWorld3D& world)
	{
		// A ball keeps the orientation physics gave it -- that is the roll.
		// A humanoid's entity stands where its feet are (its position is the
		// ground under them, as when it was placed), facing as the rig does.
		for (Character& c : m_Characters)
		{
			if (!scene.IsValid(c.Entity))
				continue;
			auto* transform = scene.GetComponent<GS::TransformComponent>(c.Entity);
			if (!transform)
				continue;
			if (c.Rig)
			{
				const GS::RigidBody3D& pelvis = world.GetBody(c.Rig->PelvisBody());
				transform->Position = pelvis.Position - glm::vec3(0.0f, c.Rig->RestPelvisHeight(), 0.0f);
				transform->Rotation = glm::vec3(0.0f, glm::degrees(c.Rig->FacingRadians()), 0.0f);
			}
			else if (c.Kind != Ball)
				transform->Rotation = glm::vec3(0.0f, c.Facing, 0.0f);
		}

		if (m_Player < 0 || m_Camera == GS::InvalidEntity || !scene.IsValid(m_Camera))
			return;
		Character& player = m_Characters[m_Player];
		const GS::PlayerComponent* settings = SettingsOf(scene, player);
		auto* camera = scene.GetComponent<GS::TransformComponent>(m_Camera);
		if (!settings || !camera || !scene.IsValid(player.Entity) || settings->CameraMode == 2)
			return;

		const GS::RigidBody3D& body = world.GetBody(player.Handle);
		// A capsule's position is its centre, 0.9 above its feet; a
		// humanoid's handle is its pelvis. The camera aims at the same
		// height over the feet either way.
		glm::vec3 centre = player.Rig ? body.Position + glm::vec3(0.0f, 0.9f - player.Rig->RestPelvisHeight(), 0.0f) : body.Position;
		glm::vec3 target = centre + glm::vec3(0.0f, settings->CameraHeight, 0.0f);
		if (settings->CameraMode == 1)
		{
			// First person: at the top of the capsule, looking out.
			camera->Position = player.Rig ? world.GetBody(player.Rig->HeadBody()).Position
				: body.Position + glm::vec3(0.0f, body.HalfHeight + body.Radius * 0.6f, 0.0f);
		}
		else
		{
			// Pulled in when something is between the character and where
			// the camera would be -- a wall, the crates behind the dummy's
			// mark -- so it is never looking out from inside a box.
			glm::vec3 back = -Forward(m_Pitch, m_Yaw);
			float distance = settings->CameraDistance;
			std::vector<GS::PhysicsWorld3D::BodyHandle> own = player.Rig ? player.Rig->Bodies()
				: std::vector<GS::PhysicsWorld3D::BodyHandle>{ player.Handle };
			// Five rays, the line and four 0.25 m off it: the view is a
			// cone, not a line, and a crate the centre ray clears by 5 cm
			// still fills the picture.
			glm::vec3 side = glm::normalize(glm::cross(back, glm::vec3(0.0f, 1.0f, 0.0f)));
			glm::vec3 lift = glm::cross(side, back);
			for (glm::vec3 offset : { glm::vec3(0.0f), side * 0.25f, -side * 0.25f, lift * 0.25f, -lift * 0.25f })
			{
				GS::PhysicsWorld3D::RayHit hit = world.Raycast(target + offset, back, distance + 0.3f, own);
				if (hit.Hit)
					distance = std::max(0.4f, hit.Distance - 0.3f);
			}
			camera->Position = target + back * distance;
		}
		camera->Rotation = RotationLookingAlong(Forward(m_Pitch, m_Yaw));
	}

	GS::EntityId CameraEntity() const { return m_Camera; }
	bool CreatedCamera() const { return m_CreatedCamera; }

	// Taking control of a character the game did not start with one of --
	// walking up to the crash-test dummy and becoming him. Input drives it
	// as it would a player, and `camera` follows it from behind; Release
	// hands it back to its own behaviour and gives the camera back
	// unmoved. False for an entity that is not a character here.
	bool Possess(GS::Scene& scene, GS::EntityId entity, GS::EntityId camera, const GS::PhysicsWorld3D& world)
	{
		for (size_t i = 0; i < m_Characters.size(); i++)
			if (m_Characters[i].Entity == entity && scene.IsValid(entity))
			{
				if (!m_Possessing)
				{
					m_HeldPlayer = m_Player;
					m_HeldCamera = m_Camera;
				}
				m_Possessing = true;
				m_Player = (int)i;
				m_Camera = camera;
				// The camera keeps looking the way it was -- walking up to him
				// from the front and pressing E does not swing the view round
				// -- and W walks him away from it, as a player is walked.
				// The pitch and yaw are of where it looks (see Forward).
				Character& c = m_Characters[i];
				if (auto* view = scene.GetComponent<GS::TransformComponent>(camera))
				{
					glm::vec2 look = PitchYawOf(ForwardFromRotation(view->Rotation));
					m_Yaw = look.y;
					m_Pitch = glm::clamp(look.x, -60.0f, 20.0f);
				}
				else
				{
					// Behind him: the rig's facing is an entity yaw (it faces
					// (sin f, cos f)) and the camera looks along (-sin y, -cos y),
					// half a turn apart, as AddHumanoid says.
					m_Yaw = c.Rig ? glm::degrees(c.Rig->FacingRadians()) + 180.0f : c.Facing;
					m_Pitch = -12.0f;
				}
				m_MouseKnown = false;
				(void)world;
				return true;
			}
		return false;
	}

	void Release()
	{
		if (!m_Possessing)
			return;
		m_Possessing = false;
		m_Player = m_HeldPlayer;
		m_Camera = m_HeldCamera;
	}

	bool IsPossessing() const { return m_Possessing; }
	GS::EntityId Controlled() const
	{
		return m_Player >= 0 ? m_Characters[(size_t)m_Player].Entity : GS::InvalidEntity;
	}

	// Where the controlled character is -- its centre, about chest height
	// for a humanoid -- for asking what it is standing near.
	bool ControlledPosition(const GS::PhysicsWorld3D& world, glm::vec3& out) const
	{
		if (m_Player < 0)
			return false;
		const Character& c = m_Characters[(size_t)m_Player];
		out = world.GetBody(c.Handle).Position;
		return true;
	}

	// The rig being controlled, if it is a humanoid: for the HUD's hint of
	// what a jump would do there (vault, climb, step on).
	const Humanoid* ControlledRig() const
	{
		if (m_Player < 0)
			return nullptr;
		return m_Characters[(size_t)m_Player].Rig.get();
	}

	// The controller's yaw (degrees), for facing the camera's way.
	float Yaw() const { return m_Yaw; }

	// Off while the keyboard belongs to something else in the game -- the
	// terminal, the cannon's controls.
	void SetInputEnabled(bool enabled) { m_InputEnabled = enabled; }

private:
	struct Character
	{
		GS::EntityId Entity = GS::InvalidEntity;
		GS::PhysicsWorld3D::BodyHandle Handle = 0;
		uint32_t Index = 0;
		bool IsPlayer = false;
		int Kind = Upright;
		bool Grounded = false;
		float Facing = 0.0f;           // degrees about y
		glm::vec3 Home{ 0.0f };        // where it started: the centre of its wander
		glm::vec3 Target{ 0.0f };
		bool HasTarget = false;
		float Wait = 0.0f;             // seconds to stand before the next wander
		float Walking = 0.0f;          // seconds spent on the current target
		uint32_t Picks = 0;            // wander targets chosen so far: the random stream's position
		std::shared_ptr<Humanoid> Rig; // Body = Humanoid: the active ragdoll it is
		float StartYaw = 0.0f;         // radians, the rig's own convention: what Respawn takes
	};

	bool IsControlled(const Character& c) const
	{
		return m_Player >= 0 && &c == &m_Characters[(size_t)m_Player];
	}

	// The controlled character's settings: its own PlayerComponent, or, for
	// one taken over, a closer camera that suits standing behind a person.
	const GS::PlayerComponent* SettingsOf(GS::Scene& scene, const Character& c) const
	{
		if (auto* settings = scene.GetComponent<GS::PlayerComponent>(c.Entity))
			return settings;
		static const GS::PlayerComponent possessed = []()
		{
			GS::PlayerComponent p;
			p.Speed = 4.0f;
			p.CameraDistance = 3.4f;
			p.CameraHeight = 0.55f;
			return p;
		}();
		return &possessed;
	}

	// A humanoid is told what to do, and does it with its own legs: the
	// same wish as any character, turned into the rig's controls -- a
	// direction (the rig's yaw is (cos, sin) where ours is (-sin, -cos))
	// and whether to go, run or jump. It falls when pushed past what it can
	// catch; lying still on ground for two seconds, it gets up.
	void DriveHumanoid(GS::Scene& scene, GS::PhysicsWorld3D& world, Character& c, bool havePlayer,
		const glm::vec3& playerPosition, float dt)
	{
		Humanoid& rig = *c.Rig;
		Humanoid::Control& control = rig.GetControl();
		control = Humanoid::Control{};
		const GS::RigidBody3D& pelvis = world.GetBody(rig.PelvisBody());

		if (IsControlled(c))
		{
			float y = glm::radians(m_Yaw);
			control.Yaw = glm::degrees(std::atan2(-std::cos(y), -std::sin(y)));
			control.Forward = m_MoveInput.y > 0.0f;
			control.Back = m_MoveInput.y < 0.0f;
			control.Right = m_MoveInput.x > 0.0f;
			control.Left = m_MoveInput.x < 0.0f;
			control.Run = m_Sprint;
			if (m_JumpPressed)
				rig.RequestJump();
		}
		else if (auto* npc = scene.GetComponent<GS::NpcComponent>(c.Entity))
		{
			glm::vec3 wish = NpcWish(c, *npc, pelvis.Position, havePlayer, playerPosition, dt);
			if (glm::length(wish) > 1e-4f)
			{
				control.Yaw = glm::degrees(std::atan2(wish.z, wish.x));
				control.Forward = true;
				control.Run = npc->Speed >= 3.0f;
			}
		}

		if (rig.IsDown() && rig.TimeDown() > 2.0f && glm::length(pelvis.Velocity) < 0.3f)
		{
			float ground = 0.0f;
			glm::vec3 normal;
			if (world.GroundBelow(pelvis.Position, ground, normal, rig.PelvisBody(), -1e9f)
				&& pelvis.Position.y - ground < 0.6f)
				rig.GetUp();
		}

		rig.Update(dt);
	}

	// A ball is driven the way a ball is: by turning it. Rolling without
	// slipping, a ball moving at v in direction d spins at v / r about
	// up x d (v = w x (r up), and (up x d) x up = d), so the wished
	// velocity becomes a wished spin, and torque pulls the spin towards it
	// -- the ground's grip turns that into motion, slope and momentum
	// included. Only the horizontal spin is driven: spin about up does not
	// move a ball. Released, it is left alone: it rolls on.
	//
	// The torque is capped at what grip can carry, mu m g r: more than that
	// would only spin the ball in place. In the air a small force steers.
	void DriveBall(GS::RigidBody3D& body, bool grounded, const glm::vec3& direction, float speed, float gravity)
	{
		if (glm::length(direction) < 1e-4f)
			return;
		float mass = body.InverseMass > 0.0f ? 1.0f / body.InverseMass : 0.0f;
		float r = body.Radius;
		if (!grounded)
		{
			body.Force += direction * mass * 4.0f;
			return;
		}
		glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 wanted = glm::cross(up, direction) * (speed / r);
		glm::vec3 spin = body.AngularVelocity;
		glm::vec3 error = wanted - spin;
		error.y = 0.0f;
		float inertia = 0.4f * mass * r * r;
		glm::vec3 torque = error * inertia * 10.0f;
		float cap = body.Friction * mass * gravity * r;
		float magnitude = glm::length(torque);
		if (magnitude > cap)
			torque *= cap / magnitude;
		body.Torque += torque;
	}

	// The keyboard and mouse, as this step sees them. Skipped while an
	// editor text field has the keyboard, as scripts' input is.
	void ReadInput(float dt)
	{
		m_MoveInput = glm::vec2(0.0f);
		m_Sprint = false;
		bool jumpDown = false;

		if (!GS::ImGuiLayer::WantsKeyboard() && m_InputEnabled)
		{
			// By action, not key: the project's Controls (GS::InputMap).
			const GS::InputMap& map = GS::InputMap::Active();
			if (map.Down("forward")) m_MoveInput.y += 1.0f;
			if (map.Down("back")) m_MoveInput.y -= 1.0f;
			if (map.Down("right")) m_MoveInput.x += 1.0f;
			if (map.Down("left")) m_MoveInput.x -= 1.0f;
			m_Sprint = map.Down("sprint");
			jumpDown = map.Down("jump");

			// The turn keys turn the camera, 90 degrees a second.
			const float turn = 90.0f * dt;
			if (map.Down("turnleft")) m_Yaw += turn;
			if (map.Down("turnright")) m_Yaw -= turn;
			if (map.Down("turnup")) m_Pitch += turn;
			if (map.Down("turndown")) m_Pitch -= turn;
		}
		// A jump on the press, not while held: holding Space does not bounce.
		m_JumpPressed = jumpDown && !m_JumpWasDown;
		m_JumpWasDown = jumpDown;

		// Right-drag orbits. The difference between this step's pointer and
		// the last one's, so it is the same however the frames fell.
		glm::vec2 mouse(GS::Input::GetMouseX(), GS::Input::GetMouseY());
		if (GS::InputMap::Active().Down("look") && m_MouseKnown && m_InputEnabled)
		{
			glm::vec2 delta = mouse - m_LastMouse;
			m_Yaw -= delta.x * 0.25f;
			m_Pitch -= delta.y * 0.25f;
		}
		m_LastMouse = mouse;
		m_MouseKnown = true;
		m_Pitch = glm::clamp(m_Pitch, -75.0f, 40.0f);
	}

	// A number in [0, 1) from this character's own stream: its index and how
	// many targets it has picked. Deterministic, so a replay walks the same
	// way; per character, so adding one does not change where another goes.
	float Random(Character& c, uint32_t salt) const
	{
		return GS::Noise::HashToUnit(GS::Noise::Hash((int)c.Index, (int)(c.Picks * 4u + salt), 0x4e5043u));
	}

	glm::vec3 NpcWish(Character& c, const GS::NpcComponent& npc, const glm::vec3& position,
		bool havePlayer, const glm::vec3& playerPosition, float dt)
	{
		glm::vec3 toPlayer = playerPosition - position;
		toPlayer.y = 0.0f;
		float playerDistance = glm::length(toPlayer);

		switch (npc.Behaviour)
		{
			case 2:   // follow
				if (havePlayer && playerDistance < npc.DetectRadius && playerDistance > npc.StopDistance)
					return toPlayer / playerDistance;
				return glm::vec3(0.0f);
			case 3:   // flee
				if (havePlayer && playerDistance < npc.DetectRadius && playerDistance > 1e-4f)
					return -toPlayer / playerDistance;
				return glm::vec3(0.0f);
			case 1:   // wander
			{
				if (c.Wait > 0.0f)
				{
					c.Wait -= dt;
					return glm::vec3(0.0f);
				}
				if (!c.HasTarget)
				{
					// Uniform over the disc: sqrt of the radius fraction.
					float angle = Random(c, 0) * glm::two_pi<float>();
					float distance = std::sqrt(Random(c, 1)) * npc.WanderRadius;
					c.Target = c.Home + glm::vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
					c.HasTarget = true;
					c.Walking = 0.0f;
				}
				glm::vec3 toTarget = c.Target - position;
				toTarget.y = 0.0f;
				float distance = glm::length(toTarget);
				c.Walking += dt;
				// Arrived -- or blocked long enough that it never will.
				if (distance < 0.3f || c.Walking > 2.0f * npc.WanderRadius / std::max(0.1f, npc.Speed) + 2.0f)
				{
					c.HasTarget = false;
					c.Wait = 1.0f + 2.0f * Random(c, 2);
					c.Picks++;
					return glm::vec3(0.0f);
				}
				return toTarget / distance;
			}
			default:  // idle
				return glm::vec3(0.0f);
		}
	}

	std::vector<Character> m_Characters;
	int m_Player = -1;                 // the first PlayerComponent: the one input drives and the camera follows
	GS::EntityId m_Camera = GS::InvalidEntity;
	bool m_CreatedCamera = false;
	float m_Yaw = 0.0f, m_Pitch = -15.0f;
	uint64_t m_Step = 0;

	glm::vec2 m_MoveInput{ 0.0f };
	bool m_Sprint = false;
	bool m_JumpPressed = false, m_JumpWasDown = false;
	glm::vec2 m_LastMouse{ 0.0f };
	bool m_MouseKnown = false;
	bool m_InputEnabled = true;

	bool m_Possessing = false;
	int m_HeldPlayer = -1;             // who m_Player was before a possession
	GS::EntityId m_HeldCamera = GS::InvalidEntity;
};
