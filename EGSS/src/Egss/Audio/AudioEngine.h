#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Audio/AudioClip.h"

#include <glm/glm.hpp>

namespace Egss {

	// Where the ears are. Forward and Up define the orientation, and Velocity
	// only matters for Doppler.
	//
	// PerspectiveCamera already exposes exactly these -- GetPosition,
	// GetForward, GetUp -- which is not a coincidence.
	struct EGSS_API AudioListener
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Forward = { 0.0f, 0.0f, -1.0f };
		glm::vec3 Up = { 0.0f, 1.0f, 0.0f };
		glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };
	};

	struct AudioParams
	{
		float Volume = 1.0f;
		// 1.0 is the original speed; 2.0 is an octave up and half as long.
		float Pitch = 1.0f;
		// -1 hard left, 0 centred, +1 hard right.
		float Pan = 0.0f;
		bool Loop = false;
	};

	struct Audio3DParams
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		// Only used for Doppler; leave at zero for a stationary source.
		glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };

		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;

		// Full volume within MinDistance, silent at MaxDistance, inverse-square
		// -ish in between. Getting MinDistance right matters more than it
		// looks: it is the size of the sound, and setting it too small makes
		// everything drop to nothing the moment you step away.
		float MinDistance = 1.0f;
		float MaxDistance = 20.0f;

		// 0 disables Doppler, 1 is physically plausible, higher exaggerates.
		float DopplerFactor = 1.0f;
	};

	// What the mixer last worked out for a voice, for debug display.
	struct EGSS_API VoiceDebug
	{
		float Distance = 0.0f;
		float Gain = 0.0f;
		float Pan = 0.0f;
		float PitchScale = 1.0f;
		// Smoothed, so this is what is actually being applied rather than the
		// value most recently requested.
		float Occlusion = 0.0f;
	};

	// A room's character, applied to everything the listener hears.
	//
	// This is a "zone" in the sense that games use the word: the game decides
	// which region the listener is in and hands over that region's settings.
	// The engine crossfades between whatever it is given, so walking through a
	// doorway is a smooth change rather than a jump.
	struct EGSS_API ReverbSettings
	{
		// How much of the wet signal is heard. 0 disables the effect entirely,
		// and skips the processing.
		float Wet = 0.0f;
		// Feedback in the comb filters -- effectively how long the tail is.
		float RoomSize = 0.7f;
		// How quickly the high frequencies die away. Real rooms lose their top
		// end fastest, so a little of this stops the tail sounding metallic.
		float Damping = 0.4f;
		// 0 collapses the tail to mono, 1 spreads it fully.
		float Width = 1.0f;
	};

	// Refers to a playing voice. Carries a generation counter, so a handle to
	// a sound that has already finished cannot accidentally control whatever
	// sound reused its slot.
	using VoiceHandle = unsigned int;
	constexpr VoiceHandle InvalidVoice = 0;

	// Playback built on a raw miniaudio device plus a small mixer of our own,
	// rather than miniaudio's higher-level engine. The mixer is where per-voice
	// gain, pan and pitch live, which is what positional audio needs to drive.
	//
	// **The mixer runs on the audio thread**, which is driven by the device and
	// has nothing to do with the frame loop. It must never block, allocate, or
	// take a lock -- a stall there is an audible glitch, not a slow frame. So
	// voices are claimed with atomics: the main thread only writes to a voice
	// while it is inactive, and publishes it with a release store; the audio
	// thread only touches voices it has acquired as active. Parameters that can
	// change mid-playback are individually atomic. No mutex anywhere.
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

		// Fixed pan and volume, for UI and non-diegetic sound.
		static VoiceHandle Play(const std::shared_ptr<AudioClip>& clip,
			const AudioParams& params = AudioParams());

		// Positioned in the world. Volume, pan and Doppler are recomputed from
		// the listener on every audio block, so moving either the source or
		// the listener is heard immediately without the caller doing anything.
		static VoiceHandle PlayAt(const std::shared_ptr<AudioClip>& clip,
			const Audio3DParams& params);

		static bool IsPlaying(VoiceHandle voice);
		static void Stop(VoiceHandle voice);
		static void StopAll();

		// Ignored if the handle is stale, so a caller holding a handle to a
		// finished one-shot cannot disturb an unrelated sound.
		static void SetVoicePosition(VoiceHandle voice, const glm::vec3& position,
			const glm::vec3& velocity = glm::vec3(0.0f));
		static void SetVoiceVolume(VoiceHandle voice, float volume);
		static void SetVoicePitch(VoiceHandle voice, float pitch);

		// 0 is a clear line to the listener, 1 is fully blocked. Quietens the
		// voice and rolls off its high frequencies, which is what actually
		// sells "behind a wall" -- attenuation alone just sounds further away.
		//
		// The engine deliberately does not work this out for itself: what
		// counts as an occluder is a game question, and the audio system has
		// no business depending on the physics one. Raycast from the listener
		// to the source and feed the answer in.
		//
		// Changes are smoothed over a few milliseconds, so a ray flicking
		// between blocked and clear does not click.
		static void SetVoiceOcclusion(VoiceHandle voice, float amount);

		static void SetListener(const AudioListener& listener);
		static AudioListener GetListener();

		// Crossfaded over a fraction of a second rather than applied instantly.
		static void SetReverb(const ReverbSettings& settings);
		static ReverbSettings GetReverb();

		static void SetMasterVolume(float volume);
		static float GetMasterVolume();

		static unsigned int GetActiveVoiceCount();
		static unsigned int GetMaxVoices();

		// Returns false for a stale handle.
		static bool GetVoiceDebug(VoiceHandle voice, VoiceDebug& out);

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
