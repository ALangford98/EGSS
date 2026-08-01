#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Audio/AudioClip.h"

namespace Egss {

	struct AudioParams
	{
		float Volume = 1.0f;
		// 1.0 is the original speed; 2.0 is an octave up and half as long.
		float Pitch = 1.0f;
		// -1 hard left, 0 centred, +1 hard right.
		float Pan = 0.0f;
		bool Loop = false;
	};

	// Playback built on a raw miniaudio device plus a small mixer of our own,
	// rather than miniaudio's higher-level engine. The mixer is where per-voice
	// gain, pan and pitch live, which is exactly what positional audio needs
	// to drive later.
	//
	// **The mixer runs on the audio thread**, which is driven by the device and
	// has nothing to do with the frame loop. It must never block, allocate, or
	// take a lock -- a stall there is an audible glitch, not a slow frame. So
	// voices are claimed with atomics: the main thread only writes to a voice
	// while it is inactive, and publishes it with a release store; the audio
	// thread only touches voices it has acquired as active. No mutex anywhere.
	class EGSS_API AudioEngine
	{
	public:
		static void Init();
		static void Shutdown();

		// False when no output device could be opened. Everything else still
		// works -- Play is simply inaudible -- so a machine with no sound card
		// doesn't need special handling at every call site.
		static bool IsAvailable();
		static const char* GetBackendName();
		static unsigned int GetSampleRate();

		static void Play(const std::shared_ptr<AudioClip>& clip, const AudioParams& params = AudioParams());
		static void StopAll();

		static void SetMasterVolume(float volume);
		static float GetMasterVolume();

		static unsigned int GetActiveVoiceCount();
		static unsigned int GetMaxVoices();

		// Runs the mixer straight into a buffer with no device involved, so
		// the output can be checked numerically -- including on a machine with
		// no audio hardware at all.
		//
		// Must not be called while a device is running: the audio thread is
		// mixing the same voices, and both would consume them. Call Shutdown
		// first, or use it before Init.
		static void RenderForTest(float* output, unsigned int frameCount);
	};

}
