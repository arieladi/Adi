#include "adi/media/blake3.hpp"
#include "freeze_fixture.hpp"
namespace {
bool samples(const fs::path &path, float gain) {
    audio::WavReader reader(path);
    if (reader.frames() != 1003 || reader.sampleRate() != 48000 || reader.channels() != 2)
        return false;
    std::vector<float> pcm(2006);
    if (reader.read(pcm.data(), 1003) != 1003)
        return false;
    for (std::uint64_t f = 0; f < 1003; ++f)
        for (unsigned c = 0; c < 2; ++c)
            if (pcm[static_cast<std::size_t>(f) * 2 + c] != gain * signal(f, c))
                return false;
    return true;
}
void sharedPass() {
    for (const auto precision :
         std::array<engine::Precision, 2>{engine::Precision::Float32, engine::Precision::Float64}) {
        Project p;
        Counts counts;
        const auto initialHead = OpJournal(*p.store).headSeq();
        auto requested = p.spec(precision);
        bool edited = false;
        render::Control control;
        control.progress = [&](render::JobId, std::uint64_t, std::uint64_t) {
            if (edited)
                return;
            edited = true;
            // Disk inputs were pinned before rendering; even a replaced external
            // asset during progress must not contaminate the remaining stream.
            std::vector<float> zeros(6000);
            audio::WavWriter changed(p.root.path() / "source.wav", 48000, 2, audio::WavFormat::Float32);
            changed.write(zeros.data(), 3000);
            changed.close();
            requested.endSample = 1;
            requested.session.channels = 64;
            requested.tracks.clear();
            OpRequest request;
            request.opType = "clip.setGain";
            request.payload = {{"id", 1}, {"gain", -90.0}};
            if (!OpJournal(*p.store).commit(request).ok)
                throw std::runtime_error("intervening fixture edit");
        };
        const auto before = audio::callbackFileIo.load();
        auto captured = render::captureFreezeAudio(*p.store, requested, p.loader(counts), control);
        check(captured.ready(),
              captured.error().empty() ? "one-pass freeze capture is ready" : captured.error().c_str());
        if (!captured.ready())
            continue;
        check(counts.loads == 3 && counts.frames[0] == 1039 && counts.frames[1] == 1039 &&
                  counts.frames[2] == 1039,
              "all streams share one loaded Session and one N+max-latency processing pass");
        check(captured.sourceHead() == initialHead && OpJournal(*p.store).headSeq() != initialHead,
              "private capture retains original journal revision during a live edit");
        check(captured.streams().size() == 2 && captured.streams()[0].preFxLatency == 20 &&
                  captured.streams()[0].postFxLatency == 36 && captured.streams()[1].preFxLatency == 0,
              "tap-specific pre/post effect latencies are retained");
        const auto &info = captured.streams()[0].source;
        check(info.devices.size() == 3 && info.prefixDevices == std::vector<std::int64_t>({1, 2}) &&
                  info.devices[1].identity.version == "runtime-2",
              "prefix comes from actual restored MIDI-consumer behavior and loaded identities, not stored "
              "subtype/version");
        check(samples(captured.streams()[0].preFx, 1) && samples(captured.streams()[0].postFx, 0.25f),
              "actual pre/post FX PCM aligns to exact source samples in both processing widths");
        check(samples(captured.streams()[1].preFx, 1) && samples(captured.streams()[1].postFx, 1),
              "bare track junction captures remain aligned alongside a delayed chain");
        check(audio::callbackFileIo.load() == before,
              "actual file capture performs zero file I/O inside Session callbacks");
        {
            SQLite::Database backup(media::pathUtf8(captured.directory() / "snapshot.adi"),
                                    SQLite::OPEN_READONLY);
            SQLite::Statement input(backup, "SELECT abs_path_hint,hash_blake3 FROM media_files WHERE id=1");
            const bool found = input.executeStep();
            const auto immutable =
                found ? media::blake3File(media::pathFromUtf8(input.getColumn(0).getString()))
                      : media::HashResult{{}, media::HashError::open};
            const auto altered = media::blake3File(p.root.path() / "source.wav");
            check(found && immutable && altered && immutable.hex == input.getColumn(1).getString() &&
                      immutable.hex != altered.hex,
                  "private input bytes retain the original immutable revision after external media "
                  "replacement");
        }
        const auto folder = captured.directory();
        auto shared = captured;
        captured = {};
        check(fs::exists(folder), "private streams survive while another captured value owns them");
        shared = {};
        check(!fs::exists(folder) && p.stages().empty(),
              "last owner removes only its private cache directory after readers close");
    }
}
void failureCases() {
    Project p;
    Counts counts;
    auto spec = p.spec();
    spec.tracks = {1, 1};
    check(!render::captureFreezeAudio(*p.store, spec, p.loader(counts)).ready() && counts.loads == 0 &&
              p.stages().empty(),
          "duplicate source identities fail before loading or staging");
    spec = p.spec();
    spec.tracks = {123};
    check(!render::captureFreezeAudio(*p.store, spec, p.loader(counts)).ready() && p.stages().empty(),
          "missing endpoint cleans all private state");
    counts = {};
    auto poisoned = render::captureFreezeAudio(*p.store, p.spec(), p.loader(counts, true));
    check(!poisoned.ready() && poisoned.error().find("nonfinite") != std::string::npos && p.stages().empty(),
          "nonfinite source aborts the complete multi-stream capture");
    counts = {};
    auto changed = render::captureFreezeAudio(*p.store, p.spec(), p.loader(counts, false, true));
    check(!changed.ready() &&
              (changed.error().find("latency") != std::string::npos ||
               changed.error().find("topology") != std::string::npos) &&
              p.stages().empty(),
          "mid-render latency changes abort all streams");
    std::stop_source stop;
    render::Control control;
    control.stop = stop.get_token();
    control.progress = [&](render::JobId, std::uint64_t, std::uint64_t) { stop.request_stop(); };
    counts = {};
    auto cancelled = render::captureFreezeAudio(*p.store, p.spec(), p.loader(counts), control);
    check(cancelled.cancelled() && !cancelled.ready() && p.stages().empty(),
          "cancellation closes streams and removes only the owned private directory");
}

void scopesOnlyPlayingAssets() {
    Project p;
    auto& db=p.store->db();
    db.exec("INSERT INTO lanes(id,track_id,kind) VALUES(7,1,'take');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,orig_name,format) VALUES"
            "(2,'muted-missing','muted.wav','muted.wav','wav'),"
            "(3,'take-missing','take.wav','take.wav','wav'),"
            "(4,'unrelated-missing','unrelated.wav','unrelated.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,lane_id,pos_ticks,length_ticks,muted) VALUES"
            "(3,1,'audio',NULL,0,100,1),(4,1,'audio',7,0,100,0),(5,2,'audio',NULL,0,100,0);"
            "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES"
            "(3,2,20),(4,3,20),(5,4,20)");
    Counts counts;
    auto spec=p.spec();
    spec.tracks={1};
    const auto scoped=render::captureFreezeAudio(*p.store,spec,p.loader(counts));
    check(scoped.ready() && scoped.streams().size()==1 &&
          samples(scoped.streams().front().preFx,1.0f),
          "Freeze ignores missing muted, take and unrelated sources while rendering the selected PCM");
    if(scoped.ready()) {
        check(fs::exists(scoped.directory()/"asset-1.wav") &&
              !fs::exists(scoped.directory()/"asset-2.wav") &&
              !fs::exists(scoped.directory()/"asset-3.wav") &&
              !fs::exists(scoped.directory()/"asset-4.wav"),
              "only the playing media is pinned into the private generation");
    }
    check(db.execAndGet("SELECT muted FROM clips WHERE id=5").getInt()==0,
          "capture scope leaves the original unrelated clip unchanged");

    db.exec("UPDATE clips SET muted=0 WHERE id=3");
    const auto audible=render::captureFreezeAudio(*p.store,spec,p.loader(counts));
    check(!audible.ready() && audible.error().find("muted.wav")!=std::string::npos,
          "the same missing source refuses Freeze once made playable");
}
} // namespace
int main() {
    try {
        sharedPass();
        failureCases();
        scopesOnlyPlayingAssets();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("freeze capture: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
