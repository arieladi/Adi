// SPDX-License-Identifier: GPL-3.0-or-later
// Original ADI sampler. Behaviour checklist: collab/win_codex/2026-09-30-oneshot.md.
#pragma once
#include "adi/engine/publisher.hpp"
#include "juce/device_model.hpp"
#include <filesystem>
#include <span>
namespace adi::device {
class OneShot final : public DeviceInstance {
  public:
    enum Param {
        Mode,
        Start,
        End,
        Length,
        LoopOn,
        LoopLength,
        Fade,
        Snap,
        Voices,
        Retrigger,
        Gate,
        SliceBy,
        Regions,
        BeatDivision,
        SlicePlayback,
        SampleBpm,
        Gain,
        Volume,
        Transpose,
        Detune,
        Attack,
        Decay,
        Sustain,
        Release,
        FadeIn,
        FadeOut,
        FilterOn,
        FilterType,
        Slope,
        Cutoff,
        Resonance,
        LfoOn,
        LfoShape,
        LfoRate,
        LfoSync,
        LfoBeat,
        LfoPitch,
        LfoVolume,
        LfoPan,
        LfoFilter,
        Count
    };
    OneShot();
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    engine::EventFlow eventFlow() const noexcept override { return engine::EventFlow::Consume; }
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    void pumpMainThread() override;
    std::int32_t paramCount() const noexcept override { return Count; }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    std::string paramText(const std::string &, double) const override;
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;
    // Message thread. Copies/validates/analyses before the immutable publication.
    bool publishSample(std::span<const float> interleaved, int channels, double rate,
                       std::span<const std::size_t> manualSlices = {});
    bool loadSample(const std::filesystem::path &, std::string &error);

  private:
    struct Sample {
        std::vector<float> data;
        int channels = 1;
        double rate = 48000;
        std::vector<std::size_t> transients, manual, zeros;
        std::size_t frames() const noexcept {
            return data.size() / static_cast<std::size_t>(channels);
        }
    };
    struct Publication : engine::Sequenced {
        std::shared_ptr<const Sample> sample;
    };
    struct Filter {
        double a = 0, b = 0;
        double run(double, double, double, int) noexcept;
    };
    struct Voice {
        bool active = false, held = false;
        std::uint64_t id = 0, age = 0;
        int key = 60, stage = 0;
        double position = 0, begin = 0, end = 0, loop = 0, env = 0, releaseStep = 0, velocity = 1,
               phase = 0, random = 0, tuning = 0;
        std::uint32_t rng = 1;
        std::uint64_t played = 0;
        Filter filters[2][2];
    };
    void event(const engine::Event &, const Sample *, std::array<double, Count> &) noexcept;
    void noteOn(const engine::Event &, const Sample &, const std::array<double, Count> &) noexcept;
    double read(const Sample &, double, int) const noexcept;
    std::array<ParamDescriptor, Count> params_;
    std::array<std::atomic<double>, Count> values_{};
    std::array<Voice, 32> voices_{};
    std::array<double,Count> modulation_{};
    engine::SnapshotPublisher<Publication> publisher_;
    std::shared_ptr<const Sample> sample_; // message thread only, including saveState
    std::uint64_t sampleSeq_ = 0, age_ = 0;
    double rate_ = 48000;
    DeviceIdentity identity_{"internal", "adi.oneshot", "OneShot", "ADI", "1"};
};
} // namespace adi::device
