#pragma once

#include "Egss/Renderer/Texture.h"

namespace Egss {

	class OpenGLTexture2D : public Texture2D
	{
	public:
		OpenGLTexture2D(unsigned int width, unsigned int height);
		OpenGLTexture2D(const std::string& path);
		// Same decode-and-upload as the path constructor, for bytes that were
		// never a file -- a .glb's embedded images.
		OpenGLTexture2D(const unsigned char* data, unsigned int size, const std::string& name);
		// Borrowed: see Texture2D::CreateFromHandle.
		OpenGLTexture2D(unsigned int handle, unsigned int width, unsigned int height);
		virtual ~OpenGLTexture2D();

		unsigned int GetWidth() const override { return m_Width; }
		unsigned int GetHeight() const override { return m_Height; }

		void SetData(void* data, unsigned int size) override;
		void Bind(unsigned int slot = 0) const override;
	private:
		// Shared by the path and memory constructors: everything from decoded
		// pixels onward. Frees `pixels` itself, since both callers get theirs
		// from stb_image and there is nowhere else that needs them after.
		void UploadDecoded(unsigned char* pixels, int width, int height,
			int channels, const std::string& name);

		std::string m_Path;
		unsigned int m_Width = 0, m_Height = 0;
		unsigned int m_RendererID = 0;
		// Kept apart because SetData needs to know how many channels the
		// caller is supplying.
		unsigned int m_InternalFormat = 0, m_DataFormat = 0;

		// True when the handle came from elsewhere, in which case the
		// destructor must leave it alone.
		bool m_Borrowed = false;
	};

}
