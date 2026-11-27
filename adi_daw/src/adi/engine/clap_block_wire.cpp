// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/clap_block_wire.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace adi::engine::clap_wire {
namespace {

// The wire stores integers and IEEE-754 bits explicitly, never native CLAP
// headers, struct padding, cookies or pointers. The payload order is the
// optional transport snapshot, planar audio, then fixed-size event records.
constexpr std::uint32_t magic = 0x42434441; // ADCB
constexpr std::uint16_t version = 1;
constexpr std::uint16_t hasTransport = 1;
constexpr std::size_t transportBytes = 96;
constexpr std::size_t eventBytes = 56;
constexpr std::uint32_t eventFlagsMask = 3;
constexpr std::uint32_t transportFlagsMask = 7;
constexpr auto noClapId = std::numeric_limits<std::uint32_t>::max();
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);

template<class T>
using Bits = std::conditional_t<sizeof(T) == 8, std::uint64_t,
             std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint16_t>>;

class Writer {
public:
    explicit Writer(std::vector<std::byte>& bytes) : bytes_(bytes) {}
    template<class T>
    void write(T value) {
        const auto bits = std::bit_cast<Bits<T>>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes_.push_back(static_cast<std::byte>((bits >> (8 * i)) & 0xffu));
    }
private:
    std::vector<std::byte>& bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    // The header and complete packet length are checked before entry.
    template<class T>
    T read() {
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bits |= std::to_integer<std::uint64_t>(bytes_[position_++]) << (8 * i);
        return std::bit_cast<T>(static_cast<Bits<T>>(bits));
    }
private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

struct Header {
    std::uint64_t sequence = 0;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;
    std::uint32_t events = 0;
    bool transport = false;
};

std::size_t sampleCount(const Header& shape) {
    return static_cast<std::size_t>(shape.channels) * shape.frames;
}

std::size_t packetBytes(const Header& shape) {
    return kBlockHeaderBytes + (shape.transport ? transportBytes : 0)
         + sampleCount(shape) * sizeof(float) + static_cast<std::size_t>(shape.events) * eventBytes;
}

bool validShape(const Header& shape) {
    return shape.channels <= kMaxChannels && shape.frames > 0 &&
           shape.frames <= kMaxFrames && shape.events <= kMaxEvents;
}

bool readHeader(Reader& cursor, Header& shape, std::string& error) {
    const auto signature = cursor.read<std::uint32_t>();
    const auto readVersion = cursor.read<std::uint16_t>();
    const auto flags = cursor.read<std::uint16_t>();
    if (signature != magic || readVersion != version || (flags & ~hasTransport) != 0) {
        error = "invalid native plug-in block header";
        return false;
    }
    shape.transport = (flags & hasTransport) != 0;
    shape.sequence = cursor.read<std::uint64_t>();
    shape.channels = cursor.read<std::uint32_t>();
    shape.frames = cursor.read<std::uint32_t>();
    shape.events = cursor.read<std::uint32_t>();
    const auto declaredBytes = cursor.read<std::uint32_t>();
    if (!validShape(shape) || declaredBytes != packetBytes(shape)) {
        error = "invalid native plug-in block shape or length";
        return false;
    }
    return true;
}

void writeHeader(Writer& out, const Header& shape) {
    out.write(magic);
    out.write(version);
    out.write(shape.transport ? hasTransport : std::uint16_t{});
    out.write(shape.sequence);
    out.write(shape.channels);
    out.write(shape.frames);
    out.write(shape.events);
    out.write(static_cast<std::uint32_t>(packetBytes(shape)));
}

bool validTransport(const DeviceTransport& position) {
    return std::isfinite(position.quarterNotes) && std::isfinite(position.barStart) &&
           std::isfinite(position.bpm) && position.bpm > 0 &&
           position.numerator > 0 && position.denominator > 0 &&
           std::isfinite(position.loopBeginQuarterNotes) && std::isfinite(position.loopEndQuarterNotes);
}

void writeTransport(Writer& out, const DeviceTransport& position) {
    out.write(position.sample);
    out.write(position.bar);
    out.write(position.quarterNotes);
    out.write(position.barStart);
    out.write(position.bpm);
    out.write(static_cast<std::int32_t>(position.numerator));
    out.write(static_cast<std::int32_t>(position.denominator));
    const std::uint32_t flags = (position.playing ? 1u : 0u) | (position.recording ? 2u : 0u)
                              | (position.looping ? 4u : 0u);
    out.write(flags);
    out.write(std::uint32_t{});
    out.write(position.loopBegin);
    out.write(position.loopEnd);
    out.write(position.loopBeginQuarterNotes);
    out.write(position.loopEndQuarterNotes);
    out.write(position.nextChange);
}

bool readTransport(Reader& in, DeviceTransport& position) {
    position.sample = in.read<std::int64_t>();
    position.bar = in.read<std::int64_t>();
    position.quarterNotes = in.read<double>();
    position.barStart = in.read<double>();
    position.bpm = in.read<double>();
    position.numerator = in.read<std::int32_t>();
    position.denominator = in.read<std::int32_t>();
    const auto flags = in.read<std::uint32_t>();
    position.playing = (flags & 1u) != 0;
    position.recording = (flags & 2u) != 0;
    position.looping = (flags & 4u) != 0;
    const auto reserved = in.read<std::uint32_t>();
    position.loopBegin = in.read<std::int64_t>();
    position.loopEnd = in.read<std::int64_t>();
    position.loopBeginQuarterNotes = in.read<double>();
    position.loopEndQuarterNotes = in.read<double>();
    position.nextChange = in.read<std::int64_t>();
    return reserved == 0 && (flags & ~transportFlagsMask) == 0 && validTransport(position);
}

bool validEvent(const Event& event, std::uint32_t frames, std::uint32_t previousFrame) {
    if (event.frame < previousFrame || event.frame >= frames || (event.flags & ~eventFlagsMask) != 0 ||
        !std::isfinite(event.value) || event.noteId < -1 || event.port < -1 || event.channel < -1 ||
        event.channel > 15 || event.key < -1 || event.key > 127)
        return false;

    const bool notePort = event.port <= std::numeric_limits<std::int16_t>::max();
    const bool unitValue = event.value >= 0 && event.value <= 1;
    const bool noVoice = event.noteId == -1 && event.port == -1 && event.channel == -1 && event.key == -1;
    const bool noMidi = event.midi == std::array<std::uint32_t, 4>{};
    switch (event.kind) {
        case Kind::NoteOn:
            return notePort && event.port >= 0 && event.channel >= 0 && event.key >= 0 &&
                   unitValue && event.selector == 0 && noMidi;
        case Kind::NoteOff:
            return notePort && unitValue && event.selector == 0 && noMidi;
        case Kind::NoteChoke:
        case Kind::NoteEnd:
            return notePort && event.selector == 0 && noMidi;
        case Kind::NoteExpression:
            if (!notePort || event.selector > 6 || !noMidi) return false;
            if (event.selector == 2) return event.value >= -120 && event.value <= 120; // tuning
            if (event.selector == 0) return event.value >= 0 && event.value <= 4; // linear volume
            return unitValue;
        case Kind::ParamValue:
        case Kind::ParamMod:
        case Kind::NormalizedParamMod:
            return notePort && event.selector != noClapId && noMidi;
        case Kind::NormalizedParamValue:
            return notePort && event.selector != noClapId && unitValue && noMidi;
        case Kind::ParamGestureBegin:
        case Kind::ParamGestureEnd:
            return noVoice && event.selector != noClapId && event.value == 0 && noMidi;
        case Kind::Controller:
            return notePort && event.port >= 0 && event.channel >= 0 && event.noteId == -1 &&
                   event.key == -1 && event.selector <= 127 && unitValue && noMidi;
        case Kind::Midi1:
        case Kind::Midi2: {
            const bool rawPort = event.port >= 0 && event.port <= std::numeric_limits<std::uint16_t>::max();
            const bool rawAddress = event.noteId == -1 && event.channel == -1 && event.key == -1;
            const bool rawMidi1 = event.midi[0] <= 0xffffff && event.midi[1] == 0 &&
                                  event.midi[2] == 0 && event.midi[3] == 0;
            return rawPort && rawAddress && event.selector == 0 && event.value == 0 &&
                   (event.kind == Kind::Midi2 || rawMidi1);
        }
    }
    return false;
}

void writeEvent(Writer& out, const Event& event) {
    out.write(static_cast<std::uint16_t>(event.kind));
    out.write(std::uint16_t{});
    out.write(event.frame);
    out.write(event.flags);
    out.write(event.noteId);
    out.write(event.port);
    out.write(event.channel);
    out.write(event.key);
    out.write(event.selector);
    out.write(std::uint32_t{});
    out.write(event.value);
    for (auto word : event.midi) out.write(word);
}

bool readEvent(Reader& in, Event& event) {
    event.kind = static_cast<Kind>(in.read<std::uint16_t>());
    const auto reserved1 = in.read<std::uint16_t>();
    event.frame = in.read<std::uint32_t>();
    event.flags = in.read<std::uint32_t>();
    event.noteId = in.read<std::int32_t>();
    event.port = in.read<std::int32_t>();
    event.channel = in.read<std::int16_t>();
    event.key = in.read<std::int16_t>();
    event.selector = in.read<std::uint32_t>();
    const auto reserved2 = in.read<std::uint32_t>();
    event.value = in.read<double>();
    for (auto& word : event.midi) word = in.read<std::uint32_t>();
    return reserved1 == 0 && reserved2 == 0;
}

bool missingHeader(std::span<const std::byte> bytes, std::string& error) {
    if (bytes.size() >= kBlockHeaderBytes) return false;
    error = "truncated native plug-in block header";
    return true;
}

bool finiteSample(float sample) { return std::isfinite(sample); }

} // namespace

bool blockPacketSize(std::span<const std::byte> header, std::size_t& size, std::string& error) {
    error.clear();
    if (missingHeader(header, error)) return false;
    Reader cursor(header);
    Header shape;
    if (!readHeader(cursor, shape, error)) return false;
    size = packetBytes(shape);
    return true;
}

bool encodeBlock(const Block& block, std::vector<std::byte>& bytes, std::string& error) {
    error.clear();
    if (block.events.size() > kMaxEvents) {
        error = "native plug-in block event capacity exceeded";
        return false;
    }
    const Header shape{block.sequence, block.channels, block.frames,
                       static_cast<std::uint32_t>(block.events.size()), block.transport.has_value()};
    if (!validShape(shape) || block.audio.size() != sampleCount(shape)) {
        error = "invalid native plug-in block shape";
        return false;
    }
    if (block.transport && !validTransport(*block.transport)) {
        error = "invalid native plug-in block transport";
        return false;
    }
    if (!std::all_of(block.audio.begin(), block.audio.end(), finiteSample)) {
        error = "nonfinite native plug-in block audio";
        return false;
    }
    std::uint32_t frame = 0;
    for (const auto& event : block.events) {
        if (!validEvent(event, block.frames, frame)) {
            error = "invalid native plug-in block event";
            return false;
        }
        frame = event.frame;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(packetBytes(shape));
    Writer out(encoded);
    writeHeader(out, shape);
    if (block.transport) writeTransport(out, *block.transport);
    for (auto sample : block.audio) out.write(sample);
    for (const auto& event : block.events) writeEvent(out, event);
    bytes = std::move(encoded);
    return true;
}

bool decodeBlock(std::span<const std::byte> bytes, Block& block, std::string& error) {
    error.clear();
    if (missingHeader(bytes, error)) return false;
    Reader in(bytes);
    Header shape;
    if (!readHeader(in, shape, error)) return false;
    if (bytes.size() != packetBytes(shape)) {
        error = "incomplete or trailing native plug-in block payload";
        return false;
    }
    Block decoded;
    decoded.sequence = shape.sequence;
    decoded.channels = shape.channels;
    decoded.frames = shape.frames;
    if (shape.transport && !readTransport(in, decoded.transport.emplace())) {
        error = "invalid native plug-in block transport";
        return false;
    }
    decoded.audio.resize(sampleCount(shape));
    for (auto& sample : decoded.audio) {
        sample = in.read<float>();
        if (!finiteSample(sample)) {
            error = "nonfinite native plug-in block audio";
            return false;
        }
    }
    decoded.events.resize(shape.events);
    std::uint32_t frame = 0;
    for (auto& event : decoded.events) {
        if (!readEvent(in, event) || !validEvent(event, shape.frames, frame)) {
            error = "invalid native plug-in block event";
            return false;
        }
        frame = event.frame;
    }
    block = std::move(decoded);
    return true;
}

} // namespace adi::engine::clap_wire
