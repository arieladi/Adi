// SPDX-License-Identifier: AGPL-3.0-only
// Load the delivered binary through the public CLAP ABI, with no audio device.
#include <algorithm>
#include <array>
#include <clap/clap.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
int main(int argc, char **argv) {
    const bool fixture = argc == 3 && std::strcmp(argv[2], "--host-fixture") == 0;
    if (argc != 2 && !fixture)
        return 2;
#if defined(_WIN32)
    const auto library = LoadLibraryA(argv[1]);
    const auto *entry =
        library ? reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(library, "clap_entry"))
                : nullptr;
#else
    auto *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    const auto *entry =
        library ? static_cast<const clap_plugin_entry_t *>(dlsym(library, "clap_entry")) : nullptr;
#endif
    if (!entry || !entry->init(argv[1]))
        return 3;
    const auto *factory =
        static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || factory->get_plugin_count(factory) != 1)
        return 4;
    const auto *desc = factory->get_plugin_descriptor(factory, 0);
    if (std::strcmp(desc->id, "com.adi.dynamic-eq") || std::strcmp(desc->name, "ADI Dynamic EQ"))
        return 5;
    const clap_host_t host{CLAP_VERSION,
                           nullptr,
                           "ADI offline test",
                           "ADI",
                           "https://github.com/arieladi/Adi",
                           "0.1",
                           [](const clap_host_t *, const char *) -> const void * { return nullptr; },
                           [](const clap_host_t *) {},
                           [](const clap_host_t *) {},
                           [](const clap_host_t *) {}};
    const auto *plugin = factory->create_plugin(factory, &host, desc->id);
    if (!plugin || !plugin->init(plugin))
        return 6;
    const auto *params =
        static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto *ports =
        static_cast<const clap_plugin_audio_ports_t *>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    // Pinned getParameterLayout: nine globals + 24 bands * 25 parameters.
    if (!params || !ports || params->count(plugin) != 9 + 24 * 25 || ports->count(plugin, false) != 1 ||
        ports->count(plugin, true) < 1 || ports->count(plugin, true) > 2)
        return 7;
    std::set<clap_id> ids;
    for (uint32_t i = 0; i < params->count(plugin); ++i) {
        clap_param_info_t info{};
        double current = 0;
        if (!params->get_info(plugin, i, &info) || !ids.insert(info.id).second ||
            !params->get_value(plugin, info.id, &current) || !std::isfinite(current) ||
            current < info.min_value || current > info.max_value)
            return 12;
        // Resolve the host fixture through the binary's ABI. No copied JUCE
        // hash algorithm or hard-coded CLAP ids/normalization in the test.
        if (fixture) {
            const char* text = nullptr;
            if (std::strcmp(info.name, "Filter Status0") == 0) text = "On";
            if (std::strcmp(info.name, "Freq0") == 0) text = "220";
            if (std::strcmp(info.name, "Gain0") == 0) text = "-12";
            if (std::strcmp(info.name, "Q0") == 0) text = "1";
            if (text) {
                double plain = 0;
                if (!params->text_to_value(plugin, info.id, text, &plain) ||
                    !std::isfinite(plain) || info.max_value <= info.min_value ||
                    plain < info.min_value || plain > info.max_value) return 13;
                std::printf("HOST_PARAM\t%u\t%.17g\t%s\n", info.id,
                    (plain-info.min_value)/(info.max_value-info.min_value), info.name);
                if (std::strcmp(info.name, "Filter Status0") == 0) {
                    if (!params->text_to_value(plugin, info.id, "Bypass", &plain)) return 14;
                    std::printf("HOST_BYPASS\t%u\t%.17g\n", info.id,
                        (plain-info.min_value)/(info.max_value-info.min_value));
                }
            }
        }
    }
    if (!plugin->activate(plugin, 48000, 1, 256) || !plugin->start_processing(plugin))
        return 8;
    std::array<float, 256> left{}, right{}, auxL{}, auxR{}, outL{}, outR{};
    float *main[]{left.data(), right.data()};
    float *aux[]{auxL.data(), auxR.data()};
    float *outputs[]{outL.data(), outR.data()};
    clap_audio_buffer_t inputs[2]{};
    inputs[0].data32 = main;
    inputs[0].channel_count = 2;
    inputs[1].data32 = aux;
    inputs[1].channel_count = 2;
    clap_audio_buffer_t output{};
    output.data32 = outputs;
    output.channel_count = 2;
    const clap_input_events_t events{
        nullptr, [](const clap_input_events_t *) -> uint32_t { return 0; },
        [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return nullptr; }};
    const clap_output_events_t outEvents{
        nullptr, [](const clap_output_events_t *, const clap_event_header_t *) { return true; }};
    clap_process_t process{};
    process.frames_count = 256;
    process.audio_inputs = inputs;
    process.audio_outputs = &output;
    process.audio_inputs_count = ports->count(plugin, true);
    process.audio_outputs_count = 1;
    process.in_events = &events;
    process.out_events = &outEvents;
    double error = 0;
    for (int block = 0; block < 8; ++block) {
        for (size_t i = 0; i < left.size(); ++i)
            left[i] = right[i] = .1f * std::sin(static_cast<float>(i) * .07f);
        process.steady_time = block * 256;
        if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR)
            return 9;
        for (size_t i = 0; i < left.size(); ++i) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
                return 10;
            error = std::max(error, std::abs(static_cast<double>(outL[i] - left[i])));
        }
    }
    const auto count = params->count(plugin);
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
#if defined(_WIN32)
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    std::printf("CLAP identity, %u parameters, ports, lifecycle and silent-device processing passed; unity "
                "error %.9g\n",
                count, error);
    return error < 1e-6 ? 0 : 11;
}
