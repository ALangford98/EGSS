#pragma once

// Single-microphone source attribution: the signal half.
//
// Deliberately free of engine headers -- standard library only. The room comes
// in as a list of (delay, gain) taps and the answer goes out as two numbers, so
// everything here can be checked against arithmetic without a window, a device
// or a GL context. `PumpDiagnostics.h` is the half that knows about EGSS.
//
// ---------------------------------------------------------------------------
// The problem
//
// One microphone, two pumps, and the question "which one is faulty". A single
// channel carrying two sources is underdetermined -- there is no inter-aural
// delay, no level difference, nothing to beamform with. So the entire question
// is *what breaks the symmetry between the two machines*, and the honest answer
// is: they stand in different places in the same room.
//
// A room is a filter. Pump A reaches the mic through H_A, pump B through H_B,
// and those two filters differ because the path lengths and the reflection
// patterns differ. Anything a pump emits arrives wearing its own channel's
// colouration. That is the only handle, and this file is built around it.
//
// ---------------------------------------------------------------------------
// Why the usual method does not apply here
//
// The textbook answer for rotating machinery is to separate the two harmonic
// combs: two nominally identical machines never run at exactly the same speed,
// so their shaft orders sit at slightly different frequencies and a fault
// attaches to whichever comb it modulates. That method is `AttributeByEnvelope`
// below, and on fixed-speed pumps sharing a grid it can fail outright -- the
// combs land on top of each other and no amount of FFT resolution separates
// them.
//
// It is worth being precise about what "same grid" locks, because it is not the
// shaft. Shaft rate is line_freq / pole_pairs * (1 - slip), and slip tracks
// load. Two 4-pole motors on 50 Hz at 1% and 2% slip run at 24.75 Hz and
// 24.50 Hz -- a 0.25 Hz gap, which a 4 second window resolves comfortably
// (resolution is 1/T, so T > 1/0.25). Whether that residual asymmetry exists on
// a given site is a measurement, not an assumption, which is why the demo makes
// slip a slider and reports which lever actually carried the result.
//
// ---------------------------------------------------------------------------
// The method that does work at identical speed
//
// `AttributeByChannel`. The derivation matters more than the code:
//
// A baseline recording of pump A running alone gives its PSD
//
//     P_A(f) = |H_A(f)|^2 * S(f)
//
// where S is the healthy source spectrum. Both pumps are the same model, so S
// is common to both and P_B(f) = |H_B(f)|^2 * S(f).
//
// The pumps are independent machines, so their contributions to the mixture add
// in power rather than amplitude. Writing the faulty source spectrum as
// S + alpha_i * G -- the same fault shape G on either machine, scaled by how bad
// it is -- the mixture PSD is
//
//     P_mix(f) = |H_A|^2 (S + a_A G) + |H_B|^2 (S + a_B G)
//
// so subtracting both baselines leaves exactly the fault, twice coloured:
//
//     R(f) = P_mix - P_A - P_B = G(f) * ( a_A |H_A|^2 + a_B |H_B|^2 )
//
// Neither |H| is observable on its own. But dividing by P_A makes everything
// observable at once:
//
//     R(f) / P_A(f) = (G/S) * ( a_A + a_B * rho(f) ),   rho(f) = P_B(f)/P_A(f)
//
// rho is a ratio of two things that were measured. Over a band where G/S is
// roughly flat -- call that constant g -- this is a straight line regression of
// y(f) = R/P_A against the two columns [1, rho(f)], and the fitted coefficients
// are g*a_A and g*a_B. The unknown g cancels in the ratio, which is the
// attribution. Non-negative, because a fault cannot subtract energy.
//
// **The failure mode is visible in the algebra rather than hidden in the code.**
// If rho(f) is constant across the band, the two columns are parallel, the
// design matrix is rank deficient, and no attribution exists at any SNR. rho is
// constant exactly when the two channels colour the spectrum the same way --
// two pumps equidistant from the mic in a symmetric room. So the condition
// number of that 2 x 2 system *is* the feasibility number for a given layout,
// and `ChannelFit::Conditioning` reports it. A site survey question ("where do
// we put the mic") becomes a number you can compute before installing anything.
//
// Two standing assumptions, both of which the demo can violate on purpose:
//   - The pumps are the same model, so S is shared. Two different models leave
//     a residual after baseline subtraction even when both are healthy.
//   - Nothing moved since the baselines were taken. Moving the mic changes both
//     H and invalidates rho. The demo warns when the geometry has drifted,
//     because this is the assumption a real installation breaks first.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>

namespace PumpDx {

	constexpr float kSpeedOfSound = 343.0f;
	constexpr float kPi = 3.14159265358979323846f;

	// The analysis runs well below the mixer's rate. Bearing resonances live
	// under about 6 kHz, so 12 kHz is a comfortable Nyquist for everything this
	// looks at, and every FFT is then four times cheaper -- or, spent the other
	// way, four times finer for the same cost. The audible path is separate and
	// still runs at the device rate; see the note in PumpDiagnostics.h about why
	// what you hear and what is measured are deliberately not the same signal.
	constexpr int kAnalysisRate = 12000;

	// ======================================================================
	// Deterministic noise
	//
	// A demo that cannot reproduce its own measurement is not a measurement.
	// std::mt19937 would do, but this is three lines and seeds explicitly.
	// ======================================================================
	class Rng
	{
	public:
		explicit Rng(uint32_t seed = 0x9E3779B9u) : m_State(seed ? seed : 1u) {}

		uint32_t Next()
		{
			// xorshift32. Period 2^32-1, which is ample for noise.
			m_State ^= m_State << 13;
			m_State ^= m_State >> 17;
			m_State ^= m_State << 5;
			return m_State;
		}

		// Uniform in [-1, 1).
		float Bipolar() { return (float)(Next() >> 8) * (1.0f / 8388608.0f) - 1.0f; }
		float Unit() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); }

		// Box-Muller would need two uniforms and a log; summing four is close
		// enough to Gaussian for broadband noise and much cheaper. The sum of
		// n uniforms has variance n/3, hence the scale.
		float Gaussian()
		{
			float sum = Bipolar() + Bipolar() + Bipolar() + Bipolar();
			return sum * 0.866025f;   // sqrt(3/4), so unit variance
		}
	private:
		uint32_t m_State;
	};

	// ======================================================================
	// FFT
	//
	// Iterative radix-2, in place. Sizes must be powers of two; callers here
	// always pad up to one.
	// ======================================================================
	inline void Fft(std::vector<std::complex<float>>& data, bool inverse)
	{
		const size_t n = data.size();
		if (n <= 1)
			return;

		// Bit-reversal permutation.
		for (size_t i = 1, j = 0; i < n; i++)
		{
			size_t bit = n >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;

			if (i < j)
				std::swap(data[i], data[j]);
		}

		for (size_t len = 2; len <= n; len <<= 1)
		{
			// Doubles rather than floats for the twiddle: at 65536 points the
			// accumulated angle error in single precision is enough to smear a
			// narrow peak across neighbouring bins, which matters here because
			// telling 24.50 Hz from 24.75 Hz is the entire point of one of the
			// methods below.
			double angle = 2.0 * 3.14159265358979323846 / (double)len * (inverse ? 1.0 : -1.0);
			std::complex<float> step((float)std::cos(angle), (float)std::sin(angle));

			for (size_t i = 0; i < n; i += len)
			{
				std::complex<float> w(1.0f, 0.0f);
				for (size_t k = 0; k < len / 2; k++)
				{
					std::complex<float> u = data[i + k];
					std::complex<float> v = data[i + k + len / 2] * w;
					data[i + k] = u + v;
					data[i + k + len / 2] = u - v;
					w *= step;
				}
			}
		}

		if (inverse)
		{
			for (std::complex<float>& c : data)
				c /= (float)n;
		}
	}

	inline size_t NextPowerOfTwo(size_t n)
	{
		size_t p = 1;
		while (p < n)
			p <<= 1;
		return p;
	}

	// ======================================================================
	// Spectra
	// ======================================================================

	// Periodic Hann, which is the right one for overlap-add and for Welch --
	// the symmetric variant duplicates the endpoint and biases the average
	// slightly. Coherent gain 0.5, so the PSD scaling below divides by the sum
	// of squares rather than by n.
	inline std::vector<float> HannWindow(int n)
	{
		std::vector<float> w((size_t)n);
		for (int i = 0; i < n; i++)
			w[(size_t)i] = 0.5f * (1.0f - std::cos(2.0f * kPi * (float)i / (float)n));
		return w;
	}

	// Welch PSD: average the periodograms of overlapping windowed segments.
	//
	// Averaging is what makes the baseline subtraction in AttributeByChannel
	// usable at all. A single periodogram of noise has 100% standard error
	// regardless of length -- the estimate never converges, it just gets more
	// finely spaced. m segments cut that by sqrt(m), and R = P_mix - P_A - P_B
	// is a difference of three noisy estimates, so its error is the part that
	// decides whether a small fault is visible.
	//
	// Returns fftSize/2 + 1 bins, scaled so that summing them gives the
	// signal's mean square. That convention is worth insisting on: it makes the
	// PSD checkable against arithmetic the transform knows nothing about -- a
	// sine of amplitude A must sum to A^2/2 -- and it was how the missing 1/N
	// below was found in the first place. Without it the sum came out N times
	// too large, and the ratio being *exactly* the FFT size is what said the
	// fault was a constant rather than the transform.
	inline std::vector<float> WelchPsd(const float* x, size_t count, int fftSize, int hop,
		int* outSegments = nullptr)
	{
		if (outSegments)
			*outSegments = 0;

		const size_t bins = (size_t)fftSize / 2 + 1;
		std::vector<float> psd(bins, 0.0f);

		if (count < (size_t)fftSize || hop <= 0)
			return psd;

		std::vector<float> window = HannWindow(fftSize);

		double windowPower = 0.0;
		for (float w : window)
			windowPower += (double)w * w;

		std::vector<std::complex<float>> buffer((size_t)fftSize);
		int segments = 0;

		for (size_t start = 0; start + (size_t)fftSize <= count; start += (size_t)hop)
		{
			for (int i = 0; i < fftSize; i++)
				buffer[(size_t)i] = std::complex<float>(x[start + (size_t)i] * window[(size_t)i], 0.0f);

			Fft(buffer, false);

			for (size_t b = 0; b < bins; b++)
			{
				float re = buffer[b].real();
				float im = buffer[b].imag();
				psd[b] += re * re + im * im;
			}

			segments++;
		}

		if (segments == 0)
			return psd;

		// Normalise by the window's power so the result does not depend on the
		// window choice, and fold the negative frequencies onto the positive
		// ones (except DC and Nyquist, which have no partner).
		if (outSegments)
			*outSegments = segments;

		float scale = 1.0f / ((float)segments * (float)windowPower * (float)fftSize);
		for (size_t b = 0; b < bins; b++)
		{
			psd[b] *= scale;
			if (b != 0 && b != bins - 1)
				psd[b] *= 2.0f;
		}

		return psd;
	}

	// Moving average across bins.
	//
	// Trades frequency resolution for variance, which is the right trade for
	// the channel method and the wrong one for the envelope method -- so it is
	// applied to one and not the other. What separates two room paths is the
	// *broad* shape of their colouration, tens of bins wide; what separates two
	// shaft speeds is a line a fraction of a bin wide. Smoothing helps the
	// first and destroys the second.
	//
	// Must be applied identically to the mixture and to both baselines, or the
	// subtraction that produces the residual is comparing differently-blurred
	// versions of the same thing and leaves a systematic residual with no fault
	// in it at all.
	inline std::vector<float> SmoothSpectrum(const std::vector<float>& psd, int halfWidth)
	{
		if (halfWidth <= 0 || psd.empty())
			return psd;

		std::vector<float> out(psd.size());

		// Running sum rather than a window per bin: this is called on every
		// analysis and the naive form is O(bins * width).
		double sum = 0.0;
		int count = 0;

		for (int i = 0; i <= halfWidth && i < (int)psd.size(); i++)
		{
			sum += psd[(size_t)i];
			count++;
		}

		for (int b = 0; b < (int)psd.size(); b++)
		{
			out[(size_t)b] = (float)(sum / (double)count);

			int add = b + halfWidth + 1;
			int drop = b - halfWidth;

			if (add < (int)psd.size())
			{
				sum += psd[(size_t)add];
				count++;
			}
			if (drop >= 0)
			{
				sum -= psd[(size_t)drop];
				count--;
			}
		}

		return out;
	}

	inline float BinToHz(size_t bin, int fftSize, float sampleRate)
	{
		return (float)bin * sampleRate / (float)fftSize;
	}

	inline size_t HzToBin(float hz, int fftSize, float sampleRate)
	{
		long bin = (long)std::lround((double)hz * (double)fftSize / (double)sampleRate);
		return (size_t)std::max(0L, bin);
	}

	// ======================================================================
	// The room, as the analysis sees it
	// ======================================================================

	// One acoustic path from a pump to the microphone, resampled to the
	// analysis rate. Sparse on purpose: a traced response is a few hundred
	// arrivals, not a dense recording, so convolution is a scatter over taps
	// rather than a full FIR and costs taps-per-sample instead of length.
	//
	// Gains are signed. `Acoustics::BuildImpulseTaps` randomises the sign of
	// each tail impulse, and that is not cosmetic -- same-sign impulses sum
	// coherently into a ringing comb, random signs sum into noise, which is
	// what a diffuse tail actually is.
	struct SparseIr
	{
		struct Tap
		{
			int Delay = 0;     // samples at kAnalysisRate
			float Gain = 0.0f; // linear, signed
		};

		std::vector<Tap> Taps;
		int MaxDelay = 0;

		void Add(float delaySeconds, float gain)
		{
			int delay = (int)std::lround((double)delaySeconds * (double)kAnalysisRate);
			if (delay < 0)
				delay = 0;

			Taps.push_back({ delay, gain });
			MaxDelay = std::max(MaxDelay, delay);
		}

		void Clear()
		{
			Taps.clear();
			MaxDelay = 0;
		}

		// Sorted by delay so the direct sound is first and the cepstral checks
		// below can find the first reflection without searching.
		void Sort()
		{
			std::sort(Taps.begin(), Taps.end(),
				[](const Tap& a, const Tap& b) { return a.Delay < b.Delay; });
		}
	};

	// ======================================================================
	// The pumps
	// ======================================================================

	// What is wrong with a pump, if anything.
	//
	// Three injectors rather than one, because they sit at different points on
	// the difficulty scale and the whole point of the rig is to find where the
	// methods stop working:
	//
	//   Imbalance   -- energy at 1x and 2x shaft. Cleanly shaft-locked, so it
	//                  carries the machine's identity in its own frequency.
	//                  Easy for the envelope method, if the speeds differ.
	//   Bearing     -- a resonance rung repeatedly at a defect rate that is a
	//                  non-integer multiple of shaft speed. The energy sits
	//                  high (kHz) but its *envelope* beats at the defect rate,
	//                  which is what demodulation recovers.
	//   Cavitation  -- broadband hiss, gated by flow rather than by rotation.
	//                  Carries almost no shaft identity, so it is the case that
	//                  defeats every method except the channel one. This is the
	//                  honest hard case and it is here to fail loudly.
	struct FaultConfig
	{
		bool Bearing = false;
		bool Cavitation = false;
		bool Imbalance = false;

		float BearingSeverity = 0.0f;      // linear amplitude of the ring
		float CavitationSeverity = 0.0f;
		float ImbalanceSeverity = 0.0f;

		// Where the bearing rings. Real outer-race resonances land between
		// about 2 and 8 kHz; the exact value does not matter to the method, but
		// it has to be well above the shaft orders or demodulation picks up the
		// running speed instead of the fault.
		float BearingResonanceHz = 3200.0f;
		// Defect rate as a multiple of shaft speed. Deliberately not an
		// integer: BPFO for a typical 8-ball bearing is around 3.05x, and a
		// non-integer rate is what lets a defect tone be told apart from a
		// shaft harmonic in the envelope spectrum.
		float BearingDefectOrder = 3.05f;

		bool Any() const { return Bearing || Cavitation || Imbalance; }
	};

	struct PumpConfig
	{
		float ShaftHz = 24.5f;      // 4-pole on 50 Hz at 2% slip
		int BladeCount = 5;
		float LineHz = 50.0f;       // hum lands at 2x this
		float FlowNoise = 0.05f;
		float Level = 1.0f;
		FaultConfig Fault;
	};

	// Generates one pump's dry signal, continuously.
	//
	// Stateful rather than a pure function of t, because the bearing impulse
	// train and the cavitation gating both carry state between blocks, and a
	// block boundary that resets either of them is an audible click and a
	// spectral artefact sitting exactly at the block rate. That artefact is
	// broadband and identical in both pumps, which is precisely the shape that
	// would fool the channel method into reporting a phantom fault.
	class PumpSynth
	{
	public:
		// `sampleRate` defaults to the analysis rate. The audible path renders
		// the same machine at the device's rate instead -- a clip is assumed to
		// already be at the mixer's rate, so generating at 12 kHz and handing it
		// over would play the pump four times too slow.
		void Reset(const PumpConfig& config, uint32_t seed, float sampleRate = (float)kAnalysisRate)
		{
			m_Config = config;
			m_SampleRate = sampleRate;
			m_Rng = Rng(seed);
			m_Phase = 0.0;
			m_HumPhase = 0.0;
			m_NoiseLow = 0.0f;
			m_NoiseHigh = 0.0f;
			m_RingPhase = 0.0f;
			m_RingAmplitude = 0.0f;
			m_NextImpulse = 0.0;
			m_CavitationGate = 0.0f;

			// A fixed but per-pump phase offset. Two pumps starting in phase is
			// a coincidence that never happens and it makes the mixture look
			// artificially coherent.
			m_Phase = (double)(seed & 0xFFFFu) / 65536.0;
			m_HumPhase = (double)((seed >> 16) & 0xFFFFu) / 65536.0;
		}

		void SetConfig(const PumpConfig& config) { m_Config = config; }
		const PumpConfig& GetConfig() const { return m_Config; }

		// `healthyOnly` renders the machine as if nothing were wrong, which is
		// how the baselines are taken. Same seed, same everything else -- so
		// the difference between a baseline and a run really is only the fault.
		void Render(float* out, int count, bool healthyOnly)
		{
			const double rate = (double)m_SampleRate;
			const double shaft = (double)m_Config.ShaftHz;
			const double hum = 2.0 * (double)m_Config.LineHz;

			const FaultConfig& fault = m_Config.Fault;
			const bool bearing = !healthyOnly && fault.Bearing && fault.BearingSeverity > 0.0f;
			const bool cavitation = !healthyOnly && fault.Cavitation && fault.CavitationSeverity > 0.0f;
			const bool imbalance = !healthyOnly && fault.Imbalance && fault.ImbalanceSeverity > 0.0f;

			// Impulses per sample for the bearing defect.
			const double defectRate = shaft * (double)fault.BearingDefectOrder;
			const double ringOmega = 2.0 * kPi * (double)fault.BearingResonanceHz / rate;
			// Ring down to roughly 5% over half a defect period, so successive
			// impulses are separable rather than merging into a tone.
			const double ringDecay = defectRate > 0.0
				? std::exp(-2.0 * defectRate / rate * 3.0)
				: 0.99;

			for (int i = 0; i < count; i++)
			{
				float sample = 0.0f;

				// --- Shaft orders -----------------------------------------
				// 1/k falloff. Real pump spectra are messier, but the shape of
				// the comb is what the methods key on, not its exact taper.
				for (int k = 1; k <= 6; k++)
				{
					float amplitude = 0.12f / (float)k;

					if (imbalance && (k == 1 || k == 2))
						amplitude *= 1.0f + fault.ImbalanceSeverity * (k == 1 ? 6.0f : 3.0f);

					sample += amplitude * (float)std::sin(2.0 * kPi * (double)k * m_Phase);
				}

				// --- Blade pass -------------------------------------------
				// The loudest thing a healthy pump does, and a strong shaft
				// harmonic, so it belongs to the comb like any other order.
				sample += 0.20f * (float)std::sin(2.0 * kPi * (double)m_Config.BladeCount * m_Phase);
				sample += 0.07f * (float)std::sin(4.0 * kPi * (double)m_Config.BladeCount * m_Phase);

				// --- Line hum ---------------------------------------------
				// **The confounder.** Both motors are on the same grid, so this
				// tone is at exactly the same frequency in both pumps no matter
				// how far apart their shaft speeds drift. Anything that tries
				// to attribute energy by frequency alone will find this line
				// unassignable, which is correct and worth seeing.
				sample += 0.10f * (float)std::sin(2.0 * kPi * m_HumPhase);

				// --- Flow noise -------------------------------------------
				// One-pole lowpassed white, which is a fair stand-in for the
				// broadband part of a healthy pump.
				float white = m_Rng.Gaussian();
				m_NoiseLow += 0.08f * (white - m_NoiseLow);
				sample += m_Config.FlowNoise * m_NoiseLow * 4.0f;

				// --- Bearing defect ---------------------------------------
				if (bearing)
				{
					m_NextImpulse -= defectRate / rate;
					if (m_NextImpulse <= 0.0)
					{
						// Bearings slip. A few percent of jitter is what stops
						// the defect tone being a pure line, and it is why the
						// envelope peak has width in the real world.
						m_NextImpulse += 1.0 + (double)m_Rng.Bipolar() * 0.02;
						m_RingAmplitude = fault.BearingSeverity;
						m_RingPhase = 0.0f;
					}

					if (m_RingAmplitude > 1e-6f)
					{
						sample += m_RingAmplitude * (float)std::sin((double)m_RingPhase);
						m_RingPhase += (float)ringOmega;
						m_RingAmplitude *= (float)ringDecay;
					}
				}

				// --- Cavitation -------------------------------------------
				if (cavitation)
				{
					// Highpassed noise, amplitude-modulated by a slow random
					// gate that is *not* locked to the shaft. That absence of
					// locking is the whole character of the fault and the
					// reason the envelope method cannot place it.
					float n = m_Rng.Gaussian();
					m_NoiseHigh += 0.45f * (n - m_NoiseHigh);
					float highpassed = n - m_NoiseHigh;

					m_CavitationGate += 0.0006f * (m_Rng.Unit() - m_CavitationGate);
					sample += fault.CavitationSeverity * highpassed * (0.4f + m_CavitationGate * 2.0f);
				}

				m_Phase += shaft / rate;
				if (m_Phase > 1.0)
					m_Phase -= std::floor(m_Phase);

				m_HumPhase += hum / rate;
				if (m_HumPhase > 1.0)
					m_HumPhase -= std::floor(m_HumPhase);

				out[i] = sample * m_Config.Level;
			}
		}
	private:
		PumpConfig m_Config;
		Rng m_Rng{ 1u };
		float m_SampleRate = (float)kAnalysisRate;

		double m_Phase = 0.0;
		double m_HumPhase = 0.0;
		float m_NoiseLow = 0.0f;
		float m_NoiseHigh = 0.0f;

		float m_RingPhase = 0.0f;
		float m_RingAmplitude = 0.0f;
		double m_NextImpulse = 0.0;
		float m_CavitationGate = 0.0f;
	};


	// ======================================================================
	// The microphone
	// ======================================================================

	// Power-of-two ring, so the wrap is a mask rather than a division. That
	// matters more than it looks: the convolution below indexes the ring once
	// per tap per sample, which is the innermost loop in the whole demo.
	class RingBuffer
	{
	public:
		void Resize(size_t minimum)
		{
			size_t size = NextPowerOfTwo(std::max<size_t>(minimum, 2));
			m_Data.assign(size, 0.0f);
			m_Mask = size - 1;
			m_Write = 0;
		}

		void Clear()
		{
			std::fill(m_Data.begin(), m_Data.end(), 0.0f);
			m_Write = 0;
		}

		size_t Size() const { return m_Data.size(); }

		void Push(float value)
		{
			m_Data[m_Write] = value;
			m_Write = (m_Write + 1) & m_Mask;
		}

		// `age` samples before the most recent push. Ago(0) is the sample just
		// pushed, which is what makes the convolution below read naturally.
		float Ago(size_t age) const
		{
			return m_Data[(m_Write - 1 - age) & m_Mask];
		}

		// Oldest first, ending at the most recent sample.
		void CopyRecent(float* out, size_t count) const
		{
			for (size_t i = 0; i < count; i++)
				out[i] = Ago(count - 1 - i);
		}
	private:
		std::vector<float> m_Data;
		size_t m_Mask = 0;
		size_t m_Write = 0;
	};

	// Two pumps, two room paths, one channel out.
	//
	// **This is the honest single-microphone model and it is deliberately not
	// the signal you hear.** The engine's mixer is stereo and pans each voice
	// by direction, which hands the analysis a left/right cue that a real mono
	// microphone does not have -- and a method that quietly leaned on that cue
	// would report a feasibility that evaporates on site. So the mixer drives
	// the speakers and this drives the numbers, both from the same trace.
	class Microphone
	{
	public:
		static constexpr int PumpCount = 2;

		void Configure(const PumpConfig& a, const PumpConfig& b, uint32_t seed)
		{
			m_Config[0] = a;
			m_Config[1] = b;

			// Different seeds, or the two pumps emit correlated noise and add
			// in amplitude rather than in power -- which breaks the additivity
			// the whole baseline subtraction rests on.
			m_Synth[0].Reset(a, seed);
			m_Synth[1].Reset(b, seed * 2654435761u + 1u);
		}

		void SetConfig(int pump, const PumpConfig& config)
		{
			m_Config[pump] = config;
			m_Synth[pump].SetConfig(config);
		}

		const PumpConfig& GetConfig(int pump) const { return m_Config[pump]; }

		void SetChannel(int pump, const SparseIr& ir) { m_Channel[pump] = ir; }
		const SparseIr& GetChannel(int pump) const { return m_Channel[pump]; }

		// historySeconds has to cover the longest analysis window *plus* the
		// longest room path, or the oldest samples in a window were convolved
		// against source that had already been overwritten.
		void Allocate(float historySeconds)
		{
			size_t samples = (size_t)(historySeconds * (float)kAnalysisRate) + 8192;

			for (int p = 0; p < PumpCount; p++)
				m_Source[p].Resize(samples);

			m_Mic.Resize(samples);
			m_Filled = 0;
		}

		void Clear()
		{
			for (int p = 0; p < PumpCount; p++)
				m_Source[p].Clear();

			m_Mic.Clear();
			m_Filled = 0;
		}

		size_t Filled() const { return m_Filled; }

		// Advance the simulation by `count` samples.
		//
		// `healthyOnly` renders a pump as if nothing were wrong; `mute`
		// silences it entirely. Between them these give the three recordings a
		// commissioning engineer can actually take: both running, A alone,
		// B alone.
		void Advance(int count, const bool healthyOnly[PumpCount], const bool mute[PumpCount])
		{
			m_Scratch.resize((size_t)count);

			// Each pump is generated into scratch, pushed into its own source
			// ring, then convolved. Generating all of a pump's block at once
			// keeps the synth's inner state coherent, which the bearing
			// impulse train needs.
			for (int p = 0; p < PumpCount; p++)
			{
				m_Synth[p].Render(m_Scratch.data(), count, healthyOnly[p]);

				if (mute[p])
					std::fill(m_Scratch.begin(), m_Scratch.end(), 0.0f);

				m_Block[p] = m_Scratch;
			}

			for (int i = 0; i < count; i++)
			{
				float mixed = 0.0f;

				for (int p = 0; p < PumpCount; p++)
				{
					m_Source[p].Push(m_Block[p][(size_t)i]);

					// Sparse convolution as a gather. A traced response is a
					// few hundred arrivals in a third of a second, so this
					// costs taps-per-sample rather than length-per-sample --
					// about 500 multiply-adds where a dense FIR would want
					// 4200.
					const SparseIr& ir = m_Channel[p];
					float sum = 0.0f;
					for (const SparseIr::Tap& tap : ir.Taps)
						sum += tap.Gain * m_Source[p].Ago((size_t)tap.Delay);

					mixed += sum;
				}

				m_Mic.Push(mixed);
			}

			m_Filled += (size_t)count;
		}

		// The most recent `count` samples of the microphone, oldest first.
		void ReadRecent(std::vector<float>& out, size_t count) const
		{
			out.resize(count);
			m_Mic.CopyRecent(out.data(), count);
		}
	private:
		PumpConfig m_Config[PumpCount];
		PumpSynth m_Synth[PumpCount];
		SparseIr m_Channel[PumpCount];

		RingBuffer m_Source[PumpCount];
		RingBuffer m_Mic;

		std::vector<float> m_Scratch;
		std::vector<float> m_Block[PumpCount];
		size_t m_Filled = 0;
	};

	// ======================================================================
	// Baselines
	// ======================================================================

	// One pump alone, healthy, measured through its own room path. This is the
	// commissioning recording, and everything in AttributeByChannel is built on
	// having it.
	//
	// **Baselines are tied to a geometry.** Move the microphone and both |H|
	// change, so a baseline taken before the move describes a room that no
	// longer exists -- and the method fails quietly rather than loudly, because
	// the arithmetic still produces two plausible-looking numbers. Fingerprint
	// is the guard: the demo stores the positions the baselines were taken at
	// and warns when they have drifted. On a real site this is the assumption
	// that breaks first, usually when someone moves the mic to run a cable.
	struct Baselines
	{
		std::vector<float> Psd[Microphone::PumpCount];
		float Fingerprint = 0.0f;
		int FftSize = 0;
		bool Valid = false;

		void Clear()
		{
			for (int p = 0; p < Microphone::PumpCount; p++)
				Psd[p].clear();

			Valid = false;
			FftSize = 0;
		}
	};

	// ======================================================================
	// M0 -- broadband level
	//
	// The control that fails, kept because the failure is the finding. A rise
	// in overall level says something got worse and carries no information
	// whatsoever about which machine did it -- and it is what a level meter
	// bolted to a wall would tell you, which is why it is worth showing next to
	// the methods that do work.
	// ======================================================================
	inline float BroadbandLevelDb(const float* x, size_t count)
	{
		if (count == 0)
			return -120.0f;

		double sum = 0.0;
		for (size_t i = 0; i < count; i++)
			sum += (double)x[i] * x[i];

		double rms = std::sqrt(sum / (double)count);
		return rms > 1e-12 ? (float)(20.0 * std::log10(rms)) : -120.0f;
	}

	// ======================================================================
	// M1 -- spectral residual
	//
	// What the mixture has that the two healthy baselines do not. This isolates
	// *what* broke -- and, being a difference of three noisy PSD estimates, it
	// is also the thing whose noise floor decides how small a fault the rig can
	// see at all. It cannot say which pump, which is exactly why M2 exists.
	// ======================================================================
	inline std::vector<float> SpectralResidual(const std::vector<float>& mixture,
		const Baselines& baselines)
	{
		std::vector<float> residual(mixture.size(), 0.0f);

		if (!baselines.Valid || baselines.Psd[0].size() != mixture.size())
			return residual;

		for (size_t b = 0; b < mixture.size(); b++)
			residual[b] = mixture[b] - baselines.Psd[0][b] - baselines.Psd[1][b];

		return residual;
	}

	// ======================================================================
	// M2 -- channel-matched attribution
	// ======================================================================

	struct ChannelFit
	{
		// Fractional rise in each pump's own band energy. 0.5 means that pump
		// is emitting 50% more power in the fault band than its baseline did.
		float Alpha[Microphone::PumpCount] = { 0.0f, 0.0f };
		// 10 log10(1 + Alpha) -- how many dB that pump got louder in band.
		float SeverityDb[Microphone::PumpCount] = { 0.0f, 0.0f };

		// Condition number of the 2-column design matrix. **This is the
		// feasibility number.** Near 1 the two channels colour the band very
		// differently and attribution is easy; large means they are nearly
		// parallel and no amount of SNR will separate them.
		float Conditioning = 0.0f;
		// Fraction of the residual's energy the two-channel model explains.
		// Low means the residual is not shaped like either channel -- noise, or
		// something the model does not cover.
		float FitQuality = 0.0f;

		// One standard error on each Alpha, propagated from the PSD estimator's
		// own variance. **This is what makes a health score trustworthy.** A
		// Welch estimate of a noisy spectrum has a standard error of about
		// P/sqrt(m) for m averaged segments, and the residual is a difference
		// of three such estimates -- so there is always some alpha even when
		// both machines are perfect. Without an error bar there is no way to
		// tell a small real fault from that floor, and the fit will happily
		// report a confident number for pure noise.
		float AlphaError[Microphone::PumpCount] = { 0.0f, 0.0f };

		// Alpha divided by its own error. Below about 3 the reading is noise;
		// this is the number to threshold on, not the severity.
		float SignalToNoise[Microphone::PumpCount] = { 0.0f, 0.0f };

		// The smallest fault this measurement could have seen at all, as a
		// fractional rise in band energy. Longer windows push it down as
		// 1/sqrt(time), which is the trade a real installation gets to make.
		float MinDetectableAlpha = 0.0f;

		// Detection: is either machine worse than its baseline?
		float Total = 0.0f;
		float TotalError = 0.0f;
		float DetectionSigma = 0.0f;

		// Attribution: is one machine worse than the other? A separate
		// question with a separate answer, and the one that actually needs the
		// covariance between the two coefficients.
		float Difference = 0.0f;
		float DifferenceError = 0.0f;
		float AttributionSigma = 0.0f;

		// How much bigger the actual misfit is than the estimator noise alone
		// predicts. Above 1 the two-channel model is not describing the
		// residual -- usually because the fault's own spectrum is not flat
		// across the chosen band -- and the error bars below have been widened
		// by its square root to say so.
		float ReducedChiSquare = 1.0f;

		float BandLowHz = 0.0f;
		float BandHighHz = 0.0f;
		int BinsUsed = 0;
		int Segments = 0;
		int SubBandsUsed = 0;
		bool Usable = false;

		// Which pump, if either, the evidence actually supports.
		//  -1 undecided, 0 pump A, 1 pump B, 2 both.
		int Verdict = -1;
	};

	// Non-negative least squares for exactly two variables.
	//
	// General NNLS is an active-set iteration; with two unknowns the active set
	// has four possibilities and each is a closed form, so the whole thing is
	// the unconstrained solve plus two one-dimensional fallbacks. Worth doing
	// properly rather than clamping the unconstrained answer: clamping a
	// negative coefficient to zero leaves the *other* coefficient at a value
	// that was optimal only while the first was free, which biases the ratio --
	// and the ratio is the entire output.
	//
	// **Every tolerance here is relative.** An earlier version compared the
	// determinant against a fixed 1e-20, which is a dimensional mistake: the
	// determinant has the units of the basis to the fourth power, and the
	// baselines this is called with are PSD bins around 1e-8. The determinant
	// then sits near 1e-27, the guard fails on every call, and the solver
	// silently returns its one-variable fallback for ever -- which looks
	// exactly like a real answer, because one of the two pumps genuinely does
	// come out at zero. Callers normalise the basis to unit norm before calling
	// (see AttributeByChannel), so uu and vv arrive as 1 and the epsilons below
	// are pure numbers.
	inline void SolveNnls2(double uu, double uv, double vv,
		double uy, double vy, double yy, double& outA, double& outB, double& outResidual)
	{
		auto residualFor = [&](double a, double b)
		{
			// ||y - aU - bV||^2 expanded, so y itself is never needed.
			return yy - 2.0 * (a * uy + b * vy)
				+ a * a * uu + 2.0 * a * b * uv + b * b * vv;
		};

		double scale = uu * vv;
		double det = uu * vv - uv * uv;

		if (scale > 0.0 && std::fabs(det) > 1e-12 * scale)
		{
			double a = (vv * uy - uv * vy) / det;
			double b = (uu * vy - uv * uy) / det;

			if (a >= 0.0 && b >= 0.0)
			{
				outA = a;
				outB = b;
				outResidual = residualFor(a, b);
				return;
			}
		}

		// One of them is pinned at zero. Try both edges and keep the better.
		double aOnly = uu > 0.0 ? std::max(0.0, uy / uu) : 0.0;
		double bOnly = vv > 0.0 ? std::max(0.0, vy / vv) : 0.0;

		double rA = residualFor(aOnly, 0.0);
		double rB = residualFor(0.0, bOnly);

		if (rA <= rB)
		{
			outA = aOnly;
			outB = 0.0;
			outResidual = rA;
		}
		else
		{
			outA = 0.0;
			outB = bOnly;
			outResidual = rB;
		}
	}

	// Fit the residual as a non-negative combination of the two baselines.
	//
	// The header derives this as regressing R/P_A against [1, rho]. What is
	// implemented is the equivalent fit of R directly against [P_A, P_B], which
	// is the same model under a different weighting -- and a much better
	// conditioned one. Dividing by P_A magnifies exactly the bins where the
	// baseline is weakest and the estimate is least trustworthy, so a single
	// spectral null could dominate the whole fit. Weighting by P_A instead
	// gives most of the say to the bins that were measured best, which is what
	// you would ask for if you were choosing the weights deliberately.
	//
	// Because P_A = |H_A|^2 S and P_B = |H_B|^2 S, and
	// R = G (a_A |H_A|^2 + a_B |H_B|^2), the fitted coefficients come out as
	// (G/S) a_A and (G/S) a_B. The common factor cancels in the ratio, and each
	// coefficient on its own is already the fractional rise in that pump's own
	// band energy -- which is the health score, without further scaling.
	//
	// `segments` is how many Welch segments went into each PSD, and `smoothing`
	// how many bins each was averaged over afterwards. Both raise the effective
	// number of independent averages, which is what sets the error bars -- pass
	// them honestly or the uncertainties are fiction.
	// One sub-band's worth of the fit. Accumulated, normalised and solved
	// independently, then combined with the others.
	struct SubBandFit
	{
		double Alpha[2] = { 0.0, 0.0 };
		double Cov[3] = { 0.0, 0.0, 0.0 };   // varA, covAB, varB
		double Conditioning = 0.0;
		double Residual = 0.0;
		double Predicted = 0.0;
		bool Usable = false;
	};

	// Fit one contiguous run of bins.
	//
	// Deliberately *unconstrained* -- negatives are allowed here and only
	// clamped after the sub-bands are combined. Clamping each one first would
	// rectify its noise: a sub-band with no fault in it scatters either side of
	// zero, and pinning the negative half at zero turns symmetric noise into a
	// positive bias that then survives the averaging. That bias is
	// indistinguishable from a small real fault, which is the worst possible
	// failure for a health score.
	inline SubBandFit FitBand(const std::vector<float>& mixturePsd,
		const Baselines& baselines, size_t first, size_t last, double effective)
	{
		SubBandFit out;

		if (last <= first + 2)
			return out;

		double uu = 0.0, uv = 0.0, vv = 0.0, uy = 0.0, vy = 0.0, yy = 0.0;
		double suu = 0.0, suv = 0.0, svv = 0.0, predicted = 0.0;

		for (size_t b = first; b <= last; b++)
		{
			double u = (double)baselines.Psd[0][b];
			double v = (double)baselines.Psd[1][b];
			double mix = (double)mixturePsd[b];
			double y = mix - u - v;

			uu += u * u; uv += u * v; vv += v * v;
			uy += u * y; vy += v * y; yy += y * y;

			double variance = (mix * mix + u * u + v * v) / effective;
			suu += u * u * variance;
			suv += u * v * variance;
			svv += v * v * variance;
			predicted += variance;
		}

		double normU = std::sqrt(uu), normV = std::sqrt(vv);
		if (normU <= 0.0 || normV <= 0.0)
			return out;

		double c = uv / (normU * normV);                 // channel correlation
		double det = 1.0 - c * c;
		if (det <= 1e-10)
			return out;

		double nuy = uy / normU, nvy = vy / normV;
		double a = (nuy - c * nvy) / det;
		double b = (nvy - c * nuy) / det;

		double residual = yy - 2.0 * (a * nuy + b * nvy)
			+ a * a + 2.0 * a * b * c + b * b;

		// Sandwich covariance on the normalised basis.
		double nsuu = suu / (normU * normU);
		double nsuv = suv / (normU * normV);
		double nsvv = svv / (normV * normV);

		double m00 = 1.0 / det, m01 = -c / det, m11 = 1.0 / det;

		double varA = m00 * m00 * nsuu + 2.0 * m00 * m01 * nsuv + m01 * m01 * nsvv;
		double varB = m01 * m01 * nsuu + 2.0 * m01 * m11 * nsuv + m11 * m11 * nsvv;
		double cov  = m00 * nsuu * m01 + m00 * nsuv * m11
			+ m01 * nsuv * m01 + m01 * nsvv * m11;

		// Back to the original scale.
		out.Alpha[0] = a / normU;
		out.Alpha[1] = b / normV;

		varA /= normU * normU;
		varB /= normV * normV;
		cov  /= normU * normV;

		// Widen by this band's own misfit, on the same reasoning as before --
		// but now applied where it belongs. A band whose fault shape really is
		// flat gets chi-square near 1 and keeps its precision; one straddling a
		// resonance edge is widened and quietly loses its vote in the
		// combination below. That is the whole reason for splitting the band:
		// the misfit is local, so the penalty should be too.
		double chi = predicted > 1e-30 ? residual / predicted : 1.0;
		double inflate = std::max(1.0, chi);

		out.Cov[0] = varA * inflate;
		out.Cov[1] = cov * inflate;
		out.Cov[2] = varB * inflate;
		out.Conditioning = std::sqrt((1.0 + std::fabs(c)) / std::max(1e-12, 1.0 - std::fabs(c)));
		out.Residual = residual;
		out.Predicted = predicted;
		out.Usable = true;

		return out;
	}

	// Fit the residual as a non-negative combination of the two baselines.
	//
	// The header derives this as regressing R/P_A against [1, rho]. What is
	// implemented is the equivalent fit of R directly against [P_A, P_B], which
	// is the same model under a different weighting -- and a much better
	// conditioned one. Dividing by P_A magnifies exactly the bins where the
	// baseline is weakest and the estimate is least trustworthy, so a single
	// spectral null could dominate the whole fit. Weighting by P_A instead
	// gives most of the say to the bins that were measured best.
	//
	// Because P_A = |H_A|^2 S and P_B = |H_B|^2 S, and
	// R = G (a_A |H_A|^2 + a_B |H_B|^2), the fitted coefficients come out as
	// (G/S) a_A and (G/S) a_B -- the common factor cancels in the ratio, and
	// each coefficient is already the fractional rise in that pump's own band
	// energy, which is the health score without further scaling.
	//
	// **Fitted in sub-bands rather than all at once**, and that is not a
	// refinement -- it is what makes the model true. The derivation needs G/S
	// constant across whatever is being fitted. A bearing resonance is a peak a
	// few hundred Hz wide, so across a 2 kHz band G/S varies by orders of
	// magnitude and the model cannot describe its own residual: measured, the
	// misfit ran 60x the estimator noise, and the error bars had to be widened
	// so far that a *larger* fault became harder to place than a small one.
	// Split into bands narrow enough for G/S to be roughly flat, each fit is
	// honest, and the combination below is a straight inverse-covariance
	// average of estimates that now agree with each other.
	//
	// `segments` is how many Welch segments went into each PSD and `smoothing`
	// how many bins each was averaged over afterwards; both set the error bars,
	// so pass them honestly or the uncertainties are fiction. Smoothing is
	// measured to *hurt* here and defaults to none -- what distinguishes two
	// room paths is the comb structure their reflections cut into the spectrum,
	// and averaging bins together erases precisely that.
	inline ChannelFit AttributeByChannel(const std::vector<float>& mixturePsd,
		const Baselines& baselines, int fftSize, float bandLowHz, float bandHighHz,
		int segments = 1, int smoothing = 0, int subBands = 8)
	{
		ChannelFit fit;
		fit.BandLowHz = bandLowHz;
		fit.BandHighHz = bandHighHz;
		fit.Segments = segments;

		if (!baselines.Valid || mixturePsd.empty()
			|| baselines.Psd[0].size() != mixturePsd.size())
			return fit;

		size_t first = std::max<size_t>(HzToBin(bandLowHz, fftSize, (float)kAnalysisRate), 1);
		size_t last = std::min(HzToBin(bandHighHz, fftSize, (float)kAnalysisRate),
			mixturePsd.size() - 1);

		if (last <= first + 8)
			return fit;

		fit.BinsUsed = (int)(last - first + 1);

		// 50%-overlapped Hann segments are not independent -- neighbours share
		// half their samples -- and the standard result is that m of them
		// behave like 9m/11. Smoothing over (2h+1) bins multiplies that again.
		// Getting this wrong makes every error bar wrong by the same factor.
		double effective = std::max(1.0, (double)segments * 9.0 / 11.0)
			* (double)(2 * std::max(0, smoothing) + 1);

		int bands = std::max(1, subBands);
		size_t width = (last - first + 1) / (size_t)bands;
		if (width < 6)
		{
			// Too few bins to split this finely; fall back to fewer, wider
			// bands rather than fitting noise.
			bands = std::max(1, (int)((last - first + 1) / 6));
			width = (last - first + 1) / (size_t)bands;
		}

		// Inverse-covariance combination: sum of C^-1, and of C^-1 x.
		double sumInv[3] = { 0.0, 0.0, 0.0 };   // [00], [01], [11]
		double sumInvX[2] = { 0.0, 0.0 };
		double conditioningSum = 0.0;
		double residualSum = 0.0, predictedSum = 0.0, ySum = 0.0;
		int used = 0;

		for (int k = 0; k < bands; k++)
		{
			size_t bandFirst = first + (size_t)k * width;
			size_t bandLast = (k == bands - 1) ? last : bandFirst + width - 1;

			SubBandFit band = FitBand(mixturePsd, baselines, bandFirst, bandLast, effective);
			if (!band.Usable)
				continue;

			double det = band.Cov[0] * band.Cov[2] - band.Cov[1] * band.Cov[1];
			if (det <= 0.0)
				continue;

			double i00 = band.Cov[2] / det;
			double i01 = -band.Cov[1] / det;
			double i11 = band.Cov[0] / det;

			sumInv[0] += i00;
			sumInv[1] += i01;
			sumInv[2] += i11;

			sumInvX[0] += i00 * band.Alpha[0] + i01 * band.Alpha[1];
			sumInvX[1] += i01 * band.Alpha[0] + i11 * band.Alpha[1];

			conditioningSum += band.Conditioning;
			residualSum += band.Residual;
			predictedSum += band.Predicted;

			for (size_t b = bandFirst; b <= bandLast; b++)
			{
				double y = (double)mixturePsd[b] - baselines.Psd[0][b] - baselines.Psd[1][b];
				ySum += y * y;
			}

			used++;
		}

		if (used == 0)
			return fit;

		double det = sumInv[0] * sumInv[2] - sumInv[1] * sumInv[1];
		if (det <= 0.0)
			return fit;

		double c00 = sumInv[2] / det;
		double c01 = -sumInv[1] / det;
		double c11 = sumInv[0] / det;

		double alphaA = c00 * sumInvX[0] + c01 * sumInvX[1];
		double alphaB = c01 * sumInvX[0] + c11 * sumInvX[1];

		// Clamped only now, after combining -- see the note in FitBand about
		// why clamping earlier would manufacture a positive bias.
		fit.Alpha[0] = (float)std::max(0.0, alphaA);
		fit.Alpha[1] = (float)std::max(0.0, alphaB);

		for (int p = 0; p < Microphone::PumpCount; p++)
			fit.SeverityDb[p] = 10.0f * std::log10(1.0f + std::max(0.0f, fit.Alpha[p]));

		fit.AlphaError[0] = (float)std::sqrt(std::max(0.0, c00));
		fit.AlphaError[1] = (float)std::sqrt(std::max(0.0, c11));

		for (int p = 0; p < Microphone::PumpCount; p++)
		{
			fit.SignalToNoise[p] = fit.AlphaError[p] > 0.0f
				? fit.Alpha[p] / fit.AlphaError[p]
				: 0.0f;
		}

		fit.Conditioning = (float)(conditioningSum / (double)used);
		fit.ReducedChiSquare = predictedSum > 1e-30 ? (float)(residualSum / predictedSum) : 1.0f;
		fit.FitQuality = ySum > 1e-30
			? std::clamp((float)(1.0 - residualSum / ySum), 0.0f, 1.0f)
			: 0.0f;
		fit.SubBandsUsed = used;

		fit.MinDetectableAlpha = 3.0f * std::min(
			fit.AlphaError[0] > 0.0f ? fit.AlphaError[0] : 1e30f,
			fit.AlphaError[1] > 0.0f ? fit.AlphaError[1] : 1e30f);

		// --- Detection and attribution are two different questions ----------
		//
		// This distinction was not in the first version and its absence was the
		// last real bug. Testing each alpha against zero asks "did pump A get
		// worse" and "did pump B get worse" separately, and when the model
		// misfits at all, *both* answers come back yes -- so a fault on one
		// machine was reported as a fault on both, every time, while the
		// correct pump always had the larger coefficient. The ranking was never
		// wrong; the test was asking the wrong question.
		//
		//   Is anything wrong at all?  ->  alpha_A + alpha_B against its error.
		//   Which machine is it?       ->  alpha_A - alpha_B against its error.
		//
		// The difference needs the covariance term. The two coefficients are
		// strongly *anti*-correlated -- nearly parallel basis vectors mean any
		// energy the fit gives to one it takes from the other -- so
		// var(A - B) = var(A) + var(B) - 2cov is much larger than the two
		// variances alone suggest, and ignoring cov would make attribution look
		// far more certain than it is.
		double varDiff = c00 + c11 - 2.0 * c01;
		double varSum = c00 + c11 + 2.0 * c01;

		fit.DifferenceError = (float)std::sqrt(std::max(0.0, varDiff));
		fit.TotalError = (float)std::sqrt(std::max(0.0, varSum));

		// **From the unclamped estimates, not the reported ones.** Clamping at
		// zero is right for a health score -- a negative severity is not a
		// thing -- but it is wrong for a test statistic: it makes the sum
		// one-sided, so a 3 sigma threshold that should fire on 0.1% of clean
		// windows fires on far more. Measured on a healthy pair, the clamped
		// version raised a false alarm in roughly one window in five.
		fit.Difference = (float)(alphaA - alphaB);
		fit.Total = (float)(alphaA + alphaB);

		fit.DetectionSigma = fit.TotalError > 0.0f ? fit.Total / fit.TotalError : 0.0f;
		fit.AttributionSigma = fit.DifferenceError > 0.0f
			? std::fabs(fit.Difference) / fit.DifferenceError
			: 0.0f;

		// Three sigma rather than two, because this runs continuously: a
		// two-sigma rule fires on 5% of clean windows, which on a 30 second
		// cadence is a false alarm every ten minutes and is how a monitoring
		// system gets switched off.
		if (fit.DetectionSigma < 3.0f)
			fit.Verdict = -1;                       // nothing wrong with either
		else if (fit.AttributionSigma < 3.0f)
			fit.Verdict = 2;                        // something is wrong, cannot say which
		else
			fit.Verdict = fit.Difference > 0.0f ? 0 : 1;

		fit.Usable = true;
		return fit;
	}

	// ======================================================================
	// M3 -- envelope demodulation
	// ======================================================================

	struct EnvelopeFit
	{
		float Score[Microphone::PumpCount] = { 0.0f, 0.0f };     // peak-to-floor, dB
		float DefectHz[Microphone::PumpCount] = { 0.0f, 0.0f };  // where it was looked for
		float PeakHz[Microphone::PumpCount] = { 0.0f, 0.0f };    // where it was found

		// |defect_A - defect_B|. Two lines this close need a window of at least
		// 1/SeparationHz seconds to resolve, full stop -- that is the Fourier
		// uncertainty limit and no window function beats it.
		float SeparationHz = 0.0f;
		float RequiredWindowSeconds = 0.0f;
		float ActualWindowSeconds = 0.0f;
		bool Resolvable = false;
		bool Usable = false;
	};

	// Zero out everything outside [low, high] and transform back.
	//
	// A brick wall in the frequency domain rings in the time domain, which
	// would normally be a poor filter -- but the very next step takes an
	// envelope, and the ringing is at the *carrier* frequency, far above
	// anything the envelope spectrum looks at. Here it costs nothing.
	inline void BandpassInPlace(std::vector<std::complex<float>>& spectrum,
		int fftSize, float lowHz, float highHz)
	{
		size_t lowBin = HzToBin(lowHz, fftSize, (float)kAnalysisRate);
		size_t highBin = HzToBin(highHz, fftSize, (float)kAnalysisRate);

		for (size_t b = 0; b < spectrum.size(); b++)
		{
			bool inBand = b >= lowBin && b <= highBin;
			if (!inBand)
				spectrum[b] = std::complex<float>(0.0f, 0.0f);
		}
	}

	// Band-limited analytic envelope.
	//
	// The analytic signal is built by keeping the positive frequencies, doubling
	// them and discarding the negative ones -- which is the Hilbert transform
	// done in the place where it is one line. Its magnitude is the envelope.
	//
	// Rectifying and lowpassing is the usual shortcut and it is worse here: it
	// folds the carrier down as a harmonic series, and those harmonics land in
	// the same few hundred Hz the envelope spectrum is being searched for
	// defect lines. The analytic magnitude has no carrier in it at all.
	inline std::vector<float> AnalyticEnvelope(const std::vector<float>& x,
		float lowHz, float highHz)
	{
		size_t n = NextPowerOfTwo(x.size());
		std::vector<std::complex<float>> buffer(n, std::complex<float>(0.0f, 0.0f));

		for (size_t i = 0; i < x.size(); i++)
			buffer[i] = std::complex<float>(x[i], 0.0f);

		Fft(buffer, false);
		BandpassInPlace(buffer, (int)n, lowHz, highHz);

		// Double the positive half, keeping DC and Nyquist alone; the negative
		// half is already zeroed by the bandpass.
		for (size_t b = 1; b < n / 2; b++)
			buffer[b] *= 2.0f;

		Fft(buffer, true);

		std::vector<float> envelope(x.size());
		for (size_t i = 0; i < x.size(); i++)
			envelope[i] = std::abs(buffer[i]);

		// The envelope is strictly positive, so it has a large DC term that
		// would otherwise dominate its own spectrum. Removing the mean is what
		// leaves the modulation behind.
		double mean = 0.0;
		for (float v : envelope)
			mean += v;
		mean /= (double)envelope.size();

		for (float& v : envelope)
			v -= (float)mean;

		return envelope;
	}

	// Height of the tallest bin within `toleranceHz` of `targetHz`, measured
	// against the median of the surrounding neighbourhood.
	//
	// Median rather than mean for the floor: a mean is dragged up by the very
	// peak being measured, and by any neighbour, so a forest of lines reads as
	// no lines at all.
	inline float PeakOverFloorDb(const std::vector<float>& spectrum, int fftSize,
		float targetHz, float toleranceHz, float& outPeakHz)
	{
		outPeakHz = 0.0f;

		if (spectrum.empty() || targetHz <= 0.0f)
			return 0.0f;

		size_t centre = HzToBin(targetHz, fftSize, (float)kAnalysisRate);
		size_t halfWidth = std::max<size_t>(HzToBin(toleranceHz, fftSize, (float)kAnalysisRate), 1);

		if (centre >= spectrum.size())
			return 0.0f;

		size_t first = centre > halfWidth ? centre - halfWidth : 1;
		size_t last = std::min(centre + halfWidth, spectrum.size() - 1);

		float peak = 0.0f;
		size_t peakBin = first;
		for (size_t b = first; b <= last; b++)
		{
			if (spectrum[b] > peak)
			{
				peak = spectrum[b];
				peakBin = b;
			}
		}

		outPeakHz = BinToHz(peakBin, fftSize, (float)kAnalysisRate);

		// Floor from a wider neighbourhood, excluding the search window itself.
		size_t floorHalf = halfWidth * 8;
		size_t floorFirst = centre > floorHalf ? centre - floorHalf : 1;
		size_t floorLast = std::min(centre + floorHalf, spectrum.size() - 1);

		std::vector<float> around;
		around.reserve(floorLast - floorFirst + 1);
		for (size_t b = floorFirst; b <= floorLast; b++)
		{
			if (b < first || b > last)
				around.push_back(spectrum[b]);
		}

		if (around.empty())
			return 0.0f;

		std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
		float floorLevel = around[around.size() / 2];

		if (floorLevel <= 1e-20f || peak <= 1e-20f)
			return 0.0f;

		return 10.0f * std::log10(peak / floorLevel);
	}

	// Attribute a bearing fault by which shaft its envelope beats with.
	//
	// This is the standard method for rotating machinery and it is here to be
	// measured rather than assumed: it works exactly as well as the two shaft
	// rates are different, and on fixed-speed pumps sharing a grid that
	// difference can be zero. `Resolvable` says whether the window was long
	// enough for the two defect lines to be separate objects at all -- if it is
	// false the two scores are reading the same peak and their ratio is
	// meaningless, which is a far more useful thing to report than two
	// confident-looking numbers.
	inline EnvelopeFit AttributeByEnvelope(const std::vector<float>& mic,
		const PumpConfig& pumpA, const PumpConfig& pumpB,
		float bandLowHz, float bandHighHz)
	{
		EnvelopeFit fit;

		if (mic.size() < 1024)
			return fit;

		fit.ActualWindowSeconds = (float)mic.size() / (float)kAnalysisRate;

		const PumpConfig* pumps[Microphone::PumpCount] = { &pumpA, &pumpB };
		for (int p = 0; p < Microphone::PumpCount; p++)
			fit.DefectHz[p] = pumps[p]->ShaftHz * pumps[p]->Fault.BearingDefectOrder;

		fit.SeparationHz = std::fabs(fit.DefectHz[0] - fit.DefectHz[1]);
		fit.RequiredWindowSeconds = fit.SeparationHz > 1e-6f
			? 1.0f / fit.SeparationHz
			: std::numeric_limits<float>::infinity();
		fit.Resolvable = fit.ActualWindowSeconds >= fit.RequiredWindowSeconds;

		std::vector<float> envelope = AnalyticEnvelope(mic, bandLowHz, bandHighHz);

		// One long transform rather than Welch: resolution is the entire point
		// here, and averaging segments would trade away the only thing that
		// separates the two defect lines.
		size_t n = NextPowerOfTwo(envelope.size());
		std::vector<float> window = HannWindow((int)envelope.size());
		std::vector<std::complex<float>> buffer(n, std::complex<float>(0.0f, 0.0f));

		for (size_t i = 0; i < envelope.size(); i++)
			buffer[i] = std::complex<float>(envelope[i] * window[i], 0.0f);

		Fft(buffer, false);

		std::vector<float> spectrum(n / 2 + 1);
		for (size_t b = 0; b < spectrum.size(); b++)
		{
			float re = buffer[b].real();
			float im = buffer[b].imag();
			spectrum[b] = re * re + im * im;
		}

		// Search within half the separation, so the two windows never overlap
		// and each score really is about its own pump. Floored, because a
		// separation of zero would otherwise give a zero-width search.
		float tolerance = std::max(fit.SeparationHz * 0.5f, 0.5f);

		for (int p = 0; p < Microphone::PumpCount; p++)
			fit.Score[p] = PeakOverFloorDb(spectrum, (int)n, fit.DefectHz[p], tolerance, fit.PeakHz[p]);

		fit.Usable = true;
		return fit;
	}

	// ======================================================================
	// M4 -- cepstral channel signature
	// ======================================================================

	struct CepstrumFit
	{
		float Score[Microphone::PumpCount] = { 0.0f, 0.0f };
		float PredictedQuefrency[Microphone::PumpCount] = { 0.0f, 0.0f };  // seconds
		float MeasuredQuefrency[Microphone::PumpCount] = { 0.0f, 0.0f };
		bool Usable = false;
	};

	// The real cepstrum: inverse transform of the log magnitude spectrum.
	//
	// An echo at delay d multiplies the spectrum by (1 + g e^{-i w d}), and the
	// log turns that product into a sum -- so the echo becomes an additive
	// ripple of period 1/d, and transforming again puts a peak at quefrency d.
	// That is the whole trick, and it is why a cepstrum finds delays that are
	// invisible in the spectrum.
	inline std::vector<float> RealCepstrum(const std::vector<float>& x)
	{
		size_t n = NextPowerOfTwo(x.size());
		std::vector<std::complex<float>> buffer(n, std::complex<float>(0.0f, 0.0f));

		std::vector<float> window = HannWindow((int)x.size());
		for (size_t i = 0; i < x.size(); i++)
			buffer[i] = std::complex<float>(x[i] * window[i], 0.0f);

		Fft(buffer, false);

		for (size_t b = 0; b < n; b++)
		{
			// The floor stops log(0) at spectral nulls. Without it a single
			// empty bin produces a -inf that smears across the entire
			// cepstrum when it is transformed back.
			float power = std::norm(buffer[b]);
			buffer[b] = std::complex<float>(0.5f * std::log(power + 1e-20f), 0.0f);
		}

		Fft(buffer, true);

		std::vector<float> cepstrum(n / 2);
		for (size_t q = 0; q < cepstrum.size(); q++)
			cepstrum[q] = buffer[q].real();

		return cepstrum;
	}

	// Attribute by which room path's echo signature the energy carries.
	//
	// Each pump stands at a different distance from the walls, so the gap
	// between its direct sound and its first reflection differs -- and that gap
	// is a property of the *channel*, so anything arriving through it is
	// stamped with it. Predicted quefrencies come from the traced geometry:
	// (reflected path length - direct distance) / speed of sound.
	//
	// **That prediction is the hand-check.** It is arithmetic the cepstrum knows
	// nothing about, so a measured peak landing on it is real agreement rather
	// than the implementation's own formula echoed back. See the test in
	// PumpDiagnostics.h.
	inline CepstrumFit AttributeByCepstrum(const std::vector<float>& mic,
		float quefrencyA, float quefrencyB)
	{
		CepstrumFit fit;
		fit.PredictedQuefrency[0] = quefrencyA;
		fit.PredictedQuefrency[1] = quefrencyB;

		if (mic.size() < 1024 || quefrencyA <= 0.0f || quefrencyB <= 0.0f)
			return fit;

		std::vector<float> cepstrum = RealCepstrum(mic);

		// Magnitude, because an echo's cepstral peak takes the sign of the
		// reflection's gain and a wall can invert.
		std::vector<float> magnitude(cepstrum.size());
		for (size_t q = 0; q < cepstrum.size(); q++)
			magnitude[q] = std::fabs(cepstrum[q]);

		// The first few quefrencies hold the spectral envelope rather than any
		// echo, and they are enormous. Blanking them stops the search finding
		// the log-spectrum's own shape.
		size_t blank = std::min<size_t>(magnitude.size(), 24);
		for (size_t q = 0; q < blank; q++)
			magnitude[q] = 0.0f;

		const float predicted[Microphone::PumpCount] = { quefrencyA, quefrencyB };

		for (int p = 0; p < Microphone::PumpCount; p++)
		{
			size_t centre = (size_t)std::lround((double)predicted[p] * (double)kAnalysisRate);
			// Half a millisecond either side. Wider and the two pumps' windows
			// start to overlap in a small room.
			size_t half = std::max<size_t>((size_t)(0.0005 * kAnalysisRate), 2);

			if (centre >= magnitude.size())
				continue;

			size_t first = centre > half ? centre - half : 1;
			size_t last = std::min(centre + half, magnitude.size() - 1);

			float peak = 0.0f;
			size_t peakQ = first;
			for (size_t q = first; q <= last; q++)
			{
				if (magnitude[q] > peak)
				{
					peak = magnitude[q];
					peakQ = q;
				}
			}

			fit.MeasuredQuefrency[p] = (float)peakQ / (float)kAnalysisRate;

			std::vector<float> around;
			size_t floorHalf = half * 12;
			size_t floorFirst = centre > floorHalf ? centre - floorHalf : blank;
			size_t floorLast = std::min(centre + floorHalf, magnitude.size() - 1);

			for (size_t q = floorFirst; q <= floorLast; q++)
			{
				if (q < first || q > last)
					around.push_back(magnitude[q]);
			}

			if (around.empty())
				continue;

			std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
			float floorLevel = around[around.size() / 2];

			if (floorLevel > 1e-20f && peak > 1e-20f)
				fit.Score[p] = 20.0f * std::log10(peak / floorLevel);
		}

		fit.Usable = true;
		return fit;
	}

}
