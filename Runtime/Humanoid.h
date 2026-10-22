#pragma once

// The active-ragdoll humanoid: a jointed body that stands, balances, walks,
// runs, jumps, staggers when hit, goes limp when it can no longer catch
// itself, and gets back up. Lifted out of TestEnv/src/Ragdoll.h, where it was
// built and tuned, so that a game's Player or NPC can be one (GameSession,
// Body = Humanoid) -- in editor Play and in every exported game. The Ragdoll
// demo, and Map Building which derives from it, drive this same class.
// docs/superpowers/specs/2026-10-14-play-keep-ball-humanoid-design.md.
//
// The commentary throughout is the rig's own, from the demo: what was tried,
// what was measured, and what is still open (the reactive step does not yet
// widen the base -- see the changelog for 2026-08-09/10).
//
// The physics world is not this class's: it builds its thirteen bodies and
// twelve joints into whichever world it is given, and steps nothing --
// whoever owns the world calls Update() before each world step. What it
// wants to do comes in through m_Control (which way is forward, the movement
// keys, run) and m_WantsJump, set by the owner each step; nothing here reads
// the keyboard or knows about a camera, so a game can drive it with a
// player's input or an NPC's wish alike.
//
// Moved verbatim apart from those inputs, and checked: a recorded session
// through the demo produces bit-identical body states, step by step, before
// and after the move.

#include <GS.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <functional>
#include <cmath>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

// The controller that drives him while he is someone's to move, posed from
// recordings (Runtime/Locomotion.h, included at the end of this file; its
// state lives there so this file does not grow by another thousand lines).
struct LocoClips;
struct LocoState;

// The weapons' movesets (Runtime/Armament.h).
struct ArmamentKit;
struct ArmamentStrike;

class Humanoid
{
public:
	struct BalanceState
	{
		glm::vec3 Com{ 0.0f };
		glm::vec3 ComVelocity{ 0.0f };
		glm::vec2 ComGround{ 0.0f };
		glm::vec2 CapturePoint{ 0.0f };
		glm::vec2 SupportCentre{ 0.0f };

		// The hull, in world xz, counter-clockwise.
		std::vector<glm::vec2> Support;

		// How far inside the polygon the capture point is, in metres.
		// Negative means outside, and outside means falling.
		float Margin = 0.0f;
		bool Standing = false;
	};

	// What the owner wants this step. Yaw is the direction "forward" is
	// measured from, in degrees, as the demo's camera yaw: forward is
	// (cos yaw, 0, sin yaw), right (-sin yaw, 0, cos yaw).
	struct Control
	{
		float Yaw = -90.0f;
		bool Enabled = true;
		bool Forward = false, Back = false, Left = false, Right = false;
		bool Run = false;
		// Weapons (Runtime/Armament.h): the guard up while held,
		// and the way he guards and swings -- degrees, as Yaw is.
		bool Guard = false;
		float AimYaw = -90.0f;
		// What the player is aiming at -- under the crosshair, the camera's
		// ray into the world -- for what is aimed by hand (a staff's bolts).
		glm::vec3 AimPoint{ 0.0f };
		bool HasAimPoint = false;
	};

	explicit Humanoid(GS::PhysicsWorld3D& world) : m_World(world) {}
	virtual ~Humanoid() = default;
	Humanoid(const Humanoid&) = delete;
	Humanoid& operator=(const Humanoid&) = delete;

	Control& GetControl() { return m_Control; }
	void RequestJump() { m_WantsJump = true; }

	// --- Weapons (Runtime/Armament.h) --------------------------------------
	//
	// A loadout -- sword and shield, longsword, katana, greatsword, twin
	// daggers, a staff, or his fists -- each with its own moveset: a combo
	// for presses of attack, a lunge from the guard, an attack from a run, and
	// a guard of its own (a shield, a blade, crossed daggers, a barrier, his
	// fists up). Holding the guard he faces his aim and walks under it --
	// sideways, backpedalling, never running. What reaches him is asked of
	// WardOff by whoever threw it: met by the guard it is blocked (he is
	// shoved back by however much that guard lets through), met within the
	// loadout's parry window of raising it it is parried (glanced away, or for
	// some struck back), met by a strike it is struck back. A staff casts
	// (TakeCasts: bolts, a nova) for the game to make real. Sheathed he can do
	// parkour; drawn he cannot (ParkourOn) -- his fists need no sheathing.
	enum class Loadout { SwordShield, Longsword, Katana, Greatsword, Daggers, Staff, Unarmed, Count };
	void SetArmed(bool armed);
	bool Armed() const { return m_Armed; }
	void SetLoadout(Loadout loadout);
	Loadout GetLoadout() const { return m_Loadout; }
	// Aimed by hand rather than by facing: a staff's bolts go at
	// Control::AimPoint, and he faces the camera's way, not what is coming.
	bool AimsByHand() const;
	static const char* LoadoutName(Loadout loadout);
	void RequestAttack() { if (m_Armed) m_WantsAttack = true; }
	void RequestSheathe() { if (m_Armed) m_WantsSheathe = true; }
	// In his hands (not on the way to or from where they are carried).
	bool Drawn() const { return m_Armed && m_Drawn && m_Stow < 0.0f; }
	enum class Ward { None, Blocked, Parried, Struck };
	struct WardResult
	{
		Ward Kind = Ward::None;
		glm::vec3 Velocity{ 0.0f };   // what the thing should go on at; for Struck, the thrower aims it
		glm::vec3 Point{ 0.0f };      // where it was met
	};
	WardResult WardOff(const glm::vec3& at, const glm::vec3& velocity, float radius, float mass, float dt);
	// The cutting part of a strike, which strike it is (to hit a thing once
	// per strike), what it does to what it hits, and where its edges are:
	// returns how many (two for both daggers at once).
	bool SwordCutting() const;
	uint32_t SwingId() const { return m_Swings; }
	float StrikeDamage() const;
	const char* StrikeName() const;
	int StrikeEdges(glm::vec3 hilt[2], glm::vec3 tip[2]) const;
	float GuardAmount() const { return m_Guard; }
	// What a staff has cast since last asked: a bolt from `At` along
	// `Direction`, or a nova about `At`.
	struct Cast { bool Nova = false; glm::vec3 At{ 0.0f }, Direction{ 0.0f }; float Damage = 0.0f; };
	void TakeCasts(std::vector<Cast>& out) { out.insert(out.end(), m_Casts.begin(), m_Casts.end()); m_Casts.clear(); }
	// Where to draw what he carries: each hand's weapon (+y up the blade, or
	// out of a shield's face), from his forearms or where it is put away --
	// held, striking or limp; the scabbards and sheaths; a staff's barrier.
	glm::mat4 PropFrame(int hand) const;
	int HolderCount() const;
	glm::mat4 HolderFrame(int index) const;
	bool BarrierFrame(glm::mat4& frame) const;

	// The body to build: set before Spawn (it is what Spawn builds).
	void SetPhysique(const GS::PhysiqueSettings& physique) { m_Physique = physique; }
	const GS::PhysiqueSettings& GetPhysique() const { return m_Physique; }
	// How it walks: takes effect at once, on top of the figure's own gait.
	void SetGait(const GS::GaitSettings& gait)
	{
		m_Gait = gait;
		m_GaitApplied = m_GaitApplied || !GaitIsNormal(gait);
		if (m_DesignStored)
			ApplyVariation();
	}
	const GS::GaitSettings& GetGait() const { return m_Gait; }

	// --- For an owner that is a game rather than the demo ----------------

	// What the rig needs of the world it lives in: a fifteen-link chain
	// solved with fewer iterations gains energy and flails (see the demo's
	// BuildScene, where these were set).
	static constexpr unsigned int VelocityIterations = 16;
	static constexpr unsigned int PositionIterations = 6;

	// Builds the rig standing on `origin`, facing `yaw` radians about +y
	// (0 faces +z, the way it is built; facing is (sin yaw, 0, cos yaw)).
	// Built facing +z, then turned as one rigid piece about the origin --
	// every joint's frame is in its bodies' own spaces, so a rigid turn
	// changes nothing between them -- and the controller re-anchored
	// where it now stands.
	void Spawn(const glm::vec3& origin, float yaw, uint32_t seed = 1)
	{
		BuildRagdoll(origin);
		SetSeed(seed);
		if (std::abs(yaw) > 1e-6f)
		{
			glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
			for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
			{
				GS::RigidBody3D& body = m_World.GetBody(handle);
				body.Position = origin + turn * (body.Position - origin);
				body.PreviousPosition = body.Position;
				body.Orientation = glm::normalize(turn * body.Orientation);
				body.PreviousOrientation = body.Orientation;
				body.UpdateInertiaWorld();
			}
			m_Facing = yaw;
			GoControlled();
		}
		if (m_LocoBaker)
			m_LocoBaker(*this);
	}

	// Standing again at `origin`, facing `yaw`, in the pose it was built
	// in -- whatever state it was in, fallen or flying. Its own bodies are
	// moved back rather than a new rig built (Spawn adds thirteen bodies
	// and twelve joints every time; the world never removes any).
	void Respawn(const glm::vec3& origin, float yaw)
	{
		UnpinFeet();
		m_BraceTimer = 0.0f;
		glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
		for (size_t i = 0; i < m_CharacterBodies.size() && i < m_BuildPose.size(); i++)
		{
			GS::RigidBody3D& body = m_World.GetBody(m_CharacterBodies[i]);
			body.Type = GS::BodyType::Dynamic;
			body.Position = origin + turn * m_BuildPose[i].first;
			body.PreviousPosition = body.Position;
			body.Orientation = glm::normalize(turn * m_BuildPose[i].second);
			body.PreviousOrientation = body.Orientation;
			body.Velocity = glm::vec3(0.0f);
			body.AngularVelocity = glm::vec3(0.0f);
			body.Awake = true;
			body.SleepTimer = 0.0f;
			body.UpdateInertiaWorld();
		}
		m_Facing = yaw;
		m_StandHeight = m_World.GetBody(m_Pelvis).Position.y;
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = AnkleWorld(side);
			m_SwingFrom[side] = m_Planted[side];
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation = m_ShoulderRest[side];
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = 0.0f;
		}
		// Out of whatever move it was in: a respawn mid-climb left the
		// joints open and the move half set up.
		m_Move = Move::None;
		m_DodgeAsked = 0.0f;
		m_DodgeCooldown = 0.0f;
		CloseJointsNow();
		m_FreshSpawn = true;
		m_Mode = Mode::Ragdoll;   // so GoControlled does the whole hand-back
		GoControlled();
	}

	const std::vector<GS::PhysicsWorld3D::BodyHandle>& Bodies() const { return m_CharacterBodies; }

	// Where his joints were, and each body, as he was built: standing on the
	// origin facing +z, x towards side 1. What a recorded movement is put
	// onto (MotionClip.h): its rotations, his own lengths.
	struct RestSkeleton
	{
		glm::vec3 Spine{ 0.0f }, Neck{ 0.0f };
		glm::vec3 Hip[2], Knee[2], Ankle[2], Shoulder[2], Elbow[2];
	};
	const RestSkeleton& Rest() const { return m_Rest; }
	// Each body's centre and turn as built, in the order of Bodies().
	const std::vector<std::pair<glm::vec3, glm::quat>>& BuildPose() const { return m_BuildPose; }

	// --- Locomotion: walking, running and turning, posed from recordings ---
	//
	// While he is someone's to move, where he goes is decided by the keys
	// alone and his body is posed to match, every step, from recorded strides
	// (Runtime/Locomotion.h) -- not simulated, so nothing lags and nothing
	// sinks. Physics has him again the moment anything hits him, and for
	// every move, fall and get-up. Without the clips (assets/mocap/cmu
	// missing) he stands as he was built and steps to keep up.
	void SetLocomotionClips(std::shared_ptr<const LocoClips> clips);
	void SetLocomotionBaker(std::function<void(Humanoid&)> bake)
	{
		m_LocoBaker = std::move(bake);
		if (m_LocoBaker)
			m_LocoBaker(*this);
	}
	bool LocomotionReady() const { return m_LocoClips != nullptr; }
	// His limbs placed by the locomotion this step, rather than simulated.
	bool LocomotionPosed() const { return m_LocoPosed; }
	GS::PhysicsWorld3D::BodyHandle PelvisBody() const { return m_Pelvis; }
	// His foot box, for laying a recorded floor under his own soles.
	glm::vec3 FootHalfExtents() const { return m_World.GetBody(m_Foot[0]).HalfExtents; }
	GS::PhysicsWorld3D::BodyHandle HeadBody() const { return m_Head; }
	bool IsFoot(GS::PhysicsWorld3D::BodyHandle handle) const { return handle == m_Foot[0] || handle == m_Foot[1]; }

	// Which part of him a body is, for drawing a look over it.
	enum class Part { Pelvis, Torso, Head, Thigh, Shin, Foot, UpperArm, Forearm, Other };
	Part PartOf(GS::PhysicsWorld3D::BodyHandle handle) const
	{
		if (handle == m_Pelvis) return Part::Pelvis;
		if (handle == m_Torso) return Part::Torso;
		if (handle == m_Head) return Part::Head;
		for (int side = 0; side < 2; side++)
		{
			if (handle == m_UpperLeg[side]) return Part::Thigh;
			if (handle == m_LowerLeg[side]) return Part::Shin;
			if (handle == m_Foot[side]) return Part::Foot;
			if (handle == m_UpperArm[side]) return Part::UpperArm;
			if (handle == m_LowerArm[side]) return Part::Forearm;
		}
		return Part::Other;
	}
	float FacingRadians() const { return m_Facing; }
	// Down: a ragdoll, no longer driven -- fallen, shoved too hard, or
	// dropped with nothing under it.
	bool IsDown() const { return m_Mode == Mode::Ragdoll; }
	// Why he last went down ("outrun", "toppled", "hit mid-move" ...).
	const char* DownReason() const { return m_RagdollReason; }
	// Hit while standing and catching it with its feet (UpdateStumble).
	bool IsStumbling() const { return m_Mode == Mode::Stumbling; }
	bool IsGettingUp() const { return m_Mode == Mode::GettingUp; }
	// Getting up from on his back (rolling over first), not his front.
	bool GettingUpFromBack() const { return m_GetUpSupine; }
	// In a parkour move -- vaulting, climbing, dropping, rolling.
	bool IsMoving() const { return m_Mode == Mode::Moving; }
	// Hanging from an edge, where the keys do other things (UpdateHang).
	bool IsHanging() const { return m_Mode == Mode::Moving && m_Move == Move::Hang; }
	const char* MoveName() const
	{
		switch (m_Move)
		{
			case Move::Vault: return m_SpeedVault ? "speed vault" : "vault";
			case Move::StepOn: return "step on";
			case Move::Climb: return "climb";
			case Move::Drop: return "drop";
			case Move::Roll: return "roll";
			case Move::WallRun: return "wall run";
			case Move::Hang: return "hang";
			case Move::DropToHang: return "drop to hang";
			case Move::CatLeap: return "cat leap";
			case Move::TicTac: return "tic-tac";
			case Move::Precision: return "precision jump";
			case Move::Dodge: return DodgeName();
			default: return "";
		}
	}
	// What a jump would do now, for an on-screen hint: "vault", "climb",
	// "step on", or "" when it would just be a jump. A little before it is
	// in reach -- a jump pressed then waits for it (see the walk's jump) --
	// and only square on to it, as TryMove asks.
	const char* ParkourHint() const
	{
		if (m_Parkour && m_Mode == Mode::Moving && m_Move == Move::Hang)
			return "climb up";
		if (!ParkourOn() || m_Mode != Mode::Controlled || m_Ahead.Type == Obstacle::None || m_Ahead.Type == Obstacle::Wall)
			return "";
		glm::vec3 F(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 n(m_Ahead.Normal.x, 0.0f, m_Ahead.Normal.z);
		if (glm::length(n) < 1e-4f || -glm::dot(F, glm::normalize(n)) <= 0.75f)
			return "";
		float s = m_Proportions.Scale;
		bool running = m_Control.Enabled && m_Control.Run && m_RunBlend > 0.5f;
		switch (m_Ahead.Type)
		{
			case Obstacle::Vault:
				if (m_Ahead.Distance > 0.55f * s + 0.22f * m_Speed + 0.6f * s)
					return "";
				return !running && StepsOnto(m_Ahead) ? "step on" : "vault";
			case Obstacle::StepOn: return m_Ahead.Distance <= 0.80f * s + 0.15f * m_Speed + 0.4f * s ? "step on" : "";
			case Obstacle::Climb: return m_Ahead.Distance <= 0.80f * s + 0.15f * m_Speed + 0.4f * s ? "climb" : "";
			default: return "";
		}
	}
	// A dodge, along `direction` (world, flattened; zero for the way
	// that suits -- back, standing): what it becomes depends on how fast
	// he is going (TryDodge). Asked for on the press, it waits a quarter
	// of a second for a moment it can go -- landing, the end of a move.
	void RequestDodge(const glm::vec3& direction)
	{
		m_DodgeAsked = 0.25f;
		m_DodgeWish = direction;
	}
	bool IsDodging() const { return m_Mode == Mode::Moving && m_Move == Move::Dodge; }
	// What a dodge was, or is: "sidestep", "cut", "brake", "dive".
	const char* DodgeName() const
	{
		switch (m_DodgeKind)
		{
			case Dodge::Step: return "sidestep";
			case Dodge::Cut: return "cut";
			case Dodge::Brake: return "brake";
			case Dodge::Dive: return "dive";
			default: return "";
		}
	}
	uint32_t DodgeCount() const { return m_Dodges; }
	// On his feet and free to act: walking, standing, or in a dodge --
	// what a game asks before it gives him something to get out of the
	// way of (a move, a hang, a fall or a get-up is none of these).
	bool CanReact() const
	{
		return (m_Mode == Mode::Controlled && m_Air == Air::Grounded) || IsDodging();
	}
	void SetParkour(bool on) { m_Parkour = on; }
	bool GetParkour() const { return m_Parkour; }
	float TimeDown() const { return m_RagdollTimer; }
	void GetUp() { if (m_Mode == Mode::Ragdoll) BeginGetUp(); }
	// The pelvis's height above the ground it was built standing on.
	float RestPelvisHeight() const { return m_StandClearance; }

	// One fixed step of everything that decides what the body does --
	// balance, stagger, walk or get-up, and the stepping strategy while it
	// is falling. Call before the world steps.
	void Update(float dt)
	{
		m_Clock += dt;
		m_LastDt = dt;
		m_MoveGrace = std::max(m_MoveGrace - dt, 0.0f);
		m_DodgeCooldown = std::max(m_DodgeCooldown - dt, 0.0f);
		RefreshStandable();
		EaseJointsClosed(dt);
		m_Balance = ComputeBalance();
		UpdateArmament(dt);

		if (m_Mode == Mode::Controlled)
		{
			// A hard enough hit anywhere but the feet ends control. Checked
			// before the step, so the impulse judged is the one that arrived
			// during the last one.
			glm::vec3 hitFrom(0.0f);
			float hit = ImpactForce(dt, hitFrom);
			// The contacts read here are the last world step's, and just
			// after a respawn those belong to wherever he was: rolling on the
			// floor when `station` moved him, they stumbled him on his mark.
			if (m_FreshSpawn)
				hit = 0.0f;
			m_FreshSpawn = false;

			// Everything above the noise floor is a stagger now. Whether he
			// actually goes down is decided by whether his feet can catch what
			// it did to him, not by the size of the number.
			if (hit > m_StaggerThreshold)
			{
				if (StumblesPhysically())
				{
					// The root was kinematic when it was hit, so the hit went
					// nowhere -- an infinite mass takes any impulse and does
					// not move. Handed to the body now that it can move.
					BeginStumble();
					m_World.ApplyImpulseAt(m_Torso, m_HitPush * (hit * dt), m_World.GetBody(m_Torso).Position);
				}
				else
					Stagger(hit * dt, hitFrom);
			}

			// A dodge asked for: from the walk, on the ground. A press that
			// cannot go yet (in the air, cooling down) waits for a moment.
			if (m_Mode == Mode::Controlled && m_DodgeAsked > 0.0f)
			{
				m_DodgeAsked -= dt;
				if (m_Air == Air::Grounded && m_DodgeCooldown <= 0.0f && TryDodge(m_DodgeWish))
				{
					m_DodgeAsked = 0.0f;
					return;
				}
			}

			if (m_Mode == Mode::Controlled)
				DriveLocomotion(dt);
		}
		else if (m_Mode == Mode::Stumbling)
		{
			UpdateStumble(dt);
		}
		else if (m_Mode == Mode::GettingUp)
		{
			UpdateGetUp(dt);
		}
		else if (m_Mode == Mode::Moving)
		{
			// Hit in the middle of a move -- a cannonball mid-vault, or mid-
			// dodge -- he goes down. The hips are carried, so the blow went
			// into an immovable body and moved nothing; it is handed to the
			// limp one. Only loose things count: in a move his hands, knees
			// and hips are meant to be on the walls and tops he is going over.
			glm::vec3 hitFrom(0.0f);
			float hit = ImpactForce(dt, hitFrom, true);
			if (hit > m_MoveHitThreshold && m_Move != Move::Hang)
			{
				glm::vec3 push = m_HitPush;
				OpenJoints(false);
				m_Move = Move::None;
				GoRagdoll("hit mid-move");
				m_World.ApplyImpulseAt(m_Torso, push * (hit * dt), m_World.GetBody(m_Torso).Position);
			}
			else
			{
				if (m_DodgeAsked > 0.0f)
					m_DodgeAsked -= dt;
				UpdateMove(dt);
				// Still in it: the limbs placed where the move's joint targets
				// put them, not pulled there by motors (Runtime/Locomotion.h).
				if (m_Mode == Mode::Moving)
					PoseFromMotors(dt);
			}
		}
		else
		{
			m_RagdollTimer += dt;
			UpdateBrace(dt);
		}

		if (m_Mode == Mode::Ragdoll && m_BalanceEnabled)
		{
			// Stepping decides first: while a foot is in the air the ankle
			// strategy must not fight the swing leg for the same joints.
			UpdateStepping(m_Balance, dt);
			DriveSwingLeg();
			ApplyBalance(m_Balance);
		}

	}


	// ---------------------------------------------------------------------
	// Natural variation
	// ---------------------------------------------------------------------
	//
	// Two figures built from the same numbers walk in lockstep, and one
	// standing still is a statue. People differ from each other, and no one
	// does the same thing twice. Both kinds are here:
	//
	//   * per figure, from its seed: walking speed, stride, stance width,
	//     arm swing, posture, bob and sway each a few percent off the
	//     design -- the character of the walk;
	//   * per moment, from the seed and the clock: each step lands a little
	//     differently, the stride breathes, and standing still he breathes,
	//     shifts his weight between his feet and glances about.
	//
	// All of it is a function of the seed and the fixed-step clock -- a
	// hash, never a random-number generator with state of its own -- so a
	// recording replays exactly and a figure is the same figure every run.
	// m_Variation scales it all; 0 is the design itself.

	// A seed of 0 is no variation at all; the default is 1.
	void SetSeed(uint32_t seed)
	{
		m_Seed = seed;
		if (!m_DesignStored)
		{
			m_Design = { m_WalkSpeed, m_RunSpeed, m_StrideLength, m_StanceWidth, m_ArmSwing,
				m_ElbowBend, m_LeanAmount, m_BobAmount, m_SwayAmount, m_TurnRate, m_RunFootLift };
			m_DesignStored = true;
		}
		ApplyVariation();
	}

	void ApplyVariation()
	{
		float amount = m_Seed == 0 ? 0.0f : m_Variation;
		auto vary = [&](int channel, float spread) { return 1.0f + amount * spread * Signed(m_Seed, (uint32_t)channel); };
		const Design& d = m_Design;
		m_WalkSpeed = d.WalkSpeed * vary(1, 0.08f);
		m_RunSpeed = d.RunSpeed * vary(2, 0.06f);
		m_StrideLength = d.StrideLength * vary(3, 0.06f);
		m_StanceWidth = d.StanceWidth * vary(4, 0.15f);
		m_ArmSwing = d.ArmSwing * vary(5, 0.25f);
		m_ElbowBend = d.ElbowBend * vary(6, 0.20f);
		m_LeanAmount = d.LeanAmount * vary(7, 0.30f);
		m_BobAmount = d.BobAmount * vary(8, 0.40f);
		m_SwayAmount = d.SwayAmount * vary(9, 0.30f);
		m_TurnRate = d.TurnRate * vary(10, 0.10f);
		m_RunFootLift = d.RunFootLift * vary(11, 0.12f);
		// A posture: some stand a little forward, some a little back.
		m_Posture = amount * 0.035f * Signed(m_Seed, 12u);
		// Breathing between 3.5 and 5 seconds a breath.
		m_BreathPeriod = 4.2f * (1.0f + amount * 0.18f * Signed(m_Seed, 13u));

		// The walking style, on top -- only once one has been asked for, so
		// a rig that never has one keeps every member as it was tuned.
		if (!m_GaitApplied)
			return;
		if (!m_StyleBaseStored)
		{
			m_StyleBase = { m_FootLift, m_RollAmount, m_Crouch };
			m_StyleBaseStored = true;
		}
		const GS::GaitSettings& g = m_Gait;
		float s = m_Proportions.Scale;
		m_WalkSpeed *= g.Speed;
		m_RunSpeed *= g.RunSpeed;
		m_StrideLength *= g.Stride;
		m_StanceWidth *= g.StanceWidth;
		m_ArmSwing *= g.ArmSwing;
		m_ElbowBend *= g.ElbowBend;
		m_LeanAmount *= g.Lean;
		m_BobAmount *= g.Bob;
		m_SwayAmount *= g.Sway;
		m_TurnRate *= g.Turn;
		m_RunFootLift *= g.FootLift;
		m_FootLift = m_StyleBase.FootLift * g.FootLift;
		m_RollAmount = m_StyleBase.RollAmount * g.HipRoll;
		m_Crouch = m_StyleBase.Crouch + g.Crouch * s;
		m_Posture += g.Posture;
	}

	// A hash of two numbers to [0, 1): the same pair, the same answer.
	static float Hash01(uint32_t a, uint32_t b)
	{
		uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
		h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
		return (h >> 8) * (1.0f / 16777216.0f);
	}
	static float Signed(uint32_t a, uint32_t b) { return 2.0f * Hash01(a, b) - 1.0f; }

	// Smooth noise in [-1, 1] over time: a hashed value at each whole t,
	// eased between. Channel picks an independent stream.
	float Noise(float t, uint32_t channel) const
	{
		float whole = std::floor(t);
		int32_t i = (int32_t)whole;
		float f = t - whole;
		float u = f * f * (3.0f - 2.0f * f);
		float a = Signed(m_Seed * 977u + channel, (uint32_t)i);
		float b = Signed(m_Seed * 977u + channel, (uint32_t)(i + 1));
		return a + (b - a) * u;
	}

	float VariationAmount() const { return m_Seed == 0 ? 0.0f : m_Variation; }

	// Standing: a breath in the chest, the weight drifting from one foot to
	// the other, the head glancing about. Faded out as he walks -- except
	// the glances, which stay, smaller. Returns the hips' sideways drift.
	float Idle(float dt, float stride, const glm::vec3& right)
	{
		float amount = VariationAmount();
		m_IdleWeight += ((1.0f - stride) - m_IdleWeight) * (1.0f - std::exp(-dt / 0.6f));
		float idle = m_IdleWeight * amount;

		// Breath: a degree or so of the chest rising and falling.
		m_Breath = idle * 0.018f * std::sin(glm::two_pi<float>() * m_Clock / std::max(m_BreathPeriod, 1.0f));

		// The head: holds a direction, then turns to a new one, every 2-5 s,
		// easing over about a third of a second -- glances, not a wobble.
		if (m_Clock >= m_NextGlance)
		{
			uint32_t n = m_GlanceCount++;
			m_GlanceYaw = 0.45f * Signed(m_Seed * 31u + 7u, n);
			m_GlancePitch = 0.12f * Signed(m_Seed * 31u + 8u, n) - 0.04f;
			// A tilt goes with a glance; on a head with no face it is most of
			// what shows of one.
			m_GlanceTilt = 0.12f * Signed(m_Seed * 31u + 10u, n);
			m_NextGlance = m_Clock + 2.0f + 3.0f * Hash01(m_Seed * 31u + 9u, n);
		}
		float reach = glm::mix(0.35f, 1.0f, m_IdleWeight) * amount;
		float ease = 1.0f - std::exp(-dt / 0.3f);
		m_HeadYaw += (m_GlanceYaw * reach - m_HeadYaw) * ease;
		m_HeadPitch += (m_GlancePitch * reach - m_HeadPitch) * ease;
		m_HeadTilt += (m_GlanceTilt * reach - m_HeadTilt) * ease;
		float headPitch = m_GaitApplied ? m_HeadPitch + m_Gait.HeadPitch : m_HeadPitch;
		// Turning, the head goes first: a person looks where they are
		// turning, then the body follows -- turning round, the head is half
		// way there 0.22 s before the hips (measured). His followed them.
		float lead = glm::clamp(m_TurnToGo * m_HeadLeadShare, -m_HeadLeadMax, m_HeadLeadMax);
		m_HeadLead += (lead - m_HeadLead) * (1.0f - std::exp(-dt / 0.12f));
		m_World.GetJoint(m_NeckJoint).MotorTargetRotation = glm::normalize(
			glm::angleAxis(m_HeadYaw + m_HeadLead, glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(headPitch, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(m_HeadTilt, glm::vec3(0.0f, 0.0f, 1.0f)) * m_NeckRest);

		// The weight shift: a slow drift of the hips over one foot and
		// back, a few centimetres, on a 6-10 s swell. The drift alone barely
		// shows; what the eye reads is the hip dropping on the unloaded side
		// and the shoulders tilting back against it, so those go with it.
		float shift = Noise(m_Clock / 7.0f, 21u);
		m_IdleRoll = idle * 0.08f * shift;

		return idle * 0.035f * shift;
	}

	// A shove at the torso, as the demo's X key and a hit in a game: the
	// impulse, and a stagger the controller then has to catch.
	void Shove(float strength, const glm::vec3& direction)
	{
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);

		// A shove applied straight to a body never arrives through a contact,
		// so ImpactForce would never see it. Judged on its own terms: the
		// impulse over one step is the force it represents.
		// The impulse is what was applied; no need to turn it into a force and
		// back again.
		if (m_Mode == Mode::Controlled)
		{
			if (StumblesPhysically())
			{
				// Dynamic first, so the impulse moves the whole body rather
				// than a torso jointed to an immovable pelvis.
				BeginStumble();
				m_World.ApplyImpulseAt(m_Torso, direction * strength, torso.Position);
				return;
			}
			Stagger(strength, direction);
		}
		m_World.ApplyImpulseAt(m_Torso, direction * strength, torso.Position);
	}


	// ---------------------------------------------------------------------
	// The body: height, mass, proportions
	// ---------------------------------------------------------------------
	//
	// The rig was laid out for one man, 1.86 m and 71.2 kg. A physique
	// (GS::PhysiqueSettings) makes it anyone:
	//
	//   * **Height** scales every length -- bones, joint positions, the
	//     controller's stride, foot lift, catch reach -- by s = h / 1.86.
	//     Speeds and times go as sqrt(s): two people walking at the same
	//     Froude number (v^2 / g L) are walking alike, and a pendulum's
	//     period is sqrt(L / g).
	//   * **Mass** against the mass the same figure would have scaled up
	//     isometrically (71.2 s^3) gives the girth, g = sqrt(m / 71.2 s^3):
	//     limbs and trunk widen by g, the belly a little more (g^1.25 front
	//     to back). Joint strength goes as the weight times the lever,
	//     (m / 71.2) s, so a heavy figure can carry itself and a light one is
	//     not over-sprung.
	//   * **Femininity** moves the proportions between the two templates
	//     anthropometry gives: shoulders 10% narrower and hips 12% wider at
	//     1, mass carried lower (pelvis and thighs heavier, chest and upper
	//     arms lighter), a slightly smaller head.
	//
	// The default physique gives exactly the numbers that were here before,
	// to the bit: every scale is a multiply by 1.0f and the masses are the
	// old literals, so the Ragdoll demo and every recording are unchanged.
	struct Proportions
	{
		float Scale = 1.0f, Girth = 1.0f, Strength = 1.0f;
		glm::vec3 PelvisHalf{ 0.14f, 0.09f, 0.10f }, TorsoHalf{ 0.17f, 0.22f, 0.11f }, FootHalf{ 0.06f, 0.05f, 0.11f };
		float HeadRadius = 0.12f;
		float HipX = 0.10f, ShoulderX = 0.20f, ArmX = 0.22f;
		float ThighRadius = 0.08f, ShinRadius = 0.07f;
		float UpperArmRadius = 0.055f, UpperArmHalf = 0.12f, ForearmRadius = 0.05f, ForearmHalf = 0.11f;
		float PelvisMass = 11.0f, TorsoMass = 24.0f, HeadMass = 5.0f, ThighMass = 7.5f, ShinMass = 3.3f, FootMass = 1.0f,
			UpperArmMass = 2.2f, ForearmMass = 1.6f;
	};

	bool PhysiqueIsDefault() const { return m_Physique == GS::PhysiqueSettings{}; }
	// The proportions it was built with: what a look is drawn to.
	const Proportions& GetProportions() const { return m_Proportions; }

	Proportions ComputeProportions() const
	{
		Proportions P;
		if (PhysiqueIsDefault())
			return P;
		const GS::PhysiqueSettings& b = m_Physique;
		float s = glm::clamp(b.Height, 0.9f, 2.4f) / 1.86f;
		float f = glm::clamp(b.Femininity, 0.0f, 1.0f);
		float mass = glm::clamp(b.Mass, 20.0f, 250.0f);
		float g = glm::clamp(std::sqrt(mass / (71.2f * s * s * s)), 0.72f, 1.7f);
		float belly = g >= 1.0f ? std::pow(g, 1.25f) : g;
		float shoulders = 1.0f - 0.10f * f, hips = 1.0f + 0.12f * f;
		P.Scale = s;
		P.Girth = g;
		P.Strength = (mass / 71.2f) * s;
		P.PelvisHalf = { 0.14f * s * hips * g, 0.09f * s, 0.10f * s * belly };
		P.TorsoHalf = { 0.17f * s * shoulders * g, 0.22f * s, 0.11f * s * belly };
		P.FootHalf = glm::vec3(0.06f, 0.05f, 0.11f) * s * (1.0f - 0.05f * f);
		P.HeadRadius = 0.12f * s * (1.0f - 0.04f * f);
		P.ThighRadius = 0.08f * s * g * (1.0f + 0.06f * f);
		P.ShinRadius = 0.07f * s * g;
		P.UpperArmRadius = 0.055f * s * g * (1.0f - 0.05f * f);
		P.ForearmRadius = 0.05f * s * g * (1.0f - 0.05f * f);
		// Same joint positions, fatter capsules: the half-height gives up
		// what the radius gains.
		P.UpperArmHalf = std::max(0.02f, 0.12f * s - (P.UpperArmRadius - 0.055f * s));
		P.ForearmHalf = std::max(0.02f, 0.11f * s - (P.ForearmRadius - 0.05f * s));
		// Apart enough that bodies with no joint between them do not start
		// inside each other: the thighs, and a forearm hanging by the hips.
		P.HipX = std::max(0.10f * s * hips, P.ThighRadius * 1.08f);
		P.ArmX = std::max(0.22f * s * shoulders, std::max(P.TorsoHalf.x * 0.95f, P.PelvisHalf.x + P.ForearmRadius + 0.012f * s));
		P.ShoulderX = 0.20f * s * shoulders + (P.ArmX - 0.22f * s * shoulders);

		// Segment masses: the old fractions, moved lower for a feminine build
		// and onto the trunk and thighs for a heavier one, then normalised to
		// the mass asked for.
		struct Segment { float* Mass; float Feminine; float Girth; int Count; };
		Segment segments[] = {
			{ &P.PelvisMass, 0.15f, 0.40f, 1 }, { &P.TorsoMass, -0.08f, 0.50f, 1 }, { &P.HeadMass, 0.0f, 0.0f, 1 },
			{ &P.ThighMass, 0.12f, 0.20f, 2 }, { &P.ShinMass, 0.0f, 0.05f, 2 }, { &P.FootMass, -0.05f, 0.0f, 2 },
			{ &P.UpperArmMass, -0.12f, 0.10f, 2 }, { &P.ForearmMass, -0.10f, 0.0f, 2 } };
		float total = 0.0f;
		for (Segment& seg : segments)
		{
			*seg.Mass *= (1.0f + seg.Feminine * f) * (1.0f + seg.Girth * (g - 1.0f));
			total += *seg.Mass * (float)seg.Count;
		}
		for (Segment& seg : segments)
			*seg.Mass *= mass / total;
		return P;
	}

	// The controller's lengths, speeds, times, forces and torques for the
	// physique (see Proportions). Each member's value is captured the first
	// time this runs and scaled from that every time after, so building again
	// never compounds. Not at all for the default physique on a rig that has
	// never had another: the Ragdoll demo tunes these members live.
	void ApplyPhysiqueScale()
	{
		if (PhysiqueIsDefault() && !m_PhysiqueApplied)
			return;
		m_PhysiqueApplied = true;
		enum Kind { Length, Speed, Time, Force, Torque, Stance };
		if (m_Scaled.empty())
		{
			auto add = [&](float& member, Kind kind) { m_Scaled.push_back({ &member, member, (int)kind }); };
			for (float* m : { &m_StrideLength, &m_RunStride, &m_FootLift, &m_RunFootLift, &m_RunFootReach,
				&m_MaxFootReach, &m_RollReach, &m_BobAmount, &m_SwayAmount, &m_JumpHeight, &m_JumpCrouchDepth, &m_MaxStepUp,
				&m_CatchReach, &m_CatchReachSide, &m_RecoverCrouch, &m_StumbleStance, &m_StumbleFootGap, &m_StumbleReach,
				&m_StumbleLift, &m_StumbleOvershoot, &m_StumbleSettle, &m_StumbleRestCrouch, &m_StumbleMaxCrouch, &m_StaggerStep,
				&m_HandForward, &m_HandWidth, &m_HandHeight, &m_PushUpHeight, &m_GetUpShift, &m_KneelHeight, &m_GetUpFootLift,
				&m_StumbleGiveUpHeight, &m_MaxStep, &m_MinStep, &m_StepTrigger, &m_StumbleTrigger, &m_JumpLegSplit,
				&m_JumpLegSplitReach, &m_StumbleCalmMargin, &m_GetUpMaxGap, &m_FootToAnkle.x, &m_FootToAnkle.y, &m_FootToAnkle.z })
				add(*m, Length);
			for (float* m : { &m_WalkSpeed, &m_RunSpeed, &m_StumbleEntrySpeed, &m_StumbleCalmWalk, &m_StaggerSettle, &m_StumbleCalmSpeed })
				add(*m, Speed);
			for (float* m : { &m_GetUpTime, &m_StumbleStepTime, &m_StepDuration, &m_JumpCrouchTime, &m_LandAbsorb, &m_LandTime })
				add(*m, Time);
			add(m_StaggerThreshold, Force);
			for (float* m : { &m_StumbleUprightGain, &m_StumbleUprightDamping, &m_StumbleMaxTorque, &m_LimpTorque })
				add(*m, Torque);
			add(m_StanceWidth, Stance);
		}
		Proportions P = ComputeProportions();
		float s = P.Scale, root = std::sqrt(s), weight = glm::clamp(m_Physique.Mass, 20.0f, 250.0f) / 71.2f;
		for (const ScaledMember& m : m_Scaled)
		{
			float factor = 1.0f;
			switch (m.Kind)
			{
				case Length: factor = s; break;
				case Speed: factor = root; break;
				case Time: factor = root; break;
				case Force: factor = weight; break;
				case Torque: factor = weight * s; break;
				case Stance: factor = P.HipX / 0.10f; break;   // feet under the hips, however wide they are
			}
			*m.Member = m.Base * factor;
		}
		// The walk's design is taken afresh from the scaled values.
		m_DesignStored = false;
	}

	// ---------------------------------------------------------------------
	// Walking styles
	// ---------------------------------------------------------------------
	//
	// A style is the gait's every parameter at once (GS::GaitSettings), on
	// top of the figure's own: scales of its tuned speed, stride, arm swing,
	// bob, sway and hip roll; a posture and a head carriage; how far the
	// knees stay bent; how regular the steps are; a wander for a drunk and a
	// limp for an injured leg. The presets are starting points -- a
	// character can carry any values.
	static const std::vector<GS::GaitSettings>& GaitPresets()
	{
		static const std::vector<GS::GaitSettings> presets = []()
		{
			std::vector<GS::GaitSettings> list;
			auto add = [&](const char* name, auto edit) { GS::GaitSettings g; g.Preset = name; edit(g); list.push_back(g); };
			add("Normal", [](GS::GaitSettings&) {});
			add("Brisk", [](GS::GaitSettings& g) { g.Speed = 1.25f; g.Stride = 1.1f; g.ArmSwing = 1.4f; g.ElbowBend = 1.7f; g.Lean = 1.4f; g.Jitter = 0.7f; });
			add("Stroll", [](GS::GaitSettings& g) { g.Speed = 0.72f; g.Stride = 0.86f; g.ArmSwing = 0.65f; g.ElbowBend = 0.7f; g.Bob = 0.8f; g.Sway = 1.25f; g.Posture = -0.015f; g.Turn = 0.8f; });
			add("Confident", [](GS::GaitSettings& g) { g.Speed = 0.95f; g.Stride = 1.06f; g.ArmSwing = 1.5f; g.ArmsOut = 0.09f; g.Sway = 1.6f; g.HipRoll = 1.4f; g.Posture = -0.035f; g.StanceWidth = 1.3f; g.HeadPitch = -0.05f; });
			add("Feminine", [](GS::GaitSettings& g) { g.Speed = 0.95f; g.Stride = 0.94f; g.StanceWidth = 0.35f; g.HipRoll = 1.9f; g.Sway = 1.35f; g.ArmSwing = 0.85f; g.ElbowBend = 1.3f; g.Posture = -0.02f; });
			add("March", [](GS::GaitSettings& g) { g.Speed = 1.1f; g.ArmSwing = 1.9f; g.ElbowBend = 0.15f; g.Posture = -0.03f; g.Sway = 0.5f; g.Bob = 0.7f; g.FootLift = 1.8f; g.Jitter = 0.15f; g.HeadPitch = -0.04f; });
			add("Tired", [](GS::GaitSettings& g) { g.Speed = 0.62f; g.Stride = 0.78f; g.ArmSwing = 0.3f; g.ElbowBend = 0.4f; g.Posture = 0.09f; g.HeadPitch = 0.28f; g.Bob = 1.3f; g.FootLift = 0.5f; g.Jitter = 1.6f; g.RunSpeed = 0.8f; });
			add("Elderly", [](GS::GaitSettings& g) { g.Speed = 0.52f; g.Stride = 0.62f; g.ArmSwing = 0.35f; g.ElbowBend = 1.4f; g.Posture = 0.13f; g.HeadPitch = 0.12f; g.Crouch = 0.035f; g.StanceWidth = 1.5f; g.FootLift = 0.55f; g.Turn = 0.55f; g.Jitter = 1.3f; g.RunSpeed = 0.55f; g.Sway = 1.3f; });
			add("Sneak", [](GS::GaitSettings& g) { g.Speed = 0.55f; g.Stride = 0.92f; g.Crouch = 0.17f; g.Posture = 0.26f; g.ArmSwing = 0.25f; g.ElbowBend = 2.4f; g.FootLift = 1.4f; g.Bob = 0.35f; g.Sway = 0.6f; g.HeadPitch = -0.15f; });
			add("Limp", [](GS::GaitSettings& g) { g.Speed = 0.6f; g.Stride = 0.82f; g.Limp = 1.0f; g.ArmSwing = 0.7f; g.Sway = 1.6f; g.RunSpeed = 0.6f; g.Jitter = 0.8f; });
			add("Drunk", [](GS::GaitSettings& g) { g.Speed = 0.72f; g.Sway = 2.6f; g.HipRoll = 1.6f; g.Wander = 0.38f; g.Jitter = 3.5f; g.Posture = 0.05f; g.HeadPitch = 0.08f; g.StanceWidth = 1.8f; g.Turn = 0.7f; g.ArmsOut = 0.12f; g.ArmSwing = 1.2f; });
			return list;
		}();
		return presets;
	}

	// The preset of that name, or Normal.
	static GS::GaitSettings GaitPreset(const std::string& name)
	{
		for (const GS::GaitSettings& g : GaitPresets())
			if (g.Preset == name)
				return g;
		return GaitPresets().front();
	}

	static bool GaitIsNormal(const GS::GaitSettings& g)
	{
		GS::GaitSettings normal;
		const float* a = &g.Speed;
		const float* b = &normal.Speed;
		for (int i = 0; i < 19; i++)
			if (a[i] != b[i])
				return false;
		return true;
	}

	// ---------------------------------------------------------------------
	// The rig
	// ---------------------------------------------------------------------
	//
	// Laid out from the floor up, in metres, for a figure about 1.86 m tall.
	// Masses are roughly anthropometric and total about 71 kg: they matter
	// more than they look like they should, because a joint's behaviour is
	// set by the *ratio* of the masses it connects, and a solver that copes
	// with 1:1 can sag badly at 15:1.
	void BuildRagdoll(const glm::vec3& origin)
	{
		auto capsule = [&](glm::vec3 centre, float radius, float halfHeight, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeCapsule(
				origin + centre, radius, halfHeight, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			// Limbs are damped more than loose debris would be. Flesh is not
			// a pendulum, and without this a passive ragdoll swings its arms
			// for an implausibly long time after it lands.
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		auto box = [&](glm::vec3 centre, glm::vec3 halfExtents, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeBox(
				origin + centre, halfExtents, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		auto sphere = [&](glm::vec3 centre, float radius, float mass)
		{
			GS::RigidBody3D body = GS::RigidBody3D::MakeSphere(
				origin + centre, radius, mass);
			body.Friction = 0.7f;
			body.Restitution = 0.0f;
			body.AngularDamping = 0.15f;
			body.LinearDamping = 0.05f;
			return m_World.AddBody(body);
		};

		ApplyPhysiqueScale();
		const Proportions P = ComputeProportions();
		m_Proportions = P;
		const float s = P.Scale;
		const float hipX = P.HipX;
		const float shoulderX = P.ShoulderX;

		// --- torso column -------------------------------------------------
		m_Pelvis = box({ 0.0f, 1.09f * s, 0.0f }, P.PelvisHalf, P.PelvisMass);
		m_Torso  = box({ 0.0f, 1.40f * s, 0.0f }, P.TorsoHalf, P.TorsoMass);
		m_Head   = sphere({ 0.0f, 1.74f * s, 0.0f }, P.HeadRadius, P.HeadMass);

		// --- legs -----------------------------------------------------------
		//
		// The hips stay where they are and the feet move outwards, so a wider
		// stance splays the legs rather than detaching them from the pelvis.
		// Everything below is placed along the hip-to-ankle line, which is why
		// the capsules need orienting: a leg at an angle is not a leg pointing
		// down.
		for (int side = 0; side < 2; side++)
		{
			float sign = side == 0 ? -1.0f : 1.0f;

			glm::vec3 hipAt(sign * hipX, 1.00f * s, 0.0f);
			glm::vec3 ankleAt(sign * m_StanceWidth, 0.10f * s, 0.0f);

			// The knee sits the same fraction along the leg as it did when the
			// leg was vertical.
			glm::vec3 kneeAt = hipAt + (ankleAt - hipAt) * 0.533f;

			glm::vec3 upperAxis = hipAt - kneeAt;
			glm::vec3 lowerAxis = kneeAt - ankleAt;

			glm::quat upperTurn = RotationBetween({ 0.0f, 1.0f, 0.0f }, glm::normalize(upperAxis));
			glm::quat lowerTurn = RotationBetween({ 0.0f, 1.0f, 0.0f }, glm::normalize(lowerAxis));

			m_UpperLeg[side] = capsule((hipAt + kneeAt) * 0.5f, P.ThighRadius,
				glm::length(upperAxis) * 0.5f - P.ThighRadius, P.ThighMass);
			m_LowerLeg[side] = capsule((kneeAt + ankleAt) * 0.5f, P.ShinRadius,
				glm::length(lowerAxis) * 0.5f - P.ShinRadius, P.ShinMass);
			m_Foot[side] = box({ sign * m_StanceWidth, 0.05f * s, 0.03f * s },
				P.FootHalf, P.FootMass);

			m_World.GetBody(m_UpperLeg[side]).Orientation = upperTurn;
			m_World.GetBody(m_UpperLeg[side]).UpdateInertiaWorld();
			m_World.GetBody(m_LowerLeg[side]).Orientation = lowerTurn;
			m_World.GetBody(m_LowerLeg[side]).UpdateInertiaWorld();

			m_KneeAt[side] = kneeAt;
			m_AnkleAt[side] = ankleAt;
		}

		// --- arms -----------------------------------------------------------
		for (int side = 0; side < 2; side++)
		{
			float x = side == 0 ? -shoulderX : shoulderX;
			float armX = side == 0 ? -P.ArmX : P.ArmX;

			m_UpperArm[side] = capsule({ armX, 1.375f * s, 0.0f }, P.UpperArmRadius, P.UpperArmHalf, P.UpperArmMass);
			m_LowerArm[side] = capsule({ armX, 1.040f * s, 0.0f }, P.ForearmRadius, P.ForearmHalf, P.ForearmMass);
			(void)x;
		}

		// --- joints ---------------------------------------------------------
		//
		// Every ball joint gets a cone and a twist range, because an unlimited
		// one lets the limb rotate straight through the body -- and every one
		// of these is measured from the pose being built here rather than from
		// the two bodies being aligned, which is why a rig assembled crooked
		// still starts inside its own limits.
		auto ball = [&](GS::PhysicsWorld3D::BodyHandle a,
			GS::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
			glm::vec3 twistAxis, float coneDegrees, float twistDegrees, float torque)
		{
			auto joint = m_World.AddBallJoint(a, b, origin + anchor);
			m_World.SetConeTwistLimits(joint, twistAxis,
				glm::radians(coneDegrees),
				glm::radians(-twistDegrees), glm::radians(twistDegrees));
			m_World.SetJointMotor(joint, m_MotorStiffness, torque);
			m_Joints.push_back(joint);
			return joint;
		};

		auto hinge = [&](GS::PhysicsWorld3D::BodyHandle a,
			GS::PhysicsWorld3D::BodyHandle b, glm::vec3 anchor,
			glm::vec3 axis, float lowerDegrees, float upperDegrees, float torque)
		{
			auto joint = m_World.AddHingeJoint(a, b, origin + anchor, axis);
			m_World.SetHingeLimits(joint, glm::radians(lowerDegrees), glm::radians(upperDegrees));
			m_World.SetJointMotor(joint, m_MotorStiffness, torque);
			m_Joints.push_back(joint);
			return joint;
		};

		// Spine and neck. The spine carries everything above it, so it is the
		// strongest joint in the rig by a wide margin.
		const float strength = P.Strength;
		m_SpineJoint = ball(m_Pelvis, m_Torso, { 0.0f, 1.18f * s, 0.0f },
			{ 0.0f, 1.0f, 0.0f }, 25.0f, 35.0f, 260.0f * strength);
		m_SpineRest = m_World.GetJoint(m_SpineJoint).MotorTargetRotation;
		m_NeckJoint = ball(m_Torso, m_Head, { 0.0f, 1.62f * s, 0.0f }, { 0.0f, 1.0f, 0.0f }, 40.0f, 60.0f, 30.0f * strength);
		m_NeckRest = m_World.GetJoint(m_NeckJoint).MotorTargetRotation;
		m_Rest.Spine = { 0.0f, 1.18f * s, 0.0f };
		m_Rest.Neck = { 0.0f, 1.62f * s, 0.0f };

		for (int side = 0; side < 2; side++)
		{
			float x = side == 0 ? -hipX : hipX;
			float armX = side == 0 ? -P.ArmX : P.ArmX;
			float shoulderSide = side == 0 ? -shoulderX : shoulderX;

			// Hip, knee, ankle.
			m_HipJoint[side] = ball(m_Pelvis, m_UpperLeg[side], { x, 1.00f * s, 0.0f },
				{ 0.0f, 1.0f, 0.0f }, 55.0f, 25.0f, 220.0f * strength);
			// The pose the balance controller offsets from. Stored rather than
			// assumed to be the identity, so the rig could be built in a
			// crouch and the controller would still lean from there.
			m_HipRest[side] = m_World.GetJoint(m_HipJoint[side]).MotorTargetRotation;

			// The thigh body's turn within the frame SolveLegPole builds
			// (+y up the bone, +z forward) -- identity for a leg built
			// straight down, kept exact for one that is not.
			{
				glm::vec3 y = glm::normalize(glm::vec3(x, 1.00f * s, 0.0f) - m_KneeAt[side]);
				glm::vec3 z = glm::vec3(0.0f, 0.0f, 1.0f) - y * y.z;
				z = glm::normalize(z);
				glm::quat frame = glm::normalize(glm::quat_cast(glm::mat3(glm::cross(y, z), y, z)));
				m_ThighRestLocal[side] = glm::normalize(glm::conjugate(frame) * m_World.GetBody(m_UpperLeg[side]).Orientation);
			}

			// TRY: widen the knee's range past 130 degrees and watch the shin
			// pass through the thigh -- the limit is the only thing stopping
			// it, since jointed bodies do not collide with each other.
			m_KneeJoint[side] = hinge(m_UpperLeg[side], m_LowerLeg[side], m_KneeAt[side],
				// Positive, not negative. The figure faces +z -- its toes stick
				// out that way -- and a positive rotation about +x sends the
				// heel backwards, which is what a knee does. Built the other
				// way round it could *only* hyperextend, and the walk looked
				// like it was bending its legs the wrong way because it was.
				{ 1.0f, 0.0f, 0.0f }, 0.0f, 130.0f, 160.0f * strength);

			// The ankle is a ball joint on a short leash, not a hinge.
			//
			// It was a hinge, and a hinge has one axis: pitch. That left the
			// figure with nothing at all to push sideways with, so the ankle
			// strategy held it forward and back and it fell over sideways
			// every time. A tight cone gives roll as well, which is the axis
			// lateral balance actually needs, and the twist range keeps the
			// foot from spinning under the leg.
			m_AnkleJoint[side] = ball(m_LowerLeg[side], m_Foot[side], m_AnkleAt[side],
				{ 0.0f, 1.0f, 0.0f }, 35.0f, 15.0f, 140.0f * strength);
			m_AnkleRest[side] = m_World.GetJoint(m_AnkleJoint[side]).MotorTargetRotation;

			// Shoulder and elbow. Kept now, rather than created and
			// forgotten -- the walk drives them.
			m_ShoulderJoint[side] = ball(m_Torso, m_UpperArm[side],
				{ shoulderSide, 1.55f * s, 0.0f }, { 0.0f, 1.0f, 0.0f }, 75.0f, 45.0f, 60.0f * strength);
			m_ShoulderRest[side] = m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation;

			m_Rest.Hip[side] = { x, 1.00f * s, 0.0f };
			m_Rest.Knee[side] = m_KneeAt[side];
			m_Rest.Ankle[side] = m_AnkleAt[side];
			m_Rest.Shoulder[side] = { shoulderSide, 1.55f * s, 0.0f };
			m_Rest.Elbow[side] = { armX, 1.20f * s, 0.0f };
			m_ElbowJoint[side] = hinge(m_UpperArm[side], m_LowerArm[side],
				{ armX, 1.20f * s, 0.0f },
				// And the elbow is the knee's mirror: the forearm swings
				// forwards, so its range is the negative one.
				{ 1.0f, 0.0f, 0.0f }, -140.0f, 0.0f, 35.0f * strength);
		}

		// The rig is built facing +z, so the heading has to say so. It did not:
		// m_Facing is only ever written when getting up, and survived a
		// rebuild. Reset after walking a while and the new rig stood facing +z
		// while the gait placed its feet for wherever he had been going --
		// left and right swapped, the legs crossed, and the knee wandered
		// between 4 and 31 degrees instead of sitting still at 5.
		m_Facing = 0.0f;
		m_RunBlend = 0.0f;
		m_Duty = m_WalkDuty;
		m_Carry = 0.0f;

		SetMotorsEnabled(m_MotorsEnabled);

		// Remembered so going limp and back is reversible -- each joint has its
		// own budget and one number could not restore them.
		m_JointTorque.clear();
		for (auto handle : m_Joints)
			m_JointTorque[handle] = m_World.GetJoint(handle).MotorMaxTorque;

		{
			// Measured from the pose the rig was built in, so changing the
			// skeleton's proportions does not leave the IK solving for a leg
			// that no longer exists.
			const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
			for (int side = 0; side < 2; side++)
			{
				glm::vec3 hipAt(side == 0 ? -hipX : hipX, 1.00f * s, 0.0f);
				hipAt += origin;

				m_HipLocal[side] = glm::conjugate(pelvis.Orientation) * (hipAt - pelvis.Position);

				// Same for the arm, measured off the pose it was built in, so
				// the get-up's hand IK solves for the arm that exists.
				const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
				glm::vec3 shoulderAt(side == 0 ? -P.ArmX : P.ArmX, 1.55f * s, 0.0f);
				glm::vec3 elbowAt(side == 0 ? -P.ArmX : P.ArmX, 1.20f * s, 0.0f);
				glm::vec3 wristAt(side == 0 ? -P.ArmX : P.ArmX, 0.88f * s, 0.0f);
				shoulderAt += origin; elbowAt += origin; wristAt += origin;

				m_ShoulderLocal[side] = glm::conjugate(torso.Orientation)
					* (shoulderAt - torso.Position);
				m_UpperArmLength = glm::length(shoulderAt - elbowAt);
				m_ForearmLength = glm::length(elbowAt - wristAt);
				// `m_KneeAt` and `m_AnkleAt` are in the **rig's own frame** --
				// the joint lambdas above add `origin` on the way in, so that is
				// the convention they are stored in. Everything from here down
				// is world, so they have to be lifted into it.
				//
				// They were not, and it cost an afternoon. With the rig built at
				// the world origin the two frames are the same numbers and
				// nothing is wrong; build it anywhere else -- on a hill, say --
				// and the thigh measures the distance from the hip to a point
				// near the map's centre, the foot targets are computed against
				// ground on the far side of the map, and the figure stands there
				// with its legs reaching for somewhere it is not. It read as a
				// terrain bug and was a coordinate bug that terrain revealed.
				glm::vec3 kneeWorld = origin + m_KneeAt[side];
				glm::vec3 ankleWorld = origin + m_AnkleAt[side];

				m_ThighLength = glm::length(hipAt - kneeWorld);
				m_ShinLength = glm::length(kneeWorld - ankleWorld);

				// Both feet start planted where they were built. Heights are
				// from the rig's own soles (origin.y), not from whatever ground
				// lies below: built in mid-air -- a Player placed above the
				// floor in the editor, the demo's drop height -- the old
				// measurement made him 5 m tall, and he stood on the air at
				// that height for good.
				m_AnkleHeight = ankleWorld.y - origin.y;
				m_Planted[side] = ankleWorld;
				m_SwingFrom[side] = m_Planted[side];
			}
		}

		m_CharacterBodies = { m_Pelvis, m_Torso, m_Head,
			m_UpperLeg[0], m_UpperLeg[1], m_LowerLeg[0], m_LowerLeg[1],
			m_Foot[0], m_Foot[1],
			m_UpperArm[0], m_UpperArm[1], m_LowerArm[0], m_LowerArm[1] };

		// What the impulse model divides by -- summed once the list exists.
		// It used to be summed just above, before the list was filled, so a
		// figure's first build had a mass of zero and Stagger, which divides
		// by it, quietly did nothing: in a game, where the rig is built once,
		// no hit ever staggered him. So did the Ragdoll demo until its first
		// Reset; Map Building builds twice at startup, and the second build
		// summed the first one's list, which is how it went unseen.
		m_BodyMass = 0.0f;
		for (unsigned int body : m_CharacterBodies)
			m_BodyMass += m_World.GetBody(body).GetMass();

		// The build pose, relative to the feet, for Respawn.
		m_BuildPose.clear();
		for (unsigned int body : m_CharacterBodies)
			m_BuildPose.push_back({ m_World.GetBody(body).Position - origin, m_World.GetBody(body).Orientation });

		m_StandHeight = m_World.GetBody(m_Pelvis).Position.y;
		// How far the pelvis rides above whatever it stands on, taken from the
		// pose the rig was built in rather than written down twice.
		m_StandClearance = m_StandHeight - origin.y;
		m_RootAnchor = m_World.GetBody(m_Pelvis).Position;
		m_Speed = 0.0f;
		m_SpeedRate = 0.0f;
		m_GaitPhase = 0.0f;
		m_StandComHeight = CentreOfMass().y - origin.y;
		GoControlled();
	}

	void SetMotorsEnabled(bool enabled)
	{
		m_MotorsEnabled = enabled;
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorEnabled = enabled;
			joint.MotorStiffness = m_MotorStiffness;
		}

		ApplyLegStrength();
	}

	// The legs are driven harder than the rest of the body.
	//
	// They are tracking an IK target that moves every frame, and a stance foot
	// that does not keep up with it slides -- which is the whole thing the
	// placement was for. Swept: at the body's own stiffness the foot slid
	// 0.0218 m a frame against 0.0267 for a foot simply dragged along, so the
	// placement was buying almost nothing. At stiffness 40 with four times the
	// torque it slides **0.0107**. Stiffer is worse, not better: 90 oscillates
	// and the slide goes back up to 0.038.
	void ApplyLegStrength()
	{
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_HipJoint[side], m_KneeJoint[side], m_AnkleJoint[side] })
			{
				GS::Joint3D& joint = m_World.GetJoint(handle);
				// The ankle is stiffer than the rest of the leg. It carries a
				// 1 kg foot rather than the body's weight, and it is chasing a
				// target that moves with the shin every frame -- a motor is a
				// velocity constraint, so its steady-state error is roughly the
				// target's speed over its stiffness, and at the leg's 40 that
				// left the sole 3 deg off level all through the stance.
				joint.MotorStiffness = handle == m_AnkleJoint[side]
					? m_AnkleStiffness : m_LegStiffness;
				joint.MotorMaxTorque = m_JointTorque.count(handle)
					? m_JointTorque[handle] * m_LegTorqueScale : joint.MotorMaxTorque;
				// The IK target can move quickly; the default cap throttles it.
				joint.MotorMaxSpeed = 30.0f;
			}
		}
	}

	// ---------------------------------------------------------------------
	// Simulation
	// ---------------------------------------------------------------------
	// The shortest rotation taking `from` to `to`, both unit length.
	//
	// Written out rather than pulled from glm's experimental extensions, which
	// need a define before the include and are not worth dragging into a demo
	// header for four lines.
	static glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
	{
		float dot = glm::dot(from, to);

		// Opposite directions: no unique shortest arc, so any perpendicular
		// axis will do. Picking one deliberately beats letting the normalise
		// below divide by zero.
		if (dot < -0.9999f)
		{
			glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
			if (glm::length(axis) < 1e-4f)
				axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), from);

			return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
		}

		glm::vec3 axis = glm::cross(from, to);
		return glm::normalize(glm::quat(1.0f + dot, axis.x, axis.y, axis.z));
	}

	// The capture point, pulled in to something a leg can actually reach.
	glm::vec2 ClampedStepTarget(const glm::vec2& capturePoint) const
	{
		glm::vec2 hip(m_World.GetBody(m_Pelvis).Position.x,
			m_World.GetBody(m_Pelvis).Position.z);

		glm::vec2 fromHip = capturePoint - hip;
		float reach = glm::length(fromHip);

		return reach > m_MaxStep ? hip + fromHip * (m_MaxStep / reach) : capturePoint;
	}

	// Stepping: the only thing that recovers a fall the ankles cannot -- and
	// it does not work here, for a reason now pinned to one number.
	//
	// The arithmetic says the rig is fine. Linear inverted pendulum, all
	// figures read off this rig: 71.2 kg, centre of mass 1.085 m,
	// tau = sqrt(h/g) = 0.3325 s, foot half-length 0.110 m. A foot placed
	// `reach` beyond the centre of mass catches a fall at (reach + footHalf)/tau,
	// and an impulse J at the chest gives v = J/m, so:
	//
	//     ankles only              0.000 m   0.33 m/s    23.6 Ns
	//     the step actually made   0.183 m   0.88 m/s    62.7 Ns
	//     the step geometry allows 0.737 m   2.55 m/s   181.4 Ns
	//
	// The first line is confirmed by experiment -- 20 Ns survives half the
	// time and 40 Ns never does, bracketing 23.6 exactly. So the model is
	// right, and by it even the modest step already achieved should nearly
	// **triple** the impulse the figure can take.
	//
	// It does not, and here is where it goes:
	//
	//     support reach towards the fall: 0.0019 m before the step,
	//                                     0.0000 m after the foot lands
	//
	// **The base does not get any bigger.** The foot travels 0.183 m and the
	// polygon it stands on gains nothing in the direction it is falling.
	//
	// Chased down, along the fall direction over twelve steps:
	//
	//     the foot moved           -0.069 m   (backwards!)
	//     the centre of mass moved +0.332 m
	//     foot behind the com:     -0.18 m -> -0.58 m
	//
	// The foot is not aimed wrongly -- its distance to the target shrinks
	// steadily through the swing, 1.243 m to 0.988 m. The trouble is that the
	// target is 1.24 m away to begin with, because by the time the swing runs
	// the pelvis has already travelled **0.724 m horizontally past the foot**.
	// The leg closes a quarter of a metre; the body covers a third of a metre
	// in the same time. The foot chases a target bolted to a pelvis that is
	// outrunning it, and finishes further behind than it started.
	//
	// The hip is not at its cone limit while this happens (0.856 of 0.960 rad),
	// the motor is not saturated, and the stance leg is fine. **The step is not
	// too weak, it is too late** -- not in the sense the trigger fires late,
	// which was measured and is false, but in the sense that a step which takes
	// 0.16 s cannot catch a body already moving faster than a leg can swing. Three controllers were tried and all three failed
	// the same way, for a reason that is mechanical rather than about timing.
	//
	// **The foot never leaves the ground.** Measured: asked to move 0.231 m,
	// it moved 0.021 m. The swing foot is still carrying weight, and a loaded
	// foot cannot be lifted however hard the hip pulls. Unloading it means
	// shifting the weight laterally onto the other foot, which is rate limited
	// to about sqrt(h/g) = 0.33 s -- and a push that needs recovering is over
	// in about a second.
	//
	// A backwards shove makes it worse, because it loads *both* feet equally:
	// the body rotates towards its heels rather than onto one side, so neither
	// foot frees itself. A person stepping backwards from a shove unweights one
	// side first, and that lateral move is the part there is no time for.
	//
	// Kept, switchable, and off. What is here is correct as far as it goes:
	// the trigger fires when the capture point leaves the feet and not during
	// sway, the foot choice and the target are right. What is missing is a way
	// to unload a foot quickly, and that is a rig and controller question
	// rather than a tuning one.
	//
	// Once the capture point is outside the feet, no ankle torque brings it
	// back -- an ankle can only move where the weight bears *within* the base.
	// The recovery is to move the base: put a foot where the capture point
	// went, and the figure arrives over its feet again instead of past them.
	//
	// The step itself is a short scripted swing rather than anything clever:
	// lift the foot by bending the knee, point the leg at the target, then
	// straighten and plant. Three quarters of a second of open loop. What makes
	// it work is not the trajectory but *where* it aims, which is the capture
	// point and not the place the body currently is.
	void UpdateStepping(const BalanceState& state, float dt)
	{
		// Waiting for a trough: the step is decided, the foot is not yet moving.
		//
		// The swing foot carries about half the body's weight on average, and
		// no hip motor lifts that. But the standing pose bounces -- each foot
		// passes under 150 N for roughly a ninth of the time, in windows of
		// about a tenth of a second -- so rather than trying to *create* an
		// unload, which is rate limited to sqrt(h/g) and far too slow, this
		// waits for one that is already coming and lifts into it.
		if (m_WaitingForTrough)
		{
			m_WaitTimer += dt;

			// The target keeps moving while we wait, so it is re-aimed rather
			// than fired at where the capture point used to be.
			m_StepTarget = ClampedStepTarget(state.CapturePoint);

			float load = FootLoad(m_SwingSide, dt);

			// Where the load will be a moment from now, not where it is.
			//
			// Waiting for the load to actually cross the gate spends the front
			// of the trough getting the lift started, and the trough is only
			// about 0.10 s long against a swing that needs longer. Committing
			// on the extrapolation centres the free window on the lift instead
			// of starting it there. The rate has to be negative as well as
			// large -- a load climbing through the gate is the *end* of a
			// trough, and lifting into that is lifting into a rising floor.
			float rate = (load - m_PreviousSwingLoad) / dt;
			m_PreviousSwingLoad = load;

			float predicted = load + rate * m_LeadTime;
			bool arriving = rate < 0.0f && predicted < m_LoadGate;

			if (load < m_LoadGate || arriving)
			{
				m_WaitingForTrough = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
			}
			else if (m_WaitTimer >= m_MaxWait)
			{
				// No trough arrived in time. Going anyway is worse than not
				// going -- it is the loaded lift that never worked -- so the
				// step is abandoned and the ankles carry on.
				m_WaitingForTrough = false;
				m_StepCooldown = m_StepCooldownTime;
			}
			return;
		}

		// Shifting: both feet still down, weight moving over the stance foot.
		//
		// This phase is the whole difference between a step that helps and one
		// that does not. Lifting a foot from a body whose weight is still
		// spread across both throws the pelvis, because the support under half
		// of it simply stops existing. A person leans first; so does this now.
		if (m_Shifting)
		{
			// A reactive step does not get a shift. Measured: the shift needs
			// sqrt(h/g), about 0.33 s, and the swing another third -- against a
			// fall that is over in a second. Deliberate steps can afford it;
			// a recovery cannot, and going without means accepting a moment of
			// single support, which is what a person catching themselves does.
			if (m_FastStep)
			{
				m_Shifting = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
				return;
			}

			m_ShiftTimer += dt;

			// Done when the weight is over the stance foot, or when it has had
			// long enough. The timeout matters: waiting for a condition that
			// may never arrive is how a balance controller freezes mid-fall.
			const GS::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
			float remaining = std::fabs(state.Com.x - stanceFoot.Position.x);

			if (remaining < m_ShiftTolerance || m_ShiftTimer >= m_ShiftDuration)
			{
				m_Shifting = false;
				m_Stepping = true;
				m_StepTimer = 0.0f;
			}
			return;
		}

		if (m_Stepping)
		{
			m_StepTimer += dt;
			if (m_StepTimer >= m_StepDuration)
			{
				m_Stepping = false;
				m_StepsTaken++;

				// The stepped leg holds where it landed instead of returning to
				// the pose it started in.
				//
				// This was the single worst bug in the step: resetting the hip
				// to its rest target swung the leg straight back under the body
				// the instant the foot touched down, undoing the step that had
				// just been taken. The figure would place a foot correctly and
				// then pull it out from under itself.
				const GS::RigidBody3D& thigh = m_World.GetBody(m_UpperLeg[m_SwingSide]);
				const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
				m_HipRest[m_SwingSide] = glm::normalize(
					glm::conjugate(pelvis.Orientation) * thigh.Orientation);

				m_World.GetJoint(m_HipJoint[m_SwingSide]).MotorTargetRotation = m_HipRest[m_SwingSide];
				m_World.GetJoint(m_KneeJoint[m_SwingSide]).MotorTargetAngle = 0.0f;
				m_StepCooldown = m_StepCooldownTime;
			}
			return;
		}

		// Three points or it is not a polygon, and the margin is a placeholder
		// rather than a measurement.
		//
		// This mattered more than it looks: a foot in the air leaves one foot
		// on the ground, which often gives fewer than three contact points, so
		// the margin read -1 and triggered *another* step the moment the last
		// one finished. The figure stepped continuously and fell over its own
		// feet -- 236 steps in fifteen seconds.
		if (!m_SteppingEnabled || state.Support.size() < 3)
			return;

		// Not while the last step is still settling. Without this the figure
		// steps, loses half its support to the lifted foot, reads the smaller
		// polygon as a worse margin, and steps again -- a loop that walks it
		// over in about a second.
		if (m_StepCooldown > 0.0f)
		{
			m_StepCooldown -= dt;
			return;
		}

		// Only when the capture point is genuinely out, and only when it stays
		// out.
		//
		// Two conditions rather than one, because they reject different
		// things. The threshold ignores small excursions; the persistence
		// count ignores brief ones. Ordinary sway produces both -- the capture
		// point dips over an edge for a frame or two and comes back -- and a
		// step taken in answer costs a foot's support for a third of a second
		// to fix something that had already fixed itself.
		if (state.Margin > -m_StepTrigger)
		{
			m_OutsideFor = 0;
			return;
		}

		m_OutsideFor++;
		if (m_OutsideFor < m_StepPersist)
			return;

		glm::vec2 away = state.CapturePoint - state.SupportCentre;
		if (glm::length(away) < 1e-4f)
			return;

		// Step with the foot on the side it is falling towards. Stepping the
		// other one crosses the legs and makes things worse -- and it is what
		// a person does too: you catch yourself on the foot nearest the fall.
		m_SwingSide = away.x > 0.0f ? 1 : 0;

		// Aim at the capture point, but no further than a leg can reach.
		glm::vec2 target = ClampedStepTarget(state.CapturePoint);

		// A step that lands where the foot already is buys nothing and costs
		// the support of a raised foot for a third of a second. If the capture
		// point is that close, the ankles can have it.
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[m_SwingSide]);
		glm::vec2 footNow(foot.Position.x, foot.Position.z);

		if (glm::length(target - footNow) < m_MinStep)
			return;

		m_StepTarget = target;

		if (m_GateOnLoad)
		{
			m_WaitingForTrough = true;
			m_WaitTimer = 0.0f;
			// Seeded, so the first frame's rate is zero rather than the
			// difference against whatever was left from the last step.
			m_PreviousSwingLoad = FootLoad(m_SwingSide, dt);
		}
		else
		{
			m_Shifting = true;
			m_ShiftTimer = 0.0f;
		}
	}

	// Drives the swinging leg. Called every step while a step is in progress.
	void DriveSwingLeg()
	{
		if (!m_Stepping)
			return;

		float t = glm::clamp(m_StepTimer / m_StepDuration, 0.0f, 1.0f);

		// Bend early to clear, straighten late to reach.
		//
		// A plain sin(pi*t) bends and straightens symmetrically, peaking
		// halfway. That spends the second half of the swing -- the half that
		// is supposed to be reaching for the target -- with the leg still
		// substantially folded, and a folded leg converts hip rotation into far
		// less foot travel. Moving the peak earlier clears the ground on the
		// same schedule and leaves the leg extended for longer while it reaches.
		//
		// The phase is warped rather than the curve replaced, so the profile
		// still starts and ends at zero bend and still peaks at exactly
		// m_StepLift -- only *when* moves.
		float peak = glm::clamp(m_LiftPeak, 0.05f, 0.95f);
		float phase = t < peak
			? 0.5f * (t / peak)
			: 0.5f + 0.5f * (t - peak) / (1.0f - peak);

		float lift = std::sin(phase * glm::pi<float>());
		m_World.GetJoint(m_KneeJoint[m_SwingSide]).MotorTargetAngle = m_StepLift * lift;

		// Point the whole leg at the target. The leg hangs along -y at rest,
		// so the hip rotation is whatever takes -y to the direction from the
		// hip to where the foot should land.
		glm::vec3 hip = m_World.GetBody(m_Pelvis).Position;
		hip.y -= 0.09f;   // roughly the hip joint, below the pelvis centre

		glm::vec3 target(m_StepTarget.x, 0.0f, m_StepTarget.y);
		glm::vec3 toTarget = target - hip;

		if (glm::length(toTarget) < 1e-4f)
			return;

		// Into the pelvis's frame before building the rotation.
		//
		// MotorTargetRotation is the thigh relative to the *pelvis*, and
		// `toTarget` is a world direction. Handing one to the other works only
		// while the pelvis happens to be upright -- and a figure that needs to
		// step is a figure whose pelvis is tilting. Measured with the world
		// vector, the foot moved 0.073 m *backwards* along the fall direction
		// while the body moved 0.329 m forwards.
		glm::vec3 localToTarget = glm::conjugate(m_World.GetBody(m_Pelvis).Orientation)
			* glm::normalize(toTarget);

		glm::quat aim = RotationBetween(glm::vec3(0.0f, -1.0f, 0.0f), localToTarget);

		// Eased in, so the leg swings rather than snaps to the target on the
		// first frame and throws the whole body with it -- but a reactive step
		// has no time for a gentle onset, so it reaches full aim by a third of
		// the way through instead of two thirds.
		float blend = glm::smoothstep(0.0f, m_FastStep ? 0.3f : 0.6f, t);
		glm::quat rest = m_HipRest[m_SwingSide];

		m_World.GetJoint(m_HipJoint[m_SwingSide]).MotorTargetRotation =
			glm::normalize(glm::slerp(rest, aim * rest, blend));
	}

	// The ankle-and-hip strategy, which is what people use for small
	// disturbances before they resort to stepping.
	//
	// Both are just a nudge to a motor target, not a torque applied from
	// nowhere: the ankles pitch to shift where the weight bears, and the hips
	// roll the body sideways over the supporting foot. Nothing here can save a
	// figure whose capture point has already left its feet -- that needs a
	// step, and stepping is the next piece.
	void ApplyBalance(const BalanceState& state)
	{
		if (!m_BalanceEnabled || !m_MotorsEnabled || state.Support.size() < 3)
			return;

		// How far the capture point is from the middle of the feet, which is
		// the error the controller is trying to null.
		glm::vec2 error = state.CapturePoint - state.SupportCentre;

		// Both axes at the ankle now. Pitch handles forward and back; roll is
		// what the hinge could not do and what sent every earlier attempt over
		// sideways.
		//
		// The signs were settled by experiment rather than by reasoning about
		// which way a foot bends -- see the changelog. Reasoning about it was
		// how the first three attempts went wrong.
		float pitch = glm::clamp( error.y * m_AnkleGain,     -0.45f, 0.45f);
		float roll  = glm::clamp(-error.x * m_AnkleRollGain, -0.45f, 0.45f);

		// While shifting, add a commanded lean towards the stance foot on top
		// of the correction. Same mechanism the lateral strategy uses -- a
		// positive roll pushes the body towards +x -- but driven by where the
		// weight needs to go rather than by where the capture point is.
		if (m_Shifting)
		{
			const GS::RigidBody3D& stanceFoot = m_World.GetBody(m_Foot[1 - m_SwingSide]);
			float towards = stanceFoot.Position.x - state.Com.x;
			roll = glm::clamp(roll + m_ShiftRoll * glm::sign(towards), -0.55f, 0.55f);
		}

		glm::quat correction = glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f));

		for (int side = 0; side < 2; side++)
		{
			// A foot in the air is not holding anything up, and correcting it
			// only wastes the swing. During the shift both feet are still
			// down, so both still get corrected.
			if (m_Stepping && side == m_SwingSide)
				continue;

			GS::Joint3D& joint = m_World.GetJoint(m_AnkleJoint[side]);
			joint.MotorTargetRotation = glm::normalize(correction * m_AnkleRest[side]);
		}
	}

	// ---------------------------------------------------------------------
	// The two modes
	// ---------------------------------------------------------------------
	//
	// This is how games do it, and it is not balance.
	//
	// **Controlled.** The pelvis is *kinematic*: infinite mass, driven
	// directly by input, immovable by any contact or joint. The character
	// cannot fall over because the thing holding it up is not simulated. The
	// rest of the skeleton is still fully dynamic and hangs off it, so the
	// limbs swing, collide and react -- which is what sells it as physical
	// rather than as an animation.
	//
	// **Ragdoll.** The pelvis becomes dynamic, inheriting the velocity it had,
	// and the motors go slack. Nothing else changes: the same bodies, the same
	// joints, the same limits. Physics simply takes over.
	//
	// The switch is the whole trick. Balancing a figure on its own skeleton is
	// the hard research problem this demo spent a long time failing at; not
	// having to is what makes a playable character tractable.
	enum class Mode { Controlled, Ragdoll, GettingUp, Stumbling, Moving };

	// `brace`: reach for the ground with the arms whatever he was doing (as a
	// lost stumble always does) -- stepping off an edge, he knows he is
	// falling.
	void GoRagdoll(const char* why, bool brace = false)
	{
		if (m_Mode == Mode::Ragdoll)
			return;
		LocoRelease();

		// A stumble that is lost goes down with its arms out: falling from
		// one, he knows which way he is going.
		bool braced = m_Bracing && (m_Mode == Mode::Stumbling || brace);

		UnpinFeet();
		m_Mode = Mode::Ragdoll;
		m_RagdollReason = why;
		m_RagdollTimer = 0.0f;

		HipsHeavy(false);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Dynamic;

		// It keeps the velocity it was being driven at, so a character shoved
		// while running tumbles forward rather than dropping on the spot.
		// Everything else is already dynamic and already moving.
		pelvis.Awake = true;
		pelvis.SleepTimer = 0.0f;

		// Limp, not rigid. A corpse that holds its pose reads as a mannequin
		// being dropped -- some residual stiffness keeps the joints from
		// looking like string, which is roughly what muscle tone does.
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_LimpStiffness;
			joint.MotorMaxTorque = m_LimpTorque;
		}

		m_BraceTimer = 0.0f;
		if (braced)
		{
			glm::vec3 velocity = CentreOfMassVelocity();
			glm::vec2 flat(velocity.x, velocity.z);
			if (glm::length(flat) > 0.05f)
			{
				m_BraceTimer = m_BraceTime;
				m_BraceDirection = glm::vec3(flat.x, 0.0f, flat.y) / glm::length(flat);
			}
		}
	}

	// Bracing: Euphoria's "catch fall". For a moment after a lost stumble
	// the arms keep their strength and reach for the ground where he is
	// going -- a hand out ahead and below the shoulder -- so the hands land
	// before the head does and take some of the fall. The rest of him is
	// as limp as any ragdoll.
	void UpdateBrace(float dt)
	{
		if (m_BraceTimer <= 0.0f)
			return;
		m_BraceTimer -= dt;
		bool holding = m_BraceTimer > 0.0f;
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_ShoulderJoint[side], m_ElbowJoint[side] })
			{
				GS::Joint3D& joint = m_World.GetJoint(handle);
				joint.MotorStiffness = holding ? m_MotorStiffness : m_LimpStiffness;
				joint.MotorMaxTorque = holding ? m_JointTorque[handle] : m_LimpTorque;
			}
			if (!holding)
				continue;
			glm::vec3 reach = glm::normalize(m_BraceDirection * m_BraceForward + glm::vec3(0.0f, -1.0f, 0.0f));
			SolveArm(side, ShoulderWorld(side) + reach * (m_UpperArmLength + m_ForearmLength) * 0.95f);
		}
	}

	glm::vec3 CentreOfMassVelocity() const
	{
		glm::vec3 momentum(0.0f);
		float total = 0.0f;
		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			momentum += body.Velocity * body.GetMass();
			total += body.GetMass();
		}
		return total > 0.0f ? momentum / total : glm::vec3(0.0f);
	}

	// Straight to standing, with no blend. Used when the scene is built and
	// when a blend finishes.
	void GoControlled()
	{
		m_Mode = Mode::Controlled;
		// Whatever had him -- a move, a stumble, a get-up, a respawn -- the
		// locomotion picks him up from where his bodies are now.
		m_LocoEntering = true;
		m_RagdollTimer = 0.0f;
		m_GetUpTimer = 0.0f;

		HipsHeavy(false);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);


		// The character stands up where it is lying, not where it fell from.
		m_RootAnchor = pelvis.Position;
		m_RootAnchor.y = m_StandHeight;
		m_Speed = 0.0f;
		m_SpeedRate = 0.0f;
		m_GaitPhase = 0.0f;

		// Whatever knocked him over does not follow him back up. Left behind,
		// the stagger that felled him is still there the instant he stands and
		// fells him again -- and it survived a scene rebuild too, which is how
		// it was found: every case after the first fall reported a fall.
		m_StaggerVelocity = glm::vec3(0.0f);
		m_StaggerTimer = 0.0f;
		m_JumpPending = false;
		m_Air = Air::Grounded;

		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = m_MotorStiffness;
			joint.MotorMaxTorque = m_JointTorque[handle];
		}

		ApplyLegStrength();
	}

	// Getting up: a blend rather than a snap.
	//
	// The pelvis becomes kinematic immediately -- it has to be drivable to be
	// blended -- and is then carried from wherever it came to rest back to
	// standing, over about a second. The limbs are not animated at all: the
	// motors come back up from limp over the same period and drag them into
	// the pose, which is why the arms trail and settle rather than snapping
	// into place. It is the cheap version of what games do by blending into a
	// get-up animation, and most of what sells it is simply that it takes time.
	void BeginGetUp()
	{
		if (m_Mode == Mode::GettingUp)
			return;
		LocoRelease();

		// Only from the ground. Asked in mid-air -- G pressed while falling
		// off the demo's platform -- it used to hoist the body to standing
		// height over nothing. Asked under his lowest body, which is the one
		// lying on whatever he is lying on, ignoring all of his own.
		{
			GS::PhysicsWorld3D::BodyHandle lowest = m_Pelvis;
			for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
				if (m_World.GetBody(handle).Position.y < m_World.GetBody(lowest).Position.y)
					lowest = handle;
			const GS::RigidBody3D& body = m_World.GetBody(lowest);
			float below = body.Position.y - GroundY(body.Position, -1e9f);
			if (below > m_GetUpMaxGap)
				return;
		}

		UnpinFeet();
		m_Mode = Mode::GettingUp;
		m_GetUpTimer = 0.0f;

		if (m_NaturalGetUp)
		{
			PlanGetUp();
			return;
		}

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		m_GetUpFrom = pelvis.Position;
		m_GetUpFromOrientation = pelvis.Orientation;

		// Stand on whatever is underneath, not at the height of the floor it
		// started on -- otherwise getting up on a step sinks into it.
		m_GetUpGround = GroundY(pelvis.Position);
		m_GetUpTo = glm::vec3(pelvis.Position.x,
			m_GetUpGround + m_StandClearance, pelvis.Position.z);

		// Face down or face up. Which one decides whether he comes up over his
		// hands or has to roll first, and they do not look the same at all.
		// Which way the *front* of him is pointing. The first version of this
		// asked the pelvis's local +y, which runs up the body towards the head
		// -- for a figure lying flat that is horizontal, so its sign was noise
		// and the branch it chose was a coin toss. The figure faces local +z.
		// Face down is -1, face up is +1, and near zero is lying on a side,
		// where a roll is neither needed nor wanted.
		glm::vec3 front = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		m_GetUpSupine = front.y > 0.5f;

		// Keep whatever heading it ended up facing, flattened. Standing up
		// facing the direction you fell reads as a stumble recovered; snapping
		// back to the old heading reads as a rewind.
		glm::vec3 facing = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		if (glm::length(glm::vec2(facing.x, facing.z)) > 0.01f)
			m_Facing = std::atan2(facing.x, facing.z);

		// Where the hands will be planted: ahead of the chest, on the ground,
		// and fixed in the world so the body can push against them instead of
		// carrying them along with it.
		glm::vec3 ahead(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 across(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		for (int side = 0; side < 2; side++)
		{
			m_HandSpot[side] = glm::vec3(pelvis.Position.x, 0.0f, pelvis.Position.z)
				+ ahead * m_HandForward
				+ across * (side == 0 ? -m_HandWidth : m_HandWidth);
			m_HandSpot[side].y = m_GetUpGround + m_HandHeight;
		}

		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);
	}

	// Getting up in two stages, because one was the problem.
	//
	// A single lerp from where the body lies to a standing pose drags the
	// whole figure through the air in the shape it fell in -- the feet arrive
	// under the hips at the same instant the hips arrive at standing height,
	// which is not getting up, it is being placed. A person gathers first and
	// rises second, and the rise happens *over* feet that are already planted.
	//
	// So: gather, then rise. The pelvis comes up only as far as kneeling while
	// the feet are collected underneath it, and only then straightens.
	void UpdateGetUp(float dt)
	{
		if (m_NaturalGetUp)
		{
			UpdateNaturalGetUp(dt);
			return;
		}
		m_GetUpTimer += dt;

		float t = glm::clamp(m_GetUpTimer / m_GetUpTime, 0.0f, 1.0f);

		float onHands = glm::smoothstep(0.0f, 1.0f,
			glm::clamp(t / m_GetUpHands, 0.0f, 1.0f));
		float gather = glm::smoothstep(0.0f, 1.0f,
			glm::clamp((t - m_GetUpHands) / std::max(m_GetUpGather - m_GetUpHands, 0.01f),
				0.0f, 1.0f));
		float rise = glm::smoothstep(0.0f, 1.0f,
			glm::clamp((t - m_GetUpGather) / std::max(1.0f - m_GetUpGather, 0.01f),
				0.0f, 1.0f));

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);

		// Three waypoints, not two. Up onto the hands first, then back over
		// the feet as they gather, and only then up.
		//
		// A person getting off the floor does not rise straight from where
		// they are lying -- there is nothing under them there. They put their
		// hands down, push their weight up and forward onto them, walk their
		// feet in underneath, and stand up over the feet. Lerping the pelvis
		// straight from lying to standing skips every part of that, which is
		// why it read as being placed rather than getting up.
		glm::vec3 ahead(std::sin(m_Facing), 0.0f, std::cos(m_Facing));

		glm::vec3 overHands = m_GetUpFrom + ahead * m_GetUpShift;
		overHands.y = m_GetUpGround + m_PushUpHeight;

		glm::vec3 overFeet(m_GetUpTo.x, m_GetUpGround + m_KneelHeight, m_GetUpTo.z);

		glm::vec3 target = glm::mix(m_GetUpFrom, overHands, onHands);
		target = glm::mix(target, overFeet, gather);
		target = glm::mix(target, m_GetUpTo, rise);

		pelvis.Velocity = (target - pelvis.Position) / std::max(dt, 1e-4f);

		// Upright by the end of the gather, not the end of the whole move, so
		// the rise happens from a pose that is already the right way up.
		glm::quat upright = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));

		// Face up, he has to get over onto a side before any of that helps.
		// Rolling the long way round through the back is what makes a supine
		// get-up read differently from a prone one.
		if (m_GetUpSupine)
		{
			float roll = std::sin(glm::clamp(t / m_GetUpHands, 0.0f, 1.0f)
				* glm::pi<float>()) * m_GetUpRoll;
			upright = glm::normalize(upright
				* glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f)));
		}

		pelvis.Orientation = glm::slerp(m_GetUpFromOrientation, upright,
			glm::max(onHands, gather));
		pelvis.UpdateInertiaWorld();

		// Strength returns over the gather, so the legs can be steered before
		// they are asked to carry anything.
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			// Twice the rate of the gather, so the legs have authority while
			// there is still time left to gather with. Ramped over the whole
			// gather instead, strength arrived just as the gather ended and
			// the feet were still 0.49 m from under the hips at the rise.
			// Back before the hands are asked to carry anything, which is
			// earlier than it used to need to be.
			float strength = glm::clamp(onHands * 2.0f, 0.0f, 1.0f);
			joint.MotorStiffness = glm::mix(m_LimpStiffness, m_MotorStiffness, strength);
			joint.MotorMaxTorque = glm::mix(m_LimpTorque, m_JointTorque[handle], strength);
			joint.MotorTargetAngle = 0.0f;
		}

		// The hands are planted while the weight comes over them, and let go
		// once the feet have taken it.
		if (t < m_GetUpGather)
		{
			for (int side = 0; side < 2; side++)
				SolveArm(side, m_HandSpot[side]);
		}
		else
		{
			for (int side = 0; side < 2; side++)
			{
				m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation =
					m_ShoulderRest[side];
				m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = 0.0f;
			}
		}

		// And the feet are collected under the hips during the gather, so that
		// by the time the body rises there is something under it. This is the
		// whole difference between getting up and being teleported upright.
		glm::vec3 right(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 hip = HipWorld(side);
			glm::vec3 foot = hip + right * (side == 0 ? -m_StanceWidth : m_StanceWidth);

			// Picked up on the way, not dragged. A foot slid along the floor
			// catches on its own friction and stays where it fell -- one leg
			// was still 0.79 m from under the hip when the body started to
			// rise. Lifting it clear costs nothing and is what a person does.
			float lift = std::sin(glm::clamp(
				(t - m_GetUpHands) / std::max(m_GetUpGather - m_GetUpHands, 0.01f),
				0.0f, 1.0f) * glm::pi<float>()) * m_GetUpFootLift;
			foot.y = m_GetUpGround + m_AnkleHeight + lift;

			SolveLeg(side, hip, foot, dt);

			// Planted where they were gathered, so the walk picks up from a
			// stance that is actually under him rather than snapping.
			m_Planted[side] = foot;
		}

		// Standing again at the height it got up to -- the ground it lay on
		// plus the clearance -- not the height it last stood at before it
		// fell. GoControlled anchors the hips at m_StandHeight, and the fall
		// check measures from it: left stale, a figure that fell off a ledge
		// got up, was hoisted back to the ledge's height, read a drop it had
		// already taken as a new one, and went down again the next step.
		// (The Ragdoll demo showed it too: back to Controlled at step 820 of
		// a recorded session, Ragdoll again at 821.)
		if (t >= 1.0f)
		{
			m_StandHeight = m_GetUpTo.y;
			GoControlled();
		}
	}


	// ---------------------------------------------------------------------
	// Getting up, the way people do
	// ---------------------------------------------------------------------
	//
	// The first get-up lifted the hips along three waypoints with the limbs
	// trailing on their motors: it went up, but it floated, and from on his
	// back he was spun over a fake roll. This one is choreographed from how a
	// person actually gets off the floor, and at every moment something is
	// carrying him -- hands, knees or feet on the ground, held there by IK
	// against the floor they are touching:
	//
	//   face down   hands under the shoulders -> push up, chest first ->
	//               knees drawn under the hips, onto all fours -> one foot
	//               forward into a kneeling lunge, a hand on that knee ->
	//               up over the front foot, the back foot stepping in.
	//   face up     knees up and hands behind -> sit up onto the hands ->
	//               turn over sideways onto hands and knees -> as above.
	//
	// It is a sequence of key poses, worked out once when he starts from
	// where he lies and how long he is (so a short figure kneels lower):
	// the pelvis's place and turn, the spine's bend, and for each hand and
	// foot a target -- planted on the ground, hanging, or resting on a knee
	// -- and for each knee which way it points. Between keys everything is
	// eased; a hand or foot that has to move between two places on the
	// ground is lifted on the way rather than dragged. The pelvis is still
	// carried kinematically (as the walk carries it); the limbs are motors
	// and contacts. m_NaturalGetUp off brings back the old one.
	struct GetUpKey
	{
		float T = 0.0f;                 // fraction of the whole
		glm::vec3 Pelvis{ 0.0f };
		glm::quat Orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
		float Spine = 0.0f;             // radians of bend, + forward
		int Hand[2] = { 0, 0 };         // 0 planted at HandAt, 1 hanging, 2 on the lead knee
		glm::vec3 HandAt[2] = {};
		glm::vec3 Foot[2] = {};         // ankle targets
		glm::vec3 Pole[2] = { glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
		bool Kneeling[2] = { false, false };   // toes tucked (ankle at rest) rather than sole flat
		float Heading = 0.0f;           // the facing a flat sole is turned to
		// HandAt / Foot / Pole given in the pelvis's own frame rather than
		// the world's: a tuck, which goes wherever the hips go.
		bool LocalHands = false, LocalFeet = false;
	};

	// A pelvis turned so its head end points along `up` and its front
	// along `front` (+x right, +y head, +z front, as built).
	static glm::quat PelvisFacing(const glm::vec3& up, const glm::vec3& front)
	{
		glm::vec3 y = glm::normalize(up);
		glm::vec3 z = glm::normalize(front - y * glm::dot(front, y));
		return glm::normalize(glm::quat_cast(glm::mat3(glm::cross(y, z), y, z)));
	}

	glm::vec3 WristWorld(int side) const
	{
		const GS::RigidBody3D& forearm = m_World.GetBody(m_LowerArm[side]);
		return forearm.Position + forearm.Orientation * glm::vec3(0.0f, -0.5f * m_ForearmLength, 0.0f);
	}

	glm::vec3 KneeWorld(int side) const
	{
		const GS::RigidBody3D& shin = m_World.GetBody(m_LowerLeg[side]);
		return shin.Position + shin.Orientation * glm::vec3(0.0f, 0.5f * m_ShinLength, 0.0f);
	}

	// The body's own measures, for laying out a key pose.
	struct Measures
	{
		float Thigh, Shin, Arm, HipX, HipDrop, Trunk, ShoulderX;
		float HandH, KneelAnkle, TuckedAnkle, Lying, Stance;
	};
	Measures BodyMeasures() const
	{
		float s = m_Proportions.Scale;
		float thigh = m_ThighLength, shin = m_ShinLength;
		float arm = m_UpperArmLength + m_ForearmLength;
		float hipX = std::fabs(m_HipLocal[0].x), hipDrop = -m_HipLocal[0].y;   // hip below the pelvis's centre
		// Hips to shoulders: hip to pelvis centre, pelvis to torso centre as
		// built, torso centre to the shoulder joint.
		float trunk = glm::length(m_BuildPose.size() > 1 ? m_BuildPose[1].first - m_BuildPose[0].first : glm::vec3(0.0f, 0.31f * s, 0.0f))
			+ m_ShoulderLocal[0].y + hipDrop;
		float shoulderX = std::fabs(m_ShoulderLocal[0].x);
		float handH = m_Proportions.ForearmRadius + 0.012f * s;   // a little over the floor, or the motors press the forearm into it
		// A knee on the floor sits a shin's radius up. The ankle behind it
		// does not: with the toes tucked the foot stands at about a right
		// angle to the shin (the ankle's cone allows 35 degrees off that), so
		// the ankle is a foot's length up and the shin slopes to it. Put at
		// knee height, as it first was, the foot was driven 10 cm into the
		// floor. Lying face down is the same: heels up, toes on the floor.
		float kneelAnkle = m_Proportions.ShinRadius + 0.01f * s;
		float tuckedAnkle = 1.9f * m_Proportions.FootHalf.z;
		float lying = m_Proportions.PelvisHalf.z;
		float stance = std::max(m_StanceWidth, hipX * 0.6f);
		return { thigh, shin, arm, hipX, hipDrop, trunk, shoulderX, handH, kneelAnkle, tuckedAnkle, lying, stance };
	}

	void PlanGetUp()
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale;
		float g = GroundY(pelvis.Position);
		m_GetUpGround = g;
		m_GetUpTimer = 0.0f;
		m_GetUpKeys.clear();

		glm::vec3 headward = pelvis.Orientation * glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 front = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		auto flat = [](glm::vec3 v, glm::vec3 fallback)
		{
			v.y = 0.0f;
			return glm::length(v) > 0.05f ? glm::normalize(v) : fallback;
		};
		glm::vec3 fallback(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		bool supine = front.y > 0.35f;
		m_GetUpSupine = supine;
		uint32_t n = m_GetUpCount++;
		int lead = (int)((m_Seed + n) & 1u);   // which foot comes forward
		m_GetUpLead = lead;

		const Measures M = BodyMeasures();
		float thigh = M.Thigh, shin = M.Shin, arm = M.Arm;
		float hipX = M.HipX, hipDrop = M.HipDrop, trunk = M.Trunk, shoulderX = M.ShoulderX;
		float handH = M.HandH, kneelAnkle = M.KneelAnkle, tuckedAnkle = M.TuckedAnkle, lying = M.Lying, stance = M.Stance;

		auto at = [&](const glm::vec3& origin, const glm::vec3& F, float along, float across, float height)
		{
			glm::vec3 right = glm::cross(up, F);
			return glm::vec3(origin.x, g + height, origin.z) + F * along + right * across;
		};

		// The start: as he lies.
		GetUpKey start;
		start.T = 0.0f;
		start.Pelvis = pelvis.Position;
		start.Orientation = pelvis.Orientation;
		for (int side = 0; side < 2; side++)
		{
			start.HandAt[side] = WristWorld(side);
			start.Foot[side] = AnkleWorld(side);
			start.Pole[side] = front;
			start.Kneeling[side] = true;
		}
		glm::vec3 F = supine ? flat(-headward, fallback) : flat(headward, fallback);
		start.Heading = std::atan2(F.x, F.z);
		m_GetUpKeys.push_back(start);

		// All fours, then the lunge and the rise, in a frame (O, F): shared
		// by both, face up arriving at it after turning over.
		auto sideSign = [](int side) { return side == 0 ? -1.0f : 1.0f; };
		// All fours: knees under the hips, hands under the shoulders, arms
		// all but straight. The trunk's tilt is what those two heights make
		// of its length.
		float foursHipH = thigh + kneelAnkle;
		float foursTilt = std::asin(glm::clamp((handH + arm * 0.96f - foursHipH) / std::max(trunk, 0.1f), -0.6f, 0.6f));
		auto allFours = [&](const glm::vec3& O, const glm::vec3& Fr, float handAlong, float t)
		{
			float tilt = foursTilt, hipH = foursHipH;
			float hipAlong = handAlong - trunk * std::cos(tilt);
			glm::vec3 U = Fr * std::cos(tilt) + up * std::sin(tilt);
			glm::vec3 belly = -(up * std::cos(tilt) - Fr * std::sin(tilt));
			GetUpKey fours;
			fours.T = t;
			fours.Orientation = PelvisFacing(U, belly);
			fours.Pelvis = at(O, Fr, hipAlong, 0.0f, hipH) + U * hipDrop;
			fours.Spine = 0.0f;
			fours.Heading = std::atan2(Fr.x, Fr.z);
			for (int side = 0; side < 2; side++)
			{
				fours.Hand[side] = 0;
				fours.HandAt[side] = at(O, Fr, handAlong, sideSign(side) * (shoulderX + 0.03f * s), handH);
				fours.Foot[side] = at(O, Fr, hipAlong - shin * 0.93f, sideSign(side) * hipX, tuckedAnkle);
				fours.Pole[side] = glm::normalize(-up + Fr * 0.6f);
				fours.Kneeling[side] = true;
			}
			return fours;
		};

		auto rise = [&](const glm::vec3& O, const glm::vec3& Fr, float tFours, float tLunge, float tStep, float handAlong, bool keepHands)
		{
			GetUpKey fours = allFours(O, Fr, handAlong, tFours);
			m_GetUpKeys.push_back(fours);
			float hipAlong = handAlong - trunk * std::cos(foursTilt);
			float heading = fours.Heading;

			// The lunge: the lead foot forward, flat, the back knee down,
			// the trunk up over the front thigh, a hand on the front knee.
			float footAlong = hipAlong + 0.48f * s;
			GetUpKey lunge = fours;
			lunge.T = tLunge;
			float lean = glm::radians(18.0f);
			glm::vec3 U2 = up * std::cos(lean) + Fr * std::sin(lean);
			lunge.Orientation = PelvisFacing(U2, Fr * std::cos(lean) - up * std::sin(lean));
			lunge.Pelvis = at(O, Fr, hipAlong + 0.10f * s, 0.0f, thigh + kneelAnkle + 0.02f * s) + U2 * hipDrop;
			lunge.Spine = glm::radians(10.0f);
			lunge.Foot[lead] = at(O, Fr, footAlong, sideSign(lead) * hipX * 1.1f, m_AnkleHeight);
			lunge.Pole[lead] = Fr;
			lunge.Kneeling[lead] = false;
			lunge.Hand[lead] = 2;
			lunge.Hand[1 - lead] = 1;
			m_GetUpKeys.push_back(lunge);

			// Rising, the back foot on its way in.
			GetUpKey step = lunge;
			step.T = tStep;
			float lean3 = glm::radians(10.0f);
			glm::vec3 U3 = up * std::cos(lean3) + Fr * std::sin(lean3);
			step.Orientation = PelvisFacing(U3, Fr * std::cos(lean3) - up * std::sin(lean3));
			step.Pelvis = at(O, Fr, footAlong - 0.08f * s, sideSign(lead) * 0.3f * hipX, 0.80f * m_StandClearance);
			step.Spine = glm::radians(6.0f);
			step.Foot[1 - lead] = at(O, Fr, footAlong - 0.18f * s, sideSign(1 - lead) * stance, m_AnkleHeight + 0.12f * s);
			step.Pole[1 - lead] = Fr;
			step.Kneeling[1 - lead] = false;
			step.Hand[0] = step.Hand[1] = 1;
			m_GetUpKeys.push_back(step);

			// Standing, feet side by side under him.
			GetUpKey stand = step;
			stand.T = 1.0f;
			stand.Orientation = PelvisFacing(up, Fr);
			stand.Pelvis = at(O, Fr, footAlong, 0.0f, m_StandClearance);
			stand.Spine = 0.0f;
			for (int side = 0; side < 2; side++)
			{
				stand.Foot[side] = at(O, Fr, footAlong, sideSign(side) * stance, m_AnkleHeight);
				stand.Pole[side] = Fr;
				stand.Kneeling[side] = false;
				stand.Hand[side] = 1;
			}
			m_GetUpKeys.push_back(stand);
			(void)keepHands;
		};

		// Face down, from (O, F): F the way his head points.
		auto prone = [&](const glm::vec3& O, const glm::vec3& F, float from)
		{
			auto remap = [&](float t) { return from + t * (1.0f - from); };
			// Hands down beside the shoulders, legs out behind.
			GetUpKey hands;
			hands.T = remap(0.14f);
			hands.Orientation = PelvisFacing(F, -up);
			hands.Pelvis = at(O, F, 0.0f, 0.0f, lying);
			hands.Heading = std::atan2(F.x, F.z);
			float handAlong = trunk * 0.92f;
			for (int side = 0; side < 2; side++)
			{
				hands.HandAt[side] = at(O, F, handAlong, sideSign(side) * (shoulderX + 0.07f * s), handH);
				hands.Foot[side] = at(O, F, -(thigh + shin) * 0.97f, sideSign(side) * hipX, tuckedAnkle);
				hands.Pole[side] = -up;
				hands.Kneeling[side] = true;
			}
			m_GetUpKeys.push_back(hands);

			// The push: chest up off the floor, hips still low.
			GetUpKey push = hands;
			push.T = remap(0.32f);
			float lift = glm::radians(20.0f);
			push.Orientation = PelvisFacing(F * std::cos(lift) + up * std::sin(lift), -(up * std::cos(lift) - F * std::sin(lift)));
			push.Pelvis = at(O, F, -0.02f * s, 0.0f, lying + 0.07f * s);
			push.Spine = -glm::radians(18.0f);
			m_GetUpKeys.push_back(push);

			rise(O, F, remap(0.55f), remap(0.76f), remap(0.88f), handAlong, true);
		};

		glm::vec3 H = flat(headward, fallback);
		glm::vec3 O(pelvis.Position.x, g, pelvis.Position.z);
		if (!supine)
		{
			m_GetUpKeys.back().Heading = std::atan2(H.x, H.z);
			prone(O, H, 0.0f);
			m_GetUpTime2 = 2.5f * std::sqrt(s);
		}
		else
		{
			// Face up he rolls onto his front first, about his own length,
			// towards the lead side -- the legs go with the hips, so nothing
			// crosses -- and gets up from there as from face down. (A sit-up
			// and a turn over bent knees was tried first: the turn asks the
			// hips for more than their 55 degree cones, and the knees flipped
			// between solutions mid-turn.)
			glm::quat onBack = PelvisFacing(H, up);
			float roll = lead == 1 ? 1.0f : -1.0f;
			glm::vec3 toward = glm::angleAxis(roll * glm::half_pi<float>(), H) * up;   // where his front faces half way
			toward.y = 0.0f;
			toward = glm::length(toward) > 1e-3f ? glm::normalize(toward) : glm::cross(up, H);
			float travel = m_Proportions.PelvisHalf.x + m_Proportions.PelvisHalf.z;   // a quarter turn's worth of rolling
			for (int k = 1; k <= 2; k++)
			{
				float angle = roll * glm::half_pi<float>() * (float)k;
				GetUpKey key;
				key.T = 0.13f * (float)k;
				key.Orientation = glm::normalize(glm::angleAxis(angle, H) * onBack);
				glm::vec3 centre = O + toward * (travel * (float)k);
				float height = k == 1 ? m_Proportions.PelvisHalf.x : lying;
				key.Pelvis = glm::vec3(centre.x, g + height, centre.z);
				key.Heading = std::atan2(H.x, H.z);
				glm::vec3 bodyRight = key.Orientation * glm::vec3(1.0f, 0.0f, 0.0f);
				for (int side = 0; side < 2; side++)
				{
					glm::vec3 foot = key.Pelvis - H * (hipDrop + (thigh + shin) * 0.97f) + bodyRight * (sideSign(side) * hipX);
					foot.y = std::max(foot.y, g + tuckedAnkle * 0.8f);
					key.Foot[side] = foot;
					key.Pole[side] = key.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
					key.Kneeling[side] = true;
					// Both hands reach the way he is rolling, near the shoulders.
					key.HandAt[side] = at(centre, H, trunk * 0.85f, 0.0f, handH)
						+ toward * (0.22f * s) + bodyRight * (sideSign(side) * 0.10f * s * (k == 2 ? 2.0f : 0.5f));
					key.HandAt[side].y = g + handH;
				}
				m_GetUpKeys.push_back(key);
			}
			glm::vec3 O1 = O + toward * (2.0f * travel);
			prone(O1, H, 0.26f);
			m_GetUpTime2 = 3.2f * std::sqrt(s);
		}

		HipsHeavy(true);
		pelvis.AngularVelocity = glm::vec3(0.0f);
		m_FloorLift = 0.0f;

		// A hand under the shoulder of a body lying flat is the arm at right
		// angles to the trunk, and a shoulder flexes far further than that --
		// but the rig's cone is 75 degrees, kept tight for the walk and the
		// fall. Opened while he gets up, closed again when he stands.
		m_JointsClosing = false;
		ShoulderCone(120.0f);
		// And the spine opened as a move opens it, with its twist: the hips
		// are turned to the keys and the torso has to come round after them
		// off the floor. At the built 25 degrees and 35 of twist the limit
		// dragged it round against the floor's grip, and a limit is as hard
		// as the hips behind it -- the floor pressed the trunk 5 times his
		// weight, then 100, then 560, step on step, until it gave. Inside the
		// wider limits it is the spine's motor that brings it, as hard as a
		// motor can.
		OpenSpine(true);
	}

	void OpenSpine(bool open)
	{
		StoreCones();
		GS::Joint3D& spine = m_World.GetJoint(m_SpineJoint);
		spine.ConeAngle = open ? glm::radians(60.0f) : m_SpineCone;
		spine.TwistLower = open ? -glm::radians(60.0f) : m_SpineTwistBuilt[0];
		spine.TwistUpper = open ? glm::radians(60.0f) : m_SpineTwistBuilt[1];
	}

	// The hardest anything not his pressed his pelvis, torso or head in the
	// last step, as a force.
	float TrunkPressed(float dt) const
	{
		if (dt <= 0.0f)
			return 0.0f;
		float worst = 0.0f;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool mineA = IsCharacter(contact.A), mineB = IsCharacter(contact.B);
			if (mineA == mineB)
				continue;
			unsigned int mine = mineA ? contact.A : contact.B;
			if (mine != m_Pelvis && mine != m_Torso && mine != m_Head)
				continue;
			worst = std::max(worst, contact.TotalNormalImpulse() / dt);
		}
		return worst;
	}

	// The hips while he gets up: carried along the keys as before, but
	// heavy -- 1000 kg -- rather than kinematic. Kinematic, they are an
	// unlimited force, and whatever was caught between them and the floor --
	// his own torso, wider on its side than they are, its corner at the
	// spine pinned into the floor -- was pressed harder every step until the
	// solver gave way and threw him about. Heavy, they give a little:
	// knock-downs on a clear floor that ended that way went from 4 in 40 to
	// 1. Their turn stays exact -- no inverse inertia, so no joint or contact
	// can turn them; heavy in turn as well measured no better -- and is set
	// to the keys' each step. What still wedges him UpdateNaturalGetUp
	// catches, letting go and trying again.
	void HipsHeavy(bool heavy)
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		if (heavy)
		{
			if (!m_HipsHeavy)
			{
				m_PelvisInverseMass = pelvis.InverseMass;
				m_PelvisInverseInertia = pelvis.InverseInertiaLocal;
			}
			pelvis.Type = GS::BodyType::Dynamic;
			pelvis.InverseMass = 1.0f / 1000.0f;
			pelvis.InverseInertiaLocal = glm::mat3(0.0f);
			pelvis.UpdateInertiaWorld();
			pelvis.Awake = true;
			pelvis.SleepTimer = 0.0f;
			m_HipsHeavy = true;
		}
		else if (m_HipsHeavy)
		{
			pelvis.InverseMass = m_PelvisInverseMass;
			pelvis.InverseInertiaLocal = m_PelvisInverseInertia;
			pelvis.UpdateInertiaWorld();
			m_HipsHeavy = false;
		}
	}

	void ShoulderCone(float degrees)
	{
		// The angle only: SetConeTwistLimits would re-measure the rest pose
		// from wherever the arm happens to be lying.
		for (int side = 0; side < 2; side++)
			m_World.GetJoint(m_ShoulderJoint[side]).ConeAngle = glm::radians(degrees);
	}

	void UpdateNaturalGetUp(float dt)
	{
		if (m_GetUpKeys.size() < 2)
		{
			GoControlled();
			return;
		}
		// Wedged: something not his pressing his trunk with more than 1500
		// times his weight. Get-ups that stood up went to 711 at the most on
		// a clear floor (7 typically) and 967 among rolling cannonballs; one
		// caught between the floor and something that will not give -- a
		// signpost, once -- climbs past that step on step, and pressed on, the
		// solver gave way and threw him about. He lets go instead, as a person
		// does who cannot get up that way, and tries again in a moment from
		// wherever he settles. (What wedged him most was the get-up's own
		// making: his spine dragged round against the floor by its limits,
		// and joints corrected eight times as hard in a step cut eight ways
		// -- OpenSpine, and PhysicsWorld3D's JointRateDt.)
		if (m_GetUpTimer > 0.0f && TrunkPressed(dt) > 1500.0f * m_BodyMass * 9.81f)
		{
			GoRagdoll("wedged getting up");
			return;
		}
		m_GetUpTimer += dt;
		float u = glm::clamp(m_GetUpTimer / std::max(m_GetUpTime2, 0.1f), 0.0f, 1.0f);
		size_t i = 0;
		while (i + 2 < m_GetUpKeys.size() && u >= m_GetUpKeys[i + 1].T)
			i++;
		const GetUpKey& a = m_GetUpKeys[i];
		const GetUpKey& b = m_GetUpKeys[i + 1];
		float w = glm::smoothstep(0.0f, 1.0f, glm::clamp((u - a.T) / std::max(b.T - a.T, 1e-4f), 0.0f, 1.0f));
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 target = glm::mix(a.Pelvis, b.Pelvis, w);
		glm::quat turn = glm::slerp(a.Orientation, b.Orientation, w);
		// Never into the floor. The pelvis is carried, not simulated, so
		// nothing else will stop it: between two keys a box turning on its
		// edge reaches lower than either key's height allows for (5.5 cm in
		// the roll, measured). Its lowest corner, for the turn it has now.
		// And the torso and head with it, the trunk bent as the keys bend it
		// (LowestBelow, the roll's own measure, without the tuck): on his
		// side the torso is 3 cm wider than the hips each way, and rolling
		// over at the hips' height put its corner into the floor.
		{
			glm::mat3 r = glm::mat3_cast(turn);
			const glm::vec3& half = m_Proportions.PelvisHalf;
			float reach = std::fabs(r[0].y) * half.x + std::fabs(r[1].y) * half.y + std::fabs(r[2].y) * half.z;
			reach = std::max(reach, LowestBelow(turn, glm::mix(a.Spine, b.Spine, w), false));
			target.y = std::max(target.y, m_GetUpGround + reach + 0.004f);
			// And the moves' give (UpdateMove): up as far as the trunk he has
			// needs, and further while an arm he lies on, or anything else of
			// his, is wedged against the floor -- in 46 knock-downs, 14 get-ups
			// pressed a forearm 5 to 15 cm into it and 8 then flung it out.
			float s = m_Proportions.Scale;
			float need = TrunkLift(target, turn);
			if (Wedged(dt, true) || ArmSunk())
				need = std::max(need, m_FloorLift + 0.03f * s);
			m_FloorLift += glm::clamp(need - m_FloorLift, -0.6f * s * dt, 2.0f * s * dt);
			target.y += m_FloorLift;
		}
		// Closing the gap to the key over a 60th of a second, however finely
		// the step is split. Closed within one substep, the push behind the
		// hips on whatever is in the way goes as one over the substep squared
		// -- 64 times as hard at an eighth of the step -- and GameSession
		// splits the step whenever anything is fast, his own limbs included.
		pelvis.Velocity = (target - pelvis.Position) / std::max(dt, 1.0f / 60.0f);
		pelvis.AngularVelocity = glm::vec3(0.0f);
		pelvis.Orientation = turn;
		pelvis.UpdateInertiaWorld();

		// Strength back from limp over the first eighth.
		float strength = glm::clamp(u / 0.12f, 0.0f, 1.0f);
		for (auto handle : m_Joints)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			joint.MotorStiffness = glm::mix(m_LimpStiffness, m_MotorStiffness, strength);
			joint.MotorMaxTorque = glm::mix(m_LimpTorque, m_JointTorque[handle], strength);
		}
		for (int side = 0; side < 2; side++)
			for (auto handle : { m_HipJoint[side], m_KneeJoint[side], m_AnkleJoint[side] })
			{
				GS::Joint3D& joint = m_World.GetJoint(handle);
				joint.MotorStiffness = glm::mix(m_LimpStiffness, m_LegStiffness, strength);
				joint.MotorMaxTorque = glm::mix(m_LimpTorque, m_JointTorque[handle] * m_LegTorqueScale, strength);
				// 12 rad/s, not the walk's 30: nothing in a get-up is that
				// quick, and at 30 a leg left folded by the fall whipped
				// round at 1,500-1,800 degrees a second as its strength came
				// back (4 get-ups in 46).
				joint.MotorMaxSpeed = 12.0f;
			}

		DriveKeyLimbs(a, b, w, m_GetUpGround);

		if (u >= 1.0f)
		{
			const GetUpKey& last = m_GetUpKeys.back();
			m_Facing = last.Heading;
			for (int side = 0; side < 2; side++)
			{
				m_Planted[side] = last.Foot[side];
				m_SwingFrom[side] = last.Foot[side];
			}
			m_StandHeight = last.Pelvis.y;
			m_JointsClosing = true;
			GoControlled();
		}
	}

	// The limbs between two keys, `w` of the way from a to b: spine, hands
	// and feet, each foot's ankle soft or flat. Shared by the get-up and the
	// parkour moves. `ground` is the floor under the knees, or below -1e8
	// to ask under each hip.
	glm::vec3 Resolve(const glm::vec3& p, bool local) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		return local ? pelvis.Position + pelvis.Orientation * p : p;
	}
	glm::vec3 ResolveDirection(const glm::vec3& d, bool local) const
	{
		return local ? m_World.GetBody(m_Pelvis).Orientation * d : d;
	}

	// `w` of the way from `from` to `to`, both in the world, along the
	// straight line between them in the pelvis's own frame as it is now.
	glm::vec3 BodyBlend(const glm::vec3& from, const glm::vec3& to, float w) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::quat inverse = glm::inverse(pelvis.Orientation);
		glm::vec3 a = inverse * (from - pelvis.Position), b = inverse * (to - pelvis.Position);
		return pelvis.Position + pelvis.Orientation * glm::mix(a, b, w);
	}

	void DriveKeyLimbs(const GetUpKey& a, const GetUpKey& b, float w, float ground)
	{
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float spine = glm::mix(a.Spine, b.Spine, w);
		m_World.GetJoint(m_SpineJoint).MotorTargetRotation =
			glm::normalize(glm::angleAxis(spine, glm::vec3(1.0f, 0.0f, 0.0f)) * m_SpineRest);
		// The chin tucked as the back rounds past 20 degrees, as a person
		// rolling tucks it -- and as LowestBelow already supposed. Held as
		// built, the head of a dive's roll met the floor at 17-32 kN while
		// the rest of him cleared it. Nothing the get-up bends that far.
		float tuck = glm::clamp(spine - glm::radians(20.0f), 0.0f, glm::radians(35.0f));
		m_World.GetJoint(m_NeckJoint).MotorTargetRotation = glm::angleAxis(tuck, glm::vec3(1.0f, 0.0f, 0.0f)) * m_NeckRest;

		// A point moved between two places on the ground is carried over,
		// not dragged: lifted by up to a fifth of the distance. And lifted
		// for as long as the limb is still on its way, not only while the
		// target is: a foot brought forward from kneeling into the lunge
		// trailed its target by half a metre, so the arc was over before
		// the foot had moved, and it was dragged the rest of the way with
		// its toe 7.5 cm into the floor (the tall body, measured).
		const float liftCap = 0.15f * m_Proportions.Scale;
		auto carry = [&](const glm::vec3& from, const glm::vec3& to, float t, const glm::vec3& now)
		{
			glm::vec3 p = glm::mix(from, to, t);
			float d = glm::length(glm::vec2(to.x - from.x, to.z - from.z));
			float arc = d > 0.08f * m_Proportions.Scale ? std::min(liftCap, 0.2f * d) * std::sin(glm::pi<float>() * t) : 0.0f;
			float behind = glm::length(glm::vec2(p.x - now.x, p.z - now.z));
			float lag = behind > 0.06f * m_Proportions.Scale ? std::min(liftCap, 0.5f * (behind - 0.06f * m_Proportions.Scale)) : 0.0f;
			p.y += std::max(arc, lag);
			return p;
		};
		// A hanging hand hangs along him, down his trunk -- not down the
		// world's up, which for a figure on his back in a roll was into
		// the floor under his shoulders: the arms pressed into it until
		// the solver threw them about (44,000 degrees a second, a dive's
		// roll). Upright the two are the same. And no hand below the floor.
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
		glm::vec3 trunkDown = torso.Orientation * glm::vec3(0.0f, -1.0f, 0.0f);
		glm::vec3 trunkFront = torso.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		bool upright = trunkDown.y < -0.7f;
		auto hand = [&](const GetUpKey& k, int side)
		{
			glm::vec3 p;
			switch (k.Hand[side])
			{
				case 1:
					p = upright
						? ShoulderWorld(side) + glm::vec3(0.0f, -0.95f * (m_UpperArmLength + m_ForearmLength), 0.0f)
							+ glm::vec3(std::sin(k.Heading), 0.0f, std::cos(k.Heading)) * (0.04f * m_Proportions.Scale)
						: ShoulderWorld(side) + trunkDown * (0.95f * (m_UpperArmLength + m_ForearmLength)) + trunkFront * (0.04f * m_Proportions.Scale);
					break;
				case 2: p = KneeWorld(m_GetUpLead) + up * (0.06f * m_Proportions.Scale); break;
				default: return Resolve(k.HandAt[side], k.LocalHands);
			}
			glm::vec3 shoulder = ShoulderWorld(side);
			float floor = GroundY(glm::vec3(p.x, std::max(p.y, shoulder.y) + 0.1f * m_Proportions.Scale, p.z), -1000.0f);
			p.y = std::max(p.y, floor + m_Proportions.ForearmRadius + 0.012f * m_Proportions.Scale);
			return p;
		};
		for (int side = 0; side < 2; side++)
		{
			// Carried over the floor only between two places on it. A hand
			// coming up off the floor to hang at his side, or onto his knee,
			// has nothing to clear, and the lift for one lagging its target
			// -- up to 15 cm -- raised the arms as he stood.
			glm::vec3 handFrom = hand(a, side), handTo = hand(b, side);
			glm::vec3 handAt = a.Hand[side] == 0 && b.Hand[side] == 0 ? carry(handFrom, handTo, w, WristWorld(side))
				: glm::mix(handFrom, handTo, w);
			if (a.Hand[side] == 0 && b.Hand[side] == 0 && a.LocalHands != b.LocalHands)
				handAt = BodyBlend(handFrom, handTo, w);
			SolveArm(side, handAt);

			glm::vec3 now = AnkleWorld(side);
			glm::vec3 footFrom = Resolve(a.Foot[side], a.LocalFeet), footTo = Resolve(b.Foot[side], b.LocalFeet);
			// From a tuck (in his frame) to the ground (in the world's), or
			// back, the way between is his: blended in the world's frame,
			// a foot tucked against him upside down in a roll went from
			// there towards the ground ahead by way of the air above him --
			// both legs straight up, a candle, then down flat on his back.
			glm::vec3 ankle = a.LocalFeet == b.LocalFeet ? carry(footFrom, footTo, w, now) : BodyBlend(footFrom, footTo, w);
			// Up before over. A foot well below where it is being lifted to
			// rises first and goes forward once it is up, as a foot lifted
			// onto a step does: carried straight at its target, the toes --
			// 13 cm ahead of the ankle -- went into the riser and were
			// dragged over the corner, 9-11 cm into the box.
			{
				float below = ankle.y - now.y - 0.06f * m_Proportions.Scale;
				if (below > 0.0f)
				{
					float go = glm::clamp(1.0f - below / (0.10f * m_Proportions.Scale), 0.0f, 1.0f);
					ankle.x = glm::mix(now.x, ankle.x, go);
					ankle.z = glm::mix(now.z, ankle.z, go);
				}
			}
			glm::vec3 pole = glm::normalize(glm::mix(ResolveDirection(a.Pole[side], a.LocalFeet),
				ResolveDirection(b.Pole[side], b.LocalFeet), w) + glm::vec3(0.0f, 1e-3f, 0.0f));
			// Under each knee, its own floor: for the get-up the ground he
			// lies on, for a move whatever is under that hip -- the top of
			// the wall he is going over.
			float under = ground > -1e8f ? ground
				: GroundY(HipWorld(side), -1000.0f);
			SolveLegPole(side, HipWorld(side), ankle, pole, under + m_Proportions.ThighRadius + 0.005f * m_Proportions.Scale);
			// How flat the sole is held, 0 toes tucked and soft (the floor
			// decides the foot's angle -- held at its rest pose with the
			// leg's full strength, the foot out-pushed the floor's contact
			// and sank 8 cm into it) to 1 flat on the ground, turned the way
			// he will face. Eased across the middle of the keys' interval:
			// switched at its half-way point, the lead foot coming forward
			// into the lunge went from soft to full strength in one step
			// with its target 90 degrees away, and every face-down get-up
			// measured had a foot snap round at 2,550 degrees a second;
			// eased, 1,800-2,200. What is left (and all of the face-up
			// ones' 2,300) is the sole slapping flat as it lands on its
			// edge, one step of 38 degrees: the contact, not the motor.
			float flat = glm::mix(a.Kneeling[side] ? 0.0f : 1.0f, b.Kneeling[side] ? 0.0f : 1.0f, glm::smoothstep(0.2f, 0.8f, w));
			GS::Joint3D& ankleJoint = m_World.GetJoint(m_AnkleJoint[side]);
			if (flat <= 0.0f)
			{
				ankleJoint.MotorTargetRotation = m_AnkleRest[side];
				ankleJoint.MotorStiffness = m_LimpStiffness * 2.0f;
				ankleJoint.MotorMaxTorque = m_LimpTorque * 2.0f;
			}
			else
			{
				const GS::RigidBody3D& shinBody = m_World.GetBody(m_LowerLeg[side]);
				float heading = glm::mix(a.Heading, b.Heading, w);
				glm::quat sole = glm::normalize(glm::conjugate(shinBody.Orientation) * glm::angleAxis(heading, up));
				if (flat >= 1.0f)
					ankleJoint.MotorTargetRotation = sole;
				else
				{
					ankleJoint.MotorTargetRotation = glm::normalize(glm::slerp(m_AnkleRest[side], sole, flat));
					ankleJoint.MotorStiffness = glm::mix(m_LimpStiffness * 2.0f, ankleJoint.MotorStiffness, flat);
					ankleJoint.MotorMaxTorque = glm::mix(m_LimpTorque * 2.0f, ankleJoint.MotorMaxTorque, flat);
				}
			}
			m_Planted[side] = ankle;
		}
	}

	// ---------------------------------------------------------------------
	// Loose things that count as solid
	// ---------------------------------------------------------------------
	//
	// The world's ground is only what holds still -- PhysicsWorld3D::
	// GroundBelow leaves every dynamic body out, so that a stand height does
	// not chase whatever wanders underfoot -- and Sense climbed only fixed
	// things. So a crate was neither ground nor an obstacle: he walked into
	// it, leaned on it until the contacts read as blows and put him down
	// (measured in the course: a stumble 3 s in, down at 8 s), and never
	// climbed it. A loose body counts as solid -- an obstacle, something
	// to put a hand or a foot on, ground -- when it is a box at rest, not
	// part of a jointed figure (another character's limb is no step), and
	// at least a fifth of his weight, so that what a climb does to it does
	// not move it. Once counted it stays counted until it is clearly moving:
	// a crate under his feet trembling past the at-rest speed for one step
	// would otherwise drop the ground he stands on by its height.
	void RefreshStandable()
	{
		const std::vector<GS::Joint3D>& joints = m_World.GetJoints();
		const std::vector<GS::RigidBody3D>& bodies = m_World.GetBodies();
		if (m_Jointed.size() != bodies.size() || m_JointsSeen != joints.size())
		{
			m_Jointed.assign(bodies.size(), 0);
			for (const GS::Joint3D& joint : joints)
				for (unsigned int b : { joint.A, joint.B })
					if (b < m_Jointed.size())
						m_Jointed[b] = 1;
			m_JointsSeen = joints.size();
		}
		std::vector<GS::PhysicsWorld3D::BodyHandle> was;
		was.swap(m_Standable);
		float heavy = 0.2f * glm::clamp(m_Physique.Mass, 20.0f, 250.0f);
		for (unsigned int i = 0; i < bodies.size(); i++)
		{
			const GS::RigidBody3D& b = bodies[i];
			if (!b.Enabled || b.Type != GS::BodyType::Dynamic || b.Shape != GS::ColliderShape3D::Box || m_Jointed[i]
				|| b.GetMass() < heavy)
				continue;
			bool counted = std::find(was.begin(), was.end(), i) != was.end();
			if (glm::length(b.Velocity) < (counted ? 1.5f : 0.3f) && glm::length(b.AngularVelocity) < (counted ? 3.0f : 0.6f))
				m_Standable.push_back(i);
		}
	}

	bool Standable(GS::PhysicsWorld3D::BodyHandle handle) const
	{
		return std::find(m_Standable.begin(), m_Standable.end(), handle) != m_Standable.end();
	}

	// Fixed, or loose and counted as fixed.
	bool Solid(GS::PhysicsWorld3D::BodyHandle handle) const
	{
		return m_World.GetBody(handle).Type == GS::BodyType::Static || Standable(handle);
	}

	// Moving about, or light: something whose touch can be a blow.
	bool Loose(GS::PhysicsWorld3D::BodyHandle handle) const
	{
		return m_World.GetBody(handle).Type == GS::BodyType::Dynamic && !Standable(handle);
	}

	// The ground under `point` for him: the world's, or the top of a loose
	// body he counts as solid -- straight down through the box in its own
	// frame, as PhysicsWorld3D::Raycast does -- whichever is higher. With
	// nothing loose about it is the world's answer exactly.
	bool GroundUnder(const glm::vec3& point, float& height, glm::vec3& normal, float floor) const
	{
		bool found = m_World.GroundBelow(point, height, normal, m_CharacterBodies, floor);
		if (!found)
		{
			height = floor;
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		for (GS::PhysicsWorld3D::BodyHandle handle : m_Standable)
		{
			const GS::RigidBody3D& b = m_World.GetBody(handle);
			glm::quat inverse = glm::inverse(b.Orientation);
			glm::vec3 lo = inverse * (point - b.Position);
			glm::vec3 ld = inverse * glm::vec3(0.0f, -1.0f, 0.0f);
			float enter = 0.0f, leave = point.y - floor;
			int axis = -1;
			bool miss = false;
			for (int a = 0; a < 3 && !miss; a++)
			{
				if (std::fabs(ld[a]) < 1e-9f)
				{
					miss = std::fabs(lo[a]) > b.HalfExtents[a];
					continue;
				}
				float t1 = (-b.HalfExtents[a] - lo[a]) / ld[a], t2 = (b.HalfExtents[a] - lo[a]) / ld[a];
				if (t1 > t2)
					std::swap(t1, t2);
				if (t1 > enter)
				{
					enter = t1;
					axis = a;
				}
				leave = std::min(leave, t2);
				miss = enter > leave;
			}
			// A start inside the box reports nothing (axis stays -1): only a
			// surface below the point is ground, as for the world's own.
			if (miss || axis < 0)
				continue;
			glm::vec3 n(0.0f);
			n[axis] = 1.0f;
			n = b.Orientation * n;
			if (n.y < 0.0f)
				n = -n;
			float y = point.y - enter;
			if (n.y > 0.7f && y > height)
			{
				height = y;
				normal = n;
				found = true;
			}
		}
		return found;
	}
	float GroundY(const glm::vec3& point, float floor = 0.0f) const
	{
		float height;
		glm::vec3 normal;
		return GroundUnder(point, height, normal, floor) ? height : floor;
	}

	// ---------------------------------------------------------------------
	// Parkour: what is in front of him, and the moves over, onto and down
	// ---------------------------------------------------------------------
	//
	// Four moves, each a choreography of the get-up's kind -- key poses for
	// the limbs (hands planted on an edge, a foot put on a top, legs tucked
	// in the hips' own frame) -- over a path for the hips that is worked out
	// rather than drawn: a vault's flight is a projectile under g, a drop
	// falls as anything falls, a roll goes round on whatever part of him is
	// lowest, the way a body rolls. The pelvis is carried, as in the walk;
	// everything else is motors and contacts, so the hands really are on
	// the wall and the feet really do land.
	//
	//   * Vault -- a waist-high wall or rail, thin: both hands on the top,
	//     knees tucked through, landing on the far side still running.
	//   * Step on -- something knee to thigh high and deep: a foot on it, a
	//     hand on it, and up.
	//   * Climb -- a ledge up to about 1.4 times his height: jump if it
	//     is high, catch the edge, pull, press over it, a foot on, stand.
	//   * Drop -- off anything higher than he walks down, up to 4.5 m (his
	//     height scaled): falls, lands, and rolls out of it if it was far.
	//
	// What is ahead is asked of the world each step by rays at knee, waist,
	// chest and head height (Sense). A wall is a wall: the hips no longer
	// walk through one. Jump when something is in reach starts the move;
	// running, a vault starts by itself. m_Parkour off brings all of it
	// back to what it was: walking through walls and falling off ledges.

	struct Obstacle
	{
		enum Kind { None, Vault, StepOn, Climb, Wall };
		Kind Type = None;
		float Distance = 0.0f;           // pelvis centre to the face, along the facing
		glm::vec3 Normal{ 0.0f };        // the face's, out of it
		float Ground = 0.0f;             // under him
		float Top = 0.0f;                // the top's height
		float Depth = 0.0f;              // how far the top goes on past the face
		float Beyond = 0.0f;             // the ground past it
	};
	enum class Move { None, Vault, StepOn, Climb, Drop, Roll, WallRun, Hang, DropToHang, CatLeap, TicTac, Precision, Dodge };

	static const char* ObstacleName(Obstacle::Kind kind)
	{
		switch (kind)
		{
			case Obstacle::Vault: return "vault";
			case Obstacle::StepOn: return "step on";
			case Obstacle::Climb: return "climb";
			case Obstacle::Wall: return "wall";
			default: return "";
		}
	}

	// Rays along `F` from the ground under him: the nearest face at knee,
	// waist, chest or head height, then down onto its top just past the
	// face, then on along the top to find where it ends. Only solid things
	// count -- fixed, or a crate heavy enough and at rest (Solid) -- and
	// nothing that faces up more than it faces him: a slope is walked.
	Obstacle Sense(const glm::vec3& F) const
	{
		Obstacle o;
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale;
		o.Ground = GroundY(pelvis.Position, -1000.0f);
		glm::vec3 base(pelvis.Position.x, o.Ground, pelvis.Position.z);
		auto fixed = [&](const GS::PhysicsWorld3D::RayHit& hit)
		{
			return hit.Hit && Solid(hit.Body);
		};
		bool any = false;
		for (float h : { 0.42f, 0.80f, 1.20f, 1.60f })
		{
			// Out to 2.8 m in his scale: far enough to see a ledge he could
			// leap to (the cat leap), not only one in arm's reach.
			GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(base + up * (h * s), F, 2.8f * s, m_CharacterBodies);
			if (!fixed(hit) || hit.Normal.y > 0.6f)
				continue;
			if (!any || hit.Distance < o.Distance)
			{
				o.Distance = hit.Distance;
				o.Normal = hit.Normal;
			}
			any = true;
		}
		if (!any)
			return o;

		const float probe = 2.6f * s;
		glm::vec3 over = base + F * (o.Distance + 0.06f * s);
		GS::PhysicsWorld3D::RayHit top = m_World.Raycast(over + up * probe, -up, probe + 0.05f * s, m_CharacterBodies);
		o.Top = top.Hit ? top.Point.y : o.Ground + probe;
		float height = o.Top - o.Ground;

		// Room over the top to put a body through.
		GS::PhysicsWorld3D::RayHit room = m_World.Raycast(base + up * (height + 0.30f * s), F, o.Distance + 0.35f * s, m_CharacterBodies);
		bool roof = fixed(room);

		// From a metre over the top, not 30 cm: a ray that starts inside
		// what rises beyond the top sees nothing of it and goes on to the
		// floor, so a flight of stairs -- the 3rd step's riser the first
		// face the rays meet, the 5th step 34 cm above it -- read as a wall
		// 60 cm deep, and running at it vaulted the stairs.
		o.Depth = 1.2f * s;
		o.Beyond = o.Top;
		for (int k = 1; k <= 8; k++)
		{
			glm::vec3 p = over + F * (0.12f * s * (float)k);
			GS::PhysicsWorld3D::RayHit down = m_World.Raycast(glm::vec3(p.x, o.Top + 1.0f * s, p.z), -up, 5.3f * s, m_CharacterBodies);
			if (!down.Hit || down.Point.y < o.Top - 0.3f * s)
			{
				o.Depth = 0.12f * s * (float)k;
				o.Beyond = down.Hit ? down.Point.y : o.Ground - 4.0f * s;
				break;
			}
		}

		// How high the face stands over the ground just in front of it,
		// which on stairs is the step below it, not the floor under him: a
		// riser the walk takes is a step however far up the flight it is.
		glm::vec3 front = base + F * std::max(o.Distance - 0.10f * s, 0.0f);
		float before = GroundY(glm::vec3(front.x, o.Top + 0.05f * s, front.z), -1000.0f);

		// To 2.6 m in his scale: the hands reach 2.1 standing, and a jump
		// of half a metre catches the edge above that (the climb's hang).
		if (!top.Hit || roof || height > 2.6f * s)
			o.Type = Obstacle::Wall;
		else if (height < m_MaxStepUp * 1.05f || o.Top - before < m_MaxStepUp * 1.05f)
			o.Type = Obstacle::None;   // a step: the walk takes it
		else if (o.Depth <= 0.75f * s && height <= 1.15f * s)
			o.Type = o.Beyond > o.Ground - 4.5f * s ? Obstacle::Vault : Obstacle::Wall;   // a drop beyond is landed, or rolled
		else if (height <= 0.80f * s)
			o.Type = Obstacle::StepOn;
		else
			o.Type = Obstacle::Climb;
		return o;
	}

	// Asked once a walking step, before he moves: is a wall in the way (and
	// how much of the walk is left once it is), and has a move begun?
	// Returns true if one has, and the walk is over for this step.
	bool ParkourStep(const glm::vec3& forward)
	{
		if (!ParkourOn())
			return false;
		m_Ahead = Sense(forward);
		const Obstacle& o = m_Ahead;
		if (o.Type == Obstacle::None)
			return TryWallRun(forward) || TryPrecision(forward);
		float s = m_Proportions.Scale;
		glm::vec3 n(o.Normal.x, 0.0f, o.Normal.z);
		float facing = glm::length(n) > 1e-4f ? -glm::dot(forward, glm::normalize(n)) : 0.0f;
		bool running = m_Control.Enabled && m_Control.Run && m_RunBlend > 0.5f;
		if (TryMove(o, forward))
			return true;
		if (TryWallRun(forward) || TryPrecision(forward))
			return true;

		// Coming at it, he slows so as to stop short of it: no faster than
		// v = sqrt(2 a d) for the distance left, at about 6 m/s^2 -- a hard
		// stop for a runner. Stopped dead at the wall, as it first was, a
		// run ended in one step and the legs could not follow: down. Not
		// for a vault he is running at, which he will take.
		bool taking = o.Type == Obstacle::Vault && running;
		if (!taking && facing > 0.3f)
		{
			float left = std::max(o.Distance - 0.32f * s, 0.0f);
			float allowed = std::sqrt(2.0f * 6.0f * left) / std::max(facing, 0.3f);
			if (m_Speed > allowed)
			{
				m_Speed = allowed;
				m_SpeedRate = std::min(m_SpeedRate, 0.0f);
			}
		}
		// Up against it: no further in. Head on, he stops; at an angle, he
		// goes along it. The body is about 0.3 m deep (and the stepping foot
		// reaches past it), so that is how near the hips come.
		if (o.Distance < 0.32f * s && facing > 0.0f)
		{
			float into = facing;
			m_Speed *= std::max(0.0f, 1.0f - into * 1.4f);
			if (m_Speed < 0.05f)
			{
				m_Speed = 0.0f;
				m_SpeedRate = std::min(m_SpeedRate, 0.0f);
			}
			m_Blocked = glm::normalize(n);
		}
		return false;
	}

	// Whether `o` starts a move now, and if so starts it: a vault when jump
	// is asked for or he is running at it, a step on or a climb on a jump,
	// each only in reach and roughly square on. From the walk, and from the
	// end of another move -- which is how one runs into the next.
	bool TryMove(const Obstacle& o, const glm::vec3& forward)
	{
		float s = m_Proportions.Scale;
		glm::vec3 n(o.Normal.x, 0.0f, o.Normal.z);
		float facing = glm::length(n) > 1e-4f ? -glm::dot(forward, glm::normalize(n)) : 0.0f;
		bool jump = m_WantsJump || m_JumpPending;
		// Running is the wish to, not the speed: landing from a drop right
		// in front of a wall leaves him nearly still, and asking for speed
		// as well had him stand at it, run held, for good.
		bool running = m_Control.Enabled && m_Control.Run && m_RunBlend > 0.5f;
		bool square = facing > 0.75f && m_Air == Air::Grounded && glm::length(m_StaggerVelocity) < 0.1f;
		// Walked up to, something low he can stand on -- a crate, a box --
		// is stepped onto rather than over; run at, it is still a vault.
		if (square && jump && !running && o.Type == Obstacle::Vault && StepsOnto(o) && o.Distance <= 0.80f * s + 0.15f * m_Speed)
		{
			PlanStepOn(o);
			return true;
		}
		if (square && o.Type == Obstacle::Vault && o.Distance <= 0.55f * s + 0.22f * m_Speed && (jump || running))
		{
			PlanVault(o);
			return true;
		}
		if (square && jump && (o.Type == Obstacle::StepOn || o.Type == Obstacle::Climb) && o.Distance <= 0.80f * s + 0.15f * m_Speed)
		{
			if (o.Type == Obstacle::StepOn)
				PlanStepOn(o);
			else
				PlanClimb(o);
			return true;
		}
		// Further off, running: a leap to the edge, caught in a hang.
		if (square && jump && running && o.Type == Obstacle::Climb && o.Distance <= 2.6f * s && PlanCatLeap(o))
			return true;
		// Met at a slant, a wall too tall to get over: a step off it, and
		// away in a new direction.
		if (jump && running && facing > 0.25f && facing <= 0.90f && o.Type == Obstacle::Wall && o.Distance <= 1.3f * s)
		{
			PlanTicTac(o);
			return true;
		}
		return false;
	}

	// A top low enough to step onto and deep enough to stand on: half a
	// metre takes the feet where PlanStepOn puts them, 22 cm past the edge.
	bool StepsOnto(const Obstacle& o) const
	{
		float s = m_Proportions.Scale;
		return o.Top - o.Ground <= 0.80f * s && o.Depth >= 0.45f * s && o.Beyond >= o.Ground - 0.3f * s;
	}

	// Running, jump, a tall wall alongside: onto it.
	bool TryWallRun(const glm::vec3& forward)
	{
		bool jump = m_WantsJump || m_JumpPending;
		bool running = m_Control.Enabled && m_Control.Run && m_RunBlend > 0.5f;
		// From the jump's crouch too: the press that missed the wall on its
		// first step (a sense ray a hair short) found it on the next, by when
		// the crouch had begun and the wall run was refused -- the press went
		// off as a hop alongside the wall, which is most of why wall runs
		// "hardly work".
		bool onGround = m_Air == Air::Grounded || m_Air == Air::Crouching;
		if (!jump || !running || !onGround || m_Speed < 0.5f * m_RunSpeed)
			return false;
		SideWall w = SenseSideWall(forward);
		if (!w.Found)
			return false;
		PlanWallRun(w);
		m_WallRunCorners = 0;
		return true;
	}

	// A move is the pelvis's path -- sampled finely, so a projectile is a
	// projectile -- and limb keys (GetUpKey, in seconds while it is built).
	struct TrackPoint
	{
		float T = 0.0f;
		glm::vec3 Position{ 0.0f };
		glm::quat Orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
	};
	struct Way
	{
		float T = 0.0f;
		glm::vec3 Position{ 0.0f };
		glm::quat Orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
		bool Ballistic = false;   // to the next way under gravity alone
	};

	// Ways to a fine track: between two, a Hermite curve whose tangents
	// come from the neighbours (Catmull-Rom over uneven times), the first
	// leaving at the velocity he had and the last arriving at the one he
	// will walk away with -- so a move starts and ends without a jolt. A
	// ballistic stretch is the parabola itself.
	void BuildTrack(const std::vector<Way>& ways, const glm::vec3& velocityIn, const glm::vec3& velocityOut)
	{
		m_Track.clear();
		size_t n = ways.size();
		if (n < 2)
			return;
		auto tangent = [&](size_t i)
		{
			if (i == 0) return velocityIn;
			if (i == n - 1) return velocityOut;
			return (ways[i + 1].Position - ways[i - 1].Position) / std::max(ways[i + 1].T - ways[i - 1].T, 1e-3f);
		};
		for (size_t i = 0; i + 1 < n; i++)
		{
			const Way& a = ways[i];
			const Way& b = ways[i + 1];
			float span = std::max(b.T - a.T, 1e-3f);
			int samples = std::max(1, (int)std::ceil(span * 60.0f));
			glm::vec3 m0 = tangent(i) * span, m1 = tangent(i + 1) * span;
			glm::vec3 launch(0.0f);
			if (a.Ballistic)
			{
				// Leaving a so as to reach b in `span` under g.
				launch = (b.Position - a.Position) / span;
				launch.y += 0.5f * 9.81f * span;
			}
			for (int k = (i == 0 ? 0 : 1); k <= samples; k++)
			{
				float u = (float)k / (float)samples;
				TrackPoint p;
				p.T = a.T + span * u;
				if (a.Ballistic)
				{
					float t = span * u;
					p.Position = a.Position + launch * t + glm::vec3(0.0f, -0.5f * 9.81f * t * t, 0.0f);
				}
				else
				{
					float u2 = u * u, u3 = u2 * u;
					p.Position = (2 * u3 - 3 * u2 + 1) * a.Position + (u3 - 2 * u2 + u) * m0
						+ (-2 * u3 + 3 * u2) * b.Position + (u3 - u2) * m1;
				}
				float w = span > 0.1f ? glm::smoothstep(0.0f, 1.0f, u) : u;
				p.Orientation = glm::normalize(glm::slerp(a.Orientation, b.Orientation, w));
				m_Track.push_back(p);
			}
		}
	}

	// What a ballistic way leaves with, for the way before it to arrive at.
	static glm::vec3 LaunchVelocity(const Way& a, const Way& b)
	{
		float span = std::max(b.T - a.T, 1e-3f);
		glm::vec3 v = (b.Position - a.Position) / span;
		v.y += 0.5f * 9.81f * span;
		return v;
	}

	// The pose he has now, as the first key.
	GetUpKey KeyNow() const
	{
		GetUpKey k;
		k.T = 0.0f;
		k.Pelvis = m_World.GetBody(m_Pelvis).Position;
		k.Orientation = m_World.GetBody(m_Pelvis).Orientation;
		glm::vec3 F(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		for (int side = 0; side < 2; side++)
		{
			k.Hand[side] = 1;
			k.HandAt[side] = WristWorld(side);
			k.Foot[side] = AnkleWorld(side);
			k.Pole[side] = F;
		}
		k.Heading = m_Facing;
		return k;
	}

	// The joints a person opens for these and the walk keeps tight: the hip
	// flexes to about 120 degrees with the knee bent (the rig's cone is 55,
	// which is about where a straight leg stops), the back rounds, the arm
	// goes overhead (170). Opened for the move or the get-up, back as built after.
	// Closed, they close as the limbs come in (EaseJointsClosed) rather than
	// at once: a cone shut on a limb still outside it shoves the limb in
	// within a step, and an arm still up from a climb went from 170 degrees
	// of room to 75 in one.
	void OpenJoints(bool open)
	{
		StoreCones();
		m_JointsClosing = !open;
		if (!open)
			return;
		for (int side = 0; side < 2; side++)
			m_World.GetJoint(m_HipJoint[side]).ConeAngle = glm::radians(118.0f);
		m_World.GetJoint(m_SpineJoint).ConeAngle = glm::radians(60.0f);
		// Overhead, for hanging and catching an edge: an arm straight up is
		// 180 degrees from hanging at the side. At 120 the hands of a hang
		// stopped 19-25 cm short of the edge they were meant to hold.
		ShoulderCone(170.0f);
	}

	void StoreCones()
	{
		if (m_ConesStored)
			return;
		for (int side = 0; side < 2; side++)
			m_HipConeBuilt[side] = m_World.GetJoint(m_HipJoint[side]).ConeAngle;
		m_SpineCone = m_World.GetJoint(m_SpineJoint).ConeAngle;
		m_SpineTwistBuilt[0] = m_World.GetJoint(m_SpineJoint).TwistLower;
		m_SpineTwistBuilt[1] = m_World.GetJoint(m_SpineJoint).TwistUpper;
		m_ConesStored = true;
	}

	// Each step while closing: a cone no tighter than its limb is now (and
	// a little), so it never shoves it, and narrowing by at least 45 degrees
	// a second, so a limb its motor holds out is brought in gently rather
	// than left with room it should not have. Done once all are as built.
	void EaseJointsClosed(float dt)
	{
		if (!m_JointsClosing)
			return;
		StoreCones();
		bool done = true;
		auto ease = [&](GS::PhysicsWorld3D::JointHandle handle, float target)
		{
			GS::Joint3D& joint = m_World.GetJoint(handle);
			float next = std::max(target, std::min(joint.SwingAngle + glm::radians(3.0f), joint.ConeAngle - glm::radians(45.0f) * dt));
			joint.ConeAngle = std::min(joint.ConeAngle, next);
			done = done && joint.ConeAngle <= target + 1e-4f;
		};
		for (int side = 0; side < 2; side++)
		{
			ease(m_HipJoint[side], m_HipConeBuilt[side]);
			ease(m_ShoulderJoint[side], glm::radians(75.0f));
		}
		ease(m_SpineJoint, m_SpineCone);
		// The spine's twist the same way, each end of its range.
		{
			GS::Joint3D& spine = m_World.GetJoint(m_SpineJoint);
			float step = glm::radians(45.0f) * dt, margin = glm::radians(3.0f);
			float upper = std::max(m_SpineTwistBuilt[1], std::min(spine.TwistAngle + margin, spine.TwistUpper - step));
			float lower = std::min(m_SpineTwistBuilt[0], std::max(spine.TwistAngle - margin, spine.TwistLower + step));
			spine.TwistUpper = std::min(spine.TwistUpper, upper);
			spine.TwistLower = std::max(spine.TwistLower, lower);
			done = done && spine.TwistUpper <= m_SpineTwistBuilt[1] + 1e-4f && spine.TwistLower >= m_SpineTwistBuilt[0] - 1e-4f;
		}
		m_JointsClosing = !done;
	}

	// Closed now, for a body put back in its built pose (Respawn).
	void CloseJointsNow()
	{
		StoreCones();
		for (int side = 0; side < 2; side++)
			m_World.GetJoint(m_HipJoint[side]).ConeAngle = m_HipConeBuilt[side];
		OpenSpine(false);
		ShoulderCone(75.0f);
		m_JointsClosing = false;
	}

	// Common to every move once it is planned: the keys' seconds become
	// fractions, the feet are let go of (the walk pins them), and the mode
	// is the move's.
	// `chainFrom` is when, in seconds, he is on his feet enough for the next
	// move to start from here rather than from the walk.
	void BeginMove(Move kind, float total, float exitSpeed, float chainFrom)
	{
		m_ChainFrom = chainFrom;
		m_SpeedVault = false;
		m_Air = Air::Grounded;
		for (GetUpKey& key : m_GetUpKeys)
			key.T = glm::clamp(key.T / std::max(total, 1e-3f), 0.0f, 1.0f);
		m_GetUpKeys.back().T = 1.0f;
		m_Move = kind;
		m_MoveTime = total;
		m_MoveTimer = 0.0f;
		m_MoveExit = exitSpeed;
		m_TrackIndex = 0;
		m_FloorLift = 0.0f;
		LocoRelease();
		m_Mode = Mode::Moving;
		m_Blocked = glm::vec3(0.0f);
		m_WantsJump = false;
		m_JumpPending = false;
		UnpinFeet();
		OpenJoints(true);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);
	}

	// A frame (origin on the ground under the hips, F ahead, R to the right)
	// that the planners lay their points out in.
	struct Frame
	{
		glm::vec3 O{ 0.0f }, F{ 0.0f, 0.0f, 1.0f }, R{ 1.0f, 0.0f, 0.0f };
		glm::vec3 At(float along, float across, float y) const { return glm::vec3(O.x, y, O.z) + F * along + R * across; }
		glm::quat Lean(float degrees) const
		{
			const glm::vec3 up(0.0f, 1.0f, 0.0f);
			float r = glm::radians(degrees);
			return PelvisFacing(up * std::cos(r) + F * std::sin(r), F * std::cos(r) - up * std::sin(r));
		}
	};
	Frame FrameHere() const
	{
		Frame f;
		f.O = m_World.GetBody(m_Pelvis).Position;
		f.F = glm::vec3(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		f.R = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), f.F);
		return f;
	}
	static float SideSign(int side) { return side == 0 ? -1.0f : 1.0f; }
	int NextLead() { return (int)((m_Seed + m_GetUpCount++) & 1u); }

	// --- the wall run ---------------------------------------------------
	//
	// Running beside a wall that is taller than he is and goes on, jump
	// takes him onto it: a few steps along the face with the feet on the
	// wall, the body tipped away from it so the legs reach it. Up the
	// wall he is a projectile under part of g only -- the rest is carried
	// by friction at the feet, as much as the legs push into the wall;
	// taken as 45% carried, so a run at 2.6 m/s up rises about 0.75 m and
	// is over in under a second. Jump again and he kicks off it, out and
	// on at 45 degrees; at its end, or where the wall ends, he comes off
	// it -- both through the drop, so the landing absorbs or rolls.
	struct SideWall
	{
		bool Found = false;
		glm::vec3 Normal{ 0.0f };   // out of the wall, towards him
		float Distance = 0.0f;      // pelvis to wall
		float Length = 0.0f;        // how far it goes on ahead
	};

	SideWall SenseSideWall(const glm::vec3& F) const
	{
		SideWall best;
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale;
		float g = GroundY(pelvis.Position, -1000.0f);
		glm::vec3 R = glm::cross(up, F);
		auto fixedWall = [&](const GS::PhysicsWorld3D::RayHit& hit)
		{
			return hit.Hit && m_World.GetBody(hit.Body).Type == GS::BodyType::Static && std::fabs(hit.Normal.y) < 0.3f;
		};
		for (float side : { -1.0f, 1.0f })
		{
			glm::vec3 at(pelvis.Position.x, g + 1.1f * s, pelvis.Position.z);
			// A metre and a quarter: at 1.0 a wall a stride's sway off was
			// found on one step and not the next.
			GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(at, R * side, 1.25f * s, m_CharacterBodies);
			if (!fixedWall(hit) || std::fabs(glm::dot(hit.Normal, F)) > 0.5f)
				continue;
			// Taller than he is.
			GS::PhysicsWorld3D::RayHit high = m_World.Raycast(at + up * (0.9f * s), R * side, 1.3f * s, m_CharacterBodies);
			if (!fixedWall(high))
				continue;
			glm::vec3 n = glm::normalize(glm::vec3(hit.Normal.x, 0.0f, hit.Normal.z));
			glm::vec3 D = glm::normalize(F - n * glm::dot(F, n));
			float length = 0.0f;
			for (int k = 1; k <= 12; k++)
			{
				glm::vec3 p = at + D * (0.35f * s * (float)k);
				if (!fixedWall(m_World.Raycast(p, -n, hit.Distance + 0.5f * s, m_CharacterBodies)))
					break;
				length = 0.35f * s * (float)k;
			}
			if (length < 1.0f * s)
				continue;
			if (!best.Found || hit.Distance < best.Distance)
			{
				best.Found = true;
				best.Normal = n;
				best.Distance = hit.Distance;
				best.Length = length;
			}
		}
		return best;
	}

	// `vy0`, when given, is the speed up the wall he starts with -- from a
	// corner, what he had on the last wall.
	void PlanWallRun(const SideWall& w, float vy0Given = -1e9f)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 n = w.Normal;
		glm::vec3 D = glm::normalize(fr.F - n * glm::dot(fr.F, n));
		float v = std::max(m_Speed, 2.2f * rs);
		float carried = 0.45f, gw = 9.81f * (1.0f - carried);
		float vy0 = vy0Given > -1e8f ? vy0Given : 2.6f * rs;
		float T = std::min(0.85f * rs, std::max(w.Length - 0.3f * s, 0.3f * s) / v);
		float off = 0.50f * s;   // the hips this far out from the face
		glm::vec3 P0 = pelvis.Position;
		// The face under him, at the hips' height.
		glm::vec3 face = P0 - n * (w.Distance);
		float heading = std::atan2(D.x, D.z);
		glm::quat tipped = PelvisFacing(up * std::cos(glm::radians(24.0f)) + n * std::sin(glm::radians(24.0f)), D);

		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation });
		const int samples = 24;
		for (int k = 1; k <= samples; k++)
		{
			float t = T * (float)k / (float)samples;
			float in = glm::smoothstep(0.0f, 1.0f, t / (0.18f * rs));
			glm::vec3 along = face + D * (v * t) + n * glm::mix(w.Distance, off, in);
			along.y = P0.y + vy0 * t - 0.5f * gw * t * t;
			ways.push_back({ t, along, glm::normalize(glm::slerp(pelvis.Orientation, tipped, in)) });
		}
		BuildTrack(ways, fr.F * m_Speed + up * vy0, D * v + up * (vy0 - gw * T));

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		// Steps on the wall, one foot then the other, each planted where
		// the hips will be over a quarter of a step on.
		float step = 0.24f * rs;
		int wallSide = glm::dot(fr.R, -n) > 0.0f ? 1 : 0;   // the foot nearer the wall
		int foot = wallSide;
		auto hipsAt = [&](float t)
		{
			glm::vec3 p = face + D * (v * t) + n * off;
			p.y = P0.y + vy0 * t - 0.5f * gw * t * t;
			return p;
		};
		GetUpKey key = now;
		for (float t = 0.5f * step; t < T; t += step)
		{
			key.T = t;
			glm::vec3 hips = hipsAt(std::min(t + 0.25f * step, T));
			glm::vec3 plant = face + D * glm::dot(hips - face, D) + D * (0.12f * s);
			plant.y = hips.y - M.HipDrop - 0.62f * (M.Thigh + M.Shin);
			key.Foot[foot] = plant + n * (m_AnkleHeight * 0.8f);
			key.Pole[foot] = glm::normalize(D + up * 0.3f);
			key.Kneeling[foot] = true;
			// The other comes off the wall and through.
			int other = 1 - foot;
			glm::vec3 swing = hips - up * (M.HipDrop + 0.45f * (M.Thigh + M.Shin)) + n * (0.10f * s) + D * (0.10f * s);
			key.Foot[other] = swing;
			key.Pole[other] = D;
			key.Kneeling[other] = true;
			// The wall-side hand along the face, the other out for balance.
			key.Hand[wallSide] = 0;
			key.HandAt[wallSide] = hips + up * (0.55f * s) + D * (0.30f * s) - n * (off - 0.12f * s);
			key.Hand[1 - wallSide] = 0;
			key.HandAt[1 - wallSide] = hips + up * (0.45f * s) + n * (0.55f * s) - D * (0.10f * s);
			key.Spine = glm::radians(8.0f);
			key.Heading = heading;
			m_GetUpKeys.push_back(key);
			foot = other;
		}
		key.T = T;
		m_GetUpKeys.push_back(key);

		m_WallNormal = n;
		m_WallRunDir = D;
		m_WallRunSpeed = v;
		m_WallRunFrom = w.Distance;
		m_Facing = heading;
		BeginMove(Move::WallRun, T, v, T + 1.0f);
	}

	// Off the wall: kicked away from it if `jump`, else just on along it,
	// either way falling to whatever is below.
	void LeaveWall(bool jump)
	{
		float rs = std::sqrt(m_Proportions.Scale);
		glm::vec3 dir = jump ? glm::normalize(m_WallRunDir * 0.75f + m_WallNormal * 0.66f) : m_WallRunDir;
		m_Facing = std::atan2(dir.x, dir.z);
		m_Speed = m_WallRunSpeed * (jump ? 0.9f : 1.0f);
		float vy = jump ? 3.2f * rs : m_World.GetBody(m_Pelvis).Velocity.y;
		m_WantsJump = false;
		if (!PlanDrop(vy))
		{
			OpenJoints(false);
			GoRagdoll("off the wall");
		}
	}

	// --- the cat leap ---------------------------------------------------
	//
	// A ledge too far to climb from where he is, but in a running jump's
	// reach: off one foot, a projectile to where the hands meet the edge
	// with the arms straight -- the feet first to the wall, knees bent to
	// take it -- and into the hang. Up to 0.8 m of rise, in his scale; the
	// flight is as long as the distance at his speed takes, and the apex
	// is where the projectile through those two points puts it.
	bool PlanCatLeap(const Obstacle& o)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float H = o.Top;
		float hangY = H - 0.92f * M.Arm - (M.Trunk - M.HipDrop);
		glm::vec3 P0 = pelvis.Position;
		float rise = hangY - P0.y;
		if (rise > 0.8f * s || rise < -1.5f * s)
			return false;
		// Only to an edge he would have to jump for (PlanClimb's test): run
		// at a crate a metre high and jump, and it was a leap into a "hang"
		// with the hips below the floor, then a climb.
		if (hangY <= o.Ground + m_StandClearance - 0.05f * s)
			return false;
		float v = std::max(m_Speed, 2.0f * rs);
		float tC = 0.10f * rs;                              // the plant and push
		glm::vec3 takeoff = P0 + fr.F * (v * tC);
		takeoff.y = P0.y - 0.05f * s;
		glm::vec3 catchAt = fr.At(o.Distance - 0.27f * s, 0.0f, hangY);
		float flat = std::max(glm::dot(catchAt - takeoff, fr.F), 0.3f * s);
		float T = glm::clamp(flat / v, 0.30f * rs, 0.80f * rs);
		// Arriving no faster upwards than it can stop on the hands: the apex
		// at or after the catch, else a longer flight.
		float vyArrive = (catchAt.y - takeoff.y) / T - 0.5f * 9.81f * T;
		if (vyArrive > 0.5f)
			T = std::min(T * 1.3f, 0.95f * rs);

		m_WallNormal = o.Normal.y > 0.9f ? -fr.F : glm::normalize(glm::vec3(o.Normal.x, 0.0f, o.Normal.z));
		m_HangTop = H;
		m_HangEdge = fr.At(o.Distance, 0.0f, H);
		if (!EdgeAt(m_HangEdge + fr.F * (0.10f * s)))
			return false;

		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation });
		ways.push_back({ tC, takeoff, fr.Lean(14.0f), true });
		ways.push_back({ tC + T, catchAt, fr.Lean(4.0f) });
		BuildTrack(ways, fr.F * m_Speed, glm::vec3(0.0f));
		RetimeIntoLaunch(0, ways, LaunchVelocity(ways[1], ways[2]));

		int lead = NextLead();
		m_GetUpLead = lead;
		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey push = now;
		push.T = tC;
		push.Foot[lead] = takeoff + fr.R * (SideSign(lead) * M.Stance) - up * (takeoff.y - (o.Ground + m_AnkleHeight)) - fr.F * (0.15f * s);
		m_GetUpKeys.push_back(push);
		// Reaching: arms up and forward, knees coming up.
		GetUpKey reach = push;
		reach.T = tC + 0.55f * T;
		reach.LocalFeet = true;
		for (int side = 0; side < 2; side++)
		{
			reach.Hand[side] = 0;
			reach.HandAt[side] = m_HangEdge + fr.R * (SideSign(side) * M.ShoulderX) - fr.F * (0.10f * s);
			reach.HandAt[side].y = H + 0.12f * s;
			reach.Foot[side] = glm::vec3(SideSign(side) * M.HipX, -(M.HipDrop + 0.55f * (M.Thigh + M.Shin)), 0.22f * s);
			reach.Pole[side] = glm::vec3(0.0f, 0.0f, 1.0f);
			reach.Kneeling[side] = true;
		}
		reach.Spine = glm::radians(8.0f);
		m_GetUpKeys.push_back(reach);
		// The catch: hands on the edge, feet on the wall below.
		GetUpKey caught = reach;
		caught.T = tC + T;
		caught.LocalFeet = false;
		for (int side = 0; side < 2; side++)
		{
			caught.HandAt[side] = m_HangEdge + fr.R * (SideSign(side) * M.ShoulderX) + fr.F * (0.10f * s);
			caught.HandAt[side].y = H + M.HandH;
			caught.Foot[side] = m_HangEdge + fr.R * (SideSign(side) * M.HipX) - fr.F * (0.18f * s);
			caught.Foot[side].y = hangY - M.HipDrop - 0.75f * (M.Thigh + M.Shin);
			caught.Pole[side] = -fr.F;
		}
		caught.Spine = glm::radians(4.0f);
		m_GetUpKeys.push_back(caught);
		BeginMove(Move::CatLeap, tC + T, 0.0f, tC + T + 1.0f);
		return true;
	}

	// --- the tic-tac ----------------------------------------------------
	//
	// Running at a wall met at a slant, too tall to get over: a step onto
	// it at hip height, and a push off it that sends him away mirrored in
	// the wall -- the way a ball comes off it, slower -- and up, half a
	// metre or so. The landing is the drop's.
	void PlanTicTac(const Obstacle& o)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 n = glm::normalize(glm::vec3(o.Normal.x, 0.0f, o.Normal.z));
		float v = std::max(m_Speed, 2.2f * rs);
		// Where the foot meets the wall: the face, along his way, at hip height.
		glm::vec3 hit = fr.At(o.Distance, 0.0f, o.Ground);
		float off = 0.55f * s;                                 // the hips that far out from it at the push
		float into = std::max(-glm::dot(fr.F, n), 0.25f);
		float approach = std::max(o.Distance - off / into, 0.0f);
		float tA = std::max(approach / v, 0.10f * rs);
		float tP = 0.14f * rs;                                  // foot on the wall
		glm::vec3 P0 = pelvis.Position;
		glm::vec3 at = P0 + fr.F * approach;
		glm::vec3 pushAt = at + fr.F * (v * tP * 0.5f) + up * (0.18f * s);

		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation });
		ways.push_back({ tA, at + up * (0.04f * s), fr.Lean(10.0f) });
		ways.push_back({ tA + tP, pushAt, fr.Lean(4.0f) });
		BuildTrack(ways, fr.F * m_Speed, fr.F * v + up * (1.0f * rs));

		int lead = NextLead();
		m_GetUpLead = lead;
		// The foot nearer the wall goes on it.
		int foot = glm::dot(fr.R, -n) > 0.0f ? 1 : 0;
		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey plant = now;
		plant.T = tA;
		glm::vec3 onWall = hit + up * (P0.y - M.HipDrop - 0.35f * (M.Thigh + M.Shin) - o.Ground) + n * (m_AnkleHeight * 0.8f);
		plant.Foot[foot] = onWall;
		plant.Pole[foot] = glm::normalize(fr.F + up * 0.4f);
		plant.Kneeling[foot] = true;
		plant.Hand[1 - foot] = 0;
		plant.HandAt[1 - foot] = at + up * (0.55f * s) + n * (0.3f * s) + fr.F * (0.15f * s);
		m_GetUpKeys.push_back(plant);
		GetUpKey push = plant;
		push.T = tA + tP;
		push.Foot[foot] = onWall + up * (0.08f * s);
		m_GetUpKeys.push_back(push);

		// Away, mirrored in the wall.
		glm::vec3 out = fr.F - 2.0f * glm::dot(fr.F, n) * n;
		out.y = 0.0f;
		out = glm::normalize(out);
		m_WallRunDir = out;
		m_WallNormal = n;
		m_WallRunSpeed = 0.85f * v;
		BeginMove(Move::TicTac, tA + tP, 0.85f * v, tA + tP + 1.0f);
	}

	// --- the precision jump ----------------------------------------------
	//
	// Standing or walking at the edge of something with a gap ahead and a
	// top beyond it near his height: jump, and it is a two-footed jump to
	// that top -- a projectile whose apex clears both by a third of a metre
	// -- landed with the feet together and stuck, knees taking it, no step.
	// The landing spot is a little past the far edge, or the middle of the
	// top if it is narrow.
	bool TryPrecision(const glm::vec3& forward)
	{
		bool jump = m_WantsJump || m_JumpPending;
		if (!jump || m_Air != Air::Grounded || (m_Control.Enabled && m_Control.Run && m_Speed > 0.5f * m_RunSpeed))
			return false;
		float s = m_Proportions.Scale;
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float g0 = GroundY(pelvis.Position, -1000.0f);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		// Down rays ahead: the gap, then the far top, its near edge and
		// where it ends.
		float gapAt = -1.0f, nearAt = -1.0f, farAt = -1.0f, topY = 0.0f;
		for (int k = 1; k <= 30; k++)
		{
			float d = 0.10f * s * (float)k;
			glm::vec3 q = pelvis.Position + forward * d;
			q.y = g0 + 1.0f * s;
			GS::PhysicsWorld3D::RayHit down = m_World.Raycast(q, -up, 3.0f * s, m_CharacterBodies);
			float y = down.Hit ? down.Point.y : -1e9f;
			if (gapAt < 0.0f)
			{
				if (y < g0 - 0.5f * s)
					gapAt = d;
				continue;
			}
			if (nearAt < 0.0f)
			{
				if (y > g0 - 0.8f * s && y < g0 + 0.6f * s)
				{
					nearAt = d;
					topY = y;
				}
				continue;
			}
			if (std::fabs(y - topY) > 0.1f * s)
			{
				farAt = d - 0.10f * s;
				break;
			}
		}
		if (gapAt < 0.0f || nearAt < 0.0f || nearAt - gapAt < 0.3f * s || nearAt > 2.6f * s)
			return false;
		if (farAt < 0.0f)
			farAt = nearAt + 0.6f * s;
		float landAt = std::min(nearAt + 0.30f * s, 0.5f * (nearAt + farAt));
		PlanPrecision(landAt, topY, g0);
		return true;
	}

	void PlanPrecision(float landAt, float topY, float g0)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float stand = m_StandClearance;
		glm::vec3 P0 = pelvis.Position;
		float tC = 0.22f * rs;                                   // the crouch
		glm::vec3 crouch = P0 + fr.F * (0.05f * s);
		crouch.y = g0 + stand - 0.16f * s;
		glm::vec3 land = fr.At(landAt, 0.0f, topY + stand - 0.08f * s);
		float apex = std::max(g0, topY) + stand + 0.30f * s;
		float tUp = std::sqrt(2.0f * std::max(apex - crouch.y, 0.05f) / 9.81f);
		float tDown = std::sqrt(2.0f * std::max(apex - land.y, 0.05f) / 9.81f);
		float T = tUp + tDown;
		float depth = glm::clamp(9.81f * tDown * 9.81f * tDown / (2.0f * 3.0f * 9.81f), 0.08f * s, 0.30f * s);
		float tAbs = 0.45f * rs;

		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation });
		ways.push_back({ tC, crouch, fr.Lean(20.0f), true });
		ways.push_back({ tC + T, land, fr.Lean(10.0f) });
		ways.push_back({ tC + T + 0.18f * rs, land - up * depth, fr.Lean(22.0f) });
		ways.push_back({ tC + T + tAbs, fr.At(landAt, 0.0f, topY + stand), fr.Lean(2.0f) });
		BuildTrack(ways, fr.F * m_Speed, glm::vec3(0.0f));
		RetimeIntoLaunch(0, ways, LaunchVelocity(ways[1], ways[2]));
		TiltTrack(tC, tC + T, fr.Lean(20.0f), fr.Lean(4.0f), fr.Lean(10.0f), tUp / T);

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey set = now;
		set.T = tC;
		for (int side = 0; side < 2; side++)
		{
			set.Foot[side] = fr.At(0.0f, SideSign(side) * M.HipX, g0 + m_AnkleHeight);
			set.Hand[side] = 0;
			set.HandAt[side] = crouch + fr.R * (SideSign(side) * 0.25f * s) - fr.F * (0.20f * s) - up * (0.10f * s);   // arms back
		}
		set.Spine = glm::radians(16.0f);
		m_GetUpKeys.push_back(set);
		GetUpKey fly = set;
		fly.T = tC + 0.5f * T;
		fly.LocalFeet = true;
		for (int side = 0; side < 2; side++)
		{
			fly.Foot[side] = glm::vec3(SideSign(side) * M.HipX * 0.8f, -(M.HipDrop + 0.75f * (M.Thigh + M.Shin)), 0.12f * s);
			fly.Pole[side] = glm::vec3(0.0f, 0.0f, 1.0f);
			fly.HandAt[side] = land + fr.R * (SideSign(side) * 0.3f * s) + up * (0.55f * s);   // arms swung forward
		}
		fly.Spine = glm::radians(4.0f);
		m_GetUpKeys.push_back(fly);
		GetUpKey stick = fly;
		stick.T = tC + T;
		stick.LocalFeet = false;
		for (int side = 0; side < 2; side++)
		{
			stick.Foot[side] = fr.At(landAt, SideSign(side) * M.HipX * 0.9f, topY + m_AnkleHeight);
			stick.Pole[side] = fr.F;
			stick.HandAt[side] = land + fr.R * (SideSign(side) * 0.3f * s) + fr.F * (0.25f * s) + up * (0.30f * s);
		}
		m_GetUpKeys.push_back(stick);
		GetUpKey stood = stick;
		stood.T = tC + T + tAbs;
		for (int side = 0; side < 2; side++)
			stood.Hand[side] = 1;
		stood.Spine = 0.0f;
		m_GetUpKeys.push_back(stood);
		BeginMove(Move::Precision, tC + T + tAbs, 0.0f, tC + T + tAbs);
	}

	// --- hanging -------------------------------------------------------
	//
	// From a ledge by the hands, the body hanging below with the arms all
	// but straight and the feet against the wall. Along the edge he goes
	// hand over hand: a hand that has fallen behind where it should be (a
	// shoulder's width either side of the hips) moves, lifted, while the
	// other holds -- never both at once. He stops where the edge stops.
	// Towards the wall, or jump, he climbs up from here (the climb from its
	// pull); away from it he lets go, and the drop has him. The keys are
	// his, not the camera's, while he hangs: forward is up, back is let go,
	// left and right are along the edge. Camera-relative, as the walk is,
	// they turned round with him when he dropped to a hang facing back the
	// way he came -- back climbed and forward let go.
	float HangY() const
	{
		const Measures M = BodyMeasures();
		return m_HangTop - 0.92f * M.Arm - (M.Trunk - M.HipDrop);
	}

	void BeginHang()
	{
		const Measures M = BodyMeasures();
		float s = m_Proportions.Scale;
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 F = -m_WallNormal;
		glm::vec3 R = glm::cross(up, F);
		m_Facing = std::atan2(F.x, F.z);
		m_Mode = Mode::Moving;
		m_Move = Move::Hang;
		m_MoveTimer = 0.0f;
		m_MoveTime = 1e9f;
		m_Track.clear();
		m_WantsJump = false;
		m_JumpPending = false;
		for (int side = 0; side < 2; side++)
		{
			m_HangHand[side] = m_HangEdge + R * (SideSign(side) * M.ShoulderX) + F * (0.10f * s);
			m_HangHand[side].y = m_HangTop + M.HandH;
			m_HangHandTimer[side] = -1.0f;
		}
		UnpinFeet();
		OpenJoints(true);
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Kinematic;
		pelvis.AngularVelocity = glm::vec3(0.0f);
	}

	// Is there edge to hold at `at` (on the top, just in from the face)?
	bool EdgeAt(const glm::vec3& at) const
	{
		float s = m_Proportions.Scale;
		GS::PhysicsWorld3D::RayHit down = m_World.Raycast(glm::vec3(at.x, m_HangTop + 0.3f * s, at.z), glm::vec3(0.0f, -1.0f, 0.0f), 0.5f * s, m_CharacterBodies);
		return down.Hit && std::fabs(down.Point.y - m_HangTop) < 0.08f * s;
	}

	void UpdateHang(float dt)
	{
		const Measures M = BodyMeasures();
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		glm::vec3 n = m_WallNormal, F = -n, R = glm::cross(up, F);
		m_MoveTimer += dt;

		// The keys are his while he hangs, not the camera's: forward climbs,
		// back lets go, right goes to his right -- round a corner too, where
		// keys read through a camera that has not turned with him would
		// point at the new wall, or away from it. R here (up x F) is his
		// left, which is why right is -R: read straight onto R, as it was,
		// D went to his left.
		bool on = m_Control.Enabled;
		float along = on ? (m_Control.Left ? 1.0f : 0.0f) - (m_Control.Right ? 1.0f : 0.0f) : 0.0f;   // + towards R
		float toward = on ? (m_Control.Forward ? 1.0f : 0.0f) - (m_Control.Back ? 1.0f : 0.0f) : 0.0f;
		bool jump = m_WantsJump;
		m_WantsJump = false;
		if ((jump || toward > 0.5f) && m_MoveTimer > 0.25f)
		{
			// Up from here.
			Obstacle o;
			GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
			o.Type = Obstacle::Climb;
			o.Distance = glm::dot(m_HangEdge - pelvis.Position, F);
			o.Normal = n;
			o.Ground = GroundY(pelvis.Position, -1000.0f);
			o.Top = m_HangTop;
			o.Depth = 1.2f * s;
			m_Speed = 0.0f;
			PlanClimb(o, true);
			return;
		}
		if (toward < -0.5f && m_MoveTimer > 0.25f)
		{
			// Let go.
			m_Speed = 0.0f;
			m_Move = Move::None;
			if (!PlanDrop())
			{
				OpenJoints(false);
				GoRagdoll("let go");
			}
			return;
		}

		// Along the edge, as far as there is edge for the leading hand --
		// and round an outside corner onto the next face, where the edge
		// turns with the wall: the face beside him, found by looking back
		// from past the end, becomes the one he hangs on.
		if (std::fabs(along) > 0.5f)
		{
			float dir = along > 0.0f ? 1.0f : -1.0f;
			glm::vec3 next = m_HangEdge + R * (dir * (M.ShoulderX + 0.22f * s)) + F * (0.10f * s);
			if (EdgeAt(next))
				m_HangEdge += R * (dir * 0.45f * rs * dt);
			else if (m_HangHandTimer[0] < 0.0f && m_HangHandTimer[1] < 0.0f)
			{
				glm::vec3 beyond = m_HangEdge + R * (dir * (M.ShoulderX + 0.45f * s)) + F * (0.25f * s);
				beyond.y = m_HangTop - 0.15f * s;
				GS::PhysicsWorld3D::RayHit side = m_World.Raycast(beyond, -R * dir, 0.6f * s + M.ShoulderX, m_CharacterBodies);
				if (side.Hit && std::fabs(side.Normal.y) < 0.3f && glm::dot(side.Normal, R * dir) > 0.7f)
				{
					glm::vec3 n2 = glm::normalize(glm::vec3(side.Normal.x, 0.0f, side.Normal.z));
					// The ray met the new face a quarter metre in from the
					// corner; the hands' middle goes a shoulder and a bit
					// further, so both have edge.
					glm::vec3 F2 = -n2;
					glm::vec3 edge2 = glm::vec3(side.Point.x, m_HangTop, side.Point.z) + F * (M.ShoulderX + 0.12f * s);
					if (EdgeAt(edge2 + F2 * (0.10f * s)))
					{
						// The feet go out round the corner first, clear of
						// both faces: carried straight from one face to the
						// other they cut through it and stuck, 40 cm in.
						glm::vec3 corner = glm::vec3(side.Point.x, 0.0f, side.Point.z) - F * (0.25f * s);
						m_HangCornerOut = corner + n * (0.45f * s) + R * (dir * 0.45f * s);
						m_HangCornerTimer = 0.0f;
						m_WallNormal = n2;
						m_HangEdge = edge2;
					}
				}
			}
		}

		// The hips under the hands, out from the wall by the chest's depth.
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 target = m_HangEdge - F * (0.27f * s);
		target.y = HangY();
		// Swaying a little with the hand that is moving.
		glm::vec3 to = target - pelvis.Position;
		pelvis.Velocity = to * std::min(1.0f / std::max(dt, 1e-4f), 10.0f);
		glm::quat hangTurn = PelvisFacing(up * std::cos(glm::radians(6.0f)) + F * std::sin(glm::radians(6.0f)), F);
		pelvis.Orientation = glm::normalize(glm::slerp(pelvis.Orientation, hangTurn, std::min(1.0f, 8.0f * dt)));
		pelvis.UpdateInertiaWorld();

		// Hand over hand.
		const float handTime = 0.20f * rs;
		GetUpKey k;
		k.Heading = m_Facing;
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 wanted = m_HangEdge + R * (SideSign(side) * M.ShoulderX) + F * (0.10f * s);
			wanted.y = m_HangTop + M.HandH;
			int other = 1 - side;
			float behind = glm::dot(wanted - m_HangHand[side], R);
			if (m_HangHandTimer[side] < 0.0f && m_HangHandTimer[other] < 0.0f && std::fabs(behind) > 0.10f * s)
			{
				m_HangHandTimer[side] = 0.0f;
				m_HangHandFrom[side] = m_HangHand[side];
				m_HangHandTo[side] = wanted + R * ((behind > 0.0f ? 1.0f : -1.0f) * 0.08f * s);
			}
			glm::vec3 hand = m_HangHand[side];
			if (m_HangHandTimer[side] >= 0.0f)
			{
				m_HangHandTimer[side] += dt;
				float u = glm::clamp(m_HangHandTimer[side] / handTime, 0.0f, 1.0f);
				hand = glm::mix(m_HangHandFrom[side], m_HangHandTo[side], glm::smoothstep(0.0f, 1.0f, u));
				hand.y += 0.06f * s * std::sin(glm::pi<float>() * u);
				if (u >= 1.0f)
				{
					m_HangHand[side] = m_HangHandTo[side];
					m_HangHandTimer[side] = -1.0f;
				}
			}
			k.Hand[side] = 0;
			k.HandAt[side] = hand;
			// Feet braced on the wall below the hips, knees out from it --
			// a climber's hang. Knees bent towards the wall went into it, a
			// shin 24 cm.
			glm::vec3 foot = m_HangEdge + R * (SideSign(side) * M.HipX) - F * (0.18f * s);
			float ground = GroundY(pelvis.Position, -1000.0f);
			foot.y = std::max(ground + m_AnkleHeight, target.y - M.HipDrop - 0.86f * (M.Thigh + M.Shin));
			if (m_HangCornerTimer >= 0.0f && m_HangCornerTimer < 0.35f * rs)
				foot = glm::vec3(m_HangCornerOut.x, foot.y, m_HangCornerOut.z) + R * (SideSign(side) * M.HipX);
			k.Foot[side] = foot;
			k.Pole[side] = -F;
			k.Kneeling[side] = true;
		}
		k.Spine = glm::radians(4.0f);
		DriveKeyLimbs(k, k, 0.0f, -1e9f);
		if (m_HangCornerTimer >= 0.0f)
			m_HangCornerTimer += dt;
	}

	// Walking off a high edge, not running: down onto the hands instead of
	// off -- a crouch at the edge, turning, lowering himself over it, hands
	// on the edge, into the hang. Asked for the edge he has just walked
	// off; false if there is no face below it to hang from.
	bool PlanDropToHang(float top)
	{
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		const Frame fr = FrameHere();
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		// The face he walked off, looking back at it from a little ahead.
		glm::vec3 from(pelvis.Position.x, top - 0.20f * s, pelvis.Position.z);
		from += fr.F * (0.30f * s);
		GS::PhysicsWorld3D::RayHit face = m_World.Raycast(from, -fr.F, 1.2f * s, m_CharacterBodies);
		if (!face.Hit || std::fabs(face.Normal.y) > 0.3f || m_World.GetBody(face.Body).Type != GS::BodyType::Static)
			return false;
		glm::vec3 n = glm::normalize(glm::vec3(face.Normal.x, 0.0f, face.Normal.z));
		m_WallNormal = n;
		m_HangTop = top;
		m_HangEdge = glm::vec3(face.Point.x, top, face.Point.z);
		if (!EdgeAt(m_HangEdge - n * (0.10f * s)))
			return false;
		glm::vec3 F = -n, R = glm::cross(up, F);
		float hangY = HangY();
		float t1 = 0.35f * rs, t2 = 0.60f * rs, t3 = 0.95f * rs;
		std::vector<Way> ways;
		ways.push_back({ 0.0f, pelvis.Position, pelvis.Orientation });
		// Crouched at the edge, turned side on.
		glm::vec3 crouch = m_HangEdge + n * (0.12f * s);
		crouch.y = top + 0.55f * s;
		glm::vec3 halfF = glm::normalize(R + n * 0.3f);
		ways.push_back({ t1, crouch, PelvisFacing(up * std::cos(0.5f) + halfF * std::sin(0.5f), halfF) });
		// Over it, face to the wall, hips at the edge.
		glm::vec3 over = m_HangEdge + n * (0.28f * s);
		over.y = top - 0.10f * s;
		ways.push_back({ t2, over, PelvisFacing(up * std::cos(0.35f) + F * std::sin(0.35f), F) });
		glm::vec3 hang = m_HangEdge + n * (0.27f * s);
		hang.y = hangY;
		ways.push_back({ t3, hang, PelvisFacing(up, F) });
		BuildTrack(ways, fr.F * m_Speed, glm::vec3(0.0f));

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey down = now;
		down.T = t1;
		for (int side = 0; side < 2; side++)
		{
			down.Hand[side] = 0;
			down.HandAt[side] = m_HangEdge + R * (SideSign(side) * M.ShoulderX) + F * (0.12f * s);
			down.HandAt[side].y = top + M.HandH;
			down.Foot[side] = m_HangEdge + R * (SideSign(side) * M.Stance) + F * (0.22f * s);
			down.Foot[side].y = top + m_AnkleHeight;
			down.Pole[side] = F;
		}
		down.Heading = std::atan2(F.x, F.z);
		down.Spine = glm::radians(20.0f);
		m_GetUpKeys.push_back(down);
		// The feet out over the edge before they go down past it: carried
		// straight from the top to below it, they went through the corner.
		GetUpKey outOver = down;
		outOver.T = 0.5f * (t1 + t2);
		for (int side = 0; side < 2; side++)
		{
			outOver.Foot[side] = m_HangEdge + R * (SideSign(side) * M.HipX) - F * (0.22f * s);
			outOver.Foot[side].y = top + m_AnkleHeight + 0.06f * s;
			outOver.Kneeling[side] = true;
		}
		m_GetUpKeys.push_back(outOver);
		GetUpKey lower = outOver;
		lower.T = t2;
		for (int side = 0; side < 2; side++)
		{
			lower.HandAt[side] = m_HangEdge + R * (SideSign(side) * M.ShoulderX) + F * (0.10f * s);
			lower.HandAt[side].y = top + M.HandH;
			lower.Foot[side] = m_HangEdge + R * (SideSign(side) * M.HipX) - F * (0.18f * s);
			lower.Foot[side].y = top - 0.55f * s;
			lower.Kneeling[side] = true;
			lower.Pole[side] = -F;
		}
		lower.Spine = glm::radians(10.0f);
		m_GetUpKeys.push_back(lower);
		GetUpKey hung = lower;
		hung.T = t3;
		for (int side = 0; side < 2; side++)
			hung.Foot[side].y = hangY - M.HipDrop - 0.86f * (M.Thigh + M.Shin);
		hung.Spine = glm::radians(4.0f);
		m_GetUpKeys.push_back(hung);
		m_Facing = std::atan2(F.x, F.z);
		BeginMove(Move::DropToHang, t3, 0.0f, t3 + 1.0f);
		return true;
	}

	// How far he could go along F from a point on the ground before
	// something fixed is in the way, at shin and chest height -- up to 4 m.
	// A roll is a body moving on the floor, carried, and it went straight
	// through a wall 2 m past a drop: asked before choosing one now.
	float RoomAhead(const glm::vec3& ground, const glm::vec3& F) const
	{
		float s = m_Proportions.Scale, room = 4.0f * s;
		for (float h : { 0.30f, 0.90f })
		{
			GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(ground + glm::vec3(0.0f, h * s, 0.0f), F, room, m_CharacterBodies);
			if (hit.Hit && hit.Normal.y < 0.6f && Solid(hit.Body))
				room = std::min(room, hit.Distance);
		}
		return room;
	}
	// What a roll covers, crouch to standing: about a turn round a body
	// 0.4 m high, and the steps either side.
	float RollRoom() const { return 3.0f * m_Proportions.Scale; }

	// --- the vault ------------------------------------------------------
	//
	// Up to the wall, a dip, and off the ground on both feet half a metre
	// short of it; hands on the top as the hips go up; knees tucked through
	// over it; hands off; down on the far side on one foot then the other,
	// absorbing, and away. The flight from take-off to landing is a
	// projectile: an apex half a metre over the top fixes the rise and the
	// fall, and so the time, and the hips cover the distance in that time
	// -- at least the wall's width and a landing, and most of the speed he
	// came in with if he has it.
	void PlanVault(const Obstacle& o)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		int lead = NextLead();
		m_GetUpLead = lead;
		float v = std::max(m_Speed, 1.4f * rs);
		float H = o.Top, g0 = o.Ground, gl = std::min(o.Beyond, H);
		float stand = m_StandClearance;

		// The speed vault, fast and over something no higher than his waist
		// and thin: one hand on the top, the body turned on its side over
		// it, legs out together past that hand -- the legs go round the
		// wall rather than up over it, so the hips need only a third of a
		// metre over the top, not half, and keep more of the speed. Hand and
		// side alternate like the lead foot.
		bool speed = m_Speed > 0.75f * m_RunSpeed && H - g0 <= 1.0f * s && o.Depth <= 0.5f * s;
		int hand = lead;
		float takeoff = std::max(0.0f, o.Distance - (speed ? 0.65f : 0.55f) * s);
		float tA = std::max(0.14f * rs, takeoff / v);
		float yA = g0 + stand - 0.07f * s;
		float apex = std::max(H + (speed ? 0.36f : 0.55f) * s, yA + 0.10f * s);
		float yL = gl + stand - 0.10f * s;
		float tUp = std::sqrt(2.0f * (apex - yA) / 9.81f);
		float tDown = std::sqrt(2.0f * (apex - yL) / 9.81f);
		float tF = tUp + tDown;
		float X = std::max(o.Distance + o.Depth + 0.62f * s - takeoff, (speed ? 0.92f : 0.8f) * v * tF);
		float vx = X / tF;
		// When the hips are clear of the far side: the legs stay tucked until
		// then. Reaching for the ground at a fixed share of the flight, at a
		// walk the heels came down onto the far edge, 12 cm into it.
		float tClear = (o.Distance + o.Depth + 0.24f * s - takeoff) / std::max(vx, 0.1f);
		float land = takeoff + X;
		// Landing: past about 4.5 m/s down (a vault onto lower ground) the
		// absorb goes as deep as v^2 / 2a at 3 g asks, and past the roll's
		// speed, with room ahead, he rolls out of it as from a drop.
		float impact = 9.81f * tDown;
		bool roll = impact >= m_RollSpeed * rs && RoomAhead(fr.At(land, 0.0f, gl), fr.F) > RollRoom();
		float depth = glm::clamp(impact * impact / (2.0f * 3.0f * 9.81f), 0.04f * s, 0.40f * s);
		bool deep = impact > 4.5f;
		float tLow = deep ? 2.0f * depth / impact : 0.0f;
		float tB = deep ? tLow + 0.40f * rs : 0.30f * rs;
		float total = tA + tF + tB;
		std::vector<Way> ways;
		ways.push_back({ 0.0f, fr.O, m_World.GetBody(m_Pelvis).Orientation });
		ways.push_back({ tA, fr.At(takeoff, 0.0f, yA), fr.Lean(22.0f), true });
		ways.push_back({ tA + tF, fr.At(land, 0.0f, yL), fr.Lean(6.0f) });
		auto shape = [&]()
		{
			// Arriving at the take-off already going up at the launch speed:
			// the Hermite into it was built from the neighbours, which knew
			// nothing of the jump. The hips' tilt over the top is put in
			// after, the flight being one ballistic way.
			RetimeIntoLaunch(0, ways, LaunchVelocity(ways[1], ways[2]));
			// On his side for a speed vault: rolled about his own front so
			// the hand's side goes down and the legs' side up.
			glm::quat peak = speed
				? glm::normalize(fr.Lean(14.0f) * glm::angleAxis(-glm::radians(40.0f) * SideSign(hand), glm::vec3(0.0f, 0.0f, 1.0f)))
				: fr.Lean(38.0f);
			TiltTrack(tA, tA + tF, fr.Lean(speed ? 12.0f : 22.0f), peak, fr.Lean(6.0f), tUp / tF);
		};
		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);

		GetUpKey reach = now;
		reach.T = 0.75f * tA;
		for (int side = 0; side < 2; side++)
		{
			reach.Hand[side] = 0;
			reach.HandAt[side] = fr.At(o.Distance + 0.10f * s, SideSign(side) * (M.ShoulderX + 0.02f * s), H + M.HandH + 0.05f * s);
			reach.Foot[side] = fr.At(takeoff - 0.12f * s, SideSign(side) * M.Stance, g0 + m_AnkleHeight);
		}
		reach.Spine = glm::radians(12.0f);
		m_GetUpKeys.push_back(reach);

		if (speed)
		{
			// One hand: the other goes out the far side for balance.
			reach.Hand[1 - hand] = 0;
			reach.HandAt[1 - hand] = fr.At(takeoff + 0.15f * s, SideSign(1 - hand) * (M.ShoulderX + 0.45f * s), yA + 0.45f * s);
			reach.HandAt[hand] = fr.At(o.Distance + 0.12f * s, SideSign(hand) * 0.10f * s, H + M.HandH + 0.05f * s);
			m_GetUpKeys.back() = reach;
		}

		GetUpKey plant = reach;
		plant.T = tA;
		for (int side = 0; side < 2; side++)
			if (!speed || side == hand)
				plant.HandAt[side].y = H + M.HandH;
		m_GetUpKeys.push_back(plant);

		// Tucked: knees up to the chest, in the hips' frame, toes soft.
		GetUpKey tuck = plant;
		tuck.T = tA + 0.30f * tF;
		tuck.LocalFeet = true;
		for (int side = 0; side < 2; side++)
		{
			tuck.Foot[side] = glm::vec3(SideSign(side) * M.HipX * 0.9f, -(M.HipDrop + 0.16f * s), 0.24f * s);
			tuck.Pole[side] = glm::normalize(glm::vec3(0.0f, 0.3f, 1.0f));
			tuck.Kneeling[side] = true;
			if (speed)
			{
				// Out together towards the legs' side (away from the hand),
				// nearly straight, one above the other.
				float out = -SideSign(hand);
				tuck.Foot[side] = glm::vec3(out * (0.50f + (side == hand ? 0.0f : 0.06f)) * s,
					-(M.HipDrop + (side == hand ? 0.30f : 0.22f) * s), 0.30f * s);
				tuck.Pole[side] = glm::normalize(glm::vec3(out * 0.4f, 0.4f, 1.0f));
			}
		}
		tuck.Spine = glm::radians(speed ? 6.0f : 22.0f);
		m_GetUpKeys.push_back(tuck);

		GetUpKey release = tuck;
		release.T = tA + std::min(0.58f * tF, 0.9f * tClear);
		release.Hand[0] = release.Hand[1] = 1;
		m_GetUpKeys.push_back(release);

		GetUpKey reachDown = release;
		reachDown.T = tA + glm::clamp(tClear, 0.80f * tF, 0.94f * tF);
		reachDown.LocalFeet = false;
		for (int side = 0; side < 2; side++)
		{
			float a = land + (side == lead ? 0.10f : -0.12f) * s;
			reachDown.Foot[side] = fr.At(a, SideSign(side) * M.Stance, gl + m_AnkleHeight + 0.08f * s);
			reachDown.Pole[side] = fr.F;
			reachDown.Kneeling[side] = false;
		}
		reachDown.Spine = glm::radians(6.0f);
		m_GetUpKeys.push_back(reachDown);

		GetUpKey down = reachDown;
		down.T = tA + tF;
		for (int side = 0; side < 2; side++)
			down.Foot[side].y = gl + m_AnkleHeight;
		m_GetUpKeys.push_back(down);

		if (roll)
		{
			Frame lf = fr;
			lf.O = fr.At(land, 0.0f, yL);
			PlanRoll(ways, tA + tF, lf, vx, lead, gl, fr.F * m_Speed, shape);
			return;
		}
		if (deep)
			ways.push_back({ tA + tF + tLow, fr.At(land + 0.5f * vx * tLow, 0.0f, gl + stand - depth), fr.Lean(10.0f + 50.0f * depth / s) });
		ways.push_back({ total, fr.At(land + 0.5f * vx * tLow + 0.75f * vx * (tB - tLow), 0.0f, gl + stand - 0.04f * s), fr.Lean(8.0f) });
		BuildTrack(ways, fr.F * m_Speed, fr.F * (0.95f * vx));
		shape();

		GetUpKey away = down;
		away.T = total;
		away.Spine = glm::radians(4.0f);
		m_GetUpKeys.push_back(away);

		BeginMove(Move::Vault, total, (speed ? 0.98f : 0.95f) * vx, tA + tF);
		m_SpeedVault = speed;
	}

	// Re-run the segment into way `i + 1` with the arrival velocity the
	// jump needs (the dip and the push), keeping the rest of the track.
	void RetimeIntoLaunch(size_t i, const std::vector<Way>& ways, const glm::vec3& launch)
	{
		const Way& a = ways[i];
		const Way& b = ways[i + 1];
		float span = std::max(b.T - a.T, 1e-3f);
		glm::vec3 m0 = (m_Track.size() > 1 ? (m_Track[1].Position - m_Track[0].Position) / std::max(m_Track[1].T - m_Track[0].T, 1e-4f) : glm::vec3(0.0f)) * span;
		glm::vec3 m1 = launch * span;
		for (TrackPoint& p : m_Track)
		{
			if (p.T < a.T || p.T > b.T)
				continue;
			float u = (p.T - a.T) / span, u2 = u * u, u3 = u2 * u;
			p.Position = (2 * u3 - 3 * u2 + 1) * a.Position + (u3 - 2 * u2 + u) * m0
				+ (-2 * u3 + 3 * u2) * b.Position + (u3 - u2) * m1;
		}
	}

	// The hips' tilt through a flight: from `a` to `peak` at fraction
	// `peakAt` of [t0, t1], then to `b`.
	void TiltTrack(float t0, float t1, const glm::quat& a, const glm::quat& peak, const glm::quat& b, float peakAt)
	{
		for (TrackPoint& p : m_Track)
		{
			if (p.T < t0 || p.T > t1)
				continue;
			float u = (p.T - t0) / std::max(t1 - t0, 1e-3f);
			p.Orientation = u < peakAt
				? glm::normalize(glm::slerp(a, peak, glm::smoothstep(0.0f, 1.0f, u / std::max(peakAt, 1e-3f))))
				: glm::normalize(glm::slerp(peak, b, glm::smoothstep(0.0f, 1.0f, (u - peakAt) / std::max(1.0f - peakAt, 1e-3f))));
		}
	}

	// --- stepping on ----------------------------------------------------
	//
	// Something between a step and a vault's height that goes on: the lead
	// foot up onto it (lifted over the edge, not dragged up the face), the
	// other hand on the top, the hips up over the foot, the back foot in.
	void PlanStepOn(const Obstacle& o)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		int lead = NextLead();
		m_GetUpLead = lead;
		float v = std::max(m_Speed, 0.8f * rs);
		float H = o.Top, g0 = o.Ground, stand = m_StandClearance;
		float f = o.Distance;
		float near = std::max(0.0f, f - 0.40f * s);
		float tA = std::max(0.12f * rs, near / v);
		float t1 = tA + 0.36f * rs, t2 = t1 + 0.42f * rs, total = t2 + 0.30f * rs;
		float exit = std::min(m_Speed, m_WalkSpeed) * 0.5f;

		std::vector<Way> ways;
		ways.push_back({ 0.0f, fr.O, m_World.GetBody(m_Pelvis).Orientation });
		ways.push_back({ tA, fr.At(near, 0.0f, g0 + stand - 0.03f * s), fr.Lean(10.0f) });
		ways.push_back({ t1, fr.At(f - 0.26f * s, 0.0f, g0 + stand - 0.05f * s), fr.Lean(24.0f) });
		ways.push_back({ t2, fr.At(f + 0.12f * s, SideSign(lead) * 0.3f * M.HipX, H + 0.88f * stand), fr.Lean(14.0f) });
		ways.push_back({ total, fr.At(f + 0.22f * s, 0.0f, H + stand), fr.Lean(4.0f) });
		BuildTrack(ways, fr.F * m_Speed, fr.F * exit);

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey approach = now;
		approach.T = tA;
		for (int side = 0; side < 2; side++)
			approach.Foot[side] = fr.At(near - 0.05f * s, SideSign(side) * M.Stance, g0 + m_AnkleHeight);
		approach.Hand[1 - lead] = 0;
		approach.HandAt[1 - lead] = fr.At(f + 0.10f * s, SideSign(1 - lead) * M.ShoulderX, H + M.HandH + 0.04f * s);
		m_GetUpKeys.push_back(approach);

		GetUpKey lift = approach;
		lift.T = 0.5f * (tA + t1);
		lift.Foot[lead] = fr.At(f - 0.08f * s, SideSign(lead) * M.Stance, H + m_AnkleHeight + 0.10f * s);
		lift.HandAt[1 - lead].y = H + M.HandH;
		m_GetUpKeys.push_back(lift);

		GetUpKey on = lift;
		on.T = t1;
		on.Foot[lead] = fr.At(f + 0.18f * s, SideSign(lead) * M.Stance, H + m_AnkleHeight);
		on.Spine = glm::radians(14.0f);
		m_GetUpKeys.push_back(on);

		GetUpKey trail = on;
		trail.T = 0.5f * (t1 + t2);
		trail.Foot[1 - lead] = fr.At(f - 0.08f * s, SideSign(1 - lead) * M.Stance, H + m_AnkleHeight + 0.12f * s);
		m_GetUpKeys.push_back(trail);

		GetUpKey up = trail;
		up.T = t2;
		up.Foot[1 - lead] = fr.At(f + 0.22f * s, SideSign(1 - lead) * M.Stance, H + m_AnkleHeight);
		up.Hand[0] = up.Hand[1] = 1;
		up.Spine = glm::radians(6.0f);
		m_GetUpKeys.push_back(up);

		GetUpKey stood = up;
		stood.T = total;
		for (int side = 0; side < 2; side++)
			stood.Foot[side] = fr.At(f + 0.22f * s, SideSign(side) * M.Stance, H + m_AnkleHeight);
		stood.Spine = 0.0f;
		m_GetUpKeys.push_back(stood);

		BeginMove(Move::StepOn, total, exit, t2);
	}

	// --- climbing up ----------------------------------------------------
	//
	// Close in; if the edge is out of reach standing, jump for it -- a
	// projectile that tops out where the hands are on the edge with the
	// arms straight; hang; pull until the chest is at the edge; press up on
	// straight arms, the trunk fairly upright; the lead foot up onto the
	// edge beside the hands, squatting on top; stand. A low ledge skips the
	// jump and the hang. (It pressed over with the trunk 55 degrees forward
	// and the hips 17 cm above the edge -- lying across the top, a belly
	// flop over it, which the owner saw on 2026-10-21. A climb as Assassin's
	// Creed has it is a mantle and a step up, and quicker.)
	// `fromHang`: he is hanging from this edge already, so the climb starts
	// at the pull.
	void PlanClimb(const Obstacle& o, bool fromHang = false)
	{
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		int lead = NextLead();
		m_GetUpLead = lead;
		float H = o.Top, g0 = o.Ground, stand = m_StandClearance;
		float f = o.Distance;
		float standY = g0 + stand;
		float shoulderAbove = M.Trunk - M.HipDrop;
		float hangY = H - 0.92f * M.Arm - shoulderAbove;
		float close = std::max(0.0f, f - 0.30f * s);
		float tA = fromHang ? 0.0f : std::max(0.15f * rs, close / std::max(m_Speed, 0.6f * rs));

		std::vector<Way> ways;
		ways.push_back({ 0.0f, fr.O, m_World.GetBody(m_Pelvis).Orientation });
		float t = tA;
		bool jump = !fromHang && hangY > standY - 0.05f * s;
		float tCatch = tA;
		// Where he is hanging, if he jumped to it: there he may stay (if
		// he is not pressing on, towards the wall), and hang.
		m_ClimbHangAt = -1.0f;
		m_HangEdge = fr.At(f, 0.0f, H);
		m_HangTop = H;
		m_WallNormal = -fr.F;
		if (fromHang)
		{
			// Already there.
		}
		else if (jump)
		{
			// Crouch, then the jump to the hang: up from the crouch so that
			// the apex is the hang.
			float crouchY = standY - 0.10f * s;
			ways.push_back({ t, fr.At(close, 0.0f, crouchY), fr.Lean(8.0f), true });
			float rise = std::max(hangY - crouchY, 0.02f * s);
			float tUp = std::sqrt(2.0f * rise / 9.81f);
			t += tUp;
			tCatch = t;
			ways.push_back({ t, fr.At(f - 0.30f * s, 0.0f, hangY), fr.Lean(4.0f) });
			t += 0.20f * rs;   // hanging, a moment
			m_ClimbHangAt = t;
			ways.push_back({ t, fr.At(f - 0.28f * s, 0.0f, hangY - 0.02f * s), fr.Lean(6.0f) });
		}
		else
		{
			ways.push_back({ t, fr.At(close, 0.0f, standY - 0.03f * s), fr.Lean(10.0f) });
			tCatch = t;
		}
		float pullFrom = ways.back().Position.y;
		float pullTo = H - 0.16f * s;
		float tPull = 0.42f * rs * glm::clamp((pullTo - pullFrom) / (0.6f * s), 0.3f, 1.3f);
		t += tPull;
		float tPulled = t;
		ways.push_back({ t, fr.At(f - 0.24f * s, 0.0f, pullTo), fr.Lean(15.0f) });
		// The mantle: up on straight arms, the hips just over the edge and
		// still in front of it, the trunk only a little forward.
		t += 0.28f * rs;
		float tPressed = t;
		ways.push_back({ t, fr.At(f - 0.10f * s, 0.0f, H + 0.12f * s), fr.Lean(25.0f) });
		// The lead foot up beside the hands: squatting on the top.
		t += 0.30f * rs;
		float tKnee = t;
		ways.push_back({ t, fr.At(f + 0.12f * s, 0.0f, H + 0.38f * s), fr.Lean(20.0f) });
		t += 0.34f * rs;
		float tRise = t;
		ways.push_back({ t, fr.At(f + 0.30f * s, SideSign(lead) * 0.3f * M.HipX, H + 0.86f * stand), fr.Lean(10.0f) });
		t += 0.22f * rs;
		float total = t;
		ways.push_back({ t, fr.At(f + 0.32f * s, 0.0f, H + stand), fr.Lean(3.0f) });
		BuildTrack(ways, fr.F * m_Speed, glm::vec3(0.0f));
		if (jump)
			RetimeIntoLaunch(0, ways, LaunchVelocity(ways[1], ways[2]));

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);

		// Hands up the face to above the edge, then onto it.
		GetUpKey up = now;
		up.T = std::max(0.6f * tCatch, tCatch - 0.15f * rs);
		for (int side = 0; side < 2; side++)
		{
			up.Hand[side] = 0;
			up.HandAt[side] = fr.At(f - 0.10f * s, SideSign(side) * M.ShoulderX, H + 0.10f * s);
			up.Foot[side] = fr.At(close - 0.06f * s, SideSign(side) * M.Stance, g0 + m_AnkleHeight);
		}
		up.Spine = glm::radians(6.0f);
		m_GetUpKeys.push_back(up);

		GetUpKey grip = up;
		grip.T = tCatch;
		for (int side = 0; side < 2; side++)
			grip.HandAt[side] = fr.At(f + 0.10f * s, SideSign(side) * M.ShoulderX, H + M.HandH);
		if (jump)
			for (int side = 0; side < 2; side++)
			{
				// Feet off the ground, against the wall.
				float y = std::max(g0 + m_AnkleHeight, hangY - M.HipDrop - 0.85f * (M.Thigh + M.Shin));
				grip.Foot[side] = fr.At(f - 0.18f * s, SideSign(side) * M.HipX, y);
				grip.Kneeling[side] = true;
				grip.Pole[side] = -fr.F;   // knees out from the wall
			}
		m_GetUpKeys.push_back(grip);

		// Half way up the pull, a foot walks up the wall and pushes: the
		// legs help the arms, as they do on a real wall. Lead foot first.
		GetUpKey push = grip;
		push.T = 0.5f * (tCatch + tPulled);
		{
			float mid = 0.5f * (pullFrom + pullTo);
			float y = std::max(g0 + m_AnkleHeight, mid - M.HipDrop - 0.55f * (M.Thigh + M.Shin));
			push.Foot[lead] = fr.At(f - 0.17f * s, SideSign(lead) * M.HipX, y);
			push.Pole[lead] = -fr.F;
			push.Kneeling[lead] = true;
		}
		m_GetUpKeys.push_back(push);

		GetUpKey pulled = push;
		pulled.T = tPulled;
		for (int side = 0; side < 2; side++)
		{
			// The other foot up beside it, both pushing.
			float y = std::max(g0 + m_AnkleHeight, pullTo - M.HipDrop - 0.62f * (M.Thigh + M.Shin));
			pulled.Foot[side] = fr.At(f - 0.17f * s, SideSign(side) * M.HipX, y);
			pulled.Kneeling[side] = true;
			pulled.Pole[side] = -fr.F;
		}
		pulled.Spine = glm::radians(12.0f);
		m_GetUpKeys.push_back(pulled);

		// Pressed up, the lead knee already coming up the wall towards the
		// edge: left low, both legs trailed out behind as the hips went
		// forward, and he lay across the top.
		GetUpKey pressed = pulled;
		pressed.T = tPressed;
		for (int side = 0; side < 2; side++)
		{
			float y = std::max(g0 + m_AnkleHeight, H - (side == lead ? 0.28f : 0.62f) * s);
			pressed.Foot[side] = fr.At(f - 0.12f * s, SideSign(side) * M.HipX, y);
		}
		pressed.Spine = glm::radians(8.0f);
		m_GetUpKeys.push_back(pressed);

		// The lead foot over the edge and onto the top, beside the hands, the
		// knee up by the chest.
		GetUpKey over = pressed;
		over.T = tPressed + 0.35f * (tKnee - tPressed);
		over.Foot[lead] = fr.At(f - 0.04f * s, SideSign(lead) * M.Stance, H + m_AnkleHeight + 0.12f * s);
		over.Kneeling[lead] = false;
		over.Pole[lead] = fr.F;
		m_GetUpKeys.push_back(over);

		GetUpKey footOn = over;
		footOn.T = tKnee;
		footOn.Foot[lead] = fr.At(f + 0.14f * s, SideSign(lead) * M.Stance, H + m_AnkleHeight);
		footOn.Spine = glm::radians(6.0f);
		m_GetUpKeys.push_back(footOn);

		GetUpKey trail = footOn;
		trail.T = 0.5f * (tKnee + tRise);
		trail.Foot[1 - lead] = fr.At(f - 0.04f * s, SideSign(1 - lead) * M.Stance, H + m_AnkleHeight + 0.14f * s);
		trail.Kneeling[1 - lead] = false;
		trail.Pole[1 - lead] = fr.F;
		m_GetUpKeys.push_back(trail);

		GetUpKey risen = trail;
		risen.T = tRise;
		risen.Foot[1 - lead] = fr.At(f + 0.24f * s, SideSign(1 - lead) * M.Stance, H + m_AnkleHeight);
		risen.Hand[0] = risen.Hand[1] = 1;
		risen.Spine = glm::radians(5.0f);
		m_GetUpKeys.push_back(risen);

		GetUpKey stood = risen;
		stood.T = total;
		for (int side = 0; side < 2; side++)
			stood.Foot[side] = fr.At(f + 0.32f * s, SideSign(side) * M.Stance, H + m_AnkleHeight);
		stood.Spine = 0.0f;
		m_GetUpKeys.push_back(stood);

		float hangAt = m_ClimbHangAt;
		BeginMove(Move::Climb, total, 0.0f, tRise);
		m_ClimbHangAt = hangAt;
	}

	// --- dropping, landing, rolling -------------------------------------
	//
	// Off an edge he falls: the hips are a projectile from where they are,
	// at the speed he was going, to where the ground under that path
	// catches them (found by asking where he will be when he gets there,
	// three times over). The legs reach for it, the arms go out. On the
	// ground he absorbs as deep as the speed asks, v^2 over a fixed
	// stopping distance -- and past m_RollSpeed (about a 2.2 m drop) he
	// turns it into a roll: over a shoulder, the trunk rounded and the knees
	// in, and up again running.
	// `launch`, when given, is the speed up he leaves with (a wall jump);
	// otherwise the hips' own.
	bool PlanDrop(float launch = -1e9f)
	{
		if (!ParkourOn())
			return false;
		const Frame fr = FrameHere();
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 P0 = pelvis.Position;
		glm::vec3 vh = fr.F * m_Speed;
		float vy = launch > -1e8f ? launch : glm::clamp(pelvis.Velocity.y, -6.0f, 3.0f);
		float stand = m_StandClearance;

		float T = 0.5f, gl = -1000.0f;
		for (int i = 0; i < 3; i++)
		{
			glm::vec3 x = P0 + vh * T;
			gl = GroundY(glm::vec3(x.x, P0.y, x.z), -1000.0f);
			float drop = P0.y - (gl + stand);
			if (gl < -999.0f || drop > 4.5f * s)
				return false;
			T = (vy + std::sqrt(std::max(vy * vy + 2.0f * 9.81f * std::max(drop, 0.0f), 0.0f))) / 9.81f;
		}
		T = std::max(T, 0.08f);
		float impact = std::fabs(vy - 9.81f * T);
		glm::vec3 L = P0 + vh * T;
		L.y = gl + stand;
		int lead = NextLead();
		m_GetUpLead = lead;

		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation, true });
		ways.push_back({ T, L, fr.Lean(-4.0f) });

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		// In the air: legs reaching for the ground, arms out for balance.
		GetUpKey air = now;
		air.T = 0.45f * T;
		air.LocalFeet = true;
		air.LocalHands = true;
		for (int side = 0; side < 2; side++)
		{
			air.Foot[side] = glm::vec3(SideSign(side) * M.HipX, -(M.HipDrop + 0.86f * (M.Thigh + M.Shin)), (side == lead ? 0.10f : -0.04f) * s);
			air.Pole[side] = glm::vec3(0.0f, 0.0f, 1.0f);
			air.Hand[side] = 0;
			air.HandAt[side] = glm::vec3(SideSign(side) * (M.ShoulderX + 0.45f * s), M.Trunk * 0.75f, 0.18f * s);
		}
		m_GetUpKeys.push_back(air);
		GetUpKey touch = air;
		touch.T = T;
		touch.LocalFeet = false;
		glm::vec3 landFr = L - fr.F * 0.0f;
		Frame lf = fr;
		lf.O = landFr;
		for (int side = 0; side < 2; side++)
			touch.Foot[side] = lf.At((side == lead ? 0.10f : -0.08f) * s, SideSign(side) * M.Stance, gl + m_AnkleHeight);
		for (int side = 0; side < 2; side++)
			touch.Pole[side] = fr.F;
		m_GetUpKeys.push_back(touch);

		float horizontal = glm::length(vh);
		if (impact < m_RollSpeed * rs || RoomAhead(glm::vec3(L.x, gl, L.z), fr.F) < RollRoom())
		{
			// Absorbed: as deep as v^2 / 2a for a stopping deceleration of
			// about 3 g, kept within a deep crouch.
			float depth = glm::clamp(impact * impact / (2.0f * 3.0f * 9.81f), 0.04f * s, 0.40f * s);
			float tDown = 2.0f * depth / std::max(impact, 0.5f);
			float tUp = 0.40f * rs;
			float exit = 0.8f * horizontal;
			// Carried on only as far as there is room: landing just short of
			// a wall, the absorb's 1.5 m of travel took the hips through it.
			float room = std::max(RoomAhead(glm::vec3(L.x, gl, L.z), fr.F) - 0.40f * s, 0.0f);
			float sink = std::min(horizontal * tDown * 0.5f, room);
			float on = std::min(exit * tUp * 0.6f, room - sink);
			if (on < exit * tUp * 0.6f)
				exit *= on / std::max(exit * tUp * 0.6f, 1e-3f);
			ways.push_back({ T + tDown, lf.At(sink, 0.0f, gl + stand - depth), lf.Lean(10.0f + 50.0f * depth / s) });
			ways.push_back({ T + tDown + tUp, lf.At(sink + on, 0.0f, gl + stand - 0.02f * s), lf.Lean(6.0f) });
			BuildTrack(ways, vh + up * vy, fr.F * exit);
			GetUpKey low = touch;
			low.T = T + tDown;
			low.LocalHands = false;
			for (int side = 0; side < 2; side++)
			{
				if (depth > 0.28f * s)
				{
					low.Hand[side] = 0;
					low.HandAt[side] = lf.At(0.45f * s, SideSign(side) * M.ShoulderX, gl + M.HandH);
				}
				else
					low.Hand[side] = 1;
				low.Foot[side] += fr.F * (0.0f);
			}
			low.Spine = glm::radians(10.0f + 30.0f * depth / s);
			m_GetUpKeys.push_back(low);
			GetUpKey stood = low;
			stood.T = T + tDown + tUp;
			stood.Hand[0] = stood.Hand[1] = 1;
			stood.Spine = 0.0f;
			m_GetUpKeys.push_back(stood);
			BeginMove(Move::Drop, T + tDown + tUp, exit, T + tDown);
			return true;
		}

		PlanRoll(ways, T, lf, horizontal, lead, gl, vh + up * vy);
		return true;
	}

	// How far below the pelvis's centre the lowest of him is, turned by `q`
	// with the back rounded by `bend` and the knees in: the pelvis, the
	// torso and head, the knees and feet of the tuck. A body rolling on a
	// floor has its centre that high above it.
	float LowestBelow(const glm::quat& q, float bend, bool tuck = true) const
	{
		float s = m_Proportions.Scale;
		const Proportions& P = m_Proportions;
		auto reach = [](const glm::quat& r, const glm::vec3& half)
		{
			glm::mat3 m = glm::mat3_cast(r);
			return std::fabs(m[0].y) * half.x + std::fabs(m[1].y) * half.y + std::fabs(m[2].y) * half.z;
		};
		float lowest = reach(q, P.PelvisHalf);
		glm::quat qt = q * glm::angleAxis(bend, glm::vec3(1.0f, 0.0f, 0.0f));
		glm::vec3 spine = q * glm::vec3(0.0f, 0.09f * s, 0.0f);
		glm::vec3 torso = spine + qt * glm::vec3(0.0f, 0.22f * s, 0.0f);
		lowest = std::max(lowest, -torso.y + reach(qt, P.TorsoHalf));
		glm::vec3 head = torso + qt * glm::vec3(0.0f, 0.26f * s, 0.10f * s);   // chin in
		lowest = std::max(lowest, -head.y + P.HeadRadius);
		if (!tuck)
			return lowest;
		const Measures M = BodyMeasures();
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 knee = q * glm::vec3(SideSign(side) * M.HipX, -M.HipDrop + 0.06f * s, 0.40f * s);
			glm::vec3 foot = q * glm::vec3(SideSign(side) * M.HipX * 0.9f, -(M.HipDrop + 0.16f * s), 0.24f * s);
			lowest = std::max(lowest, -knee.y + P.ThighRadius);
			lowest = std::max(lowest, -foot.y + P.FootHalf.y + 0.05f * s);
		}
		return lowest;
	}

	// From landing at T (the ways and keys up to it already laid down by the
	// drop or the vault), the crouch, the roll, and up. `shape` adjusts the
	// track once built, for the move that led here.
	void PlanRoll(std::vector<Way>& ways, float T, const Frame& lf, float horizontal, int lead, float gl,
		const glm::vec3& velocityIn, const std::function<void()>& shape = {})
	{
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		float stand = m_StandClearance;
		// Down into the crouch the roll starts from, hands reaching ahead.
		float tC = 0.14f * rs;
		float crouchY = gl + 0.55f * s;
		float into = std::max(horizontal, 1.2f * rs);
		ways.push_back({ T + tC, lf.At(into * tC * 0.6f, 0.0f, crouchY), lf.Lean(32.0f) });

		// The roll: round an axis across him, tipped a little towards the
		// lead shoulder, a full turn. The angle eases in and out; the hips
		// are as high as his lowest part lets them be, and travel what
		// rolling without slipping makes of that: d(along) = height * d(angle).
		float tR = 0.82f * rs;
		glm::vec3 axis = glm::normalize(lf.R + lf.F * (0.35f * SideSign(lead)));
		glm::quat start = lf.Lean(32.0f);
		float along = into * tC * 0.6f;
		const int samples = 48;
		float last = 0.0f;
		for (int k = 1; k <= samples; k++)
		{
			float u = (float)k / (float)samples;
			float phi = glm::two_pi<float>() * (u - 0.6f * std::sin(glm::two_pi<float>() * u) / glm::two_pi<float>());
			glm::quat q = glm::normalize(glm::angleAxis(phi, axis) * start);
			float bend = glm::radians(50.0f) * std::sin(glm::pi<float>() * std::min(1.0f, u * 1.3f));
			float height = LowestBelow(q, bend) + 0.01f * s;
			along += height * (phi - last);
			last = phi;
			ways.push_back({ T + tC + tR * u, lf.At(along, 0.0f, gl + height), q });
		}
		float tS = 0.34f * rs;
		float exit = std::max(0.85f * horizontal, 1.4f * rs);
		float endAlong = along + exit * tS * 0.5f;
		ways.push_back({ T + tC + tR + tS, lf.At(endAlong, 0.0f, gl + stand - 0.03f * s), lf.Lean(8.0f) });
		BuildTrack(ways, velocityIn, lf.F * exit);
		if (shape)
			shape();
		// The roll's ways are dense, so their tangents came out of the
		// neighbours; nothing to retime.

		GetUpKey crouch = m_GetUpKeys.back();
		crouch.T = T + tC;
		crouch.LocalHands = false;
		for (int side = 0; side < 2; side++)
		{
			crouch.Hand[side] = 0;
			crouch.HandAt[side] = lf.At(into * tC * 0.6f + 0.42f * s, SideSign(side) * M.ShoulderX * 0.9f, gl + M.HandH);
		}
		crouch.Spine = glm::radians(20.0f);
		m_GetUpKeys.push_back(crouch);

		GetUpKey ball = crouch;
		ball.T = T + tC + 0.18f * tR;
		ball.LocalFeet = ball.LocalHands = true;
		for (int side = 0; side < 2; side++)
		{
			ball.Foot[side] = glm::vec3(SideSign(side) * M.HipX * 0.9f, -(M.HipDrop + 0.16f * s), 0.24f * s);
			ball.Pole[side] = glm::normalize(glm::vec3(0.0f, 0.3f, 1.0f));
			ball.Kneeling[side] = true;
			ball.HandAt[side] = glm::vec3(SideSign(side) * M.HipX * 1.3f, -M.HipDrop + 0.10f * s, 0.42f * s);
		}
		ball.Spine = glm::radians(48.0f);
		m_GetUpKeys.push_back(ball);

		GetUpKey held = ball;
		held.T = T + tC + 0.70f * tR;
		held.Spine = glm::radians(40.0f);
		m_GetUpKeys.push_back(held);

		GetUpKey feet = held;
		feet.T = T + tC + 0.92f * tR;
		feet.LocalFeet = false;
		feet.LocalHands = false;
		for (int side = 0; side < 2; side++)
		{
			feet.Foot[side] = lf.At(along + (side == lead ? 0.12f : -0.06f) * s, SideSign(side) * M.Stance, gl + m_AnkleHeight);
			feet.Pole[side] = lf.F;
			feet.Kneeling[side] = false;
			feet.Hand[side] = 1;
		}
		feet.Spine = glm::radians(18.0f);
		m_GetUpKeys.push_back(feet);

		GetUpKey stood = feet;
		stood.T = T + tC + tR + tS;
		stood.Spine = 0.0f;
		m_GetUpKeys.push_back(stood);

		BeginMove(Move::Roll, T + tC + tR + tS, exit, T + tC + 0.92f * tR);
	}

	// ---------------------------------------------------------------------
	// Dodging: out of the way, on purpose
	// ---------------------------------------------------------------------
	//
	// A stumble is a body knocked off its line catching itself with its
	// feet; a dodge is the same lurch chosen, so it is planned the way the
	// moves above are -- the hips carried along a path, the feet put down
	// where a person's go -- and it is over sooner, on his feet and in the
	// walk again.
	//
	// What one can do is what a push off the ground can do to the velocity
	// he already has. The legs change it by about 3 m/s (m_DodgePush): a
	// sole grips at about mu g, 9 m/s^2, and a push lasts a third of a
	// second. Added to whatever he is doing:
	//
	//   standing   a sidestep, a hop back, a lunge: 0.9 m in 0.6 s;
	//   walking    the same with the walk carried through it -- a dodge
	//              back from a walk is the shorter, its push spent first on
	//              stopping;
	//   running    sideways a cut, the run bent off its line by the push and
	//              on along the new one; backwards a brake -- 3 m/s off 4 is
	//              not going backwards, it is stopping short, which against
	//              a shot led for where he was going does as well;
	//   sprinting  sideways or ahead a dive into a roll: the farthest and
	//              the lowest -- under a ball at chest height -- and the
	//              slowest to come back from.
	//
	// Short of room -- a wall in the way, a ledge, a step up -- it goes as
	// far as there is room, and not at all when that is next to nothing.
	enum class Dodge { None, Step, Cut, Brake, Dive };

	bool TryDodge(glm::vec3 wish)
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		glm::vec3 F(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		// What he is doing now: the walk's velocity (the hips' own carries
		// the bob and the sway), or, in a move, the hips' along its track.
		glm::vec3 v0 = m_Mode == Mode::Moving ? glm::vec3(pelvis.Velocity.x, 0.0f, pelvis.Velocity.z)
			: F * m_Speed + glm::vec3(m_StaggerVelocity.x, 0.0f, m_StaggerVelocity.z);
		float speed = glm::length(v0);
		glm::vec3 heading = speed > 0.3f * rs ? v0 / speed : F;
		wish.y = 0.0f;
		glm::vec3 d = glm::length(wish) > 1e-3f ? glm::normalize(wish) : -heading;
		float along = glm::dot(d, heading);
		bool fast = speed > 1.8f * rs;
		bool sprint = m_Control.Enabled && m_Control.Run && speed > 0.8f * m_RunSpeed;
		bool done;
		if (fast && along < -0.5f)
			done = PlanBrake(v0, heading);
		else if (fast && (sprint || along > 0.92f) && along > -0.3f && PlanDive(v0, d))
			done = true;
		else
			done = PlanDodgeStep(v0, d, fast);
		if (done)
			m_Dodges++;
		return done;
	}

	// How far he can go from where he stands along `dir` before something
	// stops him: a wall at knee, waist or head height, or the ground on the
	// way stepping up or dropping away by more than the walk takes.
	float DodgeRoom(const glm::vec3& dir, float want) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float s = m_Proportions.Scale;
		float ground = GroundY(pelvis.Position, -1000.0f);
		glm::vec3 base(pelvis.Position.x, ground, pelvis.Position.z);
		float room = want + 0.4f * s;
		for (float h : { 0.35f, 0.9f, 1.45f })
		{
			GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(base + glm::vec3(0.0f, h * s, 0.0f), dir, room, m_CharacterBodies);
			if (hit.Hit && hit.Normal.y < 0.6f && Solid(hit.Body))
				room = std::min(room, hit.Distance);
		}
		float free = std::max(room - 0.4f * s, 0.0f);
		for (float at = 0.15f * s; at <= free; at += 0.15f * s)
		{
			glm::vec3 p = base + dir * at;
			float g = GroundY(glm::vec3(p.x, ground + m_MaxStepUp + 0.05f * s, p.z), -1000.0f);
			if (g > ground + m_MaxStepUp || g < ground - 0.45f * s)
				return std::max(at - 0.15f * s, 0.0f);
		}
		return free;
	}

	// The hips' velocity through a dodge, flat: from v0 to v1 over the push,
	// eased (a smoothstep, so it never jolts), then to ve over the settle.
	struct DodgePath
	{
		glm::vec3 V0{ 0.0f }, V1{ 0.0f }, VE{ 0.0f };
		float Tp = 0.3f, Ts = 0.3f;
		float Duration() const { return Tp + Ts; }
		// The integral of the smoothstep, u^3 - u^4 / 2, for the distance.
		static float S(float u) { return u * u * u - 0.5f * u * u * u * u; }
		glm::vec3 Offset(float t) const
		{
			float a = glm::clamp(t, 0.0f, Tp);
			glm::vec3 x = V0 * a + (V1 - V0) * (Tp * S(a / std::max(Tp, 1e-4f)));
			if (t > Tp)
			{
				float b = std::min(t - Tp, Ts);
				x += V1 * b + (VE - V1) * (Ts * S(b / std::max(Ts, 1e-4f)));
				if (t - Tp > Ts)
					x += VE * (t - Tp - Ts);
			}
			return x;
		}
		glm::vec3 Acceleration(float t) const
		{
			auto ds = [](float u) { return 6.0f * u * (1.0f - u); };
			if (t < Tp)
				return (V1 - V0) * (ds(t / std::max(Tp, 1e-4f)) / std::max(Tp, 1e-4f));
			float u = (t - Tp) / std::max(Ts, 1e-4f);
			return u < 1.0f ? (VE - V1) * (ds(u) / std::max(Ts, 1e-4f)) : glm::vec3(0.0f);
		}
		glm::vec3 Velocity(float t) const
		{
			if (t < Tp)
				return V0 + (V1 - V0) * glm::smoothstep(0.0f, 1.0f, t / std::max(Tp, 1e-4f));
			return V1 + (VE - V1) * glm::smoothstep(0.0f, 1.0f, (t - Tp) / std::max(Ts, 1e-4f));
		}
	};

	// The hips along a dodge's path, every 60th of a second: on the ground
	// under them, crouched by `crouch(t)`, leaning into the push as a body
	// accelerating at `a` must -- atan(a / g), the pelvis taking about half
	// of it as in the walk -- and into the speed as the walk does, turned
	// from `yaw0` to `yaw1`. Eased out of the pose he was in over the first
	// 0.12 s: the walk's bob and sway are not on the path, and starting on
	// it would jump the hips by them in a step.
	template <typename Crouch>
	void DodgeWays(const DodgePath& path, float yaw0, float yaw1, const Crouch& crouch, std::vector<Way>& ways) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float T = path.Duration();
		glm::vec3 P0 = pelvis.Position;
		float turn = yaw1 - yaw0;
		while (turn > glm::pi<float>()) turn -= glm::two_pi<float>();
		while (turn < -glm::pi<float>()) turn += glm::two_pi<float>();
		auto pose = [&](float t, glm::vec3& at, glm::quat& q)
		{
			glm::vec3 x = P0 + path.Offset(t);
			float g = GroundY(glm::vec3(x.x, P0.y, x.z), -1000.0f);
			at = glm::vec3(x.x, g + m_StandClearance - m_Crouch - crouch(t), x.z);
			float yaw = yaw0 + turn * glm::smoothstep(0.15f * T, 0.85f * T, t);
			glm::vec3 a = path.Acceleration(t);
			a.y = 0.0f;
			glm::quat tilt(1.0f, 0.0f, 0.0f, 0.0f);
			if (glm::length(a) > 1e-3f)
			{
				// Into the push, and less against the stop: the lead foot out
				// beyond him stops the body, the trunk staying nearer upright
				// -- at the push's share it rocked 44 degrees in 0.3 s.
				float share = t < path.Tp ? 0.55f : 0.25f;
				float angle = std::min(share * std::atan(glm::length(a) / 9.81f), glm::radians(22.0f));
				tilt = glm::angleAxis(angle, glm::normalize(glm::cross(up, a)));
			}
			glm::vec3 v = path.Velocity(t);
			glm::vec3 F(std::sin(yaw), 0.0f, std::cos(yaw));
			float forward = glm::dot(v, F);
			float lean = glm::clamp((forward / std::max(m_WalkSpeed, 0.01f)) * m_LeanAmount, -m_LeanBack, m_LeanForward) * m_LeanPelvisShare;
			q = glm::normalize(tilt * glm::angleAxis(yaw, up) * glm::angleAxis(lean, glm::vec3(1.0f, 0.0f, 0.0f)));
		};
		glm::vec3 start;
		glm::quat startTurn;
		pose(0.0f, start, startTurn);
		int n = std::max(2, (int)std::ceil(T * 60.0f));
		for (int k = 0; k <= n; k++)
		{
			float t = T * (float)k / (float)n;
			glm::vec3 at;
			glm::quat q;
			pose(t, at, q);
			float ease = glm::smoothstep(0.0f, 0.12f, t);
			at += (P0 - start) * (1.0f - ease) * glm::vec3(0.0f, 1.0f, 0.0f);
			q = glm::normalize(glm::slerp(pelvis.Orientation, q, ease));
			ways.push_back({ t, k == 0 ? P0 : at, k == 0 ? pelvis.Orientation : q });
		}
	}

	// A sidestep, hop or lunge (slow), or a cut (fast): the push foot down
	// and pushing, the lead foot out the way he is going, a moment with
	// both off the ground -- a bound, as a quick sidestep is -- the lead
	// foot down short of where the hips stop, and the push foot in beside
	// it. A cut's push foot is planted wide on the outside, the lead foot
	// lands along the new line and the push foot swings through ahead.
	bool PlanDodgeStep(const glm::vec3& v0, const glm::vec3& d, bool fast)
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		float speed = glm::length(v0);
		glm::vec3 F0(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 heading = speed > 0.3f * rs ? v0 / speed : F0;
		float along = glm::dot(d, heading);
		glm::vec3 across = d - heading * along;
		if (fast && glm::length(across) < 0.2f)
			return false;

		auto build = [&](float k)
		{
			DodgePath path;
			path.V0 = v0;
			if (fast)
			{
				// The push across, a little of the run lost to the plant, and
				// on at the new velocity. Less push the faster he goes: a
				// foot is down for less time.
				float push = std::max(m_DodgePush * rs - 0.15f * speed, 1.2f * rs) * k;
				path.V1 = v0 * (1.0f - m_DodgeCutLoss * k) + glm::normalize(across) * push;
				path.VE = path.V1;
				path.Tp = 0.24f * rs;
				path.Ts = 0.18f * rs;
			}
			else
			{
				// The push along d; the lead foot stops it; the walk he was
				// in goes on unless the dodge was against it.
				path.V1 = v0 + d * (m_DodgePush * rs * k);
				path.VE = v0 * glm::clamp(1.0f + along, 0.0f, 1.0f);
				path.Tp = m_DodgeTime * rs;
				path.Ts = m_DodgeSettle * rs;
			}
			return path;
		};
		DodgePath path = build(1.0f);
		// As far as the way is clear: the push scaled down until the whole
		// of it fits, and no dodge at all for less than a third of one.
		{
			glm::vec3 shift = path.Offset(path.Duration());
			float want = glm::length(shift);
			if (want > 1e-3f)
			{
				float room = DodgeRoom(shift / want, want);
				float k = 1.0f;
				while (glm::length(path.Offset(path.Duration())) > room && k > 0.3f)
				{
					k -= 0.1f;
					path = build(k);
				}
				if (glm::length(path.Offset(path.Duration())) > room + 0.05f * s)
					return false;
			}
		}

		float T = path.Duration(), Tp = path.Tp;
		float yaw0 = m_Facing;
		float yaw1 = fast ? std::atan2(path.VE.x, path.VE.z) : m_Facing;
		float turn = yaw1 - yaw0;
		while (turn > glm::pi<float>()) turn -= glm::two_pi<float>();
		while (turn < -glm::pi<float>()) turn += glm::two_pi<float>();
		float depth = (fast ? 0.06f : 0.10f) * s;
		std::vector<Way> ways;
		DodgeWays(path, yaw0, yaw1, [&](float t)
		{
			return depth * (glm::smoothstep(0.0f, 0.45f * Tp, t) - 0.85f * glm::smoothstep(Tp, T, t));
		}, ways);
		BuildTrack(ways, v0, path.VE);

		glm::vec3 P0 = pelvis.Position;
		auto hipsAt = [&](float t) { glm::vec3 x = P0 + path.Offset(t); return glm::vec3(x.x, 0.0f, x.z); };
		auto onGround = [&](glm::vec3 p)
		{
			p.y = GroundY(glm::vec3(p.x, P0.y, p.z), -1000.0f) + m_AnkleHeight;
			return p;
		};
		auto lifted = [&](const glm::vec3& a, const glm::vec3& b, float w, float lift)
		{
			glm::vec3 p = onGround(glm::mix(a, b, w));
			p.y += lift * s;
			return p;
		};
		glm::vec3 R0 = glm::cross(up, F0);
		glm::vec3 F1(std::sin(yaw1), 0.0f, std::cos(yaw1));
		glm::vec3 R1 = glm::cross(up, F1);

		// The lead foot is the one on the side he is going, or, going
		// ahead or back, the one already further that way.
		int lead;
		if (fast)
			lead = glm::dot(across, R0) > 0.0f ? 1 : 0;
		else if (std::fabs(glm::dot(d, R0)) > 0.35f)
			lead = glm::dot(d, R0) > 0.0f ? 1 : 0;
		else
			lead = glm::dot(AnkleWorld(1) - AnkleWorld(0), d) > 0.0f ? 1 : 0;
		int push = 1 - lead;
		m_GetUpLead = lead;

		float t1, tOff, tLand;
		glm::vec3 pushPlant, leadLand, pushFinal;
		glm::vec3 leadNow = AnkleWorld(lead), pushNow = AnkleWorld(push);
		if (fast)
		{
			t1 = 0.25f * Tp;
			tOff = 0.85f * Tp;
			tLand = Tp + 0.05f * rs;
			pushPlant = onGround(hipsAt(t1) + heading * (0.20f * s) + R0 * (SideSign(push) * M.Stance * 1.5f));
			leadLand = onGround(hipsAt(tLand) + F1 * (0.25f * s) + R1 * (SideSign(lead) * M.Stance));
			pushFinal = onGround(hipsAt(T) + F1 * (0.30f * s) + R1 * (SideSign(push) * M.Stance));
		}
		else
		{
			t1 = 0.30f * Tp;
			tOff = 0.62f * Tp;
			tLand = Tp;
			glm::vec3 dir = glm::length(hipsAt(T) - hipsAt(0.0f)) > 0.05f ? glm::normalize(hipsAt(T) - hipsAt(0.0f)) : d;
			pushPlant = onGround(pushNow);
			leadLand = onGround(hipsAt(T) - dir * (0.10f * s) + R0 * (SideSign(lead) * M.Stance));
			glm::vec3 on = glm::length(path.VE) > 0.2f * rs ? glm::normalize(path.VE) * (0.15f * s) : glm::vec3(0.0f);
			pushFinal = onGround(hipsAt(T) + on + R0 * (SideSign(push) * M.Stance));
		}

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		auto key = [&](float t, const GetUpKey& from)
		{
			GetUpKey k = from;
			k.T = t;
			float u = glm::smoothstep(0.15f * T, 0.85f * T, t);
			k.Heading = yaw0 + turn * u;
			glm::vec3 F(std::sin(k.Heading), 0.0f, std::cos(k.Heading));
			for (int side = 0; side < 2; side++)
			{
				k.Pole[side] = F;
				k.Kneeling[side] = false;
			}
			return k;
		};
		// Arms out for balance, the lead one wide, the other across in front
		// of him -- or both forward a little, dodging ahead or back.
		auto arms = [&](GetUpKey& k, float amount)
		{
			k.LocalHands = true;
			bool sideways = fast || std::fabs(glm::dot(d, R0)) > 0.35f;
			for (int side = 0; side < 2; side++)
			{
				k.Hand[side] = 0;
				if (sideways)
					k.HandAt[side] = side == lead
						? glm::vec3(SideSign(side) * (M.ShoulderX + 0.28f * s * amount), M.Trunk * 0.76f, 0.10f * s)
						: glm::vec3(SideSign(lead) * 0.02f * s, M.Trunk * 0.80f, 0.28f * s);
				else
					k.HandAt[side] = glm::vec3(SideSign(side) * (M.ShoulderX + 0.12f * s * amount), M.Trunk * 0.72f, 0.20f * s);
			}
		};

		GetUpKey k1 = key(t1, now);
		k1.Foot[push] = pushPlant;
		k1.Foot[lead] = lifted(leadNow, leadLand, 0.25f, 0.10f);
		k1.Spine = glm::radians(8.0f);
		arms(k1, 0.7f);
		m_GetUpKeys.push_back(k1);

		GetUpKey kOff = key(tOff, k1);
		kOff.Foot[lead] = lifted(leadNow, leadLand, 0.65f, 0.07f);
		arms(kOff, 1.0f);
		m_GetUpKeys.push_back(kOff);

		GetUpKey kLand = key(tLand, kOff);
		kLand.Foot[lead] = leadLand;
		kLand.Foot[push] = lifted(pushPlant, pushFinal, 0.30f, fast ? 0.14f : 0.10f);
		m_GetUpKeys.push_back(kLand);

		GetUpKey k3 = key(tLand + 0.45f * (T - tLand), kLand);
		k3.Foot[push] = lifted(pushPlant, pushFinal, 0.75f, 0.06f);
		k3.Spine = glm::radians(4.0f);
		k3.LocalHands = false;
		k3.Hand[0] = k3.Hand[1] = 1;
		m_GetUpKeys.push_back(k3);

		GetUpKey kEnd = key(T, k3);
		kEnd.Foot[push] = pushFinal;
		kEnd.Spine = 0.0f;
		m_GetUpKeys.push_back(kEnd);

		m_Facing = yaw1;
		BeginMove(Move::Dodge, T, glm::length(path.VE), tLand + 0.5f * (T - tLand));
		m_DodgeKind = fast ? Dodge::Cut : Dodge::Step;
		m_DodgeCooldown = tLand;
		return true;
	}

	// Running, a dodge back: the brake. The rear foot swings through and
	// plants ahead, the other beside it, and the body comes to them leaning
	// back against the stop -- m_DodgeBrake, both feet pushing, takes 3 m/s
	// off in about 0.35 s, so from a run he ends slow, not going backwards.
	bool PlanBrake(const glm::vec3& v0, const glm::vec3& heading, float brake = -1.0f)
	{
		if (brake <= 0.0f)
			brake = m_DodgeBrake;
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		float speed = glm::length(v0);
		float take = std::min(speed, 1.1f * m_DodgePush * rs);
		DodgePath path;
		path.V0 = v0;
		path.V1 = v0 - heading * take;
		path.VE = path.V1;
		path.Tp = std::max(1.5f * take / brake, 0.18f * rs);   // the smoothstep's mean is 2/3 of its peak
		path.Ts = 0.10f * rs;
		float T = path.Duration();
		float depth = 0.10f * s;
		std::vector<Way> ways;
		DodgeWays(path, m_Facing, m_Facing, [&](float t)
		{
			return depth * (glm::smoothstep(0.0f, 0.4f * path.Tp, t) - 0.7f * glm::smoothstep(path.Tp, T, t));
		}, ways);
		BuildTrack(ways, v0, path.VE);

		glm::vec3 P0 = pelvis.Position;
		auto hipsAt = [&](float t) { glm::vec3 x = P0 + path.Offset(t); return glm::vec3(x.x, 0.0f, x.z); };
		auto onGround = [&](glm::vec3 p)
		{
			p.y = GroundY(glm::vec3(p.x, P0.y, p.z), -1000.0f) + m_AnkleHeight;
			return p;
		};
		glm::vec3 F(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 R = glm::cross(up, F);
		int lead = glm::dot(AnkleWorld(0) - AnkleWorld(1), heading) < 0.0f ? 0 : 1;   // the one behind swings through
		int other = 1 - lead;
		m_GetUpLead = lead;
		float t1 = 0.10f * rs, t2 = 0.24f * rs;
		glm::vec3 leadPlant = onGround(hipsAt(t1) + heading * (0.42f * s) + R * (SideSign(lead) * M.Stance));
		glm::vec3 otherPlant = onGround(hipsAt(t2) + heading * (0.28f * s) + R * (SideSign(other) * M.Stance));

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey k1 = now;
		k1.T = t1;
		k1.Foot[lead] = leadPlant;
		glm::vec3 mid = onGround(glm::mix(now.Foot[other], otherPlant, 0.4f));
		mid.y += 0.08f * s;
		k1.Foot[other] = mid;
		k1.LocalHands = true;
		for (int side = 0; side < 2; side++)
		{
			k1.Pole[side] = F;
			k1.Kneeling[side] = false;
			k1.Hand[side] = 0;
			k1.HandAt[side] = glm::vec3(SideSign(side) * (M.ShoulderX + 0.08f * s), M.Trunk * 0.72f, 0.30f * s);
		}
		k1.Spine = glm::radians(6.0f);
		m_GetUpKeys.push_back(k1);
		GetUpKey k2 = k1;
		k2.T = t2;
		k2.Foot[other] = otherPlant;
		m_GetUpKeys.push_back(k2);
		GetUpKey k3 = k2;
		k3.T = std::max(path.Tp, t2 + 0.05f);
		m_GetUpKeys.push_back(k3);
		GetUpKey kEnd = k3;
		kEnd.T = std::max(T, k3.T + 0.05f);
		kEnd.LocalHands = false;
		kEnd.Hand[0] = kEnd.Hand[1] = 1;
		kEnd.Spine = 0.0f;
		m_GetUpKeys.push_back(kEnd);

		BeginMove(Move::Dodge, kEnd.T, glm::length(path.VE), k3.T);
		m_DodgeKind = Dodge::Brake;
		m_DodgeCooldown = k3.T;
		return true;
	}

	// Sprinting, a dodge to the side or ahead: a dive. The push sends him
	// along the run and the push together, the hips drop, the hands reach
	// for the ground, and he rolls over a shoulder along the dive's line and
	// comes up running (PlanRoll). Not without the room a roll takes, flat:
	// the dodge is then a cut instead.
	bool PlanDive(const glm::vec3& v0, const glm::vec3& d)
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const Measures M = BodyMeasures();
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		float s = m_Proportions.Scale, rs = std::sqrt(s);
		float speed = glm::length(v0);
		glm::vec3 heading = speed > 0.3f * rs ? v0 / speed : glm::vec3(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 across = d - heading * glm::dot(d, heading);
		float push = 0.9f * m_DodgePush * rs;
		glm::vec3 v1 = glm::length(across) > 0.2f ? v0 + glm::normalize(across) * push : v0 + heading * (0.4f * push);
		float cap = std::max(speed, 1.15f * m_RunSpeed);
		if (glm::length(v1) > cap)
			v1 *= cap / glm::length(v1);
		float horizontal = glm::length(v1);
		glm::vec3 dir = v1 / std::max(horizontal, 1e-3f);

		float gl = GroundY(pelvis.Position, -1000.0f);
		glm::vec3 base(pelvis.Position.x, gl, pelvis.Position.z);
		if (RoomAhead(base, dir) < RollRoom() + 0.5f * s)
			return false;
		for (float at = 0.5f * s; at <= RollRoom() + 0.5f * s; at += 0.5f * s)
		{
			glm::vec3 p = base + dir * at;
			float g = GroundY(glm::vec3(p.x, gl + 0.5f * s, p.z), -1000.0f);
			if (std::fabs(g - gl) > 0.25f * s)
				return false;
		}

		int lead = glm::dot(across, glm::cross(up, heading)) > 0.0f ? 1 : 0;
		m_GetUpLead = lead;
		m_Facing = std::atan2(dir.x, dir.z);
		Frame fr = FrameHere();
		float Tp = 0.18f * rs;
		glm::vec3 P0 = pelvis.Position;
		glm::vec3 L = P0 + (v0 + v1) * (0.5f * Tp);
		L.y = gl + m_StandClearance - 0.25f * s;
		std::vector<Way> ways;
		ways.push_back({ 0.0f, P0, pelvis.Orientation });
		ways.push_back({ Tp, L, fr.Lean(30.0f) });

		m_GetUpKeys.clear();
		GetUpKey now = KeyNow();
		m_GetUpKeys.push_back(now);
		GetUpKey reach = now;
		reach.T = 0.55f * Tp;
		reach.LocalHands = true;
		for (int side = 0; side < 2; side++)
		{
			reach.Hand[side] = 0;
			reach.HandAt[side] = glm::vec3(SideSign(side) * 0.15f * s, M.Trunk * 0.55f, 0.45f * s);
			reach.Pole[side] = fr.F;
			reach.Kneeling[side] = false;
		}
		reach.Foot[lead] = now.Foot[lead] + up * (0.08f * s);
		reach.Spine = glm::radians(14.0f);
		m_GetUpKeys.push_back(reach);
		GetUpKey touch = reach;
		touch.T = Tp;
		touch.LocalHands = false;
		Frame lf = fr;
		lf.O = L;
		// The legs come up into the tuck as the hands reach the ground, so
		// that the roll goes over a shoulder and not the knees: left trailing
		// straight, the drop to the roll's crouch put him on his knees.
		for (int side = 0; side < 2; side++)
		{
			touch.HandAt[side] = lf.At(0.45f * s, SideSign(side) * M.ShoulderX, gl + M.HandH + 0.08f * s);
			touch.Foot[side] = glm::vec3(SideSign(side) * M.HipX * 0.9f, -(M.HipDrop + 0.30f * s), 0.05f * s);
			touch.Pole[side] = glm::normalize(glm::vec3(0.0f, 0.3f, 1.0f));
			touch.Kneeling[side] = true;
		}
		touch.LocalFeet = true;
		touch.Spine = glm::radians(24.0f);
		m_GetUpKeys.push_back(touch);

		PlanRoll(ways, Tp, lf, horizontal, lead, gl, v0);
		m_Move = Move::Dodge;
		m_DodgeKind = Dodge::Dive;
		m_DodgeCooldown = m_ChainFrom;
		return true;
	}

	// Something of his other than a hand or a foot pressed by the fixed
	// world harder than five times his weight, upwards, in the last step: a
	// limb caught between the floor and the carried hips. Hands and feet
	// carry him in a move -- a climb hangs his whole weight on them -- and
	// are left out, unless `hands`: getting up, nothing carries more than
	// half his weight on one hand, and a forearm pressed five times harder
	// is one he is lying on.
	bool Wedged(float dt, bool hands = false) const
	{
		if (dt <= 0.0f)
			return false;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool mineA = IsCharacter(contact.A), mineB = IsCharacter(contact.B);
			if (mineA == mineB)
				continue;
			unsigned int mine = mineA ? contact.A : contact.B, other = mineA ? contact.B : contact.A;
			bool forearm = mine == m_LowerArm[0] || mine == m_LowerArm[1];
			if (mine == m_Foot[0] || mine == m_Foot[1] || (forearm && !hands) || !Solid(other))
				continue;
			glm::vec3 up = mineA ? -contact.Normal : contact.Normal;
			if (up.y > 0.5f && contact.TotalNormalImpulse() / dt > 5.0f * m_BodyMass * 9.81f)
				return true;
		}
		return false;
	}

	// An arm more than 1.2 cm into the ground under it: lain on, or pressed
	// down by the trunk he is pushing up -- not always hard enough to read
	// as a wedge (Wedged), and as visible.
	bool ArmSunk() const
	{
		for (int side = 0; side < 2; side++)
			for (GS::PhysicsWorld3D::BodyHandle handle : { m_UpperArm[side], m_LowerArm[side] })
			{
				const GS::RigidBody3D& b = m_World.GetBody(handle);
				glm::vec3 axis = b.Orientation * glm::vec3(0.0f, b.HalfHeight, 0.0f);
				for (const glm::vec3& end : { b.Position + axis, b.Position - axis })
				{
					float ground = GroundY(glm::vec3(end.x, end.y + 0.3f * m_Proportions.Scale, end.z), -1000.0f);
					if (ground - (end.y - b.Radius) > 0.012f * m_Proportions.Scale)
						return true;
				}
			}
		return false;
	}

	// How far the hips must rise, carried to `at` turned `turn`, for the
	// torso and the head -- held where they are now relative to the hips --
	// to clear the ground under them. A move plans the hips' height from
	// the trunk its keys mean to bend (LowestBelow), but the spine's motor
	// gets there late and the neck holds the head as built, not tucked:
	// the plan's trunk is not the one there is. Measured in a dive's roll,
	// the floor pressed the torso with 60 kN, 85 times his weight, step on
	// step, and the head with 17 -- the wedge the get-up had, in a move.
	float TrunkLift(const glm::vec3& at, const glm::quat& turn) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::quat inverse = glm::inverse(pelvis.Orientation);
		float lift = 0.0f;
		for (GS::PhysicsWorld3D::BodyHandle handle : { m_Torso, m_Head })
		{
			const GS::RigidBody3D& b = m_World.GetBody(handle);
			glm::vec3 centre = at + turn * (inverse * (b.Position - pelvis.Position));
			float reach = b.Radius;
			if (b.Shape == GS::ColliderShape3D::Box)
			{
				glm::mat3 r = glm::mat3_cast(turn * (inverse * b.Orientation));
				reach = std::fabs(r[0].y) * b.HalfExtents.x + std::fabs(r[1].y) * b.HalfExtents.y + std::fabs(r[2].y) * b.HalfExtents.z;
			}
			// From the body's own centre: from above it, a ledge he is
			// climbing beside would read as ground over his head.
			float ground = GroundY(centre, -1000.0f);
			lift = std::max(lift, ground + 0.004f - (centre.y - reach));
		}
		return lift;
	}

	void UpdateMove(float dt)
	{
		if (m_Move == Move::Hang)
		{
			UpdateHang(dt);
			return;
		}
		if ((m_Move == Move::DropToHang || m_Move == Move::CatLeap) && m_MoveTimer + dt >= m_MoveTime)
		{
			BeginHang();
			UpdateHang(dt);
			return;
		}
		if (m_Move == Move::TicTac && m_MoveTimer + dt >= m_MoveTime)
		{
			// Off the wall, mirrored, and up.
			float rs = std::sqrt(m_Proportions.Scale);
			m_Facing = std::atan2(m_WallRunDir.x, m_WallRunDir.z);
			m_Speed = m_WallRunSpeed;
			m_WantsJump = false;
			if (!PlanDrop(3.0f * rs))
			{
				OpenJoints(false);
				GoRagdoll("off the wall");
			}
			return;
		}
		// Jumped to a ledge and not pressing on towards it: he hangs there.
		if (m_Move == Move::Climb && m_ClimbHangAt > 0.0f && m_MoveTimer >= m_ClimbHangAt)
		{
			m_ClimbHangAt = -1.0f;
			if (!(m_Control.Enabled && m_Control.Forward) && !m_WantsJump)
			{
				BeginHang();
				UpdateHang(dt);
				return;
			}
		}
		if (m_Track.size() < 2 || m_GetUpKeys.size() < 2)
		{
			FinishMove();
			return;
		}
		m_MoveTimer += dt;
		float t = std::min(m_MoveTimer, m_Track.back().T);
		while (m_TrackIndex + 2 < m_Track.size() && t >= m_Track[m_TrackIndex + 1].T)
			m_TrackIndex++;
		const TrackPoint& a = m_Track[m_TrackIndex];
		const TrackPoint& b = m_Track[m_TrackIndex + 1];
		float w = glm::clamp((t - a.T) / std::max(b.T - a.T, 1e-5f), 0.0f, 1.0f);
		glm::vec3 target = glm::mix(a.Position, b.Position, w);
		glm::quat turn = glm::normalize(glm::slerp(a.Orientation, b.Orientation, w));

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		// Never into what is under him: the pelvis is carried, and nothing
		// else would stop it -- nor the trunk it carries (TrunkLift).
		{
			float under = GroundY(target, -1000.0f);
			glm::mat3 r = glm::mat3_cast(turn);
			const glm::vec3& half = m_Proportions.PelvisHalf;
			float reach = std::fabs(r[0].y) * half.x + std::fabs(r[1].y) * half.y + std::fabs(r[2].y) * half.z;
			target.y = std::max(target.y, under + reach + 0.004f);
			// And as high again as the trunk needs (TrunkLift), and higher
			// while, for all that, something of his is still being pressed
			// into the floor -- a knee of the tuck, which the legs' IK puts
			// where the plan did not (a roll off the 3 m tower: a thigh
			// pressed with 36 kN, then more each step, to 140). The lift is
			// eased, up at 1.2 m/s and down at 0.6: taken as it came, it
			// jumped 15-25 cm in a step and slammed him back down -- 140 kN
			// on the torso and the head at once, mid-roll.
			float s = m_Proportions.Scale;
			float need = TrunkLift(target, turn);
			if (Wedged(dt))
				need = std::max(need, m_FloorLift + 0.02f * s);
			m_FloorLift += glm::clamp(need - m_FloorLift, -0.6f * s * dt, 1.2f * s * dt);
			target.y += m_FloorLift;
		}
		pelvis.Velocity = (target - pelvis.Position) / std::max(dt, 1e-4f);
		pelvis.Orientation = turn;
		pelvis.UpdateInertiaWorld();

		float u = glm::clamp(m_MoveTimer / std::max(m_MoveTime, 1e-3f), 0.0f, 1.0f);
		size_t i = 0;
		while (i + 2 < m_GetUpKeys.size() && u >= m_GetUpKeys[i + 1].T)
			i++;
		const GetUpKey& ka = m_GetUpKeys[i];
		const GetUpKey& kb = m_GetUpKeys[i + 1];
		float kw = glm::smoothstep(0.0f, 1.0f, glm::clamp((u - ka.T) / std::max(kb.T - ka.T, 1e-4f), 0.0f, 1.0f));
		DriveKeyLimbs(ka, kb, kw, -1e9f);

		if (m_Move == Move::WallRun)
		{
			// Jump again to kick off; off the end, or where the wall stops.
			float s = m_Proportions.Scale;
			bool kick = m_WantsJump && m_MoveTimer > 0.12f * std::sqrt(s);
			// The face under him, which ends where the wall does. Further
			// than the metre a run keeps while the hips close in on a wall
			// turned onto from further off: a corner met at speed is turned
			// 1.35 m short of it, and the run along it used to end there
			// on its first step, the new wall out of reach.
			float reach = m_MoveTimer < 0.25f * std::sqrt(s) ? std::max(1.0f * s, m_WallRunFrom + 0.3f * s) : 1.0f * s;
			GS::PhysicsWorld3D::RayHit face = m_World.Raycast(pelvis.Position, -m_WallNormal, reach, m_CharacterBodies);
			// An inside corner: a wall across the way ahead. Onto it, turning
			// away from the one he is on, keeping the speed up he has --
			// twice at most, so a corridor is not run up for ever.
			GS::PhysicsWorld3D::RayHit ahead = m_World.Raycast(pelvis.Position, m_WallRunDir, 0.75f * s + 0.15f * m_WallRunSpeed, m_CharacterBodies);
			if (!kick && ahead.Hit && m_WallRunCorners < 2 && m_MoveTimer > 0.12f * std::sqrt(s)
				&& m_World.GetBody(ahead.Body).Type == GS::BodyType::Static && std::fabs(ahead.Normal.y) < 0.3f)
			{
				glm::vec3 n2 = glm::normalize(glm::vec3(ahead.Normal.x, 0.0f, ahead.Normal.z));
				glm::vec3 turnTo = m_WallNormal - n2 * glm::dot(m_WallNormal, n2);
				if (glm::length(turnTo) > 0.3f)
				{
					turnTo = glm::normalize(turnTo);
					SideWall w2;
					w2.Found = true;
					w2.Normal = n2;
					w2.Distance = ahead.Distance;
					w2.Length = 0.0f;
					for (int k = 1; k <= 12; k++)
					{
						glm::vec3 q = pelvis.Position + turnTo * (0.35f * s * (float)k);
						GS::PhysicsWorld3D::RayHit along = m_World.Raycast(q, -n2, ahead.Distance + 0.6f * s, m_CharacterBodies);
						if (!along.Hit)
							break;
						w2.Length = 0.35f * s * (float)k;
					}
					if (w2.Length >= 0.7f * s)
					{
						m_Facing = std::atan2(turnTo.x, turnTo.z);
						m_Speed = m_WallRunSpeed * 0.92f;
						int corners = m_WallRunCorners + 1;
						PlanWallRun(w2, std::max(pelvis.Velocity.y, 0.6f * std::sqrt(s)));
						m_WallRunCorners = corners;
						return;
					}
				}
			}
			if (kick || m_MoveTimer >= m_MoveTime || !face.Hit)
			{
				LeaveWall(kick);
				return;
			}
			return;
		}

		// A dodge asked for as a move ends goes from here, once he is on his
		// feet: off the end of a vault, out of a roll, one dodge into the next.
		if (m_DodgeAsked > 0.0f && m_DodgeCooldown <= 0.0f && m_MoveTimer >= m_ChainFrom && m_MoveTimer < m_MoveTime)
		{
			if (TryDodge(m_DodgeWish))
			{
				m_DodgeAsked = 0.0f;
				return;
			}
		}

		// Into the next one with no walking step between: once he is on his
		// feet again, a wall or a ledge in reach starts its move from here,
		// at the speed the hips have now, with the limbs where they are.
		if (m_Parkour && m_MoveTimer >= m_ChainFrom && m_MoveTimer < m_MoveTime)
		{
			glm::vec3 F(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
			float speedBefore = m_Speed;
			m_Speed = std::max(glm::dot(pelvis.Velocity, F), 0.0f);
			m_Ahead = Sense(F);
			if (m_Ahead.Type != Obstacle::None && TryMove(m_Ahead, F))
			{
				return;
			}
			m_Speed = speedBefore;
		}

		if (m_MoveTimer >= m_MoveTime)
			FinishMove();
	}

	// Walking again, from wherever the move left him, at the speed it left
	// him with.
	void FinishMove()
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		float ground = GroundY(pelvis.Position, -1000.0f);
		m_StandHeight = pelvis.Position.y;
		OpenJoints(false);
		m_Move = Move::None;
		GoControlled();
		m_Speed = m_MoveExit;
		// The walk's hips follow its anchor a step behind, and GoControlled
		// puts the anchor where the hips are: the first step out of every
		// move stood still, a 7 cm hitch at a run. A step ahead, as the walk
		// would have had it.
		m_RootAnchor += glm::vec3(std::sin(m_Facing), 0.0f, std::cos(m_Facing)) * (m_MoveExit * m_LastDt);
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = AnkleWorld(side);
			m_SwingFrom[side] = m_Planted[side];
		}
		m_MoveGrace = 0.4f;
	}

	bool IsCharacter(unsigned int body) const
	{
		return std::find(m_CharacterBodies.begin(), m_CharacterBodies.end(), body)
			!= m_CharacterBodies.end();
	}

	// A hit hard enough to knock the character down.
	//
	// Measured as the impulse arriving through contacts on anything that is
	// not a foot -- feet are in contact with the floor constantly and would
	// trigger it every step. Read as a force so the threshold is in units a
	// person can reason about: body weight is 697 N.
	float ImpactForce(float dt)
	{
		glm::vec3 ignored(0.0f);
		return ImpactForce(dt, ignored);
	}

	// The same, and which way it pushed -- a stagger has to go somewhere.
	// `looseOnly`: blows from loose things alone (in a move, where touching
	// the scenery is the point).
	float ImpactForce(float dt, glm::vec3& direction, bool looseOnly = false)
	{
		direction = glm::vec3(0.0f);
		m_HitPush = glm::vec3(0.0f);

		if (dt <= 0.0f)
			return 0.0f;

		float worst = 0.0f;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool mineA = IsCharacter(contact.A);
			bool mineB = IsCharacter(contact.B);

			// Exactly one side must be the character. Both sides being the
			// character is the figure touching *itself* -- a forearm resting
			// against the torso, or one shin brushing the other -- and those
			// are not jointed to each other so they collide like anything else.
			// Counting them knocked the character down on the first frame it
			// stood up.
			if (mineA == mineB)
				continue;

			// The feet are on the floor permanently; only a hit somewhere else
			// is a hit.
			unsigned int mine = mineA ? contact.A : contact.B;
			if (mine == m_Foot[0] || mine == m_Foot[1])
				continue;
			// And not what his shins and feet kick on the way: a crate
			// knocked over mid-vault is not a blow.
			if (looseOnly && (!Loose(mineA ? contact.B : contact.A) || mine == m_LowerLeg[0] || mine == m_LowerLeg[1]))
				continue;

			// Nor is a leg brushing the scenery. Walking up stairs the shins
			// scrape the risers, and read as hits that put him into a stumble
			// and down the stairs -- every time, in the stairs test. Fixed or
			// driven things only: something thrown at his legs still counts.
			unsigned int other = mineA ? contact.B : contact.A;
			bool leg = mine == m_UpperLeg[0] || mine == m_UpperLeg[1] || mine == m_LowerLeg[0] || mine == m_LowerLeg[1];
			if (leg && !Loose(other))
				continue;
			// Nor a hand or arm against a wall: stopping at one, running, the
			// arms swing into it, and read as a blow that stumbled him
			// sideways along it.
			bool arm = mine == m_UpperArm[0] || mine == m_UpperArm[1] || mine == m_LowerArm[0] || mine == m_LowerArm[1];
			if (arm && !Loose(other))
				continue;
			// Just off a vault or a climb, hands and knees are still leaving
			// the top they were on.
			if (m_MoveGrace > 0.0f && !Loose(other))
				continue;
			// Posed, his limbs are immovable, so a crate he walks into takes
			// the whole of the contact and the impulse it reports is his own
			// walking. What tells the two apart is what the contact did to
			// the other body: impulse over its mass is the velocity it was
			// given. Walked into, a crate is given about his own speed; a
			// cannonball bouncing off him, twice its own. (Asked which way
			// the thing was moving instead, every hit was missed: contacts
			// are read a step after they were solved, and by then the ball
			// has already rebounded and is going away.)
			if (m_LocoPosed && Loose(other))
			{
				const GS::RigidBody3D& thing = m_World.GetBody(other);
				float given = thing.InverseMass * contact.TotalNormalImpulse();
				float walking = glm::length(m_World.GetBody(mine).Velocity);
				if (given < 1.5f * walking + 2.0f)
					continue;
			}

			float force = contact.TotalNormalImpulse() / dt;
			if (force > worst)
			{
				worst = force;

				// The manifold normal points from A to B, so it has to be
				// flipped when the character is A -- otherwise every stagger
				// goes into the thing that hit him.
				glm::vec3 push = mineA ? -contact.Normal : contact.Normal;
				// The whole of it, for a body that can now move every way --
				// a crate on the head pushes down, and the old stagger,
				// which only moves the root sideways, threw that away.
				m_HitPush = push;
				push.y = 0.0f;
				if (glm::length(push) > 1e-4f)
					direction = glm::normalize(push);
			}
		}

		return worst;
	}

	// A hit worth staggering over but not worth falling over.
	//
	// The complaint this answers: the character ragdolled abruptly at a single
	// threshold, so a shove was either nothing at all or a collapse. A person
	// hit that hard takes a couple of quick steps in the direction they were
	// pushed and stays up, and only goes down when the push outruns what a
	// step can catch.
	// An impulse is a velocity, and that is the whole model.
	//
	// This used to scale the push across an arbitrary band between two force
	// thresholds, which meant the two numbers that decided whether a person
	// stays on their feet were both invented. A hit of `J` newton-seconds on a
	// body of mass `m` changes its velocity by `J/m` -- no band, no scaling,
	// and the units come out on their own.
	void Stagger(float impulse, const glm::vec3& direction)
	{
		if (glm::length(direction) < 1e-4f || m_BodyMass <= 0.0f)
			return;

		m_StaggerVelocity += direction * (impulse / m_BodyMass);
		m_StaggerTimer = m_StaggerTime;
	}

	// How much unintended speed a step can still catch.
	//
	// A body toppling about its feet is an inverted pendulum with time
	// constant tau = sqrt(h/g), and the point it has to get a foot to is its
	// capture point, v*tau ahead. So a step of reach R catches v = R/tau, and
	// several quick steps catch a multiple of it. At the rig's numbers that is
	// about 1.5 m/s for one step.
	//
	// Note this is about *unintended* velocity only. The total is no use: a
	// run puts the capture point 1.16 m from the nearest foot as a matter of
	// course, further than any shove that used to fell him, because running is
	// controlled falling and is supposed to look like that.
	float CatchableSpeed() const
	{
		float ground = GroundY(m_World.GetBody(m_Pelvis).Position, -1000.0f);
		float height = glm::clamp(m_Balance.Com.y - ground, 0.2f, 2.0f);
		float tau = std::sqrt(height / 9.81f);

		// Reach is not the same in every direction. A leg swings a long way
		// forwards and backwards and barely at all across the body -- the feet
		// are 0.20 m apart sideways and a stride is over half a metre -- which
		// is why a shove from the side puts people down that a shove from
		// behind does not.
		float reach = m_CatchReach;
		float speed = glm::length(m_StaggerVelocity);
		if (speed > 1e-4f)
		{
			glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
			float along = std::fabs(glm::dot(m_StaggerVelocity / speed, forward));
			reach = glm::mix(m_CatchReachSide, m_CatchReach, along);
		}

		return (reach * m_CatchSteps) / std::max(tau, 0.05f);
	}

	// ---------------------------------------------------------------------
	// Stumbling: a hit the body has to catch with its own feet
	// ---------------------------------------------------------------------
	//
	// The stagger above moves a kinematic root and lets the gait chase it:
	// it decides up front, from a formula, whether he stays up. This is the
	// physical version, and what GTA IV's Euphoria characters do: the pelvis
	// goes dynamic, so a hit moves the whole body as a body, and whether he
	// stays up is decided by where his feet get to.
	//
	// Seven controllers in August failed at exactly this, and the
	// measurements say why: a leg carrying half the body's weight cannot
	// lift quickly, so by the time it swings the body has outrun it
	// (CHANGELOG 2026-08-09, "the body outruns the leg"). The real body
	// carries itself on the stance leg, which this rig's motors are too
	// soft to do quickly. So the stance leg's job is done by a **virtual
	// leg**: one force on the pelvis, the force a leg standing at the centre
	// of pressure would push with. It is bounded the way a leg is, and the
	// bounds are what make it honest:
	//
	//   * it pushes from a point inside the feet's footprint, never outside
	//     it -- so it can do what an ankle does, and nothing more. A fall
	//     that carries the capture point outside the footprint can only be
	//     caught by moving the footprint, which is a step;
	//   * along the line from that point to the centre of mass, the linear
	//     inverted pendulum's ground reaction, inside the friction cone;
	//   * only while a foot is on the ground and the stance leg can still
	//     reach it from the hip. A body that has outrun its feet loses its
	//     support and falls, which is what falling is.
	//
	// With the weight on the virtual leg, the swing foot is unloaded and
	// moves when asked, and the step lands where the capture point will be
	// when it lands -- the prediction the August steps lacked.

	// The centre of pressure, and so where a virtual leg pushes from: the
	// capture point if the footprint holds it, which brings the body to rest
	// there with time constant sqrt(h/g), or the nearest point of the
	// footprint to it, which is the best an ankle can do.
	static glm::vec2 ClosestInHull(const std::vector<glm::vec2>& hull, const glm::vec2& point)
	{
		if (hull.empty())
			return point;
		if (hull.size() < 3 || SignedDistance(hull, point) < 0.0f)
		{
			glm::vec2 best = hull[0];
			float bestDistance = std::numeric_limits<float>::max();
			for (size_t i = 0; i < hull.size(); i++)
			{
				glm::vec2 a = hull[i], b = hull[(i + 1) % hull.size()];
				glm::vec2 edge = b - a;
				float length2 = glm::dot(edge, edge);
				float t = length2 > 1e-10f ? glm::clamp(glm::dot(point - a, edge) / length2, 0.0f, 1.0f) : 0.0f;
				glm::vec2 on = a + edge * t;
				float distance = glm::length(point - on);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = on;
				}
			}
			return best;
		}
		return point;
	}

	// A foot touching something that is not himself.
	bool FootTouching(int side) const
	{
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			if (contact.PointCount == 0)
				continue;
			if (contact.A == m_Foot[side] && !IsCharacter(contact.B))
				return true;
			if (contact.B == m_Foot[side] && !IsCharacter(contact.A))
				return true;
		}
		return false;
	}

	// The sole's four corners, flattened -- the footprint, from the foot's
	// own box rather than from contact points. The manifold keeps 1.84 of a
	// planted foot's 4 corners on average (CHANGELOG 2026-08-10), which
	// would make the footprint flicker between a line and a polygon.
	void AppendSole(int side, std::vector<glm::vec2>& out) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		glm::vec3 h = foot.HalfExtents;
		for (float x : { -h.x, h.x })
			for (float z : { -h.z, h.z })
			{
				glm::vec3 corner = foot.Position + foot.Orientation * glm::vec3(x, -h.y, z);
				out.push_back({ corner.x, corner.z });
			}
	}

	glm::vec3 AnkleWorld(int side) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		return foot.Position + foot.Orientation * m_FootToAnkle;
	}

	float SoleHeight(int side) const
	{
		const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
		return foot.Position.y - foot.HalfExtents.y;
	}

	// Physically from standing or a walk, on the ground; a run keeps the
	// kinematic stagger. Stopping a walk costs as much as a 110 Ns shove
	// from standing, so a stumble from a walk does not try to stop: it
	// carries the walk's velocity and steps to keep it (the b offset in
	// UpdateStumble), and hands back to the gait once it is walking at it
	// again. Measured, 4 trials each: 20 Ns 16/16 walk on, 40 Ns 14/16,
	// 60-80 about half; a 1 Ns nudge is handed back after one or two steps
	// at 1.14-1.52 m/s against the walk's 1.30. Running, the capture point
	// is 1.3 m ahead as a matter of course and every physical hit from
	// 20 Ns felled him (64 of 64), so a run stays kinematic.
	bool StumblesPhysically() const
	{
		// From the locomotion, standing only: walking on, he keeps his feet
		// and the keys and is pushed off his line (the stagger, as a run's
		// hit always was). The physical stumble was tuned against the old
		// walk's body; handed a posed walk mid-stride it ran away forward --
		// 2.3 m/s, the trunk 25 degrees over -- and a 20 N s nudge felled him
		// 12 times in 32, where the old walk caught all 32.
		return m_PhysicalStumble && m_Speed <= m_StumbleEntrySpeed && m_Air == Air::Grounded;
	}

	// The crouch -- how far below standing the centre of mass is wanted --
	// at which a leg from `hip` to `ankle` is at `stretch` of its length.
	// Absolute, from where the hip would have to be: an earlier version
	// added "how much further down" to the crouch it already had, which
	// only ever ratcheted down, and a figure that had crouched once stayed
	// crouched and stepped until it gave up.
	float CrouchToReach(const glm::vec3& hip, const glm::vec3& ankle, float stretch) const
	{
		float legLength = m_ThighLength + m_ShinLength;
		glm::vec2 across(hip.x - ankle.x, hip.z - ankle.z);
		float reach = stretch * legLength;
		float vertical = std::sqrt(std::max(reach * reach - glm::dot(across, across), 0.0f));
		float hipAboveCom = hip.y - m_Balance.Com.y;
		float comWanted = ankle.y + vertical - hipAboveCom;
		return m_StumbleGround + m_StumbleComHeight - comWanted;
	}

	void BeginStumble()
	{
		LocoRelease();
		UnpinFeet();
		m_Mode = Mode::Stumbling;
		m_StumbleTimer = 0.0f;
		m_StumbleSteps = 0;
		m_StumbleSwing = -1;
		m_StumbleCalm = 0.0f;
		m_StumbleUnsupported = 0.0f;
		m_StumbleCrouch = 0.0f;
		m_StumbleLastSwing = -1;
		m_TouchMemory[0] = m_TouchMemory[1] = 0.0f;
		m_StumbleGathering = false;
		m_StaggerVelocity = glm::vec3(0.0f);

		// The walk he was in, which the stumble keeps going rather than
		// trying to stop: a person bumped mid-stride walks on.
		glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		m_StumbleCarry = m_Speed > 0.1f ? glm::vec2(forward.x, forward.z) * m_Speed : glm::vec2(0.0f);

		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		pelvis.Type = GS::BodyType::Dynamic;
		pelvis.Awake = true;
		pelvis.SleepTimer = 0.0f;

		// Where he stands is where each foot is now, not where the gait
		// last meant it to be.
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = AnkleWorld(side);
		}
		m_StumbleGround = std::min(SoleHeight(0), SoleHeight(1));
		m_StumbleComHeight = m_StandComHeight;
	}

	// What a step must do, in the inverted pendulum's own terms: with the
	// centre of pressure held at `p`, the capture point runs away from it as
	// xi(t) = p + (xi0 - p) e^(t/tau). A foot landing in `remaining` seconds
	// has to go where xi will be then, not where it is now.
	static glm::vec2 PredictCapture(const glm::vec2& capture, const glm::vec2& pressure, float tau, float remaining)
	{
		return pressure + (capture - pressure) * std::exp(remaining / std::max(tau, 0.05f));
	}

	void UpdateStumble(float dt)
	{
		m_StumbleTimer += dt;
		const BalanceState& state = m_Balance;
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		// Down means on the ground, asked of the geometry as well as the
		// contacts. A planted foot drops out of the manifold for several
		// steps at a time as it settles -- every time it did, the support
		// vanished and the feet stopped taking turns (the same foot was
		// picked again because the other "was not down"), which walked a
		// forward shove out into the splits. A sole within 1.5 cm of the
		// ground under it is down.
		bool touching[2];
		for (int side = 0; side < 2; side++)
		{
			const GS::RigidBody3D& foot = m_World.GetBody(m_Foot[side]);
			float ground = GroundY(foot.Position, -1000.0f);
			m_FootGround[side] = ground;
			touching[side] = FootTouching(side) || SoleHeight(side) - ground < 0.015f;
			if (touching[side])
				m_TouchMemory[side] = 0.0f;
			else
				m_TouchMemory[side] += dt;   // how long it has been off the ground
		}
		// A swinging foot is not support, even when it scuffs the floor.
		bool support[2] = { touching[0] && m_StumbleSwing != 0, touching[1] && m_StumbleSwing != 1 };

		std::vector<glm::vec2> soles;
		for (int side = 0; side < 2; side++)
			if (support[side])
				AppendSole(side, soles);
		std::vector<glm::vec2> footprint = ConvexHull(soles);
		m_StumbleFootprint = footprint;

		for (int side = 0; side < 2; side++)
			if (support[side])
				m_StumbleGround = glm::mix(m_StumbleGround, m_FootGround[side], 0.2f);

		// Measured from the ground he is standing on. BalanceState's own
		// capture point measures height from y = 0, which is the demo's
		// floor and nobody else's.
		float height = std::max(state.Com.y - m_StumbleGround, 0.2f);
		float tau = std::sqrt(height / 9.81f);
		glm::vec2 capture = state.ComGround + glm::vec2(state.ComVelocity.x, state.ComVelocity.z) * tau;
		m_StumbleCapture = capture;
		// Where to push from: the capture point, plus a little beyond it
		// away from the footprint's middle -- the capture point controller
		// (xi' = (xi - p)/tau, so p past xi drives xi back towards the
		// middle). Pushing from xi itself stops the body wherever it is, and
		// measured, that left him at rest leaning over the edge of one foot
		// with the capture point 2 cm inside it -- caught, but not upright.
		glm::vec2 centre(0.0f);
		for (const glm::vec2& point : footprint)
			centre += point;
		if (!footprint.empty())
			centre /= (float)footprint.size();
		// Walking on, the ankles regulate speed instead of position: the
		// pendulum's x'' = (g/h)(x - p), so pressing at p = x - a h / g asks
		// for an acceleration a, and a is chosen to pull the body's velocity
		// to the walk's own -- forward and sideways at once -- as far as the
		// footprint lets it. Two earlier versions, both measured: pressing at
		// the capture point (the standing rule) braked him into a 0.45 m/s
		// shuffle; pressing at the footprint's middle let the speed run away,
		// a 20 Ns bump at a walk building to 3.5 m/s over two seconds until he
		// went over.
		bool walking = glm::length(m_StumbleCarry) > 0.1f;
		glm::vec2 aim = capture + m_StumbleCentring * (capture - centre);
		if (walking)
		{
			glm::vec2 velocity(state.ComVelocity.x, state.ComVelocity.z);
			glm::vec2 wanted = m_StumbleSpeedGain * (m_StumbleCarry - velocity);
			aim = state.ComGround - wanted * height / 9.81f;
		}
		glm::vec2 pressure = ClosestInHull(footprint, aim);
		float margin = footprint.size() >= 3 ? SignedDistance(footprint, capture) : -1.0f;
		m_StumbleMargin = margin;

		// --- the virtual leg ---------------------------------------------
		//
		// How much of a leg there is: none without a foot down, and fading
		// out as the hip runs out of reach of the stance foot. A leg at full
		// stretch pushes nothing -- and without this the body could be held
		// up at standing height while it slid past its own feet.
		float reachable = 0.0f;
		float legLength = m_ThighLength + m_ShinLength;
		for (int side = 0; side < 2; side++)
		{
			if (!support[side])
				continue;
			float stretch = glm::length(HipWorld(side) - AnkleWorld(side)) / legLength;
			// Fading out from 97% of the leg to just past straight (the
			// joints give a little). It first faded over 8% past straight,
			// and measured, a walking lunge then heaved the hips back up
			// hard enough to lift the stance foot 12 cm off the floor -- a
			// leg pushing beyond its own length -- and he fell off his own
			// support. Faded by 100%, it gave a straight-legged stance
			// almost none (the rig stands with its legs straight), so the
			// stumble crouches: m_StumbleRestCrouch.
			reachable = std::max(reachable, glm::clamp((1.01f - stretch) / 0.04f, 0.0f, 1.0f));
		}
		m_StumbleSupport = reachable;
		m_StumbleUnsupported = reachable > 0.0f ? 0.0f : m_StumbleUnsupported + dt;

		glm::vec3 force(0.0f);
		if (reachable > 0.0f && !footprint.empty())
		{
			// Height: held at standing, lowered by however much the swing leg
			// needs to reach -- a lunge drops the hips.
			float wanted = m_StumbleGround + m_StumbleComHeight - m_StumbleCrouch;
			float vertical = m_BodyMass * (9.81f
				+ m_StumbleHeightGain * (wanted - state.Com.y)
				- 2.0f * std::sqrt(m_StumbleHeightGain) * state.ComVelocity.y);
			vertical = glm::clamp(vertical, 0.0f, m_StumbleMaxSupport * m_BodyMass * 9.81f) * reachable;

			// Across: the pendulum's own ground reaction, F_y (x - p) / h,
			// which is what makes the body fall away from where it pushes.
			glm::vec2 lever = state.ComGround - pressure;
			glm::vec2 across = vertical * lever / height;
			float friction = m_StumbleFriction * vertical;
			if (glm::length(across) > friction)
				across *= friction / glm::length(across);

			force = glm::vec3(across.x, vertical, across.y);
			m_World.ApplyForce(m_Pelvis, force);
		}
		m_StumbleForce = force;

		// --- upright ------------------------------------------------------
		//
		// The stance hip's torque, applied straight to the pelvis: it keeps
		// the trunk over the legs. Only as much as there is a leg to push
		// against, so a figure that has lost its feet topples freely.
		{
			glm::quat target = glm::angleAxis(m_Facing, up);
			glm::quat error = target * glm::conjugate(pelvis.Orientation);
			if (error.w < 0.0f)
				error = -error;
			glm::vec3 axis(error.x, error.y, error.z);
			float sinHalf = glm::length(axis);
			glm::vec3 rotation = sinHalf > 1e-6f
				? axis / sinHalf * (2.0f * std::atan2(sinHalf, error.w)) : glm::vec3(0.0f);
			glm::vec3 torque = m_StumbleUprightGain * rotation - m_StumbleUprightDamping * pelvis.AngularVelocity;
			float limit = m_StumbleMaxTorque * reachable;
			if (glm::length(torque) > limit)
				torque *= limit / std::max(glm::length(torque), 1e-6f);
			m_World.ApplyTorque(m_Pelvis, torque);
		}

		// --- a step --------------------------------------------------------
		glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
		glm::vec3 right(std::cos(m_Facing), 0.0f, -std::sin(m_Facing));
		glm::vec2 forward2(forward.x, forward.z), right2(right.x, right.z);

		if (m_StumbleCooldown > 0.0f)
			m_StumbleCooldown -= dt;

		// A foot left off the ground and not stepping -- outrun, or left
		// behind at the end of a split -- is brought back beside the other.
		// Measured without this: caught over one foot with the other hanging
		// 0.57 m away, out of reach of the floor, for the rest of the run.
		if (m_StumbleSwing < 0 && m_StumbleCooldown <= 0.0f && m_SteppingCatch && margin >= -m_StumbleTrigger)
		{
			for (int side = 0; side < 2; side++)
			{
				if (support[1 - side] && !support[side] && m_TouchMemory[side] > 0.1f)
				{
					m_StumbleSwing = side;
					m_StumbleGathering = true;
					m_StumbleSwingTimer = 0.0f;
					m_SwingFrom[side] = AnkleWorld(side);
					break;
				}
			}
		}

		if (m_StumbleSwing < 0 && m_StumbleCooldown <= 0.0f && m_SteppingCatch
			&& margin < -m_StumbleTrigger && (support[0] || support[1]))
		{
			glm::vec2 away = capture - pressure;
			float lateral = glm::dot(away, right2);
			float along = glm::dot(away, forward2);

			// Sideways: the foot on the side of the fall, or the legs cross.
			// Forwards or back: whichever foot is furthest behind the fall,
			// which gains the most by moving -- and only a foot that is not
			// the only one holding him up.
			// After the first step the feet take turns: the next step is the
			// other foot's, which is a shuffle sideways (it lands beside the
			// one that went out, never across it) and a run of steps
			// forwards. Stepping the same foot again splits the legs.
			int swing;
			if (m_StumbleSteps > 0 && m_StumbleLastSwing >= 0)
				swing = 1 - m_StumbleLastSwing;
			else if (std::fabs(lateral) > 0.7f * std::fabs(along))
				swing = lateral > 0.0f ? 1 : 0;
			else
			{
				glm::vec2 direction = glm::normalize(away);
				glm::vec2 foot0(AnkleWorld(0).x, AnkleWorld(0).z), foot1(AnkleWorld(1).x, AnkleWorld(1).z);
				swing = glm::dot(foot0, direction) < glm::dot(foot1, direction) ? 0 : 1;
			}
			if (!support[1 - swing])
				swing = 1 - swing;

			if (support[1 - swing])
			{
				m_StumbleSwing = swing;
				m_StumbleGathering = false;
				m_StumbleLastSwing = swing;
				m_StumbleSwingTimer = 0.0f;
				m_SwingFrom[swing] = AnkleWorld(swing);
				m_StumbleSteps++;
			}
		}

		// And as far as the legs standing on the ground need, to stay bent:
		// a long split stance with the hips held high is two straight legs,
		// and a straight leg pushes nothing (the fade above). Measured
		// without it, a walking stumble crept on at half a metre a second
		// over legs at 98% stretch, never quite caught.
		float crouchWanted = m_StumbleRestCrouch;
		for (int side = 0; side < 2; side++)
			if (support[side])
				crouchWanted = std::max(crouchWanted, CrouchToReach(HipWorld(side), AnkleWorld(side), 0.94f));
		if (m_StumbleSwing >= 0)
		{
			int swing = m_StumbleSwing, stance = 1 - swing;
			m_StumbleSwingTimer += dt;
			float t = glm::clamp(m_StumbleSwingTimer / m_StumbleStepTime, 0.0f, 1.0f);

			// Re-aimed every step at where the capture point will be when
			// the foot lands, plus a little, so it lands just past it.
			float remaining = std::max(m_StumbleStepTime - m_StumbleSwingTimer, 0.0f);
			glm::vec2 landing = PredictCapture(capture, pressure, tau, remaining);
			glm::vec2 travel = landing - pressure;
			if (glm::length(travel) > 1e-4f && !walking)
				landing += glm::normalize(travel) * m_StumbleOvershoot;

			// Walking on: land short of the capture point by the offset that
			// keeps it moving. With the pressure at the stance foot and a
			// step every T, the capture point runs from b ahead of the foot
			// to b e^(T/tau); landing the next foot b behind that, a stride L
			// on, repeats it exactly when b e^(T/tau) - L = b, so
			//     b = L / (e^(T/tau) - 1),   L = v T.
			// b = 0 is landing on the capture point, which stops him -- the
			// standing stumble is the v = 0 case of the same rule.
			if (walking)
			{
				float period = m_StumbleStepTime + m_StumbleSettle;
				float stride = glm::length(m_StumbleCarry) * period;
				float offset = stride / std::max(std::exp(period / tau) - 1.0f, 0.05f);
				landing -= glm::normalize(m_StumbleCarry) * offset;
			}
			// A gathering foot goes beside the capture point, at a normal
			// stance width, to stand on rather than to catch with.
			if (m_StumbleGathering)
				landing = capture + right2 * (swing == 1 ? 0.5f : -0.5f) * m_StumbleStance;

			// Not across the other leg: the swing foot keeps to its own side
			// of the stance foot.
			glm::vec2 stanceFoot(AnkleWorld(stance).x, AnkleWorld(stance).z);
			float side = glm::dot(landing - stanceFoot, right2);
			float minimum = swing == 1 ? m_StumbleFootGap : -m_StumbleFootGap;
			if ((swing == 1 && side < minimum) || (swing == 0 && side > minimum))
				landing += right2 * (minimum - side);

			// And no further from the hip than a leg goes.
			glm::vec3 hip = HipWorld(swing);
			glm::vec2 hip2(hip.x, hip.z);
			glm::vec2 fromHip = landing - hip2;
			if (glm::length(fromHip) > m_StumbleReach)
				landing = hip2 + fromHip * (m_StumbleReach / glm::length(fromHip));
			m_StumbleTarget = landing;

			// The hips drop as far as the leg needs to get there.
			crouchWanted = std::max(crouchWanted, CrouchToReach(hip, glm::vec3(landing.x, m_StumbleGround + m_AnkleHeight, landing.y), 0.92f));

			glm::vec3 to(landing.x, m_StumbleGround + m_AnkleHeight, landing.y);
			glm::vec3 foot = glm::mix(m_SwingFrom[swing], to, glm::smoothstep(0.0f, 1.0f, t));
			foot.y += m_StumbleLift * std::sin(glm::pi<float>() * t);
			// Pressed into the ground at the end, so it lands rather than
			// arriving exactly at floor height and hovering there: one step
			// in the first trace timed out a few millimetres up.
			foot.y -= m_StumblePress * glm::smoothstep(0.7f, 1.0f, t);
			SolveLeg(swing, hip, foot, dt);

			// Nothing pulls the foot but the leg's own motors. A force on the
			// foot with its reaction on the pelvis was tried, and measured
			// it caught no more: the foot was never the slow part once the
			// virtual leg had the weight off it.
			// Landed means a real contact. The geometric "within 1.5 cm"
			// that keeps a standing foot counted as down also fires as a low
			// swing skims the floor, and measured, it called a step landed
			// half a metre short of where it was going.
			bool landed = (t > 0.5f && FootTouching(swing)) || m_StumbleSwingTimer > 1.5f * m_StumbleStepTime;
			if (landed)
			{
				m_Planted[swing] = AnkleWorld(swing);
				m_StumbleSwing = -1;
				m_StumbleCooldown = m_StumbleSettle;
			}
		}
		crouchWanted = std::min(crouchWanted, m_StumbleMaxCrouch);
		m_StumbleCrouch += glm::clamp(crouchWanted - m_StumbleCrouch, -m_StumbleCrouchRate * dt, m_StumbleCrouchRate * dt);

		// The stance legs hold the feet where they were put.
		for (int side = 0; side < 2; side++)
			if (side != m_StumbleSwing)
				SolveLeg(side, HipWorld(side), m_Planted[side], dt);

		// Arms out for balance, further the worse it gets -- the windmill
		// that reads at once as someone about to go over.
		float alarm = glm::clamp(-margin / 0.3f, 0.0f, 1.0f);
		for (int side = 0; side < 2; side++)
		{
			float out = (side == 0 ? 1.0f : -1.0f) * (0.35f + 0.9f * alarm);
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation = glm::normalize(
				glm::angleAxis(out, glm::vec3(0.0f, 0.0f, 1.0f))
				* glm::angleAxis(-0.4f * alarm, glm::vec3(1.0f, 0.0f, 0.0f)) * m_ShoulderRest[side]);
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = -0.5f * alarm;
		}
		m_World.GetJoint(m_SpineJoint).MotorTargetRotation = m_SpineRest;

		// --- out of it, one way or the other -------------------------------
		glm::vec3 trunkUp = m_World.GetBody(m_Torso).Orientation * up;
		float tilt = std::acos(glm::clamp(trunkUp.y, -1.0f, 1.0f));
		if (tilt > glm::radians(m_StumbleGiveUpTilt))
		{
			GoRagdoll("toppled");
			return;
		}
		if (state.Com.y - m_StumbleGround < m_StumbleGiveUpHeight)
		{
			GoRagdoll("buckled");
			return;
		}
		if (m_StumbleUnsupported > 0.3f)
		{
			GoRagdoll("lost his feet");
			return;
		}
		// Walking on, steps are how he moves at all, so more of them are
		// allowed before it counts as not catching it.
		int maxSteps = walking ? 2 * m_StumbleMaxSteps : m_StumbleMaxSteps;
		if (m_StumbleSteps > maxSteps || m_StumbleTimer > m_StumbleMaxTime)
		{
			GoRagdoll("could not catch it");
			return;
		}

		// Caught: both feet down, the capture point well inside them, nearly
		// still, and staying so -- then the walk takes him back from where
		// he stands.
		glm::vec2 horizontalVelocity(state.ComVelocity.x, state.ComVelocity.z);
		bool calm = m_StumbleSwing < 0 && support[0] && support[1] && margin > m_StumbleCalmMargin
			&& glm::length(horizontalVelocity) < m_StumbleCalmSpeed && tilt < glm::radians(8.0f);
		if (walking)
		{
			// Caught, walking: a step has landed, both feet are down, and he
			// is moving at the walk's own velocity again, upright -- then the
			// gait takes him back without stopping.
			calm = m_StumbleSwing < 0 && support[0] && support[1] && m_StumbleSteps > 0
				&& glm::length(horizontalVelocity - m_StumbleCarry) < m_StumbleCalmWalk && tilt < glm::radians(10.0f);
			if (calm)
			{
				EndStumble();
				return;
			}
		}
		m_StumbleCalm = calm ? m_StumbleCalm + dt : 0.0f;
		if (m_StumbleCalm > m_StumbleCalmTime)
			EndStumble();
	}

	// Back to walking from wherever the stumble left him.
	void EndStumble()
	{
		GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		glm::vec3 front = pelvis.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		if (glm::length(glm::vec2(front.x, front.z)) > 0.01f)
			m_Facing = std::atan2(front.x, front.z);
		m_StandHeight = pelvis.Position.y;
		glm::vec3 planted[2] = { AnkleWorld(0), AnkleWorld(1) };
		GoControlled();
		pelvis.Velocity = glm::vec3(0.0f);
		// Walking on at the speed he was walking, along the way he faces.
		float carried = glm::length(m_StumbleCarry);
		if (carried > 0.1f)
		{
			m_Facing = std::atan2(m_StumbleCarry.x, m_StumbleCarry.y);
			m_Speed = carried;
			m_SpeedRate = 0.0f;
		}
		for (int side = 0; side < 2; side++)
		{
			m_Planted[side] = planted[side];
			m_SwingFrom[side] = planted[side];
		}
		for (int side = 0; side < 2; side++)
		{
			m_World.GetJoint(m_ShoulderJoint[side]).MotorTargetRotation = m_ShoulderRest[side];
			m_World.GetJoint(m_ElbowJoint[side]).MotorTargetAngle = 0.0f;
		}
	}

	// ---------------------------------------------------------------------
	// Springs
	// ---------------------------------------------------------------------
	//
	// (The walk itself is Runtime/Locomotion.h.)
	// x'' = w^2 (target - x) - 2 w x': reaches the target without
	// overshoot, and with no step in its acceleration anywhere.
	static void Spring(float& x, float& rate, float target, float omega, float dt)
	{
		rate += (omega * omega * (target - x) - 2.0f * omega * rate) * dt;
		x += rate * dt;
	}
	// The same following a target that moves at `targetRate`: damped
	// towards that rate rather than towards rest, so a steady climb is
	// followed with no lag at all (the plain spring trails it by 2 r / w).
	static void Spring(float& x, float& rate, float target, float omega, float dt, float targetRate)
	{
		rate += (omega * omega * (target - x) - 2.0f * omega * (rate - targetRate)) * dt;
		x += rate * dt;
	}

	// ---------------------------------------------------------------------
	// Feet
	// ---------------------------------------------------------------------
	//
	// Whatever takes him -- a move, a stumble, a fall, a get-up -- has his
	// legs colliding again. (The old walk pinned the feet kinematically and
	// let them go here; nothing pins them now.)
	void UnpinFeet()
	{
		SetLegsColliding(true);
	}

	// The legs' own contacts, off while the feet are driven (see
	// RigidBody3D::NoCollideGroup) and back the moment he is hit or falls,
	// so a falling body's legs still land on each other.
	void SetLegsColliding(bool colliding)
	{
		int group = colliding ? 0 : (int)m_Pelvis + 1;
		for (int side = 0; side < 2; side++)
		{
			for (auto handle : { m_UpperLeg[side], m_LowerLeg[side], m_Foot[side] })
				m_World.GetBody(handle).NoCollideGroup = group;
			// And the scenery: a shin catching a riser between two driven
			// ends could only fight the joints (RigidBody3D::IgnoreStatic).
			for (auto handle : { m_UpperLeg[side], m_LowerLeg[side] })
				m_World.GetBody(handle).IgnoreStatic = !colliding;
		}
	}

	// Where this foot is in its own cycle: 0 the instant it lands, wrapping
	// back to 0 at the next landing.
	float CyclePosition(int side) const
	{
		float phase = m_GaitPhase + (side == 0 ? 0.0f : glm::pi<float>())
			- 1.5f * glm::pi<float>();
		float c = std::fmod(phase / glm::two_pi<float>(), 1.0f);
		return c < 0.0f ? c + 1.0f : c;
	}

	// Progress through a flight window, or -1 when a foot is down. There are
	// two windows per cycle, each (0.5 - duty) long, so this is zero for any
	// duty of half or more -- a walk never leaves the ground.
	float Airborne() const
	{
		if (m_Duty >= 0.5f)
			return -1.0f;

		float c = CyclePosition(0);
		float window = 0.5f - m_Duty;

		if (c >= m_Duty && c < 0.5f)
			return (c - m_Duty) / window;
		if (c >= m_Duty + 0.5f)
			return (c - m_Duty - 0.5f) / window;

		return -1.0f;
	}

	// The hip joint's position in the world, which is what the leg has to
	// reach from.
	glm::vec3 HipWorld(int side) const
	{
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		return pelvis.Position + pelvis.Orientation * m_HipLocal[side];
	}

	// Two-link inverse kinematics: given where the foot should be, what does
	// the hip point at and how far is the knee bent?
	//
	// The law of cosines twice. The knee angle comes from the triangle made by
	// the two leg segments and the straight-line distance to the target; the
	// thigh direction is the direction to the target, rotated by the angle
	// between the thigh and that line. Everything is done in the pelvis's
	// frame, where the knee's hinge axis is +x.
	// Two-link inverse kinematics, for any limb.
	//
	// Given where the end of the limb should be, what does the proximal joint
	// point at and how far is the distal hinge bent? The law of cosines twice,
	// solved in the frame of whatever the limb hangs off -- the pelvis for a
	// leg, the torso for an arm -- where the hinge axis is +x.
	//
	// `bendSign` is +1 for a knee and -1 for an elbow: the two hinges are
	// mirror images, which is why their limits are (0, 130) and (-140, 0).
	void SolveLimb(const glm::quat& rootOrientation, const glm::vec3& anchorWorld,
		const glm::vec3& endWorld, float upperLength, float lowerLength,
		GS::PhysicsWorld3D::BodyHandle proximal, const glm::quat& proximalRest,
		GS::PhysicsWorld3D::BodyHandle distal, float bendSign)
	{
		glm::vec3 toEnd = glm::conjugate(rootOrientation) * (endWorld - anchorWorld);

		float reach = glm::length(toEnd);
		if (reach < 1e-4f)
			return;

		// A limb cannot be longer than itself, and a joint locked exactly
		// straight is a singularity the law of cosines does not enjoy -- so
		// the target is pulled just inside what the limb can do.
		float maximum = (upperLength + lowerLength) * m_LegCap;
		float minimum = std::fabs(upperLength - lowerLength) + 0.01f;
		reach = glm::clamp(reach, minimum, maximum);

		glm::vec3 direction = glm::normalize(toEnd);

		// Interior angle at the distal hinge, and the bend away from straight.
		float cosDistal = (upperLength * upperLength + lowerLength * lowerLength
			- reach * reach) / (2.0f * upperLength * lowerLength);
		float bend = glm::pi<float>() - std::acos(glm::clamp(cosDistal, -1.0f, 1.0f));

		// Angle between the upper segment and the straight line to the end.
		float cosProximal = (upperLength * upperLength + reach * reach
			- lowerLength * lowerLength) / (2.0f * upperLength * reach);
		float alpha = std::acos(glm::clamp(cosProximal, -1.0f, 1.0f));

		// Rotated about the hinge's own axis, which is what makes it bend the
		// right way rather than sideways. The lateral component of the target
		// is carried by the proximal joint pointing at it, so this stays a
		// planar solve.
		glm::quat lift = glm::angleAxis(-alpha * bendSign, glm::vec3(1.0f, 0.0f, 0.0f));
		glm::vec3 upper = lift * direction;

		glm::quat aim = RotationBetween(glm::vec3(0.0f, -1.0f, 0.0f), upper);

		m_World.GetJoint(proximal).MotorTargetRotation =
			glm::normalize(aim * proximalRest);
		m_World.GetJoint(distal).MotorTargetAngle = bend * bendSign;
	}

	// The shoulder's position in the world, which is what an arm reaches from.
	glm::vec3 ShoulderWorld(int side) const
	{
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
		return torso.Position + torso.Orientation * m_ShoulderLocal[side];
	}

	// Put a hand somewhere, the elbow pointing the way an elbow does: back
	// and down, in the torso's own frame -- so it stays natural however he
	// is turned, lying, rolling or hanging. The upper arm is given its whole
	// orientation, direction and twist, as SolveLegPole gives the thigh, and
	// the elbow its bend. It used to be aimed with the shortest rotation from
	// hanging (SolveLimb), which leaves the twist to chance: with the hands
	// overhead that rotation is all but undefined, and rolling over in the
	// get-up the elbows bent sideways (the hinge 4 degrees off square to his
	// sides) while the arms whipped round at up to 580 deg/s.
	void SolveArm(int side, const glm::vec3& handWorld)
	{
		const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
		glm::vec3 shoulder = ShoulderWorld(side);
		float upper = m_UpperArmLength, fore = m_ForearmLength;
		glm::vec3 toHand = handWorld - shoulder;
		float length = glm::length(toHand);
		if (length < 1e-4f)
			return;
		glm::vec3 d = toHand / length;
		// No nearer than the elbow can fold to, 5 degrees short of its stop:
		// a hand asked for beside the shoulder -- the bottom of a push-up --
		// wanted 150-167 degrees of an elbow that stops at 140, and the motor
		// driving into the stop flung the arm about.
		GS::Joint3D& elbowJoint = m_World.GetJoint(m_ElbowJoint[side]);
		float foldMost = std::max(-elbowJoint.LowerLimit - glm::radians(5.0f), 0.0f);
		float nearest = std::sqrt(upper * upper + fore * fore + 2.0f * upper * fore * std::cos(foldMost));
		float reach = glm::clamp(length, std::max(nearest, std::fabs(upper - fore) + 0.01f), (upper + fore) * m_LegCap);
		glm::vec3 tx = torso.Orientation * glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 ty = torso.Orientation * glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 tz = torso.Orientation * glm::vec3(0.0f, 0.0f, 1.0f);
		// Back and down. With no outward lean in it, an arm hanging at his
		// side solves to exactly the pose it was built in -- the one the walk
		// holds it in -- so nothing turns when one hands over to the other.
		glm::vec3 pole = -tz - 0.5f * ty;
		glm::vec3 e = pole - d * glm::dot(pole, d);
		if (glm::length(e) < 0.25f)
		{
			// Reaching back and down along it: down and out instead.
			pole = -ty + tx * (side == 0 ? -1.0f : 1.0f);
			e = pole - d * glm::dot(pole, d);
		}
		e = glm::normalize(e);
		float alpha = std::acos(glm::clamp((upper * upper + reach * reach - fore * fore) / (2.0f * upper * reach), -1.0f, 1.0f));
		glm::vec3 elbow = shoulder + (d * std::cos(alpha) + e * std::sin(alpha)) * upper;
		// The bone's frame as it was built: +y up it to the shoulder, +z the
		// way the forearm folds (away from where the elbow points), +x the
		// hinge. Built hanging, that frame is the torso's, so the arm's own
		// turn within it is the shoulder's rest.
		glm::vec3 y = glm::normalize(shoulder - elbow);
		glm::vec3 z = -e - y * glm::dot(-e, y);
		z = glm::length(z) > 1e-4f ? glm::normalize(z) : tz;
		glm::quat upperWorld = glm::normalize(glm::quat_cast(glm::mat3(glm::cross(y, z), y, z)));
		glm::quat target = glm::normalize(glm::conjugate(torso.Orientation) * upperWorld * m_ShoulderRest[side]);
		// And inside the shoulder's cone, 4 degrees in, for the same reason:
		// a target past it is a motor pushing against a limit that pushes
		// back, and that is a flail (an arm at 4,700 deg/s getting up).
		GS::Joint3D& shoulderJoint = m_World.GetJoint(m_ShoulderJoint[side]);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		glm::vec3 restDown = shoulderJoint.ConeTwistRest * down, wantDown = target * down;
		float swing = std::acos(glm::clamp(glm::dot(restDown, wantDown), -1.0f, 1.0f));
		float room = shoulderJoint.ConeAngle - glm::radians(4.0f);
		glm::vec3 back = glm::cross(wantDown, restDown);
		if (swing > room && glm::length(back) > 1e-5f)
			target = glm::normalize(glm::angleAxis(swing - room, glm::normalize(back)) * target);
		shoulderJoint.MotorTargetRotation = target;
		float cosElbow = (upper * upper + fore * fore - reach * reach) / (2.0f * upper * fore);
		elbowJoint.MotorTargetAngle = -(glm::pi<float>() - std::acos(glm::clamp(cosElbow, -1.0f, 1.0f)));
	}

	void SolveLeg(int side, const glm::vec3& hipWorld, const glm::vec3& footWorld, float dt)
	{
		SolveLimb(m_World.GetBody(m_Pelvis).Orientation, hipWorld, footWorld,
			m_ThighLength, m_ShinLength,
			m_HipJoint[side], m_HipRest[side], m_KneeJoint[side], 1.0f);

		// And keep the sole level with the ground rather than pointing
		// wherever the shin happens to. Without this the foot lands on an edge
		// and rolls, which is exactly the scuffing this was meant to stop.
		//
		// The ankle motor is a *relative* target, so it has to be expressed in
		// the shin's frame -- and the shin is turning. Aiming at where the shin
		// is now means aiming at where it was by the time the solver runs, and
		// the sole ends up trailing the lean by the shin's rate over the
		// motor's stiffness. So the shin is integrated forward one step first.
		// This is the whole reason the sole was not flat: not the cone limit,
		// which never engaged, and not torque, but chasing a moving target.
		const GS::RigidBody3D& shin = m_World.GetBody(m_LowerLeg[side]);

		glm::quat spin(0.0f, shin.AngularVelocity.x, shin.AngularVelocity.y,
			shin.AngularVelocity.z);
		glm::quat ahead = glm::normalize(shin.Orientation
			+ 0.5f * dt * spin * shin.Orientation);

		// "Level" means level with **the ground here**, not with the world.
		//
		// A sole held flat to the horizon lands on its heel or its toe on any
		// slope, and only the leading edge touches: measured walking across
		// generated terrain, the lowest corner of the planted foot sat 4.61 cm
		// above the surface against 0.54 cm on a flat floor. The foot is asked
		// about the ground it is *going* to, not the ground it is over, so it
		// arrives already matching.
		glm::quat level = glm::angleAxis(m_Facing, glm::vec3(0.0f, 1.0f, 0.0f));
		m_World.GetJoint(m_AnkleJoint[side]).MotorTargetRotation =
			glm::normalize(glm::conjugate(ahead) * ConformToGround(footWorld, side) * level);
	}

	// A leg solved with a pole: the knee bends towards `pole` (the foot's
	// forward), in the plane of hip, ankle and pole, and the thigh and shin
	// are given whole orientations -- direction *and* twist -- so the knee's
	// hinge is square to that plane. SolveLimb aims the thigh with the
	// shortest rotation, which leaves its twist to chance: fine for a leg
	// whose foot follows it, but with the foot driven the hinge was forced
	// out of its plane and the knee bent backwards in turns (to -118 degrees
	// in a zigzag, read off the joint itself).
	//
	// `kneeFloor`, when given, is the lowest the knee may be. A target that
	// bends the knee through the floor -- legs out behind him lying face
	// down, a little short of straight, bend the knee 11 cm down into it --
	// is a motor pushing a limb against the floor's contact, and the motor
	// wins by centimetres. So the knee is kept on the floor instead and the
	// ankle put where the shin then reaches, towards its target: the leg
	// lies straighter, as a leg on a floor does.
	void SolveLegPole(int side, const glm::vec3& hip, glm::vec3 ankle, const glm::vec3& pole,
		float kneeFloor = -1e9f)
	{
		float thigh = m_ThighLength, shin = m_ShinLength;
		if (kneeFloor > -1e8f)
			ankle = KeepKneeAbove(hip, ankle, pole, kneeFloor);
		glm::vec3 toAnkle = ankle - hip;
		float reach = glm::clamp(glm::length(toAnkle), std::fabs(thigh - shin) + 0.01f, (thigh + shin) * m_LegCap);
		if (glm::length(toAnkle) < 1e-4f)
			return;
		glm::vec3 down = glm::normalize(toAnkle);
		glm::vec3 forward = pole - down * glm::dot(pole, down);
		if (glm::length(forward) < 1e-4f)
			return;
		forward = glm::normalize(forward);

		float cosHip = (thigh * thigh + reach * reach - shin * shin) / (2.0f * thigh * reach);
		float alpha = std::acos(glm::clamp(cosHip, -1.0f, 1.0f));
		glm::vec3 knee = hip + (down * std::cos(alpha) + forward * std::sin(alpha)) * thigh;
		glm::vec3 kneeToAnkle = glm::normalize(ankle - knee);
		if (glm::length(ankle - knee) < 1e-4f)
			kneeToAnkle = down;

		// The bones' own frames: +y up the bone, +z forward, +x the knee's
		// hinge (the rig is built with both so).
		auto frame = [&](const glm::vec3& upAlong)
		{
			glm::vec3 y = glm::normalize(upAlong);
			glm::vec3 z = forward - y * glm::dot(forward, y);
			z = glm::length(z) > 1e-4f ? glm::normalize(z) : glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 x = glm::cross(y, z);
			return glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
		};
		glm::quat thighWorld = frame(hip - knee);
		const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
		m_World.GetJoint(m_HipJoint[side]).MotorTargetRotation =
			glm::normalize(glm::conjugate(pelvis.Orientation) * thighWorld * m_ThighRestLocal[side]);

		float cosKnee = (thigh * thigh + shin * shin - reach * reach) / (2.0f * thigh * shin);
		m_World.GetJoint(m_KneeJoint[side]).MotorTargetAngle =
			glm::pi<float>() - std::acos(glm::clamp(cosKnee, -1.0f, 1.0f));
		(void)kneeToAnkle;
	}

	// Where the ankle can be, reaching for `ankle`, with the knee no lower
	// than `floor`: the knee is lifted round the hip, in the leg's own plane,
	// from the bend the target asks for towards straight, until it clears;
	// the shin then points from there at the target.
	glm::vec3 KeepKneeAbove(const glm::vec3& hip, const glm::vec3& ankle, const glm::vec3& pole, float floor) const
	{
		float thigh = m_ThighLength, shin = m_ShinLength;
		glm::vec3 toAnkle = ankle - hip;
		float length = glm::length(toAnkle);
		if (length < 1e-4f)
			return ankle;
		float reach = glm::clamp(length, std::fabs(thigh - shin) + 0.01f, (thigh + shin) * m_LegCap);
		glm::vec3 down = toAnkle / length;
		glm::vec3 forward = pole - down * glm::dot(pole, down);
		if (glm::length(forward) < 1e-4f)
			return ankle;
		forward = glm::normalize(forward);
		float alpha = std::acos(glm::clamp((thigh * thigh + reach * reach - shin * shin) / (2.0f * thigh * reach), -1.0f, 1.0f));
		auto kneeAt = [&](float a) { return hip + (down * std::cos(a) + forward * std::sin(a)) * thigh; };
		if (kneeAt(alpha).y >= floor)
			return ankle;
		// From the asked-for bend towards straight, the first that clears;
		// failing that, the highest.
		float best = alpha, bestY = kneeAt(alpha).y;
		bool found = false;
		for (int i = 1; i <= 32 && !found; i++)
		{
			float a = alpha * (1.0f - (float)i / 32.0f);
			float y = kneeAt(a).y;
			if (y >= floor) { best = a; found = true; }
			else if (y > bestY) { best = a; bestY = y; }
		}
		glm::vec3 knee = kneeAt(best);
		glm::vec3 along = ankle - knee;
		float d = glm::length(along);
		return d > 1e-4f ? knee + along * (shin / d) : knee + down * shin;
	}

	// The rotation that lays a sole on the slope under `footWorld`.
	//
	// Bounded twice over. `m_SoleToSlope` is how much of the slope to follow at
	// all, and `m_MaxSoleTilt` caps the result -- the ankle is a ball joint on a
	// 35 degree cone, and a foot commanded past that is a motor pushing against
	// a limit for the whole stance, which reads as a shudder rather than as a
	// step.
	glm::quat ConformToGround(const glm::vec3& footWorld, int side) const
	{
		const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);

		float height = 0.0f;
		glm::vec3 ground = up;

		// From a metre above, so a target already at ground level still finds
		// the surface it is sitting on rather than the one below it.
		if (!GroundUnder(footWorld + up, height, ground, -1000.0f))
			return none;

		float tilt = std::acos(glm::clamp(ground.y, -1.0f, 1.0f));
		if (tilt < 1e-4f)
			return none;

		float share = m_SoleToSlope
			* glm::clamp(glm::radians(m_MaxSoleTilt) / tilt, 0.0f, 1.0f);

		return glm::slerp(none, RotationBetween(up, ground), share);
	}

	// ---------------------------------------------------------------------
	// Balance	// ---------------------------------------------------------------------
	// Balance
	// ---------------------------------------------------------------------
	//
	// Everything a balance controller needs, and nothing it does.
	//
	// The idea is standard and worth stating plainly. A standing figure is an
	// inverted pendulum. It stays up while its weight is over its feet, and
	// the honest version of "over its feet" is:
	//
	//   * the **support polygon** -- the convex hull of everywhere the feet
	//     touch the ground, flattened to the horizontal plane; and
	//   * the **capture point** -- not where the centre of mass *is*, but
	//     where it is *going*: `com + velocity * sqrt(height / g)`. That is
	//     the spot a foot would have to reach to bring the body to rest, and
	//     it is what separates leaning from falling. Standing still they are
	//     the same point; moving, the capture point leads.
	//
	// The prediction this makes is falsifiable and nothing here enforces it:
	// the figure should topple when the capture point leaves the polygon, and
	// not before.

	// Andrew's monotone chain. Small, and worth doing properly rather than
	// taking the bounding box of the contacts: a box says a figure up on one
	// toe is as stable as one flat-footed, which is exactly the case balance
	// is about.
	static std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> points)
	{
		if (points.size() < 3)
			return points;

		std::sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b)
		{
			return a.x < b.x || (a.x == b.x && a.y < b.y);
		});

		auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b)
		{
			return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
		};

		std::vector<glm::vec2> hull(points.size() * 2);
		size_t k = 0;

		for (size_t i = 0; i < points.size(); i++)
		{
			while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) k--;
			hull[k++] = points[i];
		}

		for (size_t i = points.size() - 1, t = k + 1; i > 0; i--)
		{
			while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) k--;
			hull[k++] = points[i - 1];
		}

		hull.resize(k > 0 ? k - 1 : 0);
		return hull;
	}

	// Signed distance from a point to a convex polygon: positive inside.
	static float SignedDistance(const std::vector<glm::vec2>& hull, const glm::vec2& point)
	{
		if (hull.size() < 3)
			return -1.0f;

		float smallest = std::numeric_limits<float>::max();
		bool inside = true;

		for (size_t i = 0; i < hull.size(); i++)
		{
			const glm::vec2& a = hull[i];
			const glm::vec2& b = hull[(i + 1) % hull.size()];

			glm::vec2 edge = b - a;
			float length = glm::length(edge);
			if (length < 1e-6f)
				continue;

			// Counter-clockwise winding, so the outward normal is the edge
			// turned clockwise.
			glm::vec2 outward = glm::vec2(edge.y, -edge.x) / length;
			float distance = glm::dot(point - a, outward);

			if (distance > 0.0f)
				inside = false;

			smallest = std::min(smallest, -distance);
		}

		return inside ? smallest : -std::fabs(smallest);
	}

	BalanceState ComputeBalance() const
	{
		BalanceState state;

		// Its own bodies, not every body in the world. In the demo they
		// were the only dynamic ones; in a game a crate counted towards
		// where his weight is.
		float total = 0.0f;
		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			float mass = body.GetMass();
			state.Com += body.Position * mass;
			state.ComVelocity += body.Velocity * mass;
			total += mass;
		}

		if (total <= 0.0f)
			return state;

		state.Com /= total;
		state.ComVelocity /= total;
		state.ComGround = { state.Com.x, state.Com.z };

		// Where the feet actually touch, not where they are. A foot in the air
		// contributes nothing, which is the whole point.
		std::vector<glm::vec2> contacts;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			bool footA = contact.A == m_Foot[0] || contact.A == m_Foot[1];
			bool footB = contact.B == m_Foot[0] || contact.B == m_Foot[1];
			if (!footA && !footB)
				continue;

			for (int p = 0; p < contact.PointCount; p++)
				contacts.push_back({ contact.Points[p].Position.x, contact.Points[p].Position.z });
		}

		state.Support = ConvexHull(contacts);

		for (const glm::vec2& point : state.Support)
			state.SupportCentre += point;
		if (!state.Support.empty())
			state.SupportCentre /= (float)state.Support.size();

		// sqrt(h/g) is the inverted pendulum's time constant -- the natural
		// timescale over which a lean becomes a fall. Height is measured from
		// the ground contacts to the centre of mass, so a crouching figure
		// gets a shorter one, which is correct: crouching really does buy time.
		float height = std::max(state.Com.y, 0.05f);
		float tau = std::sqrt(height / 9.81f);

		state.CapturePoint = state.ComGround
			+ glm::vec2(state.ComVelocity.x, state.ComVelocity.z) * tau;

		state.Margin = SignedDistance(state.Support, state.CapturePoint);
		state.Standing = state.Support.size() >= 3 && state.Margin > 0.0f;

		return state;
	}

	// How hard a foot is pressing on the ground, in newtons.
	//
	// The solver works in impulses -- a normal impulse is a force applied for
	// one step -- so dividing by the step recovers the force. Worth having as
	// a number rather than an intuition: the whole stepping problem turns on
	// whether a foot is carrying weight at the moment something tries to lift
	// it, and "it looks like it is standing on both" is not a measurement.
	//
	// The figure weighs about 71 kg, so 697 N is all of it and 348 N is half.
	//
	// Two things this measured, both of which had been guessed at for three
	// sessions:
	//
	//   * At the moment a step tries to lift a foot, that foot is carrying
	//     **363 N -- 52% of body weight**. Which is why no swing controller
	//     ever moved it: they were all lifting a foot with half the figure
	//     standing on it.
	//   * The load does not sit at a steady 348 N each while standing. It
	//     oscillates between 0 and over 900 N, and the two feet together
	//     briefly carry 1284 N, nearly twice the figure's weight. The standing
	//     pose is not still -- it is bouncing, and the ankle controller is
	//     driving it.
	float FootLoad(int side, float dt) const
	{
		if (dt <= 0.0f)
			return 0.0f;

		float impulse = 0.0f;
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			if (contact.A == m_Foot[side] || contact.B == m_Foot[side])
				impulse += contact.TotalNormalImpulse();
		}

		return impulse / dt;
	}

	glm::vec3 CentreOfMass() const
	{
		glm::vec3 weighted(0.0f);
		float total = 0.0f;

		for (GS::PhysicsWorld3D::BodyHandle handle : m_CharacterBodies)
		{
			const GS::RigidBody3D& body = m_World.GetBody(handle);
			float mass = body.GetMass();
			weighted += body.Position * mass;
			total += mass;
		}

		return total > 0.0f ? weighted / total : glm::vec3(0.0f);
	}

protected:
	GS::PhysicsWorld3D& m_World;
	Control m_Control;


	using Handle = GS::PhysicsWorld3D::BodyHandle;

	Handle m_Pelvis = 0, m_Torso = 0, m_Head = 0;
	Handle m_UpperLeg[2] = {}, m_LowerLeg[2] = {}, m_Foot[2] = {};
	Handle m_UpperArm[2] = {}, m_LowerArm[2] = {};
	Handle m_ShoulderJoint[2] = {}, m_ElbowJoint[2] = {};
	Handle m_SpineJoint = {};
	glm::quat m_SpineRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::quat m_ShoulderRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

	std::vector<GS::PhysicsWorld3D::JointHandle> m_Joints;
	std::vector<GS::PhysicsWorld3D::BodyHandle> m_CharacterBodies;

	GS::PhysicsWorld3D::JointHandle m_HipJoint[2] = {};
	GS::PhysicsWorld3D::JointHandle m_KneeJoint[2] = {};
	glm::vec3 m_KneeAt[2] = {};
	glm::vec3 m_AnkleAt[2] = {};

	// How far apart the feet stand. The hips are fixed at 0.10, so anything
	// wider splays the legs.
	//
	// Widening it was meant to make the two feet load less equally, so a foot
	// could be lifted. **It does the opposite**, measured over eight perturbed
	// trials each: quiet standing falls from 25.1 s at 0.10 to 20.2 s at 0.20
	// and 7.1 s at 0.30, and shove survival from 1.99 s to 1.00 s. Splayed
	// legs push outwards, and the ground reaction that answers them is another
	// thing the balance has to fight.
	//
	// Left as a slider because the refutation is worth being able to reproduce.
	float m_StanceWidth = 0.10f;

	// Stepping state.
	// Off by default, because it currently makes things *worse*: 14.8 s of
	// unpushed standing against 35.1 s without it, and marginally faster falls
	// under every shove tested. The trigger and the aiming are right -- see the
	// changelog -- and the swing is what is not. Left switchable so the failure
	// can be watched rather than described.
	bool m_SteppingEnabled = false;
	bool m_Stepping = false;
	bool m_Shifting = false;

	// Gate the lift on the swing foot's own load rather than trying to create
	// an unload. 150 N is about a fifth of body weight, and the measurement
	// that motivated it: each foot is under that figure roughly 11% of the
	// time, in windows averaging a tenth of a second.
	bool m_GateOnLoad = true;
	bool m_WaitingForTrough = false;
	float m_WaitTimer = 0.0f;
	// 150 N, swept. Foot travel as a fraction of the distance asked for:
	// **8% with no gate, 22% at 150 N**, 20% at 80 N but with eight troughs
	// missed entirely (too strict to catch), and 7% at 250 N -- which is no
	// better than not gating, because 250 N is still most of a body's weight
	// on the foot.
	float m_LoadGate = 150.0f;
	float m_MaxWait = 0.25f;
	// How far ahead to extrapolate the load when deciding to commit.
	//
	// **Zero, and that is a result.** Committing early was supposed to centre
	// the trough on the lift rather than starting it there. It cut foot travel
	// from 22% to 6%, because the load is a spike train swinging 0-900 N
	// several times a second, so its derivative is enormous and erratic and
	// the extrapolation predicts troughs that never arrive -- committing at
	// high load, which is the thing that has never worked.
	//
	// Left as a knob because the failure is instructive and cheap to see.
	float m_LeadTime = 0.0f;
	float m_PreviousSwingLoad = 0.0f;
	// The reactive step: no weight shift, a short swing, and a foot thrown at
	// the capture point. The deliberate version is still in here behind this
	// flag, because the two are genuinely different manoeuvres rather than one
	// being a tuning of the other.
	bool m_FastStep = true;
	float m_ShiftTimer = 0.0f;
	// Long enough to move the weight, short enough that the disturbance has
	// not already won. A timeout rather than a fixed duration -- the shift ends
	// early when the weight arrives.
	// 0.35 s, which is not a taste decision: shifting weight sideways is an
	// inverted-pendulum motion and its time constant is sqrt(h/g), about
	// 0.33 s for this figure -- the same constant that appears in the capture
	// point. Asking for it faster does not work. Measured: a 0.22 s shift
	// closed 1 cm of a 10 cm gap, 0.5 s closed 3 cm, 1.0 s closed 5 cm, and
	// *doubling the roll authority changed nothing*. The motion is rate
	// limited by physics, not by how hard the ankles push.
	float m_ShiftDuration = 0.35f;
	float m_ShiftTolerance = 0.04f;
	float m_ShiftRoll = 0.30f;
	int m_SwingSide = 0;
	int m_StepsTaken = 0;
	float m_StepTimer = 0.0f;
	// Short. The swing is a throw, not a stride: the foot has to be down
	// before the body has finished falling towards it.
	//
	// Swept, and 0.16 s is the best of a bad set. Absolute foot travel is
	// about 0.11-0.16 m whatever the duration -- 0.16 s manages 0.158 m,
	// 0.30 s manages 0.113 m, 0.45 s manages 0.144 m. Shortening it to 0.10 s
	// to fit inside the trough made things markedly worse, not better, so the
	// swing was never trough-limited.
	//
	// Beware the percentage: a longer swing reports a *higher* fraction of the
	// distance asked for only because it tends to trigger when the target is
	// closer. Absolute metres is the honest measure and it barely moves.
	float m_StepDuration = 0.16f;
	// How far outside the feet the capture point must get before a step is
	// worth taking. Too small and the figure steps at every sway; too large
	// and it commits too late to reach.
	float m_StepTrigger = 0.06f;
	// Consecutive fixed steps the capture point must stay outside before a
	// step is committed to. One means "step the instant it crosses".
	//
	// Eight, and there is **no trade** against stepping earlier -- which was
	// the obvious worry once the step was diagnosed as too late. Survival
	// fractions over sixteen trials, margin/persist against quiet standing and
	// two shove strengths:
	//
	//     0.00 / 1     1/16 quiet    0/16 at 20 Ns
	//     0.00 / 4     7/16          3/16
	//     0.03 / 4     9/16          7/16
	//     0.06 / 8    10/16          8/16     <- shipped
	//     0.12 / 8     9/16          8/16
	//
	// Both columns move together: triggering earlier is worse at standing
	// still *and* worse at surviving a shove. There is nothing to trade,
	// because the step has no upside to trade against -- it does not extend
	// the support polygon, so every step taken is pure cost and taking more of
	// them earlier is strictly worse.
	//
	// Eight, and this is the whole trigger fix. Measured as a survival
	// fraction over 16 perturbed trials: stepping on the first crossing costs
	// a third of the quiet standing (8/16 against 12/16), because ordinary
	// sway puts the capture point over an edge for a frame or two and a step
	// answers something that had already fixed itself. Requiring it to stay
	// out restores parity exactly (12/16). Raising the threshold to 0.12
	// instead does the same job; persistence is the cleaner of the two,
	// because it rejects brief excursions without also rejecting real ones
	// that happen to be small.
	int m_StepPersist = 8;
	int m_OutsideFor = 0;
	float m_MaxStep = 0.55f;
	// The one genuine tension in the swing, and both sides of it are measured.
	//
	// A bent knee keeps the foot off the ground -- without it the foot lands
	// four frames in and drags -- but it also folds the leg, so the same hip
	// rotation moves the foot far less. The hip reaches 4.48 rad/s, which over
	// a 0.16 s swing on a straight 0.9 m leg would carry the foot about 0.6 m;
	// folded, it delivers 0.183 m. Clearance is currently worth more than
	// reach, but not by as much as the lift sweep alone suggests.
	//
	// Large, and the reasoning that made it small was exactly backwards.
	//
	// It was 0.40 rad on the theory that a bent knee shortens the leg when it
	// needs to reach furthest. Measuring what the foot actually does killed
	// that: it lifts into the trough, then **lands again after four frames**
	// and drags for the rest of the swing. Clearing the ground is not a
	// detail, it is the thing.
	//
	// Knee bend against airborne frames (of ten) and foot travel:
	// 0.40 -> 2.3 frames, 0.083 m. 1.00 -> 6.5, 0.118 m. **1.40 -> 7.1,
	// 0.183 m.** More than double the travel for a leg that is nominally
	// shorter while swinging.
	float m_StepLift = 1.40f;
	// Where in the swing the knee is most bent, as a fraction.
	//
	// **0.5 -- symmetric -- and moving it earlier is worse, monotonically.**
	// Foot travel by peak position: 0.50 -> 0.183 m, 0.35 -> 0.161 m,
	// 0.25 -> 0.142 m, 0.15 -> 0.131 m, with the airborne fraction flat at
	// about 70% throughout.
	//
	// The idea was that bending early and straightening late would clear the
	// ground on the same schedule while leaving the leg extended for longer
	// while it reaches. It does not, and the airborne figure says why it is
	// not a clearance effect: much of the foot's forward travel comes from the
	// knee *extending*, so extension late in the swing adds to the reach and
	// extension early is spent before the hip has turned.
	//
	// Kept as a knob because it is a clean refutation of a plausible idea.
	float m_LiftPeak = 0.5f;
	float m_MinStep = 0.12f;
	float m_StepCooldown = 0.0f;
	float m_StepCooldownTime = 0.25f;
	glm::vec2 m_StepTarget{ 0.0f };

	GS::PhysicsWorld3D::JointHandle m_AnkleJoint[2] = {};
	glm::quat m_HipRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                           glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
	glm::quat m_AnkleRest[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
	                             glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

	Mode m_Mode = Mode::Controlled;
	const char* m_RagdollReason = "";
	float m_RagdollTimer = 0.0f;
	std::unordered_map<GS::PhysicsWorld3D::JointHandle, float> m_JointTorque;

	// A hit above this many newtons ends control. Body weight is about 697 N,
	// so the default is roughly three times the character's weight -- enough
	// that walking into scenery does not floor them.
	// Body weight, and that is not a coincidence: 71.2 kg is 698 N, and a
	// contact carrying less than his own weight is him standing on the floor,
	// not something hitting him. Lowered to 400 it fired on his own landing
	// and on ordinary weight-bearing -- he stood there staggering, the knee
	// oscillating between 4 and 31 degrees, and a standing jump came apart.
	float m_StaggerThreshold = 700.0f;
	float m_CatchReach = 0.50f;          // how far a protective step reaches, fore-aft
	float m_CatchReachSide = 0.26f;      // ...and across the body, where it cannot
	float m_CatchSteps = 2.5f;           // and how many of them he gets
	float m_RecoverCrouch = 0.16f;       // how far he sinks while catching himself
	float m_BodyMass = 0.0f;
	glm::vec3 m_StaggerVelocity = glm::vec3(0.0f);

	// Stumbling (see UpdateStumble). On by default: off brings back the
	// kinematic stagger, for comparing the two.
	bool m_PhysicalStumble = true;
	bool m_SteppingCatch = true;          // off: the ankles alone, for measuring
	// m/s; faster than this a hit staggers him (pushed off his line, keeping
	// his feet) rather than stumbling him physically. Standing only: from the
	// posed walk the physical stumble ran away forward and felled him with a
	// 20 N s nudge (Runtime/Locomotion.h, 2026-10-21); it was 2.0, a walk.
	float m_StumbleEntrySpeed = 0.3f;
	glm::vec2 m_StumbleCarry = glm::vec2(0.0f);
	float m_GetUpMaxGap = 0.3f;           // lowest body's centre above the ground, at most, to get up
	bool m_Bracing = true;
	float m_BraceTimer = 0.0f;
	float m_BraceTime = 0.8f;             // s the arms stay strong after a lost stumble
	float m_BraceForward = 1.0f;          // reach ahead per unit down: 1 is 45 degrees
	glm::vec3 m_BraceDirection = glm::vec3(0.0f, 0.0f, 1.0f);   // the walk's velocity, kept through the stumble
	float m_StumbleCalmWalk = 0.35f;
	float m_StumbleSpeedGain = 3.0f;      // 1/s: how hard the ankles pull a walking stumble back to the walk's speed      // m/s from the walk's velocity counts as caught
	glm::vec3 m_HitPush = glm::vec3(0.0f);
	glm::vec3 m_FootToAnkle = glm::vec3(0.0f, 0.05f, -0.03f);
	float m_StandComHeight = 1.0f;        // measured at build
	float m_StumbleTimer = 0.0f;
	int m_StumbleSteps = 0;
	int m_StumbleSwing = -1;              // the foot in the air, or -1
	int m_StumbleLastSwing = -1;
	float m_TouchMemory[2] = { 0.0f, 0.0f };   // seconds off the ground
	RestSkeleton m_Rest;
	glm::quat m_ThighRestLocal[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
	float m_FootGround[2] = { 0.0f, 0.0f };
	bool m_StumbleGathering = false;
	float m_StumbleStance = 0.22f;
	float m_StumbleCentring = 0.5f;
	float m_StumblePress = 0.03f;
	float m_StumbleSwingTimer = 0.0f;
	float m_StumbleCooldown = 0.0f;
	float m_StumbleCalm = 0.0f;
	float m_StumbleUnsupported = 0.0f;
	float m_StumbleCrouch = 0.0f;
	float m_StumbleGround = 0.0f;
	float m_StumbleMargin = 0.0f;
	float m_StumbleSupport = 0.0f;
	glm::vec3 m_StumbleForce = glm::vec3(0.0f);
	glm::vec2 m_StumbleTarget = glm::vec2(0.0f);
	glm::vec2 m_StumbleCapture = glm::vec2(0.0f);
	std::vector<glm::vec2> m_StumbleFootprint;   // for drawing
	float m_StumbleHeightGain = 100.0f;   // (rad/s)^2: a 10 rad/s spring on the hips' height
	float m_StumbleMaxSupport = 2.0f;     // body weights the virtual leg can push
	float m_StumbleFriction = 0.7f;
	float m_StumbleUprightGain = 900.0f;  // N m per radian
	float m_StumbleUprightDamping = 120.0f;
	float m_StumbleMaxTorque = 500.0f;
	float m_StumbleTrigger = 0.02f;       // m outside the footprint before stepping
	float m_StumbleStepTime = 0.25f;
	float m_StumbleOvershoot = 0.05f;
	float m_StumbleFootGap = 0.14f;
	float m_StumbleReach = 0.60f;         // from the hip, flat
	float m_StumbleLift = 0.08f;
	float m_StumbleSettle = 0.05f;
	float m_StumbleComHeight = 1.0f;
	float m_StumbleRestCrouch = 0.08f;    // knees bent while recovering, so the legs have travel to push with
	float m_StumbleMaxCrouch = 0.30f;
	float m_StumbleCrouchRate = 1.5f;
	float m_StumbleGiveUpTilt = 50.0f;
	float m_StumbleGiveUpHeight = 0.55f;
	int m_StumbleMaxSteps = 8;
	float m_StumbleMaxTime = 5.0f;
	float m_StumbleCalmMargin = 0.03f;
	float m_StumbleCalmSpeed = 0.15f;
	float m_StumbleCalmTime = 0.30f;
	float m_StaggerTimer = 0.0f;
	float m_StaggerTime = 0.9f;
	float m_StaggerRecovery = 2.2f;      // how fast the push bleeds off, per second
	float m_StaggerSettle = 0.12f;
	float m_StaggerStep = 0.22f;         // how far ahead the protective step goes
	float m_LimpStiffness = 1.5f;
	float m_LimpTorque = 8.0f;

	// Walking.
	float m_Facing = 0.0f;
	float m_WalkSpeed = 1.3f;

	// Momentum. Braking is quicker than acceleration, which is true of people
	// and stops the character feeling like it is on ice.
	float m_Speed = 0.0f;
	float m_SpeedRate = 0.0f;
	float m_TurnRate = 4.0f;          // radians a second
	float m_HeadLeadShare = 0.6f;     // the head turns ahead into a turn by this share of what is left of it
	float m_HeadLeadMax = glm::radians(45.0f);
	float m_HeadLead = 0.0f;
	float m_TurnToGo = 0.0f;          // rad still to turn
	glm::vec3 m_RootAnchor{ 0.0f };

	// Weight.
	// Tuned against a person rather than by eye. Gait studies put the hips at
	// roughly 4-5 cm of vertical travel per stride and 2-3 cm of lateral sway;
	// the first pass at these produced 6.5 and 6.0, which reads as a swagger.
	float m_BobAmount = 0.008f;      // on top of what the stride geometrically needs
	float m_FallHeight = 1.5f;       // no ground within this and he is falling
	float m_RollReach = 0.08f;
	float m_RunFootLift = 0.15f;     // swing lift at a run, at its peak, m (a runner's, measured: SwingCurve)
	float m_RunFootReach = 0.26f;    // how far ahead a running foot lands, m     // how far the rising heel is drawn back under him, m       // reach the heel-to-toe roll gives a foot, m
	float m_SwayAmount = 0.013f;
	float m_Carry = 0.0f;   // -1 fully over side 0, +1 fully over side 1
	// 0.07 rad per unit of walk speed: about 4 deg walking and 12 deg running,
	// which is roughly what a person does. It was 0.10, giving 17.6 deg at a
	// run -- too much even before it was pointing the wrong way.
	float m_LeanAmount = 0.07f;
	float m_LeanForward = 0.45f;
	float m_LeanBack = 0.20f;         // braking leans back, but not as far
	float m_LeanPelvisShare = 0.45f;  // the rest is spine, so it bends
	float m_RollAmount = 0.05f;

	// Terrain.
	//
	// StandClearance is how far the pelvis rides above whatever it is standing
	// on, taken from the pose the rig was built in rather than typed in.
	float m_StandClearance = 1.09f;
	float m_MaxStepUp = 0.35f;
	float m_StepUp = 0.0f;           // 0 flat, 1 the tallest step worth lifting for

	// Feet.
	glm::vec3 m_Planted[2] = {};      // where each foot is pinned while it carries
	glm::vec3 m_SwingFrom[2] = {};
	glm::vec3 m_HipLocal[2] = {};     // hip joint in the pelvis's own frame
	glm::vec3 m_ShoulderLocal[2] = {};
	float m_UpperArmLength = 0.35f;
	float m_ForearmLength = 0.32f;
	float m_ThighLength = 0.48f;
	float m_ShinLength = 0.42f;
	// 6 cm of clearance at mid-swing, and the leg tracks that to within a
	// millimetre -- an earlier reading of 0.24 m was the spawn transient being
	// caught by a peak taken over the whole run, not the gait. Every value
	// from 0.02 up still gets him onto the 0.30 m step, because the foot's
	// target is on top of the step either way and the lift only has to clear
	// the riser on the way there.
	float m_FootLift = 0.10f;        // swing lift walking, at its peak, m (a walker's, measured: SwingCurve)
	// Jump. Grounded -> Crouching (the windup) -> Flying (ballistic) -> back.
	enum class Air { Grounded, Crouching, Pushing, Flying };
	Air m_Air = Air::Grounded;
	bool m_WantsJump = false;
	float m_JumpHeight = 0.55f;       // apex, and it is exactly v^2/2g
	float m_JumpCrouchTime = 0.18f;   // eased: at 0.12 the descent peaked at 6 d / T^2 = 58 m/s^2
	float m_JumpCrouchDepth = 0.14f;
	float m_LandAbsorb = 0.12f;       // s: tau of the landing's give

	// The body and its gait (see Proportions, GaitPresets).
	GS::PhysiqueSettings m_Physique;
	GS::GaitSettings m_Gait;
	bool m_PhysiqueApplied = false;
	bool m_GaitApplied = false;
	Proportions m_Proportions;
	struct ScaledMember { float* Member; float Base; int Kind; };
	std::vector<ScaledMember> m_Scaled;

	// Variation (see SetSeed).
	struct Design
	{
		float WalkSpeed = 0, RunSpeed = 0, StrideLength = 0, StanceWidth = 0, ArmSwing = 0,
			ElbowBend = 0, LeanAmount = 0, BobAmount = 0, SwayAmount = 0, TurnRate = 0, RunFootLift = 0;
	};
	Design m_Design;
	bool m_DesignStored = false;
	struct StyleBase { float FootLift = 0, RollAmount = 0, Crouch = 0; };
	StyleBase m_StyleBase;
	bool m_StyleBaseStored = false;
	uint32_t m_Seed = 1;
	float m_Variation = 1.0f;         // 0 is the design exactly
	float m_Clock = 0.0f;
	float m_Posture = 0.0f;           // radians of standing lean, + forward
	float m_BreathPeriod = 4.2f;
	float m_Breath = 0.0f;
	float m_IdleWeight = 1.0f;
	float m_NextGlance = 1.0f;
	uint32_t m_GlanceCount = 0;
	float m_GlanceYaw = 0.0f, m_GlancePitch = 0.0f, m_GlanceTilt = 0.0f;
	float m_HeadYaw = 0.0f, m_HeadPitch = 0.0f, m_HeadTilt = 0.0f;
	float m_IdleRoll = 0.0f;           // radians the hips tilt with the weight shift
	GS::PhysicsWorld3D::JointHandle m_NeckJoint = {};
	glm::quat m_NeckRest = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	std::vector<std::pair<glm::vec3, glm::quat>> m_BuildPose;   // each body, relative to the feet
	float m_LandTime = 0.18f;
	bool m_JumpPending = false;       // pressed, waiting for a foot to push off
	float m_JumpWait = 0.0f;          // seconds a press has waited for a move ahead to come in reach
	float m_JumpLegSplit = 0.34f;     // fore-aft separation of the feet at full speed
	float m_JumpLegSplitReach = 0.16f;
	float m_RunSpeed = 4.0f;
	float m_RunStride = 2.4f;         // m per cycle: 3.3 steps/s at 4 m/s
	float m_RunBlend = 0.0f;
	float m_WalkDuty = 0.62f;         // fraction of its cycle a foot is down
	float m_Duty = 0.62f;
	float m_MaxFootReach = 0.32f;     // how far ahead a foot may land
	float m_ArmSwing = 0.17f;         // radians at full stride, either way: 10 degrees (it was 24, and a person's is 9)
	float m_ElbowBend = 0.84f;        // radians at the forward swing, walking; three quarters of it at the back
	float m_AnkleHeight = 0.10f;      // ankle joint above the sole

	// How far the sole follows the ground it is landing on, and the most it
	// will tilt to do it. Both swept -- see the changelog.
	float m_SoleToSlope = 1.0f;
	float m_MaxSoleTilt = 25.0f;      // degrees, inside the ankle's 35 deg cone
	// How far the hips sit below a straight-legged stand. Sets how far forward
	// a foot can be placed: with leg L and hip height v, the reach is
	// sqrt(L^2 - v^2), which is zero when the leg is straight.
	// Just enough to keep the knee off full extension, where the law of
	// cosines is a singularity and the angle is wildly sensitive to reach.
	// This was 0.09, which is not a stand at all: on a 0.48 thigh and a 0.42
	// shin it works out at 51.8 degrees of knee bend -- a half squat. The
	// stride's own dip does the rest of the work now.
	// How much of the leg's length the IK will solve for. Deliberately just
	// under 1: standing asks for slightly more reach than this, so the knee
	// rests against its extension stop instead of balancing near it.
	float m_LegCap = 0.999f;
	// Zero on purpose. Standing demands very slightly more reach than the leg
	// has, so the knee rests against its own extension stop -- which is how a
	// person stands, and is the only configuration that measured steady. Give
	// it slack instead and the knee balances near full extension where the law
	// of cosines has a gain of about 12 degrees per millimetre, and the
	// kinematic pelvis's own micro-motion drives it to thrash: at 0.006 the
	// standing knee swung from -3 to 150 degrees, through both of its limits.
	float m_Crouch = 0.0f;
	// The ankle was tried stiffer than the rest of the leg, on the theory that
	// it carries a 1 kg foot rather than the body's weight. It measured worse:
	// 40 gives 2.6 deg of tilt, 80 gives 3.2, 160 gives 8.3. A motor is a
	// velocity constraint solved once per step, so past roughly 2/dt it
	// overshoots and rings -- the same ceiling the leg stiffness sweep found.
	float m_AnkleStiffness = 40.0f;
	float m_LegStiffness = 40.0f;
	float m_LegTorqueScale = 4.0f;         // how high the foot arcs over

	// Getting up.
	// Parkour (see "Parkour" above).
	bool m_Parkour = true;
	Obstacle m_Ahead;
	glm::vec3 m_Blocked{ 0.0f };
	std::vector<TrackPoint> m_Track;
	size_t m_TrackIndex = 0;
	Move m_Move = Move::None;
	// Loose bodies counted as solid this step (RefreshStandable), and which
	// bodies are jointed to something, rebuilt when the world's counts change.
	std::vector<GS::PhysicsWorld3D::BodyHandle> m_Standable;
	std::vector<char> m_Jointed;
	size_t m_JointsSeen = 0;
	float m_MoveTime = 1.0f, m_MoveTimer = 0.0f, m_MoveExit = 0.0f, m_MoveGrace = 0.0f;
	float m_ChainFrom = 1.0f;
	glm::vec3 m_WallNormal{ 0.0f }, m_WallRunDir{ 0.0f, 0.0f, 1.0f };
	float m_WallRunSpeed = 0.0f;
	float m_WallRunFrom = 0.0f;   // how far off the wall the run began: a corner's
	bool m_SpeedVault = false;
	int m_WallRunCorners = 0;
	glm::vec3 m_HangCornerOut{ 0.0f };
	float m_HangCornerTimer = -1.0f;
	// Hanging (UpdateHang): the edge under the hands' middle, its top.
	glm::vec3 m_HangEdge{ 0.0f };
	float m_HangTop = 0.0f;
	float m_ClimbHangAt = -1.0f;      // in a climb, when the jump reaches the hang
	glm::vec3 m_HangHand[2] = {}, m_HangHandFrom[2] = {}, m_HangHandTo[2] = {};
	float m_HangHandTimer[2] = { -1.0f, -1.0f };
	float m_FloorLift = 0.0f;          // m the carried hips have given way upwards to a limb wedged on the floor (UpdateMove)
	float m_LastDt = 1.0f / 60.0f;     // the last step's length, for what is handed on to the next
	// Dodging (TryDodge).
	float m_DodgePush = 3.0f;          // m/s: the most one push changes his velocity by (mu g over a third of a second)
	float m_DodgeTime = 0.32f;         // s the push takes, standing
	float m_DodgeSettle = 0.28f;       // s from the push to standing again
	float m_DodgeCutLoss = 0.12f;      // of the run's speed a cut's plant costs
	float m_DodgeBrake = 8.5f;         // m/s^2 a brake takes off, at its peak
	float m_MoveHitThreshold = 3000.0f;   // N from something loose that puts him down mid-move
	float m_DodgeAsked = 0.0f;         // s a press still waits for a moment it can go
	glm::vec3 m_DodgeWish{ 0.0f };
	float m_DodgeCooldown = 0.0f;
	Dodge m_DodgeKind = Dodge::None;
	uint32_t m_Dodges = 0;
	float m_RollSpeed = 6.5f;          // m/s down at landing, scaled by sqrt(s), past which he rolls (about a 2.2 m drop)
	bool m_ConesStored = false;
	bool m_JointsClosing = false;     // easing the cones back as built (EaseJointsClosed)
	bool m_FreshSpawn = false;        // respawned since the last step: its contacts are not his
	// The hips heavy rather than kinematic while getting up (HipsHeavy), and
	// what they weighed before.
	bool m_HipsHeavy = false;
	float m_PelvisInverseMass = 0.0f;
	glm::mat3 m_PelvisInverseInertia = glm::mat3(1.0f);
	float m_HipConeBuilt[2] = { 0.0f, 0.0f }, m_SpineCone = 0.0f;
	float m_SpineTwistBuilt[2] = { 0.0f, 0.0f };   // lower, upper
	bool m_NaturalGetUp = true;       // the choreographed one (PlanGetUp); off, the old three-waypoint lift
	std::vector<GetUpKey> m_GetUpKeys;
	float m_GetUpTime2 = 2.5f;        // s, the choreography's length, set by PlanGetUp
	int m_GetUpLead = 1;
	uint32_t m_GetUpCount = 0;
	float m_GetUpTime = 2.0f;         // three stages now
	float m_GetUpHands = 0.38f;       // fraction spent getting up onto the hands
	float m_GetUpGather = 0.72f;      // ...and by here the feet are under him
	float m_HandForward = 0.55f;      // how far ahead of the hips the hands land
	float m_HandWidth = 0.22f;
	float m_HandHeight = 0.06f;       // the forearm's own radius off the floor
	float m_PushUpHeight = 0.38f;     // hips at the top of the push
	float m_GetUpShift = 0.20f;       // how far the weight travels onto the hands
	glm::vec3 m_HandSpot[2] = {};
	float m_KneelHeight = 0.55f;      // how high the hips get before rising
	float m_GetUpRoll = 0.6f;
	float m_GetUpFootLift = 0.18f;         // extra roll when face up
	float m_GetUpGround = 0.0f;
	bool m_GetUpSupine = false;
	float m_GetUpTimer = 0.0f;
	glm::vec3 m_GetUpFrom{ 0.0f };
	glm::vec3 m_GetUpTo{ 0.0f };
	glm::quat m_GetUpFromOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
	float m_StandHeight = 1.09f;
	float m_GaitPhase = 0.0f;
	// Bounded by what the leg can reach, not by what looks like a big step.
	// Half a stride forward with the hips 0.81 m up on a 0.90 m leg is about
	// 0.39 m at full extension; 0.45 keeps it inside that with room spare.
	// One full cycle -- two steps -- of travel. 1.2 m at 1.3 m/s is a step
	// length of 0.60 m at 2.2 steps a second, which is roughly what a person
	// does. The foot lands a quarter of this ahead of the body, the hip has to
	// drop 0.051 m for that to be reachable, and both of those are human
	// numbers too. It was 0.45, which is a 0.22 m step at seven a second.
	float m_StrideLength = 1.2f;

	BalanceState m_Balance;
	bool m_BalanceEnabled = true;
	// Swept rather than guessed. Standing time against gain, 20 s budget:
	// pitch 1.5 fell at 4.6 s whatever the roll; pitch 2.0 with roll 4.0 held
	// the full 20 s, as did pitch 3.0/roll 4.0 and pitch 4.0/roll 2.5. Roll
	// gain matters more than pitch, which is the lateral axis being the weak
	// one. These sit in the middle of the region that held rather than on its
	// edge. Too much is as bad as too little -- gain 20 fell in half a second.
	float m_AnkleGain = 3.0f;
	float m_AnkleRollGain = 4.0f;

	bool m_MotorsEnabled = true;
	float m_MotorStiffness = 14.0f;

	// Locomotion (Runtime/Locomotion.h).
	void DriveLocomotion(float dt);
	void LocoEnter();
	void LocoRelease();
	void PoseFromMotors(float dt);
	void LocoPlace(const glm::vec3* P, const glm::quat* Q, float dt, bool pelvisToo, float enterTime, bool colliding);
	std::shared_ptr<const LocoClips> m_LocoClips;
	std::shared_ptr<LocoState> m_Loco;
	std::function<void(Humanoid&)> m_LocoBaker;
	bool m_LocoPosed = false;    // his limbs kinematic, placed by the locomotion
	bool m_LocoEntering = true;  // taken back from whatever had him: start from his bodies

	// --- weapons (Runtime/Armament.h) ---
	static constexpr float kStowTime = 0.5f;
	bool ParkourOn() const;
	const ArmamentKit& Kit() const;
	const ArmamentStrike* CurrentStrike() const;
	glm::mat4 HandFrame(int hand, const glm::vec3& forearmAt, const glm::quat& forearm) const;
	glm::mat4 StowedFrame(int hand) const;
	float StowAmount() const;
	void BeginStrike(int slot, int index);
	void UpdateArmament(float dt);
	void PoseArmament(glm::vec3* P, glm::quat* Q, float dt);
	void ArmTurns(int side, const glm::vec3& shoulder, const glm::vec3& wrist, const glm::vec3& pole,
		glm::quat& upper, glm::quat& fore) const;
	glm::vec3 RestWrist(int side) const;
	// Speed, the run's flight for the legs, a whole-body spin and how low:
	// what the strike asks of the locomotion this step.
	bool StrikeSurge(float& speed) const;
	float StrikeLeap() const;
	float StrikeSpin() const;
	float StrikeDip() const;
	float GuardSpeed() const;      // of his walk, under the guard
	bool m_Armed = false;
	Loadout m_Loadout = Loadout::SwordShield;
	int m_PendingLoadout = -1;      // to change to once what he holds is put away
	bool m_WantsAttack = false;
	float m_Guard = 0.0f;           // 0 the guard down, 1 up
	float m_GuardHeld = -1.0f;      // s since the guard was raised, or -1 while it is not
	int m_StrikeSlot = -1;          // 0 the combo, 1 from the guard, 2 from a run; -1 none
	int m_StrikeIndex = 0;          // which of the combo
	float m_Strike = -1.0f;         // s into it
	float m_StrikeBase = 0.0f;      // m/s he was going when it began
	bool m_StrikeQueued = false;
	float m_ComboGrace = 0.0f;      // s left in which a press carries the combo on
	bool m_Cast = false;            // this strike's cast has gone
	uint32_t m_Swings = 0;
	std::vector<Cast> m_Casts;
	bool m_Drawn = true;            // in his hands (else carried)
	float m_Stow = -1.0f;           // s into drawing or sheathing them, or -1
	bool m_WantsSheathe = false;
	bool m_GuardWas = false;        // the guard held last step: a press of it draws them
	float m_LegYaw = 0.0f;          // the way his legs face: his facing, but under a guard turned to where he goes
	float m_LegRate = 0.0f;
	float m_Wrist[2] = { 0.75f, 0.75f };   // each blade's angle off its forearm's fold, radians
	glm::vec3 m_BladeLocal[2] = { { 0.0f, -0.68f, 0.73f }, { 0.0f, -0.68f, 0.73f } };   // each blade's way in its forearm's frame
	float m_Recoil = 0.0f, m_RecoilRate = 0.0f;   // the guard knocked back by a block, m
	glm::vec3 m_Strap{ 0.0f, 0.0f, -1.0f };       // the shield's face, in the forearm's frame
};

#include "Locomotion.h"
#include "Armament.h"
