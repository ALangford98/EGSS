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

		// The same surface, per band. Clamped below 1 because a surface that
		// absorbs everything ends the ray, and clamped above 0 because a
		// perfect mirror never lets the trace terminate.
		void BandAbsorptionFor(const AcousticsSettings& settings, unsigned int body,
			float out[AcousticBandCount])
		{
			float base = AbsorptionFor(settings, body);

			for (int band = 0; band < AcousticBandCount; band++)
				out[band] = glm::clamp(base * settings.BandAbsorptionScale[band], 0.0001f, 0.9999f);
		}

		float ScatteringFor(const AcousticsSettings& settings, unsigned int body)
		{
			float base = glm::clamp(settings.Scattering, 0.0f, 1.0f);

			if (!settings.PerBodyScattering || body >= settings.PerBodyScattering->size())
				return base;

			float value = (*settings.PerBodyScattering)[body];
			return (value < 0.0f) ? base : glm::clamp(value, 0.0f, 1.0f);
		}

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

	std::vector<ReverbTap> Acoustics2D::BuildImpulseTaps(const AcousticsResult& result,
		const ImpulseSettings& settings, float binSeconds)
	{
		std::vector<ReverbTap> taps;

		if (binSeconds <= 0.0f)
			return taps;

		// A fixed generator rather than std::rand: the same room must give the
		// same tail every time it is rebuilt, or a stationary listener hears
		// the reverb shimmer as it is recomputed.
		unsigned int rng = settings.Seed ? settings.Seed : 1u;
		auto next = [&rng]() {
			// xorshift32 -- small, fast, and good enough for noise.
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			return rng;
		};
		auto unit = [&next]() { return (float)(next() >> 8) / (float)(1 << 24); };

		// Where each band's tail actually stops. The treble runs out long
		// before the bass, which is the entire point -- and it means the
		// treble needs proportionally fewer impulses to cover it.
		size_t firstBin = (size_t)glm::max(0.0f, std::floor(settings.StartSeconds / binSeconds));

		bool haveBands = false;
		for (int band = 0; band < AcousticBandCount; band++)
			haveBands |= !result.BandEchogram[band].empty();

		size_t lastBin[AcousticBandCount] = {};
		size_t totalSpan = 0;

		for (int band = 0; band < AcousticBandCount; band++)
		{
			// Falls back to the broadband echogram, so a hand-built result
			// with only Echogram filled still works.
			const std::vector<float>& echogram = haveBands
				? result.BandEchogram[band] : result.Echogram;

			// Where the band stops being worth hearing, not where its energy
			// happens to underflow. Bass and treble reach zero at nearly the
			// same bin -- the difference is entirely in *when they got quiet*
			// -- so ending at "non-zero" would give every band the same span
			// and the same share of the impulse budget.
			float peak = 0.0f;
			for (float value : echogram)
				peak = glm::max(peak, value);

			float floor = peak * 1e-6f;   // 60 dB down
			for (size_t i = echogram.size(); i-- > 0; )
			{
				if (echogram[i] > floor) { lastBin[band] = i; break; }
			}

			// Nothing past the mixer's buffer, or the taps are built and then
			// silently discarded -- which quietly halves the density of the
			// part that does fit.
			size_t maxBin = (size_t)(AudioEngine::GetMaxImpulseLength() / binSeconds);
			lastBin[band] = glm::min(lastBin[band], maxBin > 0 ? maxBin - 1 : 0);

			if (lastBin[band] > firstBin)
				totalSpan += lastBin[band] + 1 - firstBin;
		}

		if (totalSpan == 0)
			return taps;

		int budget = glm::min(settings.MaxTaps, (int)AudioEngine::GetMaxReverbTaps());

		for (int band = 0; band < AcousticBandCount; band++)
		{
			const std::vector<float>& echogram = haveBands
				? result.BandEchogram[band] : result.Echogram;

			if (lastBin[band] <= firstBin)
				continue;

			float start = (float)firstBin * binSeconds;
			float end = (float)(lastBin[band] + 1) * binSeconds;
			float span = end - start;

			int count = (int)(span * (float)settings.Density);

			// The budget is shared out by how much tail each band has to
			// cover, so a short treble tail does not spend impulses a long
			// bass tail needs.
			size_t bandSpan = lastBin[band] + 1 - firstBin;
			int share = glm::max(1, (int)((double)budget * (double)bandSpan / (double)totalSpan));
			count = glm::clamp(count, 1, share);

			float interval = span / (float)count;

			for (int i = 0; i < count; i++)
			{
				// One per interval, jittered inside it.
				float time = start + ((float)i + unit()) * interval;

				size_t bin = (size_t)(time / binSeconds);
				if (bin >= echogram.size())
					continue;

				float energy = echogram[bin];
				if (energy <= 0.0f)
					continue;

				// Each impulse stands for its own interval, and carries the
				// energy the echogram says that interval holds: the bin's
				// energy scaled by how much of a bin the interval covers.
				//
				// Counting impulses per bin instead -- energy / n -- looks
				// equivalent and is not. At low density an interval is about
				// one bin wide, jitter pushes taps into neighbouring bins, and
				// bins left empty lose their energy entirely. That is exactly
				// what made a sparse tail measure 7.5% quiet against a dense
				// one.
				float amplitude = std::sqrt(energy * interval / binSeconds) * settings.Gain;

				ReverbTap tap;
				tap.Delay = time;
				tap.Gain = (next() & 1u) ? amplitude : -amplitude;
				// Spread across the field. Correlated pans would collapse the
				// tail towards the middle, which is the one thing a diffuse
				// tail should never sound like.
				tap.Pan = unit() * 2.0f - 1.0f;
				tap.Band = haveBands ? (unsigned int)band : ReverbTap::AllBands;

				taps.push_back(tap);
			}

			// Without per-band data there is one tail, not three, and running
			// the loop again would triple it.
			if (!haveBands)
				break;
		}

		// Time order across the bands, which is how a delay line reads them
		// and how a debug panel wants to show them.
		std::sort(taps.begin(), taps.end(),
			[](const ReverbTap& a, const ReverbTap& b) { return a.Delay < b.Delay; });

		return taps;
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

		// Scattering is the one stochastic part of the trace, so it gets its
		// own fixed stream rather than std::rand: the same room must trace the
		// same way twice, or a listener who has not moved hears the tail
		// change under them every time it is rebuilt.
		unsigned int rng = settings.ScatterSeed ? settings.ScatterSeed : 1u;
		auto unit = [&rng]() {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;   // xorshift32
			return (float)(rng >> 8) / (float)(1 << 24);
		};

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
		for (int band = 0; band < AcousticBandCount; band++)
			result.BandEchogram[band].assign(binCount, 0.0f);

		// Direction of arrival, accumulated per bin weighted by energy, so a
		// bin holding several paths points where most of its sound came from.
		std::vector<glm::vec2> binDirection(binCount, glm::vec2(0.0f));
		std::vector<float> binPathLength(binCount, 0.0f);
		std::vector<int> binBounces(binCount, 0);

		// Bin 0 holds what actually arrives directly, which is nothing when a
		// wall is in the way.
		float directGain = SpreadingGain(result.DirectDistance, settings.MinDistance);
		float unoccludedEnergy = directGain * directGain;
		float directArriving = unoccludedEnergy * (1.0f - result.Occlusion);

		result.Echogram[0] += directArriving;
		// The direct sound has not bounced off anything, so it is unfiltered:
		// every band gets its full share.
		for (int band = 0; band < AcousticBandCount; band++)
			result.BandEchogram[band][0] += directArriving;

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
		double bandAbsorptionWeighted[AcousticBandCount] = {};
		double bandAbsorptionWeight[AcousticBandCount] = {};

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

				float bandAbsorption[AcousticBandCount];
				BandAbsorptionFor(settings, hit.Body, bandAbsorption);

				for (int band = 0; band < AcousticBandCount; band++)
				{
					bandAbsorptionWeighted[band] += (double)bandAbsorption[band] * (double)bandEnergy[band];
					bandAbsorptionWeight[band] += (double)bandEnergy[band];
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
							float attenuation = spreading * spreading;
							float arriving = energy * attenuation;

							deepestBin = glm::max(deepestBin, bin);

							for (int band = 0; band < AcousticBandCount; band++)
								result.BandEchogram[band][bin] += bandEnergy[band] * attenuation;

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

		for (int band = 0; band < AcousticBandCount; band++)
		{
			result.BandMeanAbsorption[band] = bandAbsorptionWeight[band] > 0.0
				? (float)(bandAbsorptionWeighted[band] / bandAbsorptionWeight[band])
				: glm::clamp(settings.Absorption * settings.BandAbsorptionScale[band], 0.0001f, 0.9999f);

			float traced = ReverbTimeFromEchogram(result.BandEchogram[band], binSeconds, deepestBin + 1);
			result.BandReverbTime[band] = traced > 0.0f
				? traced
				: SabineReverbTime(result.MeanFreePath, result.BandMeanAbsorption[band], speed);
		}

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

		// How rough the late tail is. Each bin is compared with the local
		// average around it rather than with the whole tail, so the decay
		// itself does not register as roughness -- what is left is how much
		// the arrivals clump.
		//
		// Only bins the trace reached count. Past deepestBin they are empty
		// because tracing stopped, which says nothing about the room.
		{
			const int half = 4;   // +/- 20 ms, well inside the ear's fusion window
			const size_t last = glm::min(deepestBin, binCount - 1);

			double total = 0.0;
			int counted = 0;

			for (size_t i = earlyCutoff; i <= last; i++)
			{
				double sum = 0.0;
				int window = 0;

				for (int k = -half; k <= half; k++)
				{
					// Unsigned, so a negative offset wraps to something huge --
					// which the upper bound catches.
					size_t j = i + (size_t)k;
					if (j < earlyCutoff || j > last)
						continue;

					sum += result.Echogram[j];
					window++;
				}

				if (window == 0 || sum <= 0.0)
					continue;

				double local = sum / window;
				// Floored, or a genuinely empty bin contributes -infinity.
				double db = 10.0 * std::log10(glm::max(result.Echogram[i] / (float)local, 1e-4f));

				total += db * db;
				counted++;
			}

			result.TailRoughness = counted > 0 ? (float)std::sqrt(total / counted) : 0.0f;
		}

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
