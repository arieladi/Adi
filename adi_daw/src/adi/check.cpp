// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/check.hpp"

#include "adi/blob.hpp"
#include "adi/store.hpp"
#include "adi/media/media_ops.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <map>
#include <set>
#include <string>
#include <unordered_set>

namespace adi {

const char* toString(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Info:    return "info";
    }
    return "?";
}

namespace {

struct Ctx {
    SQLite::Database& db;
    CheckReport& r;

    void add(Severity sev, std::string code, std::string where, std::string detail) {
        if (sev == Severity::Error) ++r.errors;
        if (sev == Severity::Warning) ++r.warnings;
        r.findings.push_back({sev, std::move(code), std::move(where), std::move(detail)});
    }
    void ran() { ++r.checksRun; }
};

/// A polymorphic reference site: a (kind, id) pair whose target table is chosen
/// at runtime, which is exactly what a FOREIGN KEY cannot express.
struct PolyRef {
    const char* table;
    const char* idOfRow;    // how to name the offending row in a message
    const char* kindCol;
    const char* idCol;
    bool idNullable;
};

const PolyRef kPolyRefs[] = {
    {"routing", "id", "src_kind", "src_id", false},
    {"routing", "id", "dst_kind", "dst_id", false},
    {"automation_lanes", "id", "owner_kind", "owner_id", false},
    {"ui_view", "id", "scope_kind", "scope_id", true},
    {"extensions", "id", "scope_kind", "scope_id", true},
    {"controller_maps", "id", "target_kind", "target_id", true},
    {"ops", "seq", "target_kind", "target_id", true},
};

/// kind -> table. Anything not here is either external hardware or a scope name
/// that does not denote a row at all.
const std::map<std::string, std::string> kKindToTable = {
    {"track", "tracks"},   {"device", "devices"}, {"clip", "clips"},
    {"routing", "routing"}, {"lane", "lanes"},    {"media", "media_files"},
};

/// Kinds that deliberately name something outside the database. A failed lookup
/// for one of these is NORMAL — the project opened on different hardware, or
/// the scope is a UI concept rather than a row (SPEC §6.7).
const std::set<std::string> kNotRows = {
    "hw_in", "hw_out",   // hardware port indices on the current audio device
    "project",           // the singleton; id is meaningless
    "editor",            // a UI surface, not a row
    "",                  // ops.target_kind defaults to empty
};

// --- 1. what SQLite can tell us itself -----------------------------------------

void checkSqlite(Ctx& c) {
    c.ran();
    try {
        SQLite::Statement st(c.db, "PRAGMA integrity_check");
        while (st.executeStep()) {
            const auto v = st.getColumn(0).getString();
            if (v != "ok")
                c.add(Severity::Error, "sqlite.integrity", "database", v);
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "sqlite.integrity", "database", e.what());
    }

    c.ran();
    try {
        SQLite::Statement st(c.db, "PRAGMA foreign_key_check");
        while (st.executeStep()) {
            c.add(Severity::Error, "sqlite.foreignKey",
                  st.getColumn(0).getString() + "#" + st.getColumn(1).getString(),
                  "row violates its foreign key into " + st.getColumn(2).getString());
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "sqlite.foreignKey", "database", e.what());
    }
}

// --- 2. the polymorphic references ----------------------------------------------

void checkPolyRefs(Ctx& c) {
    for (const auto& ref : kPolyRefs) {
        c.ran();
        try {
            SQLite::Statement st(c.db, std::string("SELECT ") + ref.idOfRow + ", " +
                                           ref.kindCol + ", " + ref.idCol + " FROM " +
                                           ref.table);
            while (st.executeStep()) {
                const auto rowId = st.getColumn(0).getInt64();
                const auto kind = st.getColumn(1).getString();
                if (st.getColumn(2).isNull()) {
                    if (!ref.idNullable)
                        c.add(Severity::Error, "ref.nullId",
                              std::string(ref.table) + "#" + std::to_string(rowId),
                              std::string(ref.idCol) + " is NULL but not nullable");
                    continue;
                }
                const auto targetId = st.getColumn(2).getInt64();

                if (kNotRows.count(kind)) continue;   // external or not a row

                const auto it = kKindToTable.find(kind);
                if (it == kKindToTable.end()) {
                    // A kind we have never heard of. Not necessarily corruption
                    // -- a newer version may have added one -- so it is a
                    // warning, in line with how unknown data is treated
                    // everywhere else (ADR-0012).
                    c.add(Severity::Warning, "ref.unknownKind",
                          std::string(ref.table) + "#" + std::to_string(rowId) + "." +
                              ref.kindCol,
                          "'" + kind + "' names no table this build knows; cannot verify");
                    continue;
                }

                SQLite::Statement look(
                    c.db, "SELECT 1 FROM \"" + it->second + "\" WHERE " +
                              (it->second == "mixer_strip" ? "track_id" : "id") + " = ?");
                look.bind(1, targetId);
                if (!look.executeStep())
                    c.add(Severity::Error, "ref.dangling",
                          std::string(ref.table) + "#" + std::to_string(rowId) + "." +
                              ref.idCol,
                          "points at " + kind + " " + std::to_string(targetId) +
                              ", which does not exist");
            }
        } catch (const std::exception& e) {
            c.add(Severity::Error, "ref.checkFailed", ref.table, e.what());
        }
    }
}

// --- 3. inside the blobs ---------------------------------------------------------

/// Parses a blob's header the way any reader must, and reports what a reader
/// would hit. This is the only check in the file that opens a BLOB, and it is
/// the only one that can: to SQL these are opaque bytes.
template <typename Rec>
void verifyStream(Ctx& c, const std::vector<std::byte>& blob, FourCC cc,
                  const std::string& where) {
    StreamReader<Rec> r(blob, cc);
    if (!r.ok()) {
        c.add(Severity::Error, "blob.malformed", where, toString(r.error()));
        return;
    }
    // ADR-0023: rec_size must be a size some writer released. StreamReader
    // already rejects anything else, so reaching here means it is fine -- but
    // an unknown TAIL is worth surfacing, because it means this file was
    // written by something newer and a save from here must round-trip the bytes.
    if (r.hasUnknownTail())
        c.add(Severity::Info, "blob.newerVersion", where,
              "records are " + std::to_string(r.recSize()) + " bytes, wider than this "
              "build's " + std::to_string(sizeof(Rec)) + "; written by a newer version");
}

std::vector<std::byte> blobOf(const SQLite::Column& col) {
    if (col.isNull()) return {};
    const auto* p = static_cast<const std::byte*>(col.getBlob());
    return {p, p + col.getBytes()};
}

void checkBlobs(Ctx& c) {
    c.ran();
    try {
        SQLite::Statement st(c.db,
            "SELECT id, clip_id, stream_kind, data FROM event_streams");
        while (st.executeStep()) {
            const auto where = "event_streams#" + std::to_string(st.getColumn(0).getInt64());
            const auto kind = st.getColumn(2).getString();
            const auto data = blobOf(st.getColumn(3));
            if (kind == "notes")
                verifyStream<NoteRecord>(c, data, FourCC::Notes, where);
            else if (kind == "cc")
                verifyStream<AutomationPoint>(c, data, FourCC::Controller, where);
            // Other stream kinds have no record type yet; not an error.
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "blob.checkFailed", "event_streams", e.what());
    }

    c.ran();
    try {
        SQLite::Statement st(c.db, "SELECT id, data FROM note_expression");
        while (st.executeStep())
            verifyStream<ExpressionPoint>(
                c, blobOf(st.getColumn(1)), FourCC::Expression,
                "note_expression#" + std::to_string(st.getColumn(0).getInt64()));
    } catch (const std::exception& e) {
        c.add(Severity::Error, "blob.checkFailed", "note_expression", e.what());
    }

    c.ran();
    try {
        SQLite::Statement st(c.db, "SELECT id, data FROM automation_data");
        while (st.executeStep())
            verifyStream<AutomationPoint>(
                c, blobOf(st.getColumn(1)), FourCC::Automation,
                "automation_data#" + std::to_string(st.getColumn(0).getInt64()));
    } catch (const std::exception& e) {
        c.add(Severity::Error, "blob.checkFailed", "automation_data", e.what());
    }
}

/// A note_expression row names a note by id INSIDE another blob. Nothing in SQL
/// can check that, and an orphan means a per-note pressure curve that will never
/// be heard and never be reported.
void checkNoteExpressionTargets(Ctx& c) {
    c.ran();
    try {
        std::map<std::int64_t, std::unordered_set<std::uint64_t>> notesByClip;
        {
            SQLite::Statement st(c.db,
                "SELECT clip_id, data FROM event_streams WHERE stream_kind = 'notes'");
            while (st.executeStep()) {
                const auto clip = st.getColumn(0).getInt64();
                // Named, not a temporary: the reader holds a span into it.
                const auto bytes = blobOf(st.getColumn(1));
                StreamReader<NoteRecord> r(bytes, FourCC::Notes);
                if (!r.ok()) continue;   // already reported by checkBlobs
                for (std::uint32_t i = 0; i < r.count(); ++i)
                    if (auto n = r.at(i)) notesByClip[clip].insert(n->note_id);
            }
        }
        SQLite::Statement st(c.db,
            "SELECT id, clip_id, note_id, dimension FROM note_expression");
        while (st.executeStep()) {
            const auto id = st.getColumn(0).getInt64();
            const auto clip = st.getColumn(1).getInt64();
            const auto note = static_cast<std::uint64_t>(st.getColumn(2).getInt64());
            const auto found = notesByClip.find(clip);
            if (found == notesByClip.end() || !found->second.count(note))
                c.add(Severity::Error, "expression.orphan",
                      "note_expression#" + std::to_string(id),
                      "expression for note " + std::to_string(note) + " in clip " +
                          std::to_string(clip) + ", which has no such note");
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "expression.checkFailed", "note_expression", e.what());
    }
}

// --- 4. the history tree ----------------------------------------------------------

void checkHistory(Ctx& c) {
    c.ran();
    try {
        const auto current =
            c.db.execAndGet("SELECT COUNT(*) FROM op_branches WHERE is_current = 1").getInt();
        if (current != 1)
            c.add(Severity::Error, "history.currentBranch", "op_branches",
                  std::to_string(current) + " branches claim to be current; exactly one "
                  "must be (ADR-0026)");
    } catch (const std::exception& e) {
        c.add(Severity::Error, "history.checkFailed", "op_branches", e.what());
    }

    c.ran();
    try {
        SQLite::Statement st(c.db,
            "SELECT b.id, b.head_seq FROM op_branches b WHERE b.head_seq IS NOT NULL "
            "AND NOT EXISTS (SELECT 1 FROM ops o WHERE o.seq = b.head_seq)");
        while (st.executeStep())
            c.add(Severity::Error, "history.danglingHead",
                  "op_branches#" + std::to_string(st.getColumn(0).getInt64()),
                  "head_seq " + std::to_string(st.getColumn(1).getInt64()) +
                      " names no op");
    } catch (const std::exception& e) {
        c.add(Severity::Error, "history.checkFailed", "op_branches", e.what());
    }

    // Ephemeral ops are outside the tree by construction (ADR-0030). One with a
    // parent means something wrote it by hand, and undo would then walk into it.
    c.ran();
    try {
        SQLite::Statement st(c.db,
            "SELECT seq FROM ops WHERE tags = 'ephemeral' AND parent_seq IS NOT NULL");
        while (st.executeStep())
            c.add(Severity::Error, "history.ephemeralInTree",
                  "ops#" + std::to_string(st.getColumn(0).getInt64()),
                  "an ephemeral op has a parent_seq; it must be outside the undo tree");
    } catch (const std::exception& e) {
        c.add(Severity::Error, "history.checkFailed", "ops", e.what());
    }

    // A cycle in parent_seq would make undo loop forever. Walk every chain with
    // a visited set rather than trusting that inserts were always well-ordered.
    c.ran();
    try {
        std::map<std::int64_t, std::int64_t> parent;
        SQLite::Statement st(c.db,
            "SELECT seq, parent_seq FROM ops WHERE parent_seq IS NOT NULL");
        while (st.executeStep())
            parent[st.getColumn(0).getInt64()] = st.getColumn(1).getInt64();

        for (const auto& [seq, _] : parent) {
            std::unordered_set<std::int64_t> seen{seq};
            auto cur = seq;
            while (true) {
                const auto it = parent.find(cur);
                if (it == parent.end()) break;
                cur = it->second;
                if (!seen.insert(cur).second) {
                    c.add(Severity::Error, "history.cycle",
                          "ops#" + std::to_string(seq),
                          "the parent chain from this op loops; undo would not terminate");
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "history.checkFailed", "ops", e.what());
    }
}

// --- 5. time and media ------------------------------------------------------------

void checkTimeBase(Ctx& c) {
    // SPEC §4.1: an object populates exactly the column matching its time_base.
    //
    // `clips` has a CHECK covering HALF of it -- `time_base = 1 OR pos_ns IS
    // NULL` stops a musical-time clip carrying a nanosecond position. It says
    // nothing about a clip with NO position at all, which is equally invalid and
    // equally silent. `markers` has the full constraint; `clips` does not, and
    // the gap is only visible from here.
    c.ran();
    try {
        SQLite::Statement st(c.db,
            "SELECT id, time_base, pos_ticks, pos_ns FROM clips "
            "WHERE (time_base = 0 AND (pos_ticks IS NULL OR pos_ns IS NOT NULL)) "
            "   OR (time_base = 1 AND (pos_ns IS NULL OR pos_ticks IS NOT NULL))");
        while (st.executeStep())
            c.add(Severity::Error, "time.baseMismatch",
                  "clips#" + std::to_string(st.getColumn(0).getInt64()),
                  "time_base is " + std::to_string(st.getColumn(1).getInt()) +
                      " but the wrong position column is set (SPEC 4.1)");
    } catch (const std::exception& e) {
        c.add(Severity::Error, "time.checkFailed", "clips", e.what());
    }

    // A project with no tempo at tick 0 has no defined tempo before its first
    // event. Legal SQL, meaningless music.
    c.ran();
    try {
        const auto any = c.db.execAndGet("SELECT COUNT(*) FROM tempo_map").getInt();
        const auto atZero =
            c.db.execAndGet("SELECT COUNT(*) FROM tempo_map WHERE pos_ticks = 0").getInt();
        if (any > 0 && atZero == 0)
            c.add(Severity::Warning, "time.noInitialTempo", "tempo_map",
                  "there are tempo events but none at tick 0, so the tempo before the "
                  "first one is undefined");
    } catch (const std::exception&) {
    }
}

void checkMedia(Ctx& c) {
    c.ran();
    try {
        SQLite::Statement st(c.db,
            "SELECT m.id, m.embedded, (SELECT COUNT(*) FROM media_blobs b "
            "  WHERE b.media_id = m.id) FROM media_files m");
        while (st.executeStep()) {
            const auto id = st.getColumn(0).getInt64();
            const bool embedded = st.getColumn(1).getInt() != 0;
            const auto chunks = st.getColumn(2).getInt();
            // ADR-0127 / ADR-0136: media is never embedded. A 1.1 file cannot
            // hold any (the schema's triggers refuse it), so this finds 1.0
            // files, and a 1.1 file whose triggers someone dropped.
            if (embedded || chunks > 0)
                c.add(Severity::Error, "media.embedded",
                      "media_files#" + std::to_string(id),
                      std::string(embedded ? "marked embedded" : "not marked embedded") +
                          " with " + std::to_string(chunks) +
                          " blob chunk(s); media is never embedded (ADR-0127): "
                          "extract it to the project's audio/ folder");
        }
    } catch (const std::exception& e) {
        c.add(Severity::Error, "media.checkFailed", "media_files", e.what());
    }
}

// --- 6. remarks (ADR-0131) ---------------------------------------------------------

/// A remark whose track, clip or device is gone. A WARNING, not an error:
/// deleting the object is an ordinary edit and undoing it re-anchors the remark,
/// so the orphan is expected state between the two -- but a user who never
/// undoes should be told their note now points at nothing.
void checkRemarks(Ctx& c) {
    c.ran();
    try {
        // A 1.0 or 1.1 file has no remarks table (schema 1.2). Nothing to check.
        if (c.db.execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                            "AND name = 'remarks'").getInt() == 0)
            return;
        // Only the kinds 1.2 defines. A kind a newer minor adds is its business,
        // and "no longer exists" would be a false statement about it.
        SQLite::Statement st(c.db,
            "SELECT r.id, r.target_kind, r.target_id FROM remarks r "
            "WHERE r.target_kind IN ('track','clip','device') AND NOT ("
            "  (r.target_kind = 'track'  AND EXISTS (SELECT 1 FROM tracks  WHERE id = r.target_id)) OR "
            "  (r.target_kind = 'clip'   AND EXISTS (SELECT 1 FROM clips   WHERE id = r.target_id)) OR "
            "  (r.target_kind = 'device' AND EXISTS (SELECT 1 FROM devices WHERE id = r.target_id)))");
        while (st.executeStep())
            c.add(Severity::Warning, "remark.danglingTarget",
                  "remarks#" + std::to_string(st.getColumn(0).getInt64()),
                  "anchored to " + st.getColumn(1).getString() + " " +
                      std::to_string(st.getColumn(2).getInt64()) +
                      ", which no longer exists");
    } catch (const std::exception& e) {
        c.add(Severity::Error, "remark.checkFailed", "remarks", e.what());
    }
}

}  // namespace

CheckReport checkProject(const Store& s, bool verifyMediaFiles) {
    CheckReport r;
    Ctx c{const_cast<SQLite::Database&>(s.db()), r};
    checkSqlite(c);
    checkPolyRefs(c);
    checkBlobs(c);
    checkNoteExpressionTargets(c);
    checkHistory(c);
    checkTimeBase(c);
    checkMedia(c);
    if (verifyMediaFiles) {
        c.ran();
        try {
            SQLite::Statement rows(c.db, "SELECT id FROM media_files WHERE embedded=0 ORDER BY id");
            while (rows.executeStep()) {
                const auto id = rows.getColumn(0).getInt64();
                std::string error;
                if (media::resolveMedia(media::projectFolder(s), media::mediaRow(c.db, id), error).empty())
                    c.add(Severity::Error, "media.unresolved", "media_files#" + std::to_string(id), error);
            }
        } catch (const std::exception& e) { c.add(Severity::Error, "media.checkFailed", "media_files", e.what()); }
    }
    checkRemarks(c);
    return r;
}

}  // namespace adi
