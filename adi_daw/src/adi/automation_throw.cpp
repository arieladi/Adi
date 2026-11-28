// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_throw.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/automation.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace adi::automation {
namespace {
std::optional<std::int64_t> sampleAt(const edit::TimeMap &time, std::int64_t base, std::int64_t native,
                                     double rate) {
    if (native < 0) return std::nullopt;
    const double sample = (base == 0 ? time.seconds(native) : static_cast<double>(native) * 1e-9) * rate;
    if (!std::isfinite(sample) || sample < 0 || sample > 0x1p50) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(sample));
}
} // namespace

std::optional<double> laneValueAt(Store &store, std::int64_t laneId, std::int64_t native, double rate,
                                  std::string &error) {
    try {
        auto &db = store.db();
        SQLite::Statement lane(db, "SELECT default_value,time_base,owner_kind FROM automation_lanes WHERE id=?");
        lane.bind(1, laneId);
        if (!lane.executeStep()) {
            error = "automation lane is missing";
            return std::nullopt;
        }
        const double fallback = lane.getColumn(0).getDouble();
        const auto base = lane.getColumn(1).getInt64();
        if (lane.getColumn(2).getString() == "clip") {
            error = "a Throw needs a global automation lane";
            return std::nullopt;
        }
        if (base != 0 && base != 1) {
            error = "automation lane clock is unknown";
            return std::nullopt;
        }
        const edit::TimeMap time(db);
        const auto at = sampleAt(time, base, native, rate);
        if (!at) {
            error = "the Throw's position is outside the lane's clock";
            return std::nullopt;
        }
        SQLite::Statement data(db, "SELECT id,data FROM automation_data WHERE lane_id=? AND clip_id IS NULL");
        data.bind(1, laneId);
        if (!data.executeStep()) return fallback;   // no points: the lane plays its default
        const auto dataId = data.getColumn(0).getInt64();
        const auto blob = data.getColumn(1);
        StreamReader<AutomationPoint> reader(
            {static_cast<const std::byte *>(blob.getBlob()), static_cast<std::size_t>(blob.getBytes())},
            FourCC::Automation);
        if (!reader.ok()) {
            error = "automation stream is unreadable";
            return std::nullopt;
        }
        std::map<std::size_t, engine::CurveSlice> slices;
        SQLite::Statement ranges(db, "SELECT record_index,progress_begin,progress_end,source_first,source_last "
                                     "FROM automation_segment_ranges WHERE data_id=? AND clip_id IS NULL");
        ranges.bind(1, dataId);
        while (ranges.executeStep()) {
            const auto index = ranges.getColumn(0).getInt64();
            if (index < 0) continue;
            slices[static_cast<std::size_t>(index)] =
                engine::CurveSlice{ranges.getColumn(1).getDouble(), ranges.getColumn(2).getDouble(),
                                   ranges.getColumn(3).getDouble(), ranges.getColumn(4).getDouble()};
        }
        engine::AutomationLaneProgram program;
        program.defaultValue = fallback;
        program.points.reserve(reader.count());
        for (std::uint32_t i = 0; i < reader.count(); ++i) {
            const auto point = *reader.at(i);
            const auto sample = sampleAt(time, base, point.time, rate);
            if (!sample) {
                error = "automation point is outside the lane's clock";
                return std::nullopt;
            }
            const auto slice = slices.find(i);
            program.points.push_back({*sample, point.value, point.tension, point.curve,
                                      slice == slices.end() ? nullptr : &slice->second});
        }
        return engine::valueAt(program, *at);
    } catch (const std::exception &e) {
        error = e.what();
        return std::nullopt;
    }
}

PreparedEnvelopeSplice prepareThrow(Store &store, std::int64_t laneId, const ThrowSpec &spec, double rate,
                                    std::string_view label) {
    PreparedEnvelopeSplice result;
    if (spec.begin < 0 || spec.end <= spec.begin) {
        result.error = "a Throw needs a time selection";
        return result;
    }
    if (spec.rise < 0 || spec.rise > spec.end - spec.begin) {
        result.error = "a Throw's rise must fit in its selection";
        return result;
    }
    if (spec.release < 1 || spec.release > std::numeric_limits<std::int64_t>::max() - spec.end) {
        result.error = "a Throw needs a release of at least one unit";
        return result;
    }
    if (!std::isfinite(spec.open)) {
        result.error = "a Throw needs a finite open value";
        return result;
    }
    const auto closedAt = spec.end + spec.release;
    const auto before = laneValueAt(store, laneId, spec.begin, rate, result.error);
    const auto after = before ? laneValueAt(store, laneId, closedAt, rate, result.error) : std::nullopt;
    if (!before || !after) return result;
    const auto knot = [](std::int64_t time, double value) {
        NativeSpliceKnot k;
        k.point.time = time;
        k.point.value = value;
        k.point.curve = 1;   // linear, as a drawn ramp
        return k;
    };
    std::vector<NativeSpliceKnot> shape;
    if (spec.rise > 0) shape.push_back(knot(spec.begin, *before));
    shape.push_back(knot(spec.begin + spec.rise, spec.open));
    if (spec.begin + spec.rise < spec.end) shape.push_back(knot(spec.end, spec.open));
    shape.push_back(knot(closedAt, *after));
    return prepareEnvelopeSplice(store, laneId, spec.begin, closedAt, shape, rate, kSplicePointBudget, label);
}
} // namespace adi::automation
