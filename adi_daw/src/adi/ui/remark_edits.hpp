// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Remarks from the window (RE-01; ADR-0131 d1, ADR-0139): a note anchored to a track, a clip, a device or one of
// its parameters. Add, edit, resolve or reopen, and remove, each one batch and one undo step, built from the
// snapshot the action lands on. The pips and the Info View that show them are the window lane's. No JUCE.
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
// The remarks anchored to `kind` #`target` (a parameter's when `param` is given), oldest first.
inline std::vector<const engine::RemarkNode *> remarksOn(const engine::Snapshot &snapshot, std::string_view kind,
                                                         std::int64_t target, std::optional<std::string_view> param = {}) {
    std::vector<const engine::RemarkNode *> out;
    for (const auto &r : snapshot.remarks)
        if (r && r->targetKind == kind && r->targetId == target &&
            (param ? r->paramId && *r->paramId == *param : !r->paramId))
            out.push_back(r.get());
    std::sort(out.begin(), out.end(), [](const auto *a, const auto *b) {
        return a->createdUtc != b->createdUtc ? a->createdUtc < b->createdUtc : a->id < b->id;
    });
    return out;
}
namespace remark_edits_detail {
inline std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}
inline const engine::RemarkNode *find(const engine::Snapshot &snapshot, std::int64_t id) {
    for (const auto &r : snapshot.remarks)
        if (r && r->id == id)
            return r.get();
    return nullptr;
}
} // namespace remark_edits_detail
// A remark of the user's on `kind` #`target` (`param` for a parameter's), written at `nowUtc` (microseconds, UTC), with the next id
// past every remark the snapshot holds. Refused, with the reason, for empty text or a kind no remark anchors to.
inline TrackEdit addRemark(const engine::Snapshot &snapshot, std::string_view kind, std::int64_t target,
                           std::optional<std::string> param, std::string_view text, std::int64_t nowUtc) {
    TrackEdit edit;
    const auto body = remark_edits_detail::trimmed(text);
    if (body.empty()) {
        edit.refusal = "Type the remark first.";
        return edit;
    }
    if (kind != "track" && kind != "clip" && kind != "device") {
        edit.refusal = "A remark goes on a track, a clip, a device or a parameter.";
        return edit;
    }
    std::int64_t id = 1;
    for (const auto &r : snapshot.remarks)
        if (r) {
            if (r->id == std::numeric_limits<std::int64_t>::max()) {
                edit.refusal = "No remark id is left.";
                return edit;
            }
            id = std::max(id, r->id + 1);
        }
    Payload payload{{"id", id},         {"kind", std::string(kind)}, {"target", target},
                    {"author", "user"}, {"text", body},              {"created", nowUtc}};
    if (param)
        payload["param"] = *param;
    edit.ops.push_back(track_edits_detail::op("remark.add", std::move(payload), "Add Remark"));
    return edit;
}
// The remark's text changed; nothing when it would not change; refused for empty text or a remark that is gone.
inline TrackEdit editRemark(const engine::Snapshot &snapshot, std::int64_t id, std::string_view text) {
    TrackEdit edit;
    const auto *r = remark_edits_detail::find(snapshot, id);
    const auto body = remark_edits_detail::trimmed(text);
    if (r == nullptr)
        edit.refusal = "That remark is gone.";
    else if (body.empty())
        edit.refusal = "A remark needs text; Delete Remark removes it.";
    else if (body != r->text)
        edit.ops.push_back(track_edits_detail::op("remark.edit", {{"id", id}, {"text", body}}, "Edit Remark"));
    return edit;
}
// The remark resolved, or reopened; nothing when it is so already.
inline TrackEdit resolveRemark(const engine::Snapshot &snapshot, std::int64_t id, bool resolved) {
    TrackEdit edit;
    const auto *r = remark_edits_detail::find(snapshot, id);
    if (r == nullptr)
        edit.refusal = "That remark is gone.";
    else if (r->resolved != resolved)
        edit.ops.push_back(track_edits_detail::op("remark.resolve", {{"id", id}, {"resolved", resolved}},
                                                  resolved ? "Resolve Remark" : "Reopen Remark"));
    return edit;
}
inline TrackEdit removeRemark(const engine::Snapshot &snapshot, std::int64_t id) {
    TrackEdit edit;
    if (remark_edits_detail::find(snapshot, id) == nullptr)
        edit.refusal = "That remark is gone.";
    else
        edit.ops.push_back(track_edits_detail::op("remark.remove", {{"id", id}}, "Delete Remark"));
    return edit;
}
} // namespace adi::ui
