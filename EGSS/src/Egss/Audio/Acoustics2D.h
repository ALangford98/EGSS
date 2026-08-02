#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/PhysicsWorld2D.h"

#include <glm/glm.hpp>

namespace Egss {

	// One path from source to listener: a delayed, quietened, directional copy
	// of the sound.
	struct EGSS_API ReflectionPath
	{
		float Delay = 0.0f;          // seconds after the direct sound
		float Gain = 0.0f;           // linear amplitude
		glm::vec2 Direction = { 0.0f, 0.0f };   // unit, arriving *at* the listener
		int Bounces = 0;
		float PathLength = 0.0f;
	};

	struct EGSS_API AcousticsSettings
	{
		// Rays cast from the source. Doubling this halves the variance in the
		// tail, and costs linearly. 128 is enough for a stable RT60 estimate in
		// a simple room; a corridor with many bounces wants more.
		int RayCount = 128;
		// Sound loses energy every bounce, so paths stop being worth tracing
		// long before this. It is a safety net, not a quality knob.
		int MaxBounces = 12;

		// Fraction of *energy* absorbed per bounce. 0 is a mirror, 1 is a
		// pillow. Real rooms: bare concrete about 0.02, carpet and drapes 0.4.
		float Absorption = 0.25f;
		// Per-body override, indexed by body handle. Anything past the end, or
		// negative, falls back to Absorption.
		const std::vector<float>* PerBodyAbsorption = nullptr;

		float SpeedOfSound = 343.0f;
		// Paths longer than this are dropped: at 343 m/s this is the tail
		// length in metres, and tracing past audibility is wasted work.
		float MaxPathLength = 120.0f;

		// A ray whose energy falls below this is abandoned.
		float MinEnergy = 1e-5f;
		// Amplitude at which sound stops counting as reaching anywhere, used
		// for the effective radius.
		float AudibleThreshold = 0.02f;

		// Stops a source right against a wall producing an enormous gain, and
		// matches the MinDistance idea in Audio3DParams.
		float MinDistance = 1.0f;

		// Rays are nudged this far off a surface before continuing, so the
		// next cast does not immediately re-hit the wall it just left.
		float SurfaceEpsilon = 0.002f;

		// How finely the echogram is sampled. 5 ms is well under the ~50 ms
		// the ear fuses reflections over, and coarse enough that 128 rays fill
		// the bins rather than speckling them.
		float BinSeconds = 0.005f;
		// Discrete early reflections are taken from before this; everything
		// after is summarised as a reverb tail. 80 ms is the usual dividing
		// line -- past it the ear stops hearing separate echoes.
		float EarlyCutoffSeconds = 0.08f;

		int MaxReflections = 8;
	};

	struct EGSS_API AcousticsResult
	{
		// --- The direct sound ---
		bool DirectPathClear = true;
		float DirectDistance = 0.0f;
		// 0 clear, 1 fully blocked. Graded, because a source half behind a
		// pillar should not flip between the two.
		float Occlusion = 0.0f;

		// --- Early reflections, soonest first ---
		std::vector<ReflectionPath> Reflections;

		// --- The tail ---
		// Time for the reverberant energy to fall by 60 dB. This is the number
		// a reverb wants. Measured from the traced decay when the trace ran
		// long enough to see it, and falling back to the Sabine estimate when
		// it did not -- check ReverbTimeMeasured to know which you got.
		float ReverbTime = 0.0f;
		bool ReverbTimeMeasured = false;

		// The Sabine estimate from the traced geometry, always available and
		// far cheaper than tracing a tail out to -25 dB.
		float SabineTime = 0.0f;

		// Energy-weighted mean absorption the rays actually met, which is what
		// Sabine needs -- not the average of the materials present.
		float MeanAbsorption = 0.0f;

		// How far into the echogram anything was actually recorded. Past this
		// the bins are zero because tracing stopped, not because the room went
		// quiet, and reading them as decay is how you measure a confidently
		// wrong RT60.
		float TracedSeconds = 0.0f;
		// Energy arriving after EarlyCutoffSeconds, relative to the direct
		// sound. Drives how wet the room should sound.
		float LateEnergyRatio = 0.0f;

		// --- What the geometry turned out to be ---
		// Average distance between bounces. For a convex 2D room this should
		// come out at pi * Area / Perimeter, which is worth checking.
		float MeanFreePath = 0.0f;
		// How far the sound actually carries, straight-line from the source.
		// Larger down a corridor than in a padded cell, which a fixed falloff
		// radius cannot express.
		float EffectiveRadius = 0.0f;

		int RaysEscaped = 0;
		int PathsFound = 0;
		int BouncesTraced = 0;

		// Energy per BinSeconds, index 0 starting at the direct sound. Kept so
		// a demo can draw the echogram, which is the only way to see what the
		// numbers above are summarising.
		std::vector<float> Echogram;
	};

	// One traced ray, for drawing. Not produced unless asked for.
	struct EGSS_API TracedRay
	{
		std::vector<glm::vec2> Points;   // source, then each bounce
		float FinalEnergy = 0.0f;
		bool Escaped = false;
	};

	// Works out what a room does to a sound, by tracing rays through the
	// physics world and watching where the energy goes.
	//
	// This is stochastic ray tracing, the same idea as room-acoustics tools,
	// cut down to what a game can afford: rays leave the source, bounce
	// specularly, lose energy to absorption, and at every bounce ask whether
	// the listener can be seen from there. Each answered question is one path
	// that sound could actually take. Collected into an energy-vs-time
	// histogram, those paths *are* the room's impulse response, coarsely
	// sampled -- early reflections at the front, a decaying tail behind.
	//
	// Deliberately not on the audio thread and not per audio block. Tracing is
	// milliseconds of work and geometry changes slowly; run it when the source
	// or listener has moved enough to matter, and feed the answer to the mixer.
	//
	// Approximations worth knowing about:
	//  - Specular only. Real surfaces scatter, which fills in the tail; here a
	//    smooth room can leave gaps between reflections.
	//  - One absorption figure per surface, not per frequency. Real rooms
	//    absorb treble far faster than bass, which is why the tail gets duller
	//    as it decays. The lowpass in the mixer's occlusion path is the
	//    nearest thing to it.
	//  - 2D. A room's floor and ceiling are half its reflecting area, so a
	//    traced RT60 here is longer than the same room in 3D.
	class EGSS_API Acoustics2D
	{
	public:
		static AcousticsResult Trace(const PhysicsWorld2D& world,
			const glm::vec2& source, const glm::vec2& listener,
			const AcousticsSettings& settings = AcousticsSettings(),
			std::vector<TracedRay>* debugRays = nullptr);

		// RT60 from the Sabine model, for comparison with a traced result.
		// meanFreePath is pi * Area / Perimeter in 2D.
		//
		// Energy after n bounces is (1 - a)^n, and bounces happen every
		// meanFreePath / speed seconds, so 60 dB down -- a factor of 10^-6 --
		// takes 6 ln(10) * mfp / (speed * -ln(1 - a)).
		static float SabineReverbTime(float meanFreePath, float absorption,
			float speedOfSound = 343.0f);
	};

}
