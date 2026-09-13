#pragma once

#include "gspch.h"
#include "GS/Core.h"
#include "GS/Log.h"
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

		// Set whenever a packet arrives, cleared once the next Flush sends --
		// without this, a side with nothing of its own queued only acks via
		// the keepalive timer (up to kKeepAliveInterval late), during which
		// the peer's resend timer keeps re-sending everything unacked.
		bool m_PendingAck = false;
	};

}
