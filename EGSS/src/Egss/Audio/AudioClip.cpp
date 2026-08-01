#include "egsspch.h"
#include "Egss/Audio/AudioClip.h"
#include "Egss/Audio/AudioEngine.h"

#include "Egss/Log.h"

#include "miniaudio.h"

namespace Egss {

	std::shared_ptr<AudioClip> AudioClip::Create(const std::string& path)
	{
		// Decoding straight to the engine's format means the mixer never has
		// to convert anything on the audio thread.
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, AudioEngine::GetSampleRate());

		ma_decoder decoder;
		if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS)
		{
			EGSS_CORE_ERROR("AudioClip: could not decode '{0}'", path);
			return nullptr;
		}

		ma_uint64 frameCount = 0;
		if (ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount) != MA_SUCCESS || frameCount == 0)
		{
			EGSS_CORE_ERROR("AudioClip: '{0}' has no frames", path);
			ma_decoder_uninit(&decoder);
			return nullptr;
		}

		auto clip = std::make_shared<AudioClip>();
		clip->m_Channels = 2;
		clip->m_Samples.resize((size_t)frameCount * 2);

		ma_uint64 read = 0;
		ma_decoder_read_pcm_frames(&decoder, clip->m_Samples.data(), frameCount, &read);
		ma_decoder_uninit(&decoder);

		clip->m_FrameCount = (unsigned int)read;
		clip->m_Samples.resize((size_t)read * 2);

		EGSS_CORE_INFO("AudioClip '{0}': {1} frames ({2:.2f}s)", path, read, clip->GetDurationSeconds());
		return clip;
	}

	std::shared_ptr<AudioClip> AudioClip::CreateFromSamples(std::vector<float> interleaved,
		unsigned int channels)
	{
		if (channels == 0 || interleaved.empty())
			return nullptr;

		auto clip = std::make_shared<AudioClip>();
		clip->m_Channels = channels;
		clip->m_FrameCount = (unsigned int)(interleaved.size() / channels);
		clip->m_Samples = std::move(interleaved);
		return clip;
	}

	float AudioClip::GetDurationSeconds() const
	{
		return (float)m_FrameCount / (float)AudioEngine::GetSampleRate();
	}

}
