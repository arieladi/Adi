// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/queue.hpp"
#include "adi/audio/aiff_file.hpp"
#include "adi/render/aiff_writer.hpp"
#include "adi/render/delivery_src.hpp"
#include "adi/render/flac_writer.hpp"
#include "adi/render/mp3_writer.hpp"
#include "adi/render/opus_writer.hpp"
#include "adi/render/vorbis_writer.hpp"
#include "adi/audio/pcm_stream.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <utf8proc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace adi::render {
namespace fs = std::filesystem;
static_assert(std::is_nothrow_move_assignable_v<Result>);
static_assert(std::is_nothrow_move_assignable_v<std::string>);
namespace {
// Match the library's pinned STABLE + COMPOSE + CASEFOLD policy. The key is
// only for comparison: neither normalization nor fold expansion renames files.
std::string foldedFilename(const fs::path& path) {
    const auto name=media::pathUtf8(path.filename());
    if(name.size()>static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max()))
        throw std::runtime_error("export filename is too long to compare");
    utf8proc_uint8_t* data=nullptr;
    const auto flags=static_cast<utf8proc_option_t>(UTF8PROC_STABLE|UTF8PROC_COMPOSE|UTF8PROC_CASEFOLD);
    const auto size=utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(name.data()),
        static_cast<utf8proc_ssize_t>(name.size()),&data,flags);
    std::unique_ptr<utf8proc_uint8_t,decltype(&std::free)> owned(data,&std::free);
    if(size<0)throw std::runtime_error("invalid UTF-8 export directory entry");
    return {reinterpret_cast<const char*>(data),static_cast<std::size_t>(size)};
}
bool occupiedNative(const fs::path& path) {
    std::error_code error;const auto status=fs::symlink_status(path,error);
    if(error==std::errc::no_such_file_or_directory)return false;
    if(error)throw fs::filesystem_error("inspect export destination",path,error);
    return status.type()!=fs::file_type::not_found;
}
bool occupiedBeforePublication(const fs::path& path) {
    if(occupiedNative(path))return true;
    const auto key=foldedFilename(path);
    for(const auto& entry:fs::directory_iterator(path.parent_path()))
        if(foldedFilename(entry.path())==key)return true;
    return false;
}
std::vector<std::string> foldedOccupants(const fs::path& path) {
    const auto key=foldedFilename(path);
    std::vector<std::string> names;
    for(const auto& entry:fs::directory_iterator(path.parent_path()))
        if(foldedFilename(entry.path())==key)names.push_back(media::pathUtf8(entry.path().filename()));
    std::sort(names.begin(),names.end());
    return names;
}
media::FileIdentity ordinaryOutputIdentity(const fs::path& path) {
    std::error_code error;
    const auto status=fs::symlink_status(path,error);
    if(error || !fs::is_regular_file(status))
        throw std::runtime_error("Replace requires an existing ordinary file at the requested path");
    const auto identity=media::readFileIdentity(path,error);
    if(!identity)throw std::runtime_error("cannot identify replacement destination: "+error.message());
    return *identity;
}
void verifyReplacement(const Result& result) {
    const auto& binding=*result.replacement;
    if(result.output!=binding.target || foldedOccupants(result.output)!=binding.foldedNames)
        throw std::runtime_error("replacement path or folded collision set changed since preflight");
    if(!binding.existing) {
        if(occupiedNative(result.output))throw std::runtime_error("replacement destination arrived after absent preflight");
        return;
    }
    if(ordinaryOutputIdentity(result.output)!=*binding.existing)
        throw std::runtime_error("replacement destination identity changed since preflight");
    const auto digest=media::blake3File(result.output);
    if(!digest || digest.hex!=binding.blake3 || ordinaryOutputIdentity(result.output)!=*binding.existing)
        throw std::runtime_error("replacement destination contents or identity changed since preflight");
    if(foldedOccupants(result.output)!=binding.foldedNames)
        throw std::runtime_error("replacement collision set changed during validation");
}
std::string portableName(std::string name) {
    // The supplied text is a filename component and never becomes a path.
    for (char& c : name)
        if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == "..") throw std::invalid_argument("export name is empty");
    if (name.size() > 200) throw std::invalid_argument("export name exceeds 200 UTF-8 bytes");
    auto stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const bool numberedDevice = (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        ((stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') ||
         stem.substr(3) == "\xC2\xB9" || stem.substr(3) == "\xC2\xB2" || stem.substr(3) == "\xC2\xB3");
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$" || numberedDevice) name = "_" + name;
    return name;
}

const char* containerName(ExportContainer type) {
    switch (type) {
    case ExportContainer::Wav: return "wav";
    case ExportContainer::Flac: return "flac";
    case ExportContainer::Aiff: return "aiff";
    case ExportContainer::Mp3: return "mp3";
    case ExportContainer::OggVorbis: return "ogg_vorbis";
    case ExportContainer::OggOpus: return "opus";
    }
    throw std::invalid_argument("unknown export container");
}

std::string containerSuffix(ExportContainer type) {
    return type == ExportContainer::OggVorbis ? ".ogg" : std::string(".") + containerName(type);
}
// The {format} name token: the container, never the PCM depth (an AIFF delivery names itself "aiff").
const char* formatName(const Job& job) {
    return job.container == PcmContainer::Aiff ? "aiff" : containerName(job.encoding.container);
}

int deliveryChannels(const Job& job, ChannelFold fold) {
    switch (fold) {
        case ChannelFold::Preserve: return job.session.channels;
        case ChannelFold::StereoAverage:
            if (job.session.channels > 2 || job.niStems)
                throw std::invalid_argument("stereo fold requires an ordinary mono or stereo source");
            return 1;
    }
    throw std::invalid_argument("unknown export channel fold");
}
void validateEncoding(const Job& j, const Encoding& encoding, int channels) {
    if (encoding.format != audio::WavFormat::Float32 && encoding.format != audio::WavFormat::Pcm16 && encoding.format != audio::WavFormat::Pcm24)
        throw std::invalid_argument("unsupported export sample encoding");
    if (!audio::validDither(encoding.dither))
        throw std::invalid_argument("unsupported export dither");
    (void)containerName(encoding.container);
    if (encoding.speakerMask != 0 &&
        (encoding.container != ExportContainer::Wav || j.niStems ||
         !audio::validWavSpeakerMask(static_cast<std::uint16_t>(channels), encoding.speakerMask))) {
        throw std::invalid_argument("An explicit WAV speaker mask must match the ordinary delivery's channels");
    }
    if (encoding.container == ExportContainer::Aiff && (j.niStems || j.exactWavFilename))
        throw std::invalid_argument("AIFF export requires a template filename and an ordinary render source");
    if (encoding.container == ExportContainer::Flac) {
        if (j.niStems || j.exactWavFilename || channels > 8 || encoding.format == audio::WavFormat::Float32)
            throw std::invalid_argument("FLAC export requires PCM16 or PCM24, 1..8 channels and a template name");
        if (!flacExportAvailable()) throw std::runtime_error("FLAC export requires the pinned libFLAC build");
    }
    if (encoding.container == ExportContainer::Mp3) {
        const auto rate = j.session.sampleRate;
        if ((rate != 32000 && rate != 44100 && rate != 48000) || channels > 2 ||
            j.niStems || j.exactWavFilename || j.normalization.mode != NormalizeMode::Off)
            throw std::invalid_argument("MP3 needs one or two channels, 32/44.1/48 kHz, a template name and Normalize Off");
        if (!mp3ExportAvailable()) throw std::runtime_error("MP3 export requires the pinned LAME build");
    }
    if (encoding.container == ExportContainer::OggVorbis) {
        const auto rate = j.session.sampleRate;
        if (rate < 8000 || rate > 192000 || channels > 2 ||
            !std::isfinite(encoding.vorbisQuality) || encoding.vorbisQuality < -0.1f ||
            encoding.vorbisQuality > 1.0f ||
            j.niStems || j.exactWavFilename || j.normalization.mode != NormalizeMode::Off)
            throw std::invalid_argument("Vorbis needs one or two channels, 8-192 kHz, quality -0.1 to 1, a template name and Normalize Off");
        if (!vorbisExportAvailable()) throw std::runtime_error("Ogg Vorbis export requires the pinned Xiph build");
    }
    if (encoding.container == ExportContainer::OggOpus) {
        if (j.session.sampleRate != 48000 || !validOpusBitRate(
                static_cast<unsigned>(channels), encoding.opusBitRate) ||
            j.niStems || j.exactWavFilename || j.normalization.mode != NormalizeMode::Off) {
            throw std::invalid_argument("Opus needs mono/stereo at 48 kHz, a supported bitrate and Normalize Off");
        }
        if (!opusExportAvailable()) throw std::runtime_error("Opus export requires the pinned Xiph build");
    }
}

std::string deliveryName(const std::string& base, const DeliverySpec& delivery) {
    auto name = base + delivery.nameSuffix + containerSuffix(delivery.encoding.container);
    if (portableName(name) != name) throw std::invalid_argument("Invalid export delivery filename");
    validateOutputName(media::pathFromUtf8(name));
    return name;
}

const char* tailModeName(TailMode mode) {
    switch (mode) {
        case TailMode::Off: return "off";
        case TailMode::Fixed: return "fixed";
        case TailMode::UntilSilent: return "until_silent";
    }
    return "unknown";
}
void validateTail(const Job& job, bool plannedRange) {
    const auto& tail = job.tail;
    const bool hasTail = tail.mode != TailMode::Off;
    if (job.gaplessBatch) {
        if (hasTail || job.renderAsLoop)
            throw std::invalid_argument("gapless batches cannot render independent tails or loops");
        if (!plannedRange)
            throw std::invalid_argument("gapless render requires a continuous region plan");
    }
    if ((hasTail && job.renderAsLoop) || (job.niStems && (hasTail || job.renderAsLoop)))
        throw std::invalid_argument("tail extension and Render as Loop require separate ordinary renders");
    if (!hasTail) return;
    if (tail.maxMilliseconds == 0 || (tail.mode != TailMode::Fixed && tail.mode != TailMode::UntilSilent))
        throw std::invalid_argument("invalid export tail mode or duration");
    if (tail.mode == TailMode::UntilSilent &&
        (tail.holdMilliseconds == 0 || tail.holdMilliseconds > tail.maxMilliseconds ||
         !std::isfinite(tail.thresholdDbfs) || tail.thresholdDbfs > 0 || tail.thresholdDbfs < -300))
        throw std::invalid_argument("invalid export tail silence threshold or hold");
}

const char* signalTapName(engine::Tap tap) {
    switch (tap) {
        case engine::Tap::PreFx: return "pre_fx";
        case engine::Tap::PostFx: return "post_fx";
        case engine::Tap::PostFader: return "post_fader";
        case engine::Tap::PostMixer: return "post_mixer";
        case engine::Tap::Tail: break;
    }
    return nullptr;
}

void validate(const Job& j, bool plannedRange = false) {
    if (j.project.empty() || (!j.analysisOnly && j.directory.empty())) {
        throw std::invalid_argument("project and output directory are required for a delivery");
    }
    if (!j.regionId && (j.beginSample < 0 || j.endSample <= j.beginSample)) throw std::invalid_argument("export range must be nonempty and nonnegative");
    if (j.regionId && *j.regionId <= 0) throw std::invalid_argument("region ID must be positive");
    if (j.source.trackId < 0 || !signalTapName(j.source.resolvedTap()) ||
        (j.source.tap != engine::OfflineTap::PreFader && j.source.tap != engine::OfflineTap::PostFader))
        throw std::invalid_argument("invalid direct stem source or tap");
    validateTail(j, plannedRange);
    validateDeliveryProfile(j);
}
struct Stage {
    fs::path path;
    std::optional<StageCleanupFailure>& failure;
    Stage(const fs::path& parent, Result& result) : failure(result.stageCleanup) {
        static std::atomic<unsigned long long> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto next = parent / (".adi-render-" + std::to_string(tick) + "-" + std::to_string(serial.fetch_add(1)));
            std::error_code ec;
            if (fs::create_directory(next, ec)) { path = next; return; }
            if (ec && ec != std::errc::file_exists) throw fs::filesystem_error("create render stage", next, ec);
        }
        throw std::runtime_error("cannot reserve render stage");
    }
    ~Stage() { detail::discardStage(std::move(path), failure); }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
};
void prepareGroupIncrement(std::vector<Result>& files, const fs::path& directory) {
    if (!fs::is_directory(directory)) throw std::runtime_error("export directory does not exist");
    std::set<std::string> occupants;
    std::uint64_t entries = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entries == std::numeric_limits<std::uint64_t>::max() - 2) {
            throw std::runtime_error("export directory has too many entries");
        }
        ++entries;
        occupants.insert(foldedFilename(entry.path()));
    }
    const auto available = [&](const fs::path& path) {
        return !occupants.contains(foldedFilename(path)) && !occupiedNative(path);
    };
    if (std::ranges::all_of(files, [&](const Result& file) { return available(file.output); })) return;

    std::vector<fs::path> candidate;
    candidate.reserve(files.size());
    // Each existing file can occupy at most one ordinary numbered group.
    // The extra candidate also allows a native alias at the original spelling.
    for (std::uint64_t offset = 0; offset <= entries; ++offset) {
        candidate.clear();
        for (const auto& file : files) {
            auto name = file.output.stem();
            name += media::pathFromUtf8(" (" + std::to_string(offset + 2) + ")");
            name += file.output.extension();
            auto path = directory / name;
            validateOutputName(path);
            if (!available(path)) break;
            candidate.push_back(std::move(path));
        }
        if (candidate.size() == files.size()) {
            for (std::size_t i = 0; i < files.size(); ++i) files[i].output = std::move(candidate[i]);
            return;
        }
    }
    throw std::runtime_error("export collision set changed during preflight; retry");
}

void verifyPcmDelivery(const fs::path& file, ExportContainer container, unsigned rate,
                       unsigned channels, std::uint64_t frames,
                       const std::function<void()>& service) {
    if (service) service();
    auto input = audio::openPcmStream(file);
    if (!input.ready()) throw std::runtime_error("Cannot decode staged PCM delivery: " + input.error);
    const auto& info = input.stream->info();
    const auto source = container == ExportContainer::Aiff ? audio::SourceFormat::Aiff :
        (container == ExportContainer::Flac ? audio::SourceFormat::Flac : audio::SourceFormat::Riff);
    if (info.format != source || info.sampleRate != rate || info.channels != channels ||
        info.frames != frames) {
        throw std::runtime_error("Decoded PCM header disagrees with the render");
    }
    std::vector<float> samples(4096u * channels);
    while (true) {
        if (service) service();
        const auto read = input.stream->read(samples);
        if (read.status == audio::PcmReadStatus::End) break;
        if (read.status != audio::PcmReadStatus::Frames) {
            throw std::runtime_error("Cannot verify staged PCM delivery: " + input.stream->error());
        }
        const auto decoded = std::span<const float>(samples).first(
            static_cast<std::size_t>(read.frames) * channels);
        if (std::ranges::any_of(decoded, [](float sample) { return !std::isfinite(sample); })) {
            throw std::runtime_error("Decoded PCM delivery contains a nonfinite sample");
        }
    }
    if (input.stream->framesRead() != frames) {
        throw std::runtime_error("Decoded PCM length disagrees with the render");
    }
}

class DeliverySink {
public:
    DeliverySink(const fs::path& folder, const std::string& stem, const Job& job,
                 const Encoding& encoding, int channels, std::uint64_t total, std::uint64_t counter)
        : encoding_(encoding), seed_(encoding.ditherSeed ^ counter),
          rate_(static_cast<unsigned>(job.session.sampleRate)),
          channels_(static_cast<unsigned>(channels)), expectedFrames_(total) {
        const auto rate = rate_;
        output_ = folder / (stem + containerSuffix(encoding_.container));
        pcm_ = folder / (stem + ".wav");
        if (encoding_.container == ExportContainer::Mp3) {
            mp3_ = std::make_unique<Mp3Writer>(output_, rate, static_cast<unsigned>(channels), total);
        } else if (encoding_.container == ExportContainer::OggVorbis) {
            vorbis_ = std::make_unique<VorbisWriter>(output_, rate, static_cast<unsigned>(channels),
                                                      total, encoding_.vorbisQuality);
        } else if (encoding_.container == ExportContainer::OggOpus) {
            opus_ = std::make_unique<OpusWriter>(output_, static_cast<unsigned>(channels),
                                                  total, encoding_.opusBitRate);
        } else if (job.normalization.mode == NormalizeMode::Off) {
            wav_ = std::make_unique<audio::WavWriter>(pcm_, rate, static_cast<std::uint16_t>(channels),
                encoding_.format, nullptr, std::uint64_t{1} << 32, encoding_.dither, seed_, encoding_.speakerMask);
        } else {
            normalization_ = std::make_unique<NormalizationCapture>(folder / (stem + ".f64"),
                rate, channels, job.normalization);
        }
    }

    [[nodiscard]] bool requiresFloat32Range() const noexcept {
        return wav_ && encoding_.format == audio::WavFormat::Float32;
    }

    void append(std::span<const double> pcm, std::uint32_t frames) {
        if (mp3_) mp3_->write(pcm, frames);
        else if (vorbis_) vorbis_->write(pcm, frames);
        else if (opus_) opus_->write(pcm, frames);
        else if (normalization_) normalization_->append(pcm, frames);
        else wav_->write(pcm.data(), frames);
        writtenFrames_ += frames;
    }

    std::optional<NormalizationReport> finish(const std::function<void()>& service,
                                              std::optional<std::uint64_t> deliveryFrames = std::nullopt,
                                              const std::function<void(std::uint32_t)>& advanceFrames = {}) {
        const auto frames = deliveryFrames.value_or(expectedFrames_);
        if ((frames != 0 && writtenFrames_ != frames) || (expectedFrames_ != 0 && frames > expectedFrames_)) {
            throw std::runtime_error("Encoder received an incomplete input range");
        }
        std::optional<NormalizationReport> report;
        if (mp3_) mp3_->finish(service, deliveryFrames);
        else if (vorbis_) vorbis_->finish(service, deliveryFrames);
        else if (opus_) opus_->finish(service, deliveryFrames);
        else if (normalization_) report = normalization_->finish(pcm_, encoding_.format,
                                                                   encoding_.dither, seed_, service, advanceFrames,
                                                                   encoding_.speakerMask);
        else wav_->close();
        if (encoding_.container == ExportContainer::Aiff) encodeStagedAiff(pcm_, output_, service);
        if (encoding_.container == ExportContainer::Flac) encodeStagedFlac(pcm_, output_, service);
        if (!mp3_ && !vorbis_ && !opus_) {
            verifyPcmDelivery(output_, encoding_.container, rate_, channels_, writtenFrames_, service);
        }
        return report;
    }

    [[nodiscard]] const fs::path& output() const noexcept { return output_; }

private:
    Encoding encoding_;
    std::uint64_t seed_;
    unsigned rate_;
    unsigned channels_;
    std::uint64_t expectedFrames_;
    std::uint64_t writtenFrames_ = 0;
    fs::path pcm_, output_;
    std::unique_ptr<audio::WavWriter> wav_;
    std::unique_ptr<NormalizationCapture> normalization_;
    std::unique_ptr<Mp3Writer> mp3_;
    std::unique_ptr<VorbisWriter> vorbis_;
    std::unique_ptr<OpusWriter> opus_;
};

std::int64_t addLatency(std::int64_t position, std::int64_t latency) {
    if (latency > 0 && position > std::numeric_limits<std::int64_t>::max() - latency)
        throw std::invalid_argument("export range plus latency overflows");
    return position + latency; // position >= 0; cached endpoint latency is the sum of two int32 values
}
std::int64_t projectFrames(std::int64_t frames, std::int64_t graphRate, std::int64_t deliveryRate,
                           std::int64_t rounding) {
    const auto pages = frames / deliveryRate;
    if (pages > std::numeric_limits<std::int64_t>::max() / graphRate)
        throw std::invalid_argument("export rate conversion exceeds the supported timeline");
    const auto remainder = (frames % deliveryRate) * graphRate;
    return addLatency(pages * graphRate, (remainder + rounding) / deliveryRate);
}
std::int64_t positiveEnd(std::int64_t begin, std::int64_t length) {
    if (begin < 0 || length <= 0 || begin > std::numeric_limits<std::int64_t>::max()-length)
        throw std::invalid_argument("invalid export region bounds");
    return begin+length;
}
std::int64_t nsSample(std::int64_t ns, std::int64_t rate) {
    // Integer nearest-sample rounding, avoiding overflow even at INT64_MAX ns.
    constexpr std::int64_t billion=1000000000;
    return (ns/billion)*rate + ((ns%billion)*rate+billion/2)/billion;
}
std::int64_t tickSample(std::int64_t ticks, const engine::TempoMap& tempo, double rate) {
    const double sample=tempo.ticksToSeconds(ticks)*rate;
    if (!std::isfinite(sample) || sample < 0 || sample >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("export region exceeds the supported timeline");
    return static_cast<std::int64_t>(std::llround(sample));
}
void resolveRegion(Job& job, const rows::Model& model, const engine::TempoMap& tempo, NameContext& names) {
    const auto region=std::find_if(model.markers.begin(),model.markers.end(),
        [&](const auto& m) {return m.id==*job.regionId;});
    if (region==model.markers.end() || region->kind!="cycle")
        throw std::invalid_argument("export region must name a cycle marker");
    if (region->timeBase==0 && region->posTicks && region->lengthTicks) {
        const auto end=positiveEnd(*region->posTicks,*region->lengthTicks);
        job.beginSample=tickSample(*region->posTicks,tempo,job.session.sampleRate);
        job.endSample=tickSample(end,tempo,job.session.sampleRate);
    } else if (region->timeBase==1 && region->posNs && region->lengthNs) {
        const auto end=positiveEnd(*region->posNs,*region->lengthNs);
        const auto rate=static_cast<std::int64_t>(job.session.sampleRate);
        job.beginSample=nsSample(*region->posNs,rate);
        job.endSample=nsSample(end,rate);
    } else throw std::invalid_argument("export region has no length in its time base");
    if (job.endSample<=job.beginSample) throw std::invalid_argument("export region is shorter than one output sample");
    names.region=region->name;
}
void resolveSourceName(Job& job, const rows::Model& model, const engine::TempoMap& tempo, NameContext& names) {
    const auto& tracks = model.tracks;
    auto selected = tracks.end();
    for (auto it = tracks.begin(); it != tracks.end(); ++it)
        if (job.source.trackId ? it->id == job.source.trackId : it->kind == "master") { selected=it; break; }
    if (selected == tracks.end()) throw std::runtime_error("export source track is missing");
    job.source.trackId = selected->id;
    if (selected->kind != "audio" && selected->kind != "midi" && selected->kind != "instrument" &&
        selected->kind != "group" && selected->kind != "return" && selected->kind != "master")
        throw std::runtime_error("export source track has no audio output");
    names.track = selected->name;
    auto parent = selected;
    for (std::size_t depth = 0; parent != tracks.end() && depth < tracks.size(); ++depth) {
        if (parent->kind == "group") { names.group=parent->name; break; }
        if (!parent->parentId) break;
        const auto idOfParent = *parent->parentId;
        parent = std::find_if(tracks.begin(), tracks.end(), [idOfParent](const auto& t) {return t.id==idOfParent;});
    }
    if (job.regionId) resolveRegion(job, model, tempo, names);
    names.bpm = textproj::renderF64(tempo.bpmAt(tempo.secondsToTicks(
        static_cast<double>(job.beginSample) / job.session.sampleRate)));
}
using detail::Cancelled;
void cancellation(const Control& c) { if (c.stop.stop_requested()) throw Cancelled{}; }
// serviceHost runs on the host owner: through onHost when the caller is not it.
void serviceOwner(const Control& c) {
    if (!c.serviceHost) return;
    if (c.onHost) c.onHost(c.serviceHost);
    else c.serviceHost();
}
std::optional<std::string> comparableHash(std::string hash) {
    if(hash.size()!=64)return std::nullopt;
    for(auto& c:hash) {
        if(c>='A' && c<='F')c=static_cast<char>(c-'A'+'a');
        if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return std::nullopt;
    }
    return hash;
}
std::string hashMedia(const fs::path& path,const Control& control) {
    cancellation(control);
    auto hash=media::blake3File(path,control.stop);
    if(hash.error==media::HashError::cancelled)throw Cancelled{};
    if(!hash)throw std::runtime_error("cannot hash export media: "+media::pathUtf8(path));
    cancellation(control);
    return std::move(hash.hex);
}
media::FileIdentity mediaIdentityAt(const fs::path& path) {
    std::error_code error;
    const auto identity=media::readFileIdentity(path,error);
    if(!identity)throw fs::filesystem_error("cannot identify export media",path,error);
    return *identity;
}
void captureMedia(const engine::ClipPlayback& playback,MediaVerification& report,const Control& control) {
    // Each distinct selected spelling incurs one native identity read. Keyed
    // lookup joins hard links, symlinks and native case aliases without pairwise
    // filesystem queries or a host-independent case-folding rule.
    std::map<fs::path,std::size_t> selectedPaths;
    std::map<media::FileIdentity,std::size_t> nativeFiles;
    for(const auto& source:playback.externalSources()) {
        cancellation(control);
        auto selected=selectedPaths.find(source.path);
        if(selected==selectedPaths.end()) {
            const auto native=mediaIdentityAt(source.path);
            const auto [found,inserted]=nativeFiles.try_emplace(native,report.identities.size());
            if(inserted) {
                MediaIdentity identity;
                identity.path=fs::canonical(source.path);
                identity.nativeIdentity=native;
                identity.blake3AtStart=hashMedia(identity.path,control);
                report.identities.push_back(std::move(identity));
            }
            selected=selectedPaths.emplace(source.path,found->second).first;
        }
        auto& identity=report.identities[selected->second];
        const auto expected=comparableHash(source.storedBlake3);
        identity.references.push_back({source.clipId,source.mediaId,source.path,source.storedBlake3,
            expected ? std::optional<bool>(*expected==identity.blake3AtStart) : std::nullopt});
        if(expected && *expected!=identity.blake3AtStart)
            throw std::runtime_error("export media differs from stored BLAKE3: "+media::pathUtf8(identity.path));
    }
    report.startComplete=true;
}
void verifyMedia(MediaVerification& report, const Control& control, bool publication = true) {
    auto& verified = publication ? report.publicationVerified : report.analysisVerified;
    verified = false;
    std::map<fs::path,media::FileIdentity> selectedPaths;
    for(auto& identity:report.identities) {
        const auto digest = hashMedia(identity.path, control);
        if (publication) identity.blake3BeforePublication = digest;
        for(const auto& reference:identity.references) {
            cancellation(control);
            auto selected=selectedPaths.find(reference.selectedPath);
            if(selected==selectedPaths.end())
                selected=selectedPaths.emplace(reference.selectedPath,mediaIdentityAt(reference.selectedPath)).first;
            if(selected->second!=identity.nativeIdentity)
                throw std::runtime_error("export media path changed during render: "+media::pathUtf8(reference.selectedPath));
        }
        if (digest != identity.blake3AtStart)
            throw std::runtime_error("export media changed during render: "+media::pathUtf8(identity.path));
    }
    cancellation(control);
    verified = true;
}
} // namespace

void validateJob(const Job& j) { validate(j); }

const char* pcmSuffix(PcmContainer container) {
    switch (container) {
    case PcmContainer::Wav: return ".wav";
    case PcmContainer::Aiff: return ".aiff";
    }
    throw std::invalid_argument("Unsupported PCM export format.");
}

void validateDeliveryProfile(const Job& j) {
    if (j.collision != Collision::Fail && j.collision != Collision::Increment && j.collision != Collision::Replace)
        throw std::invalid_argument("unsupported export collision policy");
    if (!std::isfinite(j.session.sampleRate) || j.session.sampleRate < 8000 || j.session.sampleRate > 768000 ||
        std::floor(j.session.sampleRate) != j.session.sampleRate)
        throw std::invalid_argument("export sample rate must be an integer from 8000 through 768000");
    if (j.session.channels < 1 || j.session.channels > 64 || j.session.maxFrames < 1 || j.session.maxFrames > 4096)
        throw std::invalid_argument("export needs 1..64 channels and 1..4096 frames per block");
    if (j.session.precision != engine::Precision::Float32 && j.session.precision != engine::Precision::Float64)
        throw std::invalid_argument("unknown processing precision");
    if (j.ratePolicy != ExportRatePolicy::RequestedRate && j.ratePolicy != ExportRatePolicy::ProjectThenDownsample)
        throw std::invalid_argument("unknown export sample rate policy");
    // The save dialog's PCM container (WAV or AIFF) and its literal filename
    // describe one PCM delivery. The multi-format deliveries keep template names.
    const char* suffix = pcmSuffix(j.container);
    if (j.container != PcmContainer::Wav && j.niStems)
        throw std::invalid_argument("NI Stems require WAV capture.");
    if ((j.container != PcmContainer::Wav || j.exactPcmFilename) &&
        (j.analysisOnly || !j.deliveries.empty() || j.encoding.container != ExportContainer::Wav))
        throw std::invalid_argument("An AIFF or literal-filename export is one PCM delivery.");
    if (j.analysisOnly) {
        if (j.niStems || j.exactWavFilename || !j.deliveries.empty() || j.normalization.mode != NormalizeMode::Off) {
            throw std::invalid_argument("Analyze Only accepts one source and no encoding or normalization");
        }
        (void)deliveryChannels(j, j.encoding.channelFold);
        return;
    }
    const auto fold = j.deliveries.empty() ? j.encoding.channelFold : j.deliveries.front().encoding.channelFold;
    const auto channels = deliveryChannels(j, fold);
    const auto rate = static_cast<std::uint32_t>(j.session.sampleRate);
    if (j.deliveries.empty()) {
        validateEncoding(j, j.encoding, channels);
    } else {
        if (j.niStems || j.exactWavFilename || j.normalization.mode != NormalizeMode::Off) {
            throw std::invalid_argument("Multiple deliveries require a template source with Normalize Off");
        }
        std::set<std::string> names;
        std::uint64_t index = 1;
        for (const auto& delivery : j.deliveries) {
            if (delivery.encoding.channelFold != fold) {
                throw std::invalid_argument("Multiple deliveries must share the same channel layout");
            }
            validateEncoding(j, delivery.encoding, channels);
            // {format} names each delivery's own container.
            const auto base = resolveName(j.nameTemplate, {"project", "master", "group", "120", 1, "region",
                containerName(delivery.encoding.container), rate, index++});
            const auto name = deliveryName(base, delivery);
            if (!names.insert(foldedFilename(media::pathFromUtf8(name))).second) {
                throw std::invalid_argument("Multiple deliveries resolve to the same filename");
            }
        }
    }
    validateNiStems(j);
    validateNormalization(j.normalization, rate, channels);
    if (j.exactPcmFilename) {
        if (j.exactWavFilename || j.niStems || j.collision == Collision::Increment)
            throw std::invalid_argument("A literal filename requires one PCM delivery.");
        const auto& literal = *j.exactPcmFilename;
        if (portableName(literal) != literal || !foldedFilename(media::pathFromUtf8(literal)).ends_with(suffix))
            throw std::invalid_argument("The selected filename does not match the export format.");
    }
    if (j.exactWavFilename) {
        if (j.container != PcmContainer::Wav || j.niStems || j.collision == Collision::Increment)
            throw std::invalid_argument("a literal WAV filename requires a single WAV with Fail or Replace");
        const auto& literal = *j.exactWavFilename;
        if (portableName(literal) != literal ||
            !foldedFilename(media::pathFromUtf8(literal)).ends_with(".wav"))
            throw std::invalid_argument("the selected WAV filename is not a portable literal .wav name");
    } else if (!j.exactPcmFilename) {
        (void)resolveName(j.nameTemplate, {"project","master","group","120",1,"region",
            j.niStems ? "ni_stems_mp4" : formatName(j), rate, 1});
    }
}

std::string resolveName(const std::string& pattern, const NameContext& context) {
    std::string name;
    for (std::size_t at = 0; at < pattern.size();) {
        const char c = pattern[at++];
        if ((c == '{' || c == '}') && at < pattern.size() && pattern[at] == c) {
            name += c; ++at; continue;
        }
        if (c == '}') throw std::invalid_argument("unmatched closing brace in export name");
        if (c != '{') { name += c; continue; }
        const auto end = pattern.find('}', at);
        if (end == std::string::npos) throw std::invalid_argument("unclosed brace in export name");
        const auto token = pattern.substr(at, end - at);
        if (token == "project") name += context.project;
        else if (token == "track") name += context.track;
        else if (token == "group") name += context.group;
        else if (token == "bpm") name += context.bpm;
        else if (token == "region") name += context.region;
        else if (token == "counter") name += std::to_string(context.counter);
        else if (token == "format") name += context.format;
        else if (token == "rate") name += std::to_string(context.rate);
        else if (token == "index") name += std::to_string(context.index);
        else throw std::invalid_argument("unknown export name token: " + token);
        at = end + 1;
    }
    return portableName(std::move(name));
}

void validateOutputName(const fs::path& output) {
    if (media::pathUtf8(output.filename()).size() > 200)
        throw std::invalid_argument("final export name exceeds 200 UTF-8 bytes");
}

namespace {
void selectOutput(Result& result, Collision collision, const char* suffix,
                  const char* collisionMessage, const std::set<std::string>& reserved) {
    result.replacement.reset();
    if(collision!=Collision::Fail && collision!=Collision::Increment && collision!=Collision::Replace)
        throw std::invalid_argument("unsupported export collision policy");
    validateOutputName(result.output);
    const auto directory=result.output.parent_path();
    if(!fs::is_directory(directory))throw std::runtime_error("export directory does not exist");
    if(collision==Collision::Replace) {
        if (reserved.contains(foldedFilename(result.output)))
            throw std::runtime_error("two batch members cannot replace the same output");
        // Freeze the resolved parent so a later working-directory or parent
        // symlink change cannot silently redirect the selected delivery.
        result.output=fs::canonical(directory)/result.output.filename();
        ReplacementBinding binding;binding.target=result.output;
        binding.foldedNames=foldedOccupants(result.output);
        if(!occupiedNative(result.output)) {
            if(!binding.foldedNames.empty())
                throw std::runtime_error("Replace refuses a fold-only collision at another spelling");
        } else {
            binding.existing=ordinaryOutputIdentity(result.output);
            if(binding.foldedNames.size()>1)
                throw std::runtime_error("Replace refuses an ambiguous folded collision set");
            if(!binding.foldedNames.empty() &&
                ordinaryOutputIdentity(result.output.parent_path()/media::pathFromUtf8(binding.foldedNames.front()))!=*binding.existing)
                throw std::runtime_error("Replace refuses a folded collision with another file");
            const auto digest=media::blake3File(result.output);
            if(!digest)throw std::runtime_error("cannot hash replacement destination");
            binding.blake3=digest.hex;
        }
        result.replacement=std::move(binding);
        verifyReplacement(result);
        return;
    }
    auto names = reserved;
    auto entries = static_cast<std::uint64_t>(reserved.size());
    for(const auto& entry:fs::directory_iterator(directory)) {
        if(entries==std::numeric_limits<std::uint64_t>::max()-2)
            throw std::runtime_error("export directory has too many entries");
        ++entries;names.insert(foldedFilename(entry.path()));
    }
    // One directory inventory for all candidates. Native lookup additionally
    // catches aliases (including Windows short names) absent from that inventory.
    const auto occupied=[&](const fs::path& path) {
        return names.contains(foldedFilename(path)) || occupiedNative(path);
    };
    if(!occupied(result.output))return;
    if(collision==Collision::Fail)throw std::runtime_error(collisionMessage);
    const auto filename=media::pathUtf8(result.output.filename());
    const std::string_view extension(suffix);
    if(extension.empty() || !filename.ends_with(extension))throw std::invalid_argument("invalid export suffix");
    const auto stem=filename.substr(0,filename.size()-extension.size());
    // N entries bound N+1 numbered candidates, in addition to the original
    // (which might be a native short-name alias of a numbered long name).
    for(std::uint64_t offset=0;offset<=entries;++offset) {
        const auto ordinal=offset+2;
        result.output=directory/media::pathFromUtf8(stem+" ("+std::to_string(ordinal)+")"+std::string(extension));
        validateOutputName(result.output); // includes both increment and entire suffix
        if(!occupied(result.output))return;
    }
    throw std::runtime_error("export collision set changed during preflight; retry");
}

} // namespace

void detail::prepareOutput(Result& result, Collision collision, const char* suffix, const char* collisionMessage) {
    selectOutput(result, collision, suffix, collisionMessage, {});
}

void detail::discardStage(fs::path&& directory, std::optional<StageCleanupFailure>& failure) noexcept {
    std::error_code error;
    try {
        fs::remove_all(directory, error);
    } catch (const std::bad_alloc&) {
        error = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
    if (error) failure.emplace(StageCleanupFailure{std::move(directory), error});
}

void detail::retainIntermediateMedia(Result& delivery, Result& intermediate) {
    if (!intermediate.mediaVerification || !intermediate.mediaVerification->startComplete ||
        !intermediate.mediaVerification->publicationVerified)
        throw std::runtime_error("NI intermediate render has no complete media verification");
    auto& observed = *intermediate.mediaVerification;
    if (!delivery.mediaVerification) {
        auto& baseline = delivery.mediaVerification.emplace(std::move(observed));
        // An intermediate WAV's check is not evidence that the final container
        // is ready to publish. Keep only the start evidence at this point.
        baseline.publicationVerified = false;
        for (auto& identity : baseline.identities) identity.blake3BeforePublication.reset();
        return;
    }
    const auto& baseline = delivery.mediaVerification->identities;
    const auto sameInput = [](const MediaIdentity& original, const MediaIdentity& current) {
        return original.path == current.path && original.nativeIdentity == current.nativeIdentity &&
            original.blake3AtStart == current.blake3AtStart && original.references == current.references;
    };
    if (!std::equal(baseline.begin(), baseline.end(), observed.identities.begin(), observed.identities.end(), sameInput))
        throw std::runtime_error("NI Stems input media changed between renders");
}

namespace {
std::optional<std::string> fingerprintStagedDelivery(const fs::path& stage, const Control& control) {
    if (!control.fingerprintDelivery) return std::nullopt;
    cancellation(control);
    auto hash = media::blake3File(stage, control.stop);
    cancellation(control);
    if (!hash) throw std::runtime_error("Cannot fingerprint staged export for After Export");
    return std::move(hash.hex);
}
} // namespace

void detail::publishVerifiedOutput(const fs::path& stage, Result& result, const Control& control,
                                   const char* collisionMessage) {
    if (!result.mediaVerification || !result.mediaVerification->startComplete)
        throw std::runtime_error("export has no complete input media capture");
    verifyMedia(*result.mediaVerification, control);
    auto fingerprint = fingerprintStagedDelivery(stage, control);
    publishOutput(stage, result, control.publicationHooks, collisionMessage);
    result.deliveryBlake3 = std::move(fingerprint);
}

void detail::publishOutput(const fs::path& stage, Result& result, media::PublishHooks hooks,
                          const char* collisionMessage) {
    // The chosen spelling is frozen. This is a fresh folded check, not an
    // atomic cross-process folded reservation; native no-clobber still follows.
    const bool replacing=result.replacement && result.replacement->existing.has_value();
    if(result.replacement) {
        if(!replacing)verifyReplacement(result);
        else if(result.output!=result.replacement->target)
            throw std::runtime_error("replacement path changed since preflight");
    } else if(occupiedBeforePublication(result.output))throw std::runtime_error(collisionMessage);
    media::ReplaceHooks replaceHooks;
    if(replacing) {
        replaceHooks.rename=hooks.rename;replaceHooks.syncDirectory=hooks.syncDirectory;
        replaceHooks.expectedTarget=result.replacement->existing;
        replaceHooks.validateTarget=[&result](const fs::path&) {
            verifyReplacement(result);return std::error_code{};
        };
    }
    // Reserve the fallback message before the irreversible commit point.
    result.publication.emplace(PublicationReport{false, false,
        "file published; durability could not be confirmed"});
    auto published = replacing ? media::replaceFile(stage,result.output,std::move(replaceHooks)) :
        media::publishFile(stage, result.output, hooks, media::PublishPolicy::AtomicOnly);
    if (published.status != media::PublishStatus::published) {
        result.publication.reset();
        throw std::runtime_error(published.status == media::PublishStatus::exists
                                     ? collisionMessage : published.error);
    }
    result.state = JobState::Completed;
    auto& report = *result.publication;
    report.durabilityConfirmed = published.durabilityConfirmed;
    report.usedFallback = published.usedFallback;
    if (!published.warning.empty()) report.warning = std::move(published.warning);
    else if (report.durabilityConfirmed) report.warning.clear();
}

Result renderWav(Job job, engine::DeviceLoader loader, Control control,
                       JobId id, std::uint64_t counter) {
    return detail::renderWavPrepared(std::move(job),std::move(loader),std::move(control),id,counter,{});
}
Result detail::renderWavPrepared(Job job, engine::DeviceLoader loader, Control control,
                       JobId id, std::uint64_t counter,const std::function<void(const Result&)>& prepared,
                       const detail::PlanRange& planRange) {
    Result result;
    try {
        cancellation(control);
        validate(job, static_cast<bool>(planRange));
        if (job.niStems) throw std::invalid_argument("renderWav requires a WAV job; use renderNiStems for NI Stems");
        StoreError error = StoreError::Ok;
        auto store = Store::open(job.project, error, true);
        if (!store || error != StoreError::Ok) throw std::runtime_error(std::string("open export project: ") + toString(error));
        const auto onHost = [&](std::function<void()> action) {
            if (control.onHost) control.onHost(std::move(action));
            else action();
        };
        const auto destroy = [&](engine::Session* value) {
            onHost([value] { delete value; });
        };
        std::unique_ptr<engine::Session, decltype(destroy)> owned(nullptr, destroy);
        onHost([&] {
            owned.reset(new engine::Session);
            owned->graph().setFadeFrames(0);
        });
        auto& session = *owned;
        auto graphRate = static_cast<std::int64_t>(job.session.sampleRate);
        NameContext names;
        names.counter = counter;
        names.format = formatName(job);
        names.rate = static_cast<std::uint32_t>(job.session.sampleRate);
        names.index = counter;
        {
            // Metadata, device states and playback rows describe one database
            // revision, sampled when this job starts, never when it is queued.
            SQLite::Transaction snapshot(store->db());
            // Pin the read revision before loading devices (which may service
            // external writers). Do not use headSeq(): it also returns null on
            // read errors, indistinguishable there from the legitimate base.
            std::int64_t journalHead = 0;
            {
                SQLite::Statement head(store->db(), "SELECT head_seq FROM op_branches WHERE is_current=1");
                if (!head.executeStep()) throw std::runtime_error("export project has no current journal branch");
                if (!head.getColumn(0).isNull()) journalHead = head.getColumn(0).getInt64();
            }
            auto renderSpec=job.session;
            if (job.ratePolicy == ExportRatePolicy::ProjectThenDownsample) {
                const auto projectRate = store->db().execAndGet("SELECT sample_rate FROM project WHERE id=1").getInt64();
                if (projectRate < 8000 || projectRate > 768000)
                    throw std::runtime_error("project sample rate is outside the export graph's supported range");
                if (projectRate > graphRate && !deliverySrcAvailable())
                    throw std::runtime_error("Project-rate downsampling requires the pinned SoXR build");
                graphRate = (std::max)(graphRate, projectRate);
                renderSpec.sampleRate = static_cast<double>(graphRate);
            }
            renderSpec.performanceState=false;
            renderSpec.exporting=true;
            renderSpec.synchronousClipReads=true;
            onHost([&] {
                if (!session.load(*store, std::move(loader), renderSpec))
                    throw std::runtime_error(session.error());
            });
            const auto blockingProblems = session.exportBlockingProblems();
            if (session.stats().skipped || !blockingProblems.empty()) {
                std::string why = "Some parts of this song cannot be exported";
                for (const auto& problem : blockingProblems) why += "\n" + problem;
                throw std::runtime_error(why);
            }
            names.project = store->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
            const auto snap = engine::SnapshotBuilder::fromStore(*store);
            resolveSourceName(job, session.model(), *snap->tempo, names);
            if (planRange) {
                planRange(job, session.model(), *snap->tempo, names, journalHead);
                names.bpm = textproj::renderF64(snap->tempo->bpmAt(snap->tempo->secondsToTicks(
                    static_cast<double>(job.beginSample) / job.session.sampleRate)));
            }
            result.snapshot = RenderSnapshot{journalHead, job.source, job.beginSample, job.endSample,
                job.session.sampleRate, job.regionId, names, static_cast<double>(graphRate)};
            snapshot.commit();
        }
        std::vector<Result> preparedFiles;
        if (!job.analysisOnly) {
            if (job.deliveries.empty()) {
                // An AIFF from the save dialog is staged as WAV and converted at the end.
                const auto extension = job.container == PcmContainer::Aiff ? std::string(pcmSuffix(job.container)) :
                    containerSuffix(job.encoding.container);
                const auto& exactFilename = job.exactPcmFilename ? job.exactPcmFilename : job.exactWavFilename;
                const auto filename = exactFilename ? *exactFilename : resolveName(job.nameTemplate, names) + extension;
                result.output = job.directory / media::pathFromUtf8(filename);
                detail::prepareOutput(result, job.collision, extension.c_str(), "export destination already exists");
            } else {
                std::set<std::string> reserved;
                for (const auto& delivery : job.deliveries) {
                    const auto extension = containerSuffix(delivery.encoding.container);
                    auto deliveryNames = names;
                    deliveryNames.format = containerName(delivery.encoding.container);
                    deliveryNames.index = static_cast<std::uint64_t>(preparedFiles.size()) + 1;
                    const auto base = resolveName(job.nameTemplate, deliveryNames);
                    Result file;
                    file.output = job.directory / media::pathFromUtf8(deliveryName(base, delivery));
                    if (job.collision != Collision::Increment) {
                        detail::prepareOutput(file, job.collision, extension.c_str(), "export destination already exists");
                    }
                    if (!reserved.insert(foldedFilename(file.output)).second) {
                        throw std::runtime_error("Multiple deliveries resolve to the same destination");
                    }
                    preparedFiles.push_back(std::move(file));
                }
                if (job.collision == Collision::Increment) prepareGroupIncrement(preparedFiles, job.directory);
                for (const auto& file : preparedFiles) {
                    result.deliveries.push_back({file.output, JobState::Pending, {}, {}});
                }
            }
        }
        if (prepared) prepared(result);
        const auto* graph = session.graph().currentGraph();
        if (!graph) throw std::runtime_error("export has no master graph");
        const auto endpoint = session.offlineTapOutput(job.source.trackId, job.source.resolvedTap());
        if (!endpoint) throw std::runtime_error("export source has no audio output at the selected tap");
        result.mediaVerification.emplace();
        captureMedia(*session.clips(),*result.mediaVerification,control);
        const auto latency = endpoint->latency;
        auto& coalescer = session.devices().coalescer();
        coalescer.setQuietPeriodMs(0); // offline jobs must not outrun the live debounce
        const auto reports = coalescer.stats().reports;
        const auto outputRate = static_cast<std::int64_t>(job.session.sampleRate);
        const bool downsampling = graphRate != outputRate;
        const auto outputFrames = job.endSample - job.beginSample;
        const auto sourceBegin = projectFrames(job.beginSample, graphRate, outputRate, outputRate / 2);
        const auto sourceFrames = projectFrames(outputFrames, graphRate, outputRate, outputRate - 1);
        const auto sourceEnd = addLatency(sourceBegin, sourceFrames);
        const auto writtenBegin = job.renderAsLoop ? sourceEnd : sourceBegin;
        const auto rawBegin = addLatency(writtenBegin, latency);
        const auto rawEnd = addLatency(addLatency(writtenBegin, sourceFrames), latency);
        const bool hasTail = job.tail.mode != TailMode::Off;
        const auto tailFrames = hasTail ?
            nsSample(static_cast<std::int64_t>(job.tail.maxMilliseconds) * 1000000, outputRate) : 0;
        const auto outputWithTail = addLatency(outputFrames, tailFrames);
        const auto sourceWithTail = projectFrames(outputWithTail, graphRate, outputRate, outputRate - 1);
        const auto renderEnd = addLatency(addLatency(writtenBegin, sourceWithTail), latency);
        if (hasTail && rawEnd < 0)
            throw std::invalid_argument("tail requires the compensated range end to reach the render timeline");
        const auto total = static_cast<std::uint64_t>(outputWithTail);
        const bool untilSilent = job.tail.mode == TailMode::UntilSilent;
        const auto holdFrames = untilSilent ?
            nsSample(static_cast<std::int64_t>(job.tail.holdMilliseconds) * 1000000, graphRate) : 0;
        const auto threshold = untilSilent ? std::pow(10.0, job.tail.thresholdDbfs / 20.0) : 0.0;
        const auto channels = static_cast<std::size_t>(job.session.channels);
        const auto fold = job.deliveries.empty() ? job.encoding.channelFold : job.deliveries.front().encoding.channelFold;
        const auto outputChannels = deliveryChannels(job, fold);
        const bool foldStereo = static_cast<std::size_t>(outputChannels) < channels;
        if (job.container == PcmContainer::Aiff)
            audio::validateAiffSize(total, static_cast<std::uint16_t>(outputChannels), job.encoding.format);
        const auto block = static_cast<std::size_t>(job.session.maxFrames);
        std::vector<float> narrow(channels * block);
        std::vector<double> interleaved(channels * block);
        std::vector<double> wide(job.session.precision == engine::Precision::Float64 ? channels * block : 0);
        std::vector<float*> fp(channels);
        std::vector<double*> dp(wide.empty() ? 0 : channels);
        for (std::size_t c = 0; c < channels; ++c) {
            fp[c] = narrow.data() + c * block;
            if (!wide.empty()) dp[c] = wide.data() + c * block;
        }
        std::optional<Stage> stage;
        std::vector<DeliverySink> sinks;
        std::unique_ptr<ExportAnalysis> analysis;
        if (job.analysisOnly) {
            analysis = std::make_unique<ExportAnalysis>(static_cast<std::uint32_t>(outputRate), outputChannels,
                                                        job.normalization.channelWeights);
        } else {
            stage.emplace(job.directory, result);
            if (job.deliveries.empty()) {
                sinks.emplace_back(stage->path, "render", job, job.encoding, outputChannels, total, counter);
            } else {
                sinks.reserve(job.deliveries.size());
                for (std::size_t index = 0; index < job.deliveries.size(); ++index) {
                    cancellation(control);
                    sinks.emplace_back(stage->path, "delivery" + std::to_string(index), job,
                                       job.deliveries[index].encoding, outputChannels, total, counter);
                }
            }
        }
        auto& completedFrames = job.analysisOnly ? result.framesAnalyzed : result.framesWritten;
        const bool checkFloatRange = std::ranges::any_of(sinks, &DeliverySink::requiresFloat32Range);
        std::vector<double> mono;
        const auto writeDelivery = [&](std::span<const double> samples, std::uint32_t frames) {
            if (foldStereo) {
                mono.resize(frames);
                for (std::size_t i = 0; i < frames; ++i) {
                    mono[i] = samples[i * 2] * 0.5 + samples[i * 2 + 1] * 0.5;
                }
                samples = mono;
            }
            if ((downsampling || foldStereo) && std::ranges::any_of(samples, [&](double value) {
                return !std::isfinite(value) || (checkFloatRange &&
                    std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()));
            })) {
                throw std::runtime_error("Converted audio exceeds the output format");
            }
            if (analysis) analysis->append(samples, frames);
            else for (auto& sink : sinks) sink.append(samples, frames);
            completedFrames += frames;
        };
        std::unique_ptr<DeliverySrc> converter;
        if (downsampling) converter = std::make_unique<DeliverySrc>(static_cast<unsigned>(graphRate),
            static_cast<unsigned>(outputRate), static_cast<unsigned>(channels), total, writeDelivery);
        std::int64_t graphFramesWritten = 0;
        const auto write = [&](std::uint32_t frames) {
            graphFramesWritten += frames;
            const std::span<const double> samples(interleaved.data(), static_cast<std::size_t>(frames) * channels);
            if (converter) converter->append(samples, frames);
            else writeDelivery(samples, frames);
        };
        const auto leadingSilence = rawBegin < 0 ?
            (std::min)(static_cast<std::uint64_t>(sourceWithTail), static_cast<std::uint64_t>(-rawBegin)) : 0;
        const auto passes = job.analysisOnly || job.normalization.mode == NormalizeMode::Off ? 0u :
                            (job.normalization.mode == NormalizeMode::Loudness ? maxLoudnessEncodingPasses : 1u);
        const double totalWork = static_cast<double>(leadingSilence) +
                                 static_cast<double>((std::max)(std::int64_t{0}, renderEnd)) +
                                 2.0 * static_cast<double>(total) * passes; // encode and measure each pass
        double workDone = 0;
        auto advanceWork = [&](std::uint32_t frames) {
            if (!control.wavProgress) return;
            workDone += frames;
            // Keep the last percentage for verification and publication.
            control.wavProgress(id, (std::min)(workDone / totalWork, 0.99));
            cancellation(control);
        };
        auto progress = [&](std::uint32_t frames) {
            if (control.progress) control.progress(id, completedFrames, total);
            cancellation(control);
            advanceWork(frames);
        };
        progress(0);
        if (!session.clips()->waitForWarp(control.stop)) {
            cancellation(control);
            std::string why = "export Warp preparation failed";
            for (const auto& problem : session.clips()->decodeProblems()) why += "; " + problem;
            throw std::runtime_error(why);
        }
        // Signed output latency may map a requested position before transport
        // zero: that is silence, not a negative locate (which clamps).
        for (auto silence = leadingSilence; silence > 0;) {
            const auto frames = static_cast<std::uint32_t>((std::min)(silence, static_cast<std::uint64_t>(block)));
            write(frames);
            silence -= frames;
            progress(frames);
        }
        session.transport().loop(sourceBegin, sourceEnd, job.renderAsLoop);
        session.transport().locate(0);
        session.transport().play();
        const auto started = std::chrono::steady_clock::now();
        auto service = [&] {
            cancellation(control);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            onHost([&] {
                if (control.serviceHost) control.serviceHost();
                cancellation(control);
                session.tick(elapsed);
            });
            // A shorter branch can change compensation without changing the
            // master's total latency. Any latency/shape report invalidates the run.
            if (coalescer.stats().reports != reports || session.graph().currentGraph() != graph ||
                graph->outputLatencyOf(endpoint->node) != latency)
                throw std::runtime_error("device latency or topology changed during export; retry after it settles");
        };
        // Ordinary preroll remains. Loop export discards the first lap and
        // captures the second at the tap's compensated sample positions.
        std::int64_t quietFrames = 0;
        for (std::int64_t at = 0; at < renderEnd;) {
            if (hasTail && at >= sourceEnd && session.transport().playing())
                session.transport().play(false); // stop timeline input; keep the same running graph
            service();
            if (!session.clips()->prime(std::chrono::seconds(10), control.stop)) {
                cancellation(control);
                throw std::runtime_error("export clip read failed or timed out");
            }
            auto count = (std::min)(static_cast<std::int64_t>(block), renderEnd - at);
            if (hasTail && at < sourceEnd) count = (std::min)(count, sourceEnd - at);
            if (job.renderAsLoop) count = session.transport().contiguousFrames(0, count);
            const auto frames = static_cast<std::int32_t>(count);
            engine::AudioIo io;
            io.out = dp.empty() ? fp.data() : nullptr;
            io.out64 = dp.empty() ? nullptr : dp.data();
            io.numOut = job.session.channels; io.frames = frames;
            engine::AudioIo processIo;
            processIo.frames = frames;
            session.process(processIo); // the whole graph, including unexported sidechains and returns
            if (!graph->copyNodeOutput(endpoint->node, io)) throw std::runtime_error("export source has no complete rendered block");
            if (session.clips()->readErrors() || session.clips()->underrunSamples()) throw std::runtime_error("export clip read failed or underrun");
            const auto first = (std::max)(at, rawBegin);
            auto last = at + frames;
            bool tailFinished = false;
            // Validate preroll too: a nonfinite value may poison later state.
            for (std::int32_t f = 0; f < frames; ++f) {
                bool quiet = true;
                for (std::size_t c = 0; c < channels; ++c) {
                    const auto i = static_cast<std::size_t>(f);
                    const double value = dp.empty() ? static_cast<double>(fp[c][i]) : dp[c][i];
                    if (!std::isfinite(value) || (!converter && !foldStereo && checkFloatRange &&
                        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())))
                        throw std::runtime_error("nonfinite or out-of-range audio during export");
                    if (at + f >= first)
                        interleaved[static_cast<std::size_t>(at + f - first) * channels + c] = value;
                    if (untilSilent && std::abs(value) > threshold) quiet = false;
                }
                if (untilSilent && at + f >= rawEnd) {
                    quietFrames = quiet ? quietFrames + 1 : 0;
                    if (quietFrames >= holdFrames) {
                        last = at + f + 1;
                        tailFinished = true;
                        break;
                    }
                }
            }
            if (first < last) write(static_cast<std::uint32_t>(last - first));
            progress(static_cast<std::uint32_t>(last - at));
            at = last;
            if (tailFinished) break;
        }
        service(); // include reports raised by the final process call
        const auto delivered = untilSilent ? static_cast<std::uint64_t>(std::clamp(
            projectFrames(graphFramesWritten, outputRate, graphRate, graphRate / 2), outputFrames, outputWithTail)) : total;
        if (converter) {
            converter->finish(service, delivered);
            progress(0);
        }
        if (completedFrames != delivered) throw std::runtime_error("export produced an incomplete range");
        if (analysis) {
            verifyMedia(*result.mediaVerification, control, false);
            result.analysis = analysis->finish();
            result.state = JobState::Completed;
            return result;
        }
        const auto serviceEncoding = [&] {
            cancellation(control);
            if (control.serviceHost) onHost(control.serviceHost);
            cancellation(control);
        };
        if (preparedFiles.empty()) {
            result.normalization = sinks.front().finish(serviceEncoding, delivered, advanceWork);
            cancellation(control);
            auto delivery = sinks.front().output();
            if (job.container == PcmContainer::Aiff) {
                delivery = stage->path / "render.aiff";
                audio::writeAiffFromWav(sinks.front().output(), delivery, [&] { cancellation(control); });
            }
            detail::publishVerifiedOutput(delivery, result, control, "export destination already exists");
        } else {
            // Complete every encoder before the first destination is visible.
            // All files have the same captured source and input verification.
            for (auto& sink : sinks) {
                cancellation(control);
                (void)sink.finish(serviceEncoding, delivered);
            }
            verifyMedia(*result.mediaVerification, control);
            if (control.fingerprintDelivery) {
                for (std::size_t i = 0; i < sinks.size(); ++i) {
                    preparedFiles[i].deliveryBlake3 = fingerprintStagedDelivery(sinks[i].output(), control);
                }
            }
            for (std::size_t index = 0; index < sinks.size(); ++index) {
                cancellation(control);
                auto& file = preparedFiles[index];
                auto& report = result.deliveries[index];
                try {
                    detail::publishOutput(sinks[index].output(), file, control.publicationHooks,
                                          "export destination already exists");
                    report.state = file.state;
                    report.publication = std::move(file.publication);
                    report.deliveryBlake3 = std::move(file.deliveryBlake3);
                } catch (const std::exception& error) {
                    report.state = JobState::Failed;
                    report.error = error.what();
                    throw;
                }
            }
            result.state = JobState::Completed;
        }
        if (control.wavProgress) {
            // A display update cannot change the state of the published file.
            try { control.wavProgress(id, 1.0); } catch (...) {}
        }
    } catch (const Cancelled&) {
        result.state = JobState::Cancelled;
    } catch (const std::exception& e) {
        result.state = JobState::Failed; result.error = e.what();
    } catch (...) {
        result.state = JobState::Failed; result.error = "unknown exception during export";
    }
    return result;
}

namespace {
Result convertAudioFilePrepared(FileConversion job, const Control& control, JobId id,
                                const std::optional<media::FileIdentity>& expectedSource = {},
                                const std::function<void(const Result&)>& prepared = {}) {
    Result result;
    try {
        cancellation(control);
        if (job.source.empty() || job.directory.empty())
            throw std::invalid_argument("conversion needs a source and output directory");
        const auto source = fs::absolute(job.source);
        const auto sourceId = mediaIdentityAt(source);
        if (expectedSource && *expectedSource != sourceId) {
            throw std::runtime_error("Conversion source was replaced before its batch step");
        }
        const auto stem = portableName(job.name.empty() ? media::pathUtf8(source.stem()) : std::move(job.name));
        const auto extension = containerSuffix(job.encoding.container);
        result.output = job.directory / media::pathFromUtf8(stem + extension);
        detail::prepareOutput(result, job.collision, extension.c_str(), "conversion destination already exists");
        if (result.replacement && result.replacement->existing == sourceId)
            throw std::invalid_argument("conversion cannot replace its input");
        if (prepared) prepared(result);
        const auto original = hashMedia(source, control);
        auto input = audio::openPcmStream(source, control.stop);
        cancellation(control);
        if (!input.ready()) throw std::runtime_error("Cannot decode conversion source: " + input.error);
        const auto info = input.stream->info();
        const auto outputRate = job.outputSampleRate == 0 ? info.sampleRate : job.outputSampleRate;
        if (job.outputSampleRate != 0 &&
            (outputRate < 8000 || outputRate > 768000 || outputRate > info.sampleRate)) {
            throw std::invalid_argument("Converter rate must be supported and at or below the source rate");
        }
        const bool downsampling = outputRate < info.sampleRate;
        const auto encoding = job.encoding.container;
        const bool packetDelivery = encoding == ExportContainer::Mp3 || encoding == ExportContainer::OggVorbis ||
                                    encoding == ExportContainer::OggOpus;
        if ((packetDelivery || downsampling) && !info.frames) {
            throw std::invalid_argument("This conversion needs a source with a declared length");
        }
        if (downsampling && !deliverySrcAvailable()) {
            throw std::runtime_error("Converter downsampling requires the pinned SoXR build");
        }
        auto expected = info.frames.value_or(0);
        if (downsampling) {
            if (expected > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw std::invalid_argument("Conversion source exceeds the supported timeline");
            }
            expected = static_cast<std::uint64_t>(projectFrames(static_cast<std::int64_t>(expected),
                                                outputRate, info.sampleRate, info.sampleRate / 2));
            if (expected == 0) throw std::invalid_argument("Conversion would have an empty output range");
        }
        Job profile;
        profile.session.sampleRate = static_cast<double>(outputRate);
        profile.session.channels = static_cast<int>(info.channels);
        profile.normalization = job.normalization;
        const auto channels = deliveryChannels(profile, job.encoding.channelFold);
        validateEncoding(profile, job.encoding, channels);
        validateNormalization(profile.normalization, outputRate, channels);

        Stage stage(job.directory, result);
        DeliverySink delivery(stage.path, "conversion", profile, job.encoding, channels, expected, 1);
        constexpr std::size_t block = 4096;
        std::vector<float> pcm(block * info.channels);
        std::vector<double> wide(pcm.size()), mono;
        const auto writeDelivery = [&](std::span<const double> samples, std::uint32_t frames) {
            if (static_cast<unsigned>(channels) < info.channels) {
                mono.resize(frames);
                for (std::size_t i = 0; i < frames; ++i) {
                    mono[i] = samples[i * 2] * 0.5 + samples[i * 2 + 1] * 0.5;
                }
                samples = mono;
            }
            if (std::ranges::any_of(samples, [&](double value) {
                return !std::isfinite(value) || (delivery.requiresFloat32Range() &&
                    std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()));
            })) throw std::runtime_error("Converted file audio exceeds the output format");
            delivery.append(samples, frames);
            result.framesWritten += frames;
        };
        std::unique_ptr<DeliverySrc> converter;
        if (downsampling) converter = std::make_unique<DeliverySrc>(
            info.sampleRate, outputRate, info.channels, expected, writeDelivery);
        const auto checkpoint = [&] {
            cancellation(control);
            serviceOwner(control);
            cancellation(control);
        };
        for (;;) {
            checkpoint();
            const auto read = input.stream->read(pcm, control.stop);
            cancellation(control);
            if (read.status == audio::PcmReadStatus::End) break;
            if (read.status != audio::PcmReadStatus::Frames || read.frames == 0)
                throw std::runtime_error("Cannot read conversion source: " + input.stream->error());
            const auto samples = static_cast<std::size_t>(read.frames) * info.channels;
            for (std::size_t i = 0; i < samples; ++i) {
                if (!std::isfinite(pcm[i])) throw std::runtime_error("Conversion source contains nonfinite audio");
                wide[i] = pcm[i];
            }
            const std::span<const double> blockInput(wide.data(), samples);
            if (converter) converter->append(blockInput, read.frames);
            else writeDelivery(blockInput, read.frames);
            if (control.progress) control.progress(id, result.framesWritten, expected);
        }
        input.stream.reset();
        if (converter) {
            converter->finish(checkpoint);
            if (control.progress) control.progress(id, result.framesWritten, expected);
            checkpoint();
        }
        if (result.framesWritten == 0) throw std::runtime_error("Conversion source is empty");
        result.normalization = delivery.finish(checkpoint);
        checkpoint();
        if (mediaIdentityAt(source) != sourceId || hashMedia(source, control) != original || mediaIdentityAt(source) != sourceId)
            throw std::runtime_error("Conversion source changed before publication");
        auto fingerprint = fingerprintStagedDelivery(delivery.output(), control);
        detail::publishOutput(delivery.output(), result, control.publicationHooks, "conversion destination already exists");
        result.deliveryBlake3 = std::move(fingerprint);
    } catch (const Cancelled&) {
        result.state = JobState::Cancelled;
    } catch (const std::exception& error) {
        result.state = JobState::Failed;
        result.error = error.what();
    }
    return result;
}

bool sameConversionPath(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::equivalent(left, right, error)) return true;
    return foldedFilename(left) == foldedFilename(right) &&
        fs::equivalent(fs::absolute(left).parent_path(), fs::absolute(right).parent_path(), error);
}
} // namespace

Result convertAudioFile(FileConversion job, Control control) {
    return convertAudioFilePrepared(std::move(job), control, 0);
}

std::vector<Result> convertAudioFiles(std::span<const FileConversion> jobs, Control control) {
    std::vector<Result> results(jobs.size());
    std::vector<std::optional<media::FileIdentity>> sources(jobs.size());
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        if (control.stop.stop_requested()) return results;
        try {
            sources[i] = mediaIdentityAt(jobs[i].source);
        } catch (const std::exception& error) {
            results[i].state = JobState::Failed;
            results[i].error = error.what();
        }
    }
    const auto guard = [&](const Result& candidate) {
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (sources[i] && (sameConversionPath(candidate.output, jobs[i].source) ||
                (candidate.replacement && candidate.replacement->existing == sources[i]))) {
                throw std::runtime_error("Conversion output would overwrite a batch input");
            }
        }
        for (const auto& prior : results) {
            if (prior.state == JobState::Completed && sameConversionPath(candidate.output, prior.output)) {
                throw std::runtime_error("Conversion output would overwrite an earlier delivery in this batch");
            }
        }
    };
    for (std::size_t i = 0; i < jobs.size() && !control.stop.stop_requested(); ++i) {
        if (sources[i]) results[i] = convertAudioFilePrepared(
            jobs[i], control, static_cast<JobId>(i) + 1, sources[i], guard);
    }
    return results;
}

namespace {
void planCycleRegions(const Job& base, Job& capture, std::span<const std::int64_t> ids,
                      const rows::Model& rows, const engine::TempoMap& tempo, NameContext names,
                      std::int64_t head, std::vector<Result>& outputs) {
    std::vector<RenderSnapshot> plan;
    plan.reserve(ids.size());
    for (const auto id : ids) {
        auto region = capture;
        region.regionId = id;
        resolveRegion(region, rows, tempo, names);
        names.bpm = textproj::renderF64(tempo.bpmAt(tempo.secondsToTicks(
            static_cast<double>(region.beginSample) / region.session.sampleRate)));
        plan.push_back({head, capture.source, region.beginSample, region.endSample, region.session.sampleRate, id, names});
    }
    std::sort(plan.begin(), plan.end(), [](const auto& a, const auto& b) { return a.beginSample < b.beginSample; });
    std::set<std::string> filenames;
    std::vector<Result> prepared;
    prepared.reserve(plan.size());
    std::int64_t next = plan.front().beginSample;
    std::uint64_t ordinal = 1;
    for (auto& snapshot : plan) {
        if (snapshot.beginSample != next)
            throw std::invalid_argument("gapless cycle marker ranges must be exactly adjacent");
        next = snapshot.endSample;
        snapshot.names.counter = ordinal++;
        Result result;
        result.output = base.directory / media::pathFromUtf8(resolveName(base.nameTemplate, snapshot.names) + ".wav");
        detail::prepareOutput(result, base.collision, ".wav", "gapless export destination already exists");
        if (!filenames.insert(foldedFilename(result.output)).second)
            throw std::invalid_argument("gapless cycle marker filenames must be distinct");
        result.snapshot = std::move(snapshot);
        prepared.push_back(std::move(result));
    }
    outputs = std::move(prepared);
    capture.beginSample = outputs.front().snapshot->beginSample;
    capture.endSample = next;
}

std::vector<fs::path> stageGaplessSlices(const fs::path& directory, const Result& continuous,
                                        std::vector<Result>& regions, std::uint32_t speakerMask,
                                        const Control& control) {
    audio::WavReader input(continuous.output);
    constexpr std::uint32_t block = 4096;
    std::vector<float> pcm(static_cast<std::size_t>(input.channels()) * block);
    std::vector<fs::path> stages;
    stages.reserve(regions.size());
    const auto service = [&] {
        cancellation(control);
        serviceOwner(control);
        cancellation(control);
    };
    std::uint64_t sliced = 0;
    for (auto& region : regions) {
        const auto& snapshot = *region.snapshot;
        const auto frames = static_cast<std::uint64_t>(snapshot.endSample - snapshot.beginSample);
        auto target = directory / ("slice-" + std::to_string(stages.size()) + ".wav");
        // The continuous capture has already applied its one dither stream.
        // PCM16 and PCM24 decode exactly into Float32. An undithered wrapper
        // round-trips those integer codes, and Float32 preserves its bits.
        audio::WavWriter writer(target, input.sampleRate(), input.channels(), input.format(), nullptr,
                                std::uint64_t{1} << 32, audio::WavDither::None, 0, speakerMask);
        for (std::uint64_t copied = 0; copied < frames;) {
            service();
            const auto count = static_cast<std::uint32_t>((std::min)(static_cast<std::uint64_t>(block), frames - copied));
            if (input.read(pcm.data(), count) != count) throw std::runtime_error("Incomplete gapless PCM capture");
            writer.write(pcm.data(), count);
            copied += count;
        }
        writer.close();
        stages.push_back(std::move(target));
        region.framesWritten = frames;
        region.snapshot->graphSampleRate = continuous.snapshot->graphSampleRate;
        sliced += frames;
    }
    if (sliced != input.frames() || sliced != continuous.framesWritten)
        throw std::runtime_error("Gapless slices did not consume the continuous capture");
    for (std::size_t i = 0; i < stages.size(); ++i) {
        verifyPcmDelivery(stages[i], ExportContainer::Wav, input.sampleRate(), input.channels(),
                          regions[i].framesWritten, service);
    }
    return stages;
}
} // namespace

GaplessResult renderGaplessWav(Job job, std::span<const std::int64_t> regionIds,
                               engine::DeviceLoader loader, Control control) {
    GaplessResult batch;
    Result stageOwner;
    try {
        cancellation(control);
        if (regionIds.empty() || job.regionId || job.exactWavFilename || job.niStems || job.analysisOnly ||
            !job.deliveries.empty() || job.encoding.container != ExportContainer::Wav ||
            job.container != PcmContainer::Wav || job.exactPcmFilename)
            throw std::invalid_argument("gapless export needs named cycle markers and one WAV profile");
        const std::vector<std::int64_t> ids(regionIds.begin(), regionIds.end());
        // The captured revision owns the actual limits. The temporary range
        // only admits the otherwise complete job into the shared runner.
        job.beginSample = 0;
        job.endSample = 1;
        job.gaplessBatch = true;
        validate(job, true);
        Stage stage(job.directory, stageOwner);
        auto capture = job;
        capture.directory = stage.path;
        capture.nameTemplate = "continuous";
        capture.collision = Collision::Fail;
        auto renderControl = control;
        renderControl.publicationHooks = {};
        renderControl.fingerprintDelivery = false;
        auto continuous = detail::renderWavPrepared(std::move(capture), std::move(loader), renderControl, 0, 1, {},
            [&](Job& captured, const rows::Model& rows, const engine::TempoMap& tempo,
                const NameContext& names, std::int64_t head) {
                planCycleRegions(job, captured, ids, rows, tempo, names, head, batch.regions);
            });
        if (continuous.state == JobState::Cancelled) throw Cancelled{};
        if (continuous.state != JobState::Completed || !continuous.mediaVerification || !continuous.snapshot)
            throw std::runtime_error(continuous.error.empty() ? "Cannot capture continuous PCM" : continuous.error);
        const auto stages = stageGaplessSlices(stage.path, continuous, batch.regions, job.encoding.speakerMask, control);
        std::vector<std::optional<std::string>> digests;
        if (control.fingerprintDelivery) {
            digests.reserve(stages.size());
            for (const auto& slice : stages) digests.push_back(fingerprintStagedDelivery(slice, control));
        }
        verifyMedia(*continuous.mediaVerification, control);
        batch.normalization = continuous.normalization;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            cancellation(control);
            auto& member = batch.regions[i];
            member.mediaVerification = continuous.mediaVerification;
            try {
                detail::publishOutput(stages[i], member, control.publicationHooks, "gapless destination already exists");
                if (!digests.empty()) member.deliveryBlake3 = std::move(digests[i]);
            } catch (const std::exception& error) {
                member.state = JobState::Failed;
                member.error = error.what();
                throw;
            }
        }
        batch.state = JobState::Completed;
    } catch (const Cancelled&) {
        batch.state = JobState::Cancelled;
    } catch (const std::exception& error) {
        batch.state = JobState::Failed;
        batch.error = error.what();
    }
    batch.stageCleanup = std::move(stageOwner.stageCleanup);
    return batch;
}

Result renderMasterWav(Job job, engine::DeviceLoader loader, Control control,
                       JobId id, std::uint64_t counter) {
    if (job.source.trackId != 0 || job.source.resolvedTap() != engine::Tap::PostFader) {
        Result result;
        result.state = JobState::Failed;
        result.error = "renderMasterWav requires the default master source; use renderWav for stems";
        return result;
    }
    return renderWav(std::move(job), std::move(loader), std::move(control), id, counter);
}

JobId Queue::add(Job job) {
    validateJob(job);
    if (next_ == std::numeric_limits<JobId>::max()) throw std::overflow_error("export job IDs exhausted");
    const auto id = next_++;
    jobs_.push_back({id, std::move(job), {}});
    return id;
}
namespace {
std::vector<RenderCell> stemCells(std::span<const Source> sources, std::optional<std::int64_t> regionId) {
    std::vector<RenderCell> cells;
    cells.reserve(sources.size());
    for (const auto& source : sources) cells.push_back({source, regionId});
    return cells;
}
} // namespace

std::vector<StemPreview> Queue::previewStemBatch(const Job& base, std::span<const Source> sources) const {
    return previewRenderMatrix(base, stemCells(sources, base.regionId));
}

std::vector<JobId> Queue::addStemBatch(const Job& base, std::span<const Source> sources) {
    return addRenderMatrix(base, stemCells(sources, base.regionId));
}

std::vector<StemPreview> Queue::previewRenderMatrix(const Job& base, std::span<const RenderCell> cells) const {
    if (cells.empty() || base.niStems || base.exactWavFilename)
        throw std::invalid_argument("export batch requires explicit cells and template filenames");
    if (cells.size() > std::numeric_limits<JobId>::max() - next_)
        throw std::overflow_error("export job IDs exhausted");
    auto job = base;
    job.source = cells.front().source;
    job.regionId = cells.front().regionId;
    validate(job);
    StoreError error{};
    auto store = Store::open(base.project, error, true);
    if (!store || error != StoreError::Ok)
        throw std::runtime_error(std::string("open export project: ") + toString(error));
    const SQLite::Transaction read(store->db(), SQLite::TransactionBehavior::DEFERRED);
    const auto project = store->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
    const auto model = rows::readModel(*store);
    const auto snapshot = engine::SnapshotBuilder::fromStore(*store, std::set<std::int64_t>{}, false);

    std::vector<StemPreview> preview;
    preview.reserve(cells.size());
    std::set<std::tuple<std::int64_t, engine::Tap, std::optional<std::int64_t>>> members;
    std::set<std::string> reserved;
    for (const auto& cell : cells) {
        job = base;
        job.source = cell.source;
        job.regionId = cell.regionId;
        validate(job);
        NameContext names;
        names.project = project;
        names.counter = next_ + static_cast<JobId>(preview.size());
        resolveSourceName(job, model, *snapshot->tempo, names);
        if (!members.emplace(job.source.trackId, job.source.resolvedTap(), job.regionId).second)
            throw std::invalid_argument("duplicate export source, signal tap and range");
        Result output;
        output.output = job.directory / media::pathFromUtf8(resolveName(job.nameTemplate, names) + ".wav");
        selectOutput(output, job.collision, ".wav", "stem batch output name is already in use", reserved);
        reserved.insert(foldedFilename(output.output));
        preview.push_back({names.counter, job.source, std::move(output.output), job.regionId,
                           job.beginSample, job.endSample});
    }
    return preview;
}

std::vector<JobId> Queue::addRenderMatrix(const Job& base, std::span<const RenderCell> cells) {
    const auto preview = previewRenderMatrix(base, cells);
    std::vector<JobId> ids;
    std::vector<Entry> pending;
    ids.reserve(preview.size());
    pending.reserve(preview.size());
    for (std::size_t i = 0; i < preview.size(); ++i) {
        auto job = base;
        // Preserve the caller's Main alias and unresolved cycle marker.
        job.source = cells[i].source;
        job.regionId = cells[i].regionId;
        ids.push_back(preview[i].ordinal);
        pending.push_back({ids.back(), std::move(job), {}, next_});
    }
    if (pending.size() > jobs_.max_size() - jobs_.size())
        throw std::length_error("export queue capacity exceeded");
    jobs_.reserve(jobs_.size() + pending.size());
    static_assert(std::is_nothrow_move_constructible_v<Entry>);
    for (auto& entry : pending) jobs_.push_back(std::move(entry));
    next_ += static_cast<JobId>(ids.size());
    return ids;
}

bool Queue::update(JobId id, Job job) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Entry& e) {return e.id==id;});
    if (it == jobs_.end() || it->result.state != JobState::Pending) return false;
    if (it->stemBatch != 0 && (job.niStems || job.exactWavFilename || job.exactPcmFilename))
        throw std::invalid_argument("stem batch entries need ordinary template outputs");
    validateJob(job); it->job = std::move(job); return true;
}
bool Queue::remove(JobId id) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Entry& e) {return e.id==id;});
    if (it == jobs_.end() || it->result.state == JobState::Running) return false;
    jobs_.erase(it); return true;
}
bool Queue::retry(JobId id) {
    for (auto& e : jobs_) if (e.id==id && (e.result.state==JobState::Failed || e.result.state==JobState::Cancelled)) {
        e.result = {}; return true;
    }
    return false;
}

std::string serializeRunManifest(const RunReport& run) {
    using Json = nlohmann::json;
    const auto identityScheme = [](media::FileIdentityScheme scheme) {
        switch(scheme){
        case media::FileIdentityScheme::Posix:return "posix";
        case media::FileIdentityScheme::Windows128:return "windows_file_id_128";
        case media::FileIdentityScheme::WindowsLegacy64:return "windows_legacy_64";
        }
        return "unknown";
    };
    const auto stateName = [](JobState state) {
        switch (state) {
        case JobState::Pending: return "pending";
        case JobState::Running: return "running";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
        }
        return "unknown";
    };
    const auto optionalNumber = [](const std::optional<double>& value) -> Json {
        return value ? Json(*value) : Json(nullptr);
    };
    const auto exportStatistics = [&](const ExportStatistics& statistics) -> Json {
        return {{"integrated_lufs", optionalNumber(statistics.integratedLufs)},
                {"sample_peak_dbfs", optionalNumber(statistics.samplePeakDbfs)},
                {"true_peak_dbtp", optionalNumber(statistics.truePeakDbtp)},
                {"loudness_range_lu", optionalNumber(statistics.loudnessRangeLu)},
                {"maximum_momentary_lufs", optionalNumber(statistics.maximumMomentaryLufs)},
                {"maximum_short_term_lufs", optionalNumber(statistics.maximumShortTermLufs)},
                {"rms_dbfs", optionalNumber(statistics.rmsDbfs)}};
    };
    const auto identityValue = [&](const media::FileIdentity& identity) -> Json {
        return {{"volume",identity.volume},{"file",identity.file},
                {"file_high",identity.fileHigh},{"scheme",identityScheme(identity.scheme)}};
    };
    const auto sourceValue = [](const Source& source) -> Json {
        return {{"track_id", source.trackId},
                {"tap", source.tap == engine::OfflineTap::PreFader ? "pre_fader" : "post_fader"},
                {"signal_tap", signalTapName(source.resolvedTap())}};
    };
    const auto snapshotValue = [&](const RenderSnapshot& snapshot) -> Json {
        return {{"journal_head", snapshot.journalHead}, {"source", sourceValue(snapshot.source)},
            {"begin_sample", snapshot.beginSample}, {"end_sample", snapshot.endSample},
            {"sample_rate", snapshot.sampleRate}, {"graph_sample_rate", snapshot.graphSampleRate},
            {"region_id", snapshot.regionId ? Json(*snapshot.regionId) : Json(nullptr)},
            {"names", {{"project", snapshot.names.project}, {"track", snapshot.names.track},
                {"group", snapshot.names.group}, {"bpm", snapshot.names.bpm},
                {"counter", snapshot.names.counter}, {"region", snapshot.names.region},
                {"format", snapshot.names.format}, {"rate", snapshot.names.rate},
                {"index", snapshot.names.index}}}};
    };
    Json manifest{{"version", 1}, {"planned_count", run.plannedCount},
                  {"capture_complete", run.captureComplete}, {"members", Json::array()}};
    for (const auto& member : run.members) {
        Json item{{"id", member.id}, {"state", member.started ? stateName(member.state) : "unstarted"},
                  {"removed_before_start", member.removedBeforeStart},
                  {"job_capture", member.job ? (member.jobCapturedAtStart ? "job_start" : "run_entry") : "unavailable"},
                  {"job", nullptr}, {"result", nullptr},
                  {"journal_head", nullptr}, {"media_identities", nullptr}};
        if (member.job) {
            const auto& job = *member.job;
            const auto fold = job.deliveries.empty() ? job.encoding.channelFold : job.deliveries.front().encoding.channelFold;
            const auto sampleRate = job.session.sampleRate>=8000 && job.session.sampleRate<=768000
                ? static_cast<std::uint32_t>(job.session.sampleRate) : 0U;
            const auto effectiveDither = job.niStems || job.encoding.format==audio::WavFormat::Float32
                ? audio::WavDither::None
                : audio::effectiveDither(job.encoding.dither,sampleRate);
            Json profile{{"sample_rate", job.session.sampleRate}, {"channels", job.session.channels},
                {"max_frames", job.session.maxFrames},
                {"channel_fold", fold == ChannelFold::StereoAverage ? "stereo_average" : "preserve"},
                {"rate_policy", job.ratePolicy == ExportRatePolicy::ProjectThenDownsample ?
                    "project_then_downsample" : "requested_rate"},
                {"processing_precision", job.session.precision == engine::Precision::Float64 ? "float64" : "float32"},
                {"delay_compensation", job.session.delayCompensation},
                {"external_tempo_bpm", optionalNumber(job.session.externalTempoBpm)},
                {"container", job.niStems ? "ni_stems_mp4" :
                    containerName(job.encoding.container)},
                {"wav_encoding", job.encoding.format == audio::WavFormat::Float32 ? "float32" :
                    (job.encoding.format == audio::WavFormat::Pcm16 ? "pcm16" : "pcm24")},
                {"dither", audio::ditherName(job.encoding.dither)},
                {"effective_dither", audio::ditherName(effectiveDither)},
                {"dither_seed", job.encoding.ditherSeed},
                {"wave_speaker_mask", job.encoding.speakerMask},
                {"normalization", {{"mode", job.normalization.mode == NormalizeMode::Off ? "off" :
                    (job.normalization.mode == NormalizeMode::Peak ? "peak" : "loudness")},
                    {"target_lufs", job.normalization.targetLufs},
                    {"true_peak_ceiling_db", job.normalization.truePeakCeilingDb},
                    {"brickwall", job.normalization.brickwall},
                    {"channel_weights", job.normalization.channelWeights}}}};
            if (job.normalization.mode == NormalizeMode::Loudness) {
                const auto* metric = normalizationTargetName(job.normalization.target);
                auto& normalization = profile["normalization"];
                normalization["target_metric"] = metric ? Json(metric) : Json(nullptr);
                if (job.normalization.target == NormalizationTarget::Rms) {
                    normalization.erase("target_lufs");
                    normalization["target_rms_dbfs"] = job.normalization.targetRmsDbfs;
                }
            }
            if (job.normalization.when != NormalizeWhen::Always) {
                auto& normalization = profile["normalization"];
                const auto* when = normalizeWhenName(job.normalization.when);
                normalization["when"] = when ? Json(when) : Json(nullptr);
                if (job.normalization.skippedLoudnessCeiling) {
                    const auto* ceiling = skippedLoudnessCeilingName(*job.normalization.skippedLoudnessCeiling);
                    normalization["skipped_ceiling"] = ceiling ? Json(ceiling) : Json(nullptr);
                }
            }
            if (job.encoding.container == ExportContainer::Mp3) {
                profile["wav_encoding"] = nullptr;
                profile["effective_dither"] = "none";
                profile["mp3_bit_rate_kbps"] = 320;
            }
            if (job.encoding.container == ExportContainer::OggVorbis) {
                profile["wav_encoding"] = nullptr;
                profile["effective_dither"] = "none";
                profile["vorbis_vbr_quality"] = job.encoding.vorbisQuality;
            }
            if (job.encoding.container == ExportContainer::OggOpus) {
                profile["wav_encoding"] = nullptr;
                profile["effective_dither"] = "none";
                profile["opus_bit_rate"] = job.encoding.opusBitRate;
            }
            if (!job.deliveries.empty()) {
                profile["container"] = "multiple";
                profile.erase("wav_encoding");
                profile.erase("wave_speaker_mask");
                profile.erase("dither");
                profile.erase("dither_seed");
                profile.erase("effective_dither");
                profile.erase("mp3_bit_rate_kbps");
                profile.erase("vorbis_vbr_quality");
                profile.erase("opus_bit_rate");
            }
            if (job.niStems) {
                profile["ni_stems"] = {{"bit_rate_per_stream", job.niStems->bitRate}, {"stems", Json::array()}};
                for (const auto& stem : job.niStems->stems)
                    profile["ni_stems"]["stems"].push_back({{"source", sourceValue(stem.source)},
                        {"name", stem.name}, {"color", stem.color}});
            }
            item["job"] = {{"project", media::pathUtf8(job.project)}, {"directory", media::pathUtf8(job.directory)},
                {"name_template", job.nameTemplate}, {"source", sourceValue(job.source)},
                {"collision",job.collision==Collision::Fail ? "fail" : job.collision==Collision::Increment ? "increment" : "replace"},
                {"render_as_loop", job.renderAsLoop},
                {"tail", {{"mode", tailModeName(job.tail.mode)}, {"maximum_ms", job.tail.maxMilliseconds},
                          {"threshold_dbfs", job.tail.thresholdDbfs}, {"hold_ms", job.tail.holdMilliseconds}}},
                {"requested_range", {{"begin_sample", job.beginSample}, {"end_sample", job.endSample},
                    {"region_id", job.regionId ? Json(*job.regionId) : Json(nullptr)}}},
                {"requested_profile", std::move(profile)}};
            if (job.exactWavFilename) item["job"]["exact_wav_filename"] = *job.exactWavFilename;
            if (job.container != PcmContainer::Wav) item["job"]["delivery_suffix"] = pcmSuffix(job.container);
            if (job.exactPcmFilename) item["job"]["exact_pcm_filename"] = *job.exactPcmFilename;
            if (!job.deliveries.empty()) {
                auto& specs = item["job"]["deliveries"] = Json::array();
                for (const auto& delivery : job.deliveries) {
                    const auto& encoding = delivery.encoding;
                    Json spec{{"container", containerName(encoding.container)}, {"name_suffix", delivery.nameSuffix}};
                    if (encoding.container == ExportContainer::Mp3) {
                        spec["bit_rate_kbps"] = 320;
                    } else if (encoding.container == ExportContainer::OggVorbis) {
                        spec["vbr_quality"] = encoding.vorbisQuality;
                    } else if (encoding.container == ExportContainer::OggOpus) {
                        spec["bit_rate"] = encoding.opusBitRate;
                    } else {
                        spec["pcm_format"] = encoding.format == audio::WavFormat::Float32 ? "float32" :
                            (encoding.format == audio::WavFormat::Pcm16 ? "pcm16" : "pcm24");
                        spec["dither"] = encoding.format == audio::WavFormat::Float32 ? "none" :
                            audio::ditherName(encoding.dither);
                        spec["dither_seed"] = encoding.ditherSeed;
                        if (encoding.container == ExportContainer::Wav) spec["wave_speaker_mask"] = encoding.speakerMask;
                    }
                    specs.push_back(std::move(spec));
                }
            }
            if (job.analysisOnly) item["job"]["analysis_only"] = true;
        }
        if (member.result) {
            const auto& result = *member.result;
            Json value{{"output", media::pathUtf8(result.output)}, {"frames_written", result.framesWritten},
                {"error", result.error}, {"publication", nullptr}, {"stage_cleanup", nullptr},
                {"normalization", nullptr},
                {"resolved_snapshot", nullptr}, {"ni_stem_snapshots", Json::array()}, {"media_verification", nullptr}};
            if (result.deliveryBlake3) value["delivery_blake3"] = *result.deliveryBlake3;
            if (!result.deliveries.empty()) {
                auto& files = value["deliveries"] = Json::array();
                for (const auto& file : result.deliveries) {
                    Json publication = nullptr;
                    if (file.publication) publication = {{"durability_confirmed", file.publication->durabilityConfirmed},
                        {"used_fallback", file.publication->usedFallback}, {"warning", file.publication->warning}};
                    Json delivery{{"output", media::pathUtf8(file.output)}, {"state", stateName(file.state)},
                                  {"publication", std::move(publication)}, {"error", file.error}};
                    if (file.deliveryBlake3) delivery["delivery_blake3"] = *file.deliveryBlake3;
                    files.push_back(std::move(delivery));
                }
            }
            if (member.job && member.job->analysisOnly) {
                value["frames_analyzed"] = result.framesAnalyzed;
                value["analysis"] = result.analysis ? exportStatistics(*result.analysis) : Json(nullptr);
            }
            if(result.replacement) {
                const auto& binding=*result.replacement;
                value["replacement_preflight"]={{"target",media::pathUtf8(binding.target)},
                    {"existing_identity",binding.existing ? identityValue(*binding.existing) : Json(nullptr)},
                    {"blake3",binding.blake3},{"folded_names",binding.foldedNames}};
            }
            if(result.mediaVerification) {
                const auto& verification=*result.mediaVerification;
                value["media_verification"]={{"scope","external_audio_clips"},
                    {"start_complete",verification.startComplete},{"publication_verified",verification.publicationVerified}};
                if (member.job && member.job->analysisOnly) {
                    value["media_verification"]["analysis_verified"] = verification.analysisVerified;
                }
                item["media_identities"]=Json::array();
                for(const auto& identity:verification.identities) {
                    Json mediaValue{{"path",media::pathUtf8(identity.path)}, {"blake3_at_start",identity.blake3AtStart},
                        {"native_identity",identityValue(identity.nativeIdentity)},
                        {"blake3_before_publication",identity.blake3BeforePublication ? Json(*identity.blake3BeforePublication) : Json(nullptr)},
                        {"references",Json::array()}};
                    for(const auto& reference:identity.references)
                        mediaValue["references"].push_back({{"clip_id",reference.clipId},{"media_id",reference.mediaId},
                            {"selected_path",media::pathUtf8(reference.selectedPath)},
                            {"stored_blake3",reference.storedBlake3},
                            {"matches_stored",reference.matchesStored ? Json(*reference.matchesStored) : Json(nullptr)}});
                    item["media_identities"].push_back(std::move(mediaValue));
                }
            }
            if (result.snapshot) {
                item["journal_head"] = result.snapshot->journalHead;
                value["resolved_snapshot"] = snapshotValue(*result.snapshot);
            }
            for (const auto& stem : result.stemSnapshots)
                value["ni_stem_snapshots"].push_back(snapshotValue(stem));
            if (result.publication) value["publication"] = {
                {"durability_confirmed", result.publication->durabilityConfirmed},
                {"used_fallback", result.publication->usedFallback}, {"warning", result.publication->warning}};
            if (result.stageCleanup) value["stage_cleanup"] = {
                {"directory", media::pathUtf8(result.stageCleanup->directory)},
                {"error_code", result.stageCleanup->error.value()},
                {"error_category", result.stageCleanup->error.category().name()},
                {"error", result.stageCleanup->error.message()}};
            if (result.normalization) {
                const auto& report = *result.normalization;
                value["normalization"] = {{"source_lufs", optionalNumber(report.sourceLufs)},
                    {"source_sample_peak_db", optionalNumber(report.sourceSamplePeakDb)},
                    {"source_true_peak_db", optionalNumber(report.sourceTruePeakDb)},
                    {"output_lufs", optionalNumber(report.outputLufs)},
                    {"output_true_peak_db", optionalNumber(report.outputTruePeakDb)},
                    {"captured_pcm", exportStatistics(report.captured)},
                    {"decoded_delivery", exportStatistics(report.decoded)},
                    {"applied_gain_db", report.appliedGainDb}, {"ceiling_limited", report.ceilingLimited},
                    {"encoding_passes", report.encodingPasses}, {"brickwall_applied", report.brickwallApplied},
                    {"conditional_gain_skipped", report.conditionalGainSkipped}};
            }
            item["result"] = std::move(value);
        }
        manifest["members"].push_back(std::move(item));
    }
    return manifest.dump(2);
}

void Queue::run(engine::DeviceLoader loader, Control control, const QueueObserver& observer) {
    if (running_) throw std::logic_error("export queue is already running");
    running_ = true;
    struct Reset { bool& value; ~Reset() {value=false;} } reset{running_};
    // Freeze the run's membership/order. Reentrant callbacks may add jobs for
    // the next run, or edit/remove a not-yet-started job without invalidating us.
    std::vector<JobId> pending;
    for (const auto& e : jobs_) if (e.result.state==JobState::Pending) pending.push_back(e.id);
    lastRun_ = {};
    lastRun_.plannedCount = pending.size();
    try {
        lastRun_.members.reserve(pending.size());
        for (const auto& entry : jobs_) {
            if (entry.result.state != JobState::Pending) continue;
            RunMember member;
            member.id = entry.id;
            try { member.job = entry.job; }
            catch (...) { lastRun_.captureComplete = false; }
            lastRun_.members.push_back(std::move(member));
        }
    } catch (...) {
        lastRun_.members.clear();
        lastRun_.captureComplete = false;
    }
    struct BatchDelivery {
        JobId batch;
        fs::path directory;
        std::string foldedName;
    };
    std::vector<BatchDelivery> deliveries;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto id = pending[index];
        if (control.stop.stop_requested()) break;
        auto* member = lastRun_.members.empty() ? nullptr : &lastRun_.members[index];
        const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Entry& e) {return e.id==id;});
        if (it==jobs_.end() || it->result.state!=JobState::Pending) {
            if (member) member->removedBeforeStart = it==jobs_.end();
            continue;
        }
        const Job job = it->job;
        const auto stemBatch = it->stemBatch;
        const auto requested = observer.start ? observer.start(id) : std::optional{std::stop_token{}};
        if (!requested) {
            jobs_.erase(it);
            if (member) member->removedBeforeStart = true;
            continue;
        }
        it->result.state = JobState::Running;
        if (member) {
            member->started = true;
            member->state = JobState::Running;
            try { member->job = job; member->jobCapturedAtStart = true; }
            catch (...) { member->job.reset(); lastRun_.captureComplete = false; }
        }
        Result result;
        const auto deliveryCount = deliveries.size();
        try {
            Control active = control;
            // A global stop ends the run. A member stop affects only this
            // invocation of the renderer, including its file publication.
            std::stop_source either{std::nostopstate};
            if (requested->stop_possible()) {
                if (control.stop.stop_possible()) {
                    either = std::stop_source{};
                    active.stop = either.get_token();
                } else {
                    active.stop = *requested;
                }
            }
            const auto stopEither = [&either] { (void)either.request_stop(); };
            std::stop_callback stopRun(either.stop_possible() ? control.stop : std::stop_token{}, stopEither);
            std::stop_callback stopMember(either.stop_possible() ? *requested : std::stop_token{}, stopEither);
            // The enqueue ordinal is stable across removal/retry. Reusing a
            // vector index would reuse names of completed files after removal.
            if (stemBatch != 0) {
                result = detail::renderWavPrepared(job, loader, std::move(active), id, id, [&](const Result& output) {
                    const auto directory = fs::canonical(output.output.parent_path());
                    const auto name = foldedFilename(output.output);
                    if (job.collision == Collision::Replace) {
                        const auto inRun = std::any_of(deliveries.begin(), deliveries.end(), [&](const BatchDelivery& earlier) {
                            return earlier.batch == stemBatch && earlier.directory == directory && earlier.foldedName == name;
                        });
                        const auto inQueue = std::any_of(jobs_.begin(), jobs_.end(), [&](const Entry& entry) {
                            return entry.stemBatch == stemBatch && entry.result.state == JobState::Completed &&
                                   foldedFilename(entry.result.output) == name &&
                                   fs::weakly_canonical(entry.result.output.parent_path()) == directory;
                        });
                        if (inRun || inQueue)
                            throw std::runtime_error("stem batch member would replace an earlier delivery");
                    }
                    // Reserve before publication. A callback may remove an
                    // earlier completed entry from the current queue.
                    deliveries.push_back({stemBatch, directory, name});
                });
            } else {
                result = job.niStems ? renderNiStems(job, loader, std::move(active), id, id)
                                     : renderWav(job, loader, std::move(active), id, id);
            }
        } catch (const std::exception& e) {
            result.state = JobState::Failed; result.error = e.what();
        } catch (...) {
            result.state = JobState::Failed; result.error = "cannot initialize export job";
        }
        if (result.state != JobState::Completed && deliveries.size() > deliveryCount) deliveries.pop_back();
        for (auto& e : jobs_) if (e.id==id) {
            e.result=std::move(result);
            if (member) {
                member->state = e.result.state;
                try { member->result = e.result; }
                catch (...) { member->result.reset(); lastRun_.captureComplete = false; }
            }
            if (observer.finished) observer.finished(id, e.result);
            break;
        }
    }
    for (auto& member : lastRun_.members) if (!member.started)
        member.removedBeforeStart = std::none_of(jobs_.begin(), jobs_.end(),
            [&](const Entry& entry) { return entry.id==member.id; });
}
} // namespace adi::render
