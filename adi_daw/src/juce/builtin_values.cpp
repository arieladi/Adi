// SPDX-License-Identifier: AGPL-3.0-or-later

#include "juce/builtin_values.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace adi::device::builtin_values {

double natural(const PdParamDecl& declaration, double normalized) noexcept {
    normalized = std::clamp(normalized, 0.0, 1.0);
    double value = declaration.min + (declaration.max - declaration.min) * normalized;
    if (declaration.curve == PdCurve::Log)
        value = declaration.min * std::pow(declaration.max / declaration.min, normalized);
    else if (declaration.curve == PdCurve::Exponent)
        value = declaration.min + (declaration.max - declaration.min) * std::pow(normalized, declaration.exponent);
    else if (declaration.curve == PdCurve::Toggle)
        value = normalized >= 0.5 ? declaration.max : declaration.min;
    if (declaration.curve == PdCurve::Int || declaration.curve == PdCurve::Menu) value = std::round(value);
    return std::clamp(value, declaration.min, declaration.max);
}

double normalized(const PdParamDecl& declaration, double value) noexcept {
    value = std::clamp(value, declaration.min, declaration.max);
    if (declaration.max <= declaration.min) return 0;
    if (declaration.curve == PdCurve::Log)
        return std::log(value / declaration.min) / std::log(declaration.max / declaration.min);
    const double fraction = (value - declaration.min) / (declaration.max - declaration.min);
    if (declaration.curve == PdCurve::Exponent) return std::pow(fraction, 1.0 / declaration.exponent);
    return fraction;
}

double canonical(const PdParamDecl& declaration, double position) noexcept {
    return normalized(declaration, natural(declaration, position));
}

namespace {

/// The curves on which a linear reading of the range is the value itself.
bool realIsLinear(const PdParamDecl& declaration) noexcept {
    return declaration.curve == PdCurve::Lin || declaration.curve == PdCurve::Int ||
           declaration.curve == PdCurve::Menu || declaration.curve == PdCurve::Toggle;
}

/// A value in its unit, rounded or snapped by the curve, as a position.
double positionOfValue(const PdParamDecl& declaration, double value) noexcept {
    const double clamped = std::clamp(value, declaration.min, declaration.max);
    return canonical(declaration, normalized(declaration, clamped));
}

}  // namespace

std::optional<double> positionFor(const PdParamDecl& declaration, const ParamValue& value) noexcept {
    if (value.hasReal && realIsLinear(declaration)) {
        if (!std::isfinite(value.real)) return std::nullopt;
        return positionOfValue(declaration, value.real);
    }
    if (!std::isfinite(value.normalized) || value.normalized < 0 || value.normalized > 1) return std::nullopt;
    return canonical(declaration, value.normalized);
}

void write(Payload& state, const std::vector<PdParamDecl>& declarations, const float* positions) {
    auto& params = state["params"];
    if (!params.is_object()) params = Payload::object();
    auto& real = state["real"];
    if (!real.is_object()) real = Payload::object();
    auto& units = state["units"];
    if (!units.is_object()) units = Payload::object();
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        const auto id = std::to_string(declarations[i].id);
        const double position = static_cast<double>(positions[i]);
        params[id] = position;
        real[id] = natural(declarations[i], position);
        units[id] = declarations[i].unit;
    }
}

bool read(const Payload& state, const std::vector<PdParamDecl>& declarations,
          std::vector<std::pair<std::size_t, float>>& result) {
    // Filled only on success: a refused state leaves nothing to apply.
    result.clear();
    std::vector<std::pair<std::size_t, float>> out;
    if (!state.contains("params") || !state.at("params").is_object()) return false;
    const auto& params = state.at("params");
    const Payload* real = state.contains("real") && state.at("real").is_object() ? &state.at("real") : nullptr;
    const Payload* units = state.contains("units") && state.at("units").is_object() ? &state.at("units") : nullptr;
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        const auto& declaration = declarations[i];
        const auto id = std::to_string(declaration.id);
        // The real value, when it was saved in the unit the declaration has now.
        if (real != nullptr && real->contains(id)) {
            const auto& v = real->at(id);
            if (!v.is_number()) return false;
            const double value = v.get<double>();
            if (!std::isfinite(value)) return false;
            const bool sameUnit = units == nullptr || !units->contains(id) ||
                                  (units->at(id).is_string() &&
                                   units->at(id).get_ref<const std::string&>() == declaration.unit);
            if (sameUnit) {
                out.emplace_back(i, static_cast<float>(positionOfValue(declaration, value)));
                continue;
            }
        }
        if (!params.contains(id)) continue;
        const auto& p = params.at(id);
        if (!p.is_number()) return false;
        const double position = p.get<double>();
        if (!std::isfinite(position) || position < 0 || position > 1) return false;
        out.emplace_back(i, static_cast<float>(canonical(declaration, position)));
    }
    // Every saved position, known id or not, must be a number in 0..1, or the
    // state is refused before anything changes (as before).
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!it->is_number()) return false;
        const double position = it->get<double>();
        if (!std::isfinite(position) || position < 0 || position > 1) return false;
    }
    result = std::move(out);
    return true;
}

}  // namespace adi::device::builtin_values
