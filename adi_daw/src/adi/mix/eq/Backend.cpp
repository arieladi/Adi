// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Backend.hpp"
#include "../eq_chain/Backend.hpp"
#include <algorithm>

namespace adi::mix::eq {
namespace {
bool neutral(const Settings& s) noexcept {
    return s.structure == Structure::Minimum && !s.externalSidechain && !s.bypass &&
           s.outputGainDb == 0 && s.gainScalePercent == 100 &&
           !s.automaticGain && !s.staticGain && !s.polarity && s.lookaheadMs == 0 &&
           s.spectralResolution == Resolution::Medium && s.soloBand == -1 &&
           !s.soloSide && s.soloGainDb == 0 && !s.loudnessMatch &&
           std::all_of(s.bands.begin(), s.bands.end(), [](const auto& band) {
               return band.status == Status::Off && !band.dynamicOn &&
                      !band.dynamicLearn && !band.spectralOn;
           });
}
class Backend final : public EqProcessingBackend {
public:
    explicit Backend(bool offline) : serial_(offline ? eq_chain::OwnerMode::Offline : eq_chain::OwnerMode::Threaded),
                                     offline_(offline) {}
    bool supportsDouble() const noexcept override { return true; }
    bool provesZeroTail() const noexcept override { return zeroTail_; }
    void prepare(double rate, std::int32_t maximum, std::int32_t channels,
                 const Settings& settings) override {
        prepared_ = false;
        zeroTail_ = false;
        // Restrict the proof to a fresh preparation that has never run a
        // stateful configuration. Retained EQ history and learning are owned
        // by stopped reprepare. A return to authored zero does not clear them.
        pristine_ = !everPrepared_ && !offline_ && neutral(settings);
        everPrepared_ = true;
        audioSettings_.eq = settings;
        serial_.prepare({rate, maximum, channels}, audioSettings_);
        channels_ = channels;
        maximum_ = maximum;
        prepared_ = true;
    }
    void release() noexcept override {
        prepared_ = false;
        zeroTail_ = false;
        // The serial adapter joins its owner, then retains the same EQ object
        // for its authored learning/prepare law on stopped reactivation.
        serial_.release();
    }
    void process(float* const* main, const float* const* key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings& settings, std::uint64_t physicalBlock,
                 std::int32_t configuredLatency) noexcept override {
        run(main, key, keyChannels, frames, settings, physicalBlock, configuredLatency);
    }
    void process(double* const* main, const double* const* key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings& settings, std::uint64_t physicalBlock,
                 std::int32_t configuredLatency) noexcept override {
        run(main, key, keyChannels, frames, settings, physicalBlock, configuredLatency);
    }
    void pumpMainThread(const Settings&) override {
        // Main relays atomics and schedules owner work. It never becomes a
        // concurrent Core pending-plan reader or performs heavy preparation.
        serial_.pumpMainThread();
    }
    std::int32_t advertisedLatency() const noexcept override {
        return serial_.advertisedLatency();
    }
    std::string_view processingIssue() const noexcept override {
        switch (serial_.status().failure) {
        case eq_chain::BackendFailure::WorkerStart:
            return "EQ configuration could not start; the last applied processing remains active.";
        case eq_chain::BackendFailure::OwnerService:
            return "EQ configuration failed; the last applied processing remains active.";
        case eq_chain::BackendFailure::None: return {};
        }
        return {};
    }
private:
    template<class Sample>
    void run(Sample* const* main, const Sample* const* key, std::int32_t keyChannels,
             std::int32_t frames, const Settings& settings, std::uint64_t physicalBlock,
             std::int32_t configuredLatency) noexcept {
        zeroTail_ = false;
        if (!prepared_ || frames <= 0 || frames > maximum_ || !main || !main[0] ||
            (channels_ == 2 && !main[1]) || keyChannels < 0 || keyChannels > 2 || (keyChannels && !key)) return;
        // The existing EQ facade refused malformed positive-width key planes.
        // Keep that policy even though the general serial core can zero a plane.
        for (std::int32_t ch = 0; ch < keyChannels; ++ch) if (!key[ch]) return;
        pristine_ = pristine_ && configuredLatency == 0 && neutral(settings);
        audioSettings_.eq = settings;
        const bool processed = serial_.process(main, key, keyChannels, frames, audioSettings_,
                                               physicalBlock, configuredLatency);
        zeroTail_ = pristine_ && processed;
    }
    eq_chain::Backend serial_;
    // Audio-only DTO while running. Stopped preparation may initialize it;
    // the unused downstream defaults are constructed once, not per segment.
    eq_chain::Settings audioSettings_;
    std::int32_t channels_ = 0, maximum_ = 0;
    bool prepared_ = false;
    const bool offline_;
    bool everPrepared_ = false, pristine_ = false, zeroTail_ = false;
};
} // namespace
std::unique_ptr<EqProcessingBackend> makeBackend(bool offline) { return std::make_unique<Backend>(offline); }
} // namespace adi::mix::eq
