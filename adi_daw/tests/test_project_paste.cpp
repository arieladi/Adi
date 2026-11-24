// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PD-15: unit tests for cross-project track paste (ADR-0068, ADR-0111).
//
// Proves:
//   * A track with a device, clips, automation, split pan, recording options,
//     sends, and media pasted from a second read-only Store into an active Store
//     remaps every id and commits as one undo step.
//   * Referenced media stays in place (ADR-0068 d5): zero files copied into
//     dest, abs_path_hint resolves the source file, and media already present
//     by non-empty BLAKE3 hash is reused.
//   * Empty-hash media is NEVER deduplicated and is refused cleanly.
//   * Group tracks paste with all children and group_summing; take lanes and
//     take-lane clip assignments survive; MIDI event_streams + note_expression
//     survive; internal alias links survive and external aliases warn & clear.
//   * Unmappable sends and non-default pan_law emit explicit warnings rather
//     than dropping silently.
//   * Refusals: read-only dest, missing track, master track, nested rack,
//     corrupted/hash-mismatched media, empty media hash, audio thread.
//   * Undo returns the exact start digest; source bytes are untouched; two
//     Stores open concurrently run clean under ASan/UBSan and TSan.

#include "adi/project_paste.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "adi/take_lane_ops.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace adi;
namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
        std::fflush(stdout);
    }
}

void section(const char* name) {
    std::printf("[%s]\n", name);
}

std::vector<std::uint8_t> readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool hasWarningContaining(const std::vector<std::string>& warnings, std::string_view needle) {
    for (const auto& w : warnings) {
        if (w.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Write a tiny valid 16-bit mono PCM WAV file so media::prepareImport accepts it.
void writeTestWav(const fs::path& path, std::uint32_t sampleRate, std::uint32_t frames,
                  std::int16_t sampleSeed) {
    fs::create_directories(path.parent_path());
    const std::uint16_t channels = 1U;
    const std::uint16_t bitsPerSample = 16U;
    const std::uint32_t dataBytes = frames * channels * (bitsPerSample / 8U);
    const std::uint32_t riffSize = 36U + dataBytes;
    const std::uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8U);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bitsPerSample / 8U));

    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v & 0xFFU), static_cast<char>((v >> 8U) & 0xFFU)};
        out.write(b, 2);
    };
    auto u32 = [&](std::uint32_t v) {
        const char b[4] = {
            static_cast<char>(v & 0xFFU),
            static_cast<char>((v >> 8U) & 0xFFU),
            static_cast<char>((v >> 16U) & 0xFFU),
            static_cast<char>((v >> 24U) & 0xFFU),
        };
        out.write(b, 4);
    };
    out.write("RIFF", 4);
    u32(riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16U);
    u16(1U);  // PCM
    u16(channels);
    u32(sampleRate);
    u32(byteRate);
    u16(blockAlign);
    u16(bitsPerSample);
    out.write("data", 4);
    u32(dataBytes);
    for (std::uint32_t i = 0U; i < frames; ++i) {
        const auto s = static_cast<std::int16_t>(sampleSeed + static_cast<std::int16_t>(i * 17U));
        u16(static_cast<std::uint16_t>(s));
    }
    out.close();
}

std::size_t countFilesInDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return 0U;
    }
    std::size_t count = 0U;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

struct SourceProject {
    fs::path dir;
    fs::path adiPath;
    fs::path wavPath;
    std::string wavHash;
    std::int64_t trackId = 10;
    std::int64_t returnReverbId = 11;
    std::int64_t returnDelayId = 12;
    std::int64_t sendReverbRouteId = 15;
    std::int64_t sendDelayRouteId = 16;
    std::int64_t chainId = 20;
    std::int64_t deviceId = 30;
    std::int64_t mediaId = 40;
    std::int64_t clip1Id = 50;
    std::int64_t clip2Id = 51;
    std::int64_t trackLaneId = 60;
    std::int64_t devLaneId = 61;
};

/// Build a closed source project containing one rich track: mixer settings,
/// split pan, recording options, sends (one matched, one unmatched), a device,
/// two audio clips sharing one WAV media file, and track + device automation.
SourceProject buildSourceProject(const fs::path& baseDir) {
    SourceProject src;
    src.dir = baseDir / "src_proj";
    src.adiPath = src.dir / "source.adi";
    src.wavPath = src.dir / "audio" / "kick.wav";
    writeTestWav(src.wavPath, 44100U, 256U, 100);
    src.wavHash = media::blake3File(src.wavPath).hex;

    StoreError err{};
    auto store = Store::create(src.adiPath, err);
    check(store != nullptr, "source store created");

    OpJournal journal(*store);

    // 1. Create audio track + two returns + mixer + split pan + recording options + sends.
    {
        std::vector<OpRequest> ops;
        auto push = [&](const char* type, Payload p) {
            OpRequest r;
            r.opType = type;
            r.payload = std::move(p);
            ops.push_back(std::move(r));
        };

        push("track.create",
             {{"id", src.trackId}, {"kind", "audio"}, {"name", "Drums"},
              {"parent", nullptr}, {"index", 0}});
        push("track.create",
             {{"id", src.returnReverbId}, {"kind", "return"}, {"name", "Reverb"},
              {"parent", nullptr}, {"index", 1}});
        push("track.create",
             {{"id", src.returnDelayId}, {"kind", "return"}, {"name", "OnlyInSourceDelay"},
              {"parent", nullptr}, {"index", 2}});

        push("track.setColor", {{"id", src.trackId}, {"color", 7}});
        push("mixer.setVolume", {{"id", src.trackId}, {"db", -3.5}});
        push("mixer.setPan", {{"id", src.trackId}, {"pan", -0.25}});

        // Split pan settings.
        push("mixer.setPanMode", {{"id", src.trackId}, {"mode", "split"}});
        push("mixer.setPanLeft", {{"id", src.trackId}, {"pan", -0.6}});
        push("mixer.setPanRight", {{"id", src.trackId}, {"pan", 0.8}});

        // Recording options.
        push("track.setRecordQuantize", {{"id", src.trackId}, {"on", false}, {"grid", "1/8"}});
        push("track.setPlayQuantize",
             {{"id", src.trackId}, {"on", true}, {"grid", "1/32"}, {"forgivenessMs", 15.0}});
        push("track.setKeepMonitoringLatency", {{"id", src.trackId}, {"on", false}});

        // Two sends: Reverb (exists in dest) with pre_fx tap; Delay (absent in dest).
        push("routing.connect",
             {{"id", src.sendReverbRouteId}, {"srcKind", "track"}, {"src", src.trackId},
              {"dstKind", "track"}, {"dst", src.returnReverbId}, {"kind", "send"}});
        push("routing.setGain", {{"id", src.sendReverbRouteId}, {"db", -6.0}});
        push("routing.setPan", {{"id", src.sendReverbRouteId}, {"pan", 0.25}});
        push("routing.setSendTap", {{"id", src.sendReverbRouteId}, {"tap", "pre_fx"}});

        push("routing.connect",
             {{"id", src.sendDelayRouteId}, {"srcKind", "track"}, {"src", src.trackId},
              {"dstKind", "track"}, {"dst", src.returnDelayId}, {"kind", "send"}});
        push("routing.setGain", {{"id", src.sendDelayRouteId}, {"db", -12.0}});

        check(journal.commit(ops).ok, "source track, split pan, recording options & sends committed");

        // Set non-default pan_law directly in source to prove PD-15 warns rather than dropping silently.
        SQLite::Statement pl(store->db(), "UPDATE mixer_strip SET pan_law = 2 WHERE track_id = ?");
        pl.bind(1, src.trackId);
        pl.exec();
    }

    // 2. Register plugin_ref, create chain, insert device with params, panel, route.
    {
        auto& db = store->db();
        db.exec("INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype,path_hint,is_shell,shell_id) "
                "VALUES(100,'internal','adi.utility','ADI','Utility','1','effect','',0,NULL)");
        std::vector<OpRequest> ops;
        OpRequest ch;
        ch.opType = "chain.create";
        ch.payload = {{"id", src.chainId}, {"track", src.trackId}, {"ord", 0}, {"name", "Devices"}};
        ops.push_back(std::move(ch));

        OpRequest dev;
        dev.opType = "device.insert";
        dev.payload = {
            {"id", src.deviceId},
            {"chain", src.chainId},
            {"ord", 0},
            {"ref", 100},
            {"name", "GainTrim"},
            {"enabled", true},
            {"rack", false},
            {"preset", "Warm"},
            {"latency", 0},
            {"always", false},
            {"missing", false},
            {"params", Payload::array({Payload{{"param", "0"}, {"name", "Gain"},
                                               {"norm", 0.65}, {"real", 3.0},
                                               {"display", "+3.0 dB"}, {"unit", "dB"},
                                               {"flags", 0}}})},
            {"panel", Payload::array({"0"})},
            {"route", "mpe_midi"},
        };
        ops.push_back(std::move(dev));
        check(journal.commit(ops).ok, "source device committed");
    }

    // 3. Import WAV media and create two audio clips referencing it.
    {
        const auto imported = media::importMedia(*store, src.wavPath, src.mediaId, 1700000000);
        check(imported.ok && imported.mediaId == src.mediaId, "source media imported");

        std::vector<OpRequest> ops;
        for (const auto clipId : {src.clip1Id, src.clip2Id}) {
            OpRequest cc;
            cc.opType = "clip.create";
            cc.payload = {{"id", clipId}, {"track", src.trackId}, {"kind", "audio"},
                          {"name", clipId == src.clip1Id ? "Kick A" : "Kick B"},
                          {"pos", clipId == src.clip1Id ? 0 : 960000},
                          {"length", 480000}};
            ops.push_back(std::move(cc));

            OpRequest att;
            att.opType = "clip.attachAudio";
            att.payload = {{"id", clipId}, {"media", src.mediaId}, {"frames", 256}};
            ops.push_back(std::move(att));
        }
        check(journal.commit(ops).ok, "source audio clips committed");
    }

    // 4. Create track automation lane + device automation lane with points.
    {
        automation::LaneSpec trLane;
        trLane.id = src.trackLaneId;
        trLane.ownerKind = "track";
        trLane.ownerId = src.trackId;
        trLane.paramRef = "mixer.volume";
        trLane.paramName = "Volume";
        trLane.defaultValue = 0.75;

        automation::LaneSpec devLane;
        devLane.id = src.devLaneId;
        devLane.ownerKind = "device";
        devLane.ownerId = src.deviceId;
        devLane.paramRef = "0";
        devLane.paramName = "Gain";
        devLane.defaultValue = 0.5;

        std::vector<OpRequest> ops;
        ops.push_back(automation::createLane(trLane));
        ops.push_back(automation::writePoints(
            70, src.trackLaneId, nullptr,
            Payload::array({Payload{{"id", 1}, {"time", 0}, {"value", 0.25},
                                    {"curve", 0}, {"tension", 0.0}},
                            Payload{{"id", 2}, {"time", 480000}, {"value", 0.9},
                                    {"curve", 0}, {"tension", 0.0}}})));
        ops.push_back(automation::createLane(devLane));
        ops.push_back(automation::writePoints(
            71, src.devLaneId, nullptr,
            Payload::array({Payload{{"id", 1}, {"time", 0}, {"value", 0.4},
                                    {"curve", 0}, {"tension", 0.0}}})));
        check(journal.commit(ops).ok, "source automation lanes & points committed");
    }

    check(store->close(), "source store closed cleanly");
    return src;
}

/// Create a destination project whose low row ids collide with source's ids,
/// plus a matching "Reverb" return track (id 99).
std::unique_ptr<Store> createCollidingDestProject(const fs::path& adiPath) {
    StoreError err{};
    auto dest = Store::create(adiPath, err);
    check(dest != nullptr, "dest store created");

    OpJournal journal(*dest);
    std::vector<OpRequest> ops;
    auto push = [&](const char* type, Payload p) {
        OpRequest r;
        r.opType = type;
        r.payload = std::move(p);
        ops.push_back(std::move(r));
    };

    push("track.create",
         {{"id", 10}, {"kind", "audio"}, {"name", "Existing"}, {"parent", nullptr}, {"index", 0}});
    push("track.create",
         {{"id", 99}, {"kind", "return"}, {"name", "Reverb"}, {"parent", nullptr}, {"index", 1}});
    push("chain.create", {{"id", 20}, {"track", 10}, {"ord", 0}, {"name", "ExistingChain"}});
    push("device.insert", {{"id", 30}, {"chain", 20}, {"ord", 0}, {"ref", nullptr},
                           {"name", "ExistingDev"}, {"enabled", true}, {"rack", false}});
    push("clip.create",
         {{"id", 50}, {"track", 10}, {"kind", "midi"}, {"name", "OldA"},
          {"pos", 0}, {"length", 1000}});
    push("clip.create",
         {{"id", 51}, {"track", 10}, {"kind", "midi"}, {"name", "OldB"},
          {"pos", 2000}, {"length", 1000}});

    automation::LaneSpec lane;
    lane.id = 61;
    lane.ownerKind = "track";
    lane.ownerId = 10;
    lane.paramRef = "mixer.volume";
    lane.paramName = "Volume";
    ops.push_back(automation::createLane(lane));

    check(journal.commit(ops).ok, "colliding dest baseline committed");
    return dest;
}

void testInPlaceMediaReferenceTrackTablesAndUndo() {
    section("ADR-0068 d5 in-place media reference, track tables, sends, remap, undo");

    test::TempDirectory temp{"project_paste", "core"};
    const auto src = buildSourceProject(temp.path());
    const auto srcBytesBefore = readAllBytes(src.adiPath);
    const auto wavBytesBefore = readAllBytes(src.wavPath);

    const fs::path destDir = temp.path() / "dest_proj";
    fs::create_directories(destDir);
    auto dest = createCollidingDestProject(destDir / "dest.adi");
    const std::string startDigest = digestProject(*dest).text;
    const auto headBefore = OpJournal(*dest).headSeq();

    StoreError openErr{};
    auto roSource = Store::open(src.adiPath, openErr, /*readOnly=*/true);
    check(roSource != nullptr && roSource->readOnly(), "source opened read-only");

    const auto pasted = pasteTrackFromProject(*roSource, src.trackId, *dest, Actor::User, "paste");
    check(pasted.ok, "cross-project paste succeeded: " + pasted.error);
    check(pasted.txnId > 0, "paste assigned one txnId");

    // Verify EVERY id was remapped away from colliding source ids.
    check(pasted.trackId != src.trackId, "track id remapped");
    check(pasted.chainIds.size() == 1U && pasted.chainIds.at(src.chainId) != src.chainId,
          "chain id remapped");
    check(pasted.deviceIds.size() == 1U && pasted.deviceIds.at(src.deviceId) != src.deviceId,
          "device id remapped");
    check(pasted.clipIds.size() == 2U &&
              pasted.clipIds.at(src.clip1Id) != src.clip1Id &&
              pasted.clipIds.at(src.clip2Id) != src.clip2Id,
          "clip ids remapped");
    check(pasted.laneIds.size() == 2U &&
              pasted.laneIds.at(src.trackLaneId) != src.trackLaneId &&
              pasted.laneIds.at(src.devLaneId) != src.devLaneId,
          "automation lane ids remapped");

    // ADR-0068 d5: referenced media stays in place — ZERO files copied into dest/audio/.
    check(pasted.mediaIds.size() == 1U, "one media entry mapped");
    check(pasted.referencedMediaCount == 1U && pasted.reusedMediaCount == 0U,
          "shared WAV referenced in place once");
    check(countFilesInDir(destDir / "audio") == 0U,
          "ADR-0068 d5: zero media files copied into dest audio/");

    // Verify warnings for unmapped send + non-default pan_law (no silent drops).
    check(hasWarningContaining(pasted.warnings, "pan_law"),
          "non-default pan_law reported in warnings");
    check(hasWarningContaining(pasted.warnings, "OnlyInSourceDelay"),
          "unmapped send reported in warnings");

    {
        auto& db = dest->db();

        // Media row references source file in place via abs_path_hint and resolves cleanly.
        const auto newMediaId = pasted.mediaIds.at(src.mediaId);
        const Payload mRow = media::mediaRow(db, newMediaId);
        check(mRow.at("rel_path").is_null(), "dest media row rel_path is NULL");
        check(!mRow.at("abs_path_hint").is_null() && mRow.at("missing").get<int>() == 0,
              "dest media row carries abs_path_hint and missing=0");
        std::string resErr;
        const fs::path resolved = media::resolveMedia(destDir, mRow, resErr);
        check(!resolved.empty() && fs::equivalent(resolved, src.wavPath),
              "dest resolves referenced media in place at source path");

        // Split pan preserved.
        SQLite::Statement sp(
            db, "SELECT mode, pan_left, pan_right FROM mixer_pan_settings WHERE track_id=?");
        sp.bind(1, pasted.trackId);
        check(sp.executeStep() && sp.getColumn(0).getString() == "split" &&
                  sp.getColumn(1).getDouble() == -0.6 && sp.getColumn(2).getDouble() == 0.8,
              "mixer_pan_settings (split pan) preserved");

        // Recording options preserved.
        SQLite::Statement ro(
            db,
            "SELECT record_enabled, record_grid, play_enabled, play_grid, "
            "play_forgiveness_ms, keep_monitoring_latency "
            "FROM track_recording_options WHERE track_id=?");
        ro.bind(1, pasted.trackId);
        check(ro.executeStep() && ro.getColumn(0).getInt() == 0 &&
                  ro.getColumn(1).getString() == "1/8" && ro.getColumn(2).getInt() == 1 &&
                  ro.getColumn(3).getString() == "1/32" &&
                  ro.getColumn(4).getDouble() == 15.0 && ro.getColumn(5).getInt() == 0,
              "track_recording_options preserved");

        // Matched send to "Reverb" (dest track 99) + individual pre_fx tap preserved.
        check(pasted.routingIds.size() == 1U &&
                  pasted.routingIds.contains(src.sendReverbRouteId),
              "matched Reverb send mapped");
        const auto newRouteId = pasted.routingIds.at(src.sendReverbRouteId);
        SQLite::Statement rq(
            db, "SELECT src_id, dst_id, gain_db, pan FROM routing WHERE id=? AND kind='send'");
        rq.bind(1, newRouteId);
        check(rq.executeStep() && rq.getColumn(0).getInt64() == pasted.trackId &&
                  rq.getColumn(1).getInt64() == 99 && rq.getColumn(2).getDouble() == -6.0 &&
                  rq.getColumn(3).getDouble() == 0.25,
              "pasted send targets dest Reverb return with gain & pan");

        SQLite::Statement stq(db, "SELECT tap FROM routing_send_taps WHERE routing_id=?");
        stq.bind(1, newRouteId);
        check(stq.executeStep() && stq.getColumn(0).getString() == "pre_fx",
              "pasted send individual tap (pre_fx) preserved");
    }

    // One undo returns exact start digest; redo restores pasted track.
    {
        History hist(*dest);
        const auto step = hist.nextUndo();
        check(step.has_value() && step->txnId == pasted.txnId, "paste is top single undo step");

        const auto undone = hist.undo();
        check(undone.ok && undone.txnId == pasted.txnId, "single undo succeeded");
        check(hist.head() == headBefore, "head returned to pre-paste seq");
        check(digestProject(*dest).text == startDigest, "undo returned exact start digest");

        const auto redone = hist.redo();
        check(redone.ok && redone.txnId == pasted.txnId, "redo restored pasted track");
    }

    check(roSource->close(), "read-only source closed");
    check(readAllBytes(src.adiPath) == srcBytesBefore, "source .adi file bytes unchanged");
    check(readAllBytes(src.wavPath) == wavBytesBefore, "source audio file bytes unchanged");
}

void testMediaHashDedupAndEmptyHashRefusal() {
    section("media dedup by non-empty hash & refusal of empty hash");

    test::TempDirectory temp{"project_paste", "dedup_and_empty_hash"};
    const auto src = buildSourceProject(temp.path());

    const fs::path destDir = temp.path() / "dest_dedup";
    const fs::path destWav = destDir / "audio" / "existing_kick.wav";
    fs::create_directories(destWav.parent_path());
    fs::copy_file(src.wavPath, destWav);

    StoreError err{};
    auto dest = Store::create(destDir / "dest.adi", err);
    check(dest != nullptr, "dedup dest created");

    const auto pre = media::importMedia(*dest, destWav, 7, 1700000100);
    check(pre.ok && pre.mediaId == 7, "pre-existing media imported into dest");
    const std::string startDigest = digestProject(*dest).text;

    auto roSource = Store::open(src.adiPath, err, /*readOnly=*/true);
    const auto pasted = pasteTrackFromProject(*roSource, src.trackId, *dest);
    check(pasted.ok, "paste with pre-existing media succeeded: " + pasted.error);
    check(pasted.referencedMediaCount == 0U && pasted.reusedMediaCount == 1U,
          "media already present by non-empty hash was reused");
    check(pasted.mediaIds.at(src.mediaId) == 7, "reused existing dest media id 7");
    check(History(*dest).undo().ok && digestProject(*dest).text == startDigest,
          "dedup undo returns start digest");
    check(roSource->close(), "closed roSource");

    // Empty-hash refusal: even if dest ALSO contains an empty-hash row, paste MUST
    // refuse rather than deduplicating on "".
    {
        auto emptySrc = Store::create(temp.path() / "empty_hash_src.adi", err);
        auto emptyDest = Store::create(temp.path() / "empty_hash_dest.adi", err);
        check(emptySrc && emptyDest, "empty-hash test stores created");

        emptySrc->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','T',0);"
            "INSERT INTO mixer_strip(track_id) VALUES(1);"
            "INSERT INTO media_files(id,hash_blake3,orig_name,embedded,missing) VALUES(5,'','bad.wav',0,0);"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES(10,1,'audio','C',0,1000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(10,5,0,100);");
        emptyDest->db().exec(
            "INSERT INTO media_files(id,hash_blake3,orig_name,embedded,missing) VALUES(99,'','other.wav',0,0);");
        const std::string beforeDigest = digestProject(*emptyDest).text;

        const auto res = pasteTrackFromProject(*emptySrc, 1, *emptyDest);
        check(!res.ok && res.error.find("hash") != std::string::npos,
              "empty-hash source media refused cleanly (never deduplicated on '')");
        check(digestProject(*emptyDest).text == beforeDigest,
              "dest untouched on empty-hash refusal");
    }
}

void testMidiTakeLanesGroupSubtreeAndAliases() {
    section("MIDI clips, take lanes, group subtree + summing, internal & external aliases");

    test::TempDirectory temp{"project_paste", "group_midi_lanes"};
    const fs::path srcDir = temp.path() / "src_rich";
    const fs::path wavPath = srcDir / "audio" / "snare.wav";
    writeTestWav(wavPath, 44100U, 128U, 55);

    StoreError err{};
    fs::create_directories(temp.path() / "dest_rich");
    auto srcStore = Store::create(srcDir / "source.adi", err);
    auto destStore = Store::create(temp.path() / "dest_rich" / "dest.adi", err);
    check(srcStore && destStore, "rich stores created");

    OpJournal sj(*srcStore);
    {
        std::vector<OpRequest> ops;
        auto push = [&](const char* type, Payload p) {
            OpRequest r;
            r.opType = type;
            r.payload = std::move(p);
            ops.push_back(std::move(r));
        };

        // Group track 100 with group_summing + two children (audio 101, midi 102) + external track 200.
        push("track.create",
             {{"id", 100}, {"kind", "group"}, {"name", "Band"}, {"parent", nullptr}, {"index", 0}});
        push("group.setSummingFlavor", {{"id", 100}, {"flavor", "console9"}});
        push("group.setSummingDrive", {{"id", 100}, {"db", 3.5}});
        push("group.setSumming", {{"id", 100}, {"enabled", true}});

        push("track.create",
             {{"id", 101}, {"kind", "audio"}, {"name", "Snare"}, {"parent", 100}, {"index", 0}});
        push("track.create",
             {{"id", 102}, {"kind", "midi"}, {"name", "Keys"}, {"parent", 100}, {"index", 1}});
        push("track.create",
             {{"id", 200}, {"kind", "midi"}, {"name", "External"}, {"parent", nullptr}, {"index", 1}});

        // Two take lanes on audio track 101: create Lane B first then Lane A (lane.create inserts at ord=0).
        ops.push_back(createTakeLane(*srcStore, 302, 101, "Take 2"));
        ops.push_back(createTakeLane(*srcStore, 301, 101, "Take 1"));
        push("lane.setMute", {{"id", 301}, {"on", true}});

        check(sj.commit(ops).ok, "group + children + take lanes committed");
    }

    // Import snare.wav and place 3 clips on track 101: main lane, Take 1, Take 2.
    check(media::importMedia(*srcStore, wavPath, 401, 1700000200).ok, "snare media imported");
    {
        std::vector<OpRequest> ops;
        auto push = [&](const char* type, Payload p) {
            OpRequest r;
            r.opType = type;
            r.payload = std::move(p);
            ops.push_back(std::move(r));
        };

        push("clip.create", {{"id", 501}, {"track", 101}, {"kind", "audio"}, {"name", "Main"},
                             {"pos", 0}, {"length", 100000}});
        push("clip.attachAudio", {{"id", 501}, {"media", 401}, {"frames", 128}});

        push("clip.create", {{"id", 502}, {"track", 101}, {"kind", "audio"}, {"name", "T1"},
                             {"pos", 0}, {"length", 100000}});
        push("clip.attachAudio", {{"id", 502}, {"media", 401}, {"frames", 128}});
        push("clip.setLane", {{"id", 502}, {"lane", 301}});

        push("clip.create", {{"id", 503}, {"track", 101}, {"kind", "audio"}, {"name", "T2"},
                             {"pos", 0}, {"length", 100000}});
        push("clip.attachAudio", {{"id", 503}, {"media", 401}, {"frames", 128}});
        push("clip.setLane", {{"id", 503}, {"lane", 302}});

        // MIDI clips: master clip 601 on track 102, internal alias 602 on track 102,
        // external master 701 on track 200, external alias 603 on track 102.
        push("clip.create", {{"id", 601}, {"track", 102}, {"kind", "midi"}, {"name", "Chord"},
                             {"pos", 0}, {"length", 200000}});
        push("clip.create", {{"id", 602}, {"track", 102}, {"kind", "midi"}, {"name", "ChordAlias"},
                             {"pos", 200000}, {"length", 200000}});
        push("clip.create", {{"id", 701}, {"track", 200}, {"kind", "midi"}, {"name", "ExtMaster"},
                             {"pos", 0}, {"length", 100000}});
        push("clip.create", {{"id", 603}, {"track", 102}, {"kind", "midi"}, {"name", "ExtAlias"},
                             {"pos", 400000}, {"length", 100000}});
        check(sj.commit(ops).ok, "audio take-lane & MIDI clips committed");

        // Attach event_streams, note_expression, and alias_of links on source.
        {
            const std::vector<std::uint8_t> notesBlob = {0x41, 0x4E, 0x4F, 0x54, 1, 2, 3, 4,
                                                         0,    0,    0,    0,    0, 0, 0, 0};
            SQLite::Statement es(
                srcStore->db(),
                "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(801,601,'notes',?)");
            es.bind(1, notesBlob.data(), static_cast<int>(notesBlob.size()));
            es.exec();

            SQLite::Statement ne(
                srcStore->db(),
                "INSERT INTO note_expression(id,clip_id,note_id,dimension,data) "
                "VALUES(802,601,1,1,?)");
            ne.bind(1, notesBlob.data(), static_cast<int>(notesBlob.size()));
            ne.exec();

            srcStore->db().exec("UPDATE clips SET alias_of = 601 WHERE id = 602;");
            srcStore->db().exec("UPDATE clips SET alias_of = 701 WHERE id = 603;");
        }
    }

    const std::string startDigest = digestProject(*destStore).text;
    const auto pasted = pasteTrackFromProject(*srcStore, 100, *destStore);
    check(pasted.ok, "group subtree paste succeeded: " + pasted.error);
    check(pasted.trackIds.size() == 3U, "group + 2 child tracks all pasted");
    check(pasted.takeLaneIds.size() == 2U, "both take lanes pasted");
    check(pasted.clipIds.size() == 6U, "all 6 clips across child tracks pasted");
    check(hasWarningContaining(pasted.warnings, "alias_of external clip 701"),
          "external alias unlink reported in warnings");

    {
        auto& db = destStore->db();
        const auto newGroupId = pasted.trackIds.at(100);
        const auto newAudioId = pasted.trackIds.at(101);
        const auto newMidiId = pasted.trackIds.at(102);

        // Parent/child hierarchy preserved.
        SQLite::Statement pq(db, "SELECT parent_id, index_in_parent FROM tracks WHERE id=?");
        pq.bind(1, newAudioId);
        check(pq.executeStep() && pq.getColumn(0).getInt64() == newGroupId &&
                  pq.getColumn(1).getInt64() == 0,
              "audio child parented under pasted group at index 0");
        pq.reset();
        pq.bind(1, newMidiId);
        check(pq.executeStep() && pq.getColumn(0).getInt64() == newGroupId &&
                  pq.getColumn(1).getInt64() == 1,
              "midi child parented under pasted group at index 1");

        // Group summing preserved.
        SQLite::Statement gs(
            db, "SELECT enabled, flavor, drive_db FROM group_summing WHERE track_id=?");
        gs.bind(1, newGroupId);
        check(gs.executeStep() && gs.getColumn(0).getInt() == 1 &&
                  gs.getColumn(1).getString() == "console9" && gs.getColumn(2).getDouble() == 3.5,
              "group_summing preserved");

        // Take lanes preserved in exact order & mute state, and clips stay on their take lanes!
        const auto newLane1 = pasted.takeLaneIds.at(301);
        const auto newLane2 = pasted.takeLaneIds.at(302);
        SQLite::Statement lq(db, "SELECT ord, name, muted FROM lanes WHERE id=?");
        lq.bind(1, newLane1);
        check(lq.executeStep() && lq.getColumn(0).getInt() == 0 &&
                  lq.getColumn(1).getString() == "Take 1" && lq.getColumn(2).getInt() == 1,
              "Take 1 lane preserved at ord=0, muted=1");
        lq.reset();
        lq.bind(1, newLane2);
        check(lq.executeStep() && lq.getColumn(0).getInt() == 1 &&
                  lq.getColumn(1).getString() == "Take 2" && lq.getColumn(2).getInt() == 0,
              "Take 2 lane preserved at ord=1, muted=0");

        SQLite::Statement cq(db, "SELECT lane_id FROM clips WHERE id=?");
        cq.bind(1, pasted.clipIds.at(501));
        check(cq.executeStep() && cq.getColumn(0).isNull(), "main-lane clip stays on main lane");
        cq.reset();
        cq.bind(1, pasted.clipIds.at(502));
        check(cq.executeStep() && cq.getColumn(0).getInt64() == newLane1,
              "Take 1 clip stays on Take 1 lane (not flattened to main)");
        cq.reset();
        cq.bind(1, pasted.clipIds.at(503));
        check(cq.executeStep() && cq.getColumn(0).getInt64() == newLane2,
              "Take 2 clip stays on Take 2 lane");

        // MIDI event_streams + note_expression preserved.
        const auto newMidiMaster = pasted.clipIds.at(601);
        SQLite::Statement es(db, "SELECT COUNT(*) FROM event_streams WHERE clip_id=? AND stream_kind='notes'");
        es.bind(1, newMidiMaster);
        check(es.executeStep() && es.getColumn(0).getInt() == 1, "MIDI event_streams preserved");

        SQLite::Statement ne(db, "SELECT COUNT(*) FROM note_expression WHERE clip_id=? AND note_id=1 AND dimension=1");
        ne.bind(1, newMidiMaster);
        check(ne.executeStep() && ne.getColumn(0).getInt() == 1, "MIDI note_expression preserved");

        // Internal alias preserved with remapped id; external alias unlinked cleanly.
        SQLite::Statement aq(db, "SELECT alias_of FROM clips WHERE id=?");
        aq.bind(1, pasted.clipIds.at(602));
        check(aq.executeStep() && aq.getColumn(0).getInt64() == newMidiMaster,
              "internal alias_of link preserved with remapped clip id");
        aq.reset();
        aq.bind(1, pasted.clipIds.at(603));
        check(aq.executeStep() && aq.getColumn(0).isNull(),
              "external alias_of cleared cleanly");
    }

    // One undo returns exact start digest.
    check(History(*destStore).undo().ok, "single undo of whole group paste succeeded");
    check(digestProject(*destStore).text == startDigest,
          "group paste undo returned exact start digest");
}

void testRefusalsAndStateBlobs() {
    section("refusals (RO, missing, master, nested rack, hash mismatch, audio thread) & state_blobs");

    test::TempDirectory temp{"project_paste", "refusals"};
    const auto src = buildSourceProject(temp.path());

    // Add a state_blobs entry to source's device so we also test state blob transport.
    std::string blobHash;
    {
        StoreError err{};
        auto rwSrc = Store::open(src.adiPath, err, false);
        check(rwSrc != nullptr, "reopen source rw to add state blob");
        const std::vector<std::byte> payload = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
        blobHash = media::blake3Bytes(payload).hex;
        {
            SQLite::Statement ins(rwSrc->db(),
                                  "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
            ins.bind(1, blobHash);
            ins.bind(2, payload.data(), static_cast<int>(payload.size()));
            ins.bind(3, static_cast<std::int64_t>(payload.size()));
            ins.exec();
            SQLite::Statement ps(rwSrc->db(),
                                 "INSERT INTO plugin_state(device_id,stream_role,state_hash,format_hint) "
                                 "VALUES(?,'component',?,'vst3')");
            ps.bind(1, src.deviceId);
            ps.bind(2, blobHash);
            ps.exec();
        }
        // Add a master track (id 900) and a track with a nested rack (id 901) for refusal checks.
        rwSrc->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(900,'master','Main',90);"
            "INSERT INTO mixer_strip(track_id) VALUES(900);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(901,'audio','RackTrk',91);"
            "INSERT INTO mixer_strip(track_id) VALUES(901);"
            "INSERT INTO device_chains(id,track_id,ord,name) VALUES(910,901,0,'Main');"
            "INSERT INTO devices(id,chain_id,ord,name,is_rack) VALUES(920,910,0,'Rack',1);"
            "INSERT INTO device_chains(id,parent_device_id,ord,name) VALUES(911,920,0,'Sub');");
        check(rwSrc->close(), "closed source with state blob + refusal fixtures");
    }

    const fs::path destDir = temp.path() / "dest_ro";
    fs::create_directories(destDir);
    {
        StoreError err{};
        auto tmpDest = Store::create(destDir / "dest.adi", err);
        check(tmpDest != nullptr && tmpDest->close(), "created dest for refusal checks");
    }

    StoreError err{};
    auto roSource = Store::open(src.adiPath, err, true);
    auto roDest = Store::open(destDir / "dest.adi", err, true);
    check(roSource != nullptr && roDest != nullptr && roDest->readOnly(),
          "both stores open read-only");

    // 1. Read-only destination refused.
    check(!pasteTrackFromProject(*roSource, src.trackId, *roDest).ok,
          "paste into read-only destination refused");
    check(roDest->close(), "closed read-only dest");

    auto rwDest = Store::open(destDir / "dest.adi", err, false);
    check(rwDest != nullptr, "reopened dest writable");

    // 2. Non-existent track id refused.
    check(!pasteTrackFromProject(*roSource, 99999, *rwDest).ok,
          "non-existent source track refused");

    // 3. Master track refused.
    const auto masterRef = pasteTrackFromProject(*roSource, 900, *rwDest);
    check(!masterRef.ok && masterRef.error.find("master") != std::string::npos,
          "master track refused");

    // 4. Nested rack chain refused.
    const auto rackRef = pasteTrackFromProject(*roSource, 901, *rwDest);
    check(!rackRef.ok && rackRef.error.find("nested rack") != std::string::npos,
          "nested rack chain refused");

    // 5. Audio thread refused.
    {
        audio::onAudioThread = true;
        const auto audioRef = pasteTrackFromProject(*roSource, src.trackId, *rwDest);
        audio::onAudioThread = false;
        check(!audioRef.ok && audioRef.error.find("audio") != std::string::npos,
              "paste on audio thread refused");
    }

    // 6. State blobs copied once and reused on second paste (with in-place media reuse too).
    const auto first = pasteTrackFromProject(*roSource, src.trackId, *rwDest);
    check(first.ok, "first paste with state blob succeeded: " + first.error);
    check(first.copiedStateBlobCount == 1U && first.reusedStateBlobCount == 0U,
          "first paste copied state blob once");
    check(rwDest->getStateBlob(blobHash).has_value(), "state blob present in dest");

    const auto second = pasteTrackFromProject(*roSource, src.trackId, *rwDest);
    check(second.ok, "second paste reuses both media and state blob: " + second.error);
    check(second.referencedMediaCount == 0U && second.reusedMediaCount == 1U,
          "second paste reused media by hash");
    check(second.copiedStateBlobCount == 0U && second.reusedStateBlobCount == 1U,
          "second paste reused state blob by hash");

    // 7. Hash-mismatched media on disk refused cleanly.
    {
        fs::create_directories(temp.path() / "dest_mismatch");
        auto mismatchDest = Store::create(temp.path() / "dest_mismatch" / "dest.adi", err);
        writeTestWav(src.wavPath, 44100U, 256U, 9999);  // overwrite source wav with different bytes
        const auto mm = pasteTrackFromProject(*roSource, src.trackId, *mismatchDest);
        check(!mm.ok && mm.error.find("hash mismatch") != std::string::npos,
              "corrupted/hash-mismatched media on disk refused");
    }
}

void testTwoOpenStoresConcurrentlyForTsanAndAsan() {
    section("two Stores open concurrently (ASan + TSan)");

    test::TempDirectory temp{"project_paste", "concurrent"};
    const auto src = buildSourceProject(temp.path());
    const fs::path destDirA = temp.path() / "dest_a";
    const fs::path destDirB = temp.path() / "dest_b";
    fs::create_directories(destDirA);
    fs::create_directories(destDirB);

    StoreError errA{}, errB{}, errS1{}, errS2{};
    auto destA = Store::create(destDirA / "a.adi", errA);
    auto destB = Store::create(destDirB / "b.adi", errB);
    auto roSrc1 = Store::open(src.adiPath, errS1, true);
    auto roSrc2 = Store::open(src.adiPath, errS2, true);
    check(destA && destB && roSrc1 && roSrc2, "all four stores opened");

    bool okA = false;
    bool okB = false;
    std::thread t1([&]() {
        const auto r = pasteTrackFromProject(*roSrc1, src.trackId, *destA);
        okA = r.ok && r.referencedMediaCount == 1U && History(*destA).undo().ok;
    });
    std::thread t2([&]() {
        const auto r = pasteTrackFromProject(*roSrc2, src.trackId, *destB);
        okB = r.ok && r.referencedMediaCount == 1U && History(*destB).undo().ok;
    });
    t1.join();
    t2.join();

    check(okA && okB, "concurrent pastes across separate open Stores succeeded cleanly");
}

}  // namespace

int main() {
    testInPlaceMediaReferenceTrackTablesAndUndo();
    testMediaHashDedupAndEmptyHashRefusal();
    testMidiTakeLanesGroupSubtreeAndAliases();
    testRefusalsAndStateBlobs();
    testTwoOpenStoresConcurrentlyForTsanAndAsan();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
