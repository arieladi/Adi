// SPDX-License-Identifier: GPL-3.0-or-later
// Original ADI sampler recovered from arieladi/Adi#206, with #207's independent
// parameter modulation. Its values and DSP laws remain unverified against Live.
#pragma once
#include "adi/engine/publisher.hpp"
#include "juce/device_model.hpp"
#include <algorithm>
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
        std::vector<std::size_t> transients, manual, zeros;
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
        Count
    };
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
    /// on the played velocity only. `reverse` (SC-483) plays that note from
    /// its end to its start. `cents` and `pan` (-1 left .. 1 right) are that
    /// voice's own, for the Sampler's Spread; a `partner` is the second voice
    /// of such a pair and does not retrigger the first.
    void queueNoteGain(double gain, bool reverse = false, double cents = 0, double pan = 0,
                       bool partner = false) noexcept;
    /// SC-483, for the Sampler's layers: how a voice travels its sample beyond
    /// OneShot's own controls. Positions are fractions of the whole sample. A
    /// fresh Travel is OneShot's own behaviour, bit for bit.
    struct Travel {
        double loopEnd = -1;    ///< where the sustain loop ends; below 0, at the region's end
        bool pingPong = false;  ///< the sustain loop runs forward and back
        /// After note-off: 0 stay in the sustain loop, 1 play on to the end,
        /// 2 play on and loop from releaseLoop to the end, 3 that loop forward and back.
        int release = 0;
        double releaseLoop = 0; ///< where the release loop starts
        double releaseFade = 0; ///< its crossfade, 0..1 of half its length, as Fade is the sustain loop's
        bool linearFade = false; ///< loop crossfades are linear instead of constant-power
        /// Live's loop Detune: cents a voice plays at while it is inside the
        /// sustain loop, and inside the release loop after note-off.
        double loopCents = 0, releaseCents = 0;
        /// The instrument's Detune, in cents, for every voice.
        double cents = 0;
        /// How a transposed sample is read between its frames: 0 the nearest
        /// frame below, 1 a straight line (OneShot's own), 2 a four-point cubic.
        int interpolation = 1;
    };
    /// Audio thread, before process() (as setRootKey).
    void setTravel(const Travel &travel) noexcept { travel_ = travel; }
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
    };
    struct Voice {
        bool active = false, held = false, sustained = false;
        std::uint64_t id = 0, age = 0;
        int key = 60, stage = 0;
        std::uint8_t channel = 0;
        double position = 0, begin = 0, end = 0, loop = 0, env = 0, releaseStep = 0, velocity = 1,
               phase = 0, random = 0, tuning = 0, gain = 1;
        std::uint32_t rng = 1;
        std::uint64_t played = 0;
        // The last pitch's read increment (exp2 is per pitch, not per sample).
        double pitchSeen = std::numeric_limits<double>::quiet_NaN(), increment = 0;
        // SC-483: the sustain loop's end, the release loop's start, and for a
        // reversed voice the frame that position 0 reads (position p reads
        // mirror - p). backward: a forward-and-back loop on its way back.
        // tail: released, and out of the sustain loop for the rest of the sample.
        double loopEnd = 0, releaseLoop = 0, mirror = 0;
        bool backward = false, tail = false, reversed = false;
        double spreadCents = 0, pan = 0;  // SC-483: a Spread voice's own, from its ticket
        Filter filters[2][2];
    };
    /// What the next note-ons take, in order (a zone weight from the Sampler).
    struct NoteTicket {
        double gain = 1;
        bool reverse = false;
        double cents = 0, pan = 0;
        bool partner = false;
    };
    std::array<NoteTicket, 256> noteGains_{};
    std::size_t noteGainHead_ = 0, noteGainCount_ = 0;
    Travel travel_{};
    void event(const engine::Event &, const Sample *, std::array<double, Count> &) noexcept;
    static bool slice(int key, const Sample &s, const std::array<double, Count> &p, double &begin,
                      double &end) noexcept;
    void noteOn(const engine::Event &, const Sample &, const std::array<double, Count> &) noexcept;
    void releaseVoice(Voice &, const std::array<double, Count> &) noexcept;
    double read(const Sample &, double, int, int quality = 1) const noexcept;
    void readBoth(const Sample &, double position, double *x, int quality = 1) const noexcept;
    /// SC-483: a four-point cubic (Catmull-Rom) between frames a and a + 1.
    static double cubic(const Sample &, std::size_t a, std::size_t channel, double t) noexcept;
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
    int rootKey_ = 60;
    bool borrowed_ = false;
    double rate_ = 48000;
    std::int32_t frames_ = 0;
    bool prepared_ = false;
    DeviceIdentity identity_{"internal", "adi.oneshot", "OneShot", "ADI", "1"};
};
} // namespace adi::device
