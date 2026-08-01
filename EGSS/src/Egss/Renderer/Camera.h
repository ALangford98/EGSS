#pragma once

#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	// What every camera has in common: a projection, a view, and their product.
	//
	// This exists so the renderer can take "a camera" without caring whether it
	// is orthographic or perspective -- the only thing Renderer::Submit needs
	// is one matrix. Subclasses own how the two halves are built: the
	// projection from the lens (ortho bounds, or field of view), the view from
	// where the camera is and what it is looking at.
	class EGSS_API Camera
	{
	public:
		virtual ~Camera() = default;

		const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
		const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		// Cached so the shader upload is one matrix rather than two.
		const glm::mat4& GetViewProjectionMatrix() const { return m_ViewProjectionMatrix; }
	protected:
		// Subclasses call this after changing either half.
		void RecalculateViewProjection()
		{
			m_ViewProjectionMatrix = m_ProjectionMatrix * m_ViewMatrix;
		}
	protected:
		glm::mat4 m_ProjectionMatrix = glm::mat4(1.0f);
		glm::mat4 m_ViewMatrix = glm::mat4(1.0f);
		glm::mat4 m_ViewProjectionMatrix = glm::mat4(1.0f);
	};

}
