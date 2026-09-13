#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"

namespace GS {

	// Host byte order throughout -- converted to/from network byte order only
	// at the socket boundary, in Socket.cpp.
	struct NetAddress
	{
		uint32_t IP = 0;
		uint16_t Port = 0;

		bool operator==(const NetAddress& other) const { return IP == other.IP && Port == other.Port; }
	};

	GS_API bool ParseAddress(const std::string& text, NetAddress& out);

	// A non-blocking UDP socket. Linux/POSIX only.
	class GS_API Socket
	{
	public:
		Socket() = default;
		~Socket();
		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;

		bool Bind(uint16_t port);   // 0 = let the OS choose an ephemeral port
		void Close();

		bool SendTo(const NetAddress& to, const uint8_t* data, size_t size);

		// Returns bytes read, 0 if nothing is waiting, -1 on error.
		int RecvFrom(uint8_t* buffer, size_t maxSize, NetAddress& fromOut);

		uint16_t LocalPort() const { return m_LocalPort; }

	private:
		int m_Handle = -1;
		uint16_t m_LocalPort = 0;
	};

}
