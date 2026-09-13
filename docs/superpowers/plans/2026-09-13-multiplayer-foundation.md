# Multiplayer Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give GS a from-scratch UDP networking foundation -- transport, a
custom reliable channel, a client-server session model, opt-in entity
replication, and manual RPCs -- usable from game code and from the editor.

**Architecture:** Host-as-server: one `TestEnv` process runs `NetServer`
(the authority), others run `NetClient` and connect to it over UDP.
`NetConnection` implements one small reliable-ordered channel (sequence
numbers + an ack bitfield, resend-on-timeout) shared by both roles.
`NetworkIdentity`/`NetworkTransform` are opt-in `Scene` components;
`NetReplication` is the only code that knows about both `Scene` and the
network layer, keeping `NetServer`/`NetClient` scene-agnostic. Everything
else (score, events) goes through a small manual RPC registry.

**Tech Stack:** C++17, POSIX BSD sockets (`<sys/socket.h>`, `<netinet/in.h>`,
`<arpa/inet.h>`, `<fcntl.h>`) -- Linux only, matching the project's current
platform. No new vendored dependency.

**Spec:** `docs/superpowers/specs/2026-09-13-multiplayer-foundation-design.md`

## Global Constraints

- Linux/POSIX only -- no Windows socket shim (`Socket` uses raw `<sys/
  socket.h>`, not `SDL_net`/`WinSock2`).
- Max UDP payload per packet: 1200 bytes (`Protocol::kMaxPacketSize`) --
  stay under common MTU minus headers; a single message larger than this
  is unsupported in this version (assert, don't silently truncate).
- Resend interval 250 ms, keepalive interval 1 Hz, connection timeout 5 s
  (`NetConnection::kResendInterval` / `kKeepAliveInterval` /
  `kTimeoutSeconds`) -- exact values from the spec, don't retune without
  updating it.
- `TransformComponent` (Position, Rotation) is the only thing that
  auto-replicates; Scale is not sent. Everything else is a manual RPC.
- No client-side prediction/reconciliation in this project -- a client's
  own networked entity only moves when a server snapshot arrives. Input is
  sent every fixed step regardless, so prediction can be added later
  without a protocol change.
- Every network send happens from `OnFixedUpdate`, matching the project's
  existing "anything that moves belongs in OnFixedUpdate" rule.
- Tests follow this project's self-test pattern (temporary `TEMPORARY --
  delete after verifying X` headers with `Check`/`Run`, called from
  `TestApp`'s constructor, deleted once verified) -- there is no test
  framework and the project does not want one.

---

## Task 1: ByteStream

**Files:**
- Create: `GS/src/GS/Network/ByteStream.h`
- Create: `GS/src/GS/Network/ByteStream.cpp`
- Test: `TestEnv/src/NetTests/ByteStreamTest.h` (temporary)

**Interfaces:**
- Produces: `GS::ByteStream` -- write-mode default constructor, read-mode
  `ByteStream(const uint8_t* data, size_t size)` (copies `data` into its
  own buffer); `WriteU8/U16/U32/Float/String/Vec3`, matching
  `ReadU8/U16/U32/Float/String/Vec3`; `Skip(size_t n)`; `Data()`, `Size()`,
  `ReadOffset()`, `AtEnd()`.

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/ByteStreamTest.h
// TEMPORARY -- delete after verifying ByteStream round-trips every type it
// supports, in order, in one buffer.
#pragma once
#include <GS.h>
#include <GS/Network/ByteStream.h>

namespace ByteStreamTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::ByteStream out;
		out.WriteU8(200);
		out.WriteU16(60000);
		out.WriteU32(4000000000u);
		out.WriteFloat(3.5f);
		out.WriteString("hello");
		out.WriteVec3({ 1.0f, -2.5f, 0.0f });

		GS::ByteStream in(out.Data(), out.Size());
		Check(in.ReadU8() == 200, "u8 round-trips");
		Check(in.ReadU16() == 60000, "u16 round-trips");
		Check(in.ReadU32() == 4000000000u, "u32 round-trips");
		Check(in.ReadFloat() == 3.5f, "float round-trips");
		Check(in.ReadString() == "hello", "string round-trips");
		glm::vec3 v = in.ReadVec3();
		Check(v.x == 1.0f && v.y == -2.5f && v.z == 0.0f, "vec3 round-trips");
		Check(in.AtEnd(), "read cursor consumes exactly what was written");

		GS::ByteStream skip;
		skip.WriteU32(111);
		skip.WriteU32(222);
		GS::ByteStream skipIn(skip.Data(), skip.Size());
		skipIn.Skip(4);
		Check(skipIn.ReadU32() == 222, "Skip advances past a field's bytes");

		GS_TRACE("ByteStreamTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire it into `TestEnv/src/TestApp.cpp`'s constructor, right after the
existing setup, with a matching include added there.

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails to compile (`GS/Network/
ByteStream.h` does not exist yet).

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/ByteStream.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"

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
```

```cpp
// GS/src/GS/Network/ByteStream.cpp
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 5 ./gs.py run -- --hide-window`
Expected: console output includes `ByteStreamTest: 8 passed, 0 failed` and
no `[FAIL]` lines from `ByteStreamTest`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/ByteStream.h GS/src/GS/Network/ByteStream.cpp \
        TestEnv/src/NetTests/ByteStreamTest.h TestEnv/src/TestApp.cpp
git commit -m "Add ByteStream, the binary reader/writer networking builds on"
```

---

## Task 2: Socket

**Files:**
- Create: `GS/src/GS/Network/Socket.h`
- Create: `GS/src/GS/Network/Socket.cpp`
- Test: `TestEnv/src/NetTests/SocketTest.h` (temporary)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `GS::NetAddress {uint32_t IP; uint16_t Port;}`,
  `GS::ParseAddress(const std::string& "host:port", NetAddress& out) ->
  bool`, `GS::Socket` with `Bind(uint16_t port) -> bool`,
  `SendTo(const NetAddress&, const uint8_t*, size_t) -> bool`,
  `RecvFrom(uint8_t* buffer, size_t maxSize, NetAddress& fromOut) -> int`
  (0 = nothing waiting, >0 = bytes read, <0 = error), `LocalPort() ->
  uint16_t`.

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/SocketTest.h
// TEMPORARY -- delete after verifying two real UDP sockets on loopback can
// send and receive.
#pragma once
#include <GS.h>
#include <GS/Network/Socket.h>

namespace SocketTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::NetAddress parsed;
		Check(GS::ParseAddress("127.0.0.1:9001", parsed), "ParseAddress accepts host:port");
		Check(parsed.Port == 9001, "ParseAddress reads the port");

		GS::Socket a, b;
		Check(a.Bind(0), "socket a binds an ephemeral port");
		Check(b.Bind(0), "socket b binds an ephemeral port");
		Check(a.LocalPort() != 0 && b.LocalPort() != 0, "both got a real port");

		GS::NetAddress toB{ parsed.IP, b.LocalPort() };
		const char* msg = "ping";
		Check(a.SendTo(toB, (const uint8_t*)msg, 4), "SendTo succeeds");

		uint8_t buffer[64];
		GS::NetAddress from;
		int received = 0;
		// Loopback delivery is effectively immediate but not synchronous with
		// the syscall that sent it; a short bounded retry avoids a flaky test
		// without a real sleep-based race.
		for (int attempt = 0; attempt < 1000 && received == 0; attempt++)
			received = b.RecvFrom(buffer, sizeof(buffer), from);

		Check(received == 4, "b receives exactly what a sent");
		Check(std::memcmp(buffer, "ping", 4) == 0, "payload bytes match");
		Check(from.Port == a.LocalPort(), "sender address round-trips");

		int nothing = b.RecvFrom(buffer, sizeof(buffer), from);
		Check(nothing == 0, "RecvFrom returns 0 when nothing is waiting");

		GS_TRACE("SocketTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp`'s constructor alongside `ByteStreamTest::Run()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails to compile, `GS/Network/Socket.h`
missing.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/Socket.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"

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
```

```cpp
// GS/src/GS/Network/Socket.cpp
#include "gspch.h"
#include "GS/Network/Socket.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 5 ./gs.py run -- --hide-window`
Expected: `SocketTest: 8 passed, 0 failed`, no `[FAIL]` lines.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/Socket.h GS/src/GS/Network/Socket.cpp \
        TestEnv/src/NetTests/SocketTest.h TestEnv/src/TestApp.cpp
git commit -m "Add Socket, a non-blocking POSIX UDP wrapper"
```

---

## Task 3: Protocol

**Files:**
- Create: `GS/src/GS/Network/Protocol.h`
- Create: `GS/src/GS/Network/Protocol.cpp`
- Test: `TestEnv/src/NetTests/ProtocolTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::ByteStream` (Task 1).
- Produces: `GS::Protocol::kProtocolVersion`, `kMaxPacketSize`,
  `PacketHeader {uint16_t Sequence; uint16_t Ack; uint32_t AckBits;}`,
  `WriteHeader(ByteStream&, const PacketHeader&)`,
  `ReadHeader(ByteStream&) -> PacketHeader`,
  `IsMoreRecent(uint16_t a, uint16_t b) -> bool`.

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/ProtocolTest.h
// TEMPORARY -- delete after verifying sequence wraparound comparison and
// header round-tripping.
#pragma once
#include <GS.h>
#include <GS/Network/Protocol.h>
#include <GS/Network/ByteStream.h>

namespace ProtocolTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		using namespace GS::Protocol;

		Check(IsMoreRecent(5, 3), "5 is more recent than 3");
		Check(!IsMoreRecent(3, 5), "3 is not more recent than 5");
		Check(IsMoreRecent(0, 65535), "0 is more recent than 65535 (wraparound)");
		Check(!IsMoreRecent(65535, 0), "65535 is not more recent than 0 (wraparound)");
		Check(!IsMoreRecent(10, 10), "a sequence is not more recent than itself");

		PacketHeader header{ 42, 41, 0b101 };
		GS::ByteStream out;
		WriteHeader(out, header);
		GS::ByteStream in(out.Data(), out.Size());
		PacketHeader read = ReadHeader(in);
		Check(read.Sequence == 42 && read.Ack == 41 && read.AckBits == 0b101,
			"header round-trips through a ByteStream");

		GS_TRACE("ProtocolTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp` alongside the earlier two.

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS/Network/Protocol.h` missing.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/Protocol.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Network/ByteStream.h"

namespace GS::Protocol {

	constexpr uint16_t kProtocolVersion = 1;
	// Comfortably under a common 1500-byte MTU minus IP/UDP headers -- avoids
	// IP fragmentation. A single message bigger than this is not supported;
	// nothing this project sends comes close.
	constexpr size_t kMaxPacketSize = 1200;

	struct PacketHeader
	{
		uint16_t Sequence = 0;
		uint16_t Ack = 0;
		uint32_t AckBits = 0;
	};

	GS_API void WriteHeader(ByteStream& out, const PacketHeader& header);
	GS_API PacketHeader ReadHeader(ByteStream& in);

	// True if `a` is more recent than `b`, in the presence of uint16
	// wraparound -- the standard TCP-style comparison: two sequences within
	// half the numeric range of each other are ordered normally; further
	// apart than that is treated as a wrap.
	GS_API bool IsMoreRecent(uint16_t a, uint16_t b);

}
```

```cpp
// GS/src/GS/Network/Protocol.cpp
#include "gspch.h"
#include "GS/Network/Protocol.h"

namespace GS::Protocol {

	void WriteHeader(ByteStream& out, const PacketHeader& header)
	{
		out.WriteU16(header.Sequence);
		out.WriteU16(header.Ack);
		out.WriteU32(header.AckBits);
	}

	PacketHeader ReadHeader(ByteStream& in)
	{
		PacketHeader header;
		header.Sequence = in.ReadU16();
		header.Ack = in.ReadU16();
		header.AckBits = in.ReadU32();
		return header;
	}

	bool IsMoreRecent(uint16_t a, uint16_t b)
	{
		return (a != b) &&
			(((a > b) && (uint16_t)(a - b) <= 32768) ||
			 ((a < b) && (uint16_t)(b - a) > 32768));
	}

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 5 ./gs.py run -- --hide-window`
Expected: `ProtocolTest: 6 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/Protocol.h GS/src/GS/Network/Protocol.cpp \
        TestEnv/src/NetTests/ProtocolTest.h TestEnv/src/TestApp.cpp
git commit -m "Add Protocol: packet header and sequence wraparound comparison"
```

---

## Task 4: NetConnection

This is the core reliability mechanism: sequencing, the ack bitfield, and
the resend-on-timeout loop for the reliable channel. Both `NetServer` and
`NetClient` are built on this.

**Files:**
- Create: `GS/src/GS/Network/NetConnection.h`
- Create: `GS/src/GS/Network/NetConnection.cpp`
- Test: `TestEnv/src/NetTests/NetConnectionTest.h` (temporary; extended
  again in Task 5)

**Interfaces:**
- Consumes: `GS::Socket`, `GS::NetAddress` (Task 2); `GS::Protocol::*`
  (Task 3); `GS::ByteStream` (Task 1).
- Produces: `GS::Reliability {Unreliable, Reliable}`, `GS::NetConnection`
  with: constructor `(Socket* socket, const NetAddress& remote)`;
  `QueueMessage(uint32_t typeId, const uint8_t* payload, size_t size,
  Reliability)`; `Update(float deltaSeconds)`; `Flush(float
  deltaSeconds)`; `ReceivePacket(const uint8_t* data, size_t size, const
  MessageCallback& onMessage)` where `MessageCallback =
  std::function<void(uint32_t typeId, ByteStream& payload)>`;
  `IsTimedOut() -> bool`; `GetRTT() -> float`; `GetRemoteAddress() ->
  const NetAddress&`; `SimulateLoss(float probability)` (test/debug only,
  0 in production).

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/NetConnectionTest.h
// TEMPORARY -- delete after verifying NetConnection's reliable channel
// delivers exactly-once, in order, over a real loopback socket pair, with
// and without simulated loss.
#pragma once
#include <GS.h>
#include <GS/Network/NetConnection.h>
#include <GS/Network/Socket.h>

namespace NetConnectionTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	// Pumps two real, loopback-connected NetConnections against each other
	// for `steps` fixed steps of `dt` seconds, feeding whatever arrives on
	// each socket into the matching connection's ReceivePacket.
	inline void PumpBothWays(GS::Socket& socketA, GS::NetConnection& a,
		GS::Socket& socketB, GS::NetConnection& b, int steps, float dt,
		const GS::NetConnection::MessageCallback& onA,
		const GS::NetConnection::MessageCallback& onB)
	{
		for (int i = 0; i < steps; i++)
		{
			a.Update(dt);
			b.Update(dt);
			a.Flush(dt);
			b.Flush(dt);

			uint8_t buffer[2048];
			GS::NetAddress from;
			int received;
			while ((received = socketB.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				b.ReceivePacket(buffer, received, onB);
			while ((received = socketA.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				a.ReceivePacket(buffer, received, onA);
		}
	}

	inline void RunReliableDeliveryUnderLoss(float lossProbability)
	{
		GS::Socket socketA, socketB;
		socketA.Bind(0);
		socketB.Bind(0);

		GS::NetAddress toB{ 0x7F000001u, socketB.LocalPort() };
		GS::NetAddress toA{ 0x7F000001u, socketA.LocalPort() };

		GS::NetConnection a(&socketA, toB);
		GS::NetConnection b(&socketB, toA);
		a.SimulateLoss(lossProbability);
		b.SimulateLoss(lossProbability);

		std::vector<uint32_t> received;
		auto onB = [&](uint32_t typeId, GS::ByteStream&) { received.push_back(typeId); };
		auto onA = [](uint32_t, GS::ByteStream&) {};

		for (uint32_t i = 0; i < 10; i++)
		{
			uint8_t payload[1] = { 0 };
			a.QueueMessage(1000 + i, payload, 1, GS::Reliability::Reliable);
		}

		// 40 steps at 1/20s (2s of simulated time) gives the 250ms resend
		// timer several chances to recover from 30% loss.
		PumpBothWays(socketA, a, socketB, b, 40, 1.0f / 20.0f, onA, onB);

		Check(received.size() == 10, "all 10 reliable messages arrived exactly once (loss=" + std::to_string(lossProbability) + ")");
		bool inOrder = true;
		for (uint32_t i = 0; i < received.size(); i++)
			if (received[i] != 1000 + i)
				inOrder = false;
		Check(inOrder, "reliable messages arrived in the order sent");
	}

	inline void Run() {
		RunReliableDeliveryUnderLoss(0.0f);
		RunReliableDeliveryUnderLoss(0.3f);

		GS_TRACE("NetConnectionTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp` alongside the earlier three.

**Note on ordering:** this first version of the test does not yet assert
strict delivery order under packet loss with reordering -- `Flush`
sends one packet per connection per step, and loopback UDP does not
reorder in practice, so "arrived in the order sent" holds for this
harness. Task 5 extends this same file with the RTT-accuracy check.

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS/Network/NetConnection.h`
missing.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/NetConnection.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Network/Socket.h"
#include "GS/Network/Protocol.h"
#include "GS/Network/ByteStream.h"

namespace GS {

	enum class Reliability : uint8_t { Unreliable = 0, Reliable = 1 };

	// One peer connection's sequencing, ack tracking, RTT estimate, timeout,
	// and reliable-message resend buffer. Both NetServer (one per connected
	// client) and NetClient (one, for the server) are built on this.
	class GS_API NetConnection
	{
	public:
		using MessageCallback = std::function<void(uint32_t typeId, ByteStream& payload)>;

		static constexpr float kResendInterval = 0.25f;
		static constexpr float kKeepAliveInterval = 1.0f;
		static constexpr float kTimeoutSeconds = 5.0f;

		NetConnection(Socket* socket, const NetAddress& remote);

		void QueueMessage(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);

		// Advances internal clocks. Call once per fixed step, before Flush.
		void Update(float deltaSeconds);

		// Sends queued unreliable messages plus any reliable message that is
		// new or overdue for a resend. A no-op if there is nothing to send
		// and the keepalive interval hasn't elapsed.
		void Flush(float deltaSeconds);

		// Feed every packet RecvFrom hands back for this connection's remote
		// address. Invokes onMessage once per message the packet carries.
		void ReceivePacket(const uint8_t* data, size_t size, const MessageCallback& onMessage);

		bool IsTimedOut() const { return m_TimeSinceLastReceive >= kTimeoutSeconds; }
		float GetRTT() const { return m_RTTSeconds; }
		const NetAddress& GetRemoteAddress() const { return m_Remote; }

		// Test/debug only: drop a fraction of outgoing packets before they
		// reach the socket, to exercise the resend path. 0 (the default) is a
		// no-op in production.
		void SimulateLoss(float probability) { m_SimulatedLossProbability = probability; }

	private:
		struct QueuedMessage
		{
			uint32_t TypeId;
			std::vector<uint8_t> Payload;
		};

		struct PendingReliable
		{
			uint32_t TypeId;
			std::vector<uint8_t> Payload;
			float TimeSinceLastSend = 0.0f;
			bool EverSent = false;
		};

		void SendPacket(const std::vector<QueuedMessage>& unreliable, const std::vector<uint32_t>& dueReliableIds);
		void ProcessAck(uint16_t ack, uint32_t ackBits);
		void ConfirmSequence(uint16_t sequence);
		void MarkReceived(uint16_t sequence);

		Socket* m_Socket;
		NetAddress m_Remote;

		uint16_t m_LocalSequence = 0;
		uint16_t m_RemoteSequence = 0;
		uint32_t m_ReceivedBits = 0;
		bool m_HasReceivedAny = false;

		uint32_t m_NextMessageId = 1;
		std::unordered_map<uint32_t, PendingReliable> m_OutgoingReliable;
		std::unordered_map<uint16_t, std::vector<uint32_t>> m_SentPacketContents;
		std::unordered_map<uint16_t, float> m_SentPacketTime;

		std::vector<QueuedMessage> m_OutgoingUnreliable;

		float m_ClockSeconds = 0.0f;
		float m_TimeSinceLastSend = 0.0f;
		float m_TimeSinceLastReceive = 0.0f;
		float m_RTTSeconds = 0.0f;
		float m_SimulatedLossProbability = 0.0f;
	};

}
```

```cpp
// GS/src/GS/Network/NetConnection.cpp
#include "gspch.h"
#include "GS/Network/NetConnection.h"

#include <cstdlib>

namespace GS {

	NetConnection::NetConnection(Socket* socket, const NetAddress& remote)
		: m_Socket(socket), m_Remote(remote)
	{
	}

	void NetConnection::QueueMessage(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		std::vector<uint8_t> bytes(payload, payload + size);

		if (reliability == Reliability::Unreliable)
		{
			m_OutgoingUnreliable.push_back({ typeId, std::move(bytes) });
			return;
		}

		uint32_t id = m_NextMessageId++;
		PendingReliable message;
		message.TypeId = typeId;
		message.Payload = std::move(bytes);
		m_OutgoingReliable.emplace(id, std::move(message));
	}

	void NetConnection::Update(float deltaSeconds)
	{
		m_ClockSeconds += deltaSeconds;
		m_TimeSinceLastReceive += deltaSeconds;
	}

	void NetConnection::Flush(float deltaSeconds)
	{
		m_TimeSinceLastSend += deltaSeconds;

		std::vector<uint32_t> due;
		for (auto& [id, message] : m_OutgoingReliable)
		{
			message.TimeSinceLastSend += deltaSeconds;
			if (!message.EverSent || message.TimeSinceLastSend >= kResendInterval)
				due.push_back(id);
		}

		bool hasContent = !m_OutgoingUnreliable.empty() || !due.empty();
		bool keepAliveDue = m_TimeSinceLastSend >= kKeepAliveInterval;
		if (!hasContent && !keepAliveDue)
			return;

		SendPacket(m_OutgoingUnreliable, due);
		m_OutgoingUnreliable.clear();
		m_TimeSinceLastSend = 0.0f;
	}

	void NetConnection::SendPacket(const std::vector<QueuedMessage>& unreliable, const std::vector<uint32_t>& dueReliableIds)
	{
		uint16_t sequence = m_LocalSequence++;

		ByteStream out;
		Protocol::WriteHeader(out, { sequence, m_RemoteSequence, m_ReceivedBits });

		for (uint32_t id : dueReliableIds)
		{
			PendingReliable& message = m_OutgoingReliable.at(id);
			out.WriteU8((uint8_t)Reliability::Reliable);
			out.WriteU32(message.TypeId);
			out.WriteU16((uint16_t)message.Payload.size());
			for (uint8_t byte : message.Payload) out.WriteU8(byte);

			message.TimeSinceLastSend = 0.0f;
			message.EverSent = true;
		}

		for (const QueuedMessage& message : unreliable)
		{
			out.WriteU8((uint8_t)Reliability::Unreliable);
			out.WriteU32(message.TypeId);
			out.WriteU16((uint16_t)message.Payload.size());
			for (uint8_t byte : message.Payload) out.WriteU8(byte);
		}

		GS_CORE_ASSERT(out.Size() <= Protocol::kMaxPacketSize,
			"NetConnection packet exceeds kMaxPacketSize -- message too large for this version");

		m_SentPacketContents[sequence] = dueReliableIds;
		m_SentPacketTime[sequence] = m_ClockSeconds;

		bool drop = m_SimulatedLossProbability > 0.0f
			&& ((float)rand() / (float)RAND_MAX) < m_SimulatedLossProbability;
		if (!drop)
			m_Socket->SendTo(m_Remote, out.Data(), out.Size());
	}

	void NetConnection::ReceivePacket(const uint8_t* data, size_t size, const MessageCallback& onMessage)
	{
		ByteStream in(data, size);
		Protocol::PacketHeader header = Protocol::ReadHeader(in);

		m_TimeSinceLastReceive = 0.0f;
		MarkReceived(header.Sequence);
		ProcessAck(header.Ack, header.AckBits);

		while (!in.AtEnd())
		{
			in.ReadU8(); // reliability tag -- delivery order already handles both alike here
			uint32_t typeId = in.ReadU32();
			uint16_t length = in.ReadU16();
			ByteStream payload(in.Data() + in.ReadOffset(), length);
			in.Skip(length);
			onMessage(typeId, payload);
		}
	}

	void NetConnection::MarkReceived(uint16_t sequence)
	{
		if (!m_HasReceivedAny)
		{
			m_RemoteSequence = sequence;
			m_ReceivedBits = 0;
			m_HasReceivedAny = true;
			return;
		}

		if (Protocol::IsMoreRecent(sequence, m_RemoteSequence))
		{
			uint16_t shift = (uint16_t)(sequence - m_RemoteSequence);
			m_ReceivedBits = (shift <= 32)
				? ((shift == 32 ? 0u : (m_ReceivedBits << shift)) | (1u << (shift - 1)))
				: 0u;
			m_RemoteSequence = sequence;
		}
		else
		{
			uint16_t age = (uint16_t)(m_RemoteSequence - sequence);
			if (age >= 1 && age <= 32)
				m_ReceivedBits |= (1u << (age - 1));
		}
	}

	void NetConnection::ProcessAck(uint16_t ack, uint32_t ackBits)
	{
		ConfirmSequence(ack);
		for (uint32_t i = 0; i < 32; i++)
			if (ackBits & (1u << i))
				ConfirmSequence((uint16_t)(ack - 1 - i));
	}

	void NetConnection::ConfirmSequence(uint16_t sequence)
	{
		auto timeIt = m_SentPacketTime.find(sequence);
		if (timeIt != m_SentPacketTime.end())
		{
			float rtt = m_ClockSeconds - timeIt->second;
			m_RTTSeconds = (m_RTTSeconds <= 0.0f) ? rtt : (m_RTTSeconds * 0.9f + rtt * 0.1f);
			m_SentPacketTime.erase(timeIt);
		}

		auto contentsIt = m_SentPacketContents.find(sequence);
		if (contentsIt == m_SentPacketContents.end())
			return;

		for (uint32_t id : contentsIt->second)
			m_OutgoingReliable.erase(id);

		m_SentPacketContents.erase(contentsIt);
	}

}
```

Note the `shift == 32` special-case in `MarkReceived`: shifting a 32-bit
value left by 32 is undefined behaviour in C++, not "shifts to zero" --
it has to be handled explicitly rather than relying on `<< 32` doing
what it looks like it does.

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetConnectionTest: 4 passed, 0 failed` (two checks per loss
level, two loss levels).

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/NetConnection.h GS/src/GS/Network/NetConnection.cpp \
        TestEnv/src/NetTests/NetConnectionTest.h TestEnv/src/TestApp.cpp
git commit -m "Add NetConnection: reliable-ordered channel with ack bitfields"
```

---

## Task 5: NetConnection RTT accuracy test

Separate task from Task 4 so that task's review isn't blocked on getting a
timed, delayed-responder harness right too -- this one only adds a check,
no new production code.

**Files:**
- Modify: `TestEnv/src/NetTests/NetConnectionTest.h`

**Interfaces:**
- Consumes: `GS::NetConnection::GetRTT()` (Task 4).

- [ ] **Step 1: Write the failing test**

Add to `NetConnectionTest.h`:

```cpp
	inline void RunRTTAccuracy()
	{
		GS::Socket socketA, socketB;
		socketA.Bind(0);
		socketB.Bind(0);

		GS::NetAddress toB{ 0x7F000001u, socketB.LocalPort() };
		GS::NetAddress toA{ 0x7F000001u, socketA.LocalPort() };

		GS::NetConnection a(&socketA, toB);
		GS::NetConnection b(&socketB, toA);

		// B answers every reliable message from A one full step later --
		// with a fixed step of 0.1s that is a known, injected 100ms of
		// round-trip delay to measure against.
		const float dt = 0.1f;
		bool pending = false;

		auto onA = [](uint32_t, GS::ByteStream&) {};
		auto onB = [&](uint32_t typeId, GS::ByteStream&) { (void)typeId; pending = true; };

		uint8_t payload[1] = { 0 };
		a.QueueMessage(2000, payload, 1, GS::Reliability::Reliable);

		for (int i = 0; i < 20; i++)
		{
			a.Update(dt);
			b.Update(dt);

			if (pending)
			{
				pending = false;
				b.QueueMessage(2001, payload, 1, GS::Reliability::Reliable);
			}

			a.Flush(dt);
			b.Flush(dt);

			uint8_t buffer[2048];
			GS::NetAddress from;
			int received;
			while ((received = socketB.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				b.ReceivePacket(buffer, received, onB);
			while ((received = socketA.RecvFrom(buffer, sizeof(buffer), from)) > 0)
				a.ReceivePacket(buffer, received, onA);
		}

		// A's own message plus B's one-step-later reply is roughly 2 * dt of
		// round trip -- generous tolerance since this is measuring real
		// scheduling, not simulated time.
		float rtt = a.GetRTT();
		Check(rtt > 0.05f && rtt < 0.6f, "measured RTT (" + std::to_string(rtt) + "s) is in the expected ~0.2s range");
	}
```

And add the call in `Run()`:

```cpp
	inline void Run() {
		RunReliableDeliveryUnderLoss(0.0f);
		RunReliableDeliveryUnderLoss(0.3f);
		RunRTTAccuracy();

		GS_TRACE("NetConnectionTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
```

- [ ] **Step 2: Run test to verify it fails**

This one can't fail to compile (everything it uses already exists from
Task 4) -- run it once first to confirm the *assertion* is meaningful:
temporarily change the tolerance bounds to `rtt > 10.0f` (impossible),
run, confirm it prints `[FAIL]`, then put the real bounds back.

- [ ] **Step 3: (no implementation needed -- Task 4 already built GetRTT)**

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetConnectionTest: 5 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add TestEnv/src/NetTests/NetConnectionTest.h
git commit -m "Add an RTT-accuracy check to NetConnectionTest"
```

---

## Task 6: NetServer

**Files:**
- Create: `GS/src/GS/Network/NetServer.h`
- Create: `GS/src/GS/Network/NetServer.cpp`
- Test: `TestEnv/src/NetTests/NetServerClientTest.h` (temporary; extended
  in Task 7)

**Interfaces:**
- Consumes: `GS::Socket`, `GS::NetAddress`, `ParseAddress` (Task 2);
  `GS::Protocol::kProtocolVersion` (Task 3); `GS::NetConnection`,
  `GS::Reliability` (Task 4).
- Produces: `using GS::ClientId = uint16_t;`, `constexpr ClientId
  ServerOwned = 0;`, `constexpr ClientId InvalidClient = 0xFFFF;`,
  `GS::NetServer` with: `Host(uint16_t port, int maxClients = 8) -> bool`,
  `Shutdown()`, `Update(float deltaSeconds)`, `Broadcast(uint32_t typeId,
  const uint8_t*, size_t, Reliability)`, `SendTo(ClientId, uint32_t
  typeId, const uint8_t*, size_t, Reliability)`,
  `OnClientConnected(std::function<void(ClientId)>)`,
  `OnClientDisconnected(std::function<void(ClientId)>)`, `OnMessage(
  std::function<void(ClientId from, uint32_t typeId, ByteStream&)>)`,
  `IsHosting() -> bool`, `GetConnection(ClientId) -> NetConnection*`
  (nullptr if not connected), `GetConnectedClients() -> std::vector<
  ClientId>`.

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/NetServerClientTest.h
// TEMPORARY -- delete after verifying NetServer accepts a connection and
// exchanges a message with it over a real loopback socket.
#pragma once
#include <GS.h>
#include <GS/Network/NetServer.h>

namespace NetServerClientTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	// A bare, hand-built ConnectRequest packet -- this test only exercises
	// NetServer's accept path, not a real NetClient yet (Task 7 adds that
	// and a proper end-to-end check).
	inline void RunAcceptsAConnection()
	{
		GS::NetServer server;
		Check(server.Host(0), "server hosts on an ephemeral port");
		Check(server.IsHosting(), "IsHosting is true after Host");

		bool connected = false;
		GS::ClientId connectedId = GS::InvalidClient;
		server.OnClientConnected([&](GS::ClientId id) { connected = true; connectedId = id; });

		GS::Socket clientSocket;
		clientSocket.Bind(0);

		// The bare bytes a real handshake's first packet carries: a packet
		// header (all zero -- nothing to ack yet) followed by one reliable
		// message tagged as the connect-request type.
		GS::ByteStream request;
		GS::Protocol::WriteHeader(request, { 0, 0, 0 });
		request.WriteU8((uint8_t)GS::Reliability::Reliable);
		request.WriteU32(GS::Net::kMsgConnectRequest);
		request.WriteU16(2);
		request.WriteU16(GS::Protocol::kProtocolVersion);

		GS::NetAddress serverAddress{ 0x7F000001u, /*filled below*/ 0 };
		// NetServer::Host binds to `port`; when 0 was requested, discover the
		// real port the same way SocketTest does, through the server's own
		// accessor.
		serverAddress.Port = server.GetLocalPort();

		clientSocket.SendTo(serverAddress, request.Data(), request.Size());

		for (int i = 0; i < 200 && !connected; i++)
			server.Update(1.0f / 60.0f);

		Check(connected, "server's OnClientConnected fired");
		Check(connectedId != GS::InvalidClient, "a real ClientId was assigned");
		Check(server.GetConnectedClients().size() == 1, "one client is tracked as connected");

		server.Shutdown();
	}

	inline void Run() {
		RunAcceptsAConnection();
		GS_TRACE("NetServerClientTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

Wire into `TestApp.cpp`. This introduces two forward references the
implementation step below defines: `GS::Net::kMsgConnectRequest` and
`NetServer::GetLocalPort()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS/Network/NetServer.h` missing.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/NetServer.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Network/Socket.h"
#include "GS/Network/NetConnection.h"

namespace GS {

	using ClientId = uint16_t;
	constexpr ClientId ServerOwned = 0;
	constexpr ClientId InvalidClient = 0xFFFF;

	namespace Net {
		// Reserved, built-in message type IDs -- everything a game registers
		// through Net::RegisterHandler (Task 8) gets a hashed ID instead, so
		// these low, hand-picked values can never collide with one.
		constexpr uint32_t kMsgConnectRequest = 1;
		constexpr uint32_t kMsgConnectAccepted = 2;
		constexpr uint32_t kMsgConnectRejected = 3;
	}

	class GS_API NetServer
	{
	public:
		using ConnectedCallback = std::function<void(ClientId)>;
		using DisconnectedCallback = std::function<void(ClientId)>;
		using MessageCallback = std::function<void(ClientId from, uint32_t typeId, ByteStream& payload)>;

		bool Host(uint16_t port, int maxClients = 8);
		void Shutdown();

		// Pumps incoming packets, runs timeout checks, flushes every
		// connection. Call once per fixed step.
		void Update(float deltaSeconds);

		void Broadcast(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);
		void SendTo(ClientId client, uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);

		void OnClientConnected(ConnectedCallback cb) { m_OnConnected = std::move(cb); }
		void OnClientDisconnected(DisconnectedCallback cb) { m_OnDisconnected = std::move(cb); }
		void OnMessage(MessageCallback cb) { m_OnMessage = std::move(cb); }

		bool IsHosting() const { return m_Hosting; }
		uint16_t GetLocalPort() const { return m_Socket.LocalPort(); }
		NetConnection* GetConnection(ClientId client);
		std::vector<ClientId> GetConnectedClients() const;

	private:
		static uint64_t AddressKey(const NetAddress& address) { return ((uint64_t)address.IP << 16) | address.Port; }

		void HandleRawPacket(const uint8_t* data, size_t size, const NetAddress& from);

		Socket m_Socket;
		bool m_Hosting = false;
		int m_MaxClients = 8;
		ClientId m_NextClientId = 1; // 0 is ServerOwned -- never assigned to a real client

		std::unordered_map<ClientId, std::unique_ptr<NetConnection>> m_Clients;
		std::unordered_map<uint64_t, ClientId> m_AddressToClient;

		ConnectedCallback m_OnConnected;
		DisconnectedCallback m_OnDisconnected;
		MessageCallback m_OnMessage;
	};

}
```

```cpp
// GS/src/GS/Network/NetServer.cpp
#include "gspch.h"
#include "GS/Network/NetServer.h"

namespace GS {

	bool NetServer::Host(uint16_t port, int maxClients)
	{
		m_MaxClients = maxClients;
		m_Hosting = m_Socket.Bind(port);
		return m_Hosting;
	}

	void NetServer::Shutdown()
	{
		m_Socket.Close();
		m_Clients.clear();
		m_AddressToClient.clear();
		m_Hosting = false;
	}

	void NetServer::Update(float deltaSeconds)
	{
		if (!m_Hosting)
			return;

		uint8_t buffer[Protocol::kMaxPacketSize];
		NetAddress from;
		int received;
		while ((received = m_Socket.RecvFrom(buffer, sizeof(buffer), from)) > 0)
			HandleRawPacket(buffer, (size_t)received, from);

		std::vector<ClientId> timedOut;
		for (auto& [id, connection] : m_Clients)
		{
			connection->Update(deltaSeconds);
			if (connection->IsTimedOut())
				timedOut.push_back(id);
		}

		for (ClientId id : timedOut)
		{
			NetAddress address = m_Clients[id]->GetRemoteAddress();
			m_AddressToClient.erase(AddressKey(address));
			m_Clients.erase(id);
			if (m_OnDisconnected) m_OnDisconnected(id);
		}

		for (auto& [id, connection] : m_Clients)
			connection->Flush(deltaSeconds);
	}

	void NetServer::HandleRawPacket(const uint8_t* data, size_t size, const NetAddress& from)
	{
		uint64_t key = AddressKey(from);
		auto it = m_AddressToClient.find(key);

		if (it != m_AddressToClient.end())
		{
			NetConnection* connection = m_Clients.at(it->second).get();
			ClientId id = it->second;
			connection->ReceivePacket(data, size, [&](uint32_t typeId, ByteStream& payload)
			{
				if (m_OnMessage) m_OnMessage(id, typeId, payload);
			});
			return;
		}

		// Unknown address: the only thing it's allowed to say is a connect
		// request. Everything else from a stranger is silently ignored.
		ByteStream in(data, size);
		Protocol::ReadHeader(in); // sequencing is meaningless before a connection exists
		if (in.AtEnd())
			return;

		in.ReadU8(); // reliability tag
		uint32_t typeId = in.ReadU32();
		uint16_t length = in.ReadU16();
		if (typeId != Net::kMsgConnectRequest || length != 2)
			return;

		uint16_t version = in.ReadU16();

		if (version != Protocol::kProtocolVersion || (int)m_Clients.size() >= m_MaxClients)
		{
			// Rejected -- sent raw, since no NetConnection exists to carry it
			// reliably, and there is nothing to retry against a peer that
			// isn't going to become a client.
			ByteStream reject;
			Protocol::WriteHeader(reject, { 0, 0, 0 });
			reject.WriteU8((uint8_t)Reliability::Unreliable);
			reject.WriteU32(Net::kMsgConnectRejected);
			reject.WriteU16(0);
			m_Socket.SendTo(from, reject.Data(), reject.Size());
			return;
		}

		ClientId newId = m_NextClientId++;
		auto connection = std::make_unique<NetConnection>(&m_Socket, from);

		uint8_t acceptPayload[2];
		std::memcpy(acceptPayload, &newId, sizeof(newId));
		connection->QueueMessage(Net::kMsgConnectAccepted, acceptPayload, sizeof(acceptPayload), Reliability::Reliable);

		m_AddressToClient[key] = newId;
		m_Clients[newId] = std::move(connection);

		if (m_OnConnected) m_OnConnected(newId);
	}

	void NetServer::Broadcast(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		for (auto& [id, connection] : m_Clients)
			connection->QueueMessage(typeId, payload, size, reliability);
	}

	void NetServer::SendTo(ClientId client, uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		auto it = m_Clients.find(client);
		if (it != m_Clients.end())
			it->second->QueueMessage(typeId, payload, size, reliability);
	}

	NetConnection* NetServer::GetConnection(ClientId client)
	{
		auto it = m_Clients.find(client);
		return it != m_Clients.end() ? it->second.get() : nullptr;
	}

	std::vector<ClientId> NetServer::GetConnectedClients() const
	{
		std::vector<ClientId> ids;
		ids.reserve(m_Clients.size());
		for (auto& [id, connection] : m_Clients)
			ids.push_back(id);
		return ids;
	}

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetServerClientTest: 3 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/NetServer.h GS/src/GS/Network/NetServer.cpp \
        TestEnv/src/NetTests/NetServerClientTest.h TestEnv/src/TestApp.cpp
git commit -m "Add NetServer: connection acceptance, broadcast, per-client routing"
```

---

## Task 7: NetClient

**Files:**
- Create: `GS/src/GS/Network/NetClient.h`
- Create: `GS/src/GS/Network/NetClient.cpp`
- Modify: `TestEnv/src/NetTests/NetServerClientTest.h`

**Interfaces:**
- Consumes: `GS::NetServer`, `GS::Net::kMsg*` (Task 6); `GS::
  NetConnection`, `GS::Reliability` (Task 4); `GS::ParseAddress` (Task 2).
- Produces: `GS::NetClient` with: `Connect(const NetAddress& server) ->
  bool`, `Disconnect()`, `Update(float deltaSeconds)`, `Send(uint32_t
  typeId, const uint8_t*, size_t, Reliability)`,
  `OnConnected(std::function<void()>)`,
  `OnDisconnected(std::function<void()>)`, `OnMessage(std::function<
  void(uint32_t typeId, ByteStream&)>)`, `IsConnected() -> bool`,
  `GetClientId() -> ClientId` (`InvalidClient` until the server's
  `ConnectAccepted` arrives), `GetRTT() -> float`.

- [ ] **Step 1: Write the failing test**

Add to `NetServerClientTest.h`:

```cpp
	inline void RunClientConnectsToServer()
	{
		GS::NetServer server;
		Check(server.Host(0), "server hosts");

		GS::NetClient client;
		bool clientConnected = false;
		client.OnConnected([&] { clientConnected = true; });

		Check(client.Connect({ 0x7F000001u, server.GetLocalPort() }), "client starts connecting");

		bool messageArrivedAtServer = false;
		server.OnMessage([&](GS::ClientId, uint32_t typeId, GS::ByteStream&)
		{
			if (typeId == 9999) messageArrivedAtServer = true;
		});

		for (int i = 0; i < 200 && !clientConnected; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(clientConnected, "client's OnConnected fired");
		Check(client.GetClientId() != GS::InvalidClient, "client learned its own ClientId");
		Check(client.GetRTT() >= 0.0f, "client has a real (possibly zero) RTT reading");

		uint8_t payload[1] = { 0 };
		client.Send(9999, payload, 1, GS::Reliability::Reliable);

		for (int i = 0; i < 200 && !messageArrivedAtServer; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(messageArrivedAtServer, "a client->server message reaches NetServer::OnMessage");

		client.Disconnect();
		server.Shutdown();
	}

	inline void Run() {
		RunAcceptsAConnection();
		RunClientConnectsToServer();
		GS_TRACE("NetServerClientTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS::NetClient` undeclared.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/NetClient.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Network/Socket.h"
#include "GS/Network/NetConnection.h"
#include "GS/Network/NetServer.h"

namespace GS {

	class GS_API NetClient
	{
	public:
		using ConnectedCallback = std::function<void()>;
		using DisconnectedCallback = std::function<void()>;
		using MessageCallback = std::function<void(uint32_t typeId, ByteStream& payload)>;

		bool Connect(const NetAddress& server);
		void Disconnect();

		void Update(float deltaSeconds);

		void Send(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability);

		void OnConnected(ConnectedCallback cb) { m_OnConnected = std::move(cb); }
		void OnDisconnected(DisconnectedCallback cb) { m_OnDisconnected = std::move(cb); }
		void OnMessage(MessageCallback cb) { m_OnMessage = std::move(cb); }

		bool IsConnected() const { return m_ClientId != InvalidClient; }
		ClientId GetClientId() const { return m_ClientId; }
		float GetRTT() const { return m_Connection ? m_Connection->GetRTT() : 0.0f; }

	private:
		Socket m_Socket;
		std::unique_ptr<NetConnection> m_Connection;
		ClientId m_ClientId = InvalidClient;
		float m_TimeSinceConnectRequest = 0.0f;
		bool m_Connecting = false;

		ConnectedCallback m_OnConnected;
		DisconnectedCallback m_OnDisconnected;
		MessageCallback m_OnMessage;
	};

}
```

```cpp
// GS/src/GS/Network/NetClient.cpp
#include "gspch.h"
#include "GS/Network/NetClient.h"

namespace GS {

	bool NetClient::Connect(const NetAddress& server)
	{
		if (!m_Socket.Bind(0))
			return false;

		m_Connection = std::make_unique<NetConnection>(&m_Socket, server);
		m_Connecting = true;
		m_ClientId = InvalidClient;
		m_TimeSinceConnectRequest = 1000.0f; // force an immediate first send below

		return true;
	}

	void NetClient::Disconnect()
	{
		m_Connection.reset();
		m_Connecting = false;
		m_ClientId = InvalidClient;
		m_Socket.Close();
	}

	void NetClient::Update(float deltaSeconds)
	{
		if (!m_Connection)
			return;

		if (m_Connecting)
		{
			// Not yet a NetConnection-tracked reliable message -- there is no
			// confirmed peer sequence state until ConnectAccepted arrives, so
			// this is resent by hand at a fixed interval instead.
			m_TimeSinceConnectRequest += deltaSeconds;
			if (m_TimeSinceConnectRequest >= NetConnection::kResendInterval)
			{
				m_TimeSinceConnectRequest = 0.0f;
				ByteStream request;
				Protocol::WriteHeader(request, { 0, 0, 0 });
				request.WriteU8((uint8_t)Reliability::Reliable);
				request.WriteU32(Net::kMsgConnectRequest);
				request.WriteU16(2);
				request.WriteU16(Protocol::kProtocolVersion);
				m_Socket.SendTo(m_Connection->GetRemoteAddress(), request.Data(), request.Size());
			}
		}

		m_Connection->Update(deltaSeconds);

		uint8_t buffer[Protocol::kMaxPacketSize];
		NetAddress from;
		int received;
		while ((received = m_Socket.RecvFrom(buffer, sizeof(buffer), from)) > 0)
		{
			if (m_Connecting)
			{
				// Peek the message type without going through NetConnection --
				// there is no established sequence state to accept a raw
				// ConnectAccepted/Rejected through yet.
				ByteStream in(buffer, (size_t)received);
				Protocol::ReadHeader(in);
				if (in.AtEnd()) continue;
				in.ReadU8();
				uint32_t typeId = in.ReadU32();
				uint16_t length = in.ReadU16();

				if (typeId == Net::kMsgConnectAccepted && length == 2)
				{
					std::memcpy(&m_ClientId, in.Data() + in.ReadOffset(), 2);
					m_Connecting = false;
					if (m_OnConnected) m_OnConnected();
				}
				else if (typeId == Net::kMsgConnectRejected)
				{
					Disconnect();
				}
				continue;
			}

			m_Connection->ReceivePacket(buffer, (size_t)received, [&](uint32_t typeId, ByteStream& payload)
			{
				if (m_OnMessage) m_OnMessage(typeId, payload);
			});
		}

		if (!m_Connecting && m_Connection->IsTimedOut())
		{
			bool wasConnected = IsConnected();
			Disconnect();
			if (wasConnected && m_OnDisconnected) m_OnDisconnected();
			return;
		}

		if (!m_Connecting)
			m_Connection->Flush(deltaSeconds);
	}

	void NetClient::Send(uint32_t typeId, const uint8_t* payload, size_t size, Reliability reliability)
	{
		if (m_Connection && !m_Connecting)
			m_Connection->QueueMessage(typeId, payload, size, reliability);
	}

}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetServerClientTest: 7 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/NetClient.h GS/src/GS/Network/NetClient.cpp \
        TestEnv/src/NetTests/NetServerClientTest.h TestEnv/src/TestApp.cpp
git commit -m "Add NetClient: connect handshake and the client side of a session"
```

---

## Task 8: NetMessage (RPC registration)

**Files:**
- Create: `GS/src/GS/Network/NetMessage.h`
- Create: `GS/src/GS/Network/NetMessage.cpp`
- Modify: `GS/src/GS/Network/NetServer.cpp` (route unrecognised message
  types through the registry)
- Modify: `GS/src/GS/Network/NetClient.cpp` (same)
- Test: `TestEnv/src/NetTests/NetMessageTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::NetServer`, `GS::NetClient` (Tasks 6, 7);
  `GS::Reliability` (Task 4).
- Produces: `GS::Net::RegisterHandler(const std::string& name,
  std::function<void(ClientId from, ByteStream&)> handler)`,
  `GS::Net::SendRPC(NetServer&, ClientId to, const std::string& name,
  ByteStream& payload, Reliability)`, `GS::Net::SendRPC(NetServer&, const
  std::string& name, ByteStream& payload, Reliability)` (broadcast,
  `to = ServerOwned` sentinel meaning "everyone" is avoided by using an
  explicit overload instead), `GS::Net::SendRPC(NetClient&, const
  std::string& name, ByteStream& payload, Reliability)`,
  `GS::Net::HashMessageName(const std::string&) -> uint32_t` (exposed so
  tests can assert two different names hash differently, and so it's
  usable at the reserved-ID boundary check below).

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/NetMessageTest.h
// TEMPORARY -- delete after verifying RPC registration/dispatch and that
// the message-name hash never collides with the reserved built-in IDs.
#pragma once
#include <GS.h>
#include <GS/Network/NetMessage.h>
#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>

namespace NetMessageTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		Check(GS::Net::HashMessageName("BallBounced") != GS::Net::HashMessageName("Score"),
			"different names hash differently");
		Check(GS::Net::HashMessageName("BallBounced") > 100,
			"hashed IDs stay clear of the low reserved built-in range (1-3)");

		GS::NetServer server;
		server.Host(0);
		GS::NetClient client;
		client.Connect({ 0x7F000001u, server.GetLocalPort() });

		bool connected = false;
		client.OnConnected([&] { connected = true; });
		for (int i = 0; i < 200 && !connected; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}
		Check(connected, "client connected (setup for the RPC check below)");

		bool serverGotIt = false;
		int receivedValue = 0;
		GS::Net::RegisterHandler("TestEvent", [&](GS::ClientId, GS::ByteStream& payload)
		{
			serverGotIt = true;
			receivedValue = payload.ReadU32();
		});

		GS::ByteStream payload;
		payload.WriteU32(4242);
		GS::Net::SendRPC(client, "TestEvent", payload, GS::Reliability::Reliable);

		for (int i = 0; i < 200 && !serverGotIt; i++)
		{
			server.Update(1.0f / 60.0f);
			client.Update(1.0f / 60.0f);
		}

		Check(serverGotIt, "server's registered handler fired for a client-sent RPC");
		Check(receivedValue == 4242, "the RPC payload arrived intact");

		client.Disconnect();
		server.Shutdown();
		GS_TRACE("NetMessageTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS/Network/NetMessage.h`
missing.

- [ ] **Step 3: Write minimal implementation**

```cpp
// GS/src/GS/Network/NetMessage.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Network/NetServer.h"
#include "GS/Network/NetClient.h"

namespace GS::Net {

	using MessageHandler = std::function<void(ClientId from, ByteStream& payload)>;

	GS_API uint32_t HashMessageName(const std::string& name);

	GS_API void RegisterHandler(const std::string& name, MessageHandler handler);

	// Internal: called by NetServer/NetClient for any message type that
	// isn't one of the reserved built-in IDs (connect/spawn/despawn/
	// snapshot). Not part of the public API a game calls.
	GS_API void DispatchToHandler(ClientId from, uint32_t typeId, ByteStream& payload);

	GS_API void SendRPC(NetServer& server, ClientId to, const std::string& name, ByteStream& payload, Reliability reliability);
	GS_API void SendRPC(NetServer& server, const std::string& name, ByteStream& payload, Reliability reliability); // broadcast
	GS_API void SendRPC(NetClient& client, const std::string& name, ByteStream& payload, Reliability reliability);

}
```

```cpp
// GS/src/GS/Network/NetMessage.cpp
#include "gspch.h"
#include "GS/Network/NetMessage.h"

namespace GS::Net {

	static std::unordered_map<uint32_t, MessageHandler> s_Handlers;

	uint32_t HashMessageName(const std::string& name)
	{
		// FNV-1a. Simple, no dependency, plenty of distribution for the
		// handful of message names one game defines.
		uint32_t hash = 2166136261u;
		for (char c : name)
		{
			hash ^= (uint8_t)c;
			hash *= 16777619u;
		}
		// Never collide with the reserved built-in IDs (1-3 for connect,
		// 4-6 reserved for replication in Task 9) -- push every hashed ID
		// above a safe floor.
		return hash < 1000 ? hash + 1000 : hash;
	}

	void RegisterHandler(const std::string& name, MessageHandler handler)
	{
		s_Handlers[HashMessageName(name)] = std::move(handler);
	}

	void DispatchToHandler(ClientId from, uint32_t typeId, ByteStream& payload)
	{
		auto it = s_Handlers.find(typeId);
		if (it != s_Handlers.end())
			it->second(from, payload);
	}

	void SendRPC(NetServer& server, ClientId to, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		server.SendTo(to, HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

	void SendRPC(NetServer& server, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		server.Broadcast(HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

	void SendRPC(NetClient& client, const std::string& name, ByteStream& payload, Reliability reliability)
	{
		client.Send(HashMessageName(name), payload.Data(), payload.Size(), reliability);
	}

}
```

Wire dispatch in by adding, at the end of `NetServer::HandleRawPacket`'s
known-address branch (inside the `ReceivePacket` lambda, after the
`m_OnMessage` call) and at the end of `NetClient::Update`'s established
(`!m_Connecting`) `ReceivePacket` lambda:

```cpp
Net::DispatchToHandler(id, typeId, payload); // NetServer's lambda -- `id` is in scope
```
```cpp
Net::DispatchToHandler(GS::ServerOwned, typeId, payload); // NetClient's lambda -- no ClientId of its own
```

`#include "GS/Network/NetMessage.h"` in both `.cpp` files. This means
`m_OnMessage` (the app's own callback, used by the tests in Tasks 6-7)
and the RPC registry both see every message -- a game can use either the
raw callback or the named-handler registry, whichever fits.

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetMessageTest: 4 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Network/NetMessage.h GS/src/GS/Network/NetMessage.cpp \
        GS/src/GS/Network/NetServer.cpp GS/src/GS/Network/NetClient.cpp \
        TestEnv/src/NetTests/NetMessageTest.h TestEnv/src/TestApp.cpp
git commit -m "Add NetMessage: hashed-name RPC registration over NetServer/NetClient"
```

---

## Task 9: NetworkIdentity/NetworkTransform components + replication

**Files:**
- Modify: `GS/src/GS/Scene/Components.h` (add the two components)
- Create: `GS/src/GS/Network/NetReplication.h`
- Create: `GS/src/GS/Network/NetReplication.cpp`
- Modify: `GS/src/GS/Network/NetServer.h`/`.cpp` (reserve the three
  replication message IDs, add a raw-callback escape hatch is not
  needed -- replication rides the same `MessageCallback` path already
  wired)
- Test: `TestEnv/src/NetTests/NetReplicationTest.h` (temporary)

**Interfaces:**
- Consumes: `GS::Scene`, `GS::Entity`, `GS::TransformComponent` (existing
  engine); `GS::NetServer`, `GS::NetClient`, `GS::ClientId`,
  `GS::ServerOwned` (Tasks 6, 7).
- Produces: `GS::NetworkIdentity {uint32_t NetworkId; ClientId Owner;}`,
  `GS::NetworkTransform {float SendRate; bool Interpolate;}` (both in
  `Components.h`); `GS::Net::kMsgSpawnEntity/kMsgDespawnEntity/
  kMsgTransformSnapshot` (reserved IDs 4, 5, 6);
  `GS::Net::SpawnNetworkedEntity(NetServer&, Scene&, ClientId owner, const
  glm::vec3& position) -> Entity`; `GS::Net::DespawnNetworkedEntity(
  NetServer&, Scene&, Entity)`; `GS::Net::ServerBroadcastSnapshots(
  NetServer&, Scene&, float deltaSeconds)`; `GS::Net::ClientBindScene(
  NetClient&, Scene&)`.

- [ ] **Step 1: Write the failing test**

```cpp
// TestEnv/src/NetTests/NetReplicationTest.h
// TEMPORARY -- delete after verifying spawn/despawn bookkeeping and
// transform-snapshot convergence between a real NetServer and NetClient.
#pragma once
#include <GS.h>
#include <GS/Network/NetReplication.h>

namespace NetReplicationTest {
	inline int g_Pass = 0, g_Fail = 0;

	inline void Check(bool ok, const std::string& what) {
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run() {
		GS::NetServer server;
		server.Host(0);
		GS::Scene serverScene;

		GS::NetClient client;
		GS::Scene clientScene;
		GS::Net::ClientBindScene(client, clientScene);

		client.Connect({ 0x7F000001u, server.GetLocalPort() });

		bool connected = false;
		client.OnConnected([&] { connected = true; });
		auto pump = [&](int steps) {
			for (int i = 0; i < steps; i++) { server.Update(1.0f / 60.0f); client.Update(1.0f / 60.0f); }
		};
		pump(1); // register the callback above before any Update runs
		for (int i = 0; i < 200 && !connected; i++) { server.Update(1.0f / 60.0f); client.Update(1.0f / 60.0f); }
		Check(connected, "client connected (setup)");

		GS::Entity spawned = GS::Net::SpawnNetworkedEntity(server, serverScene, GS::ServerOwned, { 1.0f, 2.0f, 3.0f });
		Check(spawned.IsValid(), "server-side entity was created");
		uint32_t networkId = spawned.Get<GS::NetworkIdentity>()->NetworkId;
		Check(networkId != 0, "a real NetworkId was assigned");

		pump(30);

		GS::EntityId clientEntity = GS::InvalidEntity;
		for (GS::EntityId id : clientScene.GetEntities())
		{
			auto* identity = clientScene.GetComponent<GS::NetworkIdentity>(id);
			if (identity && identity->NetworkId == networkId)
				clientEntity = id;
		}
		Check(clientEntity != GS::InvalidEntity, "spawn replicated: client created a matching entity");

		if (clientEntity != GS::InvalidEntity)
		{
			auto* clientTransform = clientScene.GetComponent<GS::TransformComponent>(clientEntity);
			glm::vec3 delta = clientTransform->Position - glm::vec3(1.0f, 2.0f, 3.0f);
			Check(glm::length(delta) < 0.01f, "initial position replicated exactly");
		}

		// Move the server's entity and confirm the snapshot converges it.
		spawned.Get<GS::TransformComponent>()->Position = { 9.0f, 9.0f, 9.0f };
		for (int i = 0; i < 60; i++)
		{
			GS::Net::ServerBroadcastSnapshots(server, serverScene, 1.0f / 60.0f);
			pump(1);
		}

		if (clientEntity != GS::InvalidEntity)
		{
			auto* clientTransform = clientScene.GetComponent<GS::TransformComponent>(clientEntity);
			glm::vec3 delta = clientTransform->Position - glm::vec3(9.0f, 9.0f, 9.0f);
			Check(glm::length(delta) < 0.5f, "moved position converges on the client (delta=" + std::to_string(glm::length(delta)) + ")");
		}

		GS::Net::DespawnNetworkedEntity(server, serverScene, spawned);
		pump(30);

		bool stillPresent = false;
		for (GS::EntityId id : clientScene.GetEntities())
		{
			auto* identity = clientScene.GetComponent<GS::NetworkIdentity>(id);
			if (identity && identity->NetworkId == networkId)
				stillPresent = true;
		}
		Check(!stillPresent, "despawn replicated: client's entity is gone");

		client.Disconnect();
		server.Shutdown();
		GS_TRACE("NetReplicationTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gs.py build` -- expected: fails, `GS/Network/NetReplication.h`
missing and `NetworkIdentity`/`NetworkTransform` undeclared.

- [ ] **Step 3: Write minimal implementation**

Add to `GS/src/GS/Scene/Components.h`, alongside the existing components:

```cpp
	// Opt-in networking. An entity with both of these has its
	// TransformComponent auto-replicated by the server to every client;
	// anything else about it (score, health, "this ball bounced") travels
	// as a manual RPC instead -- see GS::Net::SendRPC.
	struct NetworkIdentity
	{
		uint32_t NetworkId = 0;   // assigned by the server when spawned
		ClientId Owner = ServerOwned;
	};

	struct NetworkTransform
	{
		float SendRate = 20.0f;   // Hz -- independent of the fixed-step rate
		bool Interpolate = true;
	};
```

(`ClientId`/`ServerOwned` are declared in `NetServer.h`; add `#include
"GS/Network/NetServer.h"` to `Components.h`'s includes.)

```cpp
// GS/src/GS/Network/NetReplication.h
#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Scene/Scene.h"
#include "GS/Network/NetServer.h"
#include "GS/Network/NetClient.h"

namespace GS::Net {

	constexpr uint32_t kMsgSpawnEntity = 4;
	constexpr uint32_t kMsgDespawnEntity = 5;
	constexpr uint32_t kMsgTransformSnapshot = 6;

	GS_API Entity SpawnNetworkedEntity(NetServer& server, Scene& scene, ClientId owner, const glm::vec3& position);
	GS_API void DespawnNetworkedEntity(NetServer& server, Scene& scene, Entity entity);

	// Server-side: call once per fixed step. Walks every NetworkIdentity +
	// NetworkTransform entity whose SendRate interval has elapsed and
	// broadcasts its current TransformComponent.
	GS_API void ServerBroadcastSnapshots(NetServer& server, Scene& scene, float deltaSeconds);

	// Client-side: call once, right after constructing the NetClient, before
	// the first Connect(). Wires Spawn/Despawn/Snapshot handling so incoming
	// replication messages create/destroy/update entities in `scene`.
	GS_API void ClientBindScene(NetClient& client, Scene& scene);

}
```

```cpp
// GS/src/GS/Network/NetReplication.cpp
#include "gspch.h"
#include "GS/Network/NetReplication.h"

namespace GS::Net {

	static uint32_t s_NextNetworkId = 1;

	// Per-scene-pointer client-side bookkeeping. Keyed by Scene* rather than
	// held as a member of NetClient, since NetClient (Tasks 6-7) has no
	// reason to know Scene exists -- that separation is the point of this
	// file existing at all.
	static std::unordered_map<Scene*, std::unordered_map<uint32_t, EntityId>> s_ClientNetworkIdToEntity;

	// Server-side: entities time out their SendRate independently.
	static std::unordered_map<uint32_t, float> s_TimeSinceLastSnapshot;

	Entity SpawnNetworkedEntity(NetServer& server, Scene& scene, ClientId owner, const glm::vec3& position)
	{
		Entity entity = scene.CreateEntity("NetworkedEntity");
		entity.Get<TransformComponent>()->Position = position;

		uint32_t networkId = s_NextNetworkId++;
		entity.Add<NetworkIdentity>({ networkId, owner });
		entity.Add<NetworkTransform>({});

		ByteStream spawn;
		spawn.WriteU32(networkId);
		spawn.WriteU16(owner);
		spawn.WriteVec3(position);
		server.Broadcast(kMsgSpawnEntity, spawn.Data(), spawn.Size(), Reliability::Reliable);

		return entity;
	}

	void DespawnNetworkedEntity(NetServer& server, Scene& scene, Entity entity)
	{
		auto* identity = entity.Get<NetworkIdentity>();
		if (!identity)
			return;

		ByteStream despawn;
		despawn.WriteU32(identity->NetworkId);
		server.Broadcast(kMsgDespawnEntity, despawn.Data(), despawn.Size(), Reliability::Reliable);

		s_TimeSinceLastSnapshot.erase(identity->NetworkId);
		scene.DestroyEntity(entity.GetId());
	}

	void ServerBroadcastSnapshots(NetServer& server, Scene& scene, float deltaSeconds)
	{
		auto& identities = scene.View<NetworkIdentity>();
		for (size_t i = 0; i < identities.Size(); i++)
		{
			EntityId owner = identities.Owner(i);
			auto* networkTransform = scene.GetComponent<NetworkTransform>(owner);
			if (!networkTransform)
				continue;

			uint32_t networkId = identities.Components()[i].NetworkId;
			float& elapsed = s_TimeSinceLastSnapshot[networkId];
			elapsed += deltaSeconds;

			float interval = 1.0f / networkTransform->SendRate;
			if (elapsed < interval)
				continue;
			elapsed = 0.0f;

			auto* transform = scene.GetComponent<TransformComponent>(owner);
			ByteStream snapshot;
			snapshot.WriteU32(networkId);
			snapshot.WriteVec3(transform->Position);
			snapshot.WriteVec3(transform->Rotation);
			server.Broadcast(kMsgTransformSnapshot, snapshot.Data(), snapshot.Size(), Reliability::Unreliable);
		}
	}

	void ClientBindScene(NetClient& client, Scene& scene)
	{
		auto& map = s_ClientNetworkIdToEntity[&scene];

		client.OnMessage([&scene, &map](uint32_t typeId, ByteStream& payload)
		{
			if (typeId == kMsgSpawnEntity)
			{
				uint32_t networkId = payload.ReadU32();
				ClientId owner = payload.ReadU16();
				glm::vec3 position = payload.ReadVec3();

				Entity entity = scene.CreateEntity("NetworkedEntity");
				entity.Get<TransformComponent>()->Position = position;
				entity.Add<NetworkIdentity>({ networkId, owner });
				entity.Add<NetworkTransform>({});
				map[networkId] = entity.GetId();
			}
			else if (typeId == kMsgDespawnEntity)
			{
				uint32_t networkId = payload.ReadU32();
				auto it = map.find(networkId);
				if (it != map.end())
				{
					scene.DestroyEntity(it->second);
					map.erase(it);
				}
			}
			else if (typeId == kMsgTransformSnapshot)
			{
				uint32_t networkId = payload.ReadU32();
				glm::vec3 position = payload.ReadVec3();
				glm::vec3 rotation = payload.ReadVec3();

				auto it = map.find(networkId);
				if (it != map.end())
				{
					// No interpolation smoothing yet in this version of the
					// test path -- ServerBroadcastSnapshots already only
					// sends at SendRate, and NetworkTransform::Interpolate is
					// wired up visually in the demo (Task 12), where there is
					// a render frame rate to interpolate across; a headless
					// test has no such notion of "between snapshots."
					if (auto* transform = scene.GetComponent<TransformComponent>(it->second))
					{
						transform->Position = position;
						transform->Rotation = rotation;
					}
				}
			}

			Net::DispatchToHandler(GS::ServerOwned, typeId, payload);
		});
	}

}
```

**Deliberate scope note for this task:** `ClientBindScene` snaps the
position directly rather than interpolating, so this task's own test
(headless, no render frame rate) can assert convergence without needing
a notion of "time between render frames." Task 12 (the demo) adds the
actual buffered-last-two-snapshots lerp described in the spec, in the
demo's `OnDemoUpdate` where a render frame rate genuinely exists --
that is real, additional behaviour, not a stub being backfilled later.

Also fix `NetClient::Update`'s and `NetServer::HandleRawPacket`'s message
lambdas to not double-call `Net::DispatchToHandler` -- Task 8 already
added that call at the end of each; `ClientBindScene`'s own `OnMessage`
now replaces the *app's* callback slot for the client, and calls
`Net::DispatchToHandler` itself at the end (as shown above) so RPC
handlers still fire. No change needed on the server side: `NetServer`'s
own dispatch (Task 8) already runs regardless of what the app's
`OnMessage` does, since replication messages arrive through the same
`m_OnMessage`/`DispatchToHandler` pair and `NetReplicationTest` doesn't
install its own server-side `OnMessage`.

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && timeout 10 ./gs.py run -- --hide-window`
Expected: `NetReplicationTest: 6 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add GS/src/GS/Scene/Components.h GS/src/GS/Network/NetReplication.h \
        GS/src/GS/Network/NetReplication.cpp \
        TestEnv/src/NetTests/NetReplicationTest.h TestEnv/src/TestApp.cpp
git commit -m "Add NetworkIdentity/NetworkTransform and transform replication"
```

---

## Task 10: Editor Network panel

**Files:**
- Create: `TestEnv/src/NetworkPanel.h`
- Modify: `TestEnv/src/EditorShell.h` (push the new panel like Scenes/
  Appearance)

**Interfaces:**
- Consumes: `GS::NetServer`, `GS::NetClient`, `GS::ClientId` (Tasks 6, 7).
- Produces: `g_EditorNetServer` / `g_EditorNetClient` (globals, matching
  `g_EditorScene`'s existing pattern), a `NetworkPanel : GS::Layer` pushed
  once at startup.

- [ ] **Step 1: Write the failing test**

There is no headless-testable behaviour here beyond "it draws and doesn't
crash" -- per the project's own testing philosophy, GUI click-through
isn't exercised (no GUI-automation tool exists in this environment); the
check is a capture. Before writing the panel:

Run: `./gs.py build && ./gs.py run -- --lockstep --capture-step 30 --capture shots/before-network-panel.png`

Expected: succeeds (there is no Network panel yet -- this just anchors a
before/after comparison in Step 4).

- [ ] **Step 2: (the "test" for this task is the capture diff in Step 4 --
  nothing to run yet that would fail meaningfully beforehand)**

- [ ] **Step 3: Write the panel**

```cpp
// TestEnv/src/NetworkPanel.h
#pragma once

#include <GS.h>
#include <imgui.h>

#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>

// One process-wide server and client, matching how the editor already
// keeps one g_EditorScene rather than threading state through callers.
// A given editor process is either hosting or joined, never both.
inline GS::NetServer g_EditorNetServer;
inline GS::NetClient g_EditorNetClient;

class NetworkPanel : public GS::Layer
{
public:
	NetworkPanel() : GS::Layer("NetworkPanel") {}

	void OnUpdate(GS::Timestep ts) override
	{
		(void)ts;
		if (g_EditorNetServer.IsHosting())
			g_EditorNetServer.Update(GS::Application::Get().GetFixedTimestep());
		if (g_EditorNetClient.IsConnected())
			g_EditorNetClient.Update(GS::Application::Get().GetFixedTimestep());
	}

	void OnImGui() override
	{
		if (GS::Application::Get().IsUIHidden())
			return;

		ImGui::Begin("Network");

		if (g_EditorNetServer.IsHosting())
		{
			ImGui::Text("Hosting on port %u", g_EditorNetServer.GetLocalPort());
			if (ImGui::Button("Stop Hosting"))
				g_EditorNetServer.Shutdown();

			ImGui::Separator();
			ImGui::Text("Connected clients:");
			if (ImGui::BeginTable("clients", 2, ImGuiTableFlags_Borders))
			{
				ImGui::TableSetupColumn("Client");
				ImGui::TableSetupColumn("RTT (ms)");
				ImGui::TableHeadersRow();
				for (GS::ClientId id : g_EditorNetServer.GetConnectedClients())
				{
					GS::NetConnection* connection = g_EditorNetServer.GetConnection(id);
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%u", id);
					ImGui::TableNextColumn(); ImGui::Text("%.0f", connection ? connection->GetRTT() * 1000.0f : 0.0f);
				}
				ImGui::EndTable();
			}
		}
		else if (g_EditorNetClient.IsConnected())
		{
			ImGui::Text("Connected as client %u", g_EditorNetClient.GetClientId());
			ImGui::Text("RTT: %.0f ms", g_EditorNetClient.GetRTT() * 1000.0f);
			if (ImGui::Button("Disconnect"))
				g_EditorNetClient.Disconnect();
		}
		else
		{
			ImGui::InputInt("Port", &m_HostPort);
			if (ImGui::Button("Host"))
				g_EditorNetServer.Host((uint16_t)m_HostPort);

			ImGui::Separator();

			ImGui::InputText("Address:Port", m_JoinAddress, sizeof(m_JoinAddress));
			if (ImGui::Button("Join"))
			{
				GS::NetAddress address;
				if (GS::ParseAddress(m_JoinAddress, address))
					g_EditorNetClient.Connect(address);
				else
					GS_WARN("Network panel: '{0}' is not a valid address:port", m_JoinAddress);
			}
		}

		ImGui::End();
	}

private:
	int m_HostPort = 7777;
	char m_JoinAddress[64] = "127.0.0.1:7777";
};
```

Add `#include "NetworkPanel.h"` and `layerStack.PushOverlay(new
NetworkPanel());` in `EditorShell.h`, next to where the Appearance panel
is pushed -- and add `ImGui::DockBuilderDockWindow("Network", ...)` to the
same dock-building block that places Scenes/Appearance, so it opens
docked rather than floating on first run.

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && ./gs.py run -- --lockstep --capture-step 30 --capture shots/after-network-panel.png`

Expected: succeeds, and the capture shows a "Network" panel with the port
field and Host/Join controls, docked alongside Scenes/Appearance.

- [ ] **Step 5: Commit**

```bash
git add TestEnv/src/NetworkPanel.h TestEnv/src/EditorShell.h
git commit -m "Add the editor's Network panel: host/join and connected-client status"
```

---

## Task 11: Inspector support for NetworkIdentity/NetworkTransform

**Files:**
- Modify: `TestEnv/src/EditorSceneView.h`

**Interfaces:**
- Consumes: `GS::NetworkIdentity`, `GS::NetworkTransform` (Task 9);
  existing `EditorHistory::EditFieldCommand<Component, Field>` pattern.

- [ ] **Step 1: Write the failing test**

Same capture-based approach as Task 10 (no GUI-automation tool exists).
Before writing this task's code:

Run: `./gs.py build && ./gs.py run -- --lockstep --capture-step 30 --capture shots/before-network-inspector.png --demo Cube3D`

(with an entity selected via a temporary `g_EditorScene.CreateEntity()`
call added at startup for the capture, removed again once verified.)

- [ ] **Step 2: (capture diff, as Task 10)**

- [ ] **Step 3: Write the Inspector section**

Add to `EditorSceneView.h`'s Inspector drawing, in the same place the
existing `ScriptComponent` "Add Script"/"Remove Script" block lives:

```cpp
		ImGui::SeparatorText("Network");
		if (auto* identity = g_EditorScene.GetComponent<GS::NetworkIdentity>(m_Selected))
		{
			ImGui::Text("NetworkId: %u%s", identity->NetworkId,
				identity->NetworkId == 0 ? " (not yet assigned by a live server)" : "");
			ImGui::Text("Owner: %s", identity->Owner == GS::ServerOwned ? "Server" : std::to_string(identity->Owner).c_str());

			if (auto* networkTransform = g_EditorScene.GetComponent<GS::NetworkTransform>(m_Selected))
			{
				ImGui::DragFloat("Send Rate (Hz)", &networkTransform->SendRate, 1.0f, 1.0f, 60.0f);
				if (ImGui::IsItemActivated())
					m_EditBeforeFloat = networkTransform->SendRate;
				if (ImGui::IsItemDeactivatedAfterEdit())
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::NetworkTransform, float>>(
						m_Selected, &GS::NetworkTransform::SendRate, m_EditBeforeFloat, networkTransform->SendRate));

				ImGui::Checkbox("Interpolate", &networkTransform->Interpolate);
				if (ImGui::IsItemActivated())
					m_EditBeforeBool = networkTransform->Interpolate;
				if (ImGui::IsItemDeactivatedAfterEdit())
					EditorHistory::Push(std::make_unique<EditFieldCommand<GS::NetworkTransform, bool>>(
						m_Selected, &GS::NetworkTransform::Interpolate, m_EditBeforeBool, networkTransform->Interpolate));
			}

			if (ImGui::Button("Remove Network"))
			{
				g_EditorScene.RemoveComponent<GS::NetworkTransform>(m_Selected);
				g_EditorScene.RemoveComponent<GS::NetworkIdentity>(m_Selected);
			}
		}
		else
		{
			if (ImGui::Button("Add Network"))
			{
				g_EditorScene.AddComponent<GS::NetworkIdentity>(m_Selected, GS::NetworkIdentity{});
				g_EditorScene.AddComponent<GS::NetworkTransform>(m_Selected, GS::NetworkTransform{});
			}
		}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./gs.py build && ./gs.py run -- --lockstep --capture-step 30 --capture shots/after-network-inspector.png --demo Cube3D`

Expected: succeeds; the capture shows the Inspector's "Network" section
with an "Add Network" button, and after clicking it in an interactive
run, the NetworkId/Owner/Send Rate/Interpolate fields.

- [ ] **Step 5: Commit**

```bash
git add TestEnv/src/EditorSceneView.h
git commit -m "Add Inspector support for NetworkIdentity/NetworkTransform"
```

---

## Task 12: NetworkDemo

**Files:**
- Create: `TestEnv/src/NetworkDemo.h`
- Modify: `TestEnv/src/DemoRegistry.h` (register it, append-only per the
  file's own rule)

**Interfaces:**
- Consumes: everything from Tasks 6-9; `DemoLayer` (existing,
  `TestEnv/src/Demo.h`); `GS::Application::GetCommandLine()` (existing).

- [ ] **Step 1: Write the failing test**

This is proven by running two real processes, not a headless self-test --
per the spec's testing section. Before writing the demo:

Run: `./gs.py build` -- confirms the engine-side pieces (Tasks 1-9)
already build cleanly; there is nothing named `NetworkDemo` yet to run.

- [ ] **Step 2: (verified in Step 4, by actually running two processes)**

- [ ] **Step 3: Write the demo**

```cpp
// TestEnv/src/NetworkDemo.h
#pragma once

#include <GS.h>
#include <imgui.h>

#include <GS/Network/NetServer.h>
#include <GS/Network/NetClient.h>
#include <GS/Network/NetReplication.h>
#include <GS/Network/NetMessage.h>

#include "Demo.h"

// Proves the whole stack end to end: run as two real processes,
//   ./TestEnv --demo NetworkDemo --host 7777
//   ./TestEnv --demo NetworkDemo --join 127.0.0.1:7777
// Each process gets a coloured cube it moves with WASD; NetworkIdentity +
// NetworkTransform replicate position, and Space broadcasts a "Flash" RPC
// both processes render, proving the RPC path alongside replication.
class NetworkDemo : public DemoLayer
{
public:
	NetworkDemo() : DemoLayer("NetworkDemo"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 100.0f) {}

	void OnDemoAttach() override
	{
		m_Camera.SetPosition({ 0.0f, 4.0f, 10.0f });
		m_Camera.SetRotation(-90.0f, -20.0f);

		const std::vector<std::string>& args = GS::Application::GetCommandLine();
		for (size_t i = 1; i + 1 < args.size(); i++)
		{
			if (args[i] == "--host")
				m_IsHost = true, m_HostPort = (uint16_t)std::atoi(args[i + 1].c_str());
			else if (args[i] == "--join")
				m_IsClient = true, m_JoinAddress = args[i + 1];
		}

		if (m_IsHost)
		{
			m_Server.Host(m_HostPort);
			m_LocalEntity = GS::Net::SpawnNetworkedEntity(m_Server, m_Scene, GS::ServerOwned, { -2.0f, 0.5f, 0.0f });
			GS::Net::RegisterHandler("Flash", [this](GS::ClientId, GS::ByteStream&) { m_FlashTimer = 0.2f; });
		}
		else if (m_IsClient)
		{
			GS::Net::ClientBindScene(m_Client, m_Scene);
			GS::NetAddress address;
			if (GS::ParseAddress(m_JoinAddress, address))
				m_Client.Connect(address);
			GS::Net::RegisterHandler("Flash", [this](GS::ClientId, GS::ByteStream&) { m_FlashTimer = 0.2f; });
		}
	}

	void OnDemoFixedUpdate(GS::Timestep step) override
	{
		float dt = step.GetSeconds();

		if (m_IsHost)
		{
			glm::vec3& position = m_LocalEntity.Get<GS::TransformComponent>()->Position;
			if (GS::Input::IsKeyPressed(GS_KEY_A)) position.x -= 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_D)) position.x += 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_W)) position.z -= 3.0f * dt;
			if (GS::Input::IsKeyPressed(GS_KEY_S)) position.z += 3.0f * dt;

			if (GS::Input::IsKeyPressed(GS_KEY_SPACE) && m_SpaceWasUp)
			{
				GS::ByteStream empty;
				GS::Net::SendRPC(m_Server, "Flash", empty, GS::Reliability::Reliable);
				m_FlashTimer = 0.2f;
			}
			m_SpaceWasUp = !GS::Input::IsKeyPressed(GS_KEY_SPACE);

			m_Server.Update(dt);
			GS::Net::ServerBroadcastSnapshots(m_Server, m_Scene, dt);
		}
		else if (m_IsClient)
		{
			m_Client.Update(dt);

			if (m_Client.IsConnected() && GS::Input::IsKeyPressed(GS_KEY_SPACE) && m_SpaceWasUp)
			{
				GS::ByteStream empty;
				GS::Net::SendRPC(m_Client, "Flash", empty, GS::Reliability::Reliable);
				m_FlashTimer = 0.2f;
			}
			m_SpaceWasUp = !GS::Input::IsKeyPressed(GS_KEY_SPACE);
		}

		if (m_FlashTimer > 0.0f)
			m_FlashTimer -= dt;
	}

	void OnDemoImGui() override
	{
		ImGui::Begin("NetworkDemo");
		ImGui::Text(m_IsHost ? "Hosting on port %u" : m_IsClient ? "Client" : "Neither --host nor --join was given");
		if (m_FlashTimer > 0.0f)
			ImGui::TextColored({ 1, 1, 0, 1 }, "FLASH");
		ImGui::End();
	}

private:
	GS::PerspectiveCamera m_Camera;
	GS::Scene m_Scene;

	bool m_IsHost = false;
	bool m_IsClient = false;
	uint16_t m_HostPort = 7777;
	std::string m_JoinAddress;

	GS::NetServer m_Server;
	GS::NetClient m_Client;
	GS::Entity m_LocalEntity;

	bool m_SpaceWasUp = true;
	float m_FlashTimer = 0.0f;
};
```

(`GS::PerspectiveCamera`'s exact constructor/rendering calls should match
whichever existing 3D demo -- e.g. `Cube3D.h` -- uses; wire this demo's
`OnDemoUpdate` to submit `m_Scene`'s entities as that demo does, since the
point of this task is the networking, not new rendering machinery.)

Add to `DemoRegistry.h`:

```cpp
#include "NetworkDemo.h"
```
and, appended at the end of `s_Demos` (per the file's own "append new
demos at the end" rule):
```cpp
	{ "Engine",   "NetworkDemo (2-process replication + RPC)", "NetworkDemo", []() -> DemoLayer* { return new NetworkDemo(); } },
```

- [ ] **Step 4: Run test to verify it passes**

Run, in two terminals:
```sh
./gs.py build
./TestEnv --demo NetworkDemo --host 7777 &
sleep 1
cd bin/Debug-linux-x86_64/TestEnv && ./TestEnv --demo NetworkDemo --join 127.0.0.1:7777
```
Expected: both windows show a cube; moving the host's cube with WASD
moves it in both windows within roughly a send-interval's delay; pressing
Space in either window shows "FLASH" in both.

- [ ] **Step 5: Commit**

```bash
git add TestEnv/src/NetworkDemo.h TestEnv/src/DemoRegistry.h
git commit -m "Add NetworkDemo: a two-process replication + RPC proof of concept"
```

---

## Task 13: End-to-end capture verification

**Files:** none (verification only, per the spec's testing section).

**Interfaces:** none new.

- [ ] **Step 1: Capture both processes deterministically**

```sh
cd bin/Debug-linux-x86_64/TestEnv
./TestEnv --demo NetworkDemo --host 7777 --lockstep --hide-ui \
    --capture host.png --capture-step 240 &
sleep 1
./TestEnv --demo NetworkDemo --join 127.0.0.1:7777 --lockstep --hide-ui \
    --capture client.png --capture-step 240
```

- [ ] **Step 2: Compare against the expected value, not just "it rendered"**

Read both cubes' pixel positions back with `Framebuffer::ReadPixelRGBA`
(or, simpler here, visually confirm both captures place the host's cube
at the same screen position) -- the check that matters is that the
*client's* capture, which only ever received network snapshots, shows
the cube at the position the *host* actually simulated, not merely that
something rendered.

- [ ] **Step 3: Confirm the demo's own end-to-end properties from the spec**

- Spawn: both captures show the same single cube present (no duplicate,
  no missing cube).
- If time permits, kill the client process mid-run and confirm (via the
  host's Network panel or console `GS_INFO` trace) that
  `OnClientDisconnected` fires within `kTimeoutSeconds` (5s) of the kill.

- [ ] **Step 4: Grep for TEMPORARY and decide what to remove**

Run: `grep -rn TEMPORARY TestEnv/src/NetTests/`

Every file under `TestEnv/src/NetTests/` is temporary per the self-test
pattern. Decide with the project owner whether to delete them now (they
did their job -- Tasks 1-9 are verified) or keep them a little longer as
a regression net while Tasks 10-12 are still shaking out; either is a
one-line call, not a design decision. If deleting, also remove their
`#include`s and `::Run()` calls from `TestApp.cpp`.

- [ ] **Step 5: Add the README changelog entry**

Per this project's convention ("the README changelog is part of the
work"), add an entry under the newest section of `README.md` describing
what was built (UDP transport + custom reliable channel, host-as-server
session model, opt-in Transform replication, manual RPCs, the Network
panel, NetworkDemo) and what the measurements said (reliable delivery
held at 30% simulated loss; RTT measured against a known injected delay;
replication converged within one send interval). Commit alongside any
test-file removals from Step 4.

```bash
git add README.md TestEnv/src/TestApp.cpp TestEnv/src/NetTests/  # if tests were removed
git commit -m "Verify multiplayer foundation end-to-end; changelog entry"
```

---

## Self-Review Notes

**Spec coverage:** Transport/protocol/reliability -> Tasks 2-5. Session
lifecycle (connect/accept/reject/timeout/disconnect) -> Tasks 6-7.
Replication (NetworkIdentity/NetworkTransform, spawn/despawn, snapshots)
-> Task 9. RPCs -> Task 8. Editor Network panel -> Task 10. Inspector
support -> Task 11. Demo -> Task 12. Testing strategy (reliability under
loss, RTT accuracy, replication convergence, spawn/despawn) -> Tasks 5, 9,
13. Everything in the spec's "Out of scope" list (prediction, headless
server, matchmaking, reflected replication) has no task here, correctly.

**Type consistency check performed:** `ClientId`/`ServerOwned`/
`InvalidClient` defined once in `NetServer.h` (Task 6) and reused
verbatim in every later task; `Reliability` defined once in
`NetConnection.h` (Task 4); `kMsg*` constants allocated in ascending,
non-overlapping ranges across Tasks 6 (1-3) and 9 (4-6), with Task 8's
hashed IDs pushed above 1000 specifically so a game's own RPC names can
never collide with either.
