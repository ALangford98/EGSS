#pragma once

// Weapons, and what they do to a cannonball.
//
// The owner, 2026-10-22: give him a sword and shield to block, parry and
// reflect the cannonballs -- simple mechanics first. So three answers to a
// ball, each on the button a player expects:
//
//   * hold the guard: a ball that reaches it is BLOCKED -- dropped in front
//     of him, and he is shoved back by what the guard lets through
//     (kinematically, never past what his feet can catch);
//   * raise it no more than the parry window before the ball arrives and it
//     is PARRIED -- glanced up and away, no shove; with some weapons struck
//     back instead: the timing is what the reward is for;
//   * catch it in a strike and it is STRUCK -- sent back at whatever threw it
//     (the thrower aims it: Gunnery sends it at the cannon that fired it, and
//     a cannon has health).
//
// Forgiving on purpose. A ball is not met where the blade or the shield's
// rim actually is -- a sphere about him decides, the guard's a little bigger
// than his trunk and a strike's as far as its weapon reaches in front of him
// -- because a defence that fails on a two-centimetre miss reads as broken,
// not as hard. Whether the timing was right is the whole skill.
//
// Then (same day): under the guard he walks at a fraction of his walk, his
// chest on the aim, his legs turned towards where he goes or backpedalling
// (DriveLocomotion), a little lower and wider; attack from the guard is a
// lunge and from a run a long leaping one; what he carries is put away --
// half a second of reaching -- before any parkour (ParkourOn), and a press
// of attack or the guard draws it.
//
// Then (same day): loadouts, each with its own moveset -- sword and shield
// (the standard), longsword, katana, greatsword, twin daggers, a staff, his
// fists. A loadout is data (ArmamentKit): a combo of strikes, one from the
// guard, one from a run, a guard of its own and what its parry does. A
// strike is data too (ArmamentStrike): the hand's way from its shoulder at
// the start and the end of the cut, how far out, the shoulders' turn, the
// lean, the blade's angle, a surge of speed, the legs in the run's flight, a
// spin, how low -- so one piece of code poses all of them. The staff's
// strikes cast instead (bolts, a nova), for the game to make real.
//
// The props are drawn from his forearms, so they go wherever the arms go:
// held, striking, or limp in a fall. Nothing of them is simulated. Posed, the
// arms are put where the guard and the strike say over the stride's own
// (PoseArmament, called from DriveLocomotion before the bodies are placed).
//
// Included at the end of Humanoid.h: Humanoid's own members, as
// Locomotion.h's are.

// One strike. Angles are the hand's way from its shoulder in his own frame,
// degrees towards his right (about up, from ahead) and up; a left-hand
// strike is mirrored. Times in seconds from the press.
struct ArmamentStrike
{
	const char* Name = "";
	float Time = 0.40f, CutFrom = 0.05f, CutTo = 0.24f;
	int Hand = 0;                              // 0 right, 1 left, 2 both (two-handed, or both daggers)
	glm::vec2 From{ 80.0f, 5.0f }, To{ -58.0f, -10.0f };
	glm::vec2 Reach{ 0.92f, 0.92f };           // of the arm's length, at From and at To
	glm::vec2 Twist{ -0.35f, 0.40f };          // the shoulders, radians, rising towards his left
	float Lean = 0.0f;                         // the trunk forward at the cut, radians
	float Blade = 1.45f;                       // the blade's angle off the forearm's fold (pi/2: in line)
	float PoleOut = 0.0f;                      // 0 the elbow down and back, 1 out to the side (a hook)
	float Surge = 0.0f;                        // m/s at the cut's middle, a sine over it
	bool FromRun = false;                      // keeps the speed it began at under the surge
	bool Leap = false;                         // the legs in the run's moment of flight
	float Spin = 0.0f;                         // radians the whole of him turns over the cut
	float Dip = 0.0f;                          // m the hips go down for it
	float Damage = 25.0f;                      // to a cannon
	float Meets = 1.45f;                       // m: how far out a ball is met (0, none)
	int Cast = 0;                              // 1 a bolt, 2 a nova, at the start of the cut
};

// A loadout: its moveset, its guard, how it is carried.
struct ArmamentKit
{
	const char* Name = "";
	bool Stows = true;                         // something to put away (fists have not)
	bool TwoHands = false;                     // the left hand on the right's grip
	bool Shield = false;                       // a shield on the left forearm
	bool Dual = false;                         // a weapon in each hand
	bool Barrier = false;                      // the guard is a staff's barrier
	bool Aimed = false;                        // its casts go where the player aims (Control::AimPoint)
	float Blade = 0.65f;                       // m from just past the grip to the tip (fists: the knuckles)
	float GripSpacing = 0.0f;                  // m along the grip from the right hand to the left
	std::vector<ArmamentStrike> Combo;
	ArmamentStrike Guarded, Running;
	// The guard: each wrist from its shoulder in his trunk's frame (+x his
	// left, +z ahead), the elbow's way, the blade's angle -- for the hands it
	// holds; the other hand is the stride's (or on the grip).
	glm::vec3 GuardWrist[2], GuardPole[2];
	float GuardBlade[2] = { 0.75f, 0.75f };
	// Or the blade's way outright, in his trunk's frame, where the elbow's
	// bend cannot give it (zero: the angle above): the forearm turns and the
	// wrist bends for it, as a hand does.
	glm::vec3 GuardDir[2] = { glm::vec3(0.0f), glm::vec3(0.0f) };
	bool GuardHand[2] = { true, true };
	// Carried drawn and not guarding: the right hand held (a two-handed
	// weapon at the ready), the left on the grip or not; else the stride's.
	bool Carry = false, CarryTwoHands = false;
	glm::vec3 CarryWrist{ 0.0f }, CarryPole{ 0.0f, -1.0f, 0.0f };
	float CarryBlade = 0.6f;
	glm::vec3 CarryDir{ 0.0f };
	float IdleBlade[2] = { 0.75f, 0.75f };     // in a hand the stride swings
	float ParryWindow = 0.20f;                 // s
	bool ParryStrikes = false;                 // a parry strikes the ball back (else glances it away)
	float BlockShove = 0.35f;                  // of a blocked ball's momentum, into him
	float BlockReach = 0.70f;                  // m about a point ahead of the chest
	float GuardSpeed = 0.6f;                   // of his walk, guarding
};

inline const ArmamentKit& ArmamentKitOf(Humanoid::Loadout loadout)
{
	static const std::vector<ArmamentKit> kits = []()
	{
		auto strike = [](const char* name, float time, float from, float to, int hand, glm::vec2 a, glm::vec2 b)
		{
			ArmamentStrike k;
			k.Name = name;
			k.Time = time;
			k.CutFrom = from;
			k.CutTo = to;
			k.Hand = hand;
			k.From = a;
			k.To = b;
			return k;
		};
		// A thrust: the hand from low at the hip straight out at shoulder
		// height, the blade in line with the arm, the right shoulder driven
		// forward.
		auto thrust = [&](const char* name, float time, float from, float to, int hand)
		{
			ArmamentStrike k = strike(name, time, from, to, hand, { 0.0f, -75.0f }, { -6.0f, -4.0f });
			k.Reach = { 0.40f, 0.95f };
			k.Twist = { -0.30f, 0.45f };
			k.Blade = 1.57f;
			return k;
		};
		std::vector<ArmamentKit> out((size_t)Humanoid::Loadout::Count);

		// Sword and shield -- the standard. A short sword: forehand, backhand,
		// a falling diagonal; a lunge behind the shield; a leaping lunge.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::SwordShield];
			k.Name = "sword and shield";
			k.Shield = true;
			k.Blade = 0.65f;
			ArmamentStrike forehand = strike("forehand", 0.40f, 0.05f, 0.24f, 0, { 80.0f, 5.0f }, { -58.0f, -10.0f });
			ArmamentStrike backhand = strike("backhand", 0.40f, 0.05f, 0.24f, 0, { -55.0f, 0.0f }, { 80.0f, -10.0f });
			backhand.Twist = { 0.35f, -0.40f };
			ArmamentStrike diagonal = strike("falling cut", 0.45f, 0.08f, 0.26f, 0, { 45.0f, 55.0f }, { -35.0f, -35.0f });
			diagonal.Twist = { -0.25f, 0.35f };
			diagonal.Lean = 0.10f;
			diagonal.Damage = 30.0f;
			k.Combo = { forehand, backhand, diagonal };
			k.Guarded = thrust("shield lunge", 0.48f, 0.09f, 0.34f, 0);
			k.Guarded.Lean = 0.22f;
			k.Guarded.Surge = 8.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.10f;
			k.Guarded.Damage = 40.0f;
			k.Guarded.Meets = 1.55f;
			k.Running = k.Guarded;
			k.Running.Name = "running lunge";
			k.Running.Lean = 0.30f;
			k.Running.Surge = 4.0f;
			k.Running.FromRun = true;
			// The shield across the chest, the sword up beside it.
			k.GuardWrist[1] = { -0.30f, -0.10f, 0.33f };
			k.GuardPole[1] = { 1.0f, -0.35f, -0.25f };
			k.GuardWrist[0] = { 0.0f, -0.33f, 0.26f };
			k.GuardPole[0] = { -1.0f, -0.6f, -0.4f };
		}

		// Longsword: two hands, a long blade. A falling diagonal, a rising
		// one, an overhead cleave; a two-handed thrust; a leaping sweep.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Longsword];
			k.Name = "longsword";
			k.TwoHands = true;
			k.Blade = 1.0f;
			k.GripSpacing = 0.13f;
			ArmamentStrike fall = strike("falling cut", 0.50f, 0.10f, 0.30f, 2, { 60.0f, 60.0f }, { -50.0f, -35.0f });
			fall.Reach = { 0.85f, 0.85f };
			fall.Twist = { -0.40f, 0.40f };
			fall.Lean = 0.12f;
			fall.Blade = 1.40f;
			fall.Dip = 0.03f;
			fall.Damage = 35.0f;
			fall.Meets = 1.85f;
			ArmamentStrike rise = fall;
			rise.Name = "rising cut";
			rise.From = { -50.0f, -30.0f };
			rise.To = { 55.0f, 45.0f };
			rise.Twist = { 0.35f, -0.35f };
			rise.Lean = 0.0f;
			ArmamentStrike cleave = strike("cleave", 0.62f, 0.18f, 0.36f, 2, { 5.0f, 80.0f }, { 0.0f, -40.0f });
			cleave.Reach = { 0.80f, 0.90f };
			cleave.Twist = { -0.10f, 0.10f };
			cleave.Lean = 0.30f;
			cleave.Blade = 1.40f;
			cleave.Dip = 0.08f;
			cleave.Damage = 50.0f;
			cleave.Meets = 1.85f;
			k.Combo = { fall, rise, cleave };
			k.Guarded = thrust("thrust", 0.52f, 0.10f, 0.34f, 2);
			k.Guarded.From = { 0.0f, -60.0f };
			k.Guarded.Reach = { 0.45f, 0.92f };
			k.Guarded.Twist = { -0.20f, 0.35f };
			k.Guarded.Lean = 0.25f;
			k.Guarded.Surge = 8.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.10f;
			k.Guarded.Damage = 45.0f;
			k.Guarded.Meets = 1.90f;
			k.Running = strike("running sweep", 0.55f, 0.10f, 0.34f, 2, { 75.0f, 5.0f }, { -70.0f, -5.0f });
			k.Running.Reach = { 0.90f, 0.90f };
			k.Running.Twist = { -0.50f, 0.50f };
			k.Running.Lean = 0.20f;
			k.Running.Surge = 4.0f;
			k.Running.FromRun = true;
			k.Running.Leap = true;
			k.Running.Dip = 0.08f;
			k.Running.Damage = 45.0f;
			k.Running.Meets = 1.90f;
			// Both hands on the hilt in front, the blade up and forward.
			k.GuardWrist[0] = { 0.12f, -0.30f, 0.30f };
			k.GuardPole[0] = { -1.0f, -0.7f, -0.2f };
			k.GuardBlade[0] = 0.55f;
			k.GuardHand[1] = false;
			k.Carry = k.CarryTwoHands = true;
			k.CarryWrist = { 0.12f, -0.42f, 0.22f };
			k.CarryPole = { -1.0f, -1.0f, -0.3f };
			k.CarryBlade = 0.60f;
			k.BlockShove = 0.45f;
			k.BlockReach = 0.75f;
			k.GuardSpeed = 0.55f;
		}

		// Katana: two hands, fast. Four quick cuts; a dash-cut from the guard
		// and a longer one from a run. Its parry strikes the ball back.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Katana];
			k.Name = "katana";
			k.TwoHands = true;
			k.Blade = 0.75f;
			k.GripSpacing = 0.12f;
			auto cut = [&](const char* name, glm::vec2 a, glm::vec2 b, glm::vec2 twist)
			{
				ArmamentStrike c = strike(name, 0.32f, 0.04f, 0.18f, 2, a, b);
				c.Reach = { 0.90f, 0.90f };
				c.Twist = twist;
				c.Damage = 22.0f;
				c.Meets = 1.60f;
				return c;
			};
			ArmamentStrike stab = thrust("thrust", 0.34f, 0.06f, 0.20f, 2);
			stab.From = { 0.0f, -50.0f };
			stab.Reach = { 0.45f, 0.95f };
			stab.Twist = { -0.20f, 0.35f };
			stab.Lean = 0.15f;
			stab.Damage = 26.0f;
			stab.Meets = 1.60f;
			k.Combo = { cut("cut", { 70.0f, 10.0f }, { -60.0f, -5.0f }, { -0.35f, 0.35f }),
				cut("rising cut", { -55.0f, -25.0f }, { 60.0f, 40.0f }, { 0.30f, -0.30f }),
				cut("falling cut", { 55.0f, 55.0f }, { -50.0f, -30.0f }, { -0.30f, 0.30f }), stab };
			k.Guarded = strike("dash cut", 0.45f, 0.06f, 0.28f, 2, { 80.0f, 0.0f }, { -75.0f, -5.0f });
			k.Guarded.Reach = { 0.90f, 0.90f };
			k.Guarded.Twist = { -0.45f, 0.50f };
			k.Guarded.Lean = 0.15f;
			k.Guarded.Surge = 10.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.08f;
			k.Guarded.Damage = 45.0f;
			k.Guarded.Meets = 1.70f;
			k.Running = k.Guarded;
			k.Running.Name = "running dash cut";
			k.Running.Time = 0.50f;
			k.Running.CutTo = 0.32f;
			k.Running.Surge = 7.0f;
			k.Running.FromRun = true;
			k.GuardWrist[0] = { 0.12f, -0.30f, 0.30f };
			k.GuardPole[0] = { -1.0f, -0.7f, -0.2f };
			k.GuardBlade[0] = 0.50f;
			k.GuardHand[1] = false;
			k.Carry = k.CarryTwoHands = true;
			k.CarryWrist = { 0.12f, -0.42f, 0.22f };
			k.CarryPole = { -1.0f, -1.0f, -0.3f };
			k.CarryBlade = 0.55f;
			k.ParryWindow = 0.25f;
			k.ParryStrikes = true;
			k.BlockShove = 0.55f;
			k.GuardSpeed = 0.65f;
		}

		// Greatsword: two hands, slow and huge. A sweep, an overhead slam; a
		// heavy thrust; a leaping spin all the way round. Its guard takes
		// nearly everything.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Greatsword];
			k.Name = "greatsword";
			k.TwoHands = true;
			k.Blade = 1.35f;
			k.GripSpacing = 0.16f;
			ArmamentStrike sweep = strike("sweep", 0.78f, 0.26f, 0.50f, 2, { 100.0f, 5.0f }, { -90.0f, -5.0f });
			sweep.Reach = { 0.90f, 0.90f };
			sweep.Twist = { -0.60f, 0.60f };
			sweep.Lean = 0.10f;
			sweep.Blade = 1.40f;
			sweep.Dip = 0.06f;
			sweep.Damage = 60.0f;
			sweep.Meets = 2.20f;
			ArmamentStrike slam = strike("slam", 0.86f, 0.34f, 0.54f, 2, { 0.0f, 85.0f }, { 0.0f, -60.0f });
			slam.Reach = { 0.85f, 0.85f };
			slam.Twist = { -0.10f, 0.10f };
			slam.Lean = 0.40f;
			slam.Blade = 1.40f;
			slam.Dip = 0.14f;
			slam.Damage = 75.0f;
			slam.Meets = 2.20f;
			k.Combo = { sweep, slam };
			k.Guarded = thrust("heavy thrust", 0.60f, 0.14f, 0.38f, 2);
			k.Guarded.From = { 0.0f, -60.0f };
			k.Guarded.Reach = { 0.45f, 0.92f };
			k.Guarded.Lean = 0.30f;
			k.Guarded.Surge = 6.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.10f;
			k.Guarded.Damage = 60.0f;
			k.Guarded.Meets = 2.20f;
			// Arms out to his right, the whole of him turning once under them.
			k.Running = strike("spin", 0.80f, 0.12f, 0.62f, 2, { 70.0f, 0.0f }, { 60.0f, -5.0f });
			k.Running.Reach = { 0.90f, 0.90f };
			k.Running.Twist = { -0.20f, -0.10f };
			k.Running.Blade = 1.45f;
			k.Running.Surge = 3.0f;
			k.Running.FromRun = true;
			k.Running.Leap = true;
			k.Running.Spin = glm::two_pi<float>();
			k.Running.Dip = 0.08f;
			k.Running.Damage = 60.0f;
			k.Running.Meets = 2.20f;
			k.GuardWrist[0] = { 0.12f, -0.30f, 0.30f };
			k.GuardPole[0] = { -1.0f, -0.7f, -0.2f };
			k.GuardBlade[0] = 0.90f;
			k.GuardHand[1] = false;
			k.Carry = k.CarryTwoHands = true;
			k.CarryWrist = { 0.12f, -0.42f, 0.22f };
			k.CarryPole = { -1.0f, -1.0f, -0.3f };
			k.CarryBlade = 0.75f;
			k.ParryWindow = 0.15f;
			k.BlockShove = 0.15f;
			k.BlockReach = 0.80f;
			k.GuardSpeed = 0.45f;
		}

		// Twin daggers: one in each hand, very fast and short. Slash, slash,
		// stab, stab, hand after hand; a double stab from the guard; a dash
		// through from a run. Its parry strikes back.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Daggers];
			k.Name = "twin daggers";
			k.Dual = true;
			k.Blade = 0.26f;
			auto slash = [&](const char* name, int hand)
			{
				ArmamentStrike c = strike(name, 0.22f, 0.03f, 0.13f, hand, { 60.0f, 10.0f }, { -30.0f, -10.0f });
				c.Reach = { 0.90f, 0.90f };
				c.Twist = { -0.20f, 0.25f };
				c.Blade = 1.40f;
				c.Damage = 12.0f;
				c.Meets = 1.15f;
				return c;
			};
			auto stab = [&](const char* name, int hand)
			{
				ArmamentStrike c = thrust(name, 0.22f, 0.03f, 0.13f, hand);
				c.From = { 0.0f, -50.0f };
				c.To = { -8.0f, 0.0f };
				c.Twist = { -0.20f, 0.30f };
				c.Damage = 14.0f;
				c.Meets = 1.15f;
				return c;
			};
			k.Combo = { slash("right slash", 0), slash("left slash", 1), stab("right stab", 0), stab("left stab", 1) };
			k.Guarded = thrust("double stab", 0.42f, 0.08f, 0.30f, 2);
			k.Guarded.From = { 0.0f, -60.0f };
			k.Guarded.To = { -12.0f, -3.0f };
			k.Guarded.Twist = { 0.0f, 0.0f };
			k.Guarded.Lean = 0.20f;
			k.Guarded.Surge = 8.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.10f;
			k.Guarded.Damage = 25.0f;
			k.Guarded.Meets = 1.30f;
			k.Running = strike("dash through", 0.50f, 0.06f, 0.34f, 2, { -30.0f, 0.0f }, { 80.0f, -10.0f });
			k.Running.Twist = { 0.0f, 0.0f };
			k.Running.Lean = 0.20f;
			k.Running.Surge = 9.0f;
			k.Running.FromRun = true;
			k.Running.Leap = true;
			k.Running.Dip = 0.08f;
			k.Running.Damage = 25.0f;
			k.Running.Meets = 1.30f;
			// Crossed in front of the face.
			k.GuardWrist[0] = { 0.16f, -0.12f, 0.30f };
			k.GuardPole[0] = { -1.0f, -0.6f, -0.2f };
			k.GuardWrist[1] = { -0.16f, -0.12f, 0.30f };
			k.GuardPole[1] = { 1.0f, -0.6f, -0.2f };
			k.GuardBlade[0] = k.GuardBlade[1] = 0.20f;
			k.GuardDir[0] = { 0.55f, 1.0f, 0.25f };
			k.GuardDir[1] = { -0.55f, 1.0f, 0.25f };
			k.ParryWindow = 0.18f;
			k.ParryStrikes = true;
			k.BlockShove = 0.80f;
			k.BlockReach = 0.62f;
			k.GuardSpeed = 0.70f;
		}

		// Staff: casts. Attack throws a bolt where the player aims -- the
		// crosshair, aimed by hand (the owner, 2026-10-22: no aiming for him);
		// it shoots a ball down if it meets one. From the guard a nova throws
		// every ball near him back; from a run, a sweep of the staff. Its
		// guard is a barrier that swallows a ball, and its parry strikes the
		// ball back.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Staff];
			k.Name = "staff";
			k.TwoHands = true;
			k.Blade = 1.15f;
			k.GripSpacing = -0.45f;
			ArmamentStrike bolt = strike("bolt", 0.45f, 0.10f, 0.16f, 0, { 5.0f, 10.0f }, { 0.0f, 5.0f });
			bolt.Reach = { 0.60f, 0.95f };
			bolt.Twist = { -0.15f, 0.25f };
			bolt.Blade = 1.20f;
			bolt.Damage = 30.0f;
			bolt.Meets = 0.0f;
			bolt.Cast = 1;
			k.Combo = { bolt };
			k.Guarded = strike("nova", 0.60f, 0.18f, 0.24f, 2, { 0.0f, 60.0f }, { 0.0f, -70.0f });
			k.Guarded.Reach = { 0.80f, 0.80f };
			k.Guarded.Twist = { 0.0f, 0.0f };
			k.Guarded.Lean = 0.20f;
			k.Guarded.Blade = 1.50f;
			k.Guarded.Dip = 0.10f;
			k.Guarded.Damage = 30.0f;
			k.Guarded.Meets = 0.0f;
			k.Guarded.Cast = 2;
			k.Running = strike("staff sweep", 0.50f, 0.08f, 0.30f, 2, { 80.0f, 0.0f }, { -70.0f, -10.0f });
			k.Running.Reach = { 0.90f, 0.90f };
			k.Running.Twist = { -0.40f, 0.40f };
			k.Running.Blade = 1.50f;
			k.Running.Surge = 4.0f;
			k.Running.FromRun = true;
			k.Running.Leap = true;
			k.Running.Dip = 0.06f;
			k.Running.Damage = 20.0f;
			k.Running.Meets = 1.70f;
			// The staff held across in front, the barrier beyond it -- a way
			// the elbow's bend cannot point it (GuardDir): an upright staff
			// was what the bend gave.
			k.GuardWrist[0] = { 0.02f, -0.25f, 0.32f };
			k.GuardPole[0] = { -1.0f, -0.4f, -0.3f };
			k.GuardBlade[0] = 0.0f;
			k.GuardDir[0] = { 1.0f, 0.12f, 0.0f };
			k.GuardHand[1] = false;
			k.Carry = true;
			k.CarryTwoHands = false;
			k.CarryWrist = { -0.05f, -0.40f, 0.15f };
			k.CarryPole = { -1.0f, -1.0f, -0.2f };
			k.CarryBlade = 0.35f;
			k.CarryDir = { 0.0f, 1.0f, 0.12f };
			k.Barrier = true;
			k.Aimed = true;
			k.ParryWindow = 0.25f;
			k.ParryStrikes = true;
			k.BlockShove = 0.0f;
			k.BlockReach = 0.95f;
			k.GuardSpeed = 0.6f;
		}

		// Fists: jab, cross, hook, uppercut; a lunging punch from the guard,
		// a flying one from a run. A boxer's guard lets a lot through.
		{
			ArmamentKit& k = out[(size_t)Humanoid::Loadout::Unarmed];
			k.Name = "fists";
			k.Stows = false;
			k.Blade = 0.10f;
			auto punch = [&](const char* name, int hand, float twistTo, float damage)
			{
				ArmamentStrike c = strike(name, 0.24f, 0.05f, 0.13f, hand, { 0.0f, -30.0f }, { -8.0f, -2.0f });
				c.Reach = { 0.40f, 0.97f };
				c.Twist = { -0.10f, twistTo };
				c.Blade = 1.57f;
				c.Damage = damage;
				c.Meets = 1.05f;
				return c;
			};
			ArmamentStrike hook = punch("hook", 1, 0.35f, 12.0f);
			hook.From = { 70.0f, 0.0f };
			hook.To = { -30.0f, 0.0f };
			hook.Reach = { 0.70f, 0.75f };
			hook.Twist = { -0.30f, 0.35f };
			hook.PoleOut = 1.0f;
			ArmamentStrike upper = punch("uppercut", 0, 0.30f, 14.0f);
			upper.From = { 10.0f, -60.0f };
			upper.To = { 0.0f, 40.0f };
			upper.Reach = { 0.50f, 0.80f };
			upper.Twist = { -0.20f, 0.30f };
			k.Combo = { punch("jab", 1, 0.20f, 10.0f), punch("cross", 0, 0.35f, 12.0f), hook, upper };
			k.Guarded = punch("lunging punch", 0, 0.40f, 20.0f);
			k.Guarded.Time = 0.45f;
			k.Guarded.CutFrom = 0.08f;
			k.Guarded.CutTo = 0.30f;
			k.Guarded.Lean = 0.20f;
			k.Guarded.Surge = 8.0f;
			k.Guarded.Leap = true;
			k.Guarded.Dip = 0.08f;
			k.Guarded.Meets = 1.10f;
			k.Running = k.Guarded;
			k.Running.Name = "flying punch";
			k.Running.Surge = 5.0f;
			k.Running.FromRun = true;
			k.Running.Damage = 25.0f;
			k.GuardWrist[0] = { 0.10f, 0.0f, 0.22f };
			k.GuardPole[0] = { -1.0f, -1.0f, -0.3f };
			k.GuardWrist[1] = { -0.10f, 0.0f, 0.28f };
			k.GuardPole[1] = { 1.0f, -1.0f, -0.3f };
			k.ParryWindow = 0.15f;
			k.BlockShove = 0.90f;
			k.BlockReach = 0.65f;
			k.GuardSpeed = 0.70f;
		}
		return out;
	}();
	return kits[(size_t)loadout];
}

// What a prop is made of, in its own frame (origin at the grip, +y up the
// blade, or out of a shield's face): unit primitives, scaled.
struct ArmamentPart
{
	int Shape = 0;                             // 0 cube, 1 cylinder (along +y), 2 sphere
	glm::vec3 At{ 0.0f }, Size{ 1.0f };
	glm::vec4 Colour{ 1.0f };
	float Roughness = 0.5f, Metallic = 0.0f, Emission = 0.0f;
	float Tilt = 0.0f;                         // radians about the prop's z, towards -x (a katana's curve)
};

// A blade on a grip: blade, crossguard, grip, pommel -- each sword's own
// measurements, so they are told apart at a glance.
inline std::vector<ArmamentPart> ArmamentSword(float blade, float width, float guard, float grip, float pommel)
{
	const glm::vec4 steel(0.78f, 0.80f, 0.84f, 1.0f), iron(0.30f, 0.30f, 0.32f, 1.0f), leather(0.30f, 0.18f, 0.10f, 1.0f);
	return {
		{ 0, { 0.0f, 0.08f + 0.5f * blade, 0.0f }, { width, blade, 0.010f + 0.004f * width / 0.05f }, steel, 0.25f, 0.9f },
		{ 0, { 0.0f, 0.075f, 0.0f }, { guard, 0.025f, 0.035f }, iron, 0.4f, 0.8f },
		{ 1, { 0.0f, 0.06f - 0.5f * grip, 0.0f }, { 0.034f, grip, 0.034f }, leather, 0.8f, 0.0f },
		{ 2, { 0.0f, 0.05f - grip - 0.02f, 0.0f }, { pommel, pommel, pommel }, iron, 0.4f, 0.8f } };
}

inline std::vector<ArmamentPart> ArmamentProp(Humanoid::Loadout loadout, int hand)
{
	using L = Humanoid::Loadout;
	const glm::vec4 steel(0.78f, 0.80f, 0.84f, 1.0f), iron(0.30f, 0.30f, 0.32f, 1.0f), wood(0.52f, 0.33f, 0.17f, 1.0f);
	switch (loadout)
	{
		case L::SwordShield:
		{
			if (hand == 0)
				return ArmamentSword(0.65f, 0.045f, 0.18f, 0.14f, 0.05f);
			const float across = 0.62f;
			return {
				{ 1, { 0.0f, 0.0f, 0.0f }, { across, 0.03f, across }, wood, 0.75f, 0.0f },
				{ 1, { 0.0f, -0.002f, 0.0f }, { across + 0.025f, 0.022f, across + 0.025f }, iron, 0.45f, 0.7f },
				{ 2, { 0.0f, 0.02f, 0.0f }, { 0.13f, 0.07f, 0.13f }, iron, 0.35f, 0.8f } };
		}
		case L::Longsword:
			return hand == 0 ? ArmamentSword(1.0f, 0.05f, 0.26f, 0.26f, 0.055f) : std::vector<ArmamentPart>{};
		case L::Greatsword:
			return hand == 0 ? ArmamentSword(1.35f, 0.075f, 0.36f, 0.36f, 0.07f) : std::vector<ArmamentPart>{};
		case L::Katana:
		{
			if (hand != 0)
				return {};
			// Curved: three lengths of blade, each tilted a little further
			// towards its back (the edges face along x, the way a cut moves),
			// on a circle of 2.4 m.
			std::vector<ArmamentPart> parts;
			const float radius = 2.4f, length = 0.25f;
			for (int i = 0; i < 3; i++)
			{
				float a0 = (float)i * length / radius, a1 = (float)(i + 1) * length / radius, mid = 0.5f * (a0 + a1);
				glm::vec3 at(-radius * (1.0f - std::cos(mid)), 0.06f + radius * std::sin(mid), 0.0f);
				parts.push_back({ 0, at, { 0.032f, length + 0.004f, 0.009f }, steel, 0.2f, 0.9f, 0.0f, mid });
			}
			parts.push_back({ 1, { 0.0f, 0.05f, 0.0f }, { 0.085f, 0.012f, 0.085f }, iron, 0.4f, 0.8f });
			parts.push_back({ 1, { 0.0f, 0.04f - 0.135f, 0.0f }, { 0.032f, 0.27f, 0.032f }, glm::vec4(0.08f, 0.08f, 0.1f, 1.0f), 0.8f, 0.0f });
			return parts;
		}
		case L::Daggers:
			return ArmamentSword(0.26f, 0.03f, 0.08f, 0.10f, 0.035f);
		case L::Staff:
			if (hand != 0)
				return {};
			return {
				{ 1, { 0.0f, 0.30f, 0.0f }, { 0.04f, 1.75f, 0.04f }, wood, 0.8f, 0.0f },
				{ 1, { 0.0f, 1.15f, 0.0f }, { 0.06f, 0.05f, 0.06f }, iron, 0.4f, 0.8f },
				{ 2, { 0.0f, 1.24f, 0.0f }, { 0.12f, 0.12f, 0.12f }, glm::vec4(0.35f, 0.65f, 1.0f, 1.0f), 0.2f, 0.0f, 3.0f } };
		default:
			return {};
	}
}

// What carries a weapon put away: a scabbard (a sword's at the left hip, a
// long blade's across his back), a dagger's sheath at each hip.
inline std::vector<ArmamentPart> ArmamentHolder(Humanoid::Loadout loadout)
{
	using L = Humanoid::Loadout;
	const glm::vec4 leather(0.30f, 0.18f, 0.10f, 1.0f), dark(0.10f, 0.08f, 0.08f, 1.0f), iron(0.30f, 0.30f, 0.32f, 1.0f);
	float blade = 0.0f, width = 0.07f;
	glm::vec4 colour = leather;
	switch (loadout)
	{
		case L::SwordShield: blade = 0.65f; break;
		case L::Longsword: blade = 1.0f; width = 0.075f; break;
		case L::Greatsword: blade = 1.35f; width = 0.10f; break;
		case L::Katana: blade = 0.76f; width = 0.05f; colour = dark; break;
		case L::Daggers: blade = 0.26f; width = 0.05f; break;
		default: return {};
	}
	return {
		{ 0, { 0.0f, 0.08f + 0.5f * (blade + 0.02f), 0.0f }, { width, blade + 0.02f, 0.03f }, colour, 0.7f, 0.0f },
		{ 0, { 0.0f, 0.085f, 0.0f }, { width + 0.01f, 0.03f, 0.04f }, iron, 0.4f, 0.8f } };
}

inline const char* Humanoid::LoadoutName(Loadout loadout)
{
	return loadout < Loadout::Count ? ArmamentKitOf(loadout).Name : "";
}

inline const ArmamentKit& Humanoid::Kit() const { return ArmamentKitOf(m_Loadout); }

inline const ArmamentStrike* Humanoid::CurrentStrike() const
{
	if (!m_Armed || m_Strike < 0.0f)
		return nullptr;
	const ArmamentKit& k = Kit();
	switch (m_StrikeSlot)
	{
		case 0: return k.Combo.empty() ? nullptr : &k.Combo[(size_t)m_StrikeIndex % k.Combo.size()];
		case 1: return &k.Guarded;
		case 2: return &k.Running;
		default: return nullptr;
	}
}

inline void Humanoid::SetArmed(bool armed)
{
	m_Armed = armed;
	m_WantsAttack = m_WantsSheathe = false;
	m_Drawn = true;
	m_Stow = m_Strike = -1.0f;
	m_StrikeSlot = -1;
	m_StrikeQueued = false;
	m_PendingLoadout = -1;
	m_Guard = 0.0f;
	m_GuardHeld = -1.0f;
	m_Recoil = m_RecoilRate = 0.0f;
	m_Wrist[0] = Kit().IdleBlade[0];
	m_Wrist[1] = Kit().IdleBlade[1];
	m_Casts.clear();
}

// A new loadout: what he holds is put away first (half a second), then the
// new one drawn (another). Put away already, or bare-handed, the new one is
// drawn at once. Unarmed is a loadout too: nothing to draw.
inline void Humanoid::SetLoadout(Loadout loadout)
{
	if (loadout >= Loadout::Count)
		return;
	if (!m_Armed)
	{
		m_Loadout = loadout;
		return;
	}
	if (loadout == m_Loadout && m_PendingLoadout < 0)
		return;
	m_Strike = -1.0f;
	m_StrikeSlot = -1;
	m_StrikeQueued = false;
	bool holding = Kit().Stows && m_Drawn;
	if (m_Stow >= 0.0f || (holding && m_Mode == Mode::Controlled))
	{
		m_PendingLoadout = (int)loadout;
		if (m_Stow < 0.0f)
		{
			m_Stow = 0.0f;
			m_Drawn = false;
		}
		return;
	}
	m_Loadout = loadout;
	m_PendingLoadout = -1;
	m_Drawn = true;
	if (Kit().Stows && m_Mode == Mode::Controlled)
		m_Stow = 0.0f;
}

inline bool Humanoid::ParkourOn() const
{
	return m_Parkour && !(m_Armed && Kit().Stows && (m_Drawn || m_Stow >= 0.0f));
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

inline void Humanoid::BeginStrike(int slot, int index)
{
	m_StrikeSlot = slot;
	m_StrikeIndex = index;
	m_Strike = 0.0f;
	m_StrikeBase = 0.0f;
	m_StrikeQueued = false;
	m_Cast = false;
	m_Swings++;
	if (const ArmamentStrike* k = CurrentStrike())
		m_StrikeBase = k->FromRun ? m_Speed : 0.0f;
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

	// Drawing and putting away: half a second of reaching, to where each
	// weapon is carried. Asked for, or by reaching for the guard or a strike
	// while it is away. Anything that takes him out of the walk finishes it
	// at once -- he does not fall with a hand on the hilt. A loadout waiting
	// (SetLoadout) is taken up once the old one is away, and drawn.
	auto beginStow = [&]()
	{
		m_Stow = 0.0f;
		m_Drawn = !m_Drawn;
		m_Strike = -1.0f;
		m_StrikeSlot = -1;
		m_StrikeQueued = false;
	};
	if (m_WantsSheathe)
	{
		m_WantsSheathe = false;
		if (posed && Kit().Stows && m_Stow < 0.0f && m_Strike < 0.0f)
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
		{
			m_Stow = -1.0f;
			// Asked for another while this one was being drawn: it is put
			// away first -- swapped as the draw ended, it vanished from his
			// hands.
			if (m_PendingLoadout >= 0 && m_Drawn && Kit().Stows && posed)
				beginStow();
			else if (m_PendingLoadout >= 0)
			{
				m_Loadout = (Loadout)m_PendingLoadout;
				m_PendingLoadout = -1;
				if (Kit().Stows && posed)
					beginStow();
				else
					m_Drawn = true;
			}
		}
	}

	const ArmamentKit& kit = Kit();
	const bool guard = posed && m_Control.Enabled && m_Control.Guard && Drawn();
	// Up in under a tenth of a second -- the guard is a reaction -- and down
	// a little slower.
	m_Guard = glm::clamp(m_Guard + (guard ? dt / 0.08f : -dt / 0.15f), 0.0f, 1.0f);
	m_GuardHeld = guard ? (m_GuardHeld < 0.0f ? 0.0f : m_GuardHeld + dt) : -1.0f;
	Spring(m_Recoil, m_RecoilRate, 0.0f, 16.0f, dt);
	m_Recoil = glm::clamp(m_Recoil, -0.05f * s, 0.16f * s);

	// A press starts a strike: from the guard its lunge, at a run its running
	// attack, else the next of its combo. One late in a combo strike is held
	// for the next; an early one is dropped -- mashing should not lock him
	// into a string of them -- and one soon after a combo strike ends still
	// carries it on.
	m_ComboGrace = std::max(m_ComboGrace - dt, 0.0f);
	const int combo = std::max((int)kit.Combo.size(), 1);
	if (m_WantsAttack)
	{
		m_WantsAttack = false;
		if (posed && Drawn())
		{
			const ArmamentStrike* now = CurrentStrike();
			if (!now)
			{
				bool running = m_Air == Air::Grounded && m_Speed > 1.25f * m_WalkSpeed;
				if (guard && m_Air == Air::Grounded)
					BeginStrike(1, 0);
				else if (running && !guard)
					BeginStrike(2, 0);
				else
					BeginStrike(0, m_ComboGrace > 0.0f ? (m_StrikeIndex + 1) % combo : 0);
			}
			else if (m_StrikeSlot == 0 && m_Strike > 0.4f * now->Time)
				m_StrikeQueued = true;
		}
	}
	if (const ArmamentStrike* now = CurrentStrike())
	{
		if (!posed)
		{
			m_Strike = -1.0f;
			m_StrikeSlot = -1;
			m_StrikeQueued = false;
		}
		else
		{
			m_Strike += dt;
			// A staff's cast goes as its cut begins, from the staff's head.
			if (now->Cast && !m_Cast && m_Strike >= now->CutFrom)
			{
				m_Cast = true;
				Cast c;
				c.Nova = now->Cast == 2;
				c.Damage = now->Damage;
				glm::mat4 staff = PropFrame(0);
				c.At = c.Nova ? m_World.GetBody(m_Pelvis).Position : glm::vec3(staff[3]) + glm::vec3(staff[1]) * (1.24f * s);
				c.Direction = glm::vec3(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
				// From the staff's head to what is under the crosshair: the
				// camera sees past his shoulder, so its ray and the bolt's
				// cross at the point aimed at, not along the way.
				if (Kit().Aimed && m_Control.HasAimPoint && glm::length(m_Control.AimPoint - c.At) > 0.5f)
					c.Direction = glm::normalize(m_Control.AimPoint - c.At);
				m_Casts.push_back(c);
				if (m_Casts.size() > 8)
					m_Casts.erase(m_Casts.begin());
			}
			if (m_StrikeQueued && m_StrikeSlot == 0 && m_Strike >= 0.7f * now->Time)
				BeginStrike(0, (m_StrikeIndex + 1) % combo);
			else if (m_Strike >= now->Time)
			{
				m_ComboGrace = m_StrikeSlot == 0 ? 0.35f : 0.0f;
				m_Strike = -1.0f;
				m_StrikeSlot = -1;
			}
		}
	}

	// The wrists: each blade at the angle its strike asks (nearly in line
	// with the forearm for a cut, so arm and blade sweep as one lever,
	// exactly in line for a thrust), its guard's, its carry's, or the
	// stride's -- a sword carried 43 degrees down out of the fist: at 20 he
	// carried it like a lance, and at 43 the tip stays 28 cm off the floor
	// walking and running, measured.
	const ArmamentStrike* now = CurrentStrike();
	for (int hand = 0; hand < 2; hand++)
	{
		float want = kit.IdleBlade[hand];
		bool striking = now && (now->Hand == hand || now->Hand == 2) && m_Strike < 0.75f * now->Time;
		if (striking)
			want = now->Blade;
		else if (m_Guard > 0.5f && kit.GuardHand[hand])
			want = kit.GuardBlade[hand];
		else if (kit.Carry && hand == 0)
			want = kit.CarryBlade;
		float most = 25.0f * dt;
		m_Wrist[hand] += glm::clamp(want - m_Wrist[hand], -most, most);
	}

	// The shield's strap: worked out so that, guarding, its face is straight
	// ahead -- from the guard's own solution in the frame he was built in.
	// Strapped there, it stays on his forearm wherever the arm goes.
	if (kit.Shield && m_BuildPose.size() >= 13)
	{
		glm::quat upper, fore;
		ArmTurns(1, m_Rest.Shoulder[1], m_Rest.Shoulder[1] + kit.GuardWrist[1] * s, kit.GuardPole[1], upper, fore);
		glm::quat Q = glm::normalize(fore * m_BuildPose[12].second);
		glm::vec3 face = glm::conjugate(Q) * glm::vec3(0.0f, 0.0f, 1.0f);
		face.y = 0.0f;
		if (glm::length(face) > 1e-4f)
			m_Strap = glm::normalize(face);
	}
}

// --- what a strike asks of the locomotion ----------------------------------

// A surge drives him itself: straight along his facing, at what he was going
// (a running attack) plus a sine over the cut.
inline bool Humanoid::StrikeSurge(float& speed) const
{
	const ArmamentStrike* k = CurrentStrike();
	if (!k || k->Surge <= 0.0f)
		return false;
	float u = glm::clamp((m_Strike - k->CutFrom) / std::max(k->CutTo - k->CutFrom, 1e-3f), 0.0f, 1.0f);
	float surge = m_Strike < k->CutFrom || u >= 1.0f ? 0.0f : std::sin(glm::pi<float>() * u);
	float base = m_Strike < k->CutTo ? m_StrikeBase : std::min(speed, m_StrikeBase);
	speed = base + k->Surge * std::sqrt(m_Proportions.Scale) * surge;
	return true;
}

// How far into and out of a strike he is: in over its wind-up, out over its
// recovery.
inline float ArmamentWeight(const ArmamentStrike& k, float t)
{
	return glm::smoothstep(0.0f, std::max(k.CutFrom, 0.03f), t) * (1.0f - glm::smoothstep(k.CutTo + 0.02f, k.Time, t));
}

inline float Humanoid::StrikeLeap() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k && k->Leap ? ArmamentWeight(*k, m_Strike) : 0.0f;
}

inline float Humanoid::StrikeSpin() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k && k->Spin != 0.0f ? k->Spin * glm::smoothstep(k->CutFrom, k->CutTo, m_Strike) : 0.0f;
}

inline float Humanoid::StrikeDip() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k ? k->Dip * ArmamentWeight(*k, m_Strike) : 0.0f;
}

inline float Humanoid::GuardSpeed() const { return Kit().GuardSpeed; }

inline bool Humanoid::AimsByHand() const { return m_Armed && Kit().Aimed; }

inline bool Humanoid::SwordCutting() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k && m_Strike >= k->CutFrom && m_Strike <= k->CutTo;
}

inline float Humanoid::StrikeDamage() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k ? k->Damage : 0.0f;
}

inline const char* Humanoid::StrikeName() const
{
	const ArmamentStrike* k = CurrentStrike();
	return k ? k->Name : "";
}

// Which hands a strike moves: its own, or for "both" the right (a two-handed
// weapon's left follows onto the grip) -- both daggers, both fists.
inline bool ArmamentMoves(const ArmamentStrike& k, const ArmamentKit& kit, int side)
{
	return k.Hand == side || (k.Hand == 2 && (side == 0 || kit.Dual || (!kit.TwoHands && !kit.Shield)));
}

// --- where things are -------------------------------------------------------

inline glm::mat4 ArmamentFrame(const glm::vec3& at, const glm::vec3& y, const glm::vec3& xHint)
{
	glm::vec3 x = xHint - y * glm::dot(xHint, y);
	if (glm::length(x) < 1e-5f)
		x = glm::cross(y, std::fabs(y.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
	x = glm::normalize(x);
	glm::mat4 m(1.0f);
	m[0] = glm::vec4(x, 0.0f);
	m[1] = glm::vec4(y, 0.0f);
	m[2] = glm::vec4(glm::cross(x, y), 0.0f);
	m[3] = glm::vec4(at, 1.0f);
	return m;
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

// A hand's weapon, from where its forearm is: the grip is the fist just past
// the wrist, +y along the blade -- the forearm's fold turned by the wrist's
// angle about the elbow's hinge (+x), so a blade's edges lie along the
// hinge, the way a cut moves. A shield instead: its middle on the strap side
// of the forearm, +y out of its face.
inline glm::mat4 Humanoid::HandFrame(int hand, const glm::vec3& forearmAt, const glm::quat& q) const
{
	const float s = m_Proportions.Scale;
	if (hand == 1 && Kit().Shield)
	{
		glm::vec3 y = glm::normalize(q * m_Strap);
		glm::vec3 at = forearmAt + q * glm::vec3(0.0f, -0.03f * s, 0.0f) + y * (m_Proportions.ForearmRadius + 0.03f * s);
		return ArmamentFrame(at, y, q * glm::vec3(0.0f, 1.0f, 0.0f));
	}
	glm::vec3 grip = forearmAt + q * glm::vec3(0.0f, -0.5f * m_ForearmLength - 0.05f * s, 0.0f);
	return ArmamentFrame(grip, glm::normalize(q * m_BladeLocal[hand]), q * glm::vec3(1.0f, 0.0f, 0.0f));
}

// Where each weapon is carried put away, from the pelvis and the trunk as
// they were built to face (+x his left): a sword at the left hip, the blade
// down and back 42 degrees below level -- at 29 the scabbard stuck out
// behind him like a tail, at 55 the run's forward-pitched hips swung its
// tip to the floor; a katana flatter, as it is worn; a long blade across his
// back, the hilt over the right shoulder; a dagger at each hip; the staff
// slung across his back; the shield between his shoulder blades.
inline glm::mat4 ArmamentStowed(Humanoid::Loadout loadout, int hand, const glm::vec3& pelvis, const glm::quat& p,
	const glm::vec3& torso, const glm::quat& t, float s, float torsoDepth)
{
	using L = Humanoid::Loadout;
	glm::vec3 up = p * glm::vec3(0.0f, 1.0f, 0.0f);
	switch (loadout)
	{
		case L::SwordShield:
			if (hand == 1)
				return ArmamentFrame(torso + t * glm::vec3(0.0f, 0.03f * s, -(torsoDepth + 0.035f * s)), t * glm::vec3(0.0f, 0.0f, -1.0f), t * glm::vec3(1.0f, 0.0f, 0.0f));
			return ArmamentFrame(pelvis + p * glm::vec3(0.20f * s, 0.03f * s, 0.10f * s), glm::normalize(p * glm::vec3(0.10f, -0.90f, -1.0f)), up);
		case L::Katana:
			return ArmamentFrame(pelvis + p * glm::vec3(0.20f * s, 0.04f * s, 0.12f * s), glm::normalize(p * glm::vec3(0.10f, -0.60f, -1.0f)), up);
		case L::Longsword:
		case L::Greatsword:
		{
			float lift = loadout == L::Greatsword ? 0.36f : 0.30f;
			glm::vec3 at = torso + t * glm::vec3(-0.12f * s, lift * s, -(torsoDepth + 0.06f * s));
			return ArmamentFrame(at, glm::normalize(t * glm::vec3(0.40f, -1.0f, 0.0f)), t * glm::vec3(0.0f, 0.0f, -1.0f));
		}
		case L::Daggers:
		{
			float side = hand == 1 ? 1.0f : -1.0f;
			return ArmamentFrame(pelvis + p * glm::vec3(0.19f * s * side, 0.0f, 0.04f * s), glm::normalize(p * glm::vec3(0.05f * side, -1.0f, -0.25f)), p * glm::vec3(0.0f, 0.0f, 1.0f));
		}
		case L::Staff:
		{
			glm::vec3 along = glm::normalize(t * glm::vec3(-0.60f, 1.0f, 0.0f));
			return ArmamentFrame(torso + t * glm::vec3(0.0f, 0.0f, -(torsoDepth + 0.05f * s)) - along * (0.30f * s), along, t * glm::vec3(0.0f, 0.0f, -1.0f));
		}
		default:
			return ArmamentFrame(pelvis, up, p * glm::vec3(1.0f, 0.0f, 0.0f));
	}
}

inline glm::mat4 Humanoid::StowedFrame(int hand) const
{
	const GS::RigidBody3D& pelvis = m_World.GetBody(m_Pelvis);
	const GS::RigidBody3D& torso = m_World.GetBody(m_Torso);
	return ArmamentStowed(m_Loadout, hand, pelvis.Position, glm::normalize(pelvis.Orientation * glm::conjugate(m_BuildPose[0].second)),
		torso.Position, glm::normalize(torso.Orientation * glm::conjugate(m_BuildPose[1].second)), m_Proportions.Scale, m_Proportions.TorsoHalf.z);
}

// 0 in his hands, 1 put away. Changing, they cross over while his hands
// are at the hilt and the strap -- the middle of the reach.
inline float Humanoid::StowAmount() const
{
	if (!Kit().Stows)
		return 0.0f;
	if (m_Stow < 0.0f)
		return m_Drawn ? 0.0f : 1.0f;
	float crossed = glm::smoothstep(0.15f, 0.35f, m_Stow);
	return m_Drawn ? 1.0f - crossed : crossed;
}

inline glm::mat4 Humanoid::PropFrame(int hand) const
{
	float away = StowAmount();
	const GS::RigidBody3D& forearm = m_World.GetBody(m_LowerArm[hand]);
	glm::mat4 held = away < 1.0f ? HandFrame(hand, forearm.Position, forearm.Orientation) : glm::mat4(1.0f);
	return away > 0.0f ? ArmamentBlend(held, StowedFrame(hand), away) : held;
}

inline int Humanoid::HolderCount() const
{
	if (!m_Armed)
		return 0;
	switch (m_Loadout)
	{
		case Loadout::Daggers: return 2;
		case Loadout::Staff: case Loadout::Unarmed: return 0;
		default: return 1;
	}
}

inline glm::mat4 Humanoid::HolderFrame(int index) const { return StowedFrame(index); }

// The staff's barrier: a disc of light ahead of his chest while the guard is
// up, growing with it.
inline bool Humanoid::BarrierFrame(glm::mat4& frame) const
{
	if (!m_Armed || !Kit().Barrier || m_Guard <= 0.01f)
		return false;
	const float s = m_Proportions.Scale;
	glm::vec3 forward(std::sin(m_Facing), 0.0f, std::cos(m_Facing));
	glm::vec3 chest = m_World.GetBody(m_Torso).Position;
	frame = ArmamentFrame(chest + forward * (0.55f * s - m_Recoil), forward, glm::vec3(0.0f, 1.0f, 0.0f));
	frame = frame * glm::scale(glm::mat4(1.0f), glm::vec3(glm::smoothstep(0.0f, 1.0f, m_Guard)));
	return true;
}

// The edges of the strike that is cutting: a blade from just past the grip
// to its tip, or a fist's knuckles.
inline int Humanoid::StrikeEdges(glm::vec3 hilt[2], glm::vec3 tip[2]) const
{
	const ArmamentStrike* k = CurrentStrike();
	if (!k || !SwordCutting())
		return 0;
	const ArmamentKit& kit = Kit();
	const float s = m_Proportions.Scale;
	int count = 0;
	for (int hand = 0; hand < 2; hand++)
	{
		if (!ArmamentMoves(*k, kit, hand))
			continue;
		const GS::RigidBody3D& forearm = m_World.GetBody(m_LowerArm[hand]);
		if (!kit.Stows)
		{
			glm::vec3 along = forearm.Orientation * glm::vec3(0.0f, -1.0f, 0.0f);
			glm::vec3 wrist = forearm.Position + along * (0.5f * m_ForearmLength);
			hilt[count] = wrist;
			tip[count] = wrist + along * (kit.Blade * s);
		}
		else
		{
			glm::mat4 m = HandFrame(hand, forearm.Position, forearm.Orientation);
			glm::vec3 at(m[3]), along(m[1]);
			hilt[count] = at + along * (0.08f * s);
			tip[count] = at + along * ((0.08f + kit.Blade) * s);
		}
		count++;
	}
	return count;
}

// --- posing ------------------------------------------------------------------

// The carry, the guard, the strike and the reach to put away or draw, over
// the arms the stride posed: each laid over what is there, blended in by how
// far into it he is, so nothing snaps an arm. A two-handed weapon's left hand
// goes last, onto the grip wherever the right hand put it.
inline void Humanoid::PoseArmament(glm::vec3* P, glm::quat* Q, float dt)
{
	(void)dt;
	if (!m_Armed || m_BuildPose.size() < 13)
		return;
	const ArmamentKit& kit = Kit();
	const float s = m_Proportions.Scale;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::quat yaw = glm::angleAxis(m_Facing + StrikeSpin(), up);
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

	// The strike: how far into it, where along its cut, the shoulders' turn
	// (a left-hand strike's mirrored) and the lean.
	const ArmamentStrike* k = CurrentStrike();
	float strikeW = 0.0f, along = 0.0f;
	if (k)
	{
		strikeW = ArmamentWeight(*k, m_Strike);
		along = glm::smoothstep(k->CutFrom, k->CutTo, m_Strike);
		float mirror = k->Hand == 1 ? -1.0f : 1.0f;
		float twist = mirror * glm::mix(k->Twist.x, k->Twist.y, along) * strikeW;
		if (std::fabs(twist) > 1e-4f)
			turnTrunk(glm::angleAxis(twist, up), false);
		float lean = k->Lean * glm::smoothstep(k->CutFrom * 0.5f, k->CutTo, m_Strike) * strikeW;
		if (std::fabs(lean) > 1e-4f)
			turnTrunk(glm::angleAxis(lean, yaw * glm::vec3(1.0f, 0.0f, 0.0f)), false);
	}

	// One arm towards a wrist (from its shoulder) and an elbow's way, by `w`,
	// over what it has.
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
	const float drawnW = 1.0f - StowAmount();
	const glm::vec3 back(0.0f, 0.0f, -m_Recoil / std::max(s, 1e-3f));

	// Carried at the ready (two-handed weapons, the staff upright).
	if (kit.Carry)
		reach(0, trunk * (kit.CarryWrist * s), trunk * kit.CarryPole, drawnW * (1.0f - guardW));
	// The guard, each hand that holds one.
	for (int side = 0; side < 2; side++)
		if (kit.GuardHand[side])
			reach(side, trunk * ((kit.GuardWrist[side] + back) * s), trunk * kit.GuardPole[side], guardW);
	// The strike: the hand from its shoulder along an arc from its start to its
	// end -- both hands for both daggers, the right for a two-handed weapon
	// (the left follows onto the grip below).
	if (k && strikeW > 1e-4f)
	{
		const float armLength = glm::length(R.Elbow[0] - R.Shoulder[0]) + glm::length(RestWrist(0) - R.Elbow[0]);
		auto way = [](glm::vec2 deg, float mirror)
		{
			float a = glm::radians(deg.x), e = glm::radians(deg.y);
			return glm::vec3(-std::sin(a) * std::cos(e) * mirror, std::sin(e), std::cos(a) * std::cos(e));
		};
		for (int side = 0; side < 2; side++)
		{
			if (!ArmamentMoves(*k, kit, side))
				continue;
			float mirror = side == 1 ? -1.0f : 1.0f;
			glm::vec3 a = way(k->From, mirror), b = way(k->To, mirror);
			float angle = std::acos(glm::clamp(glm::dot(a, b), -1.0f, 1.0f));
			glm::vec3 dir = angle > 1e-3f ? (std::sin((1.0f - along) * angle) * a + std::sin(along * angle) * b) / std::sin(angle) : a;
			float out = glm::mix(k->Reach.x, k->Reach.y, along) * armLength;
			glm::vec3 pole = glm::mix(glm::vec3(0.0f, -1.0f, -0.35f), glm::vec3(-mirror, 0.2f, -0.3f), k->PoleOut);
			reach(side, yaw * (dir * out), yaw * pole, strikeW);
		}
	}
	// Drawing or putting away: each hand to where its weapon is carried --
	// a hilt at the hip or over the shoulder, a shield's strap behind the
	// shoulder -- there at the half-way mark and back; the props change hands
	// as they get there (StowAmount).
	float stowW = 0.0f;
	if (m_Stow >= 0.0f && kit.Stows)
	{
		stowW = std::sin(glm::pi<float>() * glm::clamp(m_Stow / kStowTime, 0.0f, 1.0f));
		const glm::quat pelvisTurn = turnOf(0);
		for (int side = 0; side < 2; side++)
		{
			if (side == 1 && !kit.Shield && !kit.Dual)
				continue;
			glm::mat4 at = ArmamentStowed(m_Loadout, side, P[0], pelvisTurn, P[1], trunk, s, m_Proportions.TorsoHalf.z);
			glm::vec3 target = glm::vec3(at[3]) - shoulderOf(side);
			if (side == 1 && kit.Shield)
				target = trunk * (glm::vec3(0.02f, 0.10f, -0.14f) * s);
			float mirror = side == 1 ? -1.0f : 1.0f;
			reach(side, target, yaw * glm::vec3(-mirror, -0.3f, -0.5f), stowW);
		}
	}
	// Each blade's way: the wrist's angle off the forearm's fold (the
	// strike's, the guard's, the carry's -- UpdateArmament eases it), or
	// where a pose says outright, from the forearm as it now is.
	for (int side = 0; side < 2; side++)
	{
		glm::vec3 local(0.0f, -std::sin(m_Wrist[side]), std::cos(m_Wrist[side]));
		float striking = k && ArmamentMoves(*k, kit, side) ? strikeW : 0.0f;
		const glm::quat fore = turnOf(11 + side) * m_BuildPose[(size_t)(11 + side)].second;
		auto towards = [&](const glm::vec3& trunkDir, float w)
		{
			if (w < 1e-4f || glm::length(trunkDir) < 1e-4f)
				return;
			glm::vec3 want = glm::normalize(glm::conjugate(fore) * (trunk * glm::normalize(trunkDir)));
			local = glm::normalize(glm::mix(local, want, w));
		};
		if (side == 0 && kit.Carry)
			towards(kit.CarryDir, drawnW * (1.0f - guardW) * (1.0f - striking));
		towards(kit.GuardDir[side], guardW * (1.0f - striking));
		m_BladeLocal[side] = local;
	}

	// Two hands on one grip: the left wherever the right's grip is.
	if (kit.TwoHands)
	{
		float grip = kit.CarryTwoHands ? drawnW : guardW;
		if (k && k->Hand == 2)
			grip = std::max(grip, strikeW);
		grip = std::min(grip, drawnW) * (1.0f - stowW);
		if (grip > 1e-4f)
		{
			glm::mat4 right = HandFrame(0, P[11], Q[11]);
			glm::vec3 at = glm::vec3(right[3]) - glm::vec3(right[1]) * (kit.GripSpacing * s);
			reach(1, at - shoulderOf(1), trunk * glm::vec3(1.0f, -0.8f, -0.3f), grip);
		}
	}
}

// --- what reaches him -------------------------------------------------------

inline Humanoid::WardResult Humanoid::WardOff(const glm::vec3& at, const glm::vec3& velocity, float radius, float mass, float dt)
{
	WardResult r;
	if (!Drawn() || m_Mode != Mode::Controlled)
		return r;
	float speed = glm::length(velocity);
	if (speed < 2.0f)
		return r;
	const ArmamentKit& kit = Kit();
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

	// A strike first: a ball caught in it is struck, guard or no guard. A
	// strike meets a half circle in front of him, as far as it reaches -- all
	// the way round, spinning.
	const ArmamentStrike* k = CurrentStrike();
	if (k && SwordCutting() && k->Meets > 0.0f && (k->Spin != 0.0f || front > std::cos(glm::radians(100.0f))))
	{
		glm::vec3 centre = k->Spin != 0.0f ? chest : chest + forward * (0.30f * s);
		float t = arrives(centre, k->Meets * s + radius);
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

	// The guard: anything coming at his front while it is up. The sphere is
	// his trunk and the guard in front of it, with room to spare (a barrier's
	// is wider).
	if (m_GuardHeld >= 0.0f && front > std::cos(glm::radians(kit.Barrier ? 90.0f : 80.0f)))
	{
		float t = arrives(chest + forward * (0.25f * s), kit.BlockReach * s + radius);
		if (t >= 0.0f && t <= 1.5f * dt)
		{
			glm::vec3 face = forward;
			if (kit.Shield)
				face = glm::vec3(PropFrame(1)[1]);
			if (glm::dot(face, in) > 0.0f)
				face = -face;
			glm::vec3 bounced = velocity - 2.0f * glm::dot(velocity, face) * face;
			r.Point = at + velocity * t;
			if (m_GuardHeld <= kit.ParryWindow)
			{
				if (kit.ParryStrikes)
				{
					// Met on the moment and sent back: the thrower aims it.
					r.Kind = Ward::Struck;
					r.Velocity = -velocity * 1.1f + up * 2.0f;
				}
				else
				{
					// Glanced up and off to his left, most of its speed kept:
					// away, and not back down the line at him. (Up x forward
					// is his left, +x in his own frame.)
					glm::vec3 away = glm::normalize(glm::normalize(bounced) * 0.5f + up * 0.7f + glm::cross(up, forward) * 0.6f);
					r.Kind = Ward::Parried;
					r.Velocity = away * (0.8f * speed);
				}
				m_RecoilRate += 0.8f;
			}
			else
			{
				// Stopped: it drops in front of him (a barrier swallows it),
				// and what it carried shoves him back -- the share the guard
				// lets through, never past what his feet can catch.
				r.Kind = Ward::Blocked;
				r.Velocity = kit.Barrier ? bounced * 0.05f + up * 0.5f : bounced * 0.25f + up * 1.5f;
				if (m_BodyMass > 0.0f && kit.BlockShove > 0.0f)
				{
					glm::vec3 push(in.x, 0.0f, in.z);
					float along = glm::length(push);
					if (along > 1e-4f)
					{
						float shove = std::min(kit.BlockShove * mass * speed * along / m_BodyMass, 0.6f * CatchableSpeed());
						m_StaggerVelocity += push / along * shove;
					}
				}
				m_RecoilRate += 1.8f * std::max(kit.BlockShove / 0.35f, 0.3f);
			}
			return r;
		}
	}
	return r;
}
