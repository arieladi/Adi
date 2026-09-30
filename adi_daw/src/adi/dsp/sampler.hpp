// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oneshot.hpp"
namespace adi::device {
class Sampler final : public DeviceInstance {
  public:
    struct Range {
        double low = 0, high = 127, fadeLow = 0, fadeHigh = 0;
    };
    struct Zone {
        Range key{}, velocity{}, select{};
        int root = 60, channels = 1;
        double sampleRate = 48000, start = 0, end = 1, loopStart = 0, crossfade = 0;
        double gainDb = 0, pan = 0, detune = 0;
        bool loop = false, constantPower = true;
        std::vector<float> samples;
    };
    Sampler();
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    engine::EventFlow eventFlow() const noexcept override { return engine::EventFlow::Consume; }
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    void pumpMainThread() override { publisher_.collect(); }
    bool publishZones(std::span<const Zone>);
    std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;

  private:
    struct Layer {
        Zone zone;
        std::unique_ptr<OneShot> engine;
    };
    struct Bank : engine::Sequenced {
        std::vector<Layer> layers;
    };
    static bool valid(const Zone &) noexcept;
    static double weight(const Range &, double, bool) noexcept;
    void configure(Layer &);
    void publish(std::unique_ptr<Bank>);
    std::vector<ParamDescriptor> params_;
    std::vector<std::uint32_t> destinations_;
    std::array<std::atomic<double>, OneShot::Count> values_{};
    std::array<double, OneShot::Count> modulation_{};
    engine::SnapshotPublisher<Bank> publisher_;
    Bank *messageBank_ = nullptr; // publisher owns it; message thread only
    std::vector<engine::Event> events_;
    std::vector<float> scratch_;
    double rate_ = 48000;
    int maxFrames_ = 4096;
    DeviceIdentity identity_{"internal", "adi.sampler", "Sampler", "ADI", "1"};
};
} // namespace adi::device
