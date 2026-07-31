#pragma once

#include "Egss/Renderer/Framebuffer.h"

namespace Egss {

	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferSpecification& spec);
		virtual ~OpenGLFramebuffer();

		void Bind() override;
		void Unbind() override;

		void Resize(unsigned int width, unsigned int height) override;

		unsigned int GetColorAttachmentRendererID() const override { return m_ColorAttachment; }

		const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		// Tears down any existing attachments and builds them from the current
		// specification. Used by both the constructor and Resize.
		void Invalidate();
	private:
		FramebufferSpecification m_Specification;

		unsigned int m_RendererID = 0;
		unsigned int m_ColorAttachment = 0;
		unsigned int m_DepthAttachment = 0;
	};

}
