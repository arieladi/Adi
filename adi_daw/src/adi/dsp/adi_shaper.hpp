// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/adi_shaper_crush.hpp"
#include "adi/dsp/adi_shaper_drive.hpp"
#include "adi/dsp/adi_shaper_filter.hpp"
#include "adi/dsp/adi_shaper_liquid.hpp"
#include "adi/dsp/adi_shaper_noise.hpp"
#include "adi/dsp/adi_shaper_pitch.hpp"
#include "adi/dsp/adi_shaper_reverb.hpp"
#include "adi/dsp/adi_shaper_time.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

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
    static constexpr int timeBandParameterCount = 14;
    static constexpr int pitchBandParameterCount = modBandParameterCount - 1;
    static constexpr int noiseBandParameterCount = modBandParameterCount + 5;
    static constexpr int reverbBandParameterCount = pitchBandParameterCount + 6;
    enum Param { MasterMix, VolumeBands, WidthBands = VolumeBands + stageParameterCount,
                 PanBands = WidthBands + stageParameterCount,
                 VolumeLowSlope = PanBands + stageParameterCount, WidthLowSlope, PanLowSlope,
                 VolumeHighSlope, WidthHighSlope, PanHighSlope, FilterOn,
                 LiquidOn = FilterOn + modStageParameterCount, LiquidLowPolarity = LiquidOn + modStageParameterCount,
                 LiquidMidPolarity, LiquidHighPolarity, DriveOn,
                 CrushOn = DriveOn + modHeaderSize + 3 * driveBandParameterCount,
                 TimeOn = CrushOn + modStageParameterCount,
                 PitchOn = TimeOn + modHeaderSize + 3 * timeBandParameterCount,
                 NoiseOn = PitchOn + modHeaderSize + 3 * pitchBandParameterCount,
                 ReverbOn = NoiseOn + modHeaderSize + 3 * noiseBandParameterCount,
                 Count = ReverbOn + modHeaderSize + 3 * reverbBandParameterCount };
    enum class Stage { Volume, Width, Pan, Filter, Liquid, Drive, Crush, Time, Pitch, Noise, Reverb };
    enum class Band { Low, Mid, High };
    enum BandControl { Mix, Clock, Cycle, Rate, Curve, Point1, Point2, Point3, Point4,
                       Point5, Point6, Point7, Point8 };
    static constexpr Param bandParam(Stage stage, Band band, BandControl control) noexcept {
        return static_cast<Param>(VolumeBands + stageHeaderSize + stageParameterCount * static_cast<int>(stage)
                                  + bandParameterCount * static_cast<int>(band) + static_cast<int>(control));
    }
    enum ModControl { Type, ModMix, ModClock, ModCycle, ModRate, PrimaryCurve, SecondaryCurve,
                      Primary1, Primary2, Primary3, Primary4, Primary5, Primary6, Primary7, Primary8,
                      Secondary1, Secondary2, Secondary3, Secondary4, Secondary5, Secondary6, Secondary7, Secondary8 };
    struct ParameterLayout { Param start; int stride; };
    static constexpr std::array<ParameterLayout, 8> effectParameters{{
        {FilterOn, modBandParameterCount}, {LiquidOn, modBandParameterCount},
        {DriveOn, driveBandParameterCount}, {CrushOn, modBandParameterCount}, {TimeOn, timeBandParameterCount},
        {PitchOn, pitchBandParameterCount}, {NoiseOn, noiseBandParameterCount}, {ReverbOn, reverbBandParameterCount}
    }};
    static constexpr Param modParam(Stage stage, Band band, ModControl control) noexcept {
        const auto layout = effectParameters[static_cast<std::size_t>(stage) - static_cast<std::size_t>(Stage::Filter)];
        const int first = stage == Stage::Pitch || stage == Stage::Reverb ? ModMix : Type;
        return static_cast<Param>(layout.start + modHeaderSize + layout.stride * static_cast<int>(band)
                                  + static_cast<int>(control) - first);
    }
    enum NoiseOption { NoiseMono, NoiseOnly, NoiseTrim, NoiseHighPass, NoiseLowPass };
    enum ReverbOption { RoomSize, RoomDecay, RoomGate, RoomWidth, RoomPredelay, RoomClearTails };
    static constexpr Param noiseParam(Band band, NoiseOption option) noexcept {
        return static_cast<Param>(modParam(Stage::Noise, band, Secondary8) + 1 + static_cast<int>(option));
    }
    static constexpr Param reverbParam(Band band, ReverbOption option) noexcept {
        return static_cast<Param>(modParam(Stage::Reverb, band, Secondary8) + 1 + static_cast<int>(option));
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
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 std::size_t frames) noexcept;
    [[nodiscard]] int latency() const noexcept {
        return stages_[static_cast<std::size_t>(Stage::Drive)].enabled ? AdiShaperDrive::latencySamples : 0;
    }

  private:
    struct Gain { float left = 1.0f, right = 1.0f; };
    struct Contour {
        std::array<float, 8> points{}, differences{};
        bool linear = false, constant = true;
        void update() noexcept;
        [[nodiscard]] float at(std::uint64_t position) const noexcept;
    };
    struct CurveBand {
        Contour primary, secondary;
        std::uint64_t phase = 0, step = 0;
        double beats = 4.0, hz = 1.0;
        float mix = 1.0f, blend = 1.0f, returnDry = 0.0f, returnWet = 1.0f;
        int type = 0;
        bool hertz = false, active = false;
        std::array<Gain, 8> heldGains{};
    };
    struct Split {
        struct Integrator { double band = 0, low = 0; };
        struct State {
            std::array<double, 4> poles{};
            std::array<Integrator, 3> pairs{};
        };
        struct Bands { float low, high; };
        std::array<State, 2> channels{};
        std::array<Integrator, 2> compensation{};
        double coefficient = 0, a1 = 0, a2 = 0, a3 = 0;
        int slope = 0;
        void prepare(double hz, double sampleRate) noexcept;
        void reset() noexcept;
        [[nodiscard]] double pole(double x, double& z) const noexcept;
        void butterworth(double x, Integrator&, double& low, double& high, double& all) const noexcept;
        [[nodiscard]] Bands split(float input, std::size_t channel) noexcept;
        [[nodiscard]] float align(float input, std::size_t channel) noexcept;
        void quiet() noexcept;
    };
    struct StageData {
        std::array<CurveBand, 3> bands;
        Split low, high;
        double lowHz = 200.0, highHz = 2000.0;
        int layout = 0;
        bool active = false, enabled = true;
    };
    struct DriveDryDelay {
        std::array<std::array<float, 2>, AdiShaperDrive::latencySamples> samples{};
        std::size_t index = 0;
        void advance(float& l, float& r) noexcept;
    };
    struct ReverbSettings { float size = .5f, decay = 1.0f, predelay = 0.0f; bool gate = false; };
    static Gain gain(Stage kind, float amount, float mix) noexcept;
    void updateBand(Stage kind, CurveBand& band) const noexcept;
    void updateActivity(StageData& stage) noexcept;
    void updateStage(Stage kind) noexcept;
    void updateStep(CurveBand& band) const noexcept;
    void updateSplits(StageData& stage) const noexcept;
    void layout(StageData& stage, int value) noexcept;
    void slope(Split& split, float value) noexcept;
    void clock(CurveBand& band, BandControl control, float value) const noexcept;
    void setMod(Param id, float value) noexcept;
    void setNoiseOption(std::size_t band, NoiseOption option, float value) noexcept;
    void setReverbOption(std::size_t band, ReverbOption option, float value) noexcept;
    void prepareStages(double sampleRate) noexcept;
    void syncClock(double quarterNotes, bool playing) noexcept;
    void updateTimeRanges() noexcept;
    void restart(Stage kind) noexcept;
    void resetKernels(Stage kind) noexcept;
    template <Stage kind> void render(StageData& stage, const float* inL, const float* inR,
                                     float* outL, float* outR, std::size_t frames) noexcept;
    std::array<StageData, 11> stages_;
    std::array<AdiShaperFilter, 3> filters_;
    std::array<AdiShaperLiquid, 3> liquids_;
    std::array<AdiShaperDrive, 3> drives_;
    std::array<AdiShaperCrush, 3> crush_;
    std::array<AdiShaperTime, 3> times_;
    std::array<AdiShaperPitch, 3> pitch_;
    std::array<AdiShaperNoise, 3> noise_;
    std::array<AdiShaperReverb, 3> reverb_;
    std::array<std::array<float, 2>, 3> noiseFilters_{{{.254f, 1}, {.254f, 1}, {.254f, 1}}};
    std::array<ReverbSettings, 3> reverbSettings_;
    std::array<double, 3> timeSpans_{};
    std::array<std::array<float, 2>, 3> driveTrims_{{{.5f, .5f}, {.5f, .5f}, {.5f, .5f}}};
    std::array<DriveDryDelay, 3> driveDry_;
    DriveDryDelay driveSource_;
    double sampleRate_ = 48000.0, bpm_ = 120.0, quarterNotes_ = 0.0;
    double historyQuarterNotes_ = 0.0, lastHistoryStamp_ = 0.0;
    std::uint64_t expectedSample_ = 0, transportRevision_ = 0;
    float masterMix_ = 1.0f;
    bool playing_ = true, historyStampSet_ = false, sampleStampSet_ = false;
};

} // namespace adi::dsp
