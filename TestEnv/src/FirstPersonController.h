#pragma once

// Shared first-person camera and movement, standalone the same way Terrain.h
// is: no demo owns this, several can use it.
//
// This consolidates a pattern that had been written three times with
// diverging conventions: VoxelTerrain::MoveWalker and ::MoveCamera, and the
// free-fly branch of Ragdoll::MoveCamera. Forward/right from yaw, WASD
// accumulated against them, mouse-look with cursor capture, the first-
// mouse-sample skip, and the yaw convention -- all one place now.
//
// **Yaw 0 looks along +x, not -z** (PerspectiveCamera::GetForward), and yaw
// grows turning right. Worth restating here because it is a 50/50 sign and
// the arrow keys had it backwards for a while before a test checked the turn
// against GetRight() instead of against itself -- see docs/HANDOVER.md.
//
// **Input is sampled per fixed step, mouse included, never in OnUpdate.**
// A replay stream carries the cursor position per step, not per event, so a
// look done with the mouse only replays exactly if it was sampled on the
// same clock the recording used. Whatever in Config reaches the simulation
// should be registered with the owning demo's RegisterParam, the same as
// any other parameter -- this class does not register anything itself,
// since it does not own a DemoLayer.

#include <Egss.h>
#include <imgui.h>

class FirstPersonController
{
public:
	struct Config
	{
		float WalkSpeed = 4.0f;
		float JumpSpeed = 5.0f;
		float FlySpeed = 12.0f;

		// Degrees per pixel of mouse delta, and degrees per second for the
		// arrow-key fallback -- two different units because one is a ratio
		// against something the OS already scaled and the other is not.
		float MouseSensitivity = 0.12f;
		float LookRateDegreesPerSecond = 90.0f;

		float MinPitch = -85.0f;
		float MaxPitch = 85.0f;

		// How far below the eye the feet may sit above the ground and still
		// count as standing on it. Separate from the walker's own radius,
		// which is a shape fact, not a gait one.
		float GroundedTolerance = 0.25f;

		// --- Water ---------------------------------------------------------
		//
		// Off unless a caller turns it on. A demo with no water must not have
		// its jump quietly rerouted through a buoyancy model because the
		// controller happens to know how to swim.
		bool HasWater = false;
		float WaterLevel = 0.0f;

		// Standing in water shallower than this is wading: normal walking,
		// normal jumping. Deeper than this the feet have left the bottom in
		// any useful sense and it becomes swimming. Without the distinction,
		// ankle-deep surf would take control away from the player.
		float WadeDepth = 1.1f;

		float SwimSpeed = 2.6f;      // slower than walking, because water
		float SwimVerticalSpeed = 2.2f;   // space to rise, shift to dive

		// Where the feet settle when floating with nothing pressed, measured
		// down from the surface. Roughly eye-out-of-the-water for a 1.7 m
		// capsule.
		float FloatDepth = 1.35f;

		// How hard the float is corrected, per second. A spring rather than a
		// snap, so entering the water sinks and comes back up instead of
		// clamping to the surface on the first step.
		float BuoyancyStiffness = 3.0f;

		// Fraction of velocity shed per second in water, which is what makes
		// it feel like water rather than like low gravity.
		float WaterDrag = 6.0f;
	};

	// What the body is doing, for a caller that wants to show it.
	enum class Motion { Airborne, Grounded, Wading, Swimming };

	// The rule, pulled out of UpdateWalk so it can be checked without a
	// physics world: `depth` is how far the feet are below the water surface,
	// negative when dry.
	//
	// Standing on the bottom in shallow water is still walking -- including
	// jumping out of it. Deeper than a wade, or with no bottom underfoot at
	// all, it is swimming.
	static Motion Classify(float depth, bool grounded, const Config& cfg)
	{
		if (depth <= 0.0f)
			return grounded ? Motion::Grounded : Motion::Airborne;

		if (grounded && depth < cfg.WadeDepth)
			return Motion::Wading;

		return Motion::Swimming;
	}

	Config Cfg;

	explicit FirstPersonController(Egss::PerspectiveCamera& camera,
		float startYaw = -90.0f, float startPitch = 0.0f)
		: m_Camera(camera), m_Yaw(startYaw), m_Pitch(startPitch)
	{
		m_Camera.SetRotation(m_Yaw, m_Pitch);
	}

	float GetYaw() const { return m_Yaw; }
	float GetPitch() const { return m_Pitch; }
	bool IsMouseLook() const { return m_MouseLook; }

	// Said regardless of mode, because a captured cursor is a state you can
	// get stuck in and the way out is not guessable from the picture. A
	// demo's panel calls this rather than writing its own copy.
	void MouseLookHelp() const
	{
		if (m_MouseLook)
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
				"Mouse look ON -- Esc or Tab releases the cursor");
		else
			ImGui::TextDisabled("Tab captures the mouse to look");
	}

	// The logical mode and the hardware cursor are set together here and
	// nowhere else, so they cannot drift apart -- but they are still two
	// things. A replay drives mode from the recorded Tab/Escape presses and
	// must not grab the cursor: nobody is watching a playback, and a run
	// that steals the pointer while the machine is being used is the exact
	// problem the hidden window was added to solve.
	void SetMouseLook(bool on)
	{
		m_MouseLook = on;
		m_MouseSampled = false;

		if (!Egss::Input::IsPlayingBack())
			Egss::Application::Get().GetWindow().SetCursorCaptured(on);
	}

	// Tab toggles, Escape releases, the mouse turns the camera when captured
	// and the arrows always turn it. Call once per fixed step, before
	// UpdateFly or UpdateWalk.
	void UpdateLook(float dt)
	{
		bool toggle = Egss::Input::IsKeyPressed(EGSS_KEY_TAB);
		if (toggle && !m_WasToggling)
			SetMouseLook(!m_MouseLook);
		else if (m_MouseLook && Egss::Input::IsKeyPressed(EGSS_KEY_ESCAPE))
			SetMouseLook(false);

		m_WasToggling = toggle;

		auto [mouseX, mouseY] = Egss::Input::GetMousePosition();

		if (m_MouseLook)
		{
			// The step that turns mouse-look on has no previous position to
			// subtract, and capturing the cursor moves it -- so the first
			// delta would be wherever the pointer happened to be sitting.
			// Skip exactly one.
			if (m_MouseSampled)
			{
				m_Yaw += (mouseX - m_LastMouseX) * Cfg.MouseSensitivity;
				// Screen y grows downward, so pushing the mouse away is a
				// negative delta and has to raise the pitch.
				m_Pitch -= (mouseY - m_LastMouseY) * Cfg.MouseSensitivity;
			}

			m_MouseSampled = true;
		}

		m_LastMouseX = mouseX;
		m_LastMouseY = mouseY;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  m_Yaw -= Cfg.LookRateDegreesPerSecond * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) m_Yaw += Cfg.LookRateDegreesPerSecond * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    m_Pitch += Cfg.LookRateDegreesPerSecond * dt;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  m_Pitch -= Cfg.LookRateDegreesPerSecond * dt;

		m_Pitch = glm::clamp(m_Pitch, Cfg.MinPitch, Cfg.MaxPitch);
		m_Camera.SetRotation(m_Yaw, m_Pitch);
	}

	// Free movement along the camera's own axes plus world-vertical E/Q.
	void UpdateFly(float dt)
	{
		glm::vec3 forward = m_Camera.GetForward();
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 move(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) move += forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) move -= forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) move += right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) move -= right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) move.y += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) move.y -= 1.0f;

		if (glm::length(move) > 1e-4f)
			m_Camera.SetPosition(m_Camera.GetPosition() + glm::normalize(move) * Cfg.FlySpeed * dt);
	}

	// Ground-following movement for a capsule the caller owns (its shape,
	// spawn point and friction are per-demo). Sets the horizontal velocity
	// outright rather than applying a force -- a force would have the
	// player accelerating for a second after every key press and sliding
	// for a second after letting go -- fires a jump when grounded and Space
	// is held, and rides the camera at `eyeHeight` above the body. Resets
	// orientation and angular velocity every step, the standard trick for a
	// dynamic capsule that must stand rather than topple: see the note in
	// VoxelTerrain::SpawnWalker.
	//
	// Returns whether the body is currently grounded, for a caller that
	// wants to show it.
	bool UpdateWalk(Egss::PhysicsWorld3D& world, Egss::PhysicsWorld3D::BodyHandle handle,
		float eyeHeight, float dt)
	{
		Egss::RigidBody3D& body = world.GetBody(handle);

		body.Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		body.AngularVelocity = glm::vec3(0.0f);
		body.UpdateInertiaWorld();

		glm::vec3 forward = m_Camera.GetForward();
		forward.y = 0.0f;

		if (glm::length(forward) < 1e-4f)
			forward = { 0.0f, 0.0f, 1.0f };

		forward = glm::normalize(forward);
		glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

		glm::vec3 wish(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) wish += forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) wish -= forward;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) wish += right;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) wish -= right;

		// Kept for whoever is drawing the body: which way the player is *trying*
		// to go, which is not the same as which way they are looking and is the
		// thing a character turns to face.
		m_Wish = glm::length(wish) > 1e-4f ? glm::normalize(wish) : glm::vec3(0.0f);

		float feet = body.Position.y - eyeHeight;

		float ground = 0.0f;
		glm::vec3 normal(0.0f, 1.0f, 0.0f);
		bool found = world.GroundBelow(body.Position, ground, normal, handle);

		bool grounded = found && (feet - ground) < Cfg.GroundedTolerance;

		// How deep the feet are below the water surface. Negative out of water.
		float depth = Cfg.HasWater ? (Cfg.WaterLevel - feet) : -1.0f;

		// Standing on the bottom in shallow water is still walking. Only once
		// the water is deeper than a wade -- or there is no bottom within
		// reach -- does it become swimming.
		m_Motion = Classify(depth, grounded, Cfg);

		bool wading = m_Motion == Motion::Wading;
		bool swimming = m_Motion == Motion::Swimming;

		glm::vec3 velocity = body.Velocity;

		float speed = swimming ? Cfg.SwimSpeed : Cfg.WalkSpeed;

		if (glm::length(wish) > 1e-4f)
		{
			glm::vec3 move = glm::normalize(wish) * speed;
			velocity.x = move.x;
			velocity.z = move.z;
		}
		else if (grounded && !swimming)
		{
			velocity.x = 0.0f;
			velocity.z = 0.0f;
		}
		else if (swimming)
		{
			// Coasting to a stop rather than stopping dead: the only thing
			// separating "floating in water" from "standing in mid-air" is
			// that water takes the momentum back gradually.
			float keep = glm::max(0.0f, 1.0f - Cfg.WaterDrag * dt);
			velocity.x *= keep;
			velocity.z *= keep;
		}

		if (swimming)
		{
			// Vertical control is taken over entirely while swimming, which is
			// what stops gravity from winning: the body is re-assigned a
			// vertical speed every step rather than accumulating one.
			float wantY;

			if (Egss::Input::IsKeyPressed(EGSS_KEY_SPACE))
				wantY = Cfg.SwimVerticalSpeed;
			else if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT))
				wantY = -Cfg.SwimVerticalSpeed;
			else
				// Float: rise while deeper than FloatDepth, sink while
				// shallower, and the error term is zero at the depth the body
				// is meant to settle at.
				wantY = glm::clamp((depth - Cfg.FloatDepth) * Cfg.BuoyancyStiffness,
					-Cfg.SwimVerticalSpeed, Cfg.SwimVerticalSpeed);

			velocity.y = wantY;
		}
		else if ((grounded || wading) && Egss::Input::IsKeyPressed(EGSS_KEY_SPACE))
		{
			velocity.y = Cfg.JumpSpeed;
		}

		body.Velocity = velocity;
		body.Awake = true;

		m_Camera.SetPosition(body.Position + glm::vec3(0.0f, eyeHeight, 0.0f));

		return grounded || wading;
	}

	Motion GetMotion() const { return m_Motion; }

	// Zero when standing still. World space, horizontal.
	const glm::vec3& GetWish() const { return m_Wish; }

	static const char* MotionName(Motion m)
	{
		switch (m)
		{
			case Motion::Swimming: return "swimming";
			case Motion::Wading:   return "wading";
			case Motion::Grounded: return "grounded";
			default:               return "airborne";
		}
	}

private:
	Egss::PerspectiveCamera& m_Camera;

	Motion m_Motion = Motion::Airborne;
	glm::vec3 m_Wish{ 0.0f };

	float m_Yaw;
	float m_Pitch;

	bool m_MouseLook = false;
	bool m_MouseSampled = false;
	bool m_WasToggling = false;
	float m_LastMouseX = 0.0f, m_LastMouseY = 0.0f;
};
