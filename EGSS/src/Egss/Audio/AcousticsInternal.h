#pragma once

#include "Egss/Audio/Acoustics.h"

// Shared guts of the two tracers. **Not part of the public API** -- nothing
// outside Acoustics2D.cpp and Acoustics3D.cpp should include this.
//
// The split is by what depends on the dimension, not by what looked reusable.
// A tracer is: spread rays over directions, cast, absorb, ask whether the
// listener can see the bounce, scatter or mirror, repeat -- then summarise the
// histogram. Only ray spreading, the diffuse draw, and the cast itself differ
// between 2D and 3D. Everything here is the rest.

namespace Egss { namespace AcousticsDetail {

	float AbsorptionFor(const AcousticsSettings& settings, unsigned int body);

	// The same surface, per band. Clamped below 1 because a surface that
	// absorbs everything ends the ray, and clamped above 0 because a perfect
	// mirror never lets the trace terminate.
	void BandAbsorptionFor(const AcousticsSettings& settings, unsigned int body,
		float out[AcousticBandCount]);

	float ScatteringFor(const AcousticsSettings& settings, unsigned int body);

	// How much of what leaves a point reaches the listener, from distance
	// alone. Amplitude falls as 1/distance -- the same law Audio3DParams uses,
	// so a reflection and the direct sound stay on the same scale.
	float SpreadingGain(float distance, float minDistance);

	// A small fixed-stream generator. Scattering is the one stochastic part of
	// a trace, so it gets its own stream rather than std::rand: the same room
	// must trace the same way twice, or a listener who has not moved hears the
	// tail change under them every time it is rebuilt.
	struct Rng
	{
		unsigned int State;

		explicit Rng(unsigned int seed) : State(seed ? seed : 1u) {}

		float Unit()
		{
			State ^= State << 13; State ^= State >> 17; State ^= State << 5;
			return (float)(State >> 8) / (float)(1 << 24);
		}
	};

	// Schroeder backward integration: at each time, how much energy is still to
	// come. Turning a noisy decay into a monotonic curve is what makes a
	// straight-line fit meaningful -- fitting the raw echogram measures the
	// loudest spikes rather than the decay.
	//
	// `tracedBins` is how far the echogram was actually filled. Everything past
	// it is zero because tracing stopped, not because the room went quiet, and
	// backward integration turns that cliff into a plunge that looks exactly
	// like a very dead room. Fitting into it is the classic way to measure a
	// confidently wrong RT60, so the window has to end well before it.
	//
	// Returns RT60 in seconds, or 0 when the decay cannot be measured -- which
	// the caller must handle rather than treat as "no reverb".
	float ReverbTimeFromEchogram(const std::vector<float>& echogram,
		float binSeconds, size_t tracedBins);

	// What a trace accumulates that is not already a field of the result.
	//
	// Directions are vec3 in both dimensions. A 2D tracer leaves z at zero and
	// reads x and y back, which costs one unused float per bin and buys one
	// implementation of the summary instead of two.
	struct Bins
	{
		std::vector<glm::vec3> Direction;
		std::vector<float> PathLength;
		std::vector<int> Bounces;

		double AbsorptionWeighted = 0.0;
		double AbsorptionWeight = 0.0;
		double BandAbsorptionWeighted[AcousticBandCount] = {};
		double BandAbsorptionWeight[AcousticBandCount] = {};

		// How far the echogram is genuinely filled, as opposed to zero because
		// tracing stopped. The decay fit needs to know the difference.
		size_t Deepest = 0;

		float FreePathTotal = 0.0f;
		int FreePathCount = 0;
		float EffectiveRadius = 0.0f;

		// The direct sound's energy with nothing in the way. The late/direct
		// ratio is measured against this rather than against what got through:
		// dividing by what got through would divide by zero the moment the
		// source went behind a wall, and report a dry room at exactly the point
		// where all you can hear is the room.
		float UnoccludedEnergy = 0.0f;
	};

	// Sizes the echogram and the per-bin accumulators together, so a bin index
	// is always valid in all of them.
	size_t Allocate(AcousticsResultBase& result, Bins& bins,
		const AcousticsSettings& settings, float binSeconds, float speed);

	// One surface bounce that the listener can see. Shared because the thing
	// that goes wrong here is subtle and cost a session once: the spreading
	// loss applies to the **last leg only**. The packet a ray carries keeps its
	// energy as it travels; what varies is how much of what the surface
	// re-radiates the listener happens to catch, which depends on how far the
	// listener is from that surface, not on how far the sound has come. Using
	// the whole path applies the distance falloff a second time, and since path
	// length grows with time it shows up as a decay -- it made every measured
	// RT60 come out roughly half of what the room's absorption says.
	void RecordArrival(AcousticsResultBase& result, Bins& bins, size_t bin,
		float energy, const float bandEnergy[AcousticBandCount],
		float listenerDistance, float minDistance,
		const glm::vec3& arrivalDirection, float pathLength, int bounce);

	// Everything derived from a finished echogram, except the reflection list,
	// which is the one dimension-dependent part.
	void Finish(AcousticsResultBase& result, const Bins& bins,
		const AcousticsSettings& settings, float binSeconds, float speed);

	// The loudest early bins, soonest-first, capped at MaxReflections. Bin 0 is
	// the direct sound and is never a candidate.
	struct Candidate
	{
		size_t Bin = 0;
		float Energy = 0.0f;
	};

	std::vector<Candidate> EarlyReflections(const AcousticsResultBase& result,
		const AcousticsSettings& settings, float binSeconds);

} }
