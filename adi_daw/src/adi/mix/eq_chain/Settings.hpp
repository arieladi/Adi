// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "../eq/Settings.hpp"
#include "../carve/Settings.hpp"
#include "../focus/Settings.hpp"

namespace adi::mix::eq_chain {
struct Settings {
    bool eqEnabled = true, carveEnabled = false, focusEnabled = false;
    eq::Settings eq;
    carve::Settings carve;
    focus::Settings focus;
    bool operator==(const Settings &) const = default;
};
bool valid(const Settings &) noexcept;
} // namespace adi::mix::eq_chain
