// SPDX-License-Identifier: GPL-3.0-or-later
#include "zip_writer.hpp"
#include <miniz.h>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace adi::media {
namespace {
bool validPath(std::string_view name) noexcept {
    if (name.empty() || name.size() > 65535 || name.front() == '/' || name.back() == '/' ||
        name.find_first_of("\\:") != name.npos || name.find('\0') != name.npos) return false;
    while (!name.empty()) {
        const auto end = name.find('/');
        const auto part = name.substr(0, end);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == name.npos) break;
        name.remove_prefix(end + 1);
    }
    return true;
}
struct Input {
    std::ifstream stream;
    mz_uint64 size = 0;
    mz_uint64 read = 0;
    bool failed = false;
    static size_t callback(void* opaque, mz_uint64 offset, void* buffer, size_t size) noexcept {
        auto& self = *static_cast<Input*>(opaque);
        if (offset != self.read || offset > self.size) { self.failed = true; return 0; }
        const auto count = static_cast<std::streamsize>(std::min<mz_uint64>(
            std::min<size_t>(size, 65536), self.size - offset));
        if (count == 0) return 0;
        try {
            self.stream.read(static_cast<char*>(buffer), count);
            if (self.stream.gcount() != count || !self.stream) { self.failed = true; return 0; }
            self.read += static_cast<mz_uint64>(count);
            return static_cast<size_t>(count);
        } catch (...) { self.failed = true; return 0; }
    }
};
}
struct ZipWriter::Impl {
    std::filesystem::path path;
    std::ofstream output;
    mz_zip_archive zip{};
    bool started = false;
    bool finished = false;
    bool writeFailed = false;
    ~Impl() { if (started) mz_zip_writer_end(&zip); }
    static size_t write(void* opaque, mz_uint64 offset, const void* buffer, size_t size) noexcept {
        auto& self = *static_cast<Impl*>(opaque);
        try {
            if (offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
                size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
                self.writeFailed = true; return 0;
            }
            self.output.seekp(static_cast<std::streamoff>(offset));
            self.output.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
            if (!self.output) { self.writeFailed = true; return 0; }
            return size;
        } catch (...) { self.writeFailed = true; return 0; }
    }
};
bool ZipWriter::fail(ZipError error) noexcept {
    if (error_ == ZipError::none) error_ = error;
    return false;
}
ZipWriter::ZipWriter(const std::filesystem::path& output) noexcept {
    try {
        impl_ = std::make_unique<Impl>();
        impl_->path = output;
        impl_->output.open(output, std::ios::binary | std::ios::trunc);
        if (!impl_->output) { fail(ZipError::open); return; }
        impl_->zip.m_pIO_opaque = impl_.get();
        impl_->zip.m_pWrite = Impl::write;
        // Start ZIP64 deliberately: miniz's automatic promotion can miss an
        // archive crossing 4 GiB through several smaller entries (ADR-0127).
        impl_->started = mz_zip_writer_init_v2(&impl_->zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64) != 0;
        if (!impl_->started) fail(ZipError::archive);
    } catch (...) { fail(ZipError::resource); }
}
ZipWriter::~ZipWriter() = default;
bool ZipWriter::addFile(const std::filesystem::path& source, std::string_view archivePath,
                        ZipCompression compression) noexcept {
    if (error_ != ZipError::none) return false;
    if (!impl_ || impl_->finished) return fail(ZipError::state);
    if (!validPath(archivePath)) return fail(ZipError::invalidPath);
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec) || ec) return fail(ZipError::open);
        if (std::filesystem::equivalent(source, impl_->path, ec) || ec) return fail(ZipError::invalidPath);
        const std::string name(archivePath);
        Input input;
        input.stream.open(source, std::ios::binary | std::ios::ate);
        if (!input.stream) return fail(ZipError::open);
        const auto end = input.stream.tellg();
        if (end < 0) return fail(ZipError::read);
        input.size = static_cast<mz_uint64>(end);
        input.stream.seekg(0);
        const mz_uint level = compression == ZipCompression::store ? 0u : 6u;
        const bool ok = mz_zip_writer_add_read_buf_callback(&impl_->zip, name.c_str(), Input::callback,
            &input, input.size, nullptr, nullptr, 0, level, nullptr, 0, nullptr, 0) != 0;
        if (input.failed || input.read != input.size) return fail(ZipError::read);
        if (impl_->writeFailed) return fail(ZipError::write);
        if (!ok) return fail(ZipError::archive);
        return true;
    } catch (...) { return fail(ZipError::resource); }
}
bool ZipWriter::finish() noexcept {
    if (error_ != ZipError::none) return false;
    if (!impl_) return fail(ZipError::state);
    if (impl_->finished) return true;
    try {
        const bool ok = mz_zip_writer_finalize_archive(&impl_->zip) != 0;
        if (impl_->writeFailed) return fail(ZipError::write);
        if (!ok) return fail(ZipError::archive);
        impl_->output.flush();
        impl_->output.close();
        if (!impl_->output) return fail(ZipError::write);
        impl_->finished = true;
        return true;
    } catch (...) { return fail(ZipError::write); }
}
} // namespace adi::media
