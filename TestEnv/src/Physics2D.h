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

// glm::two_pi -- do not rely on another header pulling this in.
#include <glm/gtc/constants.hpp>

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
		m_ImpactClip = MakeImpactClip();
	}

	// Synthesised rather than loaded, so the sandbox still needs no asset
	// files. A sine that drops in pitch under an exponential decay is about
	// the cheapest thing that reads as an impact rather than a beep.
	static std::shared_ptr<Egss::AudioClip> MakeImpactClip()
	{
		const unsigned int rate = Egss::AudioEngine::GetSampleRate();
		const float seconds = 0.14f;
		const unsigned int frames = (unsigned int)(seconds * rate);

		std::vector<float> samples(frames);
		float phase = 0.0f;

		for (unsigned int i = 0; i < frames; i++)
		{
			float t = (float)i / (float)rate;
			float envelope = std::exp(-26.0f * t);

			// Sweeping down from 220Hz to about 70Hz over the decay.
			float frequency = 220.0f * std::exp(-8.0f * t) + 70.0f;
			phase += glm::two_pi<float>() * frequency / (float)rate;

			samples[i] = std::sin(phase) * envelope * 0.7f;
		}

		return Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
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

		PlayImpactSounds();
	}

	// A contact carries the total impulse the solver applied along its normal,
	// which is exactly "how hard did these two hit" -- so it drives both
	// whether a sound plays and how loud it is.
	//
	// Only *new* contacts count. A body resting on the floor has a large
	// normal impulse every single step (it is holding up the weight), and
	// playing that would be a permanent buzz.
	void PlayImpactSounds()
	{
		m_CurrentContacts.clear();

		for (const Egss::Contact& contact : m_World.GetContacts())
		{
			unsigned long long key = ((unsigned long long)contact.A << 32) | contact.B;
			m_CurrentContacts.insert(key);

			if (m_PreviousContacts.find(key) != m_PreviousContacts.end())
				continue;

			if (contact.NormalImpulse < m_ImpactThreshold)
				continue;

			Egss::AudioParams params;
			params.Volume = std::min(contact.NormalImpulse * 1.6f, 1.0f) * m_ImpactVolume;
			// Pan from where it happened -- the cheapest possible positional
			// audio, and a preview of what a real listener will do.
			params.Pan = std::min(std::max(contact.Point.x / 1.45f, -1.0f), 1.0f);
			// Vary the pitch so repeated hits don't sound mechanical.
			params.Pitch = 0.85f + 0.3f * std::abs(std::sin((float)m_SpawnCounter * 12.9898f));

			Egss::AudioEngine::Play(m_ImpactClip, params);
		}

		m_PreviousContacts.swap(m_CurrentContacts);
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

		if (m_ShowRays)
			DrawRayFan();

		Egss::Renderer2D::EndScene();
	}

	// A ring of rays from one point, stopping at whatever they hit. This is
	// the same query audio occlusion will use -- cast from the listener to the
	// source and see what is in the way -- so it is worth being able to watch
	// it work.
	void DrawRayFan()
	{
		const float maxDistance = 2.0f;

		for (int i = 0; i < m_RayCount; i++)
		{
			float angle = (float)i / (float)m_RayCount * glm::two_pi<float>();
			glm::vec2 direction = { std::cos(angle), std::sin(angle) };

			Egss::RaycastHit hit = m_World.Raycast(m_RayOrigin, direction, maxDistance);

			if (hit.Hit)
			{
				// Fade with distance, so the shape of what is reachable reads
				// at a glance.
				float brightness = 1.0f - hit.Fraction * 0.7f;
				Egss::Renderer2D::DrawLine(m_RayOrigin, hit.Point,
					glm::vec4(0.95f * brightness, 0.85f * brightness, 0.35f * brightness, 1.0f));

				// Surface normal at the hit.
				Egss::Renderer2D::DrawLine(hit.Point, hit.Point + hit.Normal * 0.04f,
					glm::vec4(1.0f, 0.35f, 0.35f, 1.0f));
			}
			else
			{
				Egss::Renderer2D::DrawLine(m_RayOrigin, m_RayOrigin + direction * maxDistance,
					glm::vec4(0.22f, 0.22f, 0.16f, 1.0f));
			}
		}
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
		ImGui::Text("Audio: %s   %u/%u voices", Egss::AudioEngine::GetBackendName(),
			Egss::AudioEngine::GetActiveVoiceCount(), Egss::AudioEngine::GetMaxVoices());

		float master = Egss::AudioEngine::GetMasterVolume();
		if (ImGui::SliderFloat("Master volume", &master, 0.0f, 1.0f))
			Egss::AudioEngine::SetMasterVolume(master);

		ImGui::SliderFloat("Impact volume", &m_ImpactVolume, 0.0f, 1.0f);
		ImGui::SliderFloat("Impact threshold", &m_ImpactThreshold, 0.0f, 0.5f);

		ImGui::Separator();
		ImGui::Checkbox("Show colliders", &m_ShowColliders);
		ImGui::Checkbox("Ray fan", &m_ShowRays);
		if (m_ShowRays)
		{
			ImGui::SliderInt("Rays", &m_RayCount, 8, 512);
			ImGui::SliderFloat2("Ray origin", &m_RayOrigin.x, -1.4f, 1.4f);
			// Each ray is tested against every body, so this is the quickest
			// way to make the profiler's Physics::Raycast row move.
			ImGui::TextDisabled("%d rays x %zu bodies per frame",
				m_RayCount, m_World.GetBodyCount());
		}
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

	bool m_ShowRays = true;
	int m_RayCount = 96;
	glm::vec2 m_RayOrigin = { -0.2f, 0.45f };

	float m_Gravity = -9.81f;
	float m_Bounciness = 0.45f;
	float m_SpawnRate = 6.0f;
	int m_MaxBodies = 150;

	std::shared_ptr<Egss::AudioClip> m_ImpactClip;
	std::unordered_set<unsigned long long> m_PreviousContacts;
	std::unordered_set<unsigned long long> m_CurrentContacts;
	float m_ImpactVolume = 0.55f;
	float m_ImpactThreshold = 0.05f;

	float m_FrameTime = 0.0f;
};
