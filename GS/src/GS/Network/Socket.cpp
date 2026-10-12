#include "gspch.h"
#include "GS/Network/Socket.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>

// BSD sockets on Linux, Winsock on Windows. The two differ only at the edges
// this block papers over: how a socket is closed and made non-blocking, the
// buffer pointer types, the error code for "nothing to read", and Winsock's
// need to be started once before any socket exists.
#ifdef GS_PLATFORM_WINDOWS
	#include <winsock2.h>
	#include <ws2tcpip.h>
	namespace {
		using ssize_t = long long;
		using socklen_t = int;
		// Once per process, before the first socket: Winsock refuses every
		// call until WSAStartup has run.
		void EnsureWinsock()
		{
			static bool started = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
			(void)started;
		}
		void CloseHandle(intptr_t handle) { closesocket((SOCKET)handle); }
		void MakeNonBlocking(intptr_t handle) { u_long on = 1; ioctlsocket((SOCKET)handle, FIONBIO, &on); }
		// WSAECONNRESET on a UDP receive is Windows reporting an ICMP "port
		// unreachable" for an earlier send -- meaningless for connectionless
		// UDP, and treated as an error it would make a client's socket look
		// broken whenever its server restarts. It is "nothing to read".
		bool NothingToRead()
		{
			int error = WSAGetLastError();
			return error == WSAEWOULDBLOCK || error == WSAECONNRESET;
		}
	}
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <unistd.h>
	namespace {
		void EnsureWinsock() {}
		void CloseHandle(intptr_t handle) { close((int)handle); }
		void MakeNonBlocking(intptr_t handle)
		{
			int flags = fcntl((int)handle, F_GETFL, 0);
			fcntl((int)handle, F_SETFL, flags | O_NONBLOCK);
		}
		// EAGAIN/EWOULDBLOCK just means "nothing waiting" on a non-blocking
		// socket -- not a real error.
		bool NothingToRead() { return errno == EAGAIN || errno == EWOULDBLOCK; }
	}
#endif

namespace GS {

	bool ParseAddress(const std::string& text, NetAddress& out)
	{
		size_t colon = text.find(':');
		if (colon == std::string::npos)
			return false;

		std::string host = text.substr(0, colon);
		std::string portText = text.substr(colon + 1);

		in_addr addr{};
		if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
			return false;

		out.IP = ntohl(addr.s_addr);
		out.Port = (uint16_t)std::atoi(portText.c_str());
		return out.Port != 0;
	}

	Socket::~Socket()
	{
		Close();
	}

	bool Socket::Bind(uint16_t port)
	{
		EnsureWinsock();
		m_Handle = (intptr_t)socket(AF_INET, SOCK_DGRAM, 0);
		if (m_Handle < 0)
		{
			GS_CORE_ERROR("Socket::Bind: socket() failed");
			return false;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(port);

		if (bind(m_Handle, (sockaddr*)&address, sizeof(address)) != 0)
		{
			GS_CORE_ERROR("Socket::Bind: bind() failed for port {0}", port);
			Close();
			return false;
		}

		MakeNonBlocking(m_Handle);

		sockaddr_in actual{};
		socklen_t actualLen = sizeof(actual);
		getsockname(m_Handle, (sockaddr*)&actual, &actualLen);
		m_LocalPort = ntohs(actual.sin_port);

		return true;
	}

	void Socket::Close()
	{
		if (m_Handle >= 0)
		{
			CloseHandle(m_Handle);
			m_Handle = -1;
			m_LocalPort = 0;
		}
	}

	bool Socket::SendTo(const NetAddress& to, const uint8_t* data, size_t size)
	{
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(to.IP);
		address.sin_port = htons(to.Port);

		ssize_t sent = sendto(m_Handle, (const char*)data, (int)size, 0, (sockaddr*)&address, sizeof(address));
		return sent == (ssize_t)size;
	}

	int Socket::RecvFrom(uint8_t* buffer, size_t maxSize, NetAddress& fromOut)
	{
		sockaddr_in from{};
		socklen_t fromLen = sizeof(from);

		ssize_t received = recvfrom(m_Handle, (char*)buffer, (int)maxSize, 0, (sockaddr*)&from, &fromLen);
		if (received < 0)
			return NothingToRead() ? 0 : -1;

		fromOut.IP = ntohl(from.sin_addr.s_addr);
		fromOut.Port = ntohs(from.sin_port);
		return (int)received;
	}

}
