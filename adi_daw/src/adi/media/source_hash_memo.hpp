// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"

#include <map>
#include <mutex>

namespace adi::media {

struct SourceHash {
    HashResult digest;
    bool reused = false;
};

// Session/cache-owner hint, independent of decoded WAV lifetime. Only a digest
// read from stable source bytes enters the memo. Compare it against the expected
// project hash on every use. Off-callback, safe to share by reader threads.
// End-of-decode verification should still hash the source fresh.
class SourceHashMemo {
public:
    [[nodiscard]] SourceHash verify(const std::filesystem::path& source,
                                    std::stop_token stop = {}) noexcept;

private:
    struct Stamp {
        std::uintmax_t bytes;
        std::filesystem::file_time_type modified;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };
    struct Version {
        FileIdentity identity;
        Stamp stamp;
        friend bool operator==(const Version&, const Version&) = default;
    };
    struct Entry {
        Stamp stamp;
        std::string hex;
    };
    static std::optional<Version> version(const std::filesystem::path&);
    std::mutex mutex_;
    std::map<FileIdentity, Entry> hashes_;
};

} // namespace adi::media
