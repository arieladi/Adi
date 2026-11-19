// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi::engine {
struct OfflineSourceInfo;
}
namespace adi::freeze {
struct FingerprintSpec {
    std::int64_t root = 0;
    bool subtree = false;
    // Prefix caches retain live audio inserts. Their actual ordered baked
    // prefix comes from Session::offlineSourceInfo after state restoration.
    bool prefixOnly = false;
    std::vector<std::int64_t> prefixDevices;
};
struct Fingerprint {
    std::string hash;
    Payload source;
    std::vector<std::int64_t> tracks, dependencies;
};
// Document owner, under a read/journal transaction. No filesystem or device
// loading. Canonical typed source state excludes UI and root live mix controls.
[[nodiscard]] Fingerprint fingerprint(Store &, const FingerprintSpec &);
} // namespace adi::freeze
