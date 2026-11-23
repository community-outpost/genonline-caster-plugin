// SPDX-License-Identifier: MIT
#include "core/frame_queue.h"

#include "core/frame_codec.h"

#include <cstring>
#include <new>

namespace genonline_caster
{

void FrameQueue::Clear()
{
	m_head = 0;
	m_count = 0;
}

void FrameQueue::Write(uint32_t offset, const uint8_t* data, uint32_t length)
{
	uint32_t tail = m_head + m_count + offset;
	if (tail >= kQueueCapacityBytes)
	{
		tail -= kQueueCapacityBytes;
	}
	uint32_t first = kQueueCapacityBytes - tail;
	if (first > length)
	{
		first = length;
	}
	std::memcpy(m_buffer.get() + tail, data, first);
	if (length > first)
	{
		std::memcpy(m_buffer.get(), data + first, length - first);
	}
}

uint32_t FrameQueue::FreeBytes() const
{
	return kQueueCapacityBytes - m_count;
}

bool FrameQueue::Push(const uint8_t* header, uint32_t headerLength, const uint8_t* body, uint32_t bodyLength, uint32_t reserve)
{
	if (header == nullptr || body == nullptr || bodyLength == 0 || bodyLength > kMaxBodyBytes)
	{
		return false;
	}
	const uint32_t length = headerLength + bodyLength;
	if (length + reserve > kQueueCapacityBytes - m_count)
	{
		return false;
	}
	if (!m_buffer)
	{
		m_buffer.reset(new (std::nothrow) uint8_t[kQueueCapacityBytes]);
		if (!m_buffer)
		{
			return false;
		}
	}
	Write(0, header, headerLength);
	Write(headerLength, body, bodyLength);
	m_count += length;
	return true;
}

uint32_t FrameQueue::Peek(uint8_t* dest, uint32_t capacity) const
{
	if (!m_buffer || m_count == 0 || capacity == 0)
	{
		return 0;
	}
	uint32_t available = m_count;
	if (available > capacity)
	{
		available = capacity;
	}
	uint32_t first = kQueueCapacityBytes - m_head;
	if (first > available)
	{
		first = available;
	}
	std::memcpy(dest, m_buffer.get() + m_head, first);
	if (available > first)
	{
		std::memcpy(dest + first, m_buffer.get(), available - first);
	}
	return available;
}

void FrameQueue::Consume(uint32_t length)
{
	if (length > m_count)
	{
		length = m_count;
	}
	m_head += length;
	if (m_head >= kQueueCapacityBytes)
	{
		m_head -= kQueueCapacityBytes;
	}
	m_count -= length;
}

} // namespace genonline_caster
