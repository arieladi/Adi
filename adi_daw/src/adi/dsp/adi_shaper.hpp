// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/adi_shaper_audio_trigger.hpp"
#include "adi/dsp/adi_shaper_crush.hpp"
#include "adi/dsp/adi_shaper_drive.hpp"
#include "adi/dsp/adi_shaper_filter.hpp"
#include "adi/dsp/adi_shaper_noise.hpp"
#include "adi/dsp/adi_shaper_reverb.hpp"
#include "adi/dsp/adi_shaper_follower.hpp"
#include "adi/dsp/adi_shaper_follower_filter.hpp"
#include "adi/dsp/adi_shaper_sweep.hpp"
#include "adi/dsp/adi_shaper_time.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adi::dsp {

class AdiShaper {
  public:
    // Each stage stores layout and two split frequencies, followed by the
    // low, middle and high band's controls. These IDs are persisted by Pd.
    static constexpr int bandParameterCount = 13;
    static constexpr int stageHeaderSize = 3;
    static constexpr int stageParameterCount = stageHeaderSize + 3 * bandParameterCount;
    static constexpr int modHeaderSize = 6;
    static constexpr int modBandParameterCount = 23;
    static constexpr int modStageParameterCount = modHeaderSize + 3 * modBandParameterCount;
    static constexpr int driveBandParameterCount = 16;
    enum class Stage { Volume, Width, Pan, Filter, Sweep, Drive, Crush, Time, Noise = 9, Reverb };
    // Saved stage identities also address ranks and curves. Unavailable stages leave holes.
    static constexpr std::array activeStages{Stage::Volume, Stage::Width, Stage::Pan, Stage::Filter,
                                             Stage::Sweep, Stage::Drive, Stage::Crush, Stage::Time, Stage::Noise, Stage::Reverb};
    static constexpr std::size_t stageCount = activeStages.size();
    static constexpr std::size_t stageSlots = static_cast<std::size_t>(Stage::Reverb) + 1;
    static constexpr int noiseBandParameterCount = modBandParameterCount + 5;
    static constexpr int reverbBandParameterCount = modBandParameterCount - 1 + 6;
    static constexpr int timeBandParameterCount = 14;
    static constexpr int followerParameterCount = 9;
    static constexpr int HoldFollowerCount = 9;
    enum Param : int { MasterMix, VolumeBands, WidthBands = VolumeBands + stageParameterCount,
                 PanBands = WidthBands + stageParameterCount,
                 VolumeLowSlope = PanBands + stageParameterCount, WidthLowSlope, PanLowSlope,
                 VolumeHighSlope, WidthHighSlope, PanHighSlope, FilterOn,
                 SweepOn = FilterOn + modStageParameterCount, SweepLowPolarity = SweepOn + modStageParameterCount,
                 SweepMidPolarity, SweepHighPolarity, DriveOn,
                 CrushOn = DriveOn + modHeaderSize + 3 * driveBandParameterCount,
                 BaseCount = CrushOn + modStageParameterCount,
                 TimeOn = BaseCount, TimeEnd = TimeOn + modHeaderSize + 3 * timeBandParameterCount,
                 NoiseOn = 535, NoiseEnd = NoiseOn + modHeaderSize + 3 * noiseBandParameterCount,
                 ReverbOn = 625, ReverbEnd = ReverbOn + modHeaderSize + 3 * reverbBandParameterCount,
                 VolumeOrder = 715,
                 WidthFollowerLow = 726, WidthFollowerEnd = WidthFollowerLow + 3 * followerParameterCount,
                 TriggerLowMode = 753, TriggerMidMode, TriggerHighMode, TriggerLowEnd, TriggerMidEnd, TriggerHighEnd,
                 TriggerThreshold, TriggerDetail, TriggerProfile, TriggerShift, TriggerSmoothing,
                 FilterLowOffset = 849, FilterMidOffset, FilterHighOffset,
                 SweepLowOffset = 852, SweepMidOffset, SweepHighOffset,
                 PanLowDelayBlend = 855, PanMidDelayBlend, PanHighDelayBlend,
                 CrushLowBias = 864, CrushMidBias, CrushHighBias,
                 HoldFollowerLow = 876, HoldFollowerSourceLow = HoldFollowerLow + 3 * HoldFollowerCount,
                 HoldHighPassLow = HoldFollowerSourceLow + 3, HoldLowPassLow = HoldHighPassLow + 3,
                 HoldHighLowPass = HoldLowPassLow + 2,
                 FilterLowPeak = 912, FilterMidPeak, FilterHighPeak,
                 FilterLowSteep = 915, FilterMidSteep, FilterHighSteep,
                 PanLowTransfer = 918, PanMidTransfer, PanHighTransfer,
                 PanLowDelayRange = 921, PanMidDelayRange, PanHighDelayRange,
                 PanLowGainLaw = 924, PanMidGainLaw, PanHighGainLaw,
                 DriveLowRounded = 927, DriveMidRounded, DriveHighRounded,
                 DriveLowPower, DriveMidPower, DriveHighPower,
                 DriveLowFold, DriveMidFold, DriveHighFold, Count };
    using StageRanks = std::array<std::uint8_t, stageSlots>;
    using StageOrder = std::array<Stage, stageCount>;
    static constexpr Param orderParam(Stage stage) noexcept {
        return static_cast<Param>(VolumeOrder + static_cast<int>(stage));
    }
    [[nodiscard]] static StageOrder orderFor(const StageRanks& ranks) noexcept;
    enum class Band { Low, Mid, High };
    enum FollowerControl { FollowerOn, FollowerMode, FollowerAmount, FollowerDepth, FollowerOffset,
                           FollowerThreshold, FollowerAttack, FollowerHold, FollowerRelease };
    static constexpr Param followerParam(Band band, FollowerControl control) noexcept {
        return static_cast<Param>(WidthFollowerLow + followerParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }
    static constexpr Param holdFollowerParam(Band band, AdiShaperFollower::Control control) noexcept {
        return static_cast<Param>(HoldFollowerLow + HoldFollowerCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }
    static constexpr Param holdFollowerSourceParam(Band band) noexcept {
        return static_cast<Param>(HoldFollowerSourceLow + static_cast<int>(band));
    }
    static constexpr Param holdHighPassParam(Band band) noexcept {
        return static_cast<Param>(HoldHighPassLow + static_cast<int>(band));
    }
    static constexpr Param holdLowPassParam(Band band) noexcept {
        return static_cast<Param>(HoldLowPassLow + static_cast<int>(band));
    }
    enum BandControl { Mix, Clock, Cycle, Rate, Curve, Point1, Point2, Point3, Point4,
                       Point5, Point6, Point7, Point8 };
    static constexpr Param bandParam(Stage stage, Band band, BandControl control) noexcept {
        return static_cast<Param>(VolumeBands + stageHeaderSize + stageParameterCount * static_cast<int>(stage)
                                  + bandParameterCount * static_cast<int>(band) + static_cast<int>(control));
    }
    enum ModControl { Type, ModMix, ModClock, ModCycle, ModRate, PrimaryCurve, SecondaryCurve,
                      Primary1, Primary2, Primary3, Primary4, Primary5, Primary6, Primary7, Primary8,
                      Secondary1, Secondary2, Secondary3, Secondary4, Secondary5, Secondary6, Secondary7, Secondary8 };
    static constexpr Param modParam(Stage stage, Band band, ModControl control) noexcept {
        if (stage == Stage::Noise)
            return static_cast<Param>(NoiseOn + modHeaderSize + noiseBandParameterCount * static_cast<int>(band)
                                      + static_cast<int>(control));
        if (stage == Stage::Reverb) {
            // The room has send and return contours and no type selector.
            return static_cast<Param>(ReverbOn + modHeaderSize + reverbBandParameterCount * static_cast<int>(band)
                                      + static_cast<int>(control) - ModMix);
        }
        const int start = stage == Stage::Crush ? CrushOn : stage == Stage::Sweep ? SweepOn : FilterOn;
        return static_cast<Param>(start + modHeaderSize + modBandParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }
    enum NoiseOption { NoiseMono, NoiseOnly, NoiseTrim, NoiseHighPass, NoiseLowPass };
    static constexpr Param noiseParam(Band band, NoiseOption option) noexcept {
        return static_cast<Param>(modParam(Stage::Noise, band, Secondary8) + 1 + static_cast<int>(option));
    }
    enum ReverbOption { RoomSize, RoomDecay, RoomGate, RoomWidth, RoomPredelay, RoomClearTails };
    static constexpr Param reverbParam(Band band, ReverbOption option) noexcept {
        return static_cast<Param>(modParam(Stage::Reverb, band, Secondary8) + 1 + static_cast<int>(option));
    }
    static constexpr bool hasParameter(int id) noexcept {
        return (id >= MasterMix && id < TimeEnd) || (id >= NoiseOn && id < ReverbEnd)
            || (id >= VolumeOrder && id <= orderParam(Stage::Time))
            || (id >= orderParam(Stage::Noise) && id <= orderParam(Stage::Reverb))
            || (id >= WidthFollowerLow && id <= TriggerSmoothing)
            || (id >= FilterLowOffset && id <= FilterHighOffset)
            || (id >= SweepLowOffset && id <= SweepHighOffset)
            || (id >= CrushLowBias && id <= CrushHighBias)
            || (id >= HoldFollowerLow && id <= HoldHighLowPass)
            || (id >= FilterLowPeak && id <= FilterHighSteep)
            || (id >= PanLowDelayBlend && id <= PanHighDelayBlend)
            || (id >= PanLowTransfer && id <= PanHighGainLaw)
            || (id >= DriveLowRounded && id <= DriveHighFold);
    }
    enum DriveControl { DriveStyle, DriveMix, DriveClock, DriveCycle, DriveRate, DriveCurve,
                        Drive1, Drive2, Drive3, Drive4, Drive5, Drive6, Drive7, Drive8, InputTrim, OutputTrim };
    static constexpr Param driveParam(Band band, DriveControl control) noexcept {
        return static_cast<Param>(DriveOn + modHeaderSize + driveBandParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }
    enum TimeControl { TimeRange, TimeMix, TimeClock, TimeCycle, TimeRate, TimeCurve,
                       Time1, Time2, Time3, Time4, Time5, Time6, Time7, Time8 };
    enum class DelayRange { Follow, OneBar, TwoBars, FourBars, EightBars, Short2_5ms, Short10ms, Short20ms };
    static constexpr Param timeParam(Band band, TimeControl control) noexcept {
        return static_cast<Param>(TimeOn + modHeaderSize + timeBandParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }

    AdiShaper() noexcept;
    // Off the audio thread. Three stereo Time histories require 48 MiB per
    // prepared rack, including bypassed racks ready for immediate automation.
    void prepare(double sampleRate) noexcept;
    void set(Param id, float value) noexcept;
    void tempo(double bpm) noexcept;
    void sync(double quarterNotes, bool playing = true, unsigned historyToleranceFrames = 0) noexcept;
    void syncAtSample(double quarterNotes, bool playing, std::int64_t sample, std::uint64_t revision) noexcept;
    struct KeyInput {
        const float* left = nullptr;
        const float* right = nullptr;
        [[nodiscard]] KeyInput advanced(std::size_t frames) const noexcept {
            return {left ? left + frames : nullptr, right ? right + frames : nullptr};
        }
    };
    void useExternalWidthKey(Band band, bool enabled) noexcept;
    void useVolumeNotes(Band band, bool enabled) noexcept;
    // Apply at the start of the next process span, including spans beginning within a host block.
    void noteOn(int note, int velocity) noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 std::size_t frames) noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 std::size_t frames, KeyInput key) noexcept;
    [[nodiscard]] int latency() const noexcept {
        return trigger_.latency() + (stages_[static_cast<std::size_t>(Stage::Drive)].enabled ? AdiShaperDrive::latencySamples : 0);
    }

  private:
    struct Gain { float left = 1.0f, right = 1.0f; };
    struct Contour {
        std::array<float, 8> points{}, differences{};
        bool linear = false, constant = true;
        void update() noexcept;
        [[nodiscard]] float at(std::uint64_t position) const noexcept;
        [[nodiscard]] double heldPanAt(std::uint64_t position) const noexcept;
        [[nodiscard]] float returnAt(std::uint64_t position, std::uint64_t step) const noexcept;
    };
    struct CurveBand {
        Contour primary, secondary;
        std::uint64_t phase = 0, step = 0;
        double beats = 4.0, hz = 1.0;
        float mix = 1.0f, blend = 1.0f, returnDry = 0.0f, returnWet = 1.0f;
        int type = 0;
        bool hertz = false, active = false, followWidth = false;
        std::array<Gain, 8> heldGains{};
    };
    struct Split {
        struct Integrator { double band = 0, low = 0; };
        using StereoPole = std::array<double, 2>;
        using Lanes = std::array<float, 4>; // low left/right, high left/right
        struct Section { Lanes x1{}, x2{}, y1{}, y2{}; };
        using Cascade = std::array<Section, 2>;
        struct Bands { float low, high; };
        std::array<StereoPole, 4> poles{};
        std::array<std::array<Integrator, 3>, 2> pairs{};
        std::array<Integrator, 2> compensation{};
        Cascade branches{}, alignment{};
        Lanes numerator{}, middle{};
        float feedback1 = 0, feedback2 = 0;
        bool wide = false;
        double coefficient = 0, a1 = 0, a2 = 0, a3 = 0;
        std::array<double, 3> logFrequency{};
        double targetHz = 0, logTarget = 0;
        bool rendered = false, changing = false;
        int slope = 0;
        void prepare(double hz, double sampleRate) noexcept;
        void setFrequency(double hz, double sampleRate, bool cold) noexcept;
        bool startChunk(double sampleRate) noexcept;
        void advanceFrequency(double step, double logNyquist) noexcept;
        void steepCoefficients(double low, double high) noexcept;
        void reset() noexcept;
        [[nodiscard]] double pole(double x, double& z) const noexcept;
        void butterworth(double x, Integrator&, double& low, double& high, double& all) const noexcept;
        [[nodiscard]] std::array<Bands, 2> steep(float left, float right, Cascade&) const noexcept;
        [[nodiscard]] std::array<Bands, 2> splitStereo(float left, float right) noexcept;
        void alignStereo(float& left, float& right) noexcept;
        [[nodiscard]] Bands split(float input, std::size_t channel) noexcept;
        [[nodiscard]] float align(float input, std::size_t channel) noexcept;
        bool finishBlock() noexcept;
    };
    struct StageData {
        struct Bands { std::array<float, 2> low, mid, high; };
        std::array<CurveBand, 3> bands;
        Split low, high;
        double lowHz = 200.0, highHz = 2000.0;
        int layout = 0;
        bool active = false, enabled = false;
        [[nodiscard]] Bands firstOrderBands(float left, float right) noexcept;
    };
    struct DriveDryDelay {
        std::array<std::array<float, 2>, AdiShaperDrive::latencySamples> samples{};
        std::size_t index = 0;
        void advance(float& l, float& r) noexcept;
    };
    static bool renderFixedWidth(StageData& stage, float* left, float* right, std::size_t frames) noexcept;
    struct PanHistory {
        struct Tap {
            Gain gain;
            float crossfeed = 0.f;
            std::size_t age = 0, channel = 0;
            double fraction = 0.;
        };
        std::vector<std::array<float, 2>> frames;
        std::size_t head = 0, valid = 0;
        enum class Law { Lift3, Lift45, LinearLift, CutOnly };
        Law law = Law::Lift3;
        float current = 0.f, unity = 0.f;
        bool transfer = false;
        float rangeMs = 2.f;
        double rangeScale = 0.;
        Tap blockTap;
        void prepare(double rate) noexcept;
        void setBlend(float percent) noexcept;
        void setRange(float milliseconds) noexcept;
        [[nodiscard]] Gain gains(double position) const noexcept;
        [[nodiscard]] Tap tap(double position, double rate, bool constant) const noexcept;
        void process(const Tap& tap, float& left, float& right) noexcept;
    };
    std::array<PanHistory, 3> panHistory_;
    struct ReverbSettings { float size = .5f, decay = 1.f, predelay = 0.f; bool gate = false; };
    static Gain gain(Stage kind, float amount, float mix) noexcept;
    void updateBand(Stage kind, CurveBand& band) const noexcept;
    void updateActivity(StageData& stage) noexcept;
    void updateStage(Stage kind) noexcept;
    void updateStep(CurveBand& band) const noexcept;
    void updateSplits(StageData& stage, bool cold = false) const noexcept;
    void layout(StageData& stage, int value) noexcept;
    void slope(Split& split, float value) noexcept;
    void clock(CurveBand& band, BandControl control, float value) const noexcept;
    void setNoiseOption(std::size_t band, NoiseOption option, float value) noexcept;
    void setReverbOption(std::size_t band, ReverbOption option, float value) noexcept;
    void updateFilterMode(std::size_t band) noexcept;
    void setMod(Param id, float value) noexcept;
    void prepareStages(double sampleRate) noexcept;
    void syncClock(double quarterNotes, bool playing) noexcept;
    void updateTimeRanges() noexcept;
    void restart(Stage kind) noexcept;
    void resetKernels(Stage kind) noexcept;
    void processStages(float* outL, float* outR, std::size_t frames, KeyInput key) noexcept;
    void setTrigger(Param id, float value) noexcept;
    template <Stage kind, bool moving, bool triggered = false> void render(StageData& stage, const float* inL, const float* inR,
                                     float* outL, float* outR, std::size_t frames, KeyInput key) noexcept;
    std::array<StageData, stageSlots> stages_;
    StageRanks ranks_{};
    StageOrder order_ = activeStages;
    bool orderDirty_ = false, defaultOrder_ = true;
    std::array<bool, 3> externalWidth_{};
    std::array<AdiShaperFollower, 3> widthFollowers_;
    std::array<AdiShaperFilter, 3> filters_;
    std::array<bool, 3> filterPeak_{};
    std::array<bool, 3> filterSteep_{};
    std::array<AdiShaperSweep, 3> sweeps_;
    std::array<AdiShaperDrive, 3> drives_;
    std::array<AdiShaperCrush, 3> crush_;
    std::array<AdiShaperNoise, 3> noise_;
    std::array<std::array<float, 2>, 3> noiseFilters_{{{.254f, 1}, {.254f, 1}, {.254f, 1}}};
    std::array<AdiShaperReverb, 3> reverb_;
    std::array<ReverbSettings, 3> reverbSettings_{};
    std::array<AdiShaperTime, 3> times_;
    std::array<double, 3> timeSpans_{};
    std::array<AdiShaperFollower, 3> holdFollowers_;
    std::array<bool, 3> holdUsesFullInput_{};
    std::array<AdiShaperFollowerFilter, 3> holdInputFilters_;
    std::array<std::array<float, 2>, 3> driveTrims_{{{.5f, .5f}, {.5f, .5f}, {.5f, .5f}}};
    std::array<DriveDryDelay, 3> driveDry_;
    DriveDryDelay driveSource_;
    AdiShaperAudioTrigger trigger_;
    std::array<float, 3> previousVolume_{};
    double sampleRate_ = 48000.0, bpm_ = 120.0, quarterNotes_ = 0.0;
    double historyQuarterNotes_ = 0.0, lastHistoryStamp_ = 0.0;
    std::uint64_t expectedSample_ = 0, transportRevision_ = 0;
    double splitStep_ = 0.0, steepStep_ = 0.0;
    float masterMix_ = 1.0f;
    bool playing_ = true, historyStampSet_ = false, sampleStampSet_ = false;
};

} // namespace adi::dsp
