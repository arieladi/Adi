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
    // Every brickwall starts from silence: nothing to fade from or clamp.
    from_ = -1;
    clampLeft_ = 0;
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
    if (restarted) {
        // A latency change: every path restarts from silence together.
        from_ = -1;
        clampLeft_ = 0;
    } else {
        if (ceiling < ceiling_) {
            // The samples already in the delay were limited to the old
            // ceiling; for one latency they are clamped to the new one.
            clampLeft_ = static_cast<std::size_t>(latency());
        }
        if (link != link_) {
            const std::size_t hold = static_cast<std::size_t>(latency()), fade = fadeLength();
            if (from_ >= 0 && link == from_) {
                // Back to the path still fading out: it never stopped running.
                if (faded_ < hold) {
                    from_ = -1;  // the output never left it
                } else {
                    // The two swap roles, the mix where it was.
                    const double w = weight(faded_);
                    from_ = link_;
                    faded_ = hold - 1 + static_cast<std::size_t>(std::lround(static_cast<double>(fade) * (1.0 - w)));
                }
            } else {
                // The new path starts from silence and is faded in from the
                // one running, once its delay holds the programme. Mid-fade,
                // that is the old path while the output is still on it, else
                // the path that was coming in.
                if (from_ < 0 || faded_ >= hold) {
                    from_ = link_;
                }
                const std::size_t first = firstOf(link);
                limiter_[first].setLookahead(lookahead);
                if (link != Stereo) {
                    limiter_[first + 1].setLookahead(lookahead);
                }
                faded_ = 0;
            }
        }
    }
    ceiling_ = ceiling;
    link_ = link;
}

int LiveLimiter::latency() const noexcept {
    // All five run at the same settings; the linked one stands for them.
    return limiter_[0].latencySamples();
}

std::size_t LiveLimiter::fadeLength() const noexcept {
    return std::max<std::size_t>(static_cast<std::size_t>(latency()), 64);
}

double LiveLimiter::weight(std::size_t k) const noexcept {
    // Held on the old path for a latency (the new one's delay still filling),
    // then a linear crossfade over at least another.
    const auto hold = static_cast<std::size_t>(latency());
    return k < hold ? 0.0 : std::min(1.0, static_cast<double>(k - hold + 1) / static_cast<double>(fadeLength()));
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
            l = std::clamp(l, -ceiling_, ceiling_);
            r = std::clamp(r, -ceiling_, ceiling_);
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
    // In blocks of kBlock: screened and gained into doubles, limited on the
    // link's path (and the one fading out), softened (Soft Clip), out.
    Block x, now, before;
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
        runPath(link_, x, now, n, clamp);
        if (from_ >= 0) {
            // Each path keeps its own ceiling, so their mix does too.
            runPath(from_, x, before, n, clamp);
            for (std::size_t i = 0; i < n; ++i) {
                const double w = weight(faded_ + i);
                for (std::size_t c = 0; c < 2; ++c) {
                    now[c][i] = before[c][i] + w * (now[c][i] - before[c][i]);
                }
            }
            faded_ += n;
            if (faded_ >= static_cast<std::size_t>(latency()) + fadeLength()) {
                from_ = -1;
            }
        }
        clampLeft_ -= clamp;
        for (std::size_t i = 0; i < n; ++i) {
            double l = now[0][i], r = now[1][i];
            if (kind_ == SoftClip) {
                // tanh inside the ceiling: unity slope near 0, so a quiet
                // signal passes as itself, rounding off toward the ceiling and
                // never reaching it.
                const auto soft = [this](double v) { return ceiling_ * std::tanh(v / ceiling_); };
                l = soft(l);
                r = soft(r);
            }
            if (outL) {
                outL[done + i] = static_cast<float>(l);
            }
            if (outR) {
                outR[done + i] = static_cast<float>(r);
            }
        }
        done += n;
    }
}

}  // namespace adi::dsp
