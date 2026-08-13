#include "egsspch.h"
#include "Egss/Audio/AcousticsInternal.h"

#include <glm/gtc/constants.hpp>

namespace Egss {

	namespace AcousticsDetail {

		float AbsorptionFor(const AcousticsSettings& settings, unsigned int body)
		{
			if (!settings.PerBodyAbsorption || body >= settings.PerBodyAbsorption->size())
				return settings.Absorption;

			float value = (*settings.PerBodyAbsorption)[body];
			return (value < 0.0f) ? settings.Absorption : value;
		}

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

		float SpreadingGain(float distance, float minDistance)
		{
			return minDistance / glm::max(distance, minDistance);
		}

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

		size_t Allocate(AcousticsResultBase& result, Bins& bins,
			const AcousticsSettings& settings, float binSeconds, float speed)
		{
			const float maxTime = settings.MaxPathLength / speed;
			const size_t binCount = (size_t)glm::max(4.0f, std::ceil(maxTime / binSeconds)) + 1;

			result.Echogram.assign(binCount, 0.0f);
			for (int band = 0; band < AcousticBandCount; band++)
				result.BandEchogram[band].assign(binCount, 0.0f);

			// Direction of arrival, accumulated per bin weighted by energy, so a
			// bin holding several paths points where most of its sound came from.
			bins.Direction.assign(binCount, glm::vec3(0.0f));
			bins.PathLength.assign(binCount, 0.0f);
			bins.Bounces.assign(binCount, 0);

			return binCount;
		}

		void RecordArrival(AcousticsResultBase& result, Bins& bins, size_t bin,
			float energy, const float bandEnergy[AcousticBandCount],
			float listenerDistance, float minDistance,
			const glm::vec3& arrivalDirection, float pathLength, int bounce)
		{
			float spreading = SpreadingGain(listenerDistance, minDistance);
			float attenuation = spreading * spreading;
			float arriving = energy * attenuation;

			bins.Deepest = glm::max(bins.Deepest, bin);

			for (int band = 0; band < AcousticBandCount; band++)
				result.BandEchogram[band][bin] += bandEnergy[band] * attenuation;

			result.Echogram[bin] += arriving;
			// Arriving *at* the listener, so a pan points back towards the
			// surface it came off.
			bins.Direction[bin] += arrivalDirection * arriving;
			bins.PathLength[bin] += pathLength * arriving;
			bins.Bounces[bin] = glm::max(bins.Bounces[bin], bounce + 1);

			result.PathsFound++;
		}

		void Finish(AcousticsResultBase& result, const Bins& bins,
			const AcousticsSettings& settings, float binSeconds, float speed)
		{
			const size_t binCount = result.Echogram.size();

			result.MeanFreePath = bins.FreePathCount > 0
				? bins.FreePathTotal / (float)bins.FreePathCount : 0.0f;
			result.EffectiveRadius = bins.EffectiveRadius;
			result.MeanAbsorption = bins.AbsorptionWeight > 0.0
				? (float)(bins.AbsorptionWeighted / bins.AbsorptionWeight)
				: settings.Absorption;

			result.TracedSeconds = (float)bins.Deepest * binSeconds;
			result.ReverbTime = ReverbTimeFromEchogram(result.Echogram, binSeconds,
				bins.Deepest + 1);

			// The cheap estimate, always available. Tracing a long tail costs
			// bounces; this gets the same answer from the geometry the trace
			// already measured, and is what a game should use when the tail
			// outlasts what it can afford to trace.
			result.SabineTime = Acoustics::SabineReverbTime(result.MeanFreePath,
				result.MeanAbsorption, speed);

			for (int band = 0; band < AcousticBandCount; band++)
			{
				result.BandMeanAbsorption[band] = bins.BandAbsorptionWeight[band] > 0.0
					? (float)(bins.BandAbsorptionWeighted[band] / bins.BandAbsorptionWeight[band])
					: glm::clamp(settings.Absorption * settings.BandAbsorptionScale[band], 0.0001f, 0.9999f);

				float traced = ReverbTimeFromEchogram(result.BandEchogram[band], binSeconds,
					bins.Deepest + 1);
				result.BandReverbTime[band] = traced > 0.0f
					? traced
					: Acoustics::SabineReverbTime(result.MeanFreePath,
						result.BandMeanAbsorption[band], speed);
			}

			result.ReverbTimeMeasured = (result.ReverbTime > 0.0f);
			if (!result.ReverbTimeMeasured)
				result.ReverbTime = result.SabineTime;

			size_t earlyCutoff = (size_t)glm::min((float)binCount,
				std::ceil(settings.EarlyCutoffSeconds / binSeconds));

			float lateEnergy = 0.0f;
			for (size_t i = earlyCutoff; i < binCount; i++)
				lateEnergy += result.Echogram[i];

			result.LateEnergyRatio = bins.UnoccludedEnergy > 1e-12f
				? lateEnergy / bins.UnoccludedEnergy : 0.0f;

			// How rough the late tail is. Each bin is compared with the local
			// average around it rather than with the whole tail, so the decay
			// itself does not register as roughness -- what is left is how much
			// the arrivals clump.
			//
			// Only bins the trace reached count. Past Deepest they are empty
			// because tracing stopped, which says nothing about the room.
			{
				const int half = 4;   // +/- 20 ms, well inside the ear's fusion window
				const size_t last = glm::min(bins.Deepest, binCount - 1);

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
		}

		std::vector<Candidate> EarlyReflections(const AcousticsResultBase& result,
			const AcousticsSettings& settings, float binSeconds)
		{
			const size_t binCount = result.Echogram.size();

			size_t earlyCutoff = (size_t)glm::min((float)binCount,
				std::ceil(settings.EarlyCutoffSeconds / binSeconds));

			// Bin 0 is the direct sound, which the mixer already plays.
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

			return candidates;
		}

	}

	std::vector<ReverbTap> Acoustics::BuildImpulseTaps(const AcousticsResultBase& result,
		const ImpulseSettings& settings, float binSeconds)
	{
		std::vector<ReverbTap> taps;

		if (binSeconds <= 0.0f)
			return taps;

		// A fixed generator rather than std::rand: the same room must give the
		// same tail every time it is rebuilt, or a stationary listener hears the
		// reverb shimmer as it is recomputed.
		AcousticsDetail::Rng rng(settings.Seed);
		auto next = [&rng]() {
			rng.State ^= rng.State << 13;
			rng.State ^= rng.State >> 17;
			rng.State ^= rng.State << 5;
			return rng.State;
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

	float Acoustics::SabineReverbTime(float meanFreePath, float absorption, float speedOfSound)
	{
		if (absorption <= 0.0f || absorption >= 1.0f || meanFreePath <= 0.0f || speedOfSound <= 0.0f)
			return 0.0f;

		// 6 ln(10) = 13.8155: the number of e-foldings in 60 dB of energy.
		const float sixLn10 = 13.815511f;
		return sixLn10 * meanFreePath / (speedOfSound * -std::log(1.0f - absorption));
	}

}
