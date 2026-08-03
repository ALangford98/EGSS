#include "egsspch.h"
#include "Egss/Renderer/ShaderLibrary.h"

#include "Egss/Log.h"

namespace Egss {

	void ShaderLibrary::Add(const std::shared_ptr<Shader>& shader)
	{
		if (!shader)
		{
			EGSS_CORE_WARN("ShaderLibrary::Add called with a null shader");
			return;
		}

		Add(shader->GetName(), shader);
	}

	void ShaderLibrary::Add(const std::string& name, const std::shared_ptr<Shader>& shader)
	{
		if (!shader)
		{
			EGSS_CORE_WARN("ShaderLibrary::Add called with a null shader for '{0}'", name);
			return;
		}

		// Warn rather than assert, and let the new one win. Reloading a shader
		// under a name it already has is a normal thing to want; silently
		// keeping the old one would look like the edit had no effect.
		if (Exists(name))
			EGSS_CORE_WARN("Shader '{0}' replaced in the library", name);

		m_Shaders[name] = shader;
	}

	std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& filepath)
	{
		std::shared_ptr<Shader> shader(Shader::Create(filepath));
		Add(shader);
		return shader;
	}

	std::shared_ptr<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
	{
		std::shared_ptr<Shader> shader(Shader::Create(filepath));
		Add(name, shader);
		return shader;
	}

	std::shared_ptr<Shader> ShaderLibrary::Get(const std::string& name) const
	{
		auto it = m_Shaders.find(name);
		if (it == m_Shaders.end())
		{
			EGSS_CORE_ERROR("Shader '{0}' is not in the library", name);
			return nullptr;
		}

		return it->second;
	}

	bool ShaderLibrary::Exists(const std::string& name) const
	{
		return m_Shaders.find(name) != m_Shaders.end();
	}

}
