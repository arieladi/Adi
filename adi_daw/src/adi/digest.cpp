// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/digest.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace adi {
namespace {

constexpr char kFieldSep = '\x1f';   // US, between columns within a row
constexpr char kRecordSep = '\n';    // between rows

/// Not project state. See the header for why each one is here -- the exclusions
/// are the design, not housekeeping.
const std::set<std::string> kExcluded = {
    "ops", "op_branches",                        // the log is not the project
    "session_state", "ui_view", "window_state",  // UI state; replay varies it on purpose
    "adi_meta", "session_lock",                  // timestamps, host, pid
};

/// A value, rendered with its TYPE. Without the tag, integer 1 and text "1"
/// would digest identically, and telling those apart is exactly what a digest
/// over a database with 23 REAL columns has to do.
std::string renderValue(const SQLite::Column& c) {
    // SQLiteCpp's typed predicates rather than the raw SQLITE_* constants,
    // which live in sqlite3.h and are not re-exported by SQLiteCpp.h.
    if (c.isNull()) return "~";

    if (c.isInteger()) return "i" + std::to_string(c.getInt64());

    if (c.isFloat()) {
        // %.17g is the shortest form that round-trips an IEEE double, so two
        // bit-identical values always render identically and two different ones
        // never collide.
        char buf[40];
        std::snprintf(buf, sizeof buf, "f%.17g", c.getDouble());
        return buf;
    }

    if (c.isText()) {
        std::string out = "s";
        for (const char ch : std::string(c.getString())) {
            // Escape anything that could forge a field or row boundary. A track
            // name containing the separator would otherwise let two different
            // projects digest identically -- the one failure a comparison
            // oracle must not have.
            switch (ch) {
                case '\n':   out += "\\n"; break;
                case '\r':   out += "\\r"; break;
                case '\t':   out += "\\t"; break;
                case '\\':   out += "\\\\"; break;
                case kFieldSep: out += "\\u001f"; break;
                case '\x1e': out += "\\u001e"; break;
                default:     out += ch; break;
            }
        }
        return out;
    }

    if (c.isBlob()) {
        // Blobs are hashed rather than dumped: a notes blob is kilobytes, and
        // the digest is meant to stay readable when it differs. Length is
        // included so a hash collision alone cannot hide a size change.
        const auto* p = static_cast<const unsigned char*>(c.getBlob());
        const int n = c.getBytes();
        std::uint64_t h = 1469598103934665603ull;   // FNV-1a 64
        for (int i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        char buf[48];
        std::snprintf(buf, sizeof buf, "b%d:%016llx", n, static_cast<unsigned long long>(h));
        return buf;
    }

    return "?";
}

std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) {
        // Explicit: char is signed here, and hashing a sign-extended value would
        // make the fingerprint depend on the platform's char signedness. clang
        // and gcc flag the implicit conversion under -Wsign-conversion; MSVC
        // does not, which is why CI caught this and the local build did not.
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

std::vector<std::string> digestedTables(const Store& s) {
    std::vector<std::string> out;
    try {
        SQLite::Statement st(const_cast<SQLite::Database&>(s.db()),
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' ORDER BY name");
        while (st.executeStep()) {
            const auto n = st.getColumn(0).getString();
            if (!kExcluded.count(n)) out.push_back(n);
        }
    } catch (const std::exception&) {
    }
    return out;
}

Digest digestProject(const Store& s) {
    Digest d;
    auto& db = const_cast<SQLite::Database&>(s.db());

    for (const auto& table : digestedTables(s)) {
        std::vector<std::string> columns;
        try {
            SQLite::Statement ti(db, "PRAGMA table_info(\"" + table + "\")");
            while (ti.executeStep()) columns.push_back(ti.getColumn(1).getString());
        } catch (const std::exception&) {
            continue;
        }
        if (columns.empty()) continue;

        // Every column, named. Named rather than positional so that adding a
        // column changes the digest in a readable way instead of shifting
        // everything by one, and so a diff of two digests says what differs.
        std::string sql = "SELECT ";
        for (std::size_t i = 0; i < columns.size(); ++i)
            sql += (i ? ", \"" : "\"") + columns[i] + "\"";
        sql += " FROM \"" + table + "\"";

        std::vector<std::string> rows;
        try {
            SQLite::Statement q(db, sql);
            while (q.executeStep()) {
                std::string row;
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    if (i) row += kFieldSep;
                    row += columns[i] + "=" + renderValue(q.getColumn(static_cast<int>(i)));
                }
                rows.push_back(std::move(row));
            }
        } catch (const std::exception&) {
            continue;
        }

        // Sorted by CONTENT, not by rowid. This is the whole reason the digest
        // can serve as a replay oracle: two databases holding the same project
        // but with different rowid assignments must produce the same bytes.
        std::sort(rows.begin(), rows.end());

        d.text += "[" + table + "]";
        d.text += kRecordSep;
        for (const auto& r : rows) {
            d.text += "  " + r;
            d.text += kRecordSep;
            ++d.rowCount;
        }
        ++d.tableCount;
    }

    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(fnv1a(d.text)));
    d.fingerprint = buf;
    return d;
}

}  // namespace adi
