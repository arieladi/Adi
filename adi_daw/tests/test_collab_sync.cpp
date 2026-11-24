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
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
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
            {"batchId", "b1"},
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
        malformed.batchId = std::string(kClient3) + "-bad-payload";
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
        wrongSpace.batchId = std::string(kClient3) + "-wrong-space";
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
        good5.batchId = std::string(kClient2) + "-good5";
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

}  // namespace

int main() {
    testConcurrentDeviceInsertAtOneSlot();
    testThreeStores200SeedsWithDelaysAndDrops();
    testConcurrentThreeThreadsTsan();
    testHostileCborAndStrictNumbers();
    testMalformedOpIdSpaceAndBadBatchNonWedging();
    testRewindVerifiesTxnIdsAndRestartPersistence();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
