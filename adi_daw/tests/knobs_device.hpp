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
#include <cstdio>
#include <memory>
#include <string>
namespace adi::test {
class Knobs final : public device::DeviceInstance {
  public:
    // Cutoff's id is "cutoff" unless a test needs the numeric id an automation lane's `param_ref`
    // must parse as (the engine binds a device lane only to a parameter id that is a number; a
    // "test" plug-in's id is its lane ref as it stands, where a CLAP id is hex turned decimal).
    explicit Knobs(std::string cutoffId = "cutoff") {
        identity_.name = "UI knobs";
        identity_.format = "test";
        params_[0].id = std::move(cutoffId);
        params_[0].name = "Cutoff";
        params_[0].hasRealRange = true;
        params_[0].minReal = 20;
        params_[0].maxReal = 20000;
        // The defaults are where the knobs start, so Return to Default (manual 41.6) is a move
        // back to where a test found them.
        params_[0].defaultValue = device::ParamValue::withReal(0.5, hertz(0.5));
        params_[1].id = "gain";
        params_[1].name = "Gain";
        params_[1].defaultValue = device::ParamValue::fromNormalized(0.25);
    }
    // Cutoff's Hz over its range, linear, as a test needs no curve.
    static double hertz(double normalized) noexcept { return 20 + normalized * 19980; }
    // The plug-in's own words for a value (ADR-0181 d3), as a real plug-in gives them, so the strip
    // reads as Live's Device View does: Cutoff in Hz or kHz, Gain in dB over -36 to +12.
    std::string paramText(const std::string &id, double normalized) const override {
        char text[32];
        if (id == params_[0].id) {
            const double hz = hertz(normalized);
            if (hz < 1000)
                std::snprintf(text, sizeof text, "%.0f Hz", hz);
            else
                std::snprintf(text, sizeof text, "%.1f kHz", hz / 1000);
        } else if (id == "gain") {
            std::snprintf(text, sizeof text, "%.1f dB", -36 + normalized * 48);
        } else {
            return {};
        }
        return text;
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
                return i == 0 ? device::ParamValue::withReal(value, hertz(value))
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
// for any other, so a real plug-in in the same project stays a placeholder. `cutoffId` as above.
inline engine::DeviceLoader knobsLoader(std::string cutoffId = "cutoff") {
    return [cutoffId](const engine::DeviceRequest &request, std::string &error)
               -> std::unique_ptr<device::DeviceInstance> {
        if (request.ref == nullptr || request.ref->uid != kKnobsUid) {
            error = "not a test plug-in";
            return nullptr;
        }
        return std::make_unique<Knobs>(cutoffId);
    };
}
} // namespace adi::test
