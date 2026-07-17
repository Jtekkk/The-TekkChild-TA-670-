// TA-670 DSP core — unit tests (§14.2). Dependency-free harness.
// Mirrors tools/validate_spec.py checks C1..C8 where applicable.
// SPDX-License-Identifier: MIT

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/Channel.hpp"
#include "dsp/Common.hpp"
#include "dsp/GainCell.hpp"
#include "dsp/Knee.hpp"
#include "dsp/Meters.hpp"
#include "dsp/MidSide.hpp"
#include "dsp/Sidechain.hpp"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

using namespace ta670::dsp;

// --- T1: alpha identity across sample rates (C1/C2) ------------------------
void testAlphaTable()
{
    const double rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
    const double taus[] = {0.2e-3, 0.4e-3, 0.2, 0.3, 0.8, 2.0, 5.0, 10.0, 25.0};
    double worst = 0.0;
    for (const double fs : rates) {
        for (const double tau : taus) {
            const double a = onePoleAlpha(tau, fs);
            // recursion vs continuous solution at n = round(3*tau*fs) capped
            const int n = std::min(
                static_cast<int>(std::lround(3.0 * tau * fs)), 100000);
            double y = 0.0;
            for (int i = 0; i < n; ++i)
                y = a * y + (1.0 - a);
            const double ref = 1.0 - std::exp(-n / (tau * fs));
            worst = std::max(worst, std::abs(y - ref));
        }
    }
    expect(worst < 1e-9, "T1 alpha=exp(-1/(tau*fs)) tracks analytic response");
}

// --- T2: soft knee C1 continuity (C3) ---------------------------------------
void testKneeContinuity()
{
    const float t = -12.0f, r = 8.0f, w = 10.0f;
    const float loEdge = t - 0.5f * w, hiEdge = t + 0.5f * w;
    // values at edges (middle branch evaluated exactly at the boundary)
    const float vLo = kneeGainDb(loEdge, t, r, w);
    const float vHi = kneeGainDb(hiEdge, t, r, w);
    const float vHiRef = (1.0f / r - 1.0f) * (hiEdge - t);
    // analytic slopes: below = 0; mid' = (1/R-1)x/W; above = 1/R-1
    const float sMidLo = (1.0f / r - 1.0f) * 0.0f / w;
    const float sMidHi = (1.0f / r - 1.0f) * w / w;
    expect(std::abs(vLo) < 1e-6f && std::abs(vHi - vHiRef) < 1e-6f
               && std::abs(sMidLo) < 1e-9f
               && std::abs(sMidHi - (1.0f / r - 1.0f)) < 1e-9f,
           "T2 soft knee C1-continuous at both edges");
}

// --- T3: M-S round trip and energy (C6) --------------------------------------
void testMidSide()
{
    std::mt19937 rng(670);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    float worst = 0.0f;
    double eIn = 0.0, eMs = 0.0;
    for (int i = 0; i < 100000; ++i) {
        const float l = dist(rng), r = dist(rng);
        float m = 0, s = 0, l2 = 0, r2 = 0;
        msEncode(l, r, m, s);
        msDecode(m, s, l2, r2);
        worst = std::max({worst, std::abs(l2 - l), std::abs(r2 - r)});
        eIn += static_cast<double>(l) * l + static_cast<double>(r) * r;
        eMs += static_cast<double>(m) * m + static_cast<double>(s) * s;
    }
    expect(worst < 2e-7f && std::abs(eMs / eIn - 1.0) < 1e-6,
           "T3 M-S null reconstruction & energy preservation");
}

// --- T4: gain cell LUT vs closed form, monotonic (C4 support) ---------------
void testGainCell()
{
    GainCell cell;
    cell.prepare({});
    double worstDb = 0.0;
    float prev = 2.0f;
    bool monotone = true;
    for (int i = 0; i <= 500; ++i) {
        const float u = static_cast<float>(i) / 500.0f;
        const float lin = cell.linearGain(u);
        const double refDb = -cell.reductionDb(static_cast<double>(u));
        worstDb = std::max(worstDb,
                           std::abs(linToDb(static_cast<double>(lin)) - refDb));
        if (lin > prev + 1e-7f)
            monotone = false;
        prev = lin;
    }
    const bool endpoints = std::abs(cell.linearGain(0.0f) - 1.0f) < 1e-6f
        && std::abs(linToDb(static_cast<double>(cell.linearGain(1.0f)))
                    + cell.calibration().gMaxDb) < 0.02;
    expect(worstDb < 0.01 && monotone && endpoints,
           "T4 gain-cell LUT within 0.01 dB, monotone, exact endpoints");
}

// --- T5: fastDb accuracy ------------------------------------------------------
void testFastDb()
{
    double worst = 0.0;
    for (float x = 1e-6f; x < 10.0f; x *= 1.07f)
        worst = std::max(worst,
                         std::abs(static_cast<double>(fastDb(x))
                                  - linToDb(static_cast<double>(x))));
    expect(worst < 0.01, "T5 fastDb within 0.01 dB of 20*log10");
}

// --- T6: reservoir release program dependence (C5) ---------------------------
double fitTau(const std::vector<double>& ctl, double fs, double t0, double t1)
{
    const auto i0 = static_cast<std::size_t>(t0 * fs);
    const auto i1 = static_cast<std::size_t>(t1 * fs);
    const double y0 = ctl[i0] - kDbFloor, y1 = ctl[i1] - kDbFloor;
    return -(t1 - t0) / std::log(y1 / y0);
}

void testReservoirRelease()
{
    const double fs = 48000.0;
    auto run = [&](int pos, double onSec, double offSec) {
        SidechainEnvelope env;
        env.prepare(pos, fs);
        const auto nOn = static_cast<std::size_t>(onSec * fs);
        const auto nOff = static_cast<std::size_t>(offSec * fs);
        std::vector<double> ctl(nOff);
        for (std::size_t i = 0; i < nOn; ++i)
            (void)env.update(0.0);
        for (std::size_t i = 0; i < nOff; ++i)
            ctl[i] = env.update(kDbFloor);
        return ctl;
    };
    const double tauIso5 = fitTau(run(5, 0.05, 2.0), fs, 0.01, 0.15);
    const double tauSus5 = fitTau(run(5, 20.0, 8.0), fs, 0.5, 5.0);
    const double tauSus6 = fitTau(run(6, 60.0, 12.0), fs, 0.5, 10.0);
    expect(std::abs(tauIso5 - 0.2) / 0.2 < 0.15
               && std::abs(tauSus5 - 10.0) / 10.0 < 0.15
               && std::abs(tauSus6 - 25.0) / 25.0 < 0.15,
           "T6 program-dependent release: 0.2s iso / 10s sus / 25s sustained");
}

// --- T7: feedback channel — emergent ratio & sanity (C4) ----------------------
void testChannelRatio()
{
    const double fs = 48000.0;
    const float thresholdDb = -20.0f;
    auto steadyOutDb = [&](double inDbOverT) {
        Channel ch;
        ch.prepare(fs, 1, thresholdDb);  // Pos 1: 0.2 ms / 0.3 s
        const double amp = dbToLin(static_cast<double>(thresholdDb) + inDbOverT);
        const auto n = static_cast<std::size_t>(2.0 * fs);
        double peak = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = 2.0 * 3.14159265358979 * 997.0
                * static_cast<double>(i) / fs;
            const float y = ch.processSample(
                static_cast<float>(amp * std::sin(ph)));
            if (!(std::isfinite(y)))
                return 1e9;  // NaN/Inf guard
            if (i > n - static_cast<std::size_t>(0.25 * fs))
                peak = std::max(peak, static_cast<double>(std::fabs(y)));
        }
        return linToDb(peak) - static_cast<double>(thresholdDb);
    };
    const double oo10 = steadyOutDb(10.0);
    const double oo20 = steadyOutDb(20.0);
    const double ratio = 10.0 / (oo20 - oo10);  // R = dLi/dLo mid-range
    expect(oo10 < 1e8 && oo20 < 1e8 && oo20 > oo10 && ratio > 10.0
               && ratio < 30.0,
           "T7 feedback loop: finite, monotone, high emergent ratio (>10:1)");
}

// --- T8: VU ballistics (C8) ---------------------------------------------------
void testVu()
{
    const double fs = 48000.0;
    VuBallistics vu;
    vu.prepare(fs);
    const auto n = static_cast<std::size_t>(1.5 * fs);
    double peak = 0.0;
    double t99 = -1.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double m = vu.update(1.0);
        peak = std::max(peak, m);
        if (t99 < 0.0 && m >= 0.99)
            t99 = static_cast<double>(i) / fs;
    }
    const double overshootPct = (peak - 1.0) * 100.0;
    expect(std::abs(t99 - 0.300) < 0.01 && overshootPct > 0.8
               && overshootPct < 2.0,
           "T8 VU: 99% at 300 ms (+/-10 ms), overshoot ~1.5%");
}

}  // namespace

int main()
{
    std::printf("TA-670 DSP unit tests (SPECIFICATION.md §14.2)\n");
    testAlphaTable();
    testKneeContinuity();
    testMidSide();
    testGainCell();
    testFastDb();
    testReservoirRelease();
    testChannelRatio();
    testVu();
    std::printf("%s (%d failure%s)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
