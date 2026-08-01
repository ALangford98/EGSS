#pragma once

// Rigid bodies falling under gravity, stacking, and bouncing.
//
// The world is standalone: it owns its bodies and knows nothing about the
// renderer. This layer does the joining-up -- it steps the world in
// OnFixedUpdate and reads body positions in OnUpdate -- which is exactly the
// job a scene/entity layer will take over later.
//
// Things marked TRY: are deliberate places to experiment.

#include <Egss.h>
#include <imgui.h>

#include "Demo.h"

class Physics2D : public Egss::Layer
{
public:
	Physics2D()
		: Layer("Physics2D"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
	}

	void OnAttach() override
	{
		BuildScene();
	}

	void BuildScene()
	{
		m_World.Clear();

		// Static geometry: floor and two walls. Static bodies are just bodies
		// with zero inverse mass, so the solver needs no special case.
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.0f, -0.82f }, { 1.5f, 0.06f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ -1.45f, 0.0f }, { 0.06f, 0.9f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 1.45f, 0.0f }, { 0.06f, 0.9f }));

		// A ramp would need rotation, which this solver doesn't have -- so a
		// staircase of static boxes instead.
		for (int i = 0; i < 4; i++)
		{
			m_World.AddBody(Egss::RigidBody2D::MakeStaticBox(
				{ -1.1f + i * 0.16f, -0.72f + i * 0.09f }, { 0.08f, 0.03f }));
		}

		m_StaticCount = (unsigned int)m_World.GetBodyCount();

		SpawnStack();
	}

	// A stack is the honest test: a single falling body works with almost any
	// solver, but boxes resting on each other only settle if restitution,
	// friction, position correction and iteration count are all sane.
	void SpawnStack()
	{
		for (int i = 0; i < 5; i++)
		{
			Egss::RigidBody2D box = Egss::RigidBody2D::MakeBox(
				{ 0.55f, -0.7f + i * 0.13f }, { 0.06f, 0.06f }, 1.0f);
			box.Restitution = 0.0f;
			box.Friction = 0.6f;
			m_World.AddBody(box);
		}
	}

	void SpawnCircle()
	{
		// Spread the spawn point so they don't stack into a perfect column,
		// which is a much easier case than a messy pile.
		float x = -0.9f + 0.37f * std::sin(m_SpawnCounter * 2.4f);

		Egss::RigidBody2D circle = Egss::RigidBody2D::MakeCircle(
			{ x, 0.75f }, 0.045f + 0.02f * std::abs(std::cos(m_SpawnCounter)), 1.0f);
		circle.Restitution = m_Bounciness;
		circle.Friction = 0.3f;

		m_World.AddBody(circle);
		m_SpawnCounter++;
	}

	// Simulation. The world wants a fixed dt, which is precisely what this
	// callback provides -- the two were built for each other.
	void OnFixedUpdate(Egss::Timestep fixedStep) override
	{
		if (g_ActiveDemo != Demo::Physics2D)
			return;

		if (m_Paused)
			return;

		if (m_SpawnRate > 0.0f)
		{
			m_SpawnAccumulator += fixedStep;
			while (m_SpawnAccumulator >= 1.0f / m_SpawnRate)
			{
				m_SpawnAccumulator -= 1.0f / m_SpawnRate;
				if (m_World.GetBodyCount() < (size_t)m_MaxBodies)
					SpawnCircle();
			}
		}

		m_World.Gravity = { 0.0f, m_Gravity };
		m_World.Step(fixedStep);
	}

	void OnUpdate(Egss::Timestep ts) override
	{
		if (g_ActiveDemo != Demo::Physics2D)
			return;

		m_FrameTime = ts.GetMilliseconds();

		// The world stores each body's position at the start of the step, so
		// rendering can sit between the last two states instead of snapping
		// to the newest one.
		float alpha = Egss::Application::Get().GetInterpolationAlpha();

		Egss::Renderer2D::ResetStats();
		Egss::RenderCommand::SetClearColor({ 0.06f, 0.06f, 0.08f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::BeginScene(m_Camera);

		const auto& bodies = m_World.GetBodies();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			const Egss::RigidBody2D& body = bodies[i];
			glm::vec2 position = glm::mix(body.PreviousPosition, body.Position, alpha);

			glm::vec4 color;
			if (body.Type == Egss::BodyType::Static)
				color = { 0.30f, 0.32f, 0.38f, 1.0f };
			else if (!body.Awake)
				// Asleep bodies are drawn dimmer, which makes it obvious when
				// a pile settles and stops costing anything.
				color = { 0.35f, 0.40f, 0.45f, 1.0f };
			else
				color = { 0.45f, 0.70f, 0.95f, 1.0f };

			if (body.Shape == Egss::ColliderShape::Box)
			{
				Egss::Renderer2D::DrawQuad(position, body.HalfExtents * 2.0f, color);
			}
			else
			{
				// No circle primitive -- a quad stands in for the fill, and
				// the debug outline below shows the true shape.
				Egss::Renderer2D::DrawQuad(position,
					{ body.Radius * 2.0f, body.Radius * 2.0f }, color);
			}
		}

		if (m_ShowColliders)
			DrawDebug(alpha);

		Egss::Renderer2D::EndScene();
	}

	void DrawDebug(float alpha)
	{
		const auto& bodies = m_World.GetBodies();

		for (const Egss::RigidBody2D& body : bodies)
		{
			glm::vec2 position = glm::mix(body.PreviousPosition, body.Position, alpha);
			glm::vec4 outline = body.Awake ? glm::vec4(0.2f, 0.9f, 0.5f, 1.0f)
			                               : glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);

			if (body.Shape == Egss::ColliderShape::Box)
				Egss::Renderer2D::DrawRect(position, body.HalfExtents * 2.0f, outline);
			else
				DrawCircleOutline(position, body.Radius, outline);

			// Velocity, so a body behaving oddly shows why.
			if (body.Type != Egss::BodyType::Static && body.Awake)
			{
				Egss::Renderer2D::DrawLine(position, position + body.Velocity * 0.08f,
					glm::vec4(1.0f, 0.55f, 0.2f, 1.0f));
			}
		}

		// Contact normals. When a stack misbehaves this is the first place to
		// look -- a normal pointing the wrong way is instantly visible.
		for (const Egss::Contact& contact : m_World.GetContacts())
		{
			Egss::Renderer2D::DrawLine(contact.Point, contact.Point + contact.Normal * 0.08f,
				glm::vec4(1.0f, 0.9f, 0.3f, 1.0f));
		}
	}

	// Renderer2D has no circle primitive, so this walks one out of line
	// segments. Cheap, and it only exists for debugging.
	void DrawCircleOutline(const glm::vec2& centre, float radius, const glm::vec4& color)
	{
		const int segments = 16;
		glm::vec2 previous = centre + glm::vec2(radius, 0.0f);

		for (int i = 1; i <= segments; i++)
		{
			float angle = (float)i / (float)segments * glm::two_pi<float>();
			glm::vec2 next = centre + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
			Egss::Renderer2D::DrawLine(previous, next, color);
			previous = next;
		}
	}

	void OnEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
			{
				float aspect = (float)e.GetWidth() / (float)e.GetHeight();
				m_Camera.SetProjection(-aspect * 0.9f, aspect * 0.9f, -0.9f, 0.9f);
			}
			return false;
		});

		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& e)
		{
			if (e.GetRepeatCount() > 0 || g_ActiveDemo != Demo::Physics2D)
				return false;

			if (e.GetKeyCode() == EGSS_KEY_SPACE)
				SpawnCircle();
			if (e.GetKeyCode() == EGSS_KEY_P)
				m_Paused = !m_Paused;
			if (e.GetKeyCode() == EGSS_KEY_R)
				BuildScene();

			return false;
		});
	}

	void OnImGuiRender() override
	{
		if (g_ActiveDemo != Demo::Physics2D)
			return;

		auto stats = Egss::Renderer2D::GetStats();
		Egss::Application& app = Egss::Application::Get();

		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Physics2D");

		ImGui::Text("Space  spawn      P  pause");
		ImGui::Text("R      rebuild");

		ImGui::Separator();
		ImGui::Text("Bodies: %zu  (%u awake)", m_World.GetBodyCount(), m_World.GetAwakeBodyCount());
		ImGui::Text("Contacts: %zu", m_World.GetContacts().size());
		ImGui::Text("Frame: %.2f ms (%.0f fps)", m_FrameTime,
			m_FrameTime > 0.0f ? 1000.0f / m_FrameTime : 0.0f);
		ImGui::Text("Sim steps this frame: %u", app.GetFixedStepsLastFrame());
		ImGui::Text("Draw calls: %u   Quads: %u   Lines: %u",
			stats.DrawCalls, stats.QuadCount, stats.LineCount);

		ImGui::Separator();
		ImGui::Checkbox("Show colliders", &m_ShowColliders);
		ImGui::Checkbox("Allow sleeping", &m_World.AllowSleeping);

		ImGui::SliderFloat("Gravity", &m_Gravity, -30.0f, 5.0f);
		ImGui::SliderFloat("Bounciness", &m_Bounciness, 0.0f, 0.95f);
		ImGui::SliderFloat("Spawn/sec", &m_SpawnRate, 0.0f, 30.0f);
		ImGui::SliderInt("Max bodies", &m_MaxBodies, 10, 400);

		// TRY: drop this to 1 and watch the stack sag -- one pass fixes the
		// bottom contact and the ones above it never catch up.
		int iterations = (int)m_World.VelocityIterations;
		if (ImGui::SliderInt("Solver iterations", &iterations, 1, 20))
			m_World.VelocityIterations = (unsigned int)iterations;

		if (ImGui::Button("Rebuild"))
			BuildScene();
		ImGui::SameLine();
		if (ImGui::Button("Drop stack"))
			SpawnStack();

		ImGui::End();
	}

private:
	Egss::OrthographicCamera m_Camera;
	Egss::PhysicsWorld2D m_World;

	unsigned int m_StaticCount = 0;
	int m_SpawnCounter = 0;
	float m_SpawnAccumulator = 0.0f;

	bool m_Paused = false;
	bool m_ShowColliders = true;

	float m_Gravity = -9.81f;
	float m_Bounciness = 0.45f;
	float m_SpawnRate = 6.0f;
	int m_MaxBodies = 150;

	float m_FrameTime = 0.0f;
};
