#include "egsspch.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

#include <glad/glad.h>

namespace Egss {

	VertexArray* VertexArray::Create()
	{
		return new OpenGLVertexArray();
	}

	static GLenum ShaderDataTypeToOpenGLBaseType(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:   return GL_FLOAT;
			case ShaderDataType::Float2:  return GL_FLOAT;
			case ShaderDataType::Float3:  return GL_FLOAT;
			case ShaderDataType::Float4:  return GL_FLOAT;
			case ShaderDataType::Mat3:    return GL_FLOAT;
			case ShaderDataType::Mat4:    return GL_FLOAT;
			case ShaderDataType::Int:     return GL_INT;
			case ShaderDataType::Int2:    return GL_INT;
			case ShaderDataType::Int3:    return GL_INT;
			case ShaderDataType::Int4:    return GL_INT;
			case ShaderDataType::Bool:    return GL_BOOL;
			case ShaderDataType::None:    break;
		}

		EGSS_CORE_ASSERT(false, "Unknown ShaderDataType");
		return 0;
	}

	OpenGLVertexArray::OpenGLVertexArray()
	{
		glGenVertexArrays(1, &m_RendererID);
	}

	OpenGLVertexArray::~OpenGLVertexArray()
	{
		glDeleteVertexArrays(1, &m_RendererID);
	}

	void OpenGLVertexArray::Bind() const
	{
		glBindVertexArray(m_RendererID);
	}

	void OpenGLVertexArray::Unbind() const
	{
		glBindVertexArray(0);
	}

	// Walks the buffer's layout and issues one attribute pointer per element,
	// which is the whole point of BufferLayout existing.
	void OpenGLVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer)
	{
		EGSS_CORE_ASSERT(vertexBuffer->GetLayout().GetElements().size(), "Vertex buffer has no layout");

		glBindVertexArray(m_RendererID);
		vertexBuffer->Bind();

		const auto& layout = vertexBuffer->GetLayout();
		for (const auto& element : layout)
		{
			// **A matrix is not one attribute, it is a column each.** GL caps
			// an attribute at four components, so `glVertexAttribPointer` with
			// sixteen is an error rather than a matrix -- `ShaderDataType::Mat4`
			// existed in the enum and could not be used. It is four
			// consecutive locations, sixteen bytes apart, and the shader
			// declares one `mat4` that spans them.
			if (element.Type == ShaderDataType::Mat3
				|| element.Type == ShaderDataType::Mat4)
			{
				int columns = element.Type == ShaderDataType::Mat4 ? 4 : 3;

				for (int column = 0; column < columns; column++)
				{
					glEnableVertexAttribArray(m_VertexBufferIndex);

					glVertexAttribPointer(m_VertexBufferIndex, columns, GL_FLOAT,
						element.Normalized ? GL_TRUE : GL_FALSE,
						layout.GetStride(),
						(const void*)(element.Offset
							+ sizeof(float) * (size_t)columns * column));

					if (layout.GetDivisor())
						glVertexAttribDivisor(m_VertexBufferIndex, layout.GetDivisor());

					m_VertexBufferIndex++;
				}

				continue;
			}

			glEnableVertexAttribArray(m_VertexBufferIndex);

			switch (element.Type)
			{
				case ShaderDataType::Int:
				case ShaderDataType::Int2:
				case ShaderDataType::Int3:
				case ShaderDataType::Int4:
				case ShaderDataType::Bool:
					// Integer attributes need the I variant. The float one
					// converts the bytes to float, which silently mangles any
					// value a shader reads back as an int.
					glVertexAttribIPointer(m_VertexBufferIndex,
						element.GetComponentCount(),
						ShaderDataTypeToOpenGLBaseType(element.Type),
						layout.GetStride(),
						(const void*)element.Offset);
					break;

				default:
					glVertexAttribPointer(m_VertexBufferIndex,
						element.GetComponentCount(),
						ShaderDataTypeToOpenGLBaseType(element.Type),
						element.Normalized ? GL_TRUE : GL_FALSE,
						layout.GetStride(),
						(const void*)element.Offset);
					break;
			}

			if (layout.GetDivisor())
				glVertexAttribDivisor(m_VertexBufferIndex, layout.GetDivisor());

			m_VertexBufferIndex++;
		}

		m_VertexBuffers.push_back(vertexBuffer);
	}

	void OpenGLVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& indexBuffer)
	{
		glBindVertexArray(m_RendererID);
		indexBuffer->Bind();

		m_IndexBuffer = indexBuffer;
	}

}
