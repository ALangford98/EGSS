#pragma once

// Locomotion: walking, running, turning and stopping, the way games do it.
//
// The walk before this (Humanoid::DriveWalk) worked the body out: the hips
// carried, the feet placed step by step, the thighs, shins and arms left to
// physics between them. It moved like a person and handled like one -- a
// turn waited for the feet to step round, a run braked before it turned
// back -- and the physical limbs between two driven ends lagged into the
// ground (6 cm median, 14 at the worst tenth, and nothing tried fixed it).
// The owner, 2026-10-21: snappy, responsive, fluid -- GTA and Red Dead for
// how he moves, the player's control first.
//
// So the order is turned round. The keys decide where he is going, at once,
// on curves meant to feel good rather than to be physical: about a third of a
// second to a run, the same to face the other way, a hard plant to reverse.
// The body follows: every step each of his thirteen bodies is put where a
// recording says it goes -- a person's walk, jog and run, one stride
// each (MotionClip.h, BakeLocoCycle), blended by speed and played at the
// pace that keeps the planted foot still. Placed, not simulated: kinematic
// for as long as he is yours, so no limb can trail into the floor. The feet
// are pinned where they land and the legs solved to reach them (two-link IK),
// the soles kept above the ground, the hips lowered when a leg cannot reach.
//
// Physics has him back the moment it should: a hit, a move, a fall, a
// get-up. LocoRelease hands the bodies over moving as they were and holding
// the pose they are in; GoControlled hands them back and the pose is blended
// in from wherever they lie.
//
// Included at the end of Humanoid.h: what follows is Humanoid's own members,
// with its privates to hand, kept out of a file that was already 9,600 lines.

#include <array>
#include <cstdint>

// One moment of a stride: his hip centre and each body's turn from how he
// was built, in a frame on the ground under him, facing +z. Turns rather than
// positions, because turns blend without stretching anything -- the positions
// are built from them (Humanoid::DriveLocomotion's FK, as ClipRetarget does).
struct LocoPose
{
	static constexpr int Bodies = 13;   // in the order of Humanoid::Bodies()
	glm::vec3 Root{ 0.0f };
	glm::quat D[Bodies];
	LocoPose() { for (glm::quat& q : D) q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }
};

// One stride, looped: phase 0 is his side 1's foot landing.
struct LocoCycle
{
	std::string Name;
	float Duration = 1.0f;          // seconds, as recorded
	float Speed = 1.0f;             // m/s, as recorded, at his size
	std::vector<LocoPose> Frames;   // 120 a second
	std::vector<uint8_t> Down;      // two a frame: that foot planted

	void Sample(float phase, LocoPose& out, float down[2]) const
	{
		const int n = (int)Frames.size();
		float f = (phase - std::floor(phase)) * (float)n;
		int a = std::min((int)f, n - 1), b = (a + 1) % n;
		float w = f - (float)a;
		const LocoPose& A = Frames[(size_t)a];
		const LocoPose& B = Frames[(size_t)b];
		out.Root = glm::mix(A.Root, B.Root, w);
		for (int i = 0; i < LocoPose::Bodies; i++)
			out.D[i] = glm::normalize(glm::slerp(A.D[i], B.D[i], w));
		int nearest = w < 0.5f ? a : b;
		down[0] = Down[(size_t)nearest * 2];
		down[1] = Down[(size_t)nearest * 2 + 1];
	}
};

struct LocoClips
{
	std::vector<LocoCycle> Cycles;  // slowest first
	LocoPose Idle;                  // standing
};

struct LocoState
{
	// Where he is: the frame's origin, on the ground under him (in the air,
	// a ground that flies with him), and how he is going.
	glm::vec3 Origin{ 0.0f };
	float OriginRate = 0.0f;
	float Speed = 0.0f;
	float Travel = 0.0f;            // the yaw he is going along
	float Phase = 0.0f;
	float IdleWeight = 1.0f;
	bool Holding = false;           // stopped with both feet down: the stride waits
	float Pitch = 0.0f, PitchRate = 0.0f, Bank = 0.0f, BankRate = 0.0f, LastSpeed = 0.0f, LastTravel = 0.0f;
	float Drop = 0.0f, DropRate = 0.0f;
	float Dip = 0.0f, DipRate = 0.0f;
	float AirTimer = 0.0f, AirVelocity = 0.0f, PushDip = 0.0f;
	float JumpRun = 0.0f;           // how much of a running jump this one is
	float JumpDip = 0.0f;
	LocoPose TakeOff;               // the pose he left the ground in
	LocoPose GroundPose;            // the last pose on the ground
	float LeapPhase[2] = { -1.0f, -1.0f };   // the run's flight with each foot ahead
	int LeapLead = 0;               // which of them this leap is
	LocoPose LandFrom;              // the pose he landed in, blended out of
	float LandBlend = 0.0f;
	float LastDown[2] = { 1.0f, 1.0f };
	LocoPose AirPose;

	// Each foot: free (where the stride puts it), planted (where it landed),
	// or stepping (lifted across to where the stride wants it, standing).
	enum class FootState { Free, Planted, Stepping };
	struct Foot
	{
		FootState State = FootState::Free;
		glm::vec3 Ankle{ 0.0f };
		glm::quat Turn{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 From{ 0.0f };
		glm::quat FromTurn{ 1.0f, 0.0f, 0.0f, 0.0f };
		float T = 0.0f;
		glm::vec3 Offset{ 0.0f };
		float Peak = 0.0f;      // highest the sole has been since it was let go
		float Ground = 0.0f, GroundRate = 0.0f;   // what a foot in the air is clearing, eased
		bool Early = false;     // planted on touching down, before the stride says so
	};
	Foot Feet[2];

	// The bodies as he was taken back, blended out of over EnterTime.
	glm::vec3 FromP[LocoPose::Bodies];
	glm::quat FromQ[LocoPose::Bodies];
	float Blend = 1.0f;
	glm::vec3 Spin[LocoPose::Bodies];   // each body's last turning rate, for the hand-over
	float Touched[LocoPose::Bodies] = {};   // s since each limb last touched the scenery, in a move
	LocoClips Fallback;             // standing as built, for when there are no clips
	// The arms, following their recorded turns on springs (his own frame).
	glm::quat Arm[4];
	glm::vec3 ArmRate[4];
	bool ArmsSet = false;
	bool Backing = false;           // under a guard, going back: the stride played backwards
};

inline void Humanoid::SetLocomotionClips(std::shared_ptr<const LocoClips> clips)
{
	if (!clips)
		LocoRelease();
	m_LocoClips = std::move(clips);
	if (!m_Loco)
		m_Loco = std::make_shared<LocoState>();
	m_LocoEntering = true;
}

// To physics, moving as they were: every limb dynamic again, and each joint's
// motor holding the pose it is in now, so nothing kicks before whatever has
// him next sets its own targets. The pelvis stays kinematic, as Controlled
// has it; whoever takes him from here decides what it becomes.
inline void Humanoid::LocoRelease()
{
	if (!m_LocoPosed)
		return;
	m_LocoPosed = false;
	for (size_t i = 0; i < m_CharacterBodies.size(); i++)
	{
		GS::RigidBody3D& body = m_World.GetBody(m_CharacterBodies[i]);
		if (i > 0)
			body.Type = GS::BodyType::Dynamic;
		if (m_Loco && i < (size_t)LocoPose::Bodies)
			body.AngularVelocity = i > 0 ? m_Loco->Spin[i] : glm::vec3(0.0f);
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}
	auto holdBall = [&](GS::PhysicsWorld3D::JointHandle joint, GS::PhysicsWorld3D::BodyHandle a, GS::PhysicsWorld3D::BodyHandle b)
	{
		m_World.GetJoint(joint).MotorTargetRotation = glm::normalize(
			glm::conjugate(m_World.GetBody(a).Orientation) * m_World.GetBody(b).Orientation);
	};
	// A hinge's angle reads zero as built, positive about its axis (+x as
	// built) from A to B: the turn between the two bodies' turns-from-built.
	auto holdHinge = [&](GS::PhysicsWorld3D::JointHandle joint, int a, int b)
	{
		glm::quat Da = m_World.GetBody(m_CharacterBodies[(size_t)a]).Orientation * glm::conjugate(m_BuildPose[(size_t)a].second);
		glm::quat Db = m_World.GetBody(m_CharacterBodies[(size_t)b]).Orientation * glm::conjugate(m_BuildPose[(size_t)b].second);
		glm::quat r = glm::normalize(glm::conjugate(Da) * Db);
		if (r.w < 0.0f)
			r = -r;
		m_World.GetJoint(joint).MotorTargetAngle = 2.0f * std::atan2(r.x, r.w);
	};
	holdBall(m_SpineJoint, m_Pelvis, m_Torso);
	holdBall(m_NeckJoint, m_Torso, m_Head);
	for (int side = 0; side < 2; side++)
	{
		holdBall(m_HipJoint[side], m_Pelvis, m_UpperLeg[side]);
		holdHinge(m_KneeJoint[side], 3 + side, 5 + side);
		holdBall(m_AnkleJoint[side], m_LowerLeg[side], m_Foot[side]);
		holdBall(m_ShoulderJoint[side], m_Torso, m_UpperArm[side]);
		holdHinge(m_ElbowJoint[side], 9 + side, 11 + side);
	}
	for (int side = 0; side < 2; side++)
	{
		m_Planted[side] = AnkleWorld(side);
		m_SwingFrom[side] = m_Planted[side];
	}
}

// Taken back from whatever had him: where he is, how fast, which way, and
// his bodies as they are now, to be blended out of.
inline void Humanoid::LocoEnter()
{
	if (!m_Loco)
		m_Loco = std::make_shared<LocoState>();
	LocoState& L = *m_Loco;
	m_LocoEntering = false;
	const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
	for (int i = 0; i < LocoPose::Bodies; i++)
	{
		const GS::RigidBody3D& body = m_World.GetBody(m_CharacterBodies[(size_t)i]);
		L.FromP[i] = body.Position;
		L.FromQ[i] = body.Orientation;
		L.Spin[i] = glm::vec3(0.0f);
	}
	L.Blend = 0.0f;
	L.ArmsSet = false;
	float ground = GroundY(pelvis.Position, -1000.0f);
	L.Origin = glm::vec3(pelvis.Position.x, ground, pelvis.Position.z);
	L.OriginRate = 0.0f;
	L.Speed = m_Speed;
	L.Travel = m_Facing;
	m_LegYaw = m_Facing;
	L.Backing = false;
	L.LastSpeed = m_Speed;
	L.Pitch = L.PitchRate = L.Bank = L.BankRate = 0.0f;
	L.LastTravel = m_Facing;
	L.Drop = L.DropRate = 0.0f;
	L.Dip = L.DipRate = 0.0f;
	L.IdleWeight = m_Speed < 0.2f ? 1.0f : 0.0f;
	L.Holding = false;
	L.AirTimer = 0.0f;
	L.AirVelocity = 0.0f;
	for (LocoState::Foot& f : L.Feet)
	{
		f = LocoState::Foot{};
		f.Ground = ground;
	}
	m_Air = Air::Grounded;
	m_JumpPending = false;
}

inline void Humanoid::DriveLocomotion(float dt)
{
	if (m_LocoEntering || !m_Loco)
		LocoEnter();
	LocoState& L = *m_Loco;
	// Without the clips: standing as built, no strides -- he steps to keep up.
	if (!m_LocoClips && L.Fallback.Idle.Root.y <= 0.0f)
		L.Fallback.Idle.Root = glm::vec3(0.0f, 0.5f * (m_Rest.Hip[0].y + m_Rest.Hip[1].y), 0.0f);
	const LocoClips& C = m_LocoClips ? *m_LocoClips : L.Fallback;
	const float s = m_Proportions.Scale;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	dt = std::max(dt, 1e-4f);
	auto wrap = [](float a)
	{
		while (a > glm::pi<float>()) a -= glm::two_pi<float>();
		while (a < -glm::pi<float>()) a += glm::two_pi<float>();
		return a;
	};
	auto ratio = [](float now, float design) { return design > 1e-6f ? now / design : 1.0f; };

	// --- what is asked for ------------------------------------------------
	// Camera-relative, as the walk had it: W goes away from the camera.
	float camYaw = glm::radians(m_Control.Yaw);
	glm::vec3 camForward(std::cos(camYaw), 0.0f, std::sin(camYaw));
	glm::vec3 camRight(-std::sin(camYaw), 0.0f, std::cos(camYaw));
	glm::vec3 wish(0.0f);
	if (m_Control.Enabled)
	{
		if (m_Control.Forward) wish += camForward;
		if (m_Control.Back) wish -= camForward;
		if (m_Control.Left) wish -= camRight;
		if (m_Control.Right) wish += camRight;
	}
	wish.y = 0.0f;
	bool wants = glm::length(wish) > 1e-4f;
	float wishYaw = wants ? std::atan2(wish.x, wish.z) : m_Facing;
	// Guarding or swinging (Armament.h), he faces his aim -- the camera's
	// way, or what is coming -- whichever way the keys take him, and goes
	// no faster than a guarded walk.
	bool guarding = Drawn() && m_Control.Enabled && m_Control.Guard;
	bool aiming = guarding || (m_Armed && m_Strike >= 0.0f);
	float aimYaw = glm::radians(m_Control.AimYaw);
	float faceYaw = aiming ? std::atan2(std::cos(aimYaw), std::sin(aimYaw)) : wishYaw;
	bool running = m_Control.Enabled && m_Control.Run && !guarding;
	bool airborne = m_Air == Air::Flying;

	// --- facing: at once, near enough -------------------------------------
	// Fast enough that a turn is the key, not a negotiation: a half turn in
	// 0.29 s standing, 0.45 at a run. In the air he can still turn, slower.
	float turnScale = ratio(m_TurnRate, m_Design.TurnRate);
	float turnRate = (airborne ? 5.0f : glm::mix(11.0f, 7.0f, glm::clamp(L.Speed / std::max(m_RunSpeed, 0.1f), 0.0f, 1.0f))) * turnScale;
	float facingError = wrap(faceYaw - m_Facing);
	float turnStep = wants || aiming ? glm::clamp(facingError, -turnRate * dt, turnRate * dt) : 0.0f;
	m_Facing = wrap(m_Facing + turnStep);
	float turnNow = turnStep / dt;   // the legs', once they are worked out
	m_TurnToGo = wants || aiming ? facingError : 0.0f;

	// --- speed and the way he is going ------------------------------------
	// Up to a run in a third of a second, down from one in about the same.
	// Asked to go back the way he came at speed, he plants and pivots: the
	// speed goes hard (a fifth of a second from a run) while he swings round,
	// and then it builds again the new way. From a standstill he simply goes
	// where he is asked.
	float top = running ? m_RunSpeed : guarding ? GuardSpeed() * m_WalkSpeed : m_WalkSpeed;
	float travelError = wrap(wishYaw - L.Travel);
	// Braked nearly to nothing before the way he goes flips: flipped at
	// 0.6 m/s, the hips' velocity reversed in one step (72 m/s^2).
	bool pivot = wants && L.Speed > 0.15f && std::fabs(travelError) > glm::radians(110.0f);
	float target = 0.0f, rise = 11.0f * std::sqrt(s), fall = 13.0f * std::sqrt(s);
	if (airborne)
	{
		// A little say in the air, no more.
		rise = fall = 3.0f;
		target = wants ? std::max(L.Speed, top * 0.6f) : L.Speed;
		if (wants)
			L.Travel = wrap(L.Travel + glm::clamp(travelError, -2.5f * dt, 2.5f * dt));
	}
	else if (pivot)
	{
		fall = 20.0f * std::sqrt(s);
		target = 0.0f;
	}
	else if (wants)
	{
		if (L.Speed < 0.15f)
			L.Travel = wishYaw;
		else
			L.Travel = wrap(L.Travel + glm::clamp(travelError, -turnRate * 1.1f * dt, turnRate * 1.1f * dt));
		// A sharp turn takes a little off, as cornering does.
		target = top * glm::mix(1.0f, 0.75f, glm::clamp(std::fabs(travelError) / glm::radians(110.0f), 0.0f, 1.0f));
	}
	if (L.Speed < target)
		L.Speed = std::min(target, L.Speed + rise * dt);
	else
		L.Speed = std::max(target, L.Speed - fall * dt);
	// A lunging strike (Armament.h) drives him itself: straight along his
	// facing, at what he was going plus a surge over its cut -- 1.2 m from
	// the sword and shield's guard, 2.4 m from a run.
	bool surging = false;
	if (!airborne)
	{
		float driven = L.Speed;
		if (StrikeSurge(driven))
		{
			surging = true;
			L.Travel = m_Facing;
			L.Speed = driven;
		}
	}
	m_Speed = L.Speed;
	m_SpeedRate = 0.0f;

	// --- the legs under a guard ---------------------------------------------
	// Guarding, his chest stays on his aim whichever way the keys take him,
	// and the strides were all recorded going forwards. So the legs turn
	// towards the way he goes, up to 60 degrees off his chest -- the rest is
	// a twist at the waist, put back over the stride in PoseArmament -- and
	// going back he backpedals: the legs facing the aim, the stride played
	// backwards (a planted foot then moves forward under him, as a step back
	// does). Going sideways either will do, and he keeps whichever is nearer
	// where his legs already are; stopped, they stay as they stand. Not
	// guarding, the legs are his facing, step for step.
	//
	// The legs swing on a spring, not at the turning rate: at once at 11
	// rad/s, starting and stopping a strafe swung them 60-120 degrees about
	// the planted foot with no easing, and the hips jolted (229 m/s^2).
	const float legLimit = glm::radians(60.0f);
	float legTarget = m_Facing;
	if (guarding && !airborne)
	{
		float stood = wrap(m_LegYaw - m_Facing);
		legTarget = wrap(m_Facing + glm::clamp(stood, -legLimit, legLimit));
		if (L.Speed > 0.1f)
		{
			float rel = wrap(L.Travel - m_Facing);
			float back = wrap(L.Travel + glm::pi<float>() - m_Facing);
			float forwardLegs = glm::clamp(rel, -legLimit, legLimit), backLegs = glm::clamp(back, -legLimit, legLimit);
			if (std::fabs(rel) < glm::radians(60.0f))
				L.Backing = false;
			else if (std::fabs(rel) > glm::radians(120.0f))
				L.Backing = true;
			else if (L.Speed < 0.3f)
				L.Backing = std::fabs(backLegs - stood) < std::fabs(forwardLegs - stood);
			legTarget = wrap(m_Facing + (L.Backing ? backLegs : forwardLegs));
		}
	}
	else if (!guarding)
		L.Backing = false;
	float legStep = 0.0f;
	if (!guarding && std::fabs(wrap(m_LegYaw - m_Facing)) < 1e-3f)
	{
		legStep = wrap(m_Facing - m_LegYaw);
		m_LegYaw = m_Facing;
		m_LegRate = 0.0f;
	}
	else
	{
		float before = m_LegYaw;
		float unwrapped = m_LegYaw;
		Spring(unwrapped, m_LegRate, m_LegYaw + wrap(legTarget - m_LegYaw), 14.0f, dt);
		m_LegRate = glm::clamp(m_LegRate, -turnRate, turnRate);
		m_LegYaw = wrap(unwrapped);
		legStep = wrap(m_LegYaw - before);
	}
	turnNow = legStep / dt;

	// A pivot turns about the planted foot, not about his middle: turned
	// about the middle, half a turn put that foot under the other hip and
	// the legs crossed in an X. Slow, or braking to turn back, with a foot
	// down -- the inside one of the turn if both are -- he goes round it,
	// and the other swings wide.
	if (!airborne && std::fabs(legStep) > 1e-5f && (pivot || L.Speed < 0.8f * s))
	{
		// Turning towards +x (his left, side 1) is a rising yaw.
		int inside = legStep > 0.0f ? 1 : 0;
		int around = L.Feet[inside].State == LocoState::FootState::Planted ? inside
			: L.Feet[1 - inside].State == LocoState::FootState::Planted ? 1 - inside : -1;
		if (around >= 0)
		{
			glm::vec3 pivotAt = L.Feet[around].Ankle;
			glm::vec3 arm(L.Origin.x - pivotAt.x, 0.0f, L.Origin.z - pivotAt.z);
			arm = glm::angleAxis(legStep, up) * arm;
			L.Origin.x = pivotAt.x + arm.x;
			L.Origin.z = pivotAt.z + arm.z;
		}
	}
	// What the moves ask "running?" of: the wish, and being at a run.
	float runAmount = glm::clamp((L.Speed - m_WalkSpeed) / std::max(m_RunSpeed - m_WalkSpeed, 0.1f) * 1.5f, 0.0f, 1.0f);
	m_RunBlend = running ? runAmount : std::min(m_RunBlend, runAmount);

	// --- the jump, and the moves ------------------------------------------
	// A press with a move just ahead waits for it (up to 0.6 s), as the walk's
	// did: pressed as the hint came up, it used to go off as a hop in front of
	// the wall.
	if (m_WantsJump && m_Air == Air::Grounded)
		m_JumpPending = true;
	glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
	if (!airborne)
	{
		if (ParkourStep(forward))
			return;
		// It slows him short of a wall he is not taking.
		L.Speed = std::min(L.Speed, m_Speed);
	}
	m_WantsJump = false;
	if (!m_JumpPending)
		m_JumpWait = 0.0f;
	bool waiting = m_JumpPending && ParkourOn() && ParkourHint()[0] && m_JumpWait < 0.6f;
	if (waiting)
		m_JumpWait += dt;

	// --- a hit while moving: the kinematic stagger (StumblesPhysically) -----
	if (glm::length(m_StaggerVelocity) > 1e-4f)
	{
		if (glm::length(m_StaggerVelocity) > CatchableSpeed())
		{
			GoRagdoll("outrun");
			return;
		}
		m_StaggerVelocity *= std::max(1.0f - m_StaggerRecovery * dt, 0.0f);
		if (glm::length(m_StaggerVelocity) < m_StaggerSettle)
			m_StaggerVelocity = glm::vec3(0.0f);
	}
	if (m_StaggerTimer > 0.0f)
		m_StaggerTimer = std::max(m_StaggerTimer - dt, 0.0f);

	glm::vec3 velocity = glm::vec3(std::sin(L.Travel), 0.0f, std::cos(L.Travel)) * L.Speed + m_StaggerVelocity;
	velocity.y = 0.0f;

	// --- walls: slide along them, never into them -------------------------
	// The parkour sense above slows him for what is straight ahead; this is
	// everything else -- a wall met at an angle, a corner, a pillar -- at
	// knee, waist and chest height, ahead and either side of ahead.
	{
		float flat = glm::length(velocity);
		if (flat > 1e-4f)
		{
			glm::vec3 dir = velocity / flat;
			for (float spread : { 0.0f, -0.6f, 0.6f })
			{
				glm::vec3 ray = glm::angleAxis(spread, up) * dir;
				for (float h : { m_MaxStepUp * 1.1f / s, 0.55f, 1.0f, 1.45f })
				{
					// Felt a fifth of a second out and turned along it over
					// the approach: taken off all at once at the wall, the
					// hips swung onto it in one step (136 m/s^2).
					glm::vec3 from(L.Origin.x, L.Origin.y + h * s, L.Origin.z);
					const float body = 0.30f * s;
					float reach = body + flat * 0.2f + 0.05f;
					GS::PhysicsWorld3D::RayHit hit = m_World.Raycast(from, ray, reach, m_CharacterBodies);
					if (!hit.Hit || !Solid(hit.Body) || std::fabs(hit.Normal.y) > 0.6f)
						continue;
					// A stair, not a wall, if a step higher the same ray goes
					// further: stairs recede as they rise and a wall does not.
					// Felt from a run's reach, the third riser up read as a
					// wall and stopped him at the foot of the stairs (3.65 m/s
					// to 0.93 in a step). (Asking the ground on top of what was
					// hit instead let him through a tall wall: from inside it,
					// the ground below is the floor.)
					{
						GS::PhysicsWorld3D::RayHit higher = m_World.Raycast(from + up * (0.35f * s), ray, reach + 0.6f * s, m_CharacterBodies);
						if (!higher.Hit || higher.Distance > hit.Distance + 0.2f * s)
							continue;
					}
					glm::vec3 n(hit.Normal.x, 0.0f, hit.Normal.z);
					if (glm::length(n) < 1e-4f)
						continue;
					n = glm::normalize(n);
					float along = hit.Distance * std::fabs(glm::dot(ray, n));   // straight out from the wall
					float near = 1.0f - glm::clamp((along - body) / std::max(reach - body, 1e-3f), 0.0f, 1.0f);
					float into = glm::dot(velocity, n);
					if (into < 0.0f)
						velocity -= n * into * glm::smoothstep(0.0f, 1.0f, near);
					// Inside his body's depth of it, eased back out.
					if (spread == 0.0f && along < body - 0.02f)
						L.Origin += n * std::min(body - along, 1.0f * dt);
				}
			}
		}
	}
	L.Origin.x += velocity.x * dt;
	L.Origin.z += velocity.z * dt;

	// --- the ground, and the air ------------------------------------------
	float groundHere;
	glm::vec3 groundNormal;
	// Asked from a step above him: anything taller is not stepped onto but
	// met as a wall (above) or, with parkour, climbed.
	bool found = GroundUnder(glm::vec3(L.Origin.x, L.Origin.y + m_MaxStepUp * 1.05f, L.Origin.z), groundHere, groundNormal, -1000.0f);
	if (!found)
		groundHere = -1000.0f;

	float crouchDip = 0.0f;
	if (m_JumpPending && m_Air == Air::Grounded && !waiting)
	{
		m_JumpPending = false;
		m_Air = Air::Crouching;
		L.AirTimer = 0.0f;
	}
	// Down 7 cm in 0.08 s, then up out of it under the constant push that
	// leaves the ground at the jump's speed, a = v^2 / 2d (about 0.04 s):
	// an eighth of a second from the press to the air, and the hips never
	// change speed in one step -- the take-off at once was a 250 m/s^2 pop.
	// Standing, a real dip -- 13 cm over 0.11 s -- since a jump from nothing
	// has to load the legs; running, a runner jumps out of the stride, 8 cm
	// over 0.05 s (at 5 cm the push to take-off speed was 109 m/s^2, v^2/2d). The owner: snappy, but the legs barely moved (it was 7 cm
	// over 0.08 s whatever the speed). An eighth to a sixth of a second from
	// the press to the air.
	if (m_Air == Air::Crouching && L.AirTimer == 0.0f)
	{
		L.JumpRun = glm::clamp((L.Speed - m_WalkSpeed) / std::max(m_RunSpeed - m_WalkSpeed, 0.1f), 0.0f, 1.0f);
		L.JumpDip = glm::mix(0.13f, 0.08f, L.JumpRun) * s;
	}
	const float jumpDip = std::max(L.JumpDip, 0.02f * s);
	const float crouchTime = glm::mix(0.11f, 0.05f, L.JumpRun);
	if (m_Air == Air::Crouching)
	{
		L.AirTimer += dt;
		crouchDip = jumpDip * glm::smoothstep(0.0f, crouchTime, L.AirTimer);
		if (L.AirTimer >= crouchTime)
		{
			m_Air = Air::Pushing;
			L.AirVelocity = 0.0f;
			L.PushDip = jumpDip;
		}
	}
	else if (m_Air == Air::Pushing)
	{
		float takeoff = std::sqrt(2.0f * 9.81f * m_JumpHeight);
		L.AirVelocity += takeoff * takeoff / (2.0f * jumpDip) * dt;
		L.PushDip -= L.AirVelocity * dt;
		crouchDip = std::max(L.PushDip, 0.0f);
		if (L.PushDip <= 0.0f)
		{
			m_Air = Air::Flying;
			L.Origin.y -= L.PushDip;   // what this step rose past standing
			L.AirVelocity = takeoff;
			L.AirTimer = 0.0f;
		}
	}
	else if (m_Air == Air::Flying)
	{
		L.AirTimer += dt;
		L.AirVelocity -= 9.81f * dt;
		L.Origin.y += L.AirVelocity * dt;
		if (L.Origin.y <= groundHere && L.AirVelocity < 0.0f)
		{
			float impact = -L.AirVelocity;
			L.Origin.y = groundHere;
			L.OriginRate = 0.0f;
			m_Air = Air::Grounded;
			// Out of a leap the stride carries on from the leap's own moment
			// of the run -- the leading foot coming down -- and whatever the
			// air pose was is blended out over 0.15 s. Snapped back to the
			// stride he took off from, a body moved at 42 m/s for a step.
			if (L.JumpRun > 0.05f)
				L.Phase = L.LeapPhase[L.LeapLead];
			L.LandFrom = L.AirPose;
			L.LandBlend = 1.0f;
			// Landing goes into the legs: the hips carry on down at 75% of
			// the speed they landed with and spring back -- 9 cm deep from a
			// jump. At 35%, a hard landing stopped the hips at 240 m/s^2, at
			// 60% a running jump's at 148.
			L.DipRate = -0.75f * impact;
			// Higher than about four metres is not landed.
			if (impact > 9.0f)
			{
				GoRagdoll("fell");
				return;
			}
		}
	}
	else
	{
		// Off an edge. Run off it, and it is a leap: in the air, parkour's
		// drop and roll for a big one. Walk off it -- or drift off it standing
		// -- and he has stepped onto nothing: he falls, physics has him, the
		// arms reach out for the ground, and he gets up where he lands. The
		// owner, 2026-10-21: walking off a pillar he went down neatly on his
		// feet, where stepping onto nothing should make you fall. (Walking off
		// something tall used to lower him into a hang; that is for parkour
		// to be asked for, not for a step into nothing.)
		float drop = L.Origin.y - groundHere;
		if (drop > 0.5f * s)
		{
			bool runningOff = (running || surging) && L.Speed > 1.3f * m_WalkSpeed;
			if (!runningOff)
			{
				GoRagdoll("stepped off", true);
				return;
			}
			if (drop > m_FallHeight && PlanDrop())
				return;
			m_Air = Air::Flying;
			L.AirVelocity = std::min(L.OriginRate, 0.0f);
			L.AirTimer = 0.0f;
			L.JumpRun = 0.0f;
		}
		else
		{
			// Up a step at once, down no faster than falling. A riser is a
			// step function; the spring is what keeps the hips off it.
			float rateBefore = L.OriginRate;
			Spring(L.Origin.y, L.OriginRate, groundHere, 22.0f, dt);
			if (L.OriginRate < rateBefore - 9.81f * dt)
			{
				L.Origin.y -= (L.OriginRate - (rateBefore - 9.81f * dt)) * dt;
				L.OriginRate = rateBefore - 9.81f * dt;
			}
		}
	}
	airborne = m_Air == Air::Flying;
	Spring(L.Dip, L.DipRate, 0.0f, 10.0f, dt);

	// --- the stride ---------------------------------------------------------
	// Blended by speed between standing and the recorded strides, every one
	// at the same phase -- they all start with the same foot landing -- each
	// played at the pace that matches its stride to his speed, so a foot
	// that is down stays where it is.
	const float moving = glm::length(velocity);
	float w[8] = {};
	const int cycles = std::min((int)C.Cycles.size(), 8);
	float idleTarget = 1.0f;
	if (cycles > 0)
	{
		const float slowest = C.Cycles[0].Speed;
		if (moving <= slowest)
		{
			float x = glm::smoothstep(0.0f, 0.55f * slowest, moving);
			idleTarget = 1.0f - x;
			w[0] = 1.0f;
		}
		else
		{
			idleTarget = 0.0f;
			int i = 0;
			while (i + 1 < cycles && moving > C.Cycles[(size_t)i + 1].Speed)
				i++;
			if (i + 1 < cycles)
			{
				float x = (moving - C.Cycles[(size_t)i].Speed) / std::max(C.Cycles[(size_t)i + 1].Speed - C.Cycles[(size_t)i].Speed, 1e-3f);
				w[i] = 1.0f - x;
				w[i + 1] = x;
			}
			else
				w[i] = 1.0f;
		}
	}
	if (!airborne)
		L.IdleWeight += (idleTarget - L.IdleWeight) * (1.0f - std::exp(-dt / 0.08f));

	float rate = 0.0f;
	for (int i = 0; i < cycles; i++)
		rate += w[i] * glm::clamp(moving / std::max(C.Cycles[(size_t)i].Speed, 0.1f), 0.55f, 1.6f) / C.Cycles[(size_t)i].Duration;
	// Stopping, the stride runs on to the next moment both feet are down and
	// waits there, so he never stands on one leg.
	if (!wants && L.IdleWeight > 0.5f)
	{
		if (L.LastDown[0] > 0.5f && L.LastDown[1] > 0.5f)
			L.Holding = true;
		if (!L.Holding && cycles > 0)
			rate = std::max(rate, 0.9f / C.Cycles[0].Duration);
	}
	else
		L.Holding = false;
	if (L.Holding || airborne)
		rate = 0.0f;
	if (L.Backing)
		rate = -rate;
	L.Phase += rate * dt;
	L.Phase -= std::floor(L.Phase);

	LocoPose pose;
	float down[2] = { 0.0f, 0.0f };
	float strideDown[2] = { 0.0f, 0.0f }, strideSum = 0.0f;
	{
		float strideWeight = 1.0f - L.IdleWeight;
		float sum = 0.0f;
		bool first = true;
		auto add = [&](const LocoPose& p, const float* d, float weight)
		{
			if (weight <= 1e-4f)
				return;
			sum += weight;
			float k = weight / sum;
			pose.Root = first ? p.Root : glm::mix(pose.Root, p.Root, k);
			for (int i = 0; i < LocoPose::Bodies; i++)
			{
				glm::quat q = glm::dot(pose.D[i], p.D[i]) < 0.0f ? -p.D[i] : p.D[i];
				pose.D[i] = first ? q : glm::normalize(glm::slerp(pose.D[i], q, k));
			}
			down[0] += d[0] * weight;
			down[1] += d[1] * weight;
			first = false;
		};
		for (int i = 0; i < cycles; i++)
		{
			if (w[i] * strideWeight <= 1e-4f)
				continue;
			LocoPose p;
			float d[2];
			C.Cycles[(size_t)i].Sample(L.Phase, p, d);
			add(p, d, w[i] * strideWeight);
			strideDown[0] += d[0] * w[i];
			strideDown[1] += d[1] * w[i];
			strideSum += w[i];
		}
		const float standing[2] = { 1.0f, 1.0f };
		add(C.Idle, standing, L.IdleWeight);
		if (sum > 0.0f)
		{
			down[0] /= sum;
			down[1] /= sum;
		}
	}
	// The strides' own feet, without standing's (always down) mixed in: what
	// stopping waits on.
	L.LastDown[0] = strideSum > 0.0f ? strideDown[0] / strideSum : 1.0f;
	L.LastDown[1] = strideSum > 0.0f ? strideDown[1] / strideSum : 1.0f;
	// In the air. Off a run, a leap: the recorded run's own moment of flight
	// with its legs split furthest, the foot that was coming through leading,
	// blended in over 0.12 s from the pose he left the ground in. Standing,
	// that pose held with the knees tucked up (below, at the feet).
	if (L.LeapPhase[0] < 0.0f && cycles > 0)
	{
		// Once: where in the fastest stride each foot is furthest ahead with
		// both off the ground.
		const LocoCycle& run = C.Cycles[(size_t)cycles - 1];
		float best[2] = { 0.0f, 0.0f };
		const glm::vec3 centre = 0.5f * (m_Rest.Hip[0] + m_Rest.Hip[1]);
		for (size_t k = 0; k < run.Frames.size(); k++)
		{
			if (run.Down[k * 2] || run.Down[k * 2 + 1])
				continue;
			const LocoPose& f = run.Frames[k];
			float z[2];
			for (int side = 0; side < 2; side++)
				z[side] = (f.D[0] * (m_Rest.Hip[side] - centre) + f.D[3 + side] * (m_Rest.Knee[side] - m_Rest.Hip[side])
					+ f.D[5 + side] * (m_Rest.Ankle[side] - m_Rest.Knee[side])).z;
			for (int lead = 0; lead < 2; lead++)
				if (z[lead] - z[1 - lead] > best[lead])
				{
					best[lead] = z[lead] - z[1 - lead];
					L.LeapPhase[lead] = (float)k / (float)run.Frames.size();
				}
		}
		for (float& p : L.LeapPhase)
			if (p < 0.0f)
				p = 0.0f;
	}
	if (airborne)
	{
		if (L.AirTimer <= dt)
			L.TakeOff = L.GroundPose;
		pose = L.TakeOff;
		if (L.JumpRun > 0.05f && cycles > 0)
		{
			// The leap that comes next in the stride, so the legs carry on.
			auto ahead = [&](float p) { float d = p - L.Phase; return d < 0.0f ? d + 1.0f : d; };
			int lead = L.AirTimer <= dt ? (ahead(L.LeapPhase[0]) < ahead(L.LeapPhase[1]) ? 0 : 1) : L.LeapLead;
			L.LeapLead = lead;
			LocoPose leap;
			float d[2];
			C.Cycles[(size_t)cycles - 1].Sample(L.LeapPhase[lead], leap, d);
			float w = glm::smoothstep(0.0f, 0.12f, L.AirTimer) * L.JumpRun;
			pose.Root = glm::mix(pose.Root, glm::vec3(pose.Root.x, leap.Root.y, pose.Root.z), w);
			for (int i = 0; i < LocoPose::Bodies; i++)
			{
				glm::quat q = glm::dot(pose.D[i], leap.D[i]) < 0.0f ? -leap.D[i] : leap.D[i];
				pose.D[i] = glm::normalize(glm::slerp(pose.D[i], q, w));
			}
		}
		down[0] = down[1] = 0.0f;
		L.AirPose = pose;
	}
	else
	{
		if (L.LandBlend > 0.0f)
		{
			float w = glm::smoothstep(0.0f, 1.0f, L.LandBlend);
			pose.Root = glm::mix(pose.Root, L.LandFrom.Root, w);
			for (int i = 0; i < LocoPose::Bodies; i++)
			{
				glm::quat q = glm::dot(pose.D[i], L.LandFrom.D[i]) < 0.0f ? -L.LandFrom.D[i] : L.LandFrom.D[i];
				pose.D[i] = glm::normalize(glm::slerp(pose.D[i], q, w));
			}
			L.LandBlend = std::max(L.LandBlend - dt / 0.15f, 0.0f);
		}
		L.AirPose = pose;
		L.GroundPose = pose;
	}

	// A lunge: the run's own moment of flight with the right foot leading
	// (the sword's side), blended in over the wind-up and out over the
	// recovery, the hips low -- a leaping thrust that skims the ground. The
	// feet are free of the stride while it lasts; the stride comes back at
	// that same moment of itself, so the legs carry on from where they are.
	float lungeW = 0.0f;
	if (!airborne && cycles > 0 && L.LeapPhase[0] >= 0.0f && StrikeLeap() > 0.0f)
	{
		lungeW = StrikeLeap();
		LocoPose leap;
		float d[2];
		C.Cycles[(size_t)cycles - 1].Sample(L.LeapPhase[0], leap, d);
		pose.Root = glm::mix(pose.Root, glm::vec3(pose.Root.x, leap.Root.y, pose.Root.z), lungeW);
		for (int i = 0; i < LocoPose::Bodies; i++)
		{
			glm::quat q = glm::dot(pose.D[i], leap.D[i]) < 0.0f ? -leap.D[i] : leap.D[i];
			pose.D[i] = glm::normalize(glm::slerp(pose.D[i], q, lungeW));
		}
		if (lungeW > 0.5f)
		{
			down[0] = down[1] = 0.0f;
			L.Phase = L.LeapPhase[0];
		}
	}

	// How soon he lands: the legs reach for the ground in the last 0.15 s.
	float landing = 1.0f;
	if (airborne && L.AirVelocity < 0.0f)
	{
		float height = std::max(L.Origin.y - groundHere, 0.0f);
		float v = -L.AirVelocity;
		float t = (-v + std::sqrt(v * v + 2.0f * 9.81f * height)) / 9.81f;
		landing = glm::clamp(t / 0.15f, 0.0f, 1.0f);
	}

	// --- his own way of moving, on top -------------------------------------
	// The walking styles' numbers that still mean something here: how much
	// the arms swing (towards standing's), how far forward he carries
	// himself, where his head is, how low he goes, how wide his feet are.
	float armSwing = ratio(m_ArmSwing, m_Design.ArmSwing);
	if (std::fabs(armSwing - 1.0f) > 1e-3f)
		for (int i = 9; i < 13; i++)
		{
			glm::quat q = glm::dot(C.Idle.D[i], pose.D[i]) < 0.0f ? -pose.D[i] : pose.D[i];
			pose.D[i] = glm::normalize(glm::slerp(C.Idle.D[i], q, glm::clamp(armSwing, 0.0f, 2.0f)));
		}
	// Breath, glances, the weight shifting: the standing life the walk had.
	Idle(dt, 1.0f - L.IdleWeight, glm::vec3(std::cos(m_Facing), 0.0f, -std::sin(m_Facing)));
	float posture = m_Posture + 0.08f * (ratio(m_LeanAmount, m_Design.LeanAmount) - 1.0f) - m_Breath;
	float headPitch = m_GaitApplied ? m_HeadPitch + m_Gait.HeadPitch : m_HeadPitch;
	{
		glm::quat chest = glm::angleAxis(posture, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(0.7f * m_IdleRoll, glm::vec3(0.0f, 0.0f, 1.0f));
		for (int i : { 1, 2, 9, 10, 11, 12 })
			pose.D[i] = glm::normalize(chest * pose.D[i]);
		// The head looks where he is going before the body gets there.
		glm::quat look = glm::angleAxis(m_HeadYaw + glm::clamp(facingError * 0.4f, -0.6f, 0.6f), up)
			* glm::angleAxis(headPitch, glm::vec3(1.0f, 0.0f, 0.0f))
			* glm::angleAxis(m_HeadTilt, glm::vec3(0.0f, 0.0f, 1.0f));
		pose.D[2] = glm::normalize(look * pose.D[2]);
	}

	// The arms in a jump: back as he dips, thrown forward and up as he
	// pushes, easing out over the flight (the springs below smooth it).
	{
		float swing = 0.0f;
		if (m_Air == Air::Crouching)
			swing = 0.6f;
		else if (m_Air == Air::Pushing)
			swing = -1.1f;
		else if (airborne)
			swing = glm::mix(-1.1f, -0.35f, glm::smoothstep(0.0f, 0.3f, L.AirTimer)) * landing;
		swing *= glm::mix(1.0f, 0.4f, L.JumpRun);
		if (std::fabs(swing) > 1e-4f)
		{
			glm::quat arms = glm::angleAxis(swing, glm::vec3(1.0f, 0.0f, 0.0f));
			for (int i = 9; i < 13; i++)
				pose.D[i] = glm::normalize(arms * pose.D[i]);
		}
	}

	// Follow-through in the arms. Copied exactly from the recording, upper
	// arm and forearm moved as one stiff piece, frame for frame, and the
	// owner read the walk's arms as robotic. Each follows its recorded turn
	// on a spring a little short of critically damped -- the upper arm at
	// 14 rad/s, the forearm looser at 10, so it trails the upper arm and
	// settles a touch past it -- in his own frame, so turning round does
	// not drag them.
	{
		const float omega[2] = { 14.0f, 10.0f }, damping = 0.7f;
		for (int k = 0; k < 4; k++)
		{
			int body = 9 + k;
			glm::quat target = pose.D[body];
			if (!L.ArmsSet)
			{
				L.Arm[k] = target;
				L.ArmRate[k] = glm::vec3(0.0f);
				continue;
			}
			glm::quat error = glm::normalize(target * glm::conjugate(L.Arm[k]));
			if (error.w < 0.0f)
				error = -error;
			glm::vec3 axis(error.x, error.y, error.z);
			float length = glm::length(axis);
			glm::vec3 toGo = length > 1e-6f ? axis / length * (2.0f * std::atan2(length, error.w)) : glm::vec3(0.0f);
			float w = omega[k / 2];
			L.ArmRate[k] += (w * w * toGo - 2.0f * damping * w * L.ArmRate[k]) * dt;
			float turnBy = glm::length(L.ArmRate[k]) * dt;
			if (turnBy > 1e-7f)
				L.Arm[k] = glm::normalize(glm::angleAxis(turnBy, L.ArmRate[k] / glm::length(L.ArmRate[k])) * L.Arm[k]);
			pose.D[body] = L.Arm[k];
		}
		L.ArmsSet = true;
	}
	// Guarding, a little lower and wider; lunging, low. Only the hips go
	// down (with the hip drop, below) -- the knees bend for it: taken off the
	// whole pose, the feet went down with it and the swing foot scraped the
	// floor (feet within a centimetre of it at 2 m/s p95, against 0.4 walking).
	const float guardStance = m_Armed ? glm::smoothstep(0.0f, 1.0f, m_Guard) : 0.0f;
	const float lower = (0.05f * guardStance + StrikeDip()) * s;
	pose.Root.y -= m_Crouch + crouchDip - L.Dip;

	// --- how much of the person's hips -------------------------------------
	// The recording's pelvis rolls and swings round with every step and the
	// hips sway over the foot that carries them, and on him -- the default
	// body is masculine -- the owner read it as feminine (2026-10-21). So it
	// follows his body: at Femininity 0 about half the recorded roll, swing
	// and sway, at 1 all of it, the walking style's Hip Roll and Sway on top.
	// The pitch (the tilt forward and back) is left as recorded.
	const float femininity = glm::clamp(m_Physique.Femininity, 0.0f, 1.0f);
	const float hipRoll = glm::mix(0.45f, 1.0f, femininity) * (m_GaitApplied ? m_Gait.HipRoll : 1.0f);
	const float hipSway = glm::mix(0.5f, 1.0f, femininity) * (m_GaitApplied ? m_Gait.Sway : 1.0f);
	{
		glm::quat d = pose.D[0];
		if (d.w < 0.0f)
			d = -d;
		glm::vec3 v(d.x, d.y, d.z);
		float half = std::atan2(glm::length(v), d.w);
		glm::vec3 turnVector = glm::length(v) > 1e-6f ? glm::normalize(v) * (2.0f * half) : glm::vec3(0.0f);
		turnVector.y *= hipRoll;
		turnVector.z *= hipRoll;
		float angle = glm::length(turnVector);
		pose.D[0] = angle > 1e-6f ? glm::angleAxis(angle, turnVector / angle) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		pose.Root.x *= hipSway;
	}

	// --- leaning into it ----------------------------------------------------
	// Into acceleration, back against braking, in towards a curve: the
	// weight has to be where the force comes from, atan(a / g), and a share
	// of it is what reads. The pivot's plant is most of what sells it.
	// On a critically damped spring: the lean pivots on the feet, so it
	// carries the hips with it, and following a key's step change in
	// acceleration within a step moved them 10-20 cm at a jolt of up to
	// 480 m/s^2. Rate-limited instead, it still started at full rate in one
	// step (60 m/s^2 at the hips).
	float accel = (L.Speed - L.LastSpeed) / dt;
	L.LastSpeed = L.Speed;
	float pitchWanted = glm::clamp(std::atan(accel / 9.81f) * 0.35f, -0.16f, 0.13f);
	Spring(L.Pitch, L.PitchRate, pitchWanted, 9.0f, dt);
	float pitch = L.Pitch;
	// The curve is the path's, how fast the way he is going turns, not how
	// fast he turns to face: turning on the spot while drifting at a walk,
	// the facing's 9 rad/s banked him 0.3 rad and threw the hips 30 cm out
	// and back.
	float travelRate = wrap(L.Travel - L.LastTravel) / dt;
	L.LastTravel = L.Travel;
	float bank = glm::clamp(std::atan(L.Speed * travelRate / 9.81f) * 0.5f, -0.22f, 0.22f);
	Spring(L.Bank, L.BankRate, bank, 9.0f, dt);
	glm::quat lean = glm::angleAxis(L.Bank, glm::vec3(0.0f, 0.0f, -1.0f)) * glm::angleAxis(airborne ? 0.0f : pitch, glm::vec3(1.0f, 0.0f, 0.0f));
	// A spinning strike turns the whole of him (its feet are free: it leaps).
	glm::quat yaw = glm::angleAxis(m_LegYaw + StrikeSpin(), up);
	glm::quat turn = glm::normalize(yaw * lean);

	// --- the bodies, built from the turns (as ClipRetarget::PoseFrom does) ---
	const RestSkeleton& R = m_Rest;
	const glm::vec3 hipCentre = 0.5f * (R.Hip[0] + R.Hip[1]);
	glm::vec3 P[LocoPose::Bodies], hip[2], knee[2], ankle[2], recordedAnkle[2];
	glm::quat Q[LocoPose::Bodies], D[LocoPose::Bodies];
	{
		auto toWorld = [&](const glm::vec3& p) { return L.Origin + turn * p; };
		for (int i = 0; i < LocoPose::Bodies; i++)
			D[i] = glm::normalize(turn * pose.D[i]);
		glm::vec3 root = toWorld(pose.Root);
		auto place = [&](int body, const glm::vec3& joint, const glm::vec3& restJoint)
		{
			P[body] = joint + D[body] * (m_BuildPose[(size_t)body].first - restJoint);
			Q[body] = glm::normalize(D[body] * m_BuildPose[(size_t)body].second);
		};
		auto carry = [&](int body, const glm::vec3& joint, const glm::vec3& restJoint, const glm::vec3& restNext)
		{
			return joint + D[body] * (restNext - restJoint);
		};
		place(0, root, hipCentre);
		glm::vec3 spine = carry(0, root, hipCentre, R.Spine);
		place(1, spine, R.Spine);
		glm::vec3 neck = carry(1, spine, R.Spine, R.Neck);
		place(2, neck, R.Neck);
		float stance = ratio(m_StanceWidth, m_Design.StanceWidth);
		glm::vec3 across = turn * glm::vec3(1.0f, 0.0f, 0.0f);
		for (int side = 0; side < 2; side++)
		{
			hip[side] = carry(0, root, hipCentre, R.Hip[side]);
			knee[side] = carry(3 + side, hip[side], R.Hip[side], R.Knee[side]);
			ankle[side] = carry(5 + side, knee[side], R.Knee[side], R.Ankle[side]);
			recordedAnkle[side] = ankle[side];
			// A wider or narrower stance: the feet moved out from under the
			// hips; the legs are solved to them below.
			if (std::fabs(stance - 1.0f) > 1e-3f)
				ankle[side] += across * (glm::dot(ankle[side] - root, across) * (stance - 1.0f));
			// The feet at least so far apart: standing, hip width (the person
			// recorded standing had theirs all but together, which on him
			// read as stiff); walking, nearly as wide for a masculine body
			// and narrower for a feminine one -- the recorded walk put each
			// foot nearly under the middle, a catwalk's line.
			{
				float sideSign = side == 1 ? 1.0f : -1.0f;
				float out = glm::dot(ankle[side] - root, across) * sideSign;
				float least = glm::mix(glm::mix(glm::mix(0.09f, 0.06f, femininity), 0.11f, L.IdleWeight), 0.15f, guardStance) * s * stance;
				if (out < least)
					ankle[side] += across * (sideSign * (least - out));
			}
			glm::vec3 shoulder = carry(1, spine, R.Spine, R.Shoulder[side]);
			place(9 + side, shoulder, R.Shoulder[side]);
			glm::vec3 elbow = carry(9 + side, shoulder, R.Shoulder[side], R.Elbow[side]);
			place(11 + side, elbow, R.Elbow[side]);
		}
	}

	// --- the feet -------------------------------------------------------------
	// Pinned where they land, for as long as the stride has them down; let
	// go when it lifts them. Standing and turning, a foot left behind steps
	// across (a quick lift, a fifth of a second), one at a time, and the
	// other pivots on the ball of its foot -- a pivot, not a shuffle.
	const glm::vec3 footFromAnkle[2] = { m_BuildPose[7].first - R.Ankle[0], m_BuildPose[8].first - R.Ankle[1] };
	const float standing = glm::clamp((L.IdleWeight - 0.4f) / 0.4f, 0.0f, 1.0f);
	auto yawOf = [](const glm::quat& q) { glm::vec3 f = q * glm::vec3(0.0f, 0.0f, 1.0f); return std::atan2(f.x, f.z); };
	glm::vec3 footTarget[2];
	float footGround[2];
	float lag[2] = { 0.0f, 0.0f }, yawGap[2] = { 0.0f, 0.0f };
	for (int side = 0; side < 2; side++)
	{
		const LocoState::Foot& f = L.Feet[side];
		auto groundAt = [&](const glm::vec3& at)
		{
			float h;
			glm::vec3 n;
			return GroundUnder(glm::vec3(at.x, L.Origin.y + 0.6f * s, at.z), h, n, -1000.0f) ? h : L.Origin.y;
		};
		footGround[side] = groundAt(ankle[side]);
		footTarget[side] = ankle[side];
		// The stride's foot over the ground under it, not the ground under him:
		// on a stair the foot ahead is on the step ahead. A foot in the air
		// clears what is under its toes and a little beyond them too: asked
		// only under the ankle, the toes met each riser before the ankle
		// knew it was there (11 cm into the stairs).
		// What it clears is eased (a riser is a step function, and followed
		// exactly the foot leapt half a metre in a step running at stairs),
		// and looked for no more than 20 cm past the toes.
		if (!airborne)
		{
			float under = footGround[side];
			LocoState::Foot& g = L.Feet[side];
			if (f.State != LocoState::FootState::Planted)
			{
				glm::vec3 toes = ankle[side] + D[7 + side] * (footFromAnkle[side] + glm::vec3(0.0f, 0.0f, 0.12f * s));
				glm::vec3 reach = glm::vec3(velocity.x, 0.0f, velocity.z) * 0.12f;
				if (glm::length(reach) > 0.2f * s)
					reach *= 0.2f * s / glm::length(reach);
				// Nor reached down past a step below where he stands: off an
				// edge, the floor below is for the fall to find.
				under = std::max({ under, groundAt(toes), groundAt(toes + reach), L.Origin.y - 0.35f * s });
				footGround[side] = std::max(footGround[side], groundAt(toes));
				Spring(g.Ground, g.GroundRate, under, 25.0f, dt);
				under = g.Ground;
			}
			else
			{
				g.Ground = under;
				g.GroundRate = 0.0f;
			}
			footTarget[side].y += under - L.Origin.y;
		}
		else
		{
			// The knees come up -- 30 cm of tuck standing, 8 off a run whose
			// leap already has the legs where they go -- and reach back down
			// for the ground as it comes.
			float tuck = glm::mix(0.30f, 0.08f, L.JumpRun) * s;
			footTarget[side] += up * (tuck * glm::smoothstep(0.0f, 0.15f, L.AirTimer) * landing);
			// Kept on the ground below while he flies, or he lands reaching
			// for the height he left (a foot leapt 46 cm on landing off a
			// 0.85 m ledge).
			L.Feet[side].Ground = std::max(footGround[side], L.Origin.y - 0.35f * s);
			L.Feet[side].GroundRate = 0.0f;
		}
		if (f.State == LocoState::FootState::Planted)
		{
			glm::vec2 gap(footTarget[side].x - f.Ankle.x, footTarget[side].z - f.Ankle.z);
			lag[side] = glm::length(gap);
			yawGap[side] = std::fabs(wrap(yawOf(D[7 + side]) - yawOf(f.Turn)));
		}
	}
	// How far the sole clears the ground with the ankle there, turned so --
	// at the worst of its corners, each over the ground under it, so a foot
	// over a stair's edge has its toes on the step above, not in its riser.
	// Negative is into it.
	auto soleClearance = [&](int side, const glm::vec3& at, const glm::quat& turnFrom)
	{
		const glm::vec3 he = m_World.GetBody(m_Foot[side]).HalfExtents;
		glm::vec3 centre = at + turnFrom * footFromAnkle[side];
		glm::quat q = glm::normalize(turnFrom * m_BuildPose[(size_t)(7 + side)].second);
		float clearance = 1e9f;
		for (float x : { -he.x, he.x })
			for (float z : { -he.z, he.z })
				{
					glm::vec3 corner = centre + q * glm::vec3(x, -he.y, z);
					float h;
					glm::vec3 n;
					float ground = GroundUnder(glm::vec3(corner.x, L.Origin.y + 0.6f * s, corner.z), h, n, -1000.0f) ? h : footGround[side];
					clearance = std::min(clearance, corner.y - ground);
				}
		return clearance;
	};
	glm::vec3 ankleAt[2];
	glm::quat footTurn[2];
	for (int side = 0; side < 2; side++)
	{
		LocoState::Foot& f = L.Feet[side];
		LocoState::Foot& other = L.Feet[1 - side];
		bool wantDown = !airborne && down[side] > 0.5f;
		const glm::quat animTurn = D[7 + side];
		switch (f.State)
		{
			case LocoState::FootState::Planted:
			{
				bool stepNeeded = standing > 0.5f
					&& (lag[side] > 0.12f * s || yawGap[side] > glm::radians(40.0f))
					&& other.State != LocoState::FootState::Stepping
					&& (other.State != LocoState::FootState::Planted || lag[side] >= lag[1 - side]);
				if (wantDown)
					f.Early = false;
				if (!wantDown && !f.Early)
				{
					f.State = LocoState::FootState::Free;
					f.Offset = f.Ankle - footTarget[side];
					f.Peak = 0.0f;
				}
				else if (stepNeeded)
				{
					f.State = LocoState::FootState::Stepping;
					f.From = f.Ankle;
					f.FromTurn = f.Turn;
					f.T = 0.0f;
				}
				else if (lag[side] > 0.30f * s || (f.Early && airborne))
				{
					// Left far behind by a change of pace: let it catch up.
					f.State = LocoState::FootState::Free;
					f.Offset = f.Ankle - footTarget[side];
					f.Early = false;
					f.Peak = 0.0f;
				}
				else if (yawGap[side] > glm::radians(8.0f) && (std::fabs(turnNow) > 1.0f || standing > 0.5f))
				{
					// The pivot: turned towards the way the body faces about
					// the ball of the foot, which stays where it is.
					float most = 10.0f * dt;
					float d = wrap(yawOf(animTurn) - yawOf(f.Turn));
					float by = glm::clamp(d, -most, most);
					glm::quat spin = glm::angleAxis(by, up);
					glm::vec3 ball = f.Ankle + f.Turn * (footFromAnkle[side] + glm::vec3(0.0f, 0.0f, 0.09f * s));
					f.Turn = glm::normalize(spin * f.Turn);
					f.Ankle = ball - f.Turn * (footFromAnkle[side] + glm::vec3(0.0f, 0.0f, 0.09f * s));
				}
				break;
			}
			case LocoState::FootState::Stepping:
			{
				f.T += dt / 0.2f;
				if (f.T >= 1.0f)
				{
					f.State = wantDown ? LocoState::FootState::Planted : LocoState::FootState::Free;
					f.Ankle = footTarget[side];
					f.Turn = animTurn;
					f.Offset = glm::vec3(0.0f);
					f.Early = false;
					f.Peak = 0.0f;
				}
				break;
			}
			case LocoState::FootState::Free:
			{
				f.Offset *= std::exp(-dt / 0.07f);
				glm::vec3 at = footTarget[side] + f.Offset;
				if (wantDown && standing > 0.5f && glm::length(glm::vec2(f.Offset.x, f.Offset.z)) > 0.06f * s)
				{
					// Come to a stop with this foot well off where standing
					// puts it: stepped there, not slid.
					f.State = LocoState::FootState::Stepping;
					f.From = at;
					f.FromTurn = animTurn;
					f.T = 0.0f;
					f.Offset = glm::vec3(0.0f);
				}
				else if (wantDown)
				{
					f.State = LocoState::FootState::Planted;
					f.Ankle = at;
					f.Turn = glm::normalize(ConformToGround(at, side) * animTurn);
					f.Offset = glm::vec3(0.0f);
					f.Early = false;
				}
				break;
			}
		}
		// A planted foot sits on the ground: eased onto it, so a foot planted
		// a little high (as standing takes over mid-stride) settles rather
		// than hovering, and one planted on a slope does not pop.
		if (f.State == LocoState::FootState::Planted)
			f.Ankle.y -= soleClearance(side, f.Ankle, f.Turn) * (1.0f - std::exp(-dt / 0.05f));
		switch (f.State)
		{
			case LocoState::FootState::Planted:
				ankleAt[side] = f.Ankle;
				footTurn[side] = f.Turn;
				break;
			case LocoState::FootState::Stepping:
			{
				float e = glm::smoothstep(0.0f, 1.0f, f.T);
				ankleAt[side] = glm::mix(f.From, footTarget[side], e) + up * (0.08f * s * std::sin(glm::pi<float>() * f.T));
				footTurn[side] = glm::normalize(glm::slerp(f.FromTurn, animTurn, e));
				break;
			}
			default:
				ankleAt[side] = footTarget[side] + f.Offset;
				footTurn[side] = animTurn;
				break;
		}
		// Never into the ground: the sole on it at most.
		float clearance = soleClearance(side, ankleAt[side], footTurn[side]);
		if (clearance < 0.0f)
		{
			ankleAt[side].y -= clearance;
			if (f.State == LocoState::FootState::Planted)
				f.Ankle.y = ankleAt[side].y;
		}
		// A swing that comes down onto the ground before the stride says it
		// lands -- the recording's floor is a percentile, and the heel meets
		// it a few frames early -- lands there: left free, it skidded along
		// the ground at up to 3.6 m/s until the stride caught up.
		if (f.State == LocoState::FootState::Free && !airborne)
		{
			f.Peak = std::max(f.Peak, clearance);
			if (f.Peak > 0.03f * s && clearance < 0.003f)
			{
				f.State = LocoState::FootState::Planted;
				f.Ankle = ankleAt[side];
				f.Turn = footTurn[side];
				f.Offset = glm::vec3(0.0f);
				f.Early = !wantDown;
				f.Peak = 0.0f;
			}
		}
	}

	// --- the legs reach them; the hips come down where they cannot ----------
	const float thigh = glm::length(R.Knee[0] - R.Hip[0]);
	const float shin = glm::length(R.Ankle[0] - R.Knee[0]);
	const float reachMost = (thigh + shin) * 0.995f;
	float need = 0.0f;
	for (int side = 0; side < 2; side++)
		need = std::max(need, glm::length(ankleAt[side] - hip[side]) - reachMost);
	// Down as quickly as the reach is lost, back up gently: sprung back at
	// the rate it came down, the hips shot up 9 cm in a step after a stop.
	if (need > L.Drop)
		Spring(L.Drop, L.DropRate, need, 20.0f, dt);
	else
		Spring(L.Drop, L.DropRate, need, 8.0f, dt);
	L.Drop = std::max(L.Drop, 0.0f);
	for (int i : { 0, 1, 2, 9, 10, 11, 12 })
		P[i].y -= L.Drop + lower;
	for (int side = 0; side < 2; side++)
	{
		hip[side].y -= L.Drop + lower;
		knee[side].y -= L.Drop + lower;
	}
	for (int side = 0; side < 2; side++)
	{
		glm::vec3 toEnd = ankleAt[side] - hip[side];
		float d = glm::length(toEnd);
		glm::vec3 dir = d > 1e-5f ? toEnd / d : -up;
		d = glm::clamp(d, std::fabs(thigh - shin) + 0.01f, (thigh + shin) * 0.999f);
		// The knee bends the way the stride bent it.
		// Which way the knee bends: over the toes, as a knee does. Taken from
		// the recording's knee alone, a leg nearly straight has almost no
		// knee to take a direction from, and it swung sideways when he set
		// off from standing -- 138 degrees in a step. The recording's is
		// used only as far as it bends the knee clearly (2 cm to 6 cm off the
		// line from hip to ankle). A masculine body's knees a little further
		// out (a nudge: at 0.35 of the leg he went bow-legged).
		glm::vec3 toes = footTurn[side] * glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 overToes = toes - dir * glm::dot(toes, dir);
		if (glm::length(overToes) < 1e-3f)
			overToes = forward - dir * glm::dot(forward, dir);
		overToes = glm::normalize(overToes);
		// The recorded knee's way off the recorded leg's own line: off the
		// line to where the foot is now -- moved out by the stance, held where
		// it landed -- it read as the knee pointing in (45 degrees, standing).
		glm::vec3 recordedLine = glm::normalize(recordedAnkle[side] - hip[side]);
		glm::vec3 recorded = (knee[side] - hip[side]) - recordedLine * glm::dot(knee[side] - hip[side], recordedLine);
		recorded -= dir * glm::dot(recorded, dir);
		float clearly = glm::smoothstep(0.02f, 0.06f, glm::length(recorded));
		glm::vec3 perp = glm::length(recorded) > 1e-4f
			? glm::mix(overToes, glm::normalize(recorded), clearly) : overToes;
		{
			float sideSign = side == 1 ? 1.0f : -1.0f;
			glm::vec3 across = turn * glm::vec3(1.0f, 0.0f, 0.0f);
			perp += across * (sideSign * (1.0f - femininity) * 0.08f);
		}
		perp = perp - dir * glm::dot(perp, dir);
		perp = glm::length(perp) > 1e-4f ? glm::normalize(perp) : overToes;
		float cosA = glm::clamp((thigh * thigh + d * d - shin * shin) / (2.0f * thigh * d), -1.0f, 1.0f);
		float a = std::acos(cosA);
		glm::vec3 kneeAt = hip[side] + thigh * (cosA * dir + std::sin(a) * perp);
		glm::vec3 ankleReached = hip[side] + dir * d;
		glm::quat Dt = glm::normalize(RotationBetween(glm::normalize(D[3 + side] * (R.Knee[side] - R.Hip[side])),
			glm::normalize(kneeAt - hip[side])) * D[3 + side]);
		glm::quat Ds = glm::normalize(RotationBetween(glm::normalize(D[5 + side] * (R.Ankle[side] - R.Knee[side])),
			glm::normalize(ankleReached - kneeAt)) * D[5 + side]);
		P[3 + side] = hip[side] + Dt * (m_BuildPose[(size_t)(3 + side)].first - R.Hip[side]);
		Q[3 + side] = glm::normalize(Dt * m_BuildPose[(size_t)(3 + side)].second);
		P[5 + side] = kneeAt + Ds * (m_BuildPose[(size_t)(5 + side)].first - R.Knee[side]);
		Q[5 + side] = glm::normalize(Ds * m_BuildPose[(size_t)(5 + side)].second);
		P[7 + side] = ankleReached + footTurn[side] * footFromAnkle[side];
		Q[7 + side] = glm::normalize(footTurn[side] * m_BuildPose[(size_t)(7 + side)].second);
		m_Planted[side] = ankleReached;
	}

	// The shield up and the sword swinging, over the stride's arms.
	PoseArmament(P, Q, dt);

	// --- on to the bodies -------------------------------------------------------
	LocoPlace(P, Q, dt, true, 0.25f, false);

	// What the rest of him reads of where he is.
	m_StandHeight = P[0].y;
	m_RootAnchor = P[0];
}


// The bodies put where P and Q say, kinematic: the position reached through
// the velocity (so what he walks into is pushed by something moving, not
// teleported through), the turn set outright, each body's turning rate kept
// for the hand-over to physics. Out of whatever had him, eased over
// `enterTime`.
//
// `colliding`: in a move, where a shin goes over a box and a hand onto a
// ledge. The limbs stay dynamic, steered onto the pose by their velocity and
// spin (gravity paid in advance) -- but one touching the scenery is left to
// its motor and the contact, as the moves always had it, until it comes
// free. Placed outright, a posed limb went 10-25 cm into what it met;
// steered regardless, an arm whose target lay inside a wall was driven
// back into it every step against the contact (16-19 cm, where the motors
// alone had 6).
inline void Humanoid::LocoPlace(const glm::vec3* P, const glm::quat* Q, float dt, bool pelvisToo, float enterTime, bool colliding)
{
	LocoState& L = *m_Loco;
	if (!m_LocoPosed)
	{
		// Taken from physics this step: start from where the bodies lie.
		for (int i = 0; i < LocoPose::Bodies; i++)
		{
			L.Touched[i] = 1.0f;
			const GS::RigidBody3D& body = m_World.GetBody(m_CharacterBodies[(size_t)i]);
			L.FromP[i] = body.Position;
			L.FromQ[i] = body.Orientation;
		}
		L.Blend = 0.0f;
	}
	float blend = 1.0f;
	if (L.Blend < 1.0f)
	{
		L.Blend = std::min(L.Blend + dt / std::max(enterTime, 1e-3f), 1.0f);
		blend = glm::smoothstep(0.0f, 1.0f, L.Blend);
	}
	bool touching[LocoPose::Bodies] = {};
	if (colliding)
		for (const GS::Contact3D& contact : m_World.GetContacts())
		{
			if (contact.PointCount == 0)
				continue;
			bool mineA = IsCharacter(contact.A), mineB = IsCharacter(contact.B);
			if (mineA == mineB)
				continue;
			unsigned int mine = mineA ? contact.A : contact.B;
			for (int i = 1; i < LocoPose::Bodies; i++)
				if (m_CharacterBodies[(size_t)i] == mine)
					L.Touched[i] = 0.0f;
		}
	// Held to physics for 0.15 s after it last touched: freed the step it was
	// pushed clear, it was steered straight back through the ledge's corner,
	// in and out every other step (an upper arm 18 cm into the climb's block).
	if (colliding)
		for (int i = 1; i < LocoPose::Bodies; i++)
		{
			touching[i] = L.Touched[i] < 0.15f;
			L.Touched[i] += dt;
		}
	for (int i = pelvisToo ? 0 : 1; i < LocoPose::Bodies; i++)
	{
		GS::RigidBody3D& body = m_World.GetBody(m_CharacterBodies[(size_t)i]);
		glm::vec3 p = P[i];
		glm::quat q = Q[i];
		if (blend < 1.0f)
		{
			p = glm::mix(L.FromP[i], p, blend);
			glm::quat to = glm::dot(L.FromQ[i], q) < 0.0f ? -q : q;
			q = glm::normalize(glm::slerp(L.FromQ[i], to, blend));
		}
		glm::quat delta = glm::normalize(q * glm::conjugate(body.Orientation));
		if (delta.w < 0.0f)
			delta = -delta;
		glm::vec3 axis(delta.x, delta.y, delta.z);
		float length = glm::length(axis);
		L.Spin[i] = length > 1e-6f ? axis / length * (2.0f * std::atan2(length, delta.w) / dt) : glm::vec3(0.0f);
		if (colliding && i > 0 && touching[i])
		{
			body.Type = GS::BodyType::Dynamic;
			continue;
		}
		if (colliding && i > 0)
		{
			// Back onto the pose no faster than 8 m/s and 30 rad/s: a limb
			// coming free of the scenery eases back rather than snapping (a
			// body at 36 m/s for a step), and one swinging at a ledge meets
			// it before it is deep in (at 12 m/s, 20 cm a step, an upper arm
			// went 13 cm into the climb's corner before its contact stopped
			// it).
			glm::vec3 v = (p - body.Position) / dt;
			if (glm::length(v) > 8.0f)
				v *= 8.0f / glm::length(v);
			glm::vec3 w = L.Spin[i];
			if (glm::length(w) > 30.0f)
				w *= 30.0f / glm::length(w);
			body.Type = GS::BodyType::Dynamic;
			body.Velocity = v - m_World.Gravity * (body.GravityScale * dt);
			body.AngularVelocity = w;
		}
		else
		{
			body.Type = GS::BodyType::Kinematic;
			body.Velocity = (p - body.Position) / dt;
			body.Orientation = q;
			body.AngularVelocity = glm::vec3(0.0f);
			body.UpdateInertiaWorld();
		}
		body.Awake = true;
		body.SleepTimer = 0.0f;
	}
	m_LocoPosed = true;
}

// A move -- a vault, a climb, a wall run, a dodge, a hang -- poses him as the
// walk does. The moves were written as joint targets for motors to pull the
// limbs to, and motors lag, overshoot, and lose to anything the limb hits:
// the climb's legs kicked about behind him, on the old walk as on the new
// (the owner, 2026-10-21). The targets are the pose the move means, so the
// body is built from them -- down the joints from the pelvis the move
// carries: each body turned as its parent is and then by its joint's target
// (a ball joint's is the child's turn in the parent's frame; a hinge's, its
// angle about the hinge's axis from how it was built), and put where the
// two anchors of its joint meet.
inline void Humanoid::PoseFromMotors(float dt)
{
	if (!m_Loco)
		m_Loco = std::make_shared<LocoState>();
	dt = std::max(dt, 1e-4f);
	glm::vec3 P[LocoPose::Bodies];
	glm::quat Q[LocoPose::Bodies];
	auto index = [&](unsigned int handle)
	{
		for (int i = 0; i < LocoPose::Bodies; i++)
			if (m_CharacterBodies[(size_t)i] == handle)
				return i;
		return 0;
	};
	const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
	P[0] = pelvis.Position + pelvis.Velocity * dt;
	Q[0] = pelvis.Orientation;
	auto place = [&](GS::PhysicsWorld3D::JointHandle handle)
	{
		const GS::Joint3D& j = m_World.GetJoint(handle);
		int a = index(j.A), b = index(j.B);
		glm::quat turn;
		if (j.Type == GS::JointType3D::Ball)
			turn = Q[a] * j.MotorTargetRotation;
		else
		{
			glm::quat built = glm::conjugate(m_BuildPose[(size_t)a].second) * m_BuildPose[(size_t)b].second;
			turn = Q[a] * glm::angleAxis(j.MotorTargetAngle, glm::normalize(j.LocalAxisA)) * built;
		}
		Q[b] = glm::normalize(turn);
		P[b] = P[a] + Q[a] * j.LocalAnchorA - Q[b] * j.LocalAnchorB;
	};
	place(m_SpineJoint);
	place(m_NeckJoint);
	for (int side = 0; side < 2; side++)
	{
		place(m_HipJoint[side]);
		place(m_KneeJoint[side]);
		place(m_AnkleJoint[side]);
		place(m_ShoulderJoint[side]);
		place(m_ElbowJoint[side]);
	}
	LocoPlace(P, Q, dt, false, 0.12f, true);
}
