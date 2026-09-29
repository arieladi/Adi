// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <memory>
namespace adi::dsp {
class Shifter {
public:
    enum Param { Mode, Coarse, Fine, Spread, Wide, Window, Delay, DelaySync, DelayRate,
        DelayBeats, Feedback, Tone, LfoShape, Duty, StereoMode, Phase, Spin, Width,
        LfoSync, Offset, LfoRate, LfoBeats, LfoAmount, EnvOn, Attack, Release,
        EnvAmount, DriveOn, Drive, Mix, MidiMode, Glide, BendRange, Count };
    struct Range { double low,high,initial; };
    static constexpr Range ranges[Count]={
        {0, 2, 0},
        {-48, 48, 0},
        {-100, 100, 0},
        {0, 100, 0},
        {0, 1, 0},
        {5, 170, 40},
        {0, 1, 0},
        {0, 1, 0},
        {0.1, 1000, 4},
        {0.0625, 8, 1},
        {0, 95, 0},
        {20, 20000, 10000},
        {0, 9, 0},
        {1, 99, 50},
        {0, 2, 0},
        {0, 360, 0},
        {0, 20, 0},
        {0, 100, 0},
        {0, 1, 0},
        {0, 360, 0},
        {0.01, 40, 1},
        {0.0625, 16, 1},
        {0, 24, 0},
        {0, 1, 0},
        {0.1, 1000, 10},
        {1, 5000, 100},
        {-24, 24, 0},
        {0, 1, 0},
        {0, 36, 0},
        {0, 100, 100},
        {0, 1, 0},
        {0, 2000, 0},
        {0, 24, 2},
    };
    Shifter(); ~Shifter();
    Shifter(const Shifter&)=delete; Shifter& operator=(const Shifter&)=delete;
    void prepare(double rate);
    void set(Param parameter,double value) noexcept;
    void tempo(double bpm) noexcept;
    void note(double midiNote,double velocity) noexcept;
    void bend(double normalized) noexcept;
    int latency() const noexcept;
    void process(const float* l,const float* r,float* ol,float* orr,std::size_t n) noexcept;
private:
    struct State; std::unique_ptr<State> state_;
};
}
