#pragma once

#include "Egss/Core.h"
#include "Egss/Renderer/Camera.h"

#include <glm/glm.hpp>

namespace Egss {

	// 2D camera. The projection maps a rectangle of world space onto the
	// screen; the view is the inverse of the camera's own transform, which is
	// why moving the camera right shifts the world left.
	//
	// The matrices themselves live in Camera; this only owns how they are
	// built.
	class EGSS_API OrthographicCamera : public Camera
	{
	public:
		OrthographicCamera(float left, float right, float bottom, float top);

		void SetProjection(float left, float right, float bottom, float top);

		const glm::vec3& GetPosition() const { return m_Position; }
		void SetPosition(const glm::vec3& position) { m_Position = position; RecalculateViewMatrix(); }

		float GetRotation() const { return m_Rotation; }
		void SetRotation(float rotation) { m_Rotation = rotation; RecalculateViewMatrix(); }
	private:
		void RecalculateViewMatrix();
	private:
		glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
		// Degrees, about the z axis -- a 2D camera has only one axis to turn on.
		float m_Rotation = 0.0f;
	};

}
