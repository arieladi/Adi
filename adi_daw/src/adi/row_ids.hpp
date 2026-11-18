// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
namespace SQLite {
class Database;
}
namespace adi {
/// Existing controller-lane creation alone permits a positive gap when the
/// signed row-id ceiling is occupied. Other producers refuse exhaustion.
enum class RowIdExhaustion { Refuse, FirstPositiveGap };
/// One owner-side allocation context for a captured edit. Hold one database
/// snapshot for its lifetime. Methods read schema/IDs but never write or reserve
/// in SQL; captured before-images/absence and the journal head guard publication.
class RowIds {
  public:
    explicit RowIds(SQLite::Database &db) : db_(db) {}
    RowIds(const RowIds &) = delete;
    RowIds &operator=(const RowIds &) = delete;
    [[nodiscard]] std::int64_t next(std::string_view table,
                                    RowIdExhaustion = RowIdExhaustion::Refuse);
    void reserve(std::string_view table, std::int64_t);

  private:
    struct State {
        std::int64_t high = 0;
        std::set<std::int64_t> reserved;
    };
    State &state(std::string_view);
    SQLite::Database &db_;
    std::map<std::string, State, std::less<>> states_;
};
} // namespace adi
