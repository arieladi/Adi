// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/scale_context.hpp"
namespace adi::engine {
struct ScaleKeyChange {std::int64_t id=0,posTicks=0;ClipScale scale;};
struct ScaleTimelineEvent {std::int64_t keyId=0,posTicks=0;ScaleContext context;};
struct ScaleTimelineData;
class ScaleTimeline {
public:
    // Queries borrow immutable generation data. Retain and retire this owner
    // outside the callback; no query allocates, locks, accesses SQL or retunes.
    [[nodiscard]] const ScaleContext* atTicks(std::int64_t ticks) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> nextChangeAfterTicks(std::int64_t ticks) const noexcept;
    [[nodiscard]] std::span<const ScaleTimelineEvent> events() const noexcept;
private:
    explicit ScaleTimeline(std::shared_ptr<const ScaleTimelineData> data):data_(std::move(data)){}
    std::shared_ptr<const ScaleTimelineData> data_;
    friend std::optional<ScaleTimeline> compileScaleTimeline(std::span<const ScaleKeyChange>,std::span<const TuningDefinition>,std::string&);
};
// Owner compilation: ordered unique nonnegative tick positions and positive
// unique IDs. Unreferenced definitions are not compiled; used ones must exist.
// C major/12-TET applies before the first key, including negative pre-roll.
std::optional<ScaleTimeline> compileScaleTimeline(std::span<const ScaleKeyChange>,std::span<const TuningDefinition>,std::string& error);
struct ScaleTimelineRead {std::optional<ScaleTimeline> timeline;std::string error;};
// One coherent owner snapshot of the five scale tables, with no note reads.
// Clip-local scales remain separate explicit overrides of this global map.
ScaleTimelineRead readScaleTimeline(const Store&);
} // namespace adi::engine
