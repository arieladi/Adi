// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PD-15: unit tests for cross-project track paste (ADR-0068, ADR-0111).
//
// Proves:
//   * A track with a device, clips, automation and media pasted from a second
//     read-only Store into an active Store remaps every id and commits as one
//     undo step.
//   * Undo returns the exact start digest; redo restores the pasted track.
//   * The source .adi file bytes (and source audio bytes) are untouched.
//   * A media file already present by hash in dest is reused and not copied
//     again (and two clips referencing the same media copy it once).
//   * A paste into a read-only project is refused cleanly.
//   * Two Stores open simultaneously run cleanly under ASan and TSan.

#include "adi/project_paste.hpp"

#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
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
    std::int64_t chainId = 20;
    std::int64_t deviceId = 30;
    std::int64_t mediaId = 40;
    std::int64_t clip1Id = 50;
    std::int64_t clip2Id = 51;
    std::int64_t trackLaneId = 60;
    std::int64_t devLaneId = 61;
};

/// Build a closed source project containing one rich track: mixer settings,
/// a device with parameters & panel & route, two audio clips sharing one WAV
/// media file, track automation, and device automation.
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

    // 1. Create track + non-default mixer strip.
    {
        std::vector<OpRequest> ops;
        OpRequest tr;
        tr.opType = "track.create";
        tr.payload = {{"id", src.trackId}, {"kind", "audio"}, {"name", "Drums"},
                      {"parent", nullptr}, {"index", 0}};
        ops.push_back(std::move(tr));

        OpRequest col;
        col.opType = "track.setColor";
        col.payload = {{"id", src.trackId}, {"color", 7}};
        ops.push_back(std::move(col));

        OpRequest vol;
        vol.opType = "mixer.setVolume";
        vol.payload = {{"id", src.trackId}, {"db", -3.5}};
        ops.push_back(std::move(vol));

        OpRequest pan;
        pan.opType = "mixer.setPan";
        pan.payload = {{"id", src.trackId}, {"pan", -0.25}};
        ops.push_back(std::move(pan));

        check(journal.commit(ops).ok, "source track + mixer committed");
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

/// Create a destination project whose low row ids collide with the source
/// project's ids so every remapped id is proved distinct.
std::unique_ptr<Store> createCollidingDestProject(const fs::path& adiPath) {
    StoreError err{};
    auto dest = Store::create(adiPath, err);
    check(dest != nullptr, "dest store created");

    // Pre-occupy the exact IDs the source project uses (10, 20, 30, 40, 50, 51, 60, 61, 70, 71).
    OpJournal journal(*dest);
    std::vector<OpRequest> ops;
    OpRequest tr;
    tr.opType = "track.create";
    tr.payload = {{"id", 10}, {"kind", "audio"}, {"name", "Existing"},
                  {"parent", nullptr}, {"index", 0}};
    ops.push_back(std::move(tr));

    OpRequest ch;
    ch.opType = "chain.create";
    ch.payload = {{"id", 20}, {"track", 10}, {"ord", 0}, {"name", "ExistingChain"}};
    ops.push_back(std::move(ch));

    OpRequest dev;
    dev.opType = "device.insert";
    dev.payload = {{"id", 30}, {"chain", 20}, {"ord", 0}, {"ref", nullptr},
                   {"name", "ExistingDev"}, {"enabled", true}, {"rack", false}};
    ops.push_back(std::move(dev));

    OpRequest cl1;
    cl1.opType = "clip.create";
    cl1.payload = {{"id", 50}, {"track", 10}, {"kind", "midi"}, {"name", "OldA"},
                   {"pos", 0}, {"length", 1000}};
    ops.push_back(std::move(cl1));

    OpRequest cl2;
    cl2.opType = "clip.create";
    cl2.payload = {{"id", 51}, {"track", 10}, {"kind", "midi"}, {"name", "OldB"},
                   {"pos", 2000}, {"length", 1000}};
    ops.push_back(std::move(cl2));

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

void testCrossProjectPasteRemapUndoAndSourceImmutability() {
    section("cross-project paste: remap, one undo step, start digest, source untouched");

    test::TempDirectory temp{"project_paste", "core"};
    const auto src = buildSourceProject(temp.path());
    const auto srcBytesBefore = readAllBytes(src.adiPath);
    const auto wavBytesBefore = readAllBytes(src.wavPath);

    const fs::path destDir = temp.path() / "dest_proj";
    fs::create_directories(destDir);
    auto dest = createCollidingDestProject(destDir / "dest.adi");
    const std::string startDigest = digestProject(*dest).text;
    const auto headBefore = OpJournal(*dest).headSeq();

    // Open source strictly read-only.
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
    check(pasted.mediaIds.size() == 1U, "one media entry mapped");
    check(pasted.copiedMediaCount == 1U && pasted.reusedMediaCount == 0U,
          "shared WAV referenced by 2 clips copied exactly once");
    check(countFilesInDir(destDir / "audio") == 1U, "dest audio/ has exactly 1 copied file");

    // Verify pasted track, mixer, device, clips, media, and automation rows in dest.
    {
        auto& db = dest->db();
        SQLite::Statement tr(db, "SELECT name, color FROM tracks WHERE id=?");
        tr.bind(1, pasted.trackId);
        check(tr.executeStep() && tr.getColumn(0).getString() == "Drums" &&
                  tr.getColumn(1).getInt() == 7,
              "pasted track name & color preserved");

        SQLite::Statement mx(db, "SELECT volume_db, pan FROM mixer_strip WHERE track_id=?");
        mx.bind(1, pasted.trackId);
        check(mx.executeStep() && mx.getColumn(0).getDouble() == -3.5 &&
                  mx.getColumn(1).getDouble() == -0.25,
              "pasted mixer volume & pan preserved");

        const auto newDevId = pasted.deviceIds.at(src.deviceId);
        SQLite::Statement dv(db, "SELECT name, preset_name FROM devices WHERE id=?");
        dv.bind(1, newDevId);
        check(dv.executeStep() && dv.getColumn(0).getString() == "GainTrim" &&
                  dv.getColumn(1).getString() == "Warm",
              "pasted device name & preset preserved");

        SQLite::Statement prm(db, "SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id='0'");
        prm.bind(1, newDevId);
        check(prm.executeStep() && prm.getColumn(0).getDouble() == 0.65,
              "pasted device param preserved");

        const auto newMediaId = pasted.mediaIds.at(src.mediaId);
        SQLite::Statement ac(db, "SELECT COUNT(*) FROM audio_clips WHERE media_id=?");
        ac.bind(1, newMediaId);
        check(ac.executeStep() && ac.getColumn(0).getInt() == 2,
              "both pasted audio clips point to the single remapped media id");

        // Verify device automation lane now points to newDevId, not src.deviceId.
        const auto newDevLaneId = pasted.laneIds.at(src.devLaneId);
        SQLite::Statement ln(db, "SELECT owner_kind, owner_id, param_ref FROM automation_lanes WHERE id=?");
        ln.bind(1, newDevLaneId);
        check(ln.executeStep() && ln.getColumn(0).getString() == "device" &&
                  ln.getColumn(1).getInt64() == newDevId &&
                  ln.getColumn(2).getString() == "0",
              "device automation lane remapped to new device id");
        check(dest->getAutomationData(newDevLaneId, std::nullopt).has_value(),
              "device automation stream present after paste");
    }

    // Verify ONE undo step returns the EXACT start digest, and redo brings it back.
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

    // Close read-only source and confirm its bytes are 100% unchanged.
    check(roSource->close(), "read-only source closed");
    check(readAllBytes(src.adiPath) == srcBytesBefore, "source .adi file bytes unchanged");
    check(readAllBytes(src.wavPath) == wavBytesBefore, "source audio file bytes unchanged");
}

void testMediaHashDedupNotCopiedAgain() {
    section("media already present by hash is not copied again");

    test::TempDirectory temp{"project_paste", "dedup"};
    const auto src = buildSourceProject(temp.path());

    const fs::path destDir = temp.path() / "dest_dedup";
    const fs::path destWav = destDir / "audio" / "existing_kick.wav";
    fs::create_directories(destWav.parent_path());
    fs::copy_file(src.wavPath, destWav);

    StoreError err{};
    auto dest = Store::create(destDir / "dest.adi", err);
    check(dest != nullptr, "dedup dest created");

    // Pre-import the identical audio bytes into dest under media id 7.
    const auto pre = media::importMedia(*dest, destWav, 7, 1700000100);
    check(pre.ok && pre.mediaId == 7, "pre-existing media imported into dest");
    const std::size_t filesBefore = countFilesInDir(destDir / "audio");
    const std::string startDigest = digestProject(*dest).text;

    auto roSource = Store::open(src.adiPath, err, /*readOnly=*/true);
    check(roSource != nullptr, "source opened read-only for dedup");

    const auto pasted = pasteTrackFromProject(*roSource, src.trackId, *dest);
    check(pasted.ok, "paste with pre-existing media succeeded: " + pasted.error);
    check(pasted.copiedMediaCount == 0U && pasted.reusedMediaCount == 1U,
          "media already present by hash was reused, not copied");
    check(pasted.mediaIds.at(src.mediaId) == 7, "reused existing dest media id 7");
    check(countFilesInDir(destDir / "audio") == filesBefore,
          "no new file copied into dest audio/");

    // Undo still leaves pre-existing media 7 intact and returns exact start digest.
    check(History(*dest).undo().ok, "undo after dedup paste succeeded");
    check(digestProject(*dest).text == startDigest, "dedup undo returns start digest");
}

void testReadOnlyDestRefusedAndStateBlobsCopied() {
    section("read-only destination refused & state_blobs copied by hash");

    test::TempDirectory temp{"project_paste", "ro_and_state"};
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
        check(rwSrc->close(), "closed source with state blob");
    }

    const fs::path destDir = temp.path() / "dest_ro";
    fs::create_directories(destDir);
    {
        StoreError err{};
        auto tmpDest = Store::create(destDir / "dest.adi", err);
        check(tmpDest != nullptr && tmpDest->close(), "created dest for read-only check");
    }

    StoreError err{};
    auto roSource = Store::open(src.adiPath, err, true);
    auto roDest = Store::open(destDir / "dest.adi", err, true);
    check(roSource != nullptr && roDest != nullptr && roDest->readOnly(),
          "both stores open read-only");

    const auto refused = pasteTrackFromProject(*roSource, src.trackId, *roDest);
    check(!refused.ok, "paste into read-only destination refused");
    check(countFilesInDir(destDir / "audio") == 0U, "no media published on read-only refusal");
    check(roDest->close(), "closed read-only dest");

    // Now open dest writable and paste twice: first copies state blob once, second reuses it.
    auto rwDest = Store::open(destDir / "dest.adi", err, false);
    check(rwDest != nullptr, "reopened dest writable");

    const auto first = pasteTrackFromProject(*roSource, src.trackId, *rwDest);
    check(first.ok, "first paste with state blob succeeded: " + first.error);
    check(first.copiedStateBlobCount == 1U && first.reusedStateBlobCount == 0U,
          "first paste copied state blob once");
    check(rwDest->getStateBlob(blobHash).has_value(), "state blob present in dest");

    const auto second = pasteTrackFromProject(*roSource, src.trackId, *rwDest);
    check(second.ok, "second paste reuses both media and state blob: " + second.error);
    check(second.copiedMediaCount == 0U && second.reusedMediaCount == 1U,
          "second paste reused media by hash");
    check(second.copiedStateBlobCount == 0U && second.reusedStateBlobCount == 1U,
          "second paste reused state blob by hash");
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
        okA = r.ok && r.copiedMediaCount == 1U && History(*destA).undo().ok;
    });
    std::thread t2([&]() {
        const auto r = pasteTrackFromProject(*roSrc2, src.trackId, *destB);
        okB = r.ok && r.copiedMediaCount == 1U && History(*destB).undo().ok;
    });
    t1.join();
    t2.join();

    check(okA && okB, "concurrent pastes across separate open Stores succeeded cleanly");
}

}  // namespace

int main() {
    testCrossProjectPasteRemapUndoAndSourceImmutability();
    testMediaHashDedupNotCopiedAgain();
    testReadOnlyDestRefusedAndStateBlobsCopied();
    testTwoOpenStoresConcurrentlyForTsanAndAsan();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
