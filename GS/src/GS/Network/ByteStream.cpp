#include "gspch.h"
#include "GS/Network/ByteStream.h"

namespace GS {

	ByteStream::ByteStream(const uint8_t* data, size_t size)
		: m_Buffer(data, data + size)
	{
	}

	void ByteStream::WriteU8(uint8_t v) { WriteRaw(v); }
	void ByteStream::WriteU16(uint16_t v) { WriteRaw(v); }
	void ByteStream::WriteU32(uint32_t v) { WriteRaw(v); }
	void ByteStream::WriteFloat(float v) { WriteRaw(v); }

	void ByteStream::WriteString(const std::string& v)
	{
		WriteU16((uint16_t)v.size());
		m_Buffer.insert(m_Buffer.end(), v.begin(), v.end());
	}

	void ByteStream::WriteVec3(const glm::vec3& v)
	{
		WriteFloat(v.x);
		WriteFloat(v.y);
		WriteFloat(v.z);
	}

	uint8_t ByteStream::ReadU8() { return ReadRaw<uint8_t>(); }
	uint16_t ByteStream::ReadU16() { return ReadRaw<uint16_t>(); }
	uint32_t ByteStream::ReadU32() { return ReadRaw<uint32_t>(); }
	float ByteStream::ReadFloat() { return ReadRaw<float>(); }

	std::string ByteStream::ReadString()
	{
		uint16_t length = ReadU16();
		GS_CORE_ASSERT(m_ReadOffset + length <= m_Buffer.size(), "ByteStream string read past end");
		std::string v(reinterpret_cast<const char*>(m_Buffer.data() + m_ReadOffset), length);
		m_ReadOffset += length;
		return v;
	}

	glm::vec3 ByteStream::ReadVec3()
	{
		float x = ReadFloat();
		float y = ReadFloat();
		float z = ReadFloat();
		return { x, y, z };
	}

}
