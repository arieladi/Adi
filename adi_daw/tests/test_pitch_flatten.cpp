// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/edit_rows.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/pitch_flatten.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::clip_edit;
namespace fs = std::filesystem;
constexpr std::uint32_t rate = 44100, total = 30000, first = 1800, frames = 22000;
constexpr std::uint32_t graphRate = 48000;
constexpr std::int32_t block = 1024;
constexpr std::int64_t quarter = 5765760;
int checks = 0, failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

struct Fixture {
    test::TempDirectory directory{"pitch_flatten", "material"};
    const fs::path source = directory.path() / "vocal.wav";
    const fs::path project = directory.path() / "song.adi";
    audio::DecodeCache decode{directory.path() / "decode"};
    media::AnalysisCache analysis{directory.path() / "analysis"};
    std::unique_ptr<Store> store;

    Fixture(bool warp, bool reverse) {
        writeSource(220);
        StoreError error{};
        store = Store::create(project, error);
        if (!store) throw std::runtime_error(toString(error));
        std::ifstream schema(ADI_PITCH_SCHEMA_PATH);
        if (!schema) throw std::runtime_error("read reserved pitch edit SQL");
        store->db().exec(std::string(std::istreambuf_iterator<char>(schema), std::istreambuf_iterator<char>()));
        auto& db = store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Tone'),(9,'master','Main');"
                "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,name,fade_in_ticks) VALUES"
                "(7,1,'audio',1,0,500000000,'Lead',57657),(8,1,'audio',1,1000000000,500000000,'Sibling',57657);");
        const auto imported = media::importMedia(*store, source, 1, 0);
        if (!imported.ok) throw std::runtime_error(imported.error);
        const std::array markers{WarpMarker{0, 0}, WarpMarker{frames / 4, 3 * quarter / 8}, WarpMarker{frames, quarter}};
        const auto map = audio::writeWarpMap(markers, frames);
        if (!map) throw std::runtime_error("write Warp fixture");
        SQLite::Statement insert(db, "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,"
                                     "warp_enabled,warp_mode,warp_markers,reverse) VALUES(?,1,?,?,?,?,?,?)");
        for (const std::int64_t id : {7, 8}) {
            insert.bind(1, id);
            insert.bind(2, first);
            insert.bind(3, frames);
            insert.bind(4, warp ? 1 : 0);
            insert.bind(5, warp ? "re-pitch" : "none");
            if (warp) insert.bind(6, map.bytes.data(), static_cast<int>(map.bytes.size()));
            else insert.bind(6);
            insert.bind(7, reverse ? 1 : 0);
            insert.exec();
            insert.reset();
            insert.clearBindings();
        }
        for (const std::int64_t id : {7, 8}) {
            const auto detected = media::buildClipPitchAnalysis(*store, id, {}, &decode, &analysis);
            if (!detected || detected.data->analysis.notes.size() != 1) throw std::runtime_error("one voiced fixture note required");
            audio::PitchEditState state;
            const std::array commands{audio::PitchEditCommand{0, audio::PitchSetShift{100}}};
            if (audio::applyPitchEdits(detected.data->analysis, state, commands) != audio::PitchEditError::None)
                throw std::runtime_error("prepare note edit");
            const auto prepared = prepareClipPitchEdits(*detected.data, state);
            if (!prepared.request) throw std::runtime_error(prepared.error);
            const auto committed = OpJournal(*store).commit(*prepared.request);
            if (!committed.ok) throw std::runtime_error(committed.error);
        }
    }

    void writeSource(double hz) const {
        std::vector<float> pcm(total * 2);
        for (std::size_t frame = 0; frame < total; ++frame) {
            pcm[frame * 2] = static_cast<float>(0.33 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(frame) / rate));
            pcm[frame * 2 + 1] = pcm[frame * 2] * 0.4f;
        }
        audio::WavWriter writer(source, rate, 2, audio::WavFormat::Float32);
        writer.write(pcm.data(), total);
        writer.close();
    }

    PitchFlattenResult prepare(std::int64_t clip, std::int64_t mediaId,
                               PitchFlattenScope scope = PitchFlattenScope::WholeEvent, std::stop_token stop = {}) {
        return preparePitchFlatten(*store, {scope, clip, mediaId, 0, stop}, &decode, &analysis);
    }

    [[nodiscard]] std::optional<Payload> row(const char* table, std::int64_t id) const { return edit::row(store->db(), table, id); }
    [[nodiscard]] bool mediaExists(std::int64_t id) const {
        SQLite::Statement row(store->db(), "SELECT 1 FROM media_files WHERE id=?");
        row.bind(1, id);
        return row.executeStep();
    }
    [[nodiscard]] std::size_t stages() const {
        std::size_t count = 0;
        for (const auto& item : fs::directory_iterator(directory.path()))
            if (item.path().filename().string().starts_with(".adi-edit-")) ++count;
        return count;
    }

    void reopen() {
        store.reset();
        StoreError error{};
        store = Store::open(project, error);
        if (!store) throw std::runtime_error(toString(error));
    }
};

std::vector<float> renderClip(const Store& store, std::int64_t start = 6000) {
    engine::Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, {2, graphRate, block})) throw std::runtime_error(session.error());
    for (const auto& problem : session.clips()->problems())
        throw std::runtime_error(problem);
    session.transport().locate(start);
    session.transport().play();
    std::vector<float> pcm;
    for (int iteration = 0; iteration < 6; ++iteration) {
        if (!session.clips()->prime()) throw std::runtime_error("prime clip output");
        std::array<float, block> left{}, right{};
        float* channels[]{left.data(), right.data()};
        engine::AudioIo io;
        io.frames = block;
        io.out = channels;
        io.numOut = 2;
        {
            const audio::CallbackScope callback;
            session.process(io);
        }
        pcm.insert(pcm.end(), left.begin(), left.end());
    }
    return pcm;
}

double frequency(std::span<const float> pcm) {
    std::size_t crossings = 0;
    double begin = 0, end = 0;
    for (std::size_t index = 1; index < pcm.size(); ++index) {
        if (pcm[index - 1] <= 0 && pcm[index] > 0) {
            const double crossing = static_cast<double>(index - 1) - pcm[index - 1] /
                                    (static_cast<double>(pcm[index]) - pcm[index - 1]);
            if (crossings == 0) begin = crossing;
            end = crossing;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * graphRate / (end - begin) : 0;
}

void wholeEventAndReuse() {
    Fixture fixture(true, true);
    constexpr std::int64_t body = 14000;
    const auto pitchBefore = fixture.row("clip_pitch_edits", 7);
    const auto siblingPitch = fixture.row("clip_pitch_edits", 8);
    const auto sourceBefore = fixture.row("audio_clips", 7);
    const auto siblingSource = fixture.row("audio_clips", 8);
    const auto placement = fixture.row("clips", 7);
    const auto originalHash = media::blake3File(fixture.source).hex;
    const auto audibleBefore = renderClip(*fixture.store, body);
    auto flattened = fixture.prepare(7, 2);
    check(flattened && flattened.material->edit.requests.size() == 2 && fixture.stages() == 1 &&
          fixture.row("clip_pitch_edits", 7) == pitchBefore && !fixture.mediaExists(2),
          "whole-event preparation stages a new file without editing project rows");
    if (!flattened) throw std::runtime_error(flattened.detail.value_or("prepare pitch flatten"));
    const auto committed = commitMaterial(*fixture.store, *flattened.material);
    check(committed.ok, "the existing material journal commits import and source replacement together");
    if (!committed.ok) throw std::runtime_error(committed.error);
    const auto sourceAfter = fixture.row("audio_clips", 7);
    check(sourceAfter && sourceAfter->at("media_id") == 2 && sourceAfter->at("src_start_frames") == 0 &&
          sourceAfter->at("src_len_frames") == frames && sourceAfter->at("warp_markers") == sourceBefore->at("warp_markers") &&
          sourceAfter->at("warp_mode") == sourceBefore->at("warp_mode") && sourceBefore->at("reverse") == 1 &&
          sourceAfter->at("reverse") == 0 && fixture.row("clips", 7) == placement && !fixture.row("clip_pitch_edits", 7),
          "whole event bakes reverse into its rendered aperture and retains project Warp and placement");
    check(fixture.row("audio_clips", 8) == siblingSource && fixture.row("clip_pitch_edits", 8) == siblingPitch &&
          media::blake3File(fixture.source).hex == originalHash,
          "sibling sharing the old source and original file remain identical");
    const auto newMedia = media::mediaRow(fixture.store->db(), 2);
    const auto newFile = fixture.directory.path() / media::pathFromUtf8(newMedia.at("rel_path").get<std::string>());
    const auto newHash = media::blake3File(newFile).hex;
    {
        const audio::WavReader reader(newFile);
        check(reader.sampleRate() == rate && reader.frames() == frames && reader.channels() == 2,
              "published durable render keeps the native-rate stereo trim exactly");
    }
    const auto audibleAfter = renderClip(*fixture.store, body);
    float maximumError = 0;
    for (std::size_t sample = 0; sample < audibleBefore.size(); ++sample)
        maximumError = std::max(maximumError, std::abs(audibleAfter[sample] - audibleBefore[sample]));
    std::printf("Reversed, Warped whole event flatten: %.5f Hz before and %.5f Hz after, %zu samples compared, maximum PCM error %.9f\n",
                frequency(audibleBefore), frequency(audibleAfter), audibleBefore.size(), maximumError);
    check(audibleAfter == audibleBefore && frequency(audibleAfter) > 270,
          "normal Session playback stays bit-identical after flattening reversed pitch with nonlinear Warp");

    flattened.material.reset();
    check(fixture.stages() == 0, "release after commit removes only the private material staging directory");
    check(History(*fixture.store).undo().ok && fixture.row("audio_clips", 7) == sourceBefore &&
          fixture.row("clip_pitch_edits", 7) == pitchBefore && !fixture.mediaExists(2) && fs::is_regular_file(newFile),
          "one Undo restores exact source and pitch payload and retains render bytes for redo");
    fixture.reopen();
    check(History(*fixture.store).redo().ok && fixture.row("audio_clips", 7) == sourceAfter &&
          !fixture.row("clip_pitch_edits", 7) && media::blake3File(newFile).hex == newHash &&
          renderClip(*fixture.store, body) == audibleAfter,
          "Redo after reopen restores exact baked bytes and playback from durable media");

    auto reuse = fixture.prepare(8, 3);
    check(reuse && reuse.material->edit.requests.size() == 1 && reuse.material->edit.requests.front().opType == "arranger.splice",
          "identical rendered hash captures no superfluous media import or relink");
    if (!reuse) throw std::runtime_error(reuse.detail.value_or("prepare deduplicated pitch flatten"));
    const auto reused = commitMaterial(*fixture.store, *reuse.material);
    const auto siblingAfter = fixture.row("audio_clips", 8);
    check(reused.ok && siblingAfter && siblingAfter->at("media_id") == 2 && !fixture.mediaExists(3) &&
          media::mediaRow(fixture.store->db(), 2) == newMedia && fixture.row("audio_clips", 7) == sourceAfter,
          "shared output uses the existing pool row without changing its path or prior clip");
    check(History(*fixture.store).undo().ok && fixture.row("audio_clips", 8) == siblingSource &&
          fixture.row("clip_pitch_edits", 8) == siblingPitch && fixture.mediaExists(2),
          "Undo of a deduplicated render retains other clip ownership and exact pitch state");

    auto staleReuse = fixture.prepare(8, 4);
    if (!staleReuse) throw std::runtime_error(staleReuse.detail.value_or("prepare reuse verification fixture"));
    const auto temporary = fixture.directory.path() / "kept-render.wav";
    fs::copy_file(newFile, temporary);
    {
        std::ofstream output(newFile, std::ios::binary | std::ios::trunc);
        output << "changed derivative";
    }
    const auto refused = commitMaterial(*fixture.store, *staleReuse.material);
    fs::copy_file(temporary, newFile, fs::copy_options::overwrite_existing);
    check(!refused.ok && refused.error.find("identical rendered media changed") != std::string::npos &&
          fixture.row("audio_clips", 8) == siblingSource && media::blake3File(newFile).hex == newHash,
          "removed or changed reused bytes refuse the whole transaction without relinking");

    const auto detected = media::buildClipPitchAnalysis(*fixture.store, 8, {}, &fixture.decode, &fixture.analysis);
    if (!detected) throw std::runtime_error("analyze sibling before clearing pitch edits");
    const auto clear = prepareClipPitchEdits(*detected.data, {});
    if (!clear.request) throw std::runtime_error(clear.error);
    const auto isGenericReverseRefused = [&] {
        engine::Session session;
        if (!session.load(*fixture.store, {}, {2, graphRate, block})) throw std::runtime_error(session.error());
        return std::any_of(session.clips()->problems().begin(), session.clips()->problems().end(), [](const auto& problem) {
            return problem.starts_with("clips#8: reverse unsupported");
        });
    };
    const auto cleared = OpJournal(*fixture.store).commit(*clear.request);
    check(cleared.ok && isGenericReverseRefused(), "clearing authored pitch keeps ordinary reversed Warp named silent");
    History history(*fixture.store);
    check(history.undo().ok && fixture.row("clip_pitch_edits", 8) == siblingPitch &&
          frequency(renderClip(*fixture.store, graphRate + body)) > 270,
          "Undo restores reversed Warp audio with the original authored pitch row");
    check(history.redo().ok && isGenericReverseRefused(), "Redo restores the unchanged generic reverse refusal");
}

void refusalsAndReverse() {
    Fixture fixture(false, true);
    const auto sourceBefore = fixture.row("audio_clips", 7);
    const auto pitchBefore = fixture.row("clip_pitch_edits", 7);
    const auto originalHash = media::blake3File(fixture.source).hex;
    const auto head = OpJournal(*fixture.store).headSeq();
    check(fixture.prepare(7, 2, PitchFlattenScope::SelectedSegments).problem == PitchFlattenProblem::UnsupportedScope &&
          fixture.prepare(7, 2, PitchFlattenScope::Unspecified).problem == PitchFlattenProblem::UnsupportedScope && fixture.stages() == 0,
          "selected and unspecified scopes refuse before creating any staged material");
    std::stop_source stop;
    stop.request_stop();
    check(fixture.prepare(7, 2, PitchFlattenScope::WholeEvent, stop.get_token()).problem == PitchFlattenProblem::Cancelled,
          "cancelled preparation refuses before recording a durable change");
    const PitchFlattenOptions scopes{PitchFlattenScope::WholeEvent, 7, 2, 0, {}};
    PitchFlattenResult callbackResult;
    const auto ioBefore = audio::callbackFileIo.load();
    {
        audio::CallbackScope callback;
        callbackResult = preparePitchFlatten(*fixture.store, scopes);
    }
    check(callbackResult.problem == PitchFlattenProblem::Realtime && !callbackResult.material && audio::callbackFileIo.load() == ioBefore,
          "callback refuses before file I/O and preparation");
    {
        const auto abandoned = fixture.prepare(7, 2);
        check(abandoned && fixture.stages() == 1 && !fixture.mediaExists(2), "pending render owns one unpublished stage");
    }
    check(fixture.stages() == 0 && OpJournal(*fixture.store).headSeq() == head && !fixture.mediaExists(2),
          "abandoned preparation retires its file without history or project rows");
    {
        auto stale = fixture.prepare(7, 2);
        if (!stale) throw std::runtime_error(stale.detail.value_or("stale source fixture"));
        fixture.writeSource(245);
        const auto refused = commitMaterial(*fixture.store, *stale.material);
        fixture.writeSource(220);
        check(!refused.ok && refused.error.find("source changed") != std::string::npos && !fixture.mediaExists(2) &&
              fixture.row("audio_clips", 7) == sourceBefore && fixture.row("clip_pitch_edits", 7) == pitchBefore &&
              OpJournal(*fixture.store).headSeq() == head, "source hash change after capture rolls back all edits");
    }
    {
        auto stale = fixture.prepare(7, 2);
        if (!stale) throw std::runtime_error(stale.detail.value_or("stale history fixture"));
        OpRequest name;
        name.opType = "clip.setName";
        name.payload = {{"id", 8}, {"name", "Another change"}};
        const auto other = OpJournal(*fixture.store).commit(name);
        const auto refused = commitMaterial(*fixture.store, *stale.material);
        check(other.ok && !refused.ok && refused.stale && !fixture.mediaExists(2) && fixture.row("clip_pitch_edits", 7) == pitchBefore,
              "changed journal head rejects a stale flatten without material publication");
    }
    auto reverse = fixture.prepare(7, 2);
    if (!reverse) throw std::runtime_error(reverse.detail.value_or("prepare reverse flatten"));
    const auto previous = renderClip(*fixture.store);
    const auto baked = commitMaterial(*fixture.store, *reverse.material);
    const auto sourceAfter = fixture.row("audio_clips", 7);
    check(baked.ok && sourceAfter && sourceAfter->at("reverse") == 0 && frequency(previous) > 225 &&
          renderClip(*fixture.store) == previous &&
          media::blake3File(fixture.source).hex == originalHash, "unwarped reverse keeps identical playback through the baked native PCM");
}
} // namespace

int main() {
    try {
        wholeEventAndReuse();
        refusalsAndReverse();
    } catch (const std::exception& error) {
        std::printf("FAIL fixture: %s\n", error.what());
        ++failures;
    }
    std::printf("%d pitch flatten checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
