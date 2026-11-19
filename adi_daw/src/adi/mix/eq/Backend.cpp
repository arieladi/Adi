// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Backend.hpp"
#include "Core.hpp"
#include <atomic>

namespace adi::mix::eq {
namespace {
class Backend final : public EqProcessingBackend {
public:
    bool supportsDouble() const noexcept override { return true; }
    void prepare(double rate, std::int32_t maximum, std::int32_t channels,
                 const Settings& settings) override {
        core_.prepare({rate, maximum, channels}, settings);
        latency_.store(core_.latencySamples(), std::memory_order_release);
    }
    void release() noexcept override {
        // Retain same-instance detector learning across owner-side release /
        // reprepare, matching the authored engine. No display collection or
        // background analysis is enabled by this processing-only provider.
    }
    void process(float* const* main, const float* const* key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings& settings, std::uint64_t physicalBlock,
                 std::int32_t configuredLatency) noexcept override {
        const auto block = core_.beginBlock(physicalBlock, configuredLatency, settings);
        (void)core_.process(main, key, keyChannels, frames, settings, block);
    }
    void process(double* const* main, const double* const* key, std::int32_t keyChannels,
                 std::int32_t frames, const Settings& settings, std::uint64_t physicalBlock,
                 std::int32_t configuredLatency) noexcept override {
        const auto block = core_.beginBlock(physicalBlock, configuredLatency, settings);
        (void)core_.process(main, key, keyChannels, frames, settings, block);
    }
    void pumpMainThread(const Settings&) override {
        const auto pending = core_.pendingLatencyChange();
        latency_.store(pending.token ? pending.samples : core_.latencySamples(), std::memory_order_release);
    }
    std::int32_t advertisedLatency() const noexcept override {
        return latency_.load(std::memory_order_acquire);
    }
private:
    Core core_;
    std::atomic<std::int32_t> latency_{0};
};
} // namespace
std::unique_ptr<EqProcessingBackend> makeBackend() { return std::make_unique<Backend>(); }
} // namespace adi::mix::eq
