// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <optional>

namespace adi::scan {
#if defined(_WIN32)
// Reads a bounded Shell Link's stored absolute filesystem target. No Resolve,
// shell execution, PIDL binding, target search or shortcut modification.
// Missing/malformed/unsupported links return nullopt. Owner thread only.
std::optional<std::filesystem::path> windowsShortcutTarget(const std::filesystem::path&);
#endif
}
