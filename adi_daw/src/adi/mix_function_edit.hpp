// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/mix_functions.hpp"
#include "adi/ops.hpp"
namespace adi::mix {
struct PreparedFunctionEdit {
    bool ok = false;
    std::string error, sourceFile;
    std::int64_t deviceId = 0, chainId = 0, referenceId = 0;
    std::vector<OpRequest> requests;
    CommitOptions options;
};
// Adds one function at the root chain's function tail. EQ precedes that tail, Limiter follows
// it, and newly added middle functions preserve addition order before Limiter. Existing manual
// positions are never moved to impose a default. Availability is supplied by the actual factory.
[[nodiscard]] PreparedFunctionEdit prepareFunctionInsert(Store &, Function, std::int64_t track,
                                                        bool engineAvailable);
// Moves an existing native function inside the same owning channel, including among ordinary
// devices. An absent before target appends to the root chain. Identity/state are not recreated.
[[nodiscard]] PreparedFunctionEdit prepareFunctionMove(Store &, std::int64_t device,
                                                      std::int64_t track,
                                                      std::optional<std::int64_t> before = {});
[[nodiscard]] CommitResult commitFunctionEdit(Store &, const PreparedFunctionEdit &, Actor = Actor::User);
} // namespace adi::mix
