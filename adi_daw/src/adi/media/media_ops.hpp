// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <filesystem>
#include <optional>
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
MediaResult importMedia(Store&, const std::filesystem::path&, std::int64_t id,
                        std::int64_t importedUtc, Actor actor = Actor::User);
MediaResult relinkMedia(Store&, std::int64_t id, const std::filesystem::path&,
                        Actor actor = Actor::User);
// The folder SQLite names the database file in: the base a rel_path is
// resolved against. On unix SQLite resolves every symlink in that name (macOS:
// /var is /private/var), so make rel_paths relative to this spelling.
std::filesystem::path projectFolder(const Store&);
std::filesystem::path projectFolder(const SQLite::Database&);
std::string pathUtf8(const std::filesystem::path&);
std::filesystem::path pathFromUtf8(const std::string&);
// Resolution verifies the first existing candidate; a mismatch is fatal.
std::filesystem::path resolveMedia(const std::filesystem::path& folder,
                                  const Payload& row, std::string& error);
Payload mediaRow(SQLite::Database&, std::int64_t id);
// Registry callbacks: captured payloads only, no filesystem access.
bool importApply(OpContext&, const Payload&, std::string&);
bool importInverse(OpContext&, const Payload&, Payload&, std::string&);
bool unlinkApply(OpContext&, const Payload&, std::string&);
bool unlinkInverse(OpContext&, const Payload&, Payload&, std::string&);
bool relinkApply(OpContext&, const Payload&, std::string&);
bool relinkInverse(OpContext&, const Payload&, Payload&, std::string&);
} // namespace adi::media
