// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/session_view_edits.hpp"

#include "adi/clip_image.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <optional>
#include <stdexcept>

namespace adi::ui::session_view {
namespace {

void append(clip_edit::PreparedEdit& result, const char* type, Payload payload, const std::string& label,
            std::int64_t clip) {
    OpRequest request;
    request.opType = type;
    request.payload = std::move(payload);
    request.label = label;
    request.targetKind = "clip";
    request.targetId = clip;
    result.requests.push_back(std::move(request));
}

bool flag(const Payload& row, const char* field) {
    const auto value = adi::detail::signedInteger(row.at(field));
    if (value != 0 && value != 1)
        throw std::runtime_error("invalid stored Session flag " + std::string(field));
    return value == 1;
}

/// The clip's launch settings, as its stored row holds them, given to its copy.
void launchSettings(clip_edit::PreparedEdit& result, const Payload& row, std::int64_t clip, const std::string& label) {
    append(result, "session.setLaunchMode", {{"clip", clip}, {"mode", row.at("launch_mode")}}, label, clip);
    append(result, "session.setLaunchQuant", {{"clip", clip}, {"quant", row.at("launch_quant")}}, label, clip);
    append(result, "session.setLaunchSettings",
           {{"clip", clip}, {"legato", flag(row, "legato")}, {"velocity", row.at("velocity_amount")},
            {"fade", flag(row, "clip_fade")}, {"ram", flag(row, "ram_mode")}},
           label, clip);
    Payload follow{{"clip", clip},
                   {"enabled", flag(row, "follow_enabled")},
                   {"linked", flag(row, "follow_linked")},
                   {"loops", row.at("follow_loop_count")},
                   {"duration", row.at("follow_duration_ticks")},
                   {"actionA", row.at("follow_a")},
                   {"actionB", row.at("follow_b")},
                   {"chanceB", row.at("follow_b_chance")}};
    if (!row.at("follow_a_scene_id").is_null())
        follow["jumpA"] = row.at("follow_a_scene_id");
    if (!row.at("follow_b_scene_id").is_null())
        follow["jumpB"] = row.at("follow_b_scene_id");
    append(result, "session.setFollowAction", std::move(follow), label, clip);
}

/// A clip's image and launch row, from the clipboard where it holds them, else the project.
Payload sourceOf(OpContext& context, std::int64_t clip, const Clipboard* board) {
    if (board != nullptr)
        if (const auto held = board->images.find(std::to_string(clip)); held != board->images.end())
            return *held;
    auto launch = adi::detail::captureRow(context.db, "clip_launch", "clip_id", clip);
    return {{"image", clip_edit::captureImage(context, clip)}, {"launch", launch ? *launch : Payload(nullptr)}};
}

void appendCopy(OpContext& context, clip_edit::PreparedEdit& result, clip_edit::CopyIds& ids, const SlotCopy& copy,
                const Clipboard* board, const std::string& label) {
    const auto source = sourceOf(context, copy.clip, board);
    const auto& image = source.at("image");
    const auto& kind = image.at("row").at("kind").get_ref<const std::string&>();
    if (kind != "audio" && kind != "midi")
        throw std::runtime_error("a slot takes an audio or MIDI clip");
    const auto clip = ids.next("clips");
    auto clone = clip_edit::cloneImage(image, clip, ids);
    clone["row"]["track_id"] = copy.track; // the copy belongs to the track it lands on
    edit::RowEdits rows(context.db);
    clip_edit::createImage(rows, clone);
    append(result, "clip.duplicate", {{"id", clip}, {"changes", rows.changes()}}, label, clip);
    append(result, "session.setSlotClip", {{"track", copy.track}, {"scene", copy.scene}, {"clip", clip}}, label, clip);
    if (copy.stop && !*copy.stop)
        append(result, "session.setStopButton", {{"track", copy.track}, {"scene", copy.scene}, {"on", false}}, label, clip);
    if (const auto& launch = source.at("launch"); !launch.is_null())
        launchSettings(result, launch, clip, label);
}

} // namespace

bool snapshotClipboard(Store& store, Clipboard& board, std::string& error) {
    try {
        OpContext context{store, store.db()};
        auto images = Payload::object();
        const auto take = [&](std::int64_t clip) {
            if (clip != 0 && !images.contains(std::to_string(clip)))
                images[std::to_string(clip)] = sourceOf(context, clip, nullptr);
        };
        for (const auto& clip : board.clips)
            take(clip.clip);
        for (const auto& scene : board.scenes)
            for (const auto& slot : scene.slots)
                take(slot.clip);
        board.images = std::move(images);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

clip_edit::PreparedEdit prepareEdit(Store& store, const Outcome& outcome, const Clipboard* board) {
    clip_edit::PreparedEdit result;
    try {
        if (store.readOnly())
            throw std::runtime_error("editing the Session needs a writable project");
        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        OpContext context{store, db};
        clip_edit::CopyIds ids(db);
        for (const auto& copy : outcome.copies)
            ids.reserve("clips", copy.clip); // a Cut's clip is gone, and its identity is not a copy's
        const auto label = [&](std::size_t at) {
            if (!outcome.label.empty())
                return outcome.label;
            if (at < outcome.edits.size())
                return outcome.edits[at].label;
            return outcome.edits.empty() ? std::string("Paste") : outcome.edits.back().label;
        };
        for (std::size_t at = 0; at <= outcome.edits.size(); ++at) {
            for (const auto& copy : outcome.copies)
                if (copy.before == at)
                    appendCopy(context, result, ids, copy, board, label(at));
            if (at < outcome.edits.size())
                result.requests.push_back(outcome.edits[at]);
        }
        for (const auto& copy : outcome.copies)
            if (copy.before > outcome.edits.size())
                throw std::runtime_error("a slot copy is placed after the edits");
        result.sourceFile = db.execAndGet("SELECT file FROM pragma_database_list WHERE name='main'").getString();
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        if (read)
            read->commit();
        result.ok = true;
    } catch (const std::exception& error) {
        result = {};
        result.error = error.what();
    }
    return result;
}

} // namespace adi::ui::session_view
