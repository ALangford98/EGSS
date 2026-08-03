#include "egsspch.h"
#include "Egss/Renderer/Renderer.h"

namespace Egss {

	Renderer::SceneData* Renderer::s_SceneData = new Renderer::SceneData;

	void Renderer::Init()
	{
		RenderCommand::Init();
		Renderer2D::Init();
	}

	void Renderer::Shutdown()
	{
		Renderer2D::Shutdown();
	}

	void Renderer::OnWindowResize(unsigned int width, unsigned int height)
	{
		RenderCommand::SetViewport(0, 0, width, height);
	}

	void Renderer::BeginScene(const Camera& camera)
	{
		s_SceneData->ViewProjectionMatrix = camera.GetViewProjectionMatrix();
	}

	void Renderer::EndScene()
	{
	}

	void Renderer::Submit(const std::shared_ptr<Shader>& shader,
		const std::shared_ptr<VertexArray>& vertexArray,
		const glm::mat4& transform)
	{
		shader->Bind();
		shader->SetMat4("u_ViewProjection", s_SceneData->ViewProjectionMatrix);
		shader->SetMat4("u_Transform", transform);

		vertexArray->Bind();
		RenderCommand::DrawIndexed(vertexArray);
	}

	void Renderer::Submit(const std::shared_ptr<Shader>& shader,
		const std::shared_ptr<Mesh>& mesh, const glm::mat4& transform)
	{
		if (!mesh)
			return;

		Submit(shader, mesh->GetVertexArray(), transform);
	}

	void Renderer::Submit(const std::shared_ptr<Material>& material,
		const std::shared_ptr<VertexArray>& vertexArray, const glm::mat4& transform)
	{
		if (!material)
			return;

		// Bind uploads the material's own values; the two below are the
		// renderer's, and go on afterwards so a material cannot accidentally
		// shadow the transform it is being drawn with.
		material->Bind();

		const std::shared_ptr<Shader>& shader = material->GetShader();
		shader->SetMat4("u_ViewProjection", s_SceneData->ViewProjectionMatrix);
		shader->SetMat4("u_Transform", transform);

		vertexArray->Bind();
		RenderCommand::DrawIndexed(vertexArray);
	}

	void Renderer::Submit(const std::shared_ptr<Material>& material,
		const std::shared_ptr<Mesh>& mesh, const glm::mat4& transform)
	{
		if (!mesh)
			return;

		Submit(material, mesh->GetVertexArray(), transform);
	}

	void Renderer::SubmitSubmesh(const std::shared_ptr<Material>& material,
		const std::shared_ptr<Mesh>& mesh, unsigned int submesh, const glm::mat4& transform)
	{
		if (!material || !mesh)
			return;

		const std::vector<Submesh>& submeshes = mesh->GetSubmeshes();
		if (submesh >= submeshes.size())
			return;

		material->Bind();

		const std::shared_ptr<Shader>& shader = material->GetShader();
		shader->SetMat4("u_ViewProjection", s_SceneData->ViewProjectionMatrix);
		shader->SetMat4("u_Transform", transform);

		const std::shared_ptr<VertexArray>& vertexArray = mesh->GetVertexArray();
		vertexArray->Bind();
		RenderCommand::DrawIndexed(vertexArray, submeshes[submesh].IndexCount,
			submeshes[submesh].FirstIndex);
	}

	ShaderLibrary& Renderer::GetShaderLibrary()
	{
		// Function-local, so it is constructed on first use rather than in
		// whatever order the translation units happen to initialise in.
		static ShaderLibrary library;
		return library;
	}

}
