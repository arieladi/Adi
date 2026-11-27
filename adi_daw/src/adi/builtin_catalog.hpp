// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/applied_batches.hpp"
#include "adi/ops.hpp"
#include "adi/store_rows.hpp"
#include <span>
#include <string_view>
namespace adi {
namespace engine { enum class RouteChoice : std::uint8_t; }
struct BuiltinInfo {
    std::string_view uid,name,category,group;
    int inputs=2;
    bool available=false,midi=false;
    /// Consumes notes (EventFlow::Consume) and is registered as an instrument (DEV-21).
    bool instrument=false;
    // Ask the graph for the exact musical position of each render segment.
    bool usesTransportPosition=false;
    // Its audio is held while a whole state applies. (Every built-in now
    // reopens a parameter its state omits at the default: DF-E57.)
    bool resetMissingParameters=false;
};
[[nodiscard]] std::span<const BuiltinInfo> builtinCatalog() noexcept;
[[nodiscard]] const BuiltinInfo* findBuiltin(std::string_view uid) noexcept;
/// Canonical compiled canvas, never the working directory or an installed
/// user-editable file. Empty when this build cannot host the device.
[[nodiscard]] std::string_view builtinCanvas(std::string_view uid) noexcept;
/// The parameter and sample declarations: the canvas for a Pd-backed device,
/// the Sampler's declaration text (not a canvas) for the Sampler (DEV-21).
[[nodiscard]] std::string_view builtinDeclarations(std::string_view uid) noexcept;
struct PreparedBuiltinInsert {
    bool ok=false;
    std::string error,sourceFile;
    std::int64_t deviceId=0,chainId=0,referenceId=0;
    std::optional<std::int64_t> newTrack;
    std::vector<OpRequest> requests;
    CommitOptions options;
};
// A document reserves IDs belonging to runtimes retained for Undo. For a
// drop onto a new track, the track-creation requests start the same batch.
[[nodiscard]] PreparedBuiltinInsert prepareBuiltinInsert(Store&,std::string_view uid,
    std::int64_t track,std::optional<std::int64_t> beforeDevice={},std::int64_t retainedDeviceId=0,
    std::span<const OpRequest> newTrack={},bool exclusiveArm=false);
struct PluginInsertOptions {
    bool replaceInstrument = false;
    std::optional<engine::RouteChoice> expressionRoute;
    bool exclusiveArm = false;
};
[[nodiscard]] PreparedBuiltinInsert preparePluginInsert(Store&,const rows::PluginRef&,std::int64_t track,
    std::optional<std::int64_t> beforeDevice={},const PluginInsertOptions& = {},
    std::int64_t retainedDeviceId=0,std::span<const OpRequest> newTrack={});
// Replace a device of the same role in its chain. The caller may append
// parameter and state requests for the new device before committing.
[[nodiscard]] PreparedBuiltinInsert prepareBuiltinReplacement(Store&, std::string_view uid,
    std::int64_t targetDevice, std::int64_t retainedDeviceId=0);
[[nodiscard]] PreparedBuiltinInsert preparePluginReplacement(Store&, const rows::PluginRef&,
    std::int64_t targetDevice, std::int64_t retainedDeviceId=0);
[[nodiscard]] CommitResult commitBuiltinInsert(Store&,const PreparedBuiltinInsert&,Actor=Actor::User);

struct DeviceMoveSource {
    std::int64_t device, track, chain;
};
struct PreparedDeviceMove {
    bool ok = false;
    std::string error;
    AppliedSource source;
    std::vector<OpRequest> requests;
    CommitOptions options;
};
// Move the same device and its state. A missing anchor means the destination
// track's root chain, which is created in the same Undo step when needed.
[[nodiscard]] PreparedDeviceMove prepareDeviceMove(Store&, DeviceMoveSource, std::int64_t track,
                                                    std::optional<std::int64_t> before);
// Move one contiguous range from a single chain in its existing visual order.
[[nodiscard]] PreparedDeviceMove prepareDeviceMove(Store&, std::span<const DeviceMoveSource>,
                                                    std::int64_t track, std::optional<std::int64_t> before);
[[nodiscard]] CommitResult commitDeviceMove(Store&, const PreparedDeviceMove&, Actor = Actor::User);
std::span<const OpDescriptor> builtinReferenceOps();

namespace detail {
enum class DeviceRole { MidiEffect, Instrument, AudioEffect };
struct ChainDevice {
    std::int64_t id, ord;
    DeviceRole role;
    bool rack;
};
DeviceRole deviceRole(std::string_view subtype) noexcept;
std::vector<ChainDevice> deviceChainOrder(const SQLite::Database&, std::int64_t chain);
std::size_t deviceInsertPosition(std::span<const ChainDevice>, DeviceRole, std::size_t requested);
std::int64_t deviceChainOwner(const SQLite::Database&, std::int64_t chain);
void requireDeviceInsertTrack(const SQLite::Database&, std::int64_t track,
                              DeviceRole = DeviceRole::AudioEffect, std::string_view deviceName = {});
}
}
