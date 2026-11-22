// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_limiter.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
double fromDb(double db) noexcept { return std::pow(10.0, db / 20.0); }
/// The first of a link's brickwalls: [0] linked; [1], [2] left and right; [3], [4] mid and side.
std::size_t firstOf(int link) noexcept {
    return link == LiveLimiter::Stereo ? 0 : link == LiveLimiter::LeftRight ? 1 : 3;
}
}  // namespace

LiveLimiter::LiveLimiter() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

void LiveLimiter::prepare(double sampleRate) {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    // Room for the longest lookahead: 6 ms at the prepared rate.
    const int most = static_cast<int>(std::lround(kLookaheadMs[2] * rate_ / 1000.0));
    limiter_[0].prepare(rate_, 2, most);
    for (std::size_t k = 1; k < limiter_.size(); ++k) {
        limiter_[k].prepare(rate_, 1, most);
    }
    update();
    restart();
}

void LiveLimiter::restart() noexcept {
    // Every brickwall starts from silence: the link's path alone, nothing to
    // fade or clamp.
    weight_ = {0.0, 0.0, 0.0};
    weight_[static_cast<std::size_t>(link_)] = 1.0;
    filling_ = {0, 0, 0};
    clampLeft_ = 0;
    clampScale_ = 1.0;
}

void LiveLimiter::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    p_[static_cast<std::size_t>(index)] = std::clamp(v, r.low, r.high);
    update();
}

void LiveLimiter::update() noexcept {
    gain_ = fromDb(p_[Gain]);
    const double ceiling = fromDb(p_[Ceiling]);
    const auto choice = static_cast<std::size_t>(std::clamp<long>(std::lround(p_[Lookahead]), 0, 2));
    const int lookahead = static_cast<int>(std::lround(kLookaheadMs[choice] * rate_ / 1000.0));
    const int link = static_cast<int>(std::clamp<long>(std::lround(p_[Link]), 0, 2));
    kind_ = static_cast<int>(std::clamp<long>(std::lround(p_[Mode]), 0, 2));
    bool restarted = false;
    for (auto& l : limiter_) {
        // The brickwall resets when its lookahead or detector changes (a
        // latency change); unchanged, the calls leave it running.
        if (l.truePeak() != (kind_ == TruePeak)) {
            l.setTruePeak(kind_ == TruePeak);
            restarted = true;
        }
        if (l.latencySamples() - (l.truePeak() ? l.truePeakDelay() : 0) != lookahead) {
            l.setLookahead(lookahead);
            restarted = true;
        }
        l.setCeiling(ceiling);
        l.setReleaseSeconds(p_[Release] / 1000.0);
    }
    const double before = ceiling_;
    ceiling_ = ceiling;
    const int was = link_;
    link_ = link;
    if (restarted) {
        // A latency change: every path restarts from silence together.
        restart();
        return;
    }
    if (ceiling < before) {
        // The samples already in the delay were limited to the old ceiling.
        // For one latency they are clamped to the new one, or, in True Peak
        // mode, scaled by new over old, so they stay under it between the
        // samples too (a clamp's corners would not).
        clampLeft_ = static_cast<std::size_t>(latency());
        clampScale_ *= ceiling / before;
    }
    const auto l = static_cast<std::size_t>(link);
    if (link != was && weight_[l] == 0.0) {
        // A path the output is not on starts from silence; it takes its share
        // once its delay holds the programme. A path still sounding (a change
        // back mid-fade) is warm and takes its share at once.
        const std::size_t first = firstOf(link);
        limiter_[first].setLookahead(lookahead);
        if (link != Stereo) {
            limiter_[first + 1].setLookahead(lookahead);
        }
        filling_[l] = static_cast<std::size_t>(latency());
    }
}

int LiveLimiter::latency() const noexcept {
    // All five run at the same settings; the linked one stands for them.
    return limiter_[0].latencySamples();
}

void LiveLimiter::runPath(int link, const Block& x, Block& y, std::size_t n, std::size_t clamp) noexcept {
    Block a;
    for (std::size_t i = 0; i < n; ++i) {
        double l = x[0][i], r = x[1][i];
        if (link == MidSide) {
            const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
            l = mid;
            r = side;
        }
        a[0][i] = l;
        a[1][i] = r;
    }
    const double* in2[2] = {a[0].data(), a[1].data()};
    double* out2[2] = {y[0].data(), y[1].data()};
    const std::size_t first = firstOf(link);
    if (link == Stereo) {
        limiter_[first].process(in2, out2, static_cast<int>(n));
    } else {
        limiter_[first].process(&in2[0], &out2[0], static_cast<int>(n));
        limiter_[first + 1].process(&in2[1], &out2[1], static_cast<int>(n));
    }
    for (std::size_t i = 0; i < n; ++i) {
        double l = y[0][i], r = y[1][i];
        if (i < clamp) {
            if (kind_ == TruePeak) {
                l *= clampScale_;
                r *= clampScale_;
            } else {
                l = std::clamp(l, -ceiling_, ceiling_);
                r = std::clamp(r, -ceiling_, ceiling_);
            }
        }
        if (link == MidSide) {
            const double mid = l, side = r;
            l = mid + side;
            r = mid - side;
        }
        y[0][i] = l;
        y[1][i] = r;
    }
}

void LiveLimiter::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    // In blocks of kBlock: screened and gained into doubles, limited on every
    // path that sounds (the link's, and any still fading out), mixed by their
    // weights, softened (Soft Clip), out.
    Block x;
    std::array<Block, 3> y;
    const auto current = static_cast<std::size_t>(link_);
    const double step = 1.0 / static_cast<double>(std::max(latency(), 64));
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(kBlock, frames - done);
        for (std::size_t i = 0; i < n; ++i) {
            // Each channel screened on its own, before the M/S encode, so a
            // non-finite sample on one channel leaves the other as it is.
            const double l = inL ? static_cast<double>(inL[done + i]) : 0.0;
            const double r = inR ? static_cast<double>(inR[done + i]) : 0.0;
            x[0][i] = std::isfinite(l) ? l * gain_ : 0.0;
            x[1][i] = std::isfinite(r) ? r * gain_ : 0.0;
        }
        const std::size_t clamp = std::min(n, clampLeft_);
        std::array<bool, 3> sounding{};
        for (std::size_t p = 0; p < 3; ++p) {
            sounding[p] = p == current || weight_[p] > 0.0;
            if (sounding[p]) {
                runPath(static_cast<int>(p), x, y[p], n, clamp);
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            // The link's path waits a latency while its delay fills, then
            // rises linearly over at least another; the others give way in
            // proportion, so the weights always sum to one and every path
            // keeps the ceiling, so the mix does too. A change mid-fade mixes
            // from wherever the output is: no step.
            if (filling_[current] > 0) {
                --filling_[current];
            } else if (weight_[current] < 1.0) {
                const double now = weight_[current], next = std::min(1.0, now + step);
                const double keep = (1.0 - next) / (1.0 - now);
                for (std::size_t p = 0; p < 3; ++p) {
                    weight_[p] = p == current ? next : next >= 1.0 ? 0.0 : weight_[p] * keep;
                }
            }
            double l = 0.0, r = 0.0;
            for (std::size_t p = 0; p < 3; ++p) {
                if (sounding[p] && weight_[p] > 0.0) {
                    l += weight_[p] * y[p][0][i];
                    r += weight_[p] * y[p][1][i];
                }
            }
            if (kind_ == SoftClip) {
                // tanh inside the ceiling: unity slope near 0, so a quiet
                // signal passes as itself, rounding off toward the ceiling and
                // never reaching it.
                l = ceiling_ * std::tanh(l / ceiling_);
                r = ceiling_ * std::tanh(r / ceiling_);
            }
            if (outL) {
                outL[done + i] = static_cast<float>(l);
            }
            if (outR) {
                outR[done + i] = static_cast<float>(r);
            }
        }
        clampLeft_ -= clamp;
        if (clampLeft_ == 0) {
            clampScale_ = 1.0;
        }
        done += n;
    }
}

}  // namespace adi::dsp
