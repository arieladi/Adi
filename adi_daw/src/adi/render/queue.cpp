// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/queue.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace adi::render {
namespace fs = std::filesystem;
static_assert(std::is_nothrow_move_assignable_v<Result>);
static_assert(std::is_nothrow_move_assignable_v<std::string>);
namespace {
void validate(const Job& j) {
    if (j.project.empty() || j.directory.empty()) throw std::invalid_argument("project and output directory are required");
    if (j.collision != Collision::Fail && j.collision != Collision::Increment)
        throw std::invalid_argument("unsupported export collision policy");
    if (!j.regionId && (j.beginSample < 0 || j.endSample <= j.beginSample)) throw std::invalid_argument("export range must be nonempty and nonnegative");
    if (!std::isfinite(j.session.sampleRate) || j.session.sampleRate < 8000 || j.session.sampleRate > 768000 ||
        std::floor(j.session.sampleRate) != j.session.sampleRate)
        throw std::invalid_argument("export sample rate must be an integer from 8000 through 768000");
    if (j.session.channels < 1 || j.session.channels > 64 || j.session.maxFrames < 1 || j.session.maxFrames > 4096)
        throw std::invalid_argument("export needs 1..64 channels and 1..4096 frames per block");
    if (j.session.precision != engine::Precision::Float32 && j.session.precision != engine::Precision::Float64)
        throw std::invalid_argument("unknown processing precision");
    if (j.regionId && *j.regionId <= 0) throw std::invalid_argument("region ID must be positive");
    if (j.source.trackId < 0 || (j.source.tap != engine::OfflineTap::PreFader && j.source.tap != engine::OfflineTap::PostFader))
        throw std::invalid_argument("invalid direct stem source or tap");
    if (j.encoding.format != audio::WavFormat::Float32 && j.encoding.format != audio::WavFormat::Pcm16 && j.encoding.format != audio::WavFormat::Pcm24)
        throw std::invalid_argument("unsupported export WAV encoding");
    if (j.encoding.dither != audio::WavDither::None && j.encoding.dither != audio::WavDither::Triangular && j.encoding.dither != audio::WavDither::Rectangular)
        throw std::invalid_argument("unsupported export dither");
    validateNiStems(j);
    validateNormalization(j.normalization,static_cast<std::uint32_t>(j.session.sampleRate),j.session.channels);
    (void)resolveName(j.nameTemplate, {"project","master","group","120",1,"region"});
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
std::int64_t addLatency(std::int64_t position, std::int64_t latency) {
    if (latency > 0 && position > std::numeric_limits<std::int64_t>::max() - latency)
        throw std::invalid_argument("export range plus latency overflows");
    return position + latency; // position >= 0; cached endpoint latency is the sum of two int32 values
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
using detail::Cancelled;
void cancellation(const Control& c) { if (c.stop.stop_requested()) throw Cancelled{}; }
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
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::size_t> nativeFiles;
    for(const auto& source:playback.externalSources()) {
        cancellation(control);
        auto selected=selectedPaths.find(source.path);
        if(selected==selectedPaths.end()) {
            const auto native=mediaIdentityAt(source.path);
            const auto [found,inserted]=nativeFiles.try_emplace({native.volume,native.file},report.identities.size());
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
void verifyMedia(MediaVerification& report,const Control& control) {
    report.publicationVerified = false;
    std::map<fs::path,media::FileIdentity> selectedPaths;
    for(auto& identity:report.identities) {
        identity.blake3BeforePublication=hashMedia(identity.path,control);
        for(const auto& reference:identity.references) {
            cancellation(control);
            auto selected=selectedPaths.find(reference.selectedPath);
            if(selected==selectedPaths.end())
                selected=selectedPaths.emplace(reference.selectedPath,mediaIdentityAt(reference.selectedPath)).first;
            if(selected->second!=identity.nativeIdentity)
                throw std::runtime_error("export media path changed during render: "+media::pathUtf8(reference.selectedPath));
        }
        if(*identity.blake3BeforePublication!=identity.blake3AtStart)
            throw std::runtime_error("export media changed during render: "+media::pathUtf8(identity.path));
    }
    cancellation(control);
    report.publicationVerified=true;
}
} // namespace

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
        else throw std::invalid_argument("unknown export name token: " + token);
        at = end + 1;
    }
    // Expanded values never become paths. Do not rescan braces in metadata.
    for (char& c : name)
        if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == "..") throw std::invalid_argument("export name is empty");
    if (name.size() > 200) throw std::invalid_argument("export name exceeds 200 UTF-8 bytes");
    auto stem = name.substr(0, name.find('.'));
    for (char& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const bool numberedDevice = (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        ((stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') ||
         stem.substr(3) == "\xC2\xB9" || stem.substr(3) == "\xC2\xB2" || stem.substr(3) == "\xC2\xB3");
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$" || numberedDevice) name = "_" + name;
    return name;
}

void validateOutputName(const fs::path& output) {
    if (media::pathUtf8(output.filename()).size() > 200)
        throw std::invalid_argument("final export name exceeds 200 UTF-8 bytes");
}

void detail::prepareOutput(Result& result,Collision collision,const char* suffix,const char* collisionMessage) {
    if(collision!=Collision::Fail && collision!=Collision::Increment)
        throw std::invalid_argument("unsupported export collision policy");
    validateOutputName(result.output);
    const auto directory=result.output.parent_path();
    if(!fs::is_directory(directory))throw std::runtime_error("export directory does not exist");
    const auto occupied=[](const fs::path& path) {
        std::error_code error;
        const auto status=fs::symlink_status(path,error);
        if(error==std::errc::no_such_file_or_directory)return false;
        if(error)throw fs::filesystem_error("inspect export destination",path,error);
        // A dangling symlink or directory is occupied too. Native lookup keeps
        // the actual directory's case semantics; never use ASCII-only folding.
        return status.type()!=fs::file_type::not_found;
    };
    if(!occupied(result.output))return;
    if(collision==Collision::Fail)throw std::runtime_error(collisionMessage);
    const auto filename=media::pathUtf8(result.output.filename());
    const std::string_view extension(suffix);
    if(extension.empty() || !filename.ends_with(extension))throw std::invalid_argument("invalid export suffix");
    const auto stem=filename.substr(0,filename.size()-extension.size());
    // With N directory entries, one of N+1 numbered names must be free unless
    // the directory changes while being inspected. Do not count the original:
    // it might be a native short-name alias of one numbered long filename.
    // Bound by the snapshot instead of an arbitrary maximum delivery number.
    std::uint64_t entries=0;
    for(const auto& entry:fs::directory_iterator(directory)) {
        (void)entry;
        if(entries==std::numeric_limits<std::uint64_t>::max()-2)
            throw std::runtime_error("export directory has too many entries");
        ++entries;
    }
    for(std::uint64_t offset=0;offset<=entries;++offset) {
        const auto ordinal=offset+2;
        result.output=directory/media::pathFromUtf8(stem+" ("+std::to_string(ordinal)+")"+std::string(extension));
        validateOutputName(result.output); // includes both increment and entire suffix
        if(!occupied(result.output))return;
    }
    throw std::runtime_error("export collision set changed during preflight; retry");
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

void detail::publishVerifiedOutput(const fs::path& stage, Result& result, const Control& control,
                                   const char* collisionMessage) {
    if (!result.mediaVerification || !result.mediaVerification->startComplete)
        throw std::runtime_error("export has no complete input media capture");
    verifyMedia(*result.mediaVerification, control);
    publishOutput(stage, result, control.publicationHooks, collisionMessage);
}

void detail::publishOutput(const fs::path& stage, Result& result, media::PublishHooks hooks,
                          const char* collisionMessage) {
    // Reserve the fallback message before the irreversible commit point.
    result.publication.emplace(PublicationReport{false, false,
        "file published; durability could not be confirmed"});
    auto published = media::publishFile(stage, result.output, hooks, media::PublishPolicy::AtomicOnly);
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
    Result result;
    try {
        cancellation(control);
        validate(job);
        if (job.niStems) throw std::invalid_argument("renderWav requires a WAV job; use renderNiStems for NI Stems");
        StoreError error = StoreError::Ok;
        auto store = Store::open(job.project, error, true);
        if (!store || error != StoreError::Ok) throw std::runtime_error(std::string("open export project: ") + toString(error));
        engine::Session session;
        session.graph().setFadeFrames(0);
        NameContext names;
        names.counter = counter;
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
            renderSpec.performanceState=false;
            renderSpec.exporting=true;
            if (!session.load(*store, std::move(loader), renderSpec)) throw std::runtime_error(session.error());
            if (session.stats().placeholders || session.stats().skipped || !session.problems().empty()) {
                std::string why = "export requires a fully resolved project";
                for (const auto& problem : session.problems()) why += "; " + problem;
                throw std::runtime_error(why);
            }
            names.project = store->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
            const auto& tracks = session.model().tracks;
            auto selected = tracks.end();
            for (auto it = tracks.begin(); it != tracks.end(); ++it)
                if (job.source.trackId ? it->id == job.source.trackId : it->kind == "master") { selected=it; break; }
            if (selected == tracks.end()) throw std::runtime_error("export source track is missing");
            job.source.trackId = selected->id;
            names.track = selected->name;
            auto parent = selected;
            for (std::size_t depth = 0; parent != tracks.end() && depth < tracks.size(); ++depth) {
                if (parent->kind == "group") { names.group=parent->name; break; }
                if (!parent->parentId) break;
                const auto idOfParent = *parent->parentId;
                parent = std::find_if(tracks.begin(), tracks.end(), [idOfParent](const auto& t) {return t.id==idOfParent;});
            }
            const auto snap = engine::SnapshotBuilder::fromStore(*store);
            if (job.regionId) resolveRegion(job, session.model(), *snap->tempo, names);
            names.bpm = textproj::renderF64(snap->tempo->bpmAt(snap->tempo->secondsToTicks(
                static_cast<double>(job.beginSample) / job.session.sampleRate)));
            result.snapshot = RenderSnapshot{journalHead, job.source, job.beginSample, job.endSample,
                job.session.sampleRate, job.regionId, names};
            snapshot.commit();
        }
        const auto filename = resolveName(job.nameTemplate, names) + ".wav";
        result.output = job.directory / media::pathFromUtf8(filename);
        detail::prepareOutput(result,job.collision,".wav","export destination already exists");
        const auto* graph = session.graph().currentGraph();
        if (!graph) throw std::runtime_error("export has no master graph");
        const auto endpoint = session.offlineOutput(job.source.trackId, job.source.tap);
        if (!endpoint) throw std::runtime_error("export source has no audio output at the selected tap");
        result.mediaVerification.emplace();
        captureMedia(*session.clips(),*result.mediaVerification,control);
        const auto latency = endpoint->latency;
        auto& coalescer = session.devices().coalescer();
        coalescer.setQuietPeriodMs(0); // offline jobs must not outrun the live debounce
        const auto reports = coalescer.stats().reports;
        const auto rawBegin = addLatency(job.beginSample, latency);
        const auto rawEnd = addLatency(job.endSample, latency);
        const auto total = static_cast<std::uint64_t>(job.endSample - job.beginSample);
        const auto channels = static_cast<std::size_t>(job.session.channels);
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
        Stage stage(job.directory, result);
        const auto staged = stage.path / "render.wav";
        std::unique_ptr<audio::WavWriter> writer;
        std::unique_ptr<NormalizationCapture> normalized;
        if(job.normalization.mode==NormalizeMode::Off)
            writer=std::make_unique<audio::WavWriter>(staged,static_cast<std::uint32_t>(job.session.sampleRate),
                static_cast<std::uint16_t>(channels),job.encoding.format,nullptr,std::uint64_t{1}<<32,
                job.encoding.dither,job.encoding.ditherSeed^counter);
        else normalized=std::make_unique<NormalizationCapture>(stage.path/"capture.f64",static_cast<std::uint32_t>(job.session.sampleRate),job.session.channels,job.normalization);
        auto write=[&](std::uint32_t frames) {
            if(normalized)normalized->append(std::span<const double>(interleaved.data(),static_cast<std::size_t>(frames)*channels),frames);
            else writer->write(interleaved.data(),frames);
            result.framesWritten+=frames;
        };
        auto progress = [&] {
            if (control.progress) control.progress(id, result.framesWritten, total);
            cancellation(control);
        };
        progress();
        // Signed output latency may map a requested position before transport
        // zero: that is silence, not a negative locate (which clamps).
        if (rawBegin < 0) {
            auto silence = (std::min)(total, static_cast<std::uint64_t>(-rawBegin));
            while (silence > 0) {
                const auto frames = static_cast<std::uint32_t>((std::min)(silence, static_cast<std::uint64_t>(block)));
                write(frames);
                silence -= frames; progress();
            }
        }
        session.transport().loop(0, 0, false);
        session.transport().locate(0);
        session.transport().play();
        const auto started = std::chrono::steady_clock::now();
        auto service = [&] {
            cancellation(control);
            if (control.serviceHost) control.serviceHost();
            cancellation(control);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            session.tick(elapsed);
            // A shorter branch can change compensation without changing the
            // master's total latency. Any latency/shape report invalidates the run.
            if (coalescer.stats().reports != reports || session.graph().currentGraph() != graph ||
                graph->outputLatencyOf(endpoint->node) != latency)
                throw std::runtime_error("device latency or topology changed during export; retry after it settles");
        };
        // Preroll from zero preserves instruments, effects and automation.
        for (std::int64_t at = 0; at < rawEnd;) {
            service();
            if (!session.clips()->prime()) throw std::runtime_error("export clip read timed out");
            const auto frames = static_cast<std::int32_t>((std::min)(static_cast<std::int64_t>(block), rawEnd - at));
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
            const auto last = at + frames;
            // Validate preroll too: a nonfinite value may poison later state.
            for (std::int32_t f = 0; f < frames; ++f) for (std::size_t c = 0; c < channels; ++c) {
                const auto i = static_cast<std::size_t>(f);
                const double value = dp.empty() ? static_cast<double>(fp[c][i]) : dp[c][i];
                if (!std::isfinite(value) || (!normalized && job.encoding.format == audio::WavFormat::Float32 &&
                    std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())))
                    throw std::runtime_error("nonfinite or out-of-range audio during export");
                if (at + f >= first)
                    interleaved[static_cast<std::size_t>(at + f - first) * channels + c] = value;
            }
            if (first < last) {
                const auto count = static_cast<std::uint32_t>(last - first);
                write(count);
            }
            at = last;
            progress();
        }
        if (result.framesWritten != total) throw std::runtime_error("export produced an incomplete range");
        service(); // include reports raised by the final process call
        if(normalized)result.normalization=normalized->finish(staged,job.encoding.format,job.encoding.dither,job.encoding.ditherSeed^counter,[&] {
            cancellation(control);if(control.serviceHost)control.serviceHost();cancellation(control);
        });
        else writer->close();
        cancellation(control);
        detail::publishVerifiedOutput(staged, result, control, "export destination already exists");
    } catch (const Cancelled&) {
        result.state = JobState::Cancelled;
    } catch (const std::exception& e) {
        result.state = JobState::Failed; result.error = e.what();
    } catch (...) {
        result.state = JobState::Failed; result.error = "unknown exception during export";
    }
    return result;
}

Result renderMasterWav(Job job, engine::DeviceLoader loader, Control control,
                       JobId id, std::uint64_t counter) {
    if (job.source.trackId != 0 || job.source.tap != engine::OfflineTap::PostFader) {
        Result result;
        result.state = JobState::Failed;
        result.error = "renderMasterWav requires the default master source; use renderWav for stems";
        return result;
    }
    return renderWav(std::move(job), std::move(loader), std::move(control), id, counter);
}

JobId Queue::add(Job job) {
    validate(job);
    if (next_ == std::numeric_limits<JobId>::max()) throw std::overflow_error("export job IDs exhausted");
    const auto id = next_++;
    jobs_.push_back({id, std::move(job), {}});
    return id;
}
bool Queue::update(JobId id, Job job) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Entry& e) {return e.id==id;});
    if (it == jobs_.end() || it->result.state != JobState::Pending) return false;
    validate(job); it->job = std::move(job); return true;
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
    const auto sourceValue = [](const Source& source) -> Json {
        return {{"track_id", source.trackId},
                {"tap", source.tap == engine::OfflineTap::PreFader ? "pre_fader" : "post_fader"}};
    };
    const auto snapshotValue = [&](const RenderSnapshot& snapshot) -> Json {
        return {{"journal_head", snapshot.journalHead}, {"source", sourceValue(snapshot.source)},
            {"begin_sample", snapshot.beginSample}, {"end_sample", snapshot.endSample},
            {"sample_rate", snapshot.sampleRate},
            {"region_id", snapshot.regionId ? Json(*snapshot.regionId) : Json(nullptr)},
            {"names", {{"project", snapshot.names.project}, {"track", snapshot.names.track},
                {"group", snapshot.names.group}, {"bpm", snapshot.names.bpm},
                {"counter", snapshot.names.counter}, {"region", snapshot.names.region}}}};
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
            Json profile{{"sample_rate", job.session.sampleRate}, {"channels", job.session.channels},
                {"max_frames", job.session.maxFrames},
                {"processing_precision", job.session.precision == engine::Precision::Float64 ? "float64" : "float32"},
                {"delay_compensation", job.session.delayCompensation},
                {"external_tempo_bpm", optionalNumber(job.session.externalTempoBpm)},
                {"container", job.niStems ? "ni_stems_mp4" : "wav"},
                {"wav_encoding", job.encoding.format == audio::WavFormat::Float32 ? "float32" :
                    (job.encoding.format == audio::WavFormat::Pcm16 ? "pcm16" : "pcm24")},
                {"dither", job.encoding.dither == audio::WavDither::None ? "none" :
                    (job.encoding.dither == audio::WavDither::Triangular ? "triangular" : "rectangular")},
                {"dither_seed", job.encoding.ditherSeed},
                {"normalization", {{"mode", job.normalization.mode == NormalizeMode::Off ? "off" :
                    (job.normalization.mode == NormalizeMode::Peak ? "peak" : "loudness")},
                    {"target_lufs", job.normalization.targetLufs},
                    {"true_peak_ceiling_db", job.normalization.truePeakCeilingDb},
                    {"channel_weights", job.normalization.channelWeights}}}};
            if (job.niStems) {
                profile["ni_stems"] = {{"bit_rate_per_stream", job.niStems->bitRate}, {"stems", Json::array()}};
                for (const auto& stem : job.niStems->stems)
                    profile["ni_stems"]["stems"].push_back({{"source", sourceValue(stem.source)},
                        {"name", stem.name}, {"color", stem.color}});
            }
            item["job"] = {{"project", media::pathUtf8(job.project)}, {"directory", media::pathUtf8(job.directory)},
                {"name_template", job.nameTemplate}, {"source", sourceValue(job.source)},
                {"collision",job.collision==Collision::Fail ? "fail" : "increment"},
                {"requested_range", {{"begin_sample", job.beginSample}, {"end_sample", job.endSample},
                    {"region_id", job.regionId ? Json(*job.regionId) : Json(nullptr)}}},
                {"requested_profile", std::move(profile)}};
        }
        if (member.result) {
            const auto& result = *member.result;
            Json value{{"output", media::pathUtf8(result.output)}, {"frames_written", result.framesWritten},
                {"error", result.error}, {"publication", nullptr}, {"stage_cleanup", nullptr},
                {"normalization", nullptr},
                {"resolved_snapshot", nullptr}, {"ni_stem_snapshots", Json::array()}, {"media_verification", nullptr}};
            if(result.mediaVerification) {
                const auto& verification=*result.mediaVerification;
                value["media_verification"]={{"scope","external_audio_clips"},
                    {"start_complete",verification.startComplete},{"publication_verified",verification.publicationVerified}};
                item["media_identities"]=Json::array();
                for(const auto& identity:verification.identities) {
                    Json mediaValue{{"path",media::pathUtf8(identity.path)}, {"blake3_at_start",identity.blake3AtStart},
                        {"native_identity",{{"volume",identity.nativeIdentity.volume},{"file",identity.nativeIdentity.file}}},
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
                    {"applied_gain_db", report.appliedGainDb}, {"ceiling_limited", report.ceilingLimited},
                    {"encoding_passes", report.encodingPasses}};
            }
            item["result"] = std::move(value);
        }
        manifest["members"].push_back(std::move(item));
    }
    return manifest.dump(2);
}

void Queue::run(engine::DeviceLoader loader, Control control) {
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
        it->result.state = JobState::Running;
        if (member) {
            member->started = true;
            member->state = JobState::Running;
            try { member->job = job; member->jobCapturedAtStart = true; }
            catch (...) { member->job.reset(); lastRun_.captureComplete = false; }
        }
        Result result;
        try {
            // The enqueue ordinal is stable across removal/retry. Reusing a
            // vector index would reuse names of completed files after removal.
            result = job.niStems ? renderNiStems(job, loader, control, id, id)
                                 : renderWav(job, loader, control, id, id);
        } catch (const std::exception& e) {
            result.state = JobState::Failed; result.error = e.what();
        } catch (...) {
            result.state = JobState::Failed; result.error = "cannot initialize export job";
        }
        for (auto& e : jobs_) if (e.id==id) {
            e.result=std::move(result);
            if (member) {
                member->state = e.result.state;
                try { member->result = e.result; }
                catch (...) { member->result.reset(); lastRun_.captureComplete = false; }
            }
            break;
        }
    }
    for (auto& member : lastRun_.members) if (!member.started)
        member.removedBeforeStart = std::none_of(jobs_.begin(), jobs_.end(),
            [&](const Entry& entry) { return entry.id==member.id; });
}
} // namespace adi::render
