// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/recording_commands.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/audio/decode.hpp"
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/published_rollback.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_rows.hpp"
#include "adi/edit_time.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/note_stream_detail.hpp"
#include "adi/take_lane_ops.hpp"
#include <algorithm>
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <utf8proc.h>
#include <charconv>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <limits>
#include <stdexcept>
#include <utility>
namespace adi::recording {
namespace fs=std::filesystem;
namespace {
using FileStamp = media::FileRevision;

FileStamp fileStamp(const fs::path& path) {
    std::error_code error;
    const auto revision = media::readFileRevision(path, error);
    if (!revision) throw std::runtime_error("cannot inspect recording media: " + error.message());
    return *revision;
}

fs::path recordingFolder(const fs::path& projectFolder) {
    return projectFolder / "Samples" / "Recorded";
}

void requireRecordingFolder(const fs::path& folder) {
    if (fs::is_symlink(fs::symlink_status(folder.parent_path())) || fs::is_symlink(fs::symlink_status(folder)))
        throw std::runtime_error("recording audio directories must not be symlinks");
}
}

struct StagedTake::Data {
    struct Wave {
        fs::path source, staged;
        FileStamp sourceProof, stageProof;
        std::string hash;
        int bits;
    };
    engine::AudioRecordingResult recorded;
    fs::path folder, publicationDirectory;
    std::vector<Wave> waves;
    ~Data() {
        if (!publicationDirectory.empty()) {
            std::error_code ignored;
            fs::remove_all(publicationDirectory, ignored);
        }
    }
};

struct RecordingEdit::Files {
    struct Asset {
        fs::path source,staged,target;
        std::string hash;
        std::optional<FileStamp> sourceProof{}, stageProof{}, publishedProof{};
        std::int64_t mediaId = 0;
        std::optional<Payload> previousMedia{};
        bool reusedOwned = false;
    };
    std::vector<Asset> assets;
    engine::AudioRecordingResult source;
    fs::path publicationDirectory;
    std::shared_ptr<const StagedTake::Data> stagedTake;
    std::shared_ptr<media::detail::PublishedRollback> publications;
    ~Files(){if(!publicationDirectory.empty()){std::error_code ignored;fs::remove_all(publicationDirectory,ignored);}}
    bool committed=false,keepPrivate=false;
};
struct RecordingEdit::PreparedSource {
    std::int64_t mediaId;
    std::optional<OpRequest> mediaEdit;
};
namespace {
std::string identity(Store& store) {
    SQLite::Statement query(store.db(),"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("recording needs a project connection");
}
std::string projectUuid(const SQLite::Database& db) {
    SQLite::Statement query(db,"SELECT value FROM adi_meta WHERE key='project_uuid'");
    return query.executeStep()?query.getColumn(0).getString():std::string{};
}
std::string receiptKey(const std::string& id) {
    return "recording.placed."+id;
}
bool hasReceipt(const SQLite::Database& db,const std::string& id) {
    SQLite::Statement query(db,"SELECT 1 FROM adi_meta WHERE key=?");
    query.bind(1,receiptKey(id));
    return query.executeStep();
}
bool isRecordingDestination(const SQLite::Database& db,std::int64_t id) {
    SQLite::Statement track(db,"SELECT kind,locked FROM tracks WHERE id=?");
    track.bind(1,id);
    if(!track.executeStep() || track.getColumn(1).getInt()!=0)return false;
    const auto kind=track.getColumn(0).getString();
    return kind=="audio" || kind=="midi";
}
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db){if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);}
};
std::string recordingStem(std::string name) {
    // Leave room before the 200-byte portable limit for a device-name prefix,
    // a space, all digits of a uint64_t ordinal, and ".wav".
    constexpr std::size_t maxStemBytes = 174;
    if (name.size() > maxStemBytes) {
        auto end = maxStemBytes;
        while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0U) == 0x80U)
            --end;
        name.resize(end);
    }
    render::NameContext context;
    context.track = std::move(name);
    try {
        return render::resolveName("{track}", context);
    } catch (const std::invalid_argument &) {
        return "Recording";
    }
}
std::string foldedRecordingName(const std::string &name) {
    utf8proc_uint8_t *data = nullptr;
    const auto flags = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
    const auto size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(name.data()),
                                  static_cast<utf8proc_ssize_t>(name.size()), &data, flags);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(data, &std::free);
    if (size < 0)
        return name; // an unrelated directory entry may have an opaque, non-UTF-8 name
    return {reinterpret_cast<const char *>(data), static_cast<std::size_t>(size)};
}
class RecordingFileNames {
    const fs::path &folder_;
    std::map<std::string, std::uint64_t> last_;
public:
    explicit RecordingFileNames(const fs::path &folder) : folder_(folder) {
        for (const auto &entry : fs::directory_iterator(folder_)) {
            const auto name = foldedRecordingName(media::pathUtf8(entry.path().filename()));
            if (!name.ends_with(".wav"))
                continue;
            const auto separator = name.rfind(' ');
            if (separator == std::string::npos || name.size() - separator < 9)
                continue; // a space, at least four digits, and ".wav"
            std::uint64_t ordinal = 0;
            const auto *end = name.data() + name.size() - 4;
            const auto parsed = std::from_chars(name.data() + separator + 1, end, ordinal);
            if (parsed.ec == std::errc{} && parsed.ptr == end) {
                auto &last = last_[name.substr(0, separator)];
                last = std::max(last, ordinal);
            }
        }
    }
    fs::path next(const std::string &trackName) {
        const auto stem = recordingStem(trackName);
        auto &last = last_[foldedRecordingName(stem)];
        while (last < std::numeric_limits<std::uint64_t>::max()) {
            auto digits = std::to_string(++last);
            if (digits.size() < 4)
                digits.insert(0, 4 - digits.size(), '0');
            const auto path = folder_ / media::pathFromUtf8(stem + " " + digits + ".wav");
            // Catch native aliases and entries created after the directory scan.
            std::error_code error;
            const auto status = fs::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found))
                return path;
            if (error)
                throw fs::filesystem_error("inspect recording destination", path, error);
        }
        throw std::runtime_error("recording file numbers are exhausted");
    }
};
bool recordingFileMatches(const fs::path &path, const std::string &hash) {
    if (!fs::is_regular_file(fs::symlink_status(path)))
        return false;
    const auto existing = media::blake3File(path);
    return existing && existing.hex == hash;
}
std::optional<std::pair<fs::path, FileStamp>> verifiedOwnedRecording(
    const fs::path &folder, const Payload &previous, std::stop_token stop) {
    if (previous.at("missing") != 0)
        return {};
    const auto project = folder.parent_path().parent_path();
    const auto owned = fs::weakly_canonical(folder);
    // Resolve in the same order as media::resolveMedia. Only verify a file
    // owned by this project, and honor cancellation during the full read.
    std::vector<fs::path> candidates;
    if (!previous.at("rel_path").is_null()) {
        const auto relative = media::pathFromUtf8(previous.at("rel_path").get<std::string>());
        if (relative.is_absolute()) return {};
        candidates.push_back(project / relative);
    }
    const auto basename = media::pathFromUtf8(previous.at("orig_name").get<std::string>()).filename();
    if (!basename.empty()) candidates.push_back(project / "audio" / basename);
    if (!previous.at("abs_path_hint").is_null()) {
        const auto hint = media::pathFromUtf8(previous.at("abs_path_hint").get<std::string>());
        if (hint.is_absolute()) candidates.push_back(hint);
    }
    for (const auto &path : candidates) {
        std::error_code error;
        const bool exists = fs::exists(path, error);
        if (error) return {};
        if (!exists)
            continue;
        if (fs::weakly_canonical(path.parent_path(), error) != owned || error) return {};
        const auto proof = media::readFileRevision(path, error);
        if (!proof) return {};
        const auto actual = media::blake3File(path, stop);
        if (actual && actual.hex == previous.at("hash_blake3").get_ref<const std::string &>() &&
            media::readFileRevision(path, error) == proof)
            return std::pair{path, *proof};
        return {};
    }
    return {};
}
void bindPublishedRecording(std::vector<OpRequest> &requests, std::int64_t mediaId,
                            const fs::path &target, const fs::path &project, bool reusedOwned) {
    const auto request = std::find_if(requests.begin(), requests.end(), [&](const OpRequest &item) {
        return (item.opType == "media.import" || item.opType == "media.relink") && item.payload.at("id") == mediaId;
    });
    if (request == requests.end())
        throw std::runtime_error("recording media binding has no captured request");
    if (reusedOwned) {
        requests.erase(request);
        return;
    }
    const auto relative = media::pathUtf8(target.lexically_relative(project));
    if (request->opType == "media.import") {
        request->payload["row"]["rel_path"] = relative;
        request->payload["row"]["orig_name"] = media::pathUtf8(target.filename());
    } else {
        request->payload["paths"]["rel_path"] = relative;
    }
}
fs::path reservePublication(const fs::path& parent) {
    static std::atomic<std::uint64_t> serial{0};
    for(unsigned attempt=0;attempt<128;++attempt) {
        const auto path=parent/(".adi-media-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code error;if(fs::create_directory(path,error))return path;
        if(error && error!=std::errc::file_exists)throw fs::filesystem_error("stage recording publication",path,error);
    }
    throw std::runtime_error("cannot reserve recording publication staging");
}
engine::TempoMap tempoOf(Store& store, bool addDefaultLeadIn = true) {
    engine::TempoMap result;SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
    while(points.executeStep()) {
        const auto at=points.getColumn(0).getInt64();const auto bpm=points.getColumn(1).getDouble();
        if(at<0 || !std::isfinite(bpm) || bpm<=0 || bpm>=1000)throw std::runtime_error("recording placement needs a valid tempo map");
        result.events.push_back({at,bpm,points.getColumn(2).getInt()});
    }
    if(result.events.empty() || (addDefaultLeadIn && result.events.front().posTicks!=0))
        result.events.insert(result.events.begin(),{0,120,0});
    return result;
}
std::int64_t tickAt(const engine::TempoMap& tempo,std::int64_t sample,std::uint32_t rate) {
    if(sample<0 || sample>(std::int64_t{1}<<50))throw std::runtime_error("recording exceeds supported timeline sample precision");
    const auto tick=tempo.secondsToTicks(static_cast<double>(sample)/rate);
    const double replayed=tempo.ticksToSeconds(tick)*rate;
    if(!std::isfinite(replayed) || replayed<0 || replayed>0x1p50 || std::llround(replayed)!=sample)
        throw std::runtime_error("recording boundary cannot preserve its sample on this musical clock");
    return tick;
}
void validatePunchBoundary(const engine::TempoMap& tempo,const engine::RecordingPunchWindow::Boundary& boundary,
                           std::uint32_t rate) {
    const auto sample = static_cast<long double>(tempo.ticksToSeconds(boundary.tick)) * rate;
    if(!std::isfinite(sample) || sample < 0 || sample > 0x1p50L || std::llround(sample)!=boundary.sample)
        throw std::runtime_error("the recording clock changed at a punch boundary");
}

struct RecordedPiece {
    std::int64_t id=0,begin=0,end=0;
    std::uint64_t source=0;
    std::string name;
    std::optional<std::pair<std::int64_t,std::int64_t>> placementTicks{};
};
Payload binary(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> data;
    data.reserve(bytes.size());
    for (const auto byte : bytes)
        data.push_back(std::to_integer<std::uint8_t>(byte));
    return Payload::binary(std::move(data));
}

std::vector<std::byte> recordingWarpMap(const engine::TempoMap& tempo, std::int64_t firstTick,
                                         std::int64_t lastTick, std::int64_t frames, std::uint32_t rate) {
    if (frames > std::numeric_limits<std::int32_t>::max())
        return {}; // preserve long takes beyond the current Warp renderer's frame limit
    std::vector<WarpMarker> markers{{0, 0}};
    for (const auto& point : tempo.events) {
        if (point.posTicks <= firstTick || point.posTicks >= lastTick)
            continue;
        const auto tick = point.posTicks - firstTick;
        const auto source = engine::warpSourceSamples(tempo, firstTick, tick, rate);
        if (source > markers.back().sourceFrame && source < frames)
            markers.push_back({source, tick});
    }
    markers.push_back({frames, lastTick - firstTick});
    // Compensated placement is already sample-exact. Keep the original PCM if the
    // current clock cannot reproduce that exact interval through the Warp renderer.
    try {
        const auto projected = engine::projectWarpAnchors(markers, tempo, firstTick, rate);
        if (!std::all_of(projected.begin(), projected.end(), [](const auto& anchor) {
                return anchor.sourceFrame == anchor.outputFrame;
            }))
            return {};
    } catch (const std::runtime_error&) {
        return {};
    }
    auto encoded = audio::writeWarpMap(markers, static_cast<std::uint64_t>(frames));
    return std::move(encoded.bytes);
}

Payload recordedImage(Store& store,const engine::TempoMap& tempo,const RecordedPiece& piece,
    std::int64_t track,std::int64_t media,std::uint32_t rate,std::optional<std::int64_t> lane={}) {
    const auto [first,last]=piece.placementTicks ? *piece.placementTicks
        : std::pair{tickAt(tempo,piece.begin,rate),tickAt(tempo,piece.end,rate)};
    if(last<=first)throw std::runtime_error("recorded pass has no representable musical extent");
    auto row=edit::defaultRow(store.db(),"clips",{{"id",piece.id},{"track_id",track},{"kind","audio"},
        {"name",piece.name},{"time_base",0},{"pos_ticks",first},{"length_ticks",last-first}});
    row["lane_id"]=lane?Payload(*lane):Payload{};
    auto audio=edit::defaultRow(store.db(),"audio_clips",{{"clip_id",piece.id},{"media_id",media},
        {"src_start_frames",static_cast<std::int64_t>(piece.source)},{"src_len_frames",piece.end-piece.begin}});
    if (const auto markers = recordingWarpMap(tempo, first, last, piece.end - piece.begin, rate); !markers.empty()) {
        audio["warp_enabled"] = 1;
        audio["warp_mode"] = "beats";
        audio["warp_markers"] = binary(markers);
    }
    return {{"id",piece.id},{"row",std::move(row)},
        {"children",{{"audio_clips",Payload::array({std::move(audio)})}}},{"alias_links",Payload::array()}};
}

bool validExpressionValue(ExpressionDim dimension, float value) {
    if (!std::isfinite(value))
        return false;
    switch (dimension) {
    case ExpressionDim::Pitch: return value >= -48.0f && value <= 48.0f;
    case ExpressionDim::Pressure:
    case ExpressionDim::Timbre: return value >= 0.0f && value <= 1.0f;
    case ExpressionDim::Gain: return true;
    case ExpressionDim::Pan: return value >= -1.0f && value <= 1.0f;
    }
    return static_cast<unsigned>(dimension) >= 64;
}

Payload recordedMidiImage(Store& store, const MidiLinearTake& take, clip_edit::CopyIds& allocated) {
    if (take.track <= 0 || take.clip <= 0 || take.start < 0 || take.length <= 0 ||
        take.start > std::numeric_limits<std::int64_t>::max() - take.length)
        throw std::runtime_error("invalid linear MIDI recording placement");
    SQLite::Statement track(store.db(), "SELECT kind,locked,frozen FROM tracks WHERE id=?");
    track.bind(1, take.track);
    if (!track.executeStep() || track.getColumn(1).getInt() != 0 || track.getColumn(2).getInt() != 0)
        throw std::runtime_error("MIDI recording target is missing, locked or frozen");
    const auto kind = track.getColumn(0).getString();
    if (kind != "midi" && kind != "instrument")
        throw std::runtime_error("MIDI recording needs a MIDI or instrument track");

    struct NoteExtent { std::int64_t duration; bool expressed = false; };
    std::map<std::uint64_t, NoteExtent> extents;
    for (const auto& note : take.notes) {
        if (note.start < 0 || note.start >= take.length || note.duration <= 0 || note.duration > take.length - note.start)
            throw std::runtime_error("MIDI recorded note lies outside the take");
        if (note.id == 0 || !extents.emplace(note.id, NoteExtent{note.duration}).second)
            throw std::runtime_error("MIDI recorded notes need distinct assigned identities");
    }

    Payload children = Payload::object();
    std::set<std::pair<std::uint64_t, ExpressionDim>> lanes;
    for (const auto& lane : take.expression) {
        const auto note = extents.find(lane.noteId);
        if (note == extents.end() || lane.points.empty() ||
            lane.points.size() > StreamReader<ExpressionPoint>::kMaxRecordsAtOnce ||
            !lanes.emplace(lane.noteId, lane.dimension).second)
            throw std::runtime_error("invalid MIDI recorded expression lane");
        std::int64_t previous = -1;
        for (const auto& point : lane.points) {
            if (point.time_ticks <= previous || point.time_ticks > note->second.duration ||
                !validExpressionValue(lane.dimension, point.value) || !std::isfinite(point.tension) ||
                point.curve > static_cast<std::uint8_t>(Curve::Bezier) ||
                std::any_of(std::begin(point.reserved), std::end(point.reserved), [](auto value) { return value != 0; }))
                throw std::runtime_error("invalid MIDI recorded expression point");
            previous = point.time_ticks;
        }
        note->second.expressed = true;
        if (!children.contains("note_expression"))
            children["note_expression"] = Payload::array();
        children["note_expression"].push_back(edit::defaultRow(store.db(), "note_expression", {
            {"id", allocated.next("note_expression")}, {"clip_id", take.clip},
            {"note_id", static_cast<std::int64_t>(lane.noteId)}, {"dimension", static_cast<int>(lane.dimension)},
            {"data", binary(writeStream<ExpressionPoint>(FourCC::Expression, lane.points))},
        }));
    }

    if (!take.notes.empty()) {
        notes::Diff diff;
        diff.added.reserve(take.notes.size());
        for (auto note : take.notes) {
            note.flags &= ~static_cast<std::uint32_t>(NoteFlags::HasExpression);
            if (extents.at(note.id).expressed)
                note.flags |= NoteFlags::HasExpression;
            std::string error;
            auto record = notes::makeRecord(note, {}, error);
            if (!record)
                throw std::runtime_error(error);
            diff.added.push_back(std::move(*record));
        }
        auto plan = notes::detail::planRecords(notes::detail::emptyNotes(), diff);
        if (!plan.ok || !plan.changed)
            throw std::runtime_error(plan.error);
        children["event_streams"] = Payload::array({edit::defaultRow(store.db(), "event_streams", {
            {"id", allocated.next("event_streams")}, {"clip_id", take.clip},
            {"stream_kind", "notes"}, {"data", binary(plan.bytes)},
        })});
    }

    std::set<std::pair<int, int>> controllerAddresses;
    for (const auto& lane : take.controllers) {
        if (lane.channel < 0 || lane.channel >= 16 || !controllers::validController(lane.controller) ||
            lane.points.empty() || lane.points.size() > StreamReader<AutomationPoint>::kMaxRecordsAtOnce ||
            !controllerAddresses.emplace(lane.channel, lane.controller).second)
            throw std::runtime_error("invalid recorded MIDI controller lane");
        std::vector<AutomationPoint> points;
        std::set<std::uint64_t> pointIds;
        points.reserve(lane.points.size());
        std::int64_t previous = -1;
        for (const auto& point : lane.points) {
            if (!point.id || !pointIds.insert(point.id).second || point.timeTicks < 0 ||
                point.timeTicks < previous || point.timeTicks >= take.length || !std::isfinite(point.value) ||
                point.value < 0. || point.value > 1. || !std::isfinite(point.tension) ||
                point.tension < -1.f || point.tension > 1.f || point.curve > static_cast<std::uint8_t>(Curve::Bezier))
                throw std::runtime_error("invalid recorded MIDI controller point");
            previous = point.timeTicks;
            points.push_back({.time = point.timeTicks, .value = point.value, .tension = point.tension,
                              .curve = point.curve, .flags = point.flags, .reserved = 0, .point_id = point.id});
        }
        if (!children.contains("event_streams"))
            children["event_streams"] = Payload::array();
        const bool pitch = lane.controller == controllers::kPitchBend;
        children["event_streams"].push_back(edit::defaultRow(store.db(), "event_streams", {
            {"id", allocated.next("event_streams")}, {"clip_id", take.clip}, {"channel", lane.channel},
            {"stream_kind", pitch ? "pitchbend" : "cc"}, {"controller", pitch ? Payload(nullptr) : Payload(lane.controller)},
            {"data", binary(writeStream<AutomationPoint>(FourCC::Controller, points, 1, StreamFlags::SortedByTime))},
        }));
    }

    auto row = edit::defaultRow(store.db(), "clips", {
        {"id", take.clip}, {"track_id", take.track}, {"kind", "midi"}, {"time_base", 0},
        {"name", take.name.empty() ? std::string("MIDI Recording") : take.name},
        {"pos_ticks", take.start}, {"length_ticks", take.length},
        {"loop_enabled", 1}, {"loop_start_ticks", 0}, {"loop_len_ticks", take.length},
    });
    return {{"id", take.clip}, {"row", std::move(row)}, {"children", std::move(children)},
            {"alias_links", Payload::array()}};
}

OpRequest recordingRows(const edit::RowEdits& rows, std::int64_t clip, const char* label) {
    OpRequest request;
    request.opType = "arranger.splice";
    request.payload = {{"id", clip}, {"changes", rows.changes()}};
    request.label = label;
    request.targetKind = "clip";
    request.targetId = clip;
    return request;
}

std::optional<std::vector<OpRequest>> prepareControllerOverdub(Store& store, const MidiLinearTake& take,
    std::int64_t clip, std::int64_t offset, clip_edit::CopyIds& ids, const char* label) {
    std::vector<OpRequest> requests;
    if (take.controllers.empty())
        return requests;
    const auto existing = controllers::readLanes(store, clip);
    if (!existing.ok)
        return std::nullopt;
    for (const auto& lane : take.controllers) {
        const auto found = std::find_if(existing.lanes.begin(), existing.lanes.end(), [&](const auto& entry) {
            return entry.channel == lane.channel && entry.controller == lane.controller;
        });
        auto points = std::vector<controllers::Point>(lane.points.begin(), lane.points.end());
        for (auto& point : points)
            point.timeTicks += offset;
        OpRequest request;
        if (found == existing.lanes.end()) {
            request = controllers::writeRequest({ids.next("event_streams"), clip, lane.channel, lane.controller}, points);
        } else {
            if (!found->error.empty() ||
                found->points.size() > StreamReader<AutomationPoint>::kMaxRecordsAtOnce - points.size())
                return std::nullopt;
            // The owner-side writer allocates point IDs without rewriting any existing records.
            for (auto& point : points)
                point.id = 0;
            auto prepared = controllers::prepareWrite(store, clip, lane.channel, lane.controller, points);
            if (!prepared.ok || prepared.address.id != found->id)
                return std::nullopt;
            request = std::move(prepared.request);
        }
        request.label = label;
        request.targetKind = "clip";
        request.targetId = clip;
        requests.push_back(std::move(request));
    }
    return requests;
}

// An in-place overdub needs the recorded span to map to one unique source interval. On a
// repeating, linked, or partially overlapping clip, preserve the take on a new lane.
bool prepareOverdub(Store& store, const MidiLinearTake& take, Payload& image, clip_edit::CopyIds& ids,
                    RecordingEdit& result, MidiLinearLanding& landing, const char* label) {
    const auto end = take.start + take.length;
    const edit::TimeMap time(store.db());
    SQLite::Statement overlaps(store.db(),
        "SELECT id,time_base,alias_of,muted,EXISTS(SELECT 1 FROM clips AS linked WHERE linked.alias_of=clip.id) "
        "FROM clips AS clip WHERE track_id=? AND lane_id IS NULL AND "
        "((time_base=0 AND pos_ticks<? AND pos_ticks+length_ticks>?) OR "
        "(time_base=1 AND pos_ns<? AND pos_ns+length_ns>?)) ORDER BY id");
    overlaps.bind(1, take.track);
    overlaps.bind(2, end);
    overlaps.bind(3, take.start);
    overlaps.bind(4, time.nanos(end));
    overlaps.bind(5, time.nanos(take.start));
    if (!overlaps.executeStep())
        return false; // a gap is an ordinary new recording

    for (const auto& [root, members] : freeze::groups(store.db()))
        if (root == take.track || std::any_of(members.begin(), members.end(),
                [&](const auto& member) { return member.id == take.track; }))
            throw std::runtime_error("cannot overdub inside a frozen group");

    const auto clipId = overlaps.getColumn(0).getInt64();
    const bool independent = overlaps.getColumn(1).getInt() == 0 && overlaps.getColumn(2).isNull() &&
                             overlaps.getColumn(3).getInt() == 0 && overlaps.getColumn(4).getInt() == 0;
    const auto single = !overlaps.executeStep();
    const auto read = single && independent ? engine::readClipDetails(store, clipId, true, true) : engine::ClipDetailsRead{};
    const auto* clip = read.clip ? &*read.clip : nullptr;
    constexpr auto tickLimit = std::int64_t{1} << 50;
    if (clip && clip->kind == "midi" && clip->posTicks && clip->lengthTicks && clip->contentOffsetTicks >= 0 &&
        clip->contentOffsetTicks <= tickLimit && *clip->posTicks >= 0 && *clip->posTicks <= tickLimit && *clip->posTicks <= take.start &&
        *clip->lengthTicks > 0 && *clip->lengthTicks <= tickLimit && end <= *clip->posTicks + *clip->lengthTicks) {
        const auto sourceStart = clip->contentOffsetTicks + (take.start - *clip->posTicks);
        const auto sourceEnd = sourceStart + take.length;
        const bool visible = clip->loopEnabled ?
            clip->loopLengthTicks && *clip->loopLengthTicks > 0 && *clip->loopLengthTicks <= tickLimit &&
                clip->loopStartTicks >= 0 && clip->loopStartTicks <= tickLimit &&
                clip->contentOffsetTicks + *clip->lengthTicks <= clip->loopStartTicks + *clip->loopLengthTicks :
            !clip->endMarkerTicks || sourceEnd <= *clip->endMarkerTicks;
        const bool editable = visible &&
            (take.notes.empty() || clip->notesRecordSize == 0 || clip->notesRecordSize == sizeof(NoteRecord));
        auto controllerOps = editable ? prepareControllerOverdub(store, take, clipId, sourceStart, ids, label) : std::nullopt;
        if (controllerOps) {
            const auto appendControllers = [&] {
                result.requests.insert(result.requests.end(), std::make_move_iterator(controllerOps->begin()),
                                       std::make_move_iterator(controllerOps->end()));
            };
            if (take.notes.empty()) {
                appendControllers();
                landing = {clipId, sourceStart, {}, false};
                return true;
            }
            auto captured = notes::captureNotes(store, clipId);
            if (captured.ok && !captured.hasStream())
                captured = notes::captureNotes(store, clipId, ids.next("event_streams"));
            std::uint64_t highestNoteId = 0;
            std::string error;
            for (const auto& record : captured.records()) {
                const auto note = notes::values(record, error);
                if (!note)
                    throw std::runtime_error(error);
                highestNoteId = std::max(highestNoteId, note->id);
            }
            if (captured.ok && highestNoteId <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - take.notes.size()) {
                std::map<std::uint64_t, std::uint64_t> remap;
                auto& children = image.at("children");
                if (children.contains("note_expression"))
                    for (const auto& row : children.at("note_expression"))
                        remap.emplace(row.at("note_id").get<std::uint64_t>(), 0);
                notes::Diff diff;
                diff.added.reserve(take.notes.size());
                std::vector<std::uint64_t> newIds;
                newIds.reserve(take.notes.size());
                for (auto note : take.notes) {
                    const auto oldId = note.id;
                    note.id = ++highestNoteId;
                    note.start += sourceStart;
                    note.flags &= ~static_cast<std::uint32_t>(NoteFlags::HasExpression);
                    if (const auto found = remap.find(oldId); found != remap.end()) {
                        found->second = note.id;
                        note.flags |= NoteFlags::HasExpression;
                    }
                    auto record = notes::makeRecord(note, {}, error);
                    if (!record)
                        throw std::runtime_error(error);
                    diff.added.push_back(std::move(*record));
                    newIds.push_back(note.id);
                }
                auto prepared = notes::prepareDiff(captured, diff);
                if (!prepared.ok)
                    throw std::runtime_error(prepared.error);
                for (auto& request : prepared.requests) {
                    request.label = label;
                    result.requests.push_back(std::move(request));
                }
                if (!remap.empty()) {
                    edit::RowEdits expressions(store.db());
                    for (auto& row : children.at("note_expression")) {
                        const auto id = row.at("id").get<std::int64_t>();
                        row["clip_id"] = clipId;
                        row["note_id"] = remap.at(row.at("note_id").get<std::uint64_t>());
                        expressions.set("note_expression", id, std::move(row));
                    }
                    result.requests.push_back(recordingRows(expressions, clipId, label));
                }
                appendControllers();
                landing = {clipId, sourceStart, std::move(newIds), false};
                return true;
            }
        }
    }
    if (take.notes.empty() && take.controllers.empty()) {
        landing = {clipId, 0, {}, false};
        return true;
    }
    const auto lane = ids.next("lanes");
    auto laneOp = createTakeLane(store, lane, take.track, "Overdub Take");
    laneOp.label = label;
    result.requests.push_back(std::move(laneOp));
    image.at("row")["lane_id"] = lane;
    edit::RowEdits rows(store.db());
    clip_edit::createImage(rows, image);
    result.requests.push_back(recordingRows(rows, take.clip, label));
    landing.inTakeLane = true;
    return true;
}
}
StagedTake stageTake(const engine::AudioRecordingResult& recorded,const fs::path& projectFolder,std::stop_token stop) {
    StagedTake result;
    try {
        if (audio::onAudioThread || !recorded.hasAttachableAudio() || recorded.loopSpec() ||
            recorded.directory().empty() || recorded.tracks.empty() || recorded.sampleRate < 8000 ||
            recorded.sampleRate > audio::kMaxDecodeRate)
            throw std::runtime_error("recording is not a completed linear take");
        auto staged = std::make_shared<StagedTake::Data>();
        staged->recorded = recorded;
        staged->folder = recordingFolder(projectFolder);
        requireRecordingFolder(staged->folder);
        fs::create_directories(staged->folder);
        staged->publicationDirectory = reservePublication(staged->folder);
        staged->waves.reserve(recorded.tracks.size());
        std::map<std::string,std::size_t> copies;
        for (std::size_t index = 0; index < recorded.tracks.size(); ++index) {
            if (stop.stop_requested()) throw std::runtime_error("recording publication cancelled");
            const auto& track = recorded.tracks[index];
            const auto source = recorded.directory() / ("take-" + std::to_string(index) + ".wav");
            if (track.file != source || !track.channels || track.channels > audio::kMaxDecodeChannels)
                throw std::runtime_error("recording file is not its owned regular take");
            const auto proof = fileStamp(source);
            const auto hash = media::blake3File(source, stop);
            if (!hash) throw std::runtime_error("cannot verify recorded audio");
            if (const auto duplicate = copies.find(hash.hex); duplicate != copies.end()) {
                const auto& original = staged->waves[duplicate->second];
                if (fileStamp(source) != proof || track.channels != recorded.tracks[duplicate->second].channels)
                    throw std::runtime_error("duplicate recording changed or has inconsistent dimensions");
                staged->waves.push_back({source, original.staged, proof, original.stageProof, hash.hex, original.bits});
                continue;
            }
            const auto file = staged->publicationDirectory / ("asset-" + std::to_string(index) + ".wav");
            fs::copy_file(source, file, fs::copy_options::none);
            const auto stageProof = fileStamp(file);
            const auto stagedHash = media::blake3File(file, stop);
            if (!stagedHash || stagedHash.hex != hash.hex || fileStamp(source) != proof || fileStamp(file) != stageProof)
                throw std::runtime_error("recording changed while staging publication");
            audio::WavReader wave(file);
            if (wave.frames() != recorded.frames || wave.sampleRate() != recorded.sampleRate || wave.channels() != track.channels)
                throw std::runtime_error("recording dimensions changed before attachment");
            const auto bits = wave.format() == audio::WavFormat::Pcm16 ? 16 : wave.format() == audio::WavFormat::Pcm24 ? 24 : 32;
            staged->waves.push_back({source, file, proof, stageProof, hash.hex, bits});
            copies.emplace(hash.hex, index);
        }
        result.data_ = std::move(staged);
        result.ok = true;
    } catch (const std::exception& exception) { result.error = exception.what(); }
    return result;
}

bool configureRecovery(Store& store,engine::AudioRecordingSpec& spec,std::string& error) {
    try {
        if(audio::onAudioThread)throw std::runtime_error("configure recording recovery off callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        const auto source=identity(store);
        if(source.empty())throw std::runtime_error("recording recovery needs a saved project");
        auto uuid=projectUuid(store.db());
        if(uuid.empty()) {
            SQLite::Statement persist(store.db(),
                "INSERT INTO adi_meta(key,value) VALUES('project_uuid',?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value WHERE COALESCE(adi_meta.value,'')=''");
            persist.bind(1,store.clientId());
            persist.exec();
            uuid=projectUuid(store.db());
        }
        spec.sourceProject=source;
        spec.recoveryProjectUuid=std::move(uuid);
        error.clear();
        return true;
    } catch(const std::exception& failure) {
        error=failure.what();
        return false;
    }
}
RecoveryTake recoverInterrupted(Store& store,const fs::path& recordingRoot,const fs::path& directory) {
    if (store.readOnly()) return {RecoveryStatus::Refused, {}, "project is read-only"};
    return prepareInterrupted(store,recordingRoot,directory,{},{});
}
RecoveryTake prepareInterrupted(Store& store,const fs::path& recordingRoot,const fs::path& directory,
                                std::string_view expectedRecording,std::stop_token stop) {
    RecoveryTake result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("recover recording off callback");
        for(const auto& component:directory) {
            if(component=="..")
                throw std::runtime_error("recording path contains a parent traversal");
        }
        if(recordingRoot.empty() || directory.empty() || directory.filename().empty() ||
           fs::canonical(recordingRoot)!=fs::canonical(directory.parent_path()))
            throw std::runtime_error("recording directory is outside the known recording root");
        const auto root=fs::canonical(recordingRoot);
        const auto ownedDirectory=root/directory.filename();
        auto owned=engine::recording_recovery::openInactive(ownedDirectory);
        if (!expectedRecording.empty() && owned.plan.recordingId != expectedRecording)
            throw std::runtime_error("the interrupted take changed after launch");
        const auto source=identity(store);
        if(owned.plan.projectUuid!=projectUuid(store.db()) || owned.plan.sourceProject!=source)
            throw std::runtime_error("recording belongs to a different project");
        if(hasReceipt(store.db(),owned.plan.recordingId)) {
            result.status=RecoveryStatus::AlreadyPlaced;
            return result;
        }
        for(const auto& target:owned.plan.targets) {
            if(!isRecordingDestination(store.db(),target.trackId))
                throw std::runtime_error("recording track "+std::to_string(target.trackId)+" no longer accepts a take");
        }
        result.capture=engine::AudioRecordingResult::fromInterrupted(std::move(owned),ownedDirectory,source,stop);
        result.status=RecoveryStatus::Ready;
    } catch(const std::exception& failure) {
        result.error=failure.what();
    }
    return result;
}
std::vector<engine::recording_recovery::LaunchTake> discoverInterrupted() {
    auto candidates = engine::recording_recovery::findLaunchTakes();
    std::vector<engine::recording_recovery::LaunchTake> admitted;
    for (auto &candidate : candidates) {
        try {
            StoreError error{};
            const auto project=Store::open(media::pathFromUtf8(candidate.sourceProject),error,true);
            if (!project || candidate.sourceProject!=identity(*project) ||
                candidate.projectUuid!=projectUuid(project->db()) ||
                hasReceipt(project->db(),candidate.recordingId)) continue;
            auto &db = project->db();
            SQLite::Statement title(db, "SELECT name FROM project WHERE id=1");
            if (title.executeStep()) candidate.projectName = title.getColumn(0).getString();
            SQLite::Statement track(db, "SELECT name FROM tracks WHERE id=?");
            for (const auto id : candidate.tracks) {
                track.bind(1, id);
                auto name = track.executeStep() ? track.getColumn(0).getString() : std::string{};
                if (name.empty()) name = "Track " + std::to_string(id);
                if (!candidate.trackNames.empty()) candidate.trackNames += ", ";
                candidate.trackNames += name;
                track.reset();
            }
            admitted.push_back(std::move(candidate));
        } catch(const std::exception&) {
            // A candidate whose project cannot be read stays in its private
            // recording folder, available when the project opens again.
        }
    }
    return admitted;
}
engine::AudioRecordingResult captureMemory(Store& store,const engine::AudioMemorySnapshot& memory,
    std::int64_t track,std::int64_t timelineStartSample,const fs::path& workFolder) {
    try {
        if(audio::onAudioThread)throw std::runtime_error("capture audio memory off callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        return engine::AudioRecordingResult::fromMemory(memory,{workFolder,identity(store),track,timelineStartSample});
    }catch(const std::exception& exception) {
        engine::AudioRecordingResult result;result.failure=engine::RecordingFailure::InvalidCapture;result.error=exception.what();return result;
    }
}
std::vector<RecordingEdit::PreparedSource> RecordingEdit::prepareSources(Store& store,const engine::AudioRecordingResult& recorded,
    std::span<const ClipIds> ids,RecordingEdit& result,std::span<const std::size_t> sourceIndices,
    const StagedTake* staged) {
    if(store.readOnly())throw std::runtime_error("project is read-only");
    if(audio::onAudioThread)throw std::runtime_error("prepare recording off callback");
    result.sourceProject=identity(store);result.owner_=&store;result.client_=store.clientId();
    if(recorded.sourceProject().empty() || recorded.sourceProject()!=result.sourceProject)throw std::runtime_error("recording belongs to a different project");
    if(!recorded.recordingId().empty()) {
        if(recorded.recoveryProjectUuid()!=projectUuid(store.db()))
            throw std::runtime_error("recording project identity changed");
        if(hasReceipt(store.db(),recorded.recordingId()))
            throw std::runtime_error("recording is already placed");
    }
    if(!recorded.hasAttachableAudio() || recorded.sampleRate<8000 || recorded.sampleRate>768000 ||
       recorded.startSample<0 || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-recorded.startSample) ||
       ids.size()!=recorded.tracks.size() || ids.empty() || recorded.directory().empty())throw std::runtime_error("recording is not a completed attachable take");
    result.options.expectHead.emplace(OpJournal(store).headSeq());
    const auto projectFolder = media::projectFolder(store);
    const auto folder = recordingFolder(projectFolder);
    requireRecordingFolder(folder);
    auto files=std::make_shared<RecordingEdit::Files>();files->source=recorded;result.files_=files;
    if (staged) {
        if (!staged->data_ || staged->data_->folder != folder || staged->data_->waves.size() != ids.size())
            throw std::runtime_error("staged recording belongs to a different project folder");
        files->stagedTake = staged->data_;
    } else {
        fs::create_directories(folder);
    }
    const auto ownedFolder=fs::weakly_canonical(folder);
    RecordingFileNames names(folder);
    const auto chosen = sourceIndices.empty() ? ids.size() : sourceIndices.size();
    // Keep the original WAVs available for tracks that could not be placed.
    files->keepPrivate = chosen != ids.size();
    std::set<std::int64_t> clipIds,mediaIds;std::map<std::string,std::int64_t> capturedMedia;
    std::vector<PreparedSource> sources;
    sources.reserve(chosen);
    for(std::size_t ordinal=0;ordinal<chosen;++ordinal) {
        const auto index = sourceIndices.empty() ? ordinal : sourceIndices[ordinal];
        const auto& target=recorded.tracks[index];const auto& wanted=ids[index];
        if(wanted.clip<=0 || wanted.media<=0 || !clipIds.insert(wanted.clip).second || !mediaIds.insert(wanted.media).second)
            throw std::runtime_error("recording needs distinct positive clip and media allocation ids");
        const auto expected=recorded.directory()/("take-"+std::to_string(index)+".wav");
        if(target.file!=expected || !fs::is_regular_file(fs::symlink_status(expected)))throw std::runtime_error("recording file is not its owned regular take");
        SQLite::Statement track(store.db(),"SELECT kind,locked,name FROM tracks WHERE id=?");track.bind(1,target.trackId);
        if(!track.executeStep() || (track.getColumn(0).getString()!="audio" && track.getColumn(0).getString()!="midi") || track.getColumn(1).getInt()!=0)
            throw std::runtime_error("recording target is gone, locked or not a regular track");
        const auto trackName = wanted.trackName.empty() ? track.getColumn(2).getString() : wanted.trackName;
        if (staged) {
            // The worker established exact bytes and native WAV dimensions.
            // Reads here are project metadata only, even for a reused hash.
            const auto& wave = files->stagedTake->waves[index];
            if (const auto duplicate = capturedMedia.find(wave.hash); duplicate != capturedMedia.end()) {
                files->assets.push_back({wave.source, {}, {}, wave.hash, wave.sourceProof});
                sources.push_back({duplicate->second, std::nullopt});
                continue;
            }
            SQLite::Statement known(store.db(), "SELECT id,embedded OR EXISTS(SELECT 1 FROM media_blobs WHERE media_id=m.id) "
                                                "FROM media_files m WHERE hash_blake3=?");
            known.bind(1, wave.hash);
            const bool reused = known.executeStep();
            const auto mediaId = reused ? known.getColumn(0).getInt64() : wanted.media;
            const auto file = names.next(trackName);
            Payload paths{{"rel_path", media::pathUtf8(file.lexically_relative(projectFolder))}, {"abs_path_hint", nullptr}, {"missing", 0}};
            OpRequest request;
            request.label = "Record Audio";
            std::optional<Payload> previous;
            if (reused) {
                if (known.getColumn(1).getInt() != 0) throw std::runtime_error("extract embedded media first");
                // Undo may restore an original locator into this private take.
                previous = media::mediaRow(store.db(), mediaId);
                for (const auto* column : {"rel_path", "abs_path_hint"}) {
                    if (previous->at(column).is_null()) continue;
                    const auto old = media::pathFromUtf8(previous->at(column).get<std::string>());
                    if (fs::weakly_canonical((projectFolder / old).parent_path()) == fs::weakly_canonical(recorded.directory()))
                        files->keepPrivate = true;
                }
                request.opType = "media.relink";
                request.payload = {{"id", mediaId}, {"hash", wave.hash}, {"paths", std::move(paths)}};
            } else {
                const auto frames = static_cast<std::int64_t>(recorded.frames);
                const auto rate = static_cast<std::int64_t>(recorded.sampleRate);
                constexpr std::int64_t billion = 1000000000;
                if (frames / rate > (std::numeric_limits<std::int64_t>::max() - billion) / billion)
                    throw std::runtime_error("audio duration exceeds the supported timeline");
                const auto duration = (frames / rate) * billion + (frames % rate) * billion / rate;
                if (wave.stageProof.size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
                    throw std::runtime_error("media size exceeds signed 64-bit range");
                paths.update({{"hash_blake3", wave.hash}, {"orig_name", media::pathUtf8(file.filename())},
                              {"sample_rate", rate}, {"channels", target.channels}, {"frames", frames},
                              {"duration_ns", duration}, {"format", "wav"}, {"bit_depth", wave.bits},
                              {"size_bytes", static_cast<std::int64_t>(wave.stageProof.size)}, {"imported_utc", 0}, {"peaks", nullptr}});
                request.opType = "media.import";
                request.payload = {{"id", mediaId}, {"row", std::move(paths)}};
            }
            capturedMedia.emplace(wave.hash, mediaId);
            files->assets.push_back({wave.source, wave.staged, file, wave.hash, wave.sourceProof, wave.stageProof,
                                     {}, mediaId, std::move(previous)});
            sources.push_back({mediaId, std::move(request)});
            continue;
        }
        {audio::WavReader source(expected);if(source.frames()!=recorded.frames || source.sampleRate()!=recorded.sampleRate || source.channels()!=target.channels)
            throw std::runtime_error("recording dimensions changed before attachment");}
        const auto hash=media::blake3File(expected);if(!hash)throw std::runtime_error("cannot verify recorded audio");
        std::int64_t mediaId=0;
        std::optional<OpRequest> mediaEdit;
        if(const auto found=capturedMedia.find(hash.hex);found!=capturedMedia.end()) {
            mediaId=found->second;files->assets.push_back({expected,{},{},hash.hex});
        }
        else {
            OpRequest request;const auto imported=media::prepareImport(store,expected,wanted.media,0,request);
            if(!imported.ok || !imported.audio)throw std::runtime_error(imported.error.empty()?"cannot capture recorded media":imported.error);
            if(imported.audio->frames!=static_cast<std::int64_t>(recorded.frames) || imported.audio->sampleRate!=recorded.sampleRate || imported.audio->channels!=target.channels)
                throw std::runtime_error("recording dimensions changed during media capture");
            mediaId=imported.mediaId;
            const auto existing=imported.existing?media::mediaRow(store.db(),mediaId):Payload{};
            const auto& capturedHash=imported.existing?existing.at("hash_blake3"):request.payload.at("row").at("hash_blake3");
            if(capturedHash!=hash.hex)throw std::runtime_error("recorded file changed during media capture");
            capturedMedia.emplace(hash.hex,mediaId);
            if(imported.existing) {
                std::string pathError;
                const auto oldPath=media::resolveMedia(projectFolder,existing,pathError);
                if(!oldPath.empty()) {
                    const auto oldFolder=fs::weakly_canonical(oldPath.parent_path());
                    if(oldFolder==fs::weakly_canonical(recorded.directory()))files->keepPrivate=true;
                    if(request.opType.empty() && oldFolder==ownedFolder &&
                       fs::is_regular_file(fs::symlink_status(oldPath))) {
                        // The pool already has this content on the project volume.
                        // Revalidate the file at commit without moving or duplicating it.
                        files->assets.push_back({expected,{},oldPath,hash.hex});
                        sources.push_back({mediaId,{}});
                        continue;
                    }
                }
            }
            const auto targetFile=names.next(trackName);
            const auto relative=media::pathUtf8(targetFile.lexically_relative(projectFolder));
            Payload paths={{"rel_path",relative},{"abs_path_hint",nullptr},{"missing",0}};
            if(request.opType=="media.import") {
                request.payload["row"].update(paths);
                request.payload["row"]["orig_name"]=media::pathUtf8(targetFile.filename());
            } else {request.opType="media.relink";request.payload={{"id",mediaId},{"hash",hash.hex},{"paths",paths}};}
            if(files->publicationDirectory.empty())files->publicationDirectory=reservePublication(folder);
            const auto stageFile=files->publicationDirectory/("asset-"+std::to_string(index)+".wav");fs::copy_file(expected,stageFile,fs::copy_options::none);
            const auto stagedHash=media::blake3File(stageFile);if(!stagedHash || stagedHash.hex!=hash.hex)throw std::runtime_error("recording changed while staging publication");
            files->assets.push_back({expected,stageFile,targetFile,hash.hex});
            request.label="Record Audio";
            mediaEdit=std::move(request);
        }
        sources.push_back({mediaId,std::move(mediaEdit)});
    }
    return sources;
}
RecordingEdit prepareCommit(Store& store,const engine::AudioRecordingResult& recorded,std::span<const ClipIds> ids,
                            std::int64_t placementOffsetSamples) {
    return prepareCommit(store, recorded, ids, {}, placementOffsetSamples);
}

RecordingEdit prepareCommit(Store& store, const engine::AudioRecordingResult& recorded, std::span<const ClipIds> ids,
                            std::span<const MidiLinearTake> midi, std::int64_t placementOffsetSamples) {
    return RecordingEdit::prepareLinear(store, &recorded, ids, midi, recorded.sampleRate, placementOffsetSamples);
}

RecordingEdit prepareMidiLinearCommit(Store& store, std::span<const MidiLinearTake> midi, double sampleRate) {
    return RecordingEdit::prepareLinear(store, nullptr, {}, midi, sampleRate, 0);
}

RecordingEdit RecordingEdit::prepareLinear(Store& store, const engine::AudioRecordingResult* audioTake,
                                           std::span<const ClipIds> ids, std::span<const MidiLinearTake> midi,
                                           double sampleRate, std::int64_t placementOffsetSamples, const StagedTake* staged) {
    RecordingEdit result;
    try {
        if (store.readOnly())
            throw std::runtime_error("project is read-only");
        if (audio::onAudioThread)
            throw std::runtime_error("prepare recording off callback");
        if (!audioTake && midi.empty())
            throw std::runtime_error("recording has no takes to place");
        Read read(store.db());
        const char* label = midi.empty() ? "Record Audio" : audioTake ? "Record" : "Record MIDI";
        clip_edit::CopyIds allocated(store.db());
        std::vector<std::int64_t> trackIds;
        edit_plan::detail::PlacementBatch batch;
        auto first = std::numeric_limits<std::int64_t>::max();
        std::int64_t last = 0;
        const auto place = [&](Payload image, std::int64_t track, std::int64_t begin, std::int64_t end) {
            trackIds.push_back(track);
            batch.incoming.push_back(std::move(image));
            batch.clear.push_back({{track, std::nullopt}, {begin, end}});
            first = std::min(first, begin);
            last = std::max(last, end);
        };
        if (audioTake) {
            const auto& recorded = *audioTake;
            if (recorded.loopSpec())
                throw std::runtime_error("loop recording needs take-pass publication; linear attachment is not permitted");
            const auto& punch = recorded.punchWindow();
            const auto tempo = tempoOf(store, !punch.active());
            constexpr auto ceiling = std::numeric_limits<std::int64_t>::max();
            if (recorded.frames > static_cast<std::uint64_t>(ceiling) || recorded.startSample < 0 ||
                recorded.frames > static_cast<std::uint64_t>(ceiling - recorded.startSample) ||
                recorded.sampleRate < 8000 || recorded.sampleRate > 768000)
                throw std::runtime_error("recording is not a completed attachable take");
            if (ids.empty() || ids.size() != recorded.tracks.size())
                throw std::runtime_error("recording needs one clip and media allocation per captured track");
            struct Placement {
                engine::RecordedPass range;
                std::int64_t beginTick, endTick;
            };
            std::vector<Placement> placements;
            std::vector<std::size_t> sourceIndices;
            placements.reserve(ids.size());
            sourceIndices.reserve(ids.size());
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto offset = recorded.tracks[index].placementOffsetSamples.value_or(placementOffsetSamples);
                const auto range = engine::recordingPlacementRange(recorded.startSample, recorded.frames, offset, punch);
                if (!range) {
                    // A corrected start the song clock can hold, with no frame left to place: every
                    // frame fell outside the punch, or before the song's start.
                    const auto corrected = engine::recordingPlacementStart(recorded.startSample, offset);
                    const bool fits = corrected && *corrected <= ceiling - static_cast<std::int64_t>(recorded.frames);
                    if (punch.active() && fits)
                        continue;
                    if (fits)
                        throw std::runtime_error("the take ended before its latency-corrected start");
                    throw std::runtime_error("recording latency placement overflows the song clock");
                }
                // Several ticks can project to the same sample. Prefer the authored brace at a clipped edge.
                const auto beginTick = punch.in && range->timelineStartSample == punch.in->sample
                    ? punch.in->tick : tickAt(tempo, range->timelineStartSample, recorded.sampleRate);
                const auto endTick = punch.out && range->timelineEndSample == punch.out->sample
                    ? punch.out->tick : tickAt(tempo, range->timelineEndSample, recorded.sampleRate);
                if (endTick <= beginTick)
                    throw std::runtime_error("recording has no representable musical extent");
                placements.push_back({*range, beginTick, endTick});
                sourceIndices.push_back(index);
            }
            if (placements.empty()) {
                result.audioOutsidePunch = true;
                throw std::runtime_error("no audio remains within the compensated punch range");
            }
            if (punch.in)
                validatePunchBoundary(tempo, *punch.in, recorded.sampleRate);
            if (punch.out)
                validatePunchBoundary(tempo, *punch.out, recorded.sampleRate);
            auto sources = prepareSources(store, recorded, ids, result, sourceIndices, staged);
            for (std::size_t ordinal = 0; ordinal < placements.size(); ++ordinal) {
                const auto index = sourceIndices[ordinal];
                const auto& target = recorded.tracks[index];
                const auto& wanted = ids[index];
                const auto& [range, beginTick, endTick] = placements[ordinal];
                auto& source = sources[ordinal];
                if (source.mediaEdit) {
                    source.mediaEdit->label = label;
                    result.requests.push_back(std::move(*source.mediaEdit));
                }
                const RecordedPiece take{wanted.clip, range.timelineStartSample, range.timelineEndSample,
                    range.sourceStartFrame, wanted.name.empty() ? std::string("Recording") : wanted.name,
                    std::pair{beginTick, endTick}};
                place(recordedImage(store, tempo, take, target.trackId, source.mediaId, recorded.sampleRate),
                      target.trackId, beginTick, endTick);
                result.placedAudioClips.push_back(wanted.clip);
            }
        } else {
            result.sourceProject = identity(store);
            result.owner_ = &store;
            result.client_ = store.clientId();
            result.options.expectHead.emplace(OpJournal(store).headSeq());
            result.files_ = std::make_shared<Files>();
        }
        for (const auto& take : midi) {
            auto image = recordedMidiImage(store, take, allocated);
            MidiLinearLanding landing;
            landing.clip = take.clip;
            landing.noteIds.reserve(take.notes.size());
            for (const auto& note : take.notes)
                landing.noteIds.push_back(note.id);
            if (!take.overdub || !prepareOverdub(store, take, image, allocated, result, landing, label))
                place(std::move(image), take.track, take.start, take.start + take.length);
            result.midiLandings.push_back(std::move(landing));
        }
        if (batch.incoming.empty()) {
            result.ok = true;
            return result;
        }

        // Resolve every replacement in one splice. Existing material is cut
        // around the take and any surviving fragments get distinct row IDs.
        auto scope = edit_plan::materialScope(trackIds, edit_plan::PlacementClock::Ticks, {first, last});
        scope.domains = edit_plan::Domains::Clips;
        edit_plan::Options placement{sampleRate, {}};
        placement.policy.automationFollowsClips = false;
        auto prepared = edit_plan::detail::prepareCaptured(store, scope, std::move(batch), placement, allocated, label);
        if (!prepared.ok)
            throw std::runtime_error(prepared.error);
        const auto changes = edit_plan::describe(prepared);
        if (!changes.ok)
            throw std::runtime_error(changes.error);
        for (auto& request : prepared.requests)
            result.requests.push_back(std::move(request));

        // The comp still replaces just the covered span. Keep each displaced
        // original whole in a lane from the same transaction, including the
        // offsets, fades, note streams and expression it had before the cut.
        std::map<std::int64_t, std::int64_t> lanes;
        OpContext context{store, store.db()};
        edit::RowEdits archived(store.db());
        for (const auto& effect : changes.effects) {
            if (!effect.before || effect.before->lane.lane)
                continue;
            const auto track = effect.before->lane.track;
            auto original = clip_edit::captureImage(context, effect.before->id);
            // A linked clip does not own the material it plays. Let the
            // normal splice manage its owner's references without adding one.
            if (!original.at("row").at("alias_of").is_null())
                continue;
            auto [lane, inserted] = lanes.try_emplace(track, 0);
            if (inserted) {
                lane->second = allocated.next("lanes");
                auto request = createTakeLane(store, lane->second, track, "Previous Take");
                request.label = label;
                result.requests.push_back(std::move(request));
            }
            auto copy = clip_edit::cloneImage(original, allocated.next("clips"), allocated);
            copy.at("row")["lane_id"] = lane->second;
            clip_edit::createImage(archived, copy);
        }
        if (!lanes.empty())
            result.requests.push_back(recordingRows(archived, 0, label));
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}
RecordingEdit prepareStagedCommit(Store& store,const StagedTake& staged,std::span<const ClipIds> ids,
                                  std::int64_t placementOffsetSamples) {
    return prepareStagedCommit(store, staged, ids, {}, placementOffsetSamples);
}

RecordingEdit prepareStagedCommit(Store& store,const StagedTake& staged,std::span<const ClipIds> ids,
                                  std::span<const MidiLinearTake> midi,std::int64_t placementOffsetSamples) {
    if (!staged.ok || !staged.data_) {
        RecordingEdit refused;
        refused.error = staged.error.empty() ? "recording staging has not completed" : staged.error;
        return refused;
    }
    const auto& recorded = staged.data_->recorded;
    return RecordingEdit::prepareLinear(store, &recorded, ids, midi, recorded.sampleRate, placementOffsetSamples, &staged);
}
namespace {
// Existing lanes are considered in visible order, once per capture. A lane
// whose native extent cannot prove it is wholly before the pass is not reused.
struct ReusableLane {
    std::int64_t id=0,tickEnd=0,nanoEnd=0;
    bool valid=true,hasTicks=false,hasNanos=false,used=false;
};
ReusableLane inspectReusableLane(SQLite::Database& db,std::int64_t id,std::int64_t track) {
    ReusableLane lane;lane.id=id;
    SQLite::Statement clips(db,"SELECT track_id,time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips WHERE lane_id=?");
    clips.bind(1,id);
    while(clips.executeStep()) {
        if(!clips.getColumn(0).isInteger() || clips.getColumn(0).getInt64()!=track || !clips.getColumn(1).isInteger()) {
            lane.valid=false;return lane;
        }
        const auto base=clips.getColumn(1).getInt64();
        if(base!=0 && base!=1){lane.valid=false;return lane;}
        const int position=base==0?2:4,length=position+1;
        if(!clips.getColumn(position).isInteger() || !clips.getColumn(length).isInteger()) {lane.valid=false;return lane;}
        const auto begin=clips.getColumn(position).getInt64(),count=clips.getColumn(length).getInt64();
        if(begin<0 || count<0 || begin>std::numeric_limits<std::int64_t>::max()-count) {lane.valid=false;return lane;}
        const auto end=edit::addTime(begin,count);
        if(base==0){lane.hasTicks=true;lane.tickEnd=std::max(lane.tickEnd,end);}
        else {lane.hasNanos=true;lane.nanoEnd=std::max(lane.nanoEnd,end);}
    }
    return lane;
}
std::optional<std::int64_t> captureStartNanos(std::int64_t sample,std::uint32_t rate) {
    // The clip's tick placement round-trips to this sample. Use the original
    // sample clock for physical extents, whose exact boundary can lie between ticks.
    constexpr std::int64_t billion=1000000000;
    const auto seconds=sample/rate,remainder=sample%rate;
    const auto fraction=remainder*billion/rate;
    if(seconds>(std::numeric_limits<std::int64_t>::max()-fraction)/billion)return {};
    return seconds*billion+fraction;
}
std::optional<std::int64_t> takeReusableLane(std::vector<ReusableLane>& lanes,
    std::int64_t tick,std::optional<std::int64_t> nanos) {
    for(auto& lane:lanes) {
        if(lane.used || !lane.valid || (lane.hasTicks && lane.tickEnd>tick) ||
            (lane.hasNanos && (!nanos || lane.nanoEnd>*nanos)))continue;
        lane.used=true;return lane.id;
    }
    return {};
}
// Both packets are complete changes from the same unchanged read snapshot.
// Derive a sequential native splice, without applying anything while preparing.
Payload passDelta(const Payload& previous,const Payload& next) {
    using Key=std::pair<std::string,std::string>;
    std::map<Key,std::pair<const Payload*,const Payload*>> rows;
    for(const auto& change:previous)rows[{change.at("table").get<std::string>(),change.at("id").dump()}].first=&change;
    for(const auto& change:next)rows[{change.at("table").get<std::string>(),change.at("id").dump()}].second=&change;
    Payload result=Payload::array();
    for(const auto& [key,sides]:rows) {
        (void)key;const auto* old=sides.first;const auto* now=sides.second;
        if(old && now && old->at("before")!=now->at("before"))throw std::runtime_error("recording placement snapshots differ");
        const auto& before=old?old->at("after"):now->at("before");
        const auto& after=now?now->at("after"):old->at("before");
        if(before==after)continue;
        const auto& identity=now?*now:*old;
        result.push_back({{"table",identity.at("table")},{"id",identity.at("id")},{"before",before},{"after",after}});
    }
    return result;
}
void replaceRecordedSpan(std::vector<RecordedPiece>& pieces,RecordedPiece incoming,clip_edit::CopyIds& ids) {
    std::vector<RecordedPiece> after;
    for(const auto& piece:pieces) {
        if(piece.end<=incoming.begin || piece.begin>=incoming.end){after.push_back(piece);continue;}
        if(piece.begin<incoming.begin){auto left=piece;left.end=incoming.begin;after.push_back(std::move(left));}
        if(piece.end>incoming.end) {
            auto right=piece;right.source+=static_cast<std::uint64_t>(incoming.end-piece.begin);right.begin=incoming.end;
            if(piece.begin<incoming.begin)right.id=ids.next("clips");
            after.push_back(std::move(right));
        }
    }
    after.push_back(std::move(incoming));pieces=std::move(after);
}
}
RecordingEdit prepareLoopCommit(Store& store,const engine::AudioRecordingResult& recorded,
    std::span<const LoopTrackIds> ids,std::span<const std::optional<Payload>> selections) {
    RecordingEdit result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("prepare recording off callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        Read read(store.db());const auto& loop=recorded.loopSpec();const auto passes=recorded.passes();
        if(recorded.sampleRate<8000 || recorded.sampleRate>768000 || !loop || passes.empty() || passes.size()>loop->maxPasses || loop->beginSample<0 || loop->endSample<=loop->beginSample ||
           ids.empty() || ids.size()!=recorded.tracks.size() || (!selections.empty() && selections.size()!=passes.size()))
            throw std::runtime_error("loop recording needs its complete pass log, target ids and optional per-pass selection");
        std::uint64_t consumed=0;
        for(std::size_t pass=0;pass<passes.size();++pass) {
            const auto& p=passes[pass];
            if(p.frames==0 || p.sourceStartFrame!=consumed || consumed>recorded.frames || p.frames>recorded.frames-consumed ||
               p.timelineStartSample<0 || p.timelineEndSample<=p.timelineStartSample || p.timelineEndSample>loop->endSample ||
               static_cast<std::uint64_t>(p.timelineEndSample-p.timelineStartSample)!=p.frames ||
               (pass==0?p.timelineStartSample!=recorded.startSample:
                   (p.timelineStartSample!=loop->beginSample || passes[pass-1].timelineEndSample!=loop->endSample)))
                throw std::runtime_error("loop pass log is not a contiguous capture with exact wrapped intervals");
            consumed+=p.frames;
        }
        if(consumed!=recorded.frames || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("loop pass log does not cover its complete source");
        clip_edit::CopyIds allocated(store.db());
        std::set<std::int64_t> lanes,clips,tracks;
        std::vector<ClipIds> sources;std::vector<std::int64_t> trackIds;
        std::vector<std::vector<std::int64_t>> laneOrders(ids.size());
        std::vector<std::vector<ReusableLane>> reusable(ids.size());
        std::vector<std::size_t> newLaneCounts(ids.size());
        for(std::size_t track=0;track<ids.size();++track) {
            const auto& wanted=ids[track];const auto trackId=recorded.tracks[track].trackId;
            if(wanted.passes.size()!=passes.size() || !tracks.insert(trackId).second)
                throw std::runtime_error("loop materialization needs distinct targets and one fallback lane/new clip per pass");
            for(const auto& pass:wanted.passes) {
                if(pass.lane<=0 || pass.clip<=0 || !lanes.insert(pass.lane).second || !clips.insert(pass.clip).second ||
                   edit::row(store.db(),"lanes",pass.lane) || edit::row(store.db(),"clips",pass.clip))
                    throw std::runtime_error("loop materialization needs unoccupied distinct positive lane and clip ids");
                allocated.reserve("clips",pass.clip);
            }
            sources.push_back({wanted.passes.front().clip,wanted.media,{},wanted.trackName});trackIds.push_back(trackId);
            SQLite::Statement existing(store.db(),"SELECT id FROM lanes WHERE track_id=? AND kind='take' ORDER BY ord,id");
            existing.bind(1,trackId);
            while(existing.executeStep()) {
                const auto id=existing.getColumn(0).getInt64();
                laneOrders[track].push_back(id);
                reusable[track].push_back(inspectReusableLane(store.db(),id,trackId));
            }
        }
        // Legacy direct recorder callers use the playback convention even
        // when the first authored tempo event occurs after song start.
        const auto authoredTempo=tempoOf(store, false);
        if(loop->tempo && loop->tempo->events!=authoredTempo.events)
            throw std::runtime_error("the project tempo changed after the loop recording started");
        const auto& tempo=loop->tempo ? *loop->tempo : authoredTempo;
        for(const auto& pass:passes) {
            (void)tickAt(tempo,pass.timelineStartSample,recorded.sampleRate);
            (void)tickAt(tempo,pass.timelineEndSample,recorded.sampleRate);
        }
        auto sourceEdits=RecordingEdit::prepareSources(store,recorded,sources,result);
        for(auto& source:sourceEdits)
            if(source.mediaEdit)result.requests.push_back(std::move(*source.mediaEdit));
        std::vector<std::vector<RecordedPiece>> main(ids.size());Payload previous=Payload::array();
        std::optional<std::int64_t> earliest,latest;
        edit_plan::Options placement{static_cast<double>(recorded.sampleRate),{}};
        placement.policy.automationFollowsClips=false; // Audio recording does not rewrite authored automation.
        bool firstGroup=true;
        for(std::size_t pass=0;pass<passes.size();++pass) {
            // Each track's pass as it was heard: the round-trip correction reads the file later.
            std::vector<std::optional<engine::RecordedPass>> heard(ids.size());
            for(std::size_t track=0;track<ids.size();++track) {
                const auto offset=recorded.tracks[track].placementOffsetSamples.value_or(recorded.placementOffsetSamples);
                heard[track]=engine::loopPassPlacement(passes[pass],recorded.frames,offset);
                if(!heard[track])continue;
                earliest=std::min(earliest.value_or(heard[track]->timelineStartSample),heard[track]->timelineStartSample);
                latest=std::max(latest.value_or(heard[track]->timelineEndSample),heard[track]->timelineEndSample);
            }
            // A last pass shorter than the correction holds nothing that was heard in it.
            if(std::none_of(heard.begin(),heard.end(),[](const auto& item){return item.has_value();}))continue;
            const auto groupStart=firstGroup?std::size_t{0}:result.requests.size();firstGroup=false;
            edit_plan::detail::PlacementBatch batch;
            const auto first=tickAt(tempo,*earliest,recorded.sampleRate),last=tickAt(tempo,*latest,recorded.sampleRate);
            auto scope=edit_plan::materialScope(trackIds,edit_plan::PlacementClock::Ticks,{first,last});
            scope.domains=edit_plan::Domains::Clips;
            for(std::size_t track=0;track<ids.size();++track) {
                const auto trackId=recorded.tracks[track].trackId;
                if(const auto& captured=heard[track]) {
                    const auto& wanted=ids[track].passes[pass];
                    const auto startTick=tickAt(tempo,captured->timelineStartSample,recorded.sampleRate);
                    const auto startNanos=captureStartNanos(captured->timelineStartSample,recorded.sampleRate);
                    const auto name=wanted.name.empty()?(pass==0?std::string("Audio"):"Audio "+std::to_string(pass+1)):wanted.name;
                    const auto reused=takeReusableLane(reusable[track],startTick,startNanos);
                    const auto laneId=reused.value_or(wanted.lane);
                    if(!reused) {
                        result.requests.push_back(createTakeLane(store,laneId,trackId));
                        auto& lanesInOrder=laneOrders[track];auto& created=newLaneCounts[track];
                        lanesInOrder.insert(lanesInOrder.begin()+static_cast<std::ptrdiff_t>(created),laneId);
                        if(created!=0) {
                            OpRequest order;order.opType="lane.reorder";
                            order.payload={{"track",trackId},{"lanes",lanesInOrder}};
                            result.requests.push_back(std::move(order));
                        }
                        ++created;
                    }
                    RecordedPiece take{wanted.clip,captured->timelineStartSample,captured->timelineEndSample,captured->sourceStartFrame,name};
                    edit::RowEdits takeRows(store.db());
                    clip_edit::createImage(takeRows,recordedImage(store,tempo,take,trackId,sourceEdits[track].mediaId,recorded.sampleRate,laneId));
                    OpRequest clip;clip.opType="clip.duplicate";clip.payload={{"id",wanted.clip},{"changes",takeRows.changes()}};
                    clip.targetKind="clip";clip.targetId=wanted.clip;result.requests.push_back(std::move(clip));
                    take.id=allocated.next("clips");replaceRecordedSpan(main[track],std::move(take),allocated);
                }
                // A track with nothing heard in this pass keeps its main-lane image in the batch.
                for(const auto& piece:main[track])batch.incoming.push_back(recordedImage(store,tempo,piece,trackId,sourceEdits[track].mediaId,recorded.sampleRate));
                batch.clear.push_back({{trackId,std::nullopt},{first,last}});
            }
            auto prepared=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),placement,allocated,"Record");
            if(!prepared.ok)throw std::runtime_error(prepared.error);
            Payload cumulative=Payload::array();
            for(const auto& request:prepared.requests) {
                if(request.opType!="arranger.splice" || request.payload.contains("loop"))throw std::runtime_error("unexpected recording placement operation");
                for(const auto& change:request.payload.at("changes"))cumulative.push_back(change);
            }
            auto delta=passDelta(previous,cumulative);
            if(delta.empty())throw std::runtime_error("recorded pass has no audible placement");
            OpRequest splice;splice.opType="arranger.splice";splice.payload={{"id",0},{"changes",std::move(delta)}};
            result.requests.push_back(std::move(splice));previous=std::move(cumulative);
            for(auto index=groupStart;index<result.requests.size();++index)result.requests[index].label="Record";
            if(!selections.empty())result.requests[groupStart].selBefore=selections[pass];
            result.groupEnds_.push_back(result.requests.size());
        }
        if(firstGroup)throw std::runtime_error("the take ended before its latency-corrected start");
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();result.requests.clear();result.groupEnds_.clear();}
    return result;
}

RecordingEdit prepareMidiLoopCommit(Store& store,const MidiLoopPlacement& placement,std::span<const MidiTakePass> passes) {
    RecordingEdit result;
    try {
        if(store.readOnly() || audio::onAudioThread)throw std::runtime_error("MIDI recording requires a writable owner");
        if(placement.start<0 || placement.length<=0 ||
           placement.start>std::numeric_limits<std::int64_t>::max()-placement.length ||
           placement.mainClip<=0 || placement.mainStream<=0 || passes.empty())
            throw std::runtime_error("invalid MIDI loop placement");
        Read read(store.db());
        SQLite::Statement track(store.db(),"SELECT kind,locked FROM tracks WHERE id=?");
        track.bind(1,placement.track);
        if(!track.executeStep() || track.getColumn(1).getInt()!=0 ||
           (track.getColumn(0).getString()!="midi" && track.getColumn(0).getString()!="instrument"))
            throw std::runtime_error("MIDI recording target is missing, locked or not a MIDI track");
        const auto main=notes::captureNotesForNewClip(store,placement.mainClip,placement.mainStream);
        if(!main.ok)throw std::runtime_error(main.error);
        result.sourceProject=identity(store);
        result.owner_=&store;
        result.client_=store.clientId();
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        result.files_=std::make_shared<RecordingEdit::Files>(); // a MIDI take has no published media assets
        std::vector<std::int64_t> lanes;
        SQLite::Statement existing(store.db(),"SELECT id FROM lanes WHERE track_id=? AND kind='take' ORDER BY ord,id");
        existing.bind(1,placement.track);
        while(existing.executeStep())lanes.push_back(existing.getColumn(0).getInt64());
        std::set<std::int64_t> clipIds{placement.mainClip},streamIds{placement.mainStream},laneIds;
        const auto append=[&](OpRequest request) {
            request.label="Record MIDI";
            result.requests.push_back(std::move(request));
        };
        const auto newClip=[&](std::int64_t clip,std::int64_t length,const std::string& name) {
            OpRequest request;request.opType="clip.create";request.targetKind="clip";request.targetId=clip;
            request.payload={{"id",clip},{"track",placement.track},{"kind","midi"},{"name",name},
                             {"pos",placement.start},{"length",length}};
            append(std::move(request));
        };
        const auto writeNotes=[&](const notes::Snapshot& snapshot,std::span<const notes::NoteValues> input,
                                  std::int64_t length,bool closeAtEnd) {
            notes::Diff diff;diff.added.reserve(input.size());
            std::string error;
            for(auto note:input) {
                if(note.start<0 || note.start>=length || note.duration<=0)
                    throw std::runtime_error("MIDI recorded note starts outside its pass");
                if(closeAtEnd && note.duration>length-note.start) {
                    note.duration=length-note.start;
                    note.releaseVelocity=0;
                }
                auto record=notes::makeRecord(note,{},error);
                if(!record)throw std::runtime_error(error);
                diff.added.push_back(std::move(*record));
            }
            auto edit=notes::prepareDiff(snapshot,diff);
            if(!edit.ok)throw std::runtime_error(edit.error);
            for(auto& request:edit.requests)append(std::move(request));
        };
        const auto name=placement.name.empty()?std::string("MIDI Recording"):placement.name;
        for(std::size_t index=0;index<passes.size();++index) {
            const auto& pass=passes[index];
            if(pass.length<=0 || pass.length>placement.length ||
               (index+1<passes.size() && pass.length!=placement.length) ||
               pass.take.lane<=0 || pass.take.clip<=0 || pass.stream<=0 ||
               !laneIds.insert(pass.take.lane).second || !clipIds.insert(pass.take.clip).second ||
               !streamIds.insert(pass.stream).second || edit::row(store.db(),"lanes",pass.take.lane))
                throw std::runtime_error("invalid MIDI take length or occupied identity");
            const auto take=notes::captureNotesForNewClip(store,pass.take.clip,pass.stream);
            if(!take.ok)throw std::runtime_error(take.error);
            append(createTakeLane(store,pass.take.lane,placement.track));
            lanes.insert(lanes.begin()+static_cast<std::ptrdiff_t>(index),pass.take.lane);
            if(index!=0) {
                OpRequest order;order.opType="lane.reorder";
                order.payload={{"track",placement.track},{"lanes",lanes}};
                append(std::move(order));
            }
            newClip(pass.take.clip,pass.length,pass.take.name.empty()?name:pass.take.name);
            OpRequest lane;lane.opType="clip.setLane";lane.targetKind="clip";lane.targetId=pass.take.clip;
            lane.payload={{"id",pass.take.clip},{"lane",pass.take.lane}};
            append(std::move(lane));
            writeNotes(take,pass.takeNotes,pass.length,true);
            if(index!=0) {
                // Only replace this capture's previous main image. Keeping the
                // same identity gives each pass an exact previous-image Undo.
                OpRequest previous;previous.opType="clip.delete";previous.targetKind="clip";previous.targetId=placement.mainClip;
                previous.payload={{"id",placement.mainClip}};
                append(std::move(previous));
            }
            newClip(placement.mainClip,placement.length,name);
            writeNotes(main,pass.mainNotes,placement.length,false);
            result.groupEnds_.push_back(result.requests.size());
        }
        result.ok=true;
    } catch(const std::exception& error) {
        result={};
        result.error=error.what();
    }
    return result;
}

notes::PreparedDiff prepareRecordQuantize(Store& store,std::int64_t clip,const notes::Diff& diff) {
    if(!diff.added.empty() || !diff.removed.empty()) {
        notes::PreparedDiff refused;refused.error="Record Quantize changes existing notes";
        return refused;
    }
    auto edit=notes::prepareDiff(notes::captureNotes(store,clip),diff);
    for(auto& request:edit.requests)request.label="Record Quantize";
    return edit;
}

bool publishStagedTake(RecordingEdit& edit,std::stop_token stop) {
    const auto files = edit.files_;
    if (!edit.ok || !files || !files->stagedTake || files->publications || files->committed || audio::onAudioThread) {
        edit.error = "recording media has no unpublished staging";
        return false;
    }
    try {
        auto rollback = std::make_shared<media::detail::PublishedRollback>();
        rollback->files.reserve(files->assets.size());
        files->publications = rollback;
        requireRecordingFolder(files->stagedTake->folder);
        const auto projectFolder = files->stagedTake->folder.parent_path().parent_path();
        for (auto& asset : files->assets) {
            if (stop.stop_requested()) throw std::runtime_error("recording publication cancelled");
            if (fileStamp(asset.source) != asset.sourceProof)
                throw std::runtime_error("recorded file changed after staging");
            if (asset.target.empty()) continue;
            if (asset.previousMedia) {
                if (const auto existing = verifiedOwnedRecording(files->stagedTake->folder, *asset.previousMedia, stop)) {
                    bindPublishedRecording(edit.requests, asset.mediaId, existing->first, projectFolder, true);
                    asset.target = existing->first;
                    asset.publishedProof = existing->second;
                    asset.reusedOwned = true;
                    continue;
                }
                if (stop.stop_requested()) throw std::runtime_error("recording publication cancelled");
            }
            if (fileStamp(asset.staged) != asset.stageProof)
                throw std::runtime_error("recording publication staging changed");
            rollback->files.push_back({asset.target, {}});
            const auto published = media::publishFile(asset.staged, asset.target, edit.publicationHooks,
                                                      media::PublishPolicy::AtomicOnly);
            if (published.status == media::PublishStatus::published) {
                rollback->files.back().identity = published.identity;
                const auto proof = fileStamp(asset.target);
                // Renaming the closed stage can legitimately update ctime.
                if (published.identity != proof.identity || !asset.stageProof ||
                    proof.identity != asset.stageProof->identity || proof.size != asset.stageProof->size ||
                    proof.modified != asset.stageProof->modified)
                    throw std::runtime_error("recording publication changed during rename");
                asset.publishedProof = proof;
                continue;
            }
            rollback->files.pop_back();
            if (published.status == media::PublishStatus::exists && fs::is_regular_file(fs::symlink_status(asset.target))) {
                const auto proof = fileStamp(asset.target);
                const auto existing = media::blake3File(asset.target, stop);
                if (existing && existing.hex == asset.hash && fileStamp(asset.target) == proof) {
                    asset.publishedProof = proof;
                    continue;
                }
            }
            throw std::runtime_error("Cannot publish recorded audio " + media::pathUtf8(asset.target) + ": " +
                (published.status == media::PublishStatus::exists ? "destination contains different content" : published.error));
        }
        return true;
    } catch (const std::exception& failure) {
        edit.ok = false;
        edit.error = failure.what();
        if (files->publications) {
            const auto warning = files->publications->rollback();
            if (!warning.empty()) edit.error += "\n" + warning;
        }
        return false;
    }
}

StagedRefresh refreshStagedCommit(Store& store,const StagedTake& staged,RecordingEdit& edit,std::span<const ClipIds> ids,
                                  std::int64_t placementOffsetSamples) {
    return refreshStagedCommit(store, staged, edit, ids, {}, placementOffsetSamples);
}

StagedRefresh refreshStagedCommit(Store& store,const StagedTake& staged,RecordingEdit& edit,std::span<const ClipIds> ids,
                                  std::span<const MidiLinearTake> midi,std::int64_t placementOffsetSamples) {
    const auto published = edit.files_;
    if (!edit.ok || !published || !published->stagedTake || !published->publications || published->committed)
        return StagedRefresh::refused;
    const auto refuse = [&](StagedRefresh status, std::string error) {
        edit.ok = false;
        edit.error = std::move(error);
        const auto warning = published->publications->rollback();
        if (!warning.empty()) edit.error += "\n" + warning;
        return status;
    };
    try {
        if (edit.owner_ != &store || edit.client_ != store.clientId() || identity(store) != edit.sourceProject)
            throw std::runtime_error("recording project changed during publication");
        if (edit.options.expectHead && *edit.options.expectHead == OpJournal(store).headSeq()) return StagedRefresh::ready;
        auto current = prepareStagedCommit(store, staged, ids, midi, placementOffsetSamples);
        if (!current.ok) {
            edit.audioOutsidePunch = current.audioOutsidePunch;
            throw std::runtime_error(current.error);
        }
        const auto replacement = current.files_;
        const auto projectFolder = media::projectFolder(store);
        if (published->assets.size() != replacement->assets.size())
            return refuse(StagedRefresh::restage, "recording media binding changed during publication");
        for (std::size_t index = 0; index < published->assets.size(); ++index) {
            const auto& a = published->assets[index];
            auto& b = replacement->assets[index];
            if (a.source != b.source)
                return refuse(StagedRefresh::restage, "recording track placement changed during publication");
            if (a.hash != b.hash || a.sourceProof != b.sourceProof)
                return refuse(StagedRefresh::refused, "recording source changed during publication");
            if (a.mediaId != b.mediaId || a.previousMedia != b.previousMedia)
                return refuse(StagedRefresh::restage, "recording media binding changed during publication");
            // Numbering scans now see the WAV just published. Preserve its
            // proven name when the rebased edit still binds the same media.
            if (a.target != b.target || a.reusedOwned)
                bindPublishedRecording(current.requests, b.mediaId, a.target, projectFolder, a.reusedOwned);
            b.target = a.target;
            b.publishedProof = a.publishedProof;
            b.reusedOwned = a.reusedOwned;
        }
        replacement->keepPrivate = replacement->keepPrivate || published->keepPrivate;
        replacement->publications = published->publications;
        current.options.beforeCommit = std::move(edit.options.beforeCommit);
        edit = std::move(current);
        return StagedRefresh::ready;
    } catch (const std::exception& failure) {
        return refuse(StagedRefresh::refused, failure.what());
    }
}

std::string cleanupStagedTake(RecordingEdit& edit) {
    const auto files = edit.files_;
    if (!files || !files->stagedTake || !files->committed || audio::onAudioThread)
        return {};
    const auto folder = media::pathUtf8(files->source.directory().filename());
    const auto warning = files->keepPrivate ? std::error_code{} : files->source.discard();
    files->stagedTake.reset();
    if (warning)
        return "Private recording files could not be removed (" + warning.message() + ") and remain in " + folder + ".";
    return {};
}

notes::PreparedDiff prepareRecordQuantize(Store& store, std::span<const MidiRecordQuantize> targets) {
    notes::PreparedDiff batch;
    try {
        Read read(store.db());
        for (const auto& target : targets) {
            if (target.noteIds.empty() || target.grid == 0)
                continue;
            if (target.grid < 0 || target.length <= 0 || target.sourceStart < 0 || target.timelineStart < 0 ||
                target.sourceStart > std::numeric_limits<std::int64_t>::max() - target.length ||
                target.timelineStart > std::numeric_limits<std::int64_t>::max() - target.length)
                throw std::runtime_error("invalid recorded note window");

            const auto snapshot = notes::captureNotes(store, target.clip);
            if (!snapshot.ok)
                throw std::runtime_error(snapshot.error);
            std::set<std::uint64_t> remaining(target.noteIds.begin(), target.noteIds.end());
            notes::Diff diff;
            std::string error;
            for (const auto& record : snapshot.records()) {
                auto note = notes::values(record, error);
                if (!note)
                    throw std::runtime_error(error);
                if (remaining.erase(note->id) == 0)
                    continue;
                if (note->start < target.sourceStart)
                    throw std::runtime_error("record quantize note lies outside its take");
                const auto offset = note->start - target.sourceStart;
                if (offset >= target.length || note->duration > target.length - offset)
                    throw std::runtime_error("record quantize note lies outside its take");
                const auto before = (target.timelineStart + offset) % target.grid;
                if (before == 0)
                    continue;
                const auto after = target.grid - before;
                // Keep the full note and its expression inside the recorded interval.
                const bool canMoveBack = before <= offset;
                const bool canMoveForward = after <= target.length - offset - note->duration;
                if (!canMoveBack && !canMoveForward)
                    continue;
                note->start += canMoveBack && (!canMoveForward || before < after) ? -before : after;
                auto changed = notes::withValues(record, *note, error);
                if (!changed)
                    throw std::runtime_error(error);
                diff.changed.push_back({record, std::move(*changed)});
            }
            if (!remaining.empty())
                throw std::runtime_error("a recorded note disappeared before record quantize");
            auto prepared = notes::prepareDiff(snapshot, diff);
            if (!prepared.ok)
                throw std::runtime_error(prepared.error);
            batch.sourceFile = std::move(prepared.sourceFile);
            batch.options = std::move(prepared.options);
            for (auto& request : prepared.requests) {
                request.label = "Record Quantize";
                batch.requests.push_back(std::move(request));
            }
            for (auto& change : prepared.effects.changed)
                batch.effects.changed.push_back(std::move(change));
        }
        batch.ok = true;
    } catch (const std::exception& exception) {
        batch = {};
        batch.error = exception.what();
    }
    return batch;
}

CommitResult commit(Store& store,RecordingEdit& edit,Actor actor) {
    CommitResult refused;
    if(!edit.ok){refused.error=edit.error;return refused;}
    if(audio::onAudioThread || edit.owner_!=&store || edit.client_!=store.clientId()) {
        refused.error="recording edit belongs to another open document or callback";return refused;
    }
    try {if(identity(store)!=edit.sourceProject){refused.error="recording belongs to a different project";return refused;}}
    catch(const std::exception& exception){refused.error=exception.what();return refused;}
    const auto files=edit.files_;
    if(!files || files->committed){refused.error="recording edit is absent or already committed";return refused;}
    if (files->stagedTake && !files->publications) {refused.error="recording media was not published";return refused;}
    auto rollback=files->publications ? files->publications : std::make_shared<media::detail::PublishedRollback>();
    rollback->files.reserve(files->assets.size());
    auto options=edit.options;const auto previous=options.beforeCommit;const auto hooks=edit.publicationHooks;
    options.beforeCommit=[files,previous,rollback,hooks](SQLite::Database& db,std::int64_t transaction,std::string& error) {
        if(previous && !previous(db,transaction,error))return false;
        if(!files->source.recordingId().empty()) {
            if(files->source.recoveryProjectUuid()!=projectUuid(db)) {
                error="recording project identity changed";
                return false;
            }
            SQLite::Statement receipt(db,"INSERT OR IGNORE INTO adi_meta(key,value) VALUES(?,?)");
            receipt.bind(1,receiptKey(files->source.recordingId()));
            receipt.bind(2,std::to_string(transaction));
            if(receipt.exec()!=1) {
                error="recording is already placed";
                return false;
            }
        }
        if (files->stagedTake) {
            try {
                requireRecordingFolder(files->stagedTake->folder);
                for (const auto& asset : files->assets) {
                    if (fileStamp(asset.source) != asset.sourceProof ||
                        (!asset.target.empty() && fileStamp(asset.target) != asset.publishedProof)) {
                        error = "recorded file changed after publication";
                        return false;
                    }
                }
                return true;
            } catch (const std::exception& failure) {
                error = failure.what();
                return false;
            }
        }
        for(const auto& asset:files->assets) {
            if(!fs::is_regular_file(fs::symlink_status(asset.source))){error="recorded take is no longer a regular file";return false;}
            const auto hash=media::blake3File(asset.source);if(!hash || hash.hex!=asset.hash){error="recorded file changed after capture";return false;}
            if(asset.target.empty())continue;
            if(asset.staged.empty()) {
                if(recordingFileMatches(asset.target,asset.hash))continue;
                error="Existing recorded audio changed after capture: "+media::pathUtf8(asset.target);
                return false;
            }
            const auto stagedHash=media::blake3File(asset.staged);
            if(!stagedHash || stagedHash.hex!=asset.hash){error="recording publication staging changed after capture";return false;}
            rollback->files.push_back({asset.target,{}});
            const auto published = media::publishFile(asset.staged, asset.target, hooks,
                                                      media::PublishPolicy::AtomicOnly);
            if(published.status==media::PublishStatus::published) {
                rollback->files.back().identity=published.identity;
                continue;
            }
            rollback->files.pop_back();
            if(published.status==media::PublishStatus::exists && recordingFileMatches(asset.target,asset.hash))continue;
            error="Cannot publish recorded audio "+media::pathUtf8(asset.target)+": "+(published.status==media::PublishStatus::exists?"destination contains different content":published.error);
            return false;
        }
        return true;
    };
    auto requests=edit.requests;for(auto& request:requests)request.actor=actor;
    auto result=(edit.groupEnds_.empty() || files->source.recovered())?OpJournal(store).commit(requests,options):
        OpJournal(store).commitGrouped(requests,edit.groupEnds_,options);
    if(result.ok) {
        rollback->committed=true;
        files->committed=true;
        try{if(!files->stagedTake && !files->keepPrivate)files->source.discard();}catch(...){/* committed assets remain authoritative */}
    } else if(auto warning=rollback->rollback();!warning.empty()) {
        if(!result.error.empty())result.error+='\n';
        result.error+=warning;
    }
    return result;
}
}
