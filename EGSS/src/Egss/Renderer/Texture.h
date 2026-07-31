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

		// Blank texture for uploading pixel data directly, used for solid
		// colours and font atlases.
		static Texture2D* Create(unsigned int width, unsigned int height);

		virtual void SetData(void* data, unsigned int size) = 0;
	};

}
