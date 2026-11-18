// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/changeset.hpp"

#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace adi::agent {
namespace {

/// A JSON integer that fits an int64, or a float with no fraction inside that
/// range (JSON Schema calls both an integer); nullopt otherwise, so an
/// unsigned value above INT64_MAX is refused, never wrapped to a negative.
/// The rule control/methods.cpp's asInt64 applies on the MCP path.
std::optional<std::int64_t> int64Of(const Payload& v) {
    if (v.is_number_integer() && !v.is_number_unsigned()) return v.get<std::int64_t>();
    if (v.is_number_unsigned() && v.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.get<std::uint64_t>());
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isfinite(d) && std::floor(d) == d && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
}

// --- guardrails, shared by preview and apply ---------------------------------------

/// Everything that can be refused without touching the database, in the order
/// a user would want to hear it.
Refusal screen(const Changeset& cs, const Limits& limits, std::string& error) {
    if (cs.ops.empty()) {
        error = "the changeset is empty";
        return Refusal::Empty;
    }
    if (cs.ops.size() > limits.maxOps) {
        error = std::to_string(cs.ops.size()) + " ops, over the per-request cap of " +
                std::to_string(limits.maxOps) + " (AI-AGENT 6.6)";
        return Refusal::TooManyOps;
    }
    for (const auto& q : cs.ops) {
        if (!agentMayEmit(q.opType)) {
            error = "'" + q.opType + "' is not an op the agent may emit";
            return Refusal::OpNotAllowed;
        }
        const auto* d = OpRegistry::instance().find(q.opType);
        const auto issues = validateSubmission(*d, q.payload);
        if (!issues.empty()) {
            error = q.opType + ": " + issues.front().key + " " + issues.front().problem;
            return Refusal::Invalid;
        }
    }
    return Refusal::None;
}

Refusal checkHead(const Store& store, const Changeset& cs, std::string& error) {
    const auto head = OpJournal(const_cast<Store&>(store)).headSeq();
    if (head != cs.baseHead) {
        const auto show = [](std::optional<std::int64_t> s) {
            return s ? std::to_string(*s) : std::string("the root");
        };
        error = "stale: the changeset was built on " + show(cs.baseHead) +
                " and the project is now at " + show(head) +
                "; preview it again on the current project";
        return Refusal::Stale;
    }
    return Refusal::None;
}

/// What the review list says an op targets. The caller's own target wins;
/// otherwise the op's domain and the first id-like key in its payload.
void targetOf(const OpRequest& q, Change& c) {
    if (!q.targetKind.empty()) {
        c.targetKind = q.targetKind;
        c.targetId = q.targetId;
        return;
    }
    c.targetKind = q.opType.substr(0, q.opType.find('.'));
    for (const char* key : {"id", "dev", "clip", "track"}) {
        const auto it = q.payload.find(key);
        if (it != q.payload.end() && it->is_number_integer()) {
            c.targetId = it->get<std::int64_t>();
            return;
        }
    }
}

Change describe(const OpRequest& q, const OpDescriptor& d, const std::optional<Payload>& inverse) {
    Change c;
    c.label = q.label.empty() ? q.opType : q.label;
    c.opType = q.opType;
    targetOf(q, c);
    if (!inverse) {
        c.after = q.payload;
    } else if (d.inverseOp.empty()) {
        // Symmetric, or a capture replayed through the same op: the inverse IS
        // the before-state, in the op's own keys -- a parameter's old value.
        c.before = *inverse;
        c.after = q.payload;
    } else {
        // Paired. Classified by content, not by key count (R4.6): a capture
        // op's inverse may have as many keys as its payload -- automation.
        // createLane is {id,row} against {id,state} -- and must not read as a
        // deletion.
        const bool objects = inverse->is_object() && q.payload.is_object();
        const auto everyKeyIn = [](const Payload& from, const Payload& in, bool equalValues) {
            for (auto it = from.begin(); it != from.end(); ++it) {
                const auto found = in.find(it.key());
                if (found == in.end()) return false;
                if (equalValues && *found != it.value()) return false;
            }
            return true;
        };
        if (objects && everyKeyIn(*inverse, q.payload, true)) {
            // The inverse is only the identity, repeated from the payload: a creation.
            c.after = q.payload;
        } else if (objects && everyKeyIn(q.payload, *inverse, true)) {
            // The inverse captured the row the payload names, and the payload
            // is only that row's identity: a deletion. Compared by value, so
            // clip.split ({id,changes} against {id,changes reversed}) shows
            // both sides.
            c.before = *inverse;
        } else {
            // A capture beside new values: the old state, then the new.
            c.before = *inverse;
            c.after = q.payload;
        }
    }
    return c;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < s.size()) {
        const auto nl = s.find('\n', at);
        if (nl == std::string::npos) {
            out.push_back(s.substr(at));
            break;
        }
        out.push_back(s.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

enum class Edit : char { Keep = ' ', Del = '-', Ins = '+' };

/// The most diagonals the edit script's trace may hold: about 64 MB. Past it
/// the changed middle is shown as one replacement instead (see editScript).
constexpr std::size_t kTraceBudget = std::size_t{8} << 20;

/// Myers' O(ND) shortest edit script, in bounded memory.
///
/// A project's projection is tens of thousands of lines and a changeset
/// usually touches few of them, so D is small and the quadratic LCS table this
/// replaces would be the wrong trade. But a preview is reachable from a remote
/// caller (the control stack, ADR-0233), and one request may delete a track of
/// ten thousand notes or insert six thousand, so the edit script must not grow
/// with what a request touches:
///   * the common prefix and suffix are trimmed first: a contiguous edit (a
///     track deleted, notes added to one clip) then costs no trace at all;
///   * each step keeps only its live diagonals, -(d-1)..d-1, not the whole V;
///   * past kTraceBudget diagonals the untrimmed middle becomes one deletion
///     and one insertion -- still a correct diff, only a coarser one.
std::vector<Edit> editScript(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;

    const auto n = static_cast<std::ptrdiff_t>(a.size() - prefix - suffix);
    const auto m = static_cast<std::ptrdiff_t>(b.size() - prefix - suffix);
    const auto A = [&](std::ptrdiff_t i) -> const std::string& {
        return a[prefix + static_cast<std::size_t>(i)];
    };
    const auto B = [&](std::ptrdiff_t i) -> const std::string& {
        return b[prefix + static_cast<std::size_t>(i)];
    };

    std::vector<Edit> middle;
    bool found = false;
    if (n > 0 && m > 0) {
        const std::ptrdiff_t max = n + m;
        const std::ptrdiff_t offset = max + 1;
        std::vector<std::ptrdiff_t> v(static_cast<std::size_t>(2 * max + 3), 0);
        const auto at = [&](std::ptrdiff_t k) -> std::ptrdiff_t& {
            return v[static_cast<std::size_t>(k + offset)];
        };
        // trace[d] holds V as step d found it, diagonals -(d-1)..d-1: exactly
        // what the walk back reads for step d.
        std::vector<std::vector<std::ptrdiff_t>> trace;
        std::size_t held = 0;
        std::ptrdiff_t dFound = -1;
        for (std::ptrdiff_t d = 0; d <= max && dFound < 0; ++d) {
            const std::size_t width = d == 0 ? 0 : static_cast<std::size_t>(2 * d - 1);
            if (held + width > kTraceBudget) break;
            held += width;
            std::vector<std::ptrdiff_t> snapshot(width);
            for (std::size_t i = 0; i < width; ++i) snapshot[i] = at(-(d - 1) + static_cast<std::ptrdiff_t>(i));
            trace.push_back(std::move(snapshot));
            for (std::ptrdiff_t k = -d; k <= d; k += 2) {
                std::ptrdiff_t x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
                std::ptrdiff_t y = x - k;
                while (x < n && y < m && A(x) == B(y)) {
                    ++x;
                    ++y;
                }
                at(k) = x;
                if (x >= n && y >= m) {
                    dFound = d;
                    break;
                }
            }
        }

        if (dFound >= 0) {
            found = true;
            // Walk the trace backwards from (n, m).
            std::ptrdiff_t x = n, y = m;
            for (std::ptrdiff_t d = dFound; d > 0; --d) {
                const auto& pv = trace[static_cast<std::size_t>(d)];
                const auto prev = [&](std::ptrdiff_t k) { return pv[static_cast<std::size_t>(k + (d - 1))]; };
                const std::ptrdiff_t k = x - y;
                const bool down = (k == -d || (k != d && prev(k - 1) < prev(k + 1)));
                const std::ptrdiff_t pk = down ? k + 1 : k - 1;
                const std::ptrdiff_t px = prev(pk);
                const std::ptrdiff_t py = px - pk;
                while (x > px && y > py) {
                    middle.push_back(Edit::Keep);
                    --x;
                    --y;
                }
                middle.push_back(down ? Edit::Ins : Edit::Del);
                x = px;
                y = py;
            }
            while (x > 0 && y > 0) {
                middle.push_back(Edit::Keep);
                --x;
                --y;
            }
            std::reverse(middle.begin(), middle.end());
        }
    }
    if (!found) {
        // Only deletions or only insertions (exact), or over the budget (a
        // coarser but correct diff): the changed middle, deleted then inserted.
        middle.assign(static_cast<std::size_t>(n), Edit::Del);
        middle.insert(middle.end(), static_cast<std::size_t>(m), Edit::Ins);
    }

    std::vector<Edit> out(prefix, Edit::Keep);
    out.insert(out.end(), middle.begin(), middle.end());
    out.insert(out.end(), suffix, Edit::Keep);
    return out;
}

std::string range(std::size_t start, std::size_t count) {
    // Unified diff convention: a range of zero lines names the line before it.
    const std::size_t first = count == 0 ? start : start + 1;
    return count == 1 ? std::to_string(first)
                      : std::to_string(first) + "," + std::to_string(count);
}

void dropReadTransactionNote(rows::Model& m) {
    // readModel opens its own read transaction; inside the preview's
    // transaction that is refused and noted, and harmless -- it reads the
    // uncommitted state, which is the point. Every other problem stays.
    m.problems.erase(std::remove_if(m.problems.begin(), m.problems.end(),
                                    [](const std::string& p) {
                                        return p.rfind("read transaction:", 0) == 0;
                                    }),
                     m.problems.end());
}

}  // namespace

const char* toString(Refusal r) {
    switch (r) {
        case Refusal::None:           return "none";
        case Refusal::Empty:          return "empty";
        case Refusal::TooManyOps:     return "too many ops";
        case Refusal::OpNotAllowed:   return "op not allowed";
        case Refusal::Invalid:        return "invalid";
        case Refusal::Stale:          return "stale";
        case Refusal::ReadOnly:       return "read-only";
        case Refusal::NoRequestTable: return "no agent_requests table";
    }
    return "?";
}

bool agentMayEmit(std::string_view opType) {
    const auto* d = OpRegistry::instance().find(opType);
    if (!d || !d->apply) return false;
    // Project edits only: transport is performance and hardware is the user's
    // desk, and neither belongs in a changeset the user reviews as a diff.
    if (d->scope != Scope::Edit || d->ephemeral) return false;
    // §6.4: the agent may change a REFERENCE, never a file. `media.unlink`
    // removes a row and leaves the disk alone, `media.relink` repoints one.
    // `media.import` is refused: it records a file a caller has hashed from
    // disk, and the agent has no business asserting what is on the disk.
    // Any media op added later is refused until it is named here.
    if (opType.rfind("media.", 0) == 0) return opType == "media.unlink" || opType == "media.relink";
    // The restore ops exist to replay an inverse another op captured, and
    // write what they are given past that op's own checks: raw stream bytes,
    // a lane row, a recording row. Named, not inferred from "has no inverse
    // and is some op's inverse", which would also catch controller.bind, a
    // forward op. arranger.splice, clip.split and clip.join are edits.
    for (const std::string_view internal : {"automation.restoreLane", "automation.restoreData",
                                            "cc.restoreStream", "note.restoreNotes", "track.restoreRecording"})
        if (opType == internal) return false;
    return true;
}

Changeset begin(const Store& store, std::string request, std::string actorDetail,
                std::int64_t createdUtc) {
    Changeset cs;
    cs.baseHead = OpJournal(const_cast<Store&>(store)).headSeq();
    cs.request = std::move(request);
    cs.actorDetail = std::move(actorDetail);
    cs.createdUtc = createdUtc;
    return cs;
}

Preview preview(Store& store, const Changeset& cs, const Limits& limits) {
    Preview p;
    if ((p.refusal = screen(cs, limits, p.error)) != Refusal::None) return p;
    if (store.readOnly()) {
        p.refusal = Refusal::ReadOnly;
        p.error = "project is open read-only";
        return p;
    }
    if ((p.refusal = checkHead(store, cs, p.error)) != Refusal::None) return p;

    p.textBefore = textproj::projectStore(store).text;

    auto& db = store.db();
    OpContext ctx{store, db};
    try {
        // ADR-0148: the same steps OpJournal::commit takes -- inverse from the
        // state about to be overwritten, then apply -- inside a transaction
        // that is NEVER committed. The destructor rolls it back, so the file
        // is not written and nothing is logged; what the user reviews is what
        // Apply will do, computed by the same handlers.
        SQLite::Transaction scratch(db);
        for (const auto& q : cs.ops) {
            const auto* d = OpRegistry::instance().find(q.opType);
            std::optional<Payload> inverse;
            std::string err;
            if (d->buildInverse && !d->ephemeral) {
                Payload inv;
                if (!d->buildInverse(ctx, q.payload, inv, err)) {
                    p.refusal = Refusal::Invalid;
                    p.error = q.opType + ": cannot build inverse: " + err;
                    return p;
                }
                inverse = std::move(inv);
            }
            if (!d->apply(ctx, q.payload, err)) {
                p.refusal = Refusal::Invalid;
                p.error = q.opType + ": " + err;
                return p;
            }
            p.changes.push_back(describe(q, *d, inverse));
        }
        rows::Model after = rows::readModel(store);
        dropReadTransactionNote(after);
        p.textAfter = textproj::project(textproj::buildTree(after)).text;
    } catch (const std::exception& e) {
        p.refusal = Refusal::Invalid;
        p.error = e.what();
        p.changes.clear();
        return p;
    }

    p.unifiedDiff = unifiedDiff(p.textBefore, p.textAfter);
    p.ok = true;
    return p;
}

ApplyResult apply(Store& store, const Changeset& cs, const Limits& limits) {
    ApplyResult r;
    if ((r.refusal = screen(cs, limits, r.error)) != Refusal::None) return r;
    if (store.readOnly()) {
        r.refusal = Refusal::ReadOnly;
        r.error = "project is open read-only";
        return r;
    }
    auto& db = store.db();
    try {
        if (db.execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                          "AND name = 'agent_requests'").getInt() == 0) {
            r.refusal = Refusal::NoRequestTable;
            r.error = "this file has no agent_requests table (schema 1.4); open it for "
                      "writing to upgrade it";
            return r;
        }

        // ADR-0148, ADR-0153: the request row in the SAME transaction as the
        // ops (SPEC 8.7), and the stale check inside it too, both through the
        // journal's own hook. This replaced a connection-local TEMP trigger and
        // a check made one step before the commit.
        std::vector<OpRequest> reqs = cs.ops;
        for (auto& q : reqs) {
            q.actor = Actor::Agent;
            q.actorDetail = cs.actorDetail;
            if (q.label.empty()) q.label = q.opType;
        }
        CommitOptions options;
        options.expectHead = cs.baseHead;
        options.beforeCommit = [&cs](SQLite::Database& tx, std::int64_t txnId, std::string& err) {
            try {
                // No "unless a row exists" guard: a row already there for this
                // txn id must fail the insert, and with it every op.
                SQLite::Statement st(tx,
                    "INSERT INTO agent_requests(txn_id, request, actor_detail, created_utc) "
                    "VALUES (?,?,?,?)");
                st.bind(1, txnId);
                st.bind(2, cs.request);
                st.bind(3, cs.actorDetail);
                st.bind(4, cs.createdUtc);
                st.exec();
                return true;
            } catch (const std::exception& e) {
                err = std::string("agent_requests: ") + e.what();
                return false;
            }
        };
        OpJournal journal(store);
        const CommitResult res = journal.commit(reqs, options);   // ONE transaction (§6.2)
        if (res.stale) {
            const auto show = [](std::optional<std::int64_t> s) {
                return s ? std::to_string(*s) : std::string("the root");
            };
            r.refusal = Refusal::Stale;
            r.error = "stale: the changeset was built on " + show(cs.baseHead) +
                      " and the project is now at " + show(res.headFound) +
                      "; preview it again on the current project";
            return r;
        }
        if (!res.ok) {
            r.refusal = Refusal::Invalid;
            r.error = res.error;
            return r;
        }
        r.ok = true;
        r.txnId = res.txnId;
        return r;
    } catch (const std::exception& e) {
        r.refusal = Refusal::Invalid;
        r.error = e.what();
        return r;
    }
}

std::string unifiedDiff(const std::string& before, const std::string& after,
                        std::string_view nameBefore, std::string_view nameAfter,
                        std::size_t context) {
    if (before == after) return {};
    const auto a = splitLines(before);
    const auto b = splitLines(after);
    const auto script = editScript(a, b);

    // Positions in a and b at the start of each edit.
    std::vector<std::size_t> ai(script.size() + 1), bi(script.size() + 1);
    for (std::size_t i = 0, x = 0, y = 0; i <= script.size(); ++i) {
        ai[i] = x;
        bi[i] = y;
        if (i == script.size()) break;
        if (script[i] != Edit::Ins) ++x;
        if (script[i] != Edit::Del) ++y;
    }

    std::string out = "--- " + std::string(nameBefore) + "\n+++ " + std::string(nameAfter) + "\n";
    std::size_t i = 0;
    while (i < script.size()) {
        while (i < script.size() && script[i] == Edit::Keep) ++i;
        if (i == script.size()) break;
        // A hunk: from `context` lines before this change to `context` after
        // the last change within 2*context of the one before it.
        const std::size_t start = i >= context ? i - context : 0;
        std::size_t end = i;
        for (std::size_t j = i; j < script.size(); ++j) {
            if (script[j] != Edit::Keep) end = j + 1;
            else if (j >= end + 2 * context) break;
        }
        end = std::min(script.size(), end + context);

        std::size_t delCount = 0, insCount = 0;
        for (std::size_t j = start; j < end; ++j) {
            if (script[j] != Edit::Ins) ++delCount;
            if (script[j] != Edit::Del) ++insCount;
        }
        out += "@@ -" + range(ai[start], delCount) + " +" + range(bi[start], insCount) + " @@\n";
        for (std::size_t j = start; j < end; ++j) {
            const auto& line = script[j] == Edit::Ins ? b[bi[j]] : a[ai[j]];
            out += static_cast<char>(script[j]);
            out += line;
            out += '\n';
        }
        i = end;
    }
    return out;
}

std::optional<Changeset> changesetFromJson(const Store& store, const Payload& j,
                                           std::string& error) {
    try {
        if (!j.is_object()) { error = "a changeset is a JSON object"; return std::nullopt; }
        std::int64_t createdUtc = 0;
        if (const auto c = j.find("createdUtc"); c != j.end()) {
            const auto v = int64Of(*c);
            if (!v) { error = "\"createdUtc\" is an integer"; return std::nullopt; }
            createdUtc = *v;
        }
        Changeset cs = begin(store, j.value("request", std::string{}),
                             j.value("actorDetail", std::string{}), createdUtc);
        if (j.contains("baseHead")) {
            const auto& h = j.at("baseHead");
            if (h.is_null()) {
                cs.baseHead = std::nullopt;
            } else {
                const auto v = int64Of(h);
                if (!v) { error = "\"baseHead\" is an integer or null"; return std::nullopt; }
                cs.baseHead = *v;
            }
        }
        if (cs.request.empty()) { error = "\"request\" is required (AI-AGENT 6.8)"; return std::nullopt; }
        if (!j.contains("ops") || !j.at("ops").is_array()) {
            error = "\"ops\" must be an array";
            return std::nullopt;
        }
        for (const auto& o : j.at("ops")) {
            OpRequest q;
            q.opType = o.at("op").get<std::string>();
            q.payload = o.value("payload", Payload::object());
            q.label = o.value("label", std::string{});
            q.targetKind = o.value("targetKind", std::string{});
            if (const auto t = o.find("targetId"); t != o.end() && !t->is_null()) {
                const auto v = int64Of(*t);
                if (!v) { error = "\"targetId\" is an integer"; return std::nullopt; }
                q.targetId = *v;
            }
            cs.ops.push_back(std::move(q));
        }
        return cs;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

}  // namespace adi::agent
