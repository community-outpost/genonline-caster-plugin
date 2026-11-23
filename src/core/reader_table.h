// SPDX-License-Identifier: MIT
// The state of the reader slots that the pipe worker and the host share: reader ids, events, outbound queues and inbound frames.
#pragma once

#include "core/frame_codec.h"
#include "core/frame_queue.h"
#include "core/token_bucket.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace genonline_caster
{

// One inbound frame that TakeInbound hands out. The body is a new[] array that the receiver deletes.
struct TakenInbound
{
	uint32_t readerId = 0;
	uint8_t frameType = 0;
	uint32_t flags = 0;
	uint8_t* body = nullptr;
	uint32_t bodyLength = 0;
};

class ReaderTable
{
public:
	// A new reader in slot gets the next reader id, an empty queue and a full request bucket, and a HELLO_NEEDED event. Returns the id.
	uint32_t OnConnected(int slot, uint64_t nowMs);
	// Ends the reader of slot and queues its GONE event.
	void OnClosed(int slot);
	// Queues one inbound frame of a reader whose stream has started. Requests past the rate limit are queued as refusals while there is room for them.
	void HandleInboundFrame(int slot, uint8_t frameType, const uint8_t* body, uint32_t bodyLength, uint64_t nowMs);
	uint32_t Peek(int slot, uint8_t* dest, uint32_t capacity) const;
	void Consume(int slot, uint32_t length);

	bool IsConnected() const;
	bool EnqueueFrame(uint8_t frameType, int32_t frameClass, const void* body, uint32_t bodyLength, uint64_t timestampMicros, uint32_t readerId);
	bool TakeReaderEvent(uint32_t* readerId, int32_t* event);
	bool TakeInbound(TakenInbound* out);

	// Free outbound queue bytes of slot.
	uint32_t FreeBytes(int slot) const;

private:
	enum class InboundKind
	{
		Request,
		Refusal,
		Chat,
	};

	struct InboundEntry
	{
		InboundKind kind = InboundKind::Request;
		uint8_t frameType = 0;
		std::unique_ptr<uint8_t[]> body;
		uint32_t bodyLength = 0;
	};

	struct Reader
	{
		bool connected = false;
		bool streamStarted = false;
		// The stream ended after a lost frame, and HELLO_NEEDED waits for room in the queue.
		bool helloWanted = false;
		// Reader id, 0 while the slot is empty.
		uint32_t id = 0;
		uint32_t nextSequence = 0;
		FrameQueue queue;
		TokenBucket requests;
		std::deque<InboundEntry> inbound;
		uint32_t requestCount = 0;
		uint32_t refusalCount = 0;
		uint32_t chatCount = 0;
	};

	static bool ValidSlot(int slot)
	{
		return slot >= 0 && slot < kMaxReaders;
	}

	struct Event
	{
		uint32_t readerId;
		int32_t event;
	};

	uint32_t NextIdLocked();
	Reader* FindLocked(uint32_t readerId);
	void PushEventLocked(uint32_t readerId, int32_t event);
	void PromoteHelloLocked();
	static void ClearInbound(Reader& reader);

	mutable std::mutex m_lock;
	Reader m_readers[kMaxReaders];
	std::deque<Event> m_events;
	uint32_t m_idCounter = 0;
	uint32_t m_inboundNext = 0;
};

} // namespace genonline_caster
