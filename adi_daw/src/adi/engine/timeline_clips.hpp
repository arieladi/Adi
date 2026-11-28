// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/store_rows.hpp"
#include <cstddef>
#include <iterator>
#include <vector>

namespace adi::engine {

// The clips the timeline plays: the main lane or the auditioned take for
// each track. A hand-written forward range rather than a filter view, because
// libstdc++ 12's filter view does not compile under Clang 14 (DF-E69). It
// refers to the model's vector, so it must not outlive the model, and it
// never allocates.
class TimelineClipRange {
public:
    using Clips = std::vector<rows::Clip>;
    using Auditions = std::map<std::int64_t, std::int64_t>;

    class iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = rows::Clip;
        using difference_type = std::ptrdiff_t;
        using pointer = const rows::Clip*;
        using reference = const rows::Clip&;

        iterator() = default;
        iterator(Clips::const_iterator at, Clips::const_iterator end, const Auditions* auditions)
            : at_(at), end_(end), auditions_(auditions) { skip(); }

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
            while (at_ != end_) {
                const auto lane = auditions_->find(at_->trackId);
                if (lane == auditions_->end() ? !at_->laneId : at_->laneId == lane->second)
                    return;
                ++at_;
            }
        }
        Clips::const_iterator at_{};
        Clips::const_iterator end_{};
        const Auditions* auditions_ = nullptr;
    };

    explicit TimelineClipRange(const rows::Model& model) : model_(&model) {}

    [[nodiscard]] iterator begin() const {
        return {model_->clips.begin(), model_->clips.end(), &model_->auditionedTakeLanes};
    }
    [[nodiscard]] iterator end() const {
        return {model_->clips.end(), model_->clips.end(), &model_->auditionedTakeLanes};
    }

private:
    const rows::Model* model_;
};

static_assert(std::forward_iterator<TimelineClipRange::iterator>);

// The arrangement plays the comp in the main lane by default. Editing and
// persistence still see the stored takes through Model::clips. Session clips
// have their own vector and never enter this selection.
[[nodiscard]] inline TimelineClipRange timelineClips(const rows::Model& model) {
    return TimelineClipRange(model);
}

} // namespace adi::engine
