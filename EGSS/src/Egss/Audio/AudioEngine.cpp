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

		// Metres per second. Only relevant to Doppler, and only relative to
		// whatever scale the game's world units are in.
		constexpr float s_SpeedOfSound = 343.0f;

		// A wildly fast source would otherwise produce a pitch ratio that
		// sounds like a fault rather than an effect.
		constexpr float s_MinDoppler = 0.5f;
		constexpr float s_MaxDoppler = 2.0f;

		// How loud a fully occluded source is, and how far its top end is
		// rolled off. Not silence: a wall muffles sound, it rarely removes it.
		constexpr float s_OccludedGain = 0.30f;
		constexpr float s_OpenCutoffHz = 18000.0f;   // effectively no filtering
		constexpr float s_OccludedCutoffHz = 480.0f;

		// Seconds for occlusion to travel from one value to another. Long
		// enough that a ray flickering across an edge does not click, short
		// enough to feel immediate.
		constexpr float s_OcclusionSmoothing = 0.05f;

		// Early reflections. Eight taps is enough for a room to read as a room;
		// past that the ear is hearing a tail, not echoes, and a tail is what
		// the reverb is for.
		constexpr unsigned int s_MaxReflections = 8;
		// 200 ms of history per voice. At 48 kHz that is 38 KB a voice, which
		// buys the whole early-reflection window with room to spare.
		constexpr float s_MaxReflectionDelay = 0.200f;

		// Tap sets are published by rotating through a ring of them, so the
		// mixer is never reading the one being written. Eight deep means a set
		// is not reused for eight updates -- over a tenth of a second at frame
		// rate, and orders of magnitude longer than an audio block.
		constexpr unsigned int s_TapSetCount = 8;

		// Convolution reverb. 512 impulses over two seconds is roughly 250 a
		// second, which is sparse enough to be affordable and dense enough
		// that the ear hears a tail rather than a handful of echoes.
		constexpr unsigned int s_MaxReverbTaps = 512;
		constexpr float s_MaxImpulseSeconds = 2.0f;

		// Seconds to crossfade between reverb settings, so stepping through a
		// doorway is a change of room rather than a click.
		constexpr float s_ReverbSmoothing = 0.35f;

		// Comb and allpass lengths, in samples. These are Freeverb's tunings
		// scaled from 44.1kHz to 48kHz. The exact numbers matter less than the
		// fact that they are mutually prime-ish: lengths sharing factors make
		// echoes line up, which is heard as a ringing tone rather than a room.
		constexpr unsigned int s_CombLengths[4] = { 1214, 1293, 1390, 1476 };
		constexpr unsigned int s_AllpassLengths[2] = { 605, 480 };

		// The right channel's delays are offset, which is what gives the tail
		// its stereo spread.
		constexpr unsigned int s_StereoSpread = 25;

		// Keeps the summed combs inside a sane range before the allpasses.
		constexpr float s_ReverbInputGain = 0.045f;

		// One delay line with a damping low pass inside its feedback path.
		struct Comb
		{
			std::vector<float> Buffer;
			unsigned int Index = 0;
			float FilterStore = 0.0f;

			void Resize(unsigned int length)
			{
				Buffer.assign(length, 0.0f);
				Index = 0;
				FilterStore = 0.0f;
			}

			float Process(float input, float feedback, float damping)
			{
				float output = Buffer[Index];

				// Low pass in the feedback loop: each pass round the delay
				// loses a little more of its top end.
				FilterStore = output * (1.0f - damping) + FilterStore * damping;

				Buffer[Index] = input + FilterStore * feedback;
				if (++Index >= Buffer.size())
					Index = 0;

				return output;
			}
		};

		// Passes all frequencies at equal level but smears them in time, which
		// is what turns the combs' discrete echoes into something diffuse.
		struct Allpass
		{
			std::vector<float> Buffer;
			unsigned int Index = 0;

			void Resize(unsigned int length)
			{
				Buffer.assign(length, 0.0f);
				Index = 0;
			}

			float Process(float input)
			{
				float buffered = Buffer[Index];
				float output = -input + buffered;

				Buffer[Index] = input + buffered * 0.5f;
				if (++Index >= Buffer.size())
					Index = 0;

				return output;
			}
		};

		struct ReverbChannel
		{
			Comb Combs[4];
			Allpass Allpasses[2];

			void Resize(unsigned int spread)
			{
				for (int i = 0; i < 4; i++)
					Combs[i].Resize(s_CombLengths[i] + spread);
				for (int i = 0; i < 2; i++)
					Allpasses[i].Resize(s_AllpassLengths[i] + spread);
			}

			float Process(float input, float feedback, float damping)
			{
				// Combs in parallel, allpasses in series -- the classic
				// Schroeder arrangement.
				float sum = 0.0f;
				for (Comb& comb : Combs)
					sum += comb.Process(input, feedback, damping);

				for (Allpass& allpass : Allpasses)
					sum = allpass.Process(sum);

				return sum;
			}
		};

		// Three independent atomics rather than one atomic vector. A torn read
		// across components means one axis is a block stale, which is
		// inaudible; the alternative is a lock in the audio callback.
		struct AtomicVec3
		{
			std::atomic<float> X{ 0.0f }, Y{ 0.0f }, Z{ 0.0f };

			void Store(const glm::vec3& v)
			{
				X.store(v.x, std::memory_order_relaxed);
				Y.store(v.y, std::memory_order_relaxed);
				Z.store(v.z, std::memory_order_relaxed);
			}

			glm::vec3 Load() const
			{
				return { X.load(std::memory_order_relaxed),
						 Y.load(std::memory_order_relaxed),
						 Z.load(std::memory_order_relaxed) };
			}
		};

		// A whole tap set is published at once. The fields are plain floats
		// rather than atomics: the release store on the index orders every
		// write here before the mixer's acquire can see the new index.
		struct ReflectionTapSet
		{
			unsigned int Count = 0;
			unsigned int DelaySamples[s_MaxReflections] = {};
			float GainL[s_MaxReflections] = {};
			float GainR[s_MaxReflections] = {};
		};

		struct Voice
		{
			Voice()
			{
				// The only allocation a voice ever makes, and it happens
				// before any audio thread exists. Doing it lazily on first use
				// would put a malloc in the mixer.
				ReflectionHistory.assign(
					(size_t)(s_MaxReflectionDelay * (float)s_SampleRate), 0.0f);
			}

			// The handshake between threads. Main writes the non-atomic fields
			// only while this is false, then publishes with a release store;
			// the audio thread acquires it before touching anything.
			std::atomic<bool> Active{ false };

			// Bumped every time the slot is claimed, so a handle issued for an
			// earlier sound can be recognised as stale.
			std::atomic<unsigned int> Generation{ 1 };

			// Kept so the samples cannot be freed underneath the mixer. Never
			// cleared on the audio thread -- releasing the last reference there
			// could free memory in the callback. The main thread overwrites it
			// when it reuses the voice instead.
			std::shared_ptr<AudioClip> Clip;

			const float* Samples = nullptr;
			unsigned int FrameCount = 0;
			unsigned int Channels = 2;

			// Fractional, so pitch can be anything rather than integer steps.
			// Audio thread only.
			double Cursor = 0.0;

			// Changeable while playing, so each needs to be atomic.
			std::atomic<float> Volume{ 1.0f };
			std::atomic<float> Pitch{ 1.0f };
			std::atomic<float> Pan{ 0.0f };
			std::atomic<bool> Loop{ false };

			// --- Positional ---
			std::atomic<bool> Is3D{ false };
			AtomicVec3 Position;
			AtomicVec3 Velocity;
			std::atomic<float> MinDistance{ 1.0f };
			std::atomic<float> MaxDistance{ 20.0f };
			std::atomic<float> DopplerFactor{ 1.0f };

			// Requested by the game; the mixer chases it rather than jumping.
			std::atomic<float> Occlusion{ 0.0f };

			// Audio thread only.
			float SmoothedOcclusion = 0.0f;
			float LowpassL = 0.0f;
			float LowpassR = 0.0f;

			// --- Early reflections ---
			// The voice's recent output, so a tap can read what it played some
			// milliseconds ago. Audio thread only.
			std::vector<float> ReflectionHistory;
			unsigned int ReflectionWrite = 0;

			ReflectionTapSet TapSets[s_TapSetCount];
			// Which set the mixer should read. Published with a release store
			// after the set itself is filled.
			std::atomic<unsigned int> ActiveTapSet{ 0 };
			// Main thread only: where the next set goes.
			unsigned int NextTapSet = 0;

			// What the mixer last worked out, purely so a debug panel can show
			// why something sounds the way it does.
			std::atomic<float> DebugDistance{ 0.0f };
			std::atomic<float> DebugGain{ 0.0f };
			std::atomic<float> DebugPan{ 0.0f };
			std::atomic<float> DebugPitchScale{ 1.0f };
			std::atomic<float> DebugOcclusion{ 0.0f };
		};

		// The room's response, published whole. Same rotation trick as the
		// per-voice reflection taps.
		struct ImpulseSet
		{
			unsigned int Count = 0;
			unsigned int DelaySamples[s_MaxReverbTaps] = {};
			float GainL[s_MaxReverbTaps] = {};
			float GainR[s_MaxReverbTaps] = {};
		};

		struct AudioState
		{
			AudioState()
			{
				// The mixer must never allocate, so the one buffer the
				// convolution needs is sized here -- during the first touch of
				// State(), which is always on the main thread.
				ImpulseHistory.assign(
					(size_t)(s_MaxImpulseSeconds * (float)s_SampleRate), 0.0f);
			}

			ma_device Device;
			bool DeviceReady = false;
			bool Initialised = false;

			Voice Voices[s_MaxVoices];
			std::atomic<float> MasterVolume{ 1.0f };
			std::atomic<bool> StopRequested{ false };

			AtomicVec3 ListenerPosition;
			AtomicVec3 ListenerForward;
			AtomicVec3 ListenerUp;
			AtomicVec3 ListenerVelocity;

			// Targets, set from the game thread.
			std::atomic<float> ReverbWet{ 0.0f };
			std::atomic<float> ReverbRoomSize{ 0.7f };
			std::atomic<float> ReverbDamping{ 0.4f };
			std::atomic<float> ReverbWidth{ 1.0f };

			// --- Convolution reverb ---
			// What the mix sounded like recently, so a tap can read it back.
			// Audio thread only; sized once, below.
			std::vector<float> ImpulseHistory;
			unsigned int ImpulseWrite = 0;

			ImpulseSet ImpulseSets[4];
			std::atomic<unsigned int> ActiveImpulseSet{ 0 };
			unsigned int NextImpulseSet = 0;
			// Read by the mixer to decide which reverb to run at all.
			std::atomic<bool> ImpulseActive{ false };

			// Audio thread only: what is actually being applied right now.
			float SmoothedWet = 0.0f;
			float SmoothedRoomSize = 0.7f;
			float SmoothedDamping = 0.4f;

			ReverbChannel ReverbLeft;
			ReverbChannel ReverbRight;
		};

		AudioState& State()
		{
			static AudioState state;
			return state;
		}

		// Equal power: a sound panned hard left is as loud as one panned
		// centre. Linear panning dips in the middle, which is audible as a
		// sound crosses the stereo field.
		void PanGains(float pan, float& left, float& right)
		{
			float angle = (std::min(std::max(pan, -1.0f), 1.0f) + 1.0f) * 0.25f * 3.14159265f;
			left = std::cos(angle);
			right = std::sin(angle);
		}

		// Turns a world position into gain, pan and a pitch multiplier.
		void ComputeSpatial(const Voice& voice, const AudioState& state,
			float& outGain, float& outPan, float& outPitchScale, float& outDistance)
		{
			glm::vec3 listenerPosition = state.ListenerPosition.Load();
			glm::vec3 sourcePosition = voice.Position.Load();

			glm::vec3 toSource = sourcePosition - listenerPosition;
			float distance = glm::length(toSource);
			outDistance = distance;

			glm::vec3 direction = distance > 0.0001f ? toSource / distance : glm::vec3(0.0f);

			// --- Attenuation ---
			float minDistance = voice.MinDistance.load(std::memory_order_relaxed);
			float maxDistance = voice.MaxDistance.load(std::memory_order_relaxed);

			if (distance <= minDistance)
			{
				outGain = 1.0f;
			}
			else if (distance >= maxDistance || maxDistance <= minDistance)
			{
				outGain = 0.0f;
			}
			else
			{
				// Inverse distance is how sound actually behaves, but on its
				// own it never reaches zero -- so it is multiplied by a linear
				// fade that does. Without that, distant sources pile up and
				// muddy everything.
				float inverse = minDistance / distance;
				float fade = 1.0f - (distance - minDistance) / (maxDistance - minDistance);
				outGain = inverse * fade;
			}

			// --- Pan ---
			// How far round to the listener's right the source sits. Purely
			// horizontal: with stereo output there is nothing to do with the
			// vertical component.
			glm::vec3 forward = state.ListenerForward.Load();
			glm::vec3 up = state.ListenerUp.Load();
			glm::vec3 right = glm::cross(forward, up);

			float rightLength = glm::length(right);
			if (rightLength > 0.0001f && distance > 0.0001f)
				outPan = std::min(std::max(glm::dot(direction, right / rightLength), -1.0f), 1.0f);
			else
				outPan = 0.0f;

			// --- Doppler ---
			float dopplerFactor = voice.DopplerFactor.load(std::memory_order_relaxed);
			if (dopplerFactor <= 0.0f || distance <= 0.0001f)
			{
				outPitchScale = 1.0f;
				return;
			}

			// Closing speeds along the line between the two. A source moving
			// away raises the denominator and drops the pitch; a listener
			// moving towards it raises the numerator and lifts it.
			float listenerAlong = glm::dot(state.ListenerVelocity.Load(), direction) * dopplerFactor;
			float sourceAlong = glm::dot(voice.Velocity.Load(), direction) * dopplerFactor;

			float denominator = s_SpeedOfSound + sourceAlong;
			if (std::abs(denominator) < 0.0001f)
			{
				outPitchScale = s_MaxDoppler;
				return;
			}

			float ratio = (s_SpeedOfSound + listenerAlong) / denominator;
			outPitchScale = std::min(std::max(ratio, s_MinDoppler), s_MaxDoppler);
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

				float volume = voice.Volume.load(std::memory_order_relaxed);
				float pan = voice.Pan.load(std::memory_order_relaxed);
				float pitch = voice.Pitch.load(std::memory_order_relaxed);

				// Recomputed every block rather than once at Play, so a moving
				// source or a turning listener is heard straight away.
				if (voice.Is3D.load(std::memory_order_relaxed))
				{
					float gain, spatialPan, pitchScale, distance;
					ComputeSpatial(voice, state, gain, spatialPan, pitchScale, distance);

					volume *= gain;
					pan = spatialPan;
					pitch *= pitchScale;

					voice.DebugDistance.store(distance, std::memory_order_relaxed);
					voice.DebugGain.store(gain, std::memory_order_relaxed);
					voice.DebugPan.store(spatialPan, std::memory_order_relaxed);
					voice.DebugPitchScale.store(pitchScale, std::memory_order_relaxed);
				}

				// --- Occlusion ---
				// Chased rather than jumped to. The block rate is high enough
				// (a few hundred Hz) that a per-block step is smooth.
				float targetOcclusion = voice.Occlusion.load(std::memory_order_relaxed);
				float blockSeconds = (float)frameCount / (float)s_SampleRate;
				float smoothing = std::min(blockSeconds / s_OcclusionSmoothing, 1.0f);
				voice.SmoothedOcclusion += (targetOcclusion - voice.SmoothedOcclusion) * smoothing;

				float occlusion = std::min(std::max(voice.SmoothedOcclusion, 0.0f), 1.0f);
				voice.DebugOcclusion.store(occlusion, std::memory_order_relaxed);

				volume *= 1.0f + (s_OccludedGain - 1.0f) * occlusion;

				// One-pole low pass. The coefficient is the fraction of the
				// way the filter moves towards the input each sample, derived
				// from the cutoff frequency.
				float cutoff = s_OpenCutoffHz + (s_OccludedCutoffHz - s_OpenCutoffHz) * occlusion;
				float lowpassCoefficient = 1.0f - std::exp(-6.2831853f * cutoff / (float)s_SampleRate);
				bool filtering = occlusion > 0.0001f;

				float left, right;
				PanGains(pan, left, right);
				left *= volume * master;
				right *= volume * master;

				bool loop = voice.Loop.load(std::memory_order_relaxed);

				// One tap set for the whole block: picking it up once keeps
				// every frame consistent, and a set can never be seen
				// half-updated.
				const ReflectionTapSet& taps =
					voice.TapSets[voice.ActiveTapSet.load(std::memory_order_acquire)];

				// Reflections carry their own attenuation -- the trace that
				// produced them already accounted for how far each path
				// travelled and what it bounced off. Scaling them by the
				// direct sound's distance gain as well would count the
				// distance twice, and echoes would vanish exactly when a
				// room should be making them obvious.
				float reflectionScale = voice.Volume.load(std::memory_order_relaxed) * master;

				const size_t historySize = voice.ReflectionHistory.size();

				for (unsigned int frame = 0; frame < frameCount; frame++)
				{
					unsigned int index = (unsigned int)voice.Cursor;

					if (index >= voice.FrameCount)
					{
						if (!loop)
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

					if (filtering)
					{
						voice.LowpassL += (sampleL - voice.LowpassL) * lowpassCoefficient;
						voice.LowpassR += (sampleR - voice.LowpassR) * lowpassCoefficient;
						sampleL = voice.LowpassL;
						sampleR = voice.LowpassR;
					}

					output[(size_t)frame * s_Channels + 0] += sampleL * left;
					output[(size_t)frame * s_Channels + 1] += sampleR * right;

					// --- Early reflections ---
					if (historySize > 0)
					{
						// What the source emitted, before panning: an echo off
						// a wall to the left arrives from the left whatever
						// side the source itself is on.
						voice.ReflectionHistory[voice.ReflectionWrite] =
							0.5f * (sampleL + sampleR);

						for (unsigned int t = 0; t < taps.Count; t++)
						{
							unsigned int delay = taps.DelaySamples[t];

							// Wrap backwards without a modulo of a negative.
							size_t read = (voice.ReflectionWrite + historySize - delay) % historySize;
							float echo = voice.ReflectionHistory[read] * reflectionScale;

							output[(size_t)frame * s_Channels + 0] += echo * taps.GainL[t];
							output[(size_t)frame * s_Channels + 1] += echo * taps.GainR[t];
						}

						voice.ReflectionWrite = (voice.ReflectionWrite + 1) % (unsigned int)historySize;
					}

					voice.Cursor += pitch;
				}
			}

			// --- Reverb ---
			{
				float blockSeconds = (float)frameCount / (float)s_SampleRate;
				float smoothing = std::min(blockSeconds / s_ReverbSmoothing, 1.0f);

				float targetWet = state.ReverbWet.load(std::memory_order_relaxed);
				state.SmoothedWet += (targetWet - state.SmoothedWet) * smoothing;
				state.SmoothedRoomSize += (state.ReverbRoomSize.load(std::memory_order_relaxed)
					- state.SmoothedRoomSize) * smoothing;
				state.SmoothedDamping += (state.ReverbDamping.load(std::memory_order_relaxed)
					- state.SmoothedDamping) * smoothing;

				bool convolving = state.ImpulseActive.load(std::memory_order_acquire);

				// --- Convolution tail ---
				// The mix, convolved with the room's own response, instead of
				// being pushed through comb and allpass filters that only
				// resemble one.
				if (convolving && state.SmoothedWet > 0.0005f)
				{
					const ImpulseSet& impulse =
						state.ImpulseSets[state.ActiveImpulseSet.load(std::memory_order_acquire)];

					const size_t historySize = state.ImpulseHistory.size();
					float wet = std::min(std::max(state.SmoothedWet, 0.0f), 1.0f);

					for (unsigned int frame = 0; frame < frameCount; frame++)
					{
						float dryL = output[(size_t)frame * s_Channels + 0];
						float dryR = output[(size_t)frame * s_Channels + 1];

						// One shared input, as with the comb reverb: the
						// stereo image comes from the taps' panning.
						state.ImpulseHistory[state.ImpulseWrite] = (dryL + dryR) * 0.5f;

						float wetL = 0.0f, wetR = 0.0f;
						for (unsigned int t = 0; t < impulse.Count; t++)
						{
							size_t read = (state.ImpulseWrite + historySize
								- impulse.DelaySamples[t]) % historySize;

							float sample = state.ImpulseHistory[read];
							wetL += sample * impulse.GainL[t];
							wetR += sample * impulse.GainR[t];
						}

						output[(size_t)frame * s_Channels + 0] = dryL * (1.0f - wet) + wetL * wet;
						output[(size_t)frame * s_Channels + 1] = dryR * (1.0f - wet) + wetR * wet;

						state.ImpulseWrite = (state.ImpulseWrite + 1) % (unsigned int)historySize;
					}
				}
				// Skipped entirely when dry, so a game with no reverb pays
				// nothing for it. The tail is lost rather than faded, which is
				// why the wet level is smoothed -- by the time it reaches zero
				// there is nothing audible left to cut off.
				else if (state.SmoothedWet > 0.0005f)
				{
					float feedback = std::min(std::max(state.SmoothedRoomSize, 0.0f), 0.98f);
					float damping = std::min(std::max(state.SmoothedDamping, 0.0f), 1.0f);
					float width = std::min(std::max(state.ReverbWidth.load(std::memory_order_relaxed), 0.0f), 1.0f);
					float wet = std::min(std::max(state.SmoothedWet, 0.0f), 1.0f);

					for (unsigned int frame = 0; frame < frameCount; frame++)
					{
						float dryL = output[(size_t)frame * s_Channels + 0];
						float dryR = output[(size_t)frame * s_Channels + 1];

						// Both channels are fed the same summed input; the
						// stereo image comes from the delay lengths differing.
						float input = (dryL + dryR) * s_ReverbInputGain;

						float wetL = state.ReverbLeft.Process(input, feedback, damping);
						float wetR = state.ReverbRight.Process(input, feedback, damping);

						// Width blends towards mono by mixing the two tails.
						float mixedL = wetL * width + wetR * (1.0f - width);
						float mixedR = wetR * width + wetL * (1.0f - width);

						output[(size_t)frame * s_Channels + 0] = dryL * (1.0f - wet) + mixedL * wet;
						output[(size_t)frame * s_Channels + 1] = dryR * (1.0f - wet) + mixedR * wet;
					}
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

		// Index in the low 8 bits, generation in the 24 above. No extra marker
		// bit: generation starts at 1, so a valid handle is always >= 0x100 and
		// therefore never collides with InvalidVoice. An earlier version OR'd
		// in 0x80000000 to guarantee that, which sat inside the generation
		// field and made every handle fail its own generation check.
		VoiceHandle MakeHandle(unsigned int index, unsigned int generation)
		{
			return ((generation & 0x00ffffffu) << 8) | (index & 0xffu);
		}

		Voice* Resolve(VoiceHandle handle)
		{
			if (handle == InvalidVoice)
				return nullptr;

			unsigned int index = handle & 0xffu;
			unsigned int generation = (handle >> 8) & 0x00ffffffu;

			if (index >= s_MaxVoices)
				return nullptr;

			Voice& voice = State().Voices[index];

			// A slot that has been reused, or has finished, must not be
			// controlled by an old handle.
			if (voice.Generation.load(std::memory_order_acquire) != generation)
				return nullptr;
			if (!voice.Active.load(std::memory_order_acquire))
				return nullptr;

			return &voice;
		}

		VoiceHandle ClaimVoice(const std::shared_ptr<AudioClip>& clip)
		{
			AudioState& state = State();

			for (unsigned int i = 0; i < s_MaxVoices; i++)
			{
				Voice& voice = state.Voices[i];

				// Only an inactive voice may be written to; the audio thread
				// is guaranteed not to be reading it.
				if (voice.Active.load(std::memory_order_acquire))
					continue;

				unsigned int generation = voice.Generation.load(std::memory_order_relaxed) + 1;
				if (generation > 0x00ffffffu)
					generation = 1;
				voice.Generation.store(generation, std::memory_order_release);

				voice.Clip = clip;
				voice.Samples = clip->GetSamples();
				voice.FrameCount = clip->GetFrameCount();
				voice.Channels = clip->GetChannels();
				voice.Cursor = 0.0;

				// Filter and occlusion state belong to the sound that just
				// ended, not the one starting.
				voice.SmoothedOcclusion = 0.0f;
				voice.LowpassL = 0.0f;
				voice.LowpassR = 0.0f;
				voice.Occlusion.store(0.0f, std::memory_order_relaxed);

				// The previous sound's echoes must not leak into this one.
				// Safe to touch here: the voice is inactive, so the mixer is
				// not reading it.
				std::fill(voice.ReflectionHistory.begin(), voice.ReflectionHistory.end(), 0.0f);
				voice.ReflectionWrite = 0;
				voice.TapSets[voice.ActiveTapSet.load(std::memory_order_relaxed)].Count = 0;

				return MakeHandle(i, generation);
			}

			// Dropping the sound is the right failure: stealing a voice would
			// cut off something already audible.
			return InvalidVoice;
		}

	}

	void AudioEngine::Init()
	{
		AudioState& state = State();
		if (state.Initialised)
			return;

		state.Initialised = true;

		state.ListenerForward.Store({ 0.0f, 0.0f, -1.0f });
		state.ListenerUp.Store({ 0.0f, 1.0f, 0.0f });

		// Allocated once, here, so the audio thread never allocates.
		state.ReverbLeft.Resize(0);
		state.ReverbRight.Resize(s_StereoSpread);

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

	VoiceHandle AudioEngine::Play(const std::shared_ptr<AudioClip>& clip, const AudioParams& params)
	{
		EGSS_PROFILE_SCOPE("Audio::Play");

		if (!clip || clip->GetFrameCount() == 0)
			return InvalidVoice;

		VoiceHandle handle = ClaimVoice(clip);
		if (handle == InvalidVoice)
			return InvalidVoice;

		Voice& voice = State().Voices[handle & 0xffu];

		voice.Volume.store(params.Volume, std::memory_order_relaxed);
		voice.Pitch.store(params.Pitch > 0.0f ? params.Pitch : 1.0f, std::memory_order_relaxed);
		voice.Pan.store(params.Pan, std::memory_order_relaxed);
		voice.Loop.store(params.Loop, std::memory_order_relaxed);
		voice.Is3D.store(false, std::memory_order_relaxed);

		// Everything above must be visible before the voice goes live.
		voice.Active.store(true, std::memory_order_release);
		return handle;
	}

	VoiceHandle AudioEngine::PlayAt(const std::shared_ptr<AudioClip>& clip, const Audio3DParams& params)
	{
		EGSS_PROFILE_SCOPE("Audio::PlayAt");

		if (!clip || clip->GetFrameCount() == 0)
			return InvalidVoice;

		VoiceHandle handle = ClaimVoice(clip);
		if (handle == InvalidVoice)
			return InvalidVoice;

		Voice& voice = State().Voices[handle & 0xffu];

		voice.Volume.store(params.Volume, std::memory_order_relaxed);
		voice.Pitch.store(params.Pitch > 0.0f ? params.Pitch : 1.0f, std::memory_order_relaxed);
		voice.Pan.store(0.0f, std::memory_order_relaxed);
		voice.Loop.store(params.Loop, std::memory_order_relaxed);

		voice.Position.Store(params.Position);
		voice.Velocity.Store(params.Velocity);
		voice.MinDistance.store(std::max(params.MinDistance, 0.0001f), std::memory_order_relaxed);
		voice.MaxDistance.store(params.MaxDistance, std::memory_order_relaxed);
		voice.DopplerFactor.store(params.DopplerFactor, std::memory_order_relaxed);
		voice.Is3D.store(true, std::memory_order_relaxed);

		voice.Active.store(true, std::memory_order_release);
		return handle;
	}

	bool AudioEngine::IsPlaying(VoiceHandle voice)
	{
		return Resolve(voice) != nullptr;
	}

	void AudioEngine::Stop(VoiceHandle handle)
	{
		if (Voice* voice = Resolve(handle))
			voice->Active.store(false, std::memory_order_release);
	}

	void AudioEngine::StopAll()
	{
		State().StopRequested.store(true, std::memory_order_release);
	}

	void AudioEngine::SetVoicePosition(VoiceHandle handle, const glm::vec3& position,
		const glm::vec3& velocity)
	{
		if (Voice* voice = Resolve(handle))
		{
			voice->Position.Store(position);
			voice->Velocity.Store(velocity);
		}
	}

	void AudioEngine::SetVoiceVolume(VoiceHandle handle, float volume)
	{
		if (Voice* voice = Resolve(handle))
			voice->Volume.store(volume, std::memory_order_relaxed);
	}

	void AudioEngine::SetVoicePitch(VoiceHandle handle, float pitch)
	{
		if (Voice* voice = Resolve(handle))
			voice->Pitch.store(pitch > 0.0f ? pitch : 1.0f, std::memory_order_relaxed);
	}

	void AudioEngine::SetVoiceOcclusion(VoiceHandle handle, float amount)
	{
		if (Voice* voice = Resolve(handle))
			voice->Occlusion.store(std::min(std::max(amount, 0.0f), 1.0f), std::memory_order_relaxed);
	}

	unsigned int AudioEngine::GetMaxReflections() { return s_MaxReflections; }
	float AudioEngine::GetMaxReflectionDelay() { return s_MaxReflectionDelay; }

	void AudioEngine::SetVoiceReflections(VoiceHandle handle,
		const AudioReflection* reflections, unsigned int count)
	{
		Voice* voice = Resolve(handle);
		if (!voice)
			return;

		if (!reflections)
			count = 0;

		// Filled somewhere the mixer is not looking, then published. Writing
		// the live set in place would let a block read three old taps and one
		// new one, which is a click.
		unsigned int slot = voice->NextTapSet;
		ReflectionTapSet& set = voice->TapSets[slot];

		const size_t historySize = voice->ReflectionHistory.size();
		unsigned int written = 0;

		for (unsigned int i = 0; i < count && written < s_MaxReflections; i++)
		{
			const AudioReflection& reflection = reflections[i];

			// A tap at zero delay is the direct sound, which is already
			// playing; one past the buffer would read the future.
			unsigned int delay = (unsigned int)(reflection.Delay * (float)s_SampleRate + 0.5f);
			if (delay == 0 || delay >= historySize)
				continue;

			if (!(reflection.Gain > 0.0f))
				continue;

			float left, right;
			PanGains(std::min(std::max(reflection.Pan, -1.0f), 1.0f), left, right);

			set.DelaySamples[written] = delay;
			set.GainL[written] = left * reflection.Gain;
			set.GainR[written] = right * reflection.Gain;
			written++;
		}

		set.Count = written;

		// Release: everything above must be visible before the mixer can
		// select this set.
		voice->ActiveTapSet.store(slot, std::memory_order_release);
		voice->NextTapSet = (slot + 1) % s_TapSetCount;
	}

	void AudioEngine::ClearVoiceReflections(VoiceHandle handle)
	{
		SetVoiceReflections(handle, nullptr, 0);
	}

	void AudioEngine::SetListener(const AudioListener& listener)
	{
		AudioState& state = State();
		state.ListenerPosition.Store(listener.Position);
		state.ListenerForward.Store(listener.Forward);
		state.ListenerUp.Store(listener.Up);
		state.ListenerVelocity.Store(listener.Velocity);
	}

	AudioListener AudioEngine::GetListener()
	{
		AudioState& state = State();

		AudioListener listener;
		listener.Position = state.ListenerPosition.Load();
		listener.Forward = state.ListenerForward.Load();
		listener.Up = state.ListenerUp.Load();
		listener.Velocity = state.ListenerVelocity.Load();
		return listener;
	}

	void AudioEngine::SetReverb(const ReverbSettings& settings)
	{
		AudioState& state = State();
		state.ReverbWet.store(settings.Wet, std::memory_order_relaxed);
		state.ReverbRoomSize.store(settings.RoomSize, std::memory_order_relaxed);
		state.ReverbDamping.store(settings.Damping, std::memory_order_relaxed);
		state.ReverbWidth.store(settings.Width, std::memory_order_relaxed);
	}

	ReverbSettings AudioEngine::GetReverb()
	{
		AudioState& state = State();

		ReverbSettings settings;
		settings.Wet = state.ReverbWet.load(std::memory_order_relaxed);
		settings.RoomSize = state.ReverbRoomSize.load(std::memory_order_relaxed);
		settings.Damping = state.ReverbDamping.load(std::memory_order_relaxed);
		settings.Width = state.ReverbWidth.load(std::memory_order_relaxed);
		return settings;
	}

	unsigned int AudioEngine::GetMaxReverbTaps() { return s_MaxReverbTaps; }
	float AudioEngine::GetMaxImpulseLength() { return s_MaxImpulseSeconds; }

	bool AudioEngine::HasReverbImpulse()
	{
		return State().ImpulseActive.load(std::memory_order_acquire);
	}

	void AudioEngine::SetReverbImpulse(const ReverbTap* taps, unsigned int count)
	{
		AudioState& state = State();

		if (!taps)
			count = 0;

		unsigned int slot = state.NextImpulseSet;
		ImpulseSet& set = state.ImpulseSets[slot];

		const size_t historySize = state.ImpulseHistory.size();
		unsigned int written = 0;

		for (unsigned int i = 0; i < count && written < s_MaxReverbTaps; i++)
		{
			const ReverbTap& tap = taps[i];

			unsigned int delay = (unsigned int)(tap.Delay * (float)s_SampleRate + 0.5f);
			if (delay == 0 || delay >= historySize)
				continue;

			// Gain is signed here, unlike a reflection: the sign is what makes
			// the tail diffuse rather than a ringing comb, so it must survive.
			if (!(std::abs(tap.Gain) > 0.0f))
				continue;

			float left, right;
			PanGains(std::min(std::max(tap.Pan, -1.0f), 1.0f), left, right);

			set.DelaySamples[written] = delay;
			set.GainL[written] = left * tap.Gain;
			set.GainR[written] = right * tap.Gain;
			written++;
		}

		set.Count = written;

		state.ActiveImpulseSet.store(slot, std::memory_order_release);
		state.NextImpulseSet = (slot + 1) % 4;

		// Published last, so the mixer never selects the convolution path
		// before the set it would read is complete.
		state.ImpulseActive.store(written > 0, std::memory_order_release);
	}

	void AudioEngine::ClearReverbImpulse()
	{
		AudioState& state = State();

		// Switched off before anything else, so the mixer stops reading the
		// taps before they go.
		state.ImpulseActive.store(false, std::memory_order_release);
		state.ImpulseSets[state.ActiveImpulseSet.load(std::memory_order_relaxed)].Count = 0;
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

	bool AudioEngine::GetVoiceDebug(VoiceHandle handle, VoiceDebug& out)
	{
		Voice* voice = Resolve(handle);
		if (!voice)
			return false;

		out.Distance = voice->DebugDistance.load(std::memory_order_relaxed);
		out.Gain = voice->DebugGain.load(std::memory_order_relaxed);
		out.Pan = voice->DebugPan.load(std::memory_order_relaxed);
		out.PitchScale = voice->DebugPitchScale.load(std::memory_order_relaxed);
		out.Occlusion = voice->DebugOcclusion.load(std::memory_order_relaxed);
		return true;
	}

	void AudioEngine::RenderForTest(float* output, unsigned int frameCount)
	{
		MixInto(output, frameCount);
	}

}
