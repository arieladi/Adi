// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/mix/eq_processing.hpp"
#include <vector>
#include "adi/panel.hpp"
#include "adi/mix/eq/Settings.hpp"

namespace adi::device {
// Stable decimal IDs follow the authored processing parameter order. The
// standalone analyzer-only control is deliberately outside this DSP controller.
inline constexpr std::size_t mixEqParameterCount = 730;
[[nodiscard]] std::unique_ptr<DeviceInstance> makeMixEqFunction(
    const engine::DeviceRequest&, std::string& error,
    std::unique_ptr<mix::EqProcessingBackend>);

// Decode one published panel through the same mapping as parameter playback.
std::optional<mix::eq::Settings> publishedMixEqSettings(std::span<const panel::Record>);

struct MixEqCompanionEdit { std::string id; ParamValue value; };
enum class EqNodeAxis { Frequency, Gain };
// Map a plotted physical coordinate through the native parameter's authored range.
std::optional<MixEqCompanionEdit> mixEqNodeValue(std::size_t band, EqNodeAxis axis, float physical);
// Pure owner-side planning, with no setters/host calls. A future page may append
// these changes to the SAME common undo/gesture transaction as its primary edit.
// Until that authority is bound, Side Link is read-only/off in this tier and a
// state requiring linked detector controls is refused without partial recall.
[[nodiscard]] std::vector<MixEqCompanionEdit> mixEqCompanionEdits(
    const DeviceInstance&, const std::string& id, const ParamValue& desired);
} // namespace adi::device
