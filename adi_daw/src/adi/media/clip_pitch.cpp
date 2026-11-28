// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/clip_pitch.hpp"

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <bit>
#include <new>

namespace adi::media {
namespace {

constexpr auto pitchTag = "PNOT";
constexpr std::size_t countBytes = 8, noteBytes = 48, pointBytes = 16;
constexpr std::array boundaryTag{std::byte{0x42}, std::byte{0x4e}, std::byte{0x44}, std::byte{0x31}}; // BND1
constexpr std::size_t boundaryHeaderBytes = boundaryTag.size() + countBytes, boundaryBytes = 9;
constexpr std::size_t maxPayloadBytes = 16u * 1024 * 1024;
constexpr std::uint64_t maxSourceFrames = 64u * 1024 * 1024; // 256 MiB of mono PCM

struct Source {
    AnalysisIdentity identity;
    std::uint64_t start, end;
    bool reversed;
    Payload pathFields;
};

ClipPitchResult fail(ClipPitchProblem problem) { return {problem, {}, {}}; }

ClipPitchResult fail(ClipPitchProblem problem, std::string detail) {
    return {problem, {}, std::move(detail)};
}

std::optional<Source> findSource(const Store& store, std::int64_t clipId) {
    SQLite::Statement row(store.db(),
        "SELECT a.src_start_frames,a.src_len_frames,a.reverse,m.hash_blake3,"
        "m.sample_rate,m.channels,m.frames,m.orig_name,m.rel_path,m.abs_path_hint "
        "FROM audio_clips AS a JOIN clips AS c ON c.id=a.clip_id "
        "JOIN media_files AS m ON m.id=a.media_id WHERE a.clip_id=? AND c.kind='audio'");
    row.bind(1, clipId);
    if (!row.executeStep()) return {};
    const auto start = row.getColumn(0).getInt64();
    const auto length = row.getColumn(1).getInt64();
    const auto rate = row.getColumn(4).getInt64();
    const auto channels = row.getColumn(5).getInt64();
    const auto frames = row.getColumn(6).getInt64();
    if (start < 0 || length <= 0 || rate < 0 || rate > audio::kMaxDecodeRate ||
        channels < 0 || channels > audio::kMaxDecodeChannels || frames < 0 ||
        (rate == 0 && !row.getColumn(4).isNull()) ||
        (channels == 0 && !row.getColumn(5).isNull()) ||
        (frames == 0 && !row.getColumn(6).isNull()))
        throw std::invalid_argument("invalid pitch analysis source dimensions");
    const auto text = [&row](int index) -> Payload {
        const auto column = row.getColumn(index);
        if (column.isNull()) return nullptr;
        return column.getString();
    };
    const auto hash = row.getColumn(3).getString();
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) throw std::invalid_argument("invalid pitch analysis source identity");
    return Source{
        {hash, static_cast<std::uint32_t>(rate), static_cast<std::uint32_t>(channels),
         static_cast<std::uint64_t>(frames)},
        static_cast<std::uint64_t>(start),
        static_cast<std::uint64_t>(start) + static_cast<std::uint64_t>(length),
        row.getColumn(2).getInt() != 0,
        {{"hash_blake3", hash}, {"orig_name", text(7)}, {"rel_path", text(8)}, {"abs_path_hint", text(9)}}
    };
}

void appendWord(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8;
    }
}

std::uint64_t readWord(std::span<const std::byte>& bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[index])) << (8 * index);
    bytes = bytes.subspan(8);
    return value;
}

std::optional<AnalysisSection> encodeNotes(const audio::PitchAnalysis& pitch) {
    std::size_t size = countBytes;
    for (const auto& note : pitch.notes) {
        if (maxPayloadBytes - size < noteBytes) return {};
        size += noteBytes;
        if (note.curve.size() > (maxPayloadBytes - size) / pointBytes) return {};
        size += pointBytes * note.curve.size();
    }
    if (pitch.boundaryUncertainty) {
        if (maxPayloadBytes - size < boundaryHeaderBytes) return {};
        size += boundaryHeaderBytes;
        if (pitch.boundaryUncertainty->size() > (maxPayloadBytes - size) / boundaryBytes) return {};
        size += pitch.boundaryUncertainty->size() * boundaryBytes;
    }
    AnalysisSection section{kPitchAnalysisVersion, kPitchAnalysisConfiguration, {}};
    section.bytes.reserve(size);
    appendWord(section.bytes, pitch.notes.size());
    for (const auto& note : pitch.notes) {
        appendWord(section.bytes, note.startFrame);
        appendWord(section.bytes, note.endFrame);
        appendWord(section.bytes, std::bit_cast<std::uint64_t>(note.pitchCents));
        appendWord(section.bytes, std::bit_cast<std::uint64_t>(note.driftCents));
        appendWord(section.bytes, std::bit_cast<std::uint64_t>(note.vibratoCents));
        appendWord(section.bytes, note.curve.size());
        for (const auto& point : note.curve) {
            appendWord(section.bytes, point.frame);
            appendWord(section.bytes, std::bit_cast<std::uint64_t>(point.cents));
        }
    }
    if (pitch.boundaryUncertainty) {
        section.bytes.insert(section.bytes.end(), boundaryTag.begin(), boundaryTag.end());
        appendWord(section.bytes, pitch.boundaryUncertainty->size());
        for (const auto& hint : *pitch.boundaryUncertainty) {
            appendWord(section.bytes, hint.frame);
            section.bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(hint.reasons)));
        }
    }
    return section;
}

std::optional<audio::PitchAnalysis> decodeNotes(const AnalysisIdentity& source,
                                                const AnalysisSection& section) {
    if (section.version != kPitchAnalysisVersion || section.configuration != kPitchAnalysisConfiguration ||
        section.bytes.size() < countBytes || section.bytes.size() > maxPayloadBytes) return {};
    std::span<const std::byte> bytes(section.bytes);
    const auto noteCount = readWord(bytes);
    if (noteCount > bytes.size() / (noteBytes + pointBytes)) return {};
    audio::PitchAnalysis pitch{source.sampleRate, source.frames, {}};
    pitch.notes.reserve(static_cast<std::size_t>(noteCount));
    for (std::uint64_t index = 0; index < noteCount; ++index) {
        if (bytes.size() < noteBytes) return {};
        audio::PitchNote note;
        note.startFrame = readWord(bytes);
        note.endFrame = readWord(bytes);
        note.pitchCents = std::bit_cast<double>(readWord(bytes));
        note.driftCents = std::bit_cast<double>(readWord(bytes));
        note.vibratoCents = std::bit_cast<double>(readWord(bytes));
        const auto pointCount = readWord(bytes);
        if (pointCount == 0 || pointCount > bytes.size() / pointBytes) return {};
        note.curve.reserve(static_cast<std::size_t>(pointCount));
        for (std::uint64_t point = 0; point < pointCount; ++point)
            note.curve.push_back({readWord(bytes), std::bit_cast<double>(readWord(bytes))});
        pitch.notes.push_back(std::move(note));
    }
    if (!bytes.empty()) {
        if (bytes.size() < boundaryHeaderBytes || !std::equal(boundaryTag.begin(), boundaryTag.end(), bytes.begin())) return {};
        bytes = bytes.subspan(boundaryTag.size());
        const auto count = readWord(bytes);
        if (bytes.size() % boundaryBytes != 0 || count != bytes.size() / boundaryBytes) return {};
        auto& hints = pitch.boundaryUncertainty.emplace();
        hints.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto frame = readWord(bytes);
            hints.push_back({frame, static_cast<audio::PitchBoundaryReason>(std::to_integer<std::uint8_t>(bytes.front()))});
            bytes = bytes.subspan(1);
        }
    }
    if (!audio::validPitchAnalysis(pitch)) return {};
    return pitch;
}

std::optional<audio::PitchAnalysis> readNotes(AnalysisCache& cache, const AnalysisIdentity& identity,
                                              std::stop_token stop) {
    const auto cached = cache.read(identity, stop);
    if (!cached || stop.stop_requested()) return {};
    const auto found = cached.data->sections.find(pitchTag);
    if (found == cached.data->sections.end()) return {};
    return decodeNotes(identity, found->second);
}

ClipPitchResult success(std::int64_t id, Source source, audio::PitchAnalysis pitch) {
    return {ClipPitchProblem::None,
            ClipPitchAnalysis{id, std::move(source.identity), source.start, source.end,
                              source.reversed, kPitchAnalysisVersion, kPitchAnalysisConfiguration,
                              std::move(pitch)}, {}};
}

audio::PitchAnalysisResult analyzeAndCache(const AnalysisIdentity& identity, std::span<const float> mono,
                                          AnalysisCache* cache, std::stop_token stop,
                                          std::optional<audio::PitchAnalysis> legacy) {
    auto result = audio::analyzePitch(mono, identity.sampleRate, stop);
    if (stop.stop_requested() || result.error == audio::PitchAnalysisError::Cancelled)
        return {audio::PitchAnalysisError::Cancelled, {}};
    if (legacy) {
        if (!result) return {audio::PitchAnalysisError::None, std::move(legacy)};
        // The existing note geometry and statistics are the authored edits'
        // identity. A metadata refresh adds suggestions on the source clock.
        legacy->boundaryUncertainty = std::move(result.analysis->boundaryUncertainty);
        result.analysis = std::move(legacy);
    }
    if (result && cache && !stop.stop_requested()) {
        try {
            std::string detail;
            if (auto section = encodeNotes(*result.analysis))
                (void)cache->update(identity, pitchTag, *section, detail, stop);
        } catch (const std::exception&) {
            // Rebuildable cache failures leave the worker result usable.
        }
    }
    if (stop.stop_requested()) return {audio::PitchAnalysisError::Cancelled, {}};
    return result;
}

bool loadMono(audio::WavReader& reader, std::span<float> mono, std::stop_token stop) {
    constexpr std::uint32_t blockFrames = 4096;
    const auto channels = reader.channels();
    std::vector<float> block(static_cast<std::size_t>(blockFrames) * channels);
    for (std::size_t position = 0; position < mono.size();) {
        if (stop.stop_requested()) return false;
        const auto frames = static_cast<std::uint32_t>(
            std::min<std::size_t>(blockFrames, mono.size() - position));
        if (reader.read(block.data(), frames) != frames) return false;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double sum = 0;
            for (std::size_t channel = 0; channel < channels; ++channel)
                sum += block[frame * channels + channel];
            mono[position + frame] = static_cast<float>(sum / channels);
        }
        position += frames;
    }
    return true;
}

} // namespace

audio::PitchAnalysisResult buildSourcePitchAnalysisFromMono(const AnalysisIdentity& identity,
    std::span<const float> mono, AnalysisCache* cache, std::stop_token stop) {
    using Error = audio::PitchAnalysisError;
    if (audio::onAudioThread) return {Error::Realtime, {}};
    if (stop.stop_requested()) return {Error::Cancelled, {}};
    if (identity.frames != mono.size() || mono.empty()) return {Error::InvalidSource, {}};
    try {
        if (cache) {
            auto pitch = readNotes(*cache, identity, stop);
            if (stop.stop_requested()) return {Error::Cancelled, {}};
            if (pitch) return {Error::None, std::move(pitch)};
        }
        return analyzeAndCache(identity, mono, cache, stop, {});
    } catch (const std::bad_alloc&) {
        return {Error::Resource, {}};
    }
}

ClipPitchResult buildClipPitchAnalysis(const Store& store, std::int64_t id, std::stop_token stop) {
    if (audio::onAudioThread) return fail(ClipPitchProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
    return buildClipPitchAnalysis(store, id, stop, nullptr, &defaultAnalysisCache());
}

ClipPitchResult buildClipPitchAnalysis(const Store& store, std::int64_t id, std::stop_token stop,
                                       audio::DecodeCache* decoding, AnalysisCache* cache) {
    if (audio::onAudioThread) return fail(ClipPitchProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
    std::optional<Source> selected;
    std::optional<audio::PitchAnalysis> legacy;
    const auto withFallback = [&](ClipPitchResult failure) {
        if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
        if (selected && legacy && failure.problem != ClipPitchProblem::Cancelled)
            return success(id, std::move(*selected), std::move(*legacy));
        return failure;
    };
    try {
        selected = findSource(store, id);
        if (!selected) return fail(ClipPitchProblem::NotAudio);
        auto& source = *selected;
        auto& identity = source.identity;
        if (identity.frames != 0 && source.end > identity.frames)
            return fail(ClipPitchProblem::InvalidSource, "clip read window exceeds the source");
        const bool hasDimensions = identity.sampleRate && identity.channels && identity.frames;
        if (cache && hasDimensions) {
            auto pitch = readNotes(*cache, identity, stop);
            if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
            if (pitch && pitch->boundaryUncertainty) return success(id, std::move(source), std::move(*pitch));
            legacy = std::move(pitch);
        }

        std::string detail;
        const auto path = resolveMedia(projectFolder(store), source.pathFields, detail, stop);
        if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
        if (path.empty()) return withFallback(fail(ClipPitchProblem::InvalidSource, std::move(detail)));
        auto& decode = decoding ? *decoding : audio::defaultDecodeCache();
        const auto playable = decode.playable(path, stop);
        struct Pin {
            audio::DecodeCache& cache;
            const std::filesystem::path& file;
            ~Pin() { cache.release(file); }
        } pin{decode, playable};

        audio::WavReader reader(playable);
        if ((identity.sampleRate && identity.sampleRate != reader.sampleRate()) ||
            (identity.channels && identity.channels != reader.channels()) ||
            (identity.frames && identity.frames != reader.frames()) || source.end > reader.frames())
            return withFallback(fail(ClipPitchProblem::InvalidSource, "source dimensions changed"));
        identity.sampleRate = reader.sampleRate();
        identity.channels = reader.channels();
        identity.frames = reader.frames();
        if (cache && !hasDimensions) {
            auto pitch = readNotes(*cache, identity, stop);
            if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
            if (pitch && pitch->boundaryUncertainty) return success(id, std::move(source), std::move(*pitch));
            legacy = std::move(pitch);
        }
        if (identity.frames > maxSourceFrames)
            return withFallback(fail(ClipPitchProblem::Resource, "source exceeds pitch analysis PCM budget"));
        std::vector<float> mono(static_cast<std::size_t>(identity.frames));
        if (!loadMono(reader, mono, stop))
            return withFallback(fail(ClipPitchProblem::Read));
        const auto verified = blake3File(path, stop);
        if (stop.stop_requested()) return fail(ClipPitchProblem::Cancelled);
        if (!verified || verified.hex != identity.hash)
            return withFallback(fail(ClipPitchProblem::InvalidSource, "source changed during pitch analysis read"));

        auto result = analyzeAndCache(identity, mono, cache, stop, std::move(legacy));
        if (stop.stop_requested() || result.error == audio::PitchAnalysisError::Cancelled)
            return fail(ClipPitchProblem::Cancelled);
        if (!result) return fail(result.error == audio::PitchAnalysisError::Resource ?
                                 ClipPitchProblem::Resource : ClipPitchProblem::InvalidSource);
        return success(id, std::move(source), std::move(*result.analysis));
    } catch (const std::bad_alloc&) {
        return withFallback(fail(ClipPitchProblem::Resource));
    } catch (const audio::DecodeError& error) {
        if (error.problem() == audio::DecodeProblem::Cancelled) return fail(ClipPitchProblem::Cancelled);
        return withFallback(fail(ClipPitchProblem::Read, error.what()));
    } catch (const std::invalid_argument& error) {
        return withFallback(fail(ClipPitchProblem::InvalidSource, error.what()));
    } catch (const std::exception& error) {
        return withFallback(fail(ClipPitchProblem::Read, error.what()));
    }
}

} // namespace adi::media
