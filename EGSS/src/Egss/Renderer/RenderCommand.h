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

		// The per-frame baseline -- see RendererAPI::ResetState.
		inline static void ResetState()
		{
			s_RendererAPI->ResetState();
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

		// indexCount of 0 means "the whole index buffer". firstIndex offsets
		// into it, for drawing one submesh out of a shared buffer.
		inline static void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray,
			unsigned int indexCount = 0, unsigned int firstIndex = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount, firstIndex);
		}

		inline static void DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		inline static void DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount)
		{
			s_RendererAPI->DrawTriangles(vertexArray, vertexCount);
		}

		inline static void SetLineWidth(float width)
		{
			s_RendererAPI->SetLineWidth(width);
		}

		inline static void SetBlendMode(BlendMode mode)
		{
			s_RendererAPI->SetBlendMode(mode);
		}

		inline static void SetDepthTest(bool enabled)
		{
			s_RendererAPI->SetDepthTest(enabled);
		}

		inline static void SetDepthWrite(bool enabled)
		{
			s_RendererAPI->SetDepthWrite(enabled);
		}

		inline static void SetPolygonMode(PolygonMode mode)
		{
			s_RendererAPI->SetPolygonMode(mode);
		}

		inline static void SetPointSize(float size)
		{
			s_RendererAPI->SetPointSize(size);
		}

		inline static void SetCullFace(CullFace face)
		{
			s_RendererAPI->SetCullFace(face);
		}

		inline static void BeginGpuTimer()
		{
			s_RendererAPI->BeginGpuTimer();
		}

		// Blocks -- see the note on RendererAPI::EndGpuTimerMs before using
		// this anywhere that runs every frame.
		inline static double EndGpuTimerMs()
		{
			return s_RendererAPI->EndGpuTimerMs();
		}

		// Bottom-up RGBA8 rows from the bound framebuffer. See RendererAPI.
		inline static void ReadPixels(unsigned int x, unsigned int y, unsigned int width,
			unsigned int height, unsigned char* out)
		{
			s_RendererAPI->ReadPixels(x, y, width, height, out);
		}

		// Valid only after Init. See RendererAPI.
		inline static unsigned int GetMaxTextureSlots()
		{
			return s_RendererAPI->GetMaxTextureSlots();
		}
	private:
		static RendererAPI* s_RendererAPI;
	};

}
