#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Audio/AudioEngine.h"

#include <glm/glm.hpp>

namespace Egss {

	// What a room does to a sound, in the parts that do not care how many
	// dimensions the room has.
	//
	// `Acoustics2D` and `Acoustics3D` differ in exactly three places -- how rays
	// are spread over directions, how a diffuse bounce is drawn, and which
	// world they cast against. Everything else is shared from here, on purpose:
	// the tail statistics are the subtle part (a truncation cliff that reads as
	// a dead room, a roughness window that must not measure the decay itself,
	// impulse amplitudes that have to be per interval rather than per bin), and
	// each of those was a bug once. Two copies would be two chances to fix one
	// of them and not the other.

	// Three bands is the fewest that can express the thing that matters: bass
	// outlasting treble. Real materials are measured in octave bands, but the
	// audible difference between three and eight is far smaller than the
	// difference between three and one.
	enum class AcousticBand { Low = 0, Mid = 1, High = 2 };
	constexpr int AcousticBandCount = 3;

	struct EGSS_API AcousticsSettings
	{
		// Rays cast from the source. Doubling this halves the variance in the
		// tail, and costs linearly. 128 is enough for a stable RT60 estimate in
		// a simple room; a corridor with many bounces wants more.
		//
		// **In 3D it wants far more than in 2D for the same variance.** Rays
		// spread over a sphere rather than a circle, so the same count is
		// stretched over a whole extra dimension of directions.
		int RayCount = 128;
		// Sound loses energy every bounce, so paths stop being worth tracing
		// long before this. It is a safety net, not a quality knob.
		int MaxBounces = 12;

		// Fraction of *energy* absorbed per bounce, in the mid band. 0 is a
		// mirror, 1 is a pillow. Real rooms: bare concrete about 0.02, carpet
		// and drapes 0.4.
		float Absorption = 0.25f;
		// Per-body override, indexed by body handle. Anything past the end, or
		// negative, falls back to Absorption.
		const std::vector<float>* PerBodyAbsorption = nullptr;

		// What each band's absorption is, as a multiple of the figure above.
		//
		// Almost everything absorbs treble faster than bass -- porous
		// materials by a lot, hard ones by less -- which is why a real tail
		// grows duller as it decays rather than just quieter. Setting all
		// three to 1 gives the old single-band behaviour back.
		float BandAbsorptionScale[AcousticBandCount] = { 0.55f, 1.0f, 1.9f };

		// Probability that a bounce leaves in a diffuse direction rather than
		// the mirror one. 0 is a mirror; 1 is a surface that has forgotten
		// which way the sound arrived.
		//
		// This is the assumption a specular tracer gets most visibly wrong.
		// In a rectangle with mirror walls a ray's direction only ever takes
		// four values, so the field never becomes diffuse: the late echogram
		// arrives as a comb of spikes with empty bins between them, and the
		// traced decay sits above the diffuse-field formula it gets compared
		// with. Scattering is what fills those gaps in.
		//
		// Real surfaces: flat plaster about 0.05, brickwork 0.1-0.2, a
		// bookcase or a coffered ceiling 0.4 and up.
		float Scattering = 0.0f;
		// Per-body override, indexed by body handle, on the same terms as
		// PerBodyAbsorption: past the end, or negative, falls back.
		const std::vector<float>* PerBodyScattering = nullptr;

		// Deliberately *not* per band, unlike absorption. A ray carries three
		// energy packets but only one direction, so it can only scatter or
		// mirror as a whole. A surface that mirrors bass and diffuses treble
		// -- which is what roughness comparable to a wavelength actually does
		// -- needs one ray per band, and three times the tracing cost.

		// Fixed, so a room that has not moved traces the same way every time
		// rather than shimmering as each retrace draws fresh noise.
		unsigned int ScatterSeed = 0x2545F491u;

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

	// Everything a trace measures that does not depend on the dimension. The
	// per-dimension results add their own reflection list on top, because a
	// direction of arrival is a vec2 in one and a vec3 in the other.
	struct EGSS_API AcousticsResultBase
	{
		// --- The direct sound ---
		bool DirectPathClear = true;
		float DirectDistance = 0.0f;
		// 0 clear, 1 fully blocked. Graded, because a source half behind a
		// pillar should not flip between the two.
		float Occlusion = 0.0f;

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

		// Per band. High should come out well under Low in any normal room,
		// and that difference is most of what makes a tail sound real.
		float BandReverbTime[AcousticBandCount] = {};
		float BandMeanAbsorption[AcousticBandCount] = {};

		// How far into the echogram anything was actually recorded. Past this
		// the bins are zero because tracing stopped, not because the room went
		// quiet, and reading them as decay is how you measure a confidently
		// wrong RT60.
		float TracedSeconds = 0.0f;
		// Energy arriving after EarlyCutoffSeconds, relative to the direct
		// sound. Drives how wet the room should sound.
		float LateEnergyRatio = 0.0f;

		// --- What the geometry turned out to be ---
		// Average distance between bounces. Worth checking against a formula
		// the tracer does not contain: pi * Area / Perimeter for a convex 2D
		// room, and 4 * Volume / Surface for a convex 3D one.
		float MeanFreePath = 0.0f;
		// How far the sound actually carries, straight-line from the source.
		// Larger down a corridor than in a padded cell, which a fixed falloff
		// radius cannot express.
		float EffectiveRadius = 0.0f;

		int RaysEscaped = 0;
		int PathsFound = 0;
		int BouncesTraced = 0;
		// Of those, how many left diffusely. At Scattering 0 this is 0 and the
		// trace is the old specular one exactly, which is worth being able to
		// confirm rather than assume.
		int BouncesScattered = 0;

		// How bumpy the late echogram is around its own decay, in dB RMS: for
		// each late bin, how far it sits from the average of the bins either
		// side. This is the number that says whether the tail is noise or a
		// comb. Mirror walls send every ray back through the same few
		// directions, so arrivals pile into spikes with near-empty bins
		// between them; scattering fills the gaps in and the figure falls.
		//
		// Counting *empty* bins was the obvious measure and is useless: at a
		// few hundred rays and 5 ms bins every bin gets something either way,
		// so it read 100% for a mirror room and 100% for a diffuse one. The
		// spread is what differs -- about 3.7 dB specular against 0.8 dB
		// fully scattering, in a bare rectangle.
		float TailRoughness = 0.0f;

		// Energy per BinSeconds, index 0 starting at the direct sound. Kept so
		// a demo can draw the echogram, which is the only way to see what the
		// numbers above are summarising. This is the sum of the three bands.
		std::vector<float> Echogram;

		// The same, split by band. The reverb needs these separately: one tail
		// per band is what lets the treble die away first.
		std::vector<float> BandEchogram[AcousticBandCount];
	};

	struct EGSS_API ImpulseSettings
	{
		// Impulses per second of tail. Too few and the tail rattles; too many
		// and it costs more than it sounds like. A few hundred is the range
		// where it stops being audible as separate events.
		int Density = 260;
		int MaxTaps = 512;

		// Where the tail starts. Usually the early-reflection cutoff, since
		// those are played as discrete taps on the voice and would otherwise
		// be heard twice.
		float StartSeconds = 0.08f;

		// Overall level. The traced energies are on the same scale as the
		// direct sound, which is rarely the scale a mix wants.
		float Gain = 1.0f;

		// Fixed by default, so a stationary room does not shimmer as its tail
		// is rebuilt with different noise every retrace.
		unsigned int Seed = 0x9E3779B9u;
	};

	// The parts of the answer that are the same in any number of dimensions.
	class EGSS_API Acoustics
	{
	public:
		// Turns a traced echogram into a sparse impulse response the mixer can
		// convolve with.
		//
		// The impulses are placed one per equal interval at a pseudo-random
		// offset inside it -- "velvet noise". Even spacing alone would comb;
		// fully random placement clumps and leaves gaps. One per interval,
		// jittered, gives even density without periodicity.
		//
		// **Signs are random, and that is the whole trick.** Same-sign
		// impulses sum coherently into a ringing comb; random signs sum
		// incoherently into noise, which is what a diffuse tail is. It also
		// sets the amplitude: n incoherent impulses of amplitude a carry n*a*a
		// of energy, so a bin holding energy E wants a = sqrt(E / n).
		//
		// Pans are spread across the field rather than aimed. A late tail is
		// diffuse by definition -- if you can tell which direction it came
		// from, it is still an early reflection.
		static std::vector<ReverbTap> BuildImpulseTaps(const AcousticsResultBase& result,
			const ImpulseSettings& settings = ImpulseSettings(),
			float binSeconds = 0.005f);

		// RT60 from the Sabine model, for comparison with a traced result.
		// meanFreePath is pi * Area / Perimeter in 2D and 4 * Volume / Surface
		// in 3D -- the tracer measures it, this does not assume it.
		//
		// Energy after n bounces is (1 - a)^n, and bounces happen every
		// meanFreePath / speed seconds, so 60 dB down -- a factor of 10^-6 --
		// takes 6 ln(10) * mfp / (speed * -ln(1 - a)).
		//
		// This is Eyring's form. Sabine's own is the same with `a` in place of
		// `-ln(1 - a)`, which agrees below about a = 0.2 and over-predicts the
		// tail badly above it -- a room with a = 0.8 is not four times deader
		// than one with a = 0.2, it is *far* deader. The name is kept because
		// callers already use it.
		static float SabineReverbTime(float meanFreePath, float absorption,
			float speedOfSound = 343.0f);
	};

}
