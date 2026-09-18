// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_tool -- headless CLI for the .adi format.
//
// Deliberately has no JUCE, no audio and no realtime code. If this tool ever
// needs any of those, the layering in ADR-0010 has gone wrong.

#include "adi/blob.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int usage() {
    std::printf(
        "adi_tool -- .adi format utility\n"
        "\n"
        "  adi_tool layout     print the on-disk record layouts (SPEC 6.3)\n"
        "  adi_tool versions   print library versions\n"
        "\n"
        "More commands arrive with the store layer; see docs/OPS.md.\n");
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
    std::printf("sqlite3   %s\n", SQLite::getLibVersion());
    std::printf("SQLiteCpp %d.%d.%d\n", SQLITECPP_VERSION_NUMBER / 1000000,
                (SQLITECPP_VERSION_NUMBER / 1000) % 1000, SQLITECPP_VERSION_NUMBER % 1000);
    std::printf("C++       %ld\n", static_cast<long>(__cplusplus));
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

    std::printf("unknown command: %s\n\n", cmd.c_str());
    return usage();
}
