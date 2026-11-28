// SPDX-License-Identifier: GPL-3.0-or-later
#include <clap/clap.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

constexpr std::uint32_t delay = 7;
constexpr clap_id gainParameter = 0x123456;
constexpr double gainMin = 0.5;
constexpr double gainMax = 1.5;
constexpr double gainDefault = 1.25;
const char* const features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const clap_plugin_descriptor_t descriptor{
    CLAP_VERSION, "org.adi.test.native-host", "Native host test", "ADI",
    "", "", "", "1", "Two voices with independently audible tuning", features,
};

struct Voice {
    std::int32_t id = -1;
    double velocity = 0;
    double tuning = 0;
};

struct Instrument {
    clap_plugin_t api{};
    std::array<Voice, 2> voices{};
    std::array<std::array<float, delay>, 2> audioDelay{};
    std::uint32_t cursor = 0;
    std::uint8_t gain = 2;
    double parameterGain = gainDefault;
    bool offline = false;
};

Instrument& data(const clap_plugin_t* plugin) {
    return *static_cast<Instrument*>(plugin->plugin_data);
}

bool CLAP_ABI init(const clap_plugin_t*) { return true; }
void CLAP_ABI destroy(const clap_plugin_t* plugin) { delete &data(plugin); }
bool CLAP_ABI activate(const clap_plugin_t*, double, std::uint32_t, std::uint32_t) { return true; }
bool CLAP_ABI start(const clap_plugin_t*) { return true; }
void CLAP_ABI nothing(const clap_plugin_t*) {}

void CLAP_ABI reset(const clap_plugin_t* plugin) {
    auto& instrument = data(plugin);
    instrument.voices = {};
    instrument.audioDelay = {};
    instrument.cursor = 0;
}

void update(Instrument& instrument, const clap_event_header_t* event) {
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF) {
        clap_event_note_t value{};
        if (event->size < sizeof(value)) return;
        std::memcpy(&value, event, sizeof(value));
        for (auto& voice : instrument.voices) {
            if (event->type == CLAP_EVENT_NOTE_OFF && voice.id == value.note_id) {
                voice = {};
                return;
            }
            if (event->type == CLAP_EVENT_NOTE_ON && voice.id == -1) {
                voice = {value.note_id, value.velocity, 0.0};
                return;
            }
        }
    } else if (event->type == CLAP_EVENT_NOTE_EXPRESSION) {
        clap_event_note_expression_t value{};
        if (event->size < sizeof(value)) return;
        std::memcpy(&value, event, sizeof(value));
        if (value.expression_id != CLAP_NOTE_EXPRESSION_TUNING) return;
        for (auto& voice : instrument.voices) {
            if (voice.id == value.note_id) voice.tuning = value.value;
        }
    } else if (event->type == CLAP_EVENT_PARAM_VALUE) {
        clap_event_param_value_t value{};
        if (event->size < sizeof(value)) return;
        std::memcpy(&value, event, sizeof(value));
        if (value.param_id == gainParameter && value.note_id == -1 &&
            value.value >= gainMin && value.value <= gainMax)
            instrument.parameterGain = value.value;
    }
}

clap_process_status CLAP_ABI process(const clap_plugin_t* plugin, const clap_process_t* block) {
    if (block->audio_outputs_count != 1 || block->audio_outputs[0].channel_count != 2 ||
        !block->audio_outputs[0].data32 || !block->in_events) return CLAP_PROCESS_ERROR;
    auto& instrument = data(plugin);
    auto** output = block->audio_outputs[0].data32;
    const auto& input = *block->in_events;
    const auto eventCount = input.size(&input);
    std::uint32_t nextEvent = 0;
    for (std::uint32_t frame = 0; frame < block->frames_count; ++frame) {
        while (nextEvent < eventCount) {
            const auto* event = input.get(&input, nextEvent);
            if (event->time > frame) break;
            update(instrument, event);
            ++nextEvent;
        }
        for (std::size_t channel = 0; channel < instrument.voices.size(); ++channel) {
            const auto& voice = instrument.voices[channel];
            const auto level = instrument.offline ? 0.5 : 0.125;
            auto& delayed = instrument.audioDelay[channel][instrument.cursor];
            output[channel][frame] = delayed;
            delayed = voice.id == -1 ? 0.0f : static_cast<float>(
                level * instrument.gain * instrument.parameterGain * (voice.velocity + voice.tuning));
        }
        instrument.cursor = (instrument.cursor + 1) % delay;
    }
    block->audio_outputs[0].constant_mask = 0;
    return CLAP_PROCESS_CONTINUE;
}

std::uint32_t CLAP_ABI audioCount(const clap_plugin_t*, bool input) { return input ? 0 : 1; }
bool CLAP_ABI audioInfo(const clap_plugin_t*, std::uint32_t index, bool input, clap_audio_port_info_t* info) {
    if (input || index != 0) return false;
    *info = {};
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

std::uint32_t CLAP_ABI noteCount(const clap_plugin_t*, bool input) { return input ? 1 : 0; }
bool CLAP_ABI noteInfo(const clap_plugin_t*, std::uint32_t index, bool input, clap_note_port_info_t* info) {
    if (!input || index != 0) return false;
    *info = {};
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    return true;
}

bool CLAP_ABI save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    const auto gain = data(plugin).gain;
    return stream->write(stream, &gain, 1) == 1;
}

bool CLAP_ABI load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    std::uint8_t gain = 0, extra = 0;
    if (stream->read(stream, &gain, 1) != 1 || (gain != 1 && gain != 2) ||
        stream->read(stream, &extra, 1) != 0) return false;
    data(plugin).gain = gain;
    return true;
}

bool CLAP_ABI needsRealtime(const clap_plugin_t*) { return false; }
bool CLAP_ABI renderMode(const clap_plugin_t* plugin, clap_plugin_render_mode mode) {
    data(plugin).offline = mode == CLAP_RENDER_OFFLINE;
    return true;
}
std::uint32_t CLAP_ABI latency(const clap_plugin_t*) { return delay; }

std::uint32_t CLAP_ABI parameterCount(const clap_plugin_t*) { return 1; }
bool CLAP_ABI parameterInfo(const clap_plugin_t*, std::uint32_t index, clap_param_info_t* info) {
    if (index != 0) return false;
    *info = {};
    info->id = gainParameter;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::memcpy(info->name, "Gain", sizeof("Gain"));
    info->min_value = gainMin;
    info->max_value = gainMax;
    info->default_value = gainDefault;
    return true;
}
bool CLAP_ABI parameterValue(const clap_plugin_t* plugin, clap_id id, double* value) {
    if (id != gainParameter) return false;
    *value = data(plugin).parameterGain;
    return true;
}
bool CLAP_ABI parameterText(const clap_plugin_t*, clap_id id, double value, char* text, std::uint32_t size) {
    if (id != gainParameter || size == 0) return false;
    const auto written = std::snprintf(text, size, "%.3f", value);
    return written >= 0 && static_cast<std::uint32_t>(written) < size;
}
bool CLAP_ABI parseParameter(const clap_plugin_t*, clap_id id, const char* text, double* value) {
    if (id != gainParameter) return false;
    char* end = nullptr;
    const auto parsed = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(parsed >= gainMin && parsed <= gainMax)) return false;
    *value = parsed;
    return true;
}
void CLAP_ABI flushParameters(const clap_plugin_t* plugin, const clap_input_events_t* input,
                              const clap_output_events_t*) {
    auto& instrument = data(plugin);
    for (std::uint32_t i = 0; i < input->size(input); ++i)
        update(instrument, input->get(input, i));
}

const clap_plugin_audio_ports_t audioPorts{audioCount, audioInfo};
const clap_plugin_note_ports_t notePorts{noteCount, noteInfo};
const clap_plugin_state_t state{save, load};
const clap_plugin_render_t render{needsRealtime, renderMode};
const clap_plugin_latency_t latencyExtension{latency};
const clap_plugin_params_t parameters{
    parameterCount, parameterInfo, parameterValue, parameterText, parseParameter, flushParameters,
};

const void* CLAP_ABI extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &state;
    if (std::strcmp(id, CLAP_EXT_RENDER) == 0) return &render;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &latencyExtension;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &parameters;
    return nullptr;
}

std::uint32_t CLAP_ABI count(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* CLAP_ABI describe(const clap_plugin_factory_t*, std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}

const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    if (std::strcmp(id, descriptor.id) != 0) return nullptr;
    auto* instrument = new (std::nothrow) Instrument;
    if (!instrument) return nullptr;
    instrument->api = {&descriptor, instrument, init, destroy, activate, nothing, start,
                       nothing, reset, process, extension, nothing};
    return &instrument->api;
}

const clap_plugin_factory_t factory{count, describe, create};
bool CLAP_ABI entry(const char*) { return true; }
void CLAP_ABI leave() {}
const void* CLAP_ABI getFactory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION, entry, leave, getFactory};
