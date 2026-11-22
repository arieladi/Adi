// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/roar.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
double settle(double x) noexcept { return std::abs(x) < 1e-30 ? 0 : x; }
double level(double db) noexcept { return std::pow(10.0, db / 20); }
constexpr double kLineSeconds = 1.05;
/// A constant-peak band pass (RBJ): 0 dB at its centre.
Biquad bandPass(double rate, double hz, double q) noexcept {
    const double w = 2 * std::numbers::pi * hz / rate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    return {alpha / a0, 0, -alpha / a0, -2 * std::cos(w) / a0, (1 - alpha) / a0};
}
double runBiquad(const Biquad& b, double& x1, double& x2, double& y1, double& y2, double x) noexcept {
    const double y = b.b0 * x + b.b1 * x1 + b.b2 * x2 - b.a1 * y1 - b.a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = settle(y1);
    y1 = settle(y);
    return y;
}
}  // namespace

Roar::Range Roar::range(Param p) noexcept {
    switch (p) {
    case RoutingParam:
        return {0, 4, 0};
    case Input:
        return {-24, 24, 0};
    case LowSplit:
        return {50, 1000, 200};
    case HighSplit:
        return {500, 12000, 2000};
    case Feedback:
        return {0, 95, 0};
    case FeedbackTime:
        return {1, 1000, 100};
    case FeedbackFreq:
        return {100, 20000, 5000};
    case Output:
        return {-36, 12, 0};
    case DryWet:
        return {0, 100, 100};
    default:
        break;
    }
    const auto index = static_cast<std::size_t>(p - FirstStage);
    const std::size_t s = index / kStageFields;
    switch (static_cast<StageField>(index % kStageFields)) {
    case StageShape:
        return {0, static_cast<double>(Shapes - 1), 0};
    case StageAmount:
        return {0, 100, s == 0 ? 30.0 : 0.0};
    case StageBias:
        return {-100, 100, 0};
    case StageFilter:
        return {0, 3, 0};
    case StageFreq:
        return {20, 20000, 1000};
    case StageQ:
        return {0.5, 10, 0.7071};
    default:
        return {-36, 12, 0};
    }
}

double Roar::shape(int which, double x, double amount) noexcept {
    switch (which) {
    case SoftSine:
        return 2 / std::numbers::pi * std::sin(std::numbers::pi / 2 * std::clamp(x, -1.0, 1.0));
    case Diode:
        return x >= 0 ? std::tanh(x) : std::tanh(2 * x) / 2;
    case Tube:
        return std::tanh(x);
    case Polynomial: {
        const double c = std::clamp(x, -1.0, 1.0);
        return c - c * c * c / 3;
    }
    case BitCrusher: {
        const double step = std::exp2(-(16 - 14 * amount));
        return std::round(x / step) * step;
    }
    default:
        return std::sin(x);
    }
}

double Roar::Chain::run(std::size_t c, double x) noexcept {
    for (std::size_t k = 0; k < 2; ++k) {
        auto& s = state[c][k];
        x = runBiquad(sections[k], s.x1, s.x2, s.y1, s.y2, x);
    }
    return x;
}

Roar::Roar() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = range(static_cast<Param>(i)).initial;
    }
    prepare(48000);
}

void Roar::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    const auto size = static_cast<std::size_t>(std::ceil(kLineSeconds * rate_)) + 4;
    // Memory that cannot be had leaves the feedback off, never a throw out
    // of a host's DSP callback.
    size_ = 0;
    try {
        for (auto& l : line_) {
            l.assign(size, 0.f);
        }
        size_ = size;
    } catch (...) {
        for (auto& l : line_) {
            l.clear();
        }
    }
    write_ = 0;
    feedbackState_ = {};
    dcPole_ = std::exp(-2 * std::numbers::pi * 5 / rate_);
    for (auto& s : stages_) {
        s.state = {};
        s.dcIn = s.dcOut = {};
        s.biasGlide.prepare(rate_);
        s.levelGlide.prepare(rate_);
        s.driveGlide.prepare(rate_, 0.050);   // up to 36 dB: over 50 ms
    }
    for (auto* chain : {&lowLp_, &lowHp_, &highLp_, &highHp_, &allLp_, &allHp_}) {
        chain->state = {};
    }
    for (auto* g : {&input_, &output_, &wet_, &feedback_}) {
        g->prepare(rate_);
    }
    update();
    for (auto& s : stages_) {
        s.drive = s.driveGlide.value;
        s.bias = s.biasGlide.value;
        s.level = s.levelGlide.value;
        s.offset = shape(s.shape, s.bias, s.amount);
    }
}

void Roar::set(Param p, double v) noexcept {
    const auto index = static_cast<std::size_t>(p);
    if (index >= static_cast<std::size_t>(Count) || !std::isfinite(v)) {
        return;
    }
    const Range r = range(p);
    const bool whole = p == RoutingParam || (p >= FirstStage && p < Feedback &&
                                             ((index - FirstStage) % kStageFields == StageShape || (index - FirstStage) % kStageFields == StageFilter));
    const double old = p_[index];
    p_[index] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (p == RoutingParam && p_[index] != old) {
        // A stage (or a channel of one) that sat idle in the last routing must
        // not replay its old state in this one: it starts from silence. One
        // that was running carries on, so the change does not click.
        const auto was = static_cast<int>(old);
        const auto used = [was](std::size_t s, std::size_t c) {
            switch (was) {
            case Single:
                return s == 0;
            case MidSide:
                return s < 2 && c == 0;
            default:
                return true;
            }
        };
        for (std::size_t s = 0; s < kStages; ++s) {
            for (std::size_t c = 0; c < 2; ++c) {
                if (!used(s, c)) {
                    stages_[s].state[c] = {};
                    stages_[s].dcIn[c] = stages_[s].dcOut[c] = 0;
                }
            }
        }
        if (was != Multiband) {
            for (auto* chain : {&lowLp_, &lowHp_, &highLp_, &highHp_, &allLp_, &allHp_}) {
                chain->state = {};
            }
        }
    }
    update();
}

void Roar::update() noexcept {
    const double top = 0.45 * rate_, q = 1 / std::sqrt(2.0);
    for (std::size_t s = 0; s < kStages; ++s) {
        auto& st = stages_[s];
        const auto at = [&](StageField f) { return p_[static_cast<std::size_t>(stage(s, f))]; };
        st.shape = static_cast<int>(at(StageShape));
        st.amount = at(StageAmount) / 100;
        // The bias is removed through the shape it now passes: a change of
        // Shape (or of a crusher's Amount) moves the offset with it, so
        // silence stays silent.
        st.offset = shape(st.shape, st.bias, st.amount);
        st.driveGlide.set(level(36 * st.amount));
        st.biasGlide.set(0.5 * at(StageBias) / 100);
        st.levelGlide.set(level(at(StageLevel)));
        const double f = std::min(at(StageFreq), top), sq = at(StageQ);
        switch (static_cast<int>(at(StageFilter))) {
        case LowPass:
            st.filter = lowPass(rate_, f, sq);
            break;
        case HighPass:
            st.filter = highPass(rate_, f, sq);
            break;
        case BandPass:
            st.filter = bandPass(rate_, f, sq);
            break;
        default:
            st.filter = {1, 0, 0, 0, 0};
            break;
        }
    }
    const double low = std::min(p_[LowSplit], top), high = std::min(std::max(p_[HighSplit], p_[LowSplit]), top);
    lowLp_.sections = {lowPass(rate_, low, q), lowPass(rate_, low, q)};
    lowHp_.sections = {highPass(rate_, low, q), highPass(rate_, low, q)};
    highLp_.sections = {lowPass(rate_, high, q), lowPass(rate_, high, q)};
    highHp_.sections = {highPass(rate_, high, q), highPass(rate_, high, q)};
    allLp_.sections = highLp_.sections;
    allHp_.sections = highHp_.sections;
    feedbackFilter_ = lowPass(rate_, std::min(p_[FeedbackFreq], top), q);
    feedbackDelay_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(p_[FeedbackTime] / 1000 * rate_)));
    feedback_.set(size_ > 0 ? p_[Feedback] / 100 : 0.0);
    input_.set(level(p_[Input]));
    output_.set(level(p_[Output]));
    wet_.set(p_[DryWet] / 100);
}

double Roar::runStage(std::size_t s, std::size_t c, double x) noexcept {
    auto& st = stages_[s];
    const double e = (shape(st.shape, st.drive * x + st.bias, st.amount) - st.offset) / st.drive;
    const double blocked = e - st.dcIn[c] + dcPole_ * st.dcOut[c];
    st.dcIn[c] = e;
    st.dcOut[c] = settle(blocked);
    auto& f = st.state[c];
    return st.level * runBiquad(st.filter, f.x1, f.x2, f.y1, f.y2, blocked);
}

void Roar::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const auto routing = static_cast<int>(p_[RoutingParam]);
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read before either output is written (a host may
        // hand an output plane that is an input plane).
        const double dry[2] = {inL && std::isfinite(inL[i]) ? inL[i] : 0.0, inR && std::isfinite(inR[i]) ? inR[i] : 0.0};
        for (auto& st : stages_) {
            st.drive = st.driveGlide.next();
            const double bias = st.biasGlide.next();
            if (bias != st.bias) {
                st.bias = bias;
                st.offset = shape(st.shape, bias, st.amount);
            }
            st.level = st.levelGlide.next();
        }
        const double input = input_.next(), feedback = feedback_.next(), gainOut = output_.next(), mix = wet_.next();
        double x[2] = {input * dry[0], input * dry[1]};
        if (feedback > 0) {
            for (std::size_t c = 0; c < 2; ++c) {
                const double delayed = line_[c][(write_ + size_ - feedbackDelay_) % size_];
                auto& f = feedbackState_[c];
                x[c] += feedback * runBiquad(feedbackFilter_, f.x1, f.x2, f.y1, f.y2, delayed);
            }
        }
        double y[2] = {0, 0};
        switch (routing) {
        case Serial:
            for (std::size_t c = 0; c < 2; ++c) {
                y[c] = runStage(2, c, runStage(1, c, runStage(0, c, x[c])));
            }
            break;
        case Parallel:
            for (std::size_t c = 0; c < 2; ++c) {
                y[c] = (runStage(0, c, x[c]) + runStage(1, c, x[c]) + runStage(2, c, x[c])) / 3;
            }
            break;
        case MidSide: {
            const double mid = runStage(0, 0, 0.5 * (x[0] + x[1])), side = runStage(1, 0, 0.5 * (x[0] - x[1]));
            y[0] = mid + side;
            y[1] = mid - side;
            break;
        }
        case Multiband:
            for (std::size_t c = 0; c < 2; ++c) {
                const double lowBand = lowLp_.run(c, x[c]), aligned = allLp_.run(c, lowBand) + allHp_.run(c, lowBand);
                const double rest = lowHp_.run(c, x[c]);
                y[c] = runStage(0, c, aligned) + runStage(1, c, highLp_.run(c, rest)) + runStage(2, c, highHp_.run(c, rest));
            }
            break;
        default:
            for (std::size_t c = 0; c < 2; ++c) {
                y[c] = runStage(0, c, x[c]);
            }
            break;
        }
        if (size_ > 0) {
            for (std::size_t c = 0; c < 2; ++c) {
                line_[c][write_] = static_cast<float>(std::clamp(y[c], -16.0, 16.0));
            }
            write_ = (write_ + 1) % size_;
        }
        if (outL) {
            outL[i] = static_cast<float>(dry[0] + (gainOut * y[0] - dry[0]) * mix);
        }
        if (outR) {
            outR[i] = static_cast<float>(dry[1] + (gainOut * y[1] - dry[1]) * mix);
        }
    }
}

}  // namespace adi::dsp
