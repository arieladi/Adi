// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/bounce_commands.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
unsigned processed = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
struct Multiply : device::DeviceInstance {
    device::DeviceIdentity identity_;
    const device::DeviceIdentity &identity() const noexcept override {
        return identity_;
    }
    bool loaded() const noexcept override {
        return true;
    }
    bool processesDouble() const noexcept override {
        return true;
    }
    void process(const engine::NodeIo &io) noexcept override {
        ++processed;
        for (int c = 0; c < io.channels; ++c)
            for (int f = 0; f < io.frames; ++f) {
                const auto i = io.blockOffset + f;
                if (io.wide())
                    io.out64[c][i] = io.in64[c][i] * 0.25;
                else
                    io.out[c][i] = io.in[c][i] * 0.25f;
            }
    }
};
engine::DeviceLoader loader() {
    return [](const engine::DeviceRequest &, std::string &) { return std::make_unique<Multiply>(); };
}
struct Project {
    test::TempDirectory directory{"track_bounce", "project"};
    fs::path projectDirectory;
    std::unique_ptr<Store> store;
    Project()
        : projectDirectory(directory.path() /
                           media::pathFromUtf8("Set-\xE6\x9B\xB2-\xD7\xA9\xD7\x99\xD7\xA8")) {
        fs::create_directory(projectDirectory);
        StoreError error{};
        store = Store::create(projectDirectory / "song.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,parent_id,index_in_parent,color) "
            "VALUES(10,'group','Parent',NULL,0,NULL),(21,'audio','Source',10,0,6),(22,'audio','Sibling',10,1,"
            "NULL),(29,'master','Main',NULL,1,NULL),(41,'group','Actual Output',NULL,2,NULL);INSERT INTO "
            "mixer_strip(track_id,volume_db,pan) VALUES(10,6,0),(21,-6,0.25),(41,-3,0);INSERT INTO "
            "routing(id,src_kind,src_id,dst_kind,dst_id,kind,origin) "
            "VALUES(80,'track',21,'track',41,'main','user');INSERT INTO "
            "plugin_refs(id,format,uid,name,subtype) "
            "VALUES(1,'vst3','test.multiply','Multiply','effect');INSERT INTO device_chains(id,track_id) "
            "VALUES(1,21);INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
            "VALUES(1,1,0,1,'Multiply');");
        const auto path = projectDirectory / "input.wav";
        std::vector<float> data(2048);
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<float>(static_cast<int>(i % 37) - 18) / 64;
        {
            audio::WavWriter writer(path, 48000, 2, audio::WavFormat::Float32);
            writer.write(data.data(), 1024);
            writer.close();
        }
        clip_edit::ImportOptions options;
        options.source = path;
        options.track = 21;
        options.clipId = 7;
        options.mediaId = 8;
        options.at = {1, 0};
        options.copyIn = true;
        auto imported = clip_edit::prepareImportAudio(*store, options, 48000);
        const auto committed = clip_edit::commitMaterial(*store, imported);
        if (!committed.ok)
            throw std::runtime_error(committed.error);
    }
    clip_edit::TrackBounceOptions options() const {
        clip_edit::TrackBounceOptions options;
        options.sourceTrack = 21;
        options.newTrack = 30;
        options.clipId = 31;
        options.mediaId = 32;
        options.routingId = 81;
        options.documentGeneration = 7;
        options.job.beginSample = 137;
        options.job.endSample = 777;
        options.job.session = {2, 48000, 64};
        options.job.encoding.format = audio::WavFormat::Float32;
        return options;
    }
    std::set<fs::path> assets() const {
        std::set<fs::path> result;
        for (const auto &item : fs::directory_iterator(projectDirectory / "audio"))
            result.insert(item.path());
        return result;
    }
};
std::vector<float> hear(Project &p, const std::string &name, engine::Precision precision) {
    auto job = p.options().job;
    job.project = p.store->path();
    job.directory = p.directory.path();
    job.nameTemplate = name;
    job.source = {};
    job.session.precision = precision;
    const auto result = render::renderWav(job, loader());
    check(result.state == render::JobState::Completed,
          result.error.empty() ? "native master render completes" : result.error.c_str());
    if (result.state != render::JobState::Completed)
        return {};
    audio::WavReader reader(result.output);
    std::vector<float> data(1280);
    check(reader.frames() == 640 && reader.read(data.data(), 640) == 640,
          "audition reads exact bounced range");
    return data;
}
void command() {
    for (const auto precision :
         std::array<engine::Precision, 2>{engine::Precision::Float32, engine::Precision::Float64}) {
        Project p;
        auto options = p.options();
        options.job.session.precision = precision;
        const auto before = hear(p, "before", precision);
        const auto digest = digestProject(*p.store).text;
        auto prepared = clip_edit::prepareBounceToNewTrack(*p.store, options, loader());
        check(prepared.ready(),
              prepared.error().empty() ? "captured track bounce is ready" : prepared.error().c_str());
        if (!prepared.ready())
            continue;
        check(digestProject(*p.store).text == digest, "track-bounce preparation changes no project rows");
        check(!clip_edit::commitTrackBounce(*p.store, prepared, 8).ok,
              "track-bounce document generation is checked before native commit");
        const auto committed = clip_edit::commitTrackBounce(*p.store, prepared, 7);
        check(committed.ok, committed.error.empty() ? "bounce track, source mute and clip share one commit"
                                                    : committed.error.c_str());
        if (!committed.ok)
            continue;
        auto &db = p.store->db();
        check(db.execAndGet("SELECT parent_id FROM tracks WHERE id=30").getInt64() == 10 &&
                  db.execAndGet("SELECT color FROM tracks WHERE id=30").getInt() == 6 &&
                  db.execAndGet("SELECT kind FROM tracks WHERE id=30").getString() == "audio",
              "new plain audio sibling inherits hierarchy and palette color");
        check(db.execAndGet("SELECT index_in_parent FROM tracks WHERE id=30").getInt() == 1 &&
                  db.execAndGet("SELECT index_in_parent FROM tracks WHERE id=22").getInt() == 2,
              "new sibling is inserted immediately after the source");
        check(db.execAndGet("SELECT dst_id FROM routing WHERE id=81").getInt() == 41 &&
                  db.execAndGet("SELECT muted FROM tracks WHERE id=21").getInt() == 1,
              "explicit output destination is retained and original is muted");
        check(db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=31").getInt() == 0 &&
                  db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=31").getInt() == 640 &&
                  db.execAndGet("SELECT name FROM clips WHERE id=31").getString() == "Source (Bounce)",
              "new clip is named, sample-exact and Warp off");
        const auto after = hear(p, "after", precision);
        bool matches = before.size() == after.size() && !before.empty();
        for (std::size_t i = 0; i < after.size() && matches; ++i)
            matches = std::abs(before[i] - after[i]) < 1e-7f;
        check(matches, "actual heard wet output matches through the captured parent/routing in both widths");
        check(!clip_edit::commitTrackBounce(*p.store, prepared, 7).ok,
              "completed track bounce cannot be committed twice");
        check(History(*p.store).undo().ok && digestProject(*p.store).text == digest,
              "one undo restores all original tracks, order, source state and media rows");
        check(History(*p.store).redo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt() == 2,
              "one redo restores track and immutable render without running the effect again");
        const auto path = p.store->path();
        p.store.reset();
        StoreError error{};
        p.store = Store::open(path, error);
        check(p.store && History(*p.store).undo().ok && digestProject(*p.store).text == digest,
              "track-bounce undo survives reopen");
    }
}
void inPlace() {
    for (const auto precision :
         std::array<engine::Precision, 2>{engine::Precision::Float32, engine::Precision::Float64}) {
        Project p;
        p.store->db().exec("UPDATE mixer_strip SET input_gain_db=6 WHERE track_id=21;UPDATE tracks SET "
                           "record_armed=1 WHERE id=21;");
        const auto before = hear(p, "inplace-before", precision);
        const auto state = digestProject(*p.store).text;
        clip_edit::InPlaceBounceOptions options;
        options.sourceTrack = 21;
        options.clipId = 31;
        options.mediaId = 32;
        options.documentGeneration = 7;
        options.job = p.options().job;
        options.job.beginSample = 0;
        options.job.endSample = 1024;
        options.job.session.precision = precision;
        auto prepared = clip_edit::prepareBounceInPlace(*p.store, options, loader());
        check(prepared.ready(),
              prepared.error().empty() ? "whole-chain Bounce in Place prepared" : prepared.error().c_str());
        if (!prepared.ready())
            continue;
        check(digestProject(*p.store).text == state,
              "in-place preparation parks no live state before commit");
        const auto committed = clip_edit::commitTrackBounce(*p.store, prepared, 7);
        check(committed.ok, committed.error.empty()
                                ? "in-place replacement and parked chain share one undo step"
                                : committed.error.c_str());
        if (!committed.ok)
            continue;
        auto &db = p.store->db();
        check(db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21").getInt() == 1 &&
                  db.execAndGet("SELECT id FROM clips WHERE track_id=21").getInt() == 31 &&
                  db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=31").getInt() == 0,
              "in-place render replaces the entire original material, Warp off");
        check(db.execAndGet("SELECT enabled FROM devices WHERE id=1").getInt() == 0 &&
                  db.execAndGet("SELECT input_gain_db FROM mixer_strip WHERE track_id=21").getDouble() == 0 &&
                  db.execAndGet("SELECT record_armed FROM tracks WHERE id=21").getInt() == 0 &&
                  db.execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id=21").getDouble() == -6 &&
                  db.execAndGet("SELECT pan FROM mixer_strip WHERE track_id=21").getDouble() == 0.25,
              "baked input gain resets and inserts park while fader/pan remain live");
        processed = 0;
        const auto after = hear(p, "inplace-after", precision);
        check(processed == 0, "parked device DSP is never processed when playing the bounced file");
        bool matches = before.size() == after.size() && !before.empty();
        for (std::size_t i = 0; i < after.size() && matches; ++i)
            matches = std::abs(after[i] - before[i]) < 1e-7f;
        check(
            matches,
            "whole-chain in-place playback matches actual source without processing it twice in both widths");
        check(History(*p.store).undo().ok && digestProject(*p.store).text == state,
              "one in-place undo restores exact original clips, input gain and device state");
        check(History(*p.store).redo().ok &&
                  db.execAndGet("SELECT enabled FROM devices WHERE id=1").getInt() == 0,
              "in-place redo restores immutable render without rerunning source DSP");
    }
    Project restricted;
    clip_edit::InPlaceBounceOptions options;
    options.sourceTrack = 21;
    options.clipId = 31;
    options.mediaId = 32;
    options.job = restricted.options().job;
    options.job.beginSample = 0;
    options.job.endSample = 100;
    auto partial = clip_edit::prepareBounceInPlace(*restricted.store, options, loader());
    check(!partial.ready() && partial.error().find("every clip") != std::string::npos,
          "partial whole-chain bounce refuses to disable effects for unrendered material");
    options.job.endSample = 1024;
    restricted.store->db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
                                "VALUES(90,'track',22,'track',21,'main')");
    auto incoming = clip_edit::prepareBounceInPlace(*restricted.store, options, loader());
    check(!incoming.ready() && incoming.error().find("incoming") != std::string::npos,
          "live incoming main audio refuses a double-summed in-place replacement");
}
void failureRecovery() {
    Project p;
    auto options = p.options();
    auto &db = p.store->db();
    db.exec("CREATE TABLE future_deferred(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES tracks(id) "
            "DEFERRABLE INITIALLY DEFERRED) STRICT;CREATE TRIGGER fail_bounce AFTER INSERT ON clips BEGIN "
            "INSERT INTO future_deferred VALUES(new.id,9999);END;");
    const auto before = digestProject(*p.store).text;
    const auto assets = p.assets();
    auto prepared = clip_edit::prepareBounceToNewTrack(*p.store, options, loader());
    check(prepared.ready(), "track bounce prepared before deferred SQL fault");
    const auto failed = clip_edit::commitTrackBounce(*p.store, prepared, 7);
    check(!failed.ok && digestProject(*p.store).text == before && p.assets() == assets,
          "late track-bounce SQL fault restores sibling order/mute and removes only its own publication");
    db.exec("DROP TRIGGER fail_bounce;DROP TABLE future_deferred;");
    check(clip_edit::commitTrackBounce(*p.store, prepared, 7).ok,
          "same track-bounce job retries after late SQL failure without rerendering");
    Project stale;
    options = stale.options();
    auto capture = clip_edit::prepareBounceToNewTrack(*stale.store, options, loader());
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 21}, {"name", "Newer"}};
    check(OpJournal(*stale.store).commit(rename).ok, "intervening source-track edit");
    const auto rejected = clip_edit::commitTrackBounce(*stale.store, capture, 7);
    check(!rejected.ok && rejected.stale &&
              stale.store->db().execAndGet("SELECT count(*) FROM tracks WHERE id=30").getInt() == 0,
          "stale track-bounce head refuses before sibling creation/publication");
    options = stale.options();
    options.routingId = 0;
    check(!clip_edit::prepareBounceToNewTrack(*stale.store, options, loader()).ready(),
          "explicit Main destination requires an allocated routing identity");
    std::stop_source stop;
    stop.request_stop();
    render::Control control;
    control.stop = stop.get_token();
    options = stale.options();
    check(!clip_edit::prepareBounceToNewTrack(*stale.store, options, loader(), control).ready(),
          "cancelled track bounce never creates a track");
}
} // namespace
int main() {
    try {
        command();
        inPlace();
        failureRecovery();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("track bounce: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
