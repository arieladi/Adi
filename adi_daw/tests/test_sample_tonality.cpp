// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"

#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace adi::library;
int checks = 0;
int failures = 0;

void check(bool passes, const char* what) {
    ++checks;
    if (!passes) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

[[nodiscard]] double frequency(int midi) {
    return 440. * std::exp2(static_cast<double>(midi - 69) / 12.);
}

std::vector<float> cadence(SampleKey key, std::uint32_t rate) {
    const int tonic = 48 + key.tonic;
    const std::array<std::array<int, 4>, 4> chords{{
        {{0, key.minor ? 3 : 4, 7, 12}},
        {{5, key.minor ? 8 : 9, 12, 17}},
        {{7, 11, 14, 17}},
        {{0, key.minor ? 3 : 4, 7, 12}},
    }};
    const std::array<std::uint32_t, 4> noteFrames{rate / 2, rate * 3 / 8, rate * 3 / 8, rate * 3 / 4};
    std::vector<float> samples;
    samples.reserve(rate * 2);
    for (std::size_t c = 0; c < chords.size(); ++c) {
        for (std::uint32_t frame = 0; frame < noteFrames[c]; ++frame) {
            const auto time = static_cast<double>(frame) / rate;
            const auto release = static_cast<double>(noteFrames[c] - frame) / rate;
            const auto fade = std::min({1., time * 75., release * 25.});
            double sample = 0;
            for (std::size_t i = 0; i < chords[c].size(); ++i) {
                const auto radians = 2. * std::numbers::pi * frequency(tonic + chords[c][i]) * time;
                const auto amplitude = (i == 0 ? .22 : .13) * fade;
                sample += amplitude * (std::sin(radians) + .16 * std::sin(radians * 2.) + .04 * std::sin(radians * 3.));
            }
            samples.push_back(static_cast<float>(sample));
        }
    }
    return samples;
}

void keyFixtures() {
    const auto start = std::chrono::steady_clock::now();
    int exact = 0;
    float lowestStrength = 1;
    for (const bool minor : {false, true}) {
        for (std::uint8_t root = 0; root < 12; ++root) {
            const SampleKey expected{root, minor};
            const auto samples = cadence(expected, 48000);
            SampleKeyAnalyzer analyzer(48000, 1);
            bool appended = true;
            for (std::size_t offset = 0; offset < samples.size(); offset += 571) {
                appended = appended && analyzer.append(std::span(samples).subspan(offset, std::min<std::size_t>(571, samples.size() - offset)));
            }
            check(appended, "streaming accepts arbitrary whole-frame block boundaries");
            const auto detected = analyzer.finish();
            if (detected && detected->key == expected) {
                ++exact;
                lowestStrength = std::min(lowestStrength, detected->strength);
            } else {
                std::printf("MISSED %u %s, found %d %s strength %.4f\n", static_cast<unsigned>(root),
                            minor ? "minor" : "major", detected ? static_cast<int>(detected->key.tonic) : -1,
                            detected && detected->key.minor ? "minor" : "major",
                            detected ? static_cast<double>(detected->strength) : 0.);
            }
        }
    }
    const auto duration = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("Cadences detected exactly: %d/24, minimum correct strength %.3f, fixture plus analysis %.1f ms\n",
                exact, static_cast<double>(lowestStrength), duration);
    check(exact >= 22, "at least 22 of 24 chromatically transposed cadences detect the exact key");

    const SampleKey expected{9, true};
    SampleKeyAnalyzer analyzer(96000, 2);
    const auto mono = cadence(expected, 96000);
    std::vector<float> stereo;
    stereo.reserve(mono.size() * 2);
    for (const auto sample : mono) {
        stereo.push_back(sample);
        stereo.push_back(sample * .71f);
    }
    check(!analyzer.append(std::span(stereo).first(3)), "a partial PCM frame is rejected");
    check(analyzer.append(stereo), "96 kHz stereo is accepted");
    const auto detected = analyzer.finish();
    check(detected && detected->key == expected, "96 kHz stereo cadence detects the same key");
    const auto again = analyzer.finish();
    check(again && again->key == expected && !analyzer.append(stereo), "completed analyzer is stable");
    for (std::size_t frame = 0; frame < mono.size(); ++frame) stereo[2 * frame + 1] = -mono[frame];
    SampleKeyAnalyzer antiPhase(96000, 2);
    check(antiPhase.append(stereo), "out-of-phase stereo appends whole frames");
    const auto sideKey = antiPhase.finish();
    check(sideKey && sideKey->key == expected, "audible stereo content survives destructive mono folding");

    std::vector<float> samples(48000);
    SampleKeyAnalyzer silent(48000, 1);
    check(silent.append(samples) && !silent.finish(), "silence has no detected key");
    const auto step = 2. * std::numbers::pi * 440. / 48000.;
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = .4f * static_cast<float>(std::sin(static_cast<double>(i) * step));
    SampleKeyAnalyzer singleNote(48000, 1);
    check(singleNote.append(samples) && !singleNote.finish(), "one note cannot establish major or minor");
    std::uint32_t rng = 27271;
    for (auto& sample : samples) {
        rng = 1664525u * rng + 1013904223u;
        sample = static_cast<float>(rng / 2147483648. - 1.);
    }
    SampleKeyAnalyzer noise(48000, 1);
    check(noise.append(samples) && !noise.finish(), "broadband noise has no detected key");
}

void appendInteger(std::string& target, std::uint64_t value, std::size_t width, bool little) {
    for (std::size_t i = 0; i < width; ++i) {
        const auto shift = (little ? i : width - 1 - i) * 8;
        target.push_back(static_cast<char>((value >> shift) & 255u));
    }
}

void appendChunk(std::string& target, std::string_view name, const std::string& bytes, bool little) {
    target.append(name);
    appendInteger(target, bytes.size(), 4, little);
    target.append(bytes);
    if (bytes.size() % 2 != 0) target.push_back('\0');
}

void appendId3Frame(std::string& frames, std::string_view type, std::string_view value) {
    frames.append(type);
    appendInteger(frames, 1 + value.size(), 4, false);
    appendInteger(frames, 0, 2, false);
    frames.push_back(3);
    frames.append(value);
}

std::string id3Fixture() {
    std::string id3 = "ID3\x03";
    appendInteger(id3, 0, 2, false);
    std::string frames;
    appendId3Frame(frames, "TBPM", "131.5");
    appendId3Frame(frames, "TKEY", "B♭ min");
    appendId3Frame(frames, "TXXX", std::string_view("LOOP_TYPE\0One Shot", 18));
    appendId3Frame(frames, "TCON", "Drums");
    appendInteger(id3, frames.size(), 4, false); // fixture stays below 128 bytes
    id3.append(frames);
    return id3;
}

void writeFixture(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("failed to write tag fixture");
}

void wavTags(const std::filesystem::path& directory) {
    std::string riff = "WAVE";
    appendChunk(riff, "JUNK", "odd", true);
    appendChunk(riff, "data", std::string(30, '\0'), true);
    std::string acid;
    appendInteger(acid, 2, 4, true); // loop with root note
    appendInteger(acid, 61, 2, true);
    appendInteger(acid, 0, 6, true);
    appendInteger(acid, 4, 4, true);
    appendInteger(acid, 4, 2, true);
    appendInteger(acid, 4, 2, true);
    appendInteger(acid, std::bit_cast<std::uint32_t>(120.f), 4, true);
    appendChunk(riff, "acid", acid, true);
    appendChunk(riff, "id3 ", id3Fixture(), true);
    std::string wav = "RIFF";
    appendInteger(wav, riff.size(), 4, true);
    wav += riff;
    const auto path = directory / "source.wav";
    writeFixture(path, wav);
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 131.5 && tags.acidBpm == 120., "ID3 tempo and distinct ACID tempo can both be used for precedence");
    check(tags.tagKey == SampleKey{10, true} && tags.rootNote == 61, "enharmonic text key and ACID root keep their distinct meaning");
    check(tags.tagLoop == false && tags.acidLoop == true, "explicit ID3 one-shot and distinct ACID loop both survive");
    check(tags.tags == std::vector<std::string>{"Drums"}, "genre is available for matching");
    {
        const adi::audio::CallbackScope callback;
        check(!readSampleFileTags(path).tagBpm, "metadata refuses the audio callback");
    }
    std::stop_source stop;
    stop.request_stop();
    check(!readSampleFileTags(path, stop.get_token()).tagBpm, "pre-cancelled read returns no partial tags");
    wav.pop_back();
    writeFixture(path, wav);
    check(!readSampleFileTags(path).acidBpm, "truncated RIFF envelope returns no metadata");
}

void aiffTags(const std::filesystem::path& directory) {
    std::string form = "AIFF";
    std::string common;
    appendInteger(common, 1, 2, false);
    appendInteger(common, 88200, 4, false);
    appendInteger(common, 16, 2, false);
    appendInteger(common, 16398, 2, false);
    appendInteger(common, std::uint64_t{44100} << 48, 8, false);
    appendChunk(form, "COMM", common, false);
    std::string basc;
    appendInteger(basc, 1, 4, false);
    appendInteger(basc, 4, 4, false);
    appendInteger(basc, 63, 2, false);
    appendInteger(basc, 2, 2, false);
    appendInteger(basc, 4, 2, false);
    appendInteger(basc, 4, 2, false);
    appendInteger(basc, 0, 2, false);
    appendChunk(form, "basc", basc, false);
    std::string aiff = "FORM";
    appendInteger(aiff, form.size(), 4, false);
    aiff += form;
    const auto path = directory / "apple.aiff";
    writeFixture(path, aiff);
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 120. && tags.tagKey == SampleKey{3, false} && tags.tagLoop == true,
          "Apple Loop declares musical beats, root, scale and type");
}

std::string vorbisFields() {
    std::string bytes;
    appendInteger(bytes, 4, 4, true);
    bytes += "test";
    appendInteger(bytes, 4, 4, true);
    for (const std::string_view field : {"BPM=106.25 BPM", "INITIALKEY=Db Major", "LOOP_TYPE=Loop", "GENRE=Électronique"}) {
        appendInteger(bytes, field.size(), 4, true);
        bytes += field;
    }
    return bytes;
}

void commentTags(const std::filesystem::path& directory) {
    const auto comments = vorbisFields();
    std::string flac = "fLaC";
    flac.push_back(static_cast<char>(0x84));
    appendInteger(flac, comments.size(), 3, false);
    flac += comments;
    auto path = directory / "source.flac";
    writeFixture(path, flac);
    const auto flacTags = readSampleFileTags(path);
    check(flacTags.tagBpm == 106.25 && flacTags.tagKey == SampleKey{1, false} && flacTags.tagLoop == true,
          "FLAC Vorbis comments contribute measured-filter metadata");
    check(flacTags.tags == std::vector<std::string>{"Électronique"}, "UTF-8 tag text survives");

    std::string page = "OggS";
    page.resize(27, '\0');
    page[5] = 2; // first page with two packets
    page[26] = 2;
    page.push_back(7);
    page.push_back(static_cast<char>(7 + comments.size()));
    page += "\x01vorbis\x03vorbis";
    page += comments;
    path = directory / "source.ogg";
    writeFixture(path, page);
    const auto oggTags = readSampleFileTags(path);
    check(oggTags.tagBpm == flacTags.tagBpm && oggTags.tagKey == flacTags.tagKey && oggTags.tags == flacTags.tags,
          "the same Vorbis comments in Ogg give the same result");
}
} // namespace

int main() {
    keyFixtures();
    const adi::test::TempDirectory temporary("sample_tonality", "metadata");
    wavTags(temporary.path());
    aiffTags(temporary.path());
    commentTags(temporary.path());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
