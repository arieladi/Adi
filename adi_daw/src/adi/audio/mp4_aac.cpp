// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/mp4_aac.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
namespace adi::audio {
namespace {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;
constexpr std::uint64_t maxPackets = 1000000, maxMetadata = 64 * 1024 * 1024;
[[noreturn]] void bad(const char *why) {
    throw std::runtime_error(std::string("AAC MP4: ") + why);
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        bad("integer overflow");
    return a + b;
}
View sub(View v, std::size_t at, std::size_t n) {
    if (at > v.size() || n > v.size() - at)
        bad("truncated box");
    return v.subspan(at, n);
}
std::uint64_t be(View v, std::size_t at, std::size_t n) {
    auto x = sub(v, at, n);
    std::uint64_t value = 0;
    for (auto b : x)
        value = (value << 8) | b;
    return value;
}
void put(Bytes &b, std::uint64_t v, unsigned n) {
    for (unsigned i = n; i > 0; --i)
        b.push_back(static_cast<std::uint8_t>(v >> ((i - 1) * 8)));
}
void zeros(Bytes &b, std::size_t n) {
    b.resize(b.size() + n, 0);
}
void append(Bytes &b, View v) {
    b.insert(b.end(), v.begin(), v.end());
}
void box(Bytes &b, std::string_view kind, View content) {
    if (content.size() > std::numeric_limits<std::uint32_t>::max() - 8)
        bad("box too large");
    put(b, content.size() + 8, 4);
    b.insert(b.end(), kind.begin(), kind.end());
    append(b, content);
}
struct Box {
    std::string_view type;
    View data;
};
std::vector<Box> boxes(View v) {
    std::vector<Box> out;
    for (std::size_t at = 0; at < v.size();) {
        if (out.size() > 100000)
            bad("too many boxes");
        auto n = be(v, at, 4);
        const auto type = sub(v, at + 4, 4);
        std::size_t h = 8;
        if (n == 1) {
            n = be(v, at + 8, 8);
            h = 16;
        }
        if (!n)
            n = v.size() - at;
        if (n < h || n > v.size() - at)
            bad("box exceeds parent");
        out.push_back({{reinterpret_cast<const char *>(type.data()), 4},
                       sub(v, at + h, static_cast<std::size_t>(n) - h)});
        at += static_cast<std::size_t>(n);
    }
    return out;
}
View one(View parent, std::string_view type, bool required = true) {
    View found;
    bool seen = false;
    for (const auto &b : boxes(parent))
        if (b.type == type) {
            if (seen)
                bad("duplicate structural box");
            seen = true;
            found = b.data;
        }
    if (!seen && required)
        bad("missing structural box");
    return found;
}
Bytes read(std::ifstream &f, std::uint64_t at, std::size_t n) {
    if (at > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        bad("file offset too large");
    Bytes b(n);
    f.clear();
    f.seekg(static_cast<std::streamoff>(at));
    f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(n));
    if (!f)
        bad("file read failed");
    return b;
}
std::uint64_t clockFrames(std::uint64_t value, std::uint32_t clock, std::uint32_t rate) {
    if (!clock)
        bad("zero timescale");
    const auto hi = value / clock;
    if (hi > std::numeric_limits<std::uint64_t>::max() / rate)
        bad("time overflow");
    return add(hi * rate, ((value % clock) * rate + clock / 2) / clock);
}
std::pair<std::uint32_t, std::uint64_t> clockOf(View b) {
    const auto v = be(b, 0, 1);
    if (v > 1)
        bad("unsupported clock version");
    return {static_cast<std::uint32_t>(be(b, v ? 20 : 12, 4)), be(b, v ? 24 : 16, v ? 8 : 4)};
}
// ISO/IEC 14496-1 descriptors use at most four base-128 length bytes.
View descriptor(View v, std::size_t &at, std::uint8_t tag) {
    if (be(v, at++, 1) != tag)
        bad("unexpected ES descriptor");
    std::size_t n = 0;
    bool done = false;
    for (int i = 0; i < 4; ++i) {
        auto x = be(v, at++, 1);
        n = (n << 7) | (x & 127);
        if (!(x & 128)) {
            done = true;
            break;
        }
    }
    if (!done)
        bad("descriptor length overflow");
    auto d = sub(v, at, n);
    at += n;
    return d;
}
Bytes configOf(View esds) {
    if (be(esds, 0, 4) != 0)
        bad("unsupported ES descriptor version");
    std::size_t at = 4;
    auto es = descriptor(esds, at, 3);
    at = 3;
    const auto flags = be(es, 2, 1);
    if (flags & 128)
        at += 2;
    if (flags & 64) {
        auto n = be(es, at++, 1);
        at += static_cast<std::size_t>(n);
    }
    if (flags & 32)
        at += 2;
    auto dec = descriptor(es, at, 4);
    if (be(dec, 0, 1) != 0x40 || (be(dec, 1, 1) >> 2) != 5)
        bad("not MPEG-4 AAC audio");
    at = 13;
    auto cfg = descriptor(dec, at, 5);
    (void)aacSampleRate(cfg);
    return {cfg.begin(), cfg.end()};
}
void versionZero(View v) {
    if (be(v, 0, 4))
        bad("unsupported sample table version");
}
void tableSize(View v, std::uint64_t count, std::size_t width) {
    if (count > maxPackets || v.size() != 8 + count * width)
        bad("invalid or excessive sample table");
}
void emitDescriptor(Bytes &b, unsigned tag, View payload) {
    put(b, tag, 1);
    std::array<std::uint8_t, 4> len{};
    auto n = payload.size();
    unsigned i = 0;
    do {
        len[i++] = static_cast<std::uint8_t>(n & 127);
        n >>= 7;
    } while (n && i < 4);
    if (n)
        bad("descriptor too large");
    while (i) {
        --i;
        put(b, len[i] | (i ? 128 : 0), 1);
    }
    append(b, payload);
}
Bytes sampleDescription(const AacTrack &t) {
    Bytes dc;
    put(dc, 0x40, 1);
    put(dc, 0x15, 1);
    zeros(dc, 11);
    emitDescriptor(dc, 5, t.config);
    Bytes es;
    put(es, 1, 2);
    put(es, 0, 1);
    emitDescriptor(es, 4, dc);
    const Bytes sl{2};
    emitDescriptor(es, 6, sl);
    Bytes desc(4, 0);
    emitDescriptor(desc, 3, es);
    Bytes entry(6, 0);
    put(entry, 1, 2);
    zeros(entry, 8);
    put(entry, 2, 2);
    put(entry, 16, 2);
    zeros(entry, 4);
    put(entry, std::uint64_t{t.sampleRate} << 16, 4);
    box(entry, "esds", desc);
    Bytes stsd(4, 0);
    put(stsd, 1, 4);
    box(stsd, "mp4a", entry);
    return stsd;
}
void matrix(Bytes &b) {
    for (auto v : std::array<std::uint32_t, 9>{65536, 0, 0, 0, 65536, 0, 0, 0, 0x40000000})
        put(b, v, 4);
}
} // namespace
std::uint32_t aacSampleRate(std::span<const std::uint8_t> config) {
    if (config.size() < 2 || config.size() > 64)
        bad("invalid AudioSpecificConfig size");
    std::size_t bit = 0;
    auto bits = [&](unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (bit >= config.size() * 8)
                bad("truncated AudioSpecificConfig");
            v = (v << 1) | ((config[bit / 8] >> (7 - bit % 8)) & 1);
            ++bit;
        }
        return v;
    };
    if (bits(5) != 2)
        bad("only AAC-LC is supported");
    const auto index = bits(4);
    constexpr std::array<std::uint32_t, 13> rates{96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                  22050, 16000, 12000, 11025, 8000,  7350};
    const auto rate = index == 15 ? bits(24) : (index < rates.size() ? rates[index] : 0);
    if (!rate || rate > 65535 || bits(4) != 2 || bits(1) != 0 || bits(1) != 0 || bits(1) != 0)
        bad("unsupported AAC rate, layout or frame mode");
    if (config.size() * 8 - bit >= 17 && bits(11) == 0x2b7) {
        if (bits(5) != 5 || bits(1) != 0)
            bad("HE-AAC sync extension is unsupported");
    }
    return rate;
}
AacMovie readAacMovie(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        bad("cannot open file");
    const auto size = std::filesystem::file_size(path);
    Bytes moov;
    bool gotMoov = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> mdats;
    std::size_t roots = 0;
    for (std::uint64_t at = 0; at < size;) {
        if (++roots > 100000)
            bad("too many root boxes");
        if (size - at < 8)
            bad("truncated file box");
        auto head = read(file, at, 8);
        auto n = be(head, 0, 4);
        const std::string type(reinterpret_cast<char *>(head.data() + 4), 4);
        std::uint64_t h = 8;
        if (n == 1) {
            if (size - at < 16)
                bad("truncated extended box");
            head = read(file, at, 16);
            n = be(head, 8, 8);
            h = 16;
        }
        if (!n)
            n = size - at;
        if (n < h || n > size - at)
            bad("file box exceeds file");
        if (type == "moov") {
            if (gotMoov || n - h > maxMetadata)
                bad("duplicate or excessive moov");
            moov = read(file, at + h, static_cast<std::size_t>(n - h));
            gotMoov = true;
        }
        if (type == "mdat") {
            if (mdats.size() >= 1024)
                bad("too many media boxes");
            mdats.emplace_back(at + h, at + n);
        }
        if (type == "moof")
            bad("fragmented MP4 is unsupported");
        at += n;
    }
    if (!gotMoov || mdats.empty())
        bad("missing movie/media data");
    if (!one(moov, "mvex", false).empty())
        bad("fragmented MP4 is unsupported");
    const auto movieClock = clockOf(one(moov, "mvhd")).first;
    if (!movieClock)
        bad("zero movie timescale");
    AacMovie movie;
    const auto udta = one(moov, "udta", false);
    if (!udta.empty()) {
        auto stem = one(udta, "stem", false);
        if (stem.size() > 1024 * 1024)
            bad("excessive stem metadata");
        movie.stemManifest.assign(stem.begin(), stem.end());
    }
    for (const auto &b : boxes(moov))
        if (b.type == "trak") {
            const auto mdia = one(b.data, "mdia");
            auto handler = one(mdia, "hdlr");
            const auto htype = sub(handler, 8, 4);
            if (std::string_view(reinterpret_cast<const char *>(htype.data()), 4) != "soun")
                continue;
            if (movie.tracks.size() >= 5)
                bad("too many audio tracks");
            const auto [clock, duration] = clockOf(one(mdia, "mdhd"));
            const auto stbl = one(one(mdia, "minf"), "stbl");
            auto stsd = one(stbl, "stsd");
            versionZero(stsd);
            if (be(stsd, 4, 4) != 1)
                bad("multiple sample descriptions");
            const auto entries = boxes(sub(stsd, 8, stsd.size() - 8));
            if (entries.size() != 1 || entries[0].type != "mp4a")
                bad("not unencrypted mp4a");
            const auto entry = entries[0].data;
            if (be(entry, 6, 2) != 1 || be(entry, 8, 2) != 0 || be(entry, 16, 2) != 2)
                bad("unsupported sample entry");
            auto config = configOf(one(sub(entry, 28, entry.size() >= 28 ? entry.size() - 28 : 0), "esds"));
            for (const auto &child : boxes(sub(entry, 28, entry.size() - 28)))
                if (child.type == "sinf")
                    bad("encrypted audio is unsupported");
            for (const auto &child : boxes(stbl))
                if (child.type == "ctts")
                    bad("AAC composition offsets are unsupported");
            const auto rate = aacSampleRate(config);
            if (be(entry, 24, 4) != (std::uint64_t{rate} << 16))
                bad("sample entry rate mismatch");
            auto stsz = one(stbl, "stsz");
            versionZero(stsz);
            const auto constant = be(stsz, 4, 4), count = be(stsz, 8, 4);
            if (!count || count > maxPackets || stsz.size() != 12 + (constant ? 0 : 4 * count))
                bad("invalid or excessive packet count");
            auto stts = one(stbl, "stts");
            versionZero(stts);
            const auto times = be(stts, 4, 4);
            tableSize(stts, times, 8);
            std::uint64_t samples = 0, ticks = 0;
            for (std::size_t i = 0; i < times; ++i) {
                const auto num = be(stts, 8 + i * 8, 4), delta = be(stts, 12 + i * 8, 4);
                if (!num || !delta)
                    bad("zero packet timing");
                samples = add(samples, num);
                ticks = add(ticks, num * delta);
                const auto frameDelta = clockFrames(delta, clock, rate);
                if (frameDelta > 1024 || (!frameDelta) || (frameDelta != 1024 && i + 1 != times))
                    bad("unsupported AAC packet timing");
            }
            if (samples != count || ticks != duration)
                bad("sample timing/duration mismatch");
            const auto co64 = one(stbl, "co64", false), stco = one(stbl, "stco", false);
            if (co64.empty() == stco.empty())
                bad("missing or duplicate chunk offsets");
            const auto offsets = co64.empty() ? stco : co64;
            versionZero(offsets);
            const auto chunks = be(offsets, 4, 4);
            tableSize(offsets, chunks, co64.empty() ? 4 : 8);
            auto stsc = one(stbl, "stsc");
            versionZero(stsc);
            const auto runs = be(stsc, 4, 4);
            tableSize(stsc, runs, 12);
            if (!chunks || !runs || be(stsc, 8, 4) != 1)
                bad("invalid chunk table");
            for (std::size_t i = 0; i < runs; ++i) {
                const auto first = be(stsc, 8 + i * 12, 4);
                if (!first || first > chunks || !be(stsc, 12 + i * 12, 4) || be(stsc, 16 + i * 12, 4) != 1 ||
                    (i && first <= be(stsc, 8 + (i - 1) * 12, 4)))
                    bad("invalid chunk run");
            }
            AacTrack track;
            track.file = path;
            track.config = std::move(config);
            track.sampleRate = rate;
            track.packets.reserve(static_cast<std::size_t>(count));
            std::size_t run = 0;
            std::uint64_t packet = 0;
            for (std::uint64_t chunk = 1; chunk <= chunks; ++chunk) {
                while (run + 1 < runs && be(stsc, 8 + (run + 1) * 12, 4) <= chunk)
                    ++run;
                const auto num = be(stsc, 12 + run * 12, 4);
                if (num > count - packet)
                    bad("chunk exceeds packet count");
                auto offset = be(offsets, 8 + static_cast<std::size_t>(chunk - 1) * (co64.empty() ? 4 : 8),
                                 co64.empty() ? 4 : 8);
                for (std::uint64_t j = 0; j < num; ++j, ++packet) {
                    const auto bytes =
                        constant ? constant : be(stsz, 12 + static_cast<std::size_t>(packet) * 4, 4);
                    if (!bytes || bytes > 1024 * 1024)
                        bad("invalid or excessive AAC packet size");
                    const auto end = add(offset, bytes);
                    bool inside = false;
                    for (const auto &extent : mdats)
                        if (offset >= extent.first && end <= extent.second) {
                            inside = true;
                            break;
                        }
                    if (!inside)
                        bad("AAC packet outside mdat");
                    track.packets.push_back({offset, static_cast<std::uint32_t>(bytes)});
                    offset = end;
                }
            }
            if (packet != count)
                bad("incomplete chunk table");
            track.frames = clockFrames(duration, clock, rate);
            const auto edts = one(b.data, "edts", false);
            if (!edts.empty()) {
                const auto elst = one(edts, "elst");
                const auto v = be(elst, 0, 1), n = be(elst, 4, 4);
                if (v > 1 || n != 1 || elst.size() != 8 + (v ? 20 : 12))
                    bad("complex edit list is unsupported");
                const auto length = be(elst, 8, v ? 8 : 4), start = be(elst, v ? 16 : 12, v ? 8 : 4);
                if (start > (v ? std::uint64_t{0x7fffffffffffffff} : std::uint64_t{0x7fffffff}) ||
                    be(elst, v ? 24 : 16, 4) != 65536)
                    bad("empty or non-unit edit is unsupported");
                track.priming = clockFrames(start, clock, rate);
                track.frames = clockFrames(length, movieClock, rate);
                // Coarse movie clocks may round a valid exact media end. Clamp only
                // their half-tick rounding, never invent missing compressed content.
                const auto exact = clockFrames(duration, clock, rate);
                if (track.priming <= exact && track.frames > exact - track.priming &&
                    track.frames - (exact - track.priming) <= rate / movieClock + 1)
                    track.frames = exact - track.priming;
            }
            if (!track.frames || add(track.frames, track.priming) > count * 1024)
                bad("edit exceeds decoded audio");
            if (!movie.tracks.empty() &&
                (track.sampleRate != movie.tracks[0].sampleRate || track.config != movie.tracks[0].config ||
                 track.frames != movie.tracks[0].frames))
                bad("audio tracks have different codecs, rates or durations");
            movie.tracks.push_back(std::move(track));
        }
    if (movie.tracks.empty())
        bad("no AAC audio");
    return movie;
}
void writeAacMovie(const std::filesystem::path &path, std::span<const AacTrack> tracks,
                   const std::string &manifest, const std::function<void()> &checkpoint) {
    if (tracks.empty() || tracks.size() > 5 || manifest.size() > 1024 * 1024)
        bad("invalid output track/manifest count");
    const auto rate = tracks[0].sampleRate;
    const auto frames = tracks[0].frames;
    std::uint64_t payload = 0;
    for (const auto &t : tracks) {
        if (t.sampleRate != rate || t.frames != frames || !frames || aacSampleRate(t.config) != rate ||
            t.config != tracks[0].config || t.packets.empty() || t.packets.size() > maxPackets ||
            add(t.priming, frames) > t.packets.size() * 1024)
            bad("incompatible output tracks");
        for (const auto &p : t.packets) {
            if (!p.size || p.size > 1024 * 1024)
                bad("invalid output packet size");
            payload = add(payload, p.size);
        }
    }
    Bytes ftyp;
    for (auto c : std::string_view("M4A "))
        ftyp.push_back(static_cast<std::uint8_t>(c));
    put(ftyp, 0, 4);
    for (auto c : std::string_view("M4A mp42isom"))
        ftyp.push_back(static_cast<std::uint8_t>(c));
    Bytes header;
    box(header, "ftyp", ftyp);
    const auto mediaStart = header.size() + 16;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        bad("cannot create output");
    auto write = [&](View b) {
        out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
        if (!out)
            bad("write failed");
    };
    write(header);
    Bytes mdat;
    put(mdat, 1, 4);
    for (char c : std::string_view("mdat"))
        mdat.push_back(static_cast<std::uint8_t>(c));
    put(mdat, add(payload, 16), 8);
    write(mdat);
    Bytes moov, mvhd;
    put(mvhd, 0x01000000, 4);
    zeros(mvhd, 16);
    put(mvhd, rate, 4);
    put(mvhd, frames, 8);
    put(mvhd, 65536, 4);
    put(mvhd, 256, 2);
    zeros(mvhd, 10);
    matrix(mvhd);
    zeros(mvhd, 24);
    put(mvhd, tracks.size() + 1, 4);
    box(moov, "mvhd", mvhd);
    std::uint64_t offset = mediaStart;
    std::uint32_t id = 0;
    for (const auto &t : tracks) {
        ++id;
        Bytes trak, tkhd;
        put(tkhd, 0x01000000 | (id == 1 ? 3 : 2), 4);
        zeros(tkhd, 16);
        put(tkhd, id, 4);
        zeros(tkhd, 4);
        put(tkhd, frames, 8);
        zeros(tkhd, 8);
        put(tkhd, 0, 2);
        put(tkhd, 1, 2);
        put(tkhd, 256, 2);
        zeros(tkhd, 2);
        matrix(tkhd);
        zeros(tkhd, 8);
        box(trak, "tkhd", tkhd);
        Bytes edits(4, 0);
        edits[0] = 1;
        put(edits, 1, 4);
        put(edits, frames, 8);
        put(edits, t.priming, 8);
        put(edits, 65536, 4);
        Bytes edts;
        box(edts, "elst", edits);
        box(trak, "edts", edts);
        Bytes mdia, mdhd(4, 0);
        mdhd[0] = 1;
        zeros(mdhd, 16);
        put(mdhd, rate, 4);
        put(mdhd, t.priming + frames, 8);
        put(mdhd, 0x55c4, 2);
        put(mdhd, 0, 2);
        box(mdia, "mdhd", mdhd);
        Bytes hdlr(8, 0);
        for (char c : std::string_view("soun"))
            hdlr.push_back(static_cast<std::uint8_t>(c));
        zeros(hdlr, 12);
        for (char c : std::string_view("ADI AAC"))
            hdlr.push_back(static_cast<std::uint8_t>(c));
        put(hdlr, 0, 1);
        box(mdia, "hdlr", hdlr);
        Bytes minf, smhd(8, 0);
        box(minf, "smhd", smhd);
        Bytes url(4, 0);
        url[3] = 1;
        Bytes dref(4, 0);
        put(dref, 1, 4);
        box(dref, "url ", url);
        Bytes dinf;
        box(dinf, "dref", dref);
        box(minf, "dinf", dinf);
        Bytes stbl;
        box(stbl, "stsd", sampleDescription(t));
        Bytes stts(4, 0);
        const auto decoded = t.packets.size() * 1024;
        const auto tail = decoded - (t.priming + frames);
        if (tail >= 1024)
            bad("excessive trailing AAC padding");
        put(stts, tail && t.packets.size() > 1 ? 2 : 1, 4);
        if (!tail || t.packets.size() > 1) {
            put(stts, t.packets.size() - (tail ? 1 : 0), 4);
            put(stts, 1024, 4);
        }
        if (tail) {
            put(stts, 1, 4);
            put(stts, 1024 - tail, 4);
        }
        box(stbl, "stts", stts);
        Bytes stsc(4, 0);
        put(stsc, 1, 4);
        put(stsc, 1, 4);
        put(stsc, 1, 4);
        put(stsc, 1, 4);
        box(stbl, "stsc", stsc);
        Bytes stsz(8, 0);
        put(stsz, t.packets.size(), 4);
        Bytes co64(4, 0);
        put(co64, t.packets.size(), 4);
        std::ifstream source(t.file, std::ios::binary);
        if (!source)
            bad("cannot open packet source");
        for (const auto &p : t.packets) {
            if (checkpoint)
                checkpoint();
            put(stsz, p.size, 4);
            put(co64, offset, 8);
            auto packet = read(source, p.offset, p.size);
            write(packet);
            offset = add(offset, p.size);
        }
        box(stbl, "stsz", stsz);
        box(stbl, "co64", co64);
        box(minf, "stbl", stbl);
        box(mdia, "minf", minf);
        box(trak, "mdia", mdia);
        box(moov, "trak", trak);
    }
    if (!manifest.empty()) {
        Bytes udta;
        box(udta, "stem", {reinterpret_cast<const std::uint8_t *>(manifest.data()), manifest.size()});
        box(moov, "udta", udta);
    }
    Bytes final;
    box(final, "moov", moov);
    write(final);
    out.close();
    if (!out)
        bad("close failed");
}
} // namespace adi::audio
