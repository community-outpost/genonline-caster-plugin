// SPDX-License-Identifier: MIT
#include "core/token_bucket.h"

#include "core/frame_codec.h"

namespace genonline_caster
{

void TokenBucket::Reset(uint64_t nowMs)
{
	m_tokens = static_cast<double>(kRequestBurst);
	m_refillMs = nowMs;
}

bool TokenBucket::Allow(uint64_t nowMs, bool hasRoom)
{
	const double refill = static_cast<double>(nowMs - m_refillMs) * static_cast<double>(kMaxRequestsPerSecond) / 1000.0;
	m_refillMs = nowMs;
	m_tokens += refill;
	if (m_tokens > static_cast<double>(kRequestBurst))
	{
		m_tokens = static_cast<double>(kRequestBurst);
	}
	if (m_tokens < 1.0 || !hasRoom)
	{
		return false;
	}
	m_tokens -= 1.0;
	return true;
}

} // namespace genonline_caster
