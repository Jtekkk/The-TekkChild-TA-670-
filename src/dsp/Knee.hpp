// TA-670 DSP core — static soft-knee gain computer (§6.2).
// SPDX-License-Identifier: MIT
#pragma once

namespace ta670::dsp {

/// Soft-knee gain-reduction command in dB (<= 0), C1-continuous at both knee
/// edges (Giannoulis et al. 2012; SPECIFICATION.md §6.2).
///
/// @param inputDb   detector level L_i (dB)
/// @param thresholdDb  threshold T (dB)
/// @param ratio     compression ratio R (>= 1)
/// @param kneeDb    knee width W (dB, > 0)
[[nodiscard]] inline float kneeGainDb(float inputDb, float thresholdDb,
                                      float ratio, float kneeDb) noexcept
{
    const float d = inputDb - thresholdDb;
    if (2.0f * d < -kneeDb)
        return 0.0f;
    const float invRm1 = 1.0f / ratio - 1.0f;
    if (2.0f * d > kneeDb)
        return invRm1 * d;
    const float x = d + 0.5f * kneeDb;
    return invRm1 * (x * x) / (2.0f * kneeDb);
}

}  // namespace ta670::dsp
