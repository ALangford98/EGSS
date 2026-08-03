#pragma once

#include "Egss/Renderer/Framebuffer.h"
#include "Egss/Log.h"

namespace Egss {

	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferSpecification& spec);
		virtual ~OpenGLFramebuffer();

		void Bind() override;
		void Unbind() override;

		void Resize(unsigned int width, unsigned int height) override;

		int ReadPixel(unsigned int attachmentIndex, int x, int y) override;
		glm::vec4 ReadPixelRGBA(unsigned int attachmentIndex, int x, int y) override;
		void ClearAttachment(unsigned int attachmentIndex, int value) override;

		unsigned int GetColorAttachmentRendererID(unsigned int index = 0) const override
		{
			EGSS_CORE_ASSERT(index < m_ColorAttachments.size(), "Colour attachment index out of range");
			return m_ColorAttachments[index];
		}

		const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		// Tears down any existing attachments and builds them from the current
		// specification. Used by both the constructor and Resize.
		void Invalidate();
	private:
		FramebufferSpecification m_Specification;

		// Split out of the specification once, so Invalidate doesn't have to
		// re-classify formats every time it runs.
		std::vector<FramebufferTextureFormat> m_ColorAttachmentSpecs;
		FramebufferTextureFormat m_DepthAttachmentSpec = FramebufferTextureFormat::None;

		unsigned int m_RendererID = 0;
		std::vector<unsigned int> m_ColorAttachments;
		unsigned int m_DepthAttachment = 0;
	};

}
