// TA-670 — CLAP plugin adapter (§12.1). Bridges the CLAP C ABI to the
// framework-free DSP core: params (§10) <-> clap.params, Engine (§3) <->
// clap.process, latency (§9.4) <-> clap.latency, state save/load.
// SPDX-License-Identifier: MIT
//
// Threading model: parameter values live in an array of atomics. The audio
// thread applies them to the Engine at block start via the RT-safe setters;
// CLAP param events within a block are applied on the audio thread directly.
// Topology parameters (oversampling mode) take effect on (de)activate; a
// change requests a host restart (§12.3).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include <clap/clap.h>

#include "dsp/Engine.hpp"
#include "params/Parameters.hpp"

namespace {

using namespace ta670;

constexpr const char* kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_COMPRESSOR,
    CLAP_PLUGIN_FEATURE_MASTERING,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

constexpr clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.tekkchild.ta670",
    "Tekkchild Model 670",
    "Tekkchild",
    "https://github.com/Jtekkk/The-TekkChild-TA-670-",
    "",
    "",
    "0.1.0",
    "Vari-mu tube limiter/compressor (Fairchild 670 lineage)",
    kFeatures,
};

struct Ta670Plugin {
    clap_plugin_t plugin{};
    const clap_host_t* host = nullptr;

    dsp::Engine engine;
    std::array<std::atomic<float>, params::kNumParams> values{};
    std::atomic<bool> topologyDirty{false};
    double sampleRate = 48000.0;
    uint32_t maxFrames = 0;
    bool active = false;
    bool processing = false;

    // ---- lifecycle ---------------------------------------------------------
    void loadDefaults() noexcept
    {
        for (std::size_t i = 0; i < params::kNumParams; ++i)
            values[i].store(params::kParamTable[i].def,
                            std::memory_order_relaxed);
    }

    [[nodiscard]] float get(params::ParamId id) const noexcept
    {
        return values[static_cast<std::size_t>(id)].load(
            std::memory_order_relaxed);
    }

    [[nodiscard]] dsp::EngineConfig makeConfig() const noexcept
    {
        dsp::EngineConfig cfg;
        cfg.mode = static_cast<dsp::ChannelMode>(
            static_cast<int>(get(params::ParamId::Mode)));
        cfg.osMode = static_cast<dsp::OsMode>(
            static_cast<int>(get(params::ParamId::OsMode)));
        cfg.linkAmount = get(params::ParamId::Link) * 0.01f;
        cfg.mix = get(params::ParamId::Mix) * 0.01f;
        cfg.colorEnabled = get(params::ParamId::Color) > 0.5f;
        cfg.position = {static_cast<int>(get(params::ParamId::TimeConstA)),
                        static_cast<int>(get(params::ParamId::TimeConstB))};
        cfg.inputDb = {get(params::ParamId::InputA),
                       get(params::ParamId::InputB)};
        cfg.thresholdDb = {get(params::ParamId::ThresholdA),
                           get(params::ParamId::ThresholdB)};
        cfg.makeupDb = {get(params::ParamId::MakeupA),
                        get(params::ParamId::MakeupB)};
        return cfg;
    }

    bool activate(double sr, uint32_t maxBlock)
    {
        sampleRate = sr;
        maxFrames = maxBlock;
        engine.prepare(sr, maxBlock, makeConfig());
        topologyDirty.store(false, std::memory_order_relaxed);
        active = true;
        return true;
    }

    // ---- parameter application (audio thread, RT-safe) ---------------------
    void setParam(clap_id id, double value) noexcept
    {
        using params::ParamId;
        if (id >= params::kNumParams)
            return;
        const auto& d = params::kParamTable[id];
        float v = static_cast<float>(
            std::clamp(value, static_cast<double>(d.min),
                       static_cast<double>(d.max)));
        if (d.type != params::ParamType::Float)
            v = std::round(v);
        values[id].store(v, std::memory_order_relaxed);

        switch (static_cast<ParamId>(id)) {
        case ParamId::InputA: engine.setInputDb(0, v); break;
        case ParamId::InputB: engine.setInputDb(1, v); break;
        case ParamId::ThresholdA: engine.setThresholdDb(0, v); break;
        case ParamId::ThresholdB: engine.setThresholdDb(1, v); break;
        case ParamId::MakeupA: engine.setMakeupDb(0, v); break;
        case ParamId::MakeupB: engine.setMakeupDb(1, v); break;
        case ParamId::TimeConstA:
            engine.setPosition(0, static_cast<int>(v));
            break;
        case ParamId::TimeConstB:
            engine.setPosition(1, static_cast<int>(v));
            break;
        case ParamId::Mode:
            engine.setMode(static_cast<dsp::ChannelMode>(static_cast<int>(v)));
            break;
        case ParamId::Link: engine.setLink(v * 0.01f); break;
        case ParamId::Mix: engine.setMix(v * 0.01f); break;
        case ParamId::Color: engine.setColorEnabled(v > 0.5f); break;
        case ParamId::OsMode:
            // Topology change: applied at next activate(); ask the host.
            topologyDirty.store(true, std::memory_order_relaxed);
            if (host != nullptr && host->request_restart != nullptr)
                host->request_restart(host);
            break;
        default:
            break;  // Bypass handled in process(); Knee/Meter: no-op here
        }
    }

    void applyInEvents(const clap_input_events_t* in) noexcept
    {
        if (in == nullptr)
            return;
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id == CLAP_CORE_EVENT_SPACE_ID
                && h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* ev =
                    reinterpret_cast<const clap_event_param_value_t*>(h);
                setParam(ev->param_id, ev->value);
            }
        }
    }

    // ---- audio -------------------------------------------------------------
    clap_process_status process(const clap_process_t* p) noexcept
    {
        applyInEvents(p->in_events);

        if (p->audio_inputs_count < 1 || p->audio_outputs_count < 1)
            return CLAP_PROCESS_ERROR;
        const auto& ib = p->audio_inputs[0];
        const auto& ob = p->audio_outputs[0];
        if (ib.channel_count < 2 || ob.channel_count < 2
            || ib.data32 == nullptr || ob.data32 == nullptr)
            return CLAP_PROCESS_ERROR;

        const uint32_t n = p->frames_count;
        if (ob.data32[0] != ib.data32[0])
            std::copy(ib.data32[0], ib.data32[0] + n, ob.data32[0]);
        if (ob.data32[1] != ib.data32[1])
            std::copy(ib.data32[1], ib.data32[1] + n, ob.data32[1]);

        // Soft bypass: dry path only (keeps the same latency, §10.1).
        const bool bypassed = get(params::ParamId::Bypass) > 0.5f;
        if (bypassed) {
            engine.setMix(0.0f);
            engine.setInputDb(0, 0.0f);
            engine.setInputDb(1, 0.0f);
        }

        // The engine processes full blocks up to maxFrames.
        for (uint32_t off = 0; off < n;) {
            const uint32_t chunk = std::min(n - off, maxFrames);
            engine.processBlock(ob.data32[0] + off, ob.data32[1] + off,
                                chunk);
            off += chunk;
        }

        if (bypassed) {
            engine.setMix(get(params::ParamId::Mix) * 0.01f);
            engine.setInputDb(0, get(params::ParamId::InputA));
            engine.setInputDb(1, get(params::ParamId::InputB));
        }
        return CLAP_PROCESS_CONTINUE;
    }
};

Ta670Plugin* self(const clap_plugin_t* p) noexcept
{
    return static_cast<Ta670Plugin*>(p->plugin_data);
}

// ---------------------------------------------------------------------------
// clap.params
// ---------------------------------------------------------------------------
uint32_t paramsCount(const clap_plugin_t*) noexcept
{
    return static_cast<uint32_t>(params::kNumParams);
}

bool paramsGetInfo(const clap_plugin_t*, uint32_t index,
                   clap_param_info_t* info) noexcept
{
    if (index >= params::kNumParams)
        return false;
    const auto& d = params::kParamTable[index];
    info->id = static_cast<clap_id>(index);
    info->flags = 0;
    if (d.automatable)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (d.type != params::ParamType::Float)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.id == params::ParamId::Bypass)
        info->flags |= CLAP_PARAM_IS_BYPASS;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%.*s",
                  static_cast<int>(d.name.size()), d.name.data());
    std::snprintf(info->module, sizeof(info->module), "TA-670");
    info->min_value = static_cast<double>(d.min);
    info->max_value = static_cast<double>(d.max);
    info->default_value = static_cast<double>(d.def);
    return true;
}

bool paramsGetValue(const clap_plugin_t* p, clap_id id, double* out) noexcept
{
    if (id >= params::kNumParams)
        return false;
    *out = static_cast<double>(
        self(p)->values[id].load(std::memory_order_relaxed));
    return true;
}

bool paramsValueToText(const clap_plugin_t*, clap_id id, double value,
                       char* out, uint32_t capacity) noexcept
{
    if (id >= params::kNumParams)
        return false;
    const auto& d = params::kParamTable[id];
    if (d.type == params::ParamType::Float)
        std::snprintf(out, capacity, "%.2f %.*s", value,
                      static_cast<int>(d.unit.size()), d.unit.data());
    else
        std::snprintf(out, capacity, "%d", static_cast<int>(value));
    return true;
}

bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text,
                       double* out) noexcept
{
    if (id >= params::kNumParams)
        return false;
    *out = std::strtod(text, nullptr);
    return true;
}

void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in,
                 const clap_output_events_t*) noexcept
{
    self(p)->applyInEvents(in);
}

constexpr clap_plugin_params_t kParamsExt = {
    paramsCount, paramsGetInfo, paramsGetValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

// ---------------------------------------------------------------------------
// clap.audio-ports
// ---------------------------------------------------------------------------
uint32_t portsCount(const clap_plugin_t*, bool) noexcept { return 1; }

bool portsGet(const clap_plugin_t*, uint32_t index, bool isInput,
              clap_audio_port_info_t* info) noexcept
{
    if (index != 0)
        return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s",
                  isInput ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}

constexpr clap_plugin_audio_ports_t kPortsExt = {portsCount, portsGet};

// ---------------------------------------------------------------------------
// clap.latency
// ---------------------------------------------------------------------------
uint32_t latencyGet(const clap_plugin_t* p) noexcept
{
    return static_cast<uint32_t>(self(p)->engine.latencySamples());
}

constexpr clap_plugin_latency_t kLatencyExt = {latencyGet};

// ---------------------------------------------------------------------------
// clap.state — versioned flat array of plain values
// ---------------------------------------------------------------------------
bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) noexcept
{
    const uint32_t version = 1;
    if (stream->write(stream, &version, sizeof(version))
        != static_cast<int64_t>(sizeof(version)))
        return false;
    for (std::size_t i = 0; i < params::kNumParams; ++i) {
        const float v = self(p)->values[i].load(std::memory_order_relaxed);
        if (stream->write(stream, &v, sizeof(v))
            != static_cast<int64_t>(sizeof(v)))
            return false;
    }
    return true;
}

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) noexcept
{
    uint32_t version = 0;
    if (stream->read(stream, &version, sizeof(version))
        != static_cast<int64_t>(sizeof(version)) || version != 1)
        return false;
    for (std::size_t i = 0; i < params::kNumParams; ++i) {
        float v = 0.f;
        if (stream->read(stream, &v, sizeof(v))
            != static_cast<int64_t>(sizeof(v)))
            return false;
        self(p)->setParam(static_cast<clap_id>(i), static_cast<double>(v));
    }
    return true;
}

constexpr clap_plugin_state_t kStateExt = {stateSave, stateLoad};

// ---------------------------------------------------------------------------
// clap_plugin_t callbacks
// ---------------------------------------------------------------------------
bool pluginInit(const clap_plugin_t* p) noexcept
{
    self(p)->loadDefaults();
    return true;
}

void pluginDestroy(const clap_plugin_t* p) noexcept { delete self(p); }

bool pluginActivate(const clap_plugin_t* p, double sr, uint32_t,
                    uint32_t maxFrames) noexcept
{
    return self(p)->activate(sr, maxFrames);
}

void pluginDeactivate(const clap_plugin_t* p) noexcept
{
    self(p)->active = false;
}

bool pluginStartProcessing(const clap_plugin_t* p) noexcept
{
    self(p)->engine.reset();
    self(p)->processing = true;
    return true;
}

void pluginStopProcessing(const clap_plugin_t* p) noexcept
{
    self(p)->processing = false;
}

void pluginReset(const clap_plugin_t* p) noexcept { self(p)->engine.reset(); }

clap_process_status pluginProcess(const clap_plugin_t* p,
                                  const clap_process_t* proc) noexcept
{
    return self(p)->process(proc);
}

const void* pluginGetExtension(const clap_plugin_t*, const char* id) noexcept
{
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kParamsExt;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &kPortsExt;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &kLatencyExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &kStateExt;
    return nullptr;
}

void pluginOnMainThread(const clap_plugin_t*) noexcept {}

// ---------------------------------------------------------------------------
// factory & entry
// ---------------------------------------------------------------------------
uint32_t factoryGetPluginCount(const clap_plugin_factory_t*) noexcept
{
    return 1;
}

const clap_plugin_descriptor_t*
factoryGetPluginDescriptor(const clap_plugin_factory_t*,
                           uint32_t index) noexcept
{
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* factoryCreatePlugin(const clap_plugin_factory_t*,
                                         const clap_host_t* host,
                                         const char* pluginId) noexcept
{
    if (std::strcmp(pluginId, kDescriptor.id) != 0)
        return nullptr;
    auto* p = new (std::nothrow) Ta670Plugin;
    if (p == nullptr)
        return nullptr;
    p->host = host;
    p->plugin.desc = &kDescriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = pluginInit;
    p->plugin.destroy = pluginDestroy;
    p->plugin.activate = pluginActivate;
    p->plugin.deactivate = pluginDeactivate;
    p->plugin.start_processing = pluginStartProcessing;
    p->plugin.stop_processing = pluginStopProcessing;
    p->plugin.reset = pluginReset;
    p->plugin.process = pluginProcess;
    p->plugin.get_extension = pluginGetExtension;
    p->plugin.on_main_thread = pluginOnMainThread;
    return &p->plugin;
}

constexpr clap_plugin_factory_t kFactory = {
    factoryGetPluginCount, factoryGetPluginDescriptor, factoryCreatePlugin,
};

bool entryInit(const char*) noexcept { return true; }
void entryDeinit() noexcept {}

const void* entryGetFactory(const char* id) noexcept
{
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
