// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: convergence tests for the collaboration core:
//   * Two replicas concurrently inserting a device at the same slot converge
//     to identical device order and identical digestProject() regardless of
//     which replica wins seq 1.
//   * Three real SQLite Stores and a FakeObjectStore across 200 seeds with
//     visibility delays, transient drops, and dropped PUT ACKs converge to
//     equal digests and equal sibling order with nothing pending.
//   * Multi-threaded 3-replica run clean under ASan and TSan.

#include "adi/collab/replica.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace adi;
using namespace adi::collab;
namespace fs = std::filesystem;

constexpr const char* kClient1 = "11111111111111111111111111111111";
constexpr const char* kClient2 = "22222222222222222222222222222222";
constexpr const char* kClient3 = "33333333333333333333333333333333";

constexpr std::int64_t kSharedTrackId = 1;
constexpr std::int64_t kSharedChainId = 10;
constexpr std::int64_t kSeedDeviceId = 100;

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
        std::fflush(stdout);
    }
}

void section(const char* name) {
    std::printf("[%s]\n", name);
}

/// Create a baseline .adi project containing one audio track (1), one chain
/// (10), and one seed device (100), then close it so replicas can copy it as
/// their identical starting state.
fs::path createBaselineTemplate(const fs::path& dir) {
    const fs::path path = dir / "baseline.adi";
    StoreError err{};
    auto store = Store::create(path, err);
    if (!store) {
        throw std::runtime_error(std::string("createBaselineTemplate failed: ") + toString(err));
    }
    OpJournal journal(*store);
    std::vector<OpRequest> ops;

    OpRequest tr;
    tr.opType = "track.create";
    tr.payload = {{"id", kSharedTrackId}, {"kind", "audio"}, {"name", "Track 1"},
                  {"parent", nullptr}, {"index", 0}};
    ops.push_back(std::move(tr));

    OpRequest ch;
    ch.opType = "chain.create";
    ch.payload = {{"id", kSharedChainId}, {"track", kSharedTrackId}, {"ord", 0},
                  {"name", "Devices"}};
    ops.push_back(std::move(ch));

    OpRequest dev;
    dev.opType = "device.insert";
    dev.payload = {{"id", kSeedDeviceId}, {"chain", kSharedChainId}, {"ord", 0},
                   {"ref", nullptr}, {"name", "SeedDev"}, {"enabled", true}, {"rack", false}};
    ops.push_back(std::move(dev));

    if (!journal.commit(ops).ok || !store->close()) {
        throw std::runtime_error("baseline commit/close failed");
    }
    return path;
}

std::unique_ptr<Store> openCopyFromTemplate(const fs::path& tpl, const fs::path& dst) {
    fs::copy_file(tpl, dst, fs::copy_options::overwrite_existing);
    StoreError err{};
    auto store = Store::open(dst, err, /*readOnly=*/false);
    if (!store) {
        throw std::runtime_error(std::string("openCopyFromTemplate failed: ") + toString(err));
    }
    return store;
}

BatchOp makeDeviceInsertOp(CollabReplica& rep, std::size_t slot, const std::string& name) {
    const std::int64_t id = rep.allocator().next("devices");
    BatchOp op;
    op.opType = "device.insert";
    op.payload = {{"id", id},
                  {"chain", kSharedChainId},
                  {"ord", static_cast<std::int64_t>(slot)},
                  {"ref", nullptr},
                  {"name", name},
                  {"enabled", true},
                  {"rack", false}};
    op.label = "Insert " + name;
    op.targetKind = "device";
    op.targetId = id;
    op.orderKey = rep.keyForDeviceSlot(kSharedChainId, slot);
    return op;
}

BatchOp makeTrackCreateOp(CollabReplica& rep, std::size_t slot, const std::string& name) {
    const std::int64_t id = rep.allocator().next("tracks");
    BatchOp op;
    op.opType = "track.create";
    op.payload = {{"id", id},
                  {"kind", "audio"},
                  {"name", name},
                  {"parent", nullptr},
                  {"index", static_cast<std::int64_t>(slot)}};
    op.label = "Create " + name;
    op.targetKind = "track";
    op.targetId = id;
    op.orderKey = rep.keyForTrackSlot(slot);
    return op;
}

void drainUntilConverged(std::vector<CollabReplica*>& replicas, FakeObjectStore& bucket) {
    bucket.healNetwork();
    for (int round = 0; round < 200; ++round) {
        bool anyWork = false;
        for (auto* rep : replicas) {
            const auto res = rep->syncStep(bucket);
            if (res.status == SyncStatus::Error) {
                check(false, "syncStep error during drain: " + res.error);
                return;
            }
            if (res.status != SyncStatus::Idle) {
                anyWork = true;
            }
        }
        bool allPendingEmpty = true;
        const std::uint64_t seq0 = replicas[0]->appliedSeq();
        bool allSeqEqual = true;
        for (const auto* rep : replicas) {
            if (rep->pendingCount() != 0U) {
                allPendingEmpty = false;
            }
            if (rep->appliedSeq() != seq0) {
                allSeqEqual = false;
            }
        }
        if (!anyWork && allPendingEmpty && allSeqEqual) {
            return;
        }
    }
}

void testConcurrentDeviceInsertAtOneSlot() {
    section("two replicas inserting a device at one slot converge");

    test::TempDirectory temp{"collab_sync", "one_slot"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    // Run both tie-orders: first A wins seq 1, then B wins seq 1.
    for (int winner = 0; winner < 2; ++winner) {
        auto sA = openCopyFromTemplate(tpl, temp.path() / ("a_" + std::to_string(winner) + ".adi"));
        auto sB = openCopyFromTemplate(tpl, temp.path() / ("b_" + std::to_string(winner) + ".adi"));
        CollabReplica rA(*sA, kClient1, 1U);
        CollabReplica rB(*sB, kClient2, 2U);
        FakeObjectStore bucket;

        std::string err;
        check(rA.commitLocal({makeDeviceInsertOp(rA, 0U, "CompA")}, err),
              "replica A local insert at slot 0: " + err);
        check(rB.commitLocal({makeDeviceInsertOp(rB, 0U, "EqB")}, err),
              "replica B local insert at slot 0: " + err);

        // Winner pushes first; loser hits AlreadyExists, pulls, rebases, and pushes seq 2.
        if (winner == 0) {
            check(rA.syncStep(bucket).pushedOne, "A wins seq 1");
        } else {
            check(rB.syncStep(bucket).pushedOne, "B wins seq 1");
        }

        std::vector<CollabReplica*> reps = {&rA, &rB};
        drainUntilConverged(reps, bucket);

        check(rA.pendingCount() == 0U && rB.pendingCount() == 0U,
              "nothing pending after slot collision");
        check(rA.appliedSeq() == 2U && rB.appliedSeq() == 2U, "both at seq 2");

        const auto orderA = rA.deviceOrderInChain(kSharedChainId);
        const auto orderB = rB.deviceOrderInChain(kSharedChainId);
        check(orderA.size() == 3U && orderA == orderB,
              "device sibling order converged identically");

        const std::string digA = digestProject(*sA).text;
        const std::string digB = digestProject(*sB).text;
        check(digA == digB, "digestProject equal after concurrent slot-0 device insert");

        // Verify SQLite devices.ord matches canonical rank on both stores.
        std::vector<std::int64_t> sqlOrderA;
        std::vector<std::int64_t> sqlOrderB;
        {
            SQLite::Statement qA(sA->db(), "SELECT id FROM devices WHERE chain_id=? ORDER BY ord,id");
            qA.bind(1, kSharedChainId);
            while (qA.executeStep()) {
                sqlOrderA.push_back(qA.getColumn(0).getInt64());
            }
            SQLite::Statement qB(sB->db(), "SELECT id FROM devices WHERE chain_id=? ORDER BY ord,id");
            qB.bind(1, kSharedChainId);
            while (qB.executeStep()) {
                sqlOrderB.push_back(qB.getColumn(0).getInt64());
            }
        }
        check(sqlOrderA == orderA && sqlOrderB == orderB,
              "SQLite devices.ord matches fractional order");
    }
}

void testThreeStores200SeedsWithDelaysAndDrops() {
    section("3 Stores over 200 seeds with delays and drops");

    test::TempDirectory temp{"collab_sync", "200_seeds"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    int seedsPassed = 0;
    for (std::uint64_t seed = 1ULL; seed <= 200ULL; ++seed) {
        const fs::path p1 = temp.path() / "s1.adi";
        const fs::path p2 = temp.path() / "s2.adi";
        const fs::path p3 = temp.path() / "s3.adi";

        auto s1 = openCopyFromTemplate(tpl, p1);
        auto s2 = openCopyFromTemplate(tpl, p2);
        auto s3 = openCopyFromTemplate(tpl, p3);

        CollabReplica r1(*s1, kClient1, 1U);
        CollabReplica r2(*s2, kClient2, 2U);
        CollabReplica r3(*s3, kClient3, 3U);
        std::vector<CollabReplica*> reps = {&r1, &r2, &r3};

        FaultConfig faults;
        faults.seed = seed * 7919ULL + 17ULL;
        faults.putDropRate = 0.15;
        faults.putAckDropRate = 0.10;
        faults.getDropRate = 0.15;
        faults.maxVisibilityDelayTicks = 3U;
        FakeObjectStore bucket(faults);

        std::mt19937_64 rng(seed * 104729ULL + 3ULL);

        // Execute 12 randomized concurrent steps per seed (edits + syncs + ticks).
        for (int step = 0; step < 12; ++step) {
            const std::size_t who = static_cast<std::size_t>(rng() % 3ULL);
            CollabReplica& rep = *reps[who];
            const std::uint64_t action = rng() % 10ULL;

            if (action < 6ULL) {
                std::string err;
                std::vector<BatchOp> ops;
                const std::uint64_t kind = rng() % 6ULL;
                if (kind == 0ULL) {
                    const auto curLen = rep.deviceOrderInChain(kSharedChainId).size();
                    const std::size_t slot = static_cast<std::size_t>(rng() % (curLen + 1U));
                    ops.push_back(makeDeviceInsertOp(
                        rep, slot, "D_" + std::to_string(seed) + "_" + std::to_string(step)));
                } else if (kind == 1ULL) {
                    const auto curLen = rep.topLevelTrackOrder().size();
                    const std::size_t slot = static_cast<std::size_t>(rng() % (curLen + 1U));
                    ops.push_back(makeTrackCreateOp(
                        rep, slot, "T_" + std::to_string(seed) + "_" + std::to_string(step)));
                } else if (kind == 2ULL) {
                    BatchOp op;
                    op.opType = "track.rename";
                    op.payload = {{"id", kSharedTrackId},
                                  {"name", "R" + std::to_string(who) + "_" + std::to_string(step)}};
                    op.targetKind = "track";
                    op.targetId = kSharedTrackId;
                    ops.push_back(std::move(op));
                } else if (kind == 3ULL) {
                    BatchOp op;
                    op.opType = "mixer.setVolume";
                    const double db = -12.0 + static_cast<double>(rng() % 18ULL);
                    op.payload = {{"id", kSharedTrackId}, {"db", db}};
                    op.targetKind = "track";
                    op.targetId = kSharedTrackId;
                    ops.push_back(std::move(op));
                } else if (kind == 4ULL) {
                    BatchOp op;
                    op.opType = "device.setEnabled";
                    op.payload = {{"id", kSeedDeviceId}, {"enabled", (rng() % 2ULL) == 0ULL}};
                    op.targetKind = "device";
                    op.targetId = kSeedDeviceId;
                    ops.push_back(std::move(op));
                } else {
                    const std::int64_t clipId = rep.allocator().next("clips");
                    BatchOp op;
                    op.opType = "clip.create";
                    op.payload = {{"id", clipId},
                                  {"track", kSharedTrackId},
                                  {"kind", "midi"},
                                  {"name", "C" + std::to_string(step)},
                                  {"pos", static_cast<std::int64_t>((rng() % 8ULL) * 480000ULL)},
                                  {"length", 480000}};
                    op.targetKind = "clip";
                    op.targetId = clipId;
                    ops.push_back(std::move(op));
                }
                (void)rep.commitLocal(std::move(ops), err);
            } else {
                const auto res = rep.syncStep(bucket);
                if (res.status == SyncStatus::Error) {
                    check(false, "seed " + std::to_string(seed) + " sync error: " + res.error);
                }
            }

            if ((rng() % 2ULL) == 0ULL) {
                bucket.tick(1U);
            }
        }

        drainUntilConverged(reps, bucket);

        const std::string d1 = digestProject(*s1).text;
        const std::string d2 = digestProject(*s2).text;
        const std::string d3 = digestProject(*s3).text;
        const bool converged =
            r1.pendingCount() == 0U && r2.pendingCount() == 0U && r3.pendingCount() == 0U &&
            r1.appliedSeq() == r2.appliedSeq() && r2.appliedSeq() == r3.appliedSeq() &&
            r1.deviceOrderInChain(kSharedChainId) == r2.deviceOrderInChain(kSharedChainId) &&
            r2.deviceOrderInChain(kSharedChainId) == r3.deviceOrderInChain(kSharedChainId) &&
            r1.topLevelTrackOrder() == r2.topLevelTrackOrder() &&
            r2.topLevelTrackOrder() == r3.topLevelTrackOrder() &&
            d1 == d2 && d2 == d3;

        if (converged) {
            ++seedsPassed;
        } else {
            check(false, "seed " + std::to_string(seed) + " failed to converge");
            break;
        }
    }

    check(seedsPassed == 200, "all 200 seeds converged to equal digests with nothing pending");
}

void testConcurrentThreeThreadsTsan() {
    section("3-replica concurrent threads (ASan + TSan)");

    test::TempDirectory temp{"collab_sync", "tsan_threads"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    FaultConfig faults;
    faults.seed = 424242ULL;
    faults.putDropRate = 0.10;
    faults.putAckDropRate = 0.10;
    faults.getDropRate = 0.10;
    faults.maxVisibilityDelayTicks = 2U;
    FakeObjectStore bucket(faults);

    const char* clients[3] = {kClient1, kClient2, kClient3};
    std::string digests[3];
    std::size_t deviceCounts[3] = {0U, 0U, 0U};
    std::size_t pendingCounts[3] = {1U, 1U, 1U};
    std::atomic<bool> anyErr{false};
    std::atomic<int> doneEditing{0};
    std::atomic<bool> healed{false};

    auto worker = [&](int idx) {
        auto store =
            openCopyFromTemplate(tpl, temp.path() / ("t" + std::to_string(idx + 1) + ".adi"));
        CollabReplica rep(*store, clients[idx], static_cast<std::uint32_t>(idx + 1));

        std::string err;
        for (int i = 0; i < 6; ++i) {
            if (!rep.commitLocal(
                    {makeDeviceInsertOp(rep, 0U,
                                        "Thr" + std::to_string(idx + 1) + "_" + std::to_string(i))},
                    err)) {
                anyErr.store(true);
                return;
            }
            const auto st = rep.syncStep(bucket);
            if (st.status == SyncStatus::Error) {
                anyErr.store(true);
                return;
            }
            bucket.tick(1U);
        }

        if (doneEditing.fetch_add(1) == 2) {
            bucket.healNetwork();
            healed.store(true);
        }
        while (!healed.load()) {
            std::this_thread::yield();
        }

        // Push all local pending batches concurrently, then wait for all peers
        // to finish pushing before pulling final seqs.
        for (int round = 0; round < 2000; ++round) {
            const auto st = rep.syncStep(bucket);
            if (st.status == SyncStatus::Error) {
                anyErr.store(true);
                return;
            }
            if (rep.pendingCount() == 0U && rep.appliedSeq() >= 18U) {
                break;
            }
            if (st.status == SyncStatus::Idle) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        pendingCounts[idx] = rep.pendingCount();
        deviceCounts[idx] = rep.deviceOrderInChain(kSharedChainId).size();
        digests[idx] = digestProject(*store).text;
    };

    std::thread th1(worker, 0);
    std::thread th2(worker, 1);
    std::thread th3(worker, 2);
    th1.join();
    th2.join();
    th3.join();

    check(!anyErr.load(), "no errors across concurrent replica threads");
    check(pendingCounts[0] == 0U && pendingCounts[1] == 0U && pendingCounts[2] == 0U,
          "threaded replicas drained with nothing pending");
    check(deviceCounts[0] == 19U && deviceCounts[1] == 19U && deviceCounts[2] == 19U,
          "all 18 concurrent inserts + 1 seed device present on all 3 threads");
    check(!digests[0].empty() && digests[0] == digests[1] && digests[1] == digests[2],
          "threaded replicas converged to identical digestProject");
}

void testHostileCborAndStrictNumbers() {
    section("hostile CBOR nesting caps and strict non-narrowing numeric decoding");

    // 1. 10,000-deep nested CBOR array, map, and semantic tag must be refused iteratively without crash.
    {
        std::string err;
        std::vector<std::byte> deepArray(10000U, std::byte{0x81});  // 1-element array x 10000
        deepArray.push_back(std::byte{0x00});
        check(!decodeBatch(deepArray, err).has_value(), "10k-deep CBOR array refused safely");

        std::vector<std::byte> deepMap;
        deepMap.reserve(20001U);
        for (int i = 0; i < 10000; ++i) {
            deepMap.push_back(std::byte{0xA1});  // 1-pair map
            deepMap.push_back(std::byte{0x60});  // empty key ""
        }
        deepMap.push_back(std::byte{0x00});
        check(!decodeBatch(deepMap, err).has_value(), "10k-deep CBOR map refused safely");

        std::vector<std::byte> deepTags(10000U, std::byte{0xC0});  // tag 0 x 10000
        deepTags.push_back(std::byte{0x00});
        check(!decodeBatch(deepTags, err).has_value(), "10k-deep CBOR tag chain refused safely");

        // Oversized batch refused before decoding.
        DecodeLimits tiny;
        tiny.maxBatchBytes = 16U;
        std::vector<std::byte> big(32U, std::byte{0x00});
        check(!decodeBatch(big, err, tiny).has_value(), "oversized CBOR batch refused");
    }

    // 2. Strict integer fields refuse floats, booleans, negatives, and >uint32 overflow (no silent narrowing).
    auto makeValidDoc = []() -> Payload {
        return Payload{
            {"seq", std::uint64_t{1}},
            {"batchId", std::string(kClient1) + "-1"},
            {"author", kClient1},
            {"idSpace", std::uint64_t{1}},
            {"baseSeq", std::uint64_t{0}},
            {"lamport", std::int64_t{1}},
            {"vector", Payload::object()},
            {"ops", Payload::array()},
        };
    };

    {
        std::string err;
        const auto good = encodePayload(makeValidDoc());
        check(decodeBatch(good, err).has_value(), "baseline valid batch decodes");

        auto dFloatSpace = makeValidDoc();
        dFloatSpace["idSpace"] = 1.5;
        check(!decodeBatch(encodePayload(dFloatSpace), err).has_value(),
              "float idSpace refused (no silent narrowing)");

        auto dWideSpace = makeValidDoc();
        dWideSpace["idSpace"] = (std::uint64_t{1} << 32U) | std::uint64_t{1};
        check(!decodeBatch(encodePayload(dWideSpace), err).has_value(),
              "64-bit overflowing idSpace refused (no silent uint32 truncation)");

        auto dNegSpace = makeValidDoc();
        dNegSpace["idSpace"] = std::int64_t{-1};
        check(!decodeBatch(encodePayload(dNegSpace), err).has_value(),
              "negative idSpace refused");

        auto dBoolSpace = makeValidDoc();
        dBoolSpace["idSpace"] = true;
        check(!decodeBatch(encodePayload(dBoolSpace), err).has_value(),
              "boolean idSpace refused");

        auto dFloatSeq = makeValidDoc();
        dFloatSeq["seq"] = 1.5;
        check(!decodeBatch(encodePayload(dFloatSeq), err).has_value(),
              "float seq refused");

        auto dNegSeq = makeValidDoc();
        dNegSeq["seq"] = std::int64_t{-5};
        check(!decodeBatch(encodePayload(dNegSeq), err).has_value(),
              "negative seq refused");
    }
}

void testMalformedOpIdSpaceAndBadBatchNonWedging() {
    section("malformed op payload, remote id-space check, and bad batch does not wedge sync");

    test::TempDirectory temp{"collab_sync", "resilience"};
    const fs::path tpl = createBaselineTemplate(temp.path());
    auto s1 = openCopyFromTemplate(tpl, temp.path() / "r1.adi");
    auto s2 = openCopyFromTemplate(tpl, temp.path() / "r2.adi");

    CollabReplica r1(*s1, kClient1, 1U);
    CollabReplica r2(*s2, kClient2, 2U);
    FakeObjectStore bucket;

    // seq 1: valid device insert from r1.
    std::string err;
    check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "Valid1")}, err), "r1 commit Valid1");
    check(r1.syncStep(bucket).pushedOne, "r1 pushed seq 1");

    // seq 2: hostile deeply-nested corrupt CBOR directly injected into bucket.
    std::vector<std::byte> hostile(2000U, std::byte{0x81});
    hostile.push_back(std::byte{0x00});
    check(bucket.putIfAbsent(formatSeqKey(2U), hostile) == PutStatus::Created,
          "injected hostile CBOR at seq 2");

    // seq 3: decodable batch with malformed op payload (non-integer id & null chain).
    {
        Batch malformed;
        malformed.seq = 3U;
        malformed.batchId = std::string(kClient3) + "-1";
        malformed.authorClientId = kClient3;
        malformed.idSpace = 3U;
        malformed.baseSeq = 2U;
        malformed.lamport = 1;
        BatchOp badOp;
        badOp.opType = "device.insert";
        badOp.payload = {{"id", "not_an_integer"}, {"chain", nullptr}};
        badOp.lamport = 1;
        malformed.ops.push_back(std::move(badOp));
        check(bucket.putIfAbsent(formatSeqKey(3U), encodeBatch(malformed)) == PutStatus::Created,
              "injected malformed-payload batch at seq 3");
    }

    // seq 4: decodable batch claiming idSpace=3 but trying to create a device in idSpace=1!
    {
        Batch wrongSpace;
        wrongSpace.seq = 4U;
        wrongSpace.batchId = std::string(kClient3) + "-2";
        wrongSpace.authorClientId = kClient3;
        wrongSpace.idSpace = 3U;
        wrongSpace.baseSeq = 3U;
        wrongSpace.lamport = 2;
        BatchOp spoofed;
        spoofed.opType = "device.insert";
        spoofed.payload = {{"id", makeScopedId(1U, 777U)}, {"chain", kSharedChainId},
                           {"ord", 0}, {"ref", nullptr}, {"name", "Spoofed"},
                           {"enabled", true}, {"rack", false}};
        spoofed.lamport = 2;
        wrongSpace.ops.push_back(std::move(spoofed));
        check(bucket.putIfAbsent(formatSeqKey(4U), encodeBatch(wrongSpace)) == PutStatus::Created,
              "injected wrong-idSpace batch at seq 4");
    }

    // seq 5: valid batch from r2 after all three bad slots (proves sync never wedges!).
    {
        Batch good5;
        good5.seq = 5U;
        good5.batchId = std::string(kClient2) + "-5";
        good5.authorClientId = kClient2;
        good5.idSpace = 2U;
        good5.baseSeq = 4U;
        good5.lamport = 5;
        BatchOp op;
        op.opType = "device.insert";
        op.payload = {{"id", makeScopedId(2U, 50U)}, {"chain", kSharedChainId}, {"ord", 0},
                      {"ref", nullptr}, {"name", "ValidAfterBad"}, {"enabled", true},
                      {"rack", false}};
        op.lamport = 5;
        good5.ops.push_back(std::move(op));
        check(bucket.putIfAbsent(formatSeqKey(5U), encodeBatch(good5)) == PutStatus::Created,
              "injected valid batch at seq 5");
    }

    // Both replicas pull seq 1..5 cleanly without throwing and converge identically!
    const auto st1 = r1.syncStep(bucket);
    const auto st2 = r2.syncStep(bucket);
    check(st1.status == SyncStatus::Progressed && st2.status == SyncStatus::Progressed,
          "both replicas progressed past corrupt/malformed/spoofed batches");
    check(r1.appliedSeq() == 5U && r2.appliedSeq() == 5U,
          "both replicas advanced past all bad batches to seq 5");
    check(st1.skippedBadBatchCount == 1U && st2.skippedBadBatchCount == 1U,
          "hostile seq 2 skipped deterministically");
    check(st1.rejectedRemoteBatchCount == 2U && st2.rejectedRemoteBatchCount == 2U,
          "malformed seq 3 and wrong-idSpace seq 4 rejected deterministically");
    check(r1.deviceOrderInChain(kSharedChainId) == r2.deviceOrderInChain(kSharedChainId) &&
              r1.deviceOrderInChain(kSharedChainId).size() == 3U,
          "only SeedDev + Valid1 + ValidAfterBad exist (spoofed & malformed never inserted)");
    check(digestProject(*s1).text == digestProject(*s2).text,
          "replicas converged to identical digest after bad batches");
}

void testRewindVerifiesTxnIdsAndRestartPersistence() {
    section("verified rewind txnIds, restart order-key & batch-id persistence, RemoteCommitHook");

    test::TempDirectory temp{"collab_sync", "rewind_and_restart"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    // 1. Rewind refuses rather than blindly undoing an unrelated Store transaction.
    {
        auto s1 = openCopyFromTemplate(tpl, temp.path() / "rw1.adi");
        auto s2 = openCopyFromTemplate(tpl, temp.path() / "rw2.adi");
        CollabReplica r1(*s1, kClient1, 1U);
        CollabReplica r2(*s2, kClient2, 2U);
        FakeObjectStore bucket;

        std::string err;
        check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "Pending1")}, err), "r1 pending commit");
        // Commit an unrelated direct OpJournal edit on s1 outside CollabReplica.
        OpRequest direct;
        direct.opType = "track.rename";
        direct.payload = {{"id", kSharedTrackId}, {"name", "UnrelatedLocalEdit"}};
        check(OpJournal(*s1).commit(direct).ok, "unrelated direct edit committed on s1");

        // r2 pushes seq 1, forcing r1 to attempt rewind.
        check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "Peer1")}, err), "r2 commit");
        check(r2.syncStep(bucket).pushedOne, "r2 pushed seq 1");

        const auto st = r1.syncStep(bucket);
        check(st.status == SyncStatus::Error && st.error.find("txn mismatch") != std::string::npos,
              "rewind refused with txn mismatch error instead of blindly undoing");
        SQLite::Statement tq(s1->db(), "SELECT name FROM tracks WHERE id = ?");
        tq.bind(1, kSharedTrackId);
        check(tq.executeStep() && tq.getColumn(0).getString() == "UnrelatedLocalEdit",
              "unrelated local transaction was NOT undone");
    }

    // 2. Restart preserves persisted order keys & monotonic batch ids, and invokes RemoteCommitHook.
    {
        const fs::path p1 = temp.path() / "pers1.adi";
        const fs::path p2 = temp.path() / "pers2.adi";
        auto s1 = openCopyFromTemplate(tpl, p1);
        auto s2 = openCopyFromTemplate(tpl, p2);
        FakeObjectStore bucket;

        std::size_t remoteHookCalls = 0U;
        RemoteCommitHook hook = [&](Store& st, const Batch& b,
                                    const std::vector<OpRequest>& reqs) -> CommitResult {
            if (!b.authorClientId.empty()) {
                ++remoteHookCalls;
            }
            return OpJournal(st).commit(reqs);
        };

        std::vector<std::int64_t> orderBeforeRestart;
        std::string slot0KeyBeforeRestart;
        {
            CollabReplica r1(*s1, kClient1, 1U, hook);
            CollabReplica r2(*s2, kClient2, 2U);

            std::string err;
            check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "A1")}, err), "r1 A1");
            check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "B1")}, err), "r2 B1");
            for (int i = 0; i < 6; ++i) {
                (void)r1.syncStep(bucket);
                (void)r2.syncStep(bucket);
            }
            check(r1.appliedSeq() == 2U && r2.appliedSeq() == 2U, "converged at seq 2");
            check(remoteHookCalls == 1U, "RemoteCommitHook invoked for peer batch");
            orderBeforeRestart = r1.deviceOrderInChain(kSharedChainId);
            slot0KeyBeforeRestart = r1.keyForDeviceSlot(kSharedChainId, 1U);
            check(s1->close(), "closed s1 before simulated restart");
        }

        // Reopen s1 and attach a brand-new CollabReplica instance (simulated restart).
        StoreError openErr{};
        auto s1Reopened = Store::open(p1, openErr, false);
        check(s1Reopened != nullptr, "reopened s1 after restart");
        CollabReplica r1Restarted(*s1Reopened, kClient1, 1U);
        CollabReplica r2Continued(*s2, kClient2, 2U);

        check(r1Restarted.appliedSeq() == 2U, "restarted replica restored appliedSeq=2");
        check(r1Restarted.deviceOrderInChain(kSharedChainId) == orderBeforeRestart,
              "restarted replica preserved exact device order");
        check(r1Restarted.keyForDeviceSlot(kSharedChainId, 1U) == slot0KeyBeforeRestart,
              "restarted replica produces identical fractional slot key as before restart");

        // Commit on restarted replica: batchId must NOT repeat kClient1-1, and concurrent
        // inserts after restart must converge identically with peer r2.
        std::string err;
        check(r1Restarted.commitLocal({makeDeviceInsertOp(r1Restarted, 1U, "A2_after_restart")},
                                      err),
              "restarted r1 commits A2_after_restart");
        check(r2Continued.commitLocal({makeDeviceInsertOp(r2Continued, 1U, "B2_after_restart")},
                                      err),
              "r2 commits B2_after_restart");
        for (int i = 0; i < 6; ++i) {
            (void)r1Restarted.syncStep(bucket);
            (void)r2Continued.syncStep(bucket);
        }
        check(r1Restarted.appliedSeq() == 4U && r2Continued.appliedSeq() == 4U,
              "post-restart concurrent edits sequenced to seq 4 (no repeated batchId collision)");
        check(r1Restarted.deviceOrderInChain(kSharedChainId) ==
                  r2Continued.deviceOrderInChain(kSharedChainId),
              "restarted replica and peer agree on exact device order");
        check(digestProject(*s1Reopened).text == digestProject(*s2).text,
              "restarted replica and peer converge to identical digestProject");

        // Inspect seq 3/4 in bucket to verify restarted r1's batchId is kClient1-2 (not -1).
        bool sawCounter2 = false;
        for (std::uint64_t seq = 3U; seq <= 4U; ++seq) {
            const auto g = bucket.get(formatSeqKey(seq));
            std::string decErr;
            const auto b = decodeBatch(g.bytes, decErr);
            if (b.has_value() && b->authorClientId == kClient1 &&
                b->batchId == std::string(kClient1) + "-2") {
                sawCounter2 = true;
            }
        }
        check(sawCounter2, "restarted replica emitted strictly monotonic batchId (-2, never -1)");
    }
}

void testAtomicRewindBetweenPendingAndRebaseFromBase() {
    section("atomic rewind pre-check with [P1, X, P2] foreign txn and rebaseFromBase recovery");

    test::TempDirectory temp{"collab_sync", "atomic_rewind_rebase"};
    const fs::path tpl = createBaselineTemplate(temp.path());
    auto s1 = openCopyFromTemplate(tpl, temp.path() / "ar1.adi");
    auto s2 = openCopyFromTemplate(tpl, temp.path() / "ar2.adi");

    CollabReplica r1(*s1, kClient1, 1U);
    CollabReplica r2(*s2, kClient2, 2U);
    FakeObjectStore bucket;

    std::string err;
    const auto p1Op = makeDeviceInsertOp(r1, 0U, "PendingP1");
    const std::int64_t p1Id = p1Op.payload.at("id").get<std::int64_t>();
    check(r1.commitLocal({p1Op}, err), "r1 commits P1: " + err);

    // Commit a foreign direct OpJournal transaction X BETWEEN P1 and P2!
    OpRequest foreignX;
    foreignX.opType = "track.rename";
    foreignX.payload = {{"id", kSharedTrackId}, {"name", "ForeignBetweenP1AndP2"}};
    check(OpJournal(*s1).commit(foreignX).ok, "foreign txn X committed between P1 and P2");

    const auto p2Op = makeDeviceInsertOp(r1, 0U, "PendingP2");
    const std::int64_t p2Id = p2Op.payload.at("id").get<std::int64_t>();
    check(r1.commitLocal({p2Op}, err), "r1 commits P2 after X: " + err);
    check(r1.pendingCount() == 2U, "r1 has 2 pending batches [P1, P2]");

    // r2 pushes a remote batch at seq 1, forcing r1 to attempt rewind of [P1, P2].
    check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "RemotePeer")}, err), "r2 commits RemotePeer");
    check(r2.syncStep(bucket).pushedOne, "r2 pushed seq 1");

    const std::string digestBeforeSync = digestProject(*s1).text;
    const auto orderBeforeSync = r1.deviceOrderInChain(kSharedChainId);
    const auto st = r1.syncStep(bucket);
    check(st.status == SyncStatus::Error && st.error.find("txn mismatch") != std::string::npos,
          "syncStep refuses [P1, X, P2] with txn mismatch");

    // Atomic check: P2 MUST NOT have been undone (no half-rewound store, empty redo stack).
    check(digestProject(*s1).text == digestBeforeSync,
          "failed rewind left store digest 100% unchanged (P2 not half-rewound)");
    check(r1.deviceOrderInChain(kSharedChainId) == orderBeforeSync,
          "failed rewind left currentOrderKeys_ unchanged");
    check(!History(*s1).canRedo(), "failed rewind left redo stack empty (zero partial undos)");
    check(r1.pendingCount() == 2U, "both pending batches still queued");
    {
        SQLite::Statement dq(s1->db(), "SELECT COUNT(*) FROM devices WHERE id IN (?, ?)");
        dq.bind(1, p1Id);
        dq.bind(2, p2Id);
        check(dq.executeStep() && dq.getColumn(0).getInt64() == 2,
              "both P1 and P2 devices still exist in store after atomic refusal");
    }

    // Recovery: rebaseFromBase moves atomically back to baseHistorySeq_ (preserving X on
    // a system branch per ADR-0128) and re-applies [P1, P2] cleanly at the undo head.
    check(r1.rebaseFromBase(err), "rebaseFromBase succeeded: " + err);
    check(History(*s1).branches().size() >= 2U,
          "departed tip containing foreign txn X preserved on a history branch");
    check(r1.pendingCount() == 2U, "pending batches preserved across rebaseFromBase");

    // Now sync succeeds cleanly and both replicas converge identically!
    for (int i = 0; i < 8; ++i) {
        (void)r1.syncStep(bucket);
        (void)r2.syncStep(bucket);
    }
    check(r1.appliedSeq() == 3U && r2.appliedSeq() == 3U,
          "both replicas converged to seq 3 after rebaseFromBase");
    check(r1.pendingCount() == 0U && r2.pendingCount() == 0U, "nothing pending after recovery");
    check(r1.deviceOrderInChain(kSharedChainId) == r2.deviceOrderInChain(kSharedChainId),
          "device order matches identically after recovery");
    check(digestProject(*s1).text == digestProject(*s2).text,
          "full project digests match identically after recovery");
}

void testOversizeLocalBatchRefusedBeforeApplyAndPush() {
    section("commitLocal enforces DecodeLimits before applying or pushing");

    test::TempDirectory temp{"collab_sync", "oversize_local"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    // 1. Default limits: >65536 CBOR items (like a ~6000-note clip) and >4096 ops refused locally.
    {
        auto s1 = openCopyFromTemplate(tpl, temp.path() / "os1.adi");
        CollabReplica r1(*s1, kClient1, 1U);
        FakeObjectStore bucket;
        const std::string digestBefore = digestProject(*s1).text;

        BatchOp hugeItems;
        hugeItems.opType = "track.rename";
        Payload bigArr = Payload::array();
        for (int i = 0; i < 70000; ++i) {
            bigArr.push_back(i);
        }
        hugeItems.payload = {{"id", kSharedTrackId}, {"name", "TooManyItems"}, {"extra", bigArr}};
        std::string err;
        check(!r1.commitLocal({std::move(hugeItems)}, err),
              "batch exceeding default 65536 CBOR items refused locally");
        check(r1.pendingCount() == 0U, "refused huge-items batch never queued");
        check(digestProject(*s1).text == digestBefore,
              "refused huge-items batch never mutated store");
        check(r1.syncStep(bucket).status == SyncStatus::Idle,
              "nothing pushed to bucket after huge-items refusal");
        check(bucket.get(formatSeqKey(1U)).status == GetStatus::NotFound,
              "bucket seq 1 remains empty");

        std::vector<BatchOp> tooManyOps;
        tooManyOps.reserve(4097U);
        for (std::size_t i = 0U; i < 4097U; ++i) {
            BatchOp op;
            op.opType = "track.rename";
            op.payload = {{"id", kSharedTrackId}, {"name", "N"}};
            tooManyOps.push_back(std::move(op));
        }
        check(!r1.commitLocal(std::move(tooManyOps), err),
              "batch exceeding default 4096 ops refused locally");
        check(r1.pendingCount() == 0U, "refused >4096-ops batch never queued");
        check(bucket.get(formatSeqKey(1U)).status == GetStatus::NotFound,
              "bucket seq 1 still empty after >4096-ops refusal");
    }

    // 2. Tight custom byte limit refuses a valid op before touching store or bucket.
    {
        auto s1 = openCopyFromTemplate(tpl, temp.path() / "os2.adi");
        DecodeLimits tight;
        tight.maxBatchBytes = 128U;
        CollabReplica r1(*s1, kClient1, 1U, {}, tight);
        FakeObjectStore bucket;
        const std::string digestBefore = digestProject(*s1).text;

        std::string err;
        check(!r1.commitLocal({makeDeviceInsertOp(r1, 0U, "Exceeds128Bytes")}, err),
              "batch exceeding tight maxBatchBytes refused locally");
        check(r1.pendingCount() == 0U, "oversize batch not added to pending");
        check(digestProject(*s1).text == digestBefore, "store untouched after byte-limit refusal");
        check(r1.syncStep(bucket).status == SyncStatus::Idle, "oversize batch never pushed");
    }
}

void testRestartWithUnsyncedPendingBatchesConverges() {
    section("restart with unsynced pending batches preserves pending suffix & baseOrderKeys");

    test::TempDirectory temp{"collab_sync", "restart_pending"};
    const fs::path tpl = createBaselineTemplate(temp.path());
    const fs::path p1 = temp.path() / "rp1.adi";
    const fs::path p2 = temp.path() / "rp2.adi";

    auto s1 = openCopyFromTemplate(tpl, p1);
    auto s2 = openCopyFromTemplate(tpl, p2);
    FakeObjectStore bucket;

    {
        CollabReplica r1(*s1, kClient1, 1U);
        CollabReplica r2(*s2, kClient2, 2U);
        std::string err;

        // Establish shared base at seq 1.
        check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "BasePeer")}, err), "r2 BasePeer");
        for (int i = 0; i < 4; ++i) {
            (void)r1.syncStep(bucket);
            (void)r2.syncStep(bucket);
        }
        check(r1.appliedSeq() == 1U && r2.appliedSeq() == 1U, "both at base seq 1");

        // r1 goes offline and commits TWO unsynced pending batches (device + track + device).
        check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "OfflineDev1")}, err),
              "r1 offline P1: " + err);
        check(r1.commitLocal({makeTrackCreateOp(r1, 0U, "OfflineTrack"),
                              makeDeviceInsertOp(r1, 1U, "OfflineDev2")},
                             err),
              "r1 offline P2: " + err);
        check(r1.pendingCount() == 2U, "r1 has 2 unsynced pending batches before restart");

        // r2 concurrently commits and pushes seq 2 while r1 is offline.
        check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "PeerWhileOffline")}, err),
              "r2 PeerWhileOffline");
        check(r2.syncStep(bucket).pushedOne, "r2 pushed seq 2 while r1 offline");

        // Close s1 WITHOUT syncing r1's pending batches!
        check(s1->close(), "closed s1 with 2 pending batches still unsynced");
    }

    // Reopen s1 and attach a brand-new CollabReplica instance.
    StoreError openErr{};
    auto s1Reopened = Store::open(p1, openErr, false);
    check(s1Reopened != nullptr, "reopened s1 after offline restart");
    CollabReplica r1Restarted(*s1Reopened, kClient1, 1U);
    CollabReplica r2Continued(*s2, kClient2, 2U);

    check(r1Restarted.appliedSeq() == 1U, "restarted r1 kept appliedSeq=1");
    check(r1Restarted.pendingCount() == 2U,
          "restarted r1 restored both unsynced pending batches from session_state");

    // Sync to convergence: r1 must rewind P1/P2 cleanly, apply PeerWhileOffline without
    // ghost keys in baseOrderKeys_, rebase and push P1 & P2, and match r2 identically.
    for (int i = 0; i < 12; ++i) {
        (void)r1Restarted.syncStep(bucket);
        (void)r2Continued.syncStep(bucket);
    }

    check(r1Restarted.appliedSeq() == 4U && r2Continued.appliedSeq() == 4U,
          "restarted r1 and peer r2 both converged to seq 4");
    check(r1Restarted.pendingCount() == 0U && r2Continued.pendingCount() == 0U,
          "all restored pending batches sequenced and cleared");
    check(r1Restarted.deviceOrderInChain(kSharedChainId) ==
              r2Continued.deviceOrderInChain(kSharedChainId),
          "device order matches identically after offline-pending restart");
    check(r1Restarted.topLevelTrackOrder() == r2Continued.topLevelTrackOrder(),
          "top-level track order matches identically after offline-pending restart");
    check(digestProject(*s1Reopened).text == digestProject(*s2).text,
          "full project digest matches identically after offline-pending restart");
}

void testHostileBatchIdSpoofingRejectedAndCompositeDedup() {
    section("hostile batchId spoofing rejected and victim pending edit preserved");

    test::TempDirectory temp{"collab_sync", "batchid_spoof"};
    const fs::path tpl = createBaselineTemplate(temp.path());
    auto s1 = openCopyFromTemplate(tpl, temp.path() / "sp1.adi");
    auto s2 = openCopyFromTemplate(tpl, temp.path() / "sp2.adi");

    CollabReplica r1(*s1, kClient1, 1U);
    CollabReplica r2(*s2, kClient2, 2U);
    FakeObjectStore bucket;

    // Victim r1 queues a real pending batch (kClient1-1).
    std::string err;
    check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "VictimRealEdit")}, err),
          "victim r1 commits real pending edit");
    check(r1.pendingCount() == 1U, "victim r1 has 1 pending edit");

    // Hostile peer kClient2 injects seq 1 carrying victim's batchId (kClient1-1)!
    Payload spoofedDoc = {
        {"seq", std::uint64_t{1}},
        {"batchId", std::string(kClient1) + "-1"},
        {"author", kClient2},
        {"idSpace", std::uint64_t{2}},
        {"baseSeq", std::uint64_t{0}},
        {"lamport", std::int64_t{1}},
        {"vector", Payload::object()},
        {"ops", Payload::array()},
    };
    check(bucket.putIfAbsent(formatSeqKey(1U), encodePayload(spoofedDoc)) == PutStatus::Created,
          "hostile peer injected spoofed batchId at seq 1");

    // Victim pulls seq 1: spoofed batchId must be skipped as invalid, NOT drop victim's pending!
    const auto st1 = r1.syncStep(bucket);
    check(st1.status == SyncStatus::Progressed && st1.skippedBadBatchCount == 1U,
          "hostile spoofed batchId skipped at decode");
    check(r1.pendingCount() == 1U, "victim's pending batch was NOT silently dropped");

    // Victim pushes its real kClient1-1 at seq 2; peer r2 pulls seq 1..2 and converges!
    const auto st2 = r1.syncStep(bucket);
    check(st2.pushedOne && r1.appliedSeq() == 2U && r1.pendingCount() == 0U,
          "victim pushed real kClient1-1 at seq 2");
    const auto r2Step = r2.syncStep(bucket);
    check(r2Step.skippedBadBatchCount == 1U && r2Step.appliedRemoteCount == 1U &&
              r2.appliedSeq() == 2U,
          "peer skipped spoofed seq 1 and applied victim's real seq 2");
    check(digestProject(*s1).text == digestProject(*s2).text,
          "victim and peer converged identically after batchId spoof attempt");
}

void testLamportOverflowCapAndRejectedBatchClockIsolation() {
    section("Lamport overflow cap and rejected-batch clock isolation");

    test::TempDirectory temp{"collab_sync", "clock_cap"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    // 1. Hostile INT64_MAX batches skipped; rejected wrong-idSpace batch does not pollute clocks.
    {
        auto s1 = openCopyFromTemplate(tpl, temp.path() / "clk1.adi");
        CollabReplica r1(*s1, kClient1, 1U);
        FakeObjectStore bucket;

        const std::int64_t kMax64 = (std::numeric_limits<std::int64_t>::max)();
        Payload hugeLamport = {
            {"seq", std::uint64_t{1}},
            {"batchId", std::string(kClient2) + "-1"},
            {"author", kClient2},
            {"idSpace", std::uint64_t{2}},
            {"baseSeq", std::uint64_t{0}},
            {"lamport", kMax64},
            {"vector", Payload::object()},
            {"ops", Payload::array()},
        };
        check(bucket.putIfAbsent(formatSeqKey(1U), encodePayload(hugeLamport)) == PutStatus::Created,
              "injected INT64_MAX lamport at seq 1");

        Payload hugeVec = {
            {"seq", std::uint64_t{2}},
            {"batchId", std::string(kClient2) + "-2"},
            {"author", kClient2},
            {"idSpace", std::uint64_t{2}},
            {"baseSeq", std::uint64_t{1}},
            {"lamport", std::int64_t{1}},
            {"vector", Payload{{kClient3, kMax64}}},
            {"ops", Payload::array()},
        };
        check(bucket.putIfAbsent(formatSeqKey(2U), encodePayload(hugeVec)) == PutStatus::Created,
              "injected INT64_MAX vector entry at seq 2");

        // seq 3: valid-decoding batch with high lamport=500000, but rejected for wrong idSpace.
        Batch rejectedHighClock;
        rejectedHighClock.seq = 3U;
        rejectedHighClock.batchId = std::string(kClient3) + "-1";
        rejectedHighClock.authorClientId = kClient3;
        rejectedHighClock.idSpace = 3U;
        rejectedHighClock.baseSeq = 2U;
        rejectedHighClock.lamport = 500000;
        rejectedHighClock.vector.observe(kClient3, 500000);
        BatchOp badSpaceOp;
        badSpaceOp.opType = "device.insert";
        badSpaceOp.payload = {{"id", makeScopedId(1U, 999U)}, {"chain", kSharedChainId},
                              {"ord", 0}, {"ref", nullptr}, {"name", "BadSpace"},
                              {"enabled", true}, {"rack", false}};
        badSpaceOp.lamport = 500000;
        rejectedHighClock.ops.push_back(std::move(badSpaceOp));
        check(bucket.putIfAbsent(formatSeqKey(3U), encodeBatch(rejectedHighClock)) ==
                  PutStatus::Created,
              "injected rejected wrong-idSpace batch with lamport=500000 at seq 3");

        const auto st = r1.syncStep(bucket);
        check(st.status == SyncStatus::Progressed && st.skippedBadBatchCount == 2U &&
                  st.rejectedRemoteBatchCount == 1U && r1.appliedSeq() == 3U,
              "hostile INT64_MAX slots skipped and wrong-idSpace slot rejected");
        check(r1.versionVector().clockFor(kClient2) == 0 &&
                  r1.versionVector().clockFor(kClient3) == 0,
              "clocks of skipped and rejected batches did NOT pollute version vector");

        // Subsequent local commit still works cleanly with low clock (no overflow!).
        std::string err;
        check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "CleanAfterHostileClocks")}, err),
              "local commit after hostile clocks succeeds cleanly without overflow");
    }

    // 2. Replica at maxClock refuses further local commits cleanly without signed overflow.
    {
        auto s1 = openCopyFromTemplate(tpl, temp.path() / "clk2.adi");
        DecodeLimits lowCap;
        lowCap.maxClock = 2;
        CollabReplica r1(*s1, kClient1, 1U, {}, lowCap);

        std::string err;
        check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "Op1"),
                              makeDeviceInsertOp(r1, 1U, "Op2")},
                             err),
              "commit reaching exact maxClock=2 succeeds");
        const std::string digestAtCap = digestProject(*s1).text;
        check(!r1.commitLocal({makeDeviceInsertOp(r1, 0U, "Op3OverCap")}, err),
              "commit exceeding maxClock refused cleanly before incrementing");
        check(digestProject(*s1).text == digestAtCap,
              "refused over-cap commit left store untouched");
    }
}

void testCrashBetweenJournalCommitAndSessionStateIsIdempotent() {
    section("crash between OpJournal commit / putIfAbsent and session_state write is idempotent");

    test::TempDirectory temp{"collab_sync", "crash_idempotent"};
    const fs::path tpl = createBaselineTemplate(temp.path());

    auto readSessionState = [](Store& st, const std::string& clientId) -> std::string {
        SQLite::Statement q(st.db(), "SELECT value FROM session_state WHERE key = ?");
        q.bind(1, "collab.replica_state." + clientId);
        if (q.executeStep() && !q.getColumn(0).isNull()) {
            return q.getColumn(0).getString();
        }
        return {};
    };
    auto writeSessionState = [](Store& st, const std::string& clientId, const std::string& val) {
        SQLite::Statement up(st.db(),
                             "INSERT INTO session_state(key, value) VALUES(?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        up.bind(1, "collab.replica_state." + clientId);
        up.bind(2, val);
        up.exec();
    };

    // Case A: Crash after remote OpJournal::commit and BEFORE savePersistedState().
    {
        const fs::path p1 = temp.path() / "cr1.adi";
        const fs::path p2 = temp.path() / "cr2.adi";
        auto s1 = openCopyFromTemplate(tpl, p1);
        auto s2 = openCopyFromTemplate(tpl, p2);
        FakeObjectStore bucket;

        std::string preSyncState;
        {
            CollabReplica r1(*s1, kClient1, 1U);
            CollabReplica r2(*s2, kClient2, 2U);
            preSyncState = readSessionState(*s1, kClient1);

            std::string err;
            check(r2.commitLocal({makeDeviceInsertOp(r2, 0U, "PeerCrashDev")}, err),
                  "r2 commits PeerCrashDev");
            check(r2.syncStep(bucket).pushedOne, "r2 pushed seq 1");

            const auto st = r1.syncStep(bucket);
            check(st.appliedRemoteCount == 1U && r1.appliedSeq() == 1U,
                  "r1 applied seq 1 normally before simulated crash");

            // Simulate crash between OpJournal::commit and savePersistedState():
            // revert session_state back to preSyncState while SQLite ops/devices keep seq 1!
            writeSessionState(*s1, kClient1, preSyncState);
            check(s1->close(), "closed s1 after simulated crash");
        }

        StoreError openErr{};
        auto s1Recovered = Store::open(p1, openErr, false);
        check(s1Recovered != nullptr, "reopened s1 after simulated crash");
        CollabReplica r1Recovered(*s1Recovered, kClient1, 1U);
        CollabReplica r2Check(*s2, kClient2, 2U);

        // Next syncStep must recognize seq 1 from ops.actor_detail, NOT re-apply or error!
        const auto recStep = r1Recovered.syncStep(bucket);
        check(recStep.status == SyncStatus::Progressed && recStep.appliedRemoteCount == 0U &&
                  recStep.rejectedRemoteBatchCount == 0U && r1Recovered.appliedSeq() == 1U,
              "recovered replica replayed seq 1 idempotently (0 duplicate apply, 0 reject)");
        check(r1Recovered.deviceOrderInChain(kSharedChainId) ==
                  r2Check.deviceOrderInChain(kSharedChainId),
              "recovered replica restored exact fractional device order");
        check(digestProject(*s1Recovered).text == digestProject(*s2).text,
              "recovered replica digest matches peer identically");
    }

    // Case B: Crash after bucket.putIfAbsent succeeded and BEFORE savePersistedState() at step 3.
    {
        const fs::path p1 = temp.path() / "cr3.adi";
        const fs::path p2 = temp.path() / "cr4.adi";
        auto s1 = openCopyFromTemplate(tpl, p1);
        auto s2 = openCopyFromTemplate(tpl, p2);
        FakeObjectStore bucket;

        {
            CollabReplica r1(*s1, kClient1, 1U);
            std::string err;
            check(r1.commitLocal({makeDeviceInsertOp(r1, 0U, "PushCrashDev")}, err),
                  "r1 commits PushCrashDev");
            const std::string prePushState = readSessionState(*s1, kClient1);

            check(r1.syncStep(bucket).pushedOne, "r1 putIfAbsent succeeded for seq 1");
            // Simulate crash right after putIfAbsent and before savePersistedState():
            writeSessionState(*s1, kClient1, prePushState);
            check(s1->close(), "closed s1 after simulated post-putIfAbsent crash");
        }

        StoreError openErr{};
        auto s1Recovered = Store::open(p1, openErr, false);
        check(s1Recovered != nullptr, "reopened s1 after post-putIfAbsent crash");
        CollabReplica r1Recovered(*s1Recovered, kClient1, 1U);
        CollabReplica r2(*s2, kClient2, 2U);

        const auto st1 = r1Recovered.syncStep(bucket);
        const auto st2 = r2.syncStep(bucket);
        check(st1.status == SyncStatus::Progressed && r1Recovered.appliedSeq() == 1U &&
                  r1Recovered.pendingCount() == 0U,
              "recovered pusher cleanly reconciled seq 1 with 0 pending");
        check(st2.appliedRemoteCount == 1U && r2.appliedSeq() == 1U, "peer applied seq 1");
        check(digestProject(*s1Recovered).text == digestProject(*s2).text,
              "recovered pusher and peer converge to identical digest");
    }
}

}  // namespace

int main() {
    testConcurrentDeviceInsertAtOneSlot();
    testThreeStores200SeedsWithDelaysAndDrops();
    testConcurrentThreeThreadsTsan();
    testHostileCborAndStrictNumbers();
    testMalformedOpIdSpaceAndBadBatchNonWedging();
    testRewindVerifiesTxnIdsAndRestartPersistence();
    testAtomicRewindBetweenPendingAndRebaseFromBase();
    testOversizeLocalBatchRefusedBeforeApplyAndPush();
    testRestartWithUnsyncedPendingBatchesConverges();
    testHostileBatchIdSpoofingRejectedAndCompositeDedup();
    testLamportOverflowCapAndRejectedBatchClockIsolation();
    testCrashBetweenJournalCommitAndSessionStateIsIdempotent();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
