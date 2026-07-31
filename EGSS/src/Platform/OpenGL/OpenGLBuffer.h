#pragma once

#include "Egss/Renderer/Buffer.h"

namespace Egss {

	class OpenGLVertexBuffer : public VertexBuffer
	{
	public:
		OpenGLVertexBuffer(float* vertices, unsigned int size);
		OpenGLVertexBuffer(unsigned int size);
		virtual ~OpenGLVertexBuffer();

		void Bind() const override;
		void Unbind() const override;
		void SetData(const void* data, unsigned int size) override;

		const BufferLayout& GetLayout() const override { return m_Layout; }
		void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
	private:
		unsigned int m_RendererID;
		BufferLayout m_Layout;
	};

	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		OpenGLIndexBuffer(unsigned int* indices, unsigned int count);
		virtual ~OpenGLIndexBuffer();

		void Bind() const override;
		void Unbind() const override;

		unsigned int GetCount() const override { return m_Count; }
	private:
		unsigned int m_RendererID;
		unsigned int m_Count;
	};

}
