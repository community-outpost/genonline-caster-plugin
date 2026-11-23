// SPDX-License-Identifier: MIT
// Limits and envelopes of the live caster pipe. The protocol is described in GenOnline/docs/stats-exporter.md of the game.
#pragma once

#include <GenOnlinePluginApi.h>

#include <cstdint>

namespace genonline_caster
{

// The most readers the host may ask for, and the number of reader slots the table holds.
constexpr int kMaxReaders = 16;

// Outbound envelope: u32 body length, u8 type, u32 sequence and u64 timestamp in microseconds, all little endian.
constexpr uint32_t kOutboundHeaderBytes = 17;

// Inbound envelope: u32 body length and u8 type.
constexpr uint32_t kInboundHeaderBytes = 5;

// Largest inbound frame body. A length of 0 or above this drops the reader.
constexpr uint32_t kMaxInboundBodyBytes = 4096;

// Largest outbound frame body that is queued.
constexpr uint32_t kMaxBodyBytes = 262144;

// Outbound queue capacity of every reader.
constexpr uint32_t kQueueCapacityBytes = 1048576;

// Queue space that PERIODIC frames leave free for the other classes.
constexpr uint32_t kPeriodicReserveBytes = 65536;

// Request limits of every reader: a token bucket with this sustained rate and burst, and this many queued requests.
constexpr uint32_t kMaxRequestsPerSecond = 20;
constexpr uint32_t kRequestBurst = 40;
constexpr uint32_t kRequestQueueSlots = 16;

// Refused requests that wait for the host to answer them. Further refusals are dropped so a flood cannot grow memory.
constexpr uint32_t kRefusalSlots = 4;

// ChatSend frames that wait for the host. Further ones are dropped.
constexpr uint32_t kChatMailboxSlots = 8;

constexpr uint8_t kInboundChatSend = 0x10;
constexpr uint8_t kInboundRequest = 0x11;

// Writes the kOutboundHeaderBytes envelope of one frame.
void WriteOutboundHeader(uint8_t* header, uint32_t bodyLength, uint8_t frameType, uint32_t sequence, uint64_t timestampMicros);

enum class InboundParse
{
	NeedMore,
	Frame,
	BadLength,
};

// Reads the inbound envelope at the start of buffer. Frame means the whole frame of kInboundHeaderBytes plus bodyLength is buffered.
InboundParse ParseInbound(const uint8_t* buffer, uint32_t length, uint8_t* frameType, uint32_t* bodyLength);

} // namespace genonline_caster
