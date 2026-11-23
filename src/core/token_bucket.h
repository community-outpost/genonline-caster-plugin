// SPDX-License-Identifier: MIT
// The request rate limit of one reader.
#pragma once

#include <cstdint>

namespace genonline_caster
{

class TokenBucket
{
public:
	// Fills the bucket to the burst size.
	void Reset(uint64_t nowMs);

	// Refills for the time since the last call and spends one token when one is left and hasRoom is true.
	bool Allow(uint64_t nowMs, bool hasRoom);

	double Tokens() const
	{
		return m_tokens;
	}

private:
	double m_tokens = 0.0;
	uint64_t m_refillMs = 0;
};

} // namespace genonline_caster
