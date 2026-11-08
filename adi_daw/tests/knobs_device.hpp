// SPDX-License-Identifier: GPL-3.0-or-later
//
// A headless plug-in for the window's tests: two knobs, no sound. The same
// shape as test_ui_param_capture's, cut to what a document and a strip need:
// a device the Session's loader can make from a `plugin_refs` row of format
// "test", whose parameters a drag in the strip can edit and read back.
#pragma once
#include "adi/engine/session.hpp"
#include "juce/device_model.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <string>
namespace adi::test {
class Knobs final : public device::DeviceInstance {
  public:
    Knobs() {
        identity_.name = "UI knobs";
        identity_.format = "test";
        params_[0].id = "cutoff";
        params_[0].name = "Cutoff";
        params_[0].hasRealRange = true;
        params_[0].minReal = 20;
        params_[0].maxReal = 20000;
        params_[1].id = "gain";
        params_[1].name = "Gain";
    }
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
    std::int32_t paramCount() const noexcept override { return 2; }
    const device::ParamDescriptor *paramAt(std::int32_t index) const noexcept override {
        return index >= 0 && index < 2 ? &params_[static_cast<std::size_t>(index)] : nullptr;
    }
    device::ParamValue getParam(const std::string &id) const noexcept override {
        for (std::size_t i = 0; i < 2; ++i)
            if (params_[i].id == id) {
                const auto value = values_[i].load(std::memory_order_relaxed);
                return i == 0 ? device::ParamValue::withReal(value, 20 + value * 19980)
                              : device::ParamValue::fromNormalized(value);
            }
        return {};
    }
    bool setParam(const std::string &id, const device::ParamValue &value) override {
        if (refuse)
            return false;
        for (std::size_t i = 0; i < 2; ++i)
            if (params_[i].id == id) {
                ++sets;
                values_[i].store(value.normalized, std::memory_order_relaxed);
                return true;
            }
        return false;
    }
    // Set to make the plug-in refuse every value, as a host whose set failed.
    bool refuse = false;
    int sets = 0;

  private:
    device::DeviceIdentity identity_;
    std::array<device::ParamDescriptor, 2> params_;
    std::array<std::atomic<double>, 2> values_{{0.5, 0.25}};
};
// The uid a test writes in `plugin_refs` for a Knobs; the schema allows only the real formats,
// so the row says "clap", as the engine's own test does.
inline constexpr const char *kKnobsUid = "test.knobs";
// The loader a document opens with: a Knobs for every plug-in reference with that uid, nothing
// for any other, so a real plug-in in the same project stays a placeholder.
inline engine::DeviceLoader knobsLoader() {
    return [](const engine::DeviceRequest &request, std::string &error)
               -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref == nullptr || request.ref->uid != kKnobsUid) {
            error = "not a test plug-in";
            return nullptr;
        }
        return std::make_unique<Knobs>();
    };
}
} // namespace adi::test
