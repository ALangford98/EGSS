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

		// Back to yaw and pitch, whatever an earlier SetOrientation said.
		m_ExplicitBasis = false;

		RecalculateViewMatrix();
	}

	void PerspectiveCamera::SetOrientation(const glm::vec3& forward, const glm::vec3& up)
	{
		glm::vec3 f = glm::normalize(forward);

		// Gram-Schmidt: the caller's up is a hint about which way is level, and
		// only its component perpendicular to the view direction can be used.
		glm::vec3 u = up - f * glm::dot(up, f);

		if (glm::dot(u, u) < 1e-8f)
		{
			// Looking straight along the up axis, where "level" is undefined.
			// Any perpendicular will do and this one is stable.
			glm::vec3 reference = std::abs(f.y) < 0.9f
				? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);

			u = reference - f * glm::dot(reference, f);
		}

		m_Forward = f;
		m_Up = glm::normalize(u);
		m_Right = glm::normalize(glm::cross(m_Forward, m_Up));
		m_ExplicitBasis = true;

		RecalculateViewMatrix();
	}

	glm::vec3 PerspectiveCamera::GetForward() const
	{
		if (m_ExplicitBasis)
			return m_Forward;

		float yaw = glm::radians(m_Yaw);
		float pitch = glm::radians(m_Pitch);

		return glm::normalize(glm::vec3(
			std::cos(yaw) * std::cos(pitch),
			std::sin(pitch),
			std::sin(yaw) * std::cos(pitch)));
	}

	glm::vec3 PerspectiveCamera::GetRight() const
	{
		if (m_ExplicitBasis)
			return m_Right;

		// Crossed against world up, not the camera's own up, so strafing stays
		// horizontal however far the camera is pitched.
		return glm::normalize(glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
	}

	glm::vec3 PerspectiveCamera::GetUp() const
	{
		if (m_ExplicitBasis)
			return m_Up;

		return glm::normalize(glm::cross(GetRight(), GetForward()));
	}

	void PerspectiveCamera::RecalculateViewMatrix()
	{
		// glm::lookAt builds the inverse camera transform directly, which is
		// what the view matrix is.
		// The up passed here is what decides which way is level on screen, so
		// on a planet it has to be the local one rather than world +Y.
		glm::vec3 up = m_ExplicitBasis ? m_Up : glm::vec3(0.0f, 1.0f, 0.0f);

		m_ViewMatrix = glm::lookAt(m_Position, m_Position + GetForward(), up);
		RecalculateViewProjection();
	}

}
