// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/mix/eq_processing.hpp"
#include <vector>

namespace adi::device {
// Stable decimal IDs follow the authored processing parameter order. The
// standalone analyzer-only control is deliberately outside this DSP controller.
inline constexpr std::size_t mixEqParameterCount = 730;
[[nodiscard]] std::unique_ptr<DeviceInstance> makeMixEqFunction(
    const engine::DeviceRequest&, std::string& error,
    std::unique_ptr<mix::EqProcessingBackend>);

struct MixEqCompanionEdit { std::string id; ParamValue value; };
// Pure owner-side planning, with no setters/host calls. A future page may append
// these changes to the SAME common undo/gesture transaction as its primary edit.
// Until that authority is bound, Side Link is read-only/off in this tier and a
// state requiring linked detector controls is refused without partial recall.
[[nodiscard]] std::vector<MixEqCompanionEdit> mixEqCompanionEdits(
    const DeviceInstance&, const std::string& id, const ParamValue& desired);
} // namespace adi::device
