// SPDX-License-Identifier: MIT
#include "core/frame_codec.h"

namespace genonline_caster
{

void WriteOutboundHeader(uint8_t* header, uint32_t bodyLength, uint8_t frameType, uint32_t sequence, uint64_t timestampMicros)
{
	for (int i = 0; i < 4; ++i)
	{
		header[i] = static_cast<uint8_t>((bodyLength >> (8 * i)) & 0xFF);
	}
	header[4] = frameType;
	for (int i = 0; i < 4; ++i)
	{
		header[5 + i] = static_cast<uint8_t>((sequence >> (8 * i)) & 0xFF);
	}
	for (int i = 0; i < 8; ++i)
	{
		header[9 + i] = static_cast<uint8_t>((timestampMicros >> (8 * i)) & 0xFF);
	}
}

InboundParse ParseInbound(const uint8_t* buffer, uint32_t length, uint8_t* frameType, uint32_t* bodyLength)
{
	if (length < kInboundHeaderBytes)
	{
		return InboundParse::NeedMore;
	}
	const uint32_t body = static_cast<uint32_t>(buffer[0]) | (static_cast<uint32_t>(buffer[1]) << 8) | (static_cast<uint32_t>(buffer[2]) << 16)
		| (static_cast<uint32_t>(buffer[3]) << 24);
	// A byte mode pipe cannot resynchronize on a bad length, so the caller drops the reader.
	if (body == 0 || body > kMaxInboundBodyBytes)
	{
		return InboundParse::BadLength;
	}
	if (length < kInboundHeaderBytes + body)
	{
		return InboundParse::NeedMore;
	}
	*frameType = buffer[4];
	*bodyLength = body;
	return InboundParse::Frame;
}

} // namespace genonline_caster
