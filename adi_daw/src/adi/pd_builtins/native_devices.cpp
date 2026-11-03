// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/native_devices.hpp"

#include "adi/dsp/auto_filter.hpp"
#include "adi/dsp/chorus_ensemble.hpp"
#include "adi/dsp/combchord.hpp"
#include "adi/dsp/echo.hpp"
#include "adi/dsp/eq_eight.hpp"
#include "adi/dsp/live_delay.hpp"
#include "adi/dsp/live_reverb.hpp"
#include "adi/dsp/multiband_dynamics.hpp"
#include "adi/dsp/overdrive.hpp"
#include "adi/dsp/phaser_flanger.hpp"
#include "adi/dsp/redux.hpp"
#include "adi/dsp/resonators.hpp"
#include "adi/dsp/saturator.hpp"
#include "adi/dsp/shifter.hpp"
#include "adi/dsp/utility.hpp"
#include "adi/dsp/vocoder.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/pd_builtins/colorcab_sample.hpp"
#include "adi/pd_builtins/sample_bridge.hpp"
#include "juce/pd_declarations.hpp"
#include "adi_native_pd_patches.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace adi::device {
namespace {
// C++ raw literals and Windows checkouts may use different newline encodings.
// Only CRLF/LF are equivalent; every other byte, including spaces and comments,
// is significant. Bare CR is not discarded.
bool sameCanvas(std::string_view left, std::string_view right) noexcept {
    std::size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (left[a] == '\r' && a + 1 < left.size() && left[a + 1] == '\n') ++a;
        if (right[b] == '\r' && b + 1 < right.size() && right[b + 1] == '\n') ++b;
        if (left[a] != right[b]) return false;
        ++a;
        ++b;
    }
    return a == left.size() && b == right.size();
}

// These overloads are the messages wired by the canonical canvases. Keep the
// one-based adi.param identity separate from each external's zero-based enum.
template <class Core> bool parameter(Core& core, int id, float value) noexcept {
    if (id < 1 || id > static_cast<int>(Core::Count)) return false;
    core.set(static_cast<typename Core::Param>(id - 1), value);
    return true;
}

bool parameter(dsp::Redux& core, int id, float value) noexcept {
    switch (id) {
    case 1: core.setRate(value); break;
    case 2: core.setJitter(value); break;
    case 3: core.setBits(value); break;
    case 4: core.setShape(value); break;
    case 5: core.setPre(value); break;
    case 6: core.setPost(value); break;
    case 7: core.setOctave(value); break;
    case 8: core.setDcShift(value); break;
    case 9: core.setMix(value); break;
    default: return false;
    }
    return true;
}

template <class Core> void midiMessage(Core&, const engine::MpeOut&) noexcept {}
void midiMessage(dsp::Shifter& core, const engine::MpeOut& message) noexcept {
    // Match pd_engine's MIDI wire quantisation and the canvas's notein/bendin
    // wiring. The patch merges channels and ignores other MIDI messages.
    switch (message.kind) {
    case engine::MpeOut::Kind::NoteOn: {
        const double value = std::isfinite(message.value)
                                 ? std::clamp(message.value, 0.0, 1.0) : 0.0;
        const auto velocity = std::max(1L, std::lround(value * 127.0));
        core.note(message.key, static_cast<double>(velocity));
        break;
    }
    case engine::MpeOut::Kind::NoteOff:
        core.note(message.key, 0);
        break;
    case engine::MpeOut::Kind::Control:
        if (message.ctrl == engine::kCtrlPitchBend)
            core.bend((static_cast<float>(message.word) - 8192.0f) / 8192.0f);
        break;
    case engine::MpeOut::Kind::Expression:
    case engine::MpeOut::Kind::PolyPressure:
        break;
    }
}

// The color-bass canvases contain two independent mono externals.
class ChordCombStereo {
  public:
    ChordCombStereo() noexcept {
        constexpr dsp::CombChord::Chord first{45, 48, 52, 57, 60, 64};
        for (std::size_t slot = 0; slot < dsp::CombChord::states; ++slot) {
            auto chord = first;
            for (auto& note : chord) note += static_cast<double>(slot);
            for (auto& core : cores_) core.setChord(slot, chord);
        }
    }
    void prepare(double rate) { for (auto& core : cores_) core.prepare(rate); }
    bool parameter(int id, float value) noexcept {
        if (id < 1 || id > 6) return false;
        for (auto& core : cores_) {
            switch (id) {
            case 1:
                core.setState(static_cast<std::size_t>(std::clamp(value, 0.0f, 7.0f)));
                break;
            case 2:
                core.setMode(value >= 0.5f ? dsp::CombChord::Mode::Square
                                          : dsp::CombChord::Mode::Saw);
                break;
            case 3: core.setDecay(value); break;
            case 4: core.setColor(value); break;
            case 5: core.setMix(value); break;
            case 6: core.setOutputDb(value); break;
            default: break;
            }
        }
        return true;
    }
    void process(const float* left, const float* right, float* outLeft, float* outRight,
                 std::size_t frames) noexcept {
        cores_[0].process(left, outLeft, frames);
        cores_[1].process(right, outRight, frames);
    }
  private:
    std::array<dsp::CombChord, 2> cores_;
};
bool parameter(ChordCombStereo& core, int id, float value) noexcept {
    return core.parameter(id, value);
}

class ColorCabStereo {
  public:
    void prepare(double rate) noexcept {
        rate_ = rate;
        sequence_ = 0;
        untilRetry_ = 0;
        for (auto& core : cores_) core.prepare(rate);
    }
    bool parameter(int id, float value) noexcept {
        if (id != 2) return false;
        for (auto& core : cores_) core.setMix(value);
        return true;
    }
    void process(const float* left, const float* right, float* outLeft, float* outRight,
                 std::size_t frames) noexcept {
        // LibPdEngine establishes the same per-segment sample acquisition as
        // the Pd external. No publication, ownership change or design occurs
        // here. Both channels advance together, so their fades also agree.
        const auto* sample = dynamic_cast<const PdColorCabSample*>(PdSampleBlock::get(1));
        std::size_t done = 0;
        while (done < frames) {
            if (untilRetry_ == 0) {
                if (sample && sample->seq != sequence_ && sample->design.sampleRate == rate_ &&
                    !cores_[0].fading() && !cores_[1].fading()) {
                    const bool leftSet = cores_[0].setKernel(sample->design.kernel);
                    const bool rightSet = cores_[1].setKernel(sample->design.kernel);
                    if (leftSet && rightSet) sequence_ = sample->seq;
                }
                // The external retries a pending kernel every 64 samples.
                // Retain that control cadence across arbitrary host segments;
                // audio runs immediately and is never buffered or delayed.
                untilRetry_ = 64;
            }
            const auto count = std::min(untilRetry_, frames - done);
            cores_[0].process(left + done, outLeft + done, count);
            cores_[1].process(right + done, outRight + done, count);
            done += count;
            untilRetry_ -= count;
        }
    }
  private:
    std::array<dsp::ColorCab, 2> cores_;
    std::uint64_t sequence_ = 0;
    std::size_t untilRetry_ = 0;
    double rate_ = 0;
};
bool parameter(ColorCabStereo& core, int id, float value) noexcept {
    return core.parameter(id, value);
}

template <class Core> class NativeDevice final : public NativePdDevice {
  public:
    explicit NativeDevice(int inputs) : inputs_(inputs) {}
    void prepare(double sampleRate, int maxFrames) override {
        capacity_ = static_cast<std::size_t>(std::max(1, maxFrames));
        for (auto& buffer : buffers_) buffer.assign(capacity_, 0.0f);
        // libpd_init_audio takes an integer rate, and a DSP signal then holds
        // that rate in t_float. Preserve both conversions. Re-preparing an
        // already running Pd canvas at the same rate does not rebuild its DSP
        // graph: changing the host maximum must not erase a native tail either.
        const bool validRate = std::isfinite(sampleRate) && sampleRate > 0 &&
            sampleRate <= static_cast<double>(std::numeric_limits<int>::max());
        const auto rate = static_cast<float>(validRate ? static_cast<int>(sampleRate) : 44100);
        if (!prepared_ || rate != preparedRate_) {
            core_.prepare(rate);
            preparedRate_ = rate;
            prepared_ = true;
        }
    }
    bool sendParameter(int id, float value) noexcept override {
        return std::isfinite(value) && parameter(core_, id, value);
    }
    int latencySamples() const noexcept override {
        if constexpr (requires { core_.latency(); }) {
            return core_.latency();
        } else {
            return 0;
        }
    }
    void tempo(double bpm) noexcept override {
        if constexpr (requires { core_.tempo(bpm); }) core_.tempo(static_cast<float>(bpm));
    }
    void midi(const engine::MpeOut& message) noexcept override { midiMessage(core_, message); }
    void process(const engine::NodeIo& io) noexcept override {
        if (io.frames <= 0 || io.blockOffset < 0 || capacity_ == 0) return;
        const auto frames = static_cast<std::size_t>(io.frames);
        const auto offset = static_cast<std::size_t>(io.blockOffset);
        const int channels = std::max(0, io.channels);
        for (std::size_t done = 0; done < frames;) {
            const auto count = std::min(capacity_, frames - done);
            // Pack first, then scatter after processing: missing channels are
            // silence and aliased input/output planes cannot clobber a source.
            // The existing Pd contract maps adc channels beyond io.channels
            // to the sidechain bus (which has io.channels planes too).
            for (int channel = 0; channel < inputs_; ++channel) {
                const float* source = nullptr;
                if (channel < channels) {
                    if (io.in) source = io.in[channel];
                } else if (io.sidechain && channel - channels < channels) {
                    source = io.sidechain[channel - channels];
                }
                auto* target = buffers_[static_cast<std::size_t>(channel)].data();
                if (source) std::copy_n(source + offset + done, count, target);
                else std::fill_n(target, count, 0.0f);
            }
            if constexpr (requires { core_.process(buffers_[0].data(), buffers_[1].data(),
                                                   buffers_[4].data(), buffers_[5].data(),
                                                   count, buffers_[2].data(), buffers_[3].data()); }) {
                core_.process(buffers_[0].data(), buffers_[1].data(), buffers_[4].data(),
                              buffers_[5].data(), count, buffers_[2].data(), buffers_[3].data());
            } else {
                core_.process(buffers_[0].data(), buffers_[1].data(), buffers_[4].data(),
                              buffers_[5].data(), count);
            }
            if (io.out) {
                for (int channel = 0; channel < channels; ++channel) {
                    auto* target = io.out[channel];
                    if (!target) continue;
                    if (channel < 2)
                        std::copy_n(buffers_[static_cast<std::size_t>(channel) + 4].data(),
                                    count, target + offset + done);
                    else std::fill_n(target + offset + done, count, 0.0f);
                }
            }
            done += count;
        }
    }
  private:
    Core core_;
    int inputs_;
    std::size_t capacity_ = 0;
    float preparedRate_ = 0;
    bool prepared_ = false;
    std::array<std::vector<float>, 6> buffers_;
};

template <class Core> std::unique_ptr<NativePdDevice> create(std::string_view text, int inputs) {
    auto device = std::make_unique<NativeDevice<Core>>(inputs);
    // adi.param emits its default on loadbang, before the external's DSP
    // prepare. Round through float exactly as Pd does, rather than relying
    // on a core's sometimes-higher-precision C++ defaults.
    const auto declarations = parsePdDeclarations(text);
    if (!declarations.arrays.empty() || !declarations.problems.empty()) return nullptr;
    for (const auto& declaration : declarations.params)
        if (!device->sendParameter(declaration.id, static_cast<float>(declaration.def)))
            return nullptr;
    return device;
}
} // namespace

std::unique_ptr<NativePdDevice> makeNativePdDevice(std::string_view patchText, int inChannels,
                                                  int outChannels) {
    if (outChannels != 2) return nullptr;
    if (inChannels == 4) {
        if (sameCanvas(patchText, canonical::autoFilter)) return create<dsp::AutoFilter>(patchText, 4);
        if (sameCanvas(patchText, canonical::multibandDynamics))
            return create<dsp::MultibandDynamics>(patchText, 4);
        if (sameCanvas(patchText, canonical::vocoder)) return create<dsp::Vocoder>(patchText, 4);
    } else if (inChannels == 2) {
        if (sameCanvas(patchText, canonical::chordComb)) return create<ChordCombStereo>(patchText, 2);
        if (sameCanvas(patchText, canonical::chorusEnsemble)) return create<dsp::ChorusEnsemble>(patchText, 2);
        if (sameCanvas(patchText, canonical::colorCab)) return create<ColorCabStereo>(patchText, 2);
        if (sameCanvas(patchText, canonical::delay)) return create<dsp::LiveDelay>(patchText, 2);
        if (sameCanvas(patchText, canonical::eqEight)) return create<dsp::EqEight>(patchText, 2);
        if (sameCanvas(patchText, canonical::echo)) return create<dsp::Echo>(patchText, 2);
        if (sameCanvas(patchText, canonical::overdrive)) return create<dsp::Overdrive>(patchText, 2);
        if (sameCanvas(patchText, canonical::phaserFlanger)) return create<dsp::PhaserFlanger>(patchText, 2);
        if (sameCanvas(patchText, canonical::redux)) return create<dsp::Redux>(patchText, 2);
        if (sameCanvas(patchText, canonical::resonators)) return create<dsp::Resonators>(patchText, 2);
        if (sameCanvas(patchText, canonical::reverb)) return create<dsp::LiveReverb>(patchText, 2);
        if (sameCanvas(patchText, canonical::saturator)) return create<dsp::Saturator>(patchText, 2);
        if (sameCanvas(patchText, canonical::shifter)) return create<dsp::Shifter>(patchText, 2);
        if (sameCanvas(patchText, canonical::utility)) return create<dsp::Utility>(patchText, 2);
    }
    return nullptr;
}

bool nativePdAbstractionMatches(std::string_view filename, std::string_view text) noexcept {
    if (filename == "adi.param.pd") return sameCanvas(text, canonical::paramAbstraction);
    if (filename == "adi.transport.pd") return sameCanvas(text, canonical::transportAbstraction);
    if (filename == "adi.sample.pd") return sameCanvas(text, canonical::sampleAbstraction);
    return false;
}
} // namespace adi::device
