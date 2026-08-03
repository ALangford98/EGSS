#include "egsspch.h"
#include "Egss/Renderer/Material.h"

#include "Egss/Log.h"

namespace Egss {

	std::shared_ptr<Material> Material::Create(const std::shared_ptr<Shader>& shader)
	{
		EGSS_CORE_ASSERT(shader, "A material needs a shader");

		auto material = std::shared_ptr<Material>(new Material());
		material->m_Shader = shader;
		return material;
	}

	std::shared_ptr<Material> Material::CreateInstance(const std::shared_ptr<Material>& base)
	{
		EGSS_CORE_ASSERT(base, "A material instance needs a base");

		auto material = std::shared_ptr<Material>(new Material());
		// The shader comes from the base rather than being passed again: an
		// instance that could use a *different* program would not be an
		// instance, and its inherited parameters would be uploaded into
		// uniforms that may not exist.
		material->m_Shader = base->m_Shader;
		material->m_Base = base;
		return material;
	}

	MaterialParameter& Material::Slot(const std::string& name)
	{
		for (MaterialParameter& parameter : m_Parameters)
		{
			if (parameter.Name == name)
				return parameter;
		}

		m_Parameters.push_back({});
		m_Parameters.back().Name = name;
		return m_Parameters.back();
	}

	void Material::Set(const std::string& name, int value)
	{
		MaterialParameter& parameter = Slot(name);
		parameter.Kind = MaterialParameter::Type::Int;
		parameter.Int = value;
	}

	void Material::Set(const std::string& name, float value)
	{
		MaterialParameter& parameter = Slot(name);
		parameter.Kind = MaterialParameter::Type::Float;
		parameter.Float = value;
	}

	void Material::Set(const std::string& name, const glm::vec3& value)
	{
		MaterialParameter& parameter = Slot(name);
		parameter.Kind = MaterialParameter::Type::Float3;
		parameter.Vector = glm::vec4(value, 0.0f);
	}

	void Material::Set(const std::string& name, const glm::vec4& value)
	{
		MaterialParameter& parameter = Slot(name);
		parameter.Kind = MaterialParameter::Type::Float4;
		parameter.Vector = value;
	}

	void Material::Set(const std::string& name, const glm::mat4& value)
	{
		MaterialParameter& parameter = Slot(name);
		parameter.Kind = MaterialParameter::Type::Mat4;
		parameter.Matrix = value;
	}

	void Material::SetTexture(const std::string& name,
		const std::shared_ptr<Texture2D>& texture, unsigned int slot)
	{
		for (MaterialTexture& bound : m_Textures)
		{
			if (bound.Name == name)
			{
				bound.Texture = texture;
				bound.Slot = slot;
				return;
			}
		}

		m_Textures.push_back({ name, texture, slot });
	}

	const MaterialParameter* Material::Find(const std::string& name) const
	{
		for (const MaterialParameter& parameter : m_Parameters)
		{
			if (parameter.Name == name)
				return &parameter;
		}

		return m_Base ? m_Base->Find(name) : nullptr;
	}

	void Material::Bind() const
	{
		m_Shader->Bind();
		Apply();
	}

	void Material::Apply() const
	{
		// Base first. An instance's own values are uploaded afterwards and land
		// on top, which is the entire mechanism behind overriding -- there is no
		// merge step and no lookup, just the later write winning.
		if (m_Base)
			m_Base->Apply();

		for (const MaterialParameter& parameter : m_Parameters)
		{
			switch (parameter.Kind)
			{
				case MaterialParameter::Type::Int:
					m_Shader->SetInt(parameter.Name, parameter.Int);
					break;
				case MaterialParameter::Type::Float:
					m_Shader->SetFloat(parameter.Name, parameter.Float);
					break;
				case MaterialParameter::Type::Float3:
					m_Shader->SetFloat3(parameter.Name, glm::vec3(parameter.Vector));
					break;
				case MaterialParameter::Type::Float4:
					m_Shader->SetFloat4(parameter.Name, parameter.Vector);
					break;
				case MaterialParameter::Type::Mat4:
					m_Shader->SetMat4(parameter.Name, parameter.Matrix);
					break;
			}
		}

		for (const MaterialTexture& bound : m_Textures)
		{
			if (!bound.Texture)
				continue;

			// Both halves, always together. Binding the texture without telling
			// the sampler which unit it went to samples unit 0 instead, which
			// is right by accident until the day something uses a second slot.
			m_Shader->SetInt(bound.Name, (int)bound.Slot);
			bound.Texture->Bind(bound.Slot);
		}
	}

}
