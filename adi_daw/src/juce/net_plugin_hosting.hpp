// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// DEV-27 (ADR-0261 d22): the consent ledger issues the plug-in hosting lease.
// The device's I/O worker checks a lease before every connection, block, state
// and parameter exchange (GrantedProcessor in net_plugin_admission.cpp). This
// owner makes those leases from the AGT-03 ledger, so that a server receives
// nothing the local user has not allowed for this project file, connection,
// server and disclosure, and so that a lease ends when its term runs out or
// when it is revoked, also while the device is streaming.

#include "adi/control/consent.hpp"
#include "juce/net_plugin_admission.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace adi::net {

/// The disclosure a hosting grant covers, beside the audio bytes per block that
/// the scope carries: events, parameter values and plug-in state. Each is the
/// largest the wire accepts, so the question states an upper bound.
struct PluginHostingDisclosure {
    static constexpr std::size_t eventBytesPerBlock = 56 * 4096;
    static constexpr std::size_t parameterBytes = 2 * 1024 * 1024;
    static constexpr std::size_t stateBytes = 40 * 1024 * 1024;
};

/// One refused admission, for the window's consent prompt. Answering it grants
/// exactly `request`; the device is admitted the next time it is loaded.
struct PluginHostingQuestion {
    std::uint64_t id = 0;
    PluginHostingScope scope;
    control::ConsentRequest request;
};

using UnixSecondsClock = std::function<std::int64_t()>;
[[nodiscard]] std::int64_t systemUnixSeconds();

/// The ledger request a hosting scope becomes. Audio is the scope's bytes per
/// block; events go as Notes. With an empty file binding the ledger refuses it.
[[nodiscard]] control::ConsentRequest pluginHostingRequest(const PluginHostingScope&, std::string fileBinding);

/// The application's one owner of hosting leases. Every call but the leases'
/// own check runs off the audio thread; the authorizer may run on the render
/// worker, so the owner is safe to share between threads.
class PluginHostingLeases {
public:
    explicit PluginHostingLeases(control::ConsentLedger ledger, UnixSecondsClock now = systemUnixSeconds);

    /// For makePluginServerLoader. A grant This time admits one load and its
    /// lease lasts while the device lives; a standing grant's lease ends at its
    /// expiry. A request the ledger refuses becomes one pending question.
    [[nodiscard]] PluginHostingAuthorize authorizer() const;

    /// The questions asked since the last call, each once.
    [[nodiscard]] std::vector<PluginHostingQuestion> takeQuestions();
    /// Records the local user's answer in the ledger. Nothing reconnects by itself.
    [[nodiscard]] bool answer(const PluginHostingQuestion&, control::ConsentTerm);

    /// Removes the ledger grants of every live lease for this project and
    /// connection, and ends those leases at their next check. Returns how many ended.
    std::size_t revoke(std::string_view projectUuid, std::string_view connectionId);
    /// Re-reads the ledger for the live standing leases. A grant removed by any
    /// process ends its leases; a renewed one moves their expiry. A ledger that
    /// cannot be read changes nothing. Returns how many leases ended.
    std::size_t refresh();
    /// Leases still held by a device and not ended.
    [[nodiscard]] std::size_t live() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace adi::net
