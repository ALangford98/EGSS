#pragma once

#include "Egss/Renderer/RenderCommand.h"
#include "Egss/Renderer/Camera.h"
#include "Egss/Renderer/OrthographicCamera.h"
#include "Egss/Renderer/PerspectiveCamera.h"
#include "Egss/Renderer/Shader.h"
#include "Egss/Renderer/Renderer2D.h"
#include "Egss/Renderer/Mesh.h"

namespace Egss {

	class EGSS_API Renderer
	{
	public:
		static void Init();
		static void Shutdown();
		static void OnWindowResize(unsigned int width, unsigned int height);

		// Takes any Camera -- orthographic or perspective. All the renderer
		// needs from one is its view-projection matrix.
		static void BeginScene(const Camera& camera);
		static void EndScene();

		static void Submit(const std::shared_ptr<Shader>& shader,
			const std::shared_ptr<VertexArray>& vertexArray,
			const glm::mat4& transform = glm::mat4(1.0f));

		// Convenience over the above -- a Mesh is a vertex array plus the
		// numbers describing it, and only the vertex array is needed to draw.
		static void Submit(const std::shared_ptr<Shader>& shader,
			const std::shared_ptr<Mesh>& mesh,
			const glm::mat4& transform = glm::mat4(1.0f));

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
	private:
		// Per-scene state captured at BeginScene. A real renderer would batch
		// submissions here rather than drawing immediately.
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static SceneData* s_SceneData;
	};

}
