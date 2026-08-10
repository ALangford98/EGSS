#pragma once

// Rigid bodies in three dimensions -- boxes and spheres falling onto a ramp,
// tumbling down it and settling in a pit.
//
// The point of this demo is that it is the *only* place the 3D solver can be
// looked at. It was verified entirely by arithmetic first -- momentum
// conservation, a sphere rolling at (5/7) g sin(t), a resting contact carrying
// m g dt -- and all of that can be true while the thing on screen is nonsense.
// The 2D demo caught discs being drawn as squares that way.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Demo.h"

class Physics3D : public DemoLayer
{
public:
	Physics3D()
		: DemoLayer("Physics3D"), m_Camera(50.0f, 16.0f / 9.0f, 0.1f, 200.0f)
	{
	}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.5f, 7.5f, 14.0f });
		m_Camera.SetRotation(-92.0f, -26.0f);

		BuildShader();
		m_Cube.reset(Egss::Mesh::CreateCube(1.0f));
		m_Sphere.reset(Egss::Mesh::CreateSphere(0.5f, 24, 12));
		// Unit radius and unit half-height, so the scale in the draw call is
		// the capsule's own diameter and segment length.
		m_Cylinder.reset(Egss::Mesh::CreateCylinder(0.5f, 0.5f, 24));

		BuildScene();
	}

	// ---------------------------------------------------------------------
	// Scene
	// ---------------------------------------------------------------------
	void BuildScene()
	{
		m_World.Clear();
		m_SpawnCounter = 0;
		m_SpawnAccumulator = 0.0f;

		// Floor.
		Egss::RigidBody3D floor = Egss::RigidBody3D::MakeStaticBox({ 0, -0.5f, 0 }, { 9.0f, 0.5f, 7.0f });
		floor.Friction = 0.7f;
		m_World.AddBody(floor);

		// A ramp to tumble down. Rotated about z, so downhill runs towards +x
		// -- the same arrangement the solver was checked against, which makes
		// what you see here directly comparable to the numbers.
		Egss::RigidBody3D ramp = Egss::RigidBody3D::MakeStaticBox({ -3.5f, 2.2f, 0 }, { 4.0f, 0.25f, 2.5f });
		ramp.Orientation = glm::angleAxis(glm::radians(-22.0f), glm::vec3(0, 0, 1));
		ramp.Friction = 0.6f;
		m_World.AddBody(ramp);

		// Low walls, so things stay in shot. The near one is deliberately
		// shorter than the rest -- the camera looks over it, and a wall the
		// same height as the others simply hides the floor.
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox({ 9.0f, 0.6f, 0 }, { 0.5f, 1.1f, 7.0f }));
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox({ -9.0f, 0.6f, 0 }, { 0.5f, 1.1f, 7.0f }));
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox({ 0, 0.6f, -7.0f }, { 9.0f, 1.1f, 0.5f }));
		m_World.AddBody(Egss::RigidBody3D::MakeStaticBox({ 0, 0.15f, 7.0f }, { 9.0f, 0.65f, 0.5f }));

		m_StaticCount = (unsigned int)m_World.GetBodyCount();

		BuildStack();
	}

	// A stack is the honest test: one falling body works with almost any
	// solver, but boxes resting on each other only stand if the manifold, the
	// friction and the position correction are all sane.
	//
	// This used to fall over, chaotically, depending on the iteration counts --
	// and it was the manifold that was wrong, not the friction or the
	// correction. `Sat3D` wound a face pointing down a negative axis backwards
	// relative to its own normal, so whenever the *upper* box won the near-tie
	// for reference face the contact collapsed from four points to one, and one
	// point cannot hold a box level. See the changelog entry for the sweep.
	//
	// It now stands at every setting from 4 to 24 velocity iterations, with
	// sleeping on or off. Press R to watch it build again.
	void BuildStack()
	{
		for (int i = 0; i < 4; i++)
		{
			Egss::RigidBody3D box = Egss::RigidBody3D::MakeBox(
				// Off to one side, clear of the ramp's run-out. Standing in
				// the middle it was simply bowled over by the first sphere
				// down, which is honest physics and a poor demonstration of
				// the thing that was hardest to get right.
				{ 4.5f, 0.35f + i * 0.7f, 4.2f }, { 0.35f, 0.35f, 0.35f }, 1.0f);
			box.Restitution = 0.0f;
			box.Friction = 0.6f;
			m_World.AddBody(box);
		}
	}

	void SpawnBody()
	{
		// Dropped at the top of the ramp, so everything takes the same journey
		// down it and the two shapes can be compared directly.
		float z = -1.2f + 0.8f * (float)(m_SpawnCounter % 4);
		glm::vec3 where = { -6.5f, 5.5f, z };

		// Every fourth is a capsule, dropped lying across the ramp. It should
		// roll down on its side like a barrel rather than tumbling like a
		// crate, and come to rest flat -- which takes the two-point manifold.
		if (m_SpawnCounter % 4 == 3)
		{
			float radius = 0.22f;
			float halfHeight = 0.35f + 0.15f * std::fabs(std::sin((float)m_SpawnCounter));

			Egss::RigidBody3D capsule = Egss::RigidBody3D::MakeCapsule(
				where, radius, halfHeight, 1.0f);
			// Turned onto its side, across the slope.
			capsule.Orientation = glm::angleAxis(glm::half_pi<float>(),
				glm::vec3(0.0f, 0.0f, 1.0f));
			capsule.Restitution = 0.05f;
			capsule.Friction = 0.6f;
			capsule.AngularDamping = 0.03f;
			m_World.AddBody(capsule);
		}
		// Every third is a crate, dropped already tilted and spinning so it
		// lands on a corner. Tipping flush against the slope is the thing the
		// angular half of the solver exists for.
		else if (m_SpawnCounter % 3 == 2)
		{
			float half = 0.3f + 0.1f * std::fabs(std::sin((float)m_SpawnCounter));

			Egss::RigidBody3D crate = Egss::RigidBody3D::MakeBox(where, glm::vec3(half), 1.0f);
			crate.Orientation = glm::angleAxis(0.7f * std::sin((float)m_SpawnCounter * 2.3f),
				glm::normalize(glm::vec3(0.3f, 1.0f, 0.6f)));
			crate.AngularVelocity = { 1.5f, 2.0f, -1.0f };
			crate.Restitution = 0.05f;
			crate.Friction = 0.55f;
			m_World.AddBody(crate);
		}
		else
		{
			float radius = 0.25f + 0.12f * std::fabs(std::cos((float)m_SpawnCounter * 1.7f));

			Egss::RigidBody3D ball = Egss::RigidBody3D::MakeSphere(where, radius, 1.0f);
			ball.Restitution = m_Bounciness;
			// High enough to roll rather than slide, which is what makes a
			// sphere reach the bottom of the ramp behind the crates.
			ball.Friction = 0.8f;
			ball.AngularDamping = 0.02f;
			m_World.AddBody(ball);
		}

		m_SpawnCounter++;
	}

	// ---------------------------------------------------------------------
	// Simulation
	// ---------------------------------------------------------------------
	void OnDemoFixedUpdate(Egss::Timestep fixedStep) override
	{
		// Camera movement lives here rather than in OnDemoUpdate, so how far
		// it travels does not depend on the frame rate -- and so a recorded
		// replay puts it back in the same place.
		MoveCamera(fixedStep);

		if (m_Paused)
			return;

		if (m_SpawnRate > 0.0f)
		{
			m_SpawnAccumulator += fixedStep;
			while (m_SpawnAccumulator >= 1.0f / m_SpawnRate)
			{
				m_SpawnAccumulator -= 1.0f / m_SpawnRate;
				if (m_World.GetBodyCount() < (size_t)m_MaxBodies)
					SpawnBody();
			}
		}

		m_World.Gravity = { 0.0f, m_Gravity, 0.0f };
		m_World.Step(fixedStep);
	}

	void MoveCamera(Egss::Timestep step)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		glm::vec3 position = m_Camera.GetPosition();
		float move = m_CameraSpeed * step;

		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= m_Camera.GetForward() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += m_Camera.GetRight() * move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= move;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += move;

		m_Camera.SetPosition(position);

		float look = 70.0f * step;
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += look;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= look;

		m_Camera.SetRotation(yaw, pitch);
	}

	// ---------------------------------------------------------------------
	// Drawing
	// ---------------------------------------------------------------------
	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		float alpha = Egss::Application::Get().GetInterpolationAlpha();

		Egss::RenderCommand::SetClearColor({ 0.05f, 0.06f, 0.09f, 1.0f });
		Egss::RenderCommand::Clear();
		Egss::RenderCommand::SetDepthTest(true);

		Egss::Renderer::BeginScene(m_Camera);

		m_SceneMaterial->Set("u_LightPosition", glm::vec3(2.0f, 9.0f, 6.0f));
		m_SceneMaterial->Set("u_LightColor", glm::vec3(1.0f, 0.97f, 0.9f));
		m_SceneMaterial->Set("u_CameraPosition", m_Camera.GetPosition());
		m_SceneMaterial->Set("u_AmbientStrength", 0.28f);

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody3D& body = bodies[i];

			// Position interpolates linearly; orientation has to **slerp**.
			// glm::mix on two quaternions is a straight lerp, which leaves an
			// unnormalised quaternion -- a rotation *and* a scale, so a fast
			// tumbling box visibly swells between steps.
			glm::vec3 position = glm::mix(body.PreviousPosition, body.Position, alpha);
			glm::quat orientation = glm::slerp(body.PreviousOrientation, body.Orientation, alpha);

			glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
				* glm::mat4_cast(orientation);

			glm::vec4 colour;
			if (body.Type == Egss::BodyType::Static)
			{
				colour = { 0.34f, 0.36f, 0.42f, 1.0f };
			}
			else
			{
				if (body.Shape == Egss::ColliderShape3D::Sphere)
					colour = { 0.95f, 0.62f, 0.28f, 1.0f };
				else if (body.Shape == Egss::ColliderShape3D::Capsule)
					colour = { 0.55f, 0.85f, 0.45f, 1.0f };
				else
					colour = { 0.42f, 0.68f, 0.95f, 1.0f };

				// Asleep bodies are dimmed rather than greyed, so it is obvious
				// when a pile settles *without* losing what shape it is. Grey
				// was the first attempt and it made every settled crate look
				// like part of the scenery -- which is exactly the thing you
				// are looking at the demo to check.
				if (!body.Awake)
					colour = glm::vec4(glm::vec3(colour) * 0.45f, 1.0f);
			}

			m_SceneMaterial->Set("u_Color", colour);

			if (body.Shape == Egss::ColliderShape3D::Box)
			{
				// The cube mesh is one unit across, so it scales by the full
				// extents rather than the half ones.
				Egss::Renderer::Submit(m_SceneMaterial, m_Cube,
					glm::scale(transform, body.HalfExtents * 2.0f));
			}
			else if (body.Shape == Egss::ColliderShape3D::Capsule)
			{
				// Exactly what a capsule is: a cylinder between two
				// hemispheres. Two fixed meshes cover every size, because a
				// cylinder stays a cylinder under a non-uniform scale and a
				// sphere stays a sphere under a uniform one. A single capsule
				// mesh could not -- stretching one turns its caps into
				// ellipsoids.
				//
				// This was a box shaft first, which read as a rectangle with
				// balls stuck on the ends.
				float d = body.Radius * 2.0f;

				Egss::Renderer::Submit(m_SceneMaterial, m_Cylinder,
					glm::scale(transform, { d, body.HalfHeight * 2.0f, d }));

				for (float end : { -1.0f, 1.0f })
				{
					glm::mat4 cap = glm::translate(transform,
						{ 0.0f, end * body.HalfHeight, 0.0f });
					Egss::Renderer::Submit(m_SceneMaterial, m_Sphere,
						glm::scale(cap, glm::vec3(d)));
				}
			}
			else
			{
				Egss::Renderer::Submit(m_SceneMaterial, m_Sphere,
					glm::scale(transform, glm::vec3(body.Radius * 2.0f)));
			}
		}

		Egss::Renderer::EndScene();
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
			{
				m_Camera.SetProjection(50.0f,
					(float)e.GetWidth() / (float)e.GetHeight(), 0.1f, 200.0f);
			}
			return false;
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_SPACE) SpawnBody();
			if (e.GetKeyCode() == EGSS_KEY_P)     m_Paused = !m_Paused;
			if (e.GetKeyCode() == EGSS_KEY_R)     BuildScene();

			return false;
		});
	}

	void OnDemoImGui() override
	{
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Physics3D");

		ImGui::Text("WASD/QE move    arrows look");
		ImGui::Text("Space spawn     P pause    R rebuild");

		ImGui::Separator();
		ImGui::Text("Bodies: %zu  (%u awake)", m_World.GetBodyCount(), m_World.GetAwakeBodyCount());
		ImGui::Text("Contacts: %zu", m_World.GetContacts().size());

		size_t points = 0;
		for (const Egss::Contact3D& contact : m_World.GetContacts())
			points += (size_t)contact.PointCount;
		ImGui::Text("Contact points: %zu", points);

		// What the broadphase saved, against the every-pair figure it replaced.
		// Below the body threshold the grid is not built at all -- it costs
		// more than it saves down there -- so this reads "brute force" for the
		// default scene and switches over if you raise Max bodies past it.
		size_t bodies = m_World.GetBodyCount();
		size_t allPairs = bodies > 1 ? bodies * (bodies - 1) / 2 : 0;
		bool gridActive = m_World.GetBroadphaseCellCount() > 0;

		ImGui::Text("Pairs tested: %u of %zu  (%s)",
			m_World.GetBroadphaseCandidates(), allPairs,
			gridActive ? "grid" : "brute force");

		if (gridActive)
			ImGui::Text("Grid cells: %u", m_World.GetBroadphaseCellCount());

		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);

		ImGui::Separator();
		ImGui::SliderFloat("Gravity", &m_Gravity, -30.0f, 0.0f);
		ImGui::SliderFloat("Bounciness", &m_Bounciness, 0.0f, 0.9f);
		ImGui::SliderFloat("Spawn/sec", &m_SpawnRate, 0.0f, 12.0f);
		ImGui::SliderInt("Max bodies", &m_MaxBodies, 10, 300);
		ImGui::SliderFloat("Camera speed", &m_CameraSpeed, 1.0f, 20.0f);
		ImGui::Checkbox("Allow sleeping", &m_World.AllowSleeping);

		// TRY: drop this to 2 and watch the stack sag -- the lower contacts
		// are solved before the ones above them and never catch up.
		int iterations = (int)m_World.VelocityIterations;
		if (ImGui::SliderInt("Velocity iterations", &iterations, 1, 24))
			m_World.VelocityIterations = (unsigned int)iterations;

		if (ImGui::Button("Rebuild"))
			BuildScene();
		ImGui::SameLine();
		if (ImGui::Button("Drop stack"))
			BuildStack();

		ImGui::End();
	}

private:
	void BuildShader()
	{
		std::string vertexSrc = R"(
			#version 330 core

			layout(location = 0) in vec3 a_Position;
			layout(location = 1) in vec3 a_Normal;
			layout(location = 2) in vec2 a_TexCoord;

			uniform mat4 u_ViewProjection;
			uniform mat4 u_Transform;

			out vec3 v_WorldPosition;
			out vec3 v_Normal;

			void main()
			{
				vec4 world = u_Transform * vec4(a_Position, 1.0);

				v_WorldPosition = world.xyz;
				// Correct here only because every scale in this demo is
				// uniform per axis pair... which it is not, for boxes. The
				// inverse transpose is what a non-uniform scale needs, and a
				// stretched box's lighting is visibly wrong without it.
				v_Normal = mat3(transpose(inverse(u_Transform))) * a_Normal;

				gl_Position = u_ViewProjection * world;
			}
		)";

		std::string fragmentSrc = R"(
			#version 330 core

			layout(location = 0) out vec4 color;

			in vec3 v_WorldPosition;
			in vec3 v_Normal;

			uniform vec4 u_Color;
			uniform vec3 u_LightPosition;
			uniform vec3 u_LightColor;
			uniform vec3 u_CameraPosition;
			uniform float u_AmbientStrength;

			void main()
			{
				vec3 normal = normalize(v_Normal);

				vec3 lightVector = u_LightPosition - v_WorldPosition;
				float lightDistance = length(lightVector);
				vec3 toLight = lightVector / max(lightDistance, 0.0001);

				float attenuation = 1.0 / (1.0 + 0.015 * lightDistance * lightDistance);
				float diffuse = max(dot(normal, toLight), 0.0);

				vec3 toEye   = normalize(u_CameraPosition - v_WorldPosition);
				vec3 halfway = normalize(toLight + toEye);
				float specular = pow(max(dot(normal, halfway), 0.0), 32.0);

				vec3 base = u_Color.rgb;
				vec3 lit  = base * u_AmbientStrength
				          + base * diffuse * u_LightColor * attenuation
				          + specular * u_LightColor * attenuation * 0.25;

				color = vec4(lit, 1.0);
			}
		)";

		m_Shader.reset(Egss::Shader::Create("Physics3D", vertexSrc, fragmentSrc));
		Egss::Renderer::GetShaderLibrary().Add(m_Shader);

		m_SceneMaterial = Egss::Material::Create(m_Shader);
	}

private:
	Egss::PerspectiveCamera m_Camera;
	Egss::PhysicsWorld3D m_World;

	std::shared_ptr<Egss::Shader> m_Shader;
	std::shared_ptr<Egss::Material> m_SceneMaterial;
	std::shared_ptr<Egss::Mesh> m_Cube;
	std::shared_ptr<Egss::Mesh> m_Sphere;
	std::shared_ptr<Egss::Mesh> m_Cylinder;

	unsigned int m_StaticCount = 0;
	int m_SpawnCounter = 0;
	float m_SpawnAccumulator = 0.0f;

	bool m_Paused = false;
	float m_Gravity = -9.81f;
	float m_Bounciness = 0.15f;
	float m_SpawnRate = 3.0f;
	int m_MaxBodies = 90;
	float m_CameraSpeed = 6.0f;

	float m_FrameTime = 0.0f;
};
