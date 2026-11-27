// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace adi {
class Store;

namespace control {

/// Classifies the authority of an HTTP(S) or WS(S) URL, or a bare
/// host[:port]. Uses only well-known loopback names and numeric addresses.
[[nodiscard]] bool isLoopbackDestination(std::string_view endpoint) noexcept;

/// An opaque, machine-local binding to the file held by this Store. Use it
/// alongside the project UUID for a plug-in server grant. An empty binding
/// means the file could not be identified and carries no durable grant.
/// Called by the Store owner, off the audio thread.
[[nodiscard]] std::string projectFileConsentBinding(const Store&);

}  // namespace control
}  // namespace adi
