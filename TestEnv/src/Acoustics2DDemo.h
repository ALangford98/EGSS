#pragma once

// Sound as geometry.
//
// Everything you hear here comes from rays. They leave the source, bounce off
// the walls losing energy, and at every bounce ask whether the listener can be
// seen from there. Each yes is a path the sound could actually take: how long
// it took to arrive, how much survived the trip, and which direction it came
// from. Collect enough of them and you have the room's impulse response.
//
// That single trace drives three separate things:
//   - occlusion, from whether the direct line is blocked
//   - early reflections, from the loudest paths in the first 80 ms
//   - the reverb tail, from how fast the traced energy decays
//
// Worth playing with: move the source into the side chamber and listen to the
// direct sound disappear while the echoes keep arriving through the doorway.
// Then drop absorption to 0.05 and hear the room turn into a cathedral.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Demo.h"

class Acoustics2DDemo : public DemoLayer
{
public:
	Acoustics2DDemo()
		: DemoLayer("Acoustics2D"),
		  m_Camera(-m_Aspect * m_Zoom, m_Aspect * m_Zoom, -m_Zoom, m_Zoom)
	{
	}

	void OnDemoAttach() override
	{
		BuildRoom();
		m_Clip = MakeTone(180.0f, 1.0f);

		// Deliberately not started here: OnAttach runs for every pushed layer
		// whichever demo is showing, so a loop started here plays forever
		// underneath whatever else you switch to.
	}

	void OnDemoActivated() override
	{
		StartSource();
		Retrace();
	}

	void OnDemoDeactivated() override
	{
		Egss::AudioEngine::Stop(m_Voice);
		m_Voice = Egss::InvalidVoice;

		// The room this demo described is not the next demo's room.
		Egss::AudioEngine::SetReverb(Egss::ReverbSettings());
		Egss::AudioEngine::ClearReverbImpulse();
	}

	// ---------------------------------------------------------------------
	// The room. Static boxes, which is all a ray needs to bounce off.
	// ---------------------------------------------------------------------
	void BuildRoom()
	{
		m_World.Gravity = { 0.0f, 0.0f };
		m_World.Clear();

		// Absorption is how much a surface swallows; scattering is how much of
		// what it sends back has forgotten which way the sound arrived. They
		// are independent: polished marble absorbs almost nothing and
		// scatters almost nothing, a bookcase absorbs little and scatters a
		// lot, a curtain does both.
		auto wall = [this](glm::vec2 centre, glm::vec2 halfExtents,
			float absorption, float scattering)
		{
			unsigned int body = m_World.AddBody(
				Egss::RigidBody2D::MakeStaticBox(centre, halfExtents));

			if (m_Absorptions.size() <= body)
			{
				m_Absorptions.resize(body + 1, -1.0f);
				m_Scatterings.resize(body + 1, -1.0f);
			}

			m_Absorptions[body] = absorption;
			m_Scatterings[body] = scattering;
		};

		m_Absorptions.clear();
		m_Scatterings.clear();

		const float t = 0.4f;

		// Outer shell. The hard walls are the ones that make the tail. Flat
		// plaster, so they barely scatter -- which is what leaves a specular
		// trace with a comb for a tail.
		wall({ 0.0f, -7.0f }, { 14.0f, t }, 0.06f, 0.05f);   // floor
		wall({ 0.0f,  7.0f }, { 14.0f, t }, 0.06f, 0.05f);   // ceiling
		wall({ -14.0f, 0.0f }, { t, 7.0f }, 0.06f, 0.05f);   // left
		wall({ 14.0f, 0.0f }, { t, 7.0f }, 0.06f, 0.05f);    // right

		// A dividing wall with a doorway in it, so there is somewhere to be
		// out of sight without being out of earshot.
		wall({ 4.0f, -4.6f }, { t, 2.8f }, 0.10f, 0.15f);
		wall({ 4.0f,  4.6f }, { t, 2.8f }, 0.10f, 0.15f);

		// Soft furnishing: absorbs most of what reaches it, and kills the tail
		// far more than its size suggests. What little comes back off a porous
		// surface comes back diffusely.
		wall({ -9.0f, 4.0f }, { 3.0f, 1.2f }, 0.85f, 0.70f);

		// A couple of pillars to break up the specular orbits a bare rectangle
		// would otherwise fall into. Rough stone, so they scatter heavily --
		// geometry and roughness doing the same job by different means.
		wall({ -4.0f, -2.0f }, { 0.8f, 0.8f }, 0.15f, 0.60f);
		wall({ 8.5f,  1.5f }, { 0.9f, 0.9f }, 0.15f, 0.60f);

		if (!m_Corridor)
			return;

		// A blocked-off corridor wrapped around the bottom-left of the main
		// room: in at the top left, down the west wall, east along the south
		// wall, then north into a dead end against the divider. One mouth, and
		// no way through -- everything that goes in has to come back out of the
		// same opening.
		//
		// This is the geometry a room-sized mental model gets wrong. Sabine and
		// Eyring both assume a diffuse field, energy spread evenly through the
		// space; a long thin dead end fed through one opening is the standard
		// counterexample, because sound that gets in does not come back out
		// promptly and the tail stops being a single exponential. It is also
		// what punishes a small ray budget hardest: few rays find the mouth,
		// and those that do bounce many more times before they are heard, so
		// the corridor's contribution is estimated from a thin sample. Raise
		// the ray count with this on and watch the late tail settle down.
		//
		// Bare block -- harder than the plastered shell and rougher with it, so
		// it rings without ringing specularly.
		const float ca = 0.08f, cs = 0.12f;

		// Widths follow from the shell: the inner faces are at -13.6 and -6.6,
		// so a wall centred at -11.4 or -4.4 leaves 1.8 either side.
		wall({ -11.4f, -1.4f }, { t, 3.4f }, ca, cs);   // west side, up to the mouth at y = 2
		wall({ -5.4f, -4.4f }, { 6.4f, t }, ca, cs);    // south side, east to the riser
		wall({ 1.4f, -3.0f }, { t, 1.8f }, ca, cs);     // the riser, turning it north
		wall({ 2.4f, -1.6f }, { 1.4f, t }, ca, cs);     // the cap: the dead end

		// The cap runs to x = 3.8 and the divider's face is at 3.6, so the two
		// overlap rather than meeting on a seam. Boxes that merely touch leave a
		// zero-width crack, and a ray that finds one leaks into the main room --
		// which shows up as a corridor that is mysteriously not very reverberant.
	}

	// A tone with a little harmonic content, looping seamlessly: a whole
	// number of cycles in the buffer means the end lines up with the start.
	static std::shared_ptr<Egss::AudioClip> MakeTone(float frequency, float seconds)
	{
		const unsigned int rate = Egss::AudioEngine::GetSampleRate();

		float cycles = std::round(frequency * seconds);
		float exact = cycles / seconds;
		unsigned int frames = (unsigned int)std::round(cycles * rate / exact);

		std::vector<float> samples(frames);
		for (unsigned int i = 0; i < frames; i++)
		{
			float t = (float)i / (float)rate;
			float phase = glm::two_pi<float>() * exact * t;

			// Short repeating pulses rather than a steady drone: echoes are
			// only audible as echoes if there are gaps for them to arrive in.
			float envelope = std::pow(glm::max(0.0f, std::sin(glm::two_pi<float>() * 2.0f * t)), 8.0f);

			samples[i] = (std::sin(phase) * 0.7f + std::sin(phase * 2.0f) * 0.3f) * envelope * 0.5f;
		}

		return Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
	}

	void StartSource()
	{
		Egss::AudioEngine::Stop(m_Voice);

		Egss::Audio3DParams params;
		params.Position = { m_Source.x, m_Source.y, 0.0f };
		params.Volume = 0.7f;
		params.Loop = true;
		params.MinDistance = s_SourceMinDistance;
		// The trace decides how far the sound really carries; this only has to
		// be generous enough not to cut it off first.
		params.MaxDistance = 60.0f;
		params.DopplerFactor = 0.0f;

		m_Voice = Egss::AudioEngine::PlayAt(m_Clip, params);
	}

	// ---------------------------------------------------------------------
	// Tracing
	// ---------------------------------------------------------------------
	void Retrace()
	{
		Egss::AcousticsSettings settings;
		settings.RayCount = m_RayCount;
		settings.MaxBounces = m_MaxBounces;
		settings.Absorption = m_Absorption;
		settings.PerBodyAbsorption = m_UsePerBodyAbsorption ? &m_Absorptions : nullptr;
		settings.Scattering = m_Scattering;
		settings.PerBodyScattering = m_UsePerBodyAbsorption ? &m_Scatterings : nullptr;
		settings.MaxReflections = m_MaxTaps;
		// Must match the voice's MinDistance, or reflection gains and the
		// direct sound's attenuation are on two different scales and the
		// echoes come out systematically too loud or too quiet.
		settings.MinDistance = s_SourceMinDistance;

		for (int band = 0; band < Egss::AcousticBandCount; band++)
			settings.BandAbsorptionScale[band] = m_FrequencyDependent
				? m_BandScale[band] : 1.0f;

		// Measuring the decay needs the trace to run past 25 dB down; the
		// default budget stops well before that in a live room, which is why
		// the panel usually says "Sabine fallback".
		// Sized from the *previous* trace's estimate, since the budget has to
		// be chosen before the trace that would measure it. One frame of lag,
		// and the estimate barely moves between frames.
		float expectedTail = glm::max(m_Result.ReverbTime, m_Result.SabineTime);
		if (m_TraceFullTail && expectedTail > 0.0f)
		{
			// 1.6x the expected tail, so the decay fit has margin before the
			// truncation cliff it is told to stay clear of.
			settings.MaxPathLength = glm::min(expectedTail * 1.6f * settings.SpeedOfSound, 6000.0f);
			settings.MaxBounces = glm::min(600,
				(int)(settings.MaxPathLength / glm::max(m_Result.MeanFreePath, 1.0f)) + 8);
			settings.MinEnergy = 1e-14f;
		}

		m_Result = Egss::Acoustics2D::Trace(m_World, m_Source, m_Listener,
			settings, m_DrawRays ? &m_Rays : nullptr);

		m_LastTraceSource = m_Source;
		m_LastTraceListener = m_Listener;

		ApplyToMixer();
	}

	// Everything the mixer is told, it is told from the trace.
	void ApplyToMixer()
	{
		if (!Egss::AudioEngine::IsPlaying(m_Voice))
			return;

		Egss::AudioEngine::SetVoicePosition(m_Voice, { m_Source.x, m_Source.y, 0.0f });
		Egss::AudioEngine::SetVoiceOcclusion(m_Voice, m_ApplyOcclusion ? m_Result.Occlusion : 0.0f);

		// --- Early reflections ---
		m_Taps.clear();
		if (m_ApplyReflections)
		{
			for (const Egss::ReflectionPath& path : m_Result.Reflections)
			{
				Egss::AudioReflection tap;
				tap.Delay = path.Delay;
				tap.Gain = path.Gain * m_ReflectionGain;
				// The listener faces up the screen, so world X is left/right.
				tap.Pan = glm::clamp(path.Direction.x, -1.0f, 1.0f);
				m_Taps.push_back(tap);
			}
		}
		Egss::AudioEngine::SetVoiceReflections(m_Voice, m_Taps.data(), (unsigned int)m_Taps.size());

		// --- The tail ---
		// Two ways to make it, from the same trace. The parametric reverb is
		// told *about* the room; the convolution one is given the room.
		if (m_ApplyReverb && m_UseConvolution)
		{
			Egss::ImpulseSettings impulse;
			impulse.Density = m_TailDensity;
			impulse.Gain = m_TailGain;
			// Starts where the discrete early reflections stop, or the first
			// 80 ms would be played twice.
			impulse.StartSeconds = 0.08f;

			m_ImpulseTaps = Egss::Acoustics2D::BuildImpulseTaps(m_Result, impulse);
			Egss::AudioEngine::SetReverbImpulse(m_ImpulseTaps.data(),
				(unsigned int)m_ImpulseTaps.size());
		}
		else
		{
			Egss::AudioEngine::ClearReverbImpulse();
			m_ImpulseTaps.clear();
		}

		Egss::ReverbSettings reverb;
		if (m_ApplyReverb)
		{
			// RoomSize is comb feedback, which is not a time -- but it maps
			// monotonically onto one, and the traced RT60 is what should be
			// driving it. Two seconds of tail is about as long as this reverb
			// goes, so that is the top of the range.
			reverb.RoomSize = glm::clamp(m_Result.ReverbTime / 2.0f, 0.1f, 0.95f);
			// A room whose late energy is a large fraction of the direct sound
			// is a room you hear as wet.
			reverb.Wet = glm::clamp(m_Result.LateEnergyRatio * m_WetScale, 0.0f, 0.9f);
			// Soft rooms lose their top end fastest.
			reverb.Damping = glm::clamp(m_Result.MeanAbsorption * 1.5f, 0.1f, 0.9f);
			reverb.Width = 1.0f;
		}
		Egss::AudioEngine::SetReverb(reverb);
	}

	void OnDemoUpdate(Egss::Timestep ts) override
	{
		m_FrameTime = ts.GetMilliseconds();

		MoveThings(ts);

		// Tracing is milliseconds of work, so it happens when the geometry has
		// actually changed rather than every frame. Sound does not move fast
		// enough for the difference to be audible.
		float moved = glm::length(m_Source - m_LastTraceSource)
			+ glm::length(m_Listener - m_LastTraceListener);

		m_SinceTrace += ts;
		if (moved > m_RetraceDistance || m_SinceTrace > m_RetraceInterval || m_Dirty)
		{
			Retrace();
			m_SinceTrace = 0.0f;
			m_Dirty = false;
		}

		// The listener faces up the screen, so a reflection arriving from the
		// left is heard on the left.
		Egss::AudioListener listener;
		listener.Position = { m_Listener.x, m_Listener.y, 0.0f };
		listener.Forward = { 0.0f, 0.0f, -1.0f };
		listener.Up = { 0.0f, 1.0f, 0.0f };
		Egss::AudioEngine::SetListener(listener);

		Egss::RenderCommand::SetClearColor({ 0.05f, 0.05f, 0.07f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::ResetStats();
		Egss::Renderer2D::BeginScene(m_Camera);

		DrawRoom();
		if (m_DrawRays)
			DrawRays();
		DrawReflectionPaths();
		DrawMarkers();

		Egss::Renderer2D::EndScene();
	}

	void MoveThings(Egss::Timestep ts)
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
			return;

		// WASD drives the listener, because that is the thing whose point of
		// view you are hearing from.
		glm::vec2 move(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) move.x -= 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) move.x += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) move.y += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) move.y -= 1.0f;

		if (glm::dot(move, move) > 0.0f)
			m_Listener += glm::normalize(move) * m_MoveSpeed * (float)ts;

		// Arrows drive the source.
		glm::vec2 sourceMove(0.0f);
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  sourceMove.x -= 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) sourceMove.x += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    sourceMove.y += 1.0f;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  sourceMove.y -= 1.0f;

		if (glm::dot(sourceMove, sourceMove) > 0.0f)
			m_Source += glm::normalize(sourceMove) * m_MoveSpeed * (float)ts;

		// Neither may end up inside a wall, or every ray starts underground.
		m_Listener = m_World.ResolveCircle(m_Listener, 0.35f);
		m_Source = m_World.ResolveCircle(m_Source, 0.35f);
	}

	// ---------------------------------------------------------------------
	// Drawing
	// ---------------------------------------------------------------------
	void DrawRoom()
	{
		for (size_t i = 0; i < m_World.GetBodyCount(); i++)
		{
			const Egss::RigidBody2D& body = m_World.GetBody((unsigned int)i);

			// Shade by absorption, so what the room does to sound is visible
			// rather than something you have to remember.
			float absorption = (m_UsePerBodyAbsorption && i < m_Absorptions.size()
				&& m_Absorptions[i] >= 0.0f) ? m_Absorptions[i] : m_Absorption;
			float scattering = (m_UsePerBodyAbsorption && i < m_Scatterings.size()
				&& m_Scatterings[i] >= 0.0f) ? m_Scatterings[i] : m_Scattering;

			// Hard walls bright, soft ones dark: a mirror looks like a mirror.
			float shade = 0.22f + (1.0f - absorption) * 0.45f;

			// Scattering warms the surface, because it is not the same thing
			// as absorption and should not read as the same thing: the rough
			// pillars are as hard as the walls and look it, just warmer.
			glm::vec4 color(shade * (1.0f + scattering * 0.5f),
				shade * 0.98f, shade * (1.1f - scattering * 0.7f), 1.0f);

			Egss::Renderer2D::DrawQuad(glm::vec3(body.Position, -0.1f),
				body.HalfExtents * 2.0f, color);
		}
	}

	void DrawRays()
	{
		// Only a slice of them: 128 polylines is unreadable, and the point is
		// to see the pattern rather than count the rays.
		int step = glm::max(1, (int)m_Rays.size() / m_RaysDrawn);

		for (size_t i = 0; i < m_Rays.size(); i += step)
		{
			const Egss::TracedRay& ray = m_Rays[i];

			for (size_t p = 1; p < ray.Points.size(); p++)
			{
				// Fade along the path, so where the energy went is visible.
				float t = (float)p / (float)glm::max<size_t>(ray.Points.size() - 1, 1);
				float alpha = (1.0f - t) * 0.5f + 0.06f;

				glm::vec4 color = ray.Escaped
					? glm::vec4(0.9f, 0.35f, 0.3f, alpha * 0.6f)
					: glm::vec4(0.35f, 0.7f, 1.0f, alpha);

				Egss::Renderer2D::DrawLine(glm::vec3(ray.Points[p - 1], 0.0f),
					glm::vec3(ray.Points[p], 0.0f), color);
			}
		}
	}

	// Where each early reflection arrived from, as a spoke at the listener.
	// Length is how loud it was, so the strongest direction is obvious.
	void DrawReflectionPaths()
	{
		float loudest = 0.0f;
		for (const Egss::ReflectionPath& path : m_Result.Reflections)
			loudest = glm::max(loudest, path.Gain);

		if (loudest <= 0.0f)
			return;

		for (const Egss::ReflectionPath& path : m_Result.Reflections)
		{
			float length = 1.0f + 4.0f * (path.Gain / loudest);
			glm::vec2 tip = m_Listener + path.Direction * length;

			Egss::Renderer2D::DrawLine(glm::vec3(m_Listener, 0.1f), glm::vec3(tip, 0.1f),
				{ 1.0f, 0.85f, 0.35f, 0.9f });
		}
	}

	void DrawMarkers()
	{
		// The direct line, coloured by whether it actually gets through.
		glm::vec4 directColor = m_Result.DirectPathClear
			? glm::vec4(0.4f, 1.0f, 0.5f, 0.8f)
			: glm::vec4(1.0f, 0.35f, 0.35f, 0.6f);
		Egss::Renderer2D::DrawLine(glm::vec3(m_Source, 0.1f), glm::vec3(m_Listener, 0.1f), directColor);

		// How far the sound actually carries, which is the whole point of
		// tracing rather than picking a falloff radius.
		if (m_DrawRadius && m_Result.EffectiveRadius > 0.0f)
			DrawRing(m_Source, m_Result.EffectiveRadius, { 1.0f, 0.6f, 0.2f, 0.5f });

		Egss::Renderer2D::DrawQuad(glm::vec3(m_Source, 0.2f), glm::vec2(0.5f),
			{ 1.0f, 0.75f, 0.25f, 1.0f });
		Egss::Renderer2D::DrawQuad(glm::vec3(m_Listener, 0.2f), glm::vec2(0.5f),
			{ 0.4f, 0.9f, 1.0f, 1.0f });
	}

	void DrawRing(const glm::vec2& centre, float radius, const glm::vec4& color)
	{
		const int segments = 64;
		glm::vec2 previous = centre + glm::vec2(radius, 0.0f);

		for (int i = 1; i <= segments; i++)
		{
			float angle = glm::two_pi<float>() * (float)i / (float)segments;
			glm::vec2 point = centre + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
			Egss::Renderer2D::DrawLine(glm::vec3(previous, 0.05f), glm::vec3(point, 0.05f), color);
			previous = point;
		}
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);

		dispatcher.Dispatch<Egss::WindowResizeEvent>([this](Egss::WindowResizeEvent& e)
		{
			if (e.GetHeight() > 0)
			{
				m_Aspect = (float)e.GetWidth() / (float)e.GetHeight();
				m_Camera.SetProjection(-m_Aspect * m_Zoom, m_Aspect * m_Zoom, -m_Zoom, m_Zoom);
			}
			return false;
		});
	}

	void OnDemoImGui() override
	{
		ImGui::SetNextWindowPos(ImVec2(20.0f, 180.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(430.0f, 620.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Acoustics2D");

		ImGui::TextWrapped("WASD moves the listener (blue). Arrows move the source (orange). "
			"Everything you hear comes from the rays.");

		// --- What the trace found ---------------------------------------
		ImGui::SeparatorText("What the rays found");

		ImGui::Text("Direct: %.2f m   occlusion %.0f%%   %s",
			m_Result.DirectDistance, m_Result.Occlusion * 100.0f,
			m_Result.DirectPathClear ? "clear" : "blocked");

		ImGui::Text("RT60: %.2f s  (%s)", m_Result.ReverbTime,
			m_Result.ReverbTimeMeasured ? "measured from the decay" : "Sabine fallback");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Measuring needs the trace to run until the tail is 25 dB down.\n"
				"When it does not get that far it says so and falls back to the\n"
				"formula, rather than reporting where the trace stopped as decay.");

		ImGui::Text("Sabine estimate: %.2f s     mean absorption %.2f",
			m_Result.SabineTime, m_Result.MeanAbsorption);

		ImGui::Text("Per band RT60: %.2f / %.2f / %.2f s   (low / mid / high)",
			m_Result.BandReverbTime[0], m_Result.BandReverbTime[1], m_Result.BandReverbTime[2]);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Almost everything absorbs treble faster than bass, so the\n"
				"tail grows duller as it decays rather than just quieter.\n"
				"That difference is most of what separates a room from a\n"
				"reverb preset.");
		ImGui::Text("Mean free path: %.2f m      traced to %.2f s",
			m_Result.MeanFreePath, m_Result.TracedSeconds);
		ImGui::Text("Effective radius: %.1f m", m_Result.EffectiveRadius);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How far the sound actually carries, which a single falloff\n"
				"distance cannot express: much further along a corridor than\n"
				"across a padded room of the same size.");

		ImGui::Text("Late/direct energy: %.3f", m_Result.LateEnergyRatio);

		ImGui::Text("Tail roughness: %.2f dB", m_Result.TailRoughness);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How far the late echogram's bins sit from their own local\n"
				"average. A specular room's tail is a comb of spikes with gaps\n"
				"between them and reads around 3-4 dB; a diffuse tail is noise\n"
				"and reads under 1. Drag Scattering below and watch it fall.");

		ImGui::TextDisabled("%d paths, %d bounces (%d scattered), %d rays escaped",
			m_Result.PathsFound, m_Result.BouncesTraced,
			m_Result.BouncesScattered, m_Result.RaysEscaped);

		// --- The echogram ------------------------------------------------
		ImGui::SeparatorText("Echogram");

		if (!m_Result.Echogram.empty())
		{
			// The first half second, in dB. Linear energy is all spike and no
			// tail; the ear works in dB and so should the picture.
			int shown = glm::min((int)m_Result.Echogram.size(), 100);
			m_EchogramDb.resize(shown);

			float reference = glm::max(m_Result.Echogram[0], 1e-12f);
			for (int i = 0; i < shown; i++)
				m_EchogramDb[i] = glm::max(-60.0f,
					10.0f * std::log10(glm::max(m_Result.Echogram[i] / reference, 1e-12f)));

			ImGui::PlotHistogram("##echogram", m_EchogramDb.data(), shown, 0,
				"energy vs time, dB below direct", -60.0f, 0.0f, ImVec2(0.0f, 80.0f));
			ImGui::TextDisabled("0 to %.0f ms, %.0f ms per bar", shown * 5.0f, 5.0f);
		}

		// --- The taps actually being played ------------------------------
		ImGui::SeparatorText("Early reflections");

		if (m_Result.Reflections.empty())
		{
			ImGui::TextDisabled("None -- nothing bounced back to the listener.");
		}
		else
		{
			ImGui::Text("%zu taps, loudest first arriving:", m_Result.Reflections.size());
			for (size_t i = 0; i < m_Result.Reflections.size(); i++)
			{
				const Egss::ReflectionPath& path = m_Result.Reflections[i];
				ImGui::Text("  %4.1f ms  gain %.3f  pan %+.2f  %d bounce%s  %.1f m",
					path.Delay * 1000.0f, path.Gain, path.Direction.x,
					path.Bounces, path.Bounces == 1 ? "" : "s", path.PathLength);
			}
		}

		// --- Controls -----------------------------------------------------
		ImGui::SeparatorText("The room");

		if (ImGui::Checkbox("Blocked-off corridor", &m_Corridor))
		{
			// Not a trace setting: the geometry itself changes, and the body
			// handles the material tables are indexed by change with it.
			BuildRoom();
			m_Dirty = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("A dead-end corridor around the bottom-left of the main room,\n"
				"open only at the top left. Sabine and Eyring assume energy is\n"
				"spread evenly through the space, and a long dead end fed by one\n"
				"opening is the standard case where that stops being true --\n"
				"watch RT60 and the shape of the echogram's tail.\n"
				"It also wants more rays than the open room: few of them find\n"
				"the mouth, so the corridor's share of the tail is estimated\n"
				"from a thin sample until you raise the count.");

		ImGui::SeparatorText("Tracing");

		m_Dirty |= ImGui::SliderInt("Rays", &m_RayCount, 16, 1024);
		m_Dirty |= ImGui::SliderInt("Max bounces", &m_MaxBounces, 1, 40);
		m_Dirty |= ImGui::SliderFloat("Absorption", &m_Absorption, 0.01f, 0.95f);

		m_Dirty |= ImGui::SliderFloat("Scattering", &m_Scattering, 0.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much of a bounce leaves diffusely rather than in the\n"
				"mirror direction. At 0 a ray in a rectangular room only ever\n"
				"takes four directions, so the tail arrives as a comb of\n"
				"spikes and the decay comes out about 10%% long. Watch the\n"
				"roughness figure and the echogram as you raise it.");

		m_Dirty |= ImGui::Checkbox("Per-surface materials", &m_UsePerBodyAbsorption);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Per-surface absorption *and* scattering: the bright walls are\n"
				"hard and flat, the dark block is soft, the pillars are rough\n"
				"stone. Turn this off to make every surface the same, and the\n"
				"two sliders above take over.");

		m_Dirty |= ImGui::Checkbox("Frequency-dependent", &m_FrequencyDependent);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: one absorption figure for the whole spectrum, so the\n"
				"     tail stays the same colour all the way down.\n"
				"On:  each band absorbed at its own rate. Watch the three\n"
				"     RT60s above separate.");

		if (m_FrequencyDependent)
		{
			m_Dirty |= ImGui::DragFloat3("Band scale", m_BandScale, 0.01f, 0.05f, 4.0f);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Absorption per band as a multiple of the figure above:\n"
					"low, mid, high. Set all three to 1 for one flat band.");
		}

		m_Dirty |= ImGui::SliderInt("Max taps", &m_MaxTaps, 1,
			(int)Egss::AudioEngine::GetMaxReflections());

		m_Dirty |= ImGui::Checkbox("Trace the whole tail", &m_TraceFullTail);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Costs many more bounces, and in exchange RT60 becomes\n"
				"measured rather than estimated. Watch the line above change\n"
				"from \"Sabine fallback\" to \"measured from the decay\",\n"
				"and watch the profiler.");

		ImGui::SeparatorText("Audio");

		ImGui::Checkbox("Occlusion", &m_ApplyOcclusion);
		ImGui::SameLine();
		ImGui::Checkbox("Reflections", &m_ApplyReflections);
		ImGui::SameLine();
		ImGui::Checkbox("Reverb", &m_ApplyReverb);

		ImGui::SliderFloat("Reflection gain", &m_ReflectionGain, 0.0f, 20.0f);
		ImGui::SliderFloat("Wet scale", &m_WetScale, 0.0f, 8.0f);

		m_Dirty |= ImGui::Checkbox("Convolution tail", &m_UseConvolution);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: comb and allpass filters told the room's size and damping.\n"
				"On:  the traced echogram turned into a few hundred impulses and\n"
				"     convolved with the mix -- the room's own decay rather than\n"
				"     something shaped to resemble it.");

		if (m_UseConvolution)
		{
			m_Dirty |= ImGui::SliderInt("Tail density", &m_TailDensity, 40, 800);
			m_Dirty |= ImGui::SliderFloat("Tail gain", &m_TailGain, 0.1f, 10.0f);

			int perBand[4] = {};
			for (const Egss::ReverbTap& tap : m_ImpulseTaps)
				perBand[glm::min(tap.Band, 3u)]++;

			ImGui::TextDisabled("%zu impulses over %.2f s of traced tail",
				m_ImpulseTaps.size(), m_Result.TracedSeconds);
			ImGui::TextDisabled("  %d low, %d mid, %d high  (budget follows tail length)",
				perBand[0], perBand[1], perBand[2]);

			// The convolution can only be as long as the trace: past that
			// there is no echogram to turn into impulses.
			if (m_Result.TracedSeconds < m_Result.ReverbTime * 0.5f)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
					"Tail is cut short -- turn on \"Trace the whole tail\".");
		}

		Egss::ReverbSettings reverb = Egss::AudioEngine::GetReverb();
		ImGui::TextDisabled("driving reverb: wet %.2f  size %.2f  damping %.2f",
			reverb.Wet, reverb.RoomSize, reverb.Damping);

		if (ImGui::Button("Restart sound"))
			StartSource();

		ImGui::SeparatorText("View");
		ImGui::Checkbox("Draw rays", &m_DrawRays);
		ImGui::SameLine();
		ImGui::Checkbox("Draw reach", &m_DrawRadius);
		ImGui::SliderInt("Rays drawn", &m_RaysDrawn, 4, 128);

		ImGui::TextDisabled("Frame: %.2f ms   %d draw calls",
			m_FrameTime, Egss::Renderer2D::GetStats().DrawCalls);

		ImGui::End();
	}

private:
	// Declared before the camera on purpose: members are initialised in
	// declaration order, so a camera built from these has to come after them.
	// With the camera first they are still uninitialised when it reads them,
	// and the projection comes out as whatever was on the stack -- which
	// renders an empty screen and nothing else wrong.
	float m_Aspect = 16.0f / 9.0f;
	float m_Zoom = 9.0f;

	Egss::OrthographicCamera m_Camera;

	Egss::PhysicsWorld2D m_World;
	std::vector<float> m_Absorptions;
	std::vector<float> m_Scatterings;

	// Changing this rebuilds the room, so it is not a trace setting -- it has
	// to go through BuildRoom rather than just marking the trace dirty.
	bool m_Corridor = true;

	glm::vec2 m_Source = { -6.0f, -3.0f };
	glm::vec2 m_Listener = { 8.0f, 2.0f };
	float m_MoveSpeed = 6.0f;

	Egss::AcousticsResult m_Result;
	std::vector<Egss::TracedRay> m_Rays;
	std::vector<Egss::AudioReflection> m_Taps;
	std::vector<float> m_EchogramDb;

	// Retracing is triggered by movement rather than by the clock, with the
	// interval only there to catch a settings change that moved nothing.
	glm::vec2 m_LastTraceSource = { 1e9f, 1e9f };
	glm::vec2 m_LastTraceListener = { 1e9f, 1e9f };
	float m_SinceTrace = 0.0f;
	float m_RetraceDistance = 0.25f;
	float m_RetraceInterval = 0.5f;
	bool m_Dirty = true;

	int m_RayCount = 192;
	int m_MaxBounces = 16;
	float m_Absorption = 0.15f;
	// Non-zero by default: a specular room is the special case, not the
	// normal one, and the default should be the room that behaves.
	float m_Scattering = 0.3f;
	bool m_UsePerBodyAbsorption = true;
	int m_MaxTaps = 8;
	bool m_TraceFullTail = false;

	bool m_FrequencyDependent = true;
	float m_BandScale[Egss::AcousticBandCount] = { 0.55f, 1.0f, 1.9f };

	bool m_UseConvolution = true;
	int m_TailDensity = 260;
	float m_TailGain = 2.0f;
	std::vector<Egss::ReverbTap> m_ImpulseTaps;

	// Shared by the voice and the trace so both agree what "close" means.
	static constexpr float s_SourceMinDistance = 1.5f;

	bool m_ApplyOcclusion = true;
	bool m_ApplyReflections = true;
	bool m_ApplyReverb = true;
	// Reflections off distant walls are genuinely tens of dB down. Honest,
	// and inaudible, so the demo lifts them.
	float m_ReflectionGain = 8.0f;
	float m_WetScale = 3.0f;

	bool m_DrawRays = true;
	bool m_DrawRadius = true;
	int m_RaysDrawn = 48;

	std::shared_ptr<Egss::AudioClip> m_Clip;
	Egss::VoiceHandle m_Voice = Egss::InvalidVoice;

	float m_FrameTime = 0.0f;
};
