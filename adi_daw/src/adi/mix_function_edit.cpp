// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix_function_edit.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
namespace adi::mix {
namespace {
std::string projectIdentity(SQLite::Database &db) {
    SQLite::Statement row(db, "PRAGMA database_list");
    while (row.executeStep()) if (row.getColumn(1).getString() == "main") return row.getColumn(2).getString();
    throw std::runtime_error("function edit needs a project");
}
std::int64_t nextId(SQLite::Database &db, const char *table) {
    SQLite::Statement row(db, std::string("SELECT COALESCE(MAX(id),0) FROM ") + table);
    row.executeStep();
    const auto last = std::max<std::int64_t>(0, row.getColumn(0).getInt64());
    if (last == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("function identities are exhausted");
    return last + 1;
}
std::int64_t owner(SQLite::Database &db, std::int64_t chain) {
    std::set<std::int64_t> seen;
    for (;;) {
        if (!seen.insert(chain).second) throw std::runtime_error("destination rack contains a cycle");
        SQLite::Statement row(db, "SELECT track_id,parent_device_id FROM device_chains WHERE id=?");
        row.bind(1, chain);
        if (!row.executeStep()) throw std::runtime_error("device chain is missing");
        if (!row.getColumn(0).isNull()) return row.getColumn(0).getInt64();
        if (row.getColumn(1).isNull()) throw std::runtime_error("device chain has no owning channel");
        SQLite::Statement parent(db, "SELECT chain_id FROM devices WHERE id=?");
        parent.bind(1, row.getColumn(1).getInt64());
        if (!parent.executeStep()) throw std::runtime_error("device rack owner is missing");
        chain = parent.getColumn(0).getInt64();
    }
}
void append(PreparedFunctionEdit &edit, std::string type, Payload payload, const char *target,
            std::int64_t id, const std::string &label) {
    OpRequest request;
    request.opType = std::move(type); request.payload = std::move(payload); request.label = label;
    request.targetKind = target; request.targetId = id;
    edit.requests.push_back(std::move(request));
}
void initialise(Store &store, PreparedFunctionEdit &edit) {
    if (store.readOnly()) throw std::runtime_error("project is read-only");
    edit.sourceFile = projectIdentity(store.db());
    edit.options.expectHead.emplace(OpJournal(store).headSeq());
}
void requireTrack(SQLite::Database &db, std::int64_t track) {
    SQLite::Statement row(db, "SELECT 1 FROM tracks WHERE id=?"); row.bind(1, track);
    if (!row.executeStep()) throw std::runtime_error("destination channel is missing");
}
std::int64_t rootChain(SQLite::Database &db, std::int64_t track) {
    SQLite::Statement row(db, "SELECT id FROM device_chains WHERE track_id=? AND parent_device_id IS NULL ORDER BY ord,id LIMIT 1");
    row.bind(1, track); return row.executeStep() ? row.getColumn(0).getInt64() : 0;
}
struct OrderedDevice {
    std::int64_t id = 0, ord = 0;
    const FunctionInfo *function = nullptr;
};
std::vector<OrderedDevice> chainOrder(SQLite::Database &db, std::int64_t chain) {
    SQLite::Statement rows(db, "SELECT d.id,d.ord,p.format,p.uid FROM devices d LEFT JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE d.chain_id=? ORDER BY d.ord,d.id");
    rows.bind(1, chain); std::vector<OrderedDevice> order;
    while (rows.executeStep()) order.push_back({rows.getColumn(0).getInt64(), rows.getColumn(1).getInt64(),
        rows.getColumn(2).isNull() ? nullptr : functionForReference(rows.getColumn(2).getString(), rows.getColumn(3).getString())});
    return order;
}
std::int64_t ordinal(std::size_t index) {
    if (!std::in_range<std::int64_t>(index)) throw std::runtime_error("device order is exhausted");
    return static_cast<std::int64_t>(index);
}
Payload referenceRow(SQLite::Database &db, const Payload &values) {
    SQLite::Statement ddl(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='plugin_refs'");
    if (!ddl.executeStep()) throw std::runtime_error("plug-in reference schema is missing");
    SQLite::Database scratch(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    scratch.exec(ddl.getColumn(0).getString()); detail::restoreRow(scratch, "plugin_refs", values);
    return *detail::captureRow(scratch, "plugin_refs", "id", values.at("id").get<std::int64_t>());
}
}
PreparedFunctionEdit prepareFunctionInsert(Store &store, Function kind, std::int64_t track, bool engineAvailable) {
    PreparedFunctionEdit edit;
    try {
        const auto *info = functionInfo(kind);
        if (!info) throw std::runtime_error("unknown Mix View function");
        if (!engineAvailable) throw std::runtime_error(std::string(info->name) + " engine is not available in this build");
        auto &db = store.db(); std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0) read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        initialise(store, edit);
        requireTrack(db, track);
        SQLite::Statement existing(db, "SELECT d.chain_id FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.format='internal' AND p.uid=?");
        existing.bind(1, std::string(info->uid));
        while (existing.executeStep()) if (owner(db, existing.getColumn(0).getInt64()) == track)
            throw std::runtime_error(std::string(info->name) + " is already present on this channel");
        const std::string label = "Add " + std::string(info->name) + " Function";
        SQLite::Statement reference(db, "SELECT id FROM plugin_refs WHERE format='internal' AND uid=? AND shell_id IS NULL ORDER BY id LIMIT 1");
        reference.bind(1, std::string(info->uid));
        if (reference.executeStep()) edit.referenceId = reference.getColumn(0).getInt64();
        else {
            edit.referenceId = nextId(db, "plugin_refs");
            auto row = referenceRow(db, {{"id", edit.referenceId}, {"format", "internal"}, {"uid", info->uid},
                {"vendor", "ADI"}, {"name", info->name}, {"version", "1"}, {"subtype", "effect"},
                {"path_hint", ""}, {"is_shell", 0}, {"shell_id", nullptr}});
            append(edit, "pluginRef.register", {{"id", edit.referenceId}, {"row", row}}, "plugin", edit.referenceId, label);
        }
        edit.chainId = rootChain(db, track);
        if (!edit.chainId) {
            edit.chainId = nextId(db, "device_chains");
            append(edit, "chain.create", {{"id", edit.chainId}, {"track", track}, {"ord", 0}, {"name", "Devices"}}, "chain", edit.chainId, label);
        }
        const auto order = chainOrder(db, edit.chainId);
        std::size_t tail = order.size();
        while (tail > 0 && order[tail - 1].function) --tail;
        std::size_t position = order.size();
        if (kind == Function::Eq) position = tail;
        else if (kind != Function::Limiter)
            for (auto i = tail; i < order.size(); ++i)
                if (order[i].function->kind == Function::Limiter) { position = i; break; }
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto next = ordinal(i + (i >= position ? 1u : 0u));
            if (next != order[i].ord)
                append(edit, "device.move", {{"id", order[i].id}, {"chain", edit.chainId}, {"ord", next}}, "device", order[i].id, label);
        }
        edit.deviceId = nextId(db, "devices");
        append(edit, "device.insert", {{"id", edit.deviceId}, {"chain", edit.chainId}, {"ord", ordinal(position)},
            {"ref", edit.referenceId}, {"name", info->name}, {"enabled", true}, {"rack", false}}, "device", edit.deviceId, label);
        edit.ok = true;
    } catch (const std::exception &e) { edit.error = e.what(); }
    return edit;
}
PreparedFunctionEdit prepareFunctionMove(Store &store, std::int64_t device, std::int64_t track,
                                        std::optional<std::int64_t> before) {
    PreparedFunctionEdit edit;
    try {
        auto &db = store.db(); std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0) read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        initialise(store, edit);
        requireTrack(db, track);
        SQLite::Statement source(db, "SELECT d.chain_id,p.format,p.uid FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE d.id=?");
        source.bind(1, device);
        if (!source.executeStep()) throw std::runtime_error("function device is missing");
        const auto *info = functionForReference(source.getColumn(1).getString(), source.getColumn(2).getString());
        if (!info) throw std::runtime_error("only a native Mix View function can use this move");
        const auto sourceChain = source.getColumn(0).getInt64();
        if (owner(db, sourceChain) != track) throw std::runtime_error("a function tab moves within its own channel");
        edit.deviceId = device;
        if (before) {
            SQLite::Statement destination(db, "SELECT chain_id FROM devices WHERE id=?"); destination.bind(1, *before);
            if (!destination.executeStep()) throw std::runtime_error("destination device is missing");
            edit.chainId = destination.getColumn(0).getInt64();
        } else edit.chainId = rootChain(db, track);
        if (!edit.chainId || owner(db, edit.chainId) != track) throw std::runtime_error("destination belongs to another channel");
        if (before == device) { edit.ok = true; return edit; }
        auto from = chainOrder(db, sourceChain);
        auto to = edit.chainId == sourceChain ? from : chainOrder(db, edit.chainId);
        const auto originalTo = to;
        std::erase_if(from, [device](const auto &d) { return d.id == device; });
        std::erase_if(to, [device](const auto &d) { return d.id == device; });
        auto insertion = to.end();
        if (before) insertion = std::find_if(to.begin(), to.end(), [&](const auto &d) { return d.id == *before; });
        if (before && insertion == to.end()) throw std::runtime_error("destination device changed");
        const auto old = std::find_if(originalTo.begin(), originalTo.end(), [device](const auto &d) { return d.id == device; });
        const auto moved = old == originalTo.end() ? OrderedDevice{device, 0, info} : *old;
        to.insert(insertion, moved);
        if (edit.chainId == sourceChain && std::equal(to.begin(), to.end(), originalTo.begin(), originalTo.end(),
                [](const auto &a, const auto &b) { return a.id == b.id; })) { edit.ok = true; return edit; }
        const std::string label = "Move " + std::string(info->name) + " Function";
        const auto normalise = [&](const auto &list, std::int64_t chain) {
            for (std::size_t i = 0; i < list.size(); ++i)
                if ((list[i].id == device && chain != sourceChain) || ordinal(i) != list[i].ord)
                    append(edit, "device.move", {{"id", list[i].id}, {"chain", chain}, {"ord", ordinal(i)}}, "device", list[i].id, label);
        };
        if (sourceChain != edit.chainId) normalise(from, sourceChain);
        normalise(to, edit.chainId); edit.ok = true;
    } catch (const std::exception &e) { edit.error = e.what(); }
    return edit;
}
CommitResult commitFunctionEdit(Store &store, const PreparedFunctionEdit &edit, Actor actor) {
    CommitResult result;
    if (!edit.ok) { result.error = edit.error; return result; }
    if (projectIdentity(store.db()) != edit.sourceFile) { result.error = "project changed after function edit preparation"; result.stale = true; return result; }
    if (edit.requests.empty()) { result.ok = true; return result; }
    auto requests = edit.requests; for (auto &request : requests) request.actor = actor;
    return OpJournal(store).commit(requests, edit.options);
}
} // namespace adi::mix
