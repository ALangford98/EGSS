#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	// Decoded PCM held in memory: interleaved 32-bit float, already converted
	// to the engine's sample rate at load time.
	//
	// Converting on load rather than during mixing is the whole trade: it
	// costs memory and a moment at startup, and in exchange the mixer never
	// has to resample, which is the part that runs on the audio thread where
	// there is no time to spare.
	//
	// Long music should stream rather than live here; that is a separate
	// class when it is needed.
	class EGSS_API AudioClip
	{
	public:
		// Any format miniaudio can decode -- wav, mp3, flac, ogg.
		static std::shared_ptr<AudioClip> Create(const std::string& path);

		// For procedurally generated audio. Samples are interleaved and
		// assumed to already be at the engine's sample rate.
		static std::shared_ptr<AudioClip> CreateFromSamples(std::vector<float> interleaved,
			unsigned int channels);

		const float* GetSamples() const { return m_Samples.data(); }
		unsigned int GetChannels() const { return m_Channels; }
		unsigned int GetFrameCount() const { return m_FrameCount; }
		float GetDurationSeconds() const;
	private:
		std::vector<float> m_Samples;
		unsigned int m_Channels = 2;
		unsigned int m_FrameCount = 0;
	};

}
