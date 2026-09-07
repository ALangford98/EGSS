#include "gspch.h"
#include "GS/Audio/Acoustics2D.h"
#include "GS/Audio/AcousticsInternal.h"
#include "GS/Debug/Instrumentor.h"

#include <glm/gtc/constants.hpp>

namespace GS {

	namespace {

		using namespace AcousticsDetail;

		// Mirror about the surface normal. The tangential part of the
		// direction survives, the normal part flips.
		glm::vec2 Reflect(const glm::vec2& direction, const glm::vec2& normal)
		{
			return direction - 2.0f * glm::dot(direction, normal) * normal;
		}

		// A direction drawn from Lambert's cosine law about the normal, which
		// is what a surface rough compared with the wavelength sends back
		// regardless of which way the sound arrived.
		//
		// In 2D the law is pdf(theta) = cos(theta)/2 over the half-circle
		// either side of the normal. Its CDF is (sin(theta) + 1)/2, so
		// inverting it is just theta = asin(2u - 1) -- one call, no rejection
		// loop, and every sample lands in the correct half-circle by
		// construction rather than by being retried until it does.
		//
		// Drawing uniformly over the half-circle instead is the obvious thing
		// and is wrong: it sends far too much energy along the wall, which
		// lengthens the mean free path and with it the decay.
		glm::vec2 CosineDirection(const glm::vec2& normal, float u)
		{
			float theta = std::asin(glm::clamp(2.0f * u - 1.0f, -1.0f, 1.0f));

			// Rotate the normal by theta. The normal already points back out
			// of the surface, so this cannot aim into it.
			float c = std::cos(theta), s = std::sin(theta);
			return { normal.x * c - normal.y * s, normal.x * s + normal.y * c };
		}

	}

	AcousticsResult Acoustics2D::Trace(const PhysicsWorld2D& world,
		const glm::vec2& source, const glm::vec2& listener,
		const AcousticsSettings& settings, std::vector<TracedRay>* debugRays)
	{
		GS_PROFILE_SCOPE("Acoustics2D::Trace");

		AcousticsResult result;
		AcousticsDetail::Bins bins;

		const float epsilon = settings.SurfaceEpsilon;
		const float speed = glm::max(settings.SpeedOfSound, 1.0f);
		const int rayCount = glm::max(settings.RayCount, 1);

		AcousticsDetail::Rng rng(settings.ScatterSeed);
		auto unit = [&rng]() { return rng.Unit(); };

		// --- The direct sound ---------------------------------------------
		glm::vec2 toListener = listener - source;
		result.DirectDistance = glm::length(toListener);

		if (result.DirectDistance > 1e-5f)
		{
			glm::vec2 direction = toListener / result.DirectDistance;

			// Graded rather than yes/no: five rays spread across the listener,
			// so a source half behind a pillar reads half blocked instead of
			// flickering between clear and silent as either one moves.
			const int probes = 5;
			const float spread = 0.35f;
			glm::vec2 perpendicular(-direction.y, direction.x);

			int blocked = 0;
			for (int i = 0; i < probes; i++)
			{
				float offset = ((float)i / (float)(probes - 1) - 0.5f) * 2.0f * spread;
				glm::vec2 target = listener + perpendicular * offset;
				glm::vec2 delta = target - source;
				float distance = glm::length(delta);

				if (world.Raycast(source, delta / distance, distance - epsilon).Hit)
					blocked++;
			}

			result.Occlusion = (float)blocked / (float)probes;
			result.DirectPathClear = (blocked == 0);
		}

		// --- The echogram -------------------------------------------------
		const float binSeconds = glm::max(settings.BinSeconds, 1e-4f);
		const size_t binCount = AcousticsDetail::Allocate(result, bins, settings,
			binSeconds, speed);

		// Bin 0 holds what actually arrives directly, which is nothing when a
		// wall is in the way.
		float directGain = SpreadingGain(result.DirectDistance, settings.MinDistance);
		bins.UnoccludedEnergy = directGain * directGain;
		float directArriving = bins.UnoccludedEnergy * (1.0f - result.Occlusion);

		result.Echogram[0] += directArriving;
		// The direct sound has not bounced off anything, so it is unfiltered:
		// every band gets its full share.
		for (int band = 0; band < AcousticBandCount; band++)
			result.BandEchogram[band][0] += directArriving;

		const float energyPerRay = 1.0f / (float)rayCount;

		bins.EffectiveRadius = result.DirectPathClear ? result.DirectDistance : 0.0f;

		if (debugRays)
		{
			debugRays->clear();
			debugRays->reserve(rayCount);
		}

		for (int i = 0; i < rayCount; i++)
		{
			// Evenly spread, offset by half a step so the first ray is not
			// axis-aligned -- an axis-aligned ray in a rectangular room hits
			// the corner exactly and behaves unlike every other ray.
			float angle = glm::two_pi<float>() * ((float)i + 0.5f) / (float)rayCount;
			glm::vec2 direction(std::cos(angle), std::sin(angle));

			glm::vec2 origin = source;
			// One packet per band. They start equal and diverge as they bounce,
			// which is the whole point: after a dozen surfaces the treble
			// packet is a fraction of the bass one.
			float bandEnergy[AcousticBandCount];
			for (int band = 0; band < AcousticBandCount; band++)
				bandEnergy[band] = energyPerRay;

			float energy = energyPerRay;   // broadband, for the summary figures
			float travelled = 0.0f;

			TracedRay debug;
			if (debugRays)
				debug.Points.push_back(source);

			for (int bounce = 0; bounce < settings.MaxBounces; bounce++)
			{
				float remaining = settings.MaxPathLength - travelled;
				if (remaining <= 0.0f)
					break;

				RaycastHit hit = world.Raycast(origin, direction, remaining);
				if (!hit.Hit)
				{
					// Running out of path budget is not the same as leaving the
					// room, and counting it as an escape makes a sealed room
					// look leaky. One more cast, unbounded, tells them apart.
					bool escaped = !world.Raycast(origin, direction,
						settings.MaxPathLength * 4.0f).Hit;

					if (escaped)
						result.RaysEscaped++;

					if (debugRays)
					{
						debug.Escaped = escaped;
						debug.Points.push_back(origin + direction * remaining);
					}
					break;
				}

				travelled += hit.Distance;
				result.BouncesTraced++;

				// The first leg starts at the source, not at a wall, so it is
				// not a free path between surfaces and would bias the average.
				if (bounce > 0)
				{
					bins.FreePathTotal += hit.Distance;
					bins.FreePathCount++;
				}

				if (debugRays)
					debug.Points.push_back(hit.Point);

				// Energy left after this surface takes its share.
				float absorption = AbsorptionFor(settings, hit.Body);
				bins.AbsorptionWeighted += (double)absorption * (double)energy;
				bins.AbsorptionWeight += (double)energy;
				energy *= (1.0f - absorption);

				float bandAbsorption[AcousticBandCount];
				BandAbsorptionFor(settings, hit.Body, bandAbsorption);

				for (int band = 0; band < AcousticBandCount; band++)
				{
					bins.BandAbsorptionWeighted[band] += (double)bandAbsorption[band] * (double)bandEnergy[band];
					bins.BandAbsorptionWeight[band] += (double)bandEnergy[band];
					bandEnergy[band] *= (1.0f - bandAbsorption[band]);
				}

				// Off the surface before doing anything else, or the next cast
				// starts inside the wall it just hit.
				glm::vec2 surfacePoint = hit.Point + hit.Normal * epsilon;

				// --- Can the listener hear this bounce? ---
				glm::vec2 hitToListener = listener - surfacePoint;
				float listenerDistance = glm::length(hitToListener);

				if (listenerDistance > 1e-5f)
				{
					glm::vec2 toL = hitToListener / listenerDistance;

					if (!world.Raycast(surfacePoint, toL, listenerDistance - epsilon).Hit)
					{
						float pathLength = travelled + listenerDistance;
						float delay = pathLength / speed;

						size_t bin = (size_t)(delay / binSeconds);
						if (bin < binCount)
						{
							AcousticsDetail::RecordArrival(result, bins, bin,
								energy, bandEnergy, listenerDistance,
								settings.MinDistance,
								glm::vec3(-toL, 0.0f), pathLength, bounce);
						}
					}
				}

				// How far the sound is still carrying, for the radius. Distance
				// falloff has to be in here, or an absorbent corridor reads the
				// same as a live one -- both simply run to the far wall.
				// Straight-line from the source rather than path length,
				// because this is a culling radius, not a travel time.
				float carriedAmplitude = std::sqrt(energy / energyPerRay)
					* SpreadingGain(travelled, settings.MinDistance);

				if (carriedAmplitude > settings.AudibleThreshold)
					bins.EffectiveRadius = glm::max(bins.EffectiveRadius,
						glm::length(hit.Point - source));

				// Only give up when *every* band is spent. The bass outlasts
				// the treble by a long way, and stopping on the broadband
				// figure would cut the low tail short.
				bool anyLeft = energy >= settings.MinEnergy;
				for (int band = 0; band < AcousticBandCount; band++)
					anyLeft |= bandEnergy[band] >= settings.MinEnergy;

				if (!anyLeft)
					break;

				// Scatter or mirror, decided per bounce rather than by
				// splitting the ray in two. One ray that goes diffuse a
				// fraction s of the time carries the same energy in each
				// direction as two rays weighted s and 1-s, and it keeps the
				// cost of a trace independent of how rough the room is.
				float scattering = ScatteringFor(settings, hit.Body);

				if (scattering > 0.0f && unit() < scattering)
				{
					direction = CosineDirection(hit.Normal, unit());
					result.BouncesScattered++;
				}
				else
				{
					direction = Reflect(direction, hit.Normal);
				}

				origin = surfacePoint;
			}

			if (debugRays)
			{
				debug.FinalEnergy = energy;
				debugRays->push_back(std::move(debug));
			}
		}

		// --- Summarise ----------------------------------------------------
		AcousticsDetail::Finish(result, bins, settings, binSeconds, speed);

		// --- Early reflections as discrete taps ---------------------------
		for (const AcousticsDetail::Candidate& candidate :
			AcousticsDetail::EarlyReflections(result, settings, binSeconds))
		{
			ReflectionPath path;
			path.Delay = (float)candidate.Bin * binSeconds;
			// Energy back to amplitude.
			path.Gain = std::sqrt(candidate.Energy);
			path.Bounces = bins.Bounces[candidate.Bin];
			path.PathLength = bins.PathLength[candidate.Bin] / candidate.Energy;

			glm::vec3 direction = bins.Direction[candidate.Bin];
			float length = glm::length(glm::vec2(direction));
			path.Direction = (length > 1e-6f)
				? glm::vec2(direction) / length : glm::vec2(0.0f);

			result.Reflections.push_back(path);
		}

		// Back into time order: a delay line reads taps in the order they
		// arrive, and it reads better in a debug panel.
		std::sort(result.Reflections.begin(), result.Reflections.end(),
			[](const ReflectionPath& a, const ReflectionPath& b) { return a.Delay < b.Delay; });

		return result;
	}

}
