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

		// **An arbitrary projection, for the cases the lens parameters cannot
		// describe.**
		//
		// Everything a `PerspectiveCamera` builds from a field of view has its
		// near plane square to the view direction, and there is one job that
		// needs it *not* to be: rendering through a portal, where the near
		// plane has to lie in the plane of the doorway so that whatever is
		// between the far camera and the doorway is never drawn. That is an
		// oblique frustum, and no combination of fov, aspect and clip
		// distances produces one.
		//
		// Whatever set the projection from a lens is free to overwrite this --
		// `SetAspectRatio` will -- so set it last.
		void SetProjectionMatrix(const glm::mat4& projection)
		{
			m_ProjectionMatrix = projection;

			RecalculateViewProjection();
		}
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
