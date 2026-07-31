#include "egsspch.h"
#include "Platform/OpenGL/OpenGLBuffer.h"

#include <glad/glad.h>

namespace Egss {

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
		// Bound as GL_ARRAY_BUFFER rather than GL_ELEMENT_ARRAY_BUFFER: the
		// element binding is VAO state, and uploading here would clobber
		// whichever VAO happens to be bound.
		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		glBufferData(GL_ARRAY_BUFFER, count * sizeof(unsigned int), indices, GL_STATIC_DRAW);
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
