// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/publisher.hpp"
#include "juce/device_model.hpp"
#include <span>
namespace adi::device {
class MidiModulator final : public DeviceInstance {
  public:
    enum class Kind { Expression, Shaper, MPE };
    struct Mapping {
        std::int64_t device = 0;
        std::uint32_t parameter = 0;
        bool modulation = true, bipolar = false;
        double min = 0, max = 1, depth = 1;
    };
    struct Point {
        double time = 0, value = 0, curve = 0;
        bool sustain = false;
    };
    explicit MidiModulator(Kind);
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool transformsEvents() const noexcept override { return true; }
    void refreshMappedOutput() noexcept override;
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    void transformEvents(engine::EventSpan, engine::EventList &, std::int32_t, double,
                         const engine::TransportInfo *) noexcept override;
    std::vector<std::int64_t> eventTargets() const override;
    bool setMappings(std::span<const Mapping>);
    bool setEnvelope(std::span<const Point>);
    std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;
    void pumpMainThread() override { publisher_.collect(); }

  private:
    struct Config : engine::Sequenced {
        std::array<Mapping, 8> mappings{};
        std::size_t mappingCount = 0;
        std::array<Point, 32> points{};
        std::size_t pointCount = 3;
    };
    struct Voice {
        bool active = false, onsetCaptured = false;
        engine::Event note{};
        std::array<double, 3> target{}, current{};
        double onset = .5;
        std::array<double, 3> sent{};
    };
    void add(const std::string &, double, double, double, bool = false);
    double real(std::size_t, double) const noexcept;
    double value(std::size_t) const noexcept;
    void publish();
    void mpe(engine::EventSpan, engine::EventList &, int) noexcept;
    double envelope(const Config &) noexcept;
    double random() noexcept;
    Kind kind_;
    DeviceIdentity identity_;
    std::vector<ParamDescriptor> params_;
    std::array<std::atomic<double>, 32> values_{};
    Config config_;
    engine::SnapshotPublisher<Config> publisher_;
    std::array<Voice, 256> voices_{};
    std::array<double, 8> lastMapping_{};
    std::vector<double> echo_;
    std::size_t echoAt_ = 0;
    double rate_ = 48000, phase_ = 1, velocity_ = 1, target_ = 0, current_ = 0;
    bool held_ = false, wasPlaying_ = false, forceRefresh_ = false;
    std::uint64_t note_ = 0, clock_ = 0, increment_ = 0, seenConfig_ = 0;
    std::uint32_t random_ = 1;
};
} // namespace adi::device
