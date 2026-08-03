#include "egsspch.h"
#include "Egss/Renderer/MtlLoader.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace Egss {

	namespace {

		inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

		inline void SkipSpaces(const char*& p, const char* end)
		{
			while (p < end && IsSpace(*p))
				p++;
		}

		// True if `cursor` begins with `word` followed by whitespace or the end
		// of the line. The trailing check matters: without it `Ka` also matches
		// `Kd`'s prefix in files that use longer keywords, and `map_K` would
		// match everything.
		bool Keyword(const char*& cursor, const char* end, const char* word)
		{
			size_t length = std::strlen(word);
			if ((size_t)(end - cursor) < length)
				return false;
			if (std::strncmp(cursor, word, length) != 0)
				return false;

			const char* after = cursor + length;
			if (after < end && !IsSpace(*after))
				return false;

			cursor = after;
			return true;
		}

		// Reads up to three floats. Returns how many were found.
		int ReadFloats(const char*& cursor, const char* end, float* values, int max)
		{
			int read = 0;
			while (read < max && cursor < end)
			{
				SkipSpaces(cursor, end);
				if (cursor >= end)
					break;

				char* after = nullptr;
				float value = std::strtof(cursor, &after);
				if (after == cursor)
					break;

				values[read++] = value;
				cursor = after;
			}
			return read;
		}

		// A colour is `r g b`, `r` alone (meaning grey), or one of the forms
		// this does not support. Returns false for those, having consumed
		// nothing the caller needs.
		bool ReadColour(const char*& cursor, const char* end, glm::vec3& out)
		{
			SkipSpaces(cursor, end);

			// `Ka spectral file.rfl` and `Ka xyz x y z` are both legal and
			// neither is an RGB triple. Read past them rather than misreading
			// the first number as red.
			if ((size_t)(end - cursor) >= 3 && (std::strncmp(cursor, "xyz", 3) == 0
				|| std::strncmp(cursor, "spe", 3) == 0))
				return false;

			float values[3] = { 0.0f, 0.0f, 0.0f };
			int read = ReadFloats(cursor, end, values, 3);
			if (read == 0)
				return false;

			// One value means grey, which is the .mtl shorthand.
			out = (read == 1) ? glm::vec3(values[0])
				: glm::vec3(values[0], values[1], read >= 3 ? values[2] : values[1]);
			return true;
		}

		// A map line may carry options before the filename:
		//
		//     map_Kd -s 1 1 1 -o 0 0 0 -bm 0.2 brick.png
		//
		// The number of arguments differs per option, so rather than knowing
		// them all: skip any token starting with '-', then skip the tokens
		// after it that parse as numbers. What is left is the filename.
		//
		// The filename is then the *rest of the line*, not the next token --
		// paths with spaces in them are legal and common on Windows.
		std::string ReadMapPath(const char* cursor, const char* end)
		{
			while (cursor < end)
			{
				SkipSpaces(cursor, end);
				if (cursor >= end)
					break;

				if (*cursor != '-')
					break;   // no more options; the filename starts here

				// Skip the option token itself.
				while (cursor < end && !IsSpace(*cursor))
					cursor++;

				// Then its numeric arguments, however many there are.
				while (cursor < end)
				{
					const char* probe = cursor;
					SkipSpaces(probe, end);
					if (probe >= end)
						break;

					char* after = nullptr;
					std::strtof(probe, &after);
					if (after == probe)
						break;   // not a number: the next option or the filename

					cursor = after;
				}
			}

			SkipSpaces(cursor, end);
			if (cursor >= end)
				return {};

			// Trim trailing whitespace, which a \r has usually already left.
			const char* stop = end;
			while (stop > cursor && IsSpace(stop[-1]))
				stop--;

			return std::string(cursor, (size_t)(stop - cursor));
		}

	}

	std::string MtlLoader::DirectoryOf(const std::string& path)
	{
		size_t slash = path.find_last_of("/\\");
		return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
	}

	bool MtlLoader::Load(const std::string& path, std::vector<ObjMaterial>& out,
		std::string& error)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
		{
			error = "could not open '" + path + "'";
			return false;
		}

		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::string text;
		text.resize((size_t)size);
		if (size > 0 && !file.read(&text[0], size))
		{
			error = "could not read '" + path + "'";
			return false;
		}

		if (!Parse(text.data(), text.size(), out, error))
		{
			error = path + ": " + error;
			return false;
		}

		return true;
	}

	bool MtlLoader::Parse(const char* text, size_t length, std::vector<ObjMaterial>& out,
		std::string& error)
	{
		std::vector<ObjMaterial> result;
		size_t line = 0;

		const char* p = text;
		const char* end = text + length;

		while (p < end)
		{
			line++;

			const char* lineEnd = (const char*)memchr(p, '\n', (size_t)(end - p));
			if (!lineEnd)
				lineEnd = end;

			const char* cursor = p;
			p = (lineEnd < end) ? lineEnd + 1 : end;

			SkipSpaces(cursor, lineEnd);
			if (cursor >= lineEnd || *cursor == '#')
				continue;

			if (Keyword(cursor, lineEnd, "newmtl"))
			{
				SkipSpaces(cursor, lineEnd);

				const char* stop = lineEnd;
				while (stop > cursor && IsSpace(stop[-1]))
					stop--;

				if (stop <= cursor)
				{
					error = "line " + std::to_string(line) + ": newmtl has no name";
					return false;
				}

				result.push_back({});
				result.back().Name.assign(cursor, (size_t)(stop - cursor));
				continue;
			}

			// Everything below describes the material being defined. A value
			// before any `newmtl` has nothing to belong to, and quietly
			// dropping it would lose data with no sign anything was wrong.
			if (result.empty())
			{
				error = "line " + std::to_string(line) + ": value before any newmtl";
				return false;
			}

			ObjMaterial& material = result.back();

			// --- Colours ---------------------------------------------------
			// Longest keyword first: `Ka` would otherwise never be reached
			// through a prefix match, and map_Ka must not be read as Ka.
			if (Keyword(cursor, lineEnd, "map_Kd")) { material.DiffuseMap = ReadMapPath(cursor, lineEnd); continue; }
			if (Keyword(cursor, lineEnd, "map_Ka")) { material.AmbientMap = ReadMapPath(cursor, lineEnd); continue; }
			if (Keyword(cursor, lineEnd, "map_Ks")) { material.SpecularMap = ReadMapPath(cursor, lineEnd); continue; }
			if (Keyword(cursor, lineEnd, "map_Ke")) { material.EmissiveMap = ReadMapPath(cursor, lineEnd); continue; }
			if (Keyword(cursor, lineEnd, "map_d"))  { material.OpacityMap = ReadMapPath(cursor, lineEnd); continue; }

			// Three spellings of the same thing, all of them in the wild.
			if (Keyword(cursor, lineEnd, "map_Bump") || Keyword(cursor, lineEnd, "map_bump")
				|| Keyword(cursor, lineEnd, "bump") || Keyword(cursor, lineEnd, "norm"))
			{
				material.NormalMap = ReadMapPath(cursor, lineEnd);
				continue;
			}

			if (Keyword(cursor, lineEnd, "Ka")) { ReadColour(cursor, lineEnd, material.Ambient); continue; }
			if (Keyword(cursor, lineEnd, "Kd")) { ReadColour(cursor, lineEnd, material.Diffuse); continue; }
			if (Keyword(cursor, lineEnd, "Ks")) { ReadColour(cursor, lineEnd, material.Specular); continue; }
			if (Keyword(cursor, lineEnd, "Ke")) { ReadColour(cursor, lineEnd, material.Emissive); continue; }

			// --- Scalars ---------------------------------------------------
			float value = 0.0f;

			if (Keyword(cursor, lineEnd, "Ns"))
			{
				if (ReadFloats(cursor, lineEnd, &value, 1) == 1)
					material.SpecularExponent = value;
				continue;
			}

			if (Keyword(cursor, lineEnd, "Ni"))
			{
				if (ReadFloats(cursor, lineEnd, &value, 1) == 1)
					material.IndexOfRefraction = value;
				continue;
			}

			// `d` is dissolve: 1 is opaque. `Tr` is transparency: 0 is opaque.
			// They are the same quantity written upside down, and plenty of
			// exporters get it wrong -- but following the spec is the only
			// defensible choice, and a file carrying both is answered by
			// whichever came last.
			if (Keyword(cursor, lineEnd, "d"))
			{
				if (ReadFloats(cursor, lineEnd, &value, 1) == 1)
					material.Opacity = value;
				continue;
			}

			if (Keyword(cursor, lineEnd, "Tr"))
			{
				if (ReadFloats(cursor, lineEnd, &value, 1) == 1)
					material.Opacity = 1.0f - value;
				continue;
			}

			if (Keyword(cursor, lineEnd, "illum"))
			{
				if (ReadFloats(cursor, lineEnd, &value, 1) == 1)
					material.IlluminationModel = (int)value;
				continue;
			}

			// Anything else -- PBR extensions, reflection maps, vendor lines --
			// is read past. Refusing to load a file over a line nothing here
			// needs would be unhelpful.
		}

		if (result.empty())
		{
			error = "no materials found";
			return false;
		}

		out = std::move(result);
		return true;
	}

}
