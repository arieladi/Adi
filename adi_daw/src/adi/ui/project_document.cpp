// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_document.hpp"
#include "arrangement_geometry.hpp"
#include "adi/audio/decode.hpp"
#include "adi/bounce_commands.hpp"
#include "adi/controller_data.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/freeze_commands.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/collect_export.hpp"
#include "adi/media/blake3.hpp"
#include "adi/midi_capture.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/mix_function_edit.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include "adi/device_key_routes.hpp"
#include "adi/recording_commands.hpp"
#include "adi/render/host_dispatch.hpp"
#include "adi/automation_splice_store.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/device_param_ref.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/library/index.hpp"
#include "adi/sample_defaults.hpp"
#include "juce/builtin_samples.hpp"
#include "juce/mix_function_host.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <set>
#include <thread>
#include <utility>
#include <variant>
namespace adi::ui {
// Where the document's file is now. The message thread moves it at Save As; the waveform worker
// reads it at each build.
struct WaveformSource {
    explicit WaveformSource(std::filesystem::path file) : path(std::move(file)) {}
    void waitForReader(const std::filesystem::path &retiredFile) {
        std::unique_lock lock(mutex);
        readerReleased.wait(lock, [&] { return activeReader != retiredFile; });
    }

    struct Read {
        explicit Read(WaveformSource &owner) : source(owner) {
            std::lock_guard lock(source.mutex);
            file = source.path;
            source.activeReader = file;
            store = std::move(source.idleReader);
        }
        ~Read() {
            {
                std::lock_guard lock(source.mutex);
                if (source.path == file)
                    source.idleReader = std::move(store);
                else
                    store.reset();
                source.activeReader.reset();
            }
            source.readerReleased.notify_all();
        }

        WaveformSource &source;
        std::filesystem::path file;
        std::unique_ptr<Store> store;
    };

    std::mutex mutex;
    std::condition_variable readerReleased;
    std::filesystem::path path;
    std::optional<std::filesystem::path> activeReader;
    std::unique_ptr<Store> idleReader;
    media::AnalysisFileWrites analysisFileWrites = media::AnalysisFileWrites::Enabled;
};
struct ProjectDocument::MidiRecording {
    struct Track {
        struct Held {
            std::size_t index = 0;
            bool monitored = false;
        };
        std::optional<engine::Session::MidiRecordMonitor> monitor;
        engine::MidiInputFilter filter;
        std::int64_t quantizeGrid = 0;
        std::vector<notes::NoteValues> notes;
        std::map<std::uint64_t, Held> held;
        std::map<std::pair<std::uint64_t, ExpressionDim>, std::vector<ExpressionPoint>> expression;
        std::map<std::pair<int, int>, std::vector<controllers::Point>> controllerPoints;
    };
    std::shared_ptr<const engine::TempoMap> tempo;
    double sampleRate = 0;
    std::vector<Track> tracks;
    std::vector<LiveMidiTake> preview;
    bool started = false, overdub = false;
    std::int64_t firstStreamSample = 0;
    engine::MidiCaptureState state = engine::MidiCaptureState::Waiting;
    engine::RecordingPunchWindow punchWindow;

    std::int64_t tickAt(std::int64_t sample) const noexcept {
        const double seconds = static_cast<double>(std::max<std::int64_t>(0, sample)) / sampleRate;
        const auto tick = tempo->secondsToTicks(seconds);
        if (tick == (std::int64_t{1} << 50))
            return tick;
        const auto next = tempo->ticksToSeconds(tick + 1);
        return next - seconds < seconds - tempo->ticksToSeconds(tick) ? tick + 1 : tick;
    }

    bool monitorsAt(const Track &track, std::int64_t sample) const noexcept {
        if (!track.monitor)
            return false;
        if (overdub)
            return true;
        const auto &ranges = track.monitor->blockedRanges;
        const auto range = std::upper_bound(ranges.begin(), ranges.end(), sample,
            [](std::int64_t at, const engine::MidiMonitorRange &span) { return at < span.end; });
        return range == ranges.end() || sample < range->start;
    }

    void ingest(const engine::MidiCapturePacket &packet) {
        if (packet.kind == engine::MidiCapturePacket::Kind::Span) {
            if (!started)
                firstStreamSample = packet.streamSample;
            const auto firstSample = packet.timelineSample;
            const auto lastSample = firstSample + packet.frames;
            // The authored brace can share a sample with several nearby ticks.
            const auto start = punchWindow.in && firstSample == punchWindow.in->sample
                ? punchWindow.in->tick : tickAt(firstSample);
            const auto end = punchWindow.out && lastSample == punchWindow.out->sample
                ? punchWindow.out->tick : tickAt(lastSample);
            for (auto &take : preview) {
                if (!started)
                    take.start = start;
                take.end = std::max(take.start + 1, end);
            }
            started = true;
            return;
        }
        const auto &event = packet.event;
        const bool channelPitch = capture::isChannelPitchWheel(event);
        const bool control = event.type == engine::EventType::Controller || channelPitch;
        if (!started || (!control && event.noteId == 0))
            return;
        const auto address = channelPitch ? controllers::kPitchBend : event.paramId;
        const auto controlValue = channelPitch ? capture::channelPitchWheelValue(event.value) : event.value;
        if (control && (event.channel >= 16 || !controllers::validController(address) ||
                        !std::isfinite(controlValue) || controlValue < 0. || controlValue > 1.))
            return;
        const auto played = tickAt(packet.timelineSample - (packet.streamSample - packet.inputSample));
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto &track = tracks[i];
            if (!track.filter.matches(packet.port, event.channel))
                continue;
            auto &take = preview[i];
            const auto held = track.held.find(event.noteId);
            // Latch the onset's clock for its release and expression, even if the note spans a clip edge.
            const bool monitored = control || event.type == engine::EventType::NoteOn
                ? monitorsAt(track, packet.timelineSample)
                : held != track.held.end() && held->second.monitored;
            // Live input goes directly to the instrument. A replayed clip passes through the upstream nodes first.
            const auto tick = monitored ? tickAt(packet.timelineSample - track.monitor->instrumentArrival) : played;
            const auto at = std::clamp(tick, take.start, take.end - 1);
            if (control) {
                if (!monitored && packet.inputSample < firstStreamSample)
                    continue;
                auto &lane = track.controllerPoints[{event.channel, static_cast<int>(address)}];
                lane.push_back({lane.size() + 1, at - take.start, controlValue});
            } else if (event.type == engine::EventType::NoteOn) {
                if (held != track.held.end() || (!monitored && packet.inputSample < firstStreamSample))
                    continue;
                const auto index = track.notes.size();
                const auto velocity = static_cast<int>(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
                track.held.emplace(event.noteId, Track::Held{index, monitored});
                track.notes.push_back({at - take.start, 1, index + 1, event.dim, std::max(1, velocity), 0, event.channel});
                take.notes.push_back({at, take.end, event.dim});
            } else if (held != track.held.end()) {
                auto &note = track.notes[held->second.index];
                auto &shown = take.notes[held->second.index];
                if (event.type == engine::EventType::NoteOff) {
                    shown.end = std::max(shown.start + 1, at);
                    note.duration = shown.end - shown.start;
                    note.releaseVelocity = static_cast<int>(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
                    track.held.erase(held);
                } else if (event.type == engine::EventType::NoteExpression && event.dim <= static_cast<std::uint16_t>(ExpressionDim::Timbre)) {
                    const auto dimension = static_cast<ExpressionDim>(event.dim);
                    const auto value = dimension == ExpressionDim::Pitch ? std::clamp(event.value, -48.0, 48.0)
                                                                         : std::clamp(event.value, 0.0, 1.0);
                    auto &lane = track.expression[{note.id, dimension}];
                    ExpressionPoint point{};
                    point.time_ticks = std::max(shown.start, at) - shown.start;
                    point.value = static_cast<float>(value);
                    point.curve = static_cast<std::uint8_t>(Curve::Hold);
                    if (!lane.empty() && point.time_ticks <= lane.back().time_ticks) {
                        point.time_ticks = lane.back().time_ticks;
                        lane.back() = point;
                    } else {
                        lane.push_back(point);
                    }
                }
            }
        }
    }

    void drain(engine::MidiInputCapture &capture) {
        std::array<engine::MidiCapturePacket, 128> packets;
        // The callback publishes its last packets before its terminal state. Observing the state
        // first lets the final drain collect everything before releasing the take.
        state = capture.state();
        while (const auto count = capture.drain(packets, &punchWindow))
            for (const auto &packet : std::span(packets).first(count))
                ingest(packet);
        for (std::size_t i = 0; i < tracks.size(); ++i)
            for (const auto &item : tracks[i].held) {
                auto &shown = preview[i].notes[item.second.index];
                shown.end = preview[i].end;
                tracks[i].notes[item.second.index].duration = shown.end - shown.start;
            }
    }

    bool finished() const noexcept {
        return state != engine::MidiCaptureState::Waiting && state != engine::MidiCaptureState::Recording;
    }

    struct Placement {
        std::vector<std::vector<recording::MidiExpressionLane>> expressions;
        std::vector<std::vector<recording::MidiControllerLane>> controllers;
        std::vector<recording::MidiLinearTake> takes;
    };
    Placement placements(RowIds &rows, const SnapshotReader &current) {
        Placement placement;
        placement.expressions.resize(tracks.size());
        placement.controllers.resize(tracks.size());
        placement.takes.reserve(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto &track = tracks[i];
            const auto &span = preview[i];
            for (auto &[key, points] : track.expression) {
                const auto &note = track.notes[static_cast<std::size_t>(key.first - 1)];
                std::erase_if(points, [&note](const auto &point) { return point.time_ticks > note.duration; });
                if (!points.empty())
                    placement.expressions[i].push_back({key.first, key.second, points});
            }
            for (auto &[address, points] : track.controllerPoints) {
                // Crossing a Monitor Auto boundary can change the clock for the next control.
                std::stable_sort(points.begin(), points.end(), [](const auto &a, const auto &b) {
                    return a.timeTicks < b.timeTicks;
                });
                placement.controllers[i].push_back({address.first, address.second, points});
            }
            auto name = shownTrackName(current, span.track);
            placement.takes.push_back({span.track, rows.next("clips"), span.start, span.end - span.start,
                                       name.empty() ? std::string("Take") : std::move(name),
                                       track.notes, placement.expressions[i], overdub, placement.controllers[i]});
        }
        return placement;
    }
};

struct ProjectDocument::TakePublication {
    engine::AudioRecordingResult recorded;
    std::unique_ptr<MidiRecording> midi;
    std::string midiProblem;
    std::stop_source stop;
    std::future<recording::StagedTake> staging;
    recording::StagedTake staged;
    std::future<recording::RecordingEdit> publication;
    std::future<std::string> cleanup;
    TakeLanded outcome;
    std::vector<recording::ClipIds> ids;
    bool retried = false;

    ~TakePublication() { stop.request_stop(); }
    void startStaging(const std::filesystem::path &folder) {
        staged = {};
        staging = std::async(std::launch::async, [recorded = recorded, folder, stop = stop.get_token()] {
            return recording::stageTake(recorded, folder, stop);
        });
    }
};

struct ProjectDocument::RecoveryWork {
    std::stop_source stop;
    std::future<recording::RecoveryTake> result;
    ~RecoveryWork() { stop.request_stop(); }
};

struct ProjectDocument::SaveJob {
    using Phase = SaveProgress::Phase;
    std::filesystem::path path;
    SaveKind kind;
    StoreFileCollision collision;
    std::optional<media::FileIdentity> expectedTarget;
    bool keepUndoHistory;
    Phase phase = Phase::waitingForRecording;
    bool wasRecording;
    bool keptTake = false;
    bool switched = false;
    bool readersSwitched = false;
    std::string adoptionWarning, stateWarning;
    std::chrono::steady_clock::time_point recordingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::stop_source stop;
    StoreFileSave file;
    mutable std::mutex progressMutex;
    StoreSaveProgress progress;
    std::future<StoreFileResult> copy;
    std::future<std::string> retirement;

    SaveJob(std::filesystem::path output, SaveKind operation, bool recording, StoreFileCollision collisionMode,
            const media::FileIdentity *confirmedTarget, bool keepUndo)
        : path(std::move(output)), kind(operation), collision(collisionMode),
          expectedTarget(confirmedTarget ? std::optional{*confirmedTarget} : std::nullopt),
          keepUndoHistory(keepUndo), wasRecording(recording) {}
    ~SaveJob() { stop.request_stop(); }
    bool terminal() const noexcept {
        return phase == Phase::succeeded || phase == Phase::cancelled || phase == Phase::failed;
    }
    SaveProgress status() const {
        std::lock_guard lock(progressMutex);
        auto display = phase;
        if (phase == Phase::copying && progress.phase != StoreSavePhase::copying)
            display = progress.phase == StoreSavePhase::relocating ? Phase::relocating : Phase::finishing;
        return {display, progress.pagesCopied, progress.totalPages, switched};
    }
};
struct ProjectDocument::TrackRenderWork {
    using Result = std::variant<std::string, freeze::FreezeEdit, clip_edit::TrackBounce>;
    AppliedSource source;
    std::uint64_t generation = 0;
    std::optional<std::int64_t> head;
    std::string label;
    std::shared_ptr<render::HostDispatch> host = std::make_shared<render::HostDispatch>();
    std::future<Result> completion;
    std::jthread worker;
};
namespace {
void appendSharedStorageWarning(const Store &store, std::string &warning) {
    if (!store.synchronizedLocation())
        return;
    if (!warning.empty())
        warning += ' ';
    warning += store.readOnly() ? "This read-only project is in a synced or network folder."
                               : "This project is in a synced or network folder. Committed edits are kept in its .adi file. "
                                 "Close it before copying it or opening it elsewhere.";
}

bool ordinaryTrack(const engine::TrackNode &track) {
    return track.kind == "audio" || track.kind == "midi" || track.kind == "instrument";
}

long double trackRenderLength(Store &store, const engine::TrackNode &track, double tailSeconds) {
    if (track.frozen) {
        SQLite::Statement cache(store.db(), "SELECT frames,sample_rate FROM frozen_renders WHERE track_id=?");
        cache.bind(1, track.id);
        if (!cache.executeStep() || cache.getColumn(1).getDouble() <= 0)
            throw std::runtime_error("The track has no usable freeze cache.");
        return static_cast<long double>(cache.getColumn(0).getInt64()) / cache.getColumn(1).getDouble();
    }
    edit::TimeMap time(store.db());
    SQLite::Statement ends(store.db(), R"sql(
        WITH RECURSIVE subtree(id) AS (
            SELECT ? UNION SELECT tracks.id FROM tracks JOIN subtree ON tracks.parent_id=subtree.id
        )
        SELECT time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips
            WHERE lane_id IS NULL AND track_id IN subtree
    )sql");
    ends.bind(1, track.id);
    long double end = 0;
    while (ends.executeStep()) {
        const bool absolute = ends.getColumn(0).getInt() == 1;
        const auto position = ends.getColumn(absolute ? 3 : 1).getInt64();
        const auto length = ends.getColumn(absolute ? 4 : 2).getInt64();
        const auto last = edit::addTime(position, length);
        const long double seconds = absolute ? static_cast<long double>(last) / 1000000000.0L : time.seconds(last);
        end = std::max(end, seconds);
    }
    return end > 0 ? end + tailSeconds : 0;
}

std::unique_ptr<library::Index> openSampleDefaults(const std::filesystem::path &file) {
    if (file.empty() || !std::filesystem::exists(file))
        return {};
    std::string error;
    auto index = library::Index::open(file, error, false);
    if (!index)
        throw std::runtime_error("cannot read sample defaults from the Library: " + error);
    return index;
}

// The waveform worker checks out its read-only store for a build. A Save As
// can release the idle reader immediately, and an in-flight build hands it
// back only if the document still uses its file.
WaveformCache::BuildSource peaksFrom(std::shared_ptr<WaveformSource> source) {
    return [source = std::move(source)](std::optional<std::int64_t> mediaId, const std::string &stateHash,
                                       std::stop_token stop) {
        // Create Analysis Files (TM-MD-04), taken when this request begins.
        media::AnalysisFileWrites writes;
        {
            std::lock_guard lock(source->mutex);
            writes = source->analysisFileWrites;
        }
        WaveformSource::Read read(*source);
        auto &reader = read.store;
        if (!reader) {
            StoreError error{};
            reader = Store::open(read.file, error, true);
            if (!reader) {
                media::PeakResult refused;
                refused.problem = media::PeakProblem::database;
                refused.detail = "the project could not be read for its waveforms: " + std::string(toString(error));
                return refused;
            }
        }
        constexpr std::uint32_t fineBinsPerChannel = 65536;
        if (mediaId)
            return media::buildMediaPeaks(*reader, *mediaId, fineBinsPerChannel, stop, nullptr,
                                         &media::defaultAnalysisCache(),
                                         media::PeakCacheResolution::PreferRequested, writes);
        try {
            if (stop.stop_requested()) return media::PeakResult{media::PeakProblem::cancelled, {}, {}};
            const auto bytes = reader->getStateBlob(stateHash);
            if (!bytes || bytes->empty() || bytes->size() > 40u * 1024u * 1024u)
                return media::PeakResult{media::PeakProblem::invalidSource, "The embedded sample state is unavailable.", {}};
            const auto *data = reinterpret_cast<const std::uint8_t *>(bytes->data());
            return device::buildBuiltinSamplePeaks(Payload::from_cbor(data, data + bytes->size()), stop);
        } catch (const std::exception &e) {
            return media::PeakResult{media::PeakProblem::invalidSource, e.what(), {}};
        }
    };
}
} // namespace
ProjectDocument::ProjectDocument(std::unique_ptr<Store> s, std::string name)
    : store_(std::move(s)), name_(std::move(name)), dailyHistory_(*store_),
      dailyHistorySource_(appliedSource(*store_)), dailyHistorySequence_(dailyHistorySource_.sequence()),
      ops_(*store_, view_), views_(*store_),
      sessionState_(*store_), waveformSource_(std::make_shared<WaveformSource>(store_->path())),
      waveforms_(peaksFrom(waveformSource_), [source = waveformSource_](std::int64_t mediaId, const std::string &hash,
                                                                      std::stop_token stop) {
          WaveformSource::Read read(*source);
          auto &reader = read.store;
          if (!reader) {
              StoreError error{};
              reader = Store::open(read.file, error, true);
          }
          return reader ? media::buildMediaTransients(*reader, mediaId, hash, stop) : std::vector<std::uint64_t>{};
      }) {
    // Every refresh the submitter makes carries the session's overridden lanes (N26); the set is
    // the session's own member, alive as long as this document.
    ops_.followOverrides(session_.overriddenLanes());
    const auto lifetime = session_.lifetimeToken();
    analysis_ = std::make_shared<SharedAnalysis>(
        [this, lifetime](engine::ScopeSource source, double seconds) -> std::shared_ptr<const engine::ScopeTap> {
            if (lifetime.expired()) return std::shared_ptr<const engine::ScopeTap>{};
            auto &held = analysisSources_[source];
            if (!held.lease || held.seconds != seconds) {
                // Prepare the new history while the old lease still holds its publication.
                auto replacement = session_.requestScope({source, seconds});
                held.lease = std::move(replacement);
                held.seconds = seconds;
            }
            return held.lease.tap();
        },
        [this, lifetime](engine::ScopeSource source) {
            if (!lifetime.expired()) analysisSources_.erase(source);
        });
    parameterOps_.setNativeAutomation([this](std::int64_t id,const device::ParamDescriptor &descriptor,
                                             engine::ParamEventKind kind,double before,double after,
                                             const std::shared_ptr<const gesture::OwnerToken> &owner) {
        return observeNativeAutomation(id,descriptor,kind,before,after,owner);
    });
}
void ProjectDocument::AutomationPass::append(std::int64_t sample,double value) {
    AutomationPoint point{};point.time=sample;point.value=value;
    point.curve=static_cast<std::uint8_t>(engine::CurveShape::Hold);
    auto &span=spans.back();
    if(!span.empty() && span.back().time==sample)span.back()=point;
    else span.push_back(point);
}
void ProjectDocument::AutomationPass::advance(const engine::Session::DriverClock &clock) {
    if(spans.empty() || !last.playing)return;
    const double held=spans.back().back().value;
    if(clock.revision!=last.revision || !clock.playing) {
        append(last.sample,held);
        if(clock.playing) {
            spans.emplace_back();
            append(clock.sample,held);
        }
    } else if(clock.loopEnd>clock.loopBegin &&
              clock.streamedFrames-last.streamedFrames>clock.sample-last.sample) {
        append(clock.loopEnd,held);
        const auto remaining=clock.streamedFrames-last.streamedFrames-(clock.loopEnd-last.sample);
        if(remaining>=clock.loopEnd-clock.loopBegin) {
            spans.emplace_back();
            append(clock.loopBegin,held);
            append(clock.loopEnd,held);
        }
        spans.emplace_back();
        append(clock.loopBegin,held);
    }
    last=clock;
}
void ProjectDocument::AutomationPass::value(const engine::Session::DriverClock &clock,double value) {
    advance(clock);
    if(!clock.playing)return;
    if(spans.empty() || !last.playing)spans.emplace_back();
    if(!spans.back().empty() && spans.back().back().value!=value)touched=true;
    if(spans.back().empty() && target.defaultValue!=value)touched=true;
    append(clock.sample,value);
    last=clock;
}
void ProjectDocument::AutomationPass::close(const engine::Session::DriverClock &clock) {
    advance(clock);
    if(!spans.empty() && last.playing)append(last.sample,spans.back().back().value);
}

namespace {
std::optional<automation::LaneSpec> mixerLane(Store &store,const OpRequest &request) {
    automation::LaneSpec lane;
    lane.ownerId=request.payload.at("id");
    lane.valueDomain="real";
    const char *sql=nullptr;
    if(request.opType=="mixer.setVolume") {
        lane.paramRef="volume";lane.paramName="Volume";lane.unit="dB";lane.minValue=-150;lane.maxValue=6;
        sql="SELECT volume_db FROM mixer_strip WHERE track_id=?";
    } else if(request.opType=="mixer.setPan") {
        lane.paramRef="pan";lane.paramName="Pan";lane.minValue=-1;lane.maxValue=1;
        sql="SELECT pan FROM mixer_strip WHERE track_id=?";
    } else if(request.opType=="mixer.setPanLeft" || request.opType=="mixer.setPanRight") {
        const bool left=request.opType=="mixer.setPanLeft";
        lane.paramRef=left?"panLeft":"panRight";lane.paramName=left?"Left Pan":"Right Pan";
        lane.defaultValue=left?-1:1;lane.minValue=-1;lane.maxValue=1;
        sql=left?"SELECT pan_left FROM mixer_pan_settings WHERE track_id=?":
                 "SELECT pan_right FROM mixer_pan_settings WHERE track_id=?";
    } else if(request.opType=="routing.setGain") {
        lane.ownerKind="routing";lane.paramRef="gain_db";lane.paramName="Send";lane.unit="dB";
        lane.minValue=-150;lane.maxValue=0;
        sql="SELECT gain_db FROM routing WHERE id=? AND kind='send'";
    } else return std::nullopt;
    SQLite::Statement value(store.db(),sql);value.bind(1,lane.ownerId);
    if(value.executeStep())lane.defaultValue=value.getColumn(0).getDouble();
    if(lane.minValue)lane.minValue=std::min(*lane.minValue,lane.defaultValue);
    if(lane.maxValue)lane.maxValue=std::max(*lane.maxValue,lane.defaultValue);
    return lane;
}
std::int64_t matchingLane(Store &store,const automation::LaneSpec &lane) {
    SQLite::Statement lookup(store.db(),
        "SELECT id FROM automation_lanes WHERE owner_kind=? AND owner_id=? AND param_ref=? AND value_domain=?"
        " ORDER BY enabled DESC,id LIMIT 1");
    lookup.bind(1,lane.ownerKind);lookup.bind(2,lane.ownerId);lookup.bind(3,lane.paramRef);lookup.bind(4,lane.valueDomain);
    return lookup.executeStep()?lookup.getColumn(0).getInt64():0;
}
bool sameLane(const automation::LaneSpec &a,const automation::LaneSpec &b) {
    return a.ownerKind==b.ownerKind && a.ownerId==b.ownerId && a.paramRef==b.paramRef;
}
} // namespace

ProjectDocument::~ProjectDocument() {
    parameterOps_.setNativeAutomation({});
    stopTrackRender();
    for (const auto& item : limiterReductionSources_)
        if (const auto mailbox = item.second.mailbox.lock()) mailbox->revokeObservers();
    analysis_->shutdown();
}
void ProjectDocument::setCreateAnalysisFiles(bool enabled) {
    std::lock_guard lock(waveformSource_->mutex);
    waveformSource_->analysisFileWrites = enabled ? media::AnalysisFileWrites::Enabled
                                                  : media::AnalysisFileWrites::Disabled;
}
std::unique_ptr<ProjectDocument> ProjectDocument::open(const std::filesystem::path &path,
                                                       bool create, engine::DeviceLoader loader,
                                                       std::string &error, const RecordingDefaults *defaults) {
    try {
        auto job = startOpen(path, create, std::move(loader), defaults);
        while (job->phase() != OpenJob::Phase::ready && job->phase() != OpenJob::Phase::failed)
            job->advance();
        error = job->error();
        return job->takeDocument();
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}

ProjectDocument::OpenJob::OpenJob(std::filesystem::path path, bool create, engine::DeviceLoader loader,
                                 const RecordingDefaults *defaults)
    : path_(std::move(path)), create_(create), loader_(std::move(loader)) {
    if (defaults) defaults_ = *defaults;
}

ProjectDocument::OpenJob::~OpenJob() = default;

std::unique_ptr<ProjectDocument::OpenJob> ProjectDocument::startOpen(const std::filesystem::path &path,
                                                                    bool create, engine::DeviceLoader loader,
                                                                    const RecordingDefaults *defaults) {
    return std::unique_ptr<OpenJob>(new OpenJob(path, create, std::move(loader), defaults));
}

ProjectDocument::HistoryReload ProjectDocument::reopenAtStoredHistory(HistoryRestoreTarget target,
                                                                       std::int64_t nowUtc) {
    HistoryReload out;
    if (!deviceStopped_.load(std::memory_order_acquire)) {
        out.error = "Stop the audio device before restoring a stored version.";
        return out;
    }
    if (recording() || landingTake()) {
        out.error = "Wait for the take to finish before restoring a version.";
        return out;
    }
    if (!settleParameters()) {
        out.error = error_;
        return out;
    }
    History history(*store_);
    const auto priorHead = history.head();
    constexpr auto prepared = History::StoredRestore::SessionReloadPrepared;
    const auto *snapshot = std::get_if<HistorySnapshotTarget>(&target);
    const auto result = snapshot ? history.revertToSnapshot(snapshot->id, nowUtc, prepared)
                                 : history.moveTo(std::get<HistoryHeadTarget>(target).seq, nowUtc, prepared);
    if (!result.ok) {
        out.error = result.error;
        return out;
    }
    if (!result.sessionReloadRequired) {
        if (result.opsApplied != 0) {
            view_.refresh(*store_, session_.overriddenLanes());
            if (!synchronise())
                out.error = error_;
        }
        return out;
    }
    out.replayBaseChanged = true;
    updateDailyHistory();
    const auto opening = store_->openingHistory();
    const auto reopen = [&](std::string& error) {
        auto document = open(store_->path(), false, renderLoader_, error);
        if (document && opening)
            document->store_->retainOpeningHistory(opening->seq);
        return document;
    };
    out.document = reopen(out.error);
    if (out.document)
        return out;
    const auto rollback = history.moveTo(priorHead, nowUtc, prepared);
    if (!rollback.ok) {
        out.error += " The previous version could not be restored: " + rollback.error;
        return out;
    }
    updateDailyHistory();
    std::string reloadError;
    out.document = reopen(reloadError);
    if (out.document)
        out.error += " The previous version was reopened.";
    else
        out.error += " Reopen the project to return to the previous version: " + reloadError;
    return out;
}

std::unique_ptr<ProjectDocument> ProjectDocument::OpenJob::takeDocument() {
    return phase_ == Phase::ready ? std::move(document_) : nullptr;
}

void ProjectDocument::OpenJob::fail(std::string reason) {
    error_ = std::move(reason);
    phase_ = Phase::failed;
    document_.reset();
}

void ProjectDocument::OpenJob::prepare() {
    StoreError result{};
    // New starts from the store's default Set and history root.
    auto store = !create_ ? Store::open(path_, result)
                 : defaults_ ? Store::createNewProject(path_, result, defaults_->quantize, defaults_->keepMonitoringLatency)
                             : Store::createNewProject(path_, result);
    if (!store) {
        fail("Cannot open project: " + std::string(toString(result)));
        return;
    }
    const auto stem = path_.stem().u8string();
    document_.reset(new ProjectDocument(std::move(store),
                                       std::string(reinterpret_cast<const char *>(stem.data()), stem.size())));
    auto &doc = *document_;
    doc.view_.refresh(*doc.store_);
    // Legacy projects can keep their initial New project history floor.
    doc.ops_.protectRoot(kNewProjectLabel);
    if (!doc.view_.storeCurrent(*doc.store_)) {
        fail("The project changed while opening. Open it again to load the current version.");
        return;
    }
    engine::SessionSpec spec;
    spec.sampleRate = doc.view_.current()->sampleRate;
    doc.renderLoader_ = loader_;
    if (!doc.session_.beginLoad(*doc.store_, std::move(loader_), spec)) {
        fail(doc.session_.error());
        return;
    }
    totalDevices_ = doc.session_.loadProgress().totalDevices;
    phase_ = totalDevices_ ? Phase::loadingDevices : Phase::buildingGraph;
}

void ProjectDocument::OpenJob::advance() {
    try {
        if (phase_ == Phase::preparing) {
            prepare();
            return;
        }
        if (phase_ == Phase::ready || phase_ == Phase::failed)
            return;
        auto &doc = *document_;
        if (phase_ == Phase::loadingDevices) {
            if (!doc.session_.loadNextDevice(*doc.store_)) {
                fail(doc.session_.error());
                return;
            }
            completedDevices_ = doc.session_.loadProgress().completedDevices;
            if (completedDevices_ == totalDevices_)
                phase_ = Phase::buildingGraph;
            return;
        }
        if (!doc.view_.storeCurrent(*doc.store_)) {
            fail("The project changed while opening. Open it again to load the current version.");
            return;
        }
        if (!doc.session_.finishLoad(*doc.store_)) {
            fail(doc.session_.error());
            return;
        }
        if (!doc.view_.storeCurrent(*doc.store_)) {
            fail("The project changed while opening. Open it again to load the current version.");
            return;
        }
        doc.realisedGeneration_ = doc.view_.generation();
        doc.parameterOps_.attachSession(doc.session_);
        doc.feed_.publish(doc.session_, doc.realisedGeneration_);
        doc.persistReportedLatencies();
        appendSharedStorageWarning(*doc.store_, doc.warning_);
        phase_ = Phase::ready;
    } catch (const std::exception &error) {
        fail(error.what());
    }
}
namespace {
void appendWarning(std::string &message, const std::string &addition) {
    if (addition.empty()) return;
    if (!message.empty()) message += ' ';
    message += addition;
}
} // namespace

History::SnapshotResult ProjectDocument::takeSnapshot(const std::string &name,
                                                      std::int64_t nowUtc, int utcOffsetMinutes) {
    if (store_->readOnly()) {
        History::SnapshotResult refused;
        refused.error = "Project is read-only";
        return refused;
    }
    if (!capturePluginState()) {
        History::SnapshotResult refused;
        refused.error = error_;
        return refused;
    }
    History history(*store_);
    return history.takeSnapshot(name, nowUtc, utcOffsetMinutes);
}
std::string ProjectDocument::snapshotName(std::int64_t nowUtc, int utcOffsetMinutes) const {
    return History::defaultSnapshotName(name_, nowUtc, utcOffsetMinutes);
}
bool ProjectDocument::saveAs(const std::filesystem::path &path, bool firstSave, StoreFileCollision collision,
                             const media::FileIdentity *expectedTarget) {
    warning_.clear();
    // The unconfirmed path checks before stopping a recording. The store also checks when publishing.
    std::error_code ignored;
    if (collision == StoreFileCollision::Fail &&
        (std::filesystem::exists(path, ignored) ||
         std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored)))) {
        error_ = "Save As: " + media::pathUtf8(path.filename()) +
                 " already exists. Confirm replacement or choose another name.";
        return false;
    }
    // A take recording or landing goes into the file it was recorded in, before the file switches
    // (PD06-SAVEAS-GUARD): Record stops, the take lands as its one undo step, then Save As goes on.
    const bool wasRecording = recording_;
    if (!landTakeHere()) {
        error_ = "The take is still being saved; try Save As again in a moment.";
        return false;
    }
    // No History, OpJournal or Statement of ours is alive here: every one is made per call, and
    // the members hold the Store itself, which keeps its identity across the switch.
    if (trackRender_) {
        stopTrackRender();
        trackRenderStatus_ = "Track render cancelled for Save As.";
    }
    if (!capturePluginState())
        return false;
    StoreFileSave file;
    const auto oldPath = store_->path();
    const auto kind = firstSave ? StoreSaveKind::firstSave : StoreSaveKind::saveAs;
    auto result = store_->prepareFileSave(path, kind, file, collision, expectedTarget);
    if (result.ok) result = file.write();
    if (result.ok) result = store_->adoptFileSave(file);
    if (!result.ok) {
        error_ = "Save As failed: " + result.error;
        return false;
    }
    const bool readersSwitched = followSaveAs(result, wasRecording && landed_ && landed_->ok);
    waveformSource_->waitForReader(oldPath);
    appendWarning(warning_, file.closeReplacedProject());
    appendWarning(warning_, pluginStateWarning_);
    if (readersSwitched && firstSave)
        store_->finishUntitledSave();
    return true;
}
bool ProjectDocument::followSaveAs(const StoreFileResult &result, bool keptTake) {
    warning_ = result.warning;
    // Said only of a take that landed; one that was not kept says so itself (`takeLanded`).
    if (keptTake)
        warning_ = std::string("Recording stopped, its take kept, before Save As.") + (warning_.empty() ? "" : " ") + warning_;
    {
        std::lock_guard lock(waveformSource_->mutex);
        waveformSource_->idleReader.reset();
        waveformSource_->path = store_->path();
    }
    const auto stem = store_->path().stem().u8string();
    name_.assign(reinterpret_cast<const char *>(stem.data()), stem.size());
    // The same rows, read again from the new file: the generation moves, the session follows
    // through synchronise(), and every window follows at its next frame. The store has switched
    // by now, so false would lie ("nothing moved"): a session that fails to re-read is a warning
    // on a save that stands, and the next synchronise() tries again.
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    std::string refreshError;
    try {
        view_.refresh(*store_, session_.overriddenLanes());
        if (!synchronise()) refreshError = error_;
    } catch (const std::exception &error) {
        refreshError = error.what();
    }
    if (!refreshError.empty()) {
        if (!warning_.empty())
            warning_ += ' ';
        warning_ += "Saved, but the session did not re-read the file: " + refreshError;
    }
    appendSharedStorageWarning(*store_, warning_);
    error_.clear();
    return refreshError.empty();
}
namespace {
// The store never replaces a file; the Status Bar says so in the window's words, with the name.
bool refuseExisting(const std::filesystem::path &path, const char *what, std::string &error) {
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored))) {
        error = std::string(what) + ": " + media::pathUtf8(path.filename()) +
                " already exists; ADI never replaces a file. Choose another name.";
        return true;
    }
    return false;
}
} // namespace
bool ProjectDocument::saveCopy(const std::filesystem::path &path, bool keepUndoHistory) {
    warning_.clear();
    if (refuseExisting(path, "Save a Copy", error_))
        return false;
    if (!capturePluginState())
        return false;
    const auto result = store_->saveCopy(path, StoreFileCollision::Fail,
        keepUndoHistory ? StoreCopyPolicy::shareWithUndo : StoreCopyPolicy::share);
    if (!result.ok) {
        error_ = "Save a Copy failed: " + result.error;
        return false;
    }
    warning_ = result.warning;
    appendWarning(warning_, pluginStateWarning_);
    error_.clear();
    return true;
}

bool ProjectDocument::startSave(const std::filesystem::path &path, SaveKind kind, StoreFileCollision collision,
                                const media::FileIdentity *expectedTarget, bool keepUndoHistory) {
    if (saveJob_) return false;
    warning_.clear();
    const bool switching = kind != SaveKind::copy;
    if (collision == StoreFileCollision::Fail && refuseExisting(path, switching ? "Save As" : "Save a Copy", error_))
        return false;
    error_.clear();
    saveJob_ = std::make_unique<SaveJob>(path, kind, switching && recording_, collision, expectedTarget, keepUndoHistory);
    if (saveJob_->wasRecording) stopRecording();
    advanceSave();
    return true;
}

std::optional<ProjectDocument::SaveProgress> ProjectDocument::saveProgress() const {
    return saveJob_ ? std::optional{saveJob_->status()} : std::nullopt;
}

void ProjectDocument::cancelSave() {
    if (saveJob_ && !saveJob_->terminal()) saveJob_->stop.request_stop();
}

void ProjectDocument::clearSave() {
    if (saveJob_ && saveJob_->terminal()) saveJob_.reset();
}

void ProjectDocument::advanceSave() {
    if (!saveJob_ || saveJob_->terminal()) return;
    using Phase = SaveProgress::Phase;
    using namespace std::chrono_literals;
    auto &job = *saveJob_;
    const bool switching = job.kind != SaveKind::copy;
    const auto fail = [&](std::string why, std::string warning = {}) {
        error_ = std::string(switching ? "Save As failed: " : "Save a Copy failed: ") + why;
        warning_ = std::move(warning);
        job.phase = Phase::failed;
    };
    const auto finish = [&] {
        appendWarning(warning_, job.stateWarning);
        error_.clear();
        job.phase = Phase::succeeded;
    };
    const auto finishRetirement = [&](std::string warning) {
        if (job.kind == SaveKind::firstSave && job.readersSwitched)
            store_->finishUntitledSave();
        warning_ = std::move(job.adoptionWarning);
        if (!warning.empty()) {
            if (!warning_.empty()) warning_ += ' ';
            warning_ += warning;
        }
        finish();
    };
    try {
        // Like synchronous Save As, detach the render from this Store before
        // switching files. The next ticks service its host calls and cancellation.
        if (switching && trackRender_ && !job.stop.stop_requested() &&
            (job.phase == Phase::waitingForRecording || job.phase == Phase::copying)) {
            trackRender_->worker.request_stop();
            return;
        }
        if (job.phase == Phase::waitingForRecording) {
            if (job.stop.stop_requested()) {
                job.phase = Phase::cancelled;
                return;
            }
            if (switching && (recording_ || landing_)) {
                if (!takePublication_ && std::chrono::steady_clock::now() >= job.recordingDeadline)
                    fail("The take is still being saved. Try Save As again in a moment.");
                return;
            }
            job.keptTake = job.wasRecording && landed_ && landed_->ok;
            if (!capturePluginState()) {
                fail(error_);
                return;
            }
            job.stateWarning = pluginStateWarning_;
            const auto kind = job.kind == SaveKind::firstSave ? StoreSaveKind::firstSave
                            : switching ? StoreSaveKind::saveAs : StoreSaveKind::copy;
            const auto prepared = store_->prepareFileSave(job.path, kind, job.file, job.collision,
                                                          job.expectedTarget ? &*job.expectedTarget : nullptr,
                                                          job.keepUndoHistory ? StoreCopyPolicy::shareWithUndo
                                                                              : StoreCopyPolicy::share);
            if (!prepared.ok) {
                fail(prepared.error, prepared.warning);
                return;
            }
            job.copy = std::async(std::launch::async, [job = &job] {
                return job->file.write(job->stop.get_token(), [job](StoreSaveProgress progress) {
                    std::lock_guard lock(job->progressMutex);
                    job->progress = progress;
                });
            });
            job.phase = Phase::copying;
            return;
        }
        if (job.phase == Phase::finishing) {
            if (job.retirement.wait_for(0ms) == std::future_status::ready)
                finishRetirement(job.retirement.get());
            return;
        }
        if (job.copy.wait_for(0ms) != std::future_status::ready) return;
        const auto result = job.copy.get();
        if (!result.ok) {
            if (job.file.cancelled()) {
                warning_.clear();
                error_.clear();
                job.phase = Phase::cancelled;
            } else {
                fail(result.error, result.warning);
            }
            return;
        }
        if (!switching) {
            warning_ = result.warning;
            finish();
            return;
        }
        if (job.stop.stop_requested()) {
            warning_ = "A copy was saved. The original project remains active.";
            error_.clear();
            job.phase = Phase::cancelled;
            return;
        }
        if (recording_ || landing_) {
            fail("A take is recording or still being saved. A copy was saved and the original project remains active.",
                 result.warning);
            return;
        }
        auto oldPath = store_->path();
        const auto adopted = store_->adoptFileSave(job.file);
        if (!adopted.ok) {
            fail(adopted.error, adopted.warning);
            return;
        }
        job.switched = true;
        job.phase = Phase::finishing;
        job.readersSwitched = followSaveAs(adopted, job.keptTake);
        job.adoptionWarning = std::move(warning_);
        const auto retire = [file = &job.file, source = waveformSource_, oldPath = std::move(oldPath)] {
            source->waitForReader(oldPath);
            return file->closeReplacedProject();
        };
        try {
            job.retirement = std::async(std::launch::async, retire);
        } catch (const std::system_error &) {
            finishRetirement(retire());
        }
    } catch (const std::exception &error) {
        if (job.phase == Phase::finishing) {
            finishRetirement(std::string("Saved, but could not close the old project: ") + error.what());
        } else {
            fail(error.what());
        }
    }
}

bool ProjectDocument::collectAllAndSave() {
    warning_.clear();
    if (!capturePluginState())
        return false;
    const auto result = store_->collectAllAndSave();
    warning_ = result.warning;
    if (!result.ok) {
        error_ = "Collect All and Save failed: " + result.error;
        if (!warning_.empty())error_ += "; " + warning_;
        return false;
    }
    // The media rows point at the copies now: the view and the session read them again.
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    view_.refresh(*store_, session_.overriddenLanes());
    if (!synchronise()) {
        if (!warning_.empty())
            warning_ += ' ';
        warning_ += "Collected, but the session did not re-read the project: " + error_;
    }
    appendWarning(warning_, pluginStateWarning_);
    error_.clear();
    return true;
}
bool ProjectDocument::prepareCollectAndExport(const std::filesystem::path &zip, ExportJob &out, bool keepUndoHistory) {
    if (refuseExisting(zip, "Collect and Export", error_))
        return false;
    if (!capturePluginState())
        return false;
    out.project = media::projectFolder(*store_) / store_->path().filename();
    out.zip = zip;
    out.stateWarning = pluginStateWarning_;
    out.keepUndoHistory = keepUndoHistory;
    error_.clear();
    return true;
}
StoreFileResult ProjectDocument::runCollectAndExport(const ExportJob &job) {
    auto result = runCollectAndExport(job, {});
    return {result.ok, std::move(result.error), std::move(result.warning)};
}
media::FileResult ProjectDocument::runCollectAndExport(const ExportJob &job,
                                                      const media::ArchiveControl &control) {
    try {
        auto result = media::collectExport(job.project, job.zip, control, job.keepUndoHistory);
        appendWarning(result.warning, job.stateWarning);
        return result; // typed cancellation kept
    } catch (const std::exception &e) {
        return {false, e.what(), {}};
    } catch (...) {
        return {false, "collection export worker failed", {}};
    }
}
void ProjectDocument::refreshSessionSources() {
    refreshLimiterReductionSources();
    realisedGeneration_ = view_.generation();
    // A retained retiree can share its row ID with a newer instance.
    // Detach by ID only while the retiree still owns the session's binding.
    for (std::size_t i = 0; i < session_.entryCount(); ++i) {
        const auto &entry = session_.entryAt(i);
        if (entry.retired && session_.entryFor(entry.deviceId) == &entry)
            parameterOps_.detach(entry.deviceId);
    }
    parameterOps_.attachSession(session_);
    analysis_->refreshSources();
}
bool ProjectDocument::synchronise() {
    if (realisedGeneration_ != view_.generation()) {
        const auto &edit = view_.localEdit();
        const auto local = edit && edit->fromGeneration == realisedGeneration_ ? session_.refreshLocalEdit(*store_, view_) : std::nullopt;
        const auto keyed = local ? local : session_.refreshDeviceKeys(*store_, view_.committedImage());
        const bool refreshed = keyed ? *keyed : session_.refresh(*store_);
        if (!refreshed) {
            refreshLimiterReductionSources();
            error_ = session_.error();
            return false;
        }
        refreshSessionSources();
        error_.clear();
    }
    // The session re-read the rows after the submitter's refresh, so an override it found there,
    // or one a gesture set since the last frame, reaches the snapshot now.
    followOverrides();
    syncMonitorInputs(); // a track added or removed is fed or let go (N65)
    updateDailyHistory();
    return true;
}
void ProjectDocument::updateDailyHistory(bool closing) {
    if (!dailyHistoryClock_ || store_->readOnly())
        return;
    const auto source = appliedSource(*store_);
    const auto sequence = source.sequence();
    if (!closing && source.sameConnection(dailyHistorySource_) && sequence == dailyHistorySequence_)
        return;
    auto* database = store_->db().getHandle();
    if (!sqlite3_get_autocommit(database))
        return;
    const auto [nowUtc, utcOffsetMinutes] = dailyHistoryClock_();
    const bool viewWasCurrent = view_.storeCurrent(*store_);
    const auto before = sqlite3_total_changes64(database);
    dailyHistory_.poll(nowUtc, utcOffsetMinutes);
    dailyHistorySource_ = source;
    dailyHistorySequence_ = sequence;
    const auto written = sqlite3_total_changes64(database) - before;
    if (viewWasCurrent && written > 0)
        view_.acknowledgeAdvisoryWrite(*store_, written);
}
void ProjectDocument::setExternalAudioInputs(std::vector<ExternalAudioInput> inputs) {
    externalAudioInputs_ = std::move(inputs);
    syncMonitorInputs();
}
std::span<const ExternalAudioInput> ProjectDocument::externalAudioInputs() const noexcept {
    if (externalAudioInputs_)
        return *externalAudioInputs_;
    return {};
}
std::size_t ProjectDocument::wakeMissingPlugins() {
    if (!store_)
        return 0;
    const std::size_t woke = session_.retryPlaceholders(*store_);
    if (woke == 0)
        return 0;
    refreshLimiterReductionSources();
    parameterOps_.attachSession(session_); // the woken plug-in's own edits become undo steps
    analysis_->refreshSources();
    return woke;
}
void ProjectDocument::setInputChannels(std::int32_t inputs) {
    inputChannels_ = std::max(0, inputs);
    syncMonitorInputs();
}
std::span<const std::uint32_t> ProjectDocument::channelsForInput(const std::optional<std::string> &ref,
                                                                std::int32_t legacyCount) const noexcept {
    if (externalAudioInputs_) {
        if (const auto *input = resolveExternalAudioInput(*externalAudioInputs_, ref))
            return input->channels;
        return {};
    }
    if (ref || legacyCount <= 0)
        return {};
    static constexpr std::array<std::uint32_t, 2> contiguous{0, 1};
    return std::span{contiguous}.first(legacyCount >= 2 ? 2u : 1u);
}
void ProjectDocument::syncMonitorInputs() {
    std::vector<std::pair<std::int64_t, std::vector<std::uint32_t>>> wanted;
    for (const auto &t : session_.model().tracks) {
        if (t.kind != "audio")
            continue;
        const auto channels = channelsForInput(t.inputRef, inputChannels_);
        if (!channels.empty())
            wanted.emplace_back(t.id, std::vector<std::uint32_t>(channels.begin(), channels.end()));
    }
    if (wanted == monitorInputs_)
        return;
    std::vector<engine::RecordingTarget> targets;
    for (const auto &[track, channels] : wanted)
        targets.push_back({track, channels});
    // A refusal keeps the inputs the session had; the next change tries again.
    if (session_.setAudioMonitorInputs(targets))
        monitorInputs_ = std::move(wanted);
}
void ProjectDocument::followOverrides() {
    const auto snapshot = view_.current();
    if (!snapshot)
        return;
    // A held preview publishes its own temporarily suspended lanes. Only the
    // committed view follows the Session's durable override set.
    if (mixerDrag_ && snapshot != view_.committed())
        return;
    const auto &lanes = session_.overriddenLanes();
    bool agree = snapshot->automationOverridden == !lanes.empty();
    for (const auto &lane : snapshot->automationLanes)
        agree = agree && lane->overridden == lanes.contains(lane->id);
    if (agree)
        return;
    // The same rows under a new generation: when the session had realised the view, it has
    // nothing to re-read, and every window takes the new snapshot at its next frame. A view the
    // session has not caught up with (a commit that skipped `synchronise()`) stays unrealised, so
    // the next `synchronise()` re-reads it.
    const bool realised = realisedGeneration_ == view_.generation();
    view_.refresh(*store_, lanes);
    if (realised)
        realisedGeneration_ = view_.generation();
}
bool ProjectDocument::reenableAutomation() {
    const bool ok = session_.reenableAutomation();
    const std::string refusal = ok ? std::string{} : session_.error();
    // The session reads nothing overridden now, whether or not its rebuild went through: the
    // snapshot unlights the switch and the strip's marks go back from overridden to automated in
    // the same call, so the window never shows a set the session no longer holds.
    followOverrides();
    feed_.publish(session_, realisedGeneration_);
    error_ = refusal;
    return ok;
}
bool ProjectDocument::setDelayCompensation(bool on) {
    if (session_.setDelayCompensation(on)) {
        error_.clear();
        return true;
    }
    error_ = session_.error().empty() ? std::string("The session could not change Delay Compensation.") : session_.error();
    return false;
}
bool ProjectDocument::reenableAutomation(std::int64_t laneId) {
    const bool ok = session_.reenableAutomation(laneId);
    const std::string refusal =
        ok ? std::string{} : (session_.error().empty() ? std::string("That automation is not overridden.") : session_.error());
    followOverrides();
    feed_.publish(session_, realisedGeneration_);
    error_ = refusal;
    return ok;
}
void ProjectDocument::setAutomationArmed(bool armed) {
    if (!armed) closeAutomationWrites();
    session_.setAutomationWriteState(armed, recording_);
}
std::int64_t ProjectDocument::captureNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool ProjectDocument::commitCaptured(std::vector<OpRequest> &requests) {
    if (requests.empty())
        return true;
    if (!parameterOps_.writeBlobs(*store_, error_))
        return false;
    const auto result = ops_.submit(requests);
    if (!result.ok) {
        error_ = result.error;
        return false;
    }
    return synchronise();
}
bool ProjectDocument::drainParameterEdits(int gestureLimit, std::int64_t quietDelayMs) {
    std::vector<OpRequest> requests;
    for (int round = 0; round < gestureLimit; ++round) {
        requests.clear();
        if (parameterOps_.drain(captureNowMs() + quietDelayMs, requests) == 0)
            return true;
        if (!commitCaptured(requests))
            return false;
    }
    return true;
}
bool ProjectDocument::settleParameters() {
    // ParamOps closes an unbracketed value after 150 ms with no input.
    constexpr std::int64_t kQuietMs = 150;
    return drainParameterEdits(256, kQuietMs);
}
void ProjectDocument::pollParameterEdits() {
    // Configure can follow several completed native gestures from one window update.
    drainParameterEdits(256);
    feed_.publish(session_, realisedGeneration_);
}
bool ProjectDocument::capturePluginState(std::optional<std::int64_t> deviceId) {
    return capturePluginStateImpl(deviceId, !deviceId);
}
bool ProjectDocument::requirePluginState() {
    return capturePluginStateImpl({}, false);
}
bool ProjectDocument::capturePluginStateImpl(std::optional<std::int64_t> deviceId, bool bestEffort) {
    pluginStateWarning_.clear();
    if (store_->readOnly())
        return true;
    if (!synchronise())
        return false;
    if (deviceId) {
        const auto *entry = session_.entryFor(*deviceId);
        if (!entry || entry->placeholder || entry->parked || entry->retired)
            return true;
        // Native mix controls already write their edits through the journal.
        // Saving their derived stream here would put a second Undo step before
        // Copy or Remove. A document-wide save still captures the full state.
        if (mix::functionForReference(entry->resolvedFormat, entry->resolvedUid))
            return true;
    }
    if (!deviceId && mixerDrag_ && !endMixerDrag())
        return false;
    // Observe hosted touches already queued for the owner. A recorded touch
    // must close as an envelope edit before ParamOps finishes ordinary knobs.
    if (recording_ && session_.automationArmed() && !drainParameterEdits(256))
        return false;
    if (!closeAutomationWrites(deviceId))
        return false;
    const auto now = captureNowMs();
    // State capture keeps a global pending FIFO. A targeted request can
    // refuse an unrelated open touch, but must not finish it as a static edit.
    using Gestures = engine::ParamOps::SnapshotGestures;
    const auto finish = deviceId && (!parameterWrites_.empty() || !nativeWrites_.empty())
                            ? Gestures::Completed : Gestures::FinishOpen;
    using Policy = engine::ParamOps::SnapshotPolicy;
    const auto policy = bestEffort ? Policy::UseStoredOnCaptureFailure : Policy::RequireAll;
    const auto captured = deviceId ? parameterOps_.snapshotDeviceState(*deviceId, now, finish)
                                   : parameterOps_.snapshotAll(now, finish, policy);
    // Several independently committed devices may precede a refusal. Publish those writes too.
    if (captured.journalOps != 0) {
        view_.refresh(*store_, session_.overriddenLanes());
        if (!synchronise() && captured.ok())
            return false;
    }
    if (!surfaceGestures_.empty()) {
        const auto pending = parameterOps_.pendingGestures();
        std::erase_if(surfaceGestures_, [&](const auto &item) {
            return std::none_of(pending.begin(), pending.end(), [&](const auto &edit) {
                return edit.owner && edit.owner->sameOwner(*item.second.owner);
            });
        });
    }
    if (bestEffort && (captured.ok() || !captured.staleDevices.empty()) &&
        !markStalePluginStates(captured.staleDevices, captured.ok()))
        return false;
    if (!captured.ok()) {
        const auto *entry = captured.failedDevice ? session_.entryFor(*captured.failedDevice) : nullptr;
        error_ = "Could not save plug-in state";
        if (entry)
            error_ += " for " + entry->name;
        error_ += captured.status.message.empty() ? "." : ": " + captured.status.message;
        return false;
    }
    refreshReportedLatenciesForSave();
    error_.clear();
    return true;
}
bool ProjectDocument::markStalePluginStates(std::span<const std::int64_t> stale, bool complete) {
    try {
        auto &db = store_->db();
        std::string previousText;
        {
            SQLite::Statement read(db, "SELECT value FROM main.session_state WHERE key='devices.stale_state'");
            if (read.executeStep()) previousText = read.getColumn(0).getString();
        }
        const auto previous = Payload::parse(previousText, nullptr, false);
        auto report = Payload::object();
        std::string names;
        for (std::size_t i = 0; i < session_.entryCount(); ++i) {
            const auto &entry = session_.entryAt(i);
            if (entry.retired) continue;
            const auto key = std::to_string(entry.deviceId);
            const bool failed = std::find(stale.begin(), stale.end(), entry.deviceId) != stale.end();
            const Payload identity{{"name", entry.name}, {"format", entry.resolvedFormat}, {"uid", entry.resolvedUid}};
            const auto saved = previous.find(key);
            const bool hadStaleState = saved != previous.end() && saved->contains("format") && saved->contains("uid") &&
                                       saved->at("format") == entry.resolvedFormat && saved->at("uid") == entry.resolvedUid;
            // Parked and missing devices supplied no fresh state. Keep a prior
            // flag for the same device until a subsequent live capture succeeds.
            if (!failed && !(hadStaleState && (!complete || entry.parked || entry.placeholder))) continue;
            report[key] = identity;
            if (!names.empty()) names += ", ";
            names += entry.name.empty() ? "Device " + key : entry.name;
        }
        if (!names.empty())
            pluginStateWarning_ = "Could not capture plug-in state for " + names + ". Using the last saved state (stale).";
        const bool viewWasCurrent = view_.storeCurrent(*store_);
        if (report.empty()) {
            if (previousText.empty()) return true;
            db.exec("DELETE FROM main.session_state WHERE key='devices.stale_state'");
        } else {
            const auto reportText = report.dump();
            if (reportText == previousText) return true;
            SQLite::Statement write(db, "INSERT INTO main.session_state(key,value) VALUES('devices.stale_state',?) "
                                        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
            write.bind(1, reportText);
            write.exec();
        }
        if (viewWasCurrent) view_.acknowledgeAdvisoryWrite(*store_);
        return true;
    } catch (const std::exception &error) {
        error_ = std::string("Could not mark stored plug-in state stale. ") + error.what();
        return false;
    }
}
void ProjectDocument::tick(std::int64_t milliseconds) {
    const bool synced = synchronise();
    session_.tick(milliseconds);
    advanceAutomationWrites();
    if (synced && (!saveJob_ || saveJob_->terminal()))
        store_->finishUntitledSave();
    if (!limiterReductionSources_.empty() &&
        limiterReductionPublication_ != session_.graph().stats().published)
        refreshLimiterReductionSources();
    // A take, once the driver has moved past it and its worker has finished: a stopped one, or one that
    // ended on its own while recording (a locate, an overflow, a device change), which lands at once and
    // turns Record off. A healthy capture returns nothing. With the device stopped, nothing moves the
    // callback on, so the take is settled as the driver's is.
    pollRecording(deviceStopped_.load(std::memory_order_acquire));
    drainParameterEdits(1);
    finishNativeAutomation();
    feed_.publish(session_, realisedGeneration_);
    persistReportedLatencies();
    advanceSave();
    pollTrackRender();
}
void ProjectDocument::refreshReportedLatenciesForSave() {
    if (realisedGeneration_ != view_.generation()) return;
    feed_.publish(session_, realisedGeneration_);
    persistReportedLatencies();
}
void ProjectDocument::persistReportedLatencies() {
    if (store_->readOnly() || realisedGeneration_ != view_.generation()) return;
    const auto current = feed_.current();
    const auto sequence = session_.realisedAppliedSequence();
    const auto rate = session_.spec().sampleRate;
    if (reportedLatencyFeed_ && sequence == reportedLatencySequence_ && rate == reportedLatencyRate_ &&
        (current == reportedLatencyFeed_ ||
         std::equal(current->begin(), current->end(), reportedLatencyFeed_->begin(), reportedLatencyFeed_->end(),
                    [](const DevicePanelData &a, const DevicePanelData &b) {
                        return a.id == b.id && a.instance == b.instance && a.available == b.available &&
                               a.latency == b.latency;
                    }))) {
        reportedLatencyFeed_ = current;
        return;
    }
    const auto source = appliedSource(*store_);
    if (source.sequence() != sequence || !session_.presentationReady(source, sequence) ||
        !sqlite3_get_autocommit(store_->db().getHandle())) return;
    // An open edit can preview new latency before those settings enter history.
    const auto guard = gesture::detail::beginNavigation(*store_);
    if (!guard.status.ok()) return;
    try {
        const auto head = History(*store_).head();
        Payload report{{"version", 1}, {"sample_rate", rate},
                       {"history_head", head ? Payload(*head) : Payload(nullptr)}, {"devices", Payload::array()}};
        auto &devices = report["devices"];
        for (const auto &device : *current) {
            if (!device.available) continue;
            const auto *entry = session_.entryFor(device.id);
            if (!entry || !entry->resolvedPluginRef) continue;
            devices.push_back({{"device_id", device.id}, {"plugin_ref_id", *entry->resolvedPluginRef},
                               {"format", entry->resolvedFormat}, {"uid", entry->resolvedUid},
                               {"latency_samples", device.latency}});
        }
        SQLite::Statement write(store_->db(), "INSERT INTO main.session_state(key,value) "
            "VALUES('devices.reported_latency',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value "
            "WHERE session_state.value<>excluded.value");
        write.bind(1, report.dump());
        const bool viewWasCurrent = view_.storeCurrent(*store_);
        write.exec();
        if (viewWasCurrent) view_.acknowledgeAdvisoryWrite(*store_);
        reportedLatencyFeed_ = current;
        reportedLatencySequence_ = sequence;
        reportedLatencyRate_ = rate;
    } catch (const SQLite::Exception &e) {
        warning_ = std::string("Device latency report could not be saved: ") + e.what();
    }
}
void ProjectDocument::setParameterDemand(const void *consumer, const std::vector<std::int64_t> &devices) {
    if (feed_.setDemand(consumer, devices)) feed_.publish(session_, realisedGeneration_);
}
std::shared_ptr<const ParameterPublication> ProjectDocument::automationParameters(std::int64_t track) {
    return feed_.describeForAutomation(session_, track);
}
device::DeviceInstance* ProjectDocument::limiterReductionTarget(
    std::int64_t id, const engine::DeviceRuntimeToken& expected) {
    const auto* entry = session_.entryFor(id);
    if (!entry || entry->retired || entry->parked || entry->placeholder ||
        entry->resolvedUid != "org.adidaw.mix.limiter") return nullptr;
    auto* instance = session_.resolveDevice(expected);
    if (!instance || instance != session_.instanceFor(id) || !instance->loaded() ||
        session_.devices().nodeAt(entry->hostIndex).bypassed()) return nullptr;
    return instance;
}
void ProjectDocument::refreshLimiterReductionSources() {
    limiterReductionPublication_ = session_.graph().stats().published;
    std::erase_if(limiterReductionSources_, [this](const auto& item) {
        const auto& [id, source] = item;
        const auto mailbox = source.mailbox.lock();
        if (!mailbox || !mailbox->observed()) return true;
        if (limiterReductionTarget(id, source.expected)) return false;
        mailbox->revokeObservers();
        return true;
    });
}
mix::limiter::ReductionMailbox::Lease ProjectDocument::observeLimiterReduction(
    std::int64_t id, const engine::DeviceRuntimeToken& expected) {
    refreshLimiterReductionSources();
    auto* target = limiterReductionTarget(id, expected);
    if (!target) return {};
    auto mailbox = device::limiterReductionSource(*target);
    auto lease = mix::limiter::ReductionMailbox::observe(mailbox);
    if (lease) limiterReductionSources_.insert_or_assign(id, ReductionSource{expected, mailbox});
    return lease;
}
bool ProjectDocument::autoGainStage(bool lufs) {
    engine::GainStageOptions options;
    options.measure =
        lufs ? engine::GainStageMeasure::IntegratedLufs : engine::GainStageMeasure::GatedRms;
    const auto result = session_.autoGainStage(*store_, options);
    error_ = result.commit.error;
    if (!result.commit.ok)
        return false;
    // With the session's overridden lanes, so the marks stand and one refresh does (DF-W30 b).
    view_.refresh(*store_, session_.overriddenLanes());
    return synchronise();
}
void ProjectDocument::configureFreezeTail(double seconds) noexcept {
    if (std::isfinite(seconds) && seconds >= 0)
        freezeTailSeconds_ = seconds;
}

void ProjectDocument::configureTrackRenderDispatch(std::function<void(std::function<void()>)> post) {
    postTrackRenderHost_ = std::move(post);
}

bool ProjectDocument::freezeTrack(std::int64_t track) {
    return startTrackRender(TrackRenderKind::Freeze, track);
}

bool ProjectDocument::unfreezeTrack(std::int64_t track) {
    const auto result = ops_.commitPrepared([&](Store &s, Actor) {
        return freeze::unfreeze(s, std::span<const std::int64_t>(&track, 1));
    });
    error_ = result.error;
    return result && synchronise();
}

bool ProjectDocument::flattenTrack(std::int64_t track) {
    if (recording_ || landing_) {
        error_ = "Stop recording before flattening a track.";
        return false;
    }
    try {
        RowIds rows(store_->db());
        const auto clip = rows.next("clips");
        const auto result = ops_.commitPrepared([&](Store &s, Actor actor) {
            return freeze::flatten(s, track, clip, actor);
        });
        error_ = result.error;
        return result && synchronise();
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

bool ProjectDocument::bounceTrackToNewTrack(std::int64_t track) {
    return startTrackRender(TrackRenderKind::BounceToNew, track);
}

bool ProjectDocument::bounceTrackInPlace(std::int64_t track) {
    const auto snapshot = view_.committed();
    const auto *node = snapshot ? snapshot->findTrack(track) : nullptr;
    return node && node->frozen ? flattenTrack(track) : startTrackRender(TrackRenderKind::BounceInPlace, track);
}

bool ProjectDocument::startTrackRender(TrackRenderKind kind, std::int64_t track) {
    try {
        if (trackRender_)
            throw std::runtime_error("A track is already rendering.");
        if (store_->readOnly())
            throw std::runtime_error("The project is read-only.");
        if (recording_ || landing_)
            throw std::runtime_error("Stop recording before rendering a track.");
        if (!requirePluginState())
            return false;
        const auto snapshot = view_.committed();
        const auto *node = snapshot ? snapshot->findTrack(track) : nullptr;
        if (!node || node->frozenByGroup ||
            !(ordinaryTrack(*node) || (kind == TrackRenderKind::BounceToNew && node->kind == "group")) ||
            (node->frozen && kind != TrackRenderKind::BounceToNew))
            throw std::runtime_error("This track cannot be rendered.");

        auto work = std::make_unique<TrackRenderWork>();
        work->source = appliedSource(*store_);
        work->generation = work->source.sequence();
        work->label = kind == TrackRenderKind::Freeze ? "Freeze Track"
                    : kind == TrackRenderKind::BounceToNew ? "Bounce to New Track" : "Bounce Track in Place";
        render::Job job;
        job.project = store_->path();
        job.session.sampleRate = engineRate_;
        job.session.maxFrames = std::clamp(engineFrames_, 1, 4096);
        job.encoding.format = audio::WavFormat::Float32;

        render::FreezeCaptureSpec freezeSpec;
        clip_edit::TrackBounceOptions newTrack;
        clip_edit::InPlaceBounceOptions inPlace;
        std::int64_t mediaId = 0;
        {
            SQLite::Transaction read(store_->db(), SQLite::TransactionBehavior::DEFERRED);
            work->head = OpJournal(*store_).headSeq();
            const auto frames = std::ceil(trackRenderLength(*store_, *node, freezeTailSeconds_) *
                                          job.session.sampleRate);
            if (!std::isfinite(frames) || frames > (std::int64_t{1} << 50))
                throw std::runtime_error("The track is too long to render.");
            if (frames <= 0)
                throw std::runtime_error("The track has no arrangement material to render.");
            job.endSample = static_cast<std::int64_t>(frames);
            RowIds rows(store_->db());
            mediaId = rows.next("media_files");
            if (kind == TrackRenderKind::Freeze) {
                freezeSpec.tracks = {track};
                freezeSpec.endSample = job.endSample;
                freezeSpec.session = job.session;
                freezeSpec.documentGeneration = work->generation;
            } else if (kind == TrackRenderKind::BounceToNew) {
                newTrack.job = job;
                newTrack.sourceTrack = track;
                newTrack.newTrack = rows.next("tracks");
                newTrack.clipId = rows.next("clips");
                newTrack.mediaId = mediaId;
                newTrack.routingId = rows.next("routing");
                newTrack.documentGeneration = work->generation;
                newTrack.copyMixerSettings = true;
            } else {
                inPlace.job = job;
                inPlace.sourceTrack = track;
                inPlace.clipId = rows.next("clips");
                inPlace.mediaId = mediaId;
                inPlace.documentGeneration = work->generation;
                inPlace.replaceDevices = true;
            }
            read.commit();
        }
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        std::packaged_task<TrackRenderWork::Result(std::stop_token)> prepare(
            [kind, path = std::move(job.project), head = work->head, host = work->host,
             post = postTrackRenderHost_,
             loader = renderLoader_, freezeSpec = std::move(freezeSpec), newTrack = std::move(newTrack),
             inPlace = std::move(inPlace), mediaId, now](std::stop_token stop) mutable -> TrackRenderWork::Result {
                StoreError problem{};
                auto reader = Store::open(path, problem, false);
                if (!reader || reader->readOnly())
                    return "The project could not be opened for rendering: " + std::string(toString(problem));
                if (OpJournal(*reader).headSeq() != head)
                    return "The project changed before rendering could start.";
                render::Control control;
                control.stop = stop;
                control.onHost = [host, post](std::function<void()> action) {
                    host->invoke(std::move(action), [&] {
                        if (post)
                            post([host] { (void)host->service(); });
                    });
                };
                if (kind == TrackRenderKind::Freeze) {
                    const auto generation = freezeSpec.documentGeneration;
                    auto capture = render::captureFreezeAudio(*reader, std::move(freezeSpec), std::move(loader),
                                                              std::move(control));
                    if (!capture.ready())
                        return capture.error();
                    return freeze::prepareFreeze(*reader, std::move(capture), generation, mediaId, now);
                }
                if (kind == TrackRenderKind::BounceToNew)
                    return clip_edit::prepareBounceToNewTrack(*reader, newTrack, std::move(loader), std::move(control));
                return clip_edit::prepareBounceInPlace(*reader, inPlace, std::move(loader), std::move(control));
            });
        work->completion = prepare.get_future();
        work->worker = std::jthread([prepare = std::move(prepare)](std::stop_token stop) mutable { prepare(stop); });
        trackRender_ = std::move(work);
        trackRenderStatus_.reset();
        error_.clear();
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

void ProjectDocument::pollTrackRender() {
    if (!trackRender_)
        return;
    trackRender_->host->service();
    if (trackRender_->completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    auto work = std::move(trackRender_);
    if (work->worker.get_stop_token().stop_requested()) {
        trackRenderStatus_ = "Track render cancelled for Save As.";
        return;
    }
    try {
        auto prepared = work->completion.get();
        const auto source = appliedSource(*store_);
        if (!source.sameConnection(work->source) || source.sequence() != work->generation ||
            OpJournal(*store_).headSeq() != work->head || recording_ || landing_) {
            trackRenderStatus_ = work->label + " stopped because the project changed.";
            return;
        }
        std::string failure;
        if (const auto *message = std::get_if<std::string>(&prepared)) {
            failure = *message;
        } else if (auto *freezeEdit = std::get_if<freeze::FreezeEdit>(&prepared)) {
            if (freezeEdit->ready())
                failure = ops_.commitPrepared([&](Store &s, Actor) {
                    return freeze::commitFreeze(s, *freezeEdit, work->generation);
                }).error;
            else
                failure = freezeEdit->error();
        } else {
            auto &bounce = std::get<clip_edit::TrackBounce>(prepared);
            if (bounce.ready())
                failure = ops_.commitPrepared([&](Store &s, Actor actor) {
                    return clip_edit::commitTrackBounce(s, bounce, work->generation, actor);
                }).error;
            else
                failure = bounce.error();
        }
        if (failure.empty() && !synchronise())
            failure = error_;
        trackRenderStatus_ = failure.empty() ? work->label + " complete." : work->label + " failed. " + failure;
    } catch (const std::exception &e) {
        trackRenderStatus_ = work->label + " failed. " + e.what();
    }
}

void ProjectDocument::stopTrackRender() {
    if (trackRender_) {
        trackRender_->worker.request_stop();
        trackRender_->host->wait(trackRender_->completion);
        trackRender_.reset();
    }
}

std::optional<std::string> ProjectDocument::takeTrackRenderStatus() {
    return std::exchange(trackRenderStatus_, std::nullopt);
}

bool ProjectDocument::deviceAction(const std::string &op, Payload payload) {
    if (op == "device.remove" && !capturePluginState(payload.value("id", std::int64_t{})))
        return false;
    OpRequest r;
    r.opType = op;
    r.payload = std::move(payload);
    const bool activator = op == "device.setEnabled";
    const auto id = activator || op == "device.remove" ? r.payload.value("id", std::int64_t{}) : 0;
    if (id != 0) {
        const auto row=std::find_if(session_.model().devices.begin(),session_.model().devices.end(),
            [id](const auto& device){return device.id==id;});
        const auto name=row==session_.model().devices.end()?std::string("device"):row->name;
        r.label=(op=="device.remove"?"Remove ":r.payload.value("enabled",false)?"Enable ":"Disable ")+name;
    }
    const auto result = ops_.submit(r);
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    // The requested played value can equal the saved switch. In that case the
    // durable operation succeeds but refresh sees no manual baseline change.
    const bool followed = !activator || session_.overrideDeviceActivator(id);
    if (activator)
        followOverrides();
    feed_.publish(session_, realisedGeneration_);
    if (!followed)
        error_ = session_.error();
    return followed;
}
namespace {
bool readClipboardDevice(Store &store, std::int64_t id, DeviceCopy &out, std::string &error) {
    auto copy = readDeviceCopy(store, id);
    if (!copy.ok) {
        error = copy.error.empty() ? "The device could not be copied." : copy.error;
        return false;
    }
    // A rack with nested chains reads, but the engine cannot paste it (no nested chain image yet):
    // refused here, so a Cut never removes what could not come back.
    if (copy.hasNestedChains) {
        error = "A rack with chains inside it cannot be copied yet (rack paste needs a nested chain image).";
        return false;
    }
    out = std::move(copy);
    error.clear();
    return true;
}
} // namespace

bool ProjectDocument::copyDevice(std::int64_t id, DeviceCopy &out) {
    return capturePluginState(id) && readClipboardDevice(*store_, id, out, error_);
}

std::vector<DeviceMoveSource> ProjectDocument::deviceRange(std::span<const DevicePanelData> selected, bool mutation) {
    if (selected.empty())
        throw std::runtime_error("Select one or more devices first.");
    if (mutation && store_->readOnly())
        throw std::runtime_error("The project is read-only.");
    const auto chain = selected.front().chain;
    const auto track = selected.front().track;
    std::set<std::int64_t> ids;
    for (const auto &device : selected) {
        if (device.chain != chain || device.track != track || !ids.insert(device.id).second)
            throw std::runtime_error("Select distinct devices from one chain.");
        if ((selected.size() > 1 && device.runtime.claimIdentity().expired()) ||
            !deviceTargetCurrent(device.id, device.runtime, device.instance))
            throw std::runtime_error("One of the selected devices changed or belongs to another project.");
    }
    auto &db = store_->db();
    if (track <= 0 || adi::detail::deviceChainOwner(db, chain) != track)
        throw std::runtime_error("The selected device chain changed tracks.");
    if (mutation) {
        SQLite::Statement editable(db,
            "WITH RECURSIVE enclosing(id,parent_id,frozen,locked) AS ("
            " SELECT id,parent_id,frozen,locked FROM tracks WHERE id=?"
            " UNION SELECT t.id,t.parent_id,t.frozen,t.locked FROM tracks t JOIN enclosing e ON t.id=e.parent_id)"
            " SELECT 1 FROM enclosing WHERE frozen<>0 OR locked<>0 LIMIT 1");
        editable.bind(1, track);
        if (editable.executeStep())
            throw std::runtime_error("Devices cannot be edited on a frozen or locked track.");
    }
    std::vector<DeviceMoveSource> ordered;
    ordered.reserve(selected.size());
    for (const auto &device : adi::detail::deviceChainOrder(db, chain))
        if (ids.contains(device.id))
            ordered.push_back({device.id, track, chain});
    if (ordered.size() != selected.size())
        throw std::runtime_error("One of the selected devices changed chains.");
    return ordered;
}

bool ProjectDocument::copyDevices(std::span<const DevicePanelData> selected, std::vector<DeviceCopy> &out, bool cut) {
    return editDeviceRange(selected, &out, cut);
}

bool ProjectDocument::removeDevices(std::span<const DevicePanelData> selected) {
    return editDeviceRange(selected, nullptr, true);
}

bool ProjectDocument::editDeviceRange(std::span<const DevicePanelData> selected, std::vector<DeviceCopy> *clipboard,
                                      bool remove) {
    try {
        // Validate first, then let state capture finish its own journal writes. The range read and
        // removal guards must see that committed state and still name the selected instances.
        for (const auto &device : deviceRange(selected, remove))
            if (!capturePluginState(device.device))
                return false;
        std::vector<DeviceCopy> copied;
        std::vector<OpRequest> requests;
        OpTransactionInputs inputs;
        CommitOptions options;
        const auto source = appliedSource(*store_);
        {
            auto &db = store_->db();
            std::optional<SQLite::Transaction> read;
            if (sqlite3_get_autocommit(db.getHandle()) != 0)
                read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
            const auto devices = deviceRange(selected, remove);
            options.expectHead.emplace(OpJournal(*store_).headSeq());
            for (const auto &device : devices) {
                if (clipboard) {
                    DeviceCopy copy;
                    if (!readClipboardDevice(*store_, device.device, copy, error_))
                        return false;
                    copied.push_back(std::move(copy));
                }
                if (remove) {
                    inputs.rowGuards.push_back({"devices", {{"id", device.device}},
                        adi::detail::captureRow(db, "devices", "id", device.device)});
                    OpRequest request;
                    request.opType = "device.remove";
                    request.payload = {{"id", device.device}};
                    request.label = clipboard ? "Cut Devices" : "Remove Devices";
                    request.targetKind = "device";
                    request.targetId = device.device;
                    requests.push_back(std::move(request));
                }
            }
            if (remove) {
                const auto chain = devices.front().chain;
                inputs.rowGuards.push_back({"device_chains", {{"id", chain}},
                    adi::detail::captureRow(db, "device_chains", "id", chain)});
                requests.front().selBefore = sessionState_.advisory();
            }
        }
        if (remove) {
            const auto result = ops_.commitPrepared(requests, [&](Store &store, Actor) {
                return OpJournal(store).commitCaptured(requests, source, options, inputs);
            });
            error_ = result.error;
            if (!result.ok)
                return false;
            const bool refreshed = synchronise();
            if (refreshed)
                feed_.publish(session_, realisedGeneration_);
            if (clipboard)
                *clipboard = std::move(copied);
            return true;
        }
        if (clipboard)
            *clipboard = std::move(copied);
        error_.clear();
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

std::int64_t ProjectDocument::retainedDeviceId() const noexcept {
    // Removed instances stay alive for Undo, even after their store rows leave.
    std::int64_t id = 0;
    for (std::size_t i = 0; i < session_.entryCount(); ++i)
        id = std::max(id, session_.entryAt(i).deviceId);
    return id;
}
namespace {
// Capture the new sibling order and the device batch's expected head from
// the same read transaction. Preparation does not publish a temporary track.
template <class Prepare>
PreparedBuiltinInsert prepareDeviceDrop(Store &store, bool midi, const RecordingDefaults *defaults, Prepare prepare) {
    try {
        auto &db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto snapshot = engine::SnapshotBuilder::fromStore(store, {}, false);
        const auto track = RowIds(db).next("tracks");
        auto edit = insertTrack(snapshot->tracks, 0, midi, defaults, track);
        return prepare(track, edit.ops);
    } catch (const std::exception &error) {
        PreparedBuiltinInsert refused;
        refused.error = error.what();
        return refused;
    }
}
} // namespace
bool ProjectDocument::insertBuiltin(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before,
                                    const RecordingDefaults *defaults, bool exclusiveArm) {
    const preset::Identity identity{"internal", uid, {}};
    if (track != 0)
        return finishInsert(prepareBuiltinInsert(*store_, uid, track, before, retainedDeviceId(), {}, exclusiveArm),
                            identity);
    const auto *info = findBuiltin(uid);
    return finishInsert(prepareDeviceDrop(*store_, info && info->instrument, defaults,
        [&](std::int64_t id, std::span<const OpRequest> newTrack) {
            return prepareBuiltinInsert(*store_, uid, id, before, retainedDeviceId(), newTrack, exclusiveArm);
        }), identity);
}
bool ProjectDocument::insertPlugin(const rows::PluginRef &ref, std::int64_t track,
                                   std::optional<std::int64_t> before, const PluginInsertOptions& options) {
    return finishInsert(preparePluginInsert(*store_, ref, track, before, options, retainedDeviceId()),
                        {ref.format, ref.uid, ref.shellId});
}
bool ProjectDocument::loadPlugin(const rows::PluginRef &ref, std::int64_t track, std::optional<std::int64_t> before,
                                 const RecordingDefaults *defaults, bool exclusiveArm) {
    PluginInsertOptions options;
    options.exclusiveArm = exclusiveArm;
    if (pluginRouteDefault)
        options.expressionRoute = pluginRouteDefault(ref.format, ref.uid);
    if (track == 0)
        return finishInsert(prepareDeviceDrop(*store_, ref.subtype == "instrument" || ref.subtype == "midi_effect", defaults,
            [&](std::int64_t id, std::span<const OpRequest> newTrack) {
                return preparePluginInsert(*store_, ref, id, before, options, retainedDeviceId(), newTrack);
            }), {ref.format, ref.uid, ref.shellId});
    if (ref.subtype == "instrument") {
        try {
            SQLite::Statement kind(store_->db(), "SELECT kind FROM tracks WHERE id=?");
            kind.bind(1, track);
            if (kind.executeStep() && kind.getColumn(0).getString() != "midi" &&
                kind.getColumn(0).getString() != "instrument") {
                error_ = ref.name + " is an instrument: load it onto a MIDI track";
                return false;
            }
        } catch (const std::exception &e) {
            error_ = e.what();
            return false;
        }
        options.replaceInstrument = true;
    }
    return insertPlugin(ref, track, before, options);
}

std::optional<preset::Preset> ProjectDocument::captureDevicePreset(const DevicePanelData &target) {
    error_.clear();
    if (!deviceTargetCurrent(target.id, target.runtime, target.instance)) {
        if (error_.empty()) error_ = "The device is no longer current.";
        return std::nullopt;
    }
    try {
        preset::Preset values;
        {
            SQLite::Statement device(store_->db(),
                                     "SELECT p.format,p.uid,p.shell_id,p.name FROM devices d JOIN plugin_refs p "
                                     "ON p.id=d.plugin_ref_id WHERE d.id=? AND d.is_rack=0");
            device.bind(1, target.id);
            if (!device.executeStep())
                throw std::runtime_error("This device has no portable preset identity.");
            values.device.format = device.getColumn(0).getString();
            values.device.uid = device.getColumn(1).getString();
            if (!device.getColumn(2).isNull())
                values.device.shellId = device.getColumn(2).getInt64();
            values.deviceName = device.getColumn(3).getString();
        }
        auto current = parameterOps_.readDevicePresetState(target.id, error_);
        if (!current)
            return std::nullopt;
        values.parameters = std::move(current->parameters);
        values.state = std::move(current->state);
        const auto &panels = session_.model().devicePanels;
        const auto panel = std::find_if(panels.begin(), panels.end(), [&](const auto &row) { return row.deviceId == target.id; });
        if (panel != panels.end())
            values.panel = panel->params;
        return values;
    } catch (const std::exception &e) {
        error_ = e.what();
        return std::nullopt;
    }
}

bool ProjectDocument::saveDevicePreset(const DevicePanelData &target, const std::filesystem::path &library,
                                       const std::string &name, std::filesystem::path &saved) {
    auto values = captureDevicePreset(target);
    if (!values)
        return false;
    try {
        values->name = name;
        saved = preset::write(library, *values);
        error_.clear();
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

bool ProjectDocument::saveDefaultDevicePreset(const DevicePanelData &target, const std::filesystem::path &library) {
    auto values = captureDevicePreset(target);
    if (!values)
        return false;
    try {
        values->name = "Default";
        (void)preset::writeDefault(library, *values);
        error_.clear();
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}

std::int64_t ProjectDocument::loadDevicePreset(preset::Preset presetValue, std::int64_t track,
                                              std::optional<std::int64_t> before,
                                              const std::optional<rows::PluginRef> &installed) {
    try {
        PreparedBuiltinInsert prepared;
        if (presetValue.device.format == "internal") {
            if (presetValue.device.shellId)
                throw std::runtime_error("This built-in preset has a plug-in shell ID.");
            prepared = prepareBuiltinInsert(*store_, presetValue.device.uid, track, before, retainedDeviceId());
        } else {
            if (!installed || preset::Identity{installed->format, installed->uid, installed->shellId} != presetValue.device)
                throw std::runtime_error("The exact plug-in for this preset is unavailable.");
            PluginInsertOptions options;
            options.replaceInstrument = installed->subtype == "instrument";
            prepared = preparePluginInsert(*store_, *installed, track, before, options, retainedDeviceId());
        }
        return finishDevicePreset(std::move(prepared), std::move(presetValue));
    } catch (const std::exception &e) {
        error_ = e.what();
        return 0;
    }
}

std::optional<ProjectDocument::HotSwapTargetInfo> ProjectDocument::hotSwapTarget(const DevicePanelData &target) {
    try {
        if (store_->readOnly())
            throw std::runtime_error("The project is read-only.");
        if (!deviceTargetCurrent(target.id, target.runtime, target.instance))
            throw std::runtime_error("The Hot-Swap device is no longer current.");
        SQLite::Statement device(store_->db(),
                                 "SELECT p.format,p.uid,p.shell_id,p.subtype FROM devices d "
                                 "JOIN plugin_refs p ON p.id=d.plugin_ref_id JOIN device_chains c ON c.id=d.chain_id "
                                 "WHERE d.id=? AND c.id=? AND c.track_id=? AND d.is_rack=0");
        device.bind(1, target.id);
        device.bind(2, target.chain);
        device.bind(3, target.track);
        if (!device.executeStep())
            throw std::runtime_error("The Hot-Swap device is no longer on that track and chain.");
        SQLite::Statement editable(store_->db(),
            "WITH RECURSIVE enclosing(id,parent_id,frozen,locked) AS ("
            " SELECT id,parent_id,frozen,locked FROM tracks WHERE id=?"
            " UNION SELECT t.id,t.parent_id,t.frozen,t.locked FROM tracks t JOIN enclosing e ON t.id=e.parent_id)"
            " SELECT 1 FROM enclosing WHERE frozen<>0 OR locked<>0 LIMIT 1");
        editable.bind(1, target.track);
        if (editable.executeStep())
            throw std::runtime_error("A device cannot be swapped on a frozen or locked track.");
        HotSwapTargetInfo result;
        result.device.format = device.getColumn(0).getString();
        result.device.uid = device.getColumn(1).getString();
        if (!device.getColumn(2).isNull())
            result.device.shellId = device.getColumn(2).getInt64();
        result.subtype = device.getColumn(3).getString();
        error_.clear();
        return result;
    } catch (const std::exception &e) {
        error_ = e.what();
        return std::nullopt;
    }
}

std::optional<ProjectDocument::HotSwapTargetInfo> ProjectDocument::hotSwapTargetAfterCapture(
    const DevicePanelData &target) {
    const auto current = hotSwapTarget(target);
    // Built-in edits already own their state in the journal. Preset recall
    // keeps their inverse in the same transaction, including unauthored rows.
    if (!current || current->device.format == "internal")
        return current;
    if (!capturePluginState(target.id))
        return std::nullopt;
    // The plug-in's capture runs on the owner and may yield to UI changes.
    // Prepare the swap against the committed state and the same live target.
    return hotSwapTarget(target);
}

std::int64_t ProjectDocument::hotSwapBuiltin(const DevicePanelData &target, const std::string &uid) {
    if (!hotSwapTargetAfterCapture(target))
        return 0;
    auto prepared = prepareBuiltinReplacement(*store_, uid, target.id, retainedDeviceId());
    const auto id = prepared.deviceId;
    return finishInsert(std::move(prepared), {"internal", uid, {}}) ? id : 0;
}

std::int64_t ProjectDocument::hotSwapPlugin(const DevicePanelData &target, const rows::PluginRef &ref) {
    if (!hotSwapTargetAfterCapture(target))
        return 0;
    auto prepared = preparePluginReplacement(*store_, ref, target.id, retainedDeviceId());
    const auto id = prepared.deviceId;
    return finishInsert(std::move(prepared), {ref.format, ref.uid, ref.shellId}) ? id : 0;
}

bool ProjectDocument::hotSwapSample(const DevicePanelData &target, std::int32_t slot,
                                    const std::filesystem::path &file, std::int64_t nowUtc) {
    return hotSwapTarget(target).has_value() && assignSample(target.id, slot, file, nowUtc);
}

std::int64_t ProjectDocument::hotSwapPreset(const DevicePanelData &target, preset::Preset presetValue,
                                          const std::optional<rows::PluginRef> &installed) {
    const auto identity = hotSwapTargetAfterCapture(target);
    if (!identity)
        return 0;
    PreparedBuiltinInsert prepared;
    const bool inPlace = identity->device == presetValue.device;
    if (inPlace) {
        prepared.ok = true;
        prepared.deviceId = target.id;
        prepared.chainId = target.chain;
        prepared.options.expectHead.emplace(OpJournal(*store_).headSeq());
    } else if (presetValue.device.format == "internal" && !presetValue.device.shellId) {
        prepared = prepareBuiltinReplacement(*store_, presetValue.device.uid, target.id, retainedDeviceId());
    } else if (installed && preset::Identity{installed->format, installed->uid, installed->shellId} == presetValue.device) {
        prepared = preparePluginReplacement(*store_, *installed, target.id, retainedDeviceId());
    } else {
        error_ = "The exact plug-in for this preset is unavailable.";
        return 0;
    }
    return finishDevicePreset(std::move(prepared), std::move(presetValue), inPlace ? PresetLoad::InPlace : PresetLoad::Insert);
}

std::int64_t ProjectDocument::finishDevicePreset(PreparedBuiltinInsert prepared, preset::Preset presetValue,
                                                 PresetLoad load) {
    try {
        if (!prepared.ok)
            throw std::runtime_error(prepared.error);
        const bool inPlace = load == PresetLoad::InPlace;
        if (!inPlace && prepared.sourceFile != deviceCopyIdentity())
            throw std::runtime_error("The project changed while preparing the device preset.");
        const std::string label = load == PresetLoad::Default ? prepared.requests.front().label
                                  : inPlace ? "Hot-Swap Device Preset" : "Load Device Preset";
        if (prepared.newTrack)
            prepared.requests.front().selBefore = sessionState_.advisory();

        const auto device = prepared.deviceId;
        const auto source = appliedSource(*store_);
        OpTransactionInputs inputs;
        auto add = [&](const char *type, Payload value) {
            OpRequest request;
            request.opType = type;
            request.payload = std::move(value);
            request.targetKind = "device";
            request.targetId = device;
            prepared.requests.push_back(std::move(request));
        };
        std::set<std::string> hashes;
        const auto addState = [&](const std::string &role, std::vector<std::byte> bytes) {
            std::string hint = "adi.device-preset";
            if (presetValue.device.format == "internal" && role.starts_with("sample:")) {
                if (bytes.empty())
                    throw std::runtime_error("The preset has an empty sample state stream.");
                const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
                const auto sample = Payload::from_cbor(data, data + bytes.size());
                const auto info = device::inspectBuiltinSample(sample);
                if (info.uid != presetValue.device.uid || role != "sample:" + std::to_string(info.slot))
                    throw std::runtime_error("The preset contains a sample for a different device or slot.");
                // Check the embedded PCM before marking the sample portable.
                (void)device::restoreBuiltinSample(sample, engineRate_);
                hint = device::kPresetSampleStateHint;
            }
            const auto hash = media::blake3Bytes(bytes);
            if (!hash)
                throw std::runtime_error("The preset state could not be read.");
            add("device.loadState", {{"dev", device}, {"role", role}, {"hash", hash.hex}, {"hint", hint}});
            if (hashes.insert(hash.hex).second)
                inputs.stateBlobs.push_back({hash.hex, std::move(bytes)});
        };
        const auto addParam = [&](const std::string &id, Payload value) {
            add("device.setParam", {{"dev", device}, {"param", id}, {"norm", std::move(value)}});
        };
        if (inPlace) {
            auto baseline = parameterOps_.readDevicePresetState(device, error_);
            if (!baseline)
                return 0;
            auto &db = store_->db();
            const auto deviceRow = adi::detail::captureRow(db, "devices", "id", device);
            if (!deviceRow || deviceRow->at("chain_id").get<std::int64_t>() != prepared.chainId)
                throw std::runtime_error("The Hot-Swap device is no longer in that chain.");
            const auto ref = deviceRow->at("plugin_ref_id").get<std::int64_t>();
            const auto refRow = adi::detail::captureRow(db, "plugin_refs", "id", ref);
            const auto shell = presetValue.device.shellId ? Payload(*presetValue.device.shellId) : Payload(nullptr);
            if (!refRow || refRow->at("format").get_ref<const std::string &>() != presetValue.device.format ||
                refRow->at("uid").get_ref<const std::string &>() != presetValue.device.uid || refRow->at("shell_id") != shell)
                throw std::runtime_error("The Hot-Swap device no longer matches the preset.");
            inputs.rowGuards.push_back({"devices", {{"id", device}}, deviceRow});
            inputs.rowGuards.push_back({"plugin_refs", {{"id", ref}}, refRow});
            const auto readRows = [&](const char *sql, const char *table, const char *key) {
                OpRowSetGuard guard{table, {{"device_id", device}}, {}};
                std::set<std::string> recorded;
                SQLite::Statement rows(db, sql);
                rows.bind(1, device);
                while (rows.executeStep()) {
                    auto row = adi::detail::captureQueryRow(rows);
                    recorded.insert(row.at(key).get<std::string>());
                    guard.expectedRows.push_back(std::move(row));
                }
                inputs.rowSetGuards.push_back(std::move(guard));
                return recorded;
            };
            const auto recordedParams = readRows("SELECT * FROM plugin_params WHERE device_id=? ORDER BY param_id",
                                                 "plugin_params", "param_id");
            const auto recordedStates = readRows("SELECT * FROM plugin_state WHERE device_id=? ORDER BY stream_role",
                                                 "plugin_state", "stream_role");
            for (const auto &[id, normalized] : presetValue.parameters)
                if (!baseline->parameters.contains(id) || !std::isfinite(normalized) || normalized < 0 || normalized > 1)
                    throw std::runtime_error("The preset contains an unsupported parameter or value.");
            for (const auto &[id, oldValue] : baseline->parameters) {
                if (recordedParams.contains(id) || (presetValue.state.empty() && !presetValue.parameters.contains(id)))
                    continue;
                addParam(id, oldValue);
                // On Undo, restore the old value after restoring opaque state, then remove the
                // temporary parameter row. A null replay only acknowledges that a row is absent.
                if (!presetValue.state.empty())
                    addParam(id, nullptr);
            }
            for (const auto &[role, bytes] : presetValue.state) {
                const auto old = baseline->state.find(role);
                if (old == baseline->state.end() || bytes.empty())
                    throw std::runtime_error("The preset contains an unsupported or empty state stream.");
                if (!recordedStates.contains(role))
                    addState(role, std::move(old->second));
            }
        }
        for (auto &[role, bytes] : presetValue.state)
            addState(role, std::move(bytes));
        for (const auto &[param, normalized] : presetValue.parameters)
            addParam(param, normalized);
        if (presetValue.panel)
            add("device.setPanel", {{"dev", device}, {"params", *presetValue.panel}});
        add("device.setPreset", {{"dev", device}, {"preset", presetValue.name}});
        for (auto &request : prepared.requests)
            request.label = label;

        const auto result = ops_.commitPrepared(prepared.requests, [&](Store &store, Actor) {
            return OpJournal(store).commitCaptured(prepared.requests, source, prepared.options, inputs);
        });
        error_ = result.error;
        if (!result || !synchronise())
            return 0;
        if (inPlace) {
            const auto applied = parameterOps_.retryApplied(captureNowMs());
            if (applied.state == engine::AppliedDeliveryState::Refused || applied.state == engine::AppliedDeliveryState::Invalidated) {
                error_ = "The preset was recorded, but could not be delivered to the device. " + applied.error;
                return 0;
            }
        }
        publishDeviceEdit(prepared.newTrack);
        return device;
    } catch (const std::exception &e) {
        error_ = e.what();
        return 0;
    }
}

bool ProjectDocument::finishInsert(PreparedBuiltinInsert prepared, const preset::Identity &identity) {
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The device could not be loaded." : prepared.error;
        return false;
    }
    if (devicePresetLibrary) {
        try {
            const auto library = devicePresetLibrary();
            if (!library.empty())
                if (auto saved = preset::readDefault(library, identity))
                    return finishDevicePreset(std::move(prepared), std::move(*saved), PresetLoad::Default) != 0;
        } catch (const std::exception &e) {
            error_ = std::string("The default device preset could not be read. ") + e.what();
            return false;
        }
    }
    const auto newTrack = prepared.newTrack;
    if (newTrack)
        prepared.requests.front().selBefore = sessionState_.advisory();
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedBuiltinInsert &p, Actor actor) {
        return commitBuiltinInsert(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    publishDeviceEdit(newTrack);
    return true;
}
void ProjectDocument::publishDeviceEdit(std::optional<std::int64_t> newTrack) {
    feed_.publish(session_, realisedGeneration_);
    if (newTrack) {
        auto selected = sessionState_.selection();
        selected.track = *newTrack;
        selected.toTrack = selected.clip = 0;
        selected.picked.clear();
        selected.pickedClips.clear();
        selected.start = selected.end = selected.insert;
        (void)sessionState_.select(selected, error_);
    }
}
std::vector<DeviceKeyChoice> ProjectDocument::deviceKeyChoices(const DevicePanelData& target) {
    std::vector<DeviceKeyChoice> choices;
    if(!deviceTargetCurrent(target.id,target.runtime,target.instance))return choices;
    auto* instance=session_.instanceFor(target.id);
    const bool keyAvailable=instance && instance->sidechainInputChannels()>0;
    const auto& model=session_.model();
    const rows::Routing* current=nullptr;std::size_t count=0;
    for(const auto& route:model.routing)if(route.kind=="sidechain" && route.dstKind=="device" && route.dstId==target.id){current=&route;++count;}
    if(!keyAvailable && count==0)return choices;
    choices.push_back({{},"post_mixer","None",count==0,view_.generation()});
    if(!keyAvailable)return choices; // recovery only; never fabricate an input
    // Each source named as its track's header shows it (walk-B B13).
    const SnapshotReader shownFrom(view_.current());
    const auto shown=shownFrom.get()?shownTrackNames(shownFrom):std::map<std::int64_t,std::string>{};
    for(const auto& track:model.tracks){
        if(track.kind=="vca")continue;
        const auto named=shown.find(track.id);
        const auto& name=named!=shown.end()?named->second:track.name;
        for(const auto& tap:{"pre_fx","post_fx","post_mixer"})
            choices.push_back({track.id,tap,name,count==1 && current->enabled && current->srcKind=="track" && current->srcId==track.id && current->keyTap==tap});
    }
    for(auto& choice:choices)choice.generation=view_.generation();
    return choices;
}
bool ProjectDocument::setDeviceKeyChoice(const DevicePanelData& target,const DeviceKeyChoice& choice) {
    if(!synchronise())return false;
    if(!choice.generation || choice.generation!=view_.generation()) {
        error_="The project changed while choosing a sidechain. Open the menu again.";return false;
    }
    return setDeviceKey(target,choice.source,choice.tap);
}
bool ProjectDocument::setDeviceKey(const DevicePanelData& target,std::optional<std::int64_t> source,const std::string& tap) {
    if(!deviceTargetCurrent(target.id,target.runtime,target.instance)) {error_="That device changed while choosing its sidechain.";return false;}
    if(!synchronise())return false;
    auto* instance=session_.instanceFor(target.id);
    if(source && (!instance || instance->sidechainInputChannels()<=0)) {error_="That device has no declared auxiliary input.";return false;}
    for(const auto& held:parameterOps_.pendingGestures())if(held.deviceId==target.id){error_="Finish the device edit before changing its sidechain.";return false;}
    auto prepared=prepareDeviceKey(*store_,target.id,source,tap);
    if(!prepared.ok){error_=prepared.error;return false;}
    // Removing edges cannot introduce feedback or an unavailable tap. Keep
    // recovery possible when another stored key is already unavailable.
    if(source && !prepared.requests.empty() && !session_.validateRouting(prepared.candidate,error_)) {
        error_=error_.find("cycle")!=std::string::npos
            ? "That sidechain would feed back into its own source. Choose an earlier tap or another channel."
            : "That sidechain source or input is unavailable. Choose another source or remove the unavailable key.";
        return false;
    }
    const auto result=ops_.commitPrepared(std::move(prepared),[](Store& store,PreparedDeviceKey& p,Actor actor){return commitDeviceKey(store,p,actor);});
    error_=result.error;if(!result.ok || !synchronise())return false;
    feed_.publish(session_,realisedGeneration_);return true;
}

bool ProjectDocument::addMixFunction(mix::Function kind, std::int64_t track) {
    const auto available = device::mixFunctionAvailability(kind);
    if (!available.ready) { error_ = available.reason; return false; }
    auto prepared = mix::prepareFunctionInsert(*store_, kind, track, available.ready, retainedDeviceId());
    if (!prepared.ok) { error_ = prepared.error; return false; }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, mix::PreparedFunctionEdit &p, Actor a) {
        return mix::commitFunctionEdit(s, p, a);
    });
    error_ = result.error;
    if (!result.ok || !synchronise()) return false;
    feed_.publish(session_, realisedGeneration_); return true;
}
bool ProjectDocument::moveMixFunction(const DevicePanelData &expected, std::int64_t track,
                                       std::optional<std::int64_t> before) {
    if (!deviceTargetCurrent(expected.id, expected.runtime, expected.instance)) {
        error_ = "That function changed before it could be moved."; return false;
    }
    auto prepared = mix::prepareFunctionMove(*store_, expected.id, track, before);
    if (!prepared.ok) { error_ = prepared.error; return false; }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, mix::PreparedFunctionEdit &p, Actor a) {
        return mix::commitFunctionEdit(s, p, a);
    });
    error_ = result.error;
    if (!result.ok || !synchronise()) return false;
    feed_.publish(session_, realisedGeneration_); return true;
}
bool ProjectDocument::moveDevice(const DevicePanelData &expected, std::int64_t track,
                                 std::optional<std::int64_t> before) {
    return moveDevices(std::span<const DevicePanelData>(&expected, 1), track, before);
}
bool ProjectDocument::moveDevices(std::span<const DevicePanelData> selected, std::int64_t track,
                                  std::optional<std::int64_t> before) {
    PreparedDeviceMove prepared;
    try {
        auto &db = store_->db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto sources = deviceRange(selected, true);
        if (selected.size() > 1 && std::any_of(selected.begin(), selected.end(), [](const auto &device) {
                return mix::functionForReference(device.format, device.uid) != nullptr;
            }))
            throw std::runtime_error("Move Mix View functions from their own controls.");
        prepared = prepareDeviceMove(*store_, sources, track, before);
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
    if (!prepared.ok) {
        error_ = prepared.error;
        return false;
    }
    if (prepared.requests.empty()) {
        error_.clear();
        return true;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &store, PreparedDeviceMove &move, Actor actor) {
        return commitDeviceMove(store, move, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
namespace {
// Commits a prepared sample batch and hands each new state to the live device, as the engine's
// contract asks (ENGINE-SAMPLE-SLOTS.md): forward payloads after the commit.
std::vector<Payload> loadStates(const PreparedSampleAssignment &prepared) {
    std::vector<Payload> out;
    for (const auto &r : prepared.requests)
        if (r.opType == "device.loadState")
            out.push_back(r.payload);
    return out;
}
} // namespace
bool ProjectDocument::loadSampleInstrument(std::int64_t track, const std::filesystem::path &file,
                                           std::int64_t nowUtc) {
    auto prepared = prepareSampleInstrument(*store_, track, file, engineRate_, nowUtc, &audio::defaultDecodeCache(),
                                            retainedDeviceId());
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The sample instrument could not be loaded." : prepared.error;
        return false;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &store, PreparedSampleAssignment &sample, Actor actor) {
        return commitSampleAssignment(store, sample, actor);
    });
    error_ = result.error;
    // The first graph build for the new instrument reads the committed sample
    // state when it creates the runtime, before that runtime can play a note.
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
bool ProjectDocument::assignSample(std::int64_t device, std::int32_t slot, const std::filesystem::path &file,
                                   std::int64_t nowUtc) {
    // The decode cache takes every format the browser lists (AIFF, FLAC, MP3...), not WAV alone.
    auto prepared = prepareSampleAssignment(*store_, device, slot, file, engineRate_, nowUtc, &audio::defaultDecodeCache());
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The sample could not be prepared." : prepared.error;
        return false;
    }
    const auto states = loadStates(prepared);
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedSampleAssignment &p, Actor actor) {
        return commitSampleAssignment(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok)
        return false;
    for (const auto &state : states)
        parameterOps_.appliedState(*store_, state, captureNowMs());
    if (!synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
bool ProjectDocument::clearSample(std::int64_t device, std::int32_t slot) {
    auto prepared = prepareSampleClear(*store_, device, slot);
    if (!prepared.ok) {
        error_ = prepared.error.empty() ? "The slot could not be cleared." : prepared.error;
        return false;
    }
    const auto states = loadStates(prepared);
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedSampleAssignment &p, Actor actor) {
        return commitSampleAssignment(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok)
        return false;
    for (const auto &state : states)
        parameterOps_.appliedState(*store_, state, captureNowMs());
    if (!synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
std::string ProjectDocument::deviceCopyIdentity() const {
    SQLite::Statement row(store_->db(), "PRAGMA database_list");
    while (row.executeStep())
        if (row.getColumn(1).getString() == "main")
            return row.getColumn(2).getString();
    return {};
}
bool ProjectDocument::pasteDevice(const DeviceCopy &copy, std::int64_t track, std::optional<std::int64_t> before) {
    return pasteDevices(std::span<const DeviceCopy>(&copy, 1), track, before);
}
bool ProjectDocument::pasteDevices(std::span<const DeviceCopy> copies, std::int64_t track,
                                   std::optional<std::int64_t> before) {
    PreparedDevicePaste prepared;
    try {
        if (copies.empty())
            throw std::runtime_error("The device clipboard is empty.");
        auto &db = store_->db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        std::set<std::string> functions;
        for (const auto &copy : copies) {
            if (!copy.reference || !mix::functionForReference(copy.reference->format, copy.reference->uid))
                continue;
            if (!functions.insert(copy.reference->uid).second)
                throw std::runtime_error("The device clipboard contains the same Mix View function twice.");
            SQLite::Statement exists(db,
                "WITH RECURSIVE chains(id) AS (SELECT id FROM device_chains WHERE track_id=? "
                "UNION SELECT c.id FROM device_chains c JOIN devices owner ON owner.id=c.parent_device_id "
                "JOIN chains parent ON parent.id=owner.chain_id) "
                "SELECT 1 FROM devices d JOIN chains c ON c.id=d.chain_id JOIN plugin_refs p ON p.id=d.plugin_ref_id "
                "WHERE p.format='internal' AND p.uid=? LIMIT 1");
            exists.bind(1, track);
            exists.bind(2, copy.reference->uid);
            if (exists.executeStep())
                throw std::runtime_error("That function is already present on this channel.");
        }
        // A paste can go before a device in a nested chain or at the end of
        // the track chain. The first paste can also create that track chain.
        std::int64_t chain = 0;
        if (before) {
            SQLite::Statement q(db, "SELECT chain_id FROM devices WHERE id = ?");
            q.bind(1, *before);
            if (!q.executeStep())
                throw std::runtime_error("The device selected for pasting before is gone.");
            chain = q.getColumn(0).getInt64();
            if (adi::detail::deviceChainOwner(db, chain) != track)
                throw std::runtime_error("The paste position is on a different track.");
        }
        auto floor = retainedDeviceId();
        for (const auto &copy : copies)
            if (copy.ok && copy.insert.contains("id") && copy.insert.at("id").is_number_integer())
                floor = std::max(floor, copy.insert.at("id").get<std::int64_t>());
        RowIds ids(db);
        ids.reserve("devices", floor);
        std::vector<std::int64_t> newIds;
        newIds.reserve(copies.size());
        for (std::size_t n = 0; n < copies.size(); ++n)
            newIds.push_back(ids.next("devices"));
        prepared = before ? prepareDevicePaste(*store_, copies, newIds, chain, before)
                          : prepareDevicePasteForTrack(*store_, copies, newIds, track);
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
    if (!prepared.ok) {
        error_ = prepared.error == "destination chain is missing" ? "The track has no device chain to paste into."
                 : prepared.error.empty()                         ? "The device could not be pasted."
                                                                  : prepared.error;
        return false;
    }
    const auto result = ops_.commitPrepared(std::move(prepared), [](Store &s, PreparedDevicePaste &p, Actor actor) {
        return commitDevicePaste(s, p, actor);
    });
    error_ = result.error;
    if (!result.ok || !synchronise())
        return false;
    feed_.publish(session_, realisedGeneration_);
    return true;
}
std::optional<ProjectDocument::ClipRelinkTarget> ProjectDocument::clipRelinkTarget(std::int64_t clip) {
    SQLite::Statement file(store_->db(), "SELECT m.id,m.hash_blake3,m.orig_name FROM audio_clips a "
                                        "JOIN media_files m ON m.id=a.media_id WHERE a.clip_id=? AND m.embedded=0");
    file.bind(1, clip);
    if (!file.executeStep()) return std::nullopt;
    return ClipRelinkTarget{clip, file.getColumn(0).getInt64(), file.getColumn(1).getString(),
                                file.getColumn(2).getString()};
}
bool ProjectDocument::relinkClipMedia(const ClipRelinkTarget &target, const std::filesystem::path &path) {
    const auto current = clipRelinkTarget(target.clipId);
    if (!current || current->mediaId != target.mediaId || current->hash != target.hash) {
        error_ = "The clip's sample changed while the file chooser was open. Choose it again.";
        return false;
    }
    return relinkMediaFile(target.mediaId, path);
}
bool ProjectDocument::relinkMediaFile(std::int64_t mediaId, std::string_view expectedHash,
                                      const std::filesystem::path &path) {
    {
        SQLite::Statement current(store_->db(), "SELECT hash_blake3 FROM media_files WHERE id=? AND embedded=0");
        current.bind(1, mediaId);
        if (!current.executeStep() || current.getColumn(0).getString() != expectedHash) {
            error_ = "The project media entry changed while the file chooser was open. Choose it again.";
            return false;
        }
    }
    return relinkMediaFile(mediaId, path);
}
bool ProjectDocument::relinkMediaFile(std::int64_t mediaId, const std::filesystem::path &path) {
    const auto result = media::relinkMedia(*store_, mediaId, path);
    if (!result.ok) {
        error_ = result.error.starts_with("hash mismatch or unreadable file:")
            ? "That file could not be read or does not match the original sample."
            : "Sample relink failed: " + result.error;
        return false;
    }
    try {
        {
            SQLite::Statement hash(store_->db(), "SELECT hash_blake3 FROM media_files WHERE id=?");
            hash.bind(1, mediaId);
            if (hash.executeStep()) waveforms_.retry(hash.getColumn(0).getString());
        }
        view_.refresh(*store_, session_.overriddenLanes());
        if (synchronise()) return true;
    } catch (const std::exception &e) {
        error_ = e.what();
    }
    error_ = "Sample relinked. Playback could not refresh: " + error_;
    return false;
}
bool ProjectDocument::prepareImport(const std::string &utf8Path, std::int64_t nowUtc, AudioImport &out,
                                    const std::filesystem::path &defaultsIndex) {
    // A new pool row takes the next id; a file already in the pool keeps its own (the capture
    // reuses it by content).
    // Past what files captured before it in the same drop took (AudioImport::firstFreeId).
    // One `RowIds` for the prepared row (DF-E16), past what the drop's earlier files took.
    RowIds rows(store_->db());
    if (out.firstFreeId > 1)
        rows.reserve("media_files", out.firstFreeId - 1);
    const auto id = rows.next("media_files");
    OpRequest captured;
    const auto result = media::prepareImport(*store_, pathFromUtf8(utf8Path), id, nowUtc, captured);
    if (!result.ok) {
        error_ = result.error.empty() ? "The file could not be read." : result.error;
        return false;
    }
    if (!result.audio || result.audio->frames <= 0 || result.audio->sampleRate <= 0) {
        error_ = "That file holds no audio ADI can play.";
        return false;
    }
    out.savedDefaults.reset();
    try {
        if (auto index = openSampleDefaults(defaultsIndex)) {
            if (auto saved = clip_edit::loadSampleDefaults(*store_, *index, result, captured))
                out.savedDefaults = std::make_shared<library::SampleSettings>(std::move(*saved));
        }
    } catch (const std::exception &e) {
        error_ = utf8Path + " was not imported: " + e.what();
        return false;
    }
    out.mediaId = result.mediaId;
    out.sampleRate = result.audio->sampleRate;
    out.frames = result.audio->frames;
    out.media = std::move(captured);
    error_.clear();
    return true;
}
bool ProjectDocument::saveSampleDefaults(std::int64_t clip, const std::filesystem::path &defaultsIndex) {
    auto index = library::Index::open(defaultsIndex, error_, false);
    if (!index) {
        error_ = "Cannot open the Library to save sample defaults: " + error_;
        return false;
    }
    const auto rate = engineRate_ > 0 ? engineRate_ : 48000.0;
    return clip_edit::saveSampleDefaults(*store_, clip, rate, *index, error_) == library::SampleSettingsProblem::None;
}
ProjectDocument::MaterialJob ProjectDocument::materialJob(const std::string &utf8Path, std::int64_t track,
                                                          std::int64_t atTicks, const ImportChoices &choices,
                                                          std::int64_t nowUtc, edit_plan::EditPolicy placementPolicy) const {
    MaterialJob job;
    job.project = store_->path();
    job.source = pathFromUtf8(utf8Path);
    job.track = track;
    job.atTicks = atTicks;
    job.importedUtc = nowUtc;
    job.choices = choices;
    job.placementPolicy = placementPolicy;
    job.rate = engineRate_ > 0 ? engineRate_ : 48000.0;
    // The next free ids as the project stands; a project changed before the commit refuses as stale.
    RowIds rows(store_->db()); // DF-E16: never an id a remark still holds
    job.clipId = rows.next("clips");
    job.mediaId = rows.next("media_files");
    return job;
}
clip_edit::MaterialEdit ProjectDocument::prepareMaterial(const MaterialJob &job, std::stop_token stop) {
    clip_edit::MaterialEdit out;
    // A connection of the worker's own on the project's file (SPEC §3.8): the import only reads it.
    StoreError error{};
    auto store = Store::open(job.project, error, false);
    if (!store) {
        out.edit.error = "the project could not be read for the import: " + std::string(toString(error));
        return out;
    }
    clip_edit::ImportOptions options;
    options.source = job.source;
    options.track = job.track;
    options.clipId = job.clipId;
    options.mediaId = job.mediaId;
    options.importedUtc = job.importedUtc;
    options.at = {0, job.atTicks};
    options.copyIn = job.choices.copyIn;
    options.warp = job.choices.warp;
    options.edgeFades = job.choices.edgeFades;
    options.warpMode = job.choices.mode;
    options.stop = stop;
    options.arrangementPlacement = job.placementPolicy;
    try {
        auto index = openSampleDefaults(job.defaultsIndex);
        out = clip_edit::prepareImportAudio(*store, options, job.rate, index.get());
    } catch (const std::exception &e) {
        out.edit.error = e.what();
    }
    return out;
}
namespace {
std::string importRefusal(const std::filesystem::path &path, const std::string &why) {
    const std::string file = media::pathUtf8(path.filename());
    if (why.find("source tempo") != std::string::npos || why.find("beat") != std::string::npos)
        return file + ": ADI found no steady beat to warp it by. Import it with Warp off, then enter the source BPM in the Clip View.";
    return file + " was not imported: " + (why.empty() ? std::string("the file could not be read") : why);
}
} // namespace
std::int64_t ProjectDocument::commitMaterial(clip_edit::MaterialEdit &material, const MaterialJob &job) {
    if (!material.edit.ok) {
        // The engine's words, with the file named; an indeterminate beat with Warp on says what to do.
        error_ = importRefusal(job.source, material.edit.error);
        return 0;
    }
    const auto result = ops_.commitPrepared(material.edit.requests, [&](Store &s, Actor actor) {
        return clip_edit::commitMaterial(s, material, actor);
    });
    if (!result) {
        error_ = result.stale ? "The project changed while " + media::pathUtf8(job.source.filename()) +
                                    " was being prepared: drop it again."
                              : result.error;
        return 0;
    }
    error_.clear();
    return job.clipId;
}
ProjectDocument::MaterialBatchJob ProjectDocument::materialBatchJob(
        const std::vector<std::pair<std::string, std::string>> &files, std::int64_t track, std::int64_t atTicks,
        bool separate, const ImportChoices &choices, const RecordingDefaults &defaults, std::int64_t nowUtc) const {
    if (files.empty()) throw std::runtime_error("Drop an audio file.");
    const auto current = view_.current();
    if (!current) throw std::runtime_error("Dropping audio needs an open project.");
    if (auto refusal = audioDropRefusal(*current, track); !refusal.empty())
        throw std::runtime_error(std::move(refusal));
    SQLite::Transaction read(store_->db(), SQLite::TransactionBehavior::DEFERRED);
    MaterialBatchJob job;
    job.project = store_->path();
    job.rate = engineRate_ > 0 ? engineRate_ : 48000.0;
    job.batch.expectedHead = OpJournal(*store_).headSeq();
    job.batch.consecutive = !separate;
    auto index = static_cast<std::int64_t>(current->tracks.size());
    RowIds rows(store_->db());
    const auto addTrack = [&] {
        const auto id = rows.next("tracks");
        audio_drop_detail::appendAudioTrack(job.batch.createTracks, id, index++, &defaults, "Import Audio");
        return id;
    };
    const auto first = track == 0 ? addTrack() : track;
    job.batch.imports.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        clip_edit::ImportOptions options;
        options.source = pathFromUtf8(files[i].first);
        options.track = separate && i > 0 ? addTrack() : first;
        options.clipId = rows.next("clips");
        options.mediaId = rows.next("media_files");
        options.importedUtc = nowUtc;
        options.at = {0, std::max<std::int64_t>(0, atTicks)};
        options.copyIn = choices.copyIn;
        options.warp = choices.warp;
        options.edgeFades = choices.edgeFades;
        options.warpMode = choices.mode;
        job.batch.imports.push_back(std::move(options));
    }
    return job;
}
clip_edit::MaterialEdit ProjectDocument::prepareMaterialBatch(MaterialBatchJob job, std::stop_token stop) {
    StoreError error{};
    auto store = Store::open(job.project, error, false);
    if (!store) {
        clip_edit::MaterialEdit refused;
        refused.edit.error = "The project could not be read for the import: " + std::string(toString(error));
        return refused;
    }
    for (auto &options : job.batch.imports) options.stop = stop;
    try {
        auto index = openSampleDefaults(job.defaultsIndex);
        return clip_edit::prepareImportAudioBatch(*store, job.batch, job.rate, index.get());
    } catch (const std::exception &failure) {
        clip_edit::MaterialEdit refused;
        refused.edit.error = failure.what();
        return refused;
    }
}
bool ProjectDocument::commitMaterialBatch(clip_edit::MaterialEdit &material, const MaterialBatchJob &job) {
    if (!material.edit.ok) {
        error_ = material.edit.error;
        if (const auto failed = material.refusedImport; failed && *failed < job.batch.imports.size()) {
            error_ = importRefusal(job.batch.imports[*failed].source, material.edit.error);
            const auto untried = job.batch.imports.size() - *failed - 1;
            if (job.batch.imports.size() > 1)
                error_ += " (0 files imported; " + std::to_string(untried) + " not tried.)";
        }
        return false;
    }
    const auto result = ops_.commitPrepared(material.edit.requests, [&](Store &store, Actor actor) {
        return clip_edit::commitMaterial(store, material, actor);
    });
    if (!result) {
        error_ = result.stale ? "The project changed while the drop was being prepared: drop the files again."
                              : result.error;
        return false;
    }
    error_.clear();
    return true;
}
bool ProjectDocument::deviceTargetCurrent(std::int64_t id, const engine::DeviceRuntimeToken &expected,
                                          std::uint64_t stamp) {
    const bool pending = realisedGeneration_ != view_.generation();
    if (!synchronise()) return false;
    if (pending) feed_.publish(session_, realisedGeneration_);
    const auto publication = feed_.current();
    const auto shown = std::find_if(publication->begin(), publication->end(), [&](const auto &d) {
        return d.id == id && d.instance == stamp;
    });
    if (shown == publication->end()) return false;
    if (expected.claimIdentity().expired()) return shown->runtime.claimIdentity().expired();
    const auto current = session_.deviceRuntimeToken(id);
    return expected.sameInstance(current) && session_.resolveDevice(expected);
}
bool ProjectDocument::parameterGestureExpected(std::int64_t id, const std::string &param,
                                             engine::ParamEventKind kind, double value,
                                             const engine::DeviceRuntimeToken &expected, std::uint64_t stamp) {
    const bool current = deviceTargetCurrent(id, expected, stamp);
    const auto publication = feed_.current();
    const bool available = std::any_of(publication->begin(), publication->end(), [&](const auto &d) {
        return d.id == id && d.instance == stamp && d.available;
    });
    if (!current || !available || !session_.resolveDevice(expected)) {
        const auto held = surfaceGestures_.find({id, param});
        if (held != surfaceGestures_.end() && held->second.instance == stamp) {
            // End the exact common owner issued to this surface. Sending a numeric-ID End to
            // a replacement would be unsafe; cancellation can retire a detached presentation
            // without invoking that obsolete SDK. A newer surface's owner is never cancelled.
            const auto cancelled = parameterOps_.cancelPending(*held->second.owner, captureNowMs());
            if (cancelled.status.ok()) surfaceGestures_.erase(held);
        }
        error_ = "That device changed while it was being edited. Start a new edit.";
        return false;
    }
    const auto key = std::make_pair(id, param);
    if (kind == engine::ParamEventKind::Begin && !surfaceGestures_.contains(key)) {
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.deviceId == id && (pending.parameterId == param || pending.pairedParameterId == param)) {
                error_ = "Another editor is still editing that parameter.";
                return false;
            }
    }
    const bool taken = parameterGesture(id, param, kind, value);
    if (taken && kind == engine::ParamEventKind::Begin) {
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.deviceId == id && pending.parameterId == param && pending.owner) {
                surfaceGestures_[key] = {stamp, pending.owner};
                break;
            }
    }
    if (taken && kind == engine::ParamEventKind::End) surfaceGestures_.erase(key);
    return taken;
}
bool ProjectDocument::parameterGesture(std::int64_t id, const std::string &param,
                                       engine::ParamEventKind kind, double normalized) {
    // A drag in the strip is an edit and goes through the capture, so a first touch emits its
    // opener and undo lands on a value (ADR-0124 d4, ADR-0181 d5). The engine's window producer
    // (`ParamOps::uiEvent`, N7, #390) sets the plug-in now and appends the gesture on End. Before
    // it judges a value it drains what completed before it, so an older gesture it completed is
    // committed whether or not this event was taken; a device, parameter or value it refuses
    // outright drains nothing and leaves the older gesture to the timer's frame
    // (docs/ENGINE-UI-PARAM-CAPTURE.md). The first accepted value overrides the parameter's
    // automation lane for this run, which is what lights Re-Enable Automation.
    std::vector<OpRequest> requests;
    // Dispatch can accept a new Begin while draining an older completed edit. If the
    // older transaction is refused below, this caller reports failure and cannot
    // subsequently own/End the new gesture. Track core-issued identities, never
    // infer its owner from a device/parameter id after store reconciliation.
    const auto before = kind == engine::ParamEventKind::Begin
                            ? parameterOps_.pendingGestures()
                            : std::vector<engine::PendingParameterGesture>{};
    // Explicit configuration uses this same owner, transaction and exact-runtime
    // path without treating the setting as automatable. Other non-automatable
    // parameters remain read-only.
    const auto *instance = session_.instanceFor(id);
    bool configuration = false;
    if (instance)
        for (std::int32_t i = 0; i < instance->paramCount(); ++i)
            if (const auto *d = instance->paramAt(i); d && d->id == param) {
                configuration = !d->automatable && d->configurable;
                break;
            }
    const auto key=std::make_pair(id,param);
    if(kind==engine::ParamEventKind::End && parameterWrites_.contains(key))
        return commitParameterAutomation(id,param);
    AutomationPass *write=nullptr;
    bool created=false;
    engine::Session::DriverClock clock;
    if(kind==engine::ParamEventKind::Value && instance && !configuration && recording_ &&
       session_.automationArmed() && session_.readDriverClock(clock) && clock.playing) {
        if(auto found=parameterWrites_.find(key);found!=parameterWrites_.end())write=&found->second;
        else for(std::int32_t i=0;i<instance->paramCount();++i) {
            const auto *desc=instance->paramAt(i);
            if(!desc || desc->id!=param)continue;
            if(auto pass=prepareParameterAutomation(id,*desc,instance->getParam(param).normalized,normalized)) {
                write=&parameterWrites_.emplace(key,std::move(*pass)).first->second;
                suspendParameterAutomation(*write);
                created=true;
            }
            break;
        }
    }
    const bool taken = configuration
                           ? parameterOps_.uiConfigurationEvent(id, param, kind, normalized, captureNowMs(), requests)
                           : parameterOps_.uiEvent(id, param, kind, normalized, captureNowMs(), requests);
    // The drain within uiEvent may also observe a native gesture. Find the
    // owned pass again before using it.
    if(write) {
        const auto found=parameterWrites_.find(key);
        if(found!=parameterWrites_.end() && taken) {
            auto &pass=found->second;
            if(!pass.owner)
                for(const auto &pending:parameterOps_.pendingGestures())
                    if(pending.deviceId==id && pending.parameterId==param && pending.owner) {
                        pass.owner=pending.owner;
                        break;
                    }
            pass.value(clock,std::clamp(normalized,0.0,1.0));
            pass.touched=true;
        } else if(found!=parameterWrites_.end() && created) {
            if(const auto lane=found->second.suspendedLane)session_.finishAutomationWrite(*lane,false);
            parameterWrites_.erase(found);
        }
    }
    std::shared_ptr<const gesture::OwnerToken> newOwner;
    if (taken && kind == engine::ParamEventKind::Begin)
        for (const auto &pending : parameterOps_.pendingGestures())
            if (pending.owner && pending.deviceId == id && pending.parameterId == param &&
                std::none_of(before.begin(), before.end(), [&](const auto &old) {
                    return old.owner && old.owner->sameOwner(*pending.owner);
                })) {
                newOwner = pending.owner;
                break;
            }
    const bool appended = !requests.empty();
    const bool committed = commitCaptured(requests); // error_ says why when the store refused
    if (!committed && newOwner) {
        // cancelPending retains a pending restoration if the exact runtime is busy;
        // the normal owner pump retries it. Leave the older refusal and its error
        // untouched, and never send an End to an authored id's replacement.
        (void)parameterOps_.cancelPending(*newOwner, captureNowMs());
    }
    // A value taken overrode its lane with no op (ADR-0162): the snapshot says so before any
    // frame, so Re-Enable Automation lights with the knob's move.
    followOverrides();
    if (appended && committed)
        feed_.publish(session_, realisedGeneration_);
    if (!committed)
        return false;
    if (!taken) {
        error_ = refusalReason(id, param);
        return false;
    }
    error_.clear();
    return true;
}
bool ProjectDocument::parameterPairGestureExpected(std::int64_t id, const engine::ParameterPair &values,
                                                   engine::ParameterPairEvent event,
                                                   const engine::DeviceRuntimeToken &expected, std::uint64_t stamp,
                                                   std::shared_ptr<const gesture::OwnerToken> &owner) {
    using Event = engine::ParameterPairEvent;
    std::vector<OpRequest> requests;
    const auto now = captureNowMs();
    const auto cancel = [&] {
        return !owner || parameterOps_.uiParameterPairEvent(id, values, Event::Cancel, now, owner, requests);
    };
    if (event == Event::Cancel) {
        const bool cancelled = cancel();
        if (cancelled) {
            feed_.publish(session_, realisedGeneration_);
            error_.clear();
        } else error_ = "The paired EQ edit is waiting to restore its values.";
        return cancelled;
    }
    const auto published = feed_.current();
    const bool available = std::any_of(published->begin(), published->end(), [&](const auto &device) {
        return device.id == id && device.instance == stamp && device.available;
    });
    if (!available || !deviceTargetCurrent(id, expected, stamp) || !session_.resolveDevice(expected)) {
        (void)cancel();
        error_ = "That device changed while it was being edited. Start a new edit.";
        return false;
    }
    const bool taken = parameterOps_.uiParameterPairEvent(id, values, event, now, owner, requests);
    const bool committed = commitCaptured(requests);
    followOverrides();
    if (!committed || !taken) {
        (void)cancel();
        feed_.publish(session_, realisedGeneration_);
        if (committed) error_ = "The plug-in did not take the paired EQ edit.";
        return false;
    }
    if (event == Event::End) {
        const auto result = ops_.commitPrepared([&](Store &, Actor) -> CommitResult {
            auto outcome = parameterOps_.commitPending(*owner, now);
            if (outcome.durable) return std::move(*outcome.durable);
            CommitResult refused;
            refused.error = std::move(outcome.status.message);
            return refused;
        });
        if (!result.ok) {
            (void)cancel();
            feed_.publish(session_, realisedGeneration_);
            error_ = result.error;
            return false;
        }
        owner.reset();
        if (!synchronise()) return false;
        feed_.publish(session_, realisedGeneration_);
    }
    error_.clear();
    return true;
}
std::string ProjectDocument::refusalReason(std::int64_t id, const std::string &param) {
    // The producer says only no; the Status Bar says what was wrong, in the producer's order:
    // the device, then the parameter, then the value.
    const auto *entry = session_.entryFor(id);
    auto *instance = session_.instanceFor(id);
    if (!entry || entry->retired || entry->placeholder || !instance)
        return "That device is not loaded, so its values cannot be edited.";
    const device::ParamDescriptor *descriptor = nullptr;
    for (std::int32_t i = 0; i < instance->paramCount() && !descriptor; ++i)
        if (const auto *d = instance->paramAt(i); d && d->id == param)
            descriptor = d;
    if (!descriptor)
        return "The plug-in has no parameter \"" + param + "\".";
    if (descriptor->missing)
        return "The plug-in no longer has the parameter \"" + descriptor->name + "\".";
    if (!descriptor->automatable && !descriptor->configurable)
        return "The plug-in does not let \"" + descriptor->name + "\" be edited from here.";
    return "The plug-in did not take the value.";
}
void ProjectDocument::configureGraphWorkers(std::function<std::uint32_t(std::int32_t)> count,
                                            std::int32_t minimumFrames) {
    graphWorkerCount_ = std::move(count);
    graphWorkerMinimumFrames_ = minimumFrames;
}
void ProjectDocument::prepare(double rate, std::int32_t frames) {
    deviceStopped_.store(false, std::memory_order_release);
    engineRate_ = rate;
    engineFrames_ = frames;
    if (graphWorkerCount_)
        session_.prepareWithGraphWorkers(rate, frames, graphWorkerCount_(frames), graphWorkerMinimumFrames_);
    else
        session_.prepare(rate, frames);
    mailbox_.setSampleRate(rate);
    refreshLimiterReductionSources();
    analysis_->refreshSources(true);
}
void ProjectDocument::release() {
    session_.release(); // stops a running capture; the timer lands what it holds
    externalInputMeter_->clear();
    mailbox_.publish(session_.transport(), {});
    deviceStopped_.store(true, std::memory_order_release);
}
bool ProjectDocument::landTakeHere() {
    return waitForTake(false);
}
bool ProjectDocument::settleRecording() {
    return waitForTake(true);
}
bool ProjectDocument::waitForTake(bool forceDetached) {
    if (recording_)
        stopRecording();
    // Allow ten seconds for the callback and recorder to finish. A completed capture
    // belongs to this Store, so wait for its publication before switching or closing it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (landing_) {
        pollRecording(forceDetached || deviceStopped_.load(std::memory_order_acquire));
        if (!landing_)
            break;
        if (!takePublication_ && std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
void ProjectDocument::recordMixerValues(std::span<const OpRequest> requests) {
    engine::Session::DriverClock clock;
    if(!session_.automationArmed() || !recording_ || !session_.readDriverClock(clock) || !clock.playing)return;
    // A relative preview omits values on their exact origin. Existing write
    // passes still need that value if the fader returns there before release.
    for(auto &pass:mixerWrites_) {
        const auto found=std::find_if(requests.begin(),requests.end(),[&](const auto &request) {
            const auto lane=mixerLane(*store_,request);
            return lane && sameLane(pass.target,*lane);
        });
        if(found==requests.end())pass.value(clock,pass.target.defaultValue);
    }
    for(const auto &request:requests) {
        const auto lane=mixerLane(*store_,request);
        if(!lane)continue;
        auto found=std::find_if(mixerWrites_.begin(),mixerWrites_.end(),[&](const auto &pass) {
            return sameLane(pass.target,*lane);
        });
        if(found==mixerWrites_.end()) {
            found=mixerWrites_.emplace(mixerWrites_.end());
            found->target=*lane;
        }
        found->value(clock,request.payload.at(request.payload.contains("db")?"db":"pan").get<double>());
        // Preview already established that the native value differs.
        found->touched=true;
    }
}

void ProjectDocument::advanceAutomationWrites() {
    if(mixerWrites_.empty() && parameterWrites_.empty() && nativeWrites_.empty())return;
    engine::Session::DriverClock clock;
    if(!session_.readDriverClock(clock))return;
    for(auto &pass:mixerWrites_)pass.advance(clock);
    for(auto &[key,pass]:parameterWrites_){(void)key;pass.advance(clock);}
    for(auto &pass:nativeWrites_)if(!pass.nativeReleased)pass.advance(clock);
}

bool ProjectDocument::closeAutomationWrites(std::optional<std::int64_t> deviceId) {
    std::optional<std::string> failure;
    if (!deviceId && !mixerWrites_.empty() && !endMixerDrag())
        failure = error_;
    for (;;) {
        const auto found = deviceId ? parameterWrites_.lower_bound({*deviceId, {}}) : parameterWrites_.begin();
        if (found == parameterWrites_.end() || (deviceId && found->first.first != *deviceId))
            break;
        const auto key = found->first;
        if (!commitParameterAutomation(key.first, key.second)) {
            if (!failure)
                failure = error_;
            break;
        }
    }
    const bool nativeClosed = finishNativeAutomation(true, deviceId);
    if (failure)
        error_ = std::move(*failure);
    return nativeClosed && !failure;
}

bool ProjectDocument::commitAutomation(std::vector<AutomationPass> passes,std::vector<OpRequest> requests) {
    std::vector<std::int64_t> follow;
    bool ok=false;
    try {
        edit::RowEdits rows(store_->db());clip_edit::CopyIds ids(store_->db());
        const edit::TimeMap time(store_->db());
        engine::Session::DriverClock clock;
        const bool observed=session_.readDriverClock(clock);
        for(auto &pass:passes) {
            const auto previous=matchingLane(*store_,pass.target);
            if(pass.suspendedLane)follow.push_back(*pass.suspendedLane);
            else if(previous && pass.touched && session_.overriddenLanes().contains(previous))follow.push_back(previous);
            if(!pass.touched || (pass.device && !session_.resolveDevice(*pass.device)))continue;
            if(!pass.nativeReleased)pass.close(observed?clock:pass.last);
            const auto laneId=previous?previous:ids.next("automation_lanes");
            if(!previous) {
                const auto &target=pass.target;
                const Payload low=target.minValue?Payload(*target.minValue):Payload(nullptr);
                const Payload high=target.maxValue?Payload(*target.maxValue):Payload(nullptr);
                rows.set("automation_lanes",laneId,edit::defaultRow(store_->db(),"automation_lanes",
                    {{"id",laneId},{"owner_kind",target.ownerKind},{"owner_id",target.ownerId},
                     {"param_ref",target.paramRef},{"param_name",target.paramName},{"value_domain",target.valueDomain},
                     {"unit",target.unit},{"default_value",target.defaultValue},{"min_value",low},{"max_value",high}}));
            }
            const auto lane=rows.current("automation_lanes",laneId);
            const int base=lane->at("time_base");
            auto native=[&](std::int64_t sample) {
                const auto nanos=edit::nanosFromSeconds(static_cast<double>(sample)/engineRate_);
                return base==0?time.ticks(nanos):nanos;
            };
            for(auto &span:pass.spans) {
                if(span.empty())continue;
                std::vector<automation::NativeSpliceKnot> replacement;
                const auto first=native(span.front().time);
                const auto last=native(std::max(span.back().time,edit::addTime(span.front().time,1)));
                if(first>=last)continue;
                for(auto point:span) {
                    point.time=native(point.time);
                    if(!replacement.empty() && replacement.back().point.time==point.time)replacement.back().point=point;
                    else replacement.push_back({point,{}});
                }
                if(replacement.back().point.time<last) {
                    auto end=replacement.back();end.point.time=last;replacement.push_back(end);
                }
                automation::stageEnvelopeSplice(*store_,rows,ids,time,laneId,first,last,replacement,engineRate_);
            }
        }
        auto changes=rows.changes();
        if(!changes.empty()) {
            OpRequest request;request.opType="arranger.splice";
            request.payload={{"id",0},{"changes",std::move(changes)}};
            requests.push_back(std::move(request));
        }
        for(auto &request:requests)request.label="Record Automation";
        ok=commitCaptured(requests);
        if(ok)error_.clear();
    } catch(const std::exception &error) {
        error_=error.what();
    }
    for(const auto lane:follow) {
        if(session_.writingAutomationLanes().contains(lane))session_.finishAutomationWrite(lane);
        else if(ok && session_.overriddenLanes().contains(lane))session_.reenableAutomation(lane);
    }
    followOverrides();
    feed_.publish(session_,realisedGeneration_);
    return ok;
}

std::optional<ProjectDocument::AutomationPass> ProjectDocument::prepareParameterAutomation(std::int64_t id,
        const device::ParamDescriptor &descriptor,double before,double after) {
    const auto *instance=session_.instanceFor(id);
    if(!instance || !descriptor.automatable || descriptor.missing ||
       !std::isfinite(before) || !std::isfinite(after) || before==std::clamp(after,0.0,1.0))return std::nullopt;
    const auto ref=engine::automationParamRef(instance->identity().format,descriptor.id);
    std::uint32_t numeric=0;
    if(!ref || !parseDeviceParamRef(*ref,numeric))return std::nullopt;
    AutomationPass pass;
    pass.target.ownerKind="device";pass.target.ownerId=id;pass.target.paramRef=std::to_string(numeric);
    pass.target.paramName=descriptor.name;pass.target.defaultValue=before;
    pass.target.minValue=0;pass.target.maxValue=1;
    pass.device=session_.deviceRuntimeToken(id);
    return pass;
}

void ProjectDocument::suspendParameterAutomation(AutomationPass &pass) {
    if(pass.suspendedLane)return;
    if(const auto lane=matchingLane(*store_,pass.target);lane>0) {
        (void)session_.beginAutomationWrite(lane);
        if(session_.writingAutomationLanes().contains(lane))pass.suspendedLane=lane;
    }
}

bool ProjectDocument::observeNativeAutomation(std::int64_t id,const device::ParamDescriptor &descriptor,
        engine::ParamEventKind kind,double before,double after,
        const std::shared_ptr<const gesture::OwnerToken> &owner) {
    if(!owner)return false;
    auto existing=std::find_if(nativeWrites_.begin(),nativeWrites_.end(),[&](const auto &pass) {
        return pass.owner->sameOwner(*owner);
    });
    engine::Session::DriverClock clock;
    if(kind==engine::ParamEventKind::End) {
        if(existing==nativeWrites_.end())return false;
        if(session_.readDriverClock(clock))existing->close(clock);
        existing->nativeReleased=true;
        return true;
    }
    if(!recording_ || !session_.automationArmed() || !session_.readDriverClock(clock) || !clock.playing)return false;
    if(existing==nativeWrites_.end()) {
        auto pass=prepareParameterAutomation(id,descriptor,before,after);
        if(!pass)return false;
        pass->owner=owner;
        existing=nativeWrites_.emplace(nativeWrites_.end(),std::move(*pass));
    }
    existing->value(clock,std::clamp(after,0.0,1.0));
    existing->touched=true;
    return true;
}

bool ProjectDocument::finishNativeAutomation(bool stop, std::optional<std::int64_t> deviceId) {
    std::optional<std::string> failure;
    for(std::size_t i=0;i<nativeWrites_.size();) {
        if (deviceId && nativeWrites_[i].target.ownerId != *deviceId) {
            ++i;
            continue;
        }
        if(!stop && !nativeWrites_[i].nativeReleased) {
            // The parameter drain relies on the attached runtime. Change its
            // playback ownership only after the drain releases it.
            suspendParameterAutomation(nativeWrites_[i]);
            ++i;
            continue;
        }
        const auto owner=nativeWrites_[i].owner;
        if (!releaseParameterPreview(owner)) {
            if (!failure)
                failure = error_;
            ++i;
            continue;
        }
        std::vector<AutomationPass> passes;
        passes.push_back(std::move(nativeWrites_[i]));
        nativeWrites_.erase(nativeWrites_.begin()+static_cast<std::ptrdiff_t>(i));
        if (!commitAutomation(std::move(passes)) && !failure)
            failure = error_;
    }
    if (failure)
        error_ = std::move(*failure);
    return !failure;
}

bool ProjectDocument::releaseParameterPreview(const std::shared_ptr<const gesture::OwnerToken> &owner) {
    if(!owner)return true;
    for(const auto &pending:parameterOps_.pendingGestures()) {
        if(!pending.owner || !pending.owner->sameOwner(*owner))continue;
        const auto cancelled=pending.state==gesture::State::Cancelling
            ? parameterOps_.retryPendingCancel(*owner,captureNowMs())
            : parameterOps_.cancelPending(*owner,captureNowMs());
        if(!cancelled.status.ok()) {
            error_=cancelled.status.message;
            return false;
        }
        break;
    }
    return true;
}

bool ProjectDocument::commitParameterAutomation(std::int64_t id,const std::string &param) {
    const auto found=parameterWrites_.find({id,param});
    if(found==parameterWrites_.end() || !releaseParameterPreview(found->second.owner))return false;
    std::vector<AutomationPass> passes;
    passes.push_back(std::move(found->second));parameterWrites_.erase(found);
    return commitAutomation(std::move(passes));
}

bool ProjectDocument::beginMixerDrag(const gesture::RelativeScalarSelection &selection, std::optional<double> coordinate,
                                     const std::string &label) {
    if (mixerDrag_)
        endMixerDrag();
    const bool realised = realisedGeneration_ == view_.generation() && view_.storeCurrent(*store_);
    const auto sequence = session_.realisedAppliedSequence();
    // The live values are the session's and the view's. Attached on every drag: Save As replaces the
    // store's gesture domain, and a coordinator kept from before would refuse.
    auto attached = gesture::Coordinator::attach(*store_, std::make_shared<gesture::EnginePresentation>(view_, session_));
    if (!attached.status.ok() || !attached.value) {
        error_ = "The mixer cannot be dragged live: " + attached.status.message;
        return false;
    }
    coordinator_ = std::move(attached.value);
    gesture::Spec spec;
    spec.strategy = gesture::Strategy::LiveValues;
    spec.label = label;
    auto begun = coordinator_->begin(spec, gesture::relativeScalarProducer(selection));
    if (!begun.status.ok() || !begun.session) {
        error_ = begun.status.message.empty() ? "The drag could not begin." : begun.status.message;
        return false;
    }
    mixerDrag_ = std::move(begun.session);
    if (realised && realisedGeneration_ != view_.generation() && session_.realisedAppliedSequence() == sequence &&
        appliedSource(*store_).sequence() == sequence) {
        // Beginning may have adopted a newer committed image for the preview.
        // Its graph has already been realized; reconnect the document feeds.
        refreshSessionSources();
        syncMonitorInputs();
    }
    if (!coordinate) {
        error_.clear();
        return true;
    }
    if (mixerDrag(*coordinate))
        return true;
    // The first value was refused: the gesture goes, so Undo and Save As are not held by it.
    const auto reason = error_;
    cancelMixerDrag();
    error_ = reason;
    return false;
}
bool ProjectDocument::mixerDrag(double coordinate) {
    if (!mixerDrag_)
        return false;
    const bool realised = realisedGeneration_ == view_.generation();
    const auto committed = view_.committedGeneration();
    const auto updated = mixerDrag_->update(std::make_unique<gesture::RelativeScalarInput>(coordinate));
    if (!updated.status.ok()) {
        error_ = updated.status.message;
        return false;
    }
    if (realised && committed == view_.committedGeneration())
        realisedGeneration_ = view_.generation();
    if (updated.preview)
        recordMixerValues(updated.preview->requests());
    error_.clear();
    return true;
}
bool ProjectDocument::endMixerDrag() {
    if (!mixerDrag_)
        return false;
    auto drag = std::move(mixerDrag_);
    if(!mixerWrites_.empty()) {
        std::vector<OpRequest> other;
        if(const auto preview=drag->preview())
            for(const auto &request:preview->requests()) {
                const auto lane=mixerLane(*store_,request);
                if(!lane || std::none_of(mixerWrites_.begin(),mixerWrites_.end(),[&](const auto &pass) {
                    return sameLane(pass.target,*lane);
                }))other.push_back(request);
            }
        const auto cancelled=drag->cancel();
        if(!cancelled.status.ok()) {
            error_=cancelled.status.message;
            mixerWrites_.clear();
            return false;
        }
        return commitAutomation(std::exchange(mixerWrites_,{}),std::move(other));
    }
    const bool realised = realisedGeneration_ == view_.generation();
    const auto sequence = session_.realisedAppliedSequence();
    const auto graph = session_.graph().stats().published;
    const auto outcome = drag->commit();
    if (!outcome.status.ok()) {
        error_ = outcome.status.message;
        (void)drag->cancel(); // nothing written: the values go back
        synchronise();
        return false;
    }
    const auto currentSequence = session_.realisedAppliedSequence();
    const bool direct = outcome.semanticNoOp ? currentSequence == sequence : currentSequence == sequence + 1;
    if (realised && direct && outcome.presentation.state == gesture::PresentationState::Current &&
        currentSequence == appliedSource(*store_).sequence() && session_.graph().stats().published == graph) {
        // Native promotion already installed the committed rows and view.
        realisedGeneration_ = view_.generation();
    } else {
        view_.refresh(*store_, session_.overriddenLanes());
        synchronise();
    }
    error_.clear();
    return true;
}
void ProjectDocument::cancelMixerDrag() {
    mixerWrites_.clear();
    if (!mixerDrag_)
        return;
    auto drag = std::move(mixerDrag_);
    (void)drag->cancel();
    synchronise();
}
void ProjectDocument::process(const engine::AudioIo &io) noexcept {
    externalInputMeter_->measure(io);
    // One render handle for the callback, so a Record start (`startPlayback`, which applies the saved
    // song start and Count-In) and the block it starts in read the same graph.
    auto block = session_.beginRender();
    mailbox_.drain(session_.transport(), [&block, this](std::int64_t insertTick) noexcept {
        if (!block.startPlayback(insertTick, true))
            session_.transport().play(true);
    }, [&block, &io](std::int64_t sample) noexcept {
        // Keep the first audible block at the held playhead. A source that
        // has already failed is silent and cannot hold otherwise valid clips.
        return block.sourcesReadyAt(sample, io.frames, true);
    });
    (void)block.process(io, captureReady_.load(std::memory_order_acquire));
    mailbox_.publish(session_.transport(), block.countInPosition());
}
RecordStates ProjectDocument::recordStates() const {
    RecordStates out;
    for (const auto &t : session_.model().tracks)
        out[t.id] = {session_.recordArmed(t), t.monitorMode == 1 ? Monitor::In : t.monitorMode == 2 ? Monitor::Auto : Monitor::Off};
    return out;
}
bool ProjectDocument::startRecording(std::int64_t insertTick, std::int32_t inputs, bool playing,
                                     std::int64_t deviceLatencySamples, const std::string &recordFolder,
                                     audio::WavFormat format, bool overdub) {
    if (recording_ || landing_) {
        error_ = recording_ ? "Recording is already running." : "The last take is still being saved.";
        return false;
    }
    const auto current = view_.current();
    if (!current) {
        error_ = "Recording needs an open project.";
        return false;
    }
    const auto states = recordStates();
    const auto armed = armedTracks(current->tracks, states);
    if (armed.audio.empty() && armed.midi.empty()) {
        if (!automationArmed()) {
            error_ = "Arm a track to record into it.";
            return false;
        }
        if (deviceStopped_.load(std::memory_order_acquire)) {
            error_ = "Recording needs an active audio device.";
            return false;
        }
        if (!playing && !mailbox_.post(TransportMailbox::Command::Record, insertTick)) {
            error_ = "Transport command queue is full";
            return false;
        }
        captureReady_.store(false, std::memory_order_release);
        recording_ = true;
        session_.setAutomationWriteState(true, true);
        session_.setRecordingForDevices(true);
        error_.clear();
        return true;
    }
    const auto availableInputs = static_cast<std::uint32_t>(std::max(0, inputs));
    if (availableInputs == 0 && !externalAudioInputs_ &&
        std::any_of(armed.audio.begin(), armed.audio.end(), [&](std::int64_t id) {
            const auto *track = current->findTrack(id);
            return !track || track->inputRef != kResamplingInput;
        })) {
        error_ = "The audio device has no inputs to record from. Choose an input device in the Settings' Audio page.";
        return false;
    }
    if (deviceStopped_.load(std::memory_order_acquire)) {
        error_ = "Recording needs an active audio device.";
        return false;
    }
    engine::AudioRecordingSpec spec;
    const auto playingSample = playing ? std::optional{mailbox_.position()} : std::nullopt;
    if (current->transport.loopEnabled && !session_.recordingStartsAfterLoop(insertTick, playingSample)) {
        if (!armed.midi.empty()) {
            error_ = "Loop recording supports audio tracks. Disarm MIDI tracks or turn off the loop.";
            return false;
        }
        if (!armed.audio.empty() && !session_.configureArrangementLoopRecording(spec, error_))
            return false;
    }
    if (!playing && !mailbox_.hasRoom()) {
        error_ = "Transport command queue is full";
        return false;
    }
    std::unique_ptr<MidiRecording> midi;
    if (!armed.midi.empty()) {
        midi = std::make_unique<MidiRecording>();
        midi->tempo = current->tempo;
        midi->overdub = overdub;
        midi->sampleRate = engineRate_;
        midi->tracks.resize(armed.midi.size());
        midi->preview.resize(armed.midi.size());
        const SnapshotReader source(current);
        for (std::size_t i = 0; i < armed.midi.size(); ++i) {
            const auto track = armed.midi[i];
            midi->preview[i].track = track;
            auto &timing = midi->tracks[i];
            if (const auto *node = source.findTrack(track)) {
                timing.filter = engine::midiInputFilter(node->inputRef);
                if (node->recording.recordEnabled)
                    timing.quantizeGrid = trackRecordingGridTicks(node->recording.recordGrid);
            }
            timing.monitor = session_.midiRecordMonitor(track);
        }
    }
    spec.workFolder = media::projectFolder(*store_);
    if (!armed.audio.empty() && !recordFolder.empty()) {
        if (recordFolder.find('\0') != std::string::npos) {
            error_ = "Choose a valid recording folder in Settings.";
            return false;
        }
        try {
            spec.workFolder = pathFromUtf8(recordFolder);
        } catch (const std::filesystem::filesystem_error &error) {
            error_ = std::string("Could not read the recording folder. ") + error.what();
            return false;
        }
    }
    spec.placementOffsetSamples = spec.loop ? 0 : deviceLatencySamples;
    spec.sampleRate = static_cast<std::uint32_t>(std::lround(engineRate_));
    spec.maxFrames = static_cast<std::uint32_t>(std::max<std::int32_t>(1, engineFrames_));
    spec.format = format;
    // Two seconds of input between the callback and the writer, at least eight blocks.
    spec.ringFrames = std::max<std::uint32_t>(spec.sampleRate * 2, spec.maxFrames * 8);
    for (const auto id : armed.audio) {
        const auto *track = current->findTrack(id);
        if (track && track->inputRef == kResamplingInput) {
            // Main is already on the project's callback clock. The hardware
            // input's round-trip correction does not apply to this stereo bus.
            spec.targets.push_back({id, {0, 1}, 0, engine::RecordingSource::MainMix});
            continue;
        }
        const auto channels = track ? channelsForInput(track->inputRef, inputs) : std::span<const std::uint32_t>{};
        if (channels.empty() || std::any_of(channels.begin(), channels.end(), [availableInputs](auto channel) {
                return channel >= availableInputs;
            })) {
            const auto name = shownTrackName(SnapshotReader(current), id);
            error_ = "No available recording input for " + (name.empty() ? std::string("the armed track") : name) +
                     ". Choose an enabled input under Audio From.";
            return false;
        }
        const bool keep = !spec.loop && track->recording.keepMonitoringLatency &&
                          recordState(states, id).monitor != Monitor::Off;
        spec.targets.push_back({id, std::vector<std::uint32_t>(channels.begin(), channels.end()),
                                keep ? std::optional<std::int64_t>{0} : std::nullopt});
    }
    std::string why;
    if (!spec.targets.empty() && recordingRecoveryEnabled_) {
        const bool viewWasCurrent = view_.storeCurrent(*store_);
        if (!recording::configureRecovery(*store_, spec, why)) {
            error_ = "Recording recovery could not be prepared. " + why;
            return false;
        }
        if (viewWasCurrent) view_.acknowledgeAdvisoryWrite(*store_);
    }
    if (!spec.targets.empty() && !session_.startRecording(spec, why)) {
        error_ = "Recording did not start: " + why;
        return false;
    }
    recordingAudio_ = !spec.targets.empty();
    if (midi && !session_.startMidiInputCapture(armed.midi, why, overdub)) {
        if (recordingAudio_) {
            session_.recording().stop(true);
            cancelTake_ = true;
            landing_ = true;
            captureReady_.store(true, std::memory_order_release);
        }
        error_ = "MIDI recording did not start: " + why;
        return false;
    }
    midiRecording_ = std::move(midi);
    captureReady_.store(true, std::memory_order_release);
    if (!playing && !mailbox_.post(TransportMailbox::Command::Record, insertTick)) {
        if (recordingAudio_)
            session_.recording().stop(true);
        if (midiRecording_)
            session_.midiInputCapture().stop();
        cancelTake_ = true;
        landing_ = true;
        error_ = "Transport command queue is full";
        return false;
    }
    recording_ = true;
    session_.setRecordingForDevices(true); // SC-333
    session_.setAutomationWriteState(automationArmed(), true);
    error_.clear();
    return true;
}
void ProjectDocument::stopRecording() {
    if (!recording_)
        return;
    closeAutomationWrites();
    if (recordingAudio_)
        session_.recording().stop();
    if (midiRecording_)
        session_.midiInputCapture().stop();
    landing_ = recordingAudio_ || static_cast<bool>(midiRecording_);
    recording_ = false;
    session_.setAutomationWriteState(automationArmed(), false);
    session_.setRecordingForDevices(false);
}
std::span<const LiveMidiTake> ProjectDocument::liveMidiTakes() const noexcept {
    const auto *midi = midiRecording_.get();
    if (!midi && takePublication_ && !takePublication_->outcome.ok)
        midi = takePublication_->midi.get();
    if (!midi || !midi->started || cancelTake_)
        return {};
    return midi->preview;
}
bool ProjectDocument::startTakeRecovery(const engine::recording_recovery::LaunchTake &take) {
    if (recording_ || landing_ || store_->readOnly()) {
        error_ = store_->readOnly() ? "Project is read-only." : "Finish the current recording first.";
        return false;
    }
    const auto project = store_->path();
    try {
        auto work = std::make_unique<RecoveryWork>();
        work->result = std::async(std::launch::async, [project, take, stop = work->stop.get_token()] {
            StoreError opened{};
            auto snapshot = Store::open(project, opened, true);
            if (!snapshot)
                return recording::RecoveryTake{recording::RecoveryStatus::Refused, {}, toString(opened)};
            return recording::prepareInterrupted(*snapshot, take.directory.parent_path(), take.directory,
                                                 take.recordingId, stop);
        });
        recoveryWork_ = std::move(work);
        landing_ = true;
        error_.clear();
        return true;
    } catch (const std::exception &failure) {
        error_ = failure.what();
        return false;
    }
}
void ProjectDocument::pollRecording(bool deviceStopped) {
    if (!recording_ && !landing_)
        return;
    if (takePublication_) {
        pollTakePublication();
        return;
    }
    if (recoveryWork_) {
        if (recoveryWork_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto recovered = recoveryWork_->result.get();
        recoveryWork_.reset();
        if (recovered.status == recording::RecoveryStatus::Ready) {
            audioTake_ = std::move(recovered.capture);
            landTake();
        } else {
            landing_ = false;
            const auto message = recovered.status == recording::RecoveryStatus::AlreadyPlaced
                ? "The interrupted take has already been recovered."
                : "Could not recover the interrupted take. " + recovered.error;
            landed_ = TakeLanded{false, {}, message};
        }
        return;
    }
    if (!recordingAudio_ && !midiRecording_) {
        if (deviceStopped)
            stopRecording();
        return;
    }
    if (deviceStopped && midiRecording_)
        session_.finishMidiInputCapture();
    if (midiRecording_)
        midiRecording_->drain(session_.midiInputCapture());
    if (recordingAudio_ && !audioTake_)
        audioTake_ = session_.recording().poll(deviceStopped);
    const bool midiDone = !midiRecording_ || midiRecording_->finished();
    const bool audioDone = !recordingAudio_ || audioTake_.has_value();
    if ((midiRecording_ && midiDone) || (recordingAudio_ && audioDone))
        stopRecording();
    if (midiDone && audioDone) {
        captureReady_.store(false, std::memory_order_release);
        recording_ = false;
        session_.setAutomationWriteState(automationArmed(), false);
        session_.setRecordingForDevices(false);
        landTake();
    }
}
std::optional<ProjectDocument::TakeLanded> ProjectDocument::takeLanded() {
    auto out = std::move(landed_);
    landed_.reset();
    return out;
}
namespace {
std::string recordingFailure(const engine::AudioRecordingResult &r) {
    using F = engine::RecordingFailure;
    switch (r.failure) {
    case F::None: return {};
    case F::Overflow: return "the input came faster than it could be written, and the take was stopped";
    case F::MissingInput: return "the audio device did not deliver the input channels the track records from";
    case F::BlockSize: return "the audio device changed its block size while recording";
    case F::Discontinuity: return "the audio stream jumped while recording (a locate, a loop or a device change)";
    case F::Cancelled: return "the recording was cancelled";
    case F::Io: return "the take could not be written to disk" + (r.error.empty() ? std::string() : " (" + r.error + ")");
    case F::NoFrames: return "nothing was recorded";
    case F::InvalidCapture: return "the capture was not valid" + (r.error.empty() ? std::string() : " (" + r.error + ")");
    case F::PassLimit: return "the loop recorded more passes than a take can hold";
    }
    return r.error;
}
std::string midiRecordingFailure(engine::MidiCaptureState state) {
    using State = engine::MidiCaptureState;
    switch (state) {
    case State::InputReset: return "the MIDI input was reset";
    case State::Discontinuity: return "the MIDI recording clock jumped";
    case State::Overflow: return "the MIDI recording buffer filled";
    case State::DeviceStopped: return "the MIDI recording device stopped";
    default: return {};
    }
}
std::string retainedCaptureLocation(const engine::AudioRecordingResult &result) {
    const auto &original = result.recovered() ? result.recoverySource() : result.directory();
    return " Look for the recorded WAV files in " + media::pathUtf8(original) + ".";
}
std::string recordedTakeMessage(std::size_t clips, const std::string &audioProblem,
                                const std::string &midiProblem, bool hasAudio, bool hasMidi,
                                const std::string &audioLocation = {}) {
    auto message = clips == 1 ? std::string("Recorded one take.")
                              : "Recorded " + std::to_string(clips) + " takes.";
    if (!audioProblem.empty())
        message = hasAudio ? "Kept the audio recorded before the interruption: " + audioProblem + "."
                           : message + " Audio recording ended: " + audioProblem + "." + audioLocation;
    if (!midiProblem.empty())
        message += (hasMidi ? " MIDI recording ended: " : " MIDI was not kept: ") + midiProblem + ".";
    return message;
}
std::string attachmentFailure(const engine::AudioRecordingResult &recorded, const std::string &error) {
    return "Could not add the recorded take. " + error + "." + retainedCaptureLocation(recorded);
}
std::vector<recording::ClipIds> takeClipIds(RowIds &rows, const SnapshotReader &current,
                                           const engine::AudioRecordingResult &take) {
    // The displayed track name supplies both the clip name and the audio filename.
    // Allocate past the IDs still held by remarks as well as existing rows (DF-E16).
    std::vector<recording::ClipIds> ids;
    ids.reserve(take.tracks.size());
    for (const auto &track : take.tracks) {
        auto name = shownTrackName(current, track.trackId);
        ids.push_back({rows.next("clips"), rows.next("media_files"),
                       name.empty() ? std::string("Take") : name, std::move(name)});
    }
    return ids;
}
} // namespace
// For an empty or cancelled capture, report if the system refuses to remove its private directory.
std::string ProjectDocument::discardTake(engine::AudioRecordingResult &result) {
    const auto refused = result.discard();
    if (!refused)
        return {};
    return " Its recorded files could not be removed (" + refused.message() + "); they stay in " +
           media::pathUtf8(result.directory()) + ".";
}
void ProjectDocument::finishMidiTake(const MidiRecording &midi, const recording::RecordingEdit &edit,
                                      const SubmitResult &committed, TakeLanded &outcome) {
    std::vector<recording::MidiRecordQuantize> targets;
    bool keptInTakeLane = false;
    for (std::size_t i = 0; i < edit.midiLandings.size(); ++i) {
        const auto &landing = edit.midiLandings[i];
        const auto &span = midi.preview[i];
        outcome.clips.push_back(landing.clip);
        keptInTakeLane = keptInTakeLane || landing.inTakeLane;
        const auto grid = midi.tracks[i].quantizeGrid;
        if (grid != 0 && !landing.noteIds.empty())
            targets.push_back({landing.clip, landing.sourceStart, span.start, span.end - span.start, grid, landing.noteIds});
    }
    if (keptInTakeLane)
        outcome.message += " Kept overdub in a Take Lane where merging would change other material.";
    if (targets.empty())
        return;
    auto quantized = recording::prepareRecordQuantize(*store_, targets);
    std::string problem;
    if (!quantized.ok) {
        problem = std::move(quantized.error);
    } else if (!quantized.requests.empty()) {
        if (committed.seqs.empty() || !quantized.options.expectHead ||
            *quantized.options.expectHead != committed.seqs.back()) {
            problem = "the project changed after recording";
        } else {
            const auto snapped = ops_.commitPrepared(std::move(quantized), notes::commitDiff);
            if (!snapped)
                problem = snapped.error;
        }
    }
    if (!problem.empty())
        outcome.message += " Record Quantize could not be applied: " + problem + ".";
}
ProjectDocument::TakeLanded ProjectDocument::commitMidiTake(MidiRecording &midi, const std::string &audioProblem,
                                                            const std::string &audioLocation) {
    const auto fail = [&](const std::string &why) -> TakeLanded {
        return {false, {}, "Could not add the recorded take. " + why + "." + audioLocation};
    };
    TakeLanded outcome;
    try {
        RowIds rows(store_->db());
        const auto placement = midi.placements(rows, SnapshotReader(view_.current()));
        auto edit = recording::prepareMidiLinearCommit(*store_, placement.takes, midi.sampleRate);
        if (!edit.ok)
            return fail(edit.error);
        outcome.clips.reserve(placement.takes.size());
        outcome.message = recordedTakeMessage(edit.midiLandings.size(), audioProblem,
                                               midiRecordingFailure(midi.state), false, true, audioLocation);
        const auto committed = ops_.commitPrepared(edit.requests, [&](Store &store, Actor actor) {
            return recording::commit(store, edit, actor);
        });
        if (!committed)
            return fail(committed.error);
        finishMidiTake(midi, edit, committed, outcome);
    } catch (const std::exception &error) {
        return fail(error.what());
    }
    outcome.ok = true;
    synchronise();
    return outcome;
}
bool ProjectDocument::canCaptureMidi() const noexcept {
    return !store_->readOnly() && !recording_ && !landing_ && session_.midiHistoryAvailable();
}

ProjectDocument::TakeLanded ProjectDocument::captureRecentMidi(std::int64_t positionTick, bool playing) {
    TakeLanded outcome;
    if (recording_ || landing_) {
        outcome.message = "Finish Arrangement Record before using Capture MIDI.";
        return outcome;
    }
    std::vector<engine::MidiMemorySnapshot> histories;
    if (!session_.readMidiHistories(histories, outcome.message)) return outcome;
    std::erase_if(histories, [this](const auto& history) {
        if (std::any_of(history.events.begin(), history.events.end(), [](const auto& event) {
                return event.event.type == engine::EventType::NoteOn || event.event.type == engine::EventType::Controller ||
                       capture::isChannelPitchWheel(event.event);
            })) return false;
        session_.consumeMidiHistories(std::span(&history, 1));
        return true;
    });
    if (histories.empty()) {
        outcome.message = "No new MIDI notes or controllers to capture.";
        return outcome;
    }
    try {
        const auto first = std::min_element(histories.begin(), histories.end(), [](const auto& a, const auto& b) {
            return a.events.front().sample < b.events.front().sample;
        })->events.front().sample;
        const auto latest = std::max_element(histories.begin(), histories.end(), [](const auto& a, const auto& b) {
            return a.endSample < b.endSample;
        })->endSample;
        auto start = std::max<std::int64_t>(0, positionTick);
        if (playing) {
            const edit::TimeMap time(store_->db());
            const auto now = time.nanos(start);
            const double ago = static_cast<double>(latest - first) * 1e9 / histories.front().sampleRate;
            start = ago >= static_cast<double>(now) ? 0 : time.ticks(now - std::llround(ago));
        }
        RowIds rows(store_->db());
        const SnapshotReader current(view_.current());
        std::vector<capture::MidiSpec> specs;
        for (const auto& history : histories) {
            std::set<std::uint64_t> held;
            for (const auto& remembered : history.events) {
                const auto& event = remembered.event;
                if (event.type == engine::EventType::NoteOn) held.insert(event.noteId);
                else if (event.type == engine::EventType::NoteOff) held.erase(event.noteId);
            }
            auto name = shownTrackName(current, history.trackId);
            auto& spec = specs.emplace_back();
            spec.clip = rows.next("clips");
            spec.positionTicks = start;
            spec.firstSample = first;
            spec.endSample = held.empty() ? history.events.back().sample + 1 : history.endSample;
            if (!name.empty()) spec.name = std::move(name);
            spec.liveHistory = true;
        }
        auto prepared = capture::prepareMidiBatch(*store_, histories, specs, outcome.message);
        if (!prepared) return outcome;
        const auto committed = ops_.commitPrepared(prepared->requests, [&](Store& store, Actor) {
            return capture::commitMidi(store, *prepared);
        });
        if (!committed) {
            outcome.message = committed.error;
            return outcome;
        }
        session_.consumeMidiHistories(histories);
        synchronise();
        for (const auto& spec : specs) outcome.clips.push_back(spec.clip);
        outcome.ok = true;
        outcome.message = outcome.clips.size() == 1 ? "Captured one MIDI clip." :
            "Captured " + std::to_string(outcome.clips.size()) + " MIDI clips.";
    } catch (const std::exception& exception) {
        outcome.message = exception.what();
    }
    return outcome;
}

void ProjectDocument::landTake() {
    auto audio = std::move(audioTake_);
    audioTake_.reset();
    auto midi = std::move(midiRecording_);
    recordingAudio_ = false;
    if (std::exchange(cancelTake_, false)) {
        landing_ = false;
        if (audio) {
            const auto kept = discardTake(*audio);
            if (!kept.empty())
                landed_ = TakeLanded{false, {}, "The cancelled take was not kept." + kept};
        }
        return;
    }

    const auto audioProblem = audio ? recordingFailure(*audio) : std::string{};
    auto midiProblem = midi ? midiRecordingFailure(midi->state) : std::string{};
    const bool hasAudio = audio && audio->hasAttachableAudio();
    const bool hasMidi = midi && midi->started;
    if (hasAudio && audio->loopSpec()) {
        // Loop recording uses its per-pass Undo groups. The staged linear
        // publisher cannot build the rolling main-lane and take-lane images.
        landing_ = false;
        TakeLanded outcome;
        std::vector<std::int64_t> lastPassClips;
        const auto refuse = [&](const std::string &reason) {
            landed_ = TakeLanded{false, {}, attachmentFailure(*audio, reason)};
        };
        try {
            RowIds rows(store_->db());
            const SnapshotReader current(view_.current());
            std::vector<recording::LoopTrackIds> loopIds;
            loopIds.reserve(audio->tracks.size());
            lastPassClips.reserve(audio->tracks.size());
            for (const auto &track : audio->tracks) {
                auto name = shownTrackName(current, track.trackId);
                const auto clipName = name.empty() ? std::string("Take") : name;
                recording::LoopTrackIds target{rows.next("media_files"), {}, std::move(name)};
                target.passes.reserve(audio->passes().size());
                for (std::size_t pass = 0; pass < audio->passes().size(); ++pass)
                    target.passes.push_back({rows.next("lanes"), rows.next("clips"), clipName});
                if (!target.passes.empty())
                    lastPassClips.push_back(target.passes.back().clip);
                loopIds.push_back(std::move(target));
            }
            auto edit = recording::prepareLoopCommit(*store_, *audio, loopIds);
            if (!edit.ok) {
                refuse(edit.error);
                return;
            }
            const auto committed = ops_.commitPrepared(edit.requests, [&](Store &store, Actor actor) {
                return recording::commit(store, edit, actor);
            });
            if (!committed) {
                refuse(committed.error);
                return;
            }
        } catch (const std::exception &error) {
            refuse(error.what());
            return;
        }
        synchronise();
        // The last partial pass may have left other main-lane clips. Select
        // its audible copy by the same placement and source range as the take.
        try {
            SQLite::Statement main(store_->db(),
                                   "SELECT c.id FROM clips p JOIN audio_clips pa ON pa.clip_id=p.id "
                                   "JOIN clips c ON c.track_id=p.track_id AND c.lane_id IS NULL "
                                   "AND c.pos_ticks=p.pos_ticks AND c.length_ticks=p.length_ticks "
                                   "JOIN audio_clips a ON a.clip_id=c.id AND a.media_id=pa.media_id "
                                   "AND a.src_start_frames=pa.src_start_frames WHERE p.id=?");
            for (const auto id : lastPassClips) {
                main.bind(1, id);
                if (main.executeStep())
                    outcome.clips.push_back(main.getColumn(0).getInt64());
                main.reset();
            }
        } catch (const SQLite::Exception &) {
            // The take is committed even if finding its selection fails.
        }
        if (const auto snapshot = view_.current(); snapshot && audio->preview) {
            try {
                waveforms_.seedRecording(*snapshot, outcome.clips, *audio->preview);
            } catch (const std::exception &) {
                // The committed clip's waveform can still be read from the file.
            }
        }
        outcome.ok = true;
        const auto passes = audio->passes().size();
        outcome.message = passes == 1 ? "Recorded one pass of the loop."
                                      : "Recorded " + std::to_string(passes) + " passes of the loop, a take each.";
        if (audio->recovered()) outcome.message = "Recovered the interrupted loop recording.";
        if (!audio->recoveryWarning().empty()) outcome.message += " " + audio->recoveryWarning();
        if (!audioProblem.empty())
            outcome.message += " Capture stopped because " + audioProblem + ".";
        landed_ = std::move(outcome);
        return;
    }
    if (hasAudio) {
        landing_ = true;
        try {
            takePublication_ = std::make_unique<TakePublication>();
            auto &take = *takePublication_;
            take.recorded = std::move(*audio);
            if (hasMidi)
                take.midi = std::move(midi);
            take.midiProblem = std::move(midiProblem);
            take.startStaging(media::projectFolder(*store_));
        } catch (const std::exception &error) {
            const auto &recorded = takePublication_ ? takePublication_->recorded : *audio;
            landed_ = TakeLanded{false, {}, attachmentFailure(recorded, error.what())};
            takePublication_.reset();
            landing_ = false;
        }
        return;
    }

    landing_ = false;
    using Failure = engine::RecordingFailure;
    const bool discardAudio = audio && (audio->failure == Failure::Cancelled || audio->failure == Failure::NoFrames);
    const bool retainAudio = audio && !discardAudio && !audio->directory().empty();
    const auto audioLocation = discardAudio ? discardTake(*audio) :
                               retainAudio ? retainedCaptureLocation(*audio) : std::string{};
    if (hasMidi) {
        landed_ = commitMidiTake(*midi, audioProblem, audioLocation);
        return;
    }
    const auto reason = !audioProblem.empty() ? audioProblem : !midiProblem.empty() ? midiProblem : "nothing was recorded";
    landed_ = TakeLanded{false, {}, (retainAudio ? "Recording stopped because " : "The take was not kept: ") +
                                  reason + "." + audioLocation};
}
void ProjectDocument::pollTakePublication() {
    auto &take = *takePublication_;
    const auto finish = [&](TakeLanded outcome) {
        landed_ = std::move(outcome);
        takePublication_.reset();
        landing_ = false;
    };
    const auto refuse = [&](const std::string &error) {
        finish({false, {}, attachmentFailure(take.recorded, error)});
    };
    const auto keepPunchedMidi = [&](const std::string &reason) {
        const auto location = retainedCaptureLocation(take.recorded);
        auto outcome = commitMidiTake(*take.midi, {}, location);
        if (outcome.ok)
            outcome.message += " Audio stayed in the recording folder because " + reason + "." + location;
        finish(std::move(outcome));
    };
    const auto ready = [](auto &task) { return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    const auto preparePlacement = [&] {
        RowIds rows(store_->db());
        const SnapshotReader current(view_.current());
        take.ids = takeClipIds(rows, current, take.recorded);
        auto midi = take.midi ? take.midi->placements(rows, current) : MidiRecording::Placement{};
        take.outcome.clips.clear();
        take.outcome.clips.reserve(take.ids.size() + midi.takes.size());
        return midi;
    };
    try {
        if (take.cleanup.valid()) {
            if (!ready(take.cleanup))
                return;
            const auto warning = take.cleanup.get();
            if (!warning.empty())
                take.outcome.message += " " + warning;
            finish(std::move(take.outcome));
            return;
        }
        if (take.staging.valid()) {
            if (!ready(take.staging))
                return;
            take.staged = take.staging.get();
            if (!take.staged.ok) {
                refuse(take.staged.error);
                return;
            }
            const auto midi = preparePlacement();
            auto edit = recording::prepareStagedCommit(*store_, take.staged, take.ids, midi.takes,
                                                       take.recorded.placementOffsetSamples);
            if (!edit.ok) {
                if (edit.audioOutsidePunch && take.midi)
                    keepPunchedMidi(edit.error);
                else
                    refuse(edit.error);
                return;
            }
            take.publication = std::async(std::launch::async, [edit = std::move(edit), stop = take.stop.get_token()]() mutable {
                edit.ok = recording::publishStagedTake(edit, stop);
                return std::move(edit);
            });
            return;
        }
        if (!ready(take.publication))
            return;
        auto edit = take.publication.get();
        if (edit.ok && edit.options.expectHead && *edit.options.expectHead != OpJournal(*store_).headSeq()) {
            const auto midi = preparePlacement();
            const auto refreshed = recording::refreshStagedCommit(*store_, take.staged, edit, take.ids, midi.takes,
                                                                  take.recorded.placementOffsetSamples);
            if (refreshed == recording::StagedRefresh::restage && !take.retried) {
                take.retried = true;
                take.startStaging(media::projectFolder(*store_));
                return;
            }
            edit.ok = refreshed == recording::StagedRefresh::ready;
        }
        if (!edit.ok) {
            if (edit.audioOutsidePunch && take.midi)
                keepPunchedMidi(edit.error);
            else
                refuse(edit.error);
            return;
        }
        take.outcome.clips.insert(take.outcome.clips.begin(), edit.placedAudioClips.begin(), edit.placedAudioClips.end());
        take.outcome.message = recordedTakeMessage(take.outcome.clips.size() + edit.midiLandings.size(), recordingFailure(take.recorded),
                                                   take.midiProblem, true, take.midi != nullptr);
        if (take.recorded.recovered()) take.outcome.message = "Recovered the interrupted audio recording.";
        if (!take.recorded.recoveryWarning().empty()) take.outcome.message += " " + take.recorded.recoveryWarning();
        const auto skippedAudioTracks = take.ids.size() - edit.placedAudioClips.size();
        if (skippedAudioTracks > 0) {
            take.outcome.message += skippedAudioTracks == 1
                ? " Audio on one track fell outside its compensated punch range."
                : " Audio on " + std::to_string(skippedAudioTracks) + " tracks fell outside their compensated punch ranges.";
            take.outcome.message += retainedCaptureLocation(take.recorded);
        }
        const auto committed =
            ops_.commitPrepared(edit.requests, [&](Store &store, Actor actor) { return recording::commit(store, edit, actor); });
        if (!committed) {
            refuse(committed.error);
            return;
        }
        take.outcome.ok = true;
        if (take.midi)
            finishMidiTake(*take.midi, edit, committed, take.outcome);
        synchronise();
        if (const auto snapshot = view_.current(); snapshot && take.recorded.preview) {
            try {
                waveforms_.seedRecording(*snapshot, take.outcome.clips, *take.recorded.preview);
            } catch (const std::exception &) {
                // The committed clip's waveform can still be read from the file.
            }
        }
        take.staged = {}; // The edit still owns staging until the cleanup worker releases it.
        take.cleanup = std::async(std::launch::async, [edit = std::move(edit)]() mutable {
            auto warning = recording::cleanupStagedTake(edit);
            edit = {};
            return warning;
        });
    } catch (const std::exception &error) {
        if (!take.outcome.ok) {
            refuse(error.what());
            return;
        }
        take.outcome.message += " Could not clean up the private recording." + retainedCaptureLocation(take.recorded);
        finish(std::move(take.outcome));
    }
}
} // namespace adi::ui
