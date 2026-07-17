// TA-670 DSP core — shared constants and RT-safe numeric helpers.
// SPDX-License-Identifier: MIT
// Spec references: SPECIFICATION.md §5.2, §12.3, §13.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ta670::dsp {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Envelope floor used by the detection sidechain, dB (§5.1, §13.3).
inline constexpr double kDbFloor = -80.0;

/// Rectifier epsilon: -120 dBFS, keeps log() defined at digital silence (§5.1).
inline constexpr float kTinyLin = 1.0e-6f;

// ---------------------------------------------------------------------------
// One-pole coefficient (§5.2):  alpha = exp(-1 / (tau * fs)), in (0,1).
// Computed off the audio thread; the audio thread only consumes the result.
// ---------------------------------------------------------------------------
[[nodiscard]] inline double onePoleAlpha(double tauSeconds, double fs) noexcept
{
    return std::exp(-1.0 / (tauSeconds * fs));
}

// ---------------------------------------------------------------------------
// dB <-> linear (reference versions; prepare()-time use only).
// ---------------------------------------------------------------------------
[[nodiscard]] inline double dbToLin(double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

[[nodiscard]] inline double linToDb(double lin) noexcept
{
    return 20.0 * std::log10(lin);
}

// ---------------------------------------------------------------------------
// fastDb: audio-thread-safe 20*log10(|x|) without libm calls (§12.3, §13.1).
//
// Splits the float into exponent and mantissa via bit_cast, then corrects the
// mantissa in [1,2) with a cubic minimax fit of log2(m). Absolute error is
// below 0.01 dB across the normal range — ample for level detection.
// Input must be > 0 (callers add kTinyLin to the rectified signal).
// ---------------------------------------------------------------------------
[[nodiscard]] inline float fastDb(float x) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(x);
    const auto exponent =
        static_cast<int>((bits >> 23u) & 0xffu) - 127;
    const float m = std::bit_cast<float>((bits & 0x007fffffu) | 0x3f800000u);
    // ln(m), m in [1,2): cubic minimax, |err| < 4e-4 (0.004 dB after scaling).
    const float lnM =
        -1.49278f + (2.11263f + (-0.729104f + 0.10969f * m) * m) * m;
    constexpr float kDbPerOctave = 6.02059991f;  // 20*log10(2)
    constexpr float kDbPerNat = 8.68588964f;     // 20/ln(10)
    return kDbPerOctave * static_cast<float>(exponent) + kDbPerNat * lnM;
}

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
[[nodiscard]] inline float clampUnit(float x) noexcept
{
    return std::clamp(x, 0.0f, 1.0f);
}

}  // namespace ta670::dsp
