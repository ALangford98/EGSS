#include "egsspch.h"
#include "Egss/Debug/ScreenCapture.h"

#include "Egss/Log.h"
#include "Egss/Renderer/RenderCommand.h"

#include <stb_image_write.h>

#include <filesystem>

namespace Egss {

	bool ScreenCapture::SaveFrame(const std::string& path, unsigned int width, unsigned int height)
	{
		if (width == 0 || height == 0)
		{
			EGSS_CORE_ERROR("ScreenCapture: refusing to capture a {0}x{1} frame", width, height);
			return false;
		}

		const size_t pixels = (size_t)width * (size_t)height;
		std::vector<unsigned char> buffer(pixels * 4);

		RenderCommand::ReadPixels(0, 0, width, height, buffer.data());

		// The GL hands rows back bottom-up and PNG wants them top-down. stb
		// will do the flip itself, which is cheaper than doing it here and
		// impossible to get subtly half-right.
		stbi_flip_vertically_on_write(1);

		// Create the directory rather than failing on a path the caller
		// reasonably expected to work. A screenshot that silently does not
		// appear is worse than one that costs a mkdir.
		std::error_code error;
		std::filesystem::path target(path);
		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), error);

		// Stride is given explicitly: it is the row length in bytes, and
		// passing 0 only works because our rows happen to be tightly packed.
		// Saying so keeps this correct if the read ever gains a sub-rectangle.
		int written = stbi_write_png(path.c_str(), (int)width, (int)height, 4,
			buffer.data(), (int)(width * 4));

		if (written == 0)
		{
			EGSS_CORE_ERROR("ScreenCapture: could not write '{0}'", path);
			return false;
		}

		EGSS_CORE_INFO("ScreenCapture: wrote '{0}' ({1}x{2})", path, width, height);
		return true;
	}

}
