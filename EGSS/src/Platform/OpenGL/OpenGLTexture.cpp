#include "egsspch.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#include "Egss/Log.h"

#include <glad/glad.h>
#include <stb_image.h>

namespace Egss {

	namespace {

		// How many levels `glTexStorage2D` needs for a full mip chain down to
		// 1x1. Immutable storage takes the level count up front, so this has
		// to be known before the first texel is uploaded.
		int MipLevelCount(int width, int height)
		{
			int levels = 1;

			for (int size = std::max(width, height); size > 1; size /= 2)
				levels++;

			return levels;
		}

	}

	Texture2D* Texture2D::Create(const std::string& path)
	{
		return new OpenGLTexture2D(path);
	}

	Texture2D* Texture2D::CreateFromHandle(unsigned int handle, unsigned int width, unsigned int height)
	{
		return new OpenGLTexture2D(handle, width, height);
	}

	Texture2D* Texture2D::CreateFromMemory(const unsigned char* data, unsigned int size, const std::string& name)
	{
		return new OpenGLTexture2D(data, size, name);
	}

	Texture2D* Texture2D::Create(unsigned int width, unsigned int height)
	{
		return new OpenGLTexture2D(width, height);
	}

	OpenGLTexture2D::OpenGLTexture2D(unsigned int width, unsigned int height)
		: m_Width(width), m_Height(height)
	{
		m_InternalFormat = GL_RGBA8;
		m_DataFormat = GL_RGBA;

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexStorage2D(GL_TEXTURE_2D, MipLevelCount((int)m_Width, (int)m_Height),
			m_InternalFormat, m_Width, m_Height);

		// Mip-mapped on the minifying side: a texture viewed from far enough
		// away that many texels land on one pixel -- an equirectangular map
		// wrapped around a distant sphere, worst at the poles where texels
		// bunch up -- needs the area average a mip chain gives, not a 2x2
		// bilinear sample of whichever few texels the UV happened to land on.
		// Plain `GL_LINEAR` only interpolates within one level and is exactly
		// as aliased as `GL_NEAREST` once the minification ratio is high
		// enough that neighbouring pixels sample unrelated texels -- which
		// read as fine, uncorrelated speckle. See `SetSmooth` for the other
		// half of this.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
		: m_Path(path)
	{
		int width, height, channels;
		// OpenGL's texture origin is bottom-left; image files are top-left.
		stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 0);

		if (!data)
		{
			EGSS_CORE_ERROR("Could not load image '{0}': {1}", path, stbi_failure_reason());
			return;
		}

		UploadDecoded(data, width, height, channels, path);
	}

	OpenGLTexture2D::OpenGLTexture2D(const unsigned char* memory, unsigned int size, const std::string& name)
	{
		int width, height, channels;
		stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = stbi_load_from_memory(memory, (int)size, &width, &height, &channels, 0);

		if (!data)
		{
			EGSS_CORE_ERROR("Could not decode image '{0}': {1}", name, stbi_failure_reason());
			return;
		}

		UploadDecoded(data, width, height, channels, name);
	}

	// Takes ownership of `pixels` -- both callers hand it stb_image's buffer
	// and neither needs it again once it is on the GPU.
	void OpenGLTexture2D::UploadDecoded(unsigned char* pixels, int width, int height,
		int channels, const std::string& name)
	{
		m_Width = width;
		m_Height = height;

		if (channels == 4)
		{
			m_InternalFormat = GL_RGBA8;
			m_DataFormat = GL_RGBA;
		}
		else if (channels == 3)
		{
			m_InternalFormat = GL_RGB8;
			m_DataFormat = GL_RGB;
		}
		else
		{
			EGSS_CORE_ERROR("Unsupported channel count {0} in '{1}'", channels, name);
			stbi_image_free(pixels);
			return;
		}

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexStorage2D(GL_TEXTURE_2D, MipLevelCount((int)m_Width, (int)m_Height),
			m_InternalFormat, m_Width, m_Height);

		// See the note in the (width, height) constructor on why minifying
		// needs the mip chain, not just a smoother sample within one level.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		// stb_image packs rows tightly; the default unpack alignment of 4
		// reads past the end of each row of a 3-channel image whose width is
		// not a multiple of 4, which shears the image diagonally.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, pixels);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glGenerateMipmap(GL_TEXTURE_2D);

		stbi_image_free(pixels);
	}

	// Borrowed: the handle belongs to whoever created it -- a framebuffer,
	// usually -- so this only records it.
	OpenGLTexture2D::OpenGLTexture2D(unsigned int handle, unsigned int width, unsigned int height)
		: m_Width(width), m_Height(height), m_RendererID(handle), m_Borrowed(true)
	{
	}

	OpenGLTexture2D::~OpenGLTexture2D()
	{
		// Deleting a handle we do not own would pull the texture out from
		// under whatever does.
		if (!m_Borrowed)
			glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTexture2D::SetSmooth(bool smooth)
	{
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			smooth ? GL_LINEAR : GL_NEAREST);
		// The minifying side keeps its mip chain either way -- a texture
		// asking for the blocky look up close still wants the correct sample
		// frequency far away, just without blending across mip levels.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
	}

	void OpenGLTexture2D::SetData(void* data, unsigned int size)
	{
		unsigned int bytesPerPixel = (m_DataFormat == GL_RGBA) ? 4 : 3;
		EGSS_CORE_ASSERT(size == m_Width * m_Height * bytesPerPixel, "SetData must cover the entire texture");

		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	void OpenGLTexture2D::Bind(unsigned int slot) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
	}

}
