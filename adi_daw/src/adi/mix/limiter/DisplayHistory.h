// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace adi::mix::limiter::display
{
enum class Mode { slowDown, fast, slow, infinite, off };
enum class Scale { db16, db32, db48, k12, k14, k20, loudness };

struct Levels
{
    float inputDb = -160, outputDb = -160, reductionDb = 0, rmsDb = -160, loudnessDb = -160;
    bool valid = true;
};
struct Column : Levels { float x = 0; };
struct PeakLabel { float x = 0, reductionDb = 0, alpha = 0; };
struct Tick { float db = 0, label = 0, y = 0; };

inline float levelY(float db, Scale scale) noexcept
{
    if (!std::isfinite(db)) return 1;
    if (scale >= Scale::k12 && scale <= Scale::k20) return std::clamp(-db / 46.0f, 0.0f, 1.0f);
    if (scale == Scale::loudness) return std::clamp(-db / 36.0f, 0.0f, 1.0f);
    const float range = scale == Scale::db16 ? 16.0f : scale == Scale::db32 ? 32.0f : 48.0f;
    constexpr std::array<float, 11> fractions {0, .3125f, .375f, 5.0f / 12, .5f, .625f,
                                               2.0f / 3, .75f, 5.0f / 6, 11.0f / 12, 1};
    constexpr std::array<float, 11> positions {0, .656f, .721f, .765f, .819f, .885f,
                                               .906f, .933f, .955f, .977f, 1};
    const auto f = std::clamp(-db / range, 0.0f, 1.0f);
    for (std::size_t i = 1; i < fractions.size(); ++i)
        if (f <= fractions[i])
            return positions[i - 1] + (positions[i] - positions[i - 1]) *
                (f - fractions[i - 1]) / (fractions[i] - fractions[i - 1]);
    return 1;
}

inline std::vector<Tick> grid(Scale scale)
{
    std::vector<Tick> result;
    const auto add = [&](float db, float label) { result.push_back({db, label, levelY(db, scale)}); };
    if (scale >= Scale::k12 && scale <= Scale::k20)
    {
        const float reference = scale == Scale::k12 ? 12.0f : scale == Scale::k14 ? 14.0f : 20.0f;
        add(0, reference);
        for (float label = std::ceil(reference / 4) * 4 - 4; label > reference - 46; label -= 4)
            add(label - reference, label);
    }
    else if (scale == Scale::loudness)
    {
        add(0, 0);
        for (float db = -2; db >= -35; db -= 3) add(db, db);
    }
    else
    {
        const float range = scale == Scale::db16 ? 16.0f : scale == Scale::db32 ? 32.0f : 48.0f;
        const float step = scale == Scale::db32 ? 1.0f : range / 16;
        for (float db = 0; db >= -range * .3125f; db -= step) add(db, db);
        for (float f : {.375f, .5f, .625f, 1.0f}) add(-range * f, -range * f);
    }
    return result;
}

// UI-thread-only history. Storage is bounded, and ingestion never allocates.
// Coordinates remain in display space so a mode change leaves old spacing intact.
class History
{
public:
    static constexpr std::size_t capacity = 8192;
    static constexpr std::size_t labelCapacity = 128;

    void reset(double seconds, float graphWidth) noexcept
    {
        width = std::clamp(static_cast<double>(graphWidth), 32.0, 4096.0);
        now = seconds;
        initialized = true;
        first = count = labelFirst = labelCount = 0;
        offset = 0;
        zoom = 1;
        writePosition = mode == Mode::infinite ? 0 : width * .305;
        rate = 120;
        phase = Phase::moving;
        phaseTime = 0;
        previousReduction = 0;
        rising = false;
        activeLabel = labelCapacity;
    }

    void setMode(Mode next) noexcept
    {
        if (next == mode) return;
        flatten();
        phase = Phase::moving;
        phaseTime = 0;
        rate = 120;
        const auto previousMode = mode;
        mode = next;
        if (mode == Mode::infinite && count == 0) writePosition = 0;
        if (mode == Mode::slowDown && previousMode != Mode::off && writePosition >= width)
        {
            offset += width * .65;
            prune();
        }
    }

    Mode getMode() const noexcept { return mode; }
    bool enabled() const noexcept { return mode != Mode::off; }
    float headX() const noexcept { return static_cast<float>(std::clamp((writePosition - offset) * zoom, 0.0, width)); }
    bool headVisible() const noexcept { return enabled() && phase != Phase::holding; }
    std::size_t storedColumns() const noexcept { return count; }
    double currentTime() const noexcept { return now; }

    void advance(double seconds, float graphWidth) noexcept
    {
        if (!std::isfinite(seconds)) return;
        if (!initialized) { reset(seconds, graphWidth); return; }
        const auto nextWidth = std::clamp(static_cast<double>(graphWidth), 32.0, 4096.0);
        if (std::abs(nextWidth - width) > .5)
        {
            const auto headFraction = (writePosition - offset) * zoom / width;
            flatten();
            const auto desiredHead = std::clamp(headFraction, 0.0, 1.0) * nextWidth;
            offset += writePosition - offset - desiredHead;
            width = nextWidth;
            phase = Phase::moving;
        }
        if (seconds <= now) return;
        auto remaining = seconds - now;
        now = seconds;
        if (!enabled()) return;
        while (remaining > 1.0e-8)
        {
            if (mode == Mode::fast || mode == Mode::slow)
            {
                writePosition += remaining * (mode == Mode::fast ? 120 : 30);
                offset = std::max(offset, writePosition - width);
                remaining = 0;
            }
            else if (mode == Mode::infinite)
                advanceInfinite(remaining);
            else
                advanceSlowDown(remaining);
        }
        prune();
    }

    void push(double seconds, const Levels& value) noexcept
    {
        if (!initialized) reset(seconds, static_cast<float>(width));
        advance(seconds, static_cast<float>(width));
        if (!enabled() || !value.valid) return;
        const double speed = mode == Mode::slow ? 30 : mode == Mode::infinite ? rate : 120;
        const auto x = writePosition - std::max(0.0, now - seconds) * speed;
        if ((x - offset) * zoom < -1) return;
        if (count > 0)
        {
            auto& last = samples[(first + count - 1) % capacity];
            if (std::floor(last.position) == std::floor(x))
                merge(last.levels, value);
            else
                append({x, value});
        }
        else append({x, value});
        updateLabels(seconds, x, value.reductionDb);
    }

    // One output entry per logical column, including invalid entries for gaps.
    void render(std::vector<Column>& columns, int pixelWidth) const
    {
        pixelWidth = std::clamp(pixelWidth, 1, 4096);
        columns.resize(static_cast<std::size_t>(pixelWidth));
        for (int i = 0; i < pixelWidth; ++i)
        {
            columns[static_cast<std::size_t>(i)] = Column{};
            columns[static_cast<std::size_t>(i)].valid = false;
            columns[static_cast<std::size_t>(i)].x = static_cast<float>(i);
        }
        if (!enabled()) return;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& source = samples[(first + i) % capacity];
            // A stored bin occupies a full column in its original coordinate
            // space. Rasterise its extent so a fractional scroll cannot turn
            // continuous data into alternating empty columns. Missing source
            // bins remain missing; no interpolation crosses an audio gap.
            const auto start = (std::floor(source.position) - offset) * zoom;
            const auto end = start + zoom;
            if (end <= 0 || start >= width) continue;
            const int firstBin = std::clamp(static_cast<int>(std::floor(start * pixelWidth / width)), 0, pixelWidth - 1);
            const int lastBin = std::clamp(static_cast<int>(std::ceil(end * pixelWidth / width)) - 1, 0, pixelWidth - 1);
            for (int bin = firstBin; bin <= lastBin; ++bin)
            {
                auto& destination = columns[static_cast<std::size_t>(bin)];
                if (!destination.valid) static_cast<Levels&>(destination) = source.levels;
                else merge(destination, source.levels);
            }
        }
    }

    void labels(std::vector<PeakLabel>& result) const
    {
        result.clear();
        if (!enabled()) return;
        float previousRight = -100;
        float acceptedPeak = -100;
        for (std::size_t i = 0; i < labelCount; ++i)
        {
            const auto& label = peakLabels[(labelFirst + i) % labelCapacity];
            if (std::abs(label.peak - acceptedPeak) < .5f) continue;
            acceptedPeak = label.peak;
            const auto x = static_cast<float>((label.position - offset) * zoom);
            const auto alpha = static_cast<float>(std::clamp((now - label.time - .85) / .08, 0.0, 1.0));
            if (alpha <= 0 || x < 0 || x + 43 > width || x < previousRight) continue;
            result.push_back({x, label.peak, alpha});
            previousRight = x + 47;
        }
    }

private:
    struct Sample { double position = 0; Levels levels; };
    struct Label { double position = 0, time = 0; float peak = 0; };
    enum class Phase { moving, holding, jumping };
    std::array<Sample, capacity> samples{};
    std::array<Label, labelCapacity> peakLabels{};
    std::size_t first = 0, count = 0, labelFirst = 0, labelCount = 0, activeLabel = labelCapacity;
    double width = 800, now = 0, offset = 0, writePosition = 244, zoom = 1, rate = 120, phaseTime = 0;
    Mode mode = Mode::slowDown;
    Phase phase = Phase::moving;
    bool initialized = false, rising = false;
    float previousReduction = 0;

    static double ease(double t) noexcept
    {
        t = std::clamp(t, 0.0, 1.0);
        return t * t * t * (10 + t * (-15 + 6 * t));
    }
    static void merge(Levels& a, const Levels& b) noexcept
    {
        a.inputDb = std::max(a.inputDb, b.inputDb);
        a.outputDb = std::max(a.outputDb, b.outputDb);
        a.reductionDb = std::max(a.reductionDb, b.reductionDb);
        a.rmsDb = std::max(a.rmsDb, b.rmsDb);
        a.loudnessDb = std::max(a.loudnessDb, b.loudnessDb);
        a.valid = a.valid || b.valid;
    }
    void append(const Sample& sample) noexcept
    {
        if (count == capacity) { first = (first + 1) % capacity; --count; }
        samples[(first + count++) % capacity] = sample;
    }
    void prune() noexcept
    {
        while (count && (samples[first].position - offset) * zoom < -1)
        { first = (first + 1) % capacity; --count; }
        while (labelCount && (peakLabels[labelFirst].position - offset) * zoom < -50)
        { labelFirst = (labelFirst + 1) % labelCapacity; --labelCount; }
    }
    void flatten() noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            samples[(first + i) % capacity].position = (samples[(first + i) % capacity].position - offset) * zoom;
        for (std::size_t i = 0; i < labelCount; ++i)
            peakLabels[(labelFirst + i) % labelCapacity].position = (peakLabels[(labelFirst + i) % labelCapacity].position - offset) * zoom;
        writePosition = (writePosition - offset) * zoom;
        offset = 0;
        zoom = 1;
    }
    void compact() noexcept
    {
        // In-place compaction of the circular queue preserves all per-bin peaks.
        std::size_t written = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto sample = samples[(first + i) % capacity];
            if (written && std::floor(samples[(first + written - 1) % capacity].position) == std::floor(sample.position))
                merge(samples[(first + written - 1) % capacity].levels, sample.levels);
            else samples[(first + written++) % capacity] = sample;
        }
        count = written;
    }
    void advanceSlowDown(double& remaining) noexcept
    {
        if (phase == Phase::moving)
        {
            const auto duration = std::max(0.0, (width - (writePosition - offset)) / 90);
            const auto step = std::min(remaining, duration);
            writePosition += step * 120;
            offset += step * 30;
            remaining -= step;
            if (step >= duration) { phase = Phase::holding; phaseTime = 0; }
        }
        else
        {
            const auto duration = phase == Phase::holding ? .2 : .5;
            const auto step = std::min(remaining, duration - phaseTime);
            writePosition += step * 120;
            offset += step * 30;
            if (phase == Phase::jumping)
                offset += width * .74 * (ease((phaseTime + step) / duration) - ease(phaseTime / duration));
            phaseTime += step;
            remaining -= step;
            if (phaseTime >= duration - 1.0e-9)
            { phase = phase == Phase::holding ? Phase::jumping : Phase::moving; phaseTime = 0; }
        }
    }
    void advanceInfinite(double& remaining) noexcept
    {
        if (phase != Phase::jumping)
        {
            const auto duration = std::max(0.0, (width - writePosition) / rate);
            const auto step = std::min(remaining, duration);
            writePosition += step * rate;
            remaining -= step;
            if (step >= duration) { phase = Phase::jumping; phaseTime = 0; }
        }
        else
        {
            const auto step = std::min(remaining, .4 - phaseTime);
            writePosition += step * rate;
            phaseTime += step;
            remaining -= step;
            zoom = 1 - .5 * ease(phaseTime / .4);
            if (phaseTime >= .4 - 1.0e-9)
            {
                flatten(); compact(); rate *= .5;
                phase = Phase::moving; phaseTime = 0;
            }
        }
    }
    void updateLabels(double seconds, double x, float reduction) noexcept
    {
        if (!std::isfinite(reduction)) return;
        if (reduction > previousReduction + .025f && reduction > .1f)
        {
            if (!rising)
            {
                activeLabel = labelCapacity;
                const bool clear = labelCount == 0 ||
                    (x - peakLabels[(labelFirst + labelCount - 1) % labelCapacity].position) * zoom >= 47;
                if (clear)
                {
                    if (labelCount == labelCapacity) { labelFirst = (labelFirst + 1) % labelCapacity; --labelCount; }
                    activeLabel = (labelFirst + labelCount++) % labelCapacity;
                    peakLabels[activeLabel] = {x, seconds, reduction};
                }
                rising = true;
            }
            if (activeLabel < labelCapacity) peakLabels[activeLabel].peak = std::max(peakLabels[activeLabel].peak, reduction);
        }
        else if (reduction < previousReduction - .025f) rising = false;
        previousReduction = reduction;
    }
};
}
