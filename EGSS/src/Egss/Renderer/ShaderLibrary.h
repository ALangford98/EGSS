#pragma once

#include "egsspch.h"
#include "Egss/Core.h"
#include "Egss/Renderer/Shader.h"

namespace Egss {

	// Shaders by name, so nothing has to hold a `shared_ptr<Shader>` just to
	// pass it on. A `Material` refers to its shader directly, but everything
	// *else* -- a demo setting up, a loader resolving a name out of a file --
	// wants to ask for "Cube3D" rather than be handed the object.
	//
	// Deliberately not a singleton of its own: `Renderer` owns one, and an app
	// that wants a second (per level, per tool window) can just make one.
	class EGSS_API ShaderLibrary
	{
	public:
		// Under the shader's own name. A shader built from source strings was
		// given one at creation; one loaded from a file took its filename.
		void Add(const std::shared_ptr<Shader>& shader);
		// Under a name of your choosing, for when one file is wanted twice
		// under different names.
		void Add(const std::string& name, const std::shared_ptr<Shader>& shader);

		std::shared_ptr<Shader> Load(const std::string& filepath);
		std::shared_ptr<Shader> Load(const std::string& name, const std::string& filepath);

		// Returns nullptr and logs for a name that was never added, rather
		// than asserting. A missing shader should show up as an unlit object
		// and a log line, not take the program with it.
		std::shared_ptr<Shader> Get(const std::string& name) const;

		bool Exists(const std::string& name) const;
		unsigned int Count() const { return (unsigned int)m_Shaders.size(); }

		void Clear() { m_Shaders.clear(); }
	private:
		std::unordered_map<std::string, std::shared_ptr<Shader>> m_Shaders;
	};

}
