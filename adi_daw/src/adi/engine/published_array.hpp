// SPDX-License-Identifier: GPL-3.0-or-later
//
// A published array: the audio thread writes it, the frame reads it --
// ADR-0116 d3, ADR-0183 d2.
//
// ADR-0050 d4 named the meter scalar as the only path by which the audio thread
// writes something the UI reads, and it named it there so the list would stay
// short and visible. ADR-0167 d7 added the scope tap, because a waveform is a
// stream rather than a scalar. This is the third and ADR-0183 d5 names it: a
// spectrum is neither -- it is a whole array that is only meaningful complete.
//
// THE ARRAY IS THE UNIT OF ATOMICITY, and that is the whole design. A reader
// that saw half of one spectrum and half of the next would draw a frame that
// never existed in the signal. That is worse than a stale frame, because a
// stale frame is wrong in a direction a person can allow for -- it is simply
// late -- while a spliced one shows a shape the audio never had, and nothing
// on screen says which kind you are looking at.
//
// So: two slots and a sequence counter, which is `scope.hpp`'s discipline
// rather than a new one. The writer picks the slot the reader is not holding,
// fills it, and publishes with release. The reader takes the sequence, copies,
// and takes it again; if it moved, the writer lapped it mid-copy and the read
// is retried. The writer never waits, which is the only hard requirement --
// a reader that misses a frame has a stale picture, a writer that waits is a
// dropout (ADR-0010).
//
// TWO SLOTS, NOT THREE. ADR-0116 d3 says double buffer, and a double buffer is
// enough BECAUSE the reader retries rather than holding a slot across the
// frame: it copies out and is done. A reader that wanted to borrow the memory
// for the length of a paint would need a third slot; this one does not.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace adi::engine {

class PublishedArray {
public:
    PublishedArray() = default;

    /// Message thread, with audio stopped. `length` cells, from the patch's
    /// declaration (`PdArrayDecl::length`, already bounds-checked there).
    void prepare(std::int32_t length) {
        length_ = length > 0 ? length : 0;
        slots_[0].assign(static_cast<std::size_t>(length_), 0.f);
        slots_[1].assign(static_cast<std::size_t>(length_), 0.f);
        seq_.store(0, std::memory_order_release);
    }

    [[nodiscard]] std::int32_t length() const noexcept { return length_; }

    /// AUDIO THREAD. Copies `length()` floats in and publishes them as one.
    /// Never allocates, never waits. `src` shorter than `length()` is a caller
    /// bug and is refused rather than read past.
    void publish(const float* src, std::int32_t count) noexcept {
        if (src == nullptr || count < length_ || length_ == 0) return;
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        // An even sequence means settled. Going odd says "a write is in
        // progress", which is what lets a reader detect a torn copy without
        // the writer ever checking whether anyone is reading.
        seq_.store(s + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);

        // Slot arithmetic, stated once so writer and reader cannot disagree:
        // an EVEN sequence is settled and `seq/2` counts completed
        // publications, so the one being written now is number `(s/2)+1` and
        // lives in slot `((s/2)+1) & 1`. The reader below derives its slot from
        // the same expression on the settled sequence. Getting these two out of
        // step does not fail loudly -- it hands back the previous array, every
        // time, which reads as a display one frame behind forever.
        float* dst = slots_[(((s >> 1) + 1) & 1U)].data();
        for (std::int32_t i = 0; i < length_; ++i) dst[i] = src[i];

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
        published_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Frame thread. Copies the newest settled array into `dst`.
    ///
    /// False when the writer lapped the copy, or when nothing has been
    /// published yet. A caller that gets false may retry; one that does not
    /// simply keeps last frame's picture, which is the right behaviour for a
    /// display and the reason this never blocks.
    [[nodiscard]] bool read(float* dst, std::int32_t count) const noexcept {
        if (dst == nullptr || count < length_ || length_ == 0) return false;
        const std::uint64_t before = seq_.load(std::memory_order_acquire);
        if (before == 0) return false;              // nothing published yet
        if ((before & 1U) != 0U) return false;      // a write is in progress

        const float* src = slots_[((before >> 1) & 1U)].data();
        std::atomic_thread_fence(std::memory_order_acquire);
        for (std::int32_t i = 0; i < length_; ++i) dst[i] = src[i];
        std::atomic_thread_fence(std::memory_order_acquire);

        return seq_.load(std::memory_order_acquire) == before;
    }

    /// How many complete arrays the audio thread has published. A display that
    /// is not moving and a patch that is not writing look identical on screen;
    /// this is what tells them apart.
    [[nodiscard]] std::uint64_t published() const noexcept {
        return published_.load(std::memory_order_relaxed);
    }

private:
    std::int32_t length_ = 0;
    std::vector<float> slots_[2];
    mutable std::atomic<std::uint64_t> seq_{0};
    std::atomic<std::uint64_t> published_{0};
};

}  // namespace adi::engine
