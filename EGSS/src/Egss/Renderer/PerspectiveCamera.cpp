#include "egsspch.h"
#include "Egss/Renderer/PerspectiveCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Egss {

	// Just short of straight up/down. At exactly 90 the forward vector becomes
	// parallel to the world up axis and the view matrix has no valid right
	// vector, which shows up as the view snapping round.
	static const float s_MaxPitch = 89.0f;

	PerspectiveCamera::PerspectiveCamera(float fovDegrees, float aspectRatio, float nearClip, float farClip)
	{
		SetProjection(fovDegrees, aspectRatio, nearClip, farClip);
		RecalculateViewMatrix();
	}

	void PerspectiveCamera::SetProjection(float fovDegrees, float aspectRatio, float nearClip, float farClip)
	{
		m_Fov = fovDegrees;
		m_AspectRatio = aspectRatio;
		m_NearClip = nearClip;
		m_FarClip = farClip;

		m_ProjectionMatrix = glm::perspective(glm::radians(m_Fov), m_AspectRatio, m_NearClip, m_FarClip);
		RecalculateViewProjection();
	}

	void PerspectiveCamera::SetAspectRatio(float aspectRatio)
	{
		SetProjection(m_Fov, aspectRatio, m_NearClip, m_FarClip);
	}

	void PerspectiveCamera::SetRotation(float yawDegrees, float pitchDegrees)
	{
		m_Yaw = yawDegrees;
		m_Pitch = std::max(-s_MaxPitch, std::min(s_MaxPitch, pitchDegrees));
		RecalculateViewMatrix();
	}

	glm::vec3 PerspectiveCamera::GetForward() const
	{
		float yaw = glm::radians(m_Yaw);
		float pitch = glm::radians(m_Pitch);

		return glm::normalize(glm::vec3(
			std::cos(yaw) * std::cos(pitch),
			std::sin(pitch),
			std::sin(yaw) * std::cos(pitch)));
	}

	glm::vec3 PerspectiveCamera::GetRight() const
	{
		// Crossed against world up, not the camera's own up, so strafing stays
		// horizontal however far the camera is pitched.
		return glm::normalize(glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
	}

	glm::vec3 PerspectiveCamera::GetUp() const
	{
		return glm::normalize(glm::cross(GetRight(), GetForward()));
	}

	void PerspectiveCamera::RecalculateViewMatrix()
	{
		// glm::lookAt builds the inverse camera transform directly, which is
		// what the view matrix is.
		m_ViewMatrix = glm::lookAt(m_Position, m_Position + GetForward(), glm::vec3(0.0f, 1.0f, 0.0f));
		RecalculateViewProjection();
	}

}
