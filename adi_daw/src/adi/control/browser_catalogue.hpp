// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/control/jsonrpc.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi::preset { struct Entry; }
namespace adi::ui { struct PluginListing; }

namespace adi::control {

/// Immutable projection of the same device, plug-in and preset lists used by
/// the app's Browser. The snapshot holds no installation or library paths.
class BrowserCatalogue {
public:
    static constexpr std::size_t kDefaultLimit = 64;
    static constexpr std::size_t kMaxLimit = 128;

    BrowserCatalogue(const ui::PluginListing&, std::span<const preset::Entry>);

    /// Offset within the matching rows. nextOffset is present when more rows
    /// match, including when the wire byte bound ends a page early.
    [[nodiscard]] Json page(std::string_view query = {}, std::size_t offset = 0,
                            std::size_t limit = kDefaultLimit) const;

private:
    struct Row {
        Json data;
        std::string name, vendor, deviceName;
        std::size_t bytes;
    };
    void add(Json, std::string_view name, std::string_view vendor = {}, std::string_view deviceName = {});

    std::vector<Row> rows_;
};

} // namespace adi::control
