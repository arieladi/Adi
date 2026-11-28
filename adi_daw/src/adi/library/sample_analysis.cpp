// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_analysis.hpp"
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/transients.hpp"
#include "adi/audio/warp_source_analysis.hpp"
#include "adi/dsp/loudness.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace adi::library {
namespace {
const std::string sectionTag = "ALIB";
constexpr std::uint32_t sectionVersion = 2; // Adds measured integrated loudness.
constexpr std::uint32_t configuration = 2; // Includes native AIFF title and author tags.
constexpr std::size_t maximumTagBytes = 32768;
constexpr std::uint32_t blockFrames = 4096;

using Kind = audio::SampleKind;
using Source = audio::SampleKindSource;
bool validTempo(double bpm) { return std::isfinite(bpm) && bpm > 0 && bpm <= 1000; }

class SampleLoudness {
public:
    explicit SampleLoudness(const audio::WavReader& wav) {
        const auto rate = wav.sampleRate();
        const auto channels = static_cast<std::size_t>(wav.channels());
        if (rate < 8000 || wav.frames() < (static_cast<std::uint64_t>(rate) * 2 + 4) / 5) return;
        const auto speakers = wav.speakerMask();
        constexpr std::uint32_t lfe = 0x8;
        constexpr std::uint32_t surround = 0x730; // Back left/right/centre and side left/right.
        constexpr std::uint32_t horizontalRoles = 0x7ff;
        if ((speakers == 0 && channels > 2) || (speakers != 0 &&
            (((speakers & ~horizontalRoles) != 0) || static_cast<std::size_t>(std::popcount(speakers)) != channels)))
            return;
        std::vector<double> weights(channels, 1);
        if (speakers != 0) {
            std::size_t channel = 0;
            for (std::uint32_t bit = 1; bit <= horizontalRoles; bit <<= 1) {
                if ((speakers & bit) == 0) continue;
                if (bit == lfe) weights[channel] = 0;
                else if (bit & surround) weights[channel] = dsp::LoudnessMeter::kSurroundWeight;
                ++channel;
            }
        }
        meter_.emplace();
        meter_->prepare(rate, weights);
        planar_.resize(channels * blockFrames);
        inputs_.resize(channels);
        for (std::size_t channel = 0; channel < channels; ++channel)
            inputs_[channel] = planar_.data() + channel * blockFrames;
    }

    void append(std::span<const float> pcm, std::uint32_t frames) {
        if (!meter_) return;
        for (std::size_t channel = 0; channel < inputs_.size(); ++channel) {
            auto* plane = planar_.data() + channel * blockFrames;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto sample = pcm[frame * inputs_.size() + channel];
                plane[frame] = std::isfinite(sample) ? std::clamp(sample, -1e12f, 1e12f) : 0.0f;
            }
        }
        meter_->process(inputs_.data(), static_cast<int>(frames));
    }

    [[nodiscard]] std::optional<float> read() const {
        if (!meter_) return {};
        const auto lufs = meter_->readings().integrated;
        if (!std::isfinite(lufs)) return {};
        return static_cast<float>(lufs);
    }
private:
    std::optional<dsp::LoudnessMeter> meter_;
    std::vector<float> planar_;
    std::vector<const float*> inputs_;
};

template<class UInt>
void put(std::vector<std::byte>& bytes, UInt value) {
    for (std::size_t n = 0; n < sizeof(UInt); ++n)
        bytes.push_back(static_cast<std::byte>((value >> (n * 8)) & 255u));
}

class SectionReader {
public:
    explicit SectionReader(std::span<const std::byte> data) : data_(data) {}
    template<class UInt>
    UInt get() {
        if (data_.size() < sizeof(UInt)) throw std::runtime_error("short sample analysis");
        std::uint64_t value = 0;
        for (std::size_t n = 0; n < sizeof(UInt); ++n)
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data_[n])) << (8 * n);
        data_ = data_.subspan(sizeof(UInt));
        return static_cast<UInt>(value);
    }
    float getFloat() {
        const auto value = std::bit_cast<float>(get<std::uint32_t>());
        if (!std::isfinite(value)) throw std::runtime_error("invalid sample measurement");
        return value;
    }
    std::string getTag() {
        const auto count = get<std::uint16_t>();
        if (count > data_.size() || count > maximumTagBytes)
            throw std::runtime_error("short sample tag");
        std::string text(reinterpret_cast<const char*>(data_.data()), count);
        data_ = data_.subspan(count);
        return text;
    }
    [[nodiscard]] bool empty() const { return data_.empty(); }
private:
    std::span<const std::byte> data_;
};

// The fixed numeric fields are followed by length-prefixed read-only tags.
// Tempo zero and key 255 represent missing values.
media::AnalysisSection encode(const SampleAnalysis& value, const media::AnalysisIdentity& identity) {
    media::AnalysisSection section{sectionVersion, configuration, {}};
    auto& bytes = section.bytes;
    const auto putFloat = [&](float number) { put(bytes, std::bit_cast<std::uint32_t>(number)); };
    put(bytes, identity.sampleRate);
    put(bytes, identity.channels);
    put(bytes, identity.frames);
    put(bytes, static_cast<std::uint8_t>(value.classification.kind));
    put(bytes, static_cast<std::uint8_t>(value.classification.source));
    put(bytes, static_cast<std::uint8_t>(value.classification.bars.value_or(0)));
    put(bytes, static_cast<std::uint8_t>(value.tempoSource));
    put(bytes, static_cast<std::uint8_t>(value.keySource));
    put(bytes, static_cast<std::uint8_t>(value.key ? value.key->tonic + (value.key->minor ? 12 : 0) : 255));
    put(bytes, std::bit_cast<std::uint64_t>(value.tempoBpm.value_or(0)));
    put(bytes, std::bit_cast<std::uint64_t>(value.classification.bpm.value_or(0)));
    putFloat(value.keyStrength);
    put(bytes, static_cast<std::uint8_t>(value.integratedLufs.has_value()));
    if (value.integratedLufs) putFloat(*value.integratedLufs);
    const auto putDescriptors = [&](const SampleDescriptors& descriptors) {
        putFloat(descriptors.brightnessHz);
        putFloat(descriptors.hitDensityHz);
        putFloat(descriptors.rmsDbfs);
        for (const auto number : descriptors.spectralEnergy) putFloat(number);
        for (const auto number : descriptors.envelopeEnergy) putFloat(number);
        for (const auto number : descriptors.onsetDensity) putFloat(number);
        for (const auto number : descriptors.maskingEnergy) putFloat(number);
    };
    putDescriptors(value.descriptors);
    put(bytes, static_cast<std::uint8_t>(value.similarityPrefix.has_value()));
    if (value.similarityPrefix) putDescriptors(*value.similarityPrefix);
    std::uint16_t count = 0;
    std::size_t total = 0;
    for (const auto& tag : value.tags) {
        if (count == 256 || tag.size() > maximumTagBytes - total) break;
        total += tag.size();
        ++count;
    }
    put(bytes, count);
    for (const auto& tag : std::span(value.tags).first(count)) {
        put(bytes, static_cast<std::uint16_t>(tag.size()));
        for (const unsigned char ch : std::span(reinterpret_cast<const unsigned char*>(tag.data()), tag.size()))
            bytes.push_back(static_cast<std::byte>(ch));
    }
    return section;
}

std::optional<SampleAnalysis> decode(const media::SampleAnalysis& cache) {
    const auto section = cache.sections.find(sectionTag);
    if (section == cache.sections.end() || section->second.version != sectionVersion ||
        section->second.configuration != configuration ||
        section->second.bytes.size() > maximumTagBytes + 2048) return {};
    try {
        SectionReader data(section->second.bytes);
        if (data.get<std::uint32_t>() != cache.identity.sampleRate ||
            data.get<std::uint32_t>() != cache.identity.channels ||
            data.get<std::uint64_t>() != cache.identity.frames || cache.identity.frames == 0) return {};
        SampleAnalysis value{};
        value.contentHash = cache.identity.hash;
        value.lengthSeconds = static_cast<double>(cache.identity.frames) / cache.identity.sampleRate;
        const auto kind = data.get<std::uint8_t>();
        const auto kindSource = data.get<std::uint8_t>();
        const auto bars = data.get<std::uint8_t>();
        const auto tempoSource = data.get<std::uint8_t>();
        const auto keySource = data.get<std::uint8_t>();
        const auto key = data.get<std::uint8_t>();
        if (kind > static_cast<unsigned>(Kind::Loop) ||
            kindSource > static_cast<unsigned>(Source::FileTag) ||
            tempoSource > static_cast<unsigned>(Source::FileTag) ||
            keySource > static_cast<unsigned>(Source::FileTag) ||
            (bars != 0 && bars != 1 && bars != 2 && bars != 4 && bars != 8 && bars != 16) ||
            (key != 255 && key >= 24)) return {};
        value.classification.kind = static_cast<Kind>(kind);
        value.classification.source = static_cast<Source>(kindSource);
        value.tempoSource = static_cast<Source>(tempoSource);
        value.keySource = static_cast<Source>(keySource);
        if (bars != 0) value.classification.bars = bars;
        if (key != 255) value.key = SampleKey{static_cast<std::uint8_t>(key % 12), key >= 12};
        const auto bpm = std::bit_cast<double>(data.get<std::uint64_t>());
        const auto classificationBpm = std::bit_cast<double>(data.get<std::uint64_t>());
        if (bpm != 0) {
            if (!validTempo(bpm)) return {};
            value.tempoBpm = bpm;
        }
        if (classificationBpm != 0) {
            if (!validTempo(classificationBpm)) return {};
            value.classification.bpm = classificationBpm;
        }
        value.keyStrength = data.getFloat();
        if (value.keyStrength < 0 || value.keyStrength > 1) return {};
        const auto loudnessPresent = data.get<std::uint8_t>();
        if (loudnessPresent > 1) return {};
        if (loudnessPresent) value.integratedLufs = data.getFloat();
        const auto getDescriptors = [&]() {
            SampleDescriptors descriptors;
            descriptors.brightnessHz = data.getFloat();
            descriptors.hitDensityHz = data.getFloat();
            descriptors.rmsDbfs = data.getFloat();
            if (descriptors.brightnessHz < 0 || descriptors.brightnessHz > 20000 || descriptors.hitDensityHz < 0 ||
                descriptors.rmsDbfs < -200 || descriptors.rmsDbfs > 240)
                throw std::runtime_error("invalid sample measurement range");
            const auto getShares = [&](auto& shares) {
                for (auto& number : shares) {
                    number = data.getFloat();
                    if (number < 0 || number > 1) throw std::runtime_error("invalid sample share");
                }
            };
            getShares(descriptors.spectralEnergy);
            getShares(descriptors.envelopeEnergy);
            getShares(descriptors.onsetDensity);
            getShares(descriptors.maskingEnergy);
            return descriptors;
        };
        value.descriptors = getDescriptors();
        const auto prefixPresent = data.get<std::uint8_t>();
        const bool wantsPrefix = cache.identity.frames > static_cast<std::uint64_t>(cache.identity.sampleRate) * sampleSimilarityWindowSeconds &&
            cache.identity.frames <= static_cast<std::uint64_t>(cache.identity.sampleRate) * sampleSimilarityMaximumSeconds;
        if (prefixPresent != (wantsPrefix ? 1 : 0)) return {};
        if (prefixPresent) value.similarityPrefix = getDescriptors();
        const auto count = data.get<std::uint16_t>();
        if (count > 256) return {};
        for (unsigned n = 0; n < count; ++n) value.tags.push_back(data.getTag());
        if (data.empty()) return value;
    } catch (const std::exception&) {}
    return {};
}

Kind kindHint(std::optional<bool> loop) {
    if (!loop) return Kind::Unknown;
    return *loop ? Kind::Loop : Kind::OneShot;
}

SampleAnalysisResult failure(SampleAnalysisProblem problem) {
    return {problem, {}, {}, false};
}
}

SampleAnalysis applySampleOverrides(SampleAnalysis value, const SampleAnalysisOverrides& overrides) {
    if (overrides.classification.kind != Kind::Unknown) {
        if (value.classification.kind != overrides.classification.kind) value.classification.bars.reset();
        value.classification.kind = overrides.classification.kind;
        value.classification.source = Source::User;
    }
    if (overrides.classification.bpm && validTempo(*overrides.classification.bpm)) {
        value.tempoBpm = overrides.classification.bpm;
        value.tempoSource = Source::User;
        if (value.classification.kind == Kind::Loop) {
            if (value.classification.bpm != overrides.classification.bpm) value.classification.bars.reset();
            value.classification.bpm = overrides.classification.bpm;
        }
    }
    if (overrides.key && overrides.key->tonic < 12) {
        value.key = overrides.key;
        value.keySource = Source::User;
        value.keyStrength = 1;
    }
    value.tags.insert(value.tags.end(), overrides.tags.begin(), overrides.tags.end());
    std::sort(value.tags.begin(), value.tags.end());
    value.tags.erase(std::unique(value.tags.begin(), value.tags.end()), value.tags.end());
    return value;
}

SampleAnalysisMetadata portableSampleMetadata(const SampleAnalysis& value) {
    SampleAnalysisMetadata result;
    result.contentHash = value.contentHash;
    if (value.tempoSource != Source::User) {
        result.tempoBpm = value.tempoBpm;
        result.tempoSource = value.tempoSource;
    }
    if (value.keySource != Source::User) {
        result.key = value.key;
        result.keySource = value.keySource;
        result.keyStrength = value.keyStrength;
    }
    return result;
}

void applySampleMetadata(SampleAnalysis& value, const SampleAnalysisMetadata& metadata) {
    if (value.contentHash.empty() || value.contentHash != metadata.contentHash) return;
    if (metadata.tempoBpm && validTempo(*metadata.tempoBpm) && metadata.tempoSource < Source::User &&
        (!value.tempoBpm || value.tempoSource < metadata.tempoSource)) {
        value.tempoBpm = metadata.tempoBpm;
        value.tempoSource = metadata.tempoSource;
        if (value.classification.kind == Kind::Loop && value.classification.bpm != value.tempoBpm) {
            value.classification.bpm = value.tempoBpm;
            value.classification.bars.reset();
        }
    }
    if (metadata.key && metadata.key->tonic < 12 && metadata.keySource < Source::User &&
        std::isfinite(metadata.keyStrength) && metadata.keyStrength >= 0 && metadata.keyStrength <= 1 &&
        (!value.key || value.keySource < metadata.keySource)) {
        value.key = metadata.key;
        value.keySource = metadata.keySource;
        value.keyStrength = metadata.keyStrength;
    }
}

SampleAnalysisResult SampleAnalysisService::read(const std::string& hash, std::stop_token stop) const {
    if (audio::onAudioThread) return failure(SampleAnalysisProblem::Realtime);
    if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
    const auto stored = analysis_.readByHash(hash, stop);
    if (stored) {
        if (auto value = decode(*stored.data))
            return {SampleAnalysisProblem::None, std::move(value), hash, true};
    } else if (stored.problem == media::AnalysisCacheProblem::Cancelled) {
        return failure(SampleAnalysisProblem::Cancelled);
    }
    return failure(SampleAnalysisProblem::Missing);
}

SampleAnalysisResult SampleAnalysisService::analyze(const std::filesystem::path& source,
                                                    const std::string& expectedHash, std::stop_token stop,
                                                    std::optional<audio::WarpTimingSettings> timing) {
    if (audio::onAudioThread) return failure(SampleAnalysisProblem::Realtime);
    if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
    try {
        // The file can change between a published index entry and measurement.
        // Verify the bytes before decoding (including any decoder cache reuse)
        // and before publishing. Indexed cache hits already identify old bytes.
        bool needsTiming = timing.has_value();
        std::optional<audio::WarpTimingFlight> timingFlight;
        const auto readPrevious = [&](const std::string& contentHash) {
            if (timing) {
                timingFlight = audio::acquireWarpTimingFlight(analysis_, contentHash, *timing, stop);
                if (!timingFlight)
                    return failure(stop.stop_requested() ? SampleAnalysisProblem::Cancelled : SampleAnalysisProblem::Read);
                if (audio::readWarpTimingByHash(analysis_, contentHash, *timing, stop)) {
                    needsTiming = false;
                    timingFlight.reset();
                }
            }
            return read(contentHash, stop);
        };
        SampleAnalysisResult cached;
        const auto needsMoreWork = [&] {
            return (cached && needsTiming) || cached.problem == SampleAnalysisProblem::Missing;
        };
        if (!expectedHash.empty()) {
            cached = readPrevious(expectedHash);
            if (!needsMoreWork()) return cached;
        }
        const auto hash = media::blake3File(source, stop);
        if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
        if (!hash) return failure(SampleAnalysisProblem::Read);
        if (!expectedHash.empty() && expectedHash != hash.hex)
            return failure(SampleAnalysisProblem::SourceChanged);
        if (expectedHash.empty()) {
            cached = readPrevious(hash.hex);
            if (!needsMoreWork()) return cached;
        }
        const auto playable = decode_.playable(source, stop);
        struct Release {
            audio::DecodeCache& cache;
            const std::filesystem::path& path;
            bool pinned;
            ~Release() { if (pinned) try { cache.release(path); } catch (...) {} }
        } release{decode_, playable, playable != source};
        audio::WavReader pcm(playable);
        const media::AnalysisIdentity identity{hash.hex, pcm.sampleRate(), pcm.channels(), pcm.frames()};
        std::optional<audio::WarpTimingAnalyzer> timingAnalyzer;
        if (needsTiming && identity.sampleRate >= 8000)
            timingAnalyzer.emplace(identity.sampleRate, identity.channels, *timing);
        if (!timingAnalyzer) timingFlight.reset();
        if (cached && !timingAnalyzer) return cached;
        const auto metadata = cached ? SampleFileTags{} : readSampleFileTags(source, stop);
        if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
        std::optional<audio::TransientAnalyzer> transients;
        std::optional<SampleDescriptorAnalyzer> descriptors;
        std::optional<SampleLoudness> loudness;
        std::optional<SampleKeyAnalyzer> tonality;
        const auto prefixFrames = static_cast<std::uint64_t>(identity.sampleRate) * sampleSimilarityWindowSeconds;
        std::optional<SampleDescriptorAnalyzer> prefix;
        if (!cached) {
            transients.emplace(identity.sampleRate, identity.channels);
            descriptors.emplace(identity.sampleRate, identity.channels);
            loudness.emplace(pcm);
            if (identity.sampleRate >= 1000 && !metadata.tagKey) tonality.emplace(identity.sampleRate, identity.channels);
            if (identity.frames > prefixFrames &&
                identity.frames <= static_cast<std::uint64_t>(identity.sampleRate) * sampleSimilarityMaximumSeconds)
                prefix.emplace(identity.sampleRate, identity.channels);
        }
        std::vector<float> block(static_cast<std::size_t>(blockFrames) * identity.channels);
        for (std::uint64_t left = identity.frames; left != 0;) {
            if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
            const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, blockFrames));
            if (pcm.read(block.data(), frames) != frames) return failure(SampleAnalysisProblem::Read);
            const auto samples = std::span(block).first(static_cast<std::size_t>(frames) * identity.channels);
            if (timingAnalyzer && !timingAnalyzer->append(samples, stop))
                return failure(stop.stop_requested() ? SampleAnalysisProblem::Cancelled : SampleAnalysisProblem::Read);
            if (transients && (!transients->append(samples) || !descriptors->append(samples)))
                return failure(SampleAnalysisProblem::Read);
            if (loudness) loudness->append(samples, frames);
            if (tonality && !tonality->append(samples)) return failure(SampleAnalysisProblem::Read);
            const auto offset = identity.frames - left;
            if (prefix && offset < prefixFrames) {
                const auto remaining = std::min<std::uint64_t>(frames, prefixFrames - offset);
                if (!prefix->append(samples.first(static_cast<std::size_t>(remaining) * identity.channels)))
                    return failure(SampleAnalysisProblem::Read);
            }
            left -= frames;
        }
        const auto measuredTiming = timingAnalyzer ? timingAnalyzer->finish(stop) : std::optional<audio::WarpTimingAnalysis>{};
        if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
        if (timingAnalyzer && !measuredTiming) return failure(SampleAnalysisProblem::Read);
        const auto verified = media::blake3File(source, stop);
        if (stop.stop_requested()) return failure(SampleAnalysisProblem::Cancelled);
        if (!verified || verified.hex != hash.hex) return failure(SampleAnalysisProblem::SourceChanged);

        std::string error;
        if (timingAnalyzer) {
            const auto stored = audio::storeWarpTiming(analysis_, identity, *timing, *measuredTiming, error, stop);
            timingFlight.reset();
            if (stored == media::AnalysisCacheProblem::Cancelled) return failure(SampleAnalysisProblem::Cancelled);
            if (stored != media::AnalysisCacheProblem::None) return failure(SampleAnalysisProblem::CacheWrite);
            if (cached) return cached;
        }
        const auto onsets = transients->finish();
        const auto measured = descriptors->finish(onsets.frames);
        if (!onsets || !measured) return failure(SampleAnalysisProblem::Read);
        const auto detectedKey = tonality ? tonality->finish() : std::optional<DetectedSampleKey>{};
        audio::SampleKindHints hints;
        hints.fileTag = {kindHint(metadata.tagLoop), metadata.tagBpm};
        hints.acid = {kindHint(metadata.acidLoop), metadata.acidBpm};
        SampleAnalysis value{};
        value.contentHash = identity.hash;
        value.lengthSeconds = static_cast<double>(identity.frames) / identity.sampleRate;
        value.integratedLufs = loudness->read();
        value.classification = audio::classifySample(onsets.frames, identity.frames, identity.sampleRate, hints);
        value.descriptors = *measured;
        if (prefix) {
            const auto end = std::lower_bound(onsets.frames.begin(), onsets.frames.end(), prefixFrames);
            value.similarityPrefix = prefix->finish(std::span(onsets.frames.begin(), end));
            if (!value.similarityPrefix) return failure(SampleAnalysisProblem::Read);
        }
        value.tags = metadata.tags;
        if (metadata.tagBpm && validTempo(*metadata.tagBpm)) {
            value.tempoBpm = metadata.tagBpm;
            value.tempoSource = Source::FileTag;
        } else if (metadata.acidBpm && validTempo(*metadata.acidBpm)) {
            value.tempoBpm = metadata.acidBpm;
            value.tempoSource = Source::Acid;
        } else if (value.classification.bpm) {
            value.tempoBpm = value.classification.bpm;
        } else if (onsets.frames.size() >= 3) {
            const auto estimated = audio::autoWarpFromOnsets(onsets.frames, identity.frames, identity.sampleRate, {}, stop);
            if (estimated && estimated.confidence >= 0.6) value.tempoBpm = estimated.estimatedBpm;
        }
        if (metadata.tagKey) {
            value.key = metadata.tagKey;
            value.keySource = Source::FileTag;
            value.keyStrength = 1;
        } else if (detectedKey) {
            value.key = detectedKey->key;
            value.keyStrength = detectedKey->strength;
        }

        const auto stored = analysis_.update(identity, sectionTag, encode(value, identity), error, stop);
        if (stored == media::AnalysisCacheProblem::Cancelled) return failure(SampleAnalysisProblem::Cancelled);
        if (stored != media::AnalysisCacheProblem::None) return failure(SampleAnalysisProblem::CacheWrite);
        return {SampleAnalysisProblem::None, std::move(value), hash.hex, false};
    } catch (const std::exception&) {
        return failure(stop.stop_requested() ? SampleAnalysisProblem::Cancelled : SampleAnalysisProblem::Read);
    }
}

bool samplePreviewLoops(const SampleAnalysis& value) noexcept {
    return value.classification.kind == Kind::Loop;
}
}
