#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Shader.h"
#include "Egss/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace Egss {

	// One uniform's worth of value, with a tag saying which member is real.
	//
	// A std::variant would say the same thing in fewer bytes. This is a few
	// dozen bytes per parameter and materials hold a handful, so the trade is
	// worth it for being able to read the upload loop without knowing what
	// std::visit does.
	struct EGSS_API MaterialParameter
	{
		enum class Type { Int, Float, Float3, Float4, Mat4 };

		std::string Name;
		Type Kind = Type::Float;

		int Int = 0;
		float Float = 0.0f;
		glm::vec4 Vector = glm::vec4(0.0f);   // Float3 uses xyz
		glm::mat4 Matrix = glm::mat4(1.0f);
	};

	struct EGSS_API MaterialTexture
	{
		std::string Name;                     // the sampler uniform
		std::shared_ptr<Texture2D> Texture;
		unsigned int Slot = 0;
	};

	// A shader plus the values to give it -- which is the thing the 3D renderer
	// had no word for. Without it, "how this object looks" lives as a run of
	// `SetFloat3` calls at the call site, which means it cannot be stored on an
	// entity, loaded from a file, or shared between meshes.
	//
	// **Instances are the important half.** Most uniforms in a scene are not
	// per-object at all: the light position, the camera, the ambient level are
	// the same for everything drawn that frame. Giving every object its own
	// complete material would mean setting those N times and getting them wrong
	// once. So a material may have a *base*: it holds only its overrides, and
	// falls back to the base for the rest.
	//
	//     base      = Material::Create(shader)      // light, camera, ambient
	//     perObject = Material::CreateInstance(base) // colour, entity id
	//
	// Binding an instance uploads the base's parameters first and then its own,
	// so an instance can override anything -- and the ordering is what makes
	// that true, not the storage.
	class EGSS_API Material
	{
	public:
		static std::shared_ptr<Material> Create(const std::shared_ptr<Shader>& shader);
		static std::shared_ptr<Material> CreateInstance(const std::shared_ptr<Material>& base);

		// Setting a name that is already present replaces it rather than
		// appending, so a value set every frame does not grow the list.
		void Set(const std::string& name, int value);
		void Set(const std::string& name, float value);
		void Set(const std::string& name, const glm::vec3& value);
		void Set(const std::string& name, const glm::vec4& value);
		void Set(const std::string& name, const glm::mat4& value);

		// The sampler uniform is set to the slot for you -- forgetting that is
		// the classic way to get a black object with no error anywhere.
		void SetTexture(const std::string& name, const std::shared_ptr<Texture2D>& texture,
			unsigned int slot = 0);

		// Binds the shader and uploads everything. `Renderer::Submit` does this
		// for you; call it directly only if you are issuing the draw yourself.
		void Bind() const;

		const std::shared_ptr<Shader>& GetShader() const { return m_Shader; }
		const std::shared_ptr<Material>& GetBase() const { return m_Base; }

		// This material's own parameters, not counting anything inherited.
		unsigned int GetParameterCount() const { return (unsigned int)m_Parameters.size(); }
		unsigned int GetTextureCount() const { return (unsigned int)m_Textures.size(); }

		// Looks here first, then up the base chain. Returns nullptr if neither
		// has it. Mostly useful for inspection and tests -- the renderer never
		// needs to ask.
		const MaterialParameter* Find(const std::string& name) const;
		bool Has(const std::string& name) const { return Find(name) != nullptr; }
	private:
		// Uploads without binding the shader, so a chain of bases uploads into
		// one already-bound program.
		void Apply() const;

		MaterialParameter& Slot(const std::string& name);
	private:
		std::shared_ptr<Shader> m_Shader;
		std::shared_ptr<Material> m_Base;

		// A vector, not a map: materials hold a handful of parameters, and
		// walking a contiguous few is faster than hashing each one every frame.
		// It also keeps upload order stable, which makes a capture readable.
		std::vector<MaterialParameter> m_Parameters;
		std::vector<MaterialTexture> m_Textures;
	};

}
