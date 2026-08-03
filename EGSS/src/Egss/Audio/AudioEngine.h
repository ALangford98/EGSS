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

	// One echo: the same sound again, later, quieter, from somewhere else.
	//
	// Deliberately just three numbers. Where they come from -- a ray trace, a
	// hand-authored preset, a lookup table -- is not the mixer's business, in
	// the same way that occlusion is handed in rather than worked out.
	struct EGSS_API AudioReflection
	{
		float Delay = 0.0f;   // seconds behind the direct sound
		float Gain = 0.0f;    // linear, relative to the voice's own volume
		float Pan = 0.0f;     // -1 left, +1 right
	};

	// One impulse in a room's response. Unlike a reflection, Gain is *signed*:
	// a tail built from same-sign impulses sums coherently and rings like a
	// comb filter, and it is the random signs that make it sound like a room
	// rather than a pipe.
	struct EGSS_API ReverbTap
	{
		float Delay = 0.0f;   // seconds
		float Gain = 0.0f;    // linear, may be negative
		float Pan = 0.0f;     // -1 left, +1 right

		// Which frequency band this impulse belongs to: 0 low, 1 mid, 2 high.
		// AllBands puts it in every one, which is what a broadband response
		// wants and is the default.
		//
		// Giving each band its own tail is what lets treble die away before
		// bass, which is most of what separates a room from a reverb preset.
		static constexpr unsigned int AllBands = 3u;
		unsigned int Band = AllBands;
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

		// Early reflections for one voice: a handful of delayed, quietened,
		// panned copies mixed in behind the direct sound. This is what turns
		// "the same sound, further away" into "the same sound, in a room".
		//
		// Taps past GetMaxReflections() are dropped, and so are delays longer
		// than GetMaxReflectionDelay() -- past about 80 ms the ear stops
		// hearing separate echoes and wants a reverb tail instead, which is
		// what SetReverb is for.
		//
		// Safe to call every frame. The mixer picks up whole tap sets at a
		// time, never a half-updated one.
		static void SetVoiceReflections(VoiceHandle voice,
			const AudioReflection* reflections, unsigned int count);
		static void ClearVoiceReflections(VoiceHandle voice);

		static unsigned int GetMaxReflections();
		static float GetMaxReflectionDelay();

		static void SetListener(const AudioListener& listener);
		static AudioListener GetListener();

		// Crossfaded over a fraction of a second rather than applied instantly.
		static void SetReverb(const ReverbSettings& settings);
		static ReverbSettings GetReverb();

		// Replaces the parametric tail with an actual impulse response: the
		// mixed output is convolved with these taps instead of being run
		// through comb and allpass filters. RoomSize and Damping stop applying;
		// Wet still does, because it is the only thing left to balance.
		//
		// This is direct convolution with a *sparse* response -- a few hundred
		// impulses rather than a dense recorded one. A dense two-second
		// response is 96,000 taps a sample, which needs partitioned FFT
		// convolution; a sparse one is affordable as written and is what the
		// ray tracer naturally produces anyway.
		//
		// Acoustics2D::BuildImpulseTaps turns a traced result into these.
		static void SetReverbImpulse(const ReverbTap* taps, unsigned int count);
		static void ClearReverbImpulse();
		static bool HasReverbImpulse();

		static unsigned int GetMaxReverbTaps();
		static float GetMaxImpulseLength();

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
