// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_tool -- headless CLI for the .adi format.
//
// Deliberately has no JUCE, no audio and no realtime code. If this tool ever
// needs any of those, the layering in ADR-0010 has gone wrong.

#include "adi/blob.hpp"
#include "adi/store.hpp"
#include "adi/version.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

namespace {

int usage() {
    std::printf(
        "adi_tool -- .adi format utility\n"
        "\n"
        "  adi_tool layout          print the on-disk record layouts (SPEC 6.3)\n"
        "  adi_tool versions        print library versions\n"
        "  adi_tool create <file>   create an empty .adi project\n"
        "  adi_tool info <file>     inspect an existing .adi\n"
        "\n"
        "Ops arrive next; see docs/OPS.md.\n");
    return 2;
}

void printLayout() {
    using namespace adi;
    std::printf("SPEC 6.3 record layouts, as this build compiled them:\n\n");
    std::printf("  %-18s %3zu bytes\n", "StreamHeader", sizeof(StreamHeader));
    std::printf("  %-18s %3zu bytes  (ANOT)\n", "NoteRecord", sizeof(NoteRecord));
    std::printf("  %-18s %3zu bytes  (AAUT)\n", "AutomationPoint", sizeof(AutomationPoint));
    std::printf("  %-18s %3zu bytes  (AEXP)\n", "ExpressionPoint", sizeof(ExpressionPoint));
    std::printf(
        "\nThese are static_asserts, not runtime checks -- a mismatch on this\n"
        "platform would have failed the build rather than printed here.\n");
}

void printVersions() {
    std::printf("adi_tool  %s\n", adi::kVersion);
    std::printf("sqlite3   %s\n", SQLite::getLibVersion());
    std::printf("SQLiteCpp %d.%d.%d\n", SQLITECPP_VERSION_NUMBER / 1000000,
                (SQLITECPP_VERSION_NUMBER / 1000) % 1000, SQLITECPP_VERSION_NUMBER % 1000);
    std::printf("C++       %ld\n", static_cast<long>(__cplusplus));
    std::printf("schema    %d.%d\n", adi::kSchemaMajor, adi::kSchemaMinor);
}

int cmdCreate(const char* path) {
    adi::StoreError err = adi::StoreError::Ok;
    auto st = adi::Store::create(path, err);
    if (!st) {
        std::printf("create failed: %s\n", adi::toString(err));
        return 1;
    }
    const int major = st->schemaMajor();
    const int minor = st->schemaMinor();
    const bool closed = st->close();
    std::printf("created %s  (schema %d.%d)%s\n", path, major, minor,
                closed ? "" : "  WARNING: close did not complete cleanly");
    return closed ? 0 : 1;
}

int cmdInfo(const char* path) {
    adi::StoreError err = adi::StoreError::Ok;
    auto st = adi::Store::open(path, err, /*readOnly=*/true);
    if (!st) {
        std::printf("cannot open: %s\n", adi::toString(err));
        return 1;
    }

    std::printf("%-14s %s\n", "file", path);
    std::printf("%-14s %d.%d%s\n", "schema", st->schemaMajor(), st->schemaMinor(),
                err == adi::StoreError::SchemaTooNew
                    ? "   (newer than this build -- read only)"
                    : "");
    try {
        auto& db = st->db();
        const auto name =
            db.execAndGet("SELECT IFNULL(MAX(name), '(unnamed)') FROM project").getString();
        std::printf("%-14s %s\n", "name", name.c_str());

        const char* counts[][2] = {
            {"tracks", "SELECT COUNT(*) FROM tracks"},
            {"clips", "SELECT COUNT(*) FROM clips"},
            {"event streams", "SELECT COUNT(*) FROM event_streams"},
            {"automation", "SELECT COUNT(*) FROM automation_data"},
            {"ops", "SELECT COUNT(*) FROM ops"},
            {"media", "SELECT COUNT(*) FROM media_files"},
        };
        for (const auto& c : counts)
            std::printf("%-14s %d\n", c[0], db.execAndGet(c[1]).getInt());
    } catch (const std::exception& e) {
        std::printf("read error: %s\n", e.what());
        return 1;
    }

    // SPEC 3.3. A cleanly closed .adi is exactly one file, so a sidecar next to
    // it means either the project is open elsewhere or the last session died.
    // Saying so is the whole point of the rule -- the alternative is a project
    // that opens cleanly and is quietly missing its last hour.
    if (std::filesystem::exists(std::string(path) + "-wal"))
        std::printf(
            "\nNOTE: a -wal sidecar is present. Either this project is open in\n"
            "another process, or the last session did not close cleanly. Copying\n"
            "the .adi alone right now would lose everything since the last\n"
            "checkpoint.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];

    if (cmd == "layout") {
        printLayout();
        return 0;
    }
    if (cmd == "versions") {
        printVersions();
        return 0;
    }
    if (cmd == "create") {
        if (argc < 3) return usage();
        return cmdCreate(argv[2]);
    }
    if (cmd == "info") {
        if (argc < 3) return usage();
        return cmdInfo(argv[2]);
    }

    std::printf("unknown command: %s\n\n", cmd.c_str());
    return usage();
}
