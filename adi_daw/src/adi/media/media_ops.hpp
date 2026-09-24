// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <filesystem>
namespace adi::media {
struct MediaResult {
    bool ok = false;
    std::string error;
    std::int64_t mediaId = 0;
    bool existing = false;
};
// Off-thread path-taking entry points. Relative inputs are project-folder
// relative. Hashing and metadata capture precede deterministic journal apply.
MediaResult importMedia(Store&, const std::filesystem::path&, std::int64_t id,
                        std::int64_t importedUtc, Actor actor = Actor::User);
MediaResult relinkMedia(Store&, std::int64_t id, const std::filesystem::path&,
                        Actor actor = Actor::User);
std::filesystem::path projectFolder(const Store&);
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
