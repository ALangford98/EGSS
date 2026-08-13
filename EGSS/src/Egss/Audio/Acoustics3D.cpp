#include "egsspch.h"
#include "Egss/Audio/Acoustics3D.h"
#include "Egss/Audio/AcousticsInternal.h"
#include "Egss/Physics/Raycast3D.h"
#include "Egss/Debug/Instrumentor.h"

#include <glm/gtc/constants.hpp>

namespace Egss {

	namespace {

		using namespace AcousticsDetail;

		// Mirror about the surface normal -- the same expression as in 2D, one
		// component wider.
		glm::vec3 Reflect(const glm::vec3& direction, const glm::vec3& normal)
		{
			return direction - 2.0f * glm::dot(direction, normal) * normal;
		}

		// An orthonormal frame with `normal` as its third axis.
		//
		// The helper axis is chosen away from the normal rather than fixed: with
		// a fixed (0,1,0), a floor or a ceiling -- the two surfaces 3D exists to
		// add -- would take a cross product of two parallel vectors and get a
		// zero-length basis, which is a diffuse bounce that goes nowhere.
		void Basis(const glm::vec3& normal, glm::vec3& outU, glm::vec3& outV)
		{
			glm::vec3 helper = (std::fabs(normal.x) < 0.9f)
				? glm::vec3(1.0f, 0.0f, 0.0f)
				: glm::vec3(0.0f, 1.0f, 0.0f);

			outU = glm::normalize(glm::cross(normal, helper));
			outV = glm::cross(normal, outU);
		}

		// A direction drawn from Lambert's cosine law about the normal.
		//
		// In 3D the law is pdf proportional to cos(theta) over the hemisphere,
		// and the standard inversion is the concentric-disc one: draw a point on
		// the unit disc with radius sqrt(u1), then lift it onto the hemisphere.
		// The height sqrt(1 - u1) is what makes the distribution cosine rather
		// than uniform, and it is the whole difference -- a uniform hemisphere
		// sends too much energy along the wall, lengthening the mean free path
		// and with it the decay. The 2D tracer has the same trap in its own
		// form.
		//
		// Two random numbers here against one in 2D, which is why the scatter
		// stream is drawn from in a fixed order rather than on demand.
		glm::vec3 CosineDirection(const glm::vec3& normal, float u1, float u2)
		{
			float radius = std::sqrt(glm::clamp(u1, 0.0f, 1.0f));
			float phi = glm::two_pi<float>() * u2;
			float height = std::sqrt(glm::max(0.0f, 1.0f - u1));

			glm::vec3 u, v;
			Basis(normal, u, v);

			return glm::normalize(u * (radius * std::cos(phi))
				+ v * (radius * std::sin(phi))
				+ normal * height);
		}

		// `count` directions spread as evenly over the sphere as a closed form
		// can manage -- the golden-angle spiral.
		//
		// A latitude/longitude grid is the obvious alternative and is wrong: it
		// crowds rays at the poles, so a room's floor and ceiling get sampled
		// several times as densely as its walls, which is exactly the bias 3D
		// acoustics exists to remove.
		//
		// Offset by half a step in both parameters, so no ray has an exactly
		// zero component. In a rectangular room a ray confined to a coordinate
		// plane stays confined to it forever under specular reflection, and
		// behaves unlike every other ray -- the same reason the 2D tracer
		// offsets its first angle.
		glm::vec3 SphereDirection(int index, int count)
		{
			const float goldenAngle = glm::pi<float>() * (3.0f - std::sqrt(5.0f));

			float y = 1.0f - 2.0f * ((float)index + 0.5f) / (float)count;
			float radius = std::sqrt(glm::max(0.0f, 1.0f - y * y));
			float phi = ((float)index + 0.5f) * goldenAngle;

			return { radius * std::cos(phi), y, radius * std::sin(phi) };
		}

	}

	AcousticsResult3D Acoustics3D::Trace(const Scene& scene,
		const glm::vec3& source, const glm::vec3& listener,
		const AcousticsSettings& settings, std::vector<TracedRay3D>* debugRays)
	{
		EGSS_PROFILE_SCOPE("Acoustics3D::Trace");

		AcousticsResult3D result;
		AcousticsDetail::Bins bins;

		const float epsilon = settings.SurfaceEpsilon;
		const float speed = glm::max(settings.SpeedOfSound, 1.0f);
		const int rayCount = glm::max(settings.RayCount, 1);

		AcousticsDetail::Rng rng(settings.ScatterSeed);

		// --- The direct sound ---------------------------------------------
		glm::vec3 toListener = listener - source;
		result.DirectDistance = glm::length(toListener);

		if (result.DirectDistance > 1e-5f)
		{
			// Graded, and already written: `Raycast3D::Occlusion` spreads its
			// probes over a disc facing the line, which is the 3D form of the
			// five-across-the-listener spread the 2D tracer does by hand. Using
			// it rather than repeating it also means a source half behind a
			// pillar reads the same here as it does to a 3D emitter.
			result.Occlusion = Raycast3D::Occlusion(scene, source, listener);
			result.DirectPathClear = (result.Occlusion <= 0.0f);
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
			glm::vec3 direction = SphereDirection(i, rayCount);
			glm::vec3 origin = source;

			// One packet per band. They start equal and diverge as they bounce,
			// which is the whole point: after a dozen surfaces the treble
			// packet is a fraction of the bass one.
			float bandEnergy[AcousticBandCount];
			for (int band = 0; band < AcousticBandCount; band++)
				bandEnergy[band] = energyPerRay;

			float energy = energyPerRay;   // broadband, for the summary figures
			float travelled = 0.0f;

			TracedRay3D debug;
			if (debugRays)
				debug.Points.push_back(source);

			for (int bounce = 0; bounce < settings.MaxBounces; bounce++)
			{
				float remaining = settings.MaxPathLength - travelled;
				if (remaining <= 0.0f)
					break;

				RaycastHit3D hit = Raycast3D::Against(scene, origin, direction, remaining);
				if (!hit.Hit)
				{
					// Running out of path budget is not the same as leaving the
					// room, and counting it as an escape makes a sealed room
					// look leaky. One more cast, unbounded, tells them apart.
					bool escaped = !Raycast3D::Against(scene, origin, direction,
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

				// Energy left after this surface takes its share. Indexed by
				// entity here rather than by body handle -- same vectors, the
				// scene's namespace.
				float absorption = AbsorptionFor(settings, hit.Entity);
				bins.AbsorptionWeighted += (double)absorption * (double)energy;
				bins.AbsorptionWeight += (double)energy;
				energy *= (1.0f - absorption);

				float bandAbsorption[AcousticBandCount];
				BandAbsorptionFor(settings, hit.Entity, bandAbsorption);

				for (int band = 0; band < AcousticBandCount; band++)
				{
					bins.BandAbsorptionWeighted[band] += (double)bandAbsorption[band] * (double)bandEnergy[band];
					bins.BandAbsorptionWeight[band] += (double)bandEnergy[band];
					bandEnergy[band] *= (1.0f - bandAbsorption[band]);
				}

				// Off the surface before doing anything else, or the next cast
				// starts inside the wall it just hit.
				glm::vec3 surfacePoint = hit.Point + hit.Normal * epsilon;

				// --- Can the listener hear this bounce? ---
				glm::vec3 hitToListener = listener - surfacePoint;
				float listenerDistance = glm::length(hitToListener);

				if (listenerDistance > 1e-5f)
				{
					glm::vec3 toL = hitToListener / listenerDistance;

					if (!Raycast3D::Against(scene, surfacePoint, toL,
						listenerDistance - epsilon).Hit)
					{
						float pathLength = travelled + listenerDistance;
						float delay = pathLength / speed;

						size_t bin = (size_t)(delay / binSeconds);
						if (bin < binCount)
						{
							AcousticsDetail::RecordArrival(result, bins, bin,
								energy, bandEnergy, listenerDistance,
								settings.MinDistance, -toL, pathLength, bounce);
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

				// Scatter or mirror, decided per bounce rather than by splitting
				// the ray in two. One ray that goes diffuse a fraction s of the
				// time carries the same energy in each direction as two rays
				// weighted s and 1-s, and it keeps the cost of a trace
				// independent of how rough the room is.
				float scattering = ScatteringFor(settings, hit.Entity);

				// Both draws happen whichever way the branch goes, so the
				// stream advances by the same amount either way: a trace's
				// randomness then does not depend on how many surfaces happened
				// to be rough, which is what makes two traces of the same room
				// comparable.
				float pick = rng.Unit();
				float u1 = rng.Unit();
				float u2 = rng.Unit();

				if (scattering > 0.0f && pick < scattering)
				{
					direction = CosineDirection(hit.Normal, u1, u2);
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
			ReflectionPath3D path;
			path.Delay = (float)candidate.Bin * binSeconds;
			// Energy back to amplitude.
			path.Gain = std::sqrt(candidate.Energy);
			path.Bounces = bins.Bounces[candidate.Bin];
			path.PathLength = bins.PathLength[candidate.Bin] / candidate.Energy;

			glm::vec3 direction = bins.Direction[candidate.Bin];
			float length = glm::length(direction);
			path.Direction = (length > 1e-6f) ? direction / length : glm::vec3(0.0f);

			result.Reflections.push_back(path);
		}

		// Back into time order: a delay line reads taps in the order they
		// arrive, and it reads better in a debug panel.
		std::sort(result.Reflections.begin(), result.Reflections.end(),
			[](const ReflectionPath3D& a, const ReflectionPath3D& b) { return a.Delay < b.Delay; });

		return result;
	}

}
