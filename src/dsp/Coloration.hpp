// TA-670 DSP core — analog coloration: transformer, tube residual, noise (§8).
// SPDX-License-Identifier: MIT
//
// Transformer (§8.1): a leaky flux integrator drives a tanh B-H curve; the
// output is the discrete derivative of the flux density. Small-signal this
// collapses to a first-order high-pass at the leak corner (the finite
// primary-inductance LF rolloff), and saturation engages when |flux|
// approaches phi0 — which happens first at low frequency and high level,
// exactly the "LF thickening / 3rd-harmonic bloom" of iron cores. An HF
// one-pole models leakage-inductance rolloff.
//
// Tube residual (§8.2): odd tanh shaper normalized to unity small-signal gain.
// Noise (§8.3): white hiss at a calibrated RMS via xorshift32 (RT-safe).
//
// Double-precision state (§13.1); no allocation after prepare() (§12.3).
// std::tanh appears per-sample here as the *reference* form — the shipping
// oversampled path replaces it with the band-limited LUT of §4.4.
#pragma once

#include <cmath>
#include <cstdint>

#include "Common.hpp"

namespace ta670::dsp {

struct ColorationParams {
    bool enabled = true;
    /// Flux scale: saturation onset ~0 dBFS at 50 Hz -> phi0 = 1/(2*pi*50).
    double phi0 = 1.0 / (2.0 * 3.14159265358979324 * 50.0);
    double lfCornerHz = 10.0;    ///< primary-inductance high-pass (§8.1)
    double hfCornerHz = 35000.0; ///< leakage-inductance low-pass (§8.1)
    double tubeDrive = 0.15;     ///< odd-shaper drive; THD ~ drive^2/12 (§8.2)
    double noiseDbFs = -116.0;   ///< hiss RMS; ~-110 dBFS(A) integrated (§8.3)
};

class Coloration {
public:
    void prepare(double fs, const ColorationParams& params) noexcept
    {
        p_ = params;
        ts_ = 1.0 / fs;
        leak_ = std::exp(-2.0 * 3.14159265358979324 * p_.lfCornerHz * ts_);
        const double fh = std::min(p_.hfCornerHz, 0.45 * fs);
        hfAlpha_ = std::exp(-2.0 * 3.14159265358979324 * fh * ts_);
        // uniform[-1,1] has RMS 1/sqrt(3); scale to the target dBFS RMS
        noiseGain_ = dbToLin(p_.noiseDbFs) * 1.7320508075688772;
        invTubeDrive_ = 1.0 / p_.tubeDrive;
        reset();
    }

    void reset() noexcept
    {
        flux_ = 0.0;
        bPrev_ = 0.0;
        hfState_ = 0.0;
        rng_ = 0x670670u;
    }

    void processBlock(float* buf, int numFrames) noexcept
    {
        if (!p_.enabled)
            return;
        for (int n = 0; n < numFrames; ++n)
            buf[n] = processSample(buf[n]);
    }

    [[nodiscard]] float processSample(float x) noexcept
    {
        // --- transformer core (§8.1) ---------------------------------------
        flux_ = leak_ * flux_ + ts_ * static_cast<double>(x);
        const double b = std::tanh(flux_ / p_.phi0);
        double y = p_.phi0 * (b - bPrev_) / ts_;
        bPrev_ = b;

        // --- tube residual, unity small-signal gain (§8.2) ------------------
        y = std::tanh(p_.tubeDrive * y) * invTubeDrive_;

        // --- HF rolloff (leakage inductance) --------------------------------
        hfState_ = hfAlpha_ * hfState_ + (1.0 - hfAlpha_) * y;
        y = hfState_;

        // --- noise floor (§8.3) ---------------------------------------------
        rng_ ^= rng_ << 13u;
        rng_ ^= rng_ >> 17u;
        rng_ ^= rng_ << 5u;
        const double white =
            (static_cast<double>(rng_) * (2.0 / 4294967295.0)) - 1.0;
        return static_cast<float>(y + noiseGain_ * white);
    }

    [[nodiscard]] const ColorationParams& params() const noexcept
    {
        return p_;
    }

private:
    ColorationParams p_{};
    double ts_ = 1.0 / 48000.0;
    double leak_ = 0.0;
    double hfAlpha_ = 0.0;
    double noiseGain_ = 0.0;
    double invTubeDrive_ = 1.0;
    double flux_ = 0.0;
    double bPrev_ = 0.0;
    double hfState_ = 0.0;
    std::uint32_t rng_ = 0x670670u;
};

}  // namespace ta670::dsp
