// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
namespace adi::audio { class DecodeCache; }
namespace adi::media {
struct AudioInfo {
    std::int64_t sampleRate = 0, channels = 0, frames = 0;
};
struct MediaResult {
    bool ok = false;
    std::string error;
    std::int64_t mediaId = 0;
    bool existing = false;
    std::optional<AudioInfo> audio = {};
};
// Off-thread path-taking entry points. Relative inputs are project-folder
// relative. Hashing and metadata capture precede deterministic journal apply.
// Captures without committing. Append a nonempty request.opType: a new import,
// or a relink if a reused id has missing/stale paths. existing means id reused;
// audio holds probed dimensions even for a reused media id.
// Non-audio files retain nullable audio metadata. Known corrupt audio fails.
MediaResult prepareImport(Store&, const std::filesystem::path&, std::int64_t id,
                          std::int64_t importedUtc, OpRequest&, audio::DecodeCache* = nullptr);
MediaResult prepareImport(Store&, const std::filesystem::path&, std::int64_t id,
                          std::int64_t importedUtc, OpRequest&, audio::DecodeCache*,std::stop_token);
// Header-only import for the normal unwarped material command. Exact container
// dimensions are captured without validating the whole PCM payload; a later
// decode error is surfaced by playback. Unknown lengths use full validation.
MediaResult prepareImport(Store&, const std::filesystem::path&, std::int64_t id,
                          std::int64_t importedUtc, OpRequest&, audio::DecodeCache*,std::stop_token,bool headerOnly);
MediaResult importMedia(Store&, const std::filesystem::path&, std::int64_t id,
                        std::int64_t importedUtc, Actor actor = Actor::User);
MediaResult relinkMedia(Store&, std::int64_t id, const std::filesystem::path&,
                        Actor actor = Actor::User);
struct RelinkTarget {
    std::int64_t mediaId = 0;
    std::filesystem::path path;
};
// Owner/off-callback: verify every selected candidate before committing any
// paths. One transaction/Undo step, preserving media identities and metadata.
// Empty selections, duplicate ids, embedded media and any mismatch refuse the
// whole batch. This does not discover candidates or replace different content.
// mediaId is populated only for a one-target result.
MediaResult relinkMediaBatch(Store&, std::span<const RelinkTarget>,
                             Actor actor = Actor::User);
// The folder SQLite names the database file in: the base a rel_path is
// resolved against. On unix SQLite resolves every symlink in that name (macOS:
// /var is /private/var), so make rel_paths relative to this spelling.
std::filesystem::path projectFolder(const Store&);
std::filesystem::path projectFolder(const SQLite::Database&);
std::string pathUtf8(const std::filesystem::path&);
std::filesystem::path pathFromUtf8(const std::string&);
// Resolve by content, trying the relative location first. A distinct original
// location can recover a link when moving the project changes the relative base.
std::filesystem::path resolveMedia(const std::filesystem::path& folder,
                                  const Payload& row, std::string& error);
// An empty result without an error means no candidate exists or stop fired.
std::filesystem::path resolveMedia(const std::filesystem::path& folder,
                                  const std::filesystem::path& relative,
                                  const std::filesystem::path& hint,
                                  const std::string& originalName, const std::string& hash,
                                  std::string& error, std::stop_token stop = {});
// A Browser reveal uses the pool's existing file path without reading audio bytes.
std::filesystem::path mediaPathForBrowser(const Store&, std::int64_t id, std::string& error);
Payload mediaRow(SQLite::Database&, std::int64_t id);
// Registry callbacks: captured payloads only, no filesystem access.
bool importApply(OpContext&, const Payload&, std::string&);
bool importInverse(OpContext&, const Payload&, Payload&, std::string&);
bool unlinkApply(OpContext&, const Payload&, std::string&);
bool unlinkInverse(OpContext&, const Payload&, Payload&, std::string&);
bool relinkApply(OpContext&, const Payload&, std::string&);
bool relinkInverse(OpContext&, const Payload&, Payload&, std::string&);
} // namespace adi::media
