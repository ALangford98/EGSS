#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	class EGSS_API Shader
	{
	public:
		virtual ~Shader() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void SetInt(const std::string& name, int value) = 0;
		virtual void SetFloat(const std::string& name, float value) = 0;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) = 0;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) = 0;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;

		virtual const std::string& GetName() const = 0;

		static Shader* Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);

		// Reads a single file containing both stages, separated by
		// `#type vertex` / `#type fragment` lines.
		static Shader* Create(const std::string& filepath);
	};

}
