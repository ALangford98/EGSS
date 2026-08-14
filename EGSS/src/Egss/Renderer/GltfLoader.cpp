#include "egsspch.h"
#include "Egss/Renderer/GltfLoader.h"

#include "Egss/Json.h"
#include "Egss/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>

namespace Egss {

	size_t GltfModel::TriangleCount() const
	{
		size_t total = 0;
		for (const MeshData& mesh : Meshes)
			total += mesh.TriangleCount();

		return total;
	}

	namespace {

		// --- Component types, straight out of the specification -------------
		//
		// These are OpenGL's enum values, which is not a coincidence: glTF was
		// designed so that a loader could hand them to glVertexAttribPointer
		// unchanged. This engine converts everything to float instead, because
		// MeshVertex is float and one layout is worth more here than one memcpy.

		constexpr int GL_BYTE_T = 5120;
		constexpr int GL_UNSIGNED_BYTE_T = 5121;
		constexpr int GL_SHORT_T = 5122;
		constexpr int GL_UNSIGNED_SHORT_T = 5123;
		constexpr int GL_UNSIGNED_INT_T = 5125;
		constexpr int GL_FLOAT_T = 5126;

		int ComponentSize(int componentType)
		{
			switch (componentType)
			{
			case GL_BYTE_T:
			case GL_UNSIGNED_BYTE_T:  return 1;
			case GL_SHORT_T:
			case GL_UNSIGNED_SHORT_T: return 2;
			case GL_UNSIGNED_INT_T:
			case GL_FLOAT_T:          return 4;
			default:                  return 0;
			}
		}

		int ComponentCount(const std::string& type)
		{
			if (type == "SCALAR") return 1;
			if (type == "VEC2")   return 2;
			if (type == "VEC3")   return 3;
			if (type == "VEC4")   return 4;
			if (type == "MAT2")   return 4;
			if (type == "MAT3")   return 9;
			if (type == "MAT4")   return 16;
			return 0;
		}

		// --- URIs -----------------------------------------------------------

		// Percent-decoding. URIs in a glTF are URIs, so a texture called
		// "brick wall.png" is written "brick%20wall.png" and will not open
		// unless this runs. Exporters do produce these.
		std::string PercentDecode(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());

			auto hex = [](char c) -> int
			{
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};

			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] == '%' && i + 2 < text.size())
				{
					int hi = hex(text[i + 1]), lo = hex(text[i + 2]);
					if (hi >= 0 && lo >= 0)
					{
						out.push_back((char)((hi << 4) | lo));
						i += 2;
						continue;
					}
				}

				out.push_back(text[i]);
			}

			return out;
		}

		// RFC 4648 base64, for data: URIs.
		//
		// Returns false on a character that is not base64 rather than skipping
		// it: silently ignoring junk turns a corrupted buffer into geometry
		// that is subtly wrong, which is far harder to diagnose than a refusal.
		bool DecodeBase64(const char* text, size_t length, std::vector<unsigned char>& out)
		{
			auto value = [](char c) -> int
			{
				if (c >= 'A' && c <= 'Z') return c - 'A';
				if (c >= 'a' && c <= 'z') return c - 'a' + 26;
				if (c >= '0' && c <= '9') return c - '0' + 52;
				if (c == '+') return 62;
				if (c == '/') return 63;
				return -1;
			};

			out.clear();
			out.reserve(length * 3 / 4);

			unsigned int accumulator = 0;
			int bits = 0;

			for (size_t i = 0; i < length; i++)
			{
				char c = text[i];

				// Line breaks are legal inside a base64 payload and exporters
				// do wrap long ones.
				if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
					continue;

				if (c == '=')
					break;

				int v = value(c);
				if (v < 0)
					return false;

				accumulator = (accumulator << 6) | (unsigned int)v;
				bits += 6;

				// Every four input characters make three bytes, and they fall
				// out one at a time as soon as eight bits have accumulated.
				if (bits >= 8)
				{
					bits -= 8;
					out.push_back((unsigned char)((accumulator >> bits) & 0xFF));
				}
			}

			return true;
		}

		bool StartsWith(const std::string& text, const char* prefix)
		{
			size_t n = std::strlen(prefix);
			return text.size() >= n && text.compare(0, n, prefix) == 0;
		}

		bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file)
				return false;

			std::streamsize size = file.tellg();
			if (size < 0)
				return false;

			file.seekg(0, std::ios::beg);
			out.resize((size_t)size);

			return size == 0 || (bool)file.read((char*)out.data(), size);
		}

		// --- The loader itself ----------------------------------------------

		class Gltf
		{
		public:
			Gltf(const std::string& baseDirectory) : m_Base(baseDirectory) {}

			bool Run(const unsigned char* bytes, size_t length, GltfModel& out, std::string& error)
			{
				const unsigned char* json = bytes;
				size_t jsonLength = length;

				// A .glb is told from a .gltf by its magic, not its extension.
				// "glTF" as little-endian bytes.
				if (length >= 12 && bytes[0] == 'g' && bytes[1] == 'l'
					&& bytes[2] == 'T' && bytes[3] == 'F')
				{
					if (!ReadGlb(bytes, length, json, jsonLength))
						return Fail(error);
				}

				JsonValue root;
				std::string jsonError;
				if (!JsonValue::Parse((const char*)json, jsonLength, root, jsonError))
				{
					error = jsonError;
					return false;
				}

				if (!Build(root, out))
					return Fail(error);

				return true;
			}
		private:
			// The GLB container: a 12-byte header, then length-prefixed chunks.
			// The first chunk is the JSON; a second, if present, is the binary
			// blob that buffer 0 refers to when it has no URI.
			bool ReadGlb(const unsigned char* bytes, size_t length,
				const unsigned char*& json, size_t& jsonLength)
			{
				auto u32 = [](const unsigned char* p)
				{
					// Assembled byte by byte rather than cast: glTF is
					// little-endian by definition, and a cast would silently
					// agree only on a little-endian machine.
					return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
						| ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
				};

				unsigned int version = u32(bytes + 4);
				unsigned int total = u32(bytes + 8);

				if (version != 2)
					return Error("this is glTF binary version " + std::to_string(version)
						+ "; only 2 is supported");

				// The header's own length field is authoritative, but a file
				// truncated in transit will claim more than it has.
				if (total > length)
					return Error("the .glb header claims " + std::to_string(total)
						+ " bytes but the file is " + std::to_string(length));

				bool haveJson = false;
				size_t pos = 12;

				while (pos + 8 <= total)
				{
					unsigned int chunkLength = u32(bytes + pos);
					unsigned int chunkType = u32(bytes + pos + 4);
					pos += 8;

					if (pos + chunkLength > total)
						return Error("a .glb chunk runs past the end of the file");

					if (chunkType == 0x4E4F534A)        // 'JSON'
					{
						json = bytes + pos;
						jsonLength = chunkLength;
						haveJson = true;
					}
					else if (chunkType == 0x004E4942)   // 'BIN\0'
					{
						m_Bin.assign(bytes + pos, bytes + pos + chunkLength);
						m_HasBin = true;
					}
					// Any other chunk type is an extension, and the spec says
					// to skip what you do not recognise rather than refuse it.

					// Chunks are padded to a four-byte boundary, and the
					// padding is not counted in the chunk length.
					pos += chunkLength;
					pos = (pos + 3) & ~(size_t)3;
				}

				if (!haveJson)
					return Error("the .glb has no JSON chunk");

				return true;
			}

			bool Build(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& asset = root["asset"];
				std::string version = asset["version"].GetString();

				// glTF 1.0 is a different format wearing the same name -- it
				// had shaders and techniques in the file. Refusing it by
				// version is much kinder than failing on a missing "meshes".
				if (!version.empty() && version[0] != '2')
					return Error("this is glTF " + version + "; only 2.x is supported");

				out.Generator = asset["generator"].GetString();

				if (!LoadBuffers(root))
					return false;

				m_BufferViews = &root["bufferViews"];
				m_Accessors = &root["accessors"];

				LoadImages(root, out);
				LoadMaterials(root, out);

				if (!LoadMeshes(root, out))
					return false;

				LoadNodes(root, out);
				Flatten(root, out);

				return true;
			}

			// --- Buffers ----------------------------------------------------

			bool LoadBuffers(const JsonValue& root)
			{
				const JsonValue& buffers = root["buffers"];
				m_Buffers.resize(buffers.Size());

				for (size_t i = 0; i < buffers.Size(); i++)
				{
					const JsonValue& buffer = buffers[i];
					std::string uri = buffer["uri"].GetString();

					if (uri.empty())
					{
						// No URI means the GLB's binary chunk, and only buffer
						// 0 is allowed to do that.
						if (i != 0 || !m_HasBin)
							return Error("buffer " + std::to_string(i)
								+ " has no URI and there is no binary chunk to be");

						m_Buffers[i] = m_Bin;
					}
					else if (StartsWith(uri, "data:"))
					{
						size_t comma = uri.find(',');
						if (comma == std::string::npos)
							return Error("a data: URI with no comma");

						// Only base64 payloads are read. The alternative is
						// percent-encoded raw text, which nothing writes for
						// binary data and which would be enormous if it did.
						if (uri.find(";base64", 0) == std::string::npos
							|| uri.find(";base64", 0) > comma)
							return Error("a data: URI that is not base64");

						if (!DecodeBase64(uri.data() + comma + 1, uri.size() - comma - 1, m_Buffers[i]))
							return Error("a data: URI with invalid base64");
					}
					else
					{
						std::string path = m_Base + PercentDecode(uri);
						if (!ReadWholeFile(path, m_Buffers[i]))
							return Error("could not open the buffer file '" + path + "'");
					}

					// The declared length is what the accessors were written
					// against. A short file means every offset past that point
					// is wrong, and it is much better to say so here than to
					// return silently truncated geometry.
					size_t declared = (size_t)buffer["byteLength"].GetInt();
					if (declared != 0 && m_Buffers[i].size() < declared)
						return Error("buffer " + std::to_string(i) + " declares "
							+ std::to_string(declared) + " bytes but supplies "
							+ std::to_string(m_Buffers[i].size()));
				}

				return true;
			}

			// --- Accessors --------------------------------------------------
			//
			// The mechanism worth following in this whole file. An accessor
			// says "count elements of this type, starting here"; a bufferView
			// says "this window of that buffer, with this stride". Together
			// they let one buffer hold interleaved position/normal/uv for a
			// whole model, and let two accessors read the same bytes as
			// different things.
			//
			// The address of element i is:
			//
			//     buffer + view.byteOffset + accessor.byteOffset + i * stride
			//
			// where stride is the view's byteStride if it has one, and the
			// element's own size if it does not. Getting this wrong by using
			// the element size when a stride is present is *the* classic glTF
			// bug: interleaved data reads the first attribute repeatedly and
			// the model collapses to a smear.

			// Reads `accessor` as floats, `components` per element, into `out`
			// (which is resized to count * components).
			bool ReadFloats(int accessorIndex, int components, std::vector<float>& out)
			{
				const JsonValue& accessor = (*m_Accessors)[accessorIndex];
				if (accessor.IsNull())
					return Error("accessor " + std::to_string(accessorIndex) + " does not exist");

				int componentType = accessor["componentType"].GetInt();
				int count = accessor["count"].GetInt();
				int inFile = ComponentCount(accessor["type"].GetString());
				bool normalized = accessor["normalized"].GetBool();

				if (inFile == 0)
					return Error("accessor " + std::to_string(accessorIndex)
						+ " has an unknown type '" + accessor["type"].GetString() + "'");

				out.assign((size_t)count * components, 0.0f);

				// An accessor with no bufferView is defined to be all zeros --
				// it exists so that a sparse accessor can override a few
				// entries of nothing.
				if (accessor.Has("bufferView"))
				{
					if (!ReadInto(accessor, accessor["bufferView"].GetInt(), componentType,
						inFile, count, components, normalized, 0, out))
						return false;
				}

				if (accessor.Has("sparse") && !ApplySparse(accessor["sparse"], componentType,
					inFile, components, normalized, out))
					return false;

				return true;
			}

			// The same, for indices: always scalar, always integral.
			bool ReadIndices(int accessorIndex, std::vector<unsigned int>& out)
			{
				const JsonValue& accessor = (*m_Accessors)[accessorIndex];
				if (accessor.IsNull())
					return Error("index accessor " + std::to_string(accessorIndex) + " does not exist");

				int componentType = accessor["componentType"].GetInt();
				int count = accessor["count"].GetInt();

				const unsigned char* base = nullptr;
				size_t stride = 0;
				if (!Window(accessor["bufferView"].GetInt(), accessor["byteOffset"].GetInt(),
					ComponentSize(componentType), count, base, stride))
					return false;

				out.resize((size_t)count);
				for (int i = 0; i < count; i++)
					out[i] = ReadUnsigned(base + (size_t)i * stride, componentType);

				return true;
			}
		private:
			// Resolves a bufferView plus an offset into a pointer and a stride,
			// checking that every element it will be asked for is inside the
			// buffer. Doing the bounds check once here rather than per element
			// is what keeps the read loops simple *and* safe.
			bool Window(int viewIndex, int accessorOffset, size_t elementSize, int count,
				const unsigned char*& base, size_t& stride)
			{
				const JsonValue& view = (*m_BufferViews)[viewIndex];
				if (view.IsNull())
					return Error("bufferView " + std::to_string(viewIndex) + " does not exist");

				int bufferIndex = view["buffer"].GetInt();
				if (bufferIndex < 0 || (size_t)bufferIndex >= m_Buffers.size())
					return Error("bufferView " + std::to_string(viewIndex)
						+ " names buffer " + std::to_string(bufferIndex) + ", which does not exist");

				const std::vector<unsigned char>& buffer = m_Buffers[bufferIndex];

				size_t viewOffset = (size_t)view["byteOffset"].GetInt();
				size_t viewLength = (size_t)view["byteLength"].GetInt();
				size_t byteStride = (size_t)view["byteStride"].GetInt();

				// A stride of 0 means tightly packed, which is the common case
				// and the one an exporter writes for a de-interleaved buffer.
				stride = byteStride != 0 ? byteStride : elementSize;

				if (stride < elementSize)
					return Error("bufferView " + std::to_string(viewIndex) + " has a stride of "
						+ std::to_string(stride) + ", smaller than its " + std::to_string(elementSize)
						+ "-byte elements");

				size_t start = viewOffset + (size_t)accessorOffset;

				// The last element ends at start + (count-1)*stride + size,
				// not at start + count*stride -- the trailing stride padding
				// after the final element need not exist, and a file that
				// packs the buffer exactly would be rejected if this used the
				// simpler expression.
				size_t needed = count > 0
					? (size_t)(count - 1) * stride + elementSize
					: 0;

				if (start + needed > buffer.size())
					return Error("an accessor reads past the end of buffer "
						+ std::to_string(bufferIndex));

				if (viewLength != 0 && (size_t)accessorOffset + needed > viewLength)
					return Error("an accessor reads past the end of bufferView "
						+ std::to_string(viewIndex));

				base = buffer.data() + start;
				return true;
			}

			bool ReadInto(const JsonValue& accessor, int viewIndex, int componentType,
				int inFile, int count, int components, bool normalized,
				size_t firstElement, std::vector<float>& out)
			{
				size_t componentSize = (size_t)ComponentSize(componentType);
				if (componentSize == 0)
					return Error("unknown componentType " + std::to_string(componentType));

				const unsigned char* base = nullptr;
				size_t stride = 0;
				if (!Window(viewIndex, accessor["byteOffset"].GetInt(),
					componentSize * (size_t)inFile, count, base, stride))
					return false;

				// Copy min(inFile, components) per element. A file supplying
				// VEC3 colours into a VEC4 slot leaves alpha at whatever the
				// caller seeded, and a VEC4 read as VEC3 drops the w -- both of
				// which are what the spec asks for.
				int copy = inFile < components ? inFile : components;

				for (int i = 0; i < count; i++)
				{
					const unsigned char* element = base + (size_t)i * stride;
					float* destination = out.data() + (firstElement + (size_t)i) * (size_t)components;

					for (int c = 0; c < copy; c++)
						destination[c] = ReadComponent(element + (size_t)c * componentSize,
							componentType, normalized);
				}

				return true;
			}

			// Sparse accessors: a base array plus a list of (index, value)
			// overrides. Implemented rather than refused because ignoring the
			// override list would produce a model that loads, draws, and is
			// quietly wrong -- the worst of the three outcomes.
			bool ApplySparse(const JsonValue& sparse, int componentType, int inFile,
				int components, bool normalized, std::vector<float>& out)
			{
				int count = sparse["count"].GetInt();
				if (count <= 0)
					return true;

				const JsonValue& indices = sparse["indices"];
				const JsonValue& values = sparse["values"];

				int indexType = indices["componentType"].GetInt();
				size_t indexSize = (size_t)ComponentSize(indexType);
				if (indexSize == 0)
					return Error("a sparse accessor has an unknown index componentType");

				const unsigned char* indexBase = nullptr;
				size_t indexStride = 0;
				if (!Window(indices["bufferView"].GetInt(), indices["byteOffset"].GetInt(),
					indexSize, count, indexBase, indexStride))
					return false;

				size_t componentSize = (size_t)ComponentSize(componentType);
				const unsigned char* valueBase = nullptr;
				size_t valueStride = 0;
				if (!Window(values["bufferView"].GetInt(), values["byteOffset"].GetInt(),
					componentSize * (size_t)inFile, count, valueBase, valueStride))
					return false;

				int copy = inFile < components ? inFile : components;
				size_t elements = out.size() / (size_t)components;

				for (int i = 0; i < count; i++)
				{
					unsigned int target = ReadUnsigned(indexBase + (size_t)i * indexStride, indexType);
					if (target >= elements)
						return Error("a sparse accessor overrides element "
							+ std::to_string(target) + ", past its own count");

					const unsigned char* element = valueBase + (size_t)i * valueStride;
					float* destination = out.data() + (size_t)target * (size_t)components;

					for (int c = 0; c < copy; c++)
						destination[c] = ReadComponent(element + (size_t)c * componentSize,
							componentType, normalized);
				}

				return true;
			}

			// One component, widened to float.
			//
			// `normalized` is the flag that turns an integer into a fraction,
			// and the two signednesses do not share a formula: unsigned maps
			// [0, 2^n - 1] onto [0, 1], while signed maps [-(2^(n-1) - 1),
			// 2^(n-1) - 1] onto [-1, 1] and clamps, because two's complement
			// has one more negative value than positive and -128/127 would
			// otherwise come out below -1.
			static float ReadComponent(const unsigned char* p, int componentType, bool normalized)
			{
				switch (componentType)
				{
				case GL_FLOAT_T:
				{
					float value;
					std::memcpy(&value, p, sizeof(float));
					return value;
				}
				case GL_UNSIGNED_BYTE_T:
				{
					unsigned char value = *p;
					return normalized ? (float)value / 255.0f : (float)value;
				}
				case GL_BYTE_T:
				{
					signed char value;
					std::memcpy(&value, p, 1);
					return normalized ? std::max((float)value / 127.0f, -1.0f) : (float)value;
				}
				case GL_UNSIGNED_SHORT_T:
				{
					unsigned short value;
					std::memcpy(&value, p, 2);
					return normalized ? (float)value / 65535.0f : (float)value;
				}
				case GL_SHORT_T:
				{
					short value;
					std::memcpy(&value, p, 2);
					return normalized ? std::max((float)value / 32767.0f, -1.0f) : (float)value;
				}
				case GL_UNSIGNED_INT_T:
				{
					unsigned int value;
					std::memcpy(&value, p, 4);
					return (float)value;
				}
				default:
					return 0.0f;
				}
			}

			static unsigned int ReadUnsigned(const unsigned char* p, int componentType)
			{
				switch (componentType)
				{
				case GL_UNSIGNED_BYTE_T:
					return *p;
				case GL_UNSIGNED_SHORT_T:
				{
					unsigned short value;
					std::memcpy(&value, p, 2);
					return value;
				}
				case GL_UNSIGNED_INT_T:
				{
					unsigned int value;
					std::memcpy(&value, p, 4);
					return value;
				}
				default:
					return 0;
				}
			}

			// --- Images and materials ---------------------------------------

			void LoadImages(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& images = root["images"];
				out.Images.resize(images.Size());

				for (size_t i = 0; i < images.Size(); i++)
				{
					const JsonValue& image = images[i];
					GltfImage& result = out.Images[i];

					result.Name = image["name"].GetString();
					result.MimeType = image["mimeType"].GetString();

					std::string uri = image["uri"].GetString();

					if (image.Has("bufferView"))
					{
						// Embedded in the binary blob: a whole PNG sitting in
						// a bufferView, which is how a .glb ships its textures.
						const unsigned char* base = nullptr;
						size_t stride = 0;
						int viewIndex = image["bufferView"].GetInt();
						const JsonValue& view = (*m_BufferViews)[viewIndex];
						size_t length = (size_t)view["byteLength"].GetInt();

						if (length > 0 && Window(viewIndex, 0, length, 1, base, stride))
							result.Bytes.assign(base, base + length);
					}
					else if (StartsWith(uri, "data:"))
					{
						size_t comma = uri.find(',');
						if (comma != std::string::npos)
							DecodeBase64(uri.data() + comma + 1, uri.size() - comma - 1, result.Bytes);
					}
					else if (!uri.empty())
					{
						result.Path = m_Base + PercentDecode(uri);
					}
				}
			}

			// A material points at a texture, which points at an image. That
			// indirection exists so several materials can share one image with
			// different samplers; nothing here reads samplers yet, so this
			// follows the chain and keeps the image.
			int ImageOf(const JsonValue& root, const JsonValue& textureRef) const
			{
				if (!textureRef.Has("index"))
					return -1;

				const JsonValue& texture = root["textures"][textureRef["index"].GetInt()];
				if (!texture.Has("source"))
					return -1;

				return texture["source"].GetInt();
			}

			void LoadMaterials(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& materials = root["materials"];
				out.Materials.resize(materials.Size());

				for (size_t i = 0; i < materials.Size(); i++)
				{
					const JsonValue& material = materials[i];
					GltfMaterial& result = out.Materials[i];

					result.Name = material["name"].GetString();
					result.DoubleSided = material["doubleSided"].GetBool();

					const JsonValue& pbr = material["pbrMetallicRoughness"];

					const JsonValue& colour = pbr["baseColorFactor"];
					if (colour.Size() == 4)
						result.BaseColour = {
							colour[0].GetFloat(1.0f), colour[1].GetFloat(1.0f),
							colour[2].GetFloat(1.0f), colour[3].GetFloat(1.0f)
						};

					// 1.0 is the spec default for both, and it matters: a
					// material that omits them is fully metallic and fully
					// rough, not the 0/0 a zero-initialised struct would give.
					result.Metallic = pbr["metallicFactor"].GetFloat(1.0f);
					result.Roughness = pbr["roughnessFactor"].GetFloat(1.0f);

					const JsonValue& emissive = material["emissiveFactor"];
					if (emissive.Size() == 3)
						result.Emissive = { emissive[0].GetFloat(), emissive[1].GetFloat(),
											emissive[2].GetFloat() };

					const JsonValue& baseColourTexture = pbr["baseColorTexture"];
					result.BaseColourImage = ImageOf(root, baseColourTexture);
					result.BaseColourUV = baseColourTexture["texCoord"].GetInt(0);

					result.MetallicRoughnessImage = ImageOf(root, pbr["metallicRoughnessTexture"]);
					result.NormalImage = ImageOf(root, material["normalTexture"]);
					result.OcclusionImage = ImageOf(root, material["occlusionTexture"]);
					result.EmissiveImage = ImageOf(root, material["emissiveTexture"]);

					std::string alpha = material["alphaMode"].GetString();
					if (alpha == "MASK")       result.AlphaMode = GltfMaterial::Alpha::Mask;
					else if (alpha == "BLEND") result.AlphaMode = GltfMaterial::Alpha::Blend;

					result.AlphaCutoff = material["alphaCutoff"].GetFloat(0.5f);
				}
			}

			// --- Meshes -----------------------------------------------------

			bool LoadMeshes(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& meshes = root["meshes"];
				out.Meshes.resize(meshes.Size());

				for (size_t i = 0; i < meshes.Size(); i++)
					if (!LoadMesh(meshes[i], out.Meshes[i]))
						return false;

				return true;
			}

			bool LoadMesh(const JsonValue& mesh, MeshData& out)
			{
				const JsonValue& primitives = mesh["primitives"];

				for (size_t p = 0; p < primitives.Size(); p++)
				{
					const JsonValue& primitive = primitives[p];

					// 4 is TRIANGLES. Modes 0-3 are points and lines, which
					// this renderer has no path for; 5 and 6 are strips and
					// fans, which are converted below.
					int mode = primitive["mode"].GetInt(4);
					if (mode < 4)
						continue;

					const JsonValue& attributes = primitive["attributes"];
					if (!attributes.Has("POSITION"))
						continue;

					int positionAccessor = attributes["POSITION"].GetInt();

					std::vector<float> positions, normals, texCoords;
					if (!ReadFloats(positionAccessor, 3, positions))
						return false;

					size_t vertexCount = positions.size() / 3;

					bool haveNormals = attributes.Has("NORMAL");
					if (haveNormals && !ReadFloats(attributes["NORMAL"].GetInt(), 3, normals))
						return false;

					bool haveTexCoords = attributes.Has("TEXCOORD_0");
					if (haveTexCoords && !ReadFloats(attributes["TEXCOORD_0"].GetInt(), 2, texCoords))
						return false;

					// Every primitive appends to one shared vertex buffer, so
					// its indices have to be shifted by however many vertices
					// are already in it.
					unsigned int firstVertex = (unsigned int)out.Vertices.size();
					unsigned int firstIndex = (unsigned int)out.Indices.size();

					out.Vertices.reserve(out.Vertices.size() + vertexCount);
					for (size_t v = 0; v < vertexCount; v++)
					{
						MeshVertex vertex;
						vertex.Position = { positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2] };
						vertex.Normal = haveNormals
							? glm::vec3(normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2])
							: glm::vec3(0.0f);
						vertex.TexCoord = haveTexCoords
							? glm::vec2(texCoords[v * 2], texCoords[v * 2 + 1])
							: glm::vec2(0.0f);

						out.Vertices.push_back(vertex);
					}

					std::vector<unsigned int> indices;
					if (primitive.Has("indices"))
					{
						if (!ReadIndices(primitive["indices"].GetInt(), indices))
							return false;

						for (unsigned int index : indices)
							if (index >= vertexCount)
								return Error("an index of " + std::to_string(index)
									+ " in a primitive with only " + std::to_string(vertexCount)
									+ " vertices");
					}
					else
					{
						// A primitive with no indices draws its vertices in
						// order, which is a legal and common way to ship a
						// mesh that shares nothing.
						indices.resize(vertexCount);
						for (size_t v = 0; v < vertexCount; v++)
							indices[v] = (unsigned int)v;
					}

					AppendTriangles(indices, mode, firstVertex, out.Indices);

					Submesh submesh;
					submesh.FirstIndex = firstIndex;
					submesh.IndexCount = (unsigned int)out.Indices.size() - firstIndex;
					submesh.MaterialIndex = primitive.Has("material")
						? primitive["material"].GetInt() : -1;
					out.Submeshes.push_back(submesh);

					if (!haveNormals)
						m_NeedsNormals = true;
				}

				if (out.Submeshes.empty())
				{
					// MeshData promises at least one submesh covering
					// everything, so a mesh whose primitives were all points
					// or lines still produces something coherent.
					Submesh all;
					all.FirstIndex = 0;
					all.IndexCount = (unsigned int)out.Indices.size();
					out.Submeshes.push_back(all);
				}

				if (m_NeedsNormals)
				{
					out.RecalculateNormals();
					m_NeedsNormals = false;
				}

				out.RecalculateBounds();
				return true;
			}

			// Triangles, strips and fans all become plain triangles.
			//
			// A strip alternates winding: triangle i is (i, i+1, i+2) for even
			// i and (i+1, i, i+2) for odd, and getting that backwards turns
			// every other face inside out -- which with back-face culling on
			// looks like a hole rather than like a winding bug.
			static void AppendTriangles(const std::vector<unsigned int>& indices, int mode,
				unsigned int firstVertex, std::vector<unsigned int>& out)
			{
				size_t n = indices.size();

				if (mode == 4)   // TRIANGLES
				{
					for (size_t i = 0; i + 2 < n; i += 3)
					{
						out.push_back(firstVertex + indices[i]);
						out.push_back(firstVertex + indices[i + 1]);
						out.push_back(firstVertex + indices[i + 2]);
					}
				}
				else if (mode == 5)   // TRIANGLE_STRIP
				{
					for (size_t i = 0; i + 2 < n; i++)
					{
						bool odd = (i & 1) != 0;
						out.push_back(firstVertex + indices[i + (odd ? 1 : 0)]);
						out.push_back(firstVertex + indices[i + (odd ? 0 : 1)]);
						out.push_back(firstVertex + indices[i + 2]);
					}
				}
				else if (mode == 6)   // TRIANGLE_FAN
				{
					for (size_t i = 1; i + 1 < n; i++)
					{
						out.push_back(firstVertex + indices[0]);
						out.push_back(firstVertex + indices[i]);
						out.push_back(firstVertex + indices[i + 1]);
					}
				}
			}

			// --- Nodes ------------------------------------------------------

			void LoadNodes(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& nodes = root["nodes"];
				out.Nodes.resize(nodes.Size());

				for (size_t i = 0; i < nodes.Size(); i++)
				{
					const JsonValue& node = nodes[i];
					GltfNode& result = out.Nodes[i];

					result.Name = node["name"].GetString();
					result.Mesh = node.Has("mesh") ? node["mesh"].GetInt() : -1;

					const JsonValue& children = node["children"];
					result.Children.reserve(children.Size());
					for (size_t c = 0; c < children.Size(); c++)
						result.Children.push_back(children[c].GetInt());

					result.Local = NodeTransform(node);
				}
			}

			static glm::mat4 NodeTransform(const JsonValue& node)
			{
				// A node gives either a matrix or a TRS triple, never both.
				const JsonValue& matrix = node["matrix"];
				if (matrix.Size() == 16)
				{
					// glTF writes matrices in **column-major** order, which is
					// also how glm stores them -- so the sixteen numbers go
					// straight in. Worth stating, because the same array read
					// row-major gives the transpose, and a transposed transform
					// still looks like a transform: the model appears, in the
					// wrong place, at the wrong angle.
					glm::mat4 result(1.0f);
					for (int column = 0; column < 4; column++)
						for (int row = 0; row < 4; row++)
							result[column][row] = matrix[column * 4 + row].GetFloat();

					return result;
				}

				glm::vec3 translation(0.0f);
				const JsonValue& t = node["translation"];
				if (t.Size() == 3)
					translation = { t[0].GetFloat(), t[1].GetFloat(), t[2].GetFloat() };

				glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
				const JsonValue& r = node["rotation"];
				if (r.Size() == 4)
				{
					// glTF stores a quaternion as (x, y, z, w); glm's
					// constructor takes (w, x, y, z). Swapping these produces a
					// rotation that is wrong but still a rotation, so nothing
					// crashes and the model is simply facing the wrong way.
					rotation = glm::quat(r[3].GetFloat(1.0f), r[0].GetFloat(),
						r[1].GetFloat(), r[2].GetFloat());
				}

				glm::vec3 scale(1.0f);
				const JsonValue& s = node["scale"];
				if (s.Size() == 3)
					scale = { s[0].GetFloat(1.0f), s[1].GetFloat(1.0f), s[2].GetFloat(1.0f) };

				// T * R * S, in that order, is what the specification defines.
				return glm::translate(glm::mat4(1.0f), translation)
					* glm::mat4_cast(rotation)
					* glm::scale(glm::mat4(1.0f), scale);
			}

			// Walks the tree once, composing each node's transform onto its
			// parent's, and records every node that carries a mesh.
			void Flatten(const JsonValue& root, GltfModel& out)
			{
				const JsonValue& scenes = root["scenes"];
				int sceneIndex = root["scene"].GetInt(0);

				const JsonValue& scene = scenes[sceneIndex];
				const JsonValue& roots = scene["nodes"];

				for (size_t i = 0; i < roots.Size(); i++)
					out.RootNodes.push_back(roots[i].GetInt());

				// A file with no scene at all still has nodes worth drawing --
				// treat every node that nothing else parents as a root.
				if (out.RootNodes.empty() && !out.Nodes.empty())
				{
					std::vector<bool> parented(out.Nodes.size(), false);
					for (const GltfNode& node : out.Nodes)
						for (int child : node.Children)
							if (child >= 0 && (size_t)child < parented.size())
								parented[child] = true;

					for (size_t i = 0; i < out.Nodes.size(); i++)
						if (!parented[i])
							out.RootNodes.push_back((int)i);
				}

				// Cycles would be an infinite descent. glTF forbids them, but
				// this is reading a file off disk, so it is checked rather
				// than assumed.
				std::vector<bool> visited(out.Nodes.size(), false);
				for (int node : out.RootNodes)
					Visit(node, glm::mat4(1.0f), visited, out);

				ComputeBounds(out);
			}

			void Visit(int index, const glm::mat4& parent, std::vector<bool>& visited, GltfModel& out)
			{
				if (index < 0 || (size_t)index >= out.Nodes.size() || visited[index])
					return;

				visited[index] = true;

				const GltfNode& node = out.Nodes[index];
				glm::mat4 world = parent * node.Local;

				if (node.Mesh >= 0 && (size_t)node.Mesh < out.Meshes.size())
				{
					GltfInstance instance;
					instance.Node = index;
					instance.Mesh = node.Mesh;
					instance.Transform = world;
					out.Instances.push_back(instance);
				}

				for (int child : node.Children)
					Visit(child, world, visited, out);
			}

			// The bounds of the assembled model, which is not the union of the
			// meshes' own bounds -- a mesh placed twice at two transforms
			// occupies two different boxes.
			//
			// All eight corners of each mesh's box are transformed, not just
			// the min and max: under a rotation the min corner does not map to
			// the min corner, and using two corners silently under-reports the
			// box for anything not axis-aligned.
			static void ComputeBounds(GltfModel& out)
			{
				bool any = false;
				glm::vec3 low(0.0f), high(0.0f);

				for (const GltfInstance& instance : out.Instances)
				{
					const MeshData& mesh = out.Meshes[instance.Mesh];
					if (mesh.Vertices.empty())
						continue;

					for (int corner = 0; corner < 8; corner++)
					{
						glm::vec3 point(
							(corner & 1) ? mesh.BoundsMax.x : mesh.BoundsMin.x,
							(corner & 2) ? mesh.BoundsMax.y : mesh.BoundsMin.y,
							(corner & 4) ? mesh.BoundsMax.z : mesh.BoundsMin.z);

						glm::vec3 world = glm::vec3(instance.Transform * glm::vec4(point, 1.0f));

						if (!any) { low = high = world; any = true; }
						else
						{
							low = glm::min(low, world);
							high = glm::max(high, world);
						}
					}
				}

				out.BoundsMin = low;
				out.BoundsMax = high;
			}

			bool Error(const std::string& what)
			{
				if (m_Error.empty())
					m_Error = "glTF: " + what;

				return false;
			}

			bool Fail(std::string& error)
			{
				error = m_Error.empty() ? "glTF: unknown error" : m_Error;
				return false;
			}

			std::string m_Base;
			std::string m_Error;

			std::vector<std::vector<unsigned char>> m_Buffers;
			std::vector<unsigned char> m_Bin;
			bool m_HasBin = false;

			const JsonValue* m_BufferViews = nullptr;
			const JsonValue* m_Accessors = nullptr;

			bool m_NeedsNormals = false;
		};

	}

	bool GltfLoader::Parse(const unsigned char* bytes, size_t length,
		const std::string& baseDirectory, GltfModel& out, std::string& error)
	{
		error.clear();

		GltfModel model;
		Gltf loader(baseDirectory);

		if (!loader.Run(bytes, length, model, error))
			return false;

		out = std::move(model);
		return true;
	}

	bool GltfLoader::Load(const std::string& path, GltfModel& out, std::string& error)
	{
		std::vector<unsigned char> bytes;
		if (!ReadWholeFile(path, bytes))
		{
			error = "glTF: could not open '" + path + "'";
			return false;
		}

		// URIs inside the file are relative to the file, so this is what makes
		// them openable -- the same rule .obj's `mtllib` follows.
		size_t slash = path.find_last_of("/\\");
		std::string directory = slash == std::string::npos
			? std::string() : path.substr(0, slash + 1);

		return Parse(bytes.data(), bytes.size(), directory, out, error);
	}

}
