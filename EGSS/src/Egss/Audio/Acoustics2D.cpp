#include "egsspch.h"
#include "Egss/Audio/Acoustics2D.h"
#include "Egss/Debug/Instrumentor.h"

#include <glm/gtc/constants.hpp>

namespace Egss {

	namespace {

		float AbsorptionFor(const AcousticsSettings& settings, unsigned int body)
		{
			if (!settings.PerBodyAbsorption || body >= settings.PerBodyAbsorption->size())
				return settings.Absorption;

			float value = (*settings.PerBodyAbsorption)[body];
			return (value < 0.0f) ? settings.Absorption : value;
		}

		// Mirror about the surface normal. The tangential part of the
		// direction survives, the normal part flips.
		glm::vec2 Reflect(const glm::vec2& direction, const glm::vec2& normal)
		{
			return direction - 2.0f * glm::dot(direction, normal) * normal;
		}

		// How much of what leaves a point reaches the listener, from distance
		// alone. Amplitude falls as 1/distance -- the same law Audio3DParams
		// uses, so a reflection and the direct sound stay on the same scale.
		float SpreadingGain(float distance, float minDistance)
		{
			return minDistance / glm::max(distance, minDistance);
		}

		// Schroeder backward integration: at each time, how much energy is
		// still to come. Turning a noisy decay into a monotonic curve is what
		// makes a straight-line fit meaningful -- fitting the raw echogram
		// measures the loudest spikes rather than the decay.
		//
		// `tracedBins` is how far the echogram was actually filled. Everything
		// past it is zero because tracing stopped, not because the room went
		// quiet, and backward integration turns that cliff into a plunge that
		// looks exactly like a very dead room. Fitting into it is the classic
		// way to measure a confidently wrong RT60, so the window has to end
		// well before it.
		//
		// Returns RT60 in seconds, or 0 when the decay cannot be measured --
		// which the caller must handle rather than treat as "no reverb".
		float ReverbTimeFromEchogram(const std::vector<float>& echogram,
			float binSeconds, size_t tracedBins)
		{
			if (echogram.size() < 4)
				return 0.0f;

			std::vector<float> remaining(echogram.size());
			float running = 0.0f;
			for (size_t i = echogram.size(); i-- > 0; )
			{
				running += echogram[i];
				remaining[i] = running;
			}

			if (remaining[0] <= 0.0f)
				return 0.0f;

			// Fit between -5 dB and -25 dB and extrapolate. The first few dB
			// are contaminated by the direct sound and the earliest
			// reflections, and the last few are where the ray count runs out,
			// so the usual practice is to measure the clean middle. This is
			// T20: a 20 dB slope scaled by three.
			const float startDb = -5.0f;
			const float endDb = -25.0f;

			// Stay clear of the truncation cliff. A tenth of the traced span
			// is enough margin that the plunge has not begun to bend the curve.
			size_t usable = (tracedBins > 8) ? (size_t)(tracedBins * 0.9f) : 0;
			usable = glm::min(usable, remaining.size());
			if (usable < 8)
				return 0.0f;

			int startBin = -1, endBin = -1;
			for (size_t i = 0; i < usable; i++)
			{
				float db = 10.0f * std::log10(glm::max(remaining[i] / remaining[0], 1e-20f));
				if (startBin < 0 && db <= startDb) startBin = (int)i;
				if (db <= endDb) { endBin = (int)i; break; }
			}

			// Never reached -25 dB inside the traced span: the tail outlasts
			// the trace, so there is nothing honest to report.
			if (startBin < 0 || endBin < 0 || endBin <= startBin)
				return 0.0f;

			// Least squares over the window, so a wobble in one bin does not
			// tilt the whole answer.
			double n = 0.0, sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
			for (int i = startBin; i <= endBin; i++)
			{
				double x = i * (double)binSeconds;
				double y = 10.0 * std::log10(glm::max(remaining[i] / remaining[0], 1e-20f));

				n += 1.0; sumX += x; sumY += y; sumXY += x * y; sumXX += x * x;
			}

			double denominator = n * sumXX - sumX * sumX;
			if (std::abs(denominator) < 1e-12)
				return 0.0f;

			double slope = (n * sumXY - sumX * sumY) / denominator;   // dB per second
			if (slope >= -1e-6)
				return 0.0f;

			return (float)(-60.0 / slope);
		}

	}

	float Acoustics2D::SabineReverbTime(float meanFreePath, float absorption, float speedOfSound)
	{
		if (absorption <= 0.0f || absorption >= 1.0f || meanFreePath <= 0.0f || speedOfSound <= 0.0f)
			return 0.0f;

		// 6 ln(10) = 13.8155: the number of e-foldings in 60 dB of energy.
		const float sixLn10 = 13.815511f;
		return sixLn10 * meanFreePath / (speedOfSound * -std::log(1.0f - absorption));
	}

	AcousticsResult Acoustics2D::Trace(const PhysicsWorld2D& world,
		const glm::vec2& source, const glm::vec2& listener,
		const AcousticsSettings& settings, std::vector<TracedRay>* debugRays)
	{
		EGSS_PROFILE_SCOPE("Acoustics2D::Trace");

		AcousticsResult result;

		const float epsilon = settings.SurfaceEpsilon;
		const float speed = glm::max(settings.SpeedOfSound, 1.0f);
		const int rayCount = glm::max(settings.RayCount, 1);

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
		const float maxTime = settings.MaxPathLength / speed;
		const size_t binCount = (size_t)glm::max(4.0f, std::ceil(maxTime / binSeconds)) + 1;

		result.Echogram.assign(binCount, 0.0f);

		// Direction of arrival, accumulated per bin weighted by energy, so a
		// bin holding several paths points where most of its sound came from.
		std::vector<glm::vec2> binDirection(binCount, glm::vec2(0.0f));
		std::vector<float> binPathLength(binCount, 0.0f);
		std::vector<int> binBounces(binCount, 0);

		// Bin 0 holds what actually arrives directly, which is nothing when a
		// wall is in the way.
		float directGain = SpreadingGain(result.DirectDistance, settings.MinDistance);
		float unoccludedEnergy = directGain * directGain;
		result.Echogram[0] += unoccludedEnergy * (1.0f - result.Occlusion);

		// The *reference* for the late/direct ratio is the unoccluded figure.
		// Measuring against what got through would divide by zero the moment
		// the source went behind a wall, and report a dry room at exactly the
		// point where all you can hear is the room.

		const float energyPerRay = 1.0f / (float)rayCount;

		float freePathTotal = 0.0f;
		int freePathCount = 0;
		float effectiveRadius = result.DirectPathClear ? result.DirectDistance : 0.0f;

		// Energy-weighted mean absorption actually encountered, for the Sabine
		// estimate. A room of soft walls and one mirror does not behave like
		// the average of the two unless the average is weighted by how much
		// energy meets each.
		double absorptionWeighted = 0.0, absorptionWeight = 0.0;

		// How far the echogram is genuinely filled, as opposed to zero because
		// tracing stopped. The decay fit needs to know the difference.
		size_t deepestBin = 0;

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
			float energy = energyPerRay;
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
					freePathTotal += hit.Distance;
					freePathCount++;
				}

				if (debugRays)
					debug.Points.push_back(hit.Point);

				// Energy left after this surface takes its share.
				float absorption = AbsorptionFor(settings, hit.Body);
				absorptionWeighted += (double)absorption * (double)energy;
				absorptionWeight += (double)energy;
				energy *= (1.0f - absorption);

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
							// Spreading over the *last leg only*. The packet
							// this ray carries keeps its energy as it travels;
							// what varies is how much of what the surface
							// re-radiates the listener happens to catch, and
							// that depends on how far away the listener is
							// from that surface -- not on how far the sound
							// has already come.
							//
							// Using the whole path here applies the distance
							// falloff a second time, and since path length
							// grows with time it shows up as a decay: it made
							// every measured RT60 come out roughly half of
							// what the room's absorption says it should be.
							float spreading = SpreadingGain(listenerDistance, settings.MinDistance);
							float arriving = energy * spreading * spreading;

							deepestBin = glm::max(deepestBin, bin);

							result.Echogram[bin] += arriving;
							// Arriving *at* the listener, so pan points back
							// towards the surface it came off.
							binDirection[bin] += (-toL) * arriving;
							binPathLength[bin] += pathLength * arriving;
							binBounces[bin] = glm::max(binBounces[bin], bounce + 1);

							result.PathsFound++;
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
					effectiveRadius = glm::max(effectiveRadius, glm::length(hit.Point - source));

				if (energy < settings.MinEnergy)
					break;

				direction = Reflect(direction, hit.Normal);
				origin = surfacePoint;
			}

			if (debugRays)
			{
				debug.FinalEnergy = energy;
				debugRays->push_back(std::move(debug));
			}
		}

		// --- Summarise ----------------------------------------------------
		result.MeanFreePath = freePathCount > 0 ? freePathTotal / (float)freePathCount : 0.0f;
		result.EffectiveRadius = effectiveRadius;
		result.MeanAbsorption = absorptionWeight > 0.0
			? (float)(absorptionWeighted / absorptionWeight) : settings.Absorption;

		result.TracedSeconds = (float)deepestBin * binSeconds;
		result.ReverbTime = ReverbTimeFromEchogram(result.Echogram, binSeconds, deepestBin + 1);

		// The cheap estimate, always available. Tracing a long tail costs
		// bounces; this gets the same answer from the geometry the trace
		// already measured, and is what a game should use when the tail
		// outlasts what it can afford to trace.
		result.SabineTime = SabineReverbTime(result.MeanFreePath, result.MeanAbsorption, speed);

		result.ReverbTimeMeasured = (result.ReverbTime > 0.0f);
		if (!result.ReverbTimeMeasured)
			result.ReverbTime = result.SabineTime;

		size_t earlyCutoff = (size_t)glm::min((float)binCount,
			std::ceil(settings.EarlyCutoffSeconds / binSeconds));

		float lateEnergy = 0.0f;
		for (size_t i = earlyCutoff; i < binCount; i++)
			lateEnergy += result.Echogram[i];

		result.LateEnergyRatio = unoccludedEnergy > 1e-12f
			? lateEnergy / unoccludedEnergy : 0.0f;

		// --- Early reflections as discrete taps ---------------------------
		// Bin 0 is the direct sound, which the mixer already plays.
		struct Candidate { size_t Bin; float Energy; };
		std::vector<Candidate> candidates;
		candidates.reserve(earlyCutoff);

		for (size_t i = 1; i < earlyCutoff; i++)
			if (result.Echogram[i] > 0.0f)
				candidates.push_back({ i, result.Echogram[i] });

		// Loudest first, so a small tap budget spends itself on the
		// reflections that can actually be heard.
		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& a, const Candidate& b) { return a.Energy > b.Energy; });

		if ((int)candidates.size() > settings.MaxReflections)
			candidates.resize(settings.MaxReflections);

		for (const Candidate& candidate : candidates)
		{
			ReflectionPath path;
			path.Delay = (float)candidate.Bin * binSeconds;
			// Energy back to amplitude.
			path.Gain = std::sqrt(candidate.Energy);
			path.Bounces = binBounces[candidate.Bin];
			path.PathLength = binPathLength[candidate.Bin] / candidate.Energy;

			glm::vec2 direction = binDirection[candidate.Bin];
			float length = glm::length(direction);
			path.Direction = (length > 1e-6f) ? direction / length : glm::vec2(0.0f);

			result.Reflections.push_back(path);
		}

		// Back into time order: a delay line reads taps in the order they
		// arrive, and it reads better in a debug panel.
		std::sort(result.Reflections.begin(), result.Reflections.end(),
			[](const ReflectionPath& a, const ReflectionPath& b) { return a.Delay < b.Delay; });

		return result;
	}

}
