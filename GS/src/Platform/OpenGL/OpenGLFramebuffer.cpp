#include "gspch.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

#include "GS/Log.h"

#include <glad/glad.h>

namespace GS {

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

		GS_CORE_ASSERT(false, "Unknown FramebufferTextureFormat");
		return 0;
	}

	Framebuffer* Framebuffer::Create(const FramebufferSpecification& spec)
	{
		return new OpenGLFramebuffer(spec);
	}

	static unsigned int s_ScreenRendererID = 0;

	void Framebuffer::SetScreenRendererID(unsigned int id) { s_ScreenRendererID = id; }
	unsigned int Framebuffer::GetScreenRendererID() { return s_ScreenRendererID; }

	OpenGLFramebuffer::OpenGLFramebuffer(const FramebufferSpecification& spec)
		: m_Specification(spec), m_RequestedSamples(spec.Samples)
	{
		for (auto format : spec.Attachments)
		{
			if (IsDepthFormat(format))
			{
				GS_CORE_ASSERT(m_DepthAttachmentSpec == FramebufferTextureFormat::None,
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
		Release();
	}

	void OpenGLFramebuffer::Release()
	{
		if (m_RendererID)
		{
			glDeleteFramebuffers(1, &m_RendererID);
			glDeleteTextures((GLsizei)m_ColorAttachments.size(), m_ColorAttachments.data());
			glDeleteTextures(1, &m_DepthAttachment);
		}
		if (m_MultisampleID)
		{
			glDeleteFramebuffers(1, &m_MultisampleID);
			glDeleteRenderbuffers((GLsizei)m_MultisampleColor.size(), m_MultisampleColor.data());
			glDeleteRenderbuffers(1, &m_MultisampleDepth);
		}
		m_RendererID = 0;
		m_ColorAttachments.clear();
		m_DepthAttachment = 0;
		m_MultisampleID = 0;
		m_MultisampleColor.clear();
		m_MultisampleDepth = 0;
	}

	void OpenGLFramebuffer::Invalidate()
	{
		// Recreating rather than resizing in place: attachment storage is
		// immutable once allocated, so a resize means new objects anyway.
		Release();

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
			// Read back as a shadow map, the edge must not wrap round to the
			// far side's depth.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				GL_TEXTURE_2D, m_DepthAttachment, 0);
		}

		// Fragment outputs are routed by draw-buffer index, so every colour
		// attachment has to be listed -- without this only attachment 0 is
		// written and the integer attachment stays empty.
		if (m_ColorAttachments.size() > 1)
		{
			GS_CORE_ASSERT(m_ColorAttachments.size() <= 4, "At most 4 colour attachments are supported");

			GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
								  GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
			glDrawBuffers((GLsizei)m_ColorAttachments.size(), buffers);
		}
		else if (m_ColorAttachments.empty())
		{
			// Depth-only, as a shadow map would be.
#ifdef GS_GLES
			// GLES 3 / WebGL2 has only the plural form.
			GLenum none = GL_NONE;
			glDrawBuffers(1, &none);
#else
			glDrawBuffer(GL_NONE);
#endif
		}

		GS_CORE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
			"Framebuffer is incomplete");

		glBindTexture(GL_TEXTURE_2D, 0);

		BuildMultisampled();

		glBindFramebuffer(GL_FRAMEBUFFER, s_ScreenRendererID);
	}

	// The multisampled twin of the attachments built above, when samples
	// were asked for. Every attachment of a framebuffer must have the same
	// sample count, so the integer (entity id) one is multisampled too:
	// resolving it takes one sample per pixel rather than averaging -- what
	// the GL spec does for integer formats (glBlitFramebuffer, "a single
	// sample's value is selected"), and the right thing for an id, since an
	// average of two ids is neither.
	void OpenGLFramebuffer::BuildMultisampled()
	{
		m_Specification.Samples = 1;
		if (m_RequestedSamples <= 1 || m_ColorAttachmentSpecs.empty())
			return;

		bool hasInteger = false;
		for (FramebufferTextureFormat format : m_ColorAttachmentSpecs)
			hasInteger = hasInteger || format == FramebufferTextureFormat::RED_INTEGER;

		GLint maxSamples = 1;
		glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		if (hasInteger)
		{
#ifdef GS_GLES
			// GLES 3.0 / WebGL2 cannot multisample integer formats at all.
			GS_CORE_WARN("Framebuffer: no anti-aliasing -- GLES cannot multisample the integer attachment");
			return;
#else
			GLint maxIntegerSamples = 1;
			glGetIntegerv(GL_MAX_INTEGER_SAMPLES, &maxIntegerSamples);
			maxSamples = std::min(maxSamples, maxIntegerSamples);
#endif
		}
		GLsizei samples = (GLsizei)std::min<GLint>((GLint)m_RequestedSamples, maxSamples);
		if (samples <= 1)
			return;

		glGenFramebuffers(1, &m_MultisampleID);
		glBindFramebuffer(GL_FRAMEBUFFER, m_MultisampleID);

		m_MultisampleColor.resize(m_ColorAttachmentSpecs.size());
		glGenRenderbuffers((GLsizei)m_MultisampleColor.size(), m_MultisampleColor.data());
		for (size_t i = 0; i < m_MultisampleColor.size(); i++)
		{
			glBindRenderbuffer(GL_RENDERBUFFER, m_MultisampleColor[i]);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, ToGLInternalFormat(m_ColorAttachmentSpecs[i]),
				m_Specification.Width, m_Specification.Height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_RENDERBUFFER, m_MultisampleColor[i]);
		}
		if (m_DepthAttachmentSpec != FramebufferTextureFormat::None)
		{
			glGenRenderbuffers(1, &m_MultisampleDepth);
			glBindRenderbuffer(GL_RENDERBUFFER, m_MultisampleDepth);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, ToGLInternalFormat(m_DepthAttachmentSpec),
				m_Specification.Width, m_Specification.Height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_MultisampleDepth);
		}
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		GLenum buffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
		glDrawBuffers((GLsizei)std::min<size_t>(m_MultisampleColor.size(), 4), buffers);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			// Draw single-sampled rather than not at all.
			GS_CORE_WARN("Framebuffer: {0}x multisampling incomplete on this driver; drawing without it", samples);
			glDeleteFramebuffers(1, &m_MultisampleID);
			glDeleteRenderbuffers((GLsizei)m_MultisampleColor.size(), m_MultisampleColor.data());
			glDeleteRenderbuffers(1, &m_MultisampleDepth);
			m_MultisampleID = 0;
			m_MultisampleColor.clear();
			m_MultisampleDepth = 0;
			return;
		}
		m_Specification.Samples = (unsigned int)samples;
	}

	void OpenGLFramebuffer::Resolve()
	{
		if (!m_MultisampleID)
			return;

		GLint previousRead = 0, previousDraw = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_MultisampleID);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_RendererID);

		// One attachment at a time: a blit copies from one read buffer to
		// every draw buffer, so the draw side is narrowed to the matching
		// attachment, then restored.
		size_t count = std::min<size_t>(m_ColorAttachments.size(), 4);
		for (size_t i = 0; i < count; i++)
		{
			GLenum buffers[4] = { GL_NONE, GL_NONE, GL_NONE, GL_NONE };
			buffers[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
			glReadBuffer(GL_COLOR_ATTACHMENT0 + (GLenum)i);
			glDrawBuffers((GLsizei)(i + 1), buffers);
			glBlitFramebuffer(0, 0, m_Specification.Width, m_Specification.Height,
				0, 0, m_Specification.Width, m_Specification.Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		GLenum all[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
		glDrawBuffers((GLsizei)count, all);
		glReadBuffer(GL_COLOR_ATTACHMENT0);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousRead);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previousDraw);
	}

	void OpenGLFramebuffer::Bind()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_MultisampleID ? m_MultisampleID : m_RendererID);
		// The viewport is global state, and the window's size is almost never
		// the framebuffer's, so it has to be set here rather than at creation.
		glViewport(0, 0, m_Specification.Width, m_Specification.Height);
	}

	void OpenGLFramebuffer::Unbind()
	{
		Resolve();
		glBindFramebuffer(GL_FRAMEBUFFER, s_ScreenRendererID);
	}

	void OpenGLFramebuffer::Resize(unsigned int width, unsigned int height)
	{
		if (width == 0 || height == 0 || width > s_MaxFramebufferSize || height > s_MaxFramebufferSize)
		{
			GS_CORE_WARN("Ignoring framebuffer resize to {0}x{1}", width, height);
			return;
		}

		m_Specification.Width = width;
		m_Specification.Height = height;

		Invalidate();
	}

	void OpenGLFramebuffer::SetSamples(unsigned int samples)
	{
		if (samples == m_RequestedSamples)
			return;
		m_RequestedSamples = samples;
		Invalidate();
	}

	int OpenGLFramebuffer::ReadPixel(unsigned int attachmentIndex, int x, int y)
	{
		GS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		// A multisampled buffer cannot be read with glReadPixels: the
		// resolved attachment is.
		Resolve();
		GLint previousRead = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_RendererID);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);

		int pixel = -1;
		glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &pixel);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousRead);
		return pixel;
	}

	glm::vec4 OpenGLFramebuffer::ReadPixelRGBA(unsigned int attachmentIndex, int x, int y)
	{
		GS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		Resolve();
		GLint previousRead = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_RendererID);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);

		// Read as bytes rather than floats: the attachment *is* RGBA8, so
		// asking for GL_FLOAT only makes the driver convert the same eight bits
		// per channel and hides the quantisation from the caller.
		unsigned char pixel[4] = { 0, 0, 0, 0 };
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previousRead);

		return { pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f, pixel[3] / 255.0f };
	}

	void OpenGLFramebuffer::ClearAttachment(unsigned int attachmentIndex, int value)
	{
		GS_CORE_ASSERT(attachmentIndex < m_ColorAttachments.size(), "Colour attachment index out of range");

		// The draw-buffer index matches the attachment index because
		// Invalidate lists them in order.
		glClearBufferiv(GL_COLOR, (GLint)attachmentIndex, &value);
	}

}
