// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/op_transaction_resources.hpp"
#include "adi/applied_batches.hpp"

namespace adi {
struct CapturedTransactionAccess {
    static std::shared_ptr<const OpCapturedTransaction> seal(AppliedSource,
        std::optional<std::int64_t>, std::vector<OpRequest>, OpTransactionInputs);
    static bool begin(const std::shared_ptr<const OpCapturedTransaction>&,
        std::span<const OpRequest>, const Store&, const CommitOptions&, std::string&);
    static void refuse(const std::shared_ptr<const OpCapturedTransaction>&,
                       const std::string&);
    static void durable(const std::shared_ptr<const OpCapturedTransaction>&,
                        std::int64_t) noexcept;
    static void supersede(const std::shared_ptr<const OpCapturedTransaction>&) noexcept;
    static void cancel(const std::shared_ptr<const OpCapturedTransaction>&) noexcept;
};
namespace detail {
bool sameSqlValue(const Payload&, const Payload&) noexcept;
std::vector<Payload> captureGuardRows(const Store&, const std::string& table,
                                    const Payload& equalColumns,
                                    std::size_t limit = 1000000);
}
} // namespace adi
