// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/midi_controllers.hpp"
#include "adi/engine/curves.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
namespace adi::engine {
std::uint32_t allocateControllerSourcePrefix() {
    static std::atomic<std::uint64_t> next{1};
    const auto value = next.fetch_add(1, std::memory_order_relaxed);
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("controller source identity exhausted");
    return static_cast<std::uint32_t>(value);
}
namespace {
constexpr std::int64_t tickLimit = std::int64_t{1} << 50;
constexpr double sampleLimit = static_cast<double>(std::int64_t{1} << 50);
constexpr std::size_t pointBudget = 1000000, spanBudget = 1000000;
constexpr double ppq = 5765760.0;
std::int64_t roundedSample(double sample) {
    if (!std::isfinite(sample) || sample < 0 || sample > sampleLimit)
        throw std::runtime_error("controller time outside supported sample range");
    return static_cast<std::int64_t>(
        std::llround(std::nextafter(sample, std::numeric_limits<double>::infinity())));
}
double difference(std::int64_t left, std::int64_t right) noexcept {
    if ((left < 0) == (right < 0))
        return static_cast<double>(left - right);
    return static_cast<double>(left) - static_cast<double>(right);
}
struct Clock {
    struct Step {
        std::int64_t tick;
        double sample, speed;
    };
    std::vector<Step> steps;
    Clock(const rows::Model &model, double rate) {
        if (!std::isfinite(rate) || rate < 44100 || rate > 768000)
            throw std::runtime_error("controller rate outside 44.1-768 kHz");
        steps.push_back({0, 0, 120.0 * ppq / (60.0 * rate)});
        std::int64_t previous = -1;
        for (const auto &row : model.tempo) {
            if (row.posTicks < 0 || row.posTicks <= previous || !std::isfinite(row.bpm) || row.bpm <= 0)
                throw std::runtime_error("invalid controller tempo map");
            previous = row.posTicks;
            const auto &before = steps.back();
            const auto sample = before.sample + difference(row.posTicks, before.tick) / before.speed;
            const auto speed = row.bpm * ppq / (60.0 * rate);
            if (!std::isfinite(sample) || sample < 0 || !std::isfinite(speed) || speed <= 0)
                throw std::runtime_error("controller tempo arithmetic overflow");
            if (sample > sampleLimit)
                break;
            if (row.posTicks == 0)
                steps.back() = {0, 0, speed};
            else
                steps.push_back({row.posTicks, sample, speed});
        }
    }
    const Step &atSample(double sample) const noexcept {
        auto found = std::upper_bound(steps.begin(), steps.end(), sample,
                                      [](double value, const Step &step) { return value < step.sample; });
        return found == steps.begin() ? steps.front() : *--found;
    }
    double sampleAt(std::int64_t tick) const {
        if (tick < 0)
            throw std::runtime_error("negative controller placement");
        auto found = std::upper_bound(steps.begin(), steps.end(), tick,
                                      [](std::int64_t value, const Step &step) { return value < step.tick; });
        const auto &step = found == steps.begin() ? steps.front() : *--found;
        const auto sample = step.sample + difference(tick, step.tick) / step.speed;
        if (!std::isfinite(sample) || sample > sampleLimit)
            throw std::runtime_error("controller placement exceeds supported range");
        return sample;
    }
    std::int64_t wholeTick(double sample, bool roundUp = false) const {
        if (!std::isfinite(sample) || sample < 0 || sample > sampleLimit)
            throw std::runtime_error("invalid controller source clock");
        const auto &step = atSample(sample);
        const auto delta = (sample - step.sample) * step.speed;
        if (!std::isfinite(delta) || delta < 0 || delta > static_cast<double>(tickLimit) ||
            step.tick > tickLimit)
            throw std::runtime_error("controller musical clock exceeds supported range");
        const auto integral = static_cast<std::int64_t>(roundUp ? std::ceil(delta) : std::floor(delta));
        if (integral > tickLimit - step.tick)
            throw std::runtime_error("controller musical clock exceeds supported range");
        return step.tick + integral;
    }
};
struct Point {
    std::int64_t sample, time;
    double value;
    float tension;
    std::uint8_t curve;
};
struct Span {
    std::int64_t first = 0, last = 0, clipStart = 0, clipId = 0, songBase = 0, sourceBase = 0;
    double clockOffset = 0;
    std::vector<Point> points;
};
struct Interval {
    std::int64_t first, last;
    std::size_t span;
};
struct Lane {
    std::uint16_t address = 0;
    std::vector<Span> spans;
    std::vector<Interval> intervals;
};
const Interval *intervalAt(const Lane &lane, std::int64_t sample) noexcept {
    auto found =
        std::upper_bound(lane.intervals.begin(), lane.intervals.end(), sample,
                         [](std::int64_t value, const Interval &interval) { return value < interval.first; });
    if (found == lane.intervals.begin())
        return nullptr;
    --found;
    return sample < found->last ? &*found : nullptr;
}
void overlay(Lane &lane) {
    struct Edge {
        std::int64_t sample;
        std::size_t span;
        bool start;
    };
    std::vector<Edge> edges;
    edges.reserve(lane.spans.size() * 2);
    for (std::size_t i = 0; i < lane.spans.size(); ++i) {
        edges.push_back({lane.spans[i].first, i, true});
        edges.push_back({lane.spans[i].last, i, false});
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &left, const Edge &right) {
        return std::tie(left.sample, left.start, left.span) < std::tie(right.sample, right.start, right.span);
    });
    const auto compare = [&](std::size_t left, std::size_t right) {
        const auto &a = lane.spans[left];
        const auto &b = lane.spans[right];
        return std::tie(a.clipStart, a.clipId, left) < std::tie(b.clipStart, b.clipId, right);
    };
    std::set<std::size_t, decltype(compare)> active(compare);
    std::size_t at = 0;
    while (at < edges.size()) {
        const auto time = edges[at].sample;
        while (at < edges.size() && edges[at].sample == time) {
            if (edges[at].start)
                active.insert(edges[at].span);
            else
                active.erase(edges[at].span);
            ++at;
        }
        if (active.empty() || at == edges.size())
            continue;
        const auto end = edges[at].sample;
        const auto winner = *active.rbegin();
        if (end <= time)
            continue;
        if (!lane.intervals.empty() && lane.intervals.back().last == time &&
            lane.intervals.back().span == winner)
            lane.intervals.back().last = end;
        else
            lane.intervals.push_back({time, end, winner});
    }
}
} // namespace
struct MidiControllerProgram::Data {
    std::shared_ptr<const Clock> clock;
    std::vector<Lane> lanes;
    std::vector<std::int64_t> boundaries;
};
std::size_t MidiControllerProgram::size() const noexcept {
    return data_ ? data_->lanes.size() : 0;
}
std::uint16_t MidiControllerProgram::address(std::size_t index) const noexcept {
    return data_ && index < data_->lanes.size() ? data_->lanes[index].address : 0;
}
bool MidiControllerProgram::valueAt(std::size_t index, std::int64_t sample, double &value) const noexcept {
    if (!data_ || index >= data_->lanes.size())
        return false;
    const auto &lane = data_->lanes[index];
    const auto *interval = intervalAt(lane, sample);
    if (!interval)
        return false;
    const auto &span = lane.spans[interval->span];
    const auto &points = span.points;
    if (points.empty())
        return false;
    auto found = std::upper_bound(points.begin(), points.end(), sample,
                                  [](std::int64_t time, const Point &point) { return time < point.sample; });
    if (found == points.begin()) {
        value = points.front().value;
        return true;
    }
    if (found == points.end()) {
        value = points.back().value;
        return true;
    }
    const auto &right = *found;
    const auto &left = *--found;
    const auto clockSample = static_cast<double>(sample) - span.clockOffset;
    const auto &step = data_->clock->atSample(clockSample);
    const auto elapsed =
        std::max(0.0, difference(step.tick, span.songBase) + (clockSample - step.sample) * step.speed);
    const auto duration = difference(right.time, left.time);
    const auto position =
        duration > 0 ? std::clamp((elapsed + difference(span.sourceBase, left.time)) / duration, 0.0, 1.0)
                     : 1.0;
    value = std::clamp(curveValue(left.value, right.value, position, left.curve, left.tension), 0.0, 1.0);
    return true;
}
bool MidiControllerProgram::changesOwnerAt(std::size_t index, std::int64_t sample) const noexcept {
    if (!data_ || index >= data_->lanes.size())
        return false;
    const auto &lane = data_->lanes[index];
    const auto *now = intervalAt(lane, sample);
    const auto *before =
        sample == std::numeric_limits<std::int64_t>::min() ? nullptr : intervalAt(lane, sample - 1);
    return (now ? now->span + 1 : 0) != (before ? before->span + 1 : 0);
}
std::int64_t MidiControllerProgram::nextBoundary(std::int64_t at, std::int64_t end) const noexcept {
    if (!data_)
        return end;
    const auto found = std::lower_bound(data_->boundaries.begin(), data_->boundaries.end(), at);
    return found == data_->boundaries.end() ? end : std::min(end, *found);
}
MidiControllerPrograms::MidiControllerPrograms(const rows::Model &model, double rate) {
    std::shared_ptr<const Clock> clock;
    try {
        clock = std::make_shared<const Clock>(model, rate);
    } catch (const std::exception &e) {
        problems_.push_back(e.what());
        return;
    }
    std::map<std::int64_t, const rows::Clip *> clips;
    std::set<std::int64_t> tracks;
    for (const auto &clip : model.clips)
        clips.emplace(clip.id, &clip);
    for (const auto &track : model.tracks)
        tracks.insert(track.id);
    std::map<std::int64_t, std::map<std::uint16_t, Lane>> prepared;
    std::size_t spans = 0, points = 0;
    for (const auto &row : model.controllers) {
        const auto clipIt = clips.find(row.clipId);
        if (clipIt == clips.end() || clipIt->second->muted || !tracks.contains(clipIt->second->trackId))
            continue;
        const auto &clip = *clipIt->second;
        const auto oldSpans = spans, oldPoints = points;
        Lane *target = nullptr;
        std::size_t oldSize = 0;
        try {
            if (!row.error.empty())
                throw std::runtime_error(row.error);
            if (!row.channel || !row.controller || *row.channel < 0 || *row.channel > 15 ||
                *row.controller < 0 || *row.controller > 127)
                throw std::runtime_error("invalid controller channel/address");
            if (clip.aliasOf)
                throw std::runtime_error("controller clip alias unsupported");
            if (row.points.empty())
                continue;
            if (row.points.size() > pointBudget)
                throw std::runtime_error("controller point budget exceeded");
            std::vector<controllers::Point> raw = row.points;
            std::set<std::uint64_t> ids;
            for (const auto &point : raw)
                if (!point.id || !ids.insert(point.id).second || !std::isfinite(point.value) ||
                    point.value < 0 || point.value > 1 || !std::isfinite(point.tension) ||
                    point.tension < -1 || point.tension > 1 || point.curve > 5)
                    throw std::runtime_error("invalid controller point");
            std::stable_sort(raw.begin(), raw.end(), [](const auto &left, const auto &right) {
                return left.timeTicks < right.timeTicks;
            });
            double realStart = 0, offset = 0;
            std::int64_t origin = 0, length = 0, first = 0, last = 0;
            if (clip.timeBase == 0 && clip.posTicks && clip.lengthTicks) {
                origin = *clip.posTicks;
                length = *clip.lengthTicks;
                if (origin < 0 || origin > tickLimit || length <= 0 || length > tickLimit)
                    throw std::runtime_error("invalid musical controller clip time");
                realStart = clock->sampleAt(origin);
                first = roundedSample(realStart);
                last = roundedSample(clock->sampleAt(origin + length));
            } else if (clip.timeBase == 1 && clip.posNs && clip.lengthNs && *clip.posNs >= 0 &&
                       *clip.lengthNs > 0 && *clip.posNs <= INT64_MAX - *clip.lengthNs) {
                realStart = static_cast<double>(*clip.posNs) * 1e-9 * rate;
                first = roundedSample(realStart);
                const auto realEnd = static_cast<double>(*clip.posNs + *clip.lengthNs) * 1e-9 * rate;
                last = roundedSample(realEnd);
                origin = clock->wholeTick(realStart);
                offset = realStart - clock->sampleAt(origin);
                length = clock->wholeTick(realEnd - offset, true) - origin;
                if (length <= 0 || length > tickLimit)
                    throw std::runtime_error("invalid linear controller clip clock");
            } else
                throw std::runtime_error("invalid controller clip placement");
            if (last <= first)
                continue;
            const auto sourceOffset = clip.contentOffsetTicks, loop = clip.loopLenTicks.value_or(0),
                       loopStart = clip.loopStartTicks;
            if (sourceOffset < -tickLimit || sourceOffset > tickLimit ||
                (clip.loopEnabled &&
                 (loop <= 0 || loop > tickLimit || loopStart < 0 || loopStart > tickLimit)))
                throw std::runtime_error("invalid controller clip source window/loop");
            const auto key = static_cast<std::uint16_t>(*row.channel * 128 + *row.controller);
            target = &prepared[clip.trackId][key];
            target->address = key;
            oldSize = target->spans.size();
            const auto loopEnd = loopStart + loop;
            std::int64_t elapsed = 0, source = sourceOffset;
            if (clip.loopEnabled && source >= loopEnd)
                source = loopStart + (source - loopStart) % loop;
            while (elapsed < length) {
                const auto available = clip.loopEnabled ? loopEnd - source : length - elapsed;
                const auto count = std::min(length - elapsed, available);
                if (count <= 0)
                    throw std::runtime_error("controller loop made no progress");
                if (spans >= spanBudget)
                    throw std::runtime_error("controller repeat budget exceeded");
                Span span;
                span.first = elapsed == 0
                                 ? first
                                 : std::max(first, roundedSample(clock->sampleAt(origin + elapsed) + offset));
                span.last =
                    elapsed + count == length
                        ? last
                        : std::min(last, roundedSample(clock->sampleAt(origin + elapsed + count) + offset));
                span.clipStart = first;
                span.clipId = clip.id;
                span.songBase = origin + elapsed;
                span.sourceBase = source;
                span.clockOffset = offset;
                if (span.last > span.first) {
                    auto begin = std::lower_bound(
                        raw.begin(), raw.end(), source,
                        [](const auto &point, std::int64_t time) { return point.timeTicks < time; });
                    if (begin != raw.begin())
                        --begin;
                    auto end = std::lower_bound(
                        raw.begin(), raw.end(), source + count,
                        [](const auto &point, std::int64_t time) { return point.timeTicks < time; });
                    if (end != raw.end())
                        ++end;
                    const auto size = static_cast<std::size_t>(end - begin);
                    if (size > pointBudget - points)
                        throw std::runtime_error("controller placed-point budget exceeded");
                    span.points.reserve(size);
                    for (auto point = begin; point != end; ++point) {
                        auto sample = point->timeTicks < source ? std::int64_t(0)
                                                                : std::numeric_limits<std::int64_t>::max();
                        if (point->timeTicks >= source && point->timeTicks < source + count)
                            sample = roundedSample(
                                clock->sampleAt(origin + elapsed + point->timeTicks - source) + offset);
                        span.points.push_back(
                            {sample, point->timeTicks, point->value, point->tension, point->curve});
                    }
                    points += size;
                    ++spans;
                    target->spans.push_back(std::move(span));
                }
                elapsed += count;
                if (!clip.loopEnabled)
                    break;
                source = loopStart;
            }
        } catch (const std::exception &e) {
            if (target)
                target->spans.resize(oldSize);
            spans = oldSpans;
            points = oldPoints;
            problems_.push_back("controller stream#" + std::to_string(row.id) + ": " + e.what());
        }
    }
    for (auto &[track, lanes] : prepared) {
        auto data = std::make_shared<MidiControllerProgram::Data>();
        data->clock = clock;
        for (auto &[key, lane] : lanes) {
            (void)key;
            if (lane.spans.empty())
                continue;
            overlay(lane);
            for (const auto &interval : lane.intervals) {
                data->boundaries.push_back(interval.first);
                data->boundaries.push_back(interval.last);
                data->boundaries.push_back(interval.last - 1);
                for (const auto &point : lane.spans[interval.span].points)
                    if (point.sample >= interval.first && point.sample < interval.last)
                        data->boundaries.push_back(point.sample);
            }
            data->lanes.push_back(std::move(lane));
        }
        std::sort(data->boundaries.begin(), data->boundaries.end());
        data->boundaries.erase(std::unique(data->boundaries.begin(), data->boundaries.end()),
                               data->boundaries.end());
        tracks_.push_back({track, std::make_shared<const MidiControllerProgram>(std::move(data))});
    }
}
std::shared_ptr<const MidiControllerProgram> MidiControllerPrograms::forTrack(std::int64_t track) const {
    const auto found = std::lower_bound(tracks_.begin(), tracks_.end(), track,
                                        [](const auto &item, std::int64_t id) { return item.first < id; });
    return found != tracks_.end() && found->first == track ? found->second : nullptr;
}
} // namespace adi::engine
