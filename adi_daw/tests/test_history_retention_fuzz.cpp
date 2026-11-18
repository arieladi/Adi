// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/check.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <bit>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace adi;
using Tables = std::map<std::string, std::vector<Payload>>;
int checks = 0, failures = 0;
void check(bool value, const std::string& label) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label.c_str());
    }
}
void require(bool value, const std::string& label) {
    if (!value)
        throw std::runtime_error(label);
}
struct Random {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t pick(std::size_t n) {
        require(n != 0, "nonempty fuzz choice");
        return static_cast<std::size_t>(next() % n);
    }
};
struct State {
    std::string name;
    double volume = 0;
    bool operator==(const State&) const = default;
};
Tables material(const Store& store) {
    // Independent SQL collector, not checkpoint/digest code. Kind tags and REAL
    // bits forbid JSON's cross-kind numeric equality; BLOBs compare every byte.
    constexpr std::array tables{"project",
                                "tracks",
                                "mixer_strip",
                                "routing",
                                "clips",
                                "event_streams",
                                "clip_properties",
                                "clip_playback_details",
                                "mixer_pan_settings",
                                "tempo_map",
                                "markers",
                                "audio_clips",
                                "automation_lanes",
                                "automation_data",
                                "automation_segment_ranges",
                                "devices",
                                "device_chains",
                                "plugin_refs",
                                "plugin_params",
                                "plugin_state",
                                "state_blobs"};
    Tables result;
    for (const auto* table : tables) {
        SQLite::Statement query(store.db(),
                                "SELECT * FROM \"" + std::string(table) + "\" ORDER BY 1,2");
        auto& rows = result[table];
        while (query.executeStep()) {
            Payload row = Payload::object();
            for (int i = 0; i < query.getColumnCount(); ++i) {
                const auto cell = query.getColumn(i);
                Payload value;
                switch (cell.getType()) {
                case SQLITE_INTEGER:
                    value = cell.getInt64();
                    break;
                case SQLITE_FLOAT:
                    value = std::bit_cast<std::uint64_t>(cell.getDouble());
                    break;
                case SQLITE_TEXT:
                    value = cell.getString();
                    break;
                case SQLITE_NULL:
                    value = nullptr;
                    break;
                case SQLITE_BLOB: {
                    std::vector<std::uint8_t> bytes;
                    if (cell.getBytes() > 0) {
                        const auto* start = static_cast<const std::uint8_t*>(cell.getBlob());
                        bytes.assign(start, start + cell.getBytes());
                    }
                    value = Payload::binary(std::move(bytes));
                    break;
                }
                default:
                    throw std::runtime_error("unsupported fuzz SQL kind");
                }
                row[query.getColumnName(i)] = {{"kind", cell.getType()},
                                               {"value", std::move(value)}};
            }
            rows.push_back(std::move(row));
        }
    }
    return result;
}
struct Project {
    test::TempDirectory directory{"history_retention_fuzz", "project"};
    std::unique_ptr<Store> store;
    Tables original;
    State initial;
    Project() {
        StoreError error{};
        store = Store::create(directory.path() / "song.adi", error);
        require(bool(store), "create history fuzz project");
        store->db().exec("ALTER TABLE project ADD COLUMN future_payload BLOB;UPDATE project SET "
                         "future_payload=X'00FF800001';"
                         "INSERT INTO tracks(id,kind,name,index_in_parent) "
                         "VALUES(1,'midi','Notes',0),(9,'master','Main',1);"
                         "INSERT INTO mixer_strip(track_id) VALUES(1),(9);"
                         "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
                         "VALUES(1,'track',1,'track',9,'main');"
                         "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) "
                         "VALUES(4,1,'midi',0,0,384384)");
        NoteRecord note{};
        note.note_id = UINT64_MAX - 7;
        note.key = 60;
        note.vel_on = 100;
        note.dur_ticks = 96096;
        note.probability = 10000;
        require(store->putEventStream(4, "notes",
                                      writeStream<NoteRecord>(FourCC::Notes, std::vector{note})),
                "seed retained note bytes");
        original = material(*store);
        initial = {store->db().execAndGet("SELECT name FROM project").getString(), 0};
        require(original.at("project").size() == 1 && original.at("mixer_strip").size() == 2,
                "literal independent fixture keys");
    }
    bool equals(const State& state) const {
        auto expected = original;
        expected.at("project").at(0).at("name").at("value") = state.name;
        expected.at("mixer_strip").at(0).at("volume_db") = {
            {"kind", SQLITE_FLOAT}, {"value", std::bit_cast<std::uint64_t>(state.volume)}};
        return material(*store) == expected;
    }
};
OpRequest op(const char* type, Payload payload, const std::string& label) {
    OpRequest value;
    value.opType = type;
    value.payload = std::move(payload);
    value.label = label;
    return value;
}
struct Step {
    State before, after;
    std::int64_t seq = 0;
};
constexpr auto prepared = History::StoredRestore::SessionReloadPrepared;
void seeded(std::uint64_t seed, std::int64_t cap) {
    Project p;
    History history(*p.store);
    Random random{seed};
    UndoRetentionPolicy policy;
    policy.maxTransactions = cap;
    require(history.setUndoRetentionPolicy(policy).ok, "configure seeded cap");
    const auto origin = history.takeSnapshot("Origin", 0, 0);
    require(origin.ok, origin.error);
    std::vector<std::pair<std::int64_t, State>> saved{{origin.id, p.initial}};
    std::vector<Step> line;
    std::size_t cursor = 0;
    State current = p.initial;
    std::map<std::int64_t, State> bySeq, branches;
    std::set<std::int64_t> seenBranches;
    int commits = 0, undos = 0, redos = 0, restores = 0, forks = 0, switches = 0;
    for (int iteration = 0; iteration < 416; ++iteration) {
        const auto label = "seed " + std::to_string(seed) + " step " + std::to_string(iteration);
        auto action = random.pick(100);
        if ((action >= 60 && action < 77 && cursor == 0) ||
            (action >= 77 && action < 85 && cursor == line.size()))
            action = 0;
        if (action < 60) {
            State after = current;
            std::vector<OpRequest> requests;
            const auto n = 1 + random.pick(3);
            for (std::size_t i = 0; i < n; ++i) {
                if (random.pick(2) == 0) {
                    after.name = "N" + std::to_string(random.pick(37));
                    requests.push_back(op("project.setName", {{"name", after.name}}, label));
                } else {
                    after.volume = static_cast<double>(random.pick(121)) * .25 - 24;
                    requests.push_back(
                        op("mixer.setVolume", {{"id", 1}, {"db", after.volume}}, label));
                }
            }
            const auto result = OpJournal(*p.store).commit(requests);
            check(result.ok && result.retentionWarning.empty(),
                  label + ": whole transaction admitted");
            if (result.ok) {
                require(result.seqs.size() == requests.size(), "complete committed seq list");
                forks += cursor < line.size() ? 1 : 0;
                line.resize(cursor);
                line.push_back({current, after, result.seqs.back()});
                ++cursor;
                ++commits;
                current = after;
                bySeq[result.seqs.back()] = after;
            }
        } else if (action < 77) {
            const auto head = history.head();
            require(cursor > 0, "known undo origin");
            const auto expected = line.at(cursor - 1).before;
            const auto result = history.undo();
            check(result.ok || !history.canUndo(), label + ": undo or retained-root refusal");
            if (result.ok) {
                current = expected;
                --cursor;
                ++undos;
            } else
                check(history.head() == head, label + ": refused undo holds head");
        } else if (action < 85) {
            require(cursor < line.size(), "known redo target");
            const auto expected = line.at(cursor).after;
            const auto result = history.redo();
            check(result.ok, label + ": known redo survives compaction");
            if (result.ok) {
                current = expected;
                ++cursor;
                ++redos;
            }
        } else if (action < 90) {
            const auto snapshot = history.takeSnapshot(label, iteration, 0);
            check(snapshot.ok, label + ": save named point");
            if (snapshot.ok)
                saved.emplace_back(snapshot.id, current);
        } else if (action < 98) {
            const auto& point = saved.at(random.pick(saved.size()));
            // No renderer is attached: this caller is already stopped/reloadable.
            const auto result = history.revertToSnapshot(point.first, iteration, prepared);
            check(result.ok, label + ": restore exact named state");
            if (result.ok) {
                current = point.second;
                line.clear();
                cursor = 0;
                ++restores;
            }
        } else if (!branches.empty()) {
            auto it = branches.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(random.pick(branches.size())));
            const auto result = history.switchToBranch(it->first, prepared);
            check(result.ok, label + ": retained branch remains reachable");
            if (result.ok) {
                current = it->second;
                line.clear();
                cursor = 0;
                ++switches;
            }
        }
        check(p.equals(current), label + ": independent full typed material oracle");
        const auto timeline = history.readTimeline();
        check(timeline.error.empty() && timeline.steps.size() <= static_cast<std::size_t>(cap),
              label + ": global whole-transaction cap and timeline");
        std::set<std::int64_t> present;
        for (const auto& branch : timeline.branches) {
            present.insert(branch.id);
            if (branch.isCurrent)
                branches.erase(branch.id);
            else if (branch.headSeq && bySeq.contains(*branch.headSeq))
                branches[branch.id] = bySeq.at(*branch.headSeq);
        }
        bool kept = true;
        for (const auto id : seenBranches)
            kept = kept && present.contains(id);
        check(kept, label + ": no branch identity discarded by compaction");
        seenBranches = std::move(present);
    }
    std::printf("seed %llu coverage commits=%d undo=%d redo=%d restores=%d forks=%d switches=%d\n",
                static_cast<unsigned long long>(seed), commits, undos, redos, restores, forks,
                switches);
    check(commits > 150 && undos > 25 && redos > 3 && restores > 15 && forks > 10 && switches > 0,
          "seed covers commits, whole undo/redo, forks, stored restores, branch switches");
    for (const auto& [id, expected] : saved) {
        const auto result = history.revertToSnapshot(id, 1000, prepared);
        check(result.ok && p.equals(expected), "every seeded named state remains byte/kind exact");
    }
    check(checkProject(*p.store).clean(), "seeded forest and native material pass integrity");
}
void threeThousand() {
    Project p;
    History history(*p.store);
    UndoRetentionPolicy policy;
    require(policy.maxTransactions == 2000, "director's default2000 cap");
    require(history.setUndoRetentionPolicy(policy).ok, "configure default policy");
    const auto origin = history.takeSnapshot("Day one", 0, 0);
    const auto alias = history.takeSnapshot("Day one alias", 1, 0);
    require(origin.ok && alias.ok, "save day-one states");
    State current = p.initial;
    for (int step = 0; step < 3000; ++step) {
        const State next{"Long " + std::to_string(step % 37),
                         static_cast<double>(step % 121) * .25 - 24};
        const std::array requests{
            op("project.setName", {{"name", next.name}}, "long edit"),
            op("mixer.setVolume", {{"id", 1}, {"db", next.volume}}, "long edit")};
        const auto result = OpJournal(*p.store).commit(requests);
        check(result.ok && result.retentionWarning.empty(),
              "3000: named day-one point never blocks a commit");
        if (result.ok)
            current = next;
        check(p.equals(current), "3000: independent typed material exact after every commit");
        const auto retained =
            p.store->db()
                .execAndGet("SELECT count(DISTINCT txn_id) FROM ops WHERE op_type<>'history.root'")
                .getInt64();
        check(retained <= 2000, "3000: default cap holds after every two-op edit");
        SQLite::Statement group(
            p.store->db(), "SELECT count(*) FROM ops WHERE txn_id=? AND op_type<>'history.root'");
        group.bind(1, result.txnId);
        require(group.executeStep(), "count complete current group");
        check(result.ok && result.seqs.size() == 2 && history.head() == result.seqs.back() &&
                  group.getColumn(0).getInt64() == 2,
              "3000: newest complete two-op group remains at the current head");
        if (step % 128 == 0) {
            const auto timeline = history.readTimeline();
            const auto undo = history.nextUndo();
            check(timeline.error.empty() && result.seqs.size() == 2 && undo && undo->opCount == 2 &&
                      undo->lastSeq == result.seqs.back(),
                  "3000: periodic full timeline and public Undo agree with the current group");
        }
    }
    const auto retained = history.readTimeline();
    check(retained.error.empty() && retained.steps.size() >= 1800 && retained.steps.size() <= 2000,
          "3000: count pressure retains at least the default low-water depth");
    const auto points = history.listSnapshots();
    std::optional<std::string> first, second;
    for (const auto& point : points) {
        if (point.id == origin.id)
            first = point.storedStateHash;
        if (point.id == alias.id)
            second = point.storedStateHash;
    }
    check(first && second && first == second, "day-one aliases share one stored material hash");
    const auto before = current;
    const auto back = history.revertToSnapshot(origin.id, 4000, prepared);
    check(back.ok && p.equals(p.initial),
          "day-one snapshot restores all rows/kinds/bytes after3000");
    const auto timeline = history.readTimeline();
    bool retainedFinal = false;
    for (const auto& branch : timeline.branches) {
        if (branch.isCurrent)
            continue;
        const auto result = history.switchToBranch(branch.id, prepared);
        if (result.ok && p.equals(before)) {
            retainedFinal = true;
            break;
        }
    }
    check(retainedFinal, "departing3000-step tip remains reachable after day-one restore");
    check(checkProject(*p.store).clean(), "3000-step forest passes integrity");
}
} // namespace
int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    if (argc > 2 || (selected != "all" && selected != "long" && selected != "seed1" &&
                     selected != "seed2" && selected != "seed3")) {
        std::fprintf(stderr, "usage: history_retention_fuzz [all|long|seed1|seed2|seed3]\n");
        return 2;
    }
    if (selected == "all" || selected == "long") {
        const auto before = checks;
        try {
            threeThousand();
        } catch (const std::exception& e) {
            check(false, std::string("unexpected long fixture exception: ") + e.what());
        }
        std::printf("case long: %d checks\n", checks - before);
    }
    const std::array seeds{0x4849324300112026ULL, 0x4849324300222026ULL, 0x4849324300332026ULL};
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        const auto name = "seed" + std::to_string(i + 1);
        if (selected != "all" && selected != name)
            continue;
        const auto before = checks;
        try {
            seeded(seeds[i], 31);
        } catch (const std::exception& e) {
            check(false, std::string("unexpected seeded fixture exception: ") + e.what());
        }
        std::printf("case %s: %d checks\n", name.c_str(), checks - before);
    }
    std::printf("history retention fuzz: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
