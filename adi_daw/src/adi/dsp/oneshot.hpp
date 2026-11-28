// SPDX-License-Identifier: GPL-3.0-or-later
// Original ADI sampler recovered from arieladi/Adi#206, with #207's independent
// parameter modulation. Its values and DSP laws remain unverified against Live.
#pragma once
#include "adi/engine/publisher.hpp"
#include "juce/device_model.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <span>
namespace adi::device {
class OneShot final : public DeviceInstance {
  public:
    // Opaque immutable preparation. Construction/copying/analysis happens only
    // in prepareSample(), never when the audio thread adopts a sample slot.
    class PreparedSample {
        friend class OneShot;
        PreparedSample() = default;
        std::vector<float> data;
        int channels = 1;
        double rate = 48000;
        std::vector<std::size_t> manual, zeros;
        /// SC-482: the transient slices at each step of Sensitivity, 0 % to
        /// 100 % in 5 % steps; step 10 (50 %, the default) is the one list
        /// OneShot found before Sensitivity existed, found the same way.
        static constexpr int kSensitivitySteps = 21;
        std::array<std::vector<std::size_t>, kSensitivitySteps> transients;
        std::size_t frames() const noexcept {
            return data.size() / static_cast<std::size_t>(channels);
        }
      public:
        ~PreparedSample() = default;
        PreparedSample(const PreparedSample&) = delete;
        PreparedSample& operator=(const PreparedSample&) = delete;
    };
    enum Param {
        Mode,
        Start,
        End,
        Length,
        LoopOn,
        LoopLength,
        Fade,
        Snap,
        Voices,
        Retrigger,
        Gate,
        SliceBy,
        Regions,
        BeatDivision,
        SlicePlayback,
        SampleBpm,
        Gain,
        Volume,
        Transpose,
        Detune,
        Attack,
        Decay,
        Sustain,
        Release,
        FadeIn,
        FadeOut,
        FilterOn,
        FilterType,
        Slope,
        Cutoff,
        Resonance,
        LfoOn,
        LfoShape,
        LfoRate,
        LfoSync,
        LfoBeat,
        LfoPitch,
        LfoVolume,
        LfoPan,
        LfoFilter,
        VelToVol,
        // SC-482: added after VelToVol, never renumbered. Each new control's
        // default plays exactly what OneShot played before it existed.
        FilterEnvAttack,
        FilterEnvDecay,
        FilterEnvSustain,
        FilterEnvRelease,
        FilterEnvAmount,
        PitchEnvOn,
        PitchEnvAttack,
        PitchEnvDecay,
        PitchEnvSustain,
        PitchEnvRelease,
        PitchEnvAmount,
        FilterCircuit,
        FilterCircuitBp,
        Drive,
        FilterVel,
        FilterKey,
        LfoAttack,
        LfoRetrigger,
        LfoOffset,
        LfoKey,
        GlideMode,
        GlideTime,
        Pan,
        PanRandom,
        Spread,
        SliceSensitivity,
        Count
    };
    /// SC-482: the filter circuits, as Live's two choosers name them. Low- and
    /// high-pass take all five; band-pass and notch take Clean and OSR.
    enum Circuit { Clean, Osr, Ms2, Smp, Prd };
    /// The parameters a project saved before SC-482 holds (VelToVol was last).
    static constexpr int kLegacyCount = VelToVol + 1;
    OneShot();
    const DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    engine::EventFlow eventFlow() const noexcept override { return engine::EventFlow::Consume; }
    void prepare(double, std::int32_t) override;
    void process(const engine::NodeIo &) noexcept override;
    /// Audio thread, before process(): the gain the NEXT note-on in this
    /// call's events takes, latched for its voice (DF-E37 e). Gains queue in
    /// order, one per note-on; a note-on with none queued plays at 1. The
    /// Sampler sends each zone's crossfade weight this way, so Vel > Vol acts
    /// on the played velocity only.
    void queueNoteGain(double gain) noexcept;
    // Audio thread. The caller owns sample for the whole call; no ownership is
    // retained. Use a single publication source per instance until prepare().
    // A changed nonzero generation resets voices. Null or generation zero
    // clears them and plays silence. Empty replacements still need a new seq.
    void processPrepared(const engine::NodeIo&, const PreparedSample*,
                         std::uint64_t generation) noexcept;
    void resetVoices() noexcept {
        voices_ = {};
        noteGainHead_ = noteGainCount_ = 0;
    }
    /// Audio thread: whether any voice sounds. A OneShot with none, given no
    /// note-on, plays exact silence (each voice's filter is its own: no tail).
    [[nodiscard]] bool sounding() const noexcept {
        for (const auto &v : voices_)
            if (v.active)
                return true;
        return false;
    }
    /// The next prepare() starts fresh (DF-E37 f).
    void release() override { prepared_ = false; }
    void pumpMainThread() override;
    std::int32_t paramCount() const noexcept override { return Count; }
    const ParamDescriptor *paramAt(std::int32_t) const noexcept override;
    ParamValue getParam(const std::string &) const noexcept override;
    bool setParam(const std::string &, const ParamValue &) override;
    /// Any thread (an atomic store): parameter index i to a normalized value,
    /// canonical as setParam stores it, without a string lookup (DEV-21).
    void setParamAt(int i, double normalized) noexcept;
    /// The same, from a value in the parameter's own unit.
    void setRealAt(int i, double value) noexcept;
    std::string paramText(const std::string &, double) const override;
    std::vector<std::string> stateRoles() const override { return {"component"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override;
    bool loadState(const std::string &, const std::vector<std::uint8_t> &) override;
    // Message thread. Copies/validates/analyses before the immutable publication.
    bool publishSample(std::span<const float> interleaved, int channels, double rate,
                       std::span<const std::size_t> manualSlices = {});
    [[nodiscard]] static std::shared_ptr<const PreparedSample> prepareSample(
        std::span<const float> interleaved, int channels, double rate,
        std::span<const std::size_t> manualSlices = {});
    // Sampler (DEV-21): set only before this voice engine is published by
    // Sampler. Classic mode pitches each key against it; Slice ignores it.
    void setRootKey(int key) noexcept { rootKey_ = std::clamp(key, 0, 127); }
    bool loadSample(const std::filesystem::path &, std::string &error);

    // Sampler gives each layer the same channel state at the start of a block,
    // including controls played before the layer had a sample.
    struct MidiChannel {
        double bend = 0;
        double modulation = 1; // Keep the authored depth until CC1 arrives.
        bool sustain = false;
    };
    using MidiChannels = std::array<MidiChannel, 16>;
    static void rememberMidiControl(MidiChannels &, const engine::Event &) noexcept;
    void setMidiChannels(const MidiChannels &channels) noexcept { midiChannels_ = channels; }

  private:
    using Sample = PreparedSample;
    struct Publication : engine::Sequenced {
        std::shared_ptr<const Sample> sample;
    };
    struct Filter {
        double a = 0, b = 0;
        double run(double x, double g, double r, double d, int type) noexcept;
        /// SC-482: the same state-variable stage through a circuit's
        /// saturation (ADI's laws; Live's are a reading still to take).
        /// `drive` is the input gain, applied by the first stage only.
        double runCircuit(double x, double g, double r, double d, int type, int circuit,
                          double drive) noexcept;
    };
    /// SC-482: a modulation envelope (filter, pitch): linear segments as the
    /// amp envelope's, from 0 at note-on. Stage 0 attack, 1 decay, 2 sustain,
    /// 3 release. Live's segment slopes are a reading still to take.
    struct Envelope {
        double value = 0, releaseStep = 0;
        int stage = 0;
        void run(double attackSamples, double decaySamples, double sustain) noexcept;
        void release(double releaseSamples) noexcept;
    };
    struct Voice {
        bool active = false, held = false, sustained = false;
        std::uint64_t id = 0, age = 0;
        int key = 60, stage = 0;
        Envelope filterEnv, pitchEnv;
        // SC-482: semitones still to slide to the voice's own pitch, and the
        // slide's step a sample (Glide and Portamento).
        double glide = 0, glideStep = 0;
        // SC-482: this note's random pan, -1..1 times Pan < Rnd, drawn at note-on.
        double randomPan = 0;
        std::uint8_t channel = 0;
        double position = 0, begin = 0, end = 0, loop = 0, env = 0, releaseStep = 0, velocity = 1,
               phase = 0, random = 0, tuning = 0, gain = 1;
        std::uint32_t rng = 1;
        std::uint64_t played = 0;
        // The last pitch's read increment (exp2 is per pitch, not per sample).
        double pitchSeen = std::numeric_limits<double>::quiet_NaN(), increment = 0;
        // SC-482 Spread: the second, lower voice of the note's pair: its own
        // place in the sample and read increment (the first is position and
        // increment, detuned up), and the detune the increments were made for.
        double position2 = 0, increment2 = 0, spreadSeen = 0;
        // The last cutoff offset's filter coefficients (tan is per offset, not
        // per sample): the same offset gives the same coefficients, bit for bit.
        double cutoffSeen = std::numeric_limits<double>::quiet_NaN(), g = 0, r = 0, d = 0;
        Filter filters[2][2];
    };
    /// Gains for the next note-ons, in order (a zone weight from the Sampler).
    std::array<double, 256> noteGains_{};
    std::size_t noteGainHead_ = 0, noteGainCount_ = 0;
    void event(const engine::Event &, const Sample *, std::array<double, Count> &) noexcept;
    static bool slice(int key, const Sample &s, const std::array<double, Count> &p, double &begin,
                      double &end) noexcept;
    void noteOn(const engine::Event &, const Sample &, const std::array<double, Count> &) noexcept;
    void releaseVoice(Voice &, const std::array<double, Count> &) noexcept;
    double read(const Sample &, double, int) const noexcept;
    void readBoth(const Sample &, double position, double *x) const noexcept;
    void processSample(const engine::NodeIo&, const Sample*, std::uint64_t,
                       bool borrowed) noexcept;
    std::array<ParamDescriptor, Count> params_;
    std::array<std::atomic<double>, Count> values_{};
    std::array<Voice, 32> voices_{};
    MidiChannels midiChannels_{};
    std::array<double, Count> modulation_{};
    engine::SnapshotPublisher<Publication> publisher_;
    std::shared_ptr<const Sample> sample_; // message thread only, including saveState
    std::uint64_t sampleSeq_ = 0, age_ = 0;
    /// SC-482: the LFO's free-running phase, 0..1: where a voice's LFO starts
    /// when LFO Retrigger is off. It runs while nothing sounds, too.
    double lfoClock_ = 0;
    /// SC-482: Pan < Rnd's own generator, so the LFO's random shape is untouched.
    std::uint32_t panRng_ = 1;
    int rootKey_ = 60;
    bool borrowed_ = false;
    double rate_ = 48000;
    std::int32_t frames_ = 0;
    bool prepared_ = false;
    DeviceIdentity identity_{"internal", "adi.oneshot", "OneShot", "ADI", "1"};
};
} // namespace adi::device
