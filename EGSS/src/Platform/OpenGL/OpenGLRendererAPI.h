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

		void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray, unsigned int indexCount = 0) override;
		void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount) override;

		void SetLineWidth(float width) override;
	};

}
