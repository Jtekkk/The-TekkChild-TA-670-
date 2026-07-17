// TA-670 DSP core — variable-mu gain cell (§4).
// SPDX-License-Identifier: MIT
//
// Implements the calibrated remote-cutoff gain law
//     g_dB(u) = -Gmax * [(1-theta)*u + theta*u^p],  u in [0,1]        (§4.2)
// with (theta, p) solved from the target grazing/full-drive ratios (§4.2,
// "Calibration"). The linear gain 10^(g_dB/20) is precomputed into a LUT at
// prepare() time; the audio thread performs only a table interpolation
// (§4.4, §12.3 — no transcendentals per sample).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "Common.hpp"

namespace ta670::dsp {

struct CellCalibration {
    double gMaxDb = 40.0;       ///< maximum gain reduction (dB)
    double controlRangeDb = 2.5;///< output-overshoot span U of the control range
    double ratioGrazing = 2.0;  ///< R0: ratio as drive -> 0
    double ratioFull = 30.0;    ///< R1: ratio at full control drive
};

class GainCell {
public:
    static constexpr std::size_t kTableSize = 1024;

    /// Off-thread: solve (theta, p) and build the linear-gain LUT.
    void prepare(const CellCalibration& cal) noexcept
    {
        cal_ = cal;
        const double k0 = cal.ratioGrazing - 1.0;
        const double k1 = cal.ratioFull - 1.0;
        theta_ = 1.0 - k0 * cal.controlRangeDb / cal.gMaxDb;
        p_ = (k1 - k0) * cal.controlRangeDb / (cal.gMaxDb * theta_);
        for (std::size_t i = 0; i < kTableSize; ++i) {
            const double u =
                static_cast<double>(i) / static_cast<double>(kTableSize - 1);
            table_[i] = static_cast<float>(dbToLin(-reductionDb(u)));
        }
        gainLin_ = 1.0f;
    }

    void reset() noexcept { gainLin_ = 1.0f; }

    /// Closed-form gain reduction magnitude in dB (>= 0). Reference /
    /// prepare()-time use; the audio thread uses the LUT.
    [[nodiscard]] double reductionDb(double u) const noexcept
    {
        return cal_.gMaxDb * ((1.0 - theta_) * u + theta_ * std::pow(u, p_));
    }

    /// Audio thread: linear gain for normalized control drive u (LUT +
    /// linear interpolation; no allocation, no libm).
    [[nodiscard]] float linearGain(float u) const noexcept
    {
        const float pos = clampUnit(u) * static_cast<float>(kTableSize - 1);
        const auto i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = (i0 + 1 < kTableSize) ? i0 + 1 : i0;
        const float frac = pos - static_cast<float>(i0);
        return table_[i0] + frac * (table_[i1] - table_[i0]);
    }

    /// Set the control drive for the NEXT sample (§3.3 unit-delay loop).
    void setControl(float u) noexcept { gainLin_ = linearGain(u); }

    [[nodiscard]] float currentGain() const noexcept { return gainLin_; }

    [[nodiscard]] double theta() const noexcept { return theta_; }
    [[nodiscard]] double tailExponent() const noexcept { return p_; }
    [[nodiscard]] const CellCalibration& calibration() const noexcept
    {
        return cal_;
    }

private:
    CellCalibration cal_{};
    double theta_ = 0.9375;
    double p_ = 1.8666666;
    float gainLin_ = 1.0f;
    std::array<float, kTableSize> table_{};
};

/// Push-pull odd waveshaper (§4.3): y = tanh(a1*x) has only odd harmonics;
/// small-signal H3 amplitude is (a1^3/3)*A^3/4. Reference form — the shipping
/// oversampled path uses a band-limited LUT of the same curve.
[[nodiscard]] inline float oddShaper(float x, float a1) noexcept
{
    return std::tanh(a1 * x);
}

}  // namespace ta670::dsp
