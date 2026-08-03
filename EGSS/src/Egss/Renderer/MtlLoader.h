#pragma once

#include "egsspch.h"
#include "Egss/Core.h"

#include <glm/glm.hpp>

namespace Egss {

	// One material as an .mtl file describes it: numbers and filenames, with no
	// shader and nothing on the GPU.
	//
	// Deliberately *not* an `Egss::Material`. A material binds values to a
	// particular shader's uniform names, and an .mtl knows nothing about any
	// shader -- it says "the diffuse colour is this", not "u_Color is this".
	// Keeping the parsed form separate is what lets one file feed two different
	// shaders, and keeps the parser testable with no GL context anywhere near
	// it.
	struct EGSS_API ObjMaterial
	{
		std::string Name;

		glm::vec3 Ambient = { 0.2f, 0.2f, 0.2f };    // Ka
		glm::vec3 Diffuse = { 0.8f, 0.8f, 0.8f };    // Kd
		glm::vec3 Specular = { 0.0f, 0.0f, 0.0f };   // Ks
		glm::vec3 Emissive = { 0.0f, 0.0f, 0.0f };   // Ke

		// Ns, the Phong exponent. 0 is "no specular highlight at all", which is
		// also what a file that omits it means.
		float SpecularExponent = 0.0f;
		// d. Files may instead write `Tr`, which is the *opposite* convention --
		// see the note in the parser.
		float Opacity = 1.0f;
		float IndexOfRefraction = 1.0f;   // Ni
		int IlluminationModel = 2;        // illum

		// Paths exactly as written in the file, which are relative to the .mtl.
		// Resolving them is the caller's job, because only the caller knows
		// where it read the file from.
		std::string DiffuseMap;    // map_Kd
		std::string AmbientMap;    // map_Ka
		std::string SpecularMap;   // map_Ks
		std::string EmissiveMap;   // map_Ke
		std::string OpacityMap;    // map_d
		std::string NormalMap;     // map_Bump, bump or norm
	};

	// Wavefront .mtl, the companion to .obj: plain text, one item per line, and
	// referenced by name from the model file's `mtllib`.
	//
	// What is supported: newmtl, Ka / Kd / Ks / Ke, Ns, d, Tr, Ni, illum, and
	// the map_* lines including their `-option value` prefixes.
	//
	// What is not: spectral and CIEXYZ colours (`Ka spectral file.rfl`), PBR
	// extensions (Pr / Pm / Ps), and reflection maps. Those are read past
	// rather than rejected -- an unknown line should not stop a file loading.
	class EGSS_API MtlLoader
	{
	public:
		static bool Load(const std::string& path, std::vector<ObjMaterial>& out,
			std::string& error);

		// The same parser over text already in memory, which is what makes this
		// testable without touching the filesystem.
		static bool Parse(const char* text, size_t length, std::vector<ObjMaterial>& out,
			std::string& error);

		// Everything before the last '/' or '\\' of `path`, including the
		// separator, or "" if there is none. An .mtl's texture paths and an
		// .obj's `mtllib` are both relative to the file that named them, so
		// this is what turns one into something openable.
		static std::string DirectoryOf(const std::string& path);
	};

}
