// TA-670 DSP core — detection sidechain envelope (§5).
// SPDX-License-Identifier: MIT
//
// Fast attack/release one-pole plus slow "reservoir" states that realize the
// program-dependent release of Positions 5 and 6 (§5.4). All envelope state
// is double precision (§13.1). Coefficients are computed in prepare(); the
// per-sample path is branch-lean and allocation-free (§12.3).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "Common.hpp"

namespace ta670::dsp {

/// One reservoir: slow charge toward the program level, very slow release.
struct ReservoirSpec {
    double releaseTau;  ///< seconds (10 or 25)
    double chargeTau;   ///< seconds (1.0 for the 10 s stage, 5.0 for 25 s) §5.4
};

/// Normative §5.5 position table (attack tau, fast release tau, reservoirs).
struct PositionSpec {
    double attackTau;
    double fastReleaseTau;
    std::size_t numReservoirs;
    std::array<ReservoirSpec, 2> reservoirs;
};

inline constexpr std::array<PositionSpec, 6> kPositions{{
    {0.2e-3, 0.30, 0, {}},                                   // 1
    {0.2e-3, 0.80, 0, {}},                                   // 2
    {0.4e-3, 2.00, 0, {}},                                   // 3
    {0.4e-3, 5.00, 0, {}},                                   // 4
    {0.4e-3, 0.20, 1, {{{10.0, 1.0}, {}}}},                  // 5
    {0.2e-3, 0.30, 2, {{{10.0, 1.0}, {25.0, 5.0}}}},         // 6
}};

class SidechainEnvelope {
public:
    /// Off-thread: select a position (1..6) and bake coefficients for fs.
    void prepare(int position, double fs) noexcept
    {
        const auto& spec =
            kPositions[static_cast<std::size_t>(
                std::clamp(position, 1, 6) - 1)];
        alphaAttack_ = onePoleAlpha(spec.attackTau, fs);
        alphaFast_ = onePoleAlpha(spec.fastReleaseTau, fs);
        numRes_ = spec.numReservoirs;
        for (std::size_t j = 0; j < numRes_; ++j) {
            alphaRes_[j] = onePoleAlpha(spec.reservoirs[j].releaseTau, fs);
            alphaChg_[j] = onePoleAlpha(spec.reservoirs[j].chargeTau, fs);
        }
        reset();
    }

    void reset() noexcept
    {
        fast_ = kDbFloor;
        res_.fill(kDbFloor);
    }

    /// Audio thread: advance one sample with the detector level (dB) and
    /// return the effective control envelope (dB) — max of fast state and
    /// reservoirs (§5.4).
    [[nodiscard]] double update(double levelDb) noexcept
    {
        const double a = (levelDb > fast_) ? alphaAttack_ : alphaFast_;
        fast_ = a * fast_ + (1.0 - a) * levelDb;
        double ctl = fast_;
        for (std::size_t j = 0; j < numRes_; ++j) {
            if (levelDb > res_[j])
                res_[j] = alphaChg_[j] * res_[j] + (1.0 - alphaChg_[j]) * levelDb;
            else
                res_[j] = alphaRes_[j] * res_[j] + (1.0 - alphaRes_[j]) * kDbFloor;
            ctl = std::max(ctl, res_[j]);
        }
        return ctl;
    }

private:
    double alphaAttack_ = 0.0;
    double alphaFast_ = 0.0;
    std::array<double, 2> alphaRes_{};
    std::array<double, 2> alphaChg_{};
    std::size_t numRes_ = 0;
    double fast_ = kDbFloor;
    std::array<double, 2> res_{kDbFloor, kDbFloor};
};

}  // namespace ta670::dsp
