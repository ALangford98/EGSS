#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	enum class FramebufferTextureFormat
	{
		None = 0,

		// Colour
		RGBA8,
		// One 32-bit signed integer per pixel. Used to write an ID per quad so
		// the pixel under the cursor names what was drawn there.
		RED_INTEGER,

		// Depth / stencil
		DEPTH24STENCIL8
	};

	struct FramebufferSpecification
	{
		unsigned int Width = 0;
		unsigned int Height = 0;

		// In order. Colour attachments are numbered by their position here;
		// a depth format may appear at most once, in any position.
		std::vector<FramebufferTextureFormat> Attachments;
	};

	// An off-screen render target. Draw calls issued while it is bound land in
	// its attachments instead of the window, which is what makes an editor
	// viewport possible -- the scene becomes something you can hand to
	// ImGui::Image and place inside a panel, rather than something that owns
	// the whole window.
	//
	// Extra attachments make it more than a display surface: an integer
	// attachment written alongside the colour turns a mouse position into the
	// identity of whatever was rendered under it, with no CPU-side hit testing.
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

		// Reads one pixel back from an integer attachment. Synchronous, so it
		// stalls the pipeline -- fine once per frame under the cursor, not
		// something to do in a loop. Requires the framebuffer to be bound.
		virtual int ReadPixel(unsigned int attachmentIndex, int x, int y) = 0;

		// Integer attachments cannot be cleared by glClear, which only carries
		// a float colour, so they need clearing separately.
		virtual void ClearAttachment(unsigned int attachmentIndex, int value) = 0;

		// A colour attachment's texture handle, for handing to ImGui.
		virtual unsigned int GetColorAttachmentRendererID(unsigned int index = 0) const = 0;

		virtual const FramebufferSpecification& GetSpecification() const = 0;

		static Framebuffer* Create(const FramebufferSpecification& spec);
	};

}
