// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
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
    enum Param { MasterMix, VolumeBands, WidthBands = VolumeBands + stageParameterCount,
                 PanBands = WidthBands + stageParameterCount,
                 VolumeLowSlope = PanBands + stageParameterCount, WidthLowSlope, PanLowSlope,
                 VolumeHighSlope, WidthHighSlope, PanHighSlope, Count };
    enum class Stage { Volume, Width, Pan };
    enum class Band { Low, Mid, High };
    enum BandControl { Mix, Clock, Cycle, Rate, Curve, Point1, Point2, Point3, Point4,
                       Point5, Point6, Point7, Point8 };
    static constexpr Param bandParam(Stage stage, Band band, BandControl control) noexcept {
        return static_cast<Param>(VolumeBands + stageHeaderSize + stageParameterCount * static_cast<int>(stage)
                                  + bandParameterCount * static_cast<int>(band) + static_cast<int>(control));
    }

    AdiShaper() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param id, float value) noexcept;
    void tempo(double bpm) noexcept;
    void sync(double quarterNotes, bool playing = true) noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR,
                 std::size_t frames) noexcept;
    [[nodiscard]] constexpr int latency() const noexcept { return 0; }

  private:
    struct Gain { float left = 1.0f, right = 1.0f; };
    struct CurveBand {
        std::array<float, 8> points{}, differences{};
        std::uint64_t phase = 0, step = 0;
        double beats = 4.0, hz = 1.0;
        float mix = 1.0f, blend = 1.0f;
        bool hertz = false, linear = false, constant = true, active = false;
        std::array<Gain, 8> heldGains{};
        [[nodiscard]] float valueAt(std::size_t sample) const noexcept;
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
        bool active = false;
    };
    static Gain gain(Stage kind, float amount, float mix) noexcept;
    void updateBand(Stage kind, CurveBand& band) const noexcept;
    void updateActivity(StageData& stage) noexcept;
    void updateStage(Stage kind) noexcept;
    void updateStep(CurveBand& band) const noexcept;
    void updateSplits(StageData& stage) const noexcept;
    template <Stage kind> void render(StageData& stage, const float* inL, const float* inR,
                                     float* outL, float* outR, std::size_t frames) noexcept;
    std::array<StageData, 3> stages_;
    double sampleRate_ = 48000.0, bpm_ = 120.0, quarterNotes_ = 0.0;
    float masterMix_ = 1.0f;
    bool playing_ = true;
};

} // namespace adi::dsp
