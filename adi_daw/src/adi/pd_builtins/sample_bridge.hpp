// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "juce/pd_samples.hpp"

namespace adi::device {
// One acquire/announce PER SLOT PER SEGMENT, even when stereo externals both
// use the slot. A second acquire between their reads could retire a live buffer.
class PdSampleBlock {
    struct Entry { std::int32_t id; const PdSampleBuffer* sample = nullptr; };
    std::vector<Entry> entries_;
    inline static thread_local PdSampleBlock* current_ = nullptr;
public:
    void declare(const std::vector<PdSampleDecl>& declarations) {
        entries_.clear();
        for (const auto& d : declarations) entries_.push_back({d.id, nullptr});
    }
    class Read {
        PdSampleBlock* previous_;
    public:
        Read(PdSampleBlock& block, PdSampleSlots& slots) noexcept : previous_(current_) {
            for (auto& e : block.entries_) e.sample = slots.forBlock(e.id);
            current_ = &block;
        }
        ~Read() { current_ = previous_; }
        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;
    };
    static const PdSampleBuffer* get(std::int32_t id) noexcept {
        if (current_) for (const auto& e : current_->entries_) if (e.id == id) return e.sample;
        return nullptr;
    }
};
} // namespace adi::device
