// Load for the voice pool, so a race detector has something to detect.
//
// **Silence from a race detector is only evidence if the race window was
// actually open.** TSan reports pairs of accesses it *observed*; a sweep where
// the demos happen to play three sounds proves very little about a voice pool
// under churn. `./egss.py sanitize --thread` runs every demo with
// `--audio-stress` for exactly this reason.
//
// It earned its place on 2026-08-21: under this load TSan found eight racing
// pairs between `ClaimVoice` on the main thread and `MixInto` on the device
// thread -- `Stop` cleared a voice's flag mid-block, `ClaimVoice` took the slot
// as free, and reassigning `voice.Clip` dropped the last reference to samples
// the mixer was still reading. See the voice-phase protocol in `AudioEngine.cpp`
// for what replaced it. Without this layer the same sweep is clean.
//
// Two clips, alternating, so a stolen voice's old buffer is a *different*
// allocation from the new one -- reusing one clip would hide a use-after-free
// behind the buffer still being valid.
#pragma once

#include <Egss.h>

class AudioRaceStress : public Egss::Layer
{
public:
	AudioRaceStress() : Layer("AudioRaceStress") {}

	// Opt-in: it plays six overlapping tones a step, which is the point under a
	// race detector and a nuisance in every other run.
	static bool Requested()
	{
		for (const std::string& argument : Egss::Application::GetCommandLine())
			if (argument == "--audio-stress")
				return true;

		return false;
	}

	void OnAttach() override
	{
		if (!Requested())
			return;

		for (int c = 0; c < 2; c++)
		{
			// A short tone, so voices finish on their own as well as being
			// stopped -- both routes end with the slot free for reuse.
			const unsigned int rate = 48000, frames = rate / 4;
			std::vector<float> samples(frames);

			float hz = c == 0 ? 220.0f : 330.0f;
			for (unsigned int i = 0; i < frames; i++)
			{
				float t = (float)i / (float)rate;
				samples[i] = 0.02f * std::sin(6.2831853f * hz * t)
					* (1.0f - (float)i / (float)frames);
			}

			m_Clips[c] = Egss::AudioClip::CreateFromSamples(std::move(samples), 1);
		}
	}

	void OnFixedUpdate(Egss::Timestep) override
	{
		if (!m_Clips[0])
			return;

		m_Step++;

		// More voices claimed per step than the pool holds, so slots are
		// genuinely recycled rather than handed out from a free list that never
		// wraps.
		for (int i = 0; i < 6; i++)
		{
			Egss::Audio3DParams params;
			params.Position = { (float)(m_Step % 17) - 8.0f, 0.0f, 2.0f };
			params.Velocity = { 1.0f, 0.0f, 0.0f };
			params.Volume = 0.05f;

			Egss::VoiceHandle voice = Egss::AudioEngine::PlayAt(
				m_Clips[(m_Step + i) % 2], params);

			m_Live.push_back(voice);

			// Parameter writes from the main thread while the mixer reads them.
			Egss::AudioEngine::SetVoicePitch(voice, 0.9f + 0.2f * (float)(i % 3));
			Egss::AudioEngine::SetVoiceOcclusion(voice, 0.25f);
		}

		// Stop the oldest, so a slot is released and re-claimed as close
		// together as the loop can manage.
		while (m_Live.size() > 8)
		{
			Egss::AudioEngine::Stop(m_Live.front());
			m_Live.erase(m_Live.begin());
		}

		// And occasionally the blunt instrument, which takes a different path
		// through the mixer.
		if (m_Step % 97 == 0)
			Egss::AudioEngine::StopAll();
	}

private:
	std::shared_ptr<Egss::AudioClip> m_Clips[2];
	std::vector<Egss::VoiceHandle> m_Live;
	int m_Step = 0;
};
