// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Backend.hpp"
#include "../eq_chain/Backend.hpp"
#include <array>

namespace adi::mix::eq {
namespace {
class Backend final : public EqProcessingBackend {
public:
    bool supportsDouble() const noexcept override { return true; }
    void prepare(double rate, std::int32_t maximum, std::int32_t channels,
                 const Settings& settings) override {
        prepared_ = false;
        audioSettings_.eq = settings;
        serial_.prepare({rate, maximum, channels}, audioSettings_);
        channels_ = channels;
        maximum_ = maximum;
        prepared_ = true;
    }
    void release() noexcept override {
        prepared_ = false;
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
        if (!prepared_ || frames <= 0 || frames > maximum_ || !main || !main[0] ||
            (channels_ == 2 && !main[1]) || keyChannels < 0 || keyChannels > 2 || (keyChannels && !key)) return;
        // The existing EQ facade refused malformed positive-width key planes.
        // Keep that policy even though the general serial core can zero a plane.
        for (std::int32_t ch = 0; ch < keyChannels; ++ch) if (!key[ch]) return;
        std::array<const Sample*, 2> programme{main[0], channels_ == 2 ? main[1] : main[0]};
        if (settings.externalSidechain && keyChannels == 0) {
            // Native EQ historically falls back to programme without an actual
            // external bus. Supplying that view preserves the authored toggle
            // and detector law; connected silence remains a genuine silent key.
            key = programme.data();
            keyChannels = channels_;
        }
        audioSettings_.eq = settings;
        (void)serial_.process(main, key, keyChannels, frames, audioSettings_, physicalBlock,
                              configuredLatency);
    }
    eq_chain::Backend serial_;
    // Audio-only DTO while running. Stopped preparation may initialize it;
    // the unused downstream defaults are constructed once, not per segment.
    eq_chain::Settings audioSettings_;
    std::int32_t channels_ = 0, maximum_ = 0;
    bool prepared_ = false;
};
} // namespace
std::unique_ptr<EqProcessingBackend> makeBackend() { return std::make_unique<Backend>(); }
} // namespace adi::mix::eq
