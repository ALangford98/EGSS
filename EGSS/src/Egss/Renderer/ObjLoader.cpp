#include "egsspch.h"
#include "Egss/Renderer/ObjLoader.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>

namespace Egss {

	namespace {

		// A face corner names three separate things: which position, which
		// texture coordinate, which normal. Two corners are the same vertex
		// only if all three match -- which is why a cube loaded from an .obj
		// still ends up with 24 vertices even though it has 8 positions.
		struct IndexTriplet
		{
			int Position;
			int TexCoord;
			int Normal;
			// Which face this corner belongs to, when the file carries no
			// normal and smoothing is off. Including it in the key is what
			// makes flat shading work: two faces meeting at a position get
			// separate vertices, so each can hold its own face normal. It is
			// -1 whenever the corner may be shared.
			int FlatFace;

			bool operator==(const IndexTriplet& other) const
			{
				return Position == other.Position
					&& TexCoord == other.TexCoord
					&& Normal == other.Normal
					&& FlatFace == other.FlatFace;
			}
		};

		struct TripletHash
		{
			size_t operator()(const IndexTriplet& key) const
			{
				// Odd multipliers so the three fields do not cancel each other
				// out when XORed. Any large primes would do.
				size_t h = (size_t)(key.Position + 1) * 73856093u;
				h ^= (size_t)(key.TexCoord + 1) * 19349663u;
				h ^= (size_t)(key.Normal + 1) * 83492791u;
				h ^= (size_t)(key.FlatFace + 1) * 50331653u;
				return h;
			}
		};

		inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

		inline void SkipSpaces(const char*& p, const char* end)
		{
			while (p < end && IsSpace(*p))
				p++;
		}

		// .obj indices are 1-based, and negative ones count back from the end
		// of whatever has been declared *so far*. Returns -1 for out of range,
		// which the caller reports rather than indexing with.
		inline int Resolve(long raw, size_t count)
		{
			if (raw > 0)
				return (raw <= (long)count) ? (int)(raw - 1) : -1;
			if (raw < 0)
			{
				long resolved = (long)count + raw;
				return (resolved >= 0) ? (int)resolved : -1;
			}
			return -1;   // 0 is not a valid .obj index
		}

	}

	bool ObjLoader::Load(const std::string& path, MeshData& out, std::string& error)
	{
		// Read the whole file at once. An .obj is parsed one line at a time,
		// but reading it one line at a time is dominated by the I/O -- and a
		// model file is small next to a texture.
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

	bool ObjLoader::Parse(const char* text, size_t length, MeshData& out, std::string& error)
	{
		// Parsed into these first. A face may reference any position declared
		// before it, so nothing can be finalised until the whole file is read.
		std::vector<glm::vec3> positions;
		std::vector<glm::vec2> texCoords;
		std::vector<glm::vec3> normals;

		MeshData result;
		std::unordered_map<IndexTriplet, unsigned int, TripletHash> seen;

		// Reused per face so an n-gon does not allocate.
		std::vector<unsigned int> faceCorners;

		// Which vertices still need a normal computed. A file may supply them
		// for some faces and not others, and overwriting the ones it gave
		// would throw away better information than anything derivable here.
		std::vector<bool> hasNormal;

		// `s off` is the .obj default, and it means flat: a face's corners are
		// not shared with its neighbours, so each face keeps its own normal.
		// Ignoring this made every low-poly model look like a balloon.
		bool smoothing = false;
		size_t faceIndex = 0;

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
			p = (lineEnd < end) ? lineEnd + 1 : end;   // next iteration starts here

			SkipSpaces(cursor, lineEnd);
			if (cursor >= lineEnd || *cursor == '#')
				continue;

			// --- v / vt / vn ------------------------------------------
			if (cursor[0] == 'v')
			{
				char kind = (cursor + 1 < lineEnd) ? cursor[1] : ' ';
				cursor += (kind == 't' || kind == 'n') ? 2 : 1;

				float values[3] = { 0.0f, 0.0f, 0.0f };
				int read = 0;
				while (read < 3 && cursor < lineEnd)
				{
					SkipSpaces(cursor, lineEnd);
					if (cursor >= lineEnd)
						break;

					char* after = nullptr;
					float value = std::strtof(cursor, &after);
					if (after == cursor)
						break;   // not a number; whatever follows is not ours

					values[read++] = value;
					cursor = after;
				}

				if (kind == 't')
				{
					// A `vt` may carry a third (w) component. It is meaningless
					// for a 2D sampler, so it is read and dropped.
					if (read < 2)
					{
						error = "line " + std::to_string(line) + ": vt needs 2 components";
						return false;
					}
					texCoords.push_back({ values[0], values[1] });
				}
				else if (kind == 'n')
				{
					if (read < 3)
					{
						error = "line " + std::to_string(line) + ": vn needs 3 components";
						return false;
					}
					normals.push_back({ values[0], values[1], values[2] });
				}
				else
				{
					// A `v` may carry a fourth (w) component and a vertex
					// colour. Both are ignored.
					if (read < 3)
					{
						error = "line " + std::to_string(line) + ": v needs 3 components";
						return false;
					}
					positions.push_back({ values[0], values[1], values[2] });
				}

				continue;
			}

			// --- s ----------------------------------------------------
			if (cursor[0] == 's' && (cursor + 1 >= lineEnd || IsSpace(cursor[1])))
			{
				cursor++;
				SkipSpaces(cursor, lineEnd);

				// "off" and "0" both mean flat; any group number means smooth.
				// Which group is deliberately not tracked: two smooth groups
				// meeting at an edge should not share vertices, and that is
				// worth doing when a file that cares turns up.
				size_t remaining = (size_t)(lineEnd - cursor);
				smoothing = !(remaining >= 3 && std::strncmp(cursor, "off", 3) == 0)
					&& !(remaining >= 1 && cursor[0] == '0'
						&& (remaining == 1 || IsSpace(cursor[1])));
				continue;
			}

			// --- f ----------------------------------------------------
			if (cursor[0] == 'f' && (cursor + 1 >= lineEnd || IsSpace(cursor[1])))
			{
				cursor++;
				faceCorners.clear();
				faceIndex++;

				while (cursor < lineEnd)
				{
					SkipSpaces(cursor, lineEnd);
					if (cursor >= lineEnd)
						break;

					// Every accepted form -- v, v/vt, v//vn, v/vt/vn -- falls
					// out of "read a number, and if a slash follows, read the
					// next field; a missing field between slashes stays 0".
					long raw[3] = { 0, 0, 0 };
					int field = 0;

					while (field < 3 && cursor < lineEnd && !IsSpace(*cursor))
					{
						if (*cursor == '/')
						{
							field++;
							cursor++;
							continue;
						}

						char* after = nullptr;
						raw[field] = std::strtol(cursor, &after, 10);
						if (after == cursor)
							break;

						cursor = after;
					}

					if (raw[0] == 0)
						break;   // no position index: the line is done

					IndexTriplet triplet;
					triplet.Position = Resolve(raw[0], positions.size());
					triplet.TexCoord = (raw[1] != 0) ? Resolve(raw[1], texCoords.size()) : -1;
					triplet.Normal = (raw[2] != 0) ? Resolve(raw[2], normals.size()) : -1;
					// Only unshared when the normal has to be invented *and*
					// the file asked for flat shading.
					triplet.FlatFace = (triplet.Normal < 0 && !smoothing) ? (int)faceIndex : -1;

					if (triplet.Position < 0)
					{
						error = "line " + std::to_string(line) + ": vertex index "
							+ std::to_string(raw[0]) + " out of range";
						return false;
					}
					if (raw[1] != 0 && triplet.TexCoord < 0)
					{
						error = "line " + std::to_string(line) + ": texture index "
							+ std::to_string(raw[1]) + " out of range";
						return false;
					}
					if (raw[2] != 0 && triplet.Normal < 0)
					{
						error = "line " + std::to_string(line) + ": normal index "
							+ std::to_string(raw[2]) + " out of range";
						return false;
					}

					// Emit a vertex the first time this exact triplet appears,
					// and reuse it after. Without this a shared cube corner
					// would be duplicated once per face that touches it.
					auto it = seen.find(triplet);
					if (it == seen.end())
					{
						MeshVertex vertex;
						vertex.Position = positions[triplet.Position];
						vertex.Normal = (triplet.Normal >= 0)
							? normals[triplet.Normal] : glm::vec3(0.0f);
						vertex.TexCoord = (triplet.TexCoord >= 0)
							? texCoords[triplet.TexCoord] : glm::vec2(0.0f);

						unsigned int index = (unsigned int)result.Vertices.size();
						result.Vertices.push_back(vertex);
						hasNormal.push_back(triplet.Normal >= 0);
						it = seen.emplace(triplet, index).first;
					}

					faceCorners.push_back(it->second);
				}

				if (faceCorners.size() < 3)
				{
					error = "line " + std::to_string(line) + ": face has "
						+ std::to_string(faceCorners.size()) + " corners";
					return false;
				}

				// Fan from the first corner. Correct for any *convex* face,
				// which is what modellers export; a concave n-gon would need
				// real triangulation, and is rare enough to be worth waiting
				// for a file that actually contains one.
				for (size_t i = 1; i + 1 < faceCorners.size(); i++)
				{
					result.Indices.push_back(faceCorners[0]);
					result.Indices.push_back(faceCorners[i]);
					result.Indices.push_back(faceCorners[i + 1]);
				}

				continue;
			}

			// Everything else -- o, g, s, usemtl, mtllib -- is skipped. An
			// unknown line is not an error: .obj has extensions this does not
			// need, and refusing to load a file over one would be unhelpful.
		}

		if (result.Vertices.empty() || result.Indices.empty())
		{
			error = "no geometry found";
			return false;
		}

		// Fill in only what the file left out. Flat corners belong to exactly
		// one face and so end up with that face's normal; smooth ones are
		// shared and end up with the average. The same loop does both, which
		// is the whole reason the sharing decision was pushed into the key.
		if (std::find(hasNormal.begin(), hasNormal.end(), false) != hasNormal.end())
			result.RecalculateNormals(&hasNormal);

		result.RecalculateBounds();

		out = std::move(result);
		return true;
	}

}
