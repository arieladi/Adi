// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The engine's held peak, taken once and shared (DF-W32; RESEARCH-INTAKE §6.7). A strip's meter keeps
// the highest peak since it was last taken (`StripMeter::takePeak`, adi/engine/mixer.hpp), and a take
// empties it: two views of one strip that each took it would each lose the peaks the other took. So one
// owner takes it, and keeps for each view that reads it the highest it has taken since that view last
// read. Message thread only; no allocation once each view has read once.
#pragma once
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
namespace adi::ui {
class HeldPeaks {
  public:
    // How a meter's held peak is taken: what `StripMeter::takePeak` does for the engine's.
    using Take = float (*)(const void *meter) noexcept;
    // The highest peak `meter` held since `reader` last asked: taken from the meter now, and kept for
    // every other reader of it until each asks.
    float read(const void *meter, const void *reader, Take take) {
        Entry *entry = nullptr;
        for (auto &e : meters_)
            if (e.meter == meter)
                entry = &e;
        if (entry == nullptr) {
            meters_.push_back({meter, {}});
            entry = &meters_.back();
        }
        const float fresh = take(meter);
        float *own = nullptr;
        for (auto &[who, pending] : entry->readers) {
            pending = std::max(pending, fresh);
            if (who == reader)
                own = &pending;
        }
        if (own != nullptr)
            return std::exchange(*own, 0.f);
        // A reader's first read: what was taken now, and from now on its own share.
        entry->readers.emplace_back(reader, 0.f);
        return fresh;
    }
    // `reader` reads no meter any more (it shows another, or is gone).
    void forget(const void *reader) {
        for (auto &e : meters_)
            e.readers.erase(std::remove_if(e.readers.begin(), e.readers.end(),
                                           [reader](const auto &r) { return r.first == reader; }),
                            e.readers.end());
        meters_.erase(std::remove_if(meters_.begin(), meters_.end(), [](const Entry &e) { return e.readers.empty(); }),
                      meters_.end());
    }
    std::size_t meters() const noexcept { return meters_.size(); }

  private:
    struct Entry {
        const void *meter;
        std::vector<std::pair<const void *, float>> readers;
    };
    std::vector<Entry> meters_;
};
} // namespace adi::ui
