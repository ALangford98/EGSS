#pragma once

// A jointed humanoid: thirteen bodies, twelve joints, and a switch between
// falling like a corpse and standing like a person.
//
// This demo is the point of all the joint work. Nothing here is new physics --
// it is a rig, and the interesting thing about a rig is that it is where the
// constraint code stops being tested in twos and starts being tested in a
// chain of fifteen. A ragdoll is the case that finds solver weaknesses: long
// kinematic chains, wildly different masses meeting at one joint (a 24 kg
// torso against a 1.6 kg forearm), and limits that must hold while contacts
// push back.
//
// Two modes, one key apart:
//
//   * **Motors off** -- a passive ragdoll. It falls, folds at its limits, and
//     lies still. This is what joints alone give you.
//   * **Motors on** -- every joint drives towards the pose it was built in. It
//     stands, sways, and gives when shoved. This is not balance: nothing here
//     knows where its feet are or what its centre of mass is doing. Push it
//     hard enough and it topples, still trying to hold the pose on the way
//     down, which is exactly what a powered ragdoll without a balance
//     controller looks like.
//
// **WASD** walks, relative to the camera, **shift** runs and **space** jumps.
// **Arrows** orbit. **G** drops the character into a ragdoll, and again to get
// back up. **X** shoves them -- gently and they stagger and recover, hard
// enough and they go down. **F** swaps the follow camera for the old
// fly-around.
//
// Things marked TRY: are deliberate places to experiment.

#include <GS.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Demo.h"
#include "Humanoid.h"

class Ragdoll : public DemoLayer, public Humanoid
{
public:

	Ragdoll()
		: Ragdoll("Ragdoll")
	{
	}

	// For a demo that reuses this rig and wants its own name in the selector.
	explicit Ragdoll(const std::string& name)
		: DemoLayer(name), Humanoid(m_OwnWorld), m_Camera(50.0f, 16.0f / 9.0f, 0.1f, 200.0f)
	{
	}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.2f, 1.6f, 4.2f });
		m_Camera.SetRotation(-90.0f, -6.0f);

		m_Cube.reset(GS::Mesh::CreateCube(1.0f));
		m_Sphere.reset(GS::Mesh::CreateSphere(0.5f, 24, 12));
		// Unit radius and unit half-height, so the scale in the draw call is
		// the capsule's own diameter and segment length.
		m_Cylinder.reset(GS::Mesh::CreateCylinder(0.5f, 0.5f, 24));

		// The lit solid-colour shader Physics3D registers. Shared through the
		// library rather than copied, which is what the library is for --
		// and safe to depend on because `OnAttach` is deliberately not guarded
		// by the active demo, so every demo's assets are built at startup.
		m_SceneMaterial = GS::Material::Create(
			GS::Renderer::GetShaderLibrary().Get("Physics3D"));

		// The stumble's sliders reach the simulation, so a recording made
		// while they move replays as itself.
		RegisterParam("Step reach", &m_StumbleReach);
		RegisterParam("Step time", &m_StumbleStepTime);
		RegisterParam("Leg strength", &m_StumbleMaxSupport);
		RegisterParam("Physical below", &m_StumbleEntrySpeed);

		BuildScene();
	}

	void BuildScene()
	{
		m_World.Clear();
		m_Joints.clear();
		m_Stepping = false;
		m_Shifting = false;
		m_ShiftTimer = 0.0f;
		m_WaitingForTrough = false;
		m_WaitTimer = 0.0f;
		m_TroughsUsed = 0;
		m_TroughsMissed = 0;
		m_StepsTaken = 0;
		m_StepTimer = 0.0f;

		m_World.Gravity = { 0.0f, -9.81f, 0.0f };
		// A ragdoll is a fifteen-link chain, and a chain solved with too few
		// iterations does not merely sag -- it gains energy and flails. Eight
		// is the floor; this is comfortably above it because the mass ratios
		// here are severe.
		m_World.VelocityIterations = 16;
		m_World.PositionIterations = 6;

		BuildGround();
		BuildRagdoll(SpawnPoint());
		m_CameraFocus = m_World.GetBody(m_Torso).Position;
	}

	// The world the character stands in, and the one thing a demo that reuses
	// this rig is expected to replace. Everything else here -- the gait, the
	// balance, the get-up -- already asks the world where the ground is rather
	// than assuming it is at zero, which is what makes swapping this out a
	// three-line change instead of a rewrite.
	virtual void BuildGround()
	{
		m_World.AddBody(GS::RigidBody3D::MakeStaticBox(
			{ 0.0f, -0.5f, 0.0f }, { 8.0f, 0.5f, 8.0f }));

		// A step to walk up. 0.30 m to the top, which is a tall domestic stair
		// and about the limit of what the gait will lift a foot over.
		m_World.AddBody(GS::RigidBody3D::MakeStaticBox(
			{ 0.0f, 0.15f, 2.6f }, { 1.4f, 0.15f, 1.2f }));

		// And something smaller to trip over.
		m_World.AddBody(GS::RigidBody3D::MakeStaticBox(
			{ 1.6f, 0.08f, 0.0f }, { 0.3f, 0.08f, 0.3f }));
	}

	// Where the rig is assembled. On flat ground that is the origin; on terrain
	// it has to be the surface, because the whole rig is laid out upwards from
	// this point and a figure built 5 m under a hill starts inside it.
	virtual glm::vec3 SpawnPoint() const
	{
		return { 0.0f, m_DropHeight, 0.0f };
	}

	// A shove at chest height, in the direction the camera is looking.
	void Push(float strength)
	{
		glm::vec3 direction = m_Camera.GetForward();
		direction.y = 0.0f;
		if (glm::length(direction) < 0.001f)
			direction = { 0.0f, 0.0f, -1.0f };

		direction = glm::normalize(direction);

		Shove(strength, direction);
	}

	void OnDemoFixedUpdate(GS::Timestep fixedStep) override
	{
		MoveCamera(fixedStep);


		if (m_PendingPush != 0.0f)
		{
			Push(m_PendingPush);
			m_PendingPush = 0.0f;
		}

		if (m_Paused)
			return;

		// What the character wants this step, read here -- where DriveWalk
		// used to poll it -- and handed over.
		Control& control = GetControl();
		control.Yaw = m_CameraYaw;
		control.Enabled = !m_FreeCamera;
		control.Forward = GS::Input::IsKeyPressed(GS_KEY_W);
		control.Back = GS::Input::IsKeyPressed(GS_KEY_S);
		control.Left = GS::Input::IsKeyPressed(GS_KEY_A);
		control.Right = GS::Input::IsKeyPressed(GS_KEY_D);
		control.Run = GS::Input::IsKeyPressed(GS_KEY_LEFT_SHIFT);

		Update(fixedStep);

		m_World.Step(fixedStep);
	}

	// A camera that follows rather than one that is flown.
	//
	// It orbits a point on the character at a fixed distance, and both the
	// point and the distance are eased rather than followed exactly. The lag
	// is the whole reason it feels attached to something with weight -- a
	// camera pinned rigidly to the hips inherits the walk's bob and sway and
	// makes the picture seasick.
	void MoveCamera(GS::Timestep step)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		float dt = step;

		// Orbit on the arrows. Pitch is clamped short of straight down, which
		// is where a follow camera flips over and loses its horizon.
		float turn = 90.0f * dt;
		if (GS::Input::IsKeyPressed(GS_KEY_LEFT))  m_CameraYaw -= turn;
		if (GS::Input::IsKeyPressed(GS_KEY_RIGHT)) m_CameraYaw += turn;
		if (GS::Input::IsKeyPressed(GS_KEY_UP))    m_CameraPitch -= turn;
		if (GS::Input::IsKeyPressed(GS_KEY_DOWN))  m_CameraPitch += turn;

		m_CameraPitch = glm::clamp(m_CameraPitch, -60.0f, 25.0f);

		if (m_FreeCamera)
		{
			// The old fly-around, kept for looking at things. WASD belongs to
			// the character now, so this borrows it back while it is on.
			glm::vec3 position = m_Camera.GetPosition();
			float move = m_CameraSpeed * dt;

			if (GS::Input::IsKeyPressed(GS_KEY_W)) position += m_Camera.GetForward() * move;
			if (GS::Input::IsKeyPressed(GS_KEY_S)) position -= m_Camera.GetForward() * move;
			if (GS::Input::IsKeyPressed(GS_KEY_A)) position -= m_Camera.GetRight() * move;
			if (GS::Input::IsKeyPressed(GS_KEY_D)) position += m_Camera.GetRight() * move;
			if (GS::Input::IsKeyPressed(GS_KEY_Q)) position.y -= move;
			if (GS::Input::IsKeyPressed(GS_KEY_E)) position.y += move;

			m_Camera.SetPosition(position);
			m_Camera.SetRotation(m_CameraYaw, -m_CameraPitch);
			return;
		}

		// Follow the torso rather than the pelvis: the pelvis carries the bob
		// and the sway deliberately, and the camera should not.
		glm::vec3 focus = m_World.GetBody(m_Torso).Position;
		focus.y = m_World.GetBody(m_Pelvis).Position.y + m_CameraHeight;

		// Eased towards it. A follow that is exact is a follow that shakes.
		float blend = 1.0f - std::exp(-m_CameraLag * dt);
		m_CameraFocus += (focus - m_CameraFocus) * blend;

		// The camera sits back along its own forward vector, so pointing it at
		// its own yaw and pitch looks exactly at the focus with no aiming
		// arithmetic at all.
		float yaw = glm::radians(m_CameraYaw);
		float pitch = glm::radians(-m_CameraPitch);

		glm::vec3 forward(std::cos(yaw) * std::cos(pitch),
			std::sin(pitch),
			std::sin(yaw) * std::cos(pitch));

		glm::vec3 wanted = m_CameraFocus - forward * m_CameraDistance;

		// Never let it drop through the floor, which is the one way an orbit
		// camera ends up underground looking at the inside of the world.
		float ground = m_World.GroundHeightBelow(wanted, m_Pelvis);
		wanted.y = std::max(wanted.y, ground + 0.35f);

		m_Camera.SetPosition(wanted);
		m_Camera.SetRotation(m_CameraYaw, -m_CameraPitch);
	}

	// ---------------------------------------------------------------------
	// Drawing -- every body as its own collider
	// ---------------------------------------------------------------------
	void OnDemoUpdate(GS::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		float alpha = GS::Application::Get().GetInterpolationAlpha();

		GS::RenderCommand::SetClearColor(ClearColour());
		GS::RenderCommand::Clear();
		GS::RenderCommand::SetDepthTest(true);

		GS::Renderer::BeginScene(m_Camera);

		SetSceneLighting();
		DrawWorld();

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const GS::RigidBody3D& body = bodies[i];

			// A heightfield has no primitive to stand in for it; whoever put it
			// in the world draws it, in DrawWorld.
			if (body.Shape == GS::ColliderShape3D::Heightfield)
				continue;

			glm::vec3 position = glm::mix(body.PreviousPosition, body.Position, alpha);
			glm::quat orientation = glm::slerp(body.PreviousOrientation, body.Orientation, alpha);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
				* glm::mat4_cast(orientation);

			glm::vec4 colour;
			if (body.Type == GS::BodyType::Static)
				colour = { 0.34f, 0.36f, 0.42f, 1.0f };
			else if (i == m_Head)
				colour = { 0.92f, 0.78f, 0.62f, 1.0f };
			else if (i == m_Torso || i == m_Pelvis)
				colour = { 0.36f, 0.55f, 0.80f, 1.0f };
			else
				colour = { 0.80f, 0.55f, 0.35f, 1.0f };

			if (body.Type != GS::BodyType::Static && !body.Awake)
				colour = glm::vec4(glm::vec3(colour) * 0.45f, 1.0f);

			m_SceneMaterial->Set("u_Color", colour);

			if (body.Shape == GS::ColliderShape3D::Box)
			{
				GS::Renderer::Submit(m_SceneMaterial, m_Cube,
					glm::scale(transform, body.HalfExtents * 2.0f));
			}
			else if (body.Shape == GS::ColliderShape3D::Capsule)
			{
				// Cylinder shaft, spherical caps -- exactly what a capsule is.
				float d = body.Radius * 2.0f;

				GS::Renderer::Submit(m_SceneMaterial, m_Cylinder,
					glm::scale(transform, { d, body.HalfHeight * 2.0f, d }));

				for (float end : { -1.0f, 1.0f })
				{
					glm::mat4 cap = glm::translate(transform,
						{ 0.0f, end * body.HalfHeight, 0.0f });
					GS::Renderer::Submit(m_SceneMaterial, m_Sphere,
						glm::scale(cap, glm::vec3(d)));
				}
			}
			else
			{
				GS::Renderer::Submit(m_SceneMaterial, m_Sphere,
					glm::scale(transform, glm::vec3(body.Radius * 2.0f)));
			}
		}

		GS::Renderer::EndScene();

		// The balance state, drawn as lines in the perspective scene --
		// Renderer2D::BeginScene takes any camera, which is what makes debug
		// geometry in a 3D scene possible without a second renderer.
		// Only while ragdolling. Under control the character is not balancing --
		// the capture point sits outside its feet constantly and means nothing,
		// and drawing it implies a failure that is not happening.
		if (m_ShowBalance && m_Mode == Mode::Ragdoll && m_Balance.Support.size() >= 3)
		{
			GS::RenderCommand::SetDepthTest(false);
			GS::Renderer2D::BeginScene(m_Camera);

			// Just off the floor, or it z-fights -- and the floor is wherever
			// the character is standing, not zero. On flat ground this is the
			// 0.02 it always was; on terrain the polygon would otherwise be
			// drawn buried in a hillside.
			const float y = m_World.GroundHeightBelow(m_Balance.Com, m_Pelvis, -1000.0f) + 0.02f;

			// The support polygon: everywhere the feet actually touch.
			for (size_t i = 0; i < m_Balance.Support.size(); i++)
			{
				const glm::vec2& a = m_Balance.Support[i];
				const glm::vec2& b = m_Balance.Support[(i + 1) % m_Balance.Support.size()];

				GS::Renderer2D::DrawLine({ a.x, y, a.y }, { b.x, y, b.y },
					{ 0.4f, 0.9f, 1.0f, 1.0f });
			}

			// The centre of mass, dropped to the floor: a plumb line and a
			// cross where it lands.
			glm::vec3 com = m_Balance.Com;
			GS::Renderer2D::DrawLine(com, { com.x, y, com.z }, { 1.0f, 1.0f, 0.4f, 1.0f });

			auto cross = [&](glm::vec2 at, glm::vec4 colour, float size)
			{
				GS::Renderer2D::DrawLine({ at.x - size, y, at.y }, { at.x + size, y, at.y }, colour);
				GS::Renderer2D::DrawLine({ at.x, y, at.y - size }, { at.x, y, at.y + size }, colour);
			};

			cross(m_Balance.ComGround, { 1.0f, 1.0f, 0.4f, 1.0f }, 0.06f);

			// The capture point, which is the one that decides. Green while it
			// is over the feet, red once it is not -- and it turns red before
			// the figure looks like it is falling, which is the whole idea.
			cross(m_Balance.CapturePoint,
				m_Balance.Standing ? glm::vec4(0.4f, 1.0f, 0.5f, 1.0f)
				                   : glm::vec4(1.0f, 0.35f, 0.3f, 1.0f), 0.09f);

			GS::Renderer2D::EndScene();
			GS::RenderCommand::SetDepthTest(true);
		}

		// Stumbling: the footprint the virtual leg may push from, the
		// capture point it is judged by, and where the step is going.
		if (m_ShowBalance && m_Mode == Mode::Stumbling)
		{
			GS::RenderCommand::SetDepthTest(false);
			GS::Renderer2D::BeginScene(m_Camera);
			const float y = m_StumbleGround + 0.02f;
			for (size_t i = 0; i < m_StumbleFootprint.size(); i++)
			{
				const glm::vec2& a = m_StumbleFootprint[i];
				const glm::vec2& b = m_StumbleFootprint[(i + 1) % m_StumbleFootprint.size()];
				GS::Renderer2D::DrawLine({ a.x, y, a.y }, { b.x, y, b.y }, { 0.4f, 0.9f, 1.0f, 1.0f });
			}
			auto cross = [&](glm::vec2 at, glm::vec4 colour, float size)
			{
				GS::Renderer2D::DrawLine({ at.x - size, y, at.y }, { at.x + size, y, at.y }, colour);
				GS::Renderer2D::DrawLine({ at.x, y, at.y - size }, { at.x, y, at.y + size }, colour);
			};
			glm::vec3 com = m_Balance.Com;
			GS::Renderer2D::DrawLine(com, { com.x, y, com.z }, { 1.0f, 1.0f, 0.4f, 1.0f });
			cross(m_Balance.ComGround, { 1.0f, 1.0f, 0.4f, 1.0f }, 0.06f);
			cross(m_StumbleCapture, m_StumbleMargin >= 0.0f ? glm::vec4(0.4f, 1.0f, 0.5f, 1.0f)
				: glm::vec4(1.0f, 0.35f, 0.3f, 1.0f), 0.09f);
			if (m_StumbleSwing >= 0)
				cross(m_StumbleTarget, { 1.0f, 0.6f, 0.1f, 1.0f }, 0.12f);
			// The virtual leg itself, scaled 1 m per body weight.
			glm::vec3 pelvis = m_World.GetBody(m_Pelvis).Position;
			GS::Renderer2D::DrawLine(pelvis, pelvis - m_StumbleForce / std::max(m_BodyMass * 9.81f, 1.0f),
				{ 0.8f, 0.5f, 1.0f, 1.0f });
			GS::Renderer2D::EndScene();
			GS::RenderCommand::SetDepthTest(true);
		}
	}

	// --- What a demo reusing this rig overrides to change the scenery -------

	virtual glm::vec4 ClearColour() const { return { 0.05f, 0.06f, 0.09f, 1.0f }; }

	// A point light a few metres up, which is right for a room-sized scene and
	// useless outdoors -- its `1/(1 + 0.015 d^2)` falloff leaves anything past
	// about 40 m lit by ambient alone.
	virtual void SetSceneLighting()
	{
		m_SceneMaterial->Set("u_LightPosition", glm::vec3(2.0f, 4.0f, 3.0f));
		m_SceneMaterial->Set("u_LightColor", glm::vec3(1.0f, 0.97f, 0.9f));
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", 0.30f);
	}

	// Geometry that is not a rigid body's collider. Inside the scene, so a
	// subclass does not have to know how one is begun or ended.
	virtual void DrawWorld() {}

	void OnDemoEvent(GS::Event& e) override
	{
		GS::EventDispatcher dispatcher(e);
		dispatcher.Dispatch<GS::KeyPressedEvent>([this](GS::KeyPressedEvent& key)
		{
			if (key.GetKeyCode() == GS_KEY_R) { BuildScene(); return true; }
			if (key.GetKeyCode() == GS_KEY_P) { m_Paused = !m_Paused; return true; }
			if (key.GetKeyCode() == GS_KEY_M) { SetMotorsEnabled(!m_MotorsEnabled); return true; }
			if (key.GetKeyCode() == GS_KEY_B) { m_BalanceEnabled = !m_BalanceEnabled; return true; }
			if (key.GetKeyCode() == GS_KEY_F) { m_FreeCamera = !m_FreeCamera; return true; }
			if (key.GetKeyCode() == GS_KEY_G)
			{
				m_Mode == Mode::Controlled ? GoRagdoll("key") : BeginGetUp();
				return true;
			}
			if (key.GetKeyCode() == GS_KEY_X) { m_PendingPush = m_PushStrength; return true; }
			if (key.GetKeyCode() == GS_KEY_SPACE) { m_WantsJump = true; return true; }
			return false;
		});
	}

	// ---------------------------------------------------------------------
	// Panel
	// ---------------------------------------------------------------------
	void OnDemoImGui() override
	{
		ImGui::Begin("Ragdoll");

		ImGui::TextColored(m_Mode == Mode::Controlled
			? ImVec4(0.5f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
			m_Mode == Mode::Controlled ? "CONTROLLED"
				: m_Mode == Mode::Stumbling ? "STUMBLING"
				: (m_Mode == Mode::GettingUp ? "GETTING UP" : "RAGDOLL (%s, %.1f s)"),
			m_RagdollReason, m_RagdollTimer);
		if (m_Mode == Mode::Stumbling)
			ImGui::Text("  %d step%s, %.1f s, capture %+.2f m from the feet, leg %.0f%%",
				m_StumbleSteps, m_StumbleSteps == 1 ? "" : "s", m_StumbleTimer, m_StumbleMargin, 100.0f * m_StumbleSupport);

		ImGui::Text("WASD walk   shift run   space jump   arrows orbit   G ragdoll / get up");
		ImGui::Text("X push   R reset   P pause   F free camera%s",
			m_FreeCamera ? " (ON -- WASD flies)" : "");
		ImGui::Text("Steps taken: %d", m_StepsTaken);
		ImGui::Text("WASD/QE move   arrows look");

		// Said here because otherwise it reads as a bug. It is not: a standing
		// figure is an inverted pendulum on two small feet, and the motors
		// hold *joint angles*, which do not change when the whole body tips
		// about its ankles. Staying up needs a balance controller -- centre of
		// mass against the support polygon, and a step when it leaves -- and
		// there is not one yet.
		ImGui::TextWrapped("Controlled: the pelvis is kinematic, so the character cannot "
			"fall and does not balance -- the limbs still hang off it and react. A hit "
			"harder than the threshold below makes the pelvis dynamic and physics takes "
			"over. That switch is how games do this; balancing on the skeleton is the "
			"hard problem it avoids.");

		ImGui::Separator();
		ImGui::Text("Bodies: %zu   Joints: %zu", m_World.GetBodyCount(), m_World.GetJointCount());
		ImGui::Text("Contacts: %zu", m_World.GetContacts().size());

		// The honest measure of whether the solver is keeping up with a chain
		// this long. A limb visibly pulling out of its socket shows up here
		// first, as millimetres.
		ImGui::Text("Worst joint separation: %.4f m", m_World.GetWorstJointSeparation());

		ImGui::Separator();
		ImGui::Text("Support polygon: %zu points", m_Balance.Support.size());
		ImGui::Text("Centre of mass: (%.2f, %.2f, %.2f)",
			m_Balance.Com.x, m_Balance.Com.y, m_Balance.Com.z);

		// The number that decides whether it stays up. Positive is the capture
		// point inside the feet; it crosses zero *before* the figure visibly
		// starts to fall, which is what makes it worth watching rather than
		// watching the figure.
		ImGui::TextColored(m_Balance.Standing ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
			                                  : ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
			"Capture margin: %+.3f m  (%s)", m_Balance.Margin,
			m_Balance.Standing ? "over its feet" : "falling");

		// Body weight is about 697 N, so half of it on each foot is standing
		// square. A foot cannot be lifted until its share is near zero.
		ImGui::Text("Foot load: left %.0f N, right %.0f N  (weight is 697 N)",
			FootLoad(0, 1.0f / 60.0f), FootLoad(1, 1.0f / 60.0f));

		ImGui::Text("Frame: %.2f ms", m_FrameTime);

		ImGui::Separator();
		if (ImGui::Checkbox("Motors", &m_MotorsEnabled))
			SetMotorsEnabled(m_MotorsEnabled);

		// TRY: drop the stiffness towards 2 and the figure sags like something
		// exhausted; raise it towards 40 and it snaps rigidly upright. Neither
		// is balance -- both topple identically once the centre of mass leaves
		// the feet.
		if (ImGui::SliderFloat("Motor stiffness", &m_MotorStiffness, 1.0f, 40.0f))
			SetMotorsEnabled(m_MotorsEnabled);

		ImGui::Checkbox("Balance", &m_BalanceEnabled);
		ImGui::SameLine();
		// TRY: turn this on and watch it make things worse. The step fires at
		// the right moment and aims at the right place; what it does not do is
		// shift the weight onto the stance leg first, so lifting a foot throws
		// the body as much as the step recovers.
		ImGui::Checkbox("Old stepping (worse)", &m_SteppingEnabled);
		ImGui::SameLine();
		ImGui::Checkbox("Show balance", &m_ShowBalance);

		// TRY: turn the ankle gain down to zero and push gently. The figure
		// leans and keeps leaning, because nothing is shifting its weight back
		// over its feet.
		ImGui::SliderFloat("Ankle pitch gain", &m_AnkleGain, 0.0f, 8.0f);
		ImGui::SliderFloat("Ankle roll gain", &m_AnkleRollGain, 0.0f, 8.0f);
		ImGui::SliderFloat("Push strength", &m_PushStrength, 10.0f, 400.0f);

		// The physical stumble (Humanoid::UpdateStumble). Standing, a hit
		// makes the whole body dynamic and he has to catch it with his feet.
		// TRY: push at 60 with "Step to catch" off -- the ankles alone hold
		// about 30 Ns -- then on, and watch the footprint grow to meet the
		// red capture point.
		ImGui::SeparatorText("Stumble");
		ImGui::Checkbox("Physical stumble", &m_PhysicalStumble);
		ImGui::SameLine();
		ImGui::Checkbox("Step to catch", &m_SteppingCatch);
		// TRY: take the reach down to 0.3 and he goes down at half the push;
		// the step time up to 0.4 and the steps come too late, like August.
		ImGui::SliderFloat("Step reach (m)", &m_StumbleReach, 0.2f, 0.9f);
		ImGui::SliderFloat("Step time (s)", &m_StumbleStepTime, 0.12f, 0.5f);
		ImGui::SliderFloat("Leg strength (body weights)", &m_StumbleMaxSupport, 1.0f, 3.0f);
		ImGui::SliderFloat("Physical below (m/s)", &m_StumbleEntrySpeed, 0.0f, 5.0f);
		// TRY: drop this towards 500 and the character falls over at the
		// slightest contact; raise it past 10000 and nothing knocks them down.
		// TRY: drop the reach or the step count and he becomes glass -- these
		// two are now the whole of what decides whether a hit puts him down.
		ImGui::SliderFloat("Catch reach (m)", &m_CatchReach, 0.1f, 1.0f);
		ImGui::SliderFloat("Catch reach sideways", &m_CatchReachSide, 0.1f, 1.0f);
		ImGui::SliderFloat("Recovery steps", &m_CatchSteps, 0.5f, 5.0f);
		ImGui::Text("Can still catch %.2f m/s; carrying %.2f", CatchableSpeed(),
			glm::length(m_StaggerVelocity));
		ImGui::SliderFloat("Walk speed", &m_WalkSpeed, 0.0f, 4.0f);
		ImGui::SliderFloat("Run speed", &m_RunSpeed, 1.0f, 8.0f);
		ImGui::SliderFloat("Jump height (m)", &m_JumpHeight, 0.1f, 1.5f);
		// TRY: bring this up to the ragdoll threshold and nothing staggers --
		// every hit is either ignored or a collapse, which is how it used to be.
		ImGui::SliderFloat("Stagger threshold (N)", &m_StaggerThreshold, 100.0f, 3000.0f);
		ImGui::SliderFloat("Arm swing", &m_ArmSwing, 0.0f, 1.2f);
		// TRY: on terrain, take this to 0 and watch the soles go back to being
		// level with the horizon -- the figure keeps walking, but it lands on a
		// heel or a toe every step and the mean sole tilt goes from 2 deg to 10.
		// On flat ground it does nothing at all, by construction.
		ImGui::SliderFloat("Sole follows slope", &m_SoleToSlope, 0.0f, 1.0f);
		ImGui::SliderFloat("Max sole tilt (deg)", &m_MaxSoleTilt, 0.0f, 35.0f);
		ImGui::Text("Gait: %.0f%% of the cycle on the ground, %s",
			100.0f * m_Duty, m_Duty < 0.5f ? "so it has flight" : "so it has double support");
		ImGui::SliderFloat("Get up over (s)", &m_GetUpTime, 0.2f, 3.0f);
		ImGui::SliderFloat("Camera distance", &m_CameraDistance, 1.5f, 10.0f);
		ImGui::SliderFloat("Camera lag", &m_CameraLag, 1.0f, 20.0f);
		ImGui::Text("Standing %.2f m above ground; step ahead %.0f%%",
			m_StandClearance, 100.0f * m_StepUp);
		ImGui::SliderFloat("Drop height", &m_DropHeight, 0.0f, 3.0f);
		ImGui::SliderInt("Velocity iterations", (int*)&m_World.VelocityIterations, 4, 40);
		ImGui::Checkbox("Allow sleeping", &m_World.AllowSleeping);

		if (ImGui::Button("Reset"))
			BuildScene();

		ImGui::End();
	}

// Protected rather than private because Map Building inherits the whole
// character from here. Everything below this line is the rig, its gait and its
// tuning, and there is exactly one copy of it -- see the note at the top of
// MapBuilding.h for why that demo is a subclass rather than a second rig.
protected:
	GS::PerspectiveCamera m_Camera;
	// The world the character lives in; Humanoid::m_World refers to it.
	GS::PhysicsWorld3D m_OwnWorld;

	std::shared_ptr<GS::Mesh> m_Cube;
	std::shared_ptr<GS::Mesh> m_Sphere;
	std::shared_ptr<GS::Mesh> m_Cylinder;
	std::shared_ptr<GS::Material> m_SceneMaterial;
	bool m_ShowBalance = true;
	bool m_Paused = false;
	float m_PushStrength = 120.0f;
	float m_DropHeight = 0.0f;
	float m_PendingPush = 0.0f;
	float m_CameraSpeed = 3.0f;

	// Third person.
	bool m_FreeCamera = false;
	float m_CameraYaw = -90.0f;      // looking down +z, the way the figure faces
	float m_CameraPitch = 12.0f;
	float m_CameraDistance = 4.0f;
	float m_CameraHeight = 0.55f;    // above the pelvis, so about chest high
	float m_CameraLag = 6.0f;        // how quickly it catches up, per second
	glm::vec3 m_CameraFocus{ 0.0f };
	float m_FrameTime = 0.0f;
};
