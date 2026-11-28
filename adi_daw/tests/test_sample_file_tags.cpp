// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_tonality.hpp"

#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <locale>
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

void appendRawId3Frame(std::string& frames, std::string_view type, std::string_view payload, unsigned flags = 0) {
    frames.append(type);
    appendInteger(frames, payload.size(), 4, false); // lengths below 128 are also syncsafe
    appendInteger(frames, flags, 2, false);
    frames.append(payload);
}

void appendId3Frame(std::string& frames, std::string_view type, std::string_view value) {
    std::string payload(1, 3);
    payload.append(value);
    appendRawId3Frame(frames, type, payload);
}

std::string id3Tag(unsigned version, const std::string& frames) {
    std::string id3 = "ID3";
    id3.push_back(static_cast<char>(version));
    appendInteger(id3, 0, 2, false);
    appendInteger(id3, frames.size(), 4, false); // fixtures stay below 128 bytes
    id3.append(frames);
    return id3;
}

std::string id3Fixture() {
    std::string frames;
    appendId3Frame(frames, "TBPM", "131.5");
    appendId3Frame(frames, "TKEY", "B♭ min");
    appendId3Frame(frames, "TXXX", std::string_view("LOOP_TYPE\0One Shot", 18));
    appendId3Frame(frames, "TCON", "Drums");
    return id3Tag(4, frames);
}

void writeFixture(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("failed to write tag fixture");
}

std::string readFixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("failed to read tag fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string pcmRiff() {
    std::string riff = "WAVE";
    std::string format;
    appendInteger(format, 1, 2, true); // PCM
    appendInteger(format, 1, 2, true);
    appendInteger(format, 48000, 4, true);
    appendInteger(format, 96000, 4, true);
    appendInteger(format, 2, 2, true);
    appendInteger(format, 16, 2, true);
    appendChunk(riff, "fmt ", format, true);
    appendChunk(riff, "JUNK", "odd", true);
    appendChunk(riff, "data", std::string(30, '\0'), true);
    return riff;
}

std::string riffEnvelope(std::string_view type, const std::string& riff) {
    std::string wav(type);
    appendInteger(wav, riff.size(), 4, true);
    wav += riff;
    return wav;
}

void wavTags(const std::filesystem::path& directory) {
    auto riff = pcmRiff();
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
    auto wav = riffEnvelope("RIFF", riff);
    const auto path = directory / "source.wav";
    writeFixture(path, wav);
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 131.5 && tags.acidBpm == 120., "ID3 tempo and distinct ACID tempo can both be used for precedence");
    check(tags.tagKey == SampleKey{10, true} && tags.rootNote == 61, "enharmonic text key and ACID root keep their distinct meaning");
    check(tags.tagLoop == false && tags.acidLoop == true, "explicit ID3 one-shot and distinct ACID loop both survive");
    check(tags.tags == std::vector<std::string>{"Drums"}, "genre is available for matching");
    check(readFixture(path) == wav, "reading tags does not write to the source file");
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

void bpmGrammar(const std::filesystem::path& directory) {
    struct CommaDecimal final : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
    };
    struct RestoreLocale {
        std::locale previous;
        ~RestoreLocale() { std::locale::global(previous); }
    } restore{std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))};

    const auto path = directory / "tempo-grammar.wav";
    const auto parse = [&](std::string_view text) {
        std::string frames;
        appendId3Frame(frames, "TBPM", text);
        auto riff = pcmRiff();
        appendChunk(riff, "id3 ", id3Tag(4, frames), true);
        writeFixture(path, riffEnvelope("RIFF", riff));
        return readSampleFileTags(path).tagBpm;
    };
    check(parse(" 1.25E+2 bPm\t") == 125., "tempo is locale-independent with a signed exponent and mixed-case suffix");
    check(parse(".1e+2BPM") == 10. && parse("10.") == 10. && parse("9.99e2") == 999.,
          "general decimals accept either side of the point and both finite range boundaries");
    check(parse("1500e-1") == 150., "tempo supports a negative exponent");
    check(!parse("+") && !parse("+120 BPM") && !parse("BPM") && !parse(""),
          "leading plus and empty tempo fields are refused");
    check(!parse("0x1.ep+6") && !parse("120,5") && !parse("12'0") && !parse("120/1") && !parse("120 B.P.M."),
          "hexadecimal tempo, locale punctuation and invalid suffixes are refused");
    check(!parse("120e+") && !parse("120 bpm extra") && !parse("9.99") && !parse("1000") &&
              !parse("1e+999") && !parse("nan"),
          "incomplete, trailing, non-finite and out-of-range values are refused");
}

void wavSamplerTags(const std::filesystem::path& directory) {
    const auto path = directory / "sampler.wav";
    auto riff = pcmRiff();
    std::string instrument(7, '\0');
    instrument.front() = 58;
    appendChunk(riff, "inst", instrument, true);
    std::string info = "INFO";
    appendChunk(info, "IGNR", std::string("Ambient\0", 8), true);
    appendChunk(riff, "LIST", info, true);
    writeFixture(path, riffEnvelope("RIFF", riff));
    const auto instrumentTags = readSampleFileTags(path);
    check(instrumentTags.rootNote == 58 && !instrumentTags.tagKey && !instrumentTags.tagLoop,
          "WAV instrument root is a MIDI note, without manifesting as a tonal mode or musical loop");
    check(instrumentTags.tags == std::vector<std::string>{"Ambient"}, "RIFF INFO genre is available for matching");

    std::string sampler(36, '\0');
    sampler[12] = 62;
    appendChunk(riff, "smpl", sampler, true);
    writeFixture(path, riffEnvelope("RIFF", riff));
    check(readSampleFileTags(path).rootNote == 62, "WAV sampler root overrides instrument root irrespective of chunk order");

    appendChunk(riff, "id3 ", id3Fixture(), true);
    writeFixture(path, riffEnvelope("RIFF", riff));
    const auto combined = readSampleFileTags(path);
    check(combined.tagBpm == 131.5 && combined.tags == std::vector<std::string>{"Drums", "Ambient"},
          "WAV searches text in both ID3 and RIFF INFO when they coexist");
}

void rf64Tags(const std::filesystem::path& directory) {
    auto chunks = pcmRiff().substr(4);
    chunks.replace(chunks.find("data") + 4, 4, 4, static_cast<char>(255));
    appendChunk(chunks, "id3 ", id3Fixture(), true);
    std::string sizes;
    appendInteger(sizes, 4 + 8 + 28 + chunks.size(), 8, true);
    appendInteger(sizes, 30, 8, true);
    appendInteger(sizes, 15, 8, true);
    appendInteger(sizes, 0, 4, true);
    std::string wave = "WAVE";
    appendChunk(wave, "ds64", sizes, true);
    wave += chunks;
    auto bytes = riffEnvelope("RF64", wave);
    bytes.replace(4, 4, 4, static_cast<char>(255));
    const auto path = directory / "source.rf64";
    writeFixture(path, bytes);
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 131.5 && tags.tagKey == SampleKey{10, true}, "RF64 ds64 data size reaches embedded tags after PCM");
}

std::string aiffPcm() {
    constexpr std::uint32_t frames = 88200;
    std::string form = "AIFF";
    std::string common;
    appendInteger(common, 1, 2, false);
    appendInteger(common, frames, 4, false);
    appendInteger(common, 16, 2, false);
    appendInteger(common, 16398, 2, false);
    appendInteger(common, std::uint64_t{44100} << 48, 8, false);
    appendChunk(form, "COMM", common, false);
    std::string sound(8, '\0'); // SSND offset and block size
    sound.append("NAME\0\0\0\4fake", 12); // A chunk-shaped pattern inside PCM is still audio.
    sound.resize(8 + 2 * frames, '\0');
    appendChunk(form, "SSND", sound, false);
    return form;
}

std::string aiffEnvelope(const std::string& form) {
    std::string aiff = "FORM";
    appendInteger(aiff, form.size(), 4, false);
    return aiff + form;
}

void aiffTags(const std::filesystem::path& directory) {
    auto form = aiffPcm();
    appendChunk(form, "NAME", " Odd ", false);
    appendChunk(form, "AUTH", "Élodie", false);
    appendChunk(form, "NAME", "Élodie", false);
    std::string basc;
    appendInteger(basc, 1, 4, false);
    appendInteger(basc, 4, 4, false);
    appendInteger(basc, 63, 2, false);
    appendInteger(basc, 2, 2, false);
    appendInteger(basc, 4, 2, false);
    appendInteger(basc, 4, 2, false);
    appendInteger(basc, 0, 2, false);
    appendChunk(form, "basc", basc, false);
    const auto path = directory / "apple.aiff";
    writeFixture(path, aiffEnvelope(form));
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 120. && tags.tagKey == SampleKey{3, false} && tags.tagLoop == true,
          "Apple Loop declares musical beats, root, scale and type");
    check(tags.tags == std::vector<std::string>{"Odd", "Élodie"},
          "native NAME and AUTH trim, deduplicate, and cross odd padding after SSND");

    appendChunk(form, "ID3 ", id3Fixture(), false);
    const auto combinedBytes = aiffEnvelope(form);
    writeFixture(path, combinedBytes);
    const auto combined = readSampleFileTags(path);
    check(combined.tagBpm == 131.5 && combined.tagKey == SampleKey{10, true} && combined.tagLoop == false &&
              combined.rootNote == 63 && combined.tags == std::vector<std::string>{"Drums", "Odd", "Élodie"},
          "embedded ID3 supplies primary fields and native text and basc root still contribute");
    check(readFixture(path) == combinedBytes, "reading AIFF metadata preserves the complete file bytes");

    auto bounded = aiffPcm();
    appendChunk(bounded, "NAME", std::string(513, 'x'), false);
    appendChunk(bounded, "AUTH", "bad\xc0\xaf", false);
    appendChunk(bounded, "NAME", std::string("bad\0name", 8), false);
    const std::string limit(512, 'v');
    appendChunk(bounded, "AUTH", limit, false);
    appendChunk(bounded, "NAME", "Later", false);
    writeFixture(path, aiffEnvelope(bounded));
    check(readSampleFileTags(path).tags == std::vector<std::string>{limit, "Later"},
          "oversized and malformed native text is skipped and valid later chunks still parse");

    auto malformed = aiffPcm();
    appendChunk(malformed, "AUTH", "Valid", false);
    auto overruns = malformed + "NAME";
    appendInteger(overruns, 100, 4, false);
    overruns += "partial";
    writeFixture(path, aiffEnvelope(overruns));
    check(readSampleFileTags(path).tags == std::vector<std::string>{"Valid"},
          "a native text extent beyond FORM cannot turn truncated bytes into a tag");
    malformed += "NAME";
    appendInteger(malformed, 3, 4, false);
    malformed += "Odd"; // The declared FORM ends before the mandatory pad byte.
    writeFixture(path, aiffEnvelope(malformed));
    check(readSampleFileTags(path).tags == std::vector<std::string>{"Valid"},
          "an odd native chunk outside its padded FORM extent is rejected");
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
    // Preserve the mandatory 34-byte STREAMINFO from a real FLAC fixture.
    auto flac = readFixture(std::filesystem::path(ADI_METADATA_FIXTURES) / "tone16.flac").substr(0, 42);
    flac[4] = 0; // another metadata block follows
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

std::string untaggedMp3() {
    auto bytes = readFixture(std::filesystem::path(ADI_METADATA_FIXTURES) / "tone.mp3");
    if (bytes.starts_with("ID3") && bytes.size() >= 10) {
        std::size_t offset = 0;
        for (std::size_t i = 6; i < 10; ++i) offset = (offset << 7) | (static_cast<unsigned char>(bytes[i]) & 127u);
        bytes.erase(0, offset + 10);
    }
    if (bytes.size() >= 128 && bytes.compare(bytes.size() - 128, 3, "TAG") == 0)
        bytes.resize(bytes.size() - 128);
    return bytes;
}

void mp3Tags(const std::filesystem::path& directory) {
    const auto audio = untaggedMp3();
    const auto path = directory / std::filesystem::path(u8"מדידה.sample");
    std::string frames;
    // ID3v2.3 frame payload: expanded length then zlib bytes for encoding 0 and "A minor".
    constexpr std::string_view compressed("\x00\x00\x00\x08\x78\x9c\x63\x70\x54\xc8\xcd\xcc\xcb\x2f\x02\x00\x08\xee\x02\x87", 20);
    appendRawId3Frame(frames, "TKEY", compressed, 0x80);
    appendRawId3Frame(frames, "TBPM", std::string_view("\x00" "133", 4));
    writeFixture(path, id3Tag(3, frames) + audio);
    auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 133. && tags.tagKey == SampleKey{9, true}, "compressed ID3v2.3 tags read through a native Unicode path and content sniffing");

    frames.clear();
    // UTF-16 LE BOM with unsynchronization inserting 00 after FF.
    constexpr std::string_view unsynchronized("\x01\xff\x00\xfe\xc9\x00\x6c\x00\x65\x00\x63\x00\x74\x00\x72\x00\x6f\x00\x6e\x00\x69\x00\x71\x00\x75\x00\x65\x00", 28);
    appendRawId3Frame(frames, "TCON", unsynchronized, 0x02);
    appendId3Frame(frames, "TKEY", "C# min");
    writeFixture(path, id3Tag(4, frames) + audio);
    tags = readSampleFileTags(path);
    check(tags.tagKey == SampleKey{1, true} && tags.tags == std::vector<std::string>{"Électronique"},
          "per-frame unsynchronization preserves UTF-16 and the following ID3v2.4 frame");

    std::string id3v1(128, '\0');
    id3v1.replace(0, 3, "TAG");
    id3v1.replace(3, 11, "Legacy tone");
    id3v1[127] = static_cast<char>(255);
    writeFixture(path, audio + id3v1);
    tags = readSampleFileTags(path);
    check(tags.tags == std::vector<std::string>{"Legacy tone"}, "legacy ID3v1 title survives");

    std::string ape;
    for (const auto& [field, value] : {std::pair{"BPM", "108.5"}, std::pair{"InitialKey", "8A"}, std::pair{"GENRE", "Dance"}}) {
        appendInteger(ape, std::char_traits<char>::length(value), 4, true);
        appendInteger(ape, 0, 4, true);
        ape += field;
        ape.push_back('\0');
        ape += value;
    }
    std::string footer = "APETAGEX";
    appendInteger(footer, 2000, 4, true);
    appendInteger(footer, ape.size() + 32, 4, true);
    appendInteger(footer, 3, 4, true);
    appendInteger(footer, 0, 4, true);
    appendInteger(footer, 0, 8, true);
    writeFixture(path, audio + ape + footer);
    tags = readSampleFileTags(path);
    check(tags.tagBpm == 108.5 && tags.tagKey == SampleKey{9, true} && tags.tags == std::vector<std::string>{"Dance"},
          "APE text contributes fractional tempo, circle-of-fifths key and genre");

    writeFixture(path, id3Fixture() + audio + ape + footer + id3v1);
    tags = readSampleFileTags(path);
    check(tags.tagBpm == 131.5 && tags.tagKey == SampleKey{10, true} &&
          tags.tags == std::vector<std::string>{"Drums", "Dance", "Legacy tone"},
          "existing ID3v2 values take priority and APE and ID3v1 can still contribute tags");
}

std::string box(std::string_view name, const std::string& payload) {
    std::string bytes;
    appendInteger(bytes, 8 + payload.size(), 4, false);
    bytes.append(name);
    bytes.append(payload);
    return bytes;
}

std::string mp4Data(unsigned type, const std::string& value) {
    std::string payload;
    appendInteger(payload, type, 4, false);
    appendInteger(payload, 0, 4, false);
    payload += value;
    return box("data", payload);
}

std::string mp4Freeform(const std::string& name, const std::string& value) {
    const std::string fullBoxHeader(4, '\0');
    return box("----", box("mean", fullBoxHeader + "com.apple.iTunes") +
               box("name", fullBoxHeader + name) + mp4Data(1, value));
}

void mp4Tags(const std::filesystem::path& directory) {
    const std::string fullBoxHeader(4, '\0');
    std::string integerTempo;
    appendInteger(integerTempo, 129, 2, false);
    const auto items = box("tmpo", mp4Data(21, integerTempo)) + mp4Freeform("BPM", "129.75") +
        mp4Freeform("initialkey", "F♯ minor") + mp4Freeform("LOOP_TYPE", "loop") + box("\xa9gen", mp4Data(1, "Électronique"));
    const auto bytes = box("ftyp", "M4A " + fullBoxHeader + "M4A isom") +
        box("moov", box("udta", box("meta", fullBoxHeader + box("ilst", items))));
    const auto path = directory / "source.m4a";
    writeFixture(path, bytes);
    const auto tags = readSampleFileTags(path);
    check(tags.tagBpm == 129.75 && tags.tagLoop == true && tags.tagKey == SampleKey{6, true} &&
          tags.tags == std::vector<std::string>{"Électronique"},
          "MP4 ilst and freeform metadata preserve Unicode, loop and authored fractional BPM over integer tempo");
}

std::string ebml(std::uint32_t id, std::size_t idBytes, const std::string& body) {
    if (body.size() >= 127) throw std::runtime_error("fixture EBML size must fit in one byte");
    std::string bytes;
    appendInteger(bytes, id, idBytes, false);
    appendInteger(bytes, 128 + body.size(), 1, false);
    bytes += body;
    return bytes;
}

std::string mkvTag(const std::string& name, const std::string& value) {
    return ebml(0x67c8, 2, ebml(0x45a3, 2, name) + ebml(0x4487, 2, value));
}

void matroskaTags(const std::filesystem::path& directory) {
    const auto fields = mkvTag("BPM", "135.5") + mkvTag("INITIALKEY", "2d") + mkvTag("GENRE", "Percussion");
    const auto tags = ebml(0x1254c367, 4, ebml(0x7373, 2, fields));
    const auto bytes = ebml(0x1a45dfa3, 4, ebml(0x4282, 2, "matroska")) + ebml(0x18538067, 4, tags);
    const auto path = directory / "source.mka";
    writeFixture(path, bytes);
    const auto result = readSampleFileTags(path);
    check(result.tagBpm == 135.5 && result.tagKey == SampleKey{7, false} && result.tags == std::vector<std::string>{"Percussion"},
          "Matroska simple tags contribute fractional BPM, Open Key, and genre");
}
} // namespace

int main() {
    const adi::test::TempDirectory temporary("sample_file_tags", "metadata");
    wavTags(temporary.path());
    bpmGrammar(temporary.path());
    wavSamplerTags(temporary.path());
    rf64Tags(temporary.path());
    aiffTags(temporary.path());
    commentTags(temporary.path());
    mp3Tags(temporary.path());
    mp4Tags(temporary.path());
    matroskaTags(temporary.path());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
