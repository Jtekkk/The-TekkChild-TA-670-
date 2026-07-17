// TA-670 DSP core — the full stereo processing engine (§3.1).
// SPDX-License-Identifier: MIT
//
// Pipeline per §3.1:
//   input trim -> [M-S encode] -> oversample -> feedback vari-mu loop with
//   sidechain LINK (§7.3) -> coloration (§8) -> downsample -> [M-S decode]
//   -> makeup -> delay-compensated dry/wet mix -> meters
//
// OS modes (§9.2): Eco = 2x polyphase-IIR (0 reported latency), Standard /
// High / Ultra = 4x / 8x / 16x linear-phase FIR (exact integer latency).
// prepare() allocates; processBlock() is allocation-free (§12.3).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "Coloration.hpp"
#include "Common.hpp"
#include "GainCell.hpp"
#include "IirHalfband.hpp"
#include "Meters.hpp"
#include "MidSide.hpp"
#include "Oversampler.hpp"
#include "Sidechain.hpp"

namespace ta670::dsp {

enum class ChannelMode { DualMono, Stereo, MidSide };
enum class OsMode { Eco, Standard, High, Ultra };

struct EngineConfig {
    ChannelMode mode = ChannelMode::Stereo;
    OsMode osMode = OsMode::Standard;
    float linkAmount = 1.0f;   ///< 0 = independent, 1 = fully linked (§7.3)
    float mix = 1.0f;          ///< dry/wet, 1 = fully wet
    bool colorEnabled = true;
    std::array<int, 2> position{4, 4};          ///< time-constant switch
    std::array<float, 2> inputDb{0.f, 0.f};
    std::array<float, 2> thresholdDb{-20.f, -20.f};
    std::array<float, 2> makeupDb{0.f, 0.f};
    CellCalibration cell{};
};

class Engine {
public:
    void prepare(double fs, std::size_t maxBlock, const EngineConfig& cfg)
    {
        cfg_ = cfg;
        fs_ = fs;
        maxBlock_ = maxBlock;
        useIir_ = (cfg.osMode == OsMode::Eco);
        const int firFactor = (cfg.osMode == OsMode::Standard) ? 4
            : (cfg.osMode == OsMode::High) ? 8
            : (cfg.osMode == OsMode::Ultra) ? 16
            : 2;
        osFactor_ = useIir_ ? 2 : firFactor;
        const double fsUp = fs * static_cast<double>(osFactor_);

        for (std::size_t c = 0; c < 2; ++c) {
            if (useIir_)
                iirOs_[c].prepare();
            else
                firOs_[c].prepare(osFactor_, maxBlock);
            cell_[c].prepare(cfg.cell);
            env_[c].prepare(cfg.position[c], fsUp);
            ColorationParams cp;
            cp.enabled = cfg.colorEnabled;
            color_[c].prepare(fsUp, cp);
            grMeter_[c].prepare(fs);
            inGain_[c] = static_cast<float>(
                dbToLin(static_cast<double>(cfg.inputDb[c])));
            makeupGain_[c] = static_cast<float>(
                dbToLin(static_cast<double>(cfg.makeupDb[c])));
            upBuf_[c].assign(maxBlock * static_cast<std::size_t>(osFactor_),
                             0.0f);
        }
        latency_ = useIir_ ? 0 : firOs_[0].latencyBaseSamples();
        for (std::size_t c = 0; c < 2; ++c) {
            dry_[c].assign(std::max<std::size_t>(
                               static_cast<std::size_t>(latency_), 1),
                           0.0f);
            dryPos_[c] = 0;
            dryScratch_[c].assign(maxBlock, 0.0f);
        }
        invControlRange_ =
            1.0f / static_cast<float>(cfg.cell.controlRangeDb);
        reset();
    }

    void reset() noexcept
    {
        for (std::size_t c = 0; c < 2; ++c) {
            if (useIir_)
                iirOs_[c].reset();
            else
                firOs_[c].reset();
            cell_[c].reset();
            env_[c].reset();
            color_[c].reset();
            grMeter_[c].reset();
            std::fill(dry_[c].begin(), dry_[c].end(), 0.0f);
            dryPos_[c] = 0;
        }
    }

    [[nodiscard]] int latencySamples() const noexcept { return latency_; }

    // ---- live (RT-safe) parameter setters. Heavy topology changes — OS
    // mode, cell recalibration — require prepare(); the plugin adapter
    // requests a host restart for those (§10.4, §12.3).
    void setInputDb(std::size_t ch, float db) noexcept
    {
        cfg_.inputDb[ch] = db;
        inGain_[ch] = static_cast<float>(dbToLin(static_cast<double>(db)));
    }

    void setMakeupDb(std::size_t ch, float db) noexcept
    {
        cfg_.makeupDb[ch] = db;
        makeupGain_[ch] = static_cast<float>(dbToLin(static_cast<double>(db)));
    }

    void setThresholdDb(std::size_t ch, float db) noexcept
    {
        cfg_.thresholdDb[ch] = db;
    }

    void setLink(float amount) noexcept { cfg_.linkAmount = amount; }
    void setMix(float mix) noexcept { cfg_.mix = mix; }
    void setMode(ChannelMode m) noexcept { cfg_.mode = m; }

    void setColorEnabled(bool enabled) noexcept
    {
        cfg_.colorEnabled = enabled;
        color_[0].setEnabled(enabled);
        color_[1].setEnabled(enabled);
    }

    /// Time-constant switch: coefficient recompute only (no allocation).
    void setPosition(std::size_t ch, int position) noexcept
    {
        cfg_.position[ch] = position;
        env_[ch].prepare(position,
                         fs_ * static_cast<double>(osFactor_));
    }

    [[nodiscard]] float gainReductionDb(std::size_t ch) const noexcept
    {
        return -fastDb(cell_[ch].currentGain() + kTinyLin);
    }

    void processBlock(float* left, float* right, std::size_t n) noexcept
    {
        // ---- input trim + optional M-S encode + dry capture (base rate).
        // The delay line advances sample-by-sample; dryScratch_ holds the
        // latency-compensated dry signal for the mix stage below.
        for (std::size_t i = 0; i < n; ++i) {
            float a = left[i] * inGain_[0];
            float b = right[i] * inGain_[1];
            if (cfg_.mode == ChannelMode::MidSide)
                msEncode(a, b, a, b);
            left[i] = a;
            right[i] = b;
            dryScratch_[0][i] = delayDry(0, a);
            dryScratch_[1][i] = delayDry(1, b);
        }

        // ---- upsample both channels ----------------------------------------
        float* upA;
        float* upB;
        const std::size_t nUp = n * static_cast<std::size_t>(osFactor_);
        if (useIir_) {
            iirOs_[0].processUp(left, upBuf_[0].data(), n);
            iirOs_[1].processUp(right, upBuf_[1].data(), n);
            upA = upBuf_[0].data();
            upB = upBuf_[1].data();
        } else {
            // FIR oversampler returns its internal buffer; copy so both
            // channels' data stay live simultaneously.
            const float* pa = firOs_[0].processUp(left, n);
            std::copy(pa, pa + nUp, upBuf_[0].begin());
            const float* pb = firOs_[1].processUp(right, n);
            std::copy(pb, pb + nUp, upBuf_[1].begin());
            upA = upBuf_[0].data();
            upB = upBuf_[1].data();
        }

        // ---- the linked feedback vari-mu loop (§3.3, §7.3).
        // Dual-mono means fully independent channels (§7.1).
        const float link = (cfg_.mode == ChannelMode::DualMono)
            ? 0.0f
            : cfg_.linkAmount;
        for (std::size_t i = 0; i < nUp; ++i) {
            const float outA = cell_[0].currentGain() * upA[i];
            const float outB = cell_[1].currentGain() * upB[i];
            const double lvlA = static_cast<double>(
                fastDb(std::fabs(outA) + kTinyLin));
            const double lvlB = static_cast<double>(
                fastDb(std::fabs(outB) + kTinyLin));
            const double eA = env_[0].update(std::max(lvlA, kDbFloor));
            const double eB = env_[1].update(std::max(lvlB, kDbFloor));
            const double eLink = std::max(eA, eB);          // peak link
            const double ctlA = (1.0 - link) * eA + link * eLink;
            const double ctlB = (1.0 - link) * eB + link * eLink;
            cell_[0].setControl(clampUnit(
                (static_cast<float>(ctlA) - cfg_.thresholdDb[0])
                * invControlRange_));
            cell_[1].setControl(clampUnit(
                (static_cast<float>(ctlB) - cfg_.thresholdDb[1])
                * invControlRange_));
            upA[i] = outA;
            upB[i] = outB;
        }

        // ---- coloration (oversampled, §8) -----------------------------------
        color_[0].processBlock(upA, static_cast<int>(nUp));
        color_[1].processBlock(upB, static_cast<int>(nUp));

        // ---- downsample ------------------------------------------------------
        if (useIir_) {
            iirOs_[0].processDown(upA, left, n);
            iirOs_[1].processDown(upB, right, n);
        } else {
            firOs_[0].processDown(upA, left, n);
            firOs_[1].processDown(upB, right, n);
        }

        // ---- M-S decode + makeup + delay-compensated mix (§3.4, §6.5) -------
        const float mix = cfg_.mix;
        for (std::size_t i = 0; i < n; ++i) {
            float a = left[i] * makeupGain_[0];
            float b = right[i] * makeupGain_[1];
            a = mix * a + (1.0f - mix) * dryScratch_[0][i];
            b = mix * b + (1.0f - mix) * dryScratch_[1][i];
            if (cfg_.mode == ChannelMode::MidSide) {
                // decode processed wet; dry was captured post-encode, so the
                // mixed signal decodes consistently
                msDecode(a, b, a, b);
            }
            left[i] = a;
            right[i] = b;
        }

        // ---- meters (block rate, §11) ----------------------------------------
        grMeter_[0].update(static_cast<double>(gainReductionDb(0)));
        grMeter_[1].update(static_cast<double>(gainReductionDb(1)));
    }

    [[nodiscard]] double meterGrDb(std::size_t ch) const noexcept
    {
        return grMeter_[ch].value();
    }

private:
    /// Delay line: returns the sample written `latency_` samples ago
    /// (identity when latency_ == 0, i.e. Eco mode).
    [[nodiscard]] float delayDry(std::size_t c, float v) noexcept
    {
        if (latency_ == 0)
            return v;
        const float old = dry_[c][dryPos_[c]];
        dry_[c][dryPos_[c]] = v;
        dryPos_[c] = (dryPos_[c] + 1 == dry_[c].size()) ? 0 : dryPos_[c] + 1;
        return old;
    }

    EngineConfig cfg_{};
    double fs_ = 48000.0;
    std::size_t maxBlock_ = 0;
    bool useIir_ = false;
    int osFactor_ = 4;
    int latency_ = 0;
    float invControlRange_ = 0.4f;

    std::array<Oversampler, 2> firOs_{};
    std::array<iir::IirOversampler2x, 2> iirOs_{};
    std::array<GainCell, 2> cell_{};
    std::array<SidechainEnvelope, 2> env_{};
    std::array<Coloration, 2> color_{};
    std::array<GrMeter, 2> grMeter_{};
    std::array<std::vector<float>, 2> upBuf_{};
    std::array<std::vector<float>, 2> dry_{};
    std::array<std::vector<float>, 2> dryScratch_{};
    std::array<std::size_t, 2> dryPos_{0, 0};
    std::array<float, 2> inGain_{1.f, 1.f};
    std::array<float, 2> makeupGain_{1.f, 1.f};
};

}  // namespace ta670::dsp
