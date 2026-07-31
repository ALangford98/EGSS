#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace Egss {

	// The set of operations any graphics backend must provide. Keeping this
	// narrow is what makes a second backend possible later.
	class EGSS_API RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1
		};
	public:
		virtual ~RendererAPI() = default;

		virtual void Init() = 0;
		virtual void SetViewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		virtual void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray, unsigned int indexCount = 0) = 0;

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}
