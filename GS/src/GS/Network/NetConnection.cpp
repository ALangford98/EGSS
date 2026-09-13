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

		// Message ids are assigned in queue order (m_NextMessageId++), so
		// sorting by id restores that order -- walking m_OutgoingReliable
		// itself (an unordered_map) does not preserve it, which broke
		// "reliable-ordered" for more than one message queued in the same
		// Flush.
		std::sort(due.begin(), due.end());

		bool hasContent = !m_OutgoingUnreliable.empty() || !due.empty() || m_PendingAck;
		bool keepAliveDue = m_TimeSinceLastSend >= kKeepAliveInterval;
		if (!hasContent && !keepAliveDue)
			return;

		SendPacket(m_OutgoingUnreliable, due);
		m_OutgoingUnreliable.clear();
		m_TimeSinceLastSend = 0.0f;
		m_PendingAck = false;
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
		m_PendingAck = true;
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
