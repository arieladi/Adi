// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace adi::dsp {
// One processor publishes to its own editor. Each bin accumulates extrema at
// the selected contour's phase. Publication swaps ownership of complete frames.
class AdiShaperScope {
  public:
    AdiShaperScope() = default;
    // An offline copy of a processor starts with its editor disconnected.
    AdiShaperScope(const AdiShaperScope &) noexcept {}
    AdiShaperScope &operator=(const AdiShaperScope &) = delete;
    static constexpr std::size_t bins = 512;
    static constexpr int fftSize = 2048;
    struct SpectrumFrame {
        std::array<float, fftSize> mono{};
        double sampleRate{};
        unsigned stamp{};
    };
    struct Frame {
        std::array<float, bins> inputMin{}, inputMax{}, outputMin{}, outputMax{};
        float peakLeft{}, peakRight{}, phase{};
        int selection = -1;
        unsigned stamp{};
    };
    void select(int stage, int band) noexcept { setSelection(stage * 3 + band + 1); }
    void suspend() noexcept { setSelection(0); }
    int selection() const noexcept {
        return static_cast<int>(stamp_.load(std::memory_order_acquire) & 255u) - 1;
    }
    void begin(int selection, const float *l, const float *r, std::size_t frames, std::uint64_t phase,
               std::uint64_t step, double sampleRate = 48000.) noexcept {
        const unsigned stamp = stamp_.load(std::memory_order_acquire);
        const auto drift = std::min(phase - nextPhase_, nextPhase_ - phase);
        // Transport sync is rounded to the nearest double at each callback.
        const bool jump = drift > std::max<std::uint64_t>(step, 0x100000ULL);
        if (selection != current_.selection || stamp != current_.stamp || sampleRate != sampleRate_ || jump) {
            // An idle contour can stay at the same phase while input audio continues.
            if (stamp != current_.stamp || selection != current_.selection || sampleRate != sampleRate_ ||
                phase != start_)
                spectrumFill_ = 0;
            current_ = {};
            current_.selection = selection;
            current_.stamp = stamp;
            lastInput_ = lastOutput_ = bins;
        }
        sampleRate_ = sampleRate;
        captureSpectrum(l, r, frames, stamp);
        capture(l, r, frames, phase, step, current_.inputMin, current_.inputMax, lastInput_);
        start_ = phase;
        step_ = step;
        nextPhase_ = phase + frames * step;
    }
    void end(const float *l, const float *r, std::size_t frames) noexcept {
        capture(l, r, frames, start_, step_, current_.outputMin, current_.outputMax, lastOutput_);
        float pl = 0, pr = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            pl = std::max(pl, std::abs(l[i]));
            pr = std::max(pr, std::abs(r[i]));
        }
        current_.peakLeft = pl;
        current_.peakRight = pr;
        current_.phase = static_cast<float>(nextPhase_ >> 40) * 0x1p-24f;
        frames_[writer_] = current_;
        writer_ = middle_.exchange(writer_ | fresh, std::memory_order_acq_rel) & ~fresh;
    }
    // One message-thread reader. Producer never writes the reader's slot.
    bool read(Frame &output) noexcept {
        if (!(middle_.load(std::memory_order_acquire) & fresh))
            return false;
        reader_ = middle_.exchange(reader_, std::memory_order_acq_rel) & ~fresh;
        output = frames_[reader_];
        const unsigned stamp = stamp_.load(std::memory_order_acquire);
        return output.stamp == stamp && output.selection >= 0 &&
               output.selection + 1 == static_cast<int>(stamp & 255u);
    }
    // The callback only copies contiguous input. The editor owns the FFT.
    bool readSpectrum(SpectrumFrame &output) noexcept {
        if (!(spectrumMiddle_.load(std::memory_order_acquire) & fresh))
            return false;
        spectrumReader_ = spectrumMiddle_.exchange(spectrumReader_, std::memory_order_acq_rel) & ~fresh;
        output = spectra_[spectrumReader_];
        return (output.stamp & 255u) != 0 && output.stamp == stamp_.load(std::memory_order_acquire);
    }

  private:
    // The low byte identifies the curve. Upper bits invalidate packets when an
    // editor closes and reopens at the same selection between audio callbacks.
    void setSelection(int next) noexcept {
        const unsigned previous = stamp_.load(std::memory_order_relaxed);
        if ((previous & 255u) != static_cast<unsigned>(next))
            stamp_.store(((previous & ~255u) + 256u) | static_cast<unsigned>(next), std::memory_order_release);
    }
    void captureSpectrum(const float *l, const float *r, std::size_t frames, unsigned stamp) noexcept {
        while (frames) {
            auto &packet = spectra_[spectrumWriter_];
            const auto count = std::min(frames, packet.mono.size() - spectrumFill_);
            for (std::size_t i = 0; i < count; ++i)
                packet.mono[spectrumFill_ + i] = .5f * l[i] + .5f * r[i];
            l += count;
            r += count;
            frames -= count;
            spectrumFill_ += count;
            if (spectrumFill_ == packet.mono.size()) {
                packet.sampleRate = sampleRate_;
                packet.stamp = stamp;
                spectrumWriter_ =
                    spectrumMiddle_.exchange(spectrumWriter_ | fresh, std::memory_order_acq_rel) & ~fresh;
                spectrumFill_ = 0;
            }
        }
    }
    static void capture(const float *l, const float *r, std::size_t n, std::uint64_t phase, std::uint64_t step,
                        std::array<float, bins> &mins, std::array<float, bins> &maxs,
                        std::size_t &last) noexcept {
        for (std::size_t i = 0; i < n; ++i, phase += step) {
            const std::size_t b = static_cast<std::size_t>(phase >> 55);
            const float y = 0.5f * l[i] + 0.5f * r[i];
            if (b != last) {
                mins[b] = maxs[b] = y;
                last = b;
            } else {
                mins[b] = std::min(mins[b], y);
                maxs[b] = std::max(maxs[b], y);
            }
        }
    }
    static constexpr unsigned fresh = 4;
    std::atomic<unsigned> stamp_{0};
    std::array<SpectrumFrame, 3> spectra_{};
    std::atomic<unsigned> spectrumMiddle_{1};
    unsigned spectrumWriter_ = 0, spectrumReader_ = 2;
    std::size_t spectrumFill_ = 0;
    double sampleRate_{};
    std::array<Frame, 3> frames_{};
    Frame current_{};
    std::atomic<unsigned> middle_{1};
    unsigned writer_ = 0, reader_ = 2;
    std::size_t lastInput_ = bins, lastOutput_ = bins;
    std::uint64_t start_{}, step_{}, nextPhase_{};
};
class AdiShaperScopeSource {
  public:
    virtual ~AdiShaperScopeSource() = default;
    virtual std::uint64_t shaperParameterRevision() const noexcept { return 0; }
    virtual AdiShaperScope *shaperScope() noexcept = 0;
};
} // namespace adi::dsp
