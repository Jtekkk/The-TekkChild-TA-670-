// TA-670 DSP core — one processing channel: the feedback vari-mu loop (§3).
// SPDX-License-Identifier: MIT
//
// Realizes:  y[n] = G(Vc[n]) * x[n],  Vc[n] = D(y[n-1])            (§3.3)
// The unit delay in the control path makes the loop causal without iteration;
// its error is O(Ts) and vanishing at oversampled rates (§13.2).
#pragma once

#include <cmath>

#include "Common.hpp"
#include "GainCell.hpp"
#include "Sidechain.hpp"

namespace ta670::dsp {

class Channel {
public:
    void prepare(double fs, int position, float thresholdDb,
                 const CellCalibration& cal = {}) noexcept
    {
        thresholdDb_ = thresholdDb;
        invControlRange_ =
            1.0f / static_cast<float>(cal.controlRangeDb);
        cell_.prepare(cal);
        env_.prepare(position, fs);
        reset();
    }

    void reset() noexcept
    {
        cell_.reset();
        env_.reset();
    }

    /// Audio thread: one (oversampled) sample through the feedback loop.
    float processSample(float x) noexcept
    {
        // Apply the gain computed from the PREVIOUS sample's output (§3.3).
        const float out = cell_.currentGain() * x;

        // FEEDBACK detection: sense the output we just produced (§5.1).
        const float levelDb = fastDb(std::fabs(out) + kTinyLin);
        const double ctlDb =
            env_.update(std::max(static_cast<double>(levelDb), kDbFloor));

        // Overshoot -> normalized control drive -> next-sample gain (§4.2).
        const float over = static_cast<float>(ctlDb) - thresholdDb_;
        cell_.setControl(clampUnit(over * invControlRange_));
        return out;
    }

    void processBlock(float* buf, int numFrames) noexcept
    {
        for (int n = 0; n < numFrames; ++n)
            buf[n] = processSample(buf[n]);
    }

    /// Current gain reduction in dB (>= 0) for metering (§11.2).
    [[nodiscard]] float gainReductionDb() const noexcept
    {
        return -fastDb(cell_.currentGain() + kTinyLin);
    }

private:
    GainCell cell_{};
    SidechainEnvelope env_{};
    float thresholdDb_ = -20.0f;
    float invControlRange_ = 0.4f;
};

}  // namespace ta670::dsp
