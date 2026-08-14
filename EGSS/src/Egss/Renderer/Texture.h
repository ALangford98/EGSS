#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

namespace Egss {

	class EGSS_API Texture
	{
	public:
		virtual ~Texture() = default;

		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;

		// Binds to a texture unit; the shader's sampler uniform holds the
		// same slot number.
		virtual void Bind(unsigned int slot = 0) const = 0;
	};

	class EGSS_API Texture2D : public Texture
	{
	public:
		static Texture2D* Create(const std::string& path);

		// Decodes an image already sitting in memory rather than on disk --
		// a .glb's images are embedded in the same binary as the geometry, so
		// there is never a path to hand to Create(). `name` is only for
		// logging; it does not have to be a real path.
		static Texture2D* CreateFromMemory(const unsigned char* data,
			unsigned int size, const std::string& name);

		// Blank texture for uploading pixel data directly, used for solid
		// colours and font atlases.
		static Texture2D* Create(unsigned int width, unsigned int height);

		// Wraps a texture this class did not create -- a framebuffer
		// attachment, most usefully -- so it can be handed to anything taking
		// a Texture2D.
		//
		// **It does not own the handle.** Whatever created the texture is
		// still responsible for deleting it, and the wrapper must not outlive
		// it. That is the trade for not copying the pixels.
		static Texture2D* CreateFromHandle(unsigned int handle,
			unsigned int width, unsigned int height);

		virtual void SetData(void* data, unsigned int size) = 0;
	};

}
