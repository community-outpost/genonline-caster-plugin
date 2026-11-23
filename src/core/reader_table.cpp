// SPDX-License-Identifier: MIT
#include "core/reader_table.h"

#include <cstring>
#include <new>

namespace genonline_caster
{

namespace
{

// Events that wait for the host. The oldest HELLO_NEEDED is dropped first, so a host that never polls cannot grow the list.
constexpr size_t kMaxEvents = 64;

} // namespace

uint32_t ReaderTable::NextIdLocked()
{
	// Id 0 addresses every reader, so the counter skips it when it wraps.
	if (++m_idCounter == 0)
	{
		++m_idCounter;
	}
	return m_idCounter;
}

ReaderTable::Reader* ReaderTable::FindLocked(uint32_t readerId)
{
	if (readerId == 0)
	{
		return nullptr;
	}
	for (Reader& reader : m_readers)
	{
		if (reader.connected && reader.id == readerId)
		{
			return &reader;
		}
	}
	return nullptr;
}

void ReaderTable::PushEventLocked(uint32_t readerId, int32_t event)
{
	if (event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED)
	{
		for (auto it = m_events.begin(); it != m_events.end(); ++it)
		{
			if (it->readerId == readerId && it->event == event)
			{
				m_events.erase(it);
				break;
			}
		}
	}
	if (m_events.size() >= kMaxEvents)
	{
		auto oldest = m_events.begin();
		for (auto it = m_events.begin(); it != m_events.end(); ++it)
		{
			if (it->event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED)
			{
				oldest = it;
				break;
			}
		}
		m_events.erase(oldest);
	}
	m_events.push_back(Event{readerId, event});
}

// Only once the reader drained half its queue, so the frames that are sent again fit.
void ReaderTable::PromoteHelloLocked()
{
	for (Reader& reader : m_readers)
	{
		if (reader.connected && reader.helloWanted && reader.queue.FreeBytes() >= kQueueCapacityBytes / 2)
		{
			reader.helloWanted = false;
			PushEventLocked(reader.id, GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
		}
	}
}

void ReaderTable::ClearInbound(Reader& reader)
{
	reader.inbound.clear();
	reader.requestCount = 0;
	reader.refusalCount = 0;
	reader.chatCount = 0;
}

uint32_t ReaderTable::OnConnected(int slot, uint64_t nowMs)
{
	if (!ValidSlot(slot))
	{
		return 0;
	}
	// The reset and the new id happen under one lock, so no frame produced for an earlier reader reaches this one.
	std::lock_guard<std::mutex> lock(m_lock);
	Reader& reader = m_readers[slot];
	reader.connected = true;
	reader.streamStarted = false;
	reader.helloWanted = false;
	reader.id = NextIdLocked();
	reader.nextSequence = 0;
	reader.queue.Clear();
	reader.requests.Reset(nowMs);
	ClearInbound(reader);
	PushEventLocked(reader.id, GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	return reader.id;
}

void ReaderTable::OnClosed(int slot)
{
	if (!ValidSlot(slot))
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	Reader& reader = m_readers[slot];
	if (!reader.connected)
	{
		return;
	}
	PushEventLocked(reader.id, GENONLINE_PLUGIN_LOCAL_API_READER_GONE);
	reader.connected = false;
	reader.streamStarted = false;
	reader.helloWanted = false;
	reader.id = 0;
	reader.queue.Clear();
	ClearInbound(reader);
}

void ReaderTable::HandleInboundFrame(int slot, uint8_t frameType, const uint8_t* body, uint32_t bodyLength, uint64_t nowMs)
{
	if (!ValidSlot(slot) || body == nullptr || bodyLength == 0 || bodyLength > kMaxInboundBodyBytes)
	{
		return;
	}
	if (frameType != kInboundRequest && frameType != kInboundChatSend)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(m_lock);
	Reader& reader = m_readers[slot];
	// A frame that arrives before the HELLO was queued is dropped because the reader has not seen the limits yet.
	if (!reader.connected || !reader.streamStarted)
	{
		return;
	}

	InboundKind kind;
	if (frameType == kInboundChatSend)
	{
		if (reader.chatCount >= kChatMailboxSlots)
		{
			return;
		}
		kind = InboundKind::Chat;
	}
	else if (reader.requests.Allow(nowMs, reader.requestCount < kRequestQueueSlots))
	{
		kind = InboundKind::Request;
	}
	else if (reader.refusalCount < kRefusalSlots)
	{
		kind = InboundKind::Refusal;
	}
	else
	{
		return;
	}

	InboundEntry entry;
	entry.kind = kind;
	entry.frameType = frameType;
	entry.body.reset(new (std::nothrow) uint8_t[bodyLength]);
	if (!entry.body)
	{
		return;
	}
	std::memcpy(entry.body.get(), body, bodyLength);
	entry.bodyLength = bodyLength;
	try
	{
		reader.inbound.push_back(std::move(entry));
	}
	catch (...)
	{
		return;
	}
	switch (kind)
	{
		case InboundKind::Request:
			++reader.requestCount;
			break;
		case InboundKind::Refusal:
			++reader.refusalCount;
			break;
		case InboundKind::Chat:
			++reader.chatCount;
			break;
	}
}

uint32_t ReaderTable::Peek(int slot, uint8_t* dest, uint32_t capacity) const
{
	if (!ValidSlot(slot))
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	return m_readers[slot].queue.Peek(dest, capacity);
}

void ReaderTable::Consume(int slot, uint32_t length)
{
	if (!ValidSlot(slot))
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	m_readers[slot].queue.Consume(length);
}

uint32_t ReaderTable::FreeBytes(int slot) const
{
	if (!ValidSlot(slot))
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	return m_readers[slot].queue.FreeBytes();
}

bool ReaderTable::IsConnected() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	for (const Reader& reader : m_readers)
	{
		if (reader.connected)
		{
			return true;
		}
	}
	return false;
}

bool ReaderTable::EnqueueFrame(uint8_t frameType, int32_t frameClass, const void* body, uint32_t bodyLength, uint64_t timestampMicros, uint32_t readerId)
{
	if (body == nullptr || bodyLength == 0 || bodyLength > kMaxBodyBytes)
	{
		return false;
	}
	if (frameClass != GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO && frameClass != GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE
		&& frameClass != GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC)
	{
		return false;
	}
	const bool hello = frameClass == GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO;
	const bool periodic = frameClass == GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC;
	const uint8_t* bytes = static_cast<const uint8_t*>(body);

	// Connection state is checked under the lock a new connection resets its queue under, so a frame for an earlier reader never reaches the next one.
	std::lock_guard<std::mutex> lock(m_lock);
	bool pushedAny = false;
	for (Reader& reader : m_readers)
	{
		if (!reader.connected || (readerId == GENONLINE_PLUGIN_LOCAL_API_ALL_READERS ? !reader.streamStarted : reader.id != readerId))
		{
			continue;
		}
		if (!reader.streamStarted && !hello)
		{
			continue;
		}
		uint8_t header[kOutboundHeaderBytes];
		// The number is used up even when the frame is dropped, which is how the reader notices the gap.
		WriteOutboundHeader(header, bodyLength, frameType, reader.nextSequence++, timestampMicros);
		if (reader.queue.Push(header, kOutboundHeaderBytes, bytes, bodyLength, periodic ? kPeriodicReserveBytes : 0))
		{
			pushedAny = true;
			if (hello)
			{
				reader.streamStarted = true;
				reader.helloWanted = false;
			}
		}
		else if (!periodic)
		{
			// The queued frames are whole and stay, so the reader never sees a cut frame. Only new frames stop until a HELLO restarts the stream.
			reader.streamStarted = false;
			reader.helloWanted = true;
		}
	}
	return pushedAny;
}

bool ReaderTable::TakeReaderEvent(uint32_t* readerId, int32_t* event)
{
	if (readerId == nullptr || event == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	PromoteHelloLocked();
	if (m_events.empty())
	{
		return false;
	}
	*readerId = m_events.front().readerId;
	*event = m_events.front().event;
	m_events.pop_front();
	return true;
}

bool ReaderTable::TakeInbound(TakenInbound* out)
{
	if (out == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_lock);
	for (int k = 0; k < kMaxReaders; ++k)
	{
		const int i = static_cast<int>((m_inboundNext + k) % kMaxReaders);
		Reader& reader = m_readers[i];
		if (!reader.connected || !reader.streamStarted || reader.inbound.empty())
		{
			continue;
		}
		InboundEntry& entry = reader.inbound.front();
		switch (entry.kind)
		{
			case InboundKind::Request:
				--reader.requestCount;
				break;
			case InboundKind::Refusal:
				--reader.refusalCount;
				break;
			case InboundKind::Chat:
				--reader.chatCount;
				break;
		}
		out->frameType = entry.frameType;
		out->readerId = reader.id;
		out->flags = entry.kind == InboundKind::Refusal ? GENONLINE_PLUGIN_LOCAL_API_INBOUND_RATE_LIMITED : 0;
		out->bodyLength = entry.bodyLength;
		out->body = entry.body.release();
		reader.inbound.pop_front();
		m_inboundNext = static_cast<uint32_t>((i + 1) % kMaxReaders);
		return true;
	}
	return false;
}

} // namespace genonline_caster
