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
struct DelayedMultiply final : Multiply {
    static constexpr std::int32_t delay = 17;
    std::array<std::array<double, delay>, 2> history{};
    std::size_t cursor = 0;
    std::int32_t latencySamples() const noexcept override { return delay; }
    void process(const engine::NodeIo &io) noexcept override {
        ++processed;
        for (int f = 0; f < io.frames; ++f) {
            const auto frame = io.blockOffset + f;
            for (int c = 0; c < io.channels; ++c) {
                auto& held = history[static_cast<std::size_t>(c)][cursor];
                if (io.wide()) {
                    const auto input = io.in64[c][frame];
                    io.out64[c][frame] = held * 0.25;
                    held = input;
                } else {
                    const auto input = io.in[c][frame];
                    io.out[c][frame] = static_cast<float>(held * 0.25);
                    held = input;
                }
            }
            cursor = (cursor + 1) % history.front().size();
        }
    }
};
engine::DeviceLoader latencyLoader() {
    return [](const engine::DeviceRequest &, std::string &) { return std::make_unique<DelayedMultiply>(); };
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
std::vector<float> hear(Project &p, const std::string &name, engine::Precision precision,
                        std::int64_t begin = 137, std::int64_t end = 777,
                        engine::DeviceLoader load = loader()) {
    auto job = p.options().job;
    job.project = p.store->path();
    job.directory = p.directory.path();
    job.nameTemplate = name;
    job.source = {};
    job.session.precision = precision;
    job.beginSample = begin;
    job.endSample = end;
    const auto result = render::renderWav(job, std::move(load));
    check(result.state == render::JobState::Completed,
          result.error.empty() ? "native master render completes" : result.error.c_str());
    if (result.state != render::JobState::Completed)
        return {};
    audio::WavReader reader(result.output);
    const auto count = static_cast<std::uint32_t>(end - begin);
    std::vector<float> data(static_cast<std::size_t>(count) * 2);
    check(reader.frames() == count && reader.read(data.data(), count) == count,
          "audition reads exact bounced range");
    return data;
}
void command() {
    for (const auto precision :
         std::array<engine::Precision, 2>{engine::Precision::Float32, engine::Precision::Float64}) {
        Project p;
        auto &db = p.store->db();
        const auto delay = precision == engine::Precision::Float32 ? 23 : -19;
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(51,'return','Return'),(61,'vca','VCA');"
                "INSERT INTO mixer_pan_settings(track_id,mode,pan_left,pan_right)"
                " VALUES(21,'split',-0.7,0.35);"
                "UPDATE mixer_strip SET pan_law=2,width=0.65,phase_invert=1,input_gain_db=3,"
                " vca_group_id=61 WHERE track_id=21;"
                "UPDATE tracks SET record_armed=1,monitor_mode=0,input_ref='source input' WHERE id=21;"
                "UPDATE routing SET gain_db=-3,pan=0.1 WHERE id=80;"
                "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,pan,pre_fader,enabled)"
                " VALUES(70,'track',21,'track',51,'send',-18,-0.1,0,1),"
                " (71,'track',21,'track',51,'send',-12,0.2,0,1),"
                " (72,'track',21,'track',51,'send',-5,0,1,0);"
                "INSERT INTO routing_send_taps(routing_id,tap)"
                " VALUES(70,'post_fx'),(71,'post_mixer'),(72,'pre_fx');");
        {
            SQLite::Statement setDelay(db, "UPDATE mixer_strip SET delay_samples=? WHERE track_id=21");
            setDelay.bind(1, delay);
            setDelay.exec();
        }
        auto options = p.options();
        options.job.session.precision = precision;
        const auto before = hear(p, "before", precision, 0, 1024);
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
        check(db.execAndGet("SELECT parent_id FROM tracks WHERE id=30").getInt64() == 10 &&
                  db.execAndGet("SELECT color FROM tracks WHERE id=30").getInt() == 6 &&
                  db.execAndGet("SELECT kind FROM tracks WHERE id=30").getString() == "audio",
              "new plain audio sibling inherits hierarchy and palette color");
        check(db.execAndGet("SELECT index_in_parent FROM tracks WHERE id=30").getInt() == 1 &&
                  db.execAndGet("SELECT index_in_parent FROM tracks WHERE id=22").getInt() == 2,
              "new sibling is inserted immediately after the source");
        check(db.execAndGet("SELECT count(*) FROM mixer_strip WHERE track_id=30 AND volume_db=-6 "
                            "AND pan=0.25 AND pan_law=2 AND width=0.65 AND phase_invert=1 "
                            "AND vca_group_id=61 AND input_gain_db=0").getInt() == 1 &&
                  db.execAndGet("SELECT delay_samples FROM mixer_strip WHERE track_id=30").getInt() == delay &&
                  db.execAndGet("SELECT count(*) FROM mixer_pan_settings WHERE track_id=30 "
                                "AND mode='split' AND pan_left=-0.7 AND pan_right=0.35").getInt() == 1 &&
                  db.execAndGet("SELECT count(*) FROM tracks WHERE id=30 AND record_armed=0 "
                                "AND monitor_mode=2 AND input_ref IS NULL").getInt() == 1,
              "new strip has editable mixer settings and no baked input gain or source recording input");
        check(db.execAndGet("SELECT count(*) FROM routing WHERE id=81 AND dst_id=41 "
                            "AND gain_db=-3 AND pan=0.1").getInt() == 1 &&
                  db.execAndGet("SELECT muted FROM tracks WHERE id=21").getInt() == 0 &&
                  db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21 AND muted<>0").getInt() == 1 &&
                  db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21 AND muted=0").getInt() == 2 &&
                  db.execAndGet("SELECT pos_ns FROM clips WHERE track_id=21 AND muted<>0").getInt64() == 2854167 &&
                  db.execAndGet("SELECT length_ns FROM clips WHERE track_id=21 AND muted<>0").getInt64() == 13333333,
              "the explicit output remains and only the middle source piece is muted on the sample clock");
        check(db.execAndGet("SELECT count(*) FROM routing r JOIN routing_send_taps t ON t.routing_id=r.id "
                            "WHERE r.src_id=30 AND r.dst_id=51 AND r.kind='send' AND "
                            "((r.id=82 AND r.gain_db=-18 AND r.pan=-0.1 AND r.enabled=1 AND t.tap='post_fx') OR "
                            " (r.id=83 AND r.gain_db=-12 AND r.pan=0.2 AND r.enabled=1 AND t.tap='post_mixer') OR "
                            " (r.id=84 AND r.gain_db=-5 AND r.pre_fader=1 AND r.enabled=0 AND t.tap='pre_fx'))")
                      .getInt() == 3,
              "new send identities retain gain, pan, enable and explicit tap state");
        check(db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=31").getInt() == 0 &&
                  db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=31").getInt() == 640 &&
                  db.execAndGet("SELECT name FROM clips WHERE id=31").getString() == "Source (Bounce)",
              "new clip is named, sample-exact and Warp off");
        const auto after = hear(p, "after", precision, 0, 1024);
        bool matches = before.size() == after.size() && !before.empty();
        for (std::size_t i = 0; i < after.size() && matches; ++i)
            matches = std::abs(before[i] - after[i]) < 1e-7f;
        check(matches, "both rendered overlap and unselected source tails null through the explicit route");
        check(!clip_edit::commitTrackBounce(*p.store, prepared, 7).ok,
              "completed track bounce cannot be committed twice");
        check(History(*p.store).undo().ok && digestProject(*p.store).text == digest,
              "one undo restores all original tracks, order, source state and media rows");
        check(History(*p.store).redo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt() == 4,
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
                           "record_armed=1 WHERE id=21;"
                           "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,enabled,latency_samples) "
                           "VALUES(2,1,1,1,'Disabled Insert',0,31);"
                           "INSERT INTO plugin_params(device_id,param_id,name,normalized_value) "
                           "VALUES(2,'shape','Shape',0.375);");
        const auto before = hear(p, "inplace-before", precision, 0, 1024, latencyLoader());
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
        auto prepared = clip_edit::prepareBounceInPlace(*p.store, options, latencyLoader());
        check(prepared.ready(),
              prepared.error().empty() ? "whole-chain Bounce in Place prepared" : prepared.error().c_str());
        if (!prepared.ready())
            continue;
        check(digestProject(*p.store).text == state,
              "in-place preparation removes no live state before commit");
        const auto committed = clip_edit::commitTrackBounce(*p.store, prepared, 7);
        check(committed.ok, committed.error.empty()
                                ? "in-place replacement and removed chain share one undo step"
                                : committed.error.c_str());
        if (!committed.ok)
            continue;
        auto &db = p.store->db();
        check(db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21").getInt() == 1 &&
                  db.execAndGet("SELECT id FROM clips WHERE track_id=21").getInt() == 31 &&
                  db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=31").getInt() == 0,
              "in-place render replaces the entire original material, Warp off");
        check(db.execAndGet("SELECT count(*) FROM devices WHERE id IN (1,2)").getInt() == 0 &&
                  db.execAndGet("SELECT input_gain_db FROM mixer_strip WHERE track_id=21").getDouble() == 0 &&
                  db.execAndGet("SELECT record_armed FROM tracks WHERE id=21").getInt() == 0 &&
                  db.execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id=21").getDouble() == -6 &&
                  db.execAndGet("SELECT pan FROM mixer_strip WHERE track_id=21").getDouble() == 0.25,
              "enabled and disabled inserts are removed and baked gain resets while fader/pan stay live");
        processed = 0;
        const auto after = hear(p, "inplace-after", precision, 0, 1024, latencyLoader());
        check(processed == 0, "the post-bounce graph contains none of the removed device DSP");
        bool matches = before.size() == after.size() && !before.empty();
        for (std::size_t i = 0; i < after.size() && matches; ++i)
            matches = std::abs(after[i] - before[i]) < 1e-7f;
        check(
            matches,
            "the bounced PCM keeps its alignment after removing the real 17-sample insert delay");
        check(History(*p.store).undo().ok && digestProject(*p.store).text == state,
              "one in-place undo restores exact original clips, input gain and device state");
        check(History(*p.store).redo().ok &&
                  db.execAndGet("SELECT count(*) FROM devices WHERE id IN (1,2)").getInt() == 0,
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
void inPlaceRemovalRollback() {
    Project p;
    auto& db = p.store->db();
    db.exec("CREATE TABLE late_device_fault(id INTEGER PRIMARY KEY,track_id INTEGER REFERENCES tracks(id) "
            "DEFERRABLE INITIALLY DEFERRED) STRICT;"
            "CREATE TRIGGER refuse_device_removal AFTER DELETE ON devices WHEN OLD.id=1 BEGIN "
            "INSERT INTO late_device_fault VALUES(1,9999); END;");
    const auto before = digestProject(*p.store).text;
    const auto assets = p.assets();
    clip_edit::InPlaceBounceOptions options;
    options.sourceTrack = 21;
    options.clipId = 31;
    options.mediaId = 32;
    options.job = p.options().job;
    options.job.beginSample = 0;
    options.job.endSample = 1024;
    auto prepared = clip_edit::prepareBounceInPlace(*p.store, options, latencyLoader());
    check(prepared.ready(), "in-place capture is ready before the deferred device removal check");
    if (!prepared.ready()) return;
    const auto refused = clip_edit::commitTrackBounce(*p.store, prepared, 0);
    check(!refused.ok && prepared.ready() && digestProject(*p.store).text == before && p.assets() == assets,
          "late removal failure restores the original project and capture files remain retryable");
    db.exec("DROP TRIGGER refuse_device_removal; DROP TABLE late_device_fault;");
    processed = 0;
    check(clip_edit::commitTrackBounce(*p.store, prepared, 0).ok && processed == 0 &&
              db.execAndGet("SELECT count(*) FROM devices WHERE id=1").getInt() == 0,
          "retry commits the captured PCM and removes the device without another render");
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
        inPlaceRemovalRollback();
        failureRecovery();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("track bounce: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
