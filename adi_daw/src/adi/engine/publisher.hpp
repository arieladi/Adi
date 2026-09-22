// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snapshot handoff. ADR-0010, ADR-0019.
//
// This is the one file in the tree where a mistake is a dropout or a crash in a
// user's session rather than a failing test, so its memory ordering is commented
// line by line and it is deliberately tiny. ADR-0014 chose C++, which means no
// borrow checker is watching this; the compensation is that it is small enough
// to hold in your head and hard to use wrongly by construction.
//
// THE PROTOCOL
//
//   Message thread          publish(s)   -- s becomes current; the old one is retired
//                           collect()    -- frees retired snapshots the reader has passed
//   Audio thread            AudioRead r(p) -- one acquire-load, one release-store
//
// The audio thread does NOT free, allocate, lock, or wait. It publishes which
// snapshot it is using, and the message thread frees the ones it has moved past.
//
// WHY THE FREE CONDITION IS STRICTLY GREATER
//
// A retired snapshot S may be freed only once `inUse_ > S.seq`. Consider the
// race it has to survive:
//
//   1. Audio thread loads current_ -> X (seq 5). It has not yet stored inUse_,
//      which still reads 4.
//   2. Message thread publishes Y (seq 6) and retires X.
//   3. Message thread collects. Is X freeable? inUse_ is 4, and 4 > 5 is false,
//      so X is retained. The audio thread is about to dereference it.
//   4. Audio thread stores inUse_ = 5 and uses X for the whole block.
//   5. Next collect: 5 > 5 is false. X still retained. Correct -- it is in use.
//   6. Audio thread moves to Y, stores inUse_ = 6. Now 6 > 5, and X is freed.
//
// A condition of `>=` would free X at step 5, while the audio thread was inside
// it. The strictness is the whole safety argument, which is why it is spelled
// out here rather than left as an off-by-one someone might "tidy up".
//
// This works because there is exactly ONE reader, it only moves forward, and it
// publishes its position before dereferencing. All three are enforced below:
// AudioRead is the only way to read, and it does both operations in its
// constructor in that order.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace adi::engine {

/// Anything published must carry a monotonically increasing sequence number.
/// The publisher assigns it; nothing else may.
struct Sequenced {
    std::uint64_t seq = 0;
    virtual ~Sequenced() = default;
};

template <typename T>
class SnapshotPublisher {
    static_assert(std::is_base_of_v<Sequenced, T>,
                  "a published snapshot must derive from Sequenced");

public:
    SnapshotPublisher() = default;
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    ~SnapshotPublisher() {
        // Nothing may still be reading when the publisher dies. The owner is
        // responsible for stopping audio first; this just avoids leaking.
        delete current_.load(std::memory_order_relaxed);
        std::lock_guard lock(retiredMutex_);
        for (auto* p : retired_) delete p;
    }

    // --- message thread -------------------------------------------------------

    /// Makes `s` the snapshot the audio thread will pick up on its next block.
    /// Takes ownership. The previous snapshot is retired, not freed.
    void publish(std::unique_ptr<T> s) {
        s->seq = ++nextSeq_;
        T* raw = s.release();

        // Release: everything written into the snapshot before this point must
        // be visible to the audio thread's acquire-load below. Without it the
        // reader can see the pointer before the data it points at.
        T* old = current_.exchange(raw, std::memory_order_release);

        if (old) {
            std::lock_guard lock(retiredMutex_);
            retired_.push_back(old);
        }
    }

    /// Frees every retired snapshot the audio thread has demonstrably moved
    /// past. Call it on a timer; it is never urgent and never blocks audio.
    /// Returns how many were freed.
    std::size_t collect() {
        // Acquire: pairs with the reader's release-store. Anything the reader
        // did before publishing this value happens-before what we do after.
        const std::uint64_t inUse = inUse_.load(std::memory_order_acquire);

        std::vector<T*> freeable;
        {
            std::lock_guard lock(retiredMutex_);
            for (auto it = retired_.begin(); it != retired_.end();) {
                // STRICTLY greater. See the comment at the top of this file --
                // >= frees a snapshot the audio thread is inside.
                if (inUse > (*it)->seq) {
                    freeable.push_back(*it);
                    it = retired_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Freed outside the lock: deallocation can be slow and nothing else
        // needs to wait for it.
        for (auto* p : freeable) delete p;
        return freeable.size();
    }

    [[nodiscard]] std::size_t retainedCount() const {
        std::lock_guard lock(retiredMutex_);
        return retired_.size();
    }
    [[nodiscard]] std::uint64_t publishedSeq() const {
        return nextSeq_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t inUseSeq() const {
        return inUse_.load(std::memory_order_acquire);
    }

    // --- audio thread ----------------------------------------------------------

    /// The ONLY way the audio thread reads a snapshot.
    ///
    /// Construct one per processing block, use it for the whole block, let it
    /// go. It allocates nothing, locks nothing and waits for nothing: two atomic
    /// operations, in an order that is load-bearing.
    ///
    /// Holding one across blocks would pin a snapshot forever and stall
    /// collection, which is why it is a scoped object rather than an accessor.
    class AudioRead {
    public:
        explicit AudioRead(SnapshotPublisher& p) noexcept {
            // 1. Acquire: pairs with publish()'s release-exchange, so the
            //    snapshot's contents are visible, not just its address.
            snap_ = p.current_.load(std::memory_order_acquire);

            // 2. Release, and AFTER the load. Announcing which snapshot we are
            //    on is what permits the message thread to free earlier ones. Do
            //    it before the load and we would be announcing a position we
            //    have not taken.
            if (snap_) p.inUse_.store(snap_->seq, std::memory_order_release);
        }

        [[nodiscard]] const T* get() const noexcept { return snap_; }
        [[nodiscard]] bool valid() const noexcept { return snap_ != nullptr; }
        const T* operator->() const noexcept { return snap_; }

        AudioRead(const AudioRead&) = delete;
        AudioRead& operator=(const AudioRead&) = delete;

    private:
        const T* snap_ = nullptr;
    };

    // --- audio thread, in two steps (ADR-0092) ---------------------------------
    //
    // `AudioRead` loads and announces in one constructor, and that is right for
    // every reader that only ever touches the snapshot it is on. A reader that
    // must read its PREVIOUS snapshot once more before moving on -- to carry
    // state out of it -- needs the two steps apart, and the gap between them is
    // exactly the window in which the previous snapshot is still protected.
    //
    // WHY IT IS SAFE. The previous snapshot has seq == inUse_, because it was
    // the last one announced. `collect()` frees only what is STRICTLY older
    // than inUse_ -- the same strictly-greater rule the top of this file spends
    // a page on. So until `announce()` runs, the previous snapshot cannot be
    // freed, however many times the message thread publishes and collects in
    // between. The snapshot `peek()` returned is safe too: if something newer
    // is published it is retired with a seq above inUse_, and so kept.
    //
    // THE RULE THAT MAKES IT SAFE IS THE CALLER'S: finish with the previous
    // snapshot BEFORE `announce()`. Announcing first is a use-after-free, and
    // nothing here can detect it -- which is why `AudioRead` remains the way to
    // read for anyone who does not need this.

    /// The current snapshot, WITHOUT announcing it. Acquire, pairing with
    /// `publish`'s release, so its contents are visible and not just its address.
    [[nodiscard]] const T* peek() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    /// Announce that the audio thread is now on `s`, which permits everything
    /// older to be freed. Release, pairing with `collect`'s acquire.
    void announce(const T* s) noexcept {
        if (s != nullptr) inUse_.store(s->seq, std::memory_order_release);
    }

private:
    std::atomic<T*> current_{nullptr};
    std::atomic<std::uint64_t> inUse_{0};
    std::atomic<std::uint64_t> nextSeq_{0};

    // Retired snapshots wait here. The mutex is message-thread-only: the audio
    // thread never touches this list, which is the point.
    mutable std::mutex retiredMutex_;
    std::vector<T*> retired_;
};

}  // namespace adi::engine
