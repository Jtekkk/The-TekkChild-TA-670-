// TA-670 — miniature CLAP host (§14.5). Loads the built .clap through the
// real C ABI (dlopen), drives the full lifecycle, sends parameter events,
// and verifies audio behavior — plugin-format validation without a DAW.
// SPDX-License-Identifier: MIT

#include <clap/clap.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---- portable dynamic loading ------------------------------------------------
#ifdef _WIN32
using LibHandle = HMODULE;
LibHandle libOpen(const char* path) { return LoadLibraryA(path); }
void* libSym(LibHandle h, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(h, name));
}
void libClose(LibHandle h) { FreeLibrary(h); }
const char* libError() { return "LoadLibraryA failed"; }
#else
using LibHandle = void*;
LibHandle libOpen(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void* libSym(LibHandle h, const char* name) { return dlsym(h, name); }
void libClose(LibHandle h) { dlclose(h); }
const char* libError() { return dlerror(); }
#endif

int g_failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// ---- minimal host ----------------------------------------------------------
const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t*) {}

const clap_host_t kHost = {
    CLAP_VERSION_INIT, nullptr,
    "ta670-mini-host", "Tekkchild", "", "0.1.0",
    hostGetExtension, hostRequestRestart, hostRequestProcess,
    hostRequestCallback,
};

// ---- event lists ------------------------------------------------------------
struct EventList {
    std::vector<clap_event_param_value_t> events;

    static uint32_t size(const clap_input_events_t* list)
    {
        return static_cast<uint32_t>(
            static_cast<const EventList*>(list->ctx)->events.size());
    }

    static const clap_event_header_t* get(const clap_input_events_t* list,
                                          uint32_t index)
    {
        const auto* e = static_cast<const EventList*>(list->ctx);
        return &e->events[index].header;
    }

    [[nodiscard]] clap_input_events_t input() const
    {
        return {const_cast<EventList*>(this), size, get};
    }

    void pushParam(clap_id id, double value)
    {
        clap_event_param_value_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = id;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = value;
        events.push_back(ev);
    }
};

bool outTryPush(const clap_output_events_t*, const clap_event_header_t*)
{
    return true;
}

// ---- helpers ----------------------------------------------------------------
double rmsDb(const std::vector<float>& v, std::size_t from)
{
    double acc = 0.0;
    for (std::size_t i = from; i < v.size(); ++i)
        acc += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return 10.0 * std::log10(acc / static_cast<double>(v.size() - from)
                             + 1e-300);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-.clap>\n", argv[0]);
        return 2;
    }

    std::printf("TA-670 CLAP smoke test: %s\n", argv[1]);

    // ---- load through the real ABI ----------------------------------------
    LibHandle lib = libOpen(argv[1]);
    expect(lib != nullptr, "H1 load the .clap bundle");
    if (lib == nullptr) {
        std::fprintf(stderr, "load error: %s\n", libError());
        return 1;
    }
    const auto* entry =
        static_cast<const clap_plugin_entry_t*>(libSym(lib, "clap_entry"));
    expect(entry != nullptr && entry->init(argv[1]),
           "H2 clap_entry resolves and init succeeds");

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    expect(factory != nullptr && factory->get_plugin_count(factory) == 1,
           "H3 plugin factory exposes one plugin");

    const clap_plugin_descriptor_t* desc =
        factory->get_plugin_descriptor(factory, 0);
    expect(desc != nullptr
               && std::strcmp(desc->id, "com.tekkchild.ta670") == 0,
           "H4 descriptor id is com.tekkchild.ta670");

    const clap_plugin_t* plugin =
        factory->create_plugin(factory, &kHost, desc->id);
    expect(plugin != nullptr && plugin->init(plugin),
           "H5 create_plugin + init");

    // ---- extensions ---------------------------------------------------------
    const auto* paramsExt = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* latencyExt = static_cast<const clap_plugin_latency_t*>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    const auto* portsExt = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    expect(paramsExt != nullptr && latencyExt != nullptr
               && portsExt != nullptr,
           "H6 params/latency/audio-ports extensions present");

    expect(paramsExt->count(plugin) == 17, "H7 parameter count is 17");
    clap_param_info_t info{};
    bool infoOk = paramsExt->get_info(plugin, 0, &info)
        && std::strcmp(info.name, "Input A") == 0 && info.min_value == -24.0;
    double defMix = 0.0;
    infoOk = infoOk && paramsExt->get_value(plugin, 15, &defMix)
        && defMix == 100.0;
    expect(infoOk, "H8 param info + default values match the spec table");

    clap_audio_port_info_t port{};
    expect(portsExt->get(plugin, 0, true, &port) && port.channel_count == 2,
           "H9 stereo main port");

    // ---- activate: Standard 4x -> latency 87 -------------------------------
    const double fs = 48000.0;
    const uint32_t block = 256;
    expect(plugin->activate(plugin, fs, 32, block),
           "H10 activate at 48 kHz");
    expect(latencyExt->get(plugin) == 87,
           "H11 reported latency is 87 samples (Standard 4x)");
    expect(plugin->start_processing(plugin), "H12 start_processing");

    // ---- process: threshold -20, tone at -10 -> gain reduction --------------
    EventList paramEvents;
    paramEvents.pushParam(2, -20.0);  // ThresholdA
    paramEvents.pushParam(3, -20.0);  // ThresholdB
    paramEvents.pushParam(12, 0.0);   // Color off for a clean measurement
    clap_input_events_t inEvents = paramEvents.input();
    const clap_output_events_t outEvents = {nullptr, outTryPush};

    const std::size_t total = 48000;
    std::vector<float> inL(total), inR(total), outL(total), outR(total);
    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / fs;
        inL[i] = static_cast<float>(
            0.3162 * std::sin(2.0 * 3.14159265358979324 * 997.0 * t));
        inR[i] = inL[i];
    }

    bool processedOk = true;
    for (std::size_t off = 0; off < total; off += block) {
        // final block is short (48000 % 256 != 0) — like a real host
        const auto chunk = static_cast<uint32_t>(
            std::min<std::size_t>(block, total - off));
        float* ins[2] = {inL.data() + off, inR.data() + off};
        float* outs[2] = {outL.data() + off, outR.data() + off};
        clap_audio_buffer_t ib{};
        ib.data32 = ins;
        ib.channel_count = 2;
        clap_audio_buffer_t ob{};
        ob.data32 = outs;
        ob.channel_count = 2;
        clap_process_t proc{};
        proc.steady_time = static_cast<int64_t>(off);
        proc.frames_count = chunk;
        proc.audio_inputs = &ib;
        proc.audio_outputs = &ob;
        proc.audio_inputs_count = 1;
        proc.audio_outputs_count = 1;
        proc.in_events = &inEvents;
        proc.out_events = &outEvents;
        processedOk = processedOk
            && plugin->process(plugin, &proc) == CLAP_PROCESS_CONTINUE;
        if (off == 0) {
            paramEvents.events.clear();  // params only on the first block
            inEvents = paramEvents.input();
        }
    }
    expect(processedOk, "H13 process returns CONTINUE over 1 s of audio");

    bool finite = true;
    for (std::size_t i = 0; i < total; ++i)
        finite = finite && std::isfinite(outL[i]) && std::isfinite(outR[i]);
    const double inDb = rmsDb(inL, total / 2);
    const double outDb = rmsDb(outL, total / 2);
    const double grDb = inDb - outDb;
    char msg[96];
    std::snprintf(msg, sizeof(msg),
                  "H14 finite audio with %.1f dB steady gain reduction", grDb);
    expect(finite && grDb > 4.0 && grDb < 20.0, msg);

    // ---- teardown ------------------------------------------------------------
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    libClose(lib);
    expect(true, "H15 clean teardown");

    std::printf("%s (%d failure%s)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
