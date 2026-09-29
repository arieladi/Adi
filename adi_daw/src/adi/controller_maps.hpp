// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi {
struct ControllerCc {
    std::string port;
    int channel = 1; // Settings/learn use 1..16, never the wire's zero-based nibble.
    int number = 0;
    friend bool operator==(const ControllerCc&, const ControllerCc&) = default;
};
enum class ControllerMode : int { Absolute = 0, Relative = 1, Toggle = 2 };
enum class ControllerTakeover : int { Jump = 0, Pickup = 1, Scale = 2 };
// Explicit snapshot supplied by the caller at Learn/dispatch time. No setting
// registry, hardware object, or machine identity enters the journal handlers.
struct ControllerPolicy {
    bool remoteEnabled = false; // Role of the incoming port.
    std::optional<ControllerCc> focusDial;
    ControllerTakeover takeover = ControllerTakeover::Pickup;
};
struct ControllerBinding {
    std::int64_t id = 0;
    Payload binding;
};
// Pure heuristic: a nonempty cluster of 1/127 (with zero neutral), or 63..65
// (64 neutral), is relative. A neutral-only or mixed/other trace is absolute.
// A single distinctive CC can bind immediately; an absolute knob parked at a
// relative code is inherently ambiguous. Additional observed values refine it.
[[nodiscard]] ControllerMode detectControllerMode(std::span<const int> values) noexcept;
// Reads the lane's actual owner/param_ref, not its display name. Returns one
// resolved controller.bind request; rejects non-Remote and reserved controls.
[[nodiscard]] std::optional<OpRequest> learnController(
    const Store&, std::int64_t bindingId, std::int64_t laneId,
    const ControllerCc&, std::span<const int> values,
    const ControllerPolicy&, std::string& error);
[[nodiscard]] std::vector<ControllerBinding> readControllerBindings(const Store&);
// For the binding list AND dispatch: an imported binding may collide with
// this machine's reservation even though Learn could never create that clash.
[[nodiscard]] bool controllerBindingShadowed(const ControllerBinding&, const ControllerPolicy&);
enum class ControllerDispatch { Ignored, FocusDial, Binding };
// Message-thread MIDI edge. Focus always gets first refusal; callbacks cannot
// both fire for the same CC. Parameter gestures/takeover are the caller's edge.
[[nodiscard]] ControllerDispatch dispatchController(
    const ControllerCc&, int value, const ControllerPolicy&,
    std::span<const ControllerBinding>,
    const std::function<void(int)>& focus,
    const std::function<void(const ControllerBinding&, int)>& bound);

// Op catalogue seams, separated so storage and detection share one vocabulary.
bool controllerBindApply(OpContext&, const Payload&, std::string&);
bool controllerBindInverse(OpContext&, const Payload&, Payload&, std::string&);
bool controllerUnbindApply(OpContext&, const Payload&, std::string&);
bool controllerUnbindInverse(OpContext&, const Payload&, Payload&, std::string&);
}
