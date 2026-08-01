#pragma once

#include "Egss/Renderer/RendererAPI.h"

namespace Egss {

	// Thin static forwarder to the active RendererAPI, so call sites don't
	// carry a pointer around.
	class EGSS_API RenderCommand
	{
	public:
		inline static void Init()
		{
			s_RendererAPI->Init();
		}

		inline static void SetViewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		inline static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		inline static void Clear()
		{
			s_RendererAPI->Clear();
		}

		// indexCount of 0 means "the whole index buffer".
		inline static void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray, unsigned int indexCount = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount);
		}

		inline static void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		inline static void SetLineWidth(float width)
		{
			s_RendererAPI->SetLineWidth(width);
		}
	private:
		static RendererAPI* s_RendererAPI;
	};

}
