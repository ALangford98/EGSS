#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	struct FramebufferSpecification
	{
		unsigned int Width = 0;
		unsigned int Height = 0;
	};

	// An off-screen render target. Draw calls issued while it is bound land in
	// a texture instead of the window, which is what makes an editor viewport
	// possible -- the scene becomes something you can hand to ImGui::Image and
	// place inside a panel, rather than something that owns the whole window.
	//
	// It is also the basis for post-processing and for mouse picking, both of
	// which need the rendered result readable rather than already presented.
	class EGSS_API Framebuffer
	{
	public:
		virtual ~Framebuffer() = default;

		// Bind also sets the viewport to the framebuffer's size; Unbind only
		// restores the default target, so the caller is responsible for
		// putting the viewport back if it matters.
		virtual void Bind() = 0;
		virtual void Unbind() = 0;

		// Destroys and recreates the attachments. Cheap enough to call
		// whenever a panel is resized, but not something to do every frame.
		virtual void Resize(unsigned int width, unsigned int height) = 0;

		// The colour attachment's texture handle, for handing to ImGui.
		virtual unsigned int GetColorAttachmentRendererID() const = 0;

		virtual const FramebufferSpecification& GetSpecification() const = 0;

		static Framebuffer* Create(const FramebufferSpecification& spec);
	};

}
