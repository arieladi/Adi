// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/otio_timeline.hpp"

#include <string_view>
#include <utility>

namespace adi::interchange::otio {
namespace {
using Json = nlohmann::json;

const Json *member(const Json &object, const char *key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::string_view schema(const Json &object) {
    const auto *field = member(object, "OTIO_SCHEMA");
    return field && field->is_string() ? field->get_ref<const std::string &>() : std::string_view{};
}

bool readName(const Json &object, std::string &name) {
    const auto *field = member(object, "name");
    if (!field)
        return true;
    if (!field->is_string())
        return false;
    name = field->get<std::string>();
    return true;
}

bool readEnabled(const Json &object, bool &enabled) {
    const auto *field = member(object, "enabled");
    if (!field)
        return true;
    if (!field->is_boolean())
        return false;
    enabled = field->get<bool>();
    return true;
}

std::optional<TimelineError> checkEffects(const Json &object, const std::string &path) {
    if (const auto *effects = member(object, "effects")) {
        if (!effects->is_array())
            return TimelineError{TimelineIssue::InvalidField, path + "/effects"};
        if (!effects->empty())
            return TimelineError{TimelineIssue::UnsupportedEffects, path + "/effects"};
    }
    return {};
}

std::optional<TimelineError> checkTrim(const Json &object, const std::string &path) {
    if (const auto *trim = member(object, "source_range"); trim && !trim->is_null())
        return TimelineError{TimelineIssue::UnsupportedTrim, path + "/source_range"};
    return {};
}

std::string pointerPart(std::string_view key) {
    std::string name;
    for (const auto ch : key) {
        switch (ch) {
        case '~': name += "~0"; break;
        case '/': name += "~1"; break;
        default: name += ch; break;
        }
    }
    return name;
}

struct ActiveReference {
    const Json *reference = nullptr;
    std::string path;
};

std::variant<ActiveReference, TimelineError>
readActiveReference(const Json &object, bool modern, const std::string &path) {
    if (!modern)
        return ActiveReference{member(object, "media_reference"), path + "/media_reference"};
    const auto *references = member(object, "media_references");
    if (!references || !references->is_object())
        return TimelineError{TimelineIssue::InvalidField, path + "/media_references"};
    const auto *key = member(object, "active_media_reference_key");
    if (!key || !key->is_string())
        return TimelineError{TimelineIssue::InvalidField, path + "/active_media_reference_key"};
    const auto &name = key->get_ref<const std::string &>();
    const auto found = references->find(name);
    auto referencePath = path + "/media_references/" + pointerPart(name);
    if (found == references->end())
        return TimelineError{TimelineIssue::MissingMediaReference, std::move(referencePath)};
    return ActiveReference{&*found, std::move(referencePath)};
}

std::variant<LinearItem, TimelineError> readItem(const Json &object, const std::string &path) {
    const auto type = schema(object);
    if (type != "Gap.1" && type != "Clip.1" && type != "Clip.2")
        return TimelineError{TimelineIssue::UnsupportedSchema, path + "/OTIO_SCHEMA"};
    LinearItem item;
    item.kind = type == "Gap.1" ? ItemKind::Gap : ItemKind::Clip;
    if (!readName(object, item.name))
        return TimelineError{TimelineIssue::InvalidField, path + "/name"};
    if (!readEnabled(object, item.enabled))
        return TimelineError{TimelineIssue::InvalidField, path + "/enabled"};
    if (const auto effects = checkEffects(object, path))
        return *effects;

    const Json *source = member(object, "source_range");
    std::string sourcePath = path + "/source_range";
    if (item.kind == ItemKind::Clip) {
        const auto active = readActiveReference(object, type == "Clip.2", path);
        if (const auto *error = std::get_if<TimelineError>(&active))
            return *error;
        const auto &ref = std::get<ActiveReference>(active);
        if (ref.reference && !ref.reference->is_null()) {
            const auto mediaSchema = schema(*ref.reference);
            if (mediaSchema.empty())
                return TimelineError{TimelineIssue::InvalidField, ref.path + "/OTIO_SCHEMA"};
            if (mediaSchema == "ExternalReference.1") {
                const auto *url = member(*ref.reference, "target_url");
                if (!url || !url->is_string())
                    return TimelineError{TimelineIssue::InvalidField, ref.path + "/target_url"};
                item.media = MediaKind::External;
                item.targetUrl = url->get<std::string>();
            } else if (mediaSchema != "MissingReference.1") {
                item.media = MediaKind::Unsupported;
            }
            if (!source || source->is_null()) {
                source = member(*ref.reference, "available_range");
                sourcePath = ref.path + "/available_range";
            }
        }
    }
    if (!source || source->is_null())
        return TimelineError{TimelineIssue::MissingSourceRange, std::move(sourcePath)};
    const auto range = readRange(*source);
    if (!range)
        return TimelineError{TimelineIssue::InvalidTime, std::move(sourcePath)};
    item.source = *range;
    return item;
}

std::variant<LinearTrack, TimelineError> readTrack(const Json &object, const std::string &path) {
    if (schema(object) != "Track.1")
        return TimelineError{TimelineIssue::UnsupportedSchema, path + "/OTIO_SCHEMA"};
    LinearTrack track;
    if (!readName(object, track.name))
        return TimelineError{TimelineIssue::InvalidField, path + "/name"};
    if (!readEnabled(object, track.enabled))
        return TimelineError{TimelineIssue::InvalidField, path + "/enabled"};
    if (const auto *kind = member(object, "kind")) {
        if (*kind == "Audio")
            track.kind = TrackKind::Audio;
        else if (*kind != "Video")
            return TimelineError{TimelineIssue::UnsupportedTrackKind, path + "/kind"};
    }
    if (const auto effects = checkEffects(object, path))
        return *effects;
    if (const auto trim = checkTrim(object, path))
        return *trim;
    const auto *children = member(object, "children");
    if (!children || !children->is_array())
        return TimelineError{TimelineIssue::InvalidField, path + "/children"};
    track.items.reserve(children->size());
    for (std::size_t i = 0; i < children->size(); ++i) {
        auto item = readItem((*children)[i], path + "/children/" + std::to_string(i));
        if (const auto *error = std::get_if<TimelineError>(&item))
            return *error;
        track.items.push_back(std::get<LinearItem>(std::move(item)));
    }
    return track;
}

Json composable(const char *type, const std::string &name, bool enabled) {
    return Json{{"OTIO_SCHEMA", type}, {"name", name}, {"enabled", enabled},
                {"metadata", Json::object()}, {"markers", Json::array()}, {"effects", Json::array()}};
}

std::variant<Json, TimelineError> writeItem(const LinearItem &item, const std::string &path) {
    const auto range = writeRange(item.source);
    if (!range)
        return TimelineError{TimelineIssue::InvalidTime, path + "/source_range"};
    if (item.kind == ItemKind::Gap) {
        auto gap = composable("Gap.1", item.name, item.enabled);
        gap["source_range"] = *range;
        return gap;
    }
    auto clip = composable("Clip.2", item.name, item.enabled);
    clip["source_range"] = *range;
    Json reference{{"name", ""}, {"metadata", Json::object()}, {"available_range", nullptr}};
    switch (item.media) {
    case MediaKind::Missing:
        reference["OTIO_SCHEMA"] = "MissingReference.1";
        break;
    case MediaKind::External:
        reference["OTIO_SCHEMA"] = "ExternalReference.1";
        reference["target_url"] = item.targetUrl;
        break;
    case MediaKind::Unsupported:
        return TimelineError{TimelineIssue::UnsupportedMedia, path + "/media_references"};
    }
    clip["media_references"] = Json{{"DEFAULT_MEDIA", std::move(reference)}};
    clip["active_media_reference_key"] = "DEFAULT_MEDIA";
    return clip;
}
} // namespace

std::variant<LinearTimeline, TimelineError> readLinearTimeline(const Json &document) {
    if (schema(document) != "Timeline.1")
        return TimelineError{TimelineIssue::UnsupportedSchema, "/OTIO_SCHEMA"};
    LinearTimeline result;
    if (!readName(document, result.name))
        return TimelineError{TimelineIssue::InvalidField, "/name"};
    if (const auto *start = member(document, "global_start_time"); start && !start->is_null()) {
        result.globalStart = readTime(*start);
        if (!result.globalStart)
            return TimelineError{TimelineIssue::InvalidTime, "/global_start_time"};
    }
    const auto *stack = member(document, "tracks");
    if (!stack || schema(*stack) != "Stack.1")
        return TimelineError{TimelineIssue::UnsupportedSchema, "/tracks/OTIO_SCHEMA"};
    if (const auto effects = checkEffects(*stack, "/tracks"))
        return *effects;
    if (const auto trim = checkTrim(*stack, "/tracks"))
        return *trim;
    bool enabled = true;
    if (!readEnabled(*stack, enabled))
        return TimelineError{TimelineIssue::InvalidField, "/tracks/enabled"};
    const auto *children = member(*stack, "children");
    if (!children || !children->is_array())
        return TimelineError{TimelineIssue::InvalidField, "/tracks/children"};
    result.tracks.reserve(children->size());
    for (std::size_t i = 0; i < children->size(); ++i) {
        auto track = readTrack((*children)[i], "/tracks/children/" + std::to_string(i));
        if (const auto *error = std::get_if<TimelineError>(&track))
            return *error;
        auto &parsed = std::get<LinearTrack>(track);
        parsed.enabled = parsed.enabled && enabled;
        result.tracks.push_back(std::move(parsed));
    }
    return result;
}

std::variant<Json, TimelineError> writeLinearTimeline(const LinearTimeline &timeline) {
    Json document{{"OTIO_SCHEMA", "Timeline.1"}, {"name", timeline.name},
                  {"metadata", Json::object()}, {"global_start_time", nullptr}};
    if (timeline.globalStart) {
        const auto start = writeTime(*timeline.globalStart);
        if (!start)
            return TimelineError{TimelineIssue::InvalidTime, "/global_start_time"};
        document["global_start_time"] = *start;
    }
    auto stack = composable("Stack.1", "", true);
    stack["source_range"] = nullptr;
    auto tracks = Json::array();
    for (const auto &track : timeline.tracks) {
        const auto path = "/tracks/children/" + std::to_string(tracks.size());
        auto wireTrack = composable("Track.1", track.name, track.enabled);
        wireTrack["kind"] = track.kind == TrackKind::Audio ? "Audio" : "Video";
        wireTrack["source_range"] = nullptr;
        auto items = Json::array();
        for (const auto &item : track.items) {
            auto wireItem = writeItem(item, path + "/children/" + std::to_string(items.size()));
            if (const auto *error = std::get_if<TimelineError>(&wireItem))
                return *error;
            items.push_back(std::get<Json>(std::move(wireItem)));
        }
        wireTrack["children"] = std::move(items);
        tracks.push_back(std::move(wireTrack));
    }
    stack["children"] = std::move(tracks);
    document["tracks"] = std::move(stack);
    return document;
}

} // namespace adi::interchange::otio
