// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/library/browser_sample_publication.hpp"
#include "adi/library/browser_sample_query.hpp"
#include "adi/library/index.hpp"
#include "adi/library/sample_analysis_catalog.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/ui/library_query.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::library;
using ui::BrowserLibrarySnapshot;
using ui::LibraryListing;
using ui::LibraryPlace;
using ui::LibraryQuery;
namespace fs = std::filesystem;

constexpr std::uint32_t sampleRate = 48000;
int failures = 0;

void check(bool passes, const char* description) {
    if (!passes) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void bytes32(std::string& bytes, std::size_t value, bool littleEndian) {
    for (std::size_t i = 0; i < 4; ++i) {
        const auto shift = 8 * (littleEndian ? i : 3 - i);
        bytes.push_back(static_cast<char>((value >> shift) & 255u));
    }
}

void writeSample(const fs::path& path, double frequency, std::string_view genre = {},
                 std::uint32_t frames = sampleRate / 4, std::uint32_t hits = 1, double gain = 1.0) {
    std::vector<float> samples(frames);
    const auto spacing = frames / hits;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto time = static_cast<double>(i % spacing) / sampleRate;
        samples[i] = static_cast<float>(gain * std::exp(-time * 130.0) * std::sin(2.0 * std::numbers::pi * frequency * time));
    }
    audio::WavWriter writer(path, sampleRate, 1, audio::WavFormat::Pcm24);
    writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    writer.close();
    if (genre.empty()) return;

    // Reopen a real WAV with a small embedded UTF-8 ID3 genre frame.
    std::ifstream input(path, std::ios::binary);
    std::string wave{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    input.close();
    std::string tagFrame = "TCON";
    bytes32(tagFrame, genre.size() + 1, false);
    tagFrame.append("\0\0\3", 3);
    tagFrame.append(genre);
    std::string id3{"ID3\3\0\0", 6};
    bytes32(id3, tagFrame.size(), false); // fixture frame is below 128 bytes
    id3.append(tagFrame);
    wave.append("id3 ");
    bytes32(wave, id3.size(), true);
    wave.append(id3);
    if (id3.size() % 2 != 0) wave.push_back('\0');
    std::string size;
    bytes32(size, wave.size() - 8, true);
    wave.replace(4, size.size(), size);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(wave.data(), static_cast<std::streamsize>(wave.size()));
    if (!output) throw std::runtime_error("could not write tagged WAV fixture");
}

LibraryListing listFiles(const std::vector<LibraryPlace>& places) {
    LibraryListing listing;
    listing.places = places;
    for (auto& place : listing.places) {
        auto files = ui::readUnindexedPlace(place, {});
        listing.files.insert(listing.files.end(), std::make_move_iterator(files.begin()),
                             std::make_move_iterator(files.end()));
    }
    ui::sortLibraryFiles(listing.files);
    return listing;
}

void searchableReadOnlyTags() {
    test::TempDirectory temp{"browser_sample_query", "tags"};
    const auto readOnly = temp.path() / "read-only";
    const auto other = temp.path() / "other";
    const auto nested = readOnly / "nested";
    fs::create_directories(nested);
    fs::create_directory(other);
    const auto kick = nested / "Kick A.wav";
    writeSample(kick, 75, "Percussion CAFE\xCC\x81");
    writeSample(other / "Kick B.wav", 95, "Percussion");
    writeSample(other / "Pending.wav", 4000);
    constexpr std::array<char, 26> midi{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, static_cast<char>(0xff), 0x2f, 0};
    {
        std::ofstream pattern(nested / "Kick Pattern.SMF", std::ios::binary);
        pattern.write(midi.data(), static_cast<std::streamsize>(midi.size()));
        if (!pattern) throw std::runtime_error("could not write MIDI fixture");
    }
    const auto before = media::blake3File(kick);
    if (!before) throw std::runtime_error("tagged fixture did not hash");

    const std::string readOnlyId = "folder:read-only";
    const std::vector<LibraryPlace> places{{"Read only", readOnly, readOnlyId}, {"Other", other, "folder:other"}};
    auto browser = std::make_shared<const BrowserLibrarySnapshot>(listFiles(places));
    std::string error;
    auto index = Index::open(temp.path() / "library.sqlite", error, false);
    if (!index || !index->importJson("{\"version\":1,\"media\":{\"" + before.hex +
                                    "\":{\"tags\":[\"Favorite\",\"Kick\"]}}}", error))
        throw std::runtime_error(error);

    media::AnalysisCache cache(temp.path() / "analyzed");
    audio::DecodeCache decode(temp.path() / "decoded");
    SampleAnalysisCatalog catalog(cache, decode, index.get());
    std::vector<AnalysisJob> jobs;
    std::vector<SampleAnalysisLookup> lookups;
    for (const auto& file : browser->listing().files) {
        // Unindexed Places have path-and-revision keys and no pre-scanned hash.
        lookups.push_back({file.name, {}});
        if (file.isSample() && file.name != "Pending.wav") jobs.push_back({file.path, {}, file.name});
    }
    catalog.replaceFiles(std::move(jobs));
    catalog.worker().resume();
    if (!catalog.worker().waitIdle()) throw std::runtime_error("fixture catalog did not finish");
    std::vector<std::shared_ptr<const SampleAnalysis>> records;
    catalog.read(lookups, records);
    const BrowserSampleQuery query(browser, std::move(records));
    BrowserSampleQuery<BrowserLibrarySnapshot>::Results browserResults, floatingResults;
    const auto names = [&](const auto& results) {
        std::vector<std::string> output;
        for (const auto& match : results.matches) output.push_back(browser->listing().files[match.row].name);
        return output;
    };

    std::vector<const BrowserSampleQuery<BrowserLibrarySnapshot>::File*> originalWordMatches;
    browser->appendSamples(LibraryQuery("Percussion Kick"), nullptr, originalWordMatches);
    check(originalWordMatches.empty(), "the original name index alone has no match for the embedded term");
    query.query(LibraryQuery("Percussion Kick"), nullptr, {}, browserResults);
    query.query(LibraryQuery("Kick Percussion"), nullptr, {}, floatingResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav", "Kick B.wav"} &&
          names(browserResults) == names(floatingResults), "Browser and floating queries join independent name and embedded tag words");
    query.query(LibraryQuery("Percussion Favorite"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "authored and embedded tags are searchable in a read-only Place");
    query.query(LibraryQuery("Percussion"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "tag candidates respect existing nested Place membership");

    query.query(LibraryQuery("caf\xC3\xA9 FAVORITE"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "canonical Unicode folding applies to tag fields");

    const LibraryQuery midiWords("pattern\tKICK");
    std::vector<const ui::LibraryFile*> placeMatches;
    browser->appendFiles(midiWords, &readOnlyId, placeMatches);
    query.query(midiWords, &readOnlyId, {}, browserResults);
    check(placeMatches.size() == 1 && placeMatches.front()->kind == ui::LibraryFile::Kind::Midi &&
          browserResults.matches.empty(), "the real Place index finds a nested SMF while sample search excludes it");

    query.query(LibraryQuery("Kick"), nullptr, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav", "Kick B.wav"}, "filename and tag matches merge once in listing order");
    query.query(LibraryQuery("Pending"), nullptr, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Pending.wav"}, "filename discovery does not wait for analysis");

    SampleQuery byTone;
    byTone.brightnessHz = SampleQueryRange{0, 500};
    byTone.kind = audio::SampleKind::OneShot;
    query.query(LibraryQuery("Favorite"), nullptr, byTone, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "catalog tag candidates flow through the same measured query");
    const auto after = media::blake3File(kick);
    check(after && after.hex == before.hex && !fs::exists(readOnly / ".adi-volume-id") && index->entries().empty(),
          "read-only tag search changes neither audio bytes nor the Place's index membership");

    const LibraryQuery empty("");
    {
        audio::CallbackScope callback;
        query.query(empty, nullptr, {}, browserResults);
    }
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "the adapter refuses before changing caller storage on the audio callback");

    catalog.worker().pause();
    writeSample(kick, 6500, "Cymbals");
    const auto revisedBrowser = std::make_shared<const BrowserLibrarySnapshot>(listFiles(places));
    const std::array revisedLookups{SampleAnalysisLookup{"kick-v2", {}}};
    catalog.replaceFiles({{kick, {}, "kick-v2"}});
    catalog.read(revisedLookups, records);
    check(records.size() == 1 && !records[0], "rescan cannot publish stale descriptors under a new listing revision");
    catalog.worker().resume();
    if (!catalog.worker().waitIdle()) throw std::runtime_error("revised file did not finish");
    catalog.read(revisedLookups, records);
    const BrowserSampleQuery revised(revisedBrowser, std::move(records));
    revised.query(LibraryQuery("Cymbals"), &readOnlyId, {}, browserResults);
    check(browserResults.matches.size() == 1 && browserResults.matches.front().row == 0 &&
          query.analysis(0) && revised.analysis(0) &&
          revised.analysis(0)->contentHash != query.analysis(0)->contentHash &&
          revised.analysis(0)->descriptors.spectralEnergy != query.analysis(0)->descriptors.spectralEnergy &&
          revised.analysis(0)->descriptors.brightnessHz > 6000.f,
          "a changed content hash recomputes spectral descriptors and replaces searchable tags");
    revised.query(LibraryQuery("Favorite"), nullptr, {}, browserResults);
    check(browserResults.matches.empty(), "an authored tag stays bound to the old content hash");
}

void measuredFits() {
    test::TempDirectory temp{"browser_sample_query", "fits"};
    const auto bass = temp.path() / "Bass loop.wav";
    const auto secondBass = temp.path() / "Second bass.wav";
    const auto hat = temp.path() / "Hat.wav";
    writeSample(bass, 75, {}, sampleRate * 2, 4);
    writeSample(secondBass, 75, {}, sampleRate / 4, 1, 0.25);
    writeSample(hat, 6000, {}, sampleRate / 4, 1, 0.25);
    media::AnalysisCache cache(temp.path() / "analyzed");
    audio::DecodeCache decode(temp.path() / "decoded");
    SampleAnalysisService service(cache, decode);
    auto reference = service.analyze(bass);
    auto bassHit = service.analyze(secondBass);
    auto hats = service.analyze(hat);
    if (!reference || !bassHit || !hats) throw std::runtime_error("Fits fixture did not analyze");
    reference.analysis->key = bassHit.analysis->key = hats.analysis->key = SampleKey{0, false};
    auto tritone = *hats.analysis;
    tritone.key = SampleKey{6, false};
    const std::array<const SampleAnalysis*, 3> candidates{&*bassHit.analysis, &tritone, &*hats.analysis};
    SampleQuery fits;
    fits.rank = SampleQueryRank::Fits;
    fits.reference = &*reference.analysis;
    std::vector<SampleQueryMatch> first, repeated;
    querySamples(candidates, fits, first);
    querySamples(candidates, fits, repeated);
    check(first.size() == 3 && first[0].row == 2 && first.back().row == 0,
          "a measured hat in key fits the bass-heavy loop better than the tritone hat and second bass");
    check(std::ranges::equal(first, repeated, [](const auto& a, const auto& b) {return a.row == b.row && a.score == b.score;}),
          "decoded Fits results repeat with exactly the same ordering and scores");
    if (first.size() == 3)
        std::printf("Decoded Fits scores: in-key hat %.3f, tritone hat %.3f, second bass %.3f\n",
                    first[0].score, first[1].score, first[2].score);
}

void combinedSamples() {
    test::TempDirectory temp{"browser_sample_query", "combined"};
    const auto referencePath = temp.path() / "original.wav";
    const auto hat = temp.path() / "hat.wav";
    const auto alias = temp.path() / "hat-copy.wav";
    const auto nextKick = temp.path() / "next-kick.wav";
    writeSample(referencePath, 74, "Drums");
    writeSample(hat, 6200, "Drums");
    writeSample(nextKick, 91, "Drums");
    fs::copy_file(hat, alias);

    media::AnalysisCache cache(temp.path() / "analyzed");
    audio::DecodeCache decode(temp.path() / "decoded");
    SampleAnalysisService service(cache, decode);
    const auto measure = [&](const fs::path& path) {
        auto result = service.analyze(path);
        if (!result) throw std::runtime_error("combined fixture did not analyze");
        return std::make_shared<const SampleAnalysis>(std::move(*result.analysis));
    };
    const auto reference = measure(referencePath);
    const auto hatRecord = measure(hat);
    const auto aliasRecord = measure(alias);
    const auto kickRecord = measure(nextKick);

    LibraryListing sharedFiles;
    sharedFiles.files = {{"Hat A.wav", hat, {}}, {"Hat B copy.wav", alias, {}},
                         {"Kick Library.wav", fs::canonical(referencePath), {}}, {"Pending.wav", temp.path() / "pending.wav", {}}};
    LibraryListing projectFiles;
    projectFiles.files = {{"Kick Current Project.wav", fs::canonical(referencePath), {}},
                          {"Kick Next.wav", nextKick, {}}};
    auto sharedBrowser = std::make_shared<const BrowserLibrarySnapshot>(std::move(sharedFiles));
    auto projectBrowser = std::make_shared<const BrowserLibrarySnapshot>(std::move(projectFiles));
    using Search = BrowserSampleQuery<BrowserLibrarySnapshot>;
    const Search shared(sharedBrowser, {hatRecord, aliasRecord, reference, nullptr});
    const Search project(projectBrowser, {nullptr, kickRecord});
    Search::CombinedResults results;
    const auto names = [&] {
        std::vector<std::string> output;
        for (const auto& hit : results.matches) output.push_back(hit.file->name);
        return output;
    };
    shared.queryCombined(LibraryQuery(""), &project, {}, results);
    check(names() == std::vector<std::string>{"Hat A.wav", "Hat B copy.wav", "Kick Current Project.wav", "Pending.wav", "Kick Next.wav"},
          "a matching project duplicate replaces the shared name in place, while unique project files append");
    if (results.matches.size() != 5) return;
    auto retainedReference = results.matches[2].analysis;
    check(retainedReference == reference && !results.matches[3].analysis,
          "a pending project duplicate can retain its shared measurement and an unmeasured file still matches by name");
    shared.queryCombined(LibraryQuery("Library"), &project, {}, results);
    check(names() == std::vector<std::string>{"Kick Library.wav"}, "a project display name does not displace a sole shared word match");

    SampleQuery low;
    low.brightnessHz = SampleQueryRange{0, 500};
    shared.queryCombined(LibraryQuery("Kick"), &project, low, results);
    check(names() == std::vector<std::string>{"Kick Current Project.wav", "Kick Next.wav"},
          "the joined stream applies a measured filter to both sources and honors the inherited measurement");
    shared.queryCombined(LibraryQuery("Drums Kick"), &project, {}, results);
    check(names() == std::vector<std::string>{"Kick Library.wav", "Kick Next.wav"},
          "both streams use the same indexed independent-word matching with embedded tags");

    const Search rescanned(sharedBrowser, {hatRecord, aliasRecord, std::make_shared<const SampleAnalysis>(*reference), nullptr});
    SampleQuery similar;
    similar.rank = SampleQueryRank::Similar;
    similar.reference = retainedReference.get();
    similar.limit = 2;
    rescanned.queryCombined(LibraryQuery("Drums"), &project, similar, results);
    check(names() == std::vector<std::string>{"Kick Next.wav", "Hat A.wav"},
          "one global top-K ranks a project kick ahead of shared hats and skips an equivalent reference after a rescan");

    similar.limit = sharedBrowser->listing().files.size() + projectBrowser->listing().files.size();
    rescanned.queryCombined(LibraryQuery("Drums"), &project, similar, results);
    std::vector<SimilarSampleCandidate> swaps;
    Search::makeSwapCandidates(results.matches, swaps);
    check(swaps.size() == 2 && swaps[0].file == nextKick && swaps[0].contentHash == kickRecord->contentHash &&
          swaps[1].file == hat && swaps[1].contentHash == hatRecord->contentHash,
          "ranked swaps carry exact decoded content hashes and include only the first identical file");
}

void publicationGenerations() {
    test::TempDirectory temp{"browser_sample_query", "publication"};
    const auto indexedRoot = temp.path() / "indexed";
    fs::create_directory(indexedRoot);
    const auto indexed = indexedRoot / "Indexed.wav";
    const auto indexedAlias = indexedRoot / "Indexed Alias.wav";
    const auto unindexed = temp.path() / "Unindexed.wav";
    writeSample(indexed, 81);
    fs::copy_file(indexed, indexedAlias);
    writeSample(unindexed, 97);
    const auto paths = [&] {
        LibraryListing files;
        files.files = {{"Indexed Alias.wav", indexedAlias, {}}, {"Indexed.wav", indexed, {}},
                       {"Pending.wav", temp.path() / "Pending.wav", {}}, {"Unindexed.wav", unindexed, {}}};
        return std::make_shared<const BrowserLibrarySnapshot>(std::move(files));
    };

    std::string error;
    auto index = Index::open(temp.path() / "library.sqlite", error);
    Volume volume;
    MarkerVolumeProvider provider;
    if (!index || !provider.identify(indexedRoot, volume, error) || !index->scan(volume).ok)
        throw std::runtime_error("could not index publication fixture: " + error);
    index->waitIdle();
    const auto indexedEntries = index->entries();
    if (indexedEntries.size() != 2 || indexedEntries[0].hash.empty() || indexedEntries[0].hash != indexedEntries[1].hash)
        throw std::runtime_error("fixture index did not hash its source");
    std::vector<BrowserIndexedSample> observations;
    for (const auto& entry : indexedEntries) {
        const auto path = indexedRoot / entry.relativePath;
        std::error_code revisionError;
        const auto before = media::readFileRevision(path, revisionError);
        const auto verified = media::blake3File(path);
        const auto after = media::readFileRevision(path, revisionError);
        if (!before || !after || before != after || !verified || verified.hex != entry.hash)
            throw std::runtime_error("fixture observation could not verify the indexed hash");
        observations.push_back({path, entry.hash, entry.size, entry.modifiedNs, after});
    }
    const auto& entry = indexedEntries[1];
    const std::array<std::size_t, 4> shown{3, 1, 0, 99};
    media::AnalysisCache cache(temp.path() / "analyzed");
    audio::DecodeCache decode(temp.path() / "decoded");
    SampleAnalysisCatalog catalog(cache, decode, index.get());
    BrowserSamplePublication<BrowserLibrarySnapshot> publisher(catalog);
    check(publisher.replace(1, paths(), observations, shown), "aligned jobs install for the first shown generation");
    check(publisher.prioritize(1, shown), "visible row indices reprioritize the installed job keys");
    auto pending = publisher.snapshot(1);
    BrowserSampleQuery<BrowserLibrarySnapshot>::Results result;
    if (!pending) throw std::runtime_error("pending filename publication is missing");
    pending->query(LibraryQuery("Pending"), nullptr, {}, result);
    check(result.matches.size() == 1 && result.matches[0].row == 2 && !pending->analysis(2),
          "files without measurements retain their aligned filename row");
    check(catalog.worker().stats().pending == 2,
          "exact indexed observations deduplicate equal hashes, and pending filenames enqueue no work");
    catalog.worker().resume();
    check(catalog.worker().waitIdle(), "the indexed and unindexed rows finish");
    const auto original = publisher.snapshot(1);
    if (!original || !original->analysis(0) || !original->analysis(1) || !original->analysis(3))
        throw std::runtime_error("first publication did not attach both measurements");
    check(original->analysis(1)->contentHash == entry.hash && original->analysis(0) == original->analysis(1) &&
          publisher.snapshot(1) == original && !pending->analysis(1) && original->browser()->listing().files.size() == 4,
          "a current indexed record attaches without mutating the retained pending publication");

    // Both jobs race a same-size replacement. The indexed job can see a warm
    // cache hit while the unindexed job reads the new bytes under an old key.
    catalog.worker().pause();
    check(publisher.replace(2, paths(), observations, shown), "the second listing keeps the captured file revisions");
    const auto indexedStamp = fs::last_write_time(indexed);
    const auto unindexedStamp = fs::last_write_time(unindexed);
    const auto replacement = temp.path() / "replacement.wav";
    writeSample(replacement, 6700);
    writeSample(unindexed, 7800);
    fs::last_write_time(replacement, indexedStamp);
    fs::remove(indexed);
    fs::rename(replacement, indexed);
    fs::last_write_time(unindexed, unindexedStamp + std::chrono::seconds(1));
    check(fs::file_size(indexed) == entry.size && fs::file_size(unindexed) == entry.size,
          "the raced source edits change revision and keep the same WAV length");
    catalog.worker().resume();
    check(catalog.worker().waitIdle(), "measurement worker gets through the raced requests");
    const auto raced = publisher.snapshot(2);
    check(raced && raced->analysis(0) && !raced->analysis(1) && !raced->analysis(3),
          "attach-time checks reject warm old-hash data and new bytes produced for an old unindexed key");
    check(!publisher.snapshot(1) && !publisher.prioritize(1, shown) && !publisher.replace(1, paths(), observations, shown),
          "late refresh, priority and replacement cannot revive a superseded generation");

    const auto rescanned = index->scan(volume);
    const auto afterScan = index->entries();
    check(rescanned.ok && afterScan.size() == 2 && afterScan[1].hash == entry.hash,
          "the unchanged size and indexed timestamp can still carry the previous content hash");
    catalog.worker().pause();
    check(publisher.replace(3, paths(), observations, shown), "a rescan captures the exact new source revisions");
    const auto beforeResume = publisher.snapshot(3);
    check(beforeResume && beforeResume->analysis(0) && !beforeResume->analysis(1) && !beforeResume->analysis(3),
          "a changed revision starts pending instead of borrowing an old effective record");
    catalog.worker().resume();
    check(catalog.worker().waitIdle(), "the changed observations can be measured under their current revisions");
    const auto revised = publisher.snapshot(3);
    check(revised && revised->analysis(0) && revised->analysis(1) && revised->analysis(3) &&
          revised->analysis(1)->contentHash != entry.hash && revised->analysis(1)->descriptors.brightnessHz > 6000.f &&
          revised->analysis(3)->contentHash != original->analysis(3)->contentHash &&
          original->analysis(3)->descriptors.brightnessHz < 300.f,
          "the corrected publication recomputes both changed sounds and retains old snapshot measurements separately");

    std::stop_source cancelled;
    cancelled.request_stop();
    check(!publisher.replace(4, paths(), observations, shown, cancelled.get_token()) && publisher.snapshot(3) == revised,
          "a cancelled rescan cannot replace the current catalogue generation");
    bool refused = false;
    {
        audio::CallbackScope callback;
        refused = !publisher.snapshot(3) && !publisher.prioritize(3, shown);
    }
    check(refused, "audio callbacks cannot refresh or reprioritize a library publication");
}

void staleIndexBeforeFirstPublication() {
    test::TempDirectory temp{"browser_sample_query", "stale-first-publication"};
    const auto place = temp.path() / "indexed";
    fs::create_directory(place);
    const auto source = place / "Kick.wav";
    writeSample(source, 82, "Oldtag");

    std::string error;
    auto index = Index::open(temp.path() / "library.sqlite", error);
    Volume volume;
    MarkerVolumeProvider provider;
    if (!index || !provider.identify(place, volume, error) || !index->scan(volume).ok)
        throw std::runtime_error("could not index stale-hash fixture: " + error);
    index->waitIdle();
    const auto entries = index->entries();
    if (entries.size() != 1 || entries.front().hash.empty())
        throw std::runtime_error("stale-hash fixture did not index its original bytes");
    const auto& indexed = entries.front();
    media::AnalysisCache cache(temp.path() / "analyzed");
    audio::DecodeCache decode(temp.path() / "decoded");
    SampleAnalysisService service(cache, decode);
    const auto old = service.analyze(source, indexed.hash);
    if (!old || old.analysis->descriptors.brightnessHz >= 300.f)
        throw std::runtime_error("stale-hash fixture did not warm the original bass measurement");

    const auto stamp = fs::last_write_time(source);
    const auto replacement = temp.path() / "replacement.wav";
    writeSample(replacement, 6700, "Newtag");
    fs::last_write_time(replacement, stamp);
    fs::remove(source);
    fs::rename(replacement, source);
    const auto current = media::blake3File(source);
    check(current && current.hex != indexed.hash && fs::file_size(source) == indexed.size &&
          fs::last_write_time(source) == stamp && service.read(indexed.hash),
          "same-size source replacement leaves the old indexed hash and its warm ADIA record available");

    const std::array observations{BrowserIndexedSample{source, indexed.hash, indexed.size, indexed.modifiedNs}};
    SampleAnalysisCatalog catalog(cache, decode, index.get());
    BrowserSamplePublication<BrowserLibrarySnapshot> publisher(catalog);
    auto listing = std::make_shared<const BrowserLibrarySnapshot>(listFiles({{"Indexed", place, volume.id}}));
    if (!publisher.replace(1, listing, observations))
        throw std::runtime_error("could not install the first stale-hash publication");
    const auto pending = publisher.snapshot(1);
    check(pending && !pending->analysis(0), "a fresh publication cannot borrow old ADIA from an unverified index digest");
    catalog.worker().resume();
    if (!catalog.worker().waitIdle()) throw std::runtime_error("first stale-hash publication did not finish");
    const auto measured = publisher.snapshot(1);
    const auto* sound = measured ? measured->analysis(0) : nullptr;
    check(sound && sound->contentHash == current.hex && sound->descriptors.brightnessHz > 6000.f,
          "the first publication verifies the actual bytes and measures their replacement sound");
    if (measured) {
        BrowserSampleQuery<BrowserLibrarySnapshot>::Results matches;
        measured->query(LibraryQuery("Newtag Kick"), nullptr, {}, matches);
        check(matches.matches.size() == 1 && matches.matches.front().row == 0,
              "the production matcher finds the replacement file's verified embedded tag");
    }

    catalog.worker().pause();
    check(publisher.replace(2, listing, observations), "a later listing still receives the stale persisted index digest");
    const auto rescanned = publisher.snapshot(2);
    const auto* retained = rescanned ? rescanned->analysis(0) : nullptr;
    check(sound && retained && retained->contentHash == current.hex &&
          retained->descriptors.spectralEnergy == sound->descriptors.spectralEnergy,
          "an unchanged native revision reattaches the verified measurement while the worker is paused");
}
} // namespace

int main() {
    try {
        searchableReadOnlyTags();
        measuredFits();
        combinedSamples();
        publicationGenerations();
        staleIndexBeforeFirstPublication();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return failures == 0 ? 0 : 1;
}
