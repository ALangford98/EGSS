#include "egsspch.h"
#include "Egss/Audio/AudioEngine.h"

#include "Egss/Log.h"
#include "Egss/Debug/Instrumentor.h"

#include <atomic>

#include "miniaudio.h"

namespace Egss {

	namespace {

		// Everything is mixed and played at this format. Clips are converted
		// to it on load so the mixer never has to.
		constexpr unsigned int s_SampleRate = 48000;
		constexpr unsigned int s_Channels = 2;
		constexpr unsigned int s_MaxVoices = 32;

		struct Voice
		{
			// The handshake between threads. Main writes the fields below only
			// while this is false, then publishes with a release store; the
			// audio thread acquires it before touching anything.
			std::atomic<bool> Active{ false };

			// Kept so the samples cannot be freed underneath the mixer. Never
			// cleared on the audio thread -- releasing the last reference there
			// could free memory in the callback. The main thread overwrites it
			// when it reuses the voice instead.
			std::shared_ptr<AudioClip> Clip;

			const float* Samples = nullptr;
			unsigned int FrameCount = 0;
			unsigned int Channels = 2;

			// Fractional, so pitch can be anything rather than integer steps.
			double Cursor = 0.0;

			float Volume = 1.0f;
			float Pitch = 1.0f;
			float Pan = 0.0f;
			bool Loop = false;
		};

		struct AudioState
		{
			ma_device Device;
			bool DeviceReady = false;
			bool Initialised = false;

			Voice Voices[s_MaxVoices];
			std::atomic<float> MasterVolume{ 1.0f };
			std::atomic<bool> StopRequested{ false };
		};

		AudioState& State()
		{
			static AudioState state;
			return state;
		}

		// Equal power: a sound panned hard left is as loud as one panned
		// centre. Linear panning dips in the middle, which is audible as a
		// sound crossing the stereo field.
		void PanGains(float pan, float& left, float& right)
		{
			float angle = (std::min(std::max(pan, -1.0f), 1.0f) + 1.0f) * 0.25f * 3.14159265f;
			left = std::cos(angle);
			right = std::sin(angle);
		}

		// The mixer. Runs on the audio thread in the real device path, and on
		// the calling thread from RenderForTest.
		void MixInto(float* output, unsigned int frameCount)
		{
			AudioState& state = State();

			std::fill(output, output + (size_t)frameCount * s_Channels, 0.0f);

			if (state.StopRequested.exchange(false))
			{
				for (Voice& voice : state.Voices)
					voice.Active.store(false, std::memory_order_release);
				return;
			}

			float master = state.MasterVolume.load(std::memory_order_relaxed);

			for (Voice& voice : state.Voices)
			{
				if (!voice.Active.load(std::memory_order_acquire))
					continue;

				float left, right;
				PanGains(voice.Pan, left, right);
				left *= voice.Volume * master;
				right *= voice.Volume * master;

				for (unsigned int frame = 0; frame < frameCount; frame++)
				{
					unsigned int index = (unsigned int)voice.Cursor;

					if (index >= voice.FrameCount)
					{
						if (!voice.Loop)
						{
							// Releasing here is what lets the main thread
							// safely reuse this slot.
							voice.Active.store(false, std::memory_order_release);
							break;
						}

						voice.Cursor = 0.0;
						index = 0;
					}

					// Linear interpolation between neighbouring frames, so a
					// pitch that isn't a whole number doesn't sound gritty.
					float fraction = (float)(voice.Cursor - (double)index);
					unsigned int next = index + 1 < voice.FrameCount ? index + 1 : index;

					float sampleL, sampleR;
					if (voice.Channels == 1)
					{
						float a = voice.Samples[index];
						float b = voice.Samples[next];
						sampleL = sampleR = a + (b - a) * fraction;
					}
					else
					{
						float aL = voice.Samples[(size_t)index * voice.Channels];
						float bL = voice.Samples[(size_t)next * voice.Channels];
						float aR = voice.Samples[(size_t)index * voice.Channels + 1];
						float bR = voice.Samples[(size_t)next * voice.Channels + 1];

						sampleL = aL + (bL - aL) * fraction;
						sampleR = aR + (bR - aR) * fraction;
					}

					output[(size_t)frame * s_Channels + 0] += sampleL * left;
					output[(size_t)frame * s_Channels + 1] += sampleR * right;

					voice.Cursor += voice.Pitch;
				}
			}

			// Summing voices can exceed full scale; clamping is ugly but it is
			// far less ugly than the wrap-around that happens otherwise.
			for (size_t i = 0; i < (size_t)frameCount * s_Channels; i++)
				output[i] = std::min(std::max(output[i], -1.0f), 1.0f);
		}

		void DeviceCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount)
		{
			(void)device;
			(void)input;
			MixInto((float*)output, frameCount);
		}

	}

	void AudioEngine::Init()
	{
		AudioState& state = State();
		if (state.Initialised)
			return;

		state.Initialised = true;

		ma_device_config config = ma_device_config_init(ma_device_type_playback);
		config.playback.format = ma_format_f32;
		config.playback.channels = s_Channels;
		config.sampleRate = s_SampleRate;
		config.dataCallback = DeviceCallback;

		if (ma_device_init(nullptr, &config, &state.Device) != MA_SUCCESS)
		{
			EGSS_CORE_WARN("AudioEngine: no playback device; audio will be silent");
			return;
		}

		if (ma_device_start(&state.Device) != MA_SUCCESS)
		{
			EGSS_CORE_WARN("AudioEngine: device found but would not start; audio will be silent");
			ma_device_uninit(&state.Device);
			return;
		}

		state.DeviceReady = true;

		EGSS_CORE_INFO("Audio {0} | {1} Hz, {2} ch, {3} voices",
			ma_get_backend_name(state.Device.pContext->backend),
			state.Device.sampleRate, s_Channels, s_MaxVoices);
	}

	void AudioEngine::Shutdown()
	{
		AudioState& state = State();
		if (!state.Initialised)
			return;

		if (state.DeviceReady)
		{
			// Stops the callback before anything it reads goes away.
			ma_device_uninit(&state.Device);
			state.DeviceReady = false;
		}

		for (Voice& voice : state.Voices)
		{
			voice.Active.store(false, std::memory_order_release);
			voice.Clip.reset();
		}

		state.Initialised = false;
	}

	bool AudioEngine::IsAvailable()
	{
		return State().DeviceReady;
	}

	const char* AudioEngine::GetBackendName()
	{
		AudioState& state = State();
		return state.DeviceReady ? ma_get_backend_name(state.Device.pContext->backend) : "none";
	}

	unsigned int AudioEngine::GetSampleRate()
	{
		return s_SampleRate;
	}

	void AudioEngine::Play(const std::shared_ptr<AudioClip>& clip, const AudioParams& params)
	{
		EGSS_PROFILE_SCOPE("Audio::Play");

		if (!clip || clip->GetFrameCount() == 0)
			return;

		AudioState& state = State();

		for (Voice& voice : state.Voices)
		{
			// Only an inactive voice may be written to; the audio thread is
			// guaranteed not to be reading it.
			if (voice.Active.load(std::memory_order_acquire))
				continue;

			voice.Clip = clip;
			voice.Samples = clip->GetSamples();
			voice.FrameCount = clip->GetFrameCount();
			voice.Channels = clip->GetChannels();
			voice.Cursor = 0.0;
			voice.Volume = params.Volume;
			voice.Pitch = params.Pitch > 0.0f ? params.Pitch : 1.0f;
			voice.Pan = params.Pan;
			voice.Loop = params.Loop;

			// Everything above must be visible before the voice goes live.
			voice.Active.store(true, std::memory_order_release);
			return;
		}

		// Dropping the sound is the right failure: stealing a voice would cut
		// off something already audible.
	}

	void AudioEngine::StopAll()
	{
		State().StopRequested.store(true, std::memory_order_release);
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		State().MasterVolume.store(std::min(std::max(volume, 0.0f), 1.0f), std::memory_order_relaxed);
	}

	float AudioEngine::GetMasterVolume()
	{
		return State().MasterVolume.load(std::memory_order_relaxed);
	}

	unsigned int AudioEngine::GetActiveVoiceCount()
	{
		unsigned int count = 0;
		for (Voice& voice : State().Voices)
		{
			if (voice.Active.load(std::memory_order_acquire))
				count++;
		}
		return count;
	}

	unsigned int AudioEngine::GetMaxVoices()
	{
		return s_MaxVoices;
	}

	void AudioEngine::RenderForTest(float* output, unsigned int frameCount)
	{
		MixInto(output, frameCount);
	}

}
