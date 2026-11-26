// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: unit tests for fractional sibling keys (Rocicorp / Figcraft vectors,
// LIS key reuse, respace), id-space allocator, version vectors, and batch CBOR.

#include "adi/collab/batch.hpp"
#include "adi/collab/fractional_key.hpp"

#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace adi;
using namespace adi::collab;

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

void testFractionalVectors() {
    section("fractional key vectors (Rocicorp / Figcraft)");

    auto kb = [](std::optional<std::string_view> a, std::optional<std::string_view> b) {
        return keyBetween(a, b);
    };

    check(kb(std::nullopt, std::nullopt) == std::optional<std::string>("a0"),
          "(null, null) == a0");
    check(kb(std::nullopt, "a0") == std::optional<std::string>("Zz"), "(null, a0) == Zz");
    check(kb("a0", std::nullopt) == std::optional<std::string>("a1"), "(a0, null) == a1");
    check(kb("a0", "a1") == std::optional<std::string>("a0V"), "(a0, a1) == a0V");
    check(kb("a0", "a0V") == std::optional<std::string>("a0G"), "(a0, a0V) == a0G");
    check(kb("Zz", "a0") == std::optional<std::string>("ZzV"), "(Zz, a0) == ZzV");
    check(kb("az", std::nullopt) == std::optional<std::string>("b00"), "(az, null) == b00");
    check(kb(std::nullopt, "b00") == std::optional<std::string>("az"), "(null, b00) == az");
    check(kb("bzz", std::nullopt) == std::optional<std::string>("c000"), "(bzz, null) == c000");
    check(kb(std::nullopt, "Z0") == std::optional<std::string>("Yzz"), "(null, Z0) == Yzz");
    check(kb("Yzz", std::nullopt) == std::optional<std::string>("Z0"), "(Yzz, null) == Z0");

    // Swapped and equal bounds MUST be refused (PLAN C3).
    check(!kb("a1", "a0").has_value(), "swapped bounds (a1, a0) refused");
    check(!kb("a0", "a0").has_value(), "equal bounds (a0, a0) refused");
    check(!kb("a0V", "a0G").has_value(), "swapped fractional bounds refused");

    // Invalid keys refused.
    check(!validateFractionalKey(""), "empty key invalid");
    check(!validateFractionalKey("a00"), "trailing zero in fractional part invalid");
    check(!validateFractionalKey("0a"), "digit head invalid");
    check(!validateFractionalKey("A00000000000000000000000000"), "smallest integer invalid");
    check(!kb("a0", "a00").has_value(), "invalid upper key refused");
    check(!kb(std::nullopt, "A00000000000000000000000000").has_value(),
          "below smallest integer refused");

    // 500 consecutive midpoint subdivisions between a0 and a1 stay valid & strictly ordered.
    std::string lo = "a0";
    std::string hi = "a1";
    for (int i = 0; i < 500; ++i) {
        const auto mid = keyBetween(lo, hi);
        check(mid.has_value() && validateFractionalKey(*mid) && lo < *mid && *mid < hi,
              "midpoint step " + std::to_string(i));
        if (!mid.has_value()) {
            break;
        }
        if ((i % 2) == 0) {
            lo = *mid;
        } else {
            hi = *mid;
        }
    }

    // Balanced N-key generation & respace.
    const auto batch = nKeysBetween("a0", "a1", 25U);
    check(batch.has_value() && batch->size() == 25U, "nKeysBetween generated 25 keys");
    if (batch.has_value()) {
        for (std::size_t i = 0U; i < batch->size(); ++i) {
            check(validateFractionalKey((*batch)[i]), "batch key valid");
            check("a0" < (*batch)[i] && (*batch)[i] < "a1", "batch key within bounds");
            if (i > 0U) {
                check((*batch)[i - 1U] < (*batch)[i], "batch keys strictly ascending");
            }
        }
    }

    const auto respaced = respaceKeys(16U);
    check(respaced.size() == 16U, "respaceKeys size 16");
    for (std::size_t i = 1U; i < respaced.size(); ++i) {
        check(respaced[i - 1U] < respaced[i], "respaced strictly ascending");
    }
}

void testLisKeyReuse() {
    section("longest increasing subsequence key reuse");

    // Desired order has items originally at [a0, a2, a3] untouched and one moved item [a5]
    // placed at front plus one new item "" in the middle.
    const std::vector<std::string> current = {"a5", "a0", "", "a2", "a3"};
    const auto reused = reuseKeysForOrder(current);
    check(reused.size() == current.size(), "reused size matches");
    // LIS is [a0, a2, a3] (length 3): all three MUST keep their exact original keys!
    check(reused[1] == "a0", "LIS element a0 preserved");
    check(reused[3] == "a2", "LIS element a2 preserved");
    check(reused[4] == "a3", "LIS element a3 preserved");
    for (std::size_t i = 0U; i < reused.size(); ++i) {
        check(validateFractionalKey(reused[i]), "reused key valid");
        if (i > 0U) {
            check(reused[i - 1U] < reused[i], "reused keys strictly ascending");
        }
    }
}

void testIdSpaceVersionVectorAndBatchCbor() {
    section("id-space allocator, version vector, and batch CBOR");

    // Distinct id spaces never collide.
    IdSpaceAllocator a1(1U);
    IdSpaceAllocator a2(2U);
    std::set<std::int64_t> seen;
    for (int i = 0; i < 20; ++i) {
        const auto id1 = a1.next("devices");
        const auto id2 = a2.next("devices");
        check(id1 > 0 && idSpaceOf(id1) == 1U, "space 1 id valid");
        check(id2 > 0 && idSpaceOf(id2) == 2U, "space 2 id valid");
        check(seen.insert(id1).second && seen.insert(id2).second, "disjoint id spaces");
    }

    // 20-digit zero-padded keys sort lexically.
    const std::string clientA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string clientB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    check(formatSeqKey(9U) < formatSeqKey(10U), "seq keys sort lexically (9 < 10)");
    check(parseSeqKey(formatSeqKey(42U)) == std::optional<std::uint64_t>(42U),
          "parseSeqKey round-trip");
    check(formatLamportBatchKey(9, clientA) < formatLamportBatchKey(10, clientA),
          "ADR-0188 d5 batch keys sort lexically");

    // VersionVector dominance and concurrency.
    VersionVector v1;
    v1.observe(clientA, 3);
    v1.observe(clientB, 1);
    VersionVector v2;
    v2.observe(clientA, 2);
    v2.observe(clientB, 4);
    check(v1.concurrentWith(v2), "v1 and v2 are concurrent");
    VersionVector merged = v1;
    merged.merge(v2);
    check(merged.dominates(v1) && merged.dominates(v2), "merged dominates both");
    check(!merged.concurrentWith(v1), "merged not concurrent with v1");

    // Batch CBOR round-trip.
    Batch b;
    b.seq = 7U;
    b.batchId = clientA + "-1";
    b.authorClientId = clientA;
    b.idSpace = 1U;
    b.baseSeq = 6U;
    b.lamport = 11;
    b.vector = merged;
    BatchOp op;
    op.opType = "device.insert";
    op.payload = {{"id", makeScopedId(1U, 1U)}, {"chain", 10}, {"ord", 0}, {"name", "EQ"}};
    op.lamport = 11;
    op.orderKey = "a0V";
    b.ops.push_back(op);

    const auto encoded = encodeBatch(b);
    std::string err;
    const auto decoded = decodeBatch(encoded, err);
    check(decoded.has_value() && *decoded == b, "batch CBOR round-trip exact: " + err);

    // Reject a batch carrying an inverse field (PLAN CO-01: never inverses).
    Payload bad = {
        {"seq", 1}, {"batchId", clientA + "-1"}, {"author", clientA}, {"idSpace", 1}, {"baseSeq", 0},
        {"lamport", 1}, {"vector", Payload::object()},
        {"ops", Payload::array({Payload{{"opType", "track.rename"},
                                        {"payload", Payload::object()},
                                        {"lamport", 1},
                                        {"inverse", Payload::object()}}})},
    };
    const auto badRes = decodeBatch(encodePayload(bad), err);
    check(!badRes.has_value(), "batch carrying inverse refused");

    // Author-bound batchId validation.
    check(isValidBatchIdForAuthor(clientA + "-1", clientA), "valid author-1 batchId accepted");
    check(isValidBatchIdForAuthor(clientA + "-42", clientA), "valid author-42 batchId accepted");
    check(!isValidBatchIdForAuthor(clientB + "-1", clientA),
          "batchId carrying another author's clientId rejected");
    check(!isValidBatchIdForAuthor(clientA + "-0", clientA), "zero counter batchId rejected");
    check(!isValidBatchIdForAuthor(clientA + "-01", clientA), "leading-zero counter rejected");
    check(!isValidBatchIdForAuthor(clientA + "-abc", clientA), "non-decimal counter rejected");

    // Hostile INT64_MAX clock values refused at decode.
    const std::int64_t kHugeClock = (std::numeric_limits<std::int64_t>::max)();
    Payload badLamport = {
        {"seq", 1}, {"batchId", clientA + "-1"}, {"author", clientA}, {"idSpace", 1},
        {"baseSeq", 0}, {"lamport", kHugeClock}, {"vector", Payload::object()},
        {"ops", Payload::array()},
    };
    check(!decodeBatch(encodePayload(badLamport), err).has_value(),
          "INT64_MAX batch lamport refused at decode");

    Payload badVec = {
        {"seq", 1}, {"batchId", clientA + "-1"}, {"author", clientA}, {"idSpace", 1},
        {"baseSeq", 0}, {"lamport", 1}, {"vector", Payload{{clientB, kHugeClock}}},
        {"ops", Payload::array()},
    };
    check(!decodeBatch(encodePayload(badVec), err).has_value(),
          "INT64_MAX vector clock entry refused at decode");

    Payload badOpClock = {
        {"seq", 1}, {"batchId", clientA + "-1"}, {"author", clientA}, {"idSpace", 1},
        {"baseSeq", 0}, {"lamport", 1}, {"vector", Payload::object()},
        {"ops", Payload::array({Payload{{"opType", "track.rename"},
                                        {"payload", Payload::object()},
                                        {"lamport", kHugeClock}}})},
    };
    check(!decodeBatch(encodePayload(badOpClock), err).has_value(),
          "INT64_MAX op lamport refused at decode");
}

}  // namespace

int main() {
    testFractionalVectors();
    testLisKeyReuse();
    testIdSpaceVersionVectorAndBatchCbor();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
