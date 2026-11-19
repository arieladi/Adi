// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
namespace SQLite {class Database;}
namespace adi::edit {
/// The renderer's step tempo map, with checked integer conversions for edits.
/// Construct on the Store owner thread, under the edit's read transaction.
class TimeMap {
public:
    explicit TimeMap(SQLite::Database&);
    [[nodiscard]] double seconds(std::int64_t ticks)const;
    [[nodiscard]] double durationSeconds(std::int64_t origin,std::int64_t ticks)const;
    [[nodiscard]] std::int64_t nanos(std::int64_t ticks)const;
    [[nodiscard]] std::int64_t ticks(std::int64_t nanos)const;
    [[nodiscard]] std::int64_t duration(std::int64_t origin,std::int64_t ticks)const;
    [[nodiscard]] double bpmBefore(std::int64_t ticks)const;
private:
    engine::TempoMap tempo_;
    double fastest_=120;
};
[[nodiscard]] std::int64_t addTime(std::int64_t,std::int64_t);
[[nodiscard]] std::int64_t nanosFromSeconds(double);
} // namespace adi::edit
