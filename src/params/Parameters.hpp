// TA-670 parameter layer (§10) — the normative §10.1 table as data, plus
// value mapping, dezippering, and wait-free snapshot publication (§12.3).
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace ta670::params {

enum class ParamId : std::size_t {
    InputA, InputB,
    ThresholdA, ThresholdB,
    MakeupA, MakeupB,
    TimeConstA, TimeConstB,
    Mode,
    Link,
    KneeA, KneeB,
    Color,
    MeterSelect,
    OsMode,
    Mix,
    Bypass,
    Count
};

inline constexpr std::size_t kNumParams =
    static_cast<std::size_t>(ParamId::Count);

enum class ParamType { Float, Choice, Bool };

struct ParamDesc {
    ParamId id;
    std::string_view name;
    std::string_view unit;
    ParamType type;
    float min, max, def;
    float smoothTauMs;  ///< 0 = not smoothed (stepped / boolean)
    bool automatable;
};

/// Normative parameter table (§10.1). Switches are stepped (§10.3).
inline constexpr std::array<ParamDesc, kNumParams> kParamTable{{
    {ParamId::InputA, "Input A", "dB", ParamType::Float, -24.f, 24.f, 0.f, 20.f, true},
    {ParamId::InputB, "Input B", "dB", ParamType::Float, -24.f, 24.f, 0.f, 20.f, true},
    {ParamId::ThresholdA, "Threshold A", "dB", ParamType::Float, -40.f, 10.f, 0.f, 20.f, true},
    {ParamId::ThresholdB, "Threshold B", "dB", ParamType::Float, -40.f, 10.f, 0.f, 20.f, true},
    {ParamId::MakeupA, "Makeup A", "dB", ParamType::Float, -12.f, 24.f, 0.f, 20.f, true},
    {ParamId::MakeupB, "Makeup B", "dB", ParamType::Float, -12.f, 24.f, 0.f, 20.f, true},
    {ParamId::TimeConstA, "Time Const A", "", ParamType::Choice, 1.f, 6.f, 4.f, 0.f, true},
    {ParamId::TimeConstB, "Time Const B", "", ParamType::Choice, 1.f, 6.f, 4.f, 0.f, true},
    {ParamId::Mode, "Mode", "", ParamType::Choice, 0.f, 2.f, 1.f, 0.f, true},
    {ParamId::Link, "SC Link", "%", ParamType::Float, 0.f, 100.f, 100.f, 20.f, true},
    {ParamId::KneeA, "Knee A", "", ParamType::Float, 0.f, 1.f, 0.3f, 20.f, true},
    {ParamId::KneeB, "Knee B", "", ParamType::Float, 0.f, 1.f, 0.3f, 20.f, true},
    {ParamId::Color, "Color", "", ParamType::Bool, 0.f, 1.f, 1.f, 0.f, true},
    {ParamId::MeterSelect, "Meter", "", ParamType::Choice, 0.f, 3.f, 0.f, 0.f, false},
    {ParamId::OsMode, "Oversampling", "", ParamType::Choice, 0.f, 3.f, 1.f, 0.f, true},
    {ParamId::Mix, "Mix", "%", ParamType::Float, 0.f, 100.f, 100.f, 20.f, true},
    {ParamId::Bypass, "Bypass", "", ParamType::Bool, 0.f, 1.f, 0.f, 0.f, true},
}};

[[nodiscard]] constexpr const ParamDesc& desc(ParamId id) noexcept
{
    return kParamTable[static_cast<std::size_t>(id)];
}

// ---------------------------------------------------------------------------
// Host-normalized [0,1] <-> plain value mapping. All §10.1 floats are linear
// taper; Choice/Bool quantize to integer steps (§10.3).
// ---------------------------------------------------------------------------
[[nodiscard]] inline float toPlain(ParamId id, float normalized) noexcept
{
    const auto& d = desc(id);
    const float clamped = normalized < 0.f ? 0.f
        : (normalized > 1.f ? 1.f : normalized);
    float plain = d.min + clamped * (d.max - d.min);
    if (d.type != ParamType::Float)
        plain = std::round(plain);
    return plain;
}

[[nodiscard]] inline float toNormalized(ParamId id, float plain) noexcept
{
    const auto& d = desc(id);
    const float n = (plain - d.min) / (d.max - d.min);
    return n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
}

// ---------------------------------------------------------------------------
// Dezippering (§10.4): one-pole toward the target, tau from the table.
// ---------------------------------------------------------------------------
class Smoother {
public:
    void prepare(double fs, float tauMs, float initial) noexcept
    {
        alpha_ = (tauMs > 0.f)
            ? std::exp(-1.0 / (static_cast<double>(tauMs) * 1e-3 * fs))
            : 0.0;
        current_ = target_ = static_cast<double>(initial);
    }

    void setTarget(float t) noexcept { target_ = static_cast<double>(t); }

    [[nodiscard]] float next() noexcept
    {
        current_ = alpha_ * current_ + (1.0 - alpha_) * target_;
        return static_cast<float>(current_);
    }

    void snap() noexcept { current_ = target_; }
    [[nodiscard]] float value() const noexcept
    {
        return static_cast<float>(current_);
    }

private:
    double alpha_ = 0.0;
    double current_ = 0.0;
    double target_ = 0.0;
};

// ---------------------------------------------------------------------------
// Wait-free snapshot publication (§12.3): the message thread validates and
// writes a complete plain-value snapshot; the audio thread swaps to the
// newest at block start. Single-producer / single-consumer.
// ---------------------------------------------------------------------------
struct Snapshot {
    std::array<float, kNumParams> values{};

    [[nodiscard]] static Snapshot defaults() noexcept
    {
        Snapshot s;
        for (std::size_t i = 0; i < kNumParams; ++i)
            s.values[i] = kParamTable[i].def;
        return s;
    }

    [[nodiscard]] float get(ParamId id) const noexcept
    {
        return values[static_cast<std::size_t>(id)];
    }

    void set(ParamId id, float plain) noexcept
    {
        values[static_cast<std::size_t>(id)] = plain;
    }
};

class SnapshotExchange {
public:
    SnapshotExchange() noexcept
    {
        slots_[0] = Snapshot::defaults();
        slots_[1] = slots_[0];
    }

    /// Message thread: publish a new complete snapshot.
    void publish(const Snapshot& s) noexcept
    {
        const std::size_t next = 1u - active_.load(std::memory_order_relaxed);
        slots_[next] = s;
        active_.store(next, std::memory_order_release);
    }

    /// Audio thread: fetch the newest snapshot (wait-free copy).
    [[nodiscard]] Snapshot read() const noexcept
    {
        return slots_[active_.load(std::memory_order_acquire)];
    }

private:
    std::array<Snapshot, 2> slots_{};
    std::atomic<std::size_t> active_{0};
};

}  // namespace ta670::params
