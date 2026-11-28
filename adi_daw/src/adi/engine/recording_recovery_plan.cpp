// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/engine/recording_recovery_wave.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/appdata.hpp"
#include "adi/media/media_ops.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace adi::engine::recording_recovery {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t maxJsonBytes = 256U * 1024U;
constexpr std::size_t maxTargets = 256;
constexpr std::string_view stagePrefix = ".adi-take-stage-";
constexpr std::string_view sessionPrefix = "session-";
constexpr std::string_view registrationPrefix = "recording-";

[[noreturn]] void refuse(std::string message) {
    throw std::runtime_error("recording recovery plan: " + message);
}
void ownerOnly() {
    if (audio::onAudioThread)
        refuse("metadata I/O is not allowed on the audio callback");
    audio::fileIoPoint();
}
bool hexId(std::string_view value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string newId() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    for (unsigned word = 0; word < 4; ++word) {
        const auto bits = static_cast<std::uint32_t>(random());
        for (unsigned nibble = 0; nibble < 8; ++nibble)
            value.push_back(hex[(bits >> (4U * nibble)) & 15U]);
    }
    return value;
}
void ordinary(const fs::path& path, bool directory) {
    const auto status = fs::symlink_status(path);
    if (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))
        refuse("expected an ordinary " + std::string(directory ? "directory" : "file"));
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        refuse("reparse points and inaccessible paths are not recovery inputs");
#endif
}
fs::path checkedDirectory(const fs::path& directory) {
    ownerOnly();
    if (!directory.is_absolute() || directory != directory.lexically_normal())
        refuse("take directory must be absolute and normalized");
    fs::path walked;
    for (const auto& component : directory) {
        walked /= component;
        if (!walked.has_root_directory())
            continue;
        ordinary(walked, true);
    }
    const auto resolved = fs::canonical(directory);
    if (resolved != directory || resolved == resolved.root_path())
        refuse("take directory must already have its canonical spelling");
    return resolved;
}
bool absent(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return true;
    if (error)
        throw fs::filesystem_error("inspect recording metadata", path, error);
    return status.type() == fs::file_type::not_found;
}
std::string takeName(std::size_t index) {
    return "take-" + std::to_string(index) + ".wav";
}
bool stageName(std::string_view name) {
    return name.starts_with(stagePrefix) && name.ends_with(".tmp") &&
        hexId(name.substr(stagePrefix.size(), name.size() - stagePrefix.size() - 4));
}
void shape(const fs::path& directory, std::size_t targets, bool prepared) {
    std::set<fs::path> expected;
    for (std::size_t i = 0; i < targets; ++i)
        expected.insert(fs::path(takeName(i)));
    if (prepared) {
        expected.insert(fs::path("owner.lock"));
        expected.insert(fs::path("take.json"));
    }
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (++count > targets + 11)
            refuse("too many files in the take directory");
        ordinary(entry.path(), false);
        const auto name = entry.path().filename();
        if (expected.erase(name) != 0)
            continue;
        if (prepared && name == fs::path("start.json"))
            continue;
        // A killed atomic metadata publication can leave a private stage. It
        // is never read as a plan or deleted by inspection. Refuse all others.
        if (prepared && stageName(media::pathUtf8(name)))
            continue;
        refuse("unexpected file in the take directory");
    }
    if (!expected.empty())
        refuse("a required owned take file is missing");
}

// Native no-follow final opens keep a link swapped between inspection and open
// from redirecting metadata I/O. Ancestor checks are conservative observations,
// not an atomic filesystem namespace snapshot against noncooperating writers.
class NativeFile final {
public:
#if defined(_WIN32)
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
#else
    int value = -1;
    ~NativeFile() { if (value >= 0) (void)::close(value); }
#endif
    NativeFile() = default;
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
};
std::string readText(const fs::path& path) {
    ordinary(path, false);
    NativeFile file;
#if defined(_WIN32)
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        refuse("cannot open metadata for reading");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        refuse("metadata is not an ordinary file");
#else
    file.value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file.value < 0)
        refuse("cannot open metadata for reading");
    struct stat info{};
    if (::fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode))
        refuse("metadata is not an ordinary file");
#endif
    std::string result;
    std::array<char, 8192> buffer{};
    for (;;) {
#if defined(_WIN32)
        DWORD received = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr))
            refuse("metadata read failed");
        const auto count = static_cast<std::size_t>(received);
#else
        ssize_t received;
        do { received = ::read(file.value, buffer.data(), buffer.size()); }
        while (received < 0 && errno == EINTR);
        if (received < 0)
            refuse("metadata read failed");
        const auto count = static_cast<std::size_t>(received);
#endif
        if (count == 0)
            break;
        if (count > maxJsonBytes - result.size())
            refuse("metadata exceeds its byte limit");
        result.append(buffer.data(), count);
    }
    return result;
}
Json parse(const std::string& text) {
    if (text.empty() || text.size() > maxJsonBytes || text.find('\0') != std::string::npos)
        refuse("invalid metadata bytes");
    std::vector<std::set<std::string>> keys;
    bool valid = true;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 8) {
            valid = false;
            return false;
        }
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) {
            if (keys.empty())
                valid = false;
            else
                keys.pop_back();
        } else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(value.get<std::string>()).second)
                valid = false;
        }
        return true;
    };
    const auto value = Json::parse(text, callback);
    if (!valid || !keys.empty() || !value.is_object())
        refuse("duplicate fields or excessive metadata nesting");
    return value;
}
void fields(const Json& object, std::initializer_list<std::string_view> names) {
    if (!object.is_object() || object.size() != names.size())
        refuse("unknown or missing metadata field");
    for (const auto name : names) {
        if (!object.contains(std::string(name)))
            refuse("unknown or missing metadata field");
    }
}
std::uint64_t unsignedInteger(const Json& value) {
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number >= 0)
            return static_cast<std::uint64_t>(number);
    }
    refuse("expected a nonnegative integer");
}
std::int64_t sampleInteger(const Json& value) {
    const auto number = unsignedInteger(value);
    if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        refuse("sample or track number exceeds int64");
    return static_cast<std::int64_t>(number);
}
std::int64_t offsetInteger(const Json& value) {
    if (value.is_number_unsigned()) return sampleInteger(value);
    if (value.is_number_integer()) return value.get<std::int64_t>();
    refuse("expected a signed recording offset");
}
std::string textField(const Json& value, std::size_t limit = 128) {
    if (!value.is_string())
        refuse("expected a text field");
    const auto text = value.get<std::string>();
    if (text.empty() || text.size() > limit || std::any_of(text.begin(), text.end(), [](char c) {
        return static_cast<unsigned char>(c) < 32U;
    }))
        refuse("invalid bounded identity text");
    return text;
}
std::string formatName(audio::WavFormat format) {
    switch (format) {
    case audio::WavFormat::Pcm16: return "pcm16";
    case audio::WavFormat::Pcm24: return "pcm24";
    case audio::WavFormat::Float32: return "float32";
    }
    refuse("unsupported recording encoding");
}
std::string schemeName(media::FileIdentityScheme scheme) {
    switch (scheme) {
    case media::FileIdentityScheme::Posix: return "posix";
    case media::FileIdentityScheme::Windows128: return "windows128";
    case media::FileIdentityScheme::WindowsLegacy64: return "windows64";
    }
    refuse("unsupported file identity namespace");
}
void validate(const TakePlan& plan) {
    if (!hexId(plan.recordingId))
        refuse("invalid recording ID");
    (void)textField(Json(plan.projectUuid));
    (void)textField(Json(plan.sourceProject), 65536);
    (void)formatName(plan.format);
    if (plan.sampleRate < 8000 || plan.sampleRate > 768000 || plan.targets.empty() ||
        plan.targets.size() > maxTargets || plan.identities.size() != plan.targets.size())
        refuse("invalid recording rate or targets");
    std::set<std::int64_t> tracks;
    std::set<media::FileIdentity> identities;
    for (std::size_t i = 0; i < plan.targets.size(); ++i) {
        const auto& target = plan.targets[i];
        if (target.trackId <= 0 || !tracks.insert(target.trackId).second ||
            target.channels.empty() || target.channels.size() > 64)
            refuse("invalid or duplicate recording target");
        std::set<std::uint32_t> channels;
        for (const auto channel : target.channels) {
            if (channel >= 64 || !channels.insert(channel).second)
                refuse("invalid or duplicate input channel");
        }
        const auto& identity = plan.identities[i];
        (void)schemeName(identity.scheme);
        if ((identity.scheme != media::FileIdentityScheme::Windows128 && identity.fileHigh != 0) ||
            !identities.insert(identity).second)
            refuse("invalid or aliased take file identities");
    }
    if (plan.loop && (plan.loop->beginSample < 0 || plan.loop->endSample <= plan.loop->beginSample ||
        plan.loop->maxPasses == 0 || plan.loop->maxPasses > 65536))
        refuse("invalid frozen loop");
}
Json encode(const TakePlan& plan) {
    validate(plan);
    Json value{{"schema", 2}, {"phase", "prepared"}, {"recordingId", plan.recordingId},
        {"projectUuid", plan.projectUuid}, {"sourceProject", plan.sourceProject}, {"sampleRate", plan.sampleRate},
        {"format", formatName(plan.format)}, {"targets", Json::array()}, {"loop", nullptr},
        {"placementOffsetSamples", plan.placementOffsetSamples}};
    for (std::size_t i = 0; i < plan.targets.size(); ++i) {
        const auto& target = plan.targets[i];
        const auto& id = plan.identities[i];
        value["targets"].push_back({{"trackId", target.trackId}, {"channels", target.channels},
            {"file", takeName(i)}, {"identity", {{"scheme", schemeName(id.scheme)},
            {"volume", id.volume}, {"file", id.file}, {"fileHigh", id.fileHigh}}},
            {"placementOffsetSamples", target.placementOffsetSamples ? Json(*target.placementOffsetSamples) : Json(nullptr)}});
    }
    if (plan.loop)
        value["loop"] = {{"beginSample", plan.loop->beginSample}, {"endSample", plan.loop->endSample},
                         {"maxPasses", plan.loop->maxPasses}};
    return value;
}
TakePlan decode(const Json& value) {
    const auto schema = unsignedInteger(value.at("schema"));
    const bool placed = schema == 2;
    if (schema == 1)
        fields(value, {"schema", "phase", "recordingId", "projectUuid", "sourceProject", "sampleRate", "format", "targets", "loop"});
    else if (placed)
        fields(value, {"schema", "phase", "recordingId", "projectUuid", "sourceProject", "sampleRate", "format", "targets", "loop", "placementOffsetSamples"});
    if ((schema != 1 && !placed) || value.at("phase") != "prepared")
        refuse("unsupported plan version or phase");
    TakePlan plan;
    if (placed) plan.placementOffsetSamples = offsetInteger(value.at("placementOffsetSamples"));
    plan.recordingId = textField(value.at("recordingId"));
    plan.projectUuid = textField(value.at("projectUuid"));
    plan.sourceProject = textField(value.at("sourceProject"), 65536);
    const auto rate = unsignedInteger(value.at("sampleRate"));
    if (rate > 768000)
        refuse("invalid recording rate");
    plan.sampleRate = static_cast<std::uint32_t>(rate);
    const auto format = textField(value.at("format"));
    if (format == "pcm16")
        plan.format = audio::WavFormat::Pcm16;
    else if (format == "pcm24")
        plan.format = audio::WavFormat::Pcm24;
    else if (format == "float32")
        plan.format = audio::WavFormat::Float32;
    else
        refuse("unsupported recording encoding");
    const auto& targets = value.at("targets");
    if (!targets.is_array() || targets.empty() || targets.size() > maxTargets)
        refuse("invalid target list");
    for (const auto& item : targets) {
        if (placed) fields(item, {"trackId", "channels", "file", "identity", "placementOffsetSamples"});
        else fields(item, {"trackId", "channels", "file", "identity"});
        if (item.at("file") != takeName(plan.targets.size()))
            refuse("take filename must match its fixed target index");
        RecordingTarget target;
        target.trackId = sampleInteger(item.at("trackId"));
        if (placed && !item.at("placementOffsetSamples").is_null())
            target.placementOffsetSamples = offsetInteger(item.at("placementOffsetSamples"));
        const auto& channels = item.at("channels");
        if (!channels.is_array() || channels.empty() || channels.size() > 64)
            refuse("invalid channel mapping");
        for (const auto& channel : channels) {
            const auto number = unsignedInteger(channel);
            if (number >= 64)
                refuse("invalid input channel");
            target.channels.push_back(static_cast<std::uint32_t>(number));
        }
        const auto& id = item.at("identity");
        fields(id, {"scheme", "volume", "file", "fileHigh"});
        media::FileIdentity identity;
        identity.volume = unsignedInteger(id.at("volume"));
        identity.file = unsignedInteger(id.at("file"));
        identity.fileHigh = unsignedInteger(id.at("fileHigh"));
        const auto scheme = textField(id.at("scheme"));
        if (scheme == "posix")
            identity.scheme = media::FileIdentityScheme::Posix;
        else if (scheme == "windows128")
            identity.scheme = media::FileIdentityScheme::Windows128;
        else if (scheme == "windows64")
            identity.scheme = media::FileIdentityScheme::WindowsLegacy64;
        else
            refuse("unsupported file identity namespace");
        plan.targets.push_back(std::move(target));
        plan.identities.push_back(identity);
    }
    const auto& loop = value.at("loop");
    if (!loop.is_null()) {
        fields(loop, {"beginSample", "endSample", "maxPasses"});
        const auto passes = unsignedInteger(loop.at("maxPasses"));
        if (passes > 65536)
            refuse("invalid loop capacity");
        plan.loop = AudioRecordingLoop{sampleInteger(loop.at("beginSample")),
            sampleInteger(loop.at("endSample")), static_cast<std::uint32_t>(passes)};
    }
    validate(plan);
    return plan;
}
media::FileIdentity takeIdentity(const fs::path& path) {
    ordinary(path, false);
    std::error_code error;
    const auto identity = media::readFileIdentity(path, error);
    if (!identity)
        refuse("cannot identify owned take: " + error.message());
    ordinary(path, false);
    return *identity;
}
void writeExclusive(const fs::path& path, const std::string& text) {
    NativeFile file;
#if defined(_WIN32)
    file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        refuse("cannot create private metadata stage");
    DWORD written = 0;
    if (!WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        static_cast<std::size_t>(written) != text.size())
        refuse("metadata stage write failed");
    const auto handle = file.value;
    file.value = INVALID_HANDLE_VALUE;
    if (!CloseHandle(handle))
        refuse("metadata stage close failed");
#else
    file.value = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file.value < 0)
        refuse("cannot create private metadata stage");
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(file.value, text.data() + offset, text.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            refuse("metadata stage write failed");
        offset += static_cast<std::size_t>(written);
    }
    const int descriptor = file.value;
    file.value = -1;
    if (::close(descriptor) != 0)
        refuse("metadata stage close failed");
#endif
}
std::string publish(const fs::path& directory, const char* name, const Json& value) {
    const auto text = value.dump();
    if (text.size() > maxJsonBytes)
        refuse("metadata exceeds its byte limit");
    const auto stage = directory / (std::string(stagePrefix) + newId() + ".tmp");
    writeExclusive(stage, text);
    // Leave private stages after failure as evidence; never unlink a path that
    // might have been substituted. The owning directory's discard owns cleanup.
    auto result = media::publishFile(stage, directory / name, {}, media::PublishPolicy::AtomicOnly);
    if (result.status != media::PublishStatus::published)
        refuse("atomic metadata publication failed: " + result.error);
    return std::move(result.warning);
}
TakeStart decodeStart(const Json& value, std::string_view recordingId) {
    const auto schema = unsignedInteger(value.at("schema"));
    if (schema == 1) fields(value, {"schema", "recordingId", "startSample"});
    else if (schema == 2) fields(value, {"schema", "recordingId", "startSample", "punchIn", "punchOut"});
    if (!hexId(recordingId) || (schema != 1 && schema != 2) ||
        textField(value.at("recordingId")) != recordingId)
        refuse("start metadata does not belong to this recording");
    TakeStart start{sampleInteger(value.at("startSample")), {}};
    if (schema == 2) {
        const auto boundary = [](const Json& point) -> std::optional<RecordingPunchWindow::Boundary> {
            if (point.is_null()) return {};
            fields(point, {"sample", "tick"});
            return RecordingPunchWindow::Boundary{sampleInteger(point.at("sample")), sampleInteger(point.at("tick"))};
        };
        start.punch = {boundary(value.at("punchIn")), boundary(value.at("punchOut"))};
        if (start.punch.in && start.punch.out && start.punch.in->sample >= start.punch.out->sample)
            refuse("punch boundaries are reversed");
    }
    return start;
}
fs::path sessionRoot() {
    const auto data=appdata::pathsFor(appdata::App::Daw).data;
    return data.empty()?fs::path{}:data/"recording-sessions";
}
struct LaunchSession {
    fs::path directory;
    std::shared_ptr<media::ProjectFileLease> lease;
};
LaunchSession& launchSession() {
    static LaunchSession session=[] {
        const auto root=sessionRoot();
        if(root.empty())refuse("no application data directory for launch recovery");
        fs::create_directories(root);
        const auto directory=fs::canonical(root)/(std::string(sessionPrefix)+newId());
        if(!fs::create_directory(directory))refuse("cannot reserve launch session directory");
        const auto path=directory/"owner.lock";
        std::error_code error;
        const auto marker=media::ProjectFileLease::createShared(path,error);
        if(!marker)refuse("cannot create launch owner marker: "+error.message());
        // The shared lease excludes the scanner's exclusive lease. The
        // app never replaces this file, even as recordings come and go.
#if defined(_WIN32)
        const auto pid=static_cast<std::uint64_t>(GetCurrentProcessId());
#else
        const auto pid=static_cast<std::uint64_t>(::getpid());
#endif
        const auto started=std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        (void)publish(directory,"session.json",{{"schema",1},{"pid",pid},{"started",started}});
        return LaunchSession{directory,marker};
    }();
    return session;
}
std::string registrationName(std::string_view id) {
    return std::string(registrationPrefix)+std::string(id)+".json";
}
}

std::string registerLaunchTake(const fs::path& directory,const TakePlan& plan) {
    ownerOnly();
    if(!hexId(plan.recordingId))refuse("invalid launch recording ID");
    const auto capture=checkedDirectory(directory);
    const auto name=registrationName(plan.recordingId);
    return publish(launchSession().directory,name.c_str(),{{"schema",1},{"recordingId",plan.recordingId},
        {"directory",media::pathUtf8(capture)}});
}
void forgetLaunchTake(std::string_view id) noexcept {
    if(audio::onAudioThread || !hexId(id))return;
    try {
        std::error_code error;
        fs::remove(launchSession().directory/registrationName(id),error);
    } catch(...) {
        // A stale registration cannot restore a missing take.
    }
}
std::vector<LaunchTake> findLaunchTakes() {
    ownerOnly();
    std::vector<LaunchTake> result;
    auto root=sessionRoot();
    if(root.empty() || absent(root))return result;
    root=checkedDirectory(fs::canonical(root));
    std::set<std::string> seen;
    for(const auto& session:fs::directory_iterator(root)) {
        try {
            const auto name=media::pathUtf8(session.path().filename());
            if(!name.starts_with(sessionPrefix) || !hexId(std::string_view(name).substr(sessionPrefix.size())))
                continue;
            const auto directory=checkedDirectory(session.path());
            ordinary(directory/"owner.lock",false);
            std::error_code error;
            auto lease=media::ProjectFileLease::openExclusive(directory/"owner.lock",error);
            if(!lease)continue;
            const auto header=parse(readText(directory/"session.json"));
            fields(header,{"schema","pid","started"});
            if(unsignedInteger(header.at("schema"))!=1 || unsignedInteger(header.at("pid"))==0)
                continue;
            (void)unsignedInteger(header.at("started"));
            for(const auto& registration:fs::directory_iterator(directory)) {
                try {
                    const auto leaf=media::pathUtf8(registration.path().filename());
                    if(!leaf.starts_with(registrationPrefix) || !leaf.ends_with(".json"))
                        continue;
                    const auto record=parse(readText(registration.path()));
                    fields(record,{"schema","recordingId","directory"});
                    const auto id=textField(record.at("recordingId"));
                    if(unsignedInteger(record.at("schema"))!=1 || !hexId(id) ||
                       leaf!=registrationName(id) || seen.contains(id))
                        continue;
                    const auto target=media::pathFromUtf8(textField(record.at("directory"),65536));
                    auto take=openInactive(target);
                    if(take.plan.recordingId!=id || absent(target/"start.json"))
                        continue;
                    bool hasAudio=true;
                    for(std::size_t i=0;i<take.plan.targets.size();++i) {
                        const auto channels=static_cast<std::uint16_t>(take.plan.targets[i].channels.size());
                        if(inspectWave(target/takeName(i),take.plan.identities[i],
                                       {take.plan.sampleRate,channels,take.plan.format}).frames==0) {
                            hasAudio=false;
                            break;
                        }
                    }
                    if(!hasAudio)continue;
                    auto& item=result.emplace_back(LaunchTake{target,take.plan.sourceProject,take.plan.projectUuid,id,{}});
                    for(const auto& track:take.plan.targets)item.tracks.push_back(track.trackId);
                    seen.insert(id);
                } catch(const std::exception&) {
                    // A replaced, malformed or incomplete take is neither
                    // repaired nor exposed as a launch candidate.
                }
            }
        } catch(const std::exception&) {
            // One incomplete marker does not hide other interrupted sessions.
        }
    }
    return result;
}

OwnedPlan begin(const AudioRecordingSpec& spec, const fs::path& directory, std::string_view projectUuid) {
    const auto owned = checkedDirectory(directory);
    TakePlan plan;
    plan.recordingId = newId();
    plan.projectUuid = std::string(projectUuid);
    plan.sourceProject = spec.sourceProject;
    plan.sampleRate = spec.sampleRate;
    plan.format = spec.format;
    plan.placementOffsetSamples = spec.placementOffsetSamples;
    plan.targets = spec.targets;
    plan.loop = spec.loop;
    if (plan.targets.empty() || plan.targets.size() > maxTargets)
        refuse("invalid target list");
    shape(owned, plan.targets.size(), false);
    for (std::size_t i = 0; i < plan.targets.size(); ++i)
        plan.identities.push_back(takeIdentity(owned / takeName(i)));
    const auto document = encode(plan);
    std::error_code error;
    auto marker = media::ProjectFileLease::createShared(owned / "owner.lock", error);
    if (!marker)
        refuse("cannot create take ownership marker: " + error.message());
    const auto markerIdentity = marker->identity();
    marker.reset();
    // No plan exists in this create-to-exclusive interval. Another inspector
    // cannot admit an incomplete folder; contention conservatively refuses.
    auto ownership = media::ProjectFileLease::openExclusive(owned / "owner.lock", error);
    if (!ownership)
        refuse("cannot own take marker: " + error.message());
    ordinary(owned / "owner.lock", false);
    if (ownership->identity() != markerIdentity || fs::file_size(owned / "owner.lock") != 0)
        refuse("ownership marker changed before plan publication");
    auto warning = publish(owned, "take.json", document);
    return {std::move(plan), std::move(ownership), std::move(warning)};
}
TakeStart readStart(const fs::path& directory, std::string_view recordingId) {
    const auto owned = checkedDirectory(directory);
    return decodeStart(parse(readText(owned / "start.json")), recordingId);
}
std::string writeStart(const fs::path& directory, std::string_view recordingId, std::int64_t startSample,
                       const RecordingPunchWindow& punch) {
    const auto owned = checkedDirectory(directory);
    if (!hexId(recordingId) || startSample < 0)
        refuse("invalid first accepted sample");
    const auto plan = decode(parse(readText(owned / "take.json")));
    if (plan.recordingId != recordingId)
        refuse("cannot place another recording's start");
    if (!absent(owned / "start.json")) {
        if (readStart(owned, recordingId) != TakeStart{startSample, punch})
            refuse("first accepted sample is already frozen");
        return {};
    }
    const auto boundary = [](const std::optional<RecordingPunchWindow::Boundary>& value) {
        return value ? Json{{"sample", value->sample}, {"tick", value->tick}} : Json(nullptr);
    };
    return publish(owned, "start.json", {{"schema", 2},
        {"recordingId", std::string(recordingId)}, {"startSample", startSample},
        {"punchIn", boundary(punch.in)}, {"punchOut", boundary(punch.out)}});
}
OwnedPlan openInactive(const fs::path& directory) {
    const auto owned = checkedDirectory(directory);
    ordinary(owned / "owner.lock", false);
    if (fs::file_size(owned / "owner.lock") != 0)
        refuse("ownership marker has unexpected data");
    std::error_code error;
    auto ownership = media::ProjectFileLease::openExclusive(owned / "owner.lock", error);
    if (!ownership)
        refuse("take is active or inaccessible: " + error.message());
    (void)checkedDirectory(owned);
    ordinary(owned / "owner.lock", false);
    auto plan = decode(parse(readText(owned / "take.json")));
    shape(owned, plan.targets.size(), true);
    for (std::size_t i = 0; i < plan.targets.size(); ++i) {
        if (takeIdentity(owned / takeName(i)) != plan.identities[i])
            refuse("owned take identity changed");
    }
    if (!absent(owned / "start.json"))
        (void)readStart(owned, plan.recordingId);
    return {std::move(plan), std::move(ownership), {}};
}
}
