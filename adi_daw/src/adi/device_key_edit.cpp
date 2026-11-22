// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/device_key_routes.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>
namespace adi {
namespace {
std::string identity(Store &s) {
    SQLite::Statement q(s.db(), "PRAGMA database_list");
    while (q.executeStep())
        if (q.getColumn(1).getString() == "main")
            return q.getColumn(2).getString();
    throw std::runtime_error("key routing needs a project");
}
} // namespace
PreparedDeviceKey prepareDeviceKey(Store &store, std::int64_t device,
                                   std::optional<std::int64_t> source, std::string tap) {
    PreparedDeviceKey result;
    try {
        if (audio::onAudioThread)
            throw std::runtime_error("key routing belongs off the callback");
        if (store.readOnly())
            throw std::runtime_error("project is read-only");
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(store.db().getHandle()))
            read.emplace(store.db(), SQLite::TransactionBehavior::DEFERRED);
        result.sourceFile = identity(store);
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        const auto target = detail::captureRow(store.db(), "devices", "id", device);
        if (!target || target->at("is_rack") != 0)
            throw std::runtime_error("key destination is missing or is a rack");
        if (source && !detail::captureRow(store.db(), "tracks", "id", *source))
            throw std::runtime_error("key source channel is missing");
        if (tap != "pre_fx" && tap != "post_fx" && tap != "post_mixer")
            throw std::runtime_error("unknown key tap");
        result.candidate = rows::readModel(store);
        OpTransactionInputs guards;
        for (const auto *table :
             {"tracks", "device_chains", "devices", "routing", "routing_key_taps"})
            guards.rowSetGuards.push_back(
                {table, Payload::object(),
                 detail::captureGuardRows(store, table, Payload::object())});
        std::vector<rows::Routing> previous;
        for (const auto &r : result.candidate.routing)
            if (r.dstKind == "device" && r.dstId == device && r.kind == "sidechain")
                previous.push_back(r);
        const bool unchanged = source && previous.size() == 1 && previous[0].srcKind == "track" &&
                               previous[0].srcId == *source && previous[0].keyTap == tap &&
                               previous[0].enabled;
        auto append = [&](const char *type, Payload p) {
            OpRequest r;
            r.opType = type;
            r.payload = std::move(p);
            r.label = source ? "Set Device Sidechain" : "Remove Device Sidechain";
            r.targetKind = "device";
            r.targetId = device;
            result.requests.push_back(std::move(r));
        };
        if (!unchanged) {
            for (const auto &r : previous)
                append("routing.disconnect", {{"id", r.id}});
            std::erase_if(result.candidate.routing, [&](const auto &r) {
                return r.dstKind == "device" && r.dstId == device && r.kind == "sidechain";
            });
            if (source) {
                std::int64_t id = 0;
                for (const auto &r : result.candidate.routing)
                    id = std::max(id, r.id);
                for (const auto &r : previous)
                    id = std::max(id, r.id);
                if (id == std::numeric_limits<std::int64_t>::max())
                    throw std::runtime_error("routing identities exhausted");
                ++id;
                rows::Routing r;
                r.id = id;
                r.srcKind = "track";
                r.srcId = *source;
                r.dstKind = "device";
                r.dstId = device;
                r.kind = "sidechain";
                r.keyTap = tap;
                Payload p = {
                    {"id", id},      {"srcKind", "track"},  {"src", *source}, {"dstKind", "device"},
                    {"dst", device}, {"kind", "sidechain"}, {"keyTap", tap}};
                if (previous.size() == 1) {
                    auto row = detail::captureRow(store.db(), "routing", "id", previous[0].id);
                    (*row)["id"] = id;
                    (*row)["src_kind"] = "track";
                    (*row)["src_id"] = *source;
                    (*row)["enabled"] = 1;
                    p["row"] = *row;
                    r.gainDb = previous[0].gainDb;
                    r.pan = previous[0].pan;
                }
                append("routing.connect", std::move(p));
                result.candidate.routing.push_back(std::move(r));
            }
        }
        if (!result.requests.empty()) {
            auto packet =
                CapturedTransactionAccess::seal(appliedSource(store), *result.options.expectHead,
                                                result.requests, std::move(guards));
            for (auto &r : result.requests)
                r.capturedTransaction = packet;
        }
        result.ok = true;
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    return result;
}
CommitResult commitDeviceKey(Store &store, const PreparedDeviceKey &p, Actor actor) {
    CommitResult r;
    if (!p.ok) {
        r.error = p.error;
        return r;
    }
    if (identity(store) != p.sourceFile) {
        r.error = "project changed after key route preparation";
        r.stale = true;
        return r;
    }
    auto requests = p.requests;
    for (auto &request : requests)
        request.actor = actor;
    return OpJournal(store).commit(requests, p.options);
}
} // namespace adi
