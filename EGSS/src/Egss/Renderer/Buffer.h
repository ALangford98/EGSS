#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Log.h"

namespace Egss {

	enum class ShaderDataType
	{
		None = 0,
		Float, Float2, Float3, Float4,
		Mat3, Mat4,
		Int, Int2, Int3, Int4,
		Bool
	};

	static unsigned int ShaderDataTypeSize(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:   return 4;
			case ShaderDataType::Float2:  return 4 * 2;
			case ShaderDataType::Float3:  return 4 * 3;
			case ShaderDataType::Float4:  return 4 * 4;
			case ShaderDataType::Mat3:    return 4 * 3 * 3;
			case ShaderDataType::Mat4:    return 4 * 4 * 4;
			case ShaderDataType::Int:     return 4;
			case ShaderDataType::Int2:    return 4 * 2;
			case ShaderDataType::Int3:    return 4 * 3;
			case ShaderDataType::Int4:    return 4 * 4;
			case ShaderDataType::Bool:    return 1;
			case ShaderDataType::None:    break;
		}

		EGSS_CORE_ASSERT(false, "Unknown ShaderDataType");
		return 0;
	}

	// One attribute in a vertex. Offset and stride are filled in by the
	// BufferLayout that owns it, so the declaration site only names the type.
	struct BufferElement
	{
		std::string Name;
		ShaderDataType Type;
		unsigned int Size;
		size_t Offset;
		bool Normalized;

		BufferElement() = default;

		BufferElement(ShaderDataType type, const std::string& name, bool normalized = false)
			: Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized)
		{
		}

		unsigned int GetComponentCount() const
		{
			switch (Type)
			{
				case ShaderDataType::Float:   return 1;
				case ShaderDataType::Float2:  return 2;
				case ShaderDataType::Float3:  return 3;
				case ShaderDataType::Float4:  return 4;
				case ShaderDataType::Mat3:    return 3 * 3;
				case ShaderDataType::Mat4:    return 4 * 4;
				case ShaderDataType::Int:     return 1;
				case ShaderDataType::Int2:    return 2;
				case ShaderDataType::Int3:    return 3;
				case ShaderDataType::Int4:    return 4;
				case ShaderDataType::Bool:    return 1;
				case ShaderDataType::None:    break;
			}

			EGSS_CORE_ASSERT(false, "Unknown ShaderDataType");
			return 0;
		}
	};

	// Describes a vertex buffer's contents, replacing hand-written
	// glVertexAttribPointer calls with a declaration.
	class EGSS_API BufferLayout
	{
	public:
		BufferLayout() {}

		// **`divisor` makes the whole buffer per-instance.** Zero -- the
		// default -- advances an attribute once per vertex, which is what a
		// vertex buffer is. One advances it once per *instance*, which is what
		// turns three thousand draw calls into one: the mesh is bound once and
		// the buffer supplies whatever differs between copies of it.
		//
		// It is a property of the layout rather than of each element because a
		// buffer is one or the other. Mixing the two in one buffer is legal in
		// GL and has never once been what somebody meant.
		BufferLayout(const std::initializer_list<BufferElement>& elements,
			unsigned int divisor = 0)
			: m_Elements(elements), m_Divisor(divisor)
		{
			CalculateOffsetsAndStride();
		}

		inline unsigned int GetStride() const { return m_Stride; }
		inline unsigned int GetDivisor() const { return m_Divisor; }
		inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		void CalculateOffsetsAndStride()
		{
			size_t offset = 0;
			m_Stride = 0;
			for (auto& element : m_Elements)
			{
				element.Offset = offset;
				offset += element.Size;
				m_Stride += element.Size;
			}
		}
	private:
		std::vector<BufferElement> m_Elements;
		unsigned int m_Stride = 0;
		unsigned int m_Divisor = 0;
	};

	class EGSS_API VertexBuffer
	{
	public:
		virtual ~VertexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual const BufferLayout& GetLayout() const = 0;
		virtual void SetLayout(const BufferLayout& layout) = 0;

		// Re-uploads vertex data each frame; batching needs this.
		virtual void SetData(const void* data, unsigned int size) = 0;

		static VertexBuffer* Create(float* vertices, unsigned int size);

		// Allocates without initialising, for buffers filled via SetData.
		static VertexBuffer* Create(unsigned int size);
	};

	class EGSS_API IndexBuffer
	{
	public:
		virtual ~IndexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual unsigned int GetCount() const = 0;

		static IndexBuffer* Create(unsigned int* indices, unsigned int count);
	};

}
