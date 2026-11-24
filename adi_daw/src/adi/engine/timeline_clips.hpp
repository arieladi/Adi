// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/store_rows.hpp"
#include <cstddef>
#include <iterator>
#include <vector>

namespace adi::engine {

// The clips the timeline plays: Model::clips without the clips stored in take
// lanes. A hand-written forward range rather than a filter view, because
// libstdc++ 12's filter view does not compile under Clang 14 (DF-E69). It
// refers to the model's vector, so it must not outlive the model, and it
// never allocates.
class TimelineClipRange {
public:
    using Clips = std::vector<rows::Clip>;

    class iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = rows::Clip;
        using difference_type = std::ptrdiff_t;
        using pointer = const rows::Clip*;
        using reference = const rows::Clip&;

        iterator() = default;
        iterator(Clips::const_iterator at, Clips::const_iterator end) : at_(at), end_(end) { skip(); }

        [[nodiscard]] reference operator*() const { return *at_; }
        [[nodiscard]] pointer operator->() const { return &*at_; }
        iterator& operator++() {
            ++at_;
            skip();
            return *this;
        }
        iterator operator++(int) {
            auto before = *this;
            ++*this;
            return before;
        }
        [[nodiscard]] friend bool operator==(const iterator& a, const iterator& b) { return a.at_ == b.at_; }

    private:
        void skip() {
            while (at_ != end_ && at_->laneId.has_value())
                ++at_;
        }
        Clips::const_iterator at_{};
        Clips::const_iterator end_{};
    };

    explicit TimelineClipRange(const Clips& clips) : clips_(&clips) {}

    [[nodiscard]] iterator begin() const { return {clips_->begin(), clips_->end()}; }
    [[nodiscard]] iterator end() const { return {clips_->end(), clips_->end()}; }

private:
    const Clips* clips_;
};

static_assert(std::forward_iterator<TimelineClipRange::iterator>);

// The arrangement plays the comp in the main lane by default. Editing and
// persistence still see the stored takes through Model::clips. The selection
// belongs here so audio, MIDI, envelopes and offline readers agree.
[[nodiscard]] inline TimelineClipRange timelineClips(const rows::Model& model) {
    return TimelineClipRange(model.clips);
}

} // namespace adi::engine
