#include "egsspch.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

#include "Egss/Log.h"

#include <glad/glad.h>

namespace Egss {

	// Guards against a degenerate panel size producing a GL error, and against
	// a dragged window edge asking for something the driver will refuse.
	static const unsigned int s_MaxFramebufferSize = 8192;

	static bool IsDepthFormat(FramebufferTextureFormat format)
	{
		return format == FramebufferTextureFormat::DEPTH24STENCIL8;
	}

	static GLenum ToGLInternalFormat(FramebufferTextureFormat format)
	{
		switch (format)
		{
			case FramebufferTextureFormat::RGBA8:       return GL_RGBA8;
			case FramebufferTextureFormat::RED_INTEGER: return GL_R32I;
			case FramebufferTextureFormat::DEPTH24STENCIL8: return GL_DEPTH24_STENCIL8;
			case FramebufferTextureFormat::None: break;
		}

		EGSS_CORE_ASSERT(false, "Unknown FramebufferTextureFormat");
		return 0;
	}

	Framebuffer* Framebuffer::Create(const FramebufferSpecification& spec)
	{
		return new OpenGLFramebuffer(spec);
	}

	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpecification& spec)
		: m_Specification(spec)
	{
		for (auto format : spec.Attachments)
		{
			if (IsDepthFormat(format))
			{
				EGSS_CORE_ASSERT(m_DepthAttachmentSpec == FramebufferTextureFormat::None,
					"Framebuffer can have only one depth attachment");
				m_DepthAttachmentSpec = format;
			}
			else
			{
				m_ColorAttachmentSpecs.push_back(format);
			}
		}

		Invalidate();
	}

	OpenGLFramebuffer::~OpenGLFramebuffer()
	{
		glDeleteFramebuffers(1, &m_RendererID);
		glDeleteTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());
		glDeleteTextures(1, &m_DepthAttachment);
	}

	void OpenGLFramebuffer::Invalidate()
	{
		// Recreating rather than resizing in place: attachment storage is
		// immutable once allocated, so a resize means new objects anyway.
		if (m_RendererID)
		{
			glDeleteFramebuffers(1, &m_RendererID);
			glDeleteTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());
			glDeleteTextures(1, &m_DepthAttachment);

			m_ColorAttachments.clear();
			m_DepthAttachment = 0;
		}

		glGenFramebuffers(1, &m_RendererID);
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

		m_ColorAttachments.resize(m_ColorAttachmentSpecs.size());
		glGenTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());

		for (size_t i = 0; i < m_ColorAttachments.size(); i++)
		{
			FramebufferTextureFormat format = m_ColorAttachmentSpecs[i];
			bool isInteger = format == FramebufferTextureFormat::RED_INTEGER;

			glBindTexture(GL_TEXTURE_2D, m_ColorAttachments[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, ToGLInternalFormat(format),
				m_Specification.Width, m_Specification.Height, 0,
				isInteger ? GL_RED_INTEGER : GL_RGBA,
				isInteger ? GL_INT : GL_UNSIGNED_BYTE, nullptr);

			// Integer textures cannot be filtered -- asking for LINEAR makes
			// them incomplete and every sample returns black. Colour wants
			// LINEAR, because the viewport panel is rarely a 1:1 pixel match
			// while it is being dragged.
			GLenum filter = isInteger ? GL_NEAREST : GL_LINEAR;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i,
				GL_TEXTURE_2D, m_ColorAttachments[i], 0);
		}

		if (m_DepthAttachmentSpec != FramebufferTextureFormat::None)
		{
			glGenTextures(1, &m_DepthAttachment);
			glBindTexture(GL_TEXTURE_2D, m_DepthAttachment);
			glTexImage2D(GL_TEXTURE_2D, 0, ToGLInternalFormat(m_DepthAttachmentSpec),
				m_Specification.Width, m_Specification.Height, 0,
				GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				GL_TEXTURE_2D, m_DepthAttachment, 0);
		}

		// Fragment outputs are routed by draw-buffer index, so every colour
		// attachment has to be listed -- without this only attachment 0 is
		// written and the integer attachment stays empty.
		if (m_ColorAttachments.size() > 1)
		{
			EGSS_CORE_ASSERT(m_ColorAttachments.size() <= 4, "At most 4 colour attachments are supported");

			GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
								  GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
			glDrawBuffers((GLsizei)m_ColorAttachments.size(), buffers);
		}
		else if (m_ColorAttachments.empty())
		{
			// Depth-only, as a shadow map would be.
			glDrawBuffer(GL_NONE);
		}

		EGSS_CORE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
			"Framebuffer is incomplete");

		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLFramebuffer::Bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);
		// The viewport is global state, and the window's size is almost never
		// the framebuffer's, so it has to be set here rather than at creation.
		glViewport(0, 0, m_Specification.Width, m_Specification.Height);
	}

	void OpenGLFramebuffer::Unbind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLFramebuffer::Resize(unsigned int width, unsigned int height)
	{
		if (width == 0 || height == 0 || width > s_MaxFramebufferSize || height > s_MaxFramebufferSize)
		{
			EGSS_CORE_WARN("Ignoring framebuffer resize to {0}x{1}", width, height);
			return;
		}

		m_Specification.Width = width;
		m_Specification.Height = height;

		Invalidate();
	}

	int OpenGLFramebuffer::ReadPixel(unsigned int attachmentIndex, int x, int y)
	{
		EGSS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);

		int pixel = -1;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixel);
		return pixel;
	}

	glm::vec4 OpenGLFramebuffer::ReadPixelRGBA(unsigned int attachmentIndex, int x, int y)
	{
		EGSS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);

		// Read as bytes rather than floats: the attachment *is* RGBA8, so
		// asking for GL_FLOAT only makes the driver convert the same eight bits
		// per channel and hides the quantisation from the caller.
		unsigned char pixel[4] = { 0, 0, 0, 0 };
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

		return { pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f, pixel[3] / 255.0f };
	}

	void OpenGLFramebuffer::ClearAttachment(unsigned int attachmentIndex, int value)
	{
		EGSS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		// The draw-buffer index matches the attachment index because
		// Invalidate lists them in order.
		glClearBufferiv(GL_COLOR, (GLint)attachmentIndex, &value);
	}

}
