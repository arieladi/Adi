// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "juce/device_model.hpp"
#include <array>
namespace adi::device {
// Stateful note mapping: release and expression follow the attack's decision,
// even if automation changes the mapping while the note is held.
class MidiNotes final : public DeviceInstance {
  public:
    enum class Kind {
        Pitch,
        Chord,
        Random,
        Scale,
        Velocity,
        NoteLength,
        NoteEcho,
        Monitor,
        CCControl,
        Arpeggiator
    };
    explicit MidiNotes(Kind);
    static std::unique_ptr<DeviceInstance> create(const std::string &uid);
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    void noteOffDelivered(const engine::Event &) noexcept override;
    void noteOffRejected(const engine::Event &) noexcept override;
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    bool transformsEvents() const noexcept override { return true; }
    void transformEvents(engine::EventSpan, engine::EventList &, std::int32_t, double,
                         const engine::TransportInfo *) noexcept override;
    std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(specs_.size());
    }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;
    std::uint64_t rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }

    bool readMonitor(engine::Event &) noexcept;
    void clearMonitor() noexcept;

  private:
    struct Spec {
        ParamDescriptor descriptor;
        bool integer;
    };
    struct Repeat {
        int remaining = 0, pitch = 0;
        std::int64_t period = 0;
        double decay = 1;
    };
    struct Voice {
        engine::Event input{};
        std::array<double, 3> expression{};
        std::array<bool, 3> hasExpression{};
        std::array<engine::Event, 7> output{};
        int count = 0;
        bool active = false;
        std::array<std::int64_t, 7> delay{};
        std::array<Repeat, 7> repeats{};
        bool releasing = false;
        engine::Event release{};
        std::int64_t started = 0;
        bool lengthOff = false, latch = false, inputReleased = false, latchedSounding = false;
    };
    void add(const std::string &, double, double, double, bool integer = false);
    double real(std::size_t, double) const noexcept;
    double value(std::size_t) const noexcept;
    double random() noexcept;
    int transpose(int, int) const noexcept;
    std::size_t scaleIndex_ = 64;
    void attack(const engine::Event &, Voice &) noexcept;
    double velocity(double) noexcept;
    bool emit(engine::EventList &, const engine::Event &) noexcept;
    void arpeggiate(engine::EventSpan, engine::EventList &, std::int32_t,
                    const engine::TransportInfo *) noexcept;
    double nextArp_ = 0;
    std::uint64_t arpStep_ = 0;
    std::array<std::size_t, 256> arpOrder_{};
    std::array<std::uint64_t, 2304> arpPermutation_{};
    std::uint64_t arpTotal_ = 0;
    bool queue(engine::Event, std::int64_t, const Repeat * = nullptr) noexcept;
    void flush(engine::EventList &, std::int32_t) noexcept;
    struct Pending {
        engine::Event event;
        std::int64_t due;
        std::uint64_t order;
        Repeat repeat{};
        std::uint64_t source = 0;
    };
    struct OutputNote {
        engine::Event note{};
        bool active = false;
        std::uint64_t source = 0;
    };
    std::array<OutputNote, 8192> sounding_{};
    bool wasPlaying_ = false, hadTransport_ = false;
    std::int64_t expectedTimeline_ = 0;
    std::array<Pending, 8192> pending_{};
    std::size_t pendingCount_ = 0;
    std::int64_t clock_ = 0;
    std::uint64_t order_ = 0, currentSource_ = 0;
    double rate_ = 48000;
    double bpm_ = 120;
    std::array<engine::Event, 1024> monitor_{};
    std::atomic<std::uint64_t> monitorHead_{0}, monitorTail_{0};
    engine::EventList *immediate_ = nullptr;
    Kind kind_;
    DeviceIdentity identity_;
    std::vector<Spec> specs_;
    std::array<std::atomic<double>, 64> values_{};
    std::array<double, 64> sentControls_{};
    std::array<Voice, 256> voices_{};

    std::uint32_t rng_ = 1;
    unsigned alternate_ = 0;
    std::uint64_t nextId_ = 1;
    std::atomic<std::uint64_t> rejected_{0};
};
} // namespace adi::device
