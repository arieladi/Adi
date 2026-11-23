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
    // DEV-21, the Sampler as a built-in instrument: SLOT MODE. 32 fixed layers,
    // layer i playing the host's sample slot i+1, borrowed for each call. A
    // filled slot is a zone over every key and velocity until publishSlotZones
    // gives it another shape; filling one slot never cuts another's voices.
    static constexpr std::size_t kSlots = 32;
    /// The most events one block brings a layer: OneShotPdMidi's capacity,
    /// which feeds the hosted Sampler (test_builtin_devices checks it).
    static constexpr std::size_t kEventCapacity = 512;
    struct SlotSample {
        const OneShot::PreparedSample *sample = nullptr;
        std::uint64_t generation = 0;
    };
    struct SlotBacked {};
    static constexpr SlotBacked slotBacked{};
    explicit Sampler(SlotBacked);
    /// Audio thread (slot mode). Each slot's sample is owned by the caller for
    /// the call; a changed generation resets that slot's voices only.
    void processSlots(const engine::NodeIo &, std::span<const SlotSample, kSlots>) noexcept;
    /// Message thread (slot mode): the 32 zone shapes, whole, without samples.
    /// Refused, keeping the old table, if any zone is invalid or holds samples.
    bool publishSlotZones(std::span<const Zone, kSlots>);
    /// Audio thread: every voice of every layer stops (a MIDI reset).
    void resetVoices() noexcept;
    /// Any thread: parameter index to a normalized value, no string lookup.
    bool setParamAt(std::size_t index, double normalized) noexcept;
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    engine::EventFlow eventFlow() const noexcept override { return engine::EventFlow::Consume; }
    void prepare(double, std::int32_t) override;
    // The next prepare() re-prepares even at the same format (voices reset).
    void release() override { prepared_ = false; }
    void process(const engine::NodeIo &) noexcept override;
    void pumpMainThread() override {
        publisher_.collect();
        slotZones_.collect();
    }
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
    static double real(const ParamDescriptor &, double normalized) noexcept;
    static double weight(const Range &, double, bool) noexcept;
    void configure(Layer &);
    struct SlotZones : engine::Sequenced {
        std::array<Zone, kSlots> zones{};
    };
    void initialise();
    static void applyZone(OneShot &, const Zone &) noexcept;
    void playLayer(const engine::NodeIo &, OneShot &, const Zone &,
                   const std::array<double, OneShot::Count> &initial,
                   const OneShot::PreparedSample *, std::uint64_t generation, bool slot) noexcept;
    std::array<std::unique_ptr<OneShot>, kSlots> slotEngines_{};
    engine::SnapshotPublisher<SlotZones> slotZones_;
    std::uint64_t slotZonesSeen_ = 0;              // audio thread
    std::array<std::uint64_t, kSlots> slotSeen_{}; // audio thread
    bool slotMode_ = false;
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
    bool prepared_ = false;
    DeviceIdentity identity_{"internal", "adi.sampler", "Sampler", "ADI", "1"};
};
} // namespace adi::device
