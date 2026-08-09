#pragma once

#include "Egss/Renderer/RendererAPI.h"

namespace Egss {

	class OpenGLRendererAPI : public RendererAPI
	{
	public:
		void Init() override;
		void SetViewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) override;
		void SetClearColor(const glm::vec4& color) override;
		void Clear() override;

		void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray,
			unsigned int indexCount = 0, unsigned int firstIndex = 0) override;
		void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount) override;
		void DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount) override;

		void SetLineWidth(float width) override;
		void SetBlendMode(BlendMode mode) override;
		void SetDepthTest(bool enabled) override;
		void SetBackfaceCulling(bool enabled) override;

		void ReadPixels(unsigned int x, unsigned int y, unsigned int width,
			unsigned int height, unsigned char* out) override;

		unsigned int GetMaxTextureSlots() const override { return m_MaxTextureSlots; }
	private:
		// Queried once in Init rather than per call: glGet round-trips to the
		// driver, and this cannot change for the life of the context.
		unsigned int m_MaxTextureSlots = 16;
	};

}
