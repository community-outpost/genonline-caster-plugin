// SPDX-License-Identifier: MIT
// Tests of the pure plugin logic: envelopes, inbound framing, the rate limit, the outbound queues and the reader ids.
#include "test_harness.h"

#include "core/api_table.h"
#include "core/frame_codec.h"
#include "core/frame_queue.h"
#include "core/reader_table.h"
#include "core/token_bucket.h"

#include <cstddef>
#include <cstring>
#include <vector>

using namespace genonline_caster;

namespace
{

struct Header
{
	uint32_t bodyLength;
	uint8_t frameType;
	uint32_t sequence;
	uint64_t timestamp;
};

uint32_t ReadU32(const uint8_t* bytes)
{
	return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16)
		| (static_cast<uint32_t>(bytes[3]) << 24);
}

Header ReadHeader(const uint8_t* bytes)
{
	Header header;
	header.bodyLength = ReadU32(bytes);
	header.frameType = bytes[4];
	header.sequence = ReadU32(bytes + 5);
	header.timestamp = 0;
	for (int i = 0; i < 8; ++i)
	{
		header.timestamp |= static_cast<uint64_t>(bytes[9 + i]) << (8 * i);
	}
	return header;
}

// Takes every queued frame of slot and returns their headers.
std::vector<Header> Drain(ReaderTable& table, int slot)
{
	std::vector<uint8_t> bytes(kQueueCapacityBytes);
	const uint32_t length = table.Peek(slot, bytes.data(), static_cast<uint32_t>(bytes.size()));
	table.Consume(slot, length);
	std::vector<Header> headers;
	uint32_t offset = 0;
	while (offset + kOutboundHeaderBytes <= length)
	{
		const Header header = ReadHeader(bytes.data() + offset);
		headers.push_back(header);
		offset += kOutboundHeaderBytes + header.bodyLength;
	}
	return headers;
}

// Connects slot, queues a HELLO for it so its stream starts, and drains that frame. Returns the reader id.
uint32_t ConnectStarted(ReaderTable& table, int slot, uint64_t nowMs = 0)
{
	const uint32_t id = table.OnConnected(slot, nowMs);
	const uint8_t hello[1] = {0x09};
	table.EnqueueFrame(0x09, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, hello, sizeof(hello), 0, id);
	std::vector<uint8_t> bytes(kOutboundHeaderBytes + sizeof(hello));
	table.Consume(slot, table.Peek(slot, bytes.data(), static_cast<uint32_t>(bytes.size())));
	return id;
}

bool Enqueue(ReaderTable& table, int32_t frameClass, uint32_t bodyLength, uint32_t readerId, uint8_t frameType = 0x02)
{
	std::vector<uint8_t> body(bodyLength, 0xAB);
	return table.EnqueueFrame(frameType, frameClass, body.data(), bodyLength, 1234, readerId);
}

struct Event
{
	uint32_t readerId;
	int32_t event;
};

std::vector<Event> TakeEvents(ReaderTable& table)
{
	std::vector<Event> events;
	Event event;
	while (table.TakeReaderEvent(&event.readerId, &event.event))
	{
		events.push_back(event);
	}
	return events;
}

void SendInbound(ReaderTable& table, int slot, uint8_t frameType, uint8_t marker, uint64_t nowMs)
{
	const uint8_t body[2] = {0x08, marker};
	table.HandleInboundFrame(slot, frameType, body, sizeof(body), nowMs);
}

// Takes every inbound frame and frees its body.
std::vector<TakenInbound> TakeAll(ReaderTable& table)
{
	std::vector<TakenInbound> frames;
	TakenInbound taken;
	while (table.TakeInbound(&taken))
	{
		frames.push_back(taken);
		delete[] taken.body;
		frames.back().body = nullptr;
	}
	return frames;
}

constexpr uint32_t kFullFrame = kMaxBodyBytes;

} // namespace

TEST_CASE(outbound_header_is_little_endian)
{
	uint8_t header[kOutboundHeaderBytes];
	WriteOutboundHeader(header, 0x04030201u, 0x09, 0x08070605u, 0x100F0E0D0C0B0A09ull);
	const uint8_t expected[kOutboundHeaderBytes] = {0x01, 0x02, 0x03, 0x04, 0x09, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
	CHECK(std::memcmp(header, expected, sizeof(expected)) == 0);
}

TEST_CASE(inbound_parse_waits_for_the_whole_frame)
{
	const uint8_t frame[] = {0x03, 0x00, 0x00, 0x00, 0x11, 0xAA, 0xBB, 0xCC};
	uint8_t type = 0;
	uint32_t length = 0;
	for (uint32_t have = 0; have < sizeof(frame); ++have)
	{
		CHECK(ParseInbound(frame, have, &type, &length) == InboundParse::NeedMore);
	}
	REQUIRE(ParseInbound(frame, sizeof(frame), &type, &length) == InboundParse::Frame);
	CHECK(type == 0x11);
	CHECK(length == 3);
}

TEST_CASE(inbound_parse_rejects_zero_and_oversized_lengths)
{
	uint8_t type = 0;
	uint32_t length = 0;
	const uint8_t zero[] = {0x00, 0x00, 0x00, 0x00, 0x11};
	CHECK(ParseInbound(zero, sizeof(zero), &type, &length) == InboundParse::BadLength);
	const uint8_t over[] = {0x01, 0x10, 0x00, 0x00, 0x11};
	CHECK(ParseInbound(over, sizeof(over), &type, &length) == InboundParse::BadLength);
	const uint8_t huge[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x11};
	CHECK(ParseInbound(huge, sizeof(huge), &type, &length) == InboundParse::BadLength);
	const uint8_t max[] = {0x00, 0x10, 0x00, 0x00, 0x11};
	CHECK(ParseInbound(max, sizeof(max), &type, &length) == InboundParse::NeedMore);
}

TEST_CASE(token_bucket_allows_the_burst_then_the_rate)
{
	TokenBucket bucket;
	bucket.Reset(1000);
	for (uint32_t i = 0; i < kRequestBurst; ++i)
	{
		CHECK(bucket.Allow(1000, true));
	}
	CHECK(!bucket.Allow(1000, true));
	// 20 per second refills one token every 50 ms.
	CHECK(bucket.Allow(1050, true));
	CHECK(!bucket.Allow(1050, true));
	CHECK(!bucket.Allow(1100, false));
	CHECK(bucket.Allow(1100, true));
	CHECK(bucket.Allow(100000, true));
	CHECK(bucket.Tokens() <= static_cast<double>(kRequestBurst));
}

TEST_CASE(frame_queue_drops_a_frame_that_does_not_fit)
{
	FrameQueue queue;
	std::vector<uint8_t> body(kFullFrame, 1);
	uint8_t header[kOutboundHeaderBytes] = {};
	int pushed = 0;
	while (queue.Push(header, kOutboundHeaderBytes, body.data(), kFullFrame, 0))
	{
		++pushed;
	}
	CHECK(pushed == 3);
	CHECK(queue.FreeBytes() == kQueueCapacityBytes - 3 * (kOutboundHeaderBytes + kFullFrame));
	CHECK(!queue.Push(header, kOutboundHeaderBytes, body.data(), 0, 0));
	CHECK(!queue.Push(header, kOutboundHeaderBytes, body.data(), kMaxBodyBytes + 1, 0));
}

TEST_CASE(frame_queue_wraps_around)
{
	FrameQueue queue;
	std::vector<uint8_t> body(kFullFrame);
	uint8_t header[kOutboundHeaderBytes] = {};
	for (int round = 0; round < 10; ++round)
	{
		for (size_t i = 0; i < body.size(); ++i)
		{
			body[i] = static_cast<uint8_t>(i + round);
		}
		REQUIRE(queue.Push(header, kOutboundHeaderBytes, body.data(), kFullFrame, 0));
		std::vector<uint8_t> out(kOutboundHeaderBytes + kFullFrame);
		REQUIRE(queue.Peek(out.data(), static_cast<uint32_t>(out.size())) == out.size());
		CHECK(std::memcmp(out.data() + kOutboundHeaderBytes, body.data(), kFullFrame) == 0);
		queue.Consume(static_cast<uint32_t>(out.size()));
	}
	CHECK(queue.FreeBytes() == kQueueCapacityBytes);
}

TEST_CASE(dropped_frame_leaves_a_sequence_gap)
{
	ReaderTable table;
	const uint32_t id = ConnectStarted(table, 0);
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	const std::vector<Header> first = Drain(table, 0);
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 10, id, 0x03));
	const std::vector<Header> second = Drain(table, 0);
	// The HELLO of ConnectStarted took sequence 0.
	REQUIRE(first.size() == 3);
	CHECK(first[0].sequence == 1 && first[1].sequence == 2 && first[2].sequence == 3);
	CHECK(first[0].bodyLength == kFullFrame);
	CHECK(first[0].timestamp == 1234);
	REQUIRE(second.size() == 1);
	CHECK(second[0].sequence == 5);
	CHECK(second[0].frameType == 0x03);
	CHECK(second[0].bodyLength == 10);
}

TEST_CASE(periodic_frames_leave_the_reserve_free)
{
	ReaderTable table;
	const uint32_t id = ConnectStarted(table, 1);
	for (int i = 0; i < 3; ++i)
	{
		REQUIRE(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	}
	// The free space holds 200000 bytes and the header but not the reserve on top.
	REQUIRE(table.FreeBytes(1) < 200000 + kOutboundHeaderBytes + kPeriodicReserveBytes);
	REQUIRE(table.FreeBytes(1) >= 200000 + kOutboundHeaderBytes);
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, 200000, id));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 200000, id));
	// A dropped PERIODIC frame neither ends the stream nor asks for a HELLO.
	Drain(table, 1);
	CHECK(TakeEvents(table).size() == 1);
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, 4, id));
}

TEST_CASE(lost_lifecycle_frame_ends_the_stream_and_reports_hello_needed_once_there_is_room)
{
	ReaderTable table;
	const uint32_t id = ConnectStarted(table, 2);
	TakeEvents(table);
	for (int i = 0; i < 3; ++i)
	{
		REQUIRE(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, kFullFrame, id));
	}
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, kFullFrame, id));
	CHECK(TakeEvents(table).empty());
	// The stream has ended, so no class but HELLO is taken.
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 4, id));
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, 4, GENONLINE_PLUGIN_LOCAL_API_ALL_READERS));
	Drain(table, 2);
	const std::vector<Event> events = TakeEvents(table);
	REQUIRE(events.size() == 1);
	CHECK(events[0].readerId == id);
	CHECK(events[0].event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	CHECK(TakeEvents(table).empty());
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 4, id, 0x09));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 4, id));
	const std::vector<Header> frames = Drain(table, 2);
	REQUIRE(frames.size() == 2);
	// The reader keeps its sequence numbers, so the lost frames show as a gap.
	CHECK(frames[0].frameType == 0x09);
	CHECK(frames[1].sequence == frames[0].sequence + 1);
	CHECK(frames[0].sequence > 4);
}

TEST_CASE(reader_ids_are_nonzero_and_never_reused)
{
	ReaderTable table;
	std::vector<uint32_t> seen;
	for (int round = 0; round < 6; ++round)
	{
		const uint32_t id = table.OnConnected(round % 2, 0);
		CHECK(id != 0);
		for (uint32_t earlier : seen)
		{
			CHECK(id != earlier);
		}
		seen.push_back(id);
		table.OnClosed(round % 2);
	}
	// The id of a closed reader is dead.
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, seen[0]));
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, seen.back()));
}

TEST_CASE(a_hello_starts_the_stream_and_nothing_else_does)
{
	ReaderTable table;
	CHECK(!table.IsConnected());
	const uint32_t id = table.OnConnected(3, 0);
	CHECK(table.IsConnected());
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 8, id));
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, 8, id));
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_PERIODIC, 8, GENONLINE_PLUGIN_LOCAL_API_ALL_READERS));
	CHECK(Drain(table, 3).empty());
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, id, 0x09));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, id, 0x09));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 8, id));
	const std::vector<Header> frames = Drain(table, 3);
	REQUIRE(frames.size() == 3);
	CHECK(frames[0].frameType == 0x09);
	CHECK(frames[0].sequence == 0);
	table.OnClosed(3);
	CHECK(!table.IsConnected());
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, id));
}

TEST_CASE(enqueue_rejects_bad_arguments)
{
	ReaderTable table;
	const uint32_t id = ConnectStarted(table, 0);
	const uint8_t body[1] = {1};
	CHECK(!table.EnqueueFrame(1, 3, body, 1, 0, id));
	CHECK(!table.EnqueueFrame(1, -1, body, 1, 0, id));
	CHECK(!table.EnqueueFrame(1, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, nullptr, 1, 0, id));
	CHECK(!table.EnqueueFrame(1, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, body, 0, 0, id));
	CHECK(!table.EnqueueFrame(1, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, body, kMaxBodyBytes + 1, 0, id));
	CHECK(!table.EnqueueFrame(1, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, body, 1, 0, id + 100));
	CHECK(Drain(table, 0).empty());
}

TEST_CASE(broadcast_reaches_every_reader_whose_stream_started)
{
	ReaderTable table;
	ConnectStarted(table, 0);
	ConnectStarted(table, 2);
	table.OnConnected(1, 0);
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 8, GENONLINE_PLUGIN_LOCAL_API_ALL_READERS));
	CHECK(Drain(table, 0).size() == 1);
	CHECK(Drain(table, 1).empty());
	CHECK(Drain(table, 2).size() == 1);
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 8, 9999));
}

TEST_CASE(reconnect_resets_sequence_and_queue)
{
	ReaderTable table;
	const uint32_t first = ConnectStarted(table, 0);
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, 8, first));
	table.OnClosed(0);
	const uint32_t second = table.OnConnected(0, 0);
	CHECK(second != 0 && second != first);
	CHECK(table.FreeBytes(0) == kQueueCapacityBytes);
	CHECK(!Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, first));
	CHECK(Enqueue(table, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, 8, second));
	const std::vector<Header> frames = Drain(table, 0);
	REQUIRE(frames.size() == 1);
	CHECK(frames[0].sequence == 0);
}

TEST_CASE(events_come_in_order_and_hello_needed_is_not_doubled)
{
	ReaderTable table;
	const uint32_t a = table.OnConnected(0, 0);
	const uint32_t b = table.OnConnected(1, 0);
	table.OnClosed(0);
	std::vector<Event> events = TakeEvents(table);
	REQUIRE(events.size() == 3);
	CHECK(events[0].readerId == a && events[0].event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	CHECK(events[1].readerId == b && events[1].event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	CHECK(events[2].readerId == a && events[2].event == GENONLINE_PLUGIN_LOCAL_API_READER_GONE);
	CHECK(TakeEvents(table).empty());

	// A host that never polls cannot grow the list.
	for (int i = 0; i < 200; ++i)
	{
		table.OnConnected(0, 0);
		table.OnClosed(0);
	}
	CHECK(TakeEvents(table).size() <= 64);
	uint32_t id = 0;
	int32_t event = 0;
	CHECK(!table.TakeReaderEvent(nullptr, &event));
	CHECK(!table.TakeReaderEvent(&id, nullptr));
}

TEST_CASE(inbound_waits_for_the_hello)
{
	ReaderTable table;
	const uint32_t id = table.OnConnected(1, 0);
	SendInbound(table, 1, kInboundRequest, 1, 0);
	SendInbound(table, 1, kInboundChatSend, 2, 0);
	CHECK(TakeAll(table).empty());
	const uint8_t hello[1] = {0x09};
	table.EnqueueFrame(0x09, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, hello, sizeof(hello), 0, id);

	SendInbound(table, 1, kInboundRequest, 3, 0);
	TakenInbound taken;
	REQUIRE(table.TakeInbound(&taken));
	CHECK(taken.frameType == kInboundRequest);
	CHECK(taken.readerId == id);
	CHECK(taken.flags == 0);
	REQUIRE(taken.bodyLength == 2);
	CHECK(taken.body[0] == 0x08 && taken.body[1] == 3);
	delete[] taken.body;
	CHECK(!table.TakeInbound(&taken));
}

TEST_CASE(unknown_inbound_types_are_skipped)
{
	ReaderTable table;
	ConnectStarted(table, 0);
	SendInbound(table, 0, 0x12, 1, 0);
	SendInbound(table, 0, 0x01, 1, 0);
	CHECK(TakeAll(table).empty());
}

TEST_CASE(requests_past_the_limit_become_bounded_refusals)
{
	ReaderTable table;
	ConnectStarted(table, 0, 0);
	// The queue holds kRequestQueueSlots, then refusals take kRefusalSlots, then the rest is dropped.
	for (int i = 0; i < 100; ++i)
	{
		SendInbound(table, 0, kInboundRequest, static_cast<uint8_t>(i), 0);
	}
	const std::vector<TakenInbound> frames = TakeAll(table);
	REQUIRE(frames.size() == kRequestQueueSlots + kRefusalSlots);
	for (size_t i = 0; i < frames.size(); ++i)
	{
		const uint32_t expected = i < kRequestQueueSlots ? 0u : GENONLINE_PLUGIN_LOCAL_API_INBOUND_RATE_LIMITED;
		CHECK(frames[i].flags == expected);
		CHECK(frames[i].frameType == kInboundRequest);
	}
}

TEST_CASE(empty_bucket_refuses_even_with_queue_room)
{
	ReaderTable table;
	ConnectStarted(table, 0, 0);
	int accepted = 0;
	int refused = 0;
	for (uint32_t i = 0; i < kRequestBurst + 2; ++i)
	{
		SendInbound(table, 0, kInboundRequest, 0, 0);
		for (const TakenInbound& frame : TakeAll(table))
		{
			(frame.flags == 0 ? accepted : refused) += 1;
		}
	}
	CHECK(accepted == static_cast<int>(kRequestBurst));
	CHECK(refused == 2);
	// After 50 ms one token is back.
	SendInbound(table, 0, kInboundRequest, 0, 50);
	const std::vector<TakenInbound> later = TakeAll(table);
	REQUIRE(later.size() == 1);
	CHECK(later[0].flags == 0);
}

TEST_CASE(chat_mailbox_holds_eight)
{
	ReaderTable table;
	ConnectStarted(table, 0);
	for (int i = 0; i < 20; ++i)
	{
		SendInbound(table, 0, kInboundChatSend, static_cast<uint8_t>(i), 0);
	}
	const std::vector<TakenInbound> frames = TakeAll(table);
	CHECK(frames.size() == kChatMailboxSlots);
	for (const TakenInbound& frame : frames)
	{
		CHECK(frame.frameType == kInboundChatSend);
		CHECK(frame.flags == 0);
	}
	SendInbound(table, 0, kInboundChatSend, 1, 0);
	CHECK(TakeAll(table).size() == 1);
}

TEST_CASE(inbound_takes_the_readers_in_turn)
{
	ReaderTable table;
	const uint32_t a = ConnectStarted(table, 0);
	const uint32_t b = ConnectStarted(table, 2);
	for (int i = 0; i < 3; ++i)
	{
		SendInbound(table, 0, kInboundRequest, static_cast<uint8_t>(i), 0);
		SendInbound(table, 2, kInboundRequest, static_cast<uint8_t>(10 + i), 0);
	}
	const std::vector<TakenInbound> frames = TakeAll(table);
	REQUIRE(frames.size() == 6);
	const uint32_t expectedIds[6] = {a, b, a, b, a, b};
	for (int i = 0; i < 6; ++i)
	{
		CHECK(frames[i].readerId == expectedIds[i]);
	}
}

TEST_CASE(closing_a_reader_drops_its_inbound_frames)
{
	ReaderTable table;
	ConnectStarted(table, 0);
	SendInbound(table, 0, kInboundRequest, 1, 0);
	table.OnClosed(0);
	CHECK(TakeAll(table).empty());
}

TEST_CASE(plugin_table_fills_only_whole_fields)
{
	GenOnlinePluginLocalApi localApi = {};
	GenOnlinePluginApi complete = {};
	complete.structSize = sizeof(complete);
	complete.apiVersion = GENONLINE_PLUGIN_API_VERSION;
	complete.capabilities = GENONLINE_PLUGIN_CAP_LOCAL_API;
	complete.localApi = &localApi;

	GenOnlinePluginApi full = {};
	full.structSize = sizeof(full);
	REQUIRE(FillPluginApi(complete, &full) == GENONLINE_PLUGIN_RESULT_OK);
	CHECK(full.structSize == sizeof(GenOnlinePluginApi));
	CHECK(full.localApi == &localApi);

	GenOnlinePluginApi shorter = {};
	shorter.structSize = static_cast<uint32_t>(offsetof(GenOnlinePluginApi, localApi) + 2);
	REQUIRE(FillPluginApi(complete, &shorter) == GENONLINE_PLUGIN_RESULT_OK);
	CHECK(shorter.structSize == offsetof(GenOnlinePluginApi, localApi));
	CHECK(shorter.localApi == nullptr);
	CHECK(shorter.capabilities == GENONLINE_PLUGIN_CAP_LOCAL_API);

	GenOnlinePluginApi tiny = {};
	tiny.structSize = 2;
	CHECK(FillPluginApi(complete, &tiny) == GENONLINE_PLUGIN_RESULT_INVALID_ARGUMENT);
	CHECK(CheckGetApiArguments(GENONLINE_PLUGIN_API_VERSION + 1, nullptr, nullptr) == GENONLINE_PLUGIN_RESULT_VERSION_MISMATCH);
}

int main(int argc, char** argv)
{
	return testing::RunMain(argc, argv);
}
