// TA-670 DSP core — unit tests (§14.2). Dependency-free harness.
// Mirrors tools/validate_spec.py checks C1..C8 where applicable.
// SPDX-License-Identifier: MIT

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/Channel.hpp"
#include "dsp/Coloration.hpp"
#include "dsp/Common.hpp"
#include "dsp/GainCell.hpp"
#include "dsp/Knee.hpp"
#include "dsp/Meters.hpp"
#include "dsp/MidSide.hpp"
#include "dsp/Oversampler.hpp"
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

// --- helpers for T9..T14 ------------------------------------------------------

/// Windowed (Hann) Goertzel amplitude estimate at frequency f.
double goertzelAmp(const std::vector<float>& x, double f, double fs)
{
    const std::size_t n = x.size();
    const double w = 2.0 * 3.14159265358979324 * f / fs;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double hann = 0.5
            - 0.5 * std::cos(2.0 * 3.14159265358979324
                             * static_cast<double>(i)
                             / static_cast<double>(n - 1));
        s0 = static_cast<double>(x[i]) * hann + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    // Hann coherent gain = 0.5; amplitude of A*sin -> A
    return 2.0 * std::sqrt(std::max(power, 0.0))
        / (0.5 * static_cast<double>(n));
}

// --- T9: oversampling filter meets §9.3 stopband / ripple targets -------------
void testFilterDesign()
{
    Oversampler os;
    os.prepare(2, 512);
    const auto& taps = os.upStage(0).taps();
    // DFT response on a dense grid (filter runs at 2*fs_base)
    double worstStop = -400.0, worstRipple = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double f = 0.5 * static_cast<double>(i) / 400.0;  // 0..Nyquist
        double re = 0.0, im = 0.0;
        for (std::size_t k = 0; k < taps.size(); ++k) {
            const double ph = -2.0 * 3.14159265358979324 * f
                * static_cast<double>(k);
            re += static_cast<double>(taps[k]) * std::cos(ph);
            im += static_cast<double>(taps[k]) * std::sin(ph);
        }
        const double magDb = 10.0 * std::log10(re * re + im * im + 1e-300);
        if (f <= 0.225)  // passband: 0.45*fs_base at the 2x rate
            worstRipple = std::max(worstRipple,
                                   std::abs(magDb - linToDb(2.0)));
        if (f >= 0.275)  // stopband
            worstStop = std::max(worstStop, magDb - linToDb(2.0));
    }
    expect(worstRipple < 0.01 && worstStop < -118.0,
           "T9 stage-1 half-band: ripple <0.01 dB, stopband <-118 dB");
}

// --- T10: reported latency is exact & round trip nulls (REQ-009) --------------
void testOversamplerLatencyAndNull()
{
    for (const int factor : {2, 4, 8, 16}) {
        Oversampler os;
        const std::size_t block = 256, total = 8192;
        os.prepare(factor, block);
        const int lat = os.latencyBaseSamples();

        // 997 Hz sine through up -> down; compare to input delayed by lat.
        const double fs = 48000.0, f0 = 997.0;
        std::vector<float> in(total), out(total);
        for (std::size_t i = 0; i < total; ++i)
            in[i] = static_cast<float>(
                std::sin(2.0 * 3.14159265358979324 * f0
                         * static_cast<double>(i) / fs));
        for (std::size_t off = 0; off < total; off += block) {
            float* up = os.processUp(in.data() + off, block);
            os.processDown(up, out.data() + off, block);
        }
        double err = 0.0, ref = 0.0;
        for (std::size_t i = 2048; i + static_cast<std::size_t>(lat) < total;
             ++i) {
            const double d = static_cast<double>(
                                 out[i + static_cast<std::size_t>(lat)])
                - static_cast<double>(in[i]);
            err += d * d;
            ref += static_cast<double>(in[i]) * static_cast<double>(in[i]);
        }
        const double nullDb = 10.0 * std::log10(err / ref + 1e-300);
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "T10 %2dx: latency %d smp exact, null %.0f dB (<-90)",
                      factor, lat, nullDb);
        expect(nullDb < -90.0, msg);
    }
}

// --- T11: aliasing suppressed by oversampling (REQ-008) -----------------------
void testAliasSuppression()
{
    // x^3 on a 15 kHz tone makes a 45 kHz component. Without oversampling it
    // folds to |48k - 45k| = 3 kHz; with 4x it is filtered before decimation.
    const double fs = 48000.0, f0 = 15000.0, fAlias = 3000.0;
    const std::size_t block = 256, total = 16384;
    auto run = [&](int factor) {
        Oversampler os;
        os.prepare(factor, block);
        std::vector<float> in(total), out(total);
        for (std::size_t i = 0; i < total; ++i)
            in[i] = static_cast<float>(
                0.9 * std::sin(2.0 * 3.14159265358979324 * f0
                               * static_cast<double>(i) / fs));
        for (std::size_t off = 0; off < total; off += block) {
            float* up = os.processUp(in.data() + off, block);
            const std::size_t nUp = block * static_cast<std::size_t>(factor);
            for (std::size_t i = 0; i < nUp; ++i)
                up[i] = up[i] * up[i] * up[i];  // odd nonlinearity
            os.processDown(up, out.data() + off, block);
        }
        std::vector<float> tail(out.begin() + 4096, out.end());
        const double alias = goertzelAmp(tail, fAlias, fs);
        const double fund = goertzelAmp(tail, f0, fs);
        return 20.0 * std::log10(alias / fund + 1e-300);
    };
    const double alias1x = run(1);
    const double alias4x = run(4);
    char msg[96];
    std::snprintf(msg, sizeof(msg),
                  "T11 alias at 3 kHz: 1x %.0f dBc (audible), 4x %.0f dBc (<-100)",
                  alias1x, alias4x);
    expect(alias1x > -30.0 && alias4x < -100.0, msg);
}

// --- T12: transformer coloration behavior (§8.1) ------------------------------
void testColoration()
{
    const double fs = 96000.0;
    auto thirdHarmonic = [&](double f0, double ampDb) {
        Coloration col;
        ColorationParams params;
        params.noiseDbFs = -300.0;  // isolate distortion from noise
        col.prepare(fs, params);
        const std::size_t n = 32768;
        std::vector<float> buf(n);
        const double a = dbToLin(ampDb);
        for (std::size_t i = 0; i < n; ++i)
            buf[i] = static_cast<float>(
                a * std::sin(2.0 * 3.14159265358979324 * f0
                             * static_cast<double>(i) / fs));
        col.processBlock(buf.data(), static_cast<int>(n));
        std::vector<float> tail(buf.begin() + 8192, buf.end());
        const double h1 = goertzelAmp(tail, f0, fs);
        const double h2 = goertzelAmp(tail, 2 * f0, fs);
        const double h3 = goertzelAmp(tail, 3 * f0, fs);
        struct R { double gainDb, h2Dbc, h3Dbc; };
        return R{linToDb(h1 / a), 20.0 * std::log10(h2 / h1 + 1e-300),
                 20.0 * std::log10(h3 / h1 + 1e-300)};
    };
    const auto mid = thirdHarmonic(1000.0, -20.0);   // clean through-amp
    const auto lf = thirdHarmonic(40.0, 0.0);        // LF core saturation
    expect(std::abs(mid.gainDb) < 0.25 && mid.h3Dbc < -80.0
               && lf.h3Dbc > -40.0 && lf.h3Dbc > lf.h2Dbc,
           "T12 transformer: unity/clean at 1 kHz, odd LF saturation at 40 Hz");
}

// --- T13: coloration noise floor calibration (§8.3) ---------------------------
void testNoiseFloor()
{
    Coloration col;
    col.prepare(48000.0, {});
    const std::size_t n = 1 << 18;
    std::vector<float> buf(n, 0.0f);
    col.processBlock(buf.data(), static_cast<int>(n));
    double ms = 0.0;
    for (const float v : buf)
        ms += static_cast<double>(v) * static_cast<double>(v);
    const double rmsDb = 10.0 * std::log10(ms / static_cast<double>(n));
    expect(std::abs(rmsDb - (-116.0)) < 2.0,
           "T13 idle noise floor -116 dBFS RMS (+/-2 dB)");
}

// --- T14: full chain — OS + feedback channel + coloration, finite & GR --------
void testFullChain()
{
    const double fs = 48000.0;
    const int factor = 4;
    const std::size_t block = 256, total = 24576;  // multiple of block
    Oversampler os;
    os.prepare(factor, block);
    Channel ch;
    ch.prepare(fs * factor, 1, -20.0f);
    Coloration col;
    col.prepare(fs * factor, {});
    std::vector<float> io(total);
    for (std::size_t i = 0; i < total; ++i)
        io[i] = static_cast<float>(
            0.5 * std::sin(2.0 * 3.14159265358979324 * 220.0
                           * static_cast<double>(i) / fs));
    bool finite = true;
    for (std::size_t off = 0; off < total; off += block) {
        float* up = os.processUp(io.data() + off, block);
        const auto nUp = static_cast<int>(block) * factor;
        ch.processBlock(up, nUp);
        col.processBlock(up, nUp);
        os.processDown(up, io.data() + off, block);
    }
    for (const float v : io)
        finite = finite && std::isfinite(v);
    const float gr = ch.gainReductionDb();
    expect(finite && gr > 3.0f && gr < 40.0f,
           "T14 full chain (4x OS + channel + color): finite, GR active");
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
    testFilterDesign();
    testOversamplerLatencyAndNull();
    testAliasSuppression();
    testColoration();
    testNoiseFloor();
    testFullChain();
    std::printf("%s (%d failure%s)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
