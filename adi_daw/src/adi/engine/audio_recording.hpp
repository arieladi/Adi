// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include "adi/engine/process.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace adi::engine {
class Transport;
struct RecordingTarget {
    std::int64_t trackId=0;
    std::vector<std::uint32_t> channels; // indices in the current AudioIo.in
};
struct AudioRecordingSpec {
    std::filesystem::path workFolder;
    std::string sourceProject; // Session supplies its resolved project identity
    std::uint32_t sampleRate=44100,maxFrames=512,ringFrames=88200;
    audio::WavFormat format=audio::WavFormat::Float32;
    std::vector<RecordingTarget> targets;
};
enum class RecordingFailure : std::uint8_t {None,Overflow,MissingInput,BlockSize,Discontinuity,Cancelled,Io,NoFrames};
struct RecordedTrack {
    std::int64_t trackId=0;
    std::uint16_t channels=0;
    std::filesystem::path file;
};
/// A completed/failed capture is retained on disk for retry/recovery. Only
/// discard() or successful publication removes its newly owned directory.
class AudioRecordingResult {
public:
    RecordingFailure failure=RecordingFailure::None;
    std::string error;
    std::uint32_t sampleRate=0;
    std::uint64_t frames=0;
    std::int64_t startSample=0;
    std::vector<RecordedTrack> tracks;
    [[nodiscard]] const std::filesystem::path& directory()const noexcept;
    [[nodiscard]] const std::string& sourceProject()const noexcept{return sourceProject_;}
    void discard(); // owner thread, after recording has finished
private:
    struct Files;
    std::shared_ptr<Files> files_;
    std::string sourceProject_;
    friend class AudioInputRecorder;
};
/// Optional off-callback writer adapter. The default is an actual WavWriter.
/// A sink is created before publication and thereafter used by one worker.
class AudioRecordingSink {
public:
    virtual ~AudioRecordingSink()=default;
    virtual void write(std::span<const float> interleaved,std::uint32_t frames)=0;
    virtual void close()=0;
};
using AudioRecordingSinkFactory=std::function<std::unique_ptr<AudioRecordingSink>(
    const std::filesystem::path&,std::uint32_t,std::uint16_t,audio::WavFormat)>;
/// One owner and one callback reader, matching SnapshotPublisher. Construction,
/// start/stop/poll/destruction are off the callback. Destruction requires the
/// device stopped. Initial implementation captures continuous linear takes;
/// a seek/loop discontinuity fails explicitly instead of attaching wrong audio.
class AudioInputRecorder {
public:
    AudioInputRecorder();
    ~AudioInputRecorder();
    AudioInputRecorder(const AudioInputRecorder&)=delete;
    AudioInputRecorder& operator=(const AudioInputRecorder&)=delete;
    bool start(const AudioRecordingSpec&,std::string& error,AudioRecordingSinkFactory={});
    void stop(bool cancel=false);
    /// Returns a result once the callback has moved past the stopped capture
    /// and the worker has drained/finalized it. A result is returned once.
    /// deviceStopped=true requires the driver already quiescent.
    [[nodiscard]] std::optional<AudioRecordingResult> poll(bool deviceStopped=false);
    void capture(const AudioIo&,const Transport&)noexcept; // callback only
    // Owner-thread status queries.
    [[nodiscard]] bool pending()const noexcept;
    [[nodiscard]] std::uint64_t acceptedFrames()const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
