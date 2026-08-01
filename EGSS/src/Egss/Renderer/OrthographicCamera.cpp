#include "egsspch.h"
#include "Egss/Renderer/OrthographicCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Egss {

	OrthographicCamera::OrthographicCamera(float left, float right, float bottom, float top)
	{
		SetProjection(left, right, bottom, top);
	}

	void OrthographicCamera::SetProjection(float left, float right, float bottom, float top)
	{
		// Depth range is -1..1 rather than 0..1: glm::ortho negates z on the
		// way to clip space, so a *higher* world z ends up nearer the viewer.
		m_ProjectionMatrix = glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
		RecalculateViewProjection();
	}

	void OrthographicCamera::RecalculateViewMatrix()
	{
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), m_Position) *
			glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0, 0, 1));

		// The view matrix is the inverse of the camera's transform.
		m_ViewMatrix = glm::inverse(transform);
		RecalculateViewProjection();
	}

}
