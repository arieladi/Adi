// SPDX-License-Identifier: GPL-3.0-or-later
//
// adi_tool -- headless CLI for the .adi format.
//
// Deliberately has no JUCE, no audio and no realtime code. If this tool ever
// needs any of those, the layering in ADR-0010 has gone wrong.

#include "adi/blob.hpp"
#include "adi/changeset.hpp"
#include "adi/check.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/digest.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include "adi/version.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        "  adi_tool digest <file>   canonical digest of the project tier\n"
        "                           --full prints it; default prints the id\n"
        "  adi_tool ops             list every registered op\n"
        "  adi_tool check <file>    verify structure and external media hashes\n"
        "  adi_tool collect-export <in.adi> <out.zip>  portable ZIP64 copy\n"
        "  adi_tool extract-media <file.adi>  extract legacy embedded media\n"
        "  adi_tool export <file>   the canonical text projection (ADR-0007)\n"
        "                           --strict fails on a non-canonical order\n"
        "  adi_tool propose <file.adi> <changeset.json> [--apply]\n"
        "                           an agent's changeset: print the diff; with\n"
        "                           --apply, commit it as one undo step (ADR-0148)\n"
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

int cmdDigest(const char* path, bool full) {
    adi::StoreError err = adi::StoreError::Ok;
    auto st = adi::Store::open(path, err, /*readOnly=*/true);
    if (!st) {
        std::printf("cannot open: %s\n", adi::toString(err));
        return 1;
    }
    const auto d = adi::digestProject(*st);
    if (full) std::fputs(d.text.c_str(), stdout);
    std::printf("%s  %d tables  %d rows\n", d.fingerprint.c_str(),
                d.tableCount, d.rowCount);

    // Worth saying out loud, because a digest is exactly the thing someone
    // reaches for to compare two files and then wonders why it ignored a
    // change. It covers the PROJECT, not the session.
    if (full)
        std::printf("\nExcludes the op log, UI state and volatile metadata.\n"
                    "See src/adi/digest.hpp for why each exclusion is there.\n");
    return 0;
}

int cmdOps() {
    // The registry as a catalogue -- MAGDA's system.describe, in a CLI. It is
    // also the only honest answer to "what can the agent do", since the same
    // table backs the UI, scripting and the agent (ADR-0020).
    const auto& reg = adi::OpRegistry::instance();
    int byScope[5] = {0, 0, 0, 0, 0};
    int ephemeral = 0, coalescable = 0;
    for (const auto& o : reg.all()) {
        byScope[static_cast<int>(o.scope)]++;
        if (o.ephemeral) ++ephemeral;
        if (o.coalescable) ++coalescable;
        std::printf("  %-28s %-9s %-14s %s%s\n",
                    std::string(o.name).c_str(),
                    std::string(adi::toString(o.scope)).c_str(),
                    std::string(adi::toString(o.engineImpact)).c_str(),
                    o.inverseOp.empty() ? "" : "-> ",
                    std::string(o.inverseOp).c_str());
    }
    std::printf("\n%zu ops  |  read %d, edit %d, transport %d, session %d, hardware %d\n",
                reg.all().size(), byScope[0], byScope[1], byScope[2], byScope[3],
                byScope[4]);
    std::printf("%d ephemeral, %d coalescable\n", ephemeral, coalescable);
    return 0;
}

int cmdExport(const char* path, bool strict) {
    adi::StoreError err = adi::StoreError::Ok;
    auto st = adi::Store::open(path, err, /*readOnly=*/true);
    if (!st) {
        std::printf("cannot open: %s\n", adi::toString(err));
        return 2;
    }

    const adi::rows::Model model = adi::rows::readModel(*st);
    const auto proj = adi::textproj::project(adi::textproj::buildTree(model));

    std::fwrite(proj.text.data(), 1, proj.text.size(), stdout);

    // Diagnostics go to stderr, never into the file. A lint line in the output
    // is itself content, and content that appears only sometimes breaks the
    // byte-identity the whole projection exists for (TEXT-PROJECTION 3).
    for (const std::string& p : model.problems)
        std::fprintf(stderr, "warning: %s\n", p.c_str());

    if (proj.status != adi::textproj::OrderStatus::Exact) {
        std::fprintf(stderr,
                     "warning: order is not canonical -- largest tied class %zu "
                     "(TEXT-PROJECTION 11)\n",
                     proj.largest_tied_class);
        if (strict) return 1;
    }
    if (strict && !model.problems.empty()) return 1;
    return 0;
}

int cmdCheck(const char* path) {
    adi::StoreError err = adi::StoreError::Ok;
    auto st = adi::Store::open(path, err, /*readOnly=*/true);
    if (!st) {
        std::printf("cannot open: %s\n", adi::toString(err));
        return 2;
    }
    const auto rep = adi::checkProject(*st, true);
    for (const auto& f : rep.findings)
        std::printf("  %-8s %-24s %-34s %s\n", adi::toString(f.severity),
                    f.code.c_str(), f.where.c_str(), f.detail.c_str());
    if (!rep.findings.empty()) std::printf("\n");
    std::printf("%d checks, %d error(s), %d warning(s)\n", rep.checksRun,
                rep.errors, rep.warnings);

    // These are the things a FOREIGN KEY cannot express and a blob hides.
    // Saying so keeps a clean result from being read as "the file is
    // perfect" when it means "nothing unenforceable is broken".
    if (rep.clean())
        std::printf("ok -- polymorphic references resolve, blobs parse, the undo\n"
                    "tree is acyclic. See src/adi/check.hpp for the full list.\n");
    return rep.errors ? 1 : 0;
}

/// ADR-0145 d9, ADR-0148. Exit codes: 0 previewed (or applied), 1 refused or
/// failed, 3 stale -- the file moved on and the changeset must be rebuilt.
int cmdPropose(const char* file, const char* changesetPath, bool doApply) {
    using namespace adi;
    const auto path = std::filesystem::absolute(file);

    // A preview must not write the file, and opening an older 1.x file for
    // writing upgrades it (ADR-0144). So a preview-only run looks first.
    if (!doApply) {
        try {
            SQLite::Database peek(path.string(), SQLite::OPEN_READONLY);
            const int uv = peek.execAndGet("PRAGMA user_version").getInt();
            if (uv / 1000 == kSchemaMajor && uv % 1000 < kSchemaMinor) {
                std::fprintf(stderr,
                             "propose: %s is schema 1.%d; previewing would upgrade it to 1.%d. "
                             "Open it for writing first (or pass --apply).\n",
                             file, uv % 1000, kSchemaMinor);
                return 1;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "propose: cannot read %s: %s\n", file, e.what());
            return 1;
        }
    }

    std::ifstream in(changesetPath, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "propose: cannot read %s\n", changesetPath);
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const Payload json = Payload::parse(text, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        std::fprintf(stderr, "propose: %s is not valid JSON\n", changesetPath);
        return 1;
    }

    StoreError err = StoreError::Ok;
    auto store = Store::open(path, err);
    if (!store || store->readOnly()) {
        std::fprintf(stderr, "propose: cannot open %s for writing: %s\n", file, toString(err));
        return 1;
    }
    std::string why;
    const auto cs = agent::changesetFromJson(*store, json, why);
    if (!cs) {
        std::fprintf(stderr, "propose: %s\n", why.c_str());
        return 1;
    }

    const auto p = agent::preview(*store, *cs);
    if (!p.ok) {
        std::fprintf(stderr, "propose: refused (%s): %s\n", agent::toString(p.refusal), p.error.c_str());
        return p.refusal == agent::Refusal::Stale ? 3 : 1;
    }
    std::printf("request: %s\nby: agent (%s)\n\n", cs->request.c_str(), cs->actorDetail.c_str());
    std::printf("%s", p.unifiedDiff.empty() ? "(the text projection does not change)\n"
                                            : p.unifiedDiff.c_str());
    std::printf("\n%zu change(s):\n", p.changes.size());
    for (const auto& c : p.changes) {
        std::printf("  %s  [%s %s%s]\n", c.label.c_str(), c.opType.c_str(), c.targetKind.c_str(),
                    c.targetId ? (" " + std::to_string(*c.targetId)).c_str() : "");
        std::printf("      before %s\n      after  %s\n",
                    c.before ? c.before->dump().c_str() : "(none)",
                    c.after ? c.after->dump().c_str() : "(none)");
    }
    if (!doApply) {
        std::printf("\nnot applied: run again with --apply to commit it as one undo step\n");
        return 0;
    }

    const auto r = agent::apply(*store, *cs);
    if (!r.ok) {
        std::fprintf(stderr, "propose: apply refused (%s): %s\n", agent::toString(r.refusal),
                     r.error.c_str());
        return r.refusal == agent::Refusal::Stale ? 3 : 1;
    }
    if (!store->close()) {
        std::fprintf(stderr, "propose: applied, but the close/checkpoint failed\n");
        return 1;
    }
    std::printf("\napplied: txn %lld, one undo step\n", static_cast<long long>(r.txnId));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];

    if (cmd == "collect-export" || cmd == "extract-media") {
        if (argc != (cmd == "collect-export" ? 4 : 3)) return usage();
        adi::media::FileResult result;
        if (cmd == "collect-export") result = adi::media::collectExport(argv[2], argv[3]);
        else {
            adi::StoreError error;
            auto store = adi::Store::open(std::filesystem::absolute(argv[2]), error);
            if (!store) result = {false, adi::toString(error)};
            else {
                result = adi::media::extractMedia(*store);
                if (result.ok && !store->close()) result = {false, "database close/checkpoint failed"};
            }
        }
        if (!result.ok) std::fprintf(stderr, "%s: %s\n", cmd.c_str(), result.error.c_str());
        return result.ok ? 0 : 1;
    }
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
    if (cmd == "digest") {
        if (argc < 3) return usage();
        return cmdDigest(argv[2], argc > 3 && std::string(argv[3]) == "--full");
    }
    if (cmd == "ops") {
        return cmdOps();
    }
    if (cmd == "export") {
        if (argc < 3) return usage();
        bool strict = false;
        for (int i = 3; i < argc; ++i)
            if (std::string(argv[i]) == "--strict") strict = true;
        return cmdExport(argv[2], strict);
    }
    if (cmd == "propose") {
        if (argc < 4 || argc > 5) return usage();
        const bool doApply = argc == 5 && std::string(argv[4]) == "--apply";
        if (argc == 5 && !doApply) return usage();
        return cmdPropose(argv[2], argv[3], doApply);
    }
    if (cmd == "check") {
        if (argc < 3) return usage();
        return cmdCheck(argv[2]);
    }

    std::printf("unknown command: %s\n\n", cmd.c_str());
    return usage();
}
