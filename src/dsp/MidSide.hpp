// TA-670 DSP core — orthonormal Mid-Side (LAT/VERT) matrix (§7.2).
// SPDX-License-Identifier: MIT
#pragma once

namespace ta670::dsp {

/// 1/sqrt(2): the orthonormal normalization — energy-preserving and exactly
/// self-inverse, so bypass reconstruction nulls (REQ-006).
inline constexpr float kMsNorm = 0.70710678118654752f;

inline void msEncode(float l, float r, float& m, float& s) noexcept
{
    m = kMsNorm * (l + r);
    s = kMsNorm * (l - r);
}

inline void msDecode(float m, float s, float& l, float& r) noexcept
{
    l = kMsNorm * (m + s);
    r = kMsNorm * (m - s);
}

}  // namespace ta670::dsp
