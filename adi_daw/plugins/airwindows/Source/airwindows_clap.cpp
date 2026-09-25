// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows (ADR-0166, ADR-0171): the kept Airwindows algorithms as eight
// suite CLAP plug-ins, "ADI Airwindows - Distortion" and so on. A suite holds
// every algorithm of its group; which one plays is a parameter.
//
// THREE RULES (the director's, ADR-0171), each against the obvious design:
//
//   1. **Every parameter of every algorithm exists from init, with an id that
//      never changes.** Algorithm a's parameter k is 100 + 64a + k, in a CLAP
//      module named after the algorithm. Choosing another algorithm changes
//      no parameter's meaning and never asks the host to rescan the list, so an
//      automation lane (ADR-0165) or a stored value (ADR-0142) stays bound to
//      what it was written for. The obvious design -- one set of generic slots
//      whose names change with the algorithm -- is Consolidated's, and it is
//      exactly what breaks automation.
//
//   2. **A switch is a 5 ms crossfade on the audio thread.** For those 5 ms
//      the outgoing and incoming algorithms both run on the same input and
//      the output moves linearly from one to the other: no click, no thump.
//      Every algorithm is instantiated at init, so a switch allocates nothing.
//      An algorithm that is not playing is not processed; its state waits,
//      and when it is chosen again the crossfade covers whatever tail it held.
//
//   3. **Auto gain follows whatever plays** (dsp::AutoGain, BS.1770 K-weighted),
//      after the crossfade, so a switch between a quiet and a loud algorithm
//      is levelled like any other change.
//
// The DSP is Airwindows' own, unchanged, from the airwin2rack registry (MIT).
// The editor is mac's (ADR-0171); until then a host draws the parameters.

#include "airwindows_clap.hpp"

#include "adi/dsp/auto_gain.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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

struct Algo {
    int suite;
    const char* name;
    const char* category;
    const char* description;
    bool autoGain;
    int params;
    Base* (*make)();
};

#define ADI_AW_ROWS 1
#define ADI_AW_ROW(suite, sym, name, category, description, autoGain)                        \
    {suite, name, category, description, autoGain, airwinconsolidated::sym::kNumParameters, \
     +[]() -> Base* { return new airwinconsolidated::sym::sym(0); }},
const Algo kAlgos[] = {
#include "aw_catalogue.inc"
};
#undef ADI_AW_ROW
#undef ADI_AW_ROWS

struct SuiteDef {
    int index;
    const char* key;
    const char* name;
};
#define ADI_AW_ROWS 1
#define ADI_AW_ROW(...)
#define ADI_AW_SUITE(index, key, name) {index, key, name},
const SuiteDef kSuiteDefs[] = {
#include "aw_catalogue.inc"
};
#undef ADI_AW_SUITE
#undef ADI_AW_ROW
#undef ADI_AW_ROWS

// --- the catalogue: the suites this binary carries ------------------------------

struct Suite {
    const SuiteDef* def;
    std::vector<const Algo*> algos;
    std::string id;
    std::vector<const char*> features;
    clap_plugin_descriptor_t descriptor{};
    bool autoGainDefault = false;
    bool rings = false;          // a reverb or a feedback effect: its tail is long
};

std::vector<const char*> featuresFor(const std::string& key) {
    std::vector<const char*> f{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT};
    if (key == "distortion" || key == "tape" || key == "ampsims") f.push_back(CLAP_PLUGIN_FEATURE_DISTORTION);
    else if (key == "consoles") f.push_back(CLAP_PLUGIN_FEATURE_MIXING);
    else if (key == "reverb") f.push_back(CLAP_PLUGIN_FEATURE_REVERB);
    else if (key == "lofimod") {
        f.push_back(CLAP_PLUGIN_FEATURE_DISTORTION);
        f.push_back(CLAP_PLUGIN_FEATURE_MULTI_EFFECTS);
    } else if (key == "noisedyn") {
        f.push_back(CLAP_PLUGIN_FEATURE_RESTORATION);
        f.push_back(CLAP_PLUGIN_FEATURE_GATE);
    } else f.push_back(CLAP_PLUGIN_FEATURE_MULTI_EFFECTS);
    f.push_back(CLAP_PLUGIN_FEATURE_STEREO);
    f.push_back(nullptr);
    return f;
}

std::vector<Suite>& suites() {
    static std::vector<Suite> all = [] {
        std::vector<Suite> v;
        for (const SuiteDef& d : kSuiteDefs) {
            Suite s;
            s.def = &d;
            for (const Algo& a : kAlgos)
                if (a.suite == d.index) s.algos.push_back(&a);
            if (s.algos.empty()) continue;     // a module built for another suite
            s.id = std::string("com.adi.airwindows.") + d.key;
            s.features = featuresFor(d.key);
            std::size_t on = 0;
            for (const Algo* a : s.algos) {
                on += a->autoGain ? 1u : 0u;
                const std::string c = a->category;
                s.rings = s.rings || c == "Reverb" || c == "Ambience" || c == "Effects";
            }
            s.autoGainDefault = 2 * on > s.algos.size();
            v.push_back(std::move(s));
        }
        for (Suite& s : v) {
            clap_plugin_descriptor_t& d = s.descriptor;
            d.clap_version = CLAP_VERSION_INIT;
            d.id = s.id.c_str();
            d.name = s.def->name;
            d.vendor = "Airwindows";
            d.url = "https://www.airwindows.com/";
            d.manual_url = "https://www.airwindows.com/";
            d.support_url = "https://github.com/arieladi/Adi";
            d.version = "0.2.0";
            d.description = "Airwindows algorithms of one kind, one chosen inside (ADI build)";
            d.features = s.features.data();
        }
        return v;
    }();
    return all;
}

std::string trimmed(const char* s) {
    std::string t = s;
    const auto b = t.find_first_not_of(' ');
    const auto e = t.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

// --- the plug-in -------------------------------------------------------------

struct ParamSlot {
    clap_id id;
    int algo;        // -1 for the suite's own two parameters
    int index;       // the algorithm's parameter index
    std::string name;
    double defaultValue;
};

class Plugin {
public:
    Plugin(const Suite& suite, const clap_host_t* host) : suite_(suite), host_(host) {
        clap_.desc = &suite.descriptor;
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
    int algoCount() const { return static_cast<int>(suite_.algos.size()); }

    // -- lifecycle (main thread) --

    bool init() {
        slots_.push_back({kAlgorithmParamId, -1, 0, "Algorithm", 0.0});
        slots_.push_back({kAutoGainParamId, -1, 1, "Auto Gain", suite_.autoGainDefault ? 1.0 : 0.0});
        for (int a = 0; a < algoCount(); ++a) {
            if (suite_.algos[static_cast<std::size_t>(a)]->params > static_cast<int>(kAlgoParamStride))
                return false;
            firstSlot_.push_back(static_cast<int>(slots_.size()));
            std::unique_ptr<Base> fx(suite_.algos[static_cast<std::size_t>(a)]->make());
            if (!fx) return false;
            for (int k = 0; k < suite_.algos[static_cast<std::size_t>(a)]->params; ++k) {
                char buf[64] = {};
                fx->getParameterName(k, buf);
                buf[sizeof(buf) - 1] = '\0';
                slots_.push_back({kAlgoParamBase + static_cast<clap_id>(a) * kAlgoParamStride +
                                      static_cast<clap_id>(k),
                                  a, k, trimmed(buf), static_cast<double>(fx->getParameter(k))});
            }
            dsp_.push_back(std::move(fx));
            display_.emplace_back();      // made on first use, main thread
        }
        values_ = std::make_unique<std::atomic<double>[]>(slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i) values_[i].store(slots_[i].defaultValue);
        hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
        return true;
    }

    bool activate(double sampleRate, uint32_t, uint32_t maxFrames) {
        sampleRate_ = sampleRate;
        for (auto& fx : dsp_) fx->setSampleRate(static_cast<float>(sampleRate));
        const std::size_t frames = std::max<uint32_t>(maxFrames, 1);
        dryL_.assign(frames, 0.0f);
        dryR_.assign(frames, 0.0f);
        oldL_.assign(frames, 0.0f);
        oldR_.assign(frames, 0.0f);
        spare_.assign(frames, 0.0f);
        fadeLen_ = std::max(1, static_cast<int>(std::lround(kCrossfadeSeconds * sampleRate)));
        autoGain_.prepare(sampleRate);
        pushAll();
        dirty_.store(false);
        return true;
    }

    // -- slots --

    int slotOf(clap_id id) const {
        if (id == kAlgorithmParamId) return 0;
        if (id == kAutoGainParamId) return 1;
        if (id < kAlgoParamBase) return -1;
        const clap_id a = (id - kAlgoParamBase) / kAlgoParamStride;
        const clap_id k = (id - kAlgoParamBase) % kAlgoParamStride;
        if (a >= static_cast<clap_id>(algoCount())) return -1;
        // Slots are laid out algorithm by algorithm: its first slot plus k.
        if (static_cast<int>(k) >= suite_.algos[a]->params) return -1;
        return firstSlot_[a] + static_cast<int>(k);
    }

    int algoFromValue(double v) const {
        const long a = std::lround(v);
        return static_cast<int>(std::clamp<long>(a, 0, algoCount() - 1));
    }

    // -- audio thread --

    void pushAll() {
        for (std::size_t i = 2; i < slots_.size(); ++i)
            dsp_[static_cast<std::size_t>(slots_[i].algo)]->setParameter(
                slots_[i].index, static_cast<float>(values_[i].load()));
        active_ = algoFromValue(values_[0].load());
        fadeLeft_ = 0;
        autoGain_.setEnabled(values_[1].load() >= 0.5);
    }

    /// Inside process() a switch crossfades. Outside it -- a flush while the
    /// host is not running audio -- there is no signal to fade from, and the
    /// switch is immediate, so the next block starts clean on the new algorithm.
    void choose(int algo, bool fade) {
        if (algo == active_) return;
        fadeFrom_ = active_;     // a switch during a fade starts from what is loudest now
        active_ = algo;
        fadeLeft_ = fade ? fadeLen_ : 0;
    }

    void apply(const clap_event_header_t* h, bool inProcess) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) return;
        const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
        const int s = slotOf(ev->param_id);
        if (s < 0) return;
        if (s == 0) {
            const int a = algoFromValue(ev->value);
            values_[0].store(static_cast<double>(a));
            choose(a, inProcess);
        } else if (s == 1) {
            values_[1].store(ev->value >= 0.5 ? 1.0 : 0.0);
            autoGain_.setEnabled(ev->value >= 0.5);
        } else {
            const double v = std::clamp(ev->value, 0.0, 1.0);
            values_[static_cast<std::size_t>(s)].store(v);
            const ParamSlot& slot = slots_[static_cast<std::size_t>(s)];
            dsp_[static_cast<std::size_t>(slot.algo)]->setParameter(slot.index, static_cast<float>(v));
        }
    }

    void run(float* const* in, float* const* out, uint32_t from, uint32_t count) {
        if (count == 0) return;
        // The algorithm may run in place, the outgoing one needs the same
        // input, and auto gain needs it as it was: one dry copy serves all three.
        std::memcpy(dryL_.data(), in[0] + from, count * sizeof(float));
        std::memcpy(dryR_.data(), in[1] + from, count * sizeof(float));
        float* dry[2] = {dryL_.data(), dryR_.data()};
        float* o2[2] = {out[0] + from, out[1] + from};
        dsp_[static_cast<std::size_t>(active_)]->processReplacing(dry, o2, static_cast<VstInt32>(count));
        if (fadeLeft_ > 0) {
            const uint32_t n = std::min<uint32_t>(count, static_cast<uint32_t>(fadeLeft_));
            float* old[2] = {oldL_.data(), oldR_.data()};
            dsp_[static_cast<std::size_t>(fadeFrom_)]->processReplacing(dry, old, static_cast<VstInt32>(n));
            for (uint32_t i = 0; i < n; ++i) {
                // Weight of the incoming algorithm, rising to 1 over the fade.
                const float g = 1.0f - static_cast<float>(fadeLeft_) / static_cast<float>(fadeLen_ + 1);
                o2[0][i] = oldL_[i] + (o2[0][i] - oldL_[i]) * g;
                o2[1][i] = oldR_[i] + (o2[1][i] - oldR_[i]) * g;
                --fadeLeft_;
            }
        }
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
                apply(h, true);
                ++next;
            }
            run(in, out, cursor, until - cursor);
            cursor = until;
        }
        for (; next < count; ++next) apply(evs->get(evs, next), true);
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

    static uint32_t paramCount(const clap_plugin* p) { return static_cast<uint32_t>(self(p)->slots_.size()); }
    static bool paramInfo(const clap_plugin* p, uint32_t index, clap_param_info_t* info) {
        Plugin* s = self(p);
        if (index >= s->slots_.size()) return false;
        const ParamSlot& slot = s->slots_[index];
        std::memset(info, 0, sizeof(*info));
        info->id = slot.id;
        std::snprintf(info->name, sizeof(info->name), "%s", slot.name.c_str());
        info->default_value = slot.defaultValue;
        info->min_value = 0.0;
        if (index == 0) {
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
            info->max_value = static_cast<double>(s->algoCount() - 1);
        } else if (index == 1) {
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
            info->max_value = 1.0;
        } else {
            info->flags = CLAP_PARAM_IS_AUTOMATABLE;
            info->max_value = 1.0;
            std::snprintf(info->module, sizeof(info->module), "%s",
                          s->suite_.algos[static_cast<std::size_t>(slot.algo)]->name);
        }
        return true;
    }
    static bool paramValue(const clap_plugin* p, clap_id id, double* out) {
        Plugin* s = self(p);
        const int i = s->slotOf(id);
        if (i < 0) return false;
        *out = s->values_[static_cast<std::size_t>(i)].load();
        return true;
    }
    static bool paramToText(const clap_plugin* p, clap_id id, double value, char* out, uint32_t cap) {
        Plugin* s = self(p);
        const int i = s->slotOf(id);
        if (i < 0 || cap == 0) return false;
        if (i == 0) {
            std::snprintf(out, cap, "%s", s->suite_.algos[static_cast<std::size_t>(s->algoFromValue(value))]->name);
            return true;
        }
        if (i == 1) {
            std::snprintf(out, cap, "%s", value >= 0.5 ? "On" : "Off");
            return true;
        }
        // A display copy per algorithm, made on first use and main thread only:
        // the audio thread's instances are never touched to format a number.
        const ParamSlot& slot = s->slots_[static_cast<std::size_t>(i)];
        auto& shown = s->display_[static_cast<std::size_t>(slot.algo)];
        if (!shown) {
            shown.reset(s->suite_.algos[static_cast<std::size_t>(slot.algo)]->make());
            shown->setSampleRate(static_cast<float>(s->sampleRate_));
        }
        shown->setParameter(slot.index, static_cast<float>(value));
        char text[64] = {};
        char label[64] = {};
        shown->getParameterDisplay(slot.index, text);
        shown->getParameterLabel(slot.index, label);
        text[sizeof(text) - 1] = label[sizeof(label) - 1] = '\0';
        const std::string t = trimmed(text), l = trimmed(label);
        std::snprintf(out, cap, "%s%s%s", t.c_str(), l.empty() ? "" : " ", l.c_str());
        return true;
    }
    static bool paramFromText(const clap_plugin* p, clap_id id, const char* text, double* out) {
        Plugin* s = self(p);
        const int i = s->slotOf(id);
        if (i < 0) return false;
        if (i == 0) {
            for (int a = 0; a < s->algoCount(); ++a)
                if (std::strcmp(text, s->suite_.algos[static_cast<std::size_t>(a)]->name) == 0) {
                    *out = static_cast<double>(a);
                    return true;
                }
            return false;
        }
        if (i == 1) {
            *out = (std::strcmp(text, "On") == 0 || std::strcmp(text, "1") == 0) ? 1.0 : 0.0;
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
        for (uint32_t i = 0; i < n; ++i) s->apply(in->get(in, i), false);
    }
    static constexpr clap_plugin_params_t kParams{paramCount, paramInfo, paramValue,
                                                   paramToText, paramFromText, paramFlush};

    // State: text, keyed by names, so a suite that gains an algorithm or an
    // algorithm that gains a parameter still loads what it had.
    static bool stateSave(const clap_plugin* p, const clap_ostream_t* os) {
        Plugin* s = self(p);
        std::string t = "ADI-AWS 1\nsuite ";
        t += s->suite_.def->key;
        t += "\nalgorithm ";
        t += s->suite_.algos[static_cast<std::size_t>(s->algoFromValue(s->values_[0].load()))]->name;
        t += s->values_[1].load() >= 0.5 ? "\nautogain 1\n" : "\nautogain 0\n";
        char buf[160];
        for (std::size_t i = 2; i < s->slots_.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "p %s %d %.9g\n",
                          s->suite_.algos[static_cast<std::size_t>(s->slots_[i].algo)]->name,
                          s->slots_[i].index, s->values_[i].load());
            t += buf;
        }
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
            if (t.size() > (1u << 20)) return false;
        }
        std::vector<std::string> lines;
        std::size_t at = 0;
        while (at < t.size()) {
            const std::size_t nl = t.find('\n', at);
            lines.push_back(t.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        if (lines.size() < 2 || lines[0] != "ADI-AWS 1" || lines[1] != std::string("suite ") + s->suite_.def->key)
            return false;
        for (std::size_t li = 2; li < lines.size(); ++li) {
            const std::string& l = lines[li];
            if (l.rfind("algorithm ", 0) == 0) {
                const std::string name = l.substr(10);
                for (int a = 0; a < s->algoCount(); ++a)
                    if (name == s->suite_.algos[static_cast<std::size_t>(a)]->name)
                        s->values_[0].store(static_cast<double>(a));
            } else if (l.rfind("autogain ", 0) == 0) {
                s->values_[1].store(l == "autogain 1" ? 1.0 : 0.0);
            } else if (l.rfind("p ", 0) == 0) {
                // "p <algorithm> <index> <value>"
                const std::size_t a1 = l.find(' ', 2);
                if (a1 == std::string::npos) continue;
                const std::string name = l.substr(2, a1 - 2);
                char* end = nullptr;
                const long kl = std::strtol(l.c_str() + a1 + 1, &end, 10);
                if (end == l.c_str() + a1 + 1) continue;
                const int k = static_cast<int>(kl);
                const double v = std::strtod(end, nullptr);
                for (std::size_t i = 2; i < s->slots_.size(); ++i)
                    if (s->slots_[i].index == k &&
                        name == s->suite_.algos[static_cast<std::size_t>(s->slots_[i].algo)]->name)
                        s->values_[i].store(std::clamp(v, 0.0, 1.0));
            }
        }
        s->dirty_.store(true, std::memory_order_release);
        // Values only: the parameter list itself never changes (ADR-0171).
        if (s->hostParams_ != nullptr) s->hostParams_->rescan(s->host_, CLAP_PARAM_RESCAN_VALUES);
        return true;
    }
    static constexpr clap_plugin_state_t kState{stateSave, stateLoad};

    static uint32_t tail(const clap_plugin* p) {
        Plugin* s = self(p);
        return s->suite_.rings ? UINT32_MAX : static_cast<uint32_t>(0.1 * s->sampleRate_);
    }
    static constexpr clap_plugin_tail_t kTail{tail};

    clap_plugin_t clap_{};
    const Suite& suite_;
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;
    std::vector<std::unique_ptr<Base>> dsp_;       // one per algorithm, the audio thread's once active
    std::vector<std::unique_ptr<Base>> display_;   // the main thread's, for value text
    std::vector<ParamSlot> slots_;
    std::vector<int> firstSlot_;                   // per algorithm, into slots_
    std::unique_ptr<std::atomic<double>[]> values_;
    std::atomic<bool> dirty_{false};               // state loaded: push to dsp_
    adi::dsp::AutoGain autoGain_;
    std::vector<float> dryL_, dryR_, oldL_, oldR_, spare_;
    int active_ = 0;
    int fadeFrom_ = 0;
    int fadeLeft_ = 0;
    int fadeLen_ = 240;
    double sampleRate_ = 48000.0;
};

// --- factory -----------------------------------------------------------------

uint32_t factoryCount(const clap_plugin_factory_t*) { return static_cast<uint32_t>(suites().size()); }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index < suites().size() ? &suites()[index].descriptor : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* pluginId) {
    if (host == nullptr || pluginId == nullptr || !clap_version_is_compatible(host->clap_version))
        return nullptr;
    for (const Suite& s : suites())
        if (s.id == pluginId) return (new Plugin(s, host))->clap();
    return nullptr;
}

const clap_plugin_factory_t kFactory{factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

bool entryInit(const char*) {
    // Airwindows reads the rate through getSampleRate(), which asserts on the
    // base class's default of 0 until activate() sets the real one.
    AirwinConsolidatedBase::defaultSampleRate = 48000.0f;
    suites();
    return true;
}

void entryDeinit() {}

const void* entryGetFactory(const char* factoryId) {
    return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace adi::airwindows
