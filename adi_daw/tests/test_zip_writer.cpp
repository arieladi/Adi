// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/zip_writer.hpp"
#include "temp_directory.hpp"
#include <miniz.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
namespace {
// Keep the standard getenv API while silencing MSVC C4996 at this one site,
// matching win's test_wav_file helper (2026-09-24).
const char* envVar(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
using namespace adi::media;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
struct Reader {
    std::ifstream input;
    mz_zip_archive zip{};
    bool ready = false;
    static size_t read(void* opaque, mz_uint64 offset, void* data, size_t size) {
        auto& stream = static_cast<Reader*>(opaque)->input;
        stream.clear(); stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<size_t>(stream.gcount());
    }
    explicit Reader(const std::filesystem::path& path) : input(path, std::ios::binary | std::ios::ate) {
        if (!input) return;
        const auto length = input.tellg();
        if (length < 0) return;
        zip.m_pRead = read; zip.m_pIO_opaque = this;
        ready = mz_zip_reader_init(&zip, static_cast<mz_uint64>(length), 0) != 0;
    }
    ~Reader() { if (ready) mz_zip_reader_end(&zip); }
};
std::string utf8(std::u8string_view text) { return {reinterpret_cast<const char*>(text.data()), text.size()}; }
void write(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("test source write failed");
}
void roundtrip() {
    adi::test::TempDirectory tmp("zip_writer", "roundtrip");
    const auto source = tmp.path() / std::filesystem::path(u8"音-שלום.wav");
    const auto archive = tmp.path() / std::filesystem::path(u8"ארכיון.zip");
    std::vector<char> bytes(3 * 65536 + 123);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 113);
    write(source, bytes);
    const auto unicode = utf8(u8"audio/音-שלום.wav");
    ZipWriter writer(archive);
    check(writer.error() == ZipError::none, "open Unicode archive filesystem path");
    check(writer.addFile(source, unicode), "add STORE default from Unicode source path");
    check(writer.addFile(source, "compressed.wav", ZipCompression::deflate), "add deflate file");
    write(tmp.path() / "empty", {});
    check(writer.addFile(tmp.path() / "empty", "empty"), "add empty file");
    check(writer.finish(), "finalize archive");
    check(writer.finish(), "finish is idempotent");
    Reader reader(archive);
    check(reader.ready && mz_zip_reader_get_num_files(&reader.zip) == 3, "read back completed central directory");
    if (!reader.ready) return;
    for (mz_uint i = 0; i < 3; ++i) {
        mz_zip_archive_file_stat stat{};
        check(mz_zip_reader_file_stat(&reader.zip, i, &stat) != 0, "reader file stat");
        check(stat.m_method == (i == 1 ? 8 : 0), "STORE default and deflate optional methods");
        const auto expectedSize = i == 2 ? 0 : bytes.size();
        check(stat.m_uncomp_size == expectedSize, "streamed uncompressed length");
        std::vector<char> actual(expectedSize ? expectedSize : 1);
        check(mz_zip_reader_extract_to_mem(&reader.zip, i, actual.data(), expectedSize, 0) != 0 &&
              (expectedSize == 0 || std::memcmp(actual.data(), bytes.data(), expectedSize) == 0),
              "miniz roundtrip bytes and CRC");
        if (i == 0) check(std::string(stat.m_filename) == unicode && (stat.m_bit_flag & 0x800) != 0,
                          "Unicode name preserved with UTF-8 flag");
    }
    std::array<bool, 4> matches{};
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
        const auto output = tmp.path() / ("parallel" + std::to_string(i) + ".zip");
        ZipWriter independent(output);
        if (!independent.addFile(source, "audio", ZipCompression::deflate) || !independent.finish()) return;
        Reader verify(output);
        std::vector<char> actual(bytes.size());
        matches[i] = verify.ready && mz_zip_reader_extract_to_mem(&verify.zip, 0, actual.data(), actual.size(), 0) && actual == bytes;
    });
    for (auto& thread : threads) thread.join();
    check(matches[0] && matches[1] && matches[2] && matches[3], "independent concurrent ZIP writers roundtrip");
    if (const auto* sample = envVar("ADI_ZIP_SAMPLE"))
        std::filesystem::copy_file(archive, std::filesystem::path(sample), std::filesystem::copy_options::overwrite_existing);
    check(!writer.addFile(source, "late") && writer.error() == ZipError::state, "add after finish rejected");
}
void errors() {
    adi::test::TempDirectory tmp("zip_writer", "errors");
    const auto source = tmp.path() / "source"; write(source, {'a'});
    {
        ZipWriter writer(tmp.path() / "missing" / "out.zip");
        check(writer.error() == ZipError::open && !writer.finish(), "output open failure reported");
    }
    {
        ZipWriter writer(tmp.path() / "missing.zip");
        check(!writer.addFile(tmp.path() / "absent", "audio/missing") && writer.error() == ZipError::open,
              "missing source reported without throwing");
        check(!writer.finish() && !writer.addFile(source, "later"), "failed writer cannot publish partial success");
    }
    for (const std::string name : {"", "/root", "../escape", "audio/../escape", "a\\b", "C:/a", "a//b", "./a", "a/"}) {
        ZipWriter writer(tmp.path() / "bad.zip");
        check(!writer.addFile(source, name) && writer.error() == ZipError::invalidPath, "unsafe archive path rejected");
    }
    {
        ZipWriter writer(tmp.path() / "null.zip");
        check(!writer.addFile(source, std::string("a\0b", 3)), "embedded NUL archive name rejected");
    }
    {
        const auto output = tmp.path() / "self.zip";
        ZipWriter writer(output);
        check(!writer.addFile(output, "self") && writer.error() == ZipError::invalidPath, "output cannot be its own source");
    }
    {
        ZipWriter writer(tmp.path() / "directory.zip");
        check(!writer.addFile(tmp.path(), "directory") && writer.error() == ZipError::open, "directory source refused");
    }
    {
        ZipWriter writer(tmp.path() / "empty.zip");
        check(writer.finish(), "empty archive finalizes");
        Reader reader(tmp.path() / "empty.zip");
        check(reader.ready && mz_zip_reader_get_num_files(&reader.zip) == 0, "empty archive read back");
    }
}
struct LargeCompare {
    mz_uint64 size;
    mz_uint64 seen = 0;
    bool equal = true;
    static size_t compare(void* opaque, mz_uint64 offset, const void* data, size_t length) {
        auto& self = *static_cast<LargeCompare*>(opaque);
        const auto* bytes = static_cast<const unsigned char*>(data);
        self.equal &= offset == self.seen && offset + length <= self.size;
        for (size_t i = 0; i < length; ++i) {
            const auto position = offset + i;
            const unsigned char expected = position == 0 ? 'A' : (position == self.size - 1 ? 'Z' : 0);
            if (bytes[i] != expected) { self.equal = false; return 0; }
        }
        self.seen += length;
        return length;
    }
};
void aggregateBig() {
    adi::test::TempDirectory tmp("zip_writer", "aggregate64");
    constexpr mz_uint64 size = (mz_uint64{1} << 31) + 123;
    const auto source = tmp.path() / "half";
    {
        std::ofstream out(source, std::ios::binary);
        out.put('A'); out.seekp(static_cast<std::streamoff>(size - 1)); out.put('Z');
        check(static_cast<bool>(out), "create two-GiB aggregate source");
    }
    const auto archive = tmp.path() / "aggregate.zip";
    ZipWriter writer(archive);
    check(writer.addFile(source, "first"), "aggregate first entry below 4 GiB");
    check(writer.addFile(source, "second"), "aggregate second entry crosses archive 4 GiB");
    check(writer.finish(), "ZIP64 aggregate archive finalizes without oversized entry");
    Reader reader(archive);
    check(reader.ready && mz_zip_is_zip64(&reader.zip), "aggregate ZIP64 end records");
    if (!reader.ready) return;
    LargeCompare compare{size};
    check(mz_zip_reader_extract_to_callback(&reader.zip, 1, LargeCompare::compare, &compare, 0) &&
          compare.equal && compare.seen == size, "aggregate ZIP64 second entry CRC and bytes");
}
void big() {
    if (const auto* flag = envVar("ADI_ZIP_BIG"); !flag || std::string_view(flag) != "1") return;
    adi::test::TempDirectory tmp("zip_writer", "zip64");
    constexpr mz_uint64 size = (mz_uint64{1} << 32) + 123;
    const auto source = tmp.path() / "large";
    {
        std::ofstream out(source, std::ios::binary);
        out.put('A'); out.seekp(static_cast<std::streamoff>(size - 1)); out.put('Z');
        check(static_cast<bool>(out), "create real >4 GiB logical source");
    }
    const auto archive = tmp.path() / "large.zip";
    ZipWriter writer(archive);
    check(writer.addFile(source, "large.bin"), "ZIP64 >4 GiB STORE entry streamed");
    write(tmp.path() / "tail", {'t'});
    check(writer.addFile(tmp.path() / "tail", "tail"), "entry after 4 GiB archive offset");
    check(writer.finish(), "ZIP64 archive finalized");
    Reader reader(archive);
    check(reader.ready && mz_zip_is_zip64(&reader.zip), "ZIP64 end records present");
    if (!reader.ready) return;
    mz_zip_archive_file_stat stat{};
    check(mz_zip_reader_file_stat(&reader.zip, 0, &stat) && stat.m_uncomp_size == size && stat.m_comp_size == size,
          "ZIP64 64-bit entry sizes preserved");
    LargeCompare compare{size};
    check(mz_zip_reader_extract_to_callback(&reader.zip, 0, LargeCompare::compare, &compare, 0) &&
          compare.equal && compare.seen == size, "ZIP64 entire payload and CRC roundtrip with bounded memory");
    char tail = 0;
    check(mz_zip_reader_file_stat(&reader.zip, 1, &stat) && stat.m_local_header_ofs > (mz_uint64{1} << 32) &&
          mz_zip_reader_extract_to_mem(&reader.zip, 1, &tail, 1, 0) && tail == 't', "ZIP64 central directory offsets above 4 GiB");
}
}
int main() {
    std::setbuf(stdout, nullptr);
    try { roundtrip(); errors(); big();
        if (const auto* flag = envVar("ADI_ZIP_BIG"); flag && std::string_view(flag) == "1") aggregateBig();
    } catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
