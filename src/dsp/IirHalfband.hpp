// TA-670 DSP core — minimum-phase polyphase-IIR halfband 2x resampler (§9.3).
// SPDX-License-Identifier: MIT
//
// Eco mode's zero-reported-latency oversampling path. The halfband is the
// classic two-branch polyphase allpass decomposition
//
//     H(z) = 1/2 * [ A0(z^2) + z^-1 * A1(z^2) ]
//
// where A0, A1 are cascades of first-order allpass sections in z^2
// (first-order in z when run on the decimated streams). The coefficients are
// designed analytically as an elliptic (equiripple-stopband) halfband via the
// Jacobi elliptic nome (Ansari 1985; Valenzuela & Constantinides 1984):
//
//   selectivity  k  = tan^2((1 - 4t) * pi/4),  t = 0.25 - fpass
//   nome         q  from k via Landen-style series
//   order        N  = ceil( ln(delta_s^2 / 16) / ln(q) ), rounded up to odd
//   coefficients a_i from theta-function (q-series) quotients
//
// Design happens in prepare() (double precision, allocation allowed); the
// audio path is a handful of multiplies per sample, allocation-free (§12.3).
// Being minimum-phase-like, latency is reported as 0 samples (REQ-011); the
// LF group delay is a few samples and frequency-dependent (§9.4).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ta670::dsp::iir {

inline constexpr double kPi = 3.14159265358979324;

/// Elliptic halfband allpass coefficients, ascending. fpass is the passband
/// edge as a fraction of the filter's (high) rate, e.g. 0.225; attenDb the
/// stopband attenuation target in dB.
[[nodiscard]] inline std::vector<double> designHalfband(double fpass,
                                                        double attenDb)
{
    const double t = 0.25 - fpass;             // transition half-width
    double k = std::tan((1.0 - 4.0 * t) * kPi / 4.0);
    k *= k;

    // elliptic nome q from modulus k
    const double kp = std::sqrt(1.0 - k * k);
    const double e = 0.5 * (1.0 - std::sqrt(kp)) / (1.0 + std::sqrt(kp));
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

    // Order from stopband ripple. For the POWER-COMPLEMENTARY halfband the
    // elliptic ripple parameter is k1 = delta_s^2 (passband and stopband
    // ripples are coupled), and the degree equation gives k1 = 4*q^(N/2),
    // hence delta_s = 2*q^(N/4). Verified numerically: N=9 at q=0.0473
    // predicts -53.6 dB, matching the measured equiripple stopband exactly.
    const double deltaS2 = std::pow(10.0, -attenDb / 10.0);
    int order = static_cast<int>(std::ceil(2.0 * std::log(deltaS2 / 4.0)
                                           / std::log(q)));
    if (order % 2 == 0)
        ++order;

    const int numCoefs = (order - 1) / 2;
    std::vector<double> coefs(static_cast<std::size_t>(numCoefs));
    for (int i = 1; i <= numCoefs; ++i) {
        const double angle = kPi * static_cast<double>(i)
            / static_cast<double>(order);
        double num = 0.0, den = 0.5;
        for (int m = 0; m < 16; ++m) {
            const double sign = (m % 2 == 0) ? 1.0 : -1.0;
            num += sign * std::pow(q, static_cast<double>(m * (m + 1)))
                * std::sin(static_cast<double>(2 * m + 1) * angle);
            if (m >= 1)
                den += sign * std::pow(q, static_cast<double>(m * m))
                    * std::cos(static_cast<double>(2 * m) * angle);
        }
        const double w = 2.0 * std::pow(q, 0.25) * (num / (2.0 * den));
        const double w2 = w * w;
        const double wp = std::sqrt((1.0 - w2 * k) * (1.0 - w2 / k))
            / (1.0 + w2);
        coefs[static_cast<std::size_t>(i - 1)] = (1.0 - wp) / (1.0 + wp);
    }
    std::sort(coefs.begin(), coefs.end());
    return coefs;
}

/// Complex response of one allpass cascade at z = e^{j*theta}:
/// A(z) = prod (a + z^-1) / (1 + a z^-1). For analysis/tests.
inline void allpassResponse(const std::vector<double>& coefs, double theta,
                            double& re, double& im)
{
    re = 1.0;
    im = 0.0;
    const double zr = std::cos(-theta), zi = std::sin(-theta);
    for (const double a : coefs) {
        const double nr = a + zr, ni = zi;
        const double dr = 1.0 + a * zr, di = a * zi;
        const double dd = dr * dr + di * di;
        const double hr = (nr * dr + ni * di) / dd;
        const double hi = (ni * dr - nr * di) / dd;
        const double tr = re * hr - im * hi;
        im = re * hi + im * hr;
        re = tr;
    }
}

/// Composite halfband magnitude |H(e^{j*2*pi*f})| with branch split
/// (even-indexed coefs -> branch 0, odd-indexed -> branch 1 with z^-1).
[[nodiscard]] inline double halfbandMagnitude(const std::vector<double>& coefs,
                                              double f)
{
    std::vector<double> b0, b1;
    for (std::size_t i = 0; i < coefs.size(); ++i)
        ((i % 2 == 0) ? b0 : b1).push_back(coefs[i]);
    const double theta = 2.0 * kPi * f;
    double r0, i0, r1, i1;
    allpassResponse(b0, 2.0 * theta, r0, i0);   // A0(z^2)
    allpassResponse(b1, 2.0 * theta, r1, i1);   // A1(z^2)
    const double dr = std::cos(-theta), di = std::sin(-theta);  // z^-1
    const double re = 0.5 * (r0 + (r1 * dr - i1 * di));
    const double im = 0.5 * (i0 + (r1 * di + i1 * dr));
    return std::sqrt(re * re + im * im);
}

/// One first-order allpass section: y[n] = a*(x[n] - y[n-1]) + x[n-1].
struct AllpassSection {
    double a = 0.0;
    double x1 = 0.0, y1 = 0.0;

    [[nodiscard]] double process(double x) noexcept
    {
        const double y = a * (x - y1) + x1;
        x1 = x;
        y1 = y;
        return y;
    }

    void reset() noexcept { x1 = y1 = 0.0; }
};

class AllpassChain {
public:
    void prepare(const std::vector<double>& coefs)
    {
        sections_.clear();
        for (const double a : coefs)
            sections_.push_back({a, 0.0, 0.0});
    }

    void reset() noexcept
    {
        for (auto& s : sections_)
            s.reset();
    }

    [[nodiscard]] double process(double x) noexcept
    {
        for (auto& s : sections_)
            x = s.process(x);
        return x;
    }

private:
    std::vector<AllpassSection> sections_;
};

// ---------------------------------------------------------------------------
// 2x resampler pair (Eco mode). Up: y[2n] = A0(x[n]), y[2n+1] = A1(x[n]).
// Down: y[n] = 1/2 (A0(x[2n]) + A1(x[2n-1])).
// ---------------------------------------------------------------------------
class IirOversampler2x {
public:
    static constexpr double kPassEdge = 0.225;  // 0.45*fs_base at the 2x rate
    static constexpr double kAttenDb = 100.0;

    void prepare()
    {
        const auto coefs = designHalfband(kPassEdge, kAttenDb);
        std::vector<double> b0, b1;
        for (std::size_t i = 0; i < coefs.size(); ++i)
            ((i % 2 == 0) ? b0 : b1).push_back(coefs[i]);
        up0_.prepare(b0);
        up1_.prepare(b1);
        down0_.prepare(b0);
        down1_.prepare(b1);
        reset();
    }

    void reset() noexcept
    {
        up0_.reset();
        up1_.reset();
        down0_.reset();
        down1_.reset();
        prevOdd_ = 0.0;
    }

    void processUp(const float* in, float* out, std::size_t nIn) noexcept
    {
        for (std::size_t i = 0; i < nIn; ++i) {
            const double x = static_cast<double>(in[i]);
            out[2 * i] = static_cast<float>(up0_.process(x));
            out[2 * i + 1] = static_cast<float>(up1_.process(x));
        }
    }

    void processDown(const float* in, float* out, std::size_t nOut) noexcept
    {
        for (std::size_t i = 0; i < nOut; ++i) {
            const double even = static_cast<double>(in[2 * i]);
            const double odd = static_cast<double>(in[2 * i + 1]);
            out[i] = static_cast<float>(
                0.5 * (down0_.process(even) + down1_.process(prevOdd_)));
            prevOdd_ = odd;
        }
    }

private:
    AllpassChain up0_, up1_, down0_, down1_;
    double prevOdd_ = 0.0;
};

}  // namespace ta670::dsp::iir
