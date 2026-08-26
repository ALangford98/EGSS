#pragma once

// Single-microphone fault attribution: two pumps, one mic, which one is bad.
//
// The engine-facing half. `PumpSignal.h` holds the arithmetic and knows nothing
// about EGSS; this file builds the room, traces it, plays it, and draws the
// answer.
//
// ---------------------------------------------------------------------------
// Why this is a demo in a game engine
//
// The question -- can one microphone tell two identical pumps apart -- turns
// entirely on how differently the room treats the two of them. That is an
// acoustics question about a geometry, and `Acoustics3D` already answers it
// against the scene you can see. So the two impulse responses driving the
// analysis are not invented: they are traced from the room, and moving a pump
// changes the answer for the reason it would change it on site.
//
// That makes the demo a **site survey tool** as much as an algorithm bench.
// The number to watch is `ChannelFit::Conditioning`, which says whether the
// microphone is anywhere useful *before* anyone installs one.
//
// ---------------------------------------------------------------------------
// What you hear is not what is measured, on purpose
//
// Two signal paths run from the same trace:
//
//   Audible  -- `AudioEngine::PlayAt` with the traced early reflections and
//               tail. Stereo, panned by direction, so it sounds like a room.
//   Measured -- `PumpDx::Microphone`, a mono sparse convolution.
//
// They are deliberately different. The mixer pans each voice by its direction,
// which hands the analysis a left/right cue a real mono microphone does not
// have -- and a method that quietly leaned on that cue would report a
// feasibility that evaporates on site. The speakers get the pleasant version;
// the numbers get the honest one.
//
// ---------------------------------------------------------------------------
// The four methods, weakest first
//
//   M0  Broadband level      -- the control. Rises, says nothing about which.
//   M1  Spectral residual    -- what broke, still not which machine.
//   M2  Channel attribution  -- the one that works at identical shaft speed.
//   M3  Envelope demodulation-- works only if the shaft speeds differ.
//   M4  Cepstral signature   -- hand-checkable against the traced geometry.
//
// M0 and M1 are kept because their failure is the finding: they are what a
// level meter and a spectrum analyser on the wall would tell you, and seeing
// them fail next to M2 succeeding is the argument for doing anything harder.
//
// ---------------------------------------------------------------------------
// Traps this demo walked into, so the next reader does not
//
//  - **Baselines belong to a geometry.** Move the mic and both channels change,
//    so a baseline taken before the move describes a room that is gone. The
//    failure is silent -- the arithmetic still produces two plausible numbers.
//    Hence the drift warning in the panel.
//  - **Smoothing the spectrum destroys the method.** Measured: correlation
//    between the two baselines rose from 0.93 to 0.99 as smoothing went from
//    none to +/-4 bins, and attribution went with it. What separates two room
//    paths is the fine comb structure their reflections cut into the spectrum,
//    which is exactly what a moving average removes. Resolution beats
//    averaging here, which is the opposite of the usual advice.
//  - **The fit must be done in sub-bands.** Its model assumes the fault's own
//    spectrum is flat across whatever is fitted, and a bearing resonance is
//    not flat across 2 kHz. Fitted whole, the misfit ran 60x the estimator
//    noise and a *larger* fault became harder to place than a small one. See
//    `AttributeByChannel`.
//  - **Detection and attribution are different questions.** "Is A worse than
//    its baseline" and "is B worse than its baseline" both answer yes on a
//    single-machine fault. The question that separates them is whether A is
//    worse than B.
//
// Verified numerically before any of it was drawn: PSD against a sine's
// A^2/2, the convolution against a hand-delayed copy, the cepstral peak against
// (reflected - direct)/343, and the envelope method against the Fourier limit
// 1/(defect rate difference). Those checks live in the changelog rather than in
// the tree, per the project's habit of deleting a test once it has spoken.

#include <Egss.h>
#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Demo.h"
#include "PumpSignal.h"

class PumpDiagnostics : public DemoLayer
{
public:
	PumpDiagnostics()
		: DemoLayer("PumpDiagnostics"), m_Camera(55.0f, 16.0f / 9.0f, 0.1f, 120.0f)
	{
		// Only what reaches the simulation. The fault switches and severities
		// change what the microphone hears, so a session recorded while they
		// moved has to replay as itself; the band edges and the window length
		// change only what is computed from it afterwards -- but they change
		// the numbers on screen, and those are the point of the demo, so they
		// are registered too.
		RegisterParam("Pump A shaft Hz", &m_Pump[0].ShaftHz);
		RegisterParam("Pump B shaft Hz", &m_Pump[1].ShaftHz);
		RegisterParam("A bearing", &m_Pump[0].Fault.Bearing);
		RegisterParam("B bearing", &m_Pump[1].Fault.Bearing);
		RegisterParam("A cavitation", &m_Pump[0].Fault.Cavitation);
		RegisterParam("B cavitation", &m_Pump[1].Fault.Cavitation);
		RegisterParam("A imbalance", &m_Pump[0].Fault.Imbalance);
		RegisterParam("B imbalance", &m_Pump[1].Fault.Imbalance);
		RegisterParam("A bearing severity", &m_Pump[0].Fault.BearingSeverity);
		RegisterParam("B bearing severity", &m_Pump[1].Fault.BearingSeverity);
		RegisterParam("A cavitation severity", &m_Pump[0].Fault.CavitationSeverity);
		RegisterParam("B cavitation severity", &m_Pump[1].Fault.CavitationSeverity);
		RegisterParam("A imbalance severity", &m_Pump[0].Fault.ImbalanceSeverity);
		RegisterParam("B imbalance severity", &m_Pump[1].Fault.ImbalanceSeverity);
		RegisterParam("Band low", &m_BandLowHz);
		RegisterParam("Band high", &m_BandHighHz);
		RegisterParam("Window seconds", &m_WindowSeconds);
	}

	// ---------------------------------------------------------------------
	// Setup
	// ---------------------------------------------------------------------
	void OnDemoAttach() override
	{
		// Inside the room, not outside it: the walls are drawn as wireframe
		// so an outside vantage sees through them and reads as a mess.
		m_Camera.SetPosition({ -0.5f, 5.4f, 7.5f });
		m_Camera.SetRotation(-84.0f, -26.0f);

		m_Cube.reset(Egss::Mesh::CreateCube(1.0f));

		// 4-pole motors on a 50 Hz grid. Synchronous speed is 25 Hz; the
		// difference between the two is slip, which tracks load -- so these
		// start *identical*, which is the hard case the rig exists to test.
		for (int p = 0; p < 2; p++)
		{
			m_Pump[p].ShaftHz = 24.5f;
			m_Pump[p].BladeCount = 5;
			m_Pump[p].LineHz = 50.0f;
			m_Pump[p].FlowNoise = 0.05f;
			m_Pump[p].Level = 1.0f;
			m_Pump[p].Fault.BearingSeverity = 0.10f;
			m_Pump[p].Fault.CavitationSeverity = 0.25f;
			m_Pump[p].Fault.ImbalanceSeverity = 0.40f;
		}



		BuildRoom();

		m_Microphone.Allocate(kMaxWindowSeconds + 2.0f);
		m_Microphone.Configure(m_Pump[0], m_Pump[1], 20250825u);

		m_HealthHistory[0].assign(kHistory, 0.0f);
		m_HealthHistory[1].assign(kHistory, 0.0f);
	}

	void OnDemoActivated() override
	{
		Retrace();
		StartVoices();

		// Take the baselines straight away rather than making the first thing
		// anyone sees be a panel refusing to work. They are only valid for this
		// geometry, so moving anything still needs a re-capture -- which is the
		// real constraint and is worth meeting immediately rather than reading
		// about.
		if (!m_Baselines.Valid)
			BeginCapture();
	}

	void OnDemoDeactivated() override
	{
		StopVoices();

		// Put the mixer back, or this demo's room keeps ringing under whichever
		// demo is selected next. That exact bug is why `OnDemoDeactivated`
		// exists at all -- see Demo.h.
		Egss::AudioEngine::ClearReverbImpulse();
		Egss::AudioEngine::SetReverb(Egss::ReverbSettings());
	}

	// The room. **Slabs, not one hollow box** -- a ray starting inside a box
	// gets a hit at distance zero, because that is the only answer AgainstAabb
	// can honestly give, so a source inside a single-box room never gets
	// anywhere. `Acoustics3D` documents this and it is still the easiest
	// mistake to make here.
	//
	// A plant room rather than a hall: hard walls, low ceiling. That is not
	// decoration. Absorption sets how much reflected energy there is to
	// distinguish the two channels with, and a dead room is one where every
	// path is just the direct sound scaled -- which is precisely the case the
	// channel method cannot solve.
	void BuildRoom()
	{
		m_Scene = Egss::Scene();
		m_Walls.clear();

		const float halfX = m_RoomHalfX;
		const float halfZ = m_RoomHalfZ;
		const float height = m_RoomHeight;
		const float thickness = 0.3f;

		auto slab = [this](const char* name, const glm::vec3& position, const glm::vec3& scale)
		{
			Egss::Entity entity = m_Scene.CreateEntity(name);

			auto* transform = entity.Get<Egss::TransformComponent>();
			transform->Position = position;
			transform->Scale = scale;

			Egss::MeshComponent mesh;
			mesh.Geometry = m_Cube;
			mesh.Color = { 0.34f, 0.36f, 0.42f, 1.0f };
			// **Required for tracing, not for drawing.** Raycast3D skips
			// meshes marked invisible, so a wall hidden this way stops
			// reflecting sound as well as stopping being drawn. This demo
			// draws its own wireframe and never submits these, so Visible
			// stays true and means only "the sound can see it".
			mesh.Visible = true;
			entity.Add<Egss::MeshComponent>(mesh);

			m_Walls.push_back({ position, scale });
		};

		slab("Floor",   { 0.0f, -thickness, 0.0f },        { halfX * 2, thickness * 2, halfZ * 2 });
		slab("Ceiling", { 0.0f, height + thickness, 0.0f },{ halfX * 2, thickness * 2, halfZ * 2 });
		slab("West",    { -halfX, height * 0.5f, 0.0f },   { thickness * 2, height, halfZ * 2 });
		slab("East",    {  halfX, height * 0.5f, 0.0f },   { thickness * 2, height, halfZ * 2 });
		slab("North",   { 0.0f, height * 0.5f, -halfZ },   { halfX * 2, height, thickness * 2 });
		slab("South",   { 0.0f, height * 0.5f,  halfZ },   { halfX * 2, height, thickness * 2 });
	}

	// ---------------------------------------------------------------------
	// Tracing: the room becomes two impulse responses
	// ---------------------------------------------------------------------
	void Retrace()
	{
		Egss::AcousticsSettings settings;
		settings.RayCount = m_RayCount;
		settings.Absorption = m_Absorption;
		settings.Scattering = m_Scattering;
		settings.MinDistance = m_MinDistance;

		for (int p = 0; p < 2; p++)
		{
			m_Trace[p] = Egss::Acoustics3D::Trace(m_Scene, m_PumpPosition[p], m_MicPosition, settings);
			BuildChannel(p);
		}

		m_TracedFingerprint = Fingerprint();
		ApplyToMixer();
	}

	// Turn one traced result into the sparse mono response the analysis
	// convolves with.
	//
	// Three parts, and all three matter to the method for different reasons:
	// the direct sound sets the level, the early reflections cut the comb that
	// distinguishes this channel from the other one, and the tail fills in
	// between the teeth. Drop the reflections and the two channels differ only
	// by a scale factor, which is exactly the rank-deficient case.
	void BuildChannel(int pump)
	{
		const Egss::AcousticsResult3D& trace = m_Trace[pump];

		PumpDx::SparseIr ir;

		float distance = std::max(trace.DirectDistance, m_MinDistance);
		float directGain = m_MinDistance / distance;
		ir.Add(trace.DirectDistance / PumpDx::kSpeedOfSound, directGain * (1.0f - trace.Occlusion));

		for (const Egss::ReflectionPath3D& path : trace.Reflections)
			ir.Add(path.Delay, path.Gain * m_ReflectionGain);

		if (m_UseTail)
		{
			Egss::ImpulseSettings impulse;
			impulse.StartSeconds = 0.08f;
			impulse.Gain = m_TailGain;

			std::vector<Egss::ReverbTap> taps = Egss::Acoustics::BuildImpulseTaps(trace, impulse);
			for (const Egss::ReverbTap& tap : taps)
			{
				// Pan is dropped rather than folded in. A single microphone has
				// no left and no right, and summing a panned pair would leave a
				// direction-dependent gain behind -- a directivity the real
				// sensor does not have, quietly helping the method.
				ir.Add(tap.Delay, tap.Gain);
			}
		}

		ir.Sort();
		m_Microphone.SetChannel(pump, ir);

		// The first reflection's excess path over the direct sound. This is the
		// cepstral prediction, and it is arithmetic the cepstrum is never told:
		// (reflected path - direct distance) / speed of sound.
		m_PredictedQuefrency[pump] = 0.0f;
		if (!trace.Reflections.empty())
		{
			const Egss::ReflectionPath3D& first = trace.Reflections.front();
			m_PredictedQuefrency[pump] =
				(first.PathLength - trace.DirectDistance) / PumpDx::kSpeedOfSound;
		}
	}

	// Positions the baselines were taken at, as one number. Any drift and the
	// two channels are no longer the ones the baselines describe.
	float Fingerprint() const
	{
		glm::vec3 sum = m_MicPosition + m_PumpPosition[0] * 3.0f + m_PumpPosition[1] * 7.0f;
		return sum.x + sum.y * 13.0f + sum.z * 29.0f;
	}

	bool GeometryDrifted() const
	{
		return m_Baselines.Valid && std::fabs(m_Baselines.Fingerprint - Fingerprint()) > 1e-4f;
	}

	// ---------------------------------------------------------------------
	// The audible path
	// ---------------------------------------------------------------------
	void StartVoices()
	{
		StopVoices();

		if (!Egss::AudioEngine::IsAvailable())
			return;

		float rate = (float)Egss::AudioEngine::GetSampleRate();
		int frames = (int)(rate * 2.0f);

		for (int p = 0; p < 2; p++)
		{
			// Rendered at the *device* rate, not the analysis rate. A clip is
			// assumed to already be at the mixer's rate, so a 12 kHz buffer
			// handed over would play the pump four times too slow.
			PumpDx::PumpSynth synth;
			synth.Reset(m_Pump[p], 20250825u + (uint32_t)p * 7919u, rate);

			std::vector<float> samples((size_t)frames);
			synth.Render(samples.data(), frames, false);

			m_Clip[p] = Egss::AudioClip::CreateFromSamples(std::move(samples), 1);

			Egss::Audio3DParams params;
			params.Position = m_PumpPosition[p];
			params.Loop = true;
			params.Volume = m_Volume;
			params.MinDistance = m_MinDistance;
			params.MaxDistance = 40.0f;
			// A pump does not move, so Doppler is only a source of surprises.
			params.DopplerFactor = 0.0f;

			m_Voice[p] = Egss::AudioEngine::PlayAt(m_Clip[p], params);
		}

		ApplyToMixer();
	}

	void StopVoices()
	{
		for (int p = 0; p < 2; p++)
		{
			if (m_Voice[p] != Egss::InvalidVoice)
				Egss::AudioEngine::Stop(m_Voice[p]);

			m_Voice[p] = Egss::InvalidVoice;
		}
	}

	// The loop is two seconds long, so a fault switched on mid-run is not heard
	// until the clip is rebuilt. Cheap enough to just rebuild it.
	void RefreshAudibleClips()
	{
		if (m_Voice[0] != Egss::InvalidVoice || m_Voice[1] != Egss::InvalidVoice)
			StartVoices();
	}

	void ApplyToMixer()
	{
		if (!Egss::AudioEngine::IsAvailable())
			return;

		Egss::AudioListener listener;
		listener.Position = m_Camera.GetPosition();
		listener.Forward = m_Camera.GetForward();
		listener.Up = m_Camera.GetUp();
		Egss::AudioEngine::SetListener(listener);

		glm::vec3 right = glm::normalize(glm::cross(listener.Forward, listener.Up));

		for (int p = 0; p < 2; p++)
		{
			if (!Egss::AudioEngine::IsPlaying(m_Voice[p]))
				continue;

			m_Taps.clear();
			for (const Egss::ReflectionPath3D& path : m_Trace[p].Reflections)
			{
				Egss::AudioReflection tap;
				tap.Delay = path.Delay;
				tap.Gain = path.Gain * m_ReflectionGain;
				// In 3D a pan comes from the listener, not the world: the
				// arrival is a world vector and which ear hears it depends on
				// which way the camera faces.
				tap.Pan = glm::clamp(glm::dot(path.Direction, right), -1.0f, 1.0f);
				m_Taps.push_back(tap);
			}

			Egss::AudioEngine::SetVoiceReflections(m_Voice[p], m_Taps.data(), (unsigned int)m_Taps.size());
		}

		// One tail for the room, from the nearer pump's trace. A tail is a
		// property of the room rather than of a source, so tracing both to
		// average two answers to the same question would be wasted work.
		int nearest = glm::length(m_PumpPosition[0] - listener.Position)
			< glm::length(m_PumpPosition[1] - listener.Position) ? 0 : 1;

		Egss::ImpulseSettings impulse;
		impulse.StartSeconds = 0.08f;
		impulse.Gain = m_TailGain;
		m_Impulse = Egss::Acoustics::BuildImpulseTaps(m_Trace[nearest], impulse);
		Egss::AudioEngine::SetReverbImpulse(m_Impulse.data(), (unsigned int)m_Impulse.size());

		Egss::ReverbSettings reverb;
		reverb.Wet = glm::clamp(m_Trace[nearest].LateEnergyRatio * 1.5f, 0.0f, 0.9f);
		reverb.RoomSize = glm::clamp(m_Trace[nearest].ReverbTime / 2.0f, 0.1f, 0.95f);
		reverb.Damping = glm::clamp(m_Trace[nearest].MeanAbsorption * 1.5f, 0.1f, 0.9f);
		Egss::AudioEngine::SetReverb(reverb);
	}

	// ---------------------------------------------------------------------
	// The measurement
	//
	// Runs in OnDemoFixedUpdate rather than OnUpdate, so how much has been
	// simulated by a given moment does not depend on how fast the machine ran.
	// Everything that moves belongs in the fixed step -- three demos here
	// violated that and could not reproduce themselves run to run.
	// ---------------------------------------------------------------------
	void OnDemoFixedUpdate(Egss::Timestep fixedStep) override
	{
		if (m_Paused)
			return;

		// Sample count per step is derived from the step, so the analysis rate
		// and the frame rate stay in step regardless of either.
		m_SampleDebt += (float)fixedStep * (float)PumpDx::kAnalysisRate;
		int samples = (int)m_SampleDebt;
		m_SampleDebt -= (float)samples;

		if (samples <= 0)
			return;

		bool healthy[2] = { m_ForceHealthy[0], m_ForceHealthy[1] };
		bool mute[2] = { m_Mute[0], m_Mute[1] };
		m_Microphone.Advance(samples, healthy, mute);

		m_ElapsedSeconds += (float)fixedStep;


		// Capture is a scripted sequence rather than a button that blocks: mute
		// B and record A, then the reverse. Doing it in real time is what a
		// commissioning engineer actually does, and it means the baselines
		// carry the same estimator noise as the measurement they are subtracted
		// from -- which the error bars assume.
		if (m_CaptureStage != CaptureStage::Idle)
			StepCapture();
		else if (m_ElapsedSeconds - m_LastAnalysis >= m_AnalysisInterval)
			Analyse();
	}

	enum class CaptureStage { Idle, PumpA, PumpB };

	void BeginCapture()
	{
		m_CaptureStage = CaptureStage::PumpA;
		m_CaptureStarted = m_ElapsedSeconds;
		m_Baselines.Clear();
		m_Microphone.Clear();

		// Baselines are healthy by definition: the whole point is a record of
		// the machine when nothing was wrong with it.
		m_ForceHealthy[0] = m_ForceHealthy[1] = true;
		m_Mute[0] = false;
		m_Mute[1] = true;
	}

	void StepCapture()
	{
		// A settle margin before the window, so the ring holds no silence from
		// before the mute changed and no source older than the longest room
		// path. Without it the first baseline is a fade-in and reads as a
		// quieter machine than the second.
		float needed = m_WindowSeconds + kSettleSeconds;
		if (m_ElapsedSeconds - m_CaptureStarted < needed)
			return;

		int pump = m_CaptureStage == CaptureStage::PumpA ? 0 : 1;

		std::vector<float> window;
		m_Microphone.ReadRecent(window, (size_t)(m_WindowSeconds * PumpDx::kAnalysisRate));
		m_Baselines.Psd[pump] = PumpDx::WelchPsd(window.data(), window.size(),
			m_FftSize, m_FftSize / 2, &m_Segments);

		if (m_CaptureStage == CaptureStage::PumpA)
		{
			m_CaptureStage = CaptureStage::PumpB;
			m_CaptureStarted = m_ElapsedSeconds;
			m_Mute[0] = true;
			m_Mute[1] = false;
			return;
		}

		m_CaptureStage = CaptureStage::Idle;
		m_Mute[0] = m_Mute[1] = false;
		m_ForceHealthy[0] = m_ForceHealthy[1] = false;

		m_Baselines.FftSize = m_FftSize;
		m_Baselines.Fingerprint = Fingerprint();
		m_Baselines.Valid = true;

		EGSS_TRACE("[PumpDx] baselines captured over {0:.1f} s, {1} Welch segments",
			m_WindowSeconds, m_Segments);
	}

	void Analyse()
	{
		m_LastAnalysis = m_ElapsedSeconds;

		size_t want = (size_t)(m_WindowSeconds * PumpDx::kAnalysisRate);
		if (m_Microphone.Filled() < want + (size_t)(kSettleSeconds * PumpDx::kAnalysisRate))
			return;

		m_Microphone.ReadRecent(m_Window, want);

		// --- M0: the control ------------------------------------------------
		m_LevelDb = PumpDx::BroadbandLevelDb(m_Window.data(), m_Window.size());

		m_MixturePsd = PumpDx::WelchPsd(m_Window.data(), m_Window.size(),
			m_FftSize, m_FftSize / 2, &m_Segments);

		// --- M1: what broke -------------------------------------------------
		m_Residual = PumpDx::SpectralResidual(m_MixturePsd, m_Baselines);

		// --- M2: which machine ----------------------------------------------
		m_ChannelFit = PumpDx::AttributeByChannel(m_MixturePsd, m_Baselines, m_FftSize,
			m_BandLowHz, m_BandHighHz, m_Segments, 0, m_SubBands);

		// --- M3: which shaft ------------------------------------------------
		m_EnvelopeFit = PumpDx::AttributeByEnvelope(m_Window,
			m_Microphone.GetConfig(0), m_Microphone.GetConfig(1),
			m_BandLowHz, m_BandHighHz);

		// --- M4: which room path --------------------------------------------
		m_CepstrumFit = PumpDx::AttributeByCepstrum(m_Window,
			m_PredictedQuefrency[0], m_PredictedQuefrency[1]);

		// --- The health score, trended -------------------------------------
		for (int p = 0; p < 2; p++)
		{
			m_HealthHistory[p][m_HistoryHead] = m_ChannelFit.SeverityDb[p];
			m_TruthHistory[p][m_HistoryHead] = m_Pump[p].Fault.Any() ? 1.0f : 0.0f;
		}
		m_HistoryHead = (m_HistoryHead + 1) % kHistory;

		if (m_LogVerdicts)
		{
			static const char* names[] = { "neither", "pump A", "pump B", "both" };
			int truthIndex = GroundTruth() + 1;
			int verdictIndex = m_ChannelFit.Verdict + 1;
			EGSS_TRACE("[PumpDx] t={0:.1f}s truth={1} verdict={2} | A {3:+.2f} dB B {4:+.2f} dB "
				"| detect {5:.1f}s attrib {6:.1f}s cond {7:.2f} chi2 {8:.2f} q {9:.2f}",
				m_ElapsedSeconds, names[truthIndex], names[verdictIndex],
				m_ChannelFit.SeverityDb[0], m_ChannelFit.SeverityDb[1],
				m_ChannelFit.DetectionSigma, m_ChannelFit.AttributionSigma,
				m_ChannelFit.Conditioning, m_ChannelFit.ReducedChiSquare,
				m_ChannelFit.FitQuality);
		}

		// Score the rig against itself. A feasibility answer is an accuracy
		// number, not an impression, and the ground truth is right here.
		int truth = GroundTruth();
		if (truth != -2)
		{
			m_Attempts++;
			if (m_ChannelFit.Verdict == truth)
				m_Correct++;
			else if (m_ChannelFit.Verdict == 2 || m_ChannelFit.Verdict == -1)
				m_Undecided++;
			else
				m_Wrong++;
		}
	}

	// -1 neither faulty, 0 A only, 1 B only, 2 both.
	int GroundTruth() const
	{
		bool a = m_Pump[0].Fault.Any() && Severity(0) > 0.0f;
		bool b = m_Pump[1].Fault.Any() && Severity(1) > 0.0f;

		if (a && b) return 2;
		if (a) return 0;
		if (b) return 1;
		return -1;
	}

	float Severity(int pump) const
	{
		const PumpDx::FaultConfig& f = m_Pump[pump].Fault;
		float total = 0.0f;
		if (f.Bearing) total += f.BearingSeverity;
		if (f.Cavitation) total += f.CavitationSeverity;
		if (f.Imbalance) total += f.ImbalanceSeverity;
		return total;
	}

	void PushConfigs()
	{
		for (int p = 0; p < 2; p++)
			m_Microphone.SetConfig(p, m_Pump[p]);
	}

	// ---------------------------------------------------------------------
	// Camera and rendering
	// ---------------------------------------------------------------------
	void OnDemoUpdate(Egss::Timestep ts) override
	{
		MoveCamera(ts);
		ApplyToMixer();

		Egss::RenderCommand::SetClearColor({ 0.05f, 0.06f, 0.08f, 1.0f });
		Egss::RenderCommand::Clear();

		Egss::Renderer2D::BeginScene(m_Camera);
		DrawRoom();
		DrawRays();
		DrawMachines();
		Egss::Renderer2D::EndScene();
	}

	void MoveCamera(Egss::Timestep ts)
	{
		float speed = m_CameraSpeed * (float)ts;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT_SHIFT))
			speed *= 3.0f;

		glm::vec3 position = m_Camera.GetPosition();
		glm::vec3 forward = m_Camera.GetForward();
		glm::vec3 right = m_Camera.GetRight();

		if (Egss::Input::IsKeyPressed(EGSS_KEY_W)) position += forward * speed;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_S)) position -= forward * speed;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_A)) position -= right * speed;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_D)) position += right * speed;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_E)) position.y += speed;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_Q)) position.y -= speed;

		m_Camera.SetPosition(position);

		float turn = m_TurnSpeed * (float)ts;
		float yaw = m_Camera.GetYaw();
		float pitch = m_Camera.GetPitch();

		if (Egss::Input::IsKeyPressed(EGSS_KEY_LEFT))  yaw -= turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_RIGHT)) yaw += turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_UP))    pitch += turn;
		if (Egss::Input::IsKeyPressed(EGSS_KEY_DOWN))  pitch -= turn;

		m_Camera.SetRotation(yaw, glm::clamp(pitch, -89.0f, 89.0f));
	}

	void DrawBox(const glm::vec3& centre, const glm::vec3& scale, const glm::vec4& color)
	{
		glm::vec3 h = scale * 0.5f;
		glm::vec3 corner[8];
		for (int i = 0; i < 8; i++)
		{
			corner[i] = centre + glm::vec3(
				(i & 1) ? h.x : -h.x,
				(i & 2) ? h.y : -h.y,
				(i & 4) ? h.z : -h.z);
		}

		static const int edges[12][2] = {
			{0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7}
		};

		for (const auto& e : edges)
			Egss::Renderer2D::DrawLine(corner[e[0]], corner[e[1]], color);
	}

	void DrawRoom()
	{
		for (const Slab& wall : m_Walls)
			DrawBox(wall.Position, wall.Scale, { 0.30f, 0.33f, 0.40f, 1.0f });
	}

	// The two machines and the mic. Colour is the identity that runs through
	// the whole panel: pump A amber, pump B blue, everywhere.
	void DrawMachines()
	{
		for (int p = 0; p < 2; p++)
		{
			glm::vec4 color = PumpColor(p);

			// A faulty machine is drawn hot, so ground truth is visible at a
			// glance and the panel can be read against it without scrolling.
			if (m_Pump[p].Fault.Any() && Severity(p) > 0.0f)
				color = glm::mix(color, glm::vec4(1.0f, 0.25f, 0.2f, 1.0f), 0.55f);

			DrawBox(m_PumpPosition[p], { 1.2f, 1.4f, 1.8f }, color);
			DrawBox(m_PumpPosition[p] + glm::vec3(0.0f, 0.95f, 0.0f), { 0.5f, 0.5f, 0.5f }, color);
		}

		glm::vec4 micColor = { 0.85f, 0.90f, 0.95f, 1.0f };
		DrawBox(m_MicPosition, { 0.25f, 0.25f, 0.25f }, micColor);
		Egss::Renderer2D::DrawLine(m_MicPosition,
			m_MicPosition - glm::vec3(0.0f, m_MicPosition.y, 0.0f), micColor);
	}

	// Direct paths and early reflections, in each pump's own colour.
	//
	// This is the picture the whole method rests on: if the two fans of lines
	// look alike, the two channels are alike, and no arithmetic downstream will
	// tell the machines apart. It is quicker to see than to read off the
	// conditioning number, and it is the same fact.
	void DrawRays()
	{
		for (int p = 0; p < 2; p++)
		{
			glm::vec4 color = PumpColor(p);

			if (m_ShowDirect)
				Egss::Renderer2D::DrawLine(m_PumpPosition[p], m_MicPosition, color);

			if (!m_ShowReflections)
				continue;

			glm::vec4 faint = { color.r, color.g, color.b, 0.35f };
			for (const Egss::ReflectionPath3D& path : m_Trace[p].Reflections)
			{
				// The bounce point is not reported, only the arrival direction
				// and the total length -- enough to draw the last leg, which is
				// the part that says where the energy came from.
				// Clamped, not scaled. A late reflection has travelled tens of
				// metres and drawing it to scale sends the line out through
				// the wall and off screen, which turns the one picture that
				// matters here into a starburst.
				float leg = std::min(path.PathLength * 0.45f, 3.5f);
				glm::vec3 from = m_MicPosition - path.Direction * leg;
				Egss::Renderer2D::DrawLine(from, m_MicPosition, faint);
			}
		}
	}

	static glm::vec4 PumpColor(int pump)
	{
		return pump == 0
			? glm::vec4(0.98f, 0.72f, 0.24f, 1.0f)    // A: amber
			: glm::vec4(0.35f, 0.68f, 0.98f, 1.0f);   // B: blue
	}

	// ---------------------------------------------------------------------
	// Panels
	// ---------------------------------------------------------------------
	void OnDemoImGui() override
	{
		// FirstUseEver, so a saved imgui.ini still wins. Three panels plus the
		// engine's own two stack on top of each other otherwise, and the first
		// thing anyone sees is the demo hiding itself.
		// Placed around the engine's own two, which take the top-left (the demo
		// selector) and the top-right (the profiler). The verdict gets the free
		// centre-top because it is the one panel worth reading first.
		ImGui::SetNextWindowPos(ImVec2(420.0f, 10.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(450.0f, 340.0f), ImGuiCond_FirstUseEver);
		DrawVerdictPanel();

		ImGui::SetNextWindowPos(ImVec2(10.0f, 230.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(400.0f, 480.0f), ImGuiCond_FirstUseEver);
		DrawSetupPanel();

		ImGui::SetNextWindowPos(ImVec2(880.0f, 320.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(390.0f, 390.0f), ImGuiCond_FirstUseEver);
		DrawMethodsPanel();
	}

	// The answer, and how much to believe it.
	void DrawVerdictPanel()
	{
		ImGui::Begin("Verdict");

		if (!m_Baselines.Valid)
		{
			ImGui::TextColored({ 1.0f, 0.7f, 0.3f, 1.0f },
				"No baselines. Everything below is unavailable.");
			ImGui::TextWrapped(
				"The channel method needs one recording of each pump running "
				"alone and healthy. Press the button to take both -- it mutes "
				"one machine at a time for %.0f s each.", m_WindowSeconds + kSettleSeconds);

			if (ImGui::Button("Capture baselines"))
				BeginCapture();

			ImGui::End();
			return;
		}

		if (m_CaptureStage != CaptureStage::Idle)
		{
			ImGui::Text("Capturing baseline for pump %s...",
				m_CaptureStage == CaptureStage::PumpA ? "A" : "B");
			ImGui::End();
			return;
		}

		if (GeometryDrifted())
		{
			// The failure this guards against is silent: the arithmetic still
			// produces two plausible numbers from a room that no longer exists.
			ImGui::TextColored({ 1.0f, 0.35f, 0.3f, 1.0f },
				"GEOMETRY MOVED SINCE BASELINE -- results are meaningless.");
			ImGui::TextWrapped(
				"Both channels changed, so the baselines describe a room that "
				"is gone. On site this is what happens when someone moves the "
				"microphone to run a cable.");
			if (ImGui::Button("Re-capture"))
				BeginCapture();
			ImGui::Separator();
		}

		const PumpDx::ChannelFit& fit = m_ChannelFit;

		// --- The verdict ---
		const char* verdict = "waiting";
		glm::vec4 color = { 0.7f, 0.7f, 0.7f, 1.0f };

		switch (fit.Verdict)
		{
		case -1: verdict = "BOTH HEALTHY";       color = { 0.4f, 0.9f, 0.5f, 1.0f }; break;
		case  0: verdict = "PUMP A IS FAULTY";   color = { 0.98f, 0.72f, 0.24f, 1.0f }; break;
		case  1: verdict = "PUMP B IS FAULTY";   color = { 0.35f, 0.68f, 0.98f, 1.0f }; break;
		case  2: verdict = "FAULT PRESENT -- CANNOT SAY WHICH";
			color = { 1.0f, 0.55f, 0.25f, 1.0f }; break;
		default: break;
		}

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, 1.0f));
		ImGui::SetWindowFontScale(1.35f);
		ImGui::TextUnformatted(verdict);
		ImGui::SetWindowFontScale(1.0f);
		ImGui::PopStyleColor();

		int truth = GroundTruth();
		const char* truthText = truth == -1 ? "neither" : truth == 0 ? "pump A"
			: truth == 1 ? "pump B" : "both";
		ImGui::TextDisabled("ground truth: %s", truthText);

		ImGui::Separator();

		// --- Health score per pump ---
		ImGui::Text("Health score (rise in that pump's own band energy)");
		for (int p = 0; p < 2; p++)
		{
			glm::vec4 c = PumpColor(p);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, 1.0f));
			ImGui::Text("Pump %c  %+6.2f dB  +/- %.3f   (%.1f sigma)",
				p == 0 ? 'A' : 'B',
				fit.SeverityDb[p], fit.AlphaError[p], fit.SignalToNoise[p]);
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();

		// **Detection and attribution are separate questions and the panel says
		// so.** Reading one number for both is what made an earlier version
		// call every single-machine fault "both".
		ImGui::Text("Is anything wrong?   %5.1f sigma  %s",
			fit.DetectionSigma, fit.DetectionSigma >= 3.0f ? "yes" : "no");
		ImGui::Text("Which machine?       %5.1f sigma  %s",
			fit.AttributionSigma, fit.AttributionSigma >= 3.0f ? "decided" : "cannot tell");

		ImGui::Separator();

		// --- Trend ---
		ImGui::Text("Health score over time");
		for (int p = 0; p < 2; p++)
		{
			char label[32];
			std::snprintf(label, sizeof(label), "Pump %c##trend", p == 0 ? 'A' : 'B');
			ImGui::PlotLines(label, m_HealthHistory[p].data(), kHistory, m_HistoryHead,
				nullptr, 0.0f, 8.0f, ImVec2(0.0f, 55.0f));
		}

		// --- Score ---
		if (m_Attempts > 0)
		{
			ImGui::Separator();
			ImGui::Text("Scored over %d windows: %d correct, %d undecided, %d wrong",
				m_Attempts, m_Correct, m_Undecided, m_Wrong);
			ImGui::Text("accuracy %.1f%%, never-wrong rate %.1f%%",
				100.0f * (float)m_Correct / (float)m_Attempts,
				100.0f * (float)(m_Attempts - m_Wrong) / (float)m_Attempts);
			ImGui::SameLine();
			if (ImGui::SmallButton("reset"))
				m_Attempts = m_Correct = m_Undecided = m_Wrong = 0;
		}

		ImGui::End();
	}

	void DrawSetupPanel()
	{
		ImGui::Begin("Plant");

		ImGui::TextWrapped(
			"Two identical pumps, one microphone. Drag anything and the room is "
			"re-traced, which invalidates the baselines -- that is not a "
			"limitation of the demo, it is the constraint the real installation "
			"is under.");

		ImGui::Separator();
		ImGui::Text("Placement");

		bool moved = false;
		moved |= ImGui::DragFloat3("Mic", &m_MicPosition.x, 0.05f, -12.0f, 12.0f);
		moved |= ImGui::DragFloat3("Pump A", &m_PumpPosition[0].x, 0.05f, -12.0f, 12.0f);
		moved |= ImGui::DragFloat3("Pump B", &m_PumpPosition[1].x, 0.05f, -12.0f, 12.0f);

		if (ImGui::Button("Symmetric (the bad case)"))
		{
			// Mirror-image placement about the mic. The two channels then
			// colour the spectrum identically, rho(f) is flat, and no
			// attribution exists at any signal-to-noise ratio.
			m_MicPosition = { 0.0f, 1.6f, 0.0f };
			m_PumpPosition[0] = { -4.0f, 0.7f, -2.0f };
			m_PumpPosition[1] = {  4.0f, 0.7f, -2.0f };
			moved = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Asymmetric (the good case)"))
		{
			m_MicPosition = { 1.5f, 2.4f, 3.0f };
			m_PumpPosition[0] = { -4.5f, 0.7f, -3.0f };
			m_PumpPosition[1] = {  3.5f, 0.7f, -1.0f };
			moved = true;
		}

		ImGui::Separator();
		ImGui::Text("Room");
		moved |= ImGui::SliderFloat("Absorption", &m_Absorption, 0.02f, 0.7f);
		moved |= ImGui::SliderFloat("Scattering", &m_Scattering, 0.0f, 0.8f);
		moved |= ImGui::SliderInt("Rays", &m_RayCount, 64, 2048);
		moved |= ImGui::Checkbox("Include reverb tail", &m_UseTail);

		ImGui::TextWrapped(
			"A dead room is the enemy. Absorption removes the reflections, and "
			"without reflections the two channels differ only by a scale "
			"factor -- which is the case the method cannot solve. Watch the "
			"conditioning number as this rises.");

		if (moved)
		{
			Retrace();
			RefreshAudibleClips();
		}

		ImGui::Separator();
		ImGui::Text("Machines");

		for (int p = 0; p < 2; p++)
		{
			ImGui::PushID(p);
			glm::vec4 c = PumpColor(p);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, 1.0f));
			ImGui::Text("Pump %c", p == 0 ? 'A' : 'B');
			ImGui::PopStyleColor();

			bool changed = false;
			changed |= ImGui::SliderFloat("Shaft Hz", &m_Pump[p].ShaftHz, 20.0f, 30.0f, "%.3f");
			changed |= ImGui::SliderFloat("Level", &m_Pump[p].Level, 0.2f, 2.0f);

			changed |= ImGui::Checkbox("Bearing", &m_Pump[p].Fault.Bearing);
			ImGui::SameLine();
			changed |= ImGui::SliderFloat("##bs", &m_Pump[p].Fault.BearingSeverity, 0.0f, 0.5f);

			changed |= ImGui::Checkbox("Cavitation", &m_Pump[p].Fault.Cavitation);
			ImGui::SameLine();
			changed |= ImGui::SliderFloat("##cs", &m_Pump[p].Fault.CavitationSeverity, 0.0f, 1.0f);

			changed |= ImGui::Checkbox("Imbalance", &m_Pump[p].Fault.Imbalance);
			ImGui::SameLine();
			changed |= ImGui::SliderFloat("##is", &m_Pump[p].Fault.ImbalanceSeverity, 0.0f, 1.0f);

			if (changed)
			{
				PushConfigs();
				RefreshAudibleClips();
			}

			ImGui::PopID();
			ImGui::Spacing();
		}

		if (ImGui::Button("Match shaft speeds exactly"))
		{
			// The user's stated worst case: fixed-speed motors on a shared
			// grid. Note that a shared grid locks the *line* frequency, not the
			// shaft -- slip tracks load, so identical speeds need identical
			// loads too.
			m_Pump[1].ShaftHz = m_Pump[0].ShaftHz;
			PushConfigs();
			RefreshAudibleClips();
		}
		ImGui::SameLine();
		if (ImGui::Button("1% slip difference"))
		{
			m_Pump[1].ShaftHz = m_Pump[0].ShaftHz * 0.99f;
			PushConfigs();
			RefreshAudibleClips();
		}

		ImGui::Separator();
		if (ImGui::Button("Capture baselines"))
			BeginCapture();
		ImGui::SameLine();
		ImGui::Checkbox("Pause", &m_Paused);

		ImGui::Separator();
		ImGui::Text("Audible");
		if (ImGui::SliderFloat("Volume", &m_Volume, 0.0f, 1.0f))
		{
			for (int p = 0; p < 2; p++)
				Egss::AudioEngine::SetVoiceVolume(m_Voice[p], m_Volume);
		}
		ImGui::Checkbox("Show direct paths", &m_ShowDirect);
		ImGui::SameLine();
		ImGui::Checkbox("Show reflections", &m_ShowReflections);
		ImGui::Checkbox("Log verdicts to console", &m_LogVerdicts);

		ImGui::End();
	}

	void DrawMethodsPanel()
	{
		ImGui::Begin("Methods");

		ImGui::TextWrapped(
			"Four methods on the same mono signal, weakest first. M0 and M1 are "
			"here because their failure is the finding.");

		// --- M0 ---
		if (ImGui::CollapsingHeader("M0 -- broadband level (the control)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Level: %.2f dB", m_LevelDb);
			ImGui::TextDisabled(
				"Cannot attribute, by construction. A level meter on the wall\n"
				"tells you something got worse and nothing about which machine.");
		}

		// --- M1 ---
		if (ImGui::CollapsingHeader("M1 -- spectral residual (what broke)"))
		{
			if (!m_Residual.empty())
			{
				ImGui::PlotLines("residual", m_Residual.data(), (int)m_Residual.size() / 2,
					0, nullptr, FLT_MAX, FLT_MAX, ImVec2(0.0f, 70.0f));
				ImGui::TextDisabled(
					"P_mix - P_A - P_B. Isolates the fault's own spectrum, still\n"
					"says nothing about which machine is emitting it.");
			}
		}

		// --- M2 ---
		if (ImGui::CollapsingHeader("M2 -- channel attribution", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const PumpDx::ChannelFit& fit = m_ChannelFit;

			ImGui::Text("alpha A %.4f +/- %.4f", fit.Alpha[0], fit.AlphaError[0]);
			ImGui::Text("alpha B %.4f +/- %.4f", fit.Alpha[1], fit.AlphaError[1]);

			// **The feasibility number.** Everything else is downstream of it.
			ImVec4 condColor = fit.Conditioning < 4.0f ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f)
				: fit.Conditioning < 10.0f ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
				: ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
			ImGui::TextColored(condColor, "conditioning %.2f", fit.Conditioning);
			ImGui::SameLine();
			ImGui::TextDisabled(fit.Conditioning < 4.0f ? "(the mic is well placed)"
				: fit.Conditioning < 10.0f ? "(workable)"
				: "(the two channels are nearly identical -- move the mic)");

			ImGui::Text("fit quality %.2f   chi-square %.2f   sub-bands %d",
				fit.FitQuality, fit.ReducedChiSquare, fit.SubBandsUsed);

			if (fit.ReducedChiSquare > 4.0f)
			{
				ImGui::TextColored({ 1.0f, 0.7f, 0.3f, 1.0f },
					"model misfits -- try a narrower band around the fault");
			}

			ImGui::Text("smallest fault visible: alpha %.4f (%.2f dB)",
				fit.MinDetectableAlpha, 10.0f * std::log10(1.0f + fit.MinDetectableAlpha));

			bool rerun = false;
			rerun |= ImGui::SliderFloat("Band low Hz", &m_BandLowHz, 100.0f, 5000.0f);
			rerun |= ImGui::SliderFloat("Band high Hz", &m_BandHighHz, 200.0f, 5900.0f);
			rerun |= ImGui::SliderInt("Sub-bands", &m_SubBands, 1, 24);
			rerun |= ImGui::SliderFloat("Window s", &m_WindowSeconds, 1.0f, kMaxWindowSeconds);
			(void)rerun;

			if (m_BandHighHz <= m_BandLowHz + 100.0f)
				m_BandHighHz = m_BandLowHz + 100.0f;

			ImGui::TextDisabled(
				"Sub-bands matter: the fit assumes the fault's spectrum is flat\n"
				"across whatever is fitted. One band over 2 kHz misfits 60x.");
		}

		// --- M3 ---
		if (ImGui::CollapsingHeader("M3 -- envelope demodulation (needs a speed difference)"))
		{
			const PumpDx::EnvelopeFit& fit = m_EnvelopeFit;

			ImGui::Text("defect rates: A %.4f Hz, B %.4f Hz", fit.DefectHz[0], fit.DefectHz[1]);
			ImGui::Text("separation %.4f Hz", fit.SeparationHz);

			if (!fit.Resolvable)
			{
				ImGui::TextColored({ 1.0f, 0.4f, 0.35f, 1.0f },
					"UNRESOLVABLE at %.1f s -- needs %.1f s", fit.ActualWindowSeconds,
					fit.RequiredWindowSeconds);
				ImGui::TextDisabled(
					"Two lines this close need a window of at least 1/separation\n"
					"seconds. That is the Fourier limit; no window function beats it.");
			}
			else
			{
				ImGui::Text("score A %.2f dB @ %.3f Hz", fit.Score[0], fit.PeakHz[0]);
				ImGui::Text("score B %.2f dB @ %.3f Hz", fit.Score[1], fit.PeakHz[1]);
			}

			ImGui::TextDisabled(
				"This is the textbook method, and on fixed-speed pumps sharing a\n"
				"grid it can fail outright. It is here to show when.");
		}

		// --- M4 ---
		if (ImGui::CollapsingHeader("M4 -- cepstral channel signature"))
		{
			const PumpDx::CepstrumFit& fit = m_CepstrumFit;

			for (int p = 0; p < 2; p++)
			{
				ImGui::Text("Pump %c  predicted %.5f s  measured %.5f s  (%.1f dB)",
					p == 0 ? 'A' : 'B',
					fit.PredictedQuefrency[p], fit.MeasuredQuefrency[p], fit.Score[p]);
			}

			ImGui::TextDisabled(
				"Predictions are (reflected path - direct) / 343, straight from\n"
				"the traced geometry -- arithmetic the cepstrum is never told.\n"
				"A measured peak landing on one is real agreement.");
		}

		ImGui::Separator();
		ImGui::Text("Analysis rate %d Hz, FFT %d, %d Welch segments",
			PumpDx::kAnalysisRate, m_FftSize, m_Segments);
		ImGui::TextDisabled("Traced: A %.2f m, B %.2f m from the mic. RT60 %.2f s.",
			m_Trace[0].DirectDistance, m_Trace[1].DirectDistance, m_Trace[0].ReverbTime);

		ImGui::End();
	}

	void OnDemoEvent(Egss::Event& e) override
	{
		Egss::EventDispatcher dispatcher(e);
		dispatcher.Dispatch<Egss::KeyPressedEvent>([this](Egss::KeyPressedEvent& key)
		{
			if (key.GetKeyCode() == EGSS_KEY_B)
			{
				BeginCapture();
				return true;
			}
			if (key.GetKeyCode() == EGSS_KEY_SPACE)
			{
				m_Paused = !m_Paused;
				return true;
			}
			return false;
		});
	}

private:
	struct Slab
	{
		glm::vec3 Position;
		glm::vec3 Scale;
	};

	static constexpr int kHistory = 240;
	static constexpr float kMaxWindowSeconds = 16.0f;
	// Long enough to clear the longest room path out of the ring before a
	// window is read. Without it the first baseline is a fade-in and reads as
	// a quieter machine than the second.
	static constexpr float kSettleSeconds = 1.0f;

	Egss::PerspectiveCamera m_Camera;
	Egss::Scene m_Scene;
	std::shared_ptr<Egss::Mesh> m_Cube;
	std::vector<Slab> m_Walls;

	float m_RoomHalfX = 9.0f;
	float m_RoomHalfZ = 7.0f;
	float m_RoomHeight = 4.5f;

	glm::vec3 m_MicPosition = { 1.5f, 2.4f, 3.0f };
	glm::vec3 m_PumpPosition[2] = { { -4.5f, 0.7f, -3.0f }, { 3.5f, 0.7f, -1.0f } };

	// --- Acoustics ---
	Egss::AcousticsResult3D m_Trace[2];
	std::vector<Egss::AudioReflection> m_Taps;
	std::vector<Egss::ReverbTap> m_Impulse;
	float m_PredictedQuefrency[2] = { 0.0f, 0.0f };
	float m_TracedFingerprint = 0.0f;

	int m_RayCount = 512;
	float m_Absorption = 0.12f;
	float m_Scattering = 0.15f;
	float m_MinDistance = 1.0f;
	float m_ReflectionGain = 1.0f;
	float m_TailGain = 0.6f;
	bool m_UseTail = true;

	// --- Audible ---
	std::shared_ptr<Egss::AudioClip> m_Clip[2];
	Egss::VoiceHandle m_Voice[2] = { Egss::InvalidVoice, Egss::InvalidVoice };
	float m_Volume = 0.5f;

	// --- Measured ---
	PumpDx::PumpConfig m_Pump[2];
	PumpDx::Microphone m_Microphone;
	PumpDx::Baselines m_Baselines;

	std::vector<float> m_Window;
	std::vector<float> m_MixturePsd;
	std::vector<float> m_Residual;

	PumpDx::ChannelFit m_ChannelFit;
	PumpDx::EnvelopeFit m_EnvelopeFit;
	PumpDx::CepstrumFit m_CepstrumFit;

	float m_LevelDb = -120.0f;
	// Fine and unsmoothed: measured, the comb structure that distinguishes two
	// room paths lives in the fine detail, and averaging it away took the
	// baseline correlation from 0.93 to 0.99.
	int m_FftSize = 2048;
	int m_SubBands = 8;
	int m_Segments = 0;
	float m_BandLowHz = 2400.0f;
	float m_BandHighHz = 4200.0f;
	float m_WindowSeconds = 8.0f;
	float m_AnalysisInterval = 2.0f;

	bool m_ForceHealthy[2] = { false, false };
	bool m_Mute[2] = { false, false };
	bool m_Paused = false;

	CaptureStage m_CaptureStage = CaptureStage::Idle;
	float m_CaptureStarted = 0.0f;
	float m_ElapsedSeconds = 0.0f;
	float m_LastAnalysis = 0.0f;
	float m_SampleDebt = 0.0f;

	std::vector<float> m_HealthHistory[2];
	float m_TruthHistory[2][kHistory] = {};
	int m_HistoryHead = 0;

	int m_Attempts = 0, m_Correct = 0, m_Undecided = 0, m_Wrong = 0;

	// --- View ---
	float m_CameraSpeed = 6.0f;
	float m_TurnSpeed = 90.0f;
	bool m_ShowDirect = true;
	bool m_ShowReflections = true;
	// Off by default: this prints once per analysis and would bury the log.
	bool m_LogVerdicts = false;
};
