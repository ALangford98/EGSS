#include "gspch.h"
#include "GS/Network/Socket.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>

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
		m_Handle = socket(AF_INET, SOCK_DGRAM, 0);
		if (m_Handle < 0)
		{
			GS_CORE_ERROR("Socket::Bind: socket() failed");
			return false;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(port);

		if (bind(m_Handle, (sockaddr*)&address, sizeof(address)) < 0)
		{
			GS_CORE_ERROR("Socket::Bind: bind() failed for port {0}", port);
			Close();
			return false;
		}

		int flags = fcntl(m_Handle, F_GETFL, 0);
		fcntl(m_Handle, F_SETFL, flags | O_NONBLOCK);

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
			close(m_Handle);
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

		ssize_t sent = sendto(m_Handle, data, size, 0, (sockaddr*)&address, sizeof(address));
		return sent == (ssize_t)size;
	}

	int Socket::RecvFrom(uint8_t* buffer, size_t maxSize, NetAddress& fromOut)
	{
		sockaddr_in from{};
		socklen_t fromLen = sizeof(from);

		ssize_t received = recvfrom(m_Handle, buffer, maxSize, 0, (sockaddr*)&from, &fromLen);
		if (received < 0)
		{
			// EAGAIN/EWOULDBLOCK just means "nothing waiting" on a non-blocking
			// socket -- not a real error.
			return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
		}

		fromOut.IP = ntohl(from.sin_addr.s_addr);
		fromOut.Port = ntohs(from.sin_port);
		return (int)received;
	}

}
