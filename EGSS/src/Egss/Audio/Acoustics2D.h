#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Physics/PhysicsWorld2D.h"
#include "Egss/Audio/Acoustics.h"

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

	// The bands, the settings, the tail statistics and the impulse taps all live
	// in Acoustics.h, shared with the 3D tracer. What is left here is the parts
	// that are 2D: a direction of arrival, and the ray path a demo draws.
	struct EGSS_API AcousticsResult : public AcousticsResultBase
	{
		// Early reflections, soonest first.
		std::vector<ReflectionPath> Reflections;
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
	//  - Scattering is decided per bounce, not per band, and it only steers
	//    the ray onward: a bounce is still detected at the listener with no
	//    regard for which way it was heading. That detection was always a
	//    diffuse-rain approximation -- a truly specular reflection reaches the
	//    listener only from the mirror direction -- and it stays one.
	//  - Three absorption bands, not a curve. Real materials are measured in
	//    octave bands and a soft surface's curve is nothing like a hard one's;
	//    three is only enough to hear bass outlast treble.
	//  - 2D. A room's floor and ceiling are half its reflecting area, so a
	//    traced RT60 here is longer than the same room in 3D. `Acoustics3D` is
	//    the same tracer without that approximation -- measured, this room's
	//    2D mean free path is 1.7x its 3D one.
	class EGSS_API Acoustics2D
	{
	public:
		static AcousticsResult Trace(const PhysicsWorld2D& world,
			const glm::vec2& source, const glm::vec2& listener,
			const AcousticsSettings& settings = AcousticsSettings(),
			std::vector<TracedRay>* debugRays = nullptr);

		// Both of these are dimension-independent and now live on `Acoustics`.
		// Kept here as forwarders because callers already use these names, and
		// a rename would be churn with nothing measured at the end of it.
		static std::vector<ReverbTap> BuildImpulseTaps(const AcousticsResultBase& result,
			const ImpulseSettings& settings = ImpulseSettings(),
			float binSeconds = 0.005f)
		{
			return Acoustics::BuildImpulseTaps(result, settings, binSeconds);
		}

		static float SabineReverbTime(float meanFreePath, float absorption,
			float speedOfSound = 343.0f)
		{
			return Acoustics::SabineReverbTime(meanFreePath, absorption, speedOfSound);
		}
	};

}
