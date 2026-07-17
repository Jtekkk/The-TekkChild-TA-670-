// TA-670 DSP core — metering ballistics (§11).
// SPDX-License-Identifier: MIT
//
// GR meter: asymmetric one-pole (fast attack / slow release)        (§11.2)
// VU meter: 2nd-order underdamped system per IEC 60268-17, zeta = 0.80,
//           wn ~ 13.1 rad/s (first 99% crossing at 300 ms), discretized with
//           the TPT (trapezoidal / Zavalishin SVF) method             (§11.3)
#pragma once

#include <cmath>

#include "Common.hpp"

namespace ta670::dsp {

class GrMeter {
public:
    void prepare(double fs, double attackTau = 5e-3,
                 double releaseTau = 250e-3) noexcept
    {
        alphaAttack_ = onePoleAlpha(attackTau, fs);
        alphaRelease_ = onePoleAlpha(releaseTau, fs);
        value_ = 0.0;
    }

    void reset() noexcept { value_ = 0.0; }

    /// grDb >= 0 (gain-reduction magnitude).
    double update(double grDb) noexcept
    {
        const double a = (grDb > value_) ? alphaAttack_ : alphaRelease_;
        value_ = a * value_ + (1.0 - a) * grDb;
        return value_;
    }

    [[nodiscard]] double value() const noexcept { return value_; }

private:
    double alphaAttack_ = 0.0;
    double alphaRelease_ = 0.0;
    double value_ = 0.0;
};

/// IEC-ballistics VU pointer: m'' + 2*zeta*wn*m' + wn^2 m = wn^2 x, as a
/// TPT state-variable filter whose lowpass output is the pointer position.
class VuBallistics {
public:
    static constexpr double kZeta = 0.80;   ///< 1.52% overshoot (§11.3)
    static constexpr double kOmegaN = 13.1; ///< rad/s, 99% at 300 ms (§11.3)

    void prepare(double fs) noexcept
    {
        const double g = std::tan(kOmegaN / (2.0 * fs));
        const double k = 2.0 * kZeta;
        a1_ = 1.0 / (1.0 + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
        reset();
    }

    void reset() noexcept { ic1_ = ic2_ = 0.0; }

    /// x: mean-square (or rectified) program level; returns pointer position.
    double update(double x) noexcept
    {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        return v2;  // lowpass output = pointer
    }

private:
    double a1_ = 0.0, a2_ = 0.0, a3_ = 0.0;
    double ic1_ = 0.0, ic2_ = 0.0;
};

}  // namespace ta670::dsp
