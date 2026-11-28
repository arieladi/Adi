// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_store.hpp"
#include "adi/store.hpp"

#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace {
using namespace adi;
using namespace adi::engine;
using namespace adi::engine::launch;

int checks = 0, failures = 0;

void check(bool result, const char* description) {
    ++checks;
    if (!result) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

bool refuses(const Store& store, const rows::Model& model, std::string_view diagnostic) {
    try {
        (void)readLaunchData(store, model);
    } catch (const std::exception& error) {
        return std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
    }
    return false;
}

void legacy() {
    test::TempDirectory root("launch-store", "legacy");
    StoreError error;
    auto store = Store::createEmpty(root.path() / "legacy.adi", error);
    check(store && error == StoreError::Ok, "legacy storage opened");
    if (!store)
        return;
    store->db().exec("INSERT INTO tracks(id,kind) VALUES(1,'midi');"
                     "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(2,1,'midi',0,100);");
    const auto model = rows::readModel(*store);
    const auto data = readLaunchData(*store, model);
    check(data.scenes().empty() && data.slots().empty() && data.settingsRows().empty() &&
          !data.findSlotForClip(2), "ordinary Arrangement opens with an empty Session");
    check(model.slotClips.empty() && model.clips.size() == 1 && model.clips.front().id == 2,
          "an Arrangement clip at zero stays in the Arrangement");

    if constexpr (kUserVersion < kSessionSchemaUserVersion) {
        store->db().exec("CREATE TABLE scenes(id INTEGER PRIMARY KEY) STRICT");
        check(refuses(*store, model, "incomplete Session schema"), "partial staging reports a named error");
    }
}

void staged() {
    test::TempDirectory root("launch-store", "staged");
    const auto file = root.path() / "staged.adi";
    StoreError error;
    auto store = Store::createEmpty(file, error);
    check(store && error == StoreError::Ok, "staged storage opened");
    if (!store)
        return;
    test::applySessionSchema(store->db());
    store->db().exec(
        "INSERT INTO tracks(id,kind) VALUES(10,'midi'),(20,'audio');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
        "VALUES(100,10,'midi',0,400),(101,10,'midi',0,500),(102,20,'audio',0,900);"
        "INSERT INTO scenes(id,ord,name,tempo_bpm,tempo_enabled) VALUES(40,1,'Verse',117,1);"
        "INSERT INTO scenes(id,ord,name,color,tempo_bpm,signature_numerator,signature_denominator,"
        "signature_enabled,follow_enabled,follow_duration_ticks,follow_a,follow_a_scene_id,"
        "follow_b,follow_b_chance) VALUES(50,0,'Intro',12,128.25,7,8,1,1,807,9,40,1,33);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) "
        "VALUES(10,50,100,1),(20,50,102,1),(20,40,NULL,0);"
        "INSERT INTO clip_launch(clip_id,launch_mode,launch_quant,legato,velocity_amount,"
        "follow_enabled,follow_linked,follow_loop_count,follow_duration_ticks,follow_a,"
        "follow_b,follow_b_scene_id,follow_b_chance,clip_fade,ram_mode) "
        "VALUES(100,2,11,1,0.625,1,0,3,912,4,9,40,41,1,1);");
    test::seedSessionMarker(store->db());

    LaunchData first;
    {
        SQLite::Transaction snapshot(store->db(), SQLite::TransactionBehavior::DEFERRED);
        const auto model = rows::readModel(*store);
        first = readLaunchData(*store, model);
        check(sqlite3_get_autocommit(store->db().getHandle()) == 0,
              "Session read borrows the caller's project snapshot");
        check(model.clips.size() == 1 && model.clips.front().id == 101 && model.slotClips.size() == 2,
              "stored clip ownership reaches the typed model");
        snapshot.commit();
    }
    check(first.scenes().size() == 2 && first.scenes()[0].id == 50 && first.scenes()[1].id == 40,
          "SQL scene order follows ordinal instead of ID");
    const Scene expected{
        .id = 50, .order = 0, .name = "Intro", .color = 12, .tempoBpm = 128.25,
        .signatureNumerator = 7, .signatureDenominator = 8, .signatureEnabled = true,
        .follow = {.enabled = true, .linked = false, .durationTicks = 807,
                   .first = {FollowAction::Jump, 40}, .second = {FollowAction::Stop, std::nullopt},
                   .secondChancePercent = 33},
    };
    check(first.findScene(50) && *first.findScene(50) == expected &&
          first.findScene(40) && first.findScene(40)->tempoEnabled,
          "scene read retains dormant values, independent enable flags and Jump IDs");
    const ClipLaunchSettings authored{
        .clipId = 100, .mode = LaunchMode::Toggle, .quantization = 11, .legato = true,
        .velocityAmount = 0.625,
        .follow = {.enabled = true, .linked = false, .loopCount = 3, .durationTicks = 912,
                   .first = {FollowAction::Next, std::nullopt}, .second = {FollowAction::Jump, 40},
                   .secondChancePercent = 41},
        .clipFade = true, .ramMode = true,
    };
    check(first.settings(100) == authored && first.settings(102) == ClipLaunchSettings{.clipId = 102},
          "every authored launch setting and the absent-row defaults are projected");
    check(!first.settings(101) && first.slots().size() == 3 &&
          first.slot({20, 40}) == Slot{{20, 40}, std::nullopt, false} &&
          first.slot({10, 40}) == Slot{{10, 40}, std::nullopt, true},
          "sparse empty cells preserve distinct Stop state");

    auto stale = rows::readModel(*store);
    stale.slotClips.clear();
    check(refuses(*store, stale, "missing clip"), "a mismatched projection cannot publish");
    store->db().exec("PRAGMA ignore_check_constraints=ON;"
                     "UPDATE clip_launch SET launch_mode=256 WHERE clip_id=100;");
    check(refuses(*store, rows::readModel(*store), "launch_mode"),
          "invalid stored enum refuses before narrowing to the launch type");
    store->db().exec("UPDATE clip_launch SET launch_mode=2 WHERE clip_id=100;"
                     "PRAGMA ignore_check_constraints=OFF;");
    check(store->close(), "staged fixture closes");

    auto reopened = Store::open(file, error, true);
    check(reopened && error == StoreError::Ok, "staged fixture reopens read-only");
    if (!reopened)
        return;
    const auto data = readLaunchData(*reopened, rows::readModel(*reopened));
    check(std::ranges::equal(first.scenes(), data.scenes()) &&
          std::ranges::equal(first.slots(), data.slots()) &&
          std::ranges::equal(first.settingsRows(), data.settingsRows()),
          "reopen preserves exact ordered scene, slot and settings data");
}
} // namespace

int main() {
    try {
        legacy();
        staged();
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        return 1;
    }
    std::printf("launch store: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
