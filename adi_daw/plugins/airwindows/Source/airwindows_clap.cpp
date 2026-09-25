// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows (ADR-0166): the kept Airwindows effects as CLAP plug-ins.
//
// WHY NOT JUST SHIP AIRWINDOWS CONSOLIDATED, which is already a CLAP with a
// UI? Because it is ONE plug-in with a selector inside it. Its parameters are
// generic slots whose meaning changes with the selected effect, so an
// automation lane (ADR-0165) or a saved parameter row (ADR-0142) bound to
// "slot 3" means Density's Output today and Galactic's Bigness after someone
// changes the selector. And the DAW's browser shows one entry, not 141 you can
// search for by name. Here each effect is its own plug-in with its own id and
// parameters that never change meaning. Consolidated stays available for its
// browser-in-a-plug-in; the two do not conflict.
//
// The DSP is Airwindows' own, unchanged, from the airwin2rack registry (MIT).
// This file adds only what CLAP needs, and auto gain.
//
// NO EDITOR. Airwindows plug-ins have never had one by design: the host draws
// sliders from the parameter names and value texts, which is the lightest UI
// there is, and the parameter metadata here is what makes that panel good.
// Drawing it is the device host's job (mac, ADR-0165's split).

#include "airwindows_clap.hpp"

#include "adi/dsp/auto_gain.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The effects' headers: Airwindows' code, whose warnings are not ours.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include "aw_catalogue.inc"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace adi::airwindows {
namespace {

using Base = AirwinConsolidatedBase;

struct Effect {
    const char* name;
    const char* category;
    const char* description;
    bool autoGain;          // auto gain on by default (tools/airwindows_catalogue.py)
    int params;
    Base* (*make)();
};

#define ADI_AW_ROWS 1
#define ADI_AW_ROW(sym, name, category, description, autoGain)                     \
    {name, category, description, autoGain, airwinconsolidated::sym::kNumParameters, \
     +[]() -> Base* { return new airwinconsolidated::sym::sym(0); }},
const Effect kEffects[] = {
#include "aw_catalogue.inc"
};
#undef ADI_AW_ROW
#undef ADI_AW_ROWS

constexpr std::size_t kEffectCount = sizeof(kEffects) / sizeof(kEffects[0]);

// --- descriptors -------------------------------------------------------------

/// What the category means to a host's browser, plus a few names that say more
/// than their category does.
std::vector<const char*> featuresFor(const Effect& e) {
    std::vector<const char*> f{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT};
    const std::string cat = e.category;
    const std::string name = e.name;
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("Delay")) f.push_back(CLAP_PLUGIN_FEATURE_DELAY);
    else if (cat == "Reverb" || cat == "Ambience") f.push_back(CLAP_PLUGIN_FEATURE_REVERB);
    if (has("Tremo")) f.push_back(CLAP_PLUGIN_FEATURE_TREMOLO);
    if (has("Pitch") || has("Shifter")) f.push_back(CLAP_PLUGIN_FEATURE_PITCH_SHIFTER);
    if (has("Glitch")) f.push_back(CLAP_PLUGIN_FEATURE_GLITCH);
    if (has("DeBess")) f.push_back(CLAP_PLUGIN_FEATURE_DEESSER);
    if (has("DeNoise")) f.push_back(CLAP_PLUGIN_FEATURE_RESTORATION);
    if (has("Gate")) f.push_back(CLAP_PLUGIN_FEATURE_GATE);
    if (has("Mastering")) f.push_back(CLAP_PLUGIN_FEATURE_MASTERING);
    if (cat == "Saturation" || cat == "Distortion" || cat == "Tape" || cat == "Amp Sims" ||
        cat == "Lo-Fi")
        f.push_back(CLAP_PLUGIN_FEATURE_DISTORTION);
    else if (cat == "Dynamics" && !has("Gate")) f.push_back(CLAP_PLUGIN_FEATURE_COMPRESSOR);
    else if (cat == "Clipping") f.push_back(CLAP_PLUGIN_FEATURE_LIMITER);
    else if (cat == "Filter" || cat == "XYZ Filters" || cat == "Biquads")
        f.push_back(has("EQ") ? CLAP_PLUGIN_FEATURE_EQUALIZER : CLAP_PLUGIN_FEATURE_FILTER);
    else if (cat == "Brightness" || cat == "Bass") f.push_back(CLAP_PLUGIN_FEATURE_EQUALIZER);
    else if (cat == "Consoles" || cat == "Subtlety" || cat == "Tone Color")
        f.push_back(CLAP_PLUGIN_FEATURE_MIXING);
    else if (cat == "Utility" || cat == "Stereo" || cat == "Dithers" || cat == "Noise")
        f.push_back(CLAP_PLUGIN_FEATURE_UTILITY);
    f.push_back(CLAP_PLUGIN_FEATURE_STEREO);
    f.push_back(nullptr);
    return f;
}

struct Catalogue {
    std::vector<std::string> ids;
    std::vector<std::vector<const char*>> features;
    std::vector<clap_plugin_descriptor_t> descriptors;
};

Catalogue& catalogue() {
    static Catalogue c = [] {
        Catalogue k;
        k.ids.reserve(kEffectCount);
        k.features.reserve(kEffectCount);
        k.descriptors.reserve(kEffectCount);
        for (const Effect& e : kEffects) {
            std::string id = "com.adi.airwindows.";
            for (const char* p = e.name; *p != '\0'; ++p)
                id += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            k.ids.push_back(id);
            k.features.push_back(featuresFor(e));
        }
        for (std::size_t i = 0; i < kEffectCount; ++i) {
            clap_plugin_descriptor_t d{};
            d.clap_version = CLAP_VERSION_INIT;
            d.id = k.ids[i].c_str();
            d.name = kEffects[i].name;
            d.vendor = "Airwindows";
            d.url = "https://www.airwindows.com/";
            d.manual_url = "https://www.airwindows.com/";
            d.support_url = "https://github.com/arieladi/Adi";
            d.version = "0.1.0";
            d.description = kEffects[i].description;
            d.features = k.features[i].data();
            k.descriptors.push_back(d);
        }
        return k;
    }();
    return c;
}

// --- the plug-in -------------------------------------------------------------

std::string trimmed(const char* s) {
    std::string t = s;
    const auto b = t.find_first_not_of(' ');
    const auto e = t.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

class Plugin {
public:
    Plugin(const Effect& fx, const clap_plugin_descriptor_t* desc, const clap_host_t* host)
        : fx_(fx), host_(host) {
        clap_.desc = desc;
        clap_.plugin_data = this;
        clap_.init = [](const clap_plugin* p) { return self(p)->init(); };
        clap_.destroy = [](const clap_plugin* p) { delete self(p); };
        clap_.activate = [](const clap_plugin* p, double sr, uint32_t lo, uint32_t hi) {
            return self(p)->activate(sr, lo, hi);
        };
        clap_.deactivate = [](const clap_plugin*) {};
        clap_.start_processing = [](const clap_plugin*) { return true; };
        clap_.stop_processing = [](const clap_plugin*) {};
        clap_.reset = [](const clap_plugin* p) { self(p)->autoGain_.reset(); };
        clap_.process = [](const clap_plugin* p, const clap_process_t* proc) {
            return self(p)->process(proc);
        };
        clap_.get_extension = [](const clap_plugin* p, const char* id) {
            return self(p)->extension(id);
        };
        clap_.on_main_thread = [](const clap_plugin*) {};
    }

    const clap_plugin_t* clap() const { return &clap_; }

private:
    static Plugin* self(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

    // -- lifecycle --

    bool init() {
        dsp_.reset(fx_.make());
        display_.reset(fx_.make());
        if (!dsp_ || !display_) return false;
        const int n = fx_.params;
        values_ = std::make_unique<std::atomic<double>[]>(static_cast<std::size_t>(n + 1));
        for (int i = 0; i < n; ++i) {
            char buf[64] = {};
            dsp_->getParameterName(i, buf);
            buf[sizeof(buf) - 1] = '\0';
            names_.push_back(trimmed(buf));
            const double v = static_cast<double>(dsp_->getParameter(i));
            defaults_.push_back(v);
            values_[static_cast<std::size_t>(i)].store(v);
        }
        values_[static_cast<std::size_t>(n)].store(fx_.autoGain ? 1.0 : 0.0);
        hostParams_ = static_cast<const clap_host_params_t*>(
            host_->get_extension(host_, CLAP_EXT_PARAMS));
        return true;
    }

    bool activate(double sampleRate, uint32_t, uint32_t maxFrames) {
        sampleRate_ = sampleRate;
        dsp_->setSampleRate(static_cast<float>(sampleRate));
        display_->setSampleRate(static_cast<float>(sampleRate));
        const std::size_t frames = std::max<uint32_t>(maxFrames, 1);
        dryL_.assign(frames, 0.0f);
        dryR_.assign(frames, 0.0f);
        spare_.assign(frames, 0.0f);
        autoGain_.prepare(sampleRate);
        pushAll();
        dirty_.store(false);
        return true;
    }

    // -- audio thread --

    void pushAll() {
        for (int i = 0; i < fx_.params; ++i)
            dsp_->setParameter(i, static_cast<float>(values_[static_cast<std::size_t>(i)].load()));
        autoGain_.setEnabled(values_[static_cast<std::size_t>(fx_.params)].load() >= 0.5);
    }

    void apply(const clap_event_header_t* h) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) return;
        const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
        const double v = std::clamp(ev->value, 0.0, 1.0);
        if (ev->param_id == kAutoGainParamId) {
            values_[static_cast<std::size_t>(fx_.params)].store(v >= 0.5 ? 1.0 : 0.0);
            autoGain_.setEnabled(v >= 0.5);
        } else if (ev->param_id < static_cast<clap_id>(fx_.params)) {
            values_[ev->param_id].store(v);
            dsp_->setParameter(static_cast<VstInt32>(ev->param_id), static_cast<float>(v));
        }
    }

    void run(float* const* in, float* const* out, uint32_t from, uint32_t count) {
        if (count == 0) return;
        // The effect may run in place, and auto gain needs its input as it was.
        std::memcpy(dryL_.data(), in[0] + from, count * sizeof(float));
        std::memcpy(dryR_.data(), in[1] + from, count * sizeof(float));
        float* i2[2] = {dryL_.data(), dryR_.data()};
        float* o2[2] = {out[0] + from, out[1] + from};
        dsp_->processReplacing(i2, o2, static_cast<VstInt32>(count));
        const float* d2[2] = {dryL_.data(), dryR_.data()};
        autoGain_.process(d2, o2, 2, static_cast<int>(count));
    }

    clap_process_status process(const clap_process_t* proc) {
        if (dirty_.exchange(false, std::memory_order_acquire)) pushAll();
        const uint32_t n = std::min<uint32_t>(proc->frames_count, static_cast<uint32_t>(dryL_.size()));
        if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
        const clap_audio_buffer_t& ib = proc->audio_inputs[0];
        const clap_audio_buffer_t& ob = proc->audio_outputs[0];
        if (ib.data32 == nullptr || ob.data32 == nullptr || ib.channel_count < 1 || ob.channel_count < 1)
            return CLAP_PROCESS_ERROR;
        // Stereo is the declared layout; a mono buffer is fed to both sides and
        // the right output is thrown away.
        float* in[2] = {ib.data32[0], ib.channel_count > 1 ? ib.data32[1] : ib.data32[0]};
        float* out[2] = {ob.data32[0], ob.channel_count > 1 ? ob.data32[1] : spare_.data()};

        // Split the block at every parameter change: sample-accurate.
        const clap_input_events_t* evs = proc->in_events;
        const uint32_t count = evs != nullptr ? evs->size(evs) : 0;
        uint32_t next = 0;
        uint32_t cursor = 0;
        while (cursor < n) {
            uint32_t until = n;
            while (next < count) {
                const clap_event_header_t* h = evs->get(evs, next);
                if (h->time > cursor) {
                    until = std::min(n, h->time);
                    break;
                }
                apply(h);
                ++next;
            }
            run(in, out, cursor, until - cursor);
            cursor = until;
        }
        for (; next < count; ++next) apply(evs->get(evs, next));   // at or past the end
        return CLAP_PROCESS_CONTINUE;
    }

    // -- extensions --

    const void* extension(const char* id) {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kParams;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kState;
        if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &kTail;
        return nullptr;
    }

    static uint32_t portCount(const clap_plugin*, bool) { return 1; }
    static bool portGet(const clap_plugin*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
        if (index != 0) return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = 0;
        return true;
    }
    static constexpr clap_plugin_audio_ports_t kAudioPorts{portCount, portGet};

    static uint32_t paramCount(const clap_plugin* p) {
        return static_cast<uint32_t>(self(p)->fx_.params + 1);
    }
    static bool paramInfo(const clap_plugin* p, uint32_t index, clap_param_info_t* info) {
        Plugin* s = self(p);
        if (index > static_cast<uint32_t>(s->fx_.params)) return false;
        std::memset(info, 0, sizeof(*info));
        info->min_value = 0.0;
        info->max_value = 1.0;
        if (index == static_cast<uint32_t>(s->fx_.params)) {
            info->id = kAutoGainParamId;
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
            std::snprintf(info->name, sizeof(info->name), "Auto Gain");
            info->default_value = s->fx_.autoGain ? 1.0 : 0.0;
        } else {
            info->id = index;
            info->flags = CLAP_PARAM_IS_AUTOMATABLE;
            std::snprintf(info->name, sizeof(info->name), "%s", s->names_[index].c_str());
            info->default_value = s->defaults_[index];
        }
        return true;
    }
    static int slot(const Plugin* s, clap_id id) {
        if (id == kAutoGainParamId) return s->fx_.params;
        return id < static_cast<clap_id>(s->fx_.params) ? static_cast<int>(id) : -1;
    }
    static bool paramValue(const clap_plugin* p, clap_id id, double* out) {
        Plugin* s = self(p);
        const int i = slot(s, id);
        if (i < 0) return false;
        *out = s->values_[static_cast<std::size_t>(i)].load();
        return true;
    }
    static bool paramToText(const clap_plugin* p, clap_id id, double value, char* out, uint32_t cap) {
        Plugin* s = self(p);
        const int i = slot(s, id);
        if (i < 0 || cap == 0) return false;
        if (i == s->fx_.params) {
            std::snprintf(out, cap, "%s", value >= 0.5 ? "On" : "Off");
            return true;
        }
        // The display copy, main thread only: the audio thread's instance is
        // never touched to format a number.
        s->display_->setParameter(i, static_cast<float>(value));
        char text[64] = {};
        char label[64] = {};
        s->display_->getParameterDisplay(i, text);
        s->display_->getParameterLabel(i, label);
        text[sizeof(text) - 1] = label[sizeof(label) - 1] = '\0';
        const std::string t = trimmed(text), l = trimmed(label);
        std::snprintf(out, cap, "%s%s%s", t.c_str(), l.empty() ? "" : " ", l.c_str());
        return true;
    }
    static bool paramFromText(const clap_plugin* p, clap_id id, const char* text, double* out) {
        Plugin* s = self(p);
        const int i = slot(s, id);
        if (i < 0) return false;
        if (i == s->fx_.params) {
            *out = (std::strcmp(text, "On") == 0 || std::strcmp(text, "1") == 0) ? 1.0 : 0.0;
            return true;
        }
        float v = 0.0f;
        if (s->display_->canConvertParameterTextToValue(i) &&
            s->display_->parameterTextToValue(i, text, v)) {
            *out = std::clamp(static_cast<double>(v), 0.0, 1.0);
            return true;
        }
        char* end = nullptr;
        const double d = std::strtod(text, &end);
        if (end == text) return false;
        *out = std::clamp(d, 0.0, 1.0);
        return true;
    }
    static void paramFlush(const clap_plugin* p, const clap_input_events_t* in, const clap_output_events_t*) {
        Plugin* s = self(p);
        if (s->dirty_.exchange(false, std::memory_order_acquire)) s->pushAll();
        const uint32_t n = in != nullptr ? in->size(in) : 0;
        for (uint32_t i = 0; i < n; ++i) s->apply(in->get(in, i));
    }
    static constexpr clap_plugin_params_t kParams{paramCount, paramInfo, paramValue,
                                                   paramToText, paramFromText, paramFlush};

    // State: a line naming the format, the effect, then one value per line and
    // auto gain last. Text, so a project diff shows what changed.
    static bool stateSave(const clap_plugin* p, const clap_ostream_t* os) {
        Plugin* s = self(p);
        std::string t = "ADI-AW 1\n";
        t += s->fx_.name;
        t += "\n" + std::to_string(s->fx_.params) + "\n";
        char buf[64];
        for (int i = 0; i < s->fx_.params; ++i) {
            std::snprintf(buf, sizeof(buf), "%.9g\n", s->values_[static_cast<std::size_t>(i)].load());
            t += buf;
        }
        t += s->values_[static_cast<std::size_t>(s->fx_.params)].load() >= 0.5 ? "autogain 1\n"
                                                                                : "autogain 0\n";
        const char* data = t.data();
        uint64_t left = t.size();
        while (left > 0) {
            const int64_t w = os->write(os, data, left);
            if (w <= 0) return false;
            data += w;
            left -= static_cast<uint64_t>(w);
        }
        return true;
    }
    static bool stateLoad(const clap_plugin* p, const clap_istream_t* is) {
        Plugin* s = self(p);
        std::string t;
        char buf[4096];
        for (;;) {
            const int64_t r = is->read(is, buf, sizeof(buf));
            if (r < 0) return false;
            if (r == 0) break;
            t.append(buf, static_cast<std::size_t>(r));
            if (t.size() > (1u << 16)) return false;
        }
        std::vector<std::string> lines;
        std::size_t at = 0;
        while (at < t.size()) {
            const std::size_t nl = t.find('\n', at);
            lines.push_back(t.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        if (lines.size() < 3 || lines[0] != "ADI-AW 1" || lines[1] != s->fx_.name) return false;
        const int saved = std::atoi(lines[2].c_str());
        // A newer Airwindows may have added parameters: the saved ones load,
        // the new ones keep their defaults.
        const int n = std::min(saved, s->fx_.params);
        for (int i = 0; i < n && static_cast<std::size_t>(3 + i) < lines.size(); ++i)
            s->values_[static_cast<std::size_t>(i)].store(
                std::clamp(std::strtod(lines[static_cast<std::size_t>(3 + i)].c_str(), nullptr), 0.0, 1.0));
        const std::size_t agLine = static_cast<std::size_t>(3 + std::max(saved, 0));
        if (agLine < lines.size() && lines[agLine].rfind("autogain ", 0) == 0)
            s->values_[static_cast<std::size_t>(s->fx_.params)].store(lines[agLine] == "autogain 1" ? 1.0 : 0.0);
        s->dirty_.store(true, std::memory_order_release);
        if (s->hostParams_ != nullptr) s->hostParams_->rescan(s->host_, CLAP_PARAM_RESCAN_VALUES);
        return true;
    }
    static constexpr clap_plugin_state_t kState{stateSave, stateLoad};

    // A reverb or a feedback delay rings on after its input stops; the rest
    // settle within a tenth of a second.
    static uint32_t tail(const clap_plugin* p) {
        Plugin* s = self(p);
        const std::string cat = s->fx_.category;
        if (cat == "Reverb" || cat == "Ambience" || cat == "Effects") return UINT32_MAX;
        return static_cast<uint32_t>(0.1 * s->sampleRate_);
    }
    static constexpr clap_plugin_tail_t kTail{tail};

    clap_plugin_t clap_{};
    const Effect& fx_;
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;
    std::unique_ptr<Base> dsp_;       // the audio thread's, once active
    std::unique_ptr<Base> display_;   // the main thread's, for value text
    std::vector<std::string> names_;
    std::vector<double> defaults_;
    std::unique_ptr<std::atomic<double>[]> values_;   // params, then auto gain
    std::atomic<bool> dirty_{false};                  // state loaded: push to dsp_
    adi::dsp::AutoGain autoGain_;
    std::vector<float> dryL_, dryR_, spare_;
    double sampleRate_ = 48000.0;
};

// --- factory -----------------------------------------------------------------

uint32_t factoryCount(const clap_plugin_factory_t*) { return static_cast<uint32_t>(kEffectCount); }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index < kEffectCount ? &catalogue().descriptors[index] : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* pluginId) {
    if (host == nullptr || pluginId == nullptr || !clap_version_is_compatible(host->clap_version))
        return nullptr;
    const Catalogue& c = catalogue();
    for (std::size_t i = 0; i < kEffectCount; ++i) {
        if (c.ids[i] == pluginId) {
            auto* plugin = new Plugin(kEffects[i], &c.descriptors[i], host);
            return plugin->clap();
        }
    }
    return nullptr;
}

const clap_plugin_factory_t kFactory{factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

bool entryInit(const char*) {
    // Airwindows reads the rate through getSampleRate(), which asserts on the
    // base class's default of 0 until activate() sets the real one.
    AirwinConsolidatedBase::defaultSampleRate = 48000.0f;
    catalogue();
    return true;
}

void entryDeinit() {}

const void* entryGetFactory(const char* factoryId) {
    return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace adi::airwindows
