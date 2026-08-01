#include "egsspch.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Egss/Renderer/RenderCommand.h"
#include "Egss/Log.h"

#include <glad/glad.h>

namespace Egss {

	RendererAPI::API RendererAPI::s_API = RendererAPI::API::OpenGL;
	RendererAPI* RenderCommand::s_RendererAPI = new OpenGLRendererAPI();

	// Turns GL's silent failures into log lines. Only fires if the context was
	// created with GLFW_OPENGL_DEBUG_CONTEXT.
	static void OpenGLMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
		GLsizei length, const GLchar* message, const void* userParam)
	{
		switch (severity)
		{
			case GL_DEBUG_SEVERITY_HIGH:         EGSS_CORE_CRITICAL("GL: {0}", message); return;
			case GL_DEBUG_SEVERITY_MEDIUM:       EGSS_CORE_ERROR("GL: {0}", message); return;
			case GL_DEBUG_SEVERITY_LOW:          EGSS_CORE_WARN("GL: {0}", message); return;
			case GL_DEBUG_SEVERITY_NOTIFICATION: return;
		}
	}

	void OpenGLRendererAPI::Init()
	{
#ifdef EGSS_DEBUG
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(OpenGLMessageCallback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
#endif

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_DEPTH_TEST);
	}

	void OpenGLRendererAPI::SetViewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height)
	{
		glViewport(x, y, width, height);
	}

	void OpenGLRendererAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRendererAPI::Clear()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	void OpenGLRendererAPI::DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray, unsigned int indexCount)
	{
		unsigned int count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();
		glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);

		// Deliberately does not unbind the texture. It used to, which was
		// invisible for Renderer2D because every flush rebinds its slots, but
		// it silently broke any caller that binds once and then issues several
		// draws -- the second draw onwards sampled nothing and came out black.
	}

	void OpenGLRendererAPI::DrawLines(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount)
	{
		vertexArray->Bind();
		// glDrawArrays, not glDrawElements: no index buffer is involved.
		glDrawArrays(GL_LINES, 0, vertexCount);
	}

	void OpenGLRendererAPI::DrawTriangles(const std::shared_ptr<VertexArray>& vertexArray, unsigned int vertexCount)
	{
		vertexArray->Bind();
		glDrawArrays(GL_TRIANGLES, 0, vertexCount);
	}

	void OpenGLRendererAPI::SetLineWidth(float width)
	{
		glLineWidth(width);
	}

}