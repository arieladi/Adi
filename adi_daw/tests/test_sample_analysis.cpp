// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/library/index.hpp"
#include "adi/library/sample_analysis_catalog.hpp"
#include "adi/library/sample_query.hpp"
#include "adi/material_commands.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/warp_commands.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace {
using namespace adi;
namespace fs = std::filesystem;
constexpr std::uint32_t rate = 48000;
int checks = 0, failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void writeSample(const fs::path& path, std::uint32_t frames, double frequency, unsigned hits) {
    std::vector<float> pcm(frames);
    const auto spacing = hits > 0 ? frames / hits : frames;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto time = static_cast<double>(frame % spacing) / rate;
        pcm[frame] = static_cast<float>(std::exp(-time * 130) *
            std::sin(2 * std::numbers::pi * frequency * time));
    }
    audio::WavWriter writer(path, rate, 1, audio::WavFormat::Pcm24);
    writer.write(pcm.data(), frames);
    writer.close();
}

struct Fixture {
    test::TempDirectory temp{"sample_analysis", "library"};
    fs::path kick = temp.path() / "Kick short 1.wav";
    fs::path neighbor = temp.path() / "Kick short 2.wav";
    fs::path hat = temp.path() / "Hat.wav";
    fs::path loop = temp.path() / "Loop.wav";
    media::AnalysisCache cache{temp.path() / "analysed"};
    audio::DecodeCache decode{temp.path() / "decoded"};
    library::SampleAnalysisService analysis{cache, decode};

    Fixture() {
        writeSample(kick, rate / 4, 75, 1);
        writeSample(neighbor, rate / 4, 92, 1);
        writeSample(hat, rate / 4, 6000, 1);
        writeSample(loop, rate * 4, 95, 8);
    }
};

void measureAndRead(Fixture& fixture) {
    auto kick = fixture.analysis.analyze(fixture.kick);
    auto neighbor = fixture.analysis.analyze(fixture.neighbor);
    auto hat = fixture.analysis.analyze(fixture.hat);
    auto loop = fixture.analysis.analyze(fixture.loop);
    check(kick && neighbor && hat && loop, "file decoder publishes four analysed fixtures");
    if (!kick || !neighbor || !hat || !loop) return;

    using audio::SampleKind;
    check(kick.analysis->classification.kind == SampleKind::OneShot &&
          loop.analysis->classification.kind == SampleKind::Loop && loop.analysis->classification.bars == 2,
          "fixture type and bar length come from one classifier");
    check(loop.analysis->tempoBpm && std::abs(*loop.analysis->tempoBpm - 120.0) < 0.001 &&
          loop.analysis->lengthSeconds == 4.0 && loop.analysis->descriptors.hitDensityHz == 2.0f &&
          loop.analysis->similarityPrefix && loop.analysis->similarityPrefix->hitDensityHz == 2.0f,
          "120 BPM loop has four seconds, two hits per second and a two-second similarity prefix");
    check(samplePreviewLoops(*loop.analysis) && !samplePreviewLoops(*kick.analysis),
          "preview looping consumes the same result as type filters");
    std::printf("PCM fixtures BPM %.4f, kick %.2f Hz, hat %.2f Hz, loop %.2f hits/s\n",
                loop.analysis->tempoBpm.value_or(0), static_cast<double>(kick.analysis->descriptors.brightnessHz),
                static_cast<double>(hat.analysis->descriptors.brightnessHz),
                static_cast<double>(loop.analysis->descriptors.hitDensityHz));

    const std::array<const library::SampleAnalysis*, 4> matched{
        &*hat.analysis, &*loop.analysis, &*neighbor.analysis, &*kick.analysis};
    std::vector<library::SampleQueryMatch> results;
    library::SampleQuery similar;
    similar.rank = library::SampleQueryRank::Similar;
    similar.reference = &*kick.analysis;
    library::querySamples(matched, similar, results);
    check(results.size() == 3 && results.front().row == 2 && results.back().row == 0,
          "decoded kick ranks another kick ahead of the decoded hat");

    library::SampleQuery filters;
    using Range = library::SampleQueryRange;
    filters.kind = SampleKind::Loop;
    filters.lengthSeconds = Range{3.99, 4.01};
    filters.tempoBpm = Range{119.9, 120.1};
    filters.brightnessHz = Range{50, 250};
    filters.hitsPerSecond = Range{1.99, 2.01};
    auto seeded = library::applySampleOverrides(*loop.analysis, {{}, library::SampleKey{4, true}, {}});
    filters.key = library::SampleKey{4, true};
    const std::array<const library::SampleAnalysis*, 4> withUserKey{
        &*hat.analysis, &seeded, &*neighbor.analysis, &*kick.analysis};
    library::querySamples(withUserKey, filters, results);
    check(results.size() == 1 && results[0].row == 1, "six measured filters intersect in the fixture listing");

    const auto warm = fixture.analysis.read(loop.hash);
    check(warm && warm.fromCache && warm.analysis->similarityPrefix &&
          warm.analysis->descriptors.spectralEnergy == loop.analysis->descriptors.spectralEnergy &&
          warm.analysis->tempoBpm == loop.analysis->tempoBpm, "ADIA hit reopens measurements and similarity prefix");
    const auto record = fixture.cache.readByHash(loop.hash);
    check(record && record.data->sections.contains("ALIB"), "library data lives in its named ADIA section");
    if (record) {
        std::string error;
        check(fixture.cache.update(record.data->identity, "ALIB", {99, 0, {}}, error) ==
              media::AnalysisCacheProblem::None, "replace the fixture's section with an unsupported revision");
        check(fixture.cache.update(record.data->identity, "TEST", {1, 0, {std::byte{0x25}}}, error) ==
              media::AnalysisCacheProblem::None, "put an unrelated section in the same cache");
        const auto rebuilt = fixture.analysis.analyze(fixture.loop, loop.hash);
        const auto after = fixture.cache.readByHash(loop.hash);
        check(rebuilt && !rebuilt.fromCache && after && after.data->sections.contains("TEST"),
              "unknown section revision is recomputed while unrelated analysis survives");
        {
            std::fstream damaged(fixture.cache.folder() / (loop.hash + ".adia"), std::ios::binary | std::ios::in | std::ios::out);
            damaged.seekp(72); // a different positive rate in the ADIA envelope
            damaged.put(static_cast<char>(1));
            damaged.close();
        }
        const auto recovered = fixture.analysis.analyze(fixture.loop, loop.hash);
        check(recovered && !recovered.fromCache && recovered.analysis->lengthSeconds == 4.0,
              "a changed envelope rate with an intact digest cannot publish a wrong length");
    }

    std::stop_source cancelled;
    cancelled.request_stop();
    check(fixture.analysis.analyze(fixture.loop, loop.hash, cancelled.get_token()).problem ==
          library::SampleAnalysisProblem::Cancelled, "pre-cancellation takes precedence over a warm cache");
    auto callback = library::SampleAnalysisProblem::None;
    const auto ioBefore = audio::callbackFileIo.load();
    {
        audio::CallbackScope scope;
        callback = fixture.analysis.analyze(fixture.loop, loop.hash).problem;
    }
    check(callback == library::SampleAnalysisProblem::Realtime && audio::callbackFileIo.load() == ioBefore,
          "audio callback refuses before I/O");
}

void catalogAndRevision(Fixture& fixture) {
    const auto digest = media::blake3File(fixture.kick);
    check(static_cast<bool>(digest), "registered source has a content identity");
    if (!digest) return;
    std::string error;
    auto index = library::Index::open(fixture.temp.path() / "library.sqlite", error, false);
    check(index && index->importJson("{\"version\":1,\"media\":{\"" + digest.hex +
          "\":{\"bpm\":97,\"tags\":[\"favorite\"]}}}", error), "store a user override in the existing index");
    if (!index) return;

    library::SampleAnalysisCatalog catalog(fixture.cache, fixture.decode, index.get());
    const std::array<std::string, 1> shown{"hat-v1"};
    catalog.replaceFiles({{fixture.kick, digest.hex, "kick"}, {fixture.hat, {}, "hat-v1"}}, shown);
    catalog.worker().resume();
    check(catalog.worker().waitIdle(), "catalog worker finishes a hash-eager and a hash-late item");
    std::vector<std::shared_ptr<const library::SampleAnalysis>> records;
    const std::array before{library::SampleAnalysisLookup{"kick", digest.hex},
                            library::SampleAnalysisLookup{"hat-v1", {}}};
    catalog.read(before, records);
    check(records.size() == 2 && records[0] && records[0]->tempoBpm == 97 &&
          records[0]->tempoSource == audio::SampleKindSource::User &&
          records[0]->tags == std::vector<std::string>{"favorite"} && records[1] &&
          records[1]->contentHash == media::blake3File(fixture.hat).hex,
          "catalog resolves both identities and overlays authored tempo/tags after reading ADIA");
    const auto raw = fixture.analysis.read(digest.hex);
    check(raw && !raw.analysis->tempoBpm && raw.analysis->tags.empty(), "authored values stay out of the evictable cache");

    catalog.worker().pause();
    writeSample(fixture.hat, rate / 2, 8500, 1);
    catalog.replaceFiles({{fixture.hat, {}, "hat-v2"}});
    const std::array after{library::SampleAnalysisLookup{"hat-v2", {}}, before[1]};
    catalog.read(after, records);
    check(records.size() == 2 && !records[0] && !records[1], "new listing revision cannot expose stale measurements");
    catalog.worker().resume();
    check(catalog.worker().waitIdle(), "changed hash is indexed after resuming");
    catalog.read(after, records);
    check(records[0] && records[0]->lengthSeconds == 0.5 && !records[1],
          "changed bytes recompute under the new identity");
    const std::string wrong(64, 'a');
    check(fixture.analysis.analyze(fixture.hat, wrong).problem == library::SampleAnalysisProblem::SourceChanged,
          "a stale published hash cannot receive another source's measurements");
}

void warpUsesPublishedAnswer(Fixture& fixture) {
    auto loop = fixture.analysis.analyze(fixture.loop);
    auto kick = fixture.analysis.analyze(fixture.kick);
    if (!loop || !kick) { check(false, "classification to feed Warp"); return; }

    StoreError error{};
    auto store = Store::createEmpty(fixture.temp.path() / "warp.adi", error);
    if (!store) { check(false, "create project for Warp seam"); return; }
    store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Samples')");
    clip_edit::ImportOptions options;
    options.track = 1;
    options.clipId = 7;
    options.mediaId = 8;
    options.at = {0, 0};
    options.source = fixture.kick;
    options.warp = true;
    options.shortSample = audio::ShortSampleWarpInput{"auto", kick.analysis->classification, kick.hash};
    auto imported = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, imported).ok &&
          store->db().execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=7").getInt() == 0,
          "explicit auto import of the measured one-shot stays clean");
    clip_edit::WarpOptions warp;
    warp.shortSample = options.shortSample;
    const auto ignored = clip_edit::prepareSetWarp(*store, 7, warp, rate);
    check(ignored.ok && ignored.requests.empty(), "Warp press consumes the same one-shot answer and emits no edit");

    options.clipId = 9;
    options.mediaId = 10;
    options.source = fixture.loop;
    options.warp = false;
    options.shortSample = audio::ShortSampleWarpInput{"auto", loop.analysis->classification, loop.hash};
    imported = clip_edit::prepareImportAudio(*store, options, rate);
    if (!clip_edit::commitMaterial(*store, imported).ok) { check(false, "clean loop import"); return; }
    check(store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=9").getInt() == 0,
          "ordinary drop remains clean even with a measured loop");
    warp.shortSample->contentHash = kick.hash;
    check(!clip_edit::prepareSetWarp(*store, 9, warp, rate).ok, "Warp press refuses a stale measurement hash");
    warp.shortSample = options.shortSample;
    const auto prepared = clip_edit::prepareSetWarp(*store, 9, warp, rate);
    const auto committed = clip_edit::commitPrepared(*store, prepared);
    check(committed.ok &&
          store->db().execAndGet("SELECT loop_len_ticks FROM clips WHERE id=9").getInt64() == 8 * textproj::kPPQ,
          "explicit Warp uses the measured two-bar loop length");
    if (committed.ok) {
        SQLite::Statement query(store->db(), "SELECT warp_markers FROM audio_clips WHERE clip_id=9");
        if (query.executeStep()) {
            const auto blob = query.getColumn(0);
            const auto map = audio::readWarpMap(std::span(static_cast<const std::byte*>(blob.getBlob()),
                static_cast<std::size_t>(blob.getBytes())), rate * 4);
            check(map && map.markers.size() == 2 && map.markers.back().sourceFrame == rate * 4,
                  "Warp captures exactly two full-source anchors");
        }
        check(History(*store).undo().ok && store->db().execAndGet("SELECT loop_enabled FROM clips WHERE id=9").getInt() == 0,
              "undo restores the untouched clean clip");
    }
    options.clipId = 11;
    options.warp = true;
    options.shortSample->contentHash = kick.hash;
    check(!clip_edit::prepareImportAudio(*store, options, rate).edit.ok,
          "Warp import refuses a stale measurement hash");
    options.shortSample->contentHash = loop.hash;
    options.warpAnalysis.sourceBpm = 60;
    imported = clip_edit::prepareImportAudio(*store, options, rate);
    check(clip_edit::commitMaterial(*store, imported).ok &&
          store->db().execAndGet("SELECT loop_len_ticks FROM clips WHERE id=11").getInt64() == 4 * textproj::kPPQ,
          "explicit source tempo can choose a different permitted bar count on import");
}
}

int main() {
    try {
        Fixture fixture;
        measureAndRead(fixture);
        catalogAndRevision(fixture);
        warpUsesPublishedAnswer(fixture);
    } catch (const std::exception& exception) {
        check(false, exception.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
