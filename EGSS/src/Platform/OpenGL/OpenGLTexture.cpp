#include "egsspch.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#include "Egss/Log.h"

#include <glad/glad.h>
#include <stb_image.h>

namespace Egss {

	Texture2D* Texture2D::Create(const std::string& path)
	{
		return new OpenGLTexture2D(path);
	}

	Texture2D* Texture2D::CreateFromHandle(unsigned int handle, unsigned int width, unsigned int height)
	{
		return new OpenGLTexture2D(handle, width, height);
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
		glTexStorage2D(GL_TEXTURE_2D, 1, m_InternalFormat, m_Width, m_Height);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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
			EGSS_CORE_ERROR("Unsupported channel count {0} in '{1}'", channels, path);
			stbi_image_free(data);
			return;
		}

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexStorage2D(GL_TEXTURE_2D, 1, m_InternalFormat, m_Width, m_Height);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);

		stbi_image_free(data);
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

	void OpenGLTexture2D::SetData(void* data, unsigned int size)
	{
		unsigned int bytesPerPixel = (m_DataFormat == GL_RGBA) ? 4 : 3;
		EGSS_CORE_ASSERT(size == m_Width * m_Height * bytesPerPixel, "SetData must cover the entire texture");

		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	void OpenGLTexture2D::Bind(unsigned int slot) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
	}

}
