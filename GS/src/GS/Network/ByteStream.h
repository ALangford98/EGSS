#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"

#include <glm/glm.hpp>

namespace GS {

	// A small binary writer/reader used for every message and packet this
	// module builds. Deliberately not endian-safe: this project runs on one
	// platform (Linux x86_64) on both ends of a connection, so there is no
	// second byte order to reconcile against.
	class GS_API ByteStream
	{
	public:
		ByteStream() = default;
		ByteStream(const uint8_t* data, size_t size);

		void WriteU8(uint8_t v);
		void WriteU16(uint16_t v);
		void WriteU32(uint32_t v);
		void WriteFloat(float v);
		void WriteString(const std::string& v);
		void WriteVec3(const glm::vec3& v);

		uint8_t ReadU8();
		uint16_t ReadU16();
		uint32_t ReadU32();
		float ReadFloat();
		std::string ReadString();
		glm::vec3 ReadVec3();

		void Skip(size_t count) { m_ReadOffset += count; }

		const uint8_t* Data() const { return m_Buffer.data(); }
		size_t Size() const { return m_Buffer.size(); }
		size_t ReadOffset() const { return m_ReadOffset; }
		bool AtEnd() const { return m_ReadOffset >= m_Buffer.size(); }

	private:
		template<typename T>
		void WriteRaw(T v) {
			const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&v);
			m_Buffer.insert(m_Buffer.end(), bytes, bytes + sizeof(T));
		}

		template<typename T>
		T ReadRaw() {
			GS_CORE_ASSERT(m_ReadOffset + sizeof(T) <= m_Buffer.size(), "ByteStream read past end");
			T v;
			std::memcpy(&v, m_Buffer.data() + m_ReadOffset, sizeof(T));
			m_ReadOffset += sizeof(T);
			return v;
		}

		std::vector<uint8_t> m_Buffer;
		size_t m_ReadOffset = 0;
	};

}
