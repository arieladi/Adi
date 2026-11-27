// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include "adi/engine/process.hpp"
#include "adi/engine/transport.hpp"
#include "adi/media/peaks.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>
namespace adi::engine {
class Transport;
struct AudioMemorySnapshot;
struct TempoMap;
namespace recording_recovery { struct OwnedPlan; }
enum class RecordingSource { PhysicalInput, MainMix };
struct AudioMemoryTakeSpec {
    std::filesystem::path workFolder;
    std::string sourceProject; // resolved by recording::captureMemory on the owner
    std::int64_t trackId=0,startSample=0; // explicit destination timeline, NOT input stream clock
};
struct RecordingTarget {
    std::int64_t trackId=0;
    std::vector<std::uint32_t> channels; // indices in AudioIo.in, or 0/1 for MainMix
    std::optional<std::int64_t> placementOffsetSamples{}; // linear take override for this track
    RecordingSource source = RecordingSource::PhysicalInput;
};
/// Explicit frozen sample-domain loop contract. One row per nonempty pass,
/// allocated before callback publication. Musical loop retiming ends this job.
struct AudioRecordingLoop {
    std::int64_t beginSample=0,endSample=0;
    std::uint32_t maxPasses=1024; // 1..65536; exhaustion refuses a whole callback
    std::shared_ptr<const TempoMap> tempo{}; // authored and playback clocks agreed at Arrangement admission
};
struct RecordedPass {
    std::uint64_t sourceStartFrame=0,frames=0;
    std::int64_t timelineStartSample=0,timelineEndSample=0; // half-open
};
// Subtract the signed correction from the captured timeline position and clamp
// at song zero. An unrepresentable sample position cannot be placed.
[[nodiscard]] std::optional<std::int64_t> recordingPlacementStart(std::int64_t startSample,
                                                                 std::int64_t offsetSamples) noexcept;
[[nodiscard]] std::int64_t recordingLatencyCorrection(std::int64_t driverSamples, double sampleRate,
                                                      double errorMilliseconds) noexcept;
// Intersect a latency-corrected linear take with its captured punch bounds.
[[nodiscard]] std::optional<RecordedPass> recordingPlacementRange(std::int64_t startSample,
    std::uint64_t frames,std::int64_t offsetSamples,const RecordingPunchWindow&) noexcept;
struct AudioRecordingSpec {
    std::filesystem::path workFolder;
    std::string sourceProject; // Session supplies its resolved project identity
    std::uint32_t sampleRate=44100,maxFrames=512,ringFrames=88200;
    audio::WavFormat format=audio::WavFormat::Float32;
    std::vector<RecordingTarget> targets;
    std::optional<AudioRecordingLoop> loop; // absent retains linear capture
    std::int64_t placementOffsetSamples = 0; // captured with the device settings for a linear take
    // The document owner supplies this through recording::configureRecovery.
    // Empty keeps transient callers and custom recorder sinks unchanged.
    std::string recoveryProjectUuid{};
};
enum class RecordingFailure : std::uint8_t {None,Overflow,MissingInput,BlockSize,Discontinuity,Cancelled,Io,NoFrames,InvalidCapture,PassLimit};
struct RecordedTrack {
    std::int64_t trackId=0;
    std::uint16_t channels=0;
    std::filesystem::path file;
    std::optional<std::int64_t> placementOffsetSamples{};
};
struct AudioRecordingPreview {
    struct Track {
        std::int64_t trackId=0;
        std::vector<std::uint32_t> channels; // indices into the preview's unique input channels
        std::optional<std::int64_t> placementOffsetSamples{}; // linear track override
    };
    std::int64_t startSample=0; // clip position on the timeline after the default placement correction
    std::int64_t rawStartSample=0; // captured start used by a track's linear override
    media::MediaPeaks peaks; // in file-frame order, shared by tracks with the same input
    std::vector<Track> tracks;
    struct PunchedPlacement {
        std::int64_t rawStartSample, offsetSamples;
        RecordingPunchWindow window;
    };
    std::optional<PunchedPlacement> punchedPlacement;
    std::optional<AudioRecordingLoop> loop; // bounds frozen when the recording starts
    struct Range {
        std::int64_t startSample;
        std::uint64_t sourceStartFrame, frames;
    };
    [[nodiscard]] Range latestPass(std::uint64_t acceptedFrames,
                                   std::optional<std::int64_t> trackOffsetSamples = {}) const noexcept;
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
    std::int64_t placementOffsetSamples=0;
    std::vector<RecordedTrack> tracks;
    std::shared_ptr<const AudioRecordingPreview> preview;
    [[nodiscard]] const std::filesystem::path& directory()const noexcept;
    [[nodiscard]] const std::string& sourceProject()const noexcept{return sourceProject_;}
    [[nodiscard]] const std::optional<AudioRecordingLoop>& loopSpec()const noexcept{return loop_;}
    [[nodiscard]] const RecordingPunchWindow& punchWindow()const noexcept{return punchWindow_;}
    [[nodiscard]] std::span<const RecordedPass> passes()const noexcept{return passes_;}
    [[nodiscard]] bool hasAttachableAudio()const noexcept;
    [[nodiscard]] const std::string& recordingId()const noexcept{return recordingId_;}
    [[nodiscard]] const std::string& recoveryProjectUuid()const noexcept{return recoveryProjectUuid_;}
    [[nodiscard]] const std::string& recoveryWarning()const noexcept{return recoveryWarning_;}
    [[nodiscard]] bool recovered()const noexcept{return recovered_;}
    [[nodiscard]] const std::filesystem::path& recoverySource()const noexcept{return recoverySource_;}
    // Owner thread, after recording has finished. A cleanup error retains the
    // directory reference so the owner can report and retry it.
    std::error_code discard();
    // Owner/worker only. Creates a private Float32 WAV, preserving snapshot
    // bits, for the existing prepareCommit/commit publication and undo path.
    // No project rows change; failure cleans only this attempt's private work.
    // Keep the snapshot for retry. Placement is supplied explicitly in session
    // samples at the captured rate, independently of the input stream clock.
    [[nodiscard]] static AudioRecordingResult fromMemory(const AudioMemorySnapshot&,const AudioMemoryTakeSpec&);
    // Off callback. Rebuild a private copy from the verified common PCM
    // prefix. The captured files remain intact until the project places it.
    [[nodiscard]] static AudioRecordingResult fromInterrupted(
        recording_recovery::OwnedPlan, const std::filesystem::path& directory, std::string sourceProject,
        std::stop_token = {});
private:
    struct Files;
    std::shared_ptr<Files> files_;
    std::string sourceProject_;
    std::optional<AudioRecordingLoop> loop_;
    RecordingPunchWindow punchWindow_;
    std::vector<RecordedPass> passes_;
    bool finalizedPrefix_=false;
    std::string recordingId_,recoveryProjectUuid_,recoveryWarning_;
    std::filesystem::path recoverySource_;
    bool recovered_=false;
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
/// device stopped. Linear capture remains the default. Explicit loop capture
/// keeps one source per target and a bounded pass log; it does not publish take
/// lanes. Seeks, stream discontinuities and loop reconfiguration fail explicitly.
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
    // Callback only. Returns the sorted track IDs whose input was accepted in
    // this slice. The view is valid until the next capture call on the driver.
    std::span<const std::int64_t> capture(const AudioIo&,const Transport&,
                                         RecordingPunch=RecordingPunch::Within,
                                         const RecordingPunchWindow& = {}, const TempoMap* clock=nullptr,
                                         std::uint64_t clockGeneration=0)noexcept;
    struct RenderSlice {
        void* context = nullptr;
        // Main output, when requested, is copied in stereo from the just-rendered graph.
        bool (*process)(void*, std::span<const std::int64_t> recordingTracks,
                        std::span<const std::int64_t> resamplingTracks, const AudioIo* mainOutput) noexcept = nullptr;
    };
    struct SliceResult {
        bool rendered = false, captured = false;
    };
    // Pins the same recorder job across graph rendering. Physical-only takes keep
    // their pre-render write. Mixed inputs have one common post-render write.
    SliceResult captureAndRender(const AudioIo&, const Transport&, RecordingPunch,
                                 const RecordingPunchWindow&, const TempoMap*, std::uint64_t clockGeneration,
                                 RenderSlice) noexcept;
    // Owner-thread status queries.
    [[nodiscard]] bool pending()const noexcept;
    [[nodiscard]] std::uint64_t acceptedFrames()const noexcept;
    // Writer-published peaks for the prefix written to every target so far.
    [[nodiscard]] std::shared_ptr<const AudioRecordingPreview> preview()const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
