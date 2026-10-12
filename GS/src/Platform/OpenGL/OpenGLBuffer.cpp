#include "gspch.h"
#include "Platform/OpenGL/OpenGLBuffer.h"

#include <glad/glad.h>

namespace GS {

	VertexBuffer* VertexBuffer::Create(float* vertices, unsigned int size)
	{
		return new OpenGLVertexBuffer(vertices, size);
	}

	VertexBuffer* VertexBuffer::Create(unsigned int size)
	{
		return new OpenGLVertexBuffer(size);
	}

	IndexBuffer* IndexBuffer::Create(unsigned int* indices, unsigned int count)
	{
		return new OpenGLIndexBuffer(indices, count);
	}

	/////////////////////////////////////////////////////////////////////////
	// VertexBuffer /////////////////////////////////////////////////////////
	/////////////////////////////////////////////////////////////////////////

	OpenGLVertexBuffer::OpenGLVertexBuffer(float* vertices, unsigned int size)
	{
		glGenBuffers(1, &m_RendererID);
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		glBufferData(GL_ARRAY_BUFFER, size, vertices, GL_STATIC_DRAW);
	}

	OpenGLVertexBuffer::OpenGLVertexBuffer(unsigned int size)
	{
		glGenBuffers(1, &m_RendererID);
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		// GL_DYNAMIC_DRAW: contents are respecified every frame.
		glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
	}

	void OpenGLVertexBuffer::SetData(const void* data, unsigned int size)
	{
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
	}

	OpenGLVertexBuffer::~OpenGLVertexBuffer()
	{
		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLVertexBuffer::Bind() const
	{
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
	}

	void OpenGLVertexBuffer::Unbind() const
	{
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	/////////////////////////////////////////////////////////////////////////
	// IndexBuffer //////////////////////////////////////////////////////////
	/////////////////////////////////////////////////////////////////////////

	OpenGLIndexBuffer::OpenGLIndexBuffer(unsigned int* indices, unsigned int count)
		: m_Count(count)
	{
		glGenBuffers(1, &m_RendererID);
#ifdef GS_PLATFORM_WEB
		// WebGL fixes a buffer's target the first time it is bound, and an
		// element buffer first bound as GL_ARRAY_BUFFER can never be bound as
		// an element buffer again -- every draw then failed with
		// INVALID_OPERATION and the frame came out black. So it is bound as
		// what it is, with no VAO bound while it is, so no VAO's element
		// binding is clobbered (the reason for the desktop path below).
		GLint vertexArray = 0;
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
		glBindVertexArray(0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(unsigned int), indices, GL_STATIC_DRAW);
		glBindVertexArray((GLuint)vertexArray);
#else
		// Bound as GL_ARRAY_BUFFER rather than GL_ELEMENT_ARRAY_BUFFER: the
		// element binding is VAO state, and uploading here would clobber
		// whichever VAO happens to be bound.
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		glBufferData(GL_ARRAY_BUFFER, count * sizeof(unsigned int), indices, GL_STATIC_DRAW);
#endif
	}

	OpenGLIndexBuffer::~OpenGLIndexBuffer()
	{
		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLIndexBuffer::Bind() const
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
	}

	void OpenGLIndexBuffer::Unbind() const
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

}
