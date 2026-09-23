// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <memory>
#include <string_view>

namespace adi::media {
enum class ZipError { none, open, read, write, invalidPath, archive, resource, state };
enum class ZipCompression { store, deflate };
// Off-callback, single-owner writer. UTF-8 relative archive paths use '/'.
// Filesystem timestamps are intentionally not copied.
// ZIP64 end records are used even for small archives (ADR-0127).
// Sources must remain unchanged while addFile runs. Payload memory is bounded;
// miniz retains central-directory metadata proportional to the entry count.
// Errors are sticky. finish() is required to publish a complete archive;
// destruction only releases resources. On failure the caller removes the partial
// output. No exception escapes these methods. Construction truncates output.
class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& output) noexcept;
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    [[nodiscard]] bool addFile(const std::filesystem::path& source,
                               std::string_view archivePath,
                               ZipCompression compression = ZipCompression::store) noexcept;
    [[nodiscard]] bool finish() noexcept;
    [[nodiscard]] ZipError error() const noexcept { return error_; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ZipError error_ = ZipError::none;
    bool fail(ZipError error) noexcept;
};
} // namespace adi::media
