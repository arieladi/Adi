// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_store.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <stdexcept>

namespace adi::engine::launch {
namespace {

[[noreturn]] void invalidColumn(const SQLite::Statement& row, int column) {
    throw std::runtime_error("invalid Session value for " + std::string(row.getColumnName(column)));
}

std::int64_t integer(const SQLite::Statement& row, int column) {
    const auto value = row.getColumn(column);
    if (value.getType() != SQLite::INTEGER)
        invalidColumn(row, column);
    return value.getInt64();
}

std::optional<std::int64_t> nullableInteger(const SQLite::Statement& row, int column) {
    return row.getColumn(column).isNull() ? std::nullopt : std::optional{integer(row, column)};
}

bool flag(const SQLite::Statement& row, int column) {
    const auto value = integer(row, column);
    if (value < 0 || value > 1)
        invalidColumn(row, column);
    return value == 1;
}

double number(const SQLite::Statement& row, int column) {
    const auto value = row.getColumn(column);
    if (value.getType() != SQLite::FLOAT && value.getType() != SQLite::INTEGER)
        invalidColumn(row, column);
    return value.getDouble();
}

template <typename Enum>
Enum code(const SQLite::Statement& row, int column, Enum last) {
    const auto value = integer(row, column);
    if (value < 0 || value > static_cast<std::int64_t>(last))
        invalidColumn(row, column);
    return static_cast<Enum>(value);
}

void followChoices(FollowSettings& follow, const SQLite::Statement& row, int first) {
    follow.first = {code(row, first, FollowAction::Jump), nullableInteger(row, first + 1)};
    follow.second = {code(row, first + 2, FollowAction::Jump), nullableInteger(row, first + 3)};
    follow.secondChancePercent = number(row, first + 4);
}

std::vector<Scene> readScenes(const SQLite::Database& db) {
    SQLite::Statement rows(db, "SELECT id,ord,name,color,tempo_bpm,tempo_enabled,signature_numerator,"
        "signature_denominator,signature_enabled,follow_enabled,follow_duration_ticks,"
        "follow_a,follow_a_scene_id,follow_b,follow_b_scene_id,follow_b_chance FROM main.scenes ORDER BY ord");
    std::vector<Scene> scenes;
    while (rows.executeStep()) {
        Scene scene;
        scene.id = integer(rows, 0);
        scene.order = integer(rows, 1);
        if (rows.getColumn(2).getType() != SQLite::TEXT)
            invalidColumn(rows, 2);
        scene.name = rows.getColumn(2).getString();
        scene.color = nullableInteger(rows, 3);
        scene.tempoBpm = number(rows, 4);
        scene.tempoEnabled = flag(rows, 5);
        scene.signatureNumerator = integer(rows, 6);
        scene.signatureDenominator = integer(rows, 7);
        scene.signatureEnabled = flag(rows, 8);
        scene.follow.enabled = flag(rows, 9);
        scene.follow.durationTicks = integer(rows, 10);
        followChoices(scene.follow, rows, 11);
        scenes.push_back(std::move(scene));
    }
    return scenes;
}

std::vector<Slot> readSlots(const SQLite::Database& db) {
    SQLite::Statement rows(db, "SELECT track_id,scene_id,clip_id,has_stop FROM main.clip_slots");
    std::vector<Slot> slots;
    while (rows.executeStep())
        slots.push_back({{integer(rows, 0), integer(rows, 1)}, nullableInteger(rows, 2), flag(rows, 3)});
    return slots;
}

std::vector<ClipLaunchSettings> readSettings(const SQLite::Database& db) {
    SQLite::Statement rows(db, "SELECT clip_id,launch_mode,launch_quant,legato,velocity_amount,"
        "follow_enabled,follow_linked,follow_loop_count,follow_duration_ticks,follow_a,follow_a_scene_id,"
        "follow_b,follow_b_scene_id,follow_b_chance,clip_fade,ram_mode FROM main.clip_launch");
    std::vector<ClipLaunchSettings> settings;
    while (rows.executeStep()) {
        ClipLaunchSettings item;
        item.clipId = integer(rows, 0);
        item.mode = code(rows, 1, LaunchMode::Repeat);
        item.quantization = integer(rows, 2);
        item.legato = flag(rows, 3);
        item.velocityAmount = number(rows, 4);
        item.follow.enabled = flag(rows, 5);
        item.follow.linked = flag(rows, 6);
        item.follow.loopCount = integer(rows, 7);
        item.follow.durationTicks = integer(rows, 8);
        followChoices(item.follow, rows, 9);
        item.clipFade = flag(rows, 14);
        item.ramMode = flag(rows, 15);
        settings.push_back(item);
    }
    return settings;
}

const char* describe(LaunchDataError error) noexcept {
    switch (error) {
    case LaunchDataError::None: return "no error";
    case LaunchDataError::InvalidTrack: return "invalid track identity";
    case LaunchDataError::DuplicateTrack: return "duplicate track identity";
    case LaunchDataError::InvalidClip: return "invalid clip identity";
    case LaunchDataError::DuplicateClip: return "duplicate clip identity";
    case LaunchDataError::InvalidScene: return "invalid scene";
    case LaunchDataError::DuplicateScene: return "duplicate scene identity";
    case LaunchDataError::DuplicateSceneOrder: return "duplicate scene order";
    case LaunchDataError::UnknownFollowScene: return "follow action names a missing scene";
    case LaunchDataError::DuplicateSlot: return "duplicate slot address";
    case LaunchDataError::DuplicateSlotClip: return "clip occupies more than one slot";
    case LaunchDataError::UnknownSlotTrack: return "slot names a missing track";
    case LaunchDataError::UnknownSlotScene: return "slot names a missing scene";
    case LaunchDataError::UnknownSlotClip: return "slot names a missing clip";
    case LaunchDataError::InvalidSlotClip: return "invalid slot clip placement";
    case LaunchDataError::DuplicateLaunchSettings: return "duplicate launch settings";
    case LaunchDataError::UnslottedLaunchSettings: return "launch settings have no slot clip";
    case LaunchDataError::InvalidLaunchSettings: return "invalid clip launch settings";
    }
    return "invalid Session data";
}

} // namespace

LaunchData readLaunchData(const Store& store, const rows::Model& model) {
    auto& db = const_cast<SQLite::Database&>(store.db());
    std::optional<SQLite::Transaction> read;
    if (sqlite3_get_autocommit(db.getHandle()) != 0)
        read.emplace(db, SQLite::TransactionBehavior::DEFERRED);

    const auto tables = db.execAndGet("SELECT count(*) FROM main.sqlite_master WHERE type='table' "
        "AND name IN ('scenes','clip_slots','clip_launch')").getInt();
    if (tables == 0 && model.slotClips.empty()) {
        if (read)
            read->commit();
        return {};
    }
    if (tables != 3)
        throw std::runtime_error("incomplete Session schema");

    std::vector<std::int64_t> tracks;
    tracks.reserve(model.tracks.size());
    for (const auto& track : model.tracks)
        tracks.push_back(track.id);
    const auto scenes = readScenes(db);
    const auto slots = readSlots(db);
    const auto settings = readSettings(db);
    LaunchDataError failure = LaunchDataError::None;
    auto data = LaunchData::prepare(tracks, model.slotClips, scenes, slots, settings, &failure);
    if (!data)
        throw std::runtime_error("could not prepare Session data: " + std::string(describe(failure)));
    for (const auto& clip : model.slotClips) {
        if (!data->findSlotForClip(clip.id))
            throw std::runtime_error("project snapshot contains a clip without its Session slot");
    }
    if (read)
        read->commit();
    return std::move(*data);
}

} // namespace adi::engine::launch
