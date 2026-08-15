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

		// GL_MAX_TEXTURE_IMAGE_UNITS, not GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS.
		// The combined figure sums every stage, so on a driver reporting 16 per
		// stage it comes back as 80 -- and a fragment shader declaring 80
		// samplers would fail to link on exactly the hardware the query was
		// meant to protect.
		GLint units = 0;
		glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);

		// Floor: GL 3.3 guarantees 16, so a smaller answer means the query
		// failed rather than that the hardware is small. Ceiling: past 32 the
		// generated sampler switch and the uniform array grow for nothing --
		// a batch that needs more than 32 distinct textures is rare enough
		// that the extra flush costs less than the shader does.
		m_MaxTextureSlots = (unsigned int)std::clamp(units, 16, 32);

		EGSS_CORE_INFO("GL: {0} fragment texture units reported, using {1}",
			units, m_MaxTextureSlots);
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

	void OpenGLRendererAPI::DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray,
		unsigned int indexCount, unsigned int firstIndex)
	{
		unsigned int count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();

		// The last argument is a *byte* offset into the bound index buffer,
		// cast to a pointer for historical reasons -- it has not been an actual
		// pointer since buffer objects arrived. Indices here are 32-bit, hence
		// the multiply.
		const void* offset = (const void*)(uintptr_t)(firstIndex * sizeof(unsigned int));
		glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, offset);

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

	void OpenGLRendererAPI::SetDepthTest(bool enabled)
	{
		if (enabled)
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);
	}

	void OpenGLRendererAPI::SetDepthWrite(bool enabled)
	{
		glDepthMask(enabled ? GL_TRUE : GL_FALSE);
	}

	void OpenGLRendererAPI::SetPolygonMode(PolygonMode mode)
	{
		// GL_FRONT_AND_BACK is the only face argument the core profile accepts;
		// asking for one side is a deprecated form that errors rather than
		// doing half the job.
		GLenum which = GL_FILL;

		switch (mode)
		{
		case PolygonMode::Line:  which = GL_LINE;  break;
		case PolygonMode::Point: which = GL_POINT; break;
		default: break;
		}

		glPolygonMode(GL_FRONT_AND_BACK, which);
	}

	void OpenGLRendererAPI::SetPointSize(float size)
	{
		// **Program point size stays off.** Enabling it hands the size to the
		// vertex shader's `gl_PointSize`, and a shader that never writes one
		// leaves it undefined -- which came out as points too small to see, and
		// looked like the points were not being drawn at all.
		glDisable(GL_PROGRAM_POINT_SIZE);
		glPointSize(size);
	}

	void OpenGLRendererAPI::SetCullFace(CullFace face)
	{
		if (face == CullFace::None)
		{
			glDisable(GL_CULL_FACE);
			return;
		}

		glEnable(GL_CULL_FACE);

		// GL's default: counter-clockwise is the front. Every primitive and
		// every asset agrees with it, so nothing needs glFrontFace.
		//
		// That claim used to be here untested, and was false: CreateSphere
		// and three of the four models were wound inside-out. Culling then
		// removes the near surface and shows you the far one's interior. It
		// has since been checked per triangle, which is the only way this is
		// worth asserting -- see the changelog entry for how.
		glCullFace(face == CullFace::Front ? GL_FRONT : GL_BACK);
	}

	void OpenGLRendererAPI::SetBlendMode(BlendMode mode)
	{
		switch (mode)
		{
			case BlendMode::Alpha:
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				return;

			case BlendMode::Additive:
				// Source added straight onto the destination. Overlapping
				// lights accumulate instead of the nearest one winning.
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				return;

			case BlendMode::Multiply:
				// dst * src, with the destination contributing nothing of its
				// own beyond that. GL_ZERO for the source factor is what makes
				// it a pure product rather than a sum of one.
				glEnable(GL_BLEND);
				glBlendFunc(GL_DST_COLOR, GL_ZERO);
				return;

			case BlendMode::None:
				glDisable(GL_BLEND);
				return;
		}
	}

	void OpenGLRendererAPI::ReadPixels(unsigned int x, unsigned int y, unsigned int width,
		unsigned int height, unsigned char* out)
	{
		// GL_PACK_ALIGNMENT defaults to 4, which pads every row up to a
		// multiple of four bytes. RGBA rows already are, so this changes
		// nothing today -- but it is the difference between a correct image
		// and one sheared diagonally the moment anyone reads three channels
		// at an odd width, and that is a confusing thing to debug.
		glPixelStorei(GL_PACK_ALIGNMENT, 1);

		// Whatever is bound. For the default framebuffer the read buffer is
		// already GL_BACK, which before the swap holds the frame just drawn.
		glReadPixels((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height,
			GL_RGBA, GL_UNSIGNED_BYTE, out);
	}

	void OpenGLRendererAPI::BeginGpuTimer()
	{
		if (m_GpuQuery == 0)
			glGenQueries(1, &m_GpuQuery);

		glBeginQuery(GL_TIME_ELAPSED, m_GpuQuery);
	}

	double OpenGLRendererAPI::EndGpuTimerMs()
	{
		glEndQuery(GL_TIME_ELAPSED);

		// Blocking -- see the note on RendererAPI::EndGpuTimerMs. Correct
		// only because this is a one-off diagnostic and never runs every
		// frame; a real profiler would double-buffer two query objects and
		// read last frame's result instead of this frame's.
		GLuint64 nanoseconds = 0;
		glGetQueryObjectui64v(m_GpuQuery, GL_QUERY_RESULT, &nanoseconds);

		return (double)nanoseconds / 1.0e6;
	}

}