// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace adi::dsp {
class LiveDelay {
public:
    enum Param {LeftMs,RightMs,LeftSync,RightSync,LeftNotes,RightNotes,LeftOffset,RightOffset,Link,Feedback,Freeze,Filter,Freq,Width,LfoDelay,LfoFilter,RateMode,RateHz,RateMs,RateBeats,Wave,Morph,Transition,PingPong,Mix,HiQuality,EqualLoudness,Count};
    struct Range {double low,high,initial;};
    static constexpr Range ranges[Count]={{1,5000,250},{1,5000,250},{0,1,0},{0,1,0},{1,16,4},{1,16,4},{-33,33,0},{-33,33,0},{0,1,1},{0,95,35},{0,1,0},{0,1,0},{20,20000,2000},{0.5,9,4},{0,100,0},{0,100,0},{0,5,0},{0.01,40,1},{25,100000,1000},{0.0625,16,1},{0,6,0},{0,100,50},{0,2,0},{0,1,0},{0,100,50},{0,1,0},{0,1,0}};
    LiveDelay();
    void prepare(double sampleRate);
    void set(Param,double) noexcept;
    void tempo(double bpm) noexcept;
    void process(const float*,const float*,float*,float*,std::size_t) noexcept;
private:
    struct Lane { std::vector<float> ring; double current=0,old=0,target=0,fade=1,low=0,high=0; };
    std::array<Lane,2> lanes_;
    std::array<double,Count> p_{};
    double rate_=48000,bpm_=120,phase_=0,from_=0,to_=0;
    std::uint32_t random_=0x12345678u;
    std::size_t write_=0;
    bool started_=false;
    double read(const Lane&,double) const noexcept;
};
}
