// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/queue.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace adi::render {
namespace fs = std::filesystem;
namespace {
void validate(const Job& j) {
    if (j.project.empty() || j.directory.empty()) throw std::invalid_argument("project and output directory are required");
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
    (void)resolveName(j.nameTemplate, {"project","master","group","120",1,"region"});
}
struct Stage {
    fs::path path;
    explicit Stage(const fs::path& parent) {
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
    ~Stage() { std::error_code ec; fs::remove_all(path, ec); }
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
struct Cancelled {};
void cancellation(const Control& c) { if (c.stop.stop_requested()) throw Cancelled{}; }
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

Result renderWav(Job job, engine::DeviceLoader loader, Control control,
                       JobId id, std::uint64_t counter) {
    Result result;
    try {
        cancellation(control);
        validate(job);
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
            if (!session.load(*store, std::move(loader), job.session)) throw std::runtime_error(session.error());
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
            snapshot.commit();
        }
        const auto filename = resolveName(job.nameTemplate, names) + ".wav";
        result.output = job.directory / media::pathFromUtf8(filename);
        if (!fs::is_directory(job.directory)) throw std::runtime_error("export directory does not exist");
        if (fs::exists(result.output)) throw std::runtime_error("export destination already exists");
        const auto* graph = session.graph().currentGraph();
        if (!graph) throw std::runtime_error("export has no master graph");
        const auto endpoint = session.offlineOutput(job.source.trackId, job.source.tap);
        if (!endpoint) throw std::runtime_error("export source has no audio output at the selected tap");
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
        Stage stage(job.directory);
        const auto staged = stage.path / "render.wav";
        audio::WavWriter writer(staged, static_cast<std::uint32_t>(job.session.sampleRate),
                                static_cast<std::uint16_t>(channels), job.encoding.format, nullptr,
                                std::uint64_t{1} << 32, job.encoding.dither, job.encoding.ditherSeed ^ counter);
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
                writer.write(interleaved.data(), frames);
                result.framesWritten += frames; silence -= frames; progress();
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
                if (!std::isfinite(value) || (job.encoding.format == audio::WavFormat::Float32 &&
                    std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())))
                    throw std::runtime_error("nonfinite or out-of-range audio during export");
                if (at + f >= first)
                    interleaved[static_cast<std::size_t>(at + f - first) * channels + c] = value;
            }
            if (first < last) {
                const auto count = static_cast<std::uint32_t>(last - first);
                writer.write(interleaved.data(), count); result.framesWritten += count;
            }
            at = last;
            progress();
        }
        if (result.framesWritten != total) throw std::runtime_error("export produced an incomplete range");
        service(); // include reports raised by the final process call
        writer.close();
        cancellation(control);
        const auto published = media::publishFile(staged, result.output);
        if (published.status != media::PublishStatus::published)
            throw std::runtime_error(published.status == media::PublishStatus::exists ? "export destination already exists" : published.error);
        result.state = JobState::Completed; // publication is the commit point
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
void Queue::run(engine::DeviceLoader loader, Control control) {
    if (running_) throw std::logic_error("export queue is already running");
    running_ = true;
    struct Reset { bool& value; ~Reset() {value=false;} } reset{running_};
    // Freeze the run's membership/order. Reentrant callbacks may add jobs for
    // the next run, or edit/remove a not-yet-started job without invalidating us.
    std::vector<JobId> pending;
    for (const auto& e : jobs_) if (e.result.state==JobState::Pending) pending.push_back(e.id);
    for (const auto id : pending) {
        if (control.stop.stop_requested()) break;
        const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Entry& e) {return e.id==id;});
        if (it==jobs_.end() || it->result.state!=JobState::Pending) continue;
        const Job job = it->job;
        it->result.state = JobState::Running;
        Result result;
        try {
            // The enqueue ordinal is stable across removal/retry. Reusing a
            // vector index would reuse names of completed files after removal.
            result = renderWav(job, loader, control, id, id);
        } catch (const std::exception& e) {
            result.state = JobState::Failed; result.error = e.what();
        } catch (...) {
            result.state = JobState::Failed; result.error = "cannot initialize export job";
        }
        for (auto& e : jobs_) if (e.id==id) {e.result=std::move(result); break;}
    }
}
} // namespace adi::render
