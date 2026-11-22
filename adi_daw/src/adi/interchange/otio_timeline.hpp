// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/interchange/otio_time.hpp"

#include <string>
#include <variant>
#include <vector>

namespace adi::interchange::otio {

enum class TrackKind { Audio, Video };
enum class ItemKind { Clip, Gap };
enum class MediaKind { Missing, External, Unsupported };

struct LinearItem {
    ItemKind kind = ItemKind::Clip;
    std::string name;
    bool enabled = true;
    TimeRange source;
    MediaKind media = MediaKind::Missing;
    std::string targetUrl;
};

struct LinearTrack {
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool enabled = true;
    std::vector<LinearItem> items;
};

struct LinearTimeline {
    std::string name;
    std::optional<RationalTime> globalStart;
    std::vector<LinearTrack> tracks;
};

enum class TimelineIssue {
    InvalidField, UnsupportedSchema, UnsupportedTrackKind, UnsupportedTrim,
    UnsupportedEffects, InvalidTime, MissingSourceRange, MissingMediaReference
};

struct TimelineError {
    TimelineIssue issue;
    std::string path; // JSON Pointer into the original document.
};

/// Project-independent projection of the serial Clip/Gap subset. Items are in
/// presentation order and retain source-frame positions and durations. The
/// caller retains the original DOM for metadata, markers and inactive media
/// references. A timing-affecting unsupported construct refuses the projection.
[[nodiscard]] std::variant<LinearTimeline, TimelineError> readLinearTimeline(const nlohmann::json &);

} // namespace adi::interchange::otio
