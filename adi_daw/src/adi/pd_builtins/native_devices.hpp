// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string_view>

namespace adi::engine {
struct NodeIo;
struct MpeOut;
}

namespace adi::device {
// ADR-0203 d4: a shipped canvas can use the same native core as its compiled
// Pd external, without introducing a second 64-sample scheduler. Construction
// and prepare are off-thread; every other method is serialized on the audio
// thread. The host still owns parameter declarations, MIDI routing and samples.
class NativePdDevice {
  public:
    virtual ~NativePdDevice() = default;
    virtual void prepare(double sampleRate, int maxFrames) = 0;
    virtual void process(const engine::NodeIo&) noexcept = 0;
    virtual bool sendParameter(int id, float value) noexcept = 0;
    [[nodiscard]] virtual int latencySamples() const noexcept = 0;
    virtual void tempo(double) noexcept {}
    virtual void midi(const engine::MpeOut&) noexcept {}
};

// Compiled-in canvas bytes (only CRLF/LF may differ), not a filename or an
// external's name. Any other edit or unsupported I/O stays on the Pd path.
[[nodiscard]] std::unique_ptr<NativePdDevice> makeNativePdDevice(
    std::string_view patchText, int inChannels, int outChannels);

// The host verifies the resolved abstractions too: a user-edited adi.param or
// adi.transport must retain its Pd behaviour instead of being substituted.
[[nodiscard]] bool nativePdAbstractionMatches(std::string_view filename,
                                             std::string_view text) noexcept;
} // namespace adi::device
