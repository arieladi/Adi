// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/adi_shaper_audio_trigger.hpp"
#include "adi/dsp/adi_shaper_crush.hpp"
#include "adi/dsp/adi_shaper_drive.hpp"
#include "adi/dsp/adi_shaper_filter.hpp"
#include "adi/dsp/adi_shaper_sweep.hpp"
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
    enum Param : int { MasterMix, VolumeBands, WidthBands = VolumeBands + stageParameterCount,
                 PanBands = WidthBands + stageParameterCount,
                 VolumeLowSlope = PanBands + stageParameterCount, WidthLowSlope, PanLowSlope,
                 VolumeHighSlope, WidthHighSlope, PanHighSlope, FilterOn,
                 SweepOn = FilterOn + modStageParameterCount, SweepLowPolarity = SweepOn + modStageParameterCount,
                 SweepMidPolarity, SweepHighPolarity, DriveOn,
                 CrushOn = DriveOn + modHeaderSize + 3 * driveBandParameterCount,
                 BaseCount = CrushOn + modStageParameterCount,
                 TriggerLowMode = 753, TriggerMidMode, TriggerHighMode, TriggerLowEnd, TriggerMidEnd, TriggerHighEnd,
                 TriggerThreshold, TriggerDetail, TriggerProfile, TriggerShift, TriggerSmoothing,
                 PanLowDelayBlend = 855, PanMidDelayBlend, PanHighDelayBlend, Count };
    enum class Stage { Volume, Width, Pan, Filter, Sweep, Drive, Crush };
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
    static constexpr Param modParam(Stage stage, Band band, ModControl control) noexcept {
        const int start = stage == Stage::Crush ? CrushOn : stage == Stage::Sweep ? SweepOn : FilterOn;
        return static_cast<Param>(start + modHeaderSize + modBandParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }
    enum DriveControl { DriveStyle, DriveMix, DriveClock, DriveCycle, DriveRate, DriveCurve,
                        Drive1, Drive2, Drive3, Drive4, Drive5, Drive6, Drive7, Drive8, InputTrim, OutputTrim };
    static constexpr Param driveParam(Band band, DriveControl control) noexcept {
        return static_cast<Param>(DriveOn + modHeaderSize + driveBandParameterCount * static_cast<int>(band)
                                  + static_cast<int>(control));
    }

    AdiShaper() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param id, float value) noexcept;
    void tempo(double bpm) noexcept;
    void sync(double quarterNotes, bool playing = true) noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 std::size_t frames) noexcept;
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
    };
    struct CurveBand {
        Contour primary, secondary;
        std::uint64_t phase = 0, step = 0;
        double beats = 4.0, hz = 1.0;
        float mix = 1.0f, blend = 1.0f;
        int type = 0;
        bool hertz = false, active = false;
        std::array<Gain, 8> heldGains{};
    };
    struct Split {
        struct Integrator { double band = 0, low = 0; };
        using StereoPole = std::array<double, 2>;
        struct Bands { float low, high; };
        std::array<StereoPole, 4> poles{};
        std::array<std::array<Integrator, 3>, 2> pairs{};
        std::array<Integrator, 2> compensation{};
        double coefficient = 0, a1 = 0, a2 = 0, a3 = 0;
        int slope = 0;
        void prepare(double hz, double sampleRate) noexcept;
        void reset() noexcept;
        [[nodiscard]] double pole(double x, double& z) const noexcept;
        void butterworth(double x, Integrator&, double& low, double& high, double& all) const noexcept;
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
        bool active = false, enabled = true;
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
            std::size_t age = 0, channel = 0;
            double fraction = 0.;
        };
        std::vector<std::array<float, 2>> frames;
        std::size_t head = 0, valid = 0;
        float current = 0.f, unity = 0.f;
        Tap constantTap;
        void prepare(double rate) noexcept;
        void setBlend(float percent) noexcept;
        [[nodiscard]] Tap tap(double position, double rate, bool constant) const noexcept;
        void process(const Tap& tap, float& left, float& right) noexcept;
    };
    std::array<PanHistory, 3> panHistory_;
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
    void restart(Stage kind) noexcept;
    void resetKernels(Stage kind) noexcept;
    void processStages(float* outL, float* outR, std::size_t frames) noexcept;
    void setTrigger(Param id, float value) noexcept;
    template <Stage kind, bool triggered = false> void render(StageData& stage, const float* inL, const float* inR,
                                     float* outL, float* outR, std::size_t frames) noexcept;
    std::array<StageData, 7> stages_;
    std::array<AdiShaperFilter, 3> filters_;
    std::array<AdiShaperSweep, 3> sweeps_;
    std::array<AdiShaperDrive, 3> drives_;
    std::array<AdiShaperCrush, 3> crush_;
    std::array<std::array<float, 2>, 3> driveTrims_{{{.5f, .5f}, {.5f, .5f}, {.5f, .5f}}};
    std::array<DriveDryDelay, 3> driveDry_;
    DriveDryDelay driveSource_;
    AdiShaperAudioTrigger trigger_;
    std::array<float, 3> previousVolume_{};
    double sampleRate_ = 48000.0, bpm_ = 120.0, quarterNotes_ = 0.0;
    float masterMix_ = 1.0f;
    bool playing_ = true;
};

} // namespace adi::dsp
