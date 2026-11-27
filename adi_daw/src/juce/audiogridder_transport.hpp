// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "juce/remote_device.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adi::device {

/// Supplied by an owner that has already authorized contact with this server.
/// v13 uses plaintext TCP. The only addresses this prototype resolves are the
/// numeric IPv4 address supplied here and a worker port on that same address.
struct AudioGridderEndpoint {
    std::string address;
    std::uint16_t port = 0;
    std::chrono::milliseconds setupTimeout{5000};
    std::chrono::milliseconds blockTimeout{1000};
};

struct AudioGridderParameterBinding {
    std::int32_t serverIndex = -1;
    /// Stable ID established for this exact paired plug-in, never inferred
    /// from the stock server's presentation index or display name.
    std::string id;
};

struct AudioGridderPlugin {
    /// The stock server's VST3 id2 token. ADI's stable class ID remains separate.
    std::string serverToken;
    std::string layout = "Default";
    /// Optional stock wire state, using the server's JUCE MemoryBlock encoding.
    std::string encodedState{};
    bool instrument = false;
    std::vector<AudioGridderParameterBinding> parameterBindings{};
};

struct AudioGridderCatalogueEntry {
    /// Stock id2 token. A paired source must supply the full class ID.
    std::string serverToken;
    std::string name;
    std::string format;
    bool instrument = false;
};

/// Worker-side query for an explicitly authorized endpoint. Requests the
/// unfiltered stock list and closes without activating any plug-in. Returns
/// supported VST3 rows with unambiguous tokens. Failure leaves rows intact.
[[nodiscard]] bool readAudioGridderCatalogue(const AudioGridderEndpoint& endpoint,
    std::vector<AudioGridderCatalogueEntry>& rows, std::string& error);

/// Builds the worker-owned side of a stock protocol v13 mono/stereo float32
/// device. Creation does not connect. See docs/ENGINE-PLUGIN-SERVER.md.
[[nodiscard]] std::unique_ptr<RemoteProcessor> makeAudioGridderProcessor(
    AudioGridderEndpoint endpoint, AudioGridderPlugin plugin);

} // namespace adi::device
