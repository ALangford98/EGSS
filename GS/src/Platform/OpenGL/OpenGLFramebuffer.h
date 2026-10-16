#pragma once

#include "GS/Renderer/Framebuffer.h"
#include "GS/Log.h"

namespace GS {

	class OpenGLFramebuffer : public Framebuffer
	{
	public:
		OpenGLFramebuffer(const FramebufferSpecification& spec);
		virtual ~OpenGLFramebuffer();

		void Bind() override;
		void Unbind() override;

		void Resize(unsigned int width, unsigned int height) override;
		void SetSamples(unsigned int samples) override;

		int ReadPixel(unsigned int attachmentIndex, int x, int y) override;
		glm::vec4 ReadPixelRGBA(unsigned int attachmentIndex, int x, int y) override;
		void ClearAttachment(unsigned int attachmentIndex, int value) override;

		unsigned int GetColorAttachmentRendererID(unsigned int index = 0) const override
		{
			GS_CORE_ASSERT(index < m_ColorAttachments.size(), "Colour attachment index out of range");
			return m_ColorAttachments[index];
		}

		unsigned int GetDepthAttachmentRendererID() const override { return m_DepthAttachment; }

		const FramebufferSpecification& GetSpecification() const override { return m_Specification; }
	private:
		// Tears down any existing attachments and builds them from the current
		// specification. Used by both the constructor and Resize.
		void Invalidate();
		void Release();
		void BuildMultisampled();

		// Multisampled buffers -> the attachment textures. Leaves the
		// framebuffer bindings as it found them.
		void Resolve();
	private:
		FramebufferSpecification m_Specification;

		// Split out of the specification once, so Invalidate doesn't have to
		// re-classify formats every time it runs.
		std::vector<FramebufferTextureFormat> m_ColorAttachmentSpecs;
		FramebufferTextureFormat m_DepthAttachmentSpec = FramebufferTextureFormat::None;

		unsigned int m_RendererID = 0;
		std::vector<unsigned int> m_ColorAttachments;
		unsigned int m_DepthAttachment = 0;

		// What was asked for; m_Specification.Samples is what was built.
		unsigned int m_RequestedSamples = 1;

		// With Samples > 1: where drawing goes. Renderbuffers, because
		// nothing samples a multisampled image directly -- it is only ever
		// resolved into the textures above.
		unsigned int m_MultisampleID = 0;
		std::vector<unsigned int> m_MultisampleColor;
		unsigned int m_MultisampleDepth = 0;
	};

}
