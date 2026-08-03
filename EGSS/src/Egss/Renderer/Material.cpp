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

	std::shared_ptr<Material> Material::FromObj(const ObjMaterial& source,
		const std::shared_ptr<Material>& base, const std::string& directory,
		const ObjMaterialUniforms& names,
		std::unordered_map<std::string, std::shared_ptr<Texture2D>>* cache)
	{
		auto material = CreateInstance(base);

		// The diffuse colour goes in as a vec4 with the dissolve as its alpha,
		// because that is the shape a shader's colour uniform almost always
		// has. Everything else stays the type the file gave it.
		if (!names.Diffuse.empty())
			material->Set(names.Diffuse, glm::vec4(source.Diffuse, source.Opacity));
		if (!names.Ambient.empty())
			material->Set(names.Ambient, source.Ambient);
		if (!names.Specular.empty())
			material->Set(names.Specular, source.Specular);
		if (!names.Emissive.empty())
			material->Set(names.Emissive, source.Emissive);
		if (!names.SpecularExponent.empty())
			material->Set(names.SpecularExponent, source.SpecularExponent);
		if (!names.Opacity.empty())
			material->Set(names.Opacity, source.Opacity);

		if (!names.DiffuseMap.empty() && !source.DiffuseMap.empty())
		{
			std::string path = directory + source.DiffuseMap;

			std::shared_ptr<Texture2D> texture;
			if (cache)
			{
				auto it = cache->find(path);
				if (it != cache->end())
					texture = it->second;
			}

			if (!texture)
			{
				texture.reset(Texture2D::Create(path));
				if (cache)
					(*cache)[path] = texture;
			}

			// A texture that failed to load is left unset rather than bound as
			// nothing: the base's texture then shows through, which reads as
			// "wrong texture" instead of "black object" and is far easier to
			// diagnose.
			if (texture)
				material->SetTexture(names.DiffuseMap, texture, names.DiffuseSlot);
		}

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
