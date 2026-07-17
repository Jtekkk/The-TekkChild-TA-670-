// TA-670 DSP core — oversampling & anti-aliasing (§9).
// SPDX-License-Identifier: MIT
//
// Linear-phase Kaiser-windowed FIR cascade. Stage 1 (fs -> 2fs) is a true
// half-band design (cutoff at exactly 1/4 of the filter rate, so every other
// tap is zero); later stages have generous transition bands and are short.
//
// Latency (§9.4): an up+down stage pair at rate 2^k*fs contributes
// (N_k - 1) / 2^k base-rate samples. Stage lengths are chosen with
// N_k = 1 (mod 2^k) so the total round-trip latency is an EXACT INTEGER of
// base-rate samples — enabling sample-exact host reporting and null tests:
//
//   stage 1 (->  2x): N = 159 (half-band, A = 120 dB, df = 0.05)  -> 79
//   stage 2 (->  4x): N =  33                                     ->  8
//   stage 3 (->  8x): N =  25                                     ->  3
//   stage 4 (-> 16x): N =  33                                     ->  2
//
//   round-trip latency: 2x = 79, 4x = 87, 8x = 90, 16x = 92 base samples.
//
// prepare() allocates; process paths are allocation-free (§12.3). The straight
// ring-buffer convolution here is the clarity-first reference; production
// swaps in the polyphase form (identical output, fewer MACs).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ta670::dsp {

// ---------------------------------------------------------------------------
// Kaiser-window FIR design (prepare()-time only).
// ---------------------------------------------------------------------------
namespace fir {

/// Modified Bessel function of the first kind, order zero (series).
[[nodiscard]] inline double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-14 * sum)
            break;
    }
    return sum;
}

/// Kaiser beta for a target stopband attenuation A (dB), A > 50.
[[nodiscard]] inline double kaiserBeta(double attenDb) noexcept
{
    return 0.1102 * (attenDb - 8.7);
}

/// Linear-phase lowpass: N taps, cutoff fc (fraction of the sample rate,
/// midpoint of the transition band), Kaiser window with parameter beta.
/// The result is normalized to unity DC gain, then scaled by `gain`
/// (interpolators need gain 2 to compensate zero-stuffing).
[[nodiscard]] inline std::vector<float> designLowpass(std::size_t numTaps,
                                                      double fc, double beta,
                                                      double gain)
{
    std::vector<double> h(numTaps);
    const double mid = 0.5 * static_cast<double>(numTaps - 1);
    const double i0b = besselI0(beta);
    double dc = 0.0;
    for (std::size_t n = 0; n < numTaps; ++n) {
        const double t = static_cast<double>(n) - mid;
        const double sinc = (t == 0.0)
            ? 2.0 * fc
            : std::sin(2.0 * 3.14159265358979324 * fc * t)
                / (3.14159265358979324 * t);
        const double r = 2.0 * t / static_cast<double>(numTaps - 1);
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r)))
            / i0b;
        h[n] = sinc * w;
        dc += h[n];
    }
    std::vector<float> out(numTaps);
    for (std::size_t n = 0; n < numTaps; ++n)
        out[n] = static_cast<float>(h[n] / dc * gain);
    return out;
}

}  // namespace fir

// ---------------------------------------------------------------------------
// One FIR resampling stage (ring-buffer convolution).
// ---------------------------------------------------------------------------
class FirStage {
public:
    void prepare(std::vector<float> taps, std::size_t maxFramesAtHighRate)
    {
        taps_ = std::move(taps);
        state_.assign(taps_.size(), 0.0f);
        work_.assign(maxFramesAtHighRate, 0.0f);
        pos_ = 0;
    }

    void reset() noexcept
    {
        std::fill(state_.begin(), state_.end(), 0.0f);
        pos_ = 0;
    }

    /// Upsample 2x: nIn low-rate frames in -> 2*nIn high-rate frames out.
    void processUp(const float* in, float* out, std::size_t nIn) noexcept
    {
        // zero-stuff into the work buffer, then filter at the high rate
        for (std::size_t i = 0; i < nIn; ++i) {
            work_[2 * i] = in[i];
            work_[2 * i + 1] = 0.0f;
        }
        filter(work_.data(), out, 2 * nIn);
    }

    /// Downsample 2x: 2*nOut high-rate frames in -> nOut low-rate frames out.
    void processDown(const float* in, float* out, std::size_t nOut) noexcept
    {
        filter(in, work_.data(), 2 * nOut);
        for (std::size_t i = 0; i < nOut; ++i)
            out[i] = work_[2 * i];
    }

    [[nodiscard]] std::size_t numTaps() const noexcept { return taps_.size(); }
    [[nodiscard]] const std::vector<float>& taps() const noexcept
    {
        return taps_;
    }

private:
    void filter(const float* in, float* out, std::size_t n) noexcept
    {
        const std::size_t len = taps_.size();
        for (std::size_t i = 0; i < n; ++i) {
            state_[pos_] = in[i];
            float acc = 0.0f;
            std::size_t idx = pos_;
            for (std::size_t k = 0; k < len; ++k) {
                acc += taps_[k] * state_[idx];
                idx = (idx == 0) ? len - 1 : idx - 1;
            }
            out[i] = acc;
            pos_ = (pos_ + 1 == len) ? 0 : pos_ + 1;
        }
    }

    std::vector<float> taps_;
    std::vector<float> state_;
    std::vector<float> work_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Oversampler: cascade of 2x stages, factors 1/2/4/8/16 (§9.2).
// ---------------------------------------------------------------------------
class Oversampler {
public:
    static constexpr double kStopbandDb = 120.0;  // Ultra-grade for all modes

    /// Per-stage lengths (see header comment). Index = stage number - 1.
    static constexpr std::array<std::size_t, 4> kStageTaps{159, 33, 25, 33};

    void prepare(int factor, std::size_t maxBaseFrames)
    {
        factor_ = factor;
        numStages_ = 0;
        for (int f = factor; f > 1; f /= 2)
            ++numStages_;

        const double beta = fir::kaiserBeta(kStopbandDb);
        std::size_t framesAtRate = maxBaseFrames;
        for (std::size_t s = 0; s < numStages_; ++s) {
            framesAtRate *= 2;
            // Passband always protects 0.45*fs_base; the transition midpoint
            // for the stage running at 2^(s+1)*fs_base:
            const double rate = std::exp2(static_cast<double>(s + 1));
            const double passEdge = 0.45 / rate;
            const double stopEdge = (s == 0)
                ? 0.55 / rate                       // half-band symmetric
                : (rate / 2.0 - 0.45) / rate;       // image-band edge
            const double fc = 0.5 * (passEdge + stopEdge);
            auto tapsUp = fir::designLowpass(kStageTaps[s], fc, beta, 2.0);
            auto tapsDn = fir::designLowpass(kStageTaps[s], fc, beta, 1.0);
            up_[s].prepare(std::move(tapsUp), framesAtRate);
            down_[s].prepare(std::move(tapsDn), framesAtRate);
        }
        bufA_.assign(maxBaseFrames * static_cast<std::size_t>(factor), 0.0f);
        bufB_.assign(maxBaseFrames * static_cast<std::size_t>(factor), 0.0f);
    }

    void reset() noexcept
    {
        for (std::size_t s = 0; s < numStages_; ++s) {
            up_[s].reset();
            down_[s].reset();
        }
    }

    /// Round-trip (up + down) latency in base-rate samples — exact integer
    /// by construction (§9.4).
    [[nodiscard]] int latencyBaseSamples() const noexcept
    {
        std::size_t num = 0;
        for (std::size_t s = 0; s < numStages_; ++s)
            num += (kStageTaps[s] - 1) << (numStages_ - 1 - s);
        return static_cast<int>(num >> numStages_);
    }

    [[nodiscard]] int factor() const noexcept { return factor_; }

    /// Upsample a base-rate block; returns pointer to factor*n frames.
    [[nodiscard]] float* processUp(const float* in, std::size_t n) noexcept
    {
        if (numStages_ == 0) {
            for (std::size_t i = 0; i < n; ++i)
                bufA_[i] = in[i];
            return bufA_.data();
        }
        const float* src = in;
        float* dst = bufA_.data();
        std::size_t frames = n;
        for (std::size_t s = 0; s < numStages_; ++s) {
            up_[s].processUp(src, dst, frames);
            frames *= 2;
            src = dst;
            dst = (dst == bufA_.data()) ? bufB_.data() : bufA_.data();
        }
        return const_cast<float*>(src);
    }

    /// Downsample factor*n oversampled frames back into out (n frames).
    void processDown(const float* in, float* out, std::size_t n) noexcept
    {
        if (numStages_ == 0) {
            for (std::size_t i = 0; i < n; ++i)
                out[i] = in[i];
            return;
        }
        std::size_t frames = n << numStages_;
        const float* src = in;
        float* dst = bufA_.data();
        for (std::size_t s = numStages_; s-- > 0;) {
            frames /= 2;
            float* target = (s == 0) ? out : dst;
            down_[s].processDown(src, target, frames);
            src = target;
            dst = (dst == bufA_.data()) ? bufB_.data() : bufA_.data();
        }
    }

    [[nodiscard]] const FirStage& upStage(std::size_t s) const noexcept
    {
        return up_[s];
    }

private:
    int factor_ = 1;
    std::size_t numStages_ = 0;
    std::array<FirStage, 4> up_{};
    std::array<FirStage, 4> down_{};
    std::vector<float> bufA_, bufB_;
};

}  // namespace ta670::dsp
