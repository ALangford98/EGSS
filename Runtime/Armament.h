#pragma once

// A sword and a shield, and what they do to a cannonball.
//
// The owner, 2026-10-22: give him a sword and shield to block, parry and
// reflect the cannonballs -- simple mechanics first, the styling later. So
// three answers to a ball, each on the button a player expects:
//
//   * hold the guard and the shield comes up across his chest: a ball that
//     reaches it is BLOCKED -- dropped in front of him, and he is shoved
//     back a little (kinematically, never past what his feet can catch);
//   * raise it no more than kParryWindow before the ball arrives and it is
//     PARRIED -- glanced up and away at speed, no shove: the timing is what
//     the reward is for;
//   * swing the sword and catch it in the cut, and it is STRUCK -- sent back
//     at whatever threw it (the thrower aims it: Gunnery sends it at the
//     cannon that fired it, and a cannon has health).
//
// Forgiving on purpose. A ball is not met where the blade or the shield's
// rim actually is -- a sphere about him decides, the shield's a little
// bigger than his trunk and the sword's as far as the blade reaches in front
// of him -- because a defence that fails on a two-centimetre miss reads as
// broken, not as hard. Whether the timing was right is the whole skill.
//
// Then (the owner, same day): the guard should look right -- no running
// under it -- with lunges for the sword, and the sword and shield put away
// before any parkour. So: under the guard he walks at 0.6 of his walk, his
// chest on the aim, his legs turned towards where he goes or backpedalling
// (DriveLocomotion), a little lower and wider, the sword held up beside the
// shield. Attack from the guard is a short lunge behind the shield (1.2 m),
// from a run a long leaping one (2.4 m): a thrust, the run's moment of
// flight for the legs. Sheathed -- the sword at the left hip, the shield on
// his back, half a second of reaching -- he can vault and climb; drawn he
// cannot (ParkourOn), and a press of attack or the guard draws them.
//
// The props are drawn from his forearms, so they go wherever the arms go:
// held, swinging, or limp in a fall. Nothing of them is simulated. Posed, the
// arms are put where the guard and the swing say over the stride's own
// (PoseArmament, called from DriveLocomotion before the bodies are placed).
//
// Included at the end of Humanoid.h, after Locomotion.h: Humanoid's own
// members, as that file's are.

// Where the left hand goes to hold the shield up, from the left shoulder in
// his own frame (+x his left, +z ahead), and which way the elbow points: out
// to the side, a little down and back -- the forearm then lies across the
// chest with the shield's face straight ahead.
inline void ArmamentGuardTarget(float s, float recoil, glm::vec3& wrist, glm::vec3& pole)
{
	wrist = glm::vec3(-0.30f * s, -0.10f * s, 0.33f * s - recoil);
	pole = glm::vec3(1.0f, -0.35f, -0.25f);
}

inline void Humanoid::SetArmed(bool armed)
{
	m_Armed = armed;
	m_WantsAttack = m_WantsSheathe = false;
	m_Drawn = true;
	m_Stow = m_Lunge = -1.0f;
	m_Guard = 0.0f;
	m_GuardHeld = -1.0f;
	m_Swing = -1.0f;
	m_SwingQueued = false;
	m_Recoil = m_RecoilRate = 0.0f;
	m_Wrist = 0.75f;
}

inline glm::vec3 Humanoid::RestWrist(int side) const
{
	const auto& built = m_BuildPose[(size_t)(11 + side)];
	return built.first + built.second * glm::vec3(0.0f, -0.5f * m_ForearmLength, 0.0f);
}

// The turns (from how they were built) that put the wrist at `wrist` with the
// elbow towards `pole`. Each bone gets a whole frame, as SolveArm builds its
// motor targets: +y up the bone, +z the way the forearm folds (away from the
// elbow's point), +x the hinge -- shared by both bones, so the elbow only
// ever bends the way an elbow can. The same frames built of the arm as it
// was made (hanging, folding forward) are what the turns are taken from.
inline void Humanoid::ArmTurns(int side, const glm::vec3& shoulder, const glm::vec3& wrist, const glm::vec3& pole,
	glm::quat& upper, glm::quat& fore) const
{
	const glm::vec3 u0 = m_Rest.Elbow[side] - m_Rest.Shoulder[side];
	const glm::vec3 f0 = RestWrist(side) - m_Rest.Elbow[side];
	const float a = glm::length(u0), b = glm::length(f0);
	glm::vec3 to = wrist - shoulder;
	float d = glm::length(to);
	glm::vec3 dir = d > 1e-5f ? to / d : glm::vec3(0.0f, -1.0f, 0.0f);
	d = glm::clamp(d, std::fabs(a - b) + 0.01f, (a + b) * 0.999f);
	glm::vec3 e = pole - dir * glm::dot(pole, dir);
	if (glm::length(e) < 1e-3f)
	{
		glm::vec3 down(0.0f, -1.0f, 0.0f);
		e = down - dir * glm::dot(down, dir);
		if (glm::length(e) < 1e-3f)
			e = glm::vec3(0.0f, 0.0f, -1.0f);
	}
	e = glm::normalize(e);
	float cosA = glm::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
	glm::vec3 elbow = shoulder + a * (cosA * dir + std::sqrt(std::max(1.0f - cosA * cosA, 0.0f)) * e);
	glm::vec3 reached = shoulder + dir * d;

	auto frame = [](const glm::vec3& y, const glm::vec3& zHint)
	{
		glm::vec3 z = zHint - y * glm::dot(zHint, y);
		z = glm::length(z) > 1e-5f ? glm::normalize(z) : glm::vec3(0.0f, 0.0f, 1.0f);
		return glm::mat3(glm::cross(y, z), y, z);
	};
	glm::mat3 U = frame(glm::normalize(shoulder - elbow), -e);
	glm::vec3 hinge = U[0];
	glm::vec3 yf = glm::normalize(elbow - reached);
	glm::mat3 F(hinge, yf, glm::cross(hinge, yf));
	glm::mat3 U0 = frame(glm::normalize(-u0), glm::vec3(0.0f, 0.0f, 1.0f));
	glm::vec3 y0 = glm::normalize(-f0);
	glm::mat3 F0(U0[0], y0, glm::cross(U0[0], y0));
	upper = glm::normalize(glm::quat_cast(U * glm::transpose(U0)));
	fore = glm::normalize(glm::quat_cast(F * glm::transpose(F0)));
}

inline void Humanoid::UpdateArmament(float dt)
{
	if (!m_Armed)
	{
		m_WantsAttack = m_WantsSheathe = false;
		return;
	}
	const float s = m_Proportions.Scale;
	// Only posed: in a move, a fall or getting up the arms are the move's.
	const bool posed = m_Mode == Mode::Controlled;
	const bool pressedGuard = m_Control.Enabled && m_Control.Guard && !m_GuardWas;
	m_GuardWas = m_Control.Enabled && m_Control.Guard;

	// Drawing and sheathing: half a second of reaching, the sword from the
	// left hip, the shield off his back. Asked for, or by reaching for the
	// guard or the sword while they are put away. Anything that takes him out
	// of the walk finishes it at once -- he does not fall with a hand on
	// the hilt.
	auto beginStow = [&]()
	{
		m_Stow = 0.0f;
		m_Drawn = !m_Drawn;
		m_Swing = m_Lunge = -1.0f;
		m_SwingQueued = false;
	};
	if (m_WantsSheathe)
	{
		m_WantsSheathe = false;
		if (posed && m_Stow < 0.0f && m_Swing < 0.0f && m_Lunge < 0.0f)
			beginStow();
	}
	if (!m_Drawn && m_Stow < 0.0f && posed && (m_WantsAttack || pressedGuard))
	{
		m_WantsAttack = false;
		beginStow();
	}
	if (m_Stow >= 0.0f)
	{
		m_Stow += dt;
		if (m_Stow >= kStowTime || !posed)
			m_Stow = -1.0f;
	}

	const bool guard = posed && m_Control.Enabled && m_Control.Guard && Drawn();
	// Up in under a tenth of a second -- the guard is a reaction -- and down
	// a little slower.
	m_Guard = glm::clamp(m_Guard + (guard ? dt / 0.08f : -dt / 0.15f), 0.0f, 1.0f);
	m_GuardHeld = guard ? (m_GuardHeld < 0.0f ? 0.0f : m_GuardHeld + dt) : -1.0f;
	Spring(m_Recoil, m_RecoilRate, 0.0f, 16.0f, dt);
	m_Recoil = glm::clamp(m_Recoil, -0.05f * s, 0.16f * s);

	// A press starts a swing; one late in a swing is held for the next, which
	// comes back the other way (forehand, then backhand). An early press is
	// dropped, not queued: mashing should not lock him into a string of them.
	// From the guard it is a lunge instead -- a step in behind the shield --
	// and at a run a long one, leaping.
	if (m_WantsAttack)
	{
		m_WantsAttack = false;
		if (posed && Drawn() && m_Lunge < 0.0f)
		{
			bool fromGuard = guard;
			bool fromRun = !guard && m_Air == Air::Grounded && m_Speed > 1.25f * m_WalkSpeed;
			if (m_Swing < 0.0f && (fromGuard || fromRun) && m_Air == Air::Grounded)
			{
				m_Lunge = 0.0f;
				m_LungeRunning = fromRun;
				m_LungeBase = fromRun ? m_Speed : 0.0f;
				m_Swings++;
			}
			else if (m_Swing < 0.0f)
			{
				m_Swing = 0.0f;
				m_SwingWay = 1.0f;
				m_Swings++;
			}
			else if (m_Swing > 0.15f)
				m_SwingQueued = true;
		}
	}
	if (m_Swing >= 0.0f)
	{
		if (!posed)
		{
			m_Swing = -1.0f;
			m_SwingQueued = false;
		}
		else
		{
			m_Swing += dt;
			if (m_SwingQueued && m_Swing >= 0.28f)
			{
				m_Swing = 0.0f;
				m_SwingWay = -m_SwingWay;
				m_SwingQueued = false;
				m_Swings++;
			}
			else if (m_Swing >= kSwingTime)
				m_Swing = -1.0f;
		}
	}
	if (m_Lunge >= 0.0f)
	{
		m_Lunge += dt;
		if (!posed || m_Lunge >= kLungeTime)
			m_Lunge = -1.0f;
	}
	// The wrist: the blade nearly in line with the forearm for the cut, so the
	// whole arm and blade sweep as one lever, and exactly in line for a
	// thrust; carried, 43 degrees down from straight out of the fist -- at 20
	// he carried it like a lance, and at 43 the tip stays 28 cm off the floor
	// walking and running, measured.
	float wristWant = m_Lunge >= 0.0f && m_Lunge < 0.40f ? 1.57f : m_Swing >= 0.0f && m_Swing < 0.30f ? 1.45f : 0.75f;
	float most = 25.0f * dt;
	m_Wrist += glm::clamp(wristWant - m_Wrist, -most, most);

	// The shield's strap: worked out so that, guarding, its face is straight
	// ahead -- from the guard's own solution in the frame he was built in.
	// Strapped there, it stays on his forearm wherever the arm goes.
	if (m_BuildPose.size() >= 13)
	{
		glm::vec3 wrist, pole;
		ArmamentGuardTarget(s, 0.0f, wrist, pole);
		glm::quat upper, fore;
		ArmTurns(1, m_Rest.Shoulder[1], m_Rest.Shoulder[1] + wrist, pole, upper, fore);
		glm::quat Q = glm::normalize(fore * m_BuildPose[12].second);
		glm::vec3 face = glm::conjugate(Q) * glm::vec3(0.0f, 0.0f, 1.0f);
		face.y = 0.0f;
		if (glm::length(face) > 1e-4f)
			m_Strap = glm::normalize(face);
	}
}

// Where the sword sits sheathed: the grip in front of the left hip, the
// blade down and back past it, 42 degrees below level -- at 29 the scabbard
// stuck out behind him like a tail, at 55 the run's forward-pitched hips
// swung its tip to the floor -- in the pelvis's frame (+x his left).
inline glm::mat4 ArmamentFrame(const glm::vec3& at, const glm::vec3& y, const glm::vec3& xHint)
{
	glm::vec3 x = glm::normalize(xHint - y * glm::dot(xHint, y));
	glm::mat4 m(1.0f);
	m[0] = glm::vec4(x, 0.0f);
	m[1] = glm::vec4(y, 0.0f);
	m[2] = glm::vec4(glm::cross(x, y), 0.0f);
	m[3] = glm::vec4(at, 1.0f);
	return m;
}
inline glm::mat4 ArmamentHipFrame(const glm::vec3& pelvis, const glm::quat& q, float s)
{
	return ArmamentFrame(pelvis + q * glm::vec3(0.20f * s, 0.03f * s, 0.10f * s),
		glm::normalize(q * glm::vec3(0.10f, -0.90f, -1.0f)), q * glm::vec3(0.0f, 1.0f, 0.0f));
}

// The guard, the swing, the lunge and the reach for the hip and the back,
// over the arms the stride posed: the left arm up with the shield, the right
// ready beside it, swept across in front of him or thrust straight out with
// the shoulders turning into it. Each laid over what is there, blended in by
// how far into it he is, so nothing snaps an arm.
inline void Humanoid::PoseArmament(glm::vec3* P, glm::quat* Q, float dt)
{
	(void)dt;
	if (!m_Armed || m_BuildPose.size() < 13)
		return;
	const float s = m_Proportions.Scale;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::quat yaw = glm::angleAxis(m_Facing, up);
	const RestSkeleton& R = m_Rest;
	auto turnOf = [&](int b) { return glm::normalize(Q[b] * glm::conjugate(m_BuildPose[(size_t)b].second)); };
	auto spineAt = [&]() { return P[1] - turnOf(1) * (m_BuildPose[1].first - R.Spine); };
	// Turn the trunk and arms about the spine -- and the head with them, or
	// only carried (it keeps watching what it was watching).
	auto turnTrunk = [&](const glm::quat& r, bool head)
	{
		glm::vec3 spine = spineAt();
		for (int b : { 1, 2, 9, 10, 11, 12 })
		{
			P[b] = spine + r * (P[b] - spine);
			if (b != 2 || head)
				Q[b] = glm::normalize(r * Q[b]);
		}
	};

	// The chest back on his aim, over legs turned to where he is going (the
	// guard's walk, in DriveLocomotion).
	float waist = m_Facing - m_LegYaw;
	while (waist > glm::pi<float>()) waist -= glm::two_pi<float>();
	while (waist < -glm::pi<float>()) waist += glm::two_pi<float>();
	if (std::fabs(waist) > 1e-4f)
		turnTrunk(glm::angleAxis(waist, up), true);

	// The swing: the hand on an arc about the right shoulder, from out to
	// his right and high to across his front and low (the backhand from
	// across to out), nearly at full reach. Angles are about up from ahead,
	// towards his right -- the backhand's start is kept short of straight
	// across, where the hand would be inside his chest.
	float swingW = 0.0f, arc = 0.0f, height = 0.0f, twist = 0.0f, lean = 0.0f;
	if (m_Swing >= 0.0f)
	{
		float t = m_Swing;
		float u = glm::smoothstep(kSwingCutFrom, kSwingCutTo, t);
		swingW = glm::smoothstep(0.0f, kSwingCutFrom, t) * (1.0f - glm::smoothstep(kSwingCutTo + 0.02f, kSwingTime, t));
		float from = m_SwingWay > 0.0f ? 80.0f : -55.0f, to = m_SwingWay > 0.0f ? -58.0f : 80.0f;
		arc = glm::radians(glm::mix(from, to, u));
		height = glm::mix(0.08f, -0.16f, u) * s;
		// Turning towards his right is a falling yaw.
		twist = m_SwingWay * glm::mix(-0.35f, 0.40f, u) * swingW;
	}
	// The lunge: the hand drawn back to the hip, then thrust straight out at
	// shoulder height, the right shoulder driven forward and the trunk
	// leaning into it.
	float lungeW = 0.0f, thrust = 0.0f;
	if (m_Lunge >= 0.0f)
	{
		float t = m_Lunge;
		thrust = glm::smoothstep(kLungeWindup, kLungeWindup + 0.10f, t);
		lungeW = glm::smoothstep(0.0f, 0.06f, t) * (1.0f - glm::smoothstep(kLungeReach, kLungeTime, t));
		twist += glm::mix(-0.30f, 0.45f, thrust) * lungeW;
		lean = (m_LungeRunning ? 0.30f : 0.22f) * thrust * lungeW;
	}
	if (std::fabs(twist) > 1e-4f)
		turnTrunk(glm::angleAxis(twist, up), false);
	if (lean > 1e-4f)
		turnTrunk(glm::angleAxis(lean, yaw * glm::vec3(1.0f, 0.0f, 0.0f)), false);

	// One arm towards a wrist and an elbow's way, by `w`, over what it has.
	auto reach = [&](int side, const glm::vec3& wrist, const glm::vec3& pole, float w)
	{
		if (w < 1e-4f)
			return;
		const int upperBody = 9 + side, foreBody = 11 + side;
		glm::quat upper0 = turnOf(upperBody), fore0 = turnOf(foreBody);
		glm::vec3 shoulder = P[upperBody] - upper0 * (m_BuildPose[(size_t)upperBody].first - R.Shoulder[side]);
		glm::quat upper, fore;
		ArmTurns(side, shoulder, shoulder + wrist, pole, upper, fore);
		upper = glm::normalize(glm::slerp(upper0, upper, w));
		fore = glm::normalize(glm::slerp(fore0, fore, w));
		P[upperBody] = shoulder + upper * (m_BuildPose[(size_t)upperBody].first - R.Shoulder[side]);
		Q[upperBody] = glm::normalize(upper * m_BuildPose[(size_t)upperBody].second);
		glm::vec3 elbow = shoulder + upper * (R.Elbow[side] - R.Shoulder[side]);
		P[foreBody] = elbow + fore * (m_BuildPose[(size_t)foreBody].first - R.Elbow[side]);
		Q[foreBody] = glm::normalize(fore * m_BuildPose[(size_t)foreBody].second);
	};
	auto shoulderOf = [&](int side) { return P[9 + side] - turnOf(9 + side) * (m_BuildPose[(size_t)(9 + side)].first - R.Shoulder[side]); };
	// The trunk's own frame now (turned, leaning), for poses held against it.
	const glm::quat trunk = glm::normalize(turnOf(1));

	const float guardW = glm::smoothstep(0.0f, 1.0f, m_Guard);
	const float armLength = glm::length(R.Elbow[0] - R.Shoulder[0]) + glm::length(RestWrist(0) - R.Elbow[0]);
	// The shield up; the sword ready beside it, the hand at the waist in
	// front, the blade up and forward.
	{
		glm::vec3 wrist, pole;
		ArmamentGuardTarget(s, m_Recoil, wrist, pole);
		reach(1, trunk * wrist, trunk * pole, guardW);
		reach(0, trunk * glm::vec3(0.0f, -0.33f * s, 0.26f * s), trunk * glm::vec3(-1.0f, -0.6f, -0.4f), guardW);
	}
	if (swingW > 1e-4f)
	{
		glm::vec3 along(-std::sin(arc), 0.0f, std::cos(arc));
		reach(0, yaw * (along * (0.92f * armLength) + glm::vec3(0.0f, height, 0.0f)), yaw * glm::vec3(0.0f, -1.0f, -0.35f), swingW);
	}
	if (lungeW > 1e-4f)
	{
		glm::vec3 back = trunk * glm::vec3(0.0f, -0.25f * s, -0.04f * s);
		glm::vec3 out = trunk * glm::vec3(0.10f * s, -0.06f * s, 0.95f * armLength);
		glm::vec3 pole = glm::mix(trunk * glm::vec3(-1.0f, -0.2f, -0.6f), trunk * glm::vec3(0.0f, -1.0f, -0.2f), thrust);
		reach(0, glm::mix(back, out, thrust), pole, lungeW);
	}
	// Drawing or sheathing: the right hand across to the hilt at the left
	// hip, the left up behind its shoulder for the shield's strap, there at
	// the half-way mark and back -- the props change hands as they get there
	// (StowAmount).
	if (m_Stow >= 0.0f)
	{
		float w = std::sin(glm::pi<float>() * glm::clamp(m_Stow / kStowTime, 0.0f, 1.0f));
		glm::mat4 hip = ArmamentHipFrame(P[0], turnOf(0), s);
		reach(0, glm::vec3(hip[3]) - shoulderOf(0), yaw * glm::vec3(-1.0f, -0.3f, -0.5f), w);
		reach(1, trunk * glm::vec3(0.02f * s, 0.10f * s, -0.14f * s), trunk * glm::vec3(1.0f, 0.3f, 0.6f), w);
	}
}

// The grip: the fist just past the right wrist, +y along the blade -- the
// forearm's fold turned by the wrist's angle about the elbow's hinge (+x),
// so the blade's edges lie along the hinge, the way a cut moves.
inline glm::mat4 Humanoid::HandSwordFrame() const
{
	const float s = m_Proportions.Scale;
	const GS::RigidBody3D& forearm = m_World.GetBody(m_LowerArm[0]);
	const glm::quat& q = forearm.Orientation;
	glm::vec3 grip = forearm.Position + q * glm::vec3(0.0f, -0.5f * m_ForearmLength - 0.05f * s, 0.0f);
	return ArmamentFrame(grip, q * glm::vec3(0.0f, -std::sin(m_Wrist), std::cos(m_Wrist)), q * glm::vec3(1.0f, 0.0f, 0.0f));
}

// The shield's middle: on the strap side of the left forearm, a little
// towards the wrist, +y out of its face.
inline glm::mat4 Humanoid::HandShieldFrame() const
{
	const float s = m_Proportions.Scale;
	const GS::RigidBody3D& forearm = m_World.GetBody(m_LowerArm[1]);
	const glm::quat& q = forearm.Orientation;
	glm::vec3 y = glm::normalize(q * m_Strap);
	glm::vec3 at = forearm.Position + q * glm::vec3(0.0f, -0.03f * s, 0.0f) + y * (m_Proportions.ForearmRadius + 0.03f * s);
	return ArmamentFrame(at, y, q * glm::vec3(0.0f, 1.0f, 0.0f));
}

// Put away: the sword in its scabbard at the left hip (ArmamentHipFrame),
// the shield on his back between the shoulder blades, face out.
inline glm::mat4 Humanoid::StowedSwordFrame() const
{
	const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
	return ArmamentHipFrame(pelvis.Position, glm::normalize(pelvis.Orientation * glm::conjugate(m_BuildPose[0].second)), m_Proportions.Scale);
}

inline glm::mat4 Humanoid::StowedShieldFrame() const
{
	const float s = m_Proportions.Scale;
	const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
	glm::quat q = glm::normalize(torso.Orientation * glm::conjugate(m_BuildPose[1].second));
	glm::vec3 at = torso.Position + q * glm::vec3(0.0f, 0.03f * s, -(m_Proportions.TorsoHalf.z + 0.035f * s));
	return ArmamentFrame(at, q * glm::vec3(0.0f, 0.0f, -1.0f), q * glm::vec3(1.0f, 0.0f, 0.0f));
}

// 0 in his hands, 1 put away. Changing, they cross over while his hands
// are at the hilt and the strap -- the middle of the reach.
inline float Humanoid::StowAmount() const
{
	if (m_Stow < 0.0f)
		return m_Drawn ? 0.0f : 1.0f;
	float crossed = glm::smoothstep(0.15f, 0.35f, m_Stow);
	return m_Drawn ? 1.0f - crossed : crossed;
}

inline glm::mat4 ArmamentBlend(const glm::mat4& a, const glm::mat4& b, float t)
{
	if (t <= 0.0f) return a;
	if (t >= 1.0f) return b;
	glm::quat q = glm::slerp(glm::quat_cast(glm::mat3(a)), glm::quat_cast(glm::mat3(b)), t);
	glm::mat4 m = glm::mat4_cast(glm::normalize(q));
	m[3] = glm::vec4(glm::mix(glm::vec3(a[3]), glm::vec3(b[3]), t), 1.0f);
	return m;
}

inline glm::mat4 Humanoid::SwordFrame() const
{
	float away = StowAmount();
	return ArmamentBlend(away < 1.0f ? HandSwordFrame() : glm::mat4(1.0f), StowedSwordFrame(), away);
}

inline glm::mat4 Humanoid::ShieldFrame() const
{
	float away = StowAmount();
	return ArmamentBlend(away < 1.0f ? HandShieldFrame() : glm::mat4(1.0f), StowedShieldFrame(), away);
}

inline void Humanoid::SwordEdge(glm::vec3& hilt, glm::vec3& tip) const
{
	const float s = m_Proportions.Scale;
	glm::mat4 m = SwordFrame();
	glm::vec3 at(m[3]), along(m[1]);
	hilt = at + along * (0.08f * s);
	tip = at + along * ((0.08f + kSwordLength) * s);
}

inline Humanoid::WardResult Humanoid::WardOff(const glm::vec3& at, const glm::vec3& velocity, float radius, float mass, float dt)
{
	WardResult r;
	if (!Drawn() || m_Mode != Mode::Controlled)
		return r;
	float speed = glm::length(velocity);
	if (speed < 2.0f)
		return r;
	const float s = m_Proportions.Scale;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
	const glm::vec3 chest = m_World.GetBody(m_Torso).Position;
	const glm::vec3 in = velocity / speed;
	// How squarely it comes at his front: 1 head on, 0 from the side.
	glm::vec3 from(-in.x, 0.0f, -in.z);
	float front = glm::length(from) > 1e-4f ? glm::dot(glm::normalize(from), forward) : 0.0f;

	// When it will first be within `reach` of `centre`, along its line: 0 if
	// it is already in and coming closer, -1 if never. Within the next step
	// and a half counts as now, so it is met before a substep can carry it
	// into him.
	auto arrives = [&](const glm::vec3& centre, float reach)
	{
		glm::vec3 rel = at - centre;
		float a = glm::dot(velocity, velocity), b = 2.0f * glm::dot(rel, velocity), c = glm::dot(rel, rel) - reach * reach;
		if (c <= 0.0f)
			return b < 0.0f ? 0.0f : -1.0f;
		float disc = b * b - 4.0f * a * c;
		if (disc < 0.0f)
			return -1.0f;
		float t = (-b - std::sqrt(disc)) / (2.0f * a);
		return t >= 0.0f ? t : -1.0f;
	};

	// The sword first: a ball caught in the cut is struck, guard or no guard.
	// The cut sweeps a half circle in front of him, as far as the blade reaches.
	if (SwordCutting() && front > std::cos(glm::radians(100.0f)))
	{
		float t = arrives(chest + forward * (0.30f * s), 1.55f * s + radius);
		if (t >= 0.0f && t <= 1.5f * dt)
		{
			r.Kind = Ward::Struck;
			r.Point = at + velocity * t;
			// Back the way it came, a little faster and lifted: the thrower
			// is asked to aim it properly (Gunnery sends it at its cannon).
			r.Velocity = -velocity * 1.1f + up * 2.0f;
			return r;
		}
	}

	// The shield: anything coming at his front while it is up. The sphere is
	// his trunk and the shield in front of it, with room to spare.
	if (m_GuardHeld >= 0.0f && front > std::cos(glm::radians(80.0f)))
	{
		float t = arrives(chest + forward * (0.25f * s), 0.70f * s + radius);
		if (t >= 0.0f && t <= 1.5f * dt)
		{
			glm::mat4 shield = ShieldFrame();
			glm::vec3 face(shield[1]);
			if (glm::dot(face, in) > 0.0f)
				face = -face;
			glm::vec3 bounced = velocity - 2.0f * glm::dot(velocity, face) * face;
			r.Point = at + velocity * t;
			if (m_GuardHeld <= kParryWindow)
			{
				// Glanced up and off to his shield side, most of its speed
				// kept: away, and not back down the line at him.
				// (up x forward is his left, +x in his own frame.)
				glm::vec3 away = glm::normalize(glm::normalize(bounced) * 0.5f + up * 0.7f + glm::cross(up, forward) * 0.6f);
				r.Kind = Ward::Parried;
				r.Velocity = away * (0.8f * speed);
				m_RecoilRate += 0.8f;
			}
			else
			{
				// Stopped: it drops in front of him, and what it carried
				// shoves him back -- a third of it (the arm and the stance take
				// the rest), never past what his feet can catch.
				r.Kind = Ward::Blocked;
				r.Velocity = bounced * 0.25f + up * 1.5f;
				if (m_BodyMass > 0.0f)
				{
					glm::vec3 push(in.x, 0.0f, in.z);
					float along = glm::length(push);
					if (along > 1e-4f)
					{
						float shove = std::min(0.35f * mass * speed * along / m_BodyMass, 0.6f * CatchableSpeed());
						m_StaggerVelocity += push / along * shove;
					}
				}
				m_RecoilRate += 1.8f;
			}
			return r;
		}
	}
	return r;
}
