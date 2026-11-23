// SPDX-License-Identifier: MIT
// A bounded ring buffer of whole outbound frames. A frame that does not fit is dropped and never split.
#pragma once

#include <cstdint>
#include <memory>

namespace genonline_caster
{

class FrameQueue
{
public:
	// Keeps reserve bytes of the capacity free, so the frame is dropped when it would use them.
	bool Push(const uint8_t* header, uint32_t headerLength, const uint8_t* body, uint32_t bodyLength, uint32_t reserve);
	uint32_t FreeBytes() const;
	uint32_t Peek(uint8_t* dest, uint32_t capacity) const;
	void Consume(uint32_t length);
	void Clear();

private:
	void Write(uint32_t offset, const uint8_t* data, uint32_t length);

	// Allocated on the first push, so a slot that never gets a reader costs no memory.
	std::unique_ptr<uint8_t[]> m_buffer;
	uint32_t m_head = 0;
	uint32_t m_count = 0;
};

} // namespace genonline_caster
