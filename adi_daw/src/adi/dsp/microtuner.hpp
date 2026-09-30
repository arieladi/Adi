// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/publisher.hpp"
#include "juce/device_model.hpp"
namespace adi::device {
class Microtuner final : public DeviceInstance {
  public:
    Microtuner();
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    bool transformsEvents() const noexcept override { return true; }
    void transformEvents(engine::EventSpan, engine::EventList &, std::int32_t, double,
                         const engine::TransportInfo *) noexcept override;
    std::int32_t paramCount() const noexcept override { return 3; }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    void pumpMainThread() override { publisher_.collect(); }
    // Message thread. Each table contains pitch offsets in semitones for MIDI keys 0..127.
    bool setDeck(int, const std::array<double, 128> &);
    bool importScala(int, const std::string &, int root = 60);
    bool generate(int, int divisions, double periodRatio, int root = 60);
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;

  private:
    struct Tuning : engine::Sequenced {
        std::array<std::array<double, 128>, 2> decks{};
    };
    struct Note {
        bool active = false;
        engine::Event source{};
        double expression = 0, last = 0;
    };
    void publish();
    double real(std::size_t, double) const noexcept;
    double offset(const Tuning &, std::uint16_t) const noexcept;
    std::array<ParamDescriptor, 3> params_{};
    std::array<std::atomic<double>, 3> values_{};
    std::array<Note, 256> notes_{};

    Tuning tuning_; // message-thread state
    engine::SnapshotPublisher<Tuning> publisher_;
    DeviceIdentity identity_{"internal", "adi.microtuner", "Microtuner", "ADI", "1"};
};
} // namespace adi::device
