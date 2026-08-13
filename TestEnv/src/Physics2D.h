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

class Physics2D : public DemoLayer
{
public:
	Physics2D()
		: DemoLayer("Physics2D"), m_Camera(-1.6f, 1.6f, -0.9f, 0.9f)
	{
		// The panel's simulation knobs, so a session recorded while they were
		// moved replays as the session that was recorded. Everything here feeds
		// the world or what is spawned into it; the drawing and audio sliders
		// beside them deliberately do not, because they change the picture
		// rather than the physics.
		RegisterParam("Gravity", &m_Gravity);
		RegisterParam("Bounciness", &m_Bounciness);
		RegisterParam("Spawn rate", &m_SpawnRate);
		RegisterParam("Max bodies", &m_MaxBodies);
		RegisterParam("Spawn crates", &m_SpawnCrates);
		RegisterParam("Allow sleeping", &m_World.AllowSleeping);
	}

	void OnDemoAttach() override
	{
		BuildScene();
		m_ImpactClip = MakeImpactClip();
		m_ToneClip = MakeToneClip();
		// Deliberately not started here. OnAttach runs for every layer that
		// was pushed, whichever demo is selected, so starting a looping sound
		// here means it plays forever behind whatever else you switch to.
		// UpdateOcclusion starts it once this demo is actually running.
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

	// A steady tone, so occlusion is audible as a change in timbre rather
	// than something you have to catch during a one-shot.
	static std::shared_ptr<Egss::AudioClip> MakeToneClip()
	{
		const unsigned int rate = Egss::AudioEngine::GetSampleRate();
		const float seconds = 1.0f;

		// A whole number of cycles, so the loop point is seamless.
		float cycles = std::round(330.0f * seconds);
		unsigned int frames = (unsigned int)(seconds * rate);

		std::vector<float> samples(frames);
		for (unsigned int i = 0; i < frames; i++)
		{
			float t = (float)i / (float)frames;
			// Harmonics matter here: a pure sine has nothing above its
			// fundamental for the occlusion filter to remove, so it would
			// barely change. Real sounds are not pure sines.
			float phase = glm::two_pi<float>() * cycles * t;
			samples[i] = (std::sin(phase) * 0.5f
				+ std::sin(phase * 3.0f) * 0.25f
				+ std::sin(phase * 7.0f) * 0.15f) * 0.6f;
		}

		return Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
	}

	// Continuous things belong on the activation edges, not in OnDemoAttach.
	void OnDemoActivated() override { StartEmitter(); }

	void OnDemoDeactivated() override
	{
		Egss::AudioEngine::Stop(m_Emitter);
		m_Emitter = Egss::InvalidVoice;
	}

	void StartEmitter()
	{
		Egss::AudioEngine::Stop(m_Emitter);

		Egss::Audio3DParams params;
		params.Position = { m_EmitterPosition.x, m_EmitterPosition.y, 0.0f };
		params.Volume = 0.45f;
		params.Loop = true;
		params.MinDistance = m_ListenerMinDistance;
		params.MaxDistance = m_ListenerMaxDistance;
		params.DopplerFactor = 0.0f;

		m_Emitter = Egss::AudioEngine::PlayAt(m_ToneClip, params);
	}

	// A reverb zone is just a region the game tests the listener against. The
	// engine crossfades between whatever settings it is handed, so stepping
	// over the boundary is a change of room rather than a click -- there is no
	// need for the zone itself to know anything about fading.
	void UpdateReverbZone()
	{
		glm::vec2 offset = glm::abs(m_ListenerPosition - m_ZoneCentre);
		m_ListenerInZone = offset.x <= m_ZoneHalfExtents.x && offset.y <= m_ZoneHalfExtents.y;

		Egss::ReverbSettings settings;
		if (m_ListenerInZone)
		{
			settings.Wet = m_ZoneWet;
			settings.RoomSize = m_ZoneRoomSize;
			settings.Damping = m_ZoneDamping;
			settings.Width = 1.0f;
		}
		else
		{
			// Outside, the space is open: no reverb at all.
			settings.Wet = 0.0f;
		}

		Egss::AudioEngine::SetReverb(settings);
	}

	// Occlusion is a game question, not an audio one -- the engine has no idea
	// what counts as an obstruction. Three rays rather than one gives a graded
	// answer instead of a hard on/off, so a source edging behind a body fades
	// rather than snapping.
	void UpdateOcclusion()
	{
		if (!Egss::AudioEngine::IsPlaying(m_Emitter))
			StartEmitter();

		glm::vec2 delta = m_EmitterPosition - m_ListenerPosition;
		float distance = glm::length(delta);

		if (distance < 0.0001f)
		{
			m_Occlusion = 0.0f;
			Egss::AudioEngine::SetVoiceOcclusion(m_Emitter, 0.0f);
			return;
		}

		glm::vec2 direction = delta / distance;
		glm::vec2 perpendicular = { -direction.y, direction.x };

		int blocked = 0;
		for (int i = 0; i < 3; i++)
		{
			glm::vec2 offset = perpendicular * (float)(i - 1) * 0.05f;
			m_OcclusionRayHit[i] = m_World.Raycast(m_ListenerPosition + offset, direction, distance).Hit;
			blocked += m_OcclusionRayHit[i] ? 1 : 0;
		}

		m_Occlusion = (float)blocked / 3.0f;
		Egss::AudioEngine::SetVoiceOcclusion(m_Emitter, m_Occlusion);
	}

	void BuildScene()
	{
		m_World.Clear();

		// Static geometry: floor and two walls. Static bodies are just bodies
		// with zero inverse mass, so the solver needs no special case.
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 0.0f, -0.82f }, { 1.5f, 0.06f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ -1.45f, 0.0f }, { 0.06f, 0.9f }));
		m_World.AddBody(Egss::RigidBody2D::MakeStaticBox({ 1.45f, 0.0f }, { 0.06f, 0.9f }));

		// A ramp. This used to be a staircase of axis-aligned boxes, because
		// the solver had no rotation and a slope was not expressible -- it is
		// the single best thing to point a new collider at, since a body on a
		// slope exercises the contact normal, friction and the angular terms
		// all at once and disagrees visibly when any of them is wrong.
		Egss::RigidBody2D ramp = Egss::RigidBody2D::MakeStaticBox(
			{ -0.70f, -0.45f }, { 0.58f, 0.035f });
		ramp.Rotation = glm::radians(-22.0f);   // descending to the right
		ramp.Friction = 0.55f;
		m_World.AddBody(ramp);

		// A second, shallower one below it, so something that leaves the first
		// has somewhere to go.
		Egss::RigidBody2D lower = Egss::RigidBody2D::MakeStaticBox(
			{ 0.35f, -0.66f }, { 0.45f, 0.035f });
		lower.Rotation = glm::radians(9.0f);
		lower.Friction = 0.55f;
		m_World.AddBody(lower);

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

		// Every third body is a crate, dropped already tilted so it lands on a
		// corner. That is the case the whole rotation exercise was for: it
		// tips, tumbles down the ramp and settles flush against it, and none
		// of those three steps is possible without the angular terms.
		if (m_SpawnCrates && m_SpawnCounter % 3 == 2)
		{
			float half = 0.05f + 0.015f * std::abs(std::cos(m_SpawnCounter * 1.7f));

			Egss::RigidBody2D crate = Egss::RigidBody2D::MakeBox(
				{ x, 0.75f }, { half, half }, 1.0f);
			crate.Rotation = 0.6f * std::sin(m_SpawnCounter * 3.1f);
			// A little spin on the way in, so it is obvious the rotation is
			// integrated and not just drawn.
			crate.AngularVelocity = 2.5f * std::cos(m_SpawnCounter * 2.2f);
			crate.Restitution = m_Bounciness * 0.4f;
			crate.Friction = 0.5f;

			m_World.AddBody(crate);
			m_SpawnCounter++;
			return;
		}

		Egss::RigidBody2D circle = Egss::RigidBody2D::MakeCircle(
			{ x, 0.75f }, 0.045f + 0.02f * std::abs(std::cos(m_SpawnCounter)), 1.0f);
		circle.Restitution = m_Bounciness;
		circle.Friction = 0.3f;
		// Discs roll on the ramp rather than sliding, which needs the friction
		// impulse to reach the contact point rather than the centre.
		circle.AngularDamping = 0.02f;

		m_World.AddBody(circle);
		m_SpawnCounter++;
	}

	// Simulation. The world wants a fixed dt, which is precisely what this
	// callback provides -- the two were built for each other.
	void OnDemoFixedUpdate(Egss::Timestep fixedStep) override
	{
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

		// The listener sits slightly in front of the plane looking at it, so
		// sources are never at exactly zero distance.
		Egss::AudioListener listener;
		listener.Position = { m_ListenerPosition.x, m_ListenerPosition.y, 1.0f };
		listener.Forward = { 0.0f, 0.0f, -1.0f };
		listener.Up = { 0.0f, 1.0f, 0.0f };
		Egss::AudioEngine::SetListener(listener);

		m_World.Gravity = { 0.0f, m_Gravity };
		m_World.Step(fixedStep);

		PlayImpactSounds();
		UpdateOcclusion();
		UpdateReverbZone();
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

			// Summed over the contact's points. A box landing flat produces two
			// of them sharing one total, so reading a single point's impulse
			// would report a face-on landing as half as hard as it was.
			float impulse = contact.TotalNormalImpulse();
			if (impulse < m_ImpactThreshold)
				continue;

			// Positioned in the world rather than panned by hand: the engine
			// works out gain and pan from where the listener is, so moving the
			// listener slider below changes what you hear without this code
			// knowing anything about it.
			Egss::Audio3DParams params;
			params.Position = { contact.Point.x, contact.Point.y, 0.0f };
			params.Volume = std::min(impulse * 1.6f, 1.0f) * m_ImpactVolume;
			// Vary the pitch so repeated hits don't sound mechanical.
			params.Pitch = 0.85f + 0.3f * std::abs(std::sin((float)m_SpawnCounter * 12.9898f));
			params.MinDistance = m_ListenerMinDistance;
			params.MaxDistance = m_ListenerMaxDistance;
			// Impacts are instantaneous; Doppler on a one-shot this short is
			// not worth the confusion.
			params.DopplerFactor = 0.0f;

			Egss::AudioEngine::PlayAt(m_ImpactClip, params);
		}

		m_PreviousContacts.swap(m_CurrentContacts);
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{
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

			// Rotation interpolates exactly as position does. Both are stored
			// per step for this, and a body drawn at its newest angle while
			// its position lags reads as jitter.
			float rotation = glm::mix(body.PreviousRotation, body.Rotation, alpha);

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
				// Degrees, not radians: DrawRotatedQuad converts, and the
				// physics works in radians throughout. Passing radians here
				// draws a box turned about a seventh of the way it should be,
				// which looks like a solver bug rather than a units mistake.
				Egss::Renderer2D::DrawRotatedQuad(position, body.HalfExtents * 2.0f,
					glm::degrees(rotation), color);
			}
			else
			{
				// An actual circle. This drew a quad standing in for one until
				// rotation arrived, behind a comment saying there was no
				// circle primitive -- there is, and once discs started turning
				// the stand-in became a lie: a square spinning on a slope
				// reads as a tumbling crate, not a rolling ball. The debug
				// spoke is what shows the spin now that the fill is round.
				Egss::Renderer2D::DrawCircle(position, body.Radius, color);
			}
		}

		if (m_ShowColliders)
			DrawDebug(alpha);

		if (m_ShowRays)
			DrawRayFan();

		// Where the ears are.
		Egss::Renderer2D::DrawRect(m_ListenerPosition, { 0.07f, 0.07f },
			glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));

		// The reverb zone, brighter while the listener is inside it.
		glm::vec4 zoneColor = m_ListenerInZone
			? glm::vec4(0.55f, 0.45f, 1.0f, 1.0f)
			: glm::vec4(0.30f, 0.25f, 0.55f, 1.0f);
		Egss::Renderer2D::DrawRect(m_ZoneCentre, m_ZoneHalfExtents * 2.0f, zoneColor);

		// The emitter, and the three rays deciding how muffled it sounds.
		Egss::Renderer2D::DrawRect(m_EmitterPosition, { 0.09f, 0.09f },
			glm::vec4(0.4f, 1.0f, 0.6f, 1.0f));

		glm::vec2 delta = m_EmitterPosition - m_ListenerPosition;
		float distance = glm::length(delta);
		if (distance > 0.0001f)
		{
			glm::vec2 direction = delta / distance;
			glm::vec2 perpendicular = { -direction.y, direction.x };

			for (int i = 0; i < 3; i++)
			{
				glm::vec2 offset = perpendicular * (float)(i - 1) * 0.05f;
				glm::vec4 color = m_OcclusionRayHit[i]
					? glm::vec4(1.0f, 0.3f, 0.3f, 1.0f)
					: glm::vec4(0.4f, 1.0f, 0.6f, 1.0f);

				Egss::Renderer2D::DrawLine(m_ListenerPosition + offset,
					m_EmitterPosition + offset, color);
			}
		}
		DrawCircleOutline(m_ListenerPosition, m_ListenerMaxDistance,
			glm::vec4(1.0f, 0.85f, 0.2f, 0.25f));

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
			float rotation = glm::mix(body.PreviousRotation, body.Rotation, alpha);

			glm::vec4 outline = body.Awake ? glm::vec4(0.2f, 0.9f, 0.5f, 1.0f)
			                               : glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);

			if (body.Shape == Egss::ColliderShape::Box)
			{
				DrawBoxOutline(position, body.HalfExtents, rotation, outline);
			}
			else
			{
				DrawCircleOutline(position, body.Radius, outline);

				// A spoke, because a circle's outline gives no way to see it
				// turning. Watching this line roll is the quickest check that
				// a disc on the ramp is rolling and not sliding.
				Egss::Renderer2D::DrawLine(position,
					position + glm::vec2(std::cos(rotation), std::sin(rotation)) * body.Radius,
					outline);
			}

			// Velocity, so a body behaving oddly shows why.
			if (body.Type != Egss::BodyType::Static && body.Awake)
			{
				Egss::Renderer2D::DrawLine(position, position + body.Velocity * 0.08f,
					glm::vec4(1.0f, 0.55f, 0.2f, 1.0f));
			}
		}

		// Contact normals. When a stack misbehaves this is the first place to
		// look -- a normal pointing the wrong way is instantly visible.
		//
		// Every point, not just the representative one: a face contact has two
		// and the difference between one and two is exactly what stops a crate
		// rocking, so seeing which one you got is worth the extra lines.
		for (const Egss::Contact& contact : m_World.GetContacts())
		{
			for (int p = 0; p < contact.PointCount; p++)
			{
				const glm::vec2& point = contact.Points[p].Position;

				Egss::Renderer2D::DrawLine(point, point + contact.Normal * 0.08f,
					glm::vec4(1.0f, 0.9f, 0.3f, 1.0f));

				// A tick across the normal, so a single point is
				// distinguishable from two that nearly coincide.
				glm::vec2 tangent = { -contact.Normal.y, contact.Normal.x };
				Egss::Renderer2D::DrawLine(point - tangent * 0.012f, point + tangent * 0.012f,
					glm::vec4(1.0f, 0.6f, 0.2f, 1.0f));
			}
		}
	}

	// Renderer2D::DrawRect is axis-aligned, so an oriented collider has to be
	// walked out of its own corners. The same order Sat2D uses.
	void DrawBoxOutline(const glm::vec2& centre, const glm::vec2& halfExtents,
		float rotation, const glm::vec4& color)
	{
		glm::vec2 axisX = { std::cos(rotation), std::sin(rotation) };
		glm::vec2 axisY = { -std::sin(rotation), std::cos(rotation) };

		glm::vec2 x = axisX * halfExtents.x;
		glm::vec2 y = axisY * halfExtents.y;

		glm::vec2 corners[4] = {
			centre - x - y, centre + x - y, centre + x + y, centre - x + y
		};

		for (int i = 0; i < 4; i++)
			Egss::Renderer2D::DrawLine(corners[i], corners[(i + 1) % 4], color);
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

	void OnDemoEvent(Egss::Event& e) override
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
			if (e.GetRepeatCount() > 0)
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

	void OnDemoImGui() override
	{

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
		ImGui::SliderFloat2("Listener", &m_ListenerPosition.x, -1.4f, 1.4f);
		ImGui::SliderFloat2("Emitter", &m_EmitterPosition.x, -1.4f, 1.4f);

		Egss::VoiceDebug debug;
		if (Egss::AudioEngine::GetVoiceDebug(m_Emitter, debug))
		{
			ImGui::Text("emitter: %.2fm  gain %.2f  pan %+.2f", debug.Distance, debug.Gain, debug.Pan);
			ImGui::Text("occlusion: %.2f requested, %.2f applied", m_Occlusion, debug.Occlusion);
		}
		else
		{
			ImGui::TextDisabled("emitter: not playing");
		}
		ImGui::SliderFloat("Hearing range", &m_ListenerMaxDistance, 0.5f, 8.0f);

		ImGui::Separator();
		ImGui::Text("Reverb zone: %s", m_ListenerInZone ? "inside" : "outside");
		ImGui::Text("applied wet %.3f", Egss::AudioEngine::GetReverb().Wet);
		ImGui::SliderFloat2("Zone centre", &m_ZoneCentre.x, -1.4f, 1.4f);
		ImGui::SliderFloat("Zone wet", &m_ZoneWet, 0.0f, 1.0f);
		ImGui::SliderFloat("Zone room size", &m_ZoneRoomSize, 0.0f, 0.95f);
		ImGui::SliderFloat("Zone damping", &m_ZoneDamping, 0.0f, 1.0f);

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
		// TRY: off, for a pile of discs only -- they roll down the ramp and
		// come to rest against the wall. On, and every third body is a crate
		// that lands on a corner and tips flush against the slope.
		ImGui::Checkbox("Spawn crates", &m_SpawnCrates);

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
	bool m_SpawnCrates = true;

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
	glm::vec2 m_ListenerPosition = { -1.0f, 0.35f };
	glm::vec2 m_EmitterPosition = { 1.1f, -0.3f };
	std::shared_ptr<Egss::AudioClip> m_ToneClip;
	Egss::VoiceHandle m_Emitter = Egss::InvalidVoice;
	float m_Occlusion = 0.0f;

	glm::vec2 m_ZoneCentre = { 0.85f, -0.1f };
	glm::vec2 m_ZoneHalfExtents = { 0.5f, 0.55f };
	float m_ZoneWet = 0.65f;
	float m_ZoneRoomSize = 0.85f;
	float m_ZoneDamping = 0.25f;
	bool m_ListenerInZone = false;
	bool m_OcclusionRayHit[3] = { false, false, false };
	float m_ListenerMinDistance = 0.35f;
	float m_ListenerMaxDistance = 3.0f;
	float m_ImpactThreshold = 0.05f;

	float m_FrameTime = 0.0f;
};
