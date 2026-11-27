// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/library/browser_sample_query.hpp"
#include "adi/library/index.hpp"
#include "adi/library/sample_analysis_catalog.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"

#if defined(ADI_TEST_BROWSER_SAMPLE_QUERY_REMOTE)
#include "adi/ui/library_query.hpp"
#endif

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numbers>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::library;
namespace fs = std::filesystem;

constexpr std::uint32_t sampleRate = 48000;
int failures = 0;

void check(bool passes, const char* description) {
    if (!passes) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

#if defined(ADI_TEST_BROWSER_SAMPLE_QUERY_REMOTE)
using FilenameIndex = ui::BrowserLibrarySnapshot;
using WordQuery = ui::LibraryQuery;
using Listing = ui::LibraryListing;
using Place = ui::LibraryPlace;
#else
// Test the adapter against the public engine-only Browser interface while
// the production filename index is carried on the adjacent Browser branch.
class WordQuery {
public:
    explicit WordQuery(std::string_view text) {
        while (!text.empty()) {
            const auto space = text.find(' ');
            words_.emplace_back(text.substr(0, space));
            if (space == std::string_view::npos) break;
            text.remove_prefix(space + 1);
        }
    }
    [[nodiscard]] bool empty() const { return words_.empty(); }
    [[nodiscard]] bool matches(std::string_view name, std::string_view tags = {}) const {
        return std::ranges::all_of(words_, [&](const auto& word) {
            return name.find(word) != std::string_view::npos || tags.find(word) != std::string_view::npos;
        });
    }
private:
    std::vector<std::string> words_;
};

struct Place { std::string name; fs::path root; std::string volumeId; };
struct File {
    std::string name;
    fs::path path;
    std::string volumeId;
    [[nodiscard]] bool isSample() const { return path.extension() == ".wav"; }
};
struct Listing { std::vector<Place> places; std::vector<File> files; };

class FilenameIndex {
public:
    struct Folder { std::vector<std::size_t> files; };
    explicit FilenameIndex(Listing listing) : listing_(std::move(listing)) {
        for (const auto& place : listing_.places) {
            auto& rows = folders_[place.volumeId].emplace_back().files;
            for (std::size_t row = 0; row < listing_.files.size(); ++row) {
                const auto relative = listing_.files[row].path.lexically_relative(place.root);
                if (!relative.empty() && *relative.begin() != "..") rows.push_back(row);
            }
        }
    }
    [[nodiscard]] const Listing& listing() const { return listing_; }
    [[nodiscard]] std::span<const Folder> folders(const std::string& place) const {
        if (const auto found = folders_.find(place); found != folders_.end()) return found->second;
        return {};
    }
    void appendSamples(const WordQuery& words, const std::string* place, std::vector<const File*>& results) const {
        const auto append = [&](const auto& file) {
            if (file.isSample() && words.matches(file.name)) results.push_back(&file);
        };
        if (!place) {
            for (const auto& file : listing_.files) append(file);
        } else {
            for (const auto& folder : folders(*place))
                for (const auto row : folder.files) append(listing_.files[row]);
        }
    }
private:
    Listing listing_;
    std::map<std::string, std::vector<Folder>> folders_;
};
#endif

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

Listing listFiles(const std::vector<Place>& places) {
    Listing listing;
    listing.places = places;
    for (auto& place : listing.places) {
#if defined(ADI_TEST_BROWSER_SAMPLE_QUERY_REMOTE)
        auto files = ui::readUnindexedPlace(place, {});
        listing.files.insert(listing.files.end(), std::make_move_iterator(files.begin()),
                             std::make_move_iterator(files.end()));
#else
        for (const auto& entry : fs::recursive_directory_iterator(place.root)) {
            if (entry.is_regular_file())
                listing.files.push_back({entry.path().filename().string(), entry.path(), place.volumeId});
        }
#endif
    }
    std::ranges::sort(listing.files, {}, [](const auto& file) { return file.name; });
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
    const auto before = media::blake3File(kick);
    if (!before) throw std::runtime_error("tagged fixture did not hash");

    const std::string readOnlyId = "folder:read-only";
    const std::vector<Place> places{{"Read only", readOnly, readOnlyId}, {"Other", other, "folder:other"}};
    auto browser = std::make_shared<const FilenameIndex>(listFiles(places));
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
        if (file.name != "Pending.wav") jobs.push_back({file.path, {}, file.name});
    }
    catalog.replaceFiles(std::move(jobs));
    catalog.worker().resume();
    if (!catalog.worker().waitIdle()) throw std::runtime_error("fixture catalog did not finish");
    std::vector<std::shared_ptr<const SampleAnalysis>> records;
    catalog.read(lookups, records);
    const BrowserSampleQuery query(browser, std::move(records));
    BrowserSampleQuery<FilenameIndex>::Results browserResults, floatingResults;
    const auto names = [&](const auto& results) {
        std::vector<std::string> output;
        for (const auto& match : results.matches) output.push_back(browser->listing().files[match.row].name);
        return output;
    };

    std::vector<const BrowserSampleQuery<FilenameIndex>::File*> originalWordMatches;
    browser->appendSamples(WordQuery("Percussion Kick"), nullptr, originalWordMatches);
    check(originalWordMatches.empty(), "the original name index alone has no match for the embedded term");
    query.query(WordQuery("Percussion Kick"), nullptr, {}, browserResults);
    query.query(WordQuery("Kick Percussion"), nullptr, {}, floatingResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav", "Kick B.wav"} &&
          names(browserResults) == names(floatingResults), "Browser and floating queries join independent name and embedded tag words");
    query.query(WordQuery("Percussion Favorite"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "authored and embedded tags are searchable in a read-only Place");
    query.query(WordQuery("Percussion"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "tag candidates respect existing nested Place membership");

#if defined(ADI_TEST_BROWSER_SAMPLE_QUERY_REMOTE)
    query.query(WordQuery("caf\xC3\xA9 FAVORITE"), &readOnlyId, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "canonical Unicode folding applies to tag fields");
#endif

    query.query(WordQuery("Kick"), nullptr, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav", "Kick B.wav"}, "filename and tag matches merge once in listing order");
    query.query(WordQuery("Pending"), nullptr, {}, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Pending.wav"}, "filename discovery does not wait for analysis");

    SampleQuery byTone;
    byTone.brightnessHz = SampleQueryRange{0, 500};
    byTone.kind = audio::SampleKind::OneShot;
    query.query(WordQuery("Favorite"), nullptr, byTone, browserResults);
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "catalog tag candidates flow through the same measured query");
    const auto after = media::blake3File(kick);
    check(after && after.hex == before.hex && !fs::exists(readOnly / ".adi-volume-id") && index->entries().empty(),
          "read-only tag search changes neither audio bytes nor the Place's index membership");

    const WordQuery empty("");
    {
        audio::CallbackScope callback;
        query.query(empty, nullptr, {}, browserResults);
    }
    check(names(browserResults) == std::vector<std::string>{"Kick A.wav"}, "the adapter refuses before changing caller storage on the audio callback");

    catalog.worker().pause();
    writeSample(kick, 6500, "Cymbals");
    const auto revisedBrowser = std::make_shared<const FilenameIndex>(listFiles(places));
    const std::array revisedLookups{SampleAnalysisLookup{"kick-v2", {}}};
    catalog.replaceFiles({{kick, {}, "kick-v2"}});
    catalog.read(revisedLookups, records);
    check(records.size() == 1 && !records[0], "rescan cannot publish stale descriptors under a new listing revision");
    catalog.worker().resume();
    if (!catalog.worker().waitIdle()) throw std::runtime_error("revised file did not finish");
    catalog.read(revisedLookups, records);
    const BrowserSampleQuery revised(revisedBrowser, std::move(records));
    revised.query(WordQuery("Cymbals"), &readOnlyId, {}, browserResults);
    check(browserResults.matches.size() == 1 && browserResults.matches.front().row == 0 &&
          query.analysis(0) && revised.analysis(0) &&
          revised.analysis(0)->contentHash != query.analysis(0)->contentHash &&
          revised.analysis(0)->descriptors.spectralEnergy != query.analysis(0)->descriptors.spectralEnergy &&
          revised.analysis(0)->descriptors.brightnessHz > 6000.f,
          "a changed content hash recomputes spectral descriptors and replaces searchable tags");
    revised.query(WordQuery("Favorite"), nullptr, {}, browserResults);
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
} // namespace

int main() {
    try {
        searchableReadOnlyTags();
        measuredFits();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return failures == 0 ? 0 : 1;
}
