// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/engine/graph.hpp"
#include "adi/engine/scope.hpp"
#include "adi/engine/render_workers.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <type_traits>

namespace adi::engine {

Graph::Graph() = default;
Graph::~Graph() {workers_.reset();}
bool Graph::setParallelRendering(std::uint32_t count,std::int32_t minimumFrames,std::uint32_t minimumNodes,RenderWorkers::Initializer initialize,void* context) noexcept {
    if(count>RenderWorkers::maximum || minimumFrames<1 || minimumNodes<2)return false;
    requestedWorkers_=count;parallelMinimumFrames_=minimumFrames;parallelMinimumNodes_=minimumNodes;
    workerInitializer_=initialize;workerInitializationContext_=context;prepared_=false;return true;
}
std::uint32_t Graph::renderWorkerCount() const noexcept{return static_cast<std::uint32_t>(workerRenders_.size());}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NodeId Graph::addNode(Node& node) {
    slots_.push_back(Slot{});
    slots_.back().node = &node;
    prepared_ = false;
    return static_cast<NodeId>(slots_.size() - 1);
}

bool Graph::connect(NodeId from, NodeId to, Bus bus) {
    const auto n = static_cast<NodeId>(slots_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    Slot& dst = slots_[static_cast<std::size_t>(to)];
    if (bus == Bus::Sidechain) dst.sidechains.push_back(from);
    else                       dst.inputs.push_back(from);
    prepared_ = false;
    return true;
}

bool Graph::pushInputEvent(NodeId to, const Event& e) {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return false;
    return slots_[static_cast<std::size_t>(to)].events.push(e);
}

std::int32_t Graph::deriveEventCapacity(double sampleRate, std::int32_t maxFrames,
                                        std::int32_t polyphony) noexcept {
    // ADR-0054's instrument at ADR-0049's largest block: 500 Hz update frames
    // x three dimensions x polyphony, plus a note-on and note-off each, plus a
    // floor so a 64-frame block still holds a chord arriving at once.
    if (sampleRate <= 0.0 || maxFrames <= 0) return 1024;
    const double frames = 500.0 * (static_cast<double>(maxFrames) / sampleRate);
    const double total = frames * 3.0 * static_cast<double>(polyphony)
                       + 2.0 * static_cast<double>(polyphony);
    const auto v = static_cast<std::int32_t>(total) + 64;
    return v < 256 ? 256 : v;
}

std::int32_t Graph::maxFloorFor(double sampleRate) noexcept {
    // ADR-0054. A Continuum emits update frames at 500 Hz; a floor wider than
    // the gap between them puts two frames in one segment and the later one is
    // discarded, which is quantising the stream in TIME rather than in bit
    // depth -- the same mandate broken through a different door.
    if (sampleRate <= 0.0) return 64;
    const auto v = static_cast<std::int32_t>(sampleRate / 500.0);
    return v < 1 ? 1 : v;
}

// ---------------------------------------------------------------------------
// Topological order
// ---------------------------------------------------------------------------

void Graph::permuteLevelsForTest(std::uint32_t seed) {
    std::mt19937 random(seed);
    for (auto& level : levels_) {
        std::sort(level.begin(), level.end());
        // Explicit Fisher-Yates: unlike std::shuffle, this mapping from a
        // seed to a permutation is the same across standard libraries.
        for (std::size_t count = level.size(); count > 1; --count) {
            const auto choice = static_cast<std::size_t>(random()) % count;
            std::swap(level[count - 1], level[choice]);
        }
    }
}

bool Graph::topoSort() {
    const auto n = static_cast<std::int32_t>(slots_.size());
    order_.clear();
    order_.reserve(static_cast<std::size_t>(n));

    std::vector<std::int32_t> indegree(static_cast<std::size_t>(n), 0);
    for (std::int32_t i = 0; i < n; ++i) {
        const Slot& sl = slots_[static_cast<std::size_t>(i)];
        indegree[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(sl.inputs.size() + sl.sidechains.size());
    }

    // Kahn's algorithm. Ready nodes are taken in id order rather than from a
    // stack, so the order is deterministic for a given graph -- two runs that
    // process the same nodes in a different order would make a floating-point
    // sum differ, and ADR-0021's oracle compares bytes.
    std::vector<NodeId> ready;
    for (std::int32_t i = 0; i < n; ++i)
        if (indegree[static_cast<std::size_t>(i)] == 0) ready.push_back(i);

    while (!ready.empty()) {
        const NodeId id = *std::min_element(ready.begin(), ready.end());
        ready.erase(std::find(ready.begin(), ready.end(), id));
        order_.push_back(id);

        for (std::int32_t j = 0; j < n; ++j) {
            Slot& sj = slots_[static_cast<std::size_t>(j)];
            std::int32_t edges = 0;
            for (NodeId in : sj.inputs)     if (in == id) ++edges;
            for (NodeId in : sj.sidechains) if (in == id) ++edges;
            if (edges == 0) continue;
            indegree[static_cast<std::size_t>(j)] -= edges;
            if (indegree[static_cast<std::size_t>(j)] == 0) ready.push_back(j);
        }
    }

    // Dependency depth. Every node in one level depends only on levels below
    // it, so a level may run in any order -- including concurrently -- and the
    // output is unchanged, because each node writes its own buffer and every
    // sum happens in its consumer's fixed input order (ADR-0056).
    levels_.clear();
    if (static_cast<std::int32_t>(order_.size()) == n) {
        for (NodeId id : order_) {
            Slot& sl = slots_[static_cast<std::size_t>(id)];
            std::int32_t depth = 0;
            for (NodeId in : sl.inputs)
                depth = (std::max)(depth, slots_[static_cast<std::size_t>(in)].level + 1);
            for (NodeId in : sl.sidechains)
                depth = (std::max)(depth, slots_[static_cast<std::size_t>(in)].level + 1);
            sl.level = depth;
            if (static_cast<std::int32_t>(levels_.size()) <= depth)
                levels_.resize(static_cast<std::size_t>(depth) + 1);
            levels_[static_cast<std::size_t>(depth)].push_back(id);
        }
    }

    if (static_cast<std::int32_t>(order_.size()) != n) {
        // A feedback loop. Refused rather than broken by inserting a delay:
        // a graph that silently became something other than what was asked for
        // is worse than one that says it cannot run.
        error_ = "the graph has a cycle; " + std::to_string(order_.size()) +
                 " of " + std::to_string(n) + " nodes are reachable in order";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Delay, and the compensation that uses it
// ---------------------------------------------------------------------------

void DelayLine::prepare(std::int32_t channels, std::int32_t capacity, bool wide) {
    channels_ = channels > 0 ? channels : 0;
    capacity_ = capacity > 0 ? capacity : 0;
    // ONE MORE THAN THE CAPACITY. The write happens before the read, so a tap
    // at distance `capacity_` must land on a slot the write has not just
    // overwritten. With a ring of exactly `capacity_` it lands on the write
    // index itself and the longest delay silently becomes no delay at all.
    ring_ = capacity_ > 0 ? capacity_ + 1 : 0;
    wide_ = wide;
    const std::size_t n = static_cast<std::size_t>(channels_) * static_cast<std::size_t>(ring_);
    // One storage, the other emptied: a line's bytes are what it holds.
    if (wide_) {
        wbuf_.assign(n, 0.0);
        std::vector<float>().swap(buf_);
    } else {
        buf_.assign(n, 0.0f);
        std::vector<double>().swap(wbuf_);
    }
    delay_ = 0;
    write_ = 0;
    incoming_.clear();
    wincoming_.clear();
    inCapacity_ = inRing_ = inWrite_ = targetDelay_ = primeRemaining_ = 0;
    limit_ = capacity_;
    filled_ = capacity_;   // the silence before the stream is real history
    quiet_ = capacity_;    // and it is silence
    inFilled_ = ran_ = 0;
    target_.store(0, std::memory_order_relaxed);
    gliding_.store(false, std::memory_order_relaxed);
    grow_.store(Grow::Idle, std::memory_order_relaxed);
}

void DelayLine::setDelay(std::int32_t d) noexcept {
    if (d < 0) d = 0;
    if (d > capacity_) d = capacity_;
    delay_ = d;
    target_.store(d, std::memory_order_relaxed);
    gliding_.store(false, std::memory_order_relaxed);
}

void DelayLine::reset() noexcept {
    for (auto& v : buf_) v = 0.0f;
    for (auto& v : wbuf_) v = 0.0;
    write_ = 0;
    filled_ = capacity_;
    quiet_ = capacity_;
}

bool DelayLine::beginGlide(std::int32_t d) noexcept {
    // `limit_` and the two atomics, nothing else: `capacity_` and `delay_` are
    // the audio thread's to move (a swap, the end of a glide) while this runs.
    if (d < 0 || d > limit_) return false;
    // Not gliding, the delay in effect is the target, or is about to be: a
    // glide's end, or a swap, is already taking it there.
    if (!gliding_.load(std::memory_order_acquire) &&
        d == target_.load(std::memory_order_relaxed))
        return true;                            // nothing to do, and not a failure
    target_.store(d, std::memory_order_relaxed);
    gliding_.store(true, std::memory_order_release);
    return true;
}

void DelayLine::settle() noexcept {
    // Down FIRST, then the target read again, whether or not the flag was up.
    // A glide the message thread raised after this edge's snapshot either
    // lands before the exchange, which then reads its release and so sees
    // its target here, or after it, and stays up on its own. And a target
    // left behind with the flag down -- a glide back to the old delay asked
    // for, and ended on the old ring, while a grow was being offered -- is
    // seen at the swap, which has acquired the offer. None is lost.
    gliding_.exchange(false, std::memory_order_acq_rel);
    if (target_.load(std::memory_order_relaxed) != delay_)
        gliding_.store(true, std::memory_order_release);
}

template <typename T>
bool DelayLine::offer(std::vector<T>& incoming, std::vector<T> ring, std::int32_t capacity,
                      std::int32_t targetDelay) {
    if (grow_.load(std::memory_order_acquire) != Grow::Idle) return false;
    // A line with no ring is never run (`process` passes it through), so a
    // grow offered to it would prime forever. Refused: the caller's rebuild
    // is what gives it a ring.
    if (ring_ <= 0) return false;
    if (capacity <= 0 || targetDelay < 0 || targetDelay > capacity) return false;
    const std::int32_t r = capacity + 1;
    if (ring.size() < static_cast<std::size_t>(channels_) * static_cast<std::size_t>(r))
        return false;

    incoming = std::move(ring);
    for (auto& v : incoming) v = T{};   // message thread: the audio thread must not
    inCapacity_ = capacity;
    inRing_ = r;
    inWrite_ = 0;
    targetDelay_ = targetDelay;
    // The new ring is valid for a tap at `targetDelay` only once it holds that
    // many real samples. Until then it holds zeros, and reading it would be a
    // fade to silence rather than a change of alignment.
    primeRemaining_ = targetDelay;
    inFilled_ = 0;
    // From here a glide is asked for against the ring that is coming, and
    // the delay asked for is this one: a glide still pending from before the
    // offer is superseded, so the swap does not glide back to it.
    limit_ = capacity;
    target_.store(targetDelay, std::memory_order_relaxed);

    // RELEASE, and last: everything above must be visible to the audio thread
    // before it can observe the state that tells it to look.
    grow_.store(Grow::Priming, std::memory_order_release);
    return true;
}

bool DelayLine::offerRing(std::vector<float> ring, std::int32_t capacity,
                          std::int32_t targetDelay) {
    if (wide_) return false;
    return offer(incoming_, std::move(ring), capacity, targetDelay);
}

bool DelayLine::offerRing(std::vector<double> ring, std::int32_t capacity,
                          std::int32_t targetDelay) {
    if (!wide_) return false;
    return offer(wincoming_, std::move(ring), capacity, targetDelay);
}

std::vector<float> DelayLine::collectRing() {
    if (grow_.load(std::memory_order_acquire) != Grow::Spent) return {};
    std::vector<float> old = std::move(incoming_);
    incoming_.clear();
    std::vector<double>().swap(wincoming_);   // freed here, on the message thread
    inCapacity_ = inRing_ = inWrite_ = 0;
    grow_.store(Grow::Idle, std::memory_order_release);
    return old;
}

bool DelayLine::reclaim() {
    if (grow_.load(std::memory_order_acquire) != Grow::Spent) return false;
    // The vector returned is destroyed here, on the message thread: the
    // deallocation the audio thread deliberately did not do.
    (void)collectRing();
    return true;
}

template <typename T, typename U>
void DelayLine::adopt(std::vector<T>& buf, const std::vector<U>& from, std::int32_t fromRing,
                      std::int32_t fromWrite, std::int32_t k) noexcept {
    for (std::int32_t c = 0; c < channels_; ++c) {
        const U* src = from.data() + static_cast<std::size_t>(c) *
                                     static_cast<std::size_t>(fromRing);
        T* dst = buf.data() + static_cast<std::size_t>(c) *
                              static_cast<std::size_t>(ring_);
        for (std::int32_t i = 0; i < k; ++i) {
            // dst[0] is the OLDEST sample carried and dst[k-1] the newest.
            // `fromWrite` is the NEXT write, so the newest written sample is
            // one behind it.
            std::int32_t r = (fromWrite - k + i) % fromRing;
            if (r < 0) r += fromRing;
            dst[i] = static_cast<T>(src[r]);
        }
    }
}

bool DelayLine::adoptHistory(const DelayLine& old) noexcept {
    if (ring_ <= 0 || old.ring_ <= 0 || channels_ != old.channels_) return false;

    // `buf_` and `write_` are the live history in every state: while an old
    // ring is growing both rings are written and `buf_` is the one read, and
    // after its swap `buf_` IS the grown ring. But while it grows, the ring
    // it is growing INTO may already hold more of the edge's past than the
    // small one can, and this line was built for the delay that ring is for.
    // This is the audio thread, which owns the incoming ring from Priming to
    // the swap.
    const Grow g = old.grow_.load(std::memory_order_acquire);
    const bool growing = g == Grow::Priming || g == Grow::Fading;
    // Only what each ring really holds: one whose edge sat at zero, or
    // slept, holds whatever it last had, and handing that on would play it.
    const std::int32_t inOld = (std::min)(old.capacity_, old.filled_);
    const std::int32_t inNew = growing ? (std::min)(old.inCapacity_, old.inFilled_) : 0;
    const bool fromIncoming = inNew > inOld;

    std::int32_t k = delay_;
    if (k > capacity_) k = capacity_;
    if (k > (fromIncoming ? inNew : inOld)) k = fromIncoming ? inNew : inOld;
    if (k < 0) k = 0;
    // What it was handed is all it holds: a glide past it waits for the rest.
    // And as much of it as the old line knew was silence is silence here: a
    // handover in the middle of a drain hands on audio still due out, which
    // this line's consumer must stay awake to play (`due`). Both rings of a
    // grow take the same writes, so the old count holds for either.
    filled_ = k;
    quiet_ = (std::min)(old.quiet_, k);
    if (k == 0) return false;
    const std::int32_t fromRing = fromIncoming ? old.inRing_ : old.ring_;
    const std::int32_t fromWrite = fromIncoming ? old.inWrite_ : old.write_;
    const std::vector<float>& f32 = fromIncoming ? old.incoming_ : old.buf_;
    const std::vector<double>& f64 = fromIncoming ? old.wincoming_ : old.wbuf_;

    //
    // ACROSS A PRECISION SWITCH TOO (ADR-0207 d2). The switch is a rebuild,
    // and a compensated edge that started its new ring from zeros would drop
    // `delay` samples of that track: a hole, not a seam. Float history widens
    // exactly; double history is rounded once, as the output it was going to
    // be would have been at 32.
    if (wide_ && old.wide_)        adopt(wbuf_, f64, fromRing, fromWrite, k);
    else if (wide_ && !old.wide_)  adopt(wbuf_, f32, fromRing, fromWrite, k);
    else if (!wide_ && old.wide_)  adopt(buf_, f64, fromRing, fromWrite, k);
    else                           adopt(buf_, f32, fromRing, fromWrite, k);
    // The next write lands right after the newest carried sample, so a tap at
    // `delay_` reads exactly what the old line's tap would have read next.
    write_ = k % ring_;
    return true;
}

DelayLine::Edge DelayLine::beginEdge() const noexcept {
    Edge e;
    e.grow_ = grow_.load(std::memory_order_acquire);
    e.glide_ = gliding_.load(std::memory_order_acquire);
    e.target_ = e.glide_ ? target_.load(std::memory_order_relaxed) : delay_;
    // A target past the history THIS ring holds waits, held: it is meant for
    // the ring being offered (an offer stores its delay, and a glide may be
    // asked for against the new ring, before this thread is sure to see the
    // Priming that says so), or it is a ring fresh from a swap that has not
    // been written that far yet. Nothing reads past the ring it has, and no
    // tap reads samples nobody stored. `filled_` is at most the capacity.
    if (e.glide_ && e.target_ > filled_) {
        e.glide_ = false;
        e.target_ = delay_;
    }
    e.write_ = write_;
    // The incoming ring's cursor and count are the audio thread's only from
    // Priming to the swap; the acquire above is what makes the offer's
    // values visible. In Idle and Spent they are the message thread's.
    if (e.grow_ == Grow::Priming || e.grow_ == Grow::Fading) {
        e.inWrite_ = inWrite_;
        e.primeRemaining_ = primeRemaining_;
    }
    return e;
}

void DelayLine::endEdge(const Edge& e, bool silentSource) noexcept {
    // The history this edge wrote, once for all its channels.
    const std::int32_t ran = ran_;
    ran_ = 0;
    filled_ = static_cast<std::int32_t>(
        (std::min)(static_cast<std::int64_t>(capacity_), static_cast<std::int64_t>(filled_) + ran));
    // Silence written extends the silence; anything else ends it. A block is
    // judged whole (`Slot::silent`), so a loud one counts for none of it.
    quiet_ = silentSource ? static_cast<std::int32_t>((std::min)(
                                static_cast<std::int64_t>(filled_),
                                static_cast<std::int64_t>(quiet_) + ran))
                          : 0;
    const Grow g = e.grow_;
    if (g == Grow::Priming || g == Grow::Fading)
        inFilled_ = static_cast<std::int32_t>((std::min)(static_cast<std::int64_t>(inCapacity_),
                                                         static_cast<std::int64_t>(inFilled_) + ran));
    if (g == Grow::Idle || g == Grow::Spent) {
        if (e.glide_) {
            // The delay the crossfade reached, which is the snapshot's: a
            // target the message thread moved since is the NEXT glide.
            delay_ = e.target_;
            settle();
        }
        return;
    }

    // A glide asked for while a ring grows waits for the swap: the old ring
    // is the one being read and it is on its way out.
    if (g == Grow::Priming) {
        // PRIMING ENDS ON A BLOCK BOUNDARY, not mid-call. Letting it end
        // mid-call means one call that is part prime and part crossfade, and
        // the bookkeeping for that is worth more than the one extra block it
        // saves.
        if (primeRemaining_ <= 0) {
            primeRemaining_ = 0;
            grow_.store(Grow::Fading, std::memory_order_release);
        }
        return;
    }
    // Fading: the swap. A vector swap moves pointers -- no allocation, which
    // is what lets this happen on the audio thread at all. The old buffer
    // lands in the incoming slot and waits for `collectRing`; the audio
    // thread never deallocates. Both widths are swapped: the unused pair is
    // two empty vectors, and swapping them costs nothing.
    buf_.swap(incoming_);
    wbuf_.swap(wincoming_);
    std::swap(capacity_, inCapacity_);
    std::swap(ring_, inRing_);
    write_ = inWrite_;
    filled_ = inFilled_;
    if (quiet_ > filled_) quiet_ = filled_;   // the same writes, on a ring that holds less
    delay_ = targetDelay_;
    // `target_` is the message thread's: the offer set it to this delay, so
    // a glide pending from before the offer comes down here, and one asked
    // for during the grow (against the new ring) goes on from the next block.
    settle();
    grow_.store(Grow::Spent, std::memory_order_release);
}

template <typename T>
void DelayLine::processGrowing(std::vector<T>& buf, std::vector<T>& incoming,
                               std::int32_t channel, const T* src, T* dst,
                               std::int32_t frames, Grow state) noexcept {
    T* oldHist = buf.data() + static_cast<std::size_t>(channel) *
                              static_cast<std::size_t>(ring_);
    T* newHist = incoming.data() + static_cast<std::size_t>(channel) *
                                   static_cast<std::size_t>(inRing_);
    std::int32_t w = write_, nw = inWrite_;

    // BOTH RINGS ARE WRITTEN THROUGHOUT. The new one is accumulating the
    // history it will need; the old one is still the one being read, and will
    // be until the very last sample of the fade.
    if (state == Grow::Priming) {
        const std::int32_t d = delay_;
        for (std::int32_t i = 0; i < frames; ++i) {
            const T in = src[i];   // src == dst is legal: read before writing
            oldHist[w] = in;
            newHist[nw] = in;
            dst[i] = tapAt(oldHist, w, ring_, d);
            if (++w == ring_) w = 0;
            if (++nw == inRing_) nw = 0;
        }
        write_ = w;
        inWrite_ = nw;
        primeRemaining_ -= frames;
        if (primeRemaining_ < 0) primeRemaining_ = 0;
        return;
    }

    // Fading: the old tap and the new one are genuinely different samples --
    // that is what a latency change IS -- so this is a crossfade and not a
    // reconciliation. One block, same as ADR-0079's tap move, except the two
    // taps live in different rings.
    const std::int32_t dOld = delay_;
    const std::int32_t dNew = targetDelay_;
    const T step = frames > 0 ? T{1} / static_cast<T>(frames) : T{0};
    T t = T{0};
    for (std::int32_t i = 0; i < frames; ++i) {
        const T in = src[i];
        oldHist[w] = in;
        newHist[nw] = in;
        t += step;
        const T a = tapAt(oldHist, w, ring_, dOld);
        const T b = tapAt(newHist, nw, inRing_, dNew);
        dst[i] = a + (b - a) * t;
        if (++w == ring_) w = 0;
        if (++nw == inRing_) nw = 0;
    }
    write_ = w;
    inWrite_ = nw;
}

template <typename T>
void DelayLine::run(const Edge& e, std::vector<T>& buf, std::vector<T>& incoming,
                    std::int32_t channel, const T* src, T* dst,
                    std::int32_t frames) noexcept {
    // Every channel starts from the edge's cursors. Only the ones this thread
    // owns in the edge's state are written back (see `Edge`).
    write_ = e.write_;
    ran_ = frames > 0 ? frames : 0;
    const Grow g = e.grow_;
    if (g == Grow::Priming || g == Grow::Fading) {
        inWrite_ = e.inWrite_;
        primeRemaining_ = e.primeRemaining_;
        processGrowing(buf, incoming, channel, src, dst, frames, g);
        return;
    }

    T* hist = buf.data() + static_cast<std::size_t>(channel) *
                           static_cast<std::size_t>(ring_);
    std::int32_t w = write_;

    const bool glide = e.glide_;
    const std::int32_t to = e.target_;

    if (!glide || to == delay_ || frames <= 0) {
        const std::int32_t d = delay_;
        for (std::int32_t i = 0; i < frames; ++i) {
            const T in = src[i];   // src == dst is legal, so read before writing
            hist[w] = in;
            dst[i] = tapAt(hist, w, ring_, d);
            if (++w == ring_) w = 0;
        }
        write_ = w;
        return;
    }

    // THE TAP MOVES, THE AUDIO IS RENDERED ONCE (ADR-0079).
    //
    // Both taps read the SAME history, which is the whole reason this works
    // and the reason a freshly published buffer cannot: a new ring holds
    // nothing, so fading into it is a fade to silence for `delay` samples,
    // not a transition between two alignments.
    //
    // A latency change IS a time shift, so the two taps genuinely differ and
    // no crossfade makes that inaudible. What it does buy is that the
    // difference arrives as a brief flange rather than as a click.
    const std::int32_t from = delay_;
    const T step = T{1} / static_cast<T>(frames);
    T t = T{0};
    for (std::int32_t i = 0; i < frames; ++i) {
        const T in = src[i];
        hist[w] = in;
        t += step;
        const T a = tapAt(hist, w, ring_, from);
        const T b = tapAt(hist, w, ring_, to);
        dst[i] = a + (b - a) * t;
        if (++w == ring_) w = 0;
    }
    write_ = w;
}

void DelayLine::process(const Edge& e, std::int32_t channel, const float* src, float* dst,
                        std::int32_t frames) noexcept {
    if (wide_ || ring_ <= 0 || channel < 0 || channel >= channels_) {
        if (src != dst)
            std::memcpy(dst, src, static_cast<std::size_t>(frames) * sizeof(float));
        return;
    }
    run(e, buf_, incoming_, channel, src, dst, frames);
}

void DelayLine::process(const Edge& e, std::int32_t channel, const double* src, double* dst,
                        std::int32_t frames) noexcept {
    if (!wide_ || ring_ <= 0 || channel < 0 || channel >= channels_) {
        if (src != dst)
            std::memcpy(dst, src, static_cast<std::size_t>(frames) * sizeof(double));
        return;
    }
    run(e, wbuf_, wincoming_, channel, src, dst, frames);
}

// One thread alone: the state as it stands, which is the cursors the caller
// restored.
void DelayLine::process(std::int32_t channel, const float* src, float* dst,
                        std::int32_t frames) noexcept {
    process(beginEdge(), channel, src, dst, frames);
}

void DelayLine::process(std::int32_t channel, const double* src, double* dst,
                        std::int32_t frames) noexcept {
    process(beginEdge(), channel, src, dst, frames);
}

void Graph::prepareLine(DelayLine& line, std::int32_t delaySamples) {
    const std::int32_t d = delaySamples > 0 ? delaySamples : 0;
    // The ring is the delay PLUS the headroom, so a plugin that switches to
    // linear phase can move its tap without anyone allocating (ADR-0079).
    // Headroom 0 -- the default -- gives exactly today's behaviour and costs
    // exactly today's memory, so a project that never changes latency at
    // runtime pays nothing for the ability.
    //
    // An edge with no delay AND no headroom gets no ring at all. With headroom
    // it gets one, because an edge at zero today is the one most likely to
    // need a delay tomorrow: it is the direct path everything else is
    // compensated against.
    const std::int32_t cap = d + latencyHeadroom_;
    line.prepare(channels_, cap, precision_ == Precision::Float64);
    line.setDelay(d);
}

namespace {

/// ADR-0203 d3: an edge from a feedback loop's reader or into its writer is
/// the loop's own, and its line never delays. (The reader's arrival and
/// latency are 0, so it never sets its junction's arrival either.)
bool loopEdge(const Node* from, const Node* to) noexcept {
    return (from != nullptr && from->closesFeedback()) || (to != nullptr && to->closesFeedback());
}

}  // namespace

void Graph::computeCompensation() {
    // ADR-0058 decision 2. A node's input is whole only once the LATEST of its
    // feeds has arrived, so its arrival is the max over inputs of (that input's
    // arrival + that input's own latency). Every earlier feed is then delayed
    // to match, and that is the whole of phase alignment.
    //
    // Walking `order_` means every input's arrival is already final when it is
    // read. That is what the topological order is for, beyond scheduling.
    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        std::int64_t latest = 0;
        for (NodeId in : s.inputs) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        // ADR-0058 decision 5: a sidechain is compensated WITH the rest, not
        // apart from it. A compressor whose key arrives early ducks early,
        // which is the same defect as a misaligned kick and harder to hear.
        for (NodeId in : s.sidechains) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        s.arrival = static_cast<std::int32_t>(latest);
    }

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        s.inDelays.resize(s.inputs.size());
        if (s.hasBypass) prepareLine(s.dryDelay, s.node->latencySamples());
        for (std::size_t k = 0; k < s.inputs.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            if (!delayCompensation_ || loopEdge(u.node, s.node)) {
                // No ring at all, not even the headroom: it never delays,
                // so a retap that tried to move it would be refused.
                s.inDelays[k].prepare(channels_, 0, precision_ == Precision::Float64);
                s.inDelays[k].setDelay(0);
                continue;
            }
            prepareLine(s.inDelays[k], s.arrival - ready);
        }
        s.sideDelays.resize(s.sidechains.size());
        for (std::size_t k = 0; k < s.sidechains.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = delayCompensation_ ? s.arrival - ready : 0;
            if(delayCompensation_)prepareLine(s.sideDelays[k],d);
            else {
                s.sideDelays[k].prepare(channels_,0,precision_==Precision::Float64);
                s.sideDelays[k].setDelay(0);
            }
        }
    }

    // ADR-0123: the answers, kept for the audio thread. Asked here, on the
    // message thread, because a node may be a CLAP plugin whose latency
    // getter is main-thread only.
    if (slotLatency_.size() != slots_.size())
        slotLatency_ = std::vector<std::atomic<std::int32_t>>(slots_.size());
    if (slotArrival_.size() != slots_.size())
        slotArrival_ = std::vector<std::atomic<std::int32_t>>(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slotLatency_[i].store(slots_[i].node != nullptr ? slots_[i].node->latencySamples() : 0,
                              std::memory_order_relaxed);
        slotArrival_[i].store(slots_[i].arrival, std::memory_order_relaxed);
    }
    const Slot& out = slots_[static_cast<std::size_t>(output_)];
    graphLatency_.store(out.arrival + (out.node != nullptr ? out.node->latencySamples() : 0),
                        std::memory_order_release);
}

bool Graph::retapLatency() noexcept {
    if (!prepared_) return false;

    // The same arithmetic as `computeCompensation`, against the latencies the
    // nodes report NOW. Deliberately not factored into one function with it:
    // that one ALLOCATES and this one must not, and a shared helper is one
    // edit away from allocating on the audio thread's behalf.
    bool allFit = true;

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        std::int64_t latest = 0;
        for (NodeId in : s.inputs) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        for (NodeId in : s.sidechains) {
            const Slot& u = slots_[static_cast<std::size_t>(in)];
            const std::int64_t ready = static_cast<std::int64_t>(u.arrival) +
                (u.node != nullptr ? u.node->latencySamples() : 0);
            if (ready > latest) latest = ready;
        }
        s.arrival = static_cast<std::int32_t>(latest);
    }

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        if (s.hasBypass && !s.dryDelay.beginGlide((std::max)(0, s.node->latencySamples())))
            allFit = false;
        for (std::size_t k = 0; k < s.inputs.size() && k < s.inDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = !delayCompensation_ || loopEdge(u.node, s.node) ? 0 : s.arrival - ready;
            // EVERYTHING THAT FITS IS MOVED, even when something else does not.
            // A partial correction is closer to right than none, and the edges
            // that failed are about to be rebuilt anyway.
            if (!s.inDelays[k].beginGlide(d > 0 ? d : 0)) allFit = false;
        }
        for (std::size_t k = 0; k < s.sidechains.size() && k < s.sideDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            const std::int32_t ready =
                u.arrival + (u.node != nullptr ? u.node->latencySamples() : 0);
            const std::int32_t d = delayCompensation_ ? s.arrival - ready : 0;
            if (!s.sideDelays[k].beginGlide(d > 0 ? d : 0)) allFit = false;
        }
    }
    // After every glide it started: a sleeping node that sees this move looks
    // at its lines again (`Slot::dozeGlides`).
    glides_.fetch_add(1, std::memory_order_release);

    // ADR-0123: refresh the audio thread's copies. The event delay jumps to the
    // new value while the audio tap glides; that is what it did before, when
    // the audio thread asked the node itself.
    for (std::size_t i = 0; i < slots_.size() && i < slotLatency_.size(); ++i)
        slotLatency_[i].store(slots_[i].node != nullptr ? slots_[i].node->latencySamples() : 0,
                              std::memory_order_relaxed);
    for (std::size_t i = 0; i < slots_.size() && i < slotArrival_.size(); ++i)
        slotArrival_[i].store(slots_[i].arrival, std::memory_order_relaxed);
    const Slot& out = slots_[static_cast<std::size_t>(output_)];
    graphLatency_.store(out.arrival + (out.node != nullptr ? out.node->latencySamples() : 0),
                        std::memory_order_release);
    return allFit;
}

namespace {

/// The required delay on one edge, from the arrivals already computed.
std::int32_t requiredDelay(std::int32_t toArrival, std::int32_t fromArrival,
                           const Node* fromNode, const Node* toNode) noexcept {
    if (loopEdge(fromNode, toNode)) return 0;
    const std::int32_t ready =
        fromArrival + (fromNode != nullptr ? fromNode->latencySamples() : 0);
    const std::int32_t d = toArrival - ready;
    return d > 0 ? d : 0;
}

}  // namespace

std::size_t Graph::escalateLatency(Unresolved* left) {
    if (!prepared_) return 0;

    std::size_t grown = 0;
    Unresolved unresolved;
    // THE ALLOCATION IS HERE, on the message thread, and that is the whole
    // point of the split. The audio thread receives a ready-made buffer and
    // never does anything but write into it and swap a pointer.
    auto offer = [&](DelayLine& line, std::int32_t want) {
        // In this order: the audio thread swaps the capacity at the end of a
        // grow, so it is read only once no grow is in flight.
        if (line.growing()) {
            // An offer is already in flight. If it is for less than this edge
            // now needs, the edge is grown again once that ring is collected.
            if (want > line.limit()) ++unresolved.inFlight;
            return;
        }
        if (want <= line.capacity()) return;   // the tap move already handles it

        // Headroom on top of the requirement, so a plugin that steps its
        // latency up repeatedly does not force a grow per step. Doubling is
        // the usual answer and is wrong here: a 2-sample edge growing to 4 is
        // a grow every time. The requirement plus the graph's headroom gives
        // the same slack the edge would have had if it had been built at this
        // size in the first place.
        const std::int32_t cap = want + latencyHeadroom_;
        const std::size_t size =
            static_cast<std::size_t>(channels_) * static_cast<std::size_t>(cap + 1);
        // The ring at the line's own width (ADR-0207 d2).
        if (line.capacity() <= 0) {
            ++unresolved.ringless;             // refused: nothing would run it
            return;
        }
        const bool taken = line.wide()
            ? line.offerRing(std::vector<double>(size, 0.0), cap, want)
            : line.offerRing(std::vector<float>(size, 0.0f), cap, want);
        if (taken) ++grown;
    };

    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];
        if (s.hasBypass) offer(s.dryDelay, (std::max)(0, s.node->latencySamples()));
        // Disabled alignment has no required input rings. Bypass delay is
        // physical and still grows independently through the offer above.
        if(!delayCompensation_)continue;
        for (std::size_t k = 0; k < s.inputs.size() && k < s.inDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            offer(s.inDelays[k], requiredDelay(s.arrival, u.arrival, u.node, s.node));
        }
        for (std::size_t k = 0; k < s.sidechains.size() && k < s.sideDelays.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.sidechains[k])];
            offer(s.sideDelays[k], requiredDelay(s.arrival, u.arrival, u.node, s.node));
        }
    }
    if (left != nullptr) *left = unresolved;
    return grown;
}

std::size_t Graph::collectRings() {
    std::size_t freed = 0;
    // The returned vector is destroyed here, on the message thread. That is
    // the deallocation the audio thread deliberately did not do.
    auto take = [&](DelayLine& line) {
        if (line.reclaim()) ++freed;
    };
    for (Slot& s : slots_) {
        if (s.hasBypass) take(s.dryDelay);
        for (DelayLine& d : s.inDelays) take(d);
        for (DelayLine& d : s.sideDelays) take(d);
    }
    return freed;
}

DelayLine* Graph::edgeLine(NodeId from, NodeId to, Bus bus) noexcept {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return nullptr;
    Slot& s = slots_[static_cast<std::size_t>(to)];
    auto& edges = (bus == Bus::Sidechain) ? s.sidechains : s.inputs;
    auto& lines = (bus == Bus::Sidechain) ? s.sideDelays : s.inDelays;
    for (std::size_t k = 0; k < edges.size() && k < lines.size(); ++k)
        if (edges[k] == from) return &lines[k];
    return nullptr;
}

std::size_t Graph::compensationBytes() const noexcept {
    std::size_t n = 0;
    for (const Slot& s : slots_) {
        if (s.hasBypass) n += s.dryDelay.bytes();
        for (const DelayLine& d : s.inDelays) n += d.bytes();
        for (const DelayLine& d : s.sideDelays) n += d.bytes();
    }
    return n;
}

std::size_t Graph::growingEdges() const noexcept {
    std::size_t n = 0;
    for (const Slot& s : slots_) {
        if (s.hasBypass && s.dryDelay.growing()) ++n;
        for (const DelayLine& d : s.inDelays) if (d.growing()) ++n;
        for (const DelayLine& d : s.sideDelays) if (d.growing()) ++n;
    }
    return n;
}

std::int32_t Graph::arrivalOf(NodeId id) const noexcept {
    if (id < 0 || id >= static_cast<NodeId>(slots_.size())) return -1;
    return slots_[static_cast<std::size_t>(id)].arrival;
}

std::int32_t Graph::compensationFor(NodeId from, NodeId to, Bus bus) const noexcept {
    if (to < 0 || to >= static_cast<NodeId>(slots_.size())) return -1;
    const Slot& s = slots_[static_cast<std::size_t>(to)];
    const auto& edges = (bus == Bus::Sidechain) ? s.sidechains : s.inputs;
    const auto& lines = (bus == Bus::Sidechain) ? s.sideDelays : s.inDelays;
    for (std::size_t k = 0; k < edges.size() && k < lines.size(); ++k)
        if (edges[k] == from) return lines[k].delay();
    return -1;
}

// ---------------------------------------------------------------------------
// prepare / release
// ---------------------------------------------------------------------------

void Graph::prepare(double sampleRate, std::int32_t maxFrames) {
    workers_.reset();workerRenders_.clear();levelWork_.clear();
    ok_ = false;
    prepared_ = false;
    renderedFrames_ = 0;
    error_.clear();
    stats_ = GraphStats{};

    if (maxFrames <= 0) { error_ = "maxFrames must be positive"; return; }
    if (slots_.empty()) { error_ = "the graph has no nodes"; return; }
    if (output_ < 0 || output_ >= static_cast<NodeId>(slots_.size())) {
        error_ = "no output node set";
        return;
    }
    if (!topoSort()) return;

    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    controllerSources_.clear();
    controllerSources_.reserve(slots_.size());
    for (const auto &slot : slots_)
        if (slot.node) {
            const auto prefix = slot.node->controllerSourcePrefix();
            if (prefix)
                controllerSources_.push_back({prefix, slot.node});
        }
    std::sort(controllerSources_.begin(), controllerSources_.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    for (std::size_t i = 1; i < controllerSources_.size(); ++i)
        if (controllerSources_[i - 1].first == controllerSources_[i].first) {
            error_ = "duplicate controller source prefix";
            return;
        }

    // ADR-0042's floor, clamped by ADR-0054's bound. Clamped rather than
    // rejected: a caller asking for 128 at 48 kHz wants fewer segments, and
    // giving them 96 is closer to the request than refusing to run.
    const std::int32_t cap = maxFloorFor(sampleRate);
    if (floor_ > cap) floor_ = cap;
    if (floor_ < 1) floor_ = 1;

    if (eventCapacity_ <= 0)
        eventCapacity_ = deriveEventCapacity(sampleRate, maxFrames, maxPolyphony_);

    const auto ch = static_cast<std::size_t>(channels_);
    const auto fr = static_cast<std::size_t>(maxFrames);

    const bool wide = precision_ == Precision::Float64;
    // Establish worker resources before any borrowed device is prepared, so
    // driver initialization failure cannot reset the running graph's nodes.
    prepareRenderState(render_);
    if(requestedWorkers_>0 && maxFrames_>=parallelMinimumFrames_) {
        std::map<Node*,std::size_t,std::less<Node*>> occurrences;
        for(const auto& slot:slots_)if(slot.node)++occurrences[slot.node];
        std::size_t widestParallel=0;levelWork_.resize(levels_.size());
        for(std::size_t index=0;index<levels_.size();++index) {
            auto& work=levelWork_[index];
            for(const auto id:levels_[index]) {
                auto* node=slots_[static_cast<std::size_t>(id)].node;
                if(node && occurrences[node]==1 && node->canProcessOnWorker())work.parallel.push_back(id);
                else work.serial.push_back(id);
            }
            widestParallel=(std::max)(widestParallel,work.parallel.size());
        }
        if(widestParallel>=parallelMinimumNodes_) {
            const auto count=(std::min)(static_cast<std::size_t>(requestedWorkers_),widestParallel-1);
            workerRenders_.resize(count);for(auto& state:workerRenders_)prepareRenderState(state);
            try{workers_=std::make_unique<RenderWorkers>(count,workerInitializer_,workerInitializationContext_);}
            catch(const std::exception& exception){workerRenders_.clear();error_=std::string("render worker setup: ")+exception.what();return;}
        }
    }
    bypassNodes_.clear();
    eventNodes_.clear();
    for (auto& s : slots_) {
        if(s.node)eventNodes_.emplace_back(s.node,static_cast<NodeId>(&s-slots_.data()));
        // A persistent device can be rebound while this graph is rendering.
        // The pointer and lifetime belong to the prepared graph generation;
        // no shared_ptr operation or live binding read occurs on the callback.
        s.addressedSourceOwner = s.node ? s.node->eventSourceLifetime() : std::shared_ptr<void>{};
        s.addressedSource = s.node ? s.node->eventSource() : nullptr;
        s.pendingSources.clear();
        s.hasBypass = s.node != nullptr && s.node->hasBypass();
        if (s.hasBypass)
            bypassNodes_.emplace_back(s.node, static_cast<NodeId>(&s - slots_.data()));
        // ADR-0207 d2. At 64 every slot's output is double; a node that does
        // not process double keeps a float buffer of its own as well, the
        // one it writes, so its view of its output is exactly what it is at 32.
        s.doubles = wide && s.node != nullptr && s.node->processesDouble();
        if (wide) {
            s.wideAudio.assign(ch * fr, 0.0);
            s.widePtrs.resize(ch);
            for (std::size_t c = 0; c < ch; ++c) s.widePtrs[c] = s.wideAudio.data() + c * fr;
        } else {
            std::vector<double>().swap(s.wideAudio);
            s.widePtrs.clear();
        }
        if (s.doubles) std::vector<float>().swap(s.audio);
        else s.audio.assign(ch * fr, 0.0f);
        s.chanPtrs.resize(ch);
        for (std::size_t c = 0; c < ch; ++c)
            s.chanPtrs[c] = s.doubles ? nullptr : s.audio.data() + c * fr;
        s.eventStore.assign(static_cast<std::size_t>(eventCapacity_), Event{});
        s.events = EventList(s.eventStore.data(), eventCapacity_);
        s.eventOrigins.assign(static_cast<std::size_t>(eventCapacity_),nullptr);
        // Same capacity as the live list. A deferred event is one that WOULD
        // have been in the list if its delay had been shorter, so the two
        // share one budget rather than one being an afterthought.
        s.pending.assign(static_cast<std::size_t>(eventCapacity_), Slot::Pending{});
        s.pendingCount = 0;
        // ARMED FROM THE DECLARATION, not zero. Starting at zero suspends a
        // kInfiniteTail node on its very first block -- the one thing ADR-0043
        // says must never happen -- because the counter has expired before
        // anything has had a chance to arm it. It cost a generator that never
        // ran and four tests that looked like fixture errors.
        const std::int64_t t = s.node != nullptr ? s.node->tailSamples() : 0;
        s.tailRemaining = (t == kInfiniteTail) ? 0 : t;
        s.silent = true;
        s.zeroed = true;                 // `assign` above filled it with zeros
        s.dozeGlides = kLinesUnchecked;
        if (s.node != nullptr) {
            s.node->willBeHanded(s.doubles ? Precision::Float64 : Precision::Float32);
            s.node->prepare(sampleRate, maxFrames);
        }
    }

    std::sort(bypassNodes_.begin(), bypassNodes_.end(), [](const auto& a, const auto& b) {
        return std::less<Node*>{}(a.first, b.first);
    });
    std::sort(eventNodes_.begin(),eventNodes_.end(),[](const auto& a,const auto& b){return std::less<Node*>{}(a.first,b.first);});
    for(const auto id:order_) {
        auto& slot=slots_[static_cast<std::size_t>(id)];
        slot.deliversNoteStream=slot.node &&
            (id==output_ || slot.node->eventFlow()==EventFlow::Consume);
        for(const auto input:slot.inputs) {
            const auto& source=slots_[static_cast<std::size_t>(input)];
            if(source.node && source.node->eventFlow()==EventFlow::Through)
                slot.pendingSources.insert(slot.pendingSources.end(),source.pendingSources.begin(),source.pendingSources.end());
        }
        if(slot.node && slot.node->retainsQueuedSourceEvents())slot.pendingSources.push_back(slot.node);
        std::sort(slot.pendingSources.begin(),slot.pendingSources.end(),std::less<Node*>{});
        slot.pendingSources.erase(std::unique(slot.pendingSources.begin(),slot.pendingSources.end()),slot.pendingSources.end());
    }
    // A delayed dead-end Stop branch cannot postpone another branch's release.
    // Build reachability on the owner; the callback only scans bounded queues.
    for(std::size_t i=order_.size();i>0;--i) {
        const auto& slot=slots_[static_cast<std::size_t>(order_[i-1])];
        if(!slot.deliversNoteStream)continue;
        for(const auto input:slot.inputs) {
            auto& upstream=slots_[static_cast<std::size_t>(input)];
            if(upstream.node && upstream.node->eventFlow()==EventFlow::Through)
                upstream.deliversNoteStream=true;
        }
    }

    // One extra split beyond the maximum distinct frame count.
    splits_.assign(fr + 2, 0);
    frameMark_.assign(fr, 0);
    now_ = 0;

    // ADR-0058 decisions 2-5, after the buffers exist: a delay line is a
    // buffer too, and `process` may not allocate.
    computeCompensation();


    ok_ = true;
    prepared_ = true;
}

void Graph::prepareRenderState(RenderState& r) {
    const auto ch=static_cast<std::size_t>(channels_);
    const auto fr=static_cast<std::size_t>(maxFrames_);
    const bool wide=precision_==Precision::Float64;
    r.mixBuf_.assign(ch * fr, 0.0f);
    r.mixPtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) r.mixPtrs_[c] = r.mixBuf_.data() + c * fr;
    r.sideBuf_.assign(ch * fr, 0.0f);
    r.sidePtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) r.sidePtrs_[c] = r.sideBuf_.data() + c * fr;
    r.mixWide_.assign(ch * fr, 0.0);
    r.mixWidePtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) r.mixWidePtrs_[c] = r.mixWide_.data() + c * fr;
    r.sideWide_.assign(ch * fr, 0.0);
    r.sideWidePtrs_.resize(ch);
    for (std::size_t c = 0; c < ch; ++c) r.sideWidePtrs_[c] = r.sideWide_.data() + c * fr;

    r.delayScratch_.assign(fr, 0.0f);
    if (wide) r.delayScratch64_.assign(fr, 0.0);
    else std::vector<double>().swap(r.delayScratch64_);
    std::size_t widest = 0;
    for (const Slot& s : slots_)
        widest = (std::max)(widest, (std::max)(s.inputs.size(), s.sidechains.size()));
    r.asleep_.assign(widest, 0);
    r.stats=GraphStats{};
}
void Graph::collectRenderStats(const RenderState& r) noexcept {
    stats_.nodeCalls+=r.stats.nodeCalls;
    stats_.nodesSuspended+=r.stats.nodesSuspended;
    stats_.suspendClears+=r.stats.suspendClears;
    stats_.inputsSkipped+=r.stats.inputsSkipped;
    stats_.wideMixes+=r.stats.wideMixes;
    stats_.workerNodeBlocks+=r.stats.workerNodeBlocks;
}

void Graph::release() {
    workers_.reset();workerRenders_.clear();levelWork_.clear();
    for (auto& s : slots_)
        if (s.node != nullptr) s.node->release();
    prepared_ = false;
    ok_ = false;
}

std::int32_t Graph::adoptBypassHistory(const Graph& old) noexcept {
    // A changed rate describes a different time base, not the same history.
    if (sampleRate_ != old.sampleRate_) return 0;
    std::size_t i = 0, j = 0;
    std::int32_t carried = 0;
    while (i < bypassNodes_.size() && j < old.bypassNodes_.size()) {
        const auto& a = bypassNodes_[i];
        const auto& b = old.bypassNodes_[j];
        if (std::less<Node*>{}(a.first, b.first)) { ++i; continue; }
        if (std::less<Node*>{}(b.first, a.first)) { ++j; continue; }
        auto& dst = slots_[static_cast<std::size_t>(a.second)];
        const auto& src = old.slots_[static_cast<std::size_t>(b.second)];
        if (dst.dryDelay.adoptHistory(src.dryDelay)) ++carried;
        dst.dozeGlides = kLinesUnchecked;
        ++i;
        ++j;
    }
    return carried;
}

std::int32_t Graph::adoptEventHistory(const Graph& old) noexcept {
    if(sampleRate_!=old.sampleRate_)return 0;
    std::size_t i=0,j=0;
    std::int32_t carried=0;
    while(i<eventNodes_.size() && j<old.eventNodes_.size()) {
        const auto& a=eventNodes_[i];const auto& b=old.eventNodes_[j];
        if(std::less<Node*>{}(a.first,b.first)){++i;continue;}
        if(std::less<Node*>{}(b.first,a.first)){++j;continue;}
        auto nextI=i+1,nextJ=j+1;
        while(nextI<eventNodes_.size() && eventNodes_[nextI].first==a.first)++nextI;
        while(nextJ<old.eventNodes_.size() && old.eventNodes_[nextJ].first==b.first)++nextJ;
        if(nextI==i+1 && nextJ==j+1) {
            auto& dst=slots_[static_cast<std::size_t>(a.second)];
            const auto& src=old.slots_[static_cast<std::size_t>(b.second)];
            const auto arrival=slotArrival_[static_cast<std::size_t>(a.second)].load(std::memory_order_relaxed);
            const auto oldArrival=old.slotArrival_[static_cast<std::size_t>(b.second)].load(std::memory_order_relaxed);
            if(arrival==oldArrival)for(std::int32_t n=0;n<src.pendingCount;++n) {
                if(static_cast<std::size_t>(dst.pendingCount)>=dst.pending.size())break;
                const auto& pending=src.pending[static_cast<std::size_t>(n)];
                const auto remaining=pending.due-old.now_;
                if(remaining<0 || remaining>std::numeric_limits<std::int64_t>::max()-now_)continue;
                Node* owner=nullptr;
                for(std::size_t source=0;source<dst.pendingSources.size();++source) {
                    auto* candidate=dst.pendingSources[source];
                    if(candidate->canRetainPendingEvent(pending.e)) {
                        // Ambiguous identities cannot transfer another source's event.
                        if(owner){owner=nullptr;break;}
                        owner=candidate;
                    }
                }
                if(!owner)continue;
                dst.pending[static_cast<std::size_t>(dst.pendingCount++)]={now_+remaining,pending.e,owner};
                owner->pendingEventRetained(pending.e);++carried;
            }
        }
        i=nextI;j=nextJ;
    }
    return carried;
}

// ---------------------------------------------------------------------------
// process
// ---------------------------------------------------------------------------

bool Graph::idle(const Slot& src, const DelayLine& delay) noexcept {
    return src.zeroed && !delay.busy() && delay.delay() == 0;
}

template <typename T>
T* const* Graph::outputOf(const Slot& s) noexcept {
    if constexpr (std::is_same_v<T, float>) return s.chanPtrs.data();
    else return s.widePtrs.data();
}

template <typename T>
T* Graph::scratch(RenderState& r) noexcept {
    if constexpr (std::is_same_v<T, float>) return r.delayScratch_.data();
    else return r.delayScratch64_.data();
}

template <typename Src, typename Acc>
void Graph::accumulate(RenderState& r, std::vector<Acc*>& dst, const Slot& src, DelayLine& delay,
                       std::int32_t begin, std::int32_t frames, bool first,
                       bool asleep) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);
    Src* const* from = outputOf<Src>(src);
    const auto n = static_cast<std::size_t>(frames);

    // A SLEEPING SOURCE IS NOT READ. `zeroed` says its whole buffer holds
    // +0.0f (set only by the suspension clear, dropped the moment it
    // processes), and this edge has nothing in flight, so reading it is 2 MB
    // of zeros per block for 63 sleeping tracks -- the residual left after
    // #60. This is the "active inputs" list, derived rather than kept: the
    // source ran before this consumer in topological order on this same
    // thread, so the flag is current, and no signalling, no second list and
    // no lock can be more right than reading it. `asleep` is that reading,
    // taken once by the caller (`idle`): the edge's half of it is the
    // message thread's to move.
    //
    // Bytes: `first` writes the zeros it would have copied, identical. A
    // later input's add of +0.0f is skipped, which differs from the sum in
    // exactly one case -- a -0.0f already in the mix stays -0.0f instead of
    // becoming +0.0f -- and testMixSkipsSleepingSources pins that so it is a
    // fact rather than a surprise. A source that is silent but NOT zeroed (a
    // generator emitting -0.0f, a node whose block was all zero but never
    // slept) is read as before.
    if (asleep) {
        // All-bits-zero is +0.0 in both widths.
        if (first)
            for (std::size_t c = 0; c < ch; ++c)
                std::memset(dst[c] + begin, 0, n * sizeof(Acc));
        delay.forget();   // its ring went unwritten
        ++r.stats.inputsSkipped;
        return;
    }

    // Zero compensation is the common case and must cost nothing: most edges in
    // most projects join paths of equal latency, and this is the path they take.
    //
    // A PENDING GLIDE DISQUALIFIES IT. An edge at zero that has been asked to
    // move to 128 is exactly the case this fast path would swallow: it would
    // memcpy, return, and leave the glide pending forever (ADR-0079).
    if (!delay.busy() && delay.delay() == 0) {
        // The ring is not written, so it no longer holds this edge's
        // history: a glide away from zero waits until it does (`forget`).
        delay.forget();
        for (std::size_t c = 0; c < ch; ++c) {
            const Src* s = from[c] + begin;
            Acc* d = dst[c] + begin;
            if (first) {
                if constexpr (std::is_same_v<Src, Acc>) {
                    std::memcpy(d, s, n * sizeof(Acc));
                } else {
                    for (std::size_t i = 0; i < n; ++i) d[i] = s[i];
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) d[i] += s[i];
            }
        }
        return;
    }

    // One write cursor is shared across the channels of an edge, so every
    // channel must start from the same position and only the last may leave it
    // advanced. Each channel owns its own history region, and the cursor is the
    // only thing that says where in that region it stopped.
    //
    // Getting this wrong does NOT pull the channels apart -- every channel
    // drifts by the same (channels - 1) * frames, so left still matches right
    // exactly, which is why a stereo comparison cannot see it. What it does is
    // make the compensation the wrong LENGTH on every block after the first:
    // the ring is self-consistent within a call, so a single-block test is also
    // blind to it. It takes two blocks and a delay that does not divide the
    // block size. See testPhaseAlignment.
    //
    // ONE SNAPSHOT for the whole edge, cursors and state together: the message
    // thread grows this line while it plays, and a round trip of cursors it
    // does not own here would overwrite an offer (`DelayLine::Edge`).
    const DelayLine::Edge edge = delay.beginEdge();
    for (std::size_t c = 0; c < ch; ++c) {
        const Src* s = from[c] + begin;
        Acc* d = dst[c] + begin;
        if constexpr (std::is_same_v<Src, Acc>) {
            if (first) {
                delay.process(edge, static_cast<std::int32_t>(c), s, d, frames);
                continue;
            }
        }
        // Through scratch, then summed. `process` WRITES its output; used
        // directly on a second input it would clobber the first rather than
        // add to it, and the first input would vanish silently. The delay
        // line holds the sources' width, which is exact: it moves samples,
        // it does not add them.
        Src* tmp = scratch<Src>(r);
        std::memcpy(tmp, s, n * sizeof(Src));
        delay.process(edge, static_cast<std::int32_t>(c), tmp, tmp, frames);
        if (first) {
            for (std::size_t i = 0; i < n; ++i) d[i] = tmp[i];
        } else {
            for (std::size_t i = 0; i < n; ++i) d[i] += tmp[i];
        }
    }
    // ONCE PER EDGE, after every channel -- the same rule as the cursor, and
    // for the same reason: the glide state is shared across the channels of
    // one edge. Ending it inside the loop would crossfade the left channel and
    // hard-switch the right.
    delay.endEdge(edge, src.silent);
}

bool Graph::mix(RenderState& r, const std::vector<NodeId>& inputs, std::vector<DelayLine>& delays,
                std::vector<float*>& dst, std::vector<double*>& wide,
                std::int32_t begin, std::int32_t frames) noexcept {
    if (inputs.empty()) return false;

    // ADR-0207 d2, Live's scheme: 32-bit processing, 64-bit summing at every
    // mix point. In float every partial sum past the first rounds to 24 bits,
    // so an input and its inverted copy arriving in different places in the
    // order leave a residue that is not in either; in double a sum of float
    // samples is exact for any count a project holds, and the one rounding
    // is the conversion at the end.
    //
    // ONE LIVE INPUT IS NOT A SUM. With at most one input able to add
    // anything, the others being asleep with nothing in flight, the result
    // is that input's samples, and the float path below produces exactly
    // the bytes the double path would (zeros add exactly in both widths, and
    // IEEE signs zeros the same in both). So the 63-sleeping-tracks project
    // ADR-0102 d5 measured keeps its cost, and nothing changes for a chain.
    //
    // EACH INPUT IS JUDGED ONCE. `idle` reads the edge's `busy()`, which the
    // message thread moves (an offer, a collection), so judging an input
    // twice can count it live and then find it idle: with every input so
    // found, the scan for the last one below ran off the front of the list.
    std::size_t live = 0;
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        const bool a = idle(slots_[static_cast<std::size_t>(inputs[k])], delays[k]);
        r.asleep_[k] = a ? 1 : 0;
        if (!a) ++live;
    }

    if (live < 2) {
        for (std::size_t k = 0; k < inputs.size(); ++k)
            accumulate<float, float>(r, dst, slots_[static_cast<std::size_t>(inputs[k])], delays[k],
                                     begin, frames, k == 0, r.asleep_[k] != 0);
        return true;
    }

    // In the consumer's fixed input order, as before: ADR-0056's
    // order-independence across a level is about WHICH node runs first,
    // never about the order a sum is taken in, and double does not change it.
    //
    // The LAST input that adds anything is added and converted in one pass,
    // straight into the float buffer the node reads: a separate conversion
    // pass over the accumulator was a fifth of a cheap project's callback.
    // Two live inputs mean it is never the first. Idle inputs after it add
    // nothing and are only counted as skipped.
    std::size_t last = inputs.size() - 1;
    while (r.asleep_[last] != 0) --last;   // two are live: it stops at one of them
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        const Slot& src = slots_[static_cast<std::size_t>(inputs[k])];
        if (k == last) accumulateLast(r, dst, wide, src, delays[k], begin, frames);
        else accumulate<float, double>(r, wide, src, delays[k], begin, frames, k == 0,
                                       r.asleep_[k] != 0);
    }
    ++r.stats.wideMixes;
    return true;
}

void Graph::accumulateLast(RenderState& r, std::vector<float*>& dst, const std::vector<double*>& wide,
                           const Slot& src, DelayLine& delay, std::int32_t begin,
                           std::int32_t frames) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);
    const auto n = static_cast<std::size_t>(frames);
    if (!delay.busy() && delay.delay() == 0) {
        delay.forget();   // as in `accumulate`
        for (std::size_t c = 0; c < ch; ++c) {
            const float* s = src.chanPtrs[c] + begin;
            const double* w = wide[c] + begin;
            float* d = dst[c] + begin;
            for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<float>(w[i] + s[i]);
        }
        return;
    }
    // Compensated: the same one snapshot per edge as `accumulate`.
    const DelayLine::Edge edge = delay.beginEdge();
    for (std::size_t c = 0; c < ch; ++c) {
        float* tmp = r.delayScratch_.data();
        std::memcpy(tmp, src.chanPtrs[c] + begin, n * sizeof(float));
        delay.process(edge, static_cast<std::int32_t>(c), tmp, tmp, frames);
        const double* w = wide[c] + begin;
        float* d = dst[c] + begin;
        for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<float>(w[i] + tmp[i]);
    }
    delay.endEdge(edge, src.silent);
}

bool Graph::mixWide(RenderState& r, const std::vector<NodeId>& inputs, std::vector<DelayLine>& delays,
                    std::vector<double*>& wide, std::int32_t begin, std::int32_t frames) noexcept {
    if (inputs.empty()) return false;
    // At 64 the sources are double already, so even one input is a copy
    // into the accumulator rather than a pass-through: the node reads its
    // input from there either way, and a copy of doubles is exact.
    std::size_t live = 0;
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        const Slot& src = slots_[static_cast<std::size_t>(inputs[k])];
        const bool asleep = idle(src, delays[k]);
        if (!asleep) ++live;
        accumulate<double, double>(r, wide, src, delays[k], begin, frames, k == 0, asleep);
    }
    if (live >= 2) ++r.stats.wideMixes;
    return true;
}

void Graph::acknowledgeController(const Event &event, bool delivered) noexcept {
    const auto prefix = static_cast<std::uint32_t>(event.noteId >> 32);
    if (!prefix)
        return;
    const auto found = std::lower_bound(controllerSources_.begin(), controllerSources_.end(), prefix,
                                        [](const auto &item, std::uint32_t key) { return item.first < key; });
    if (found == controllerSources_.end() || found->first != prefix)
        return;
    if (delivered)
        found->second->controllerDelivered(event);
    else
        found->second->controllerRejected(event);
}
bool Graph::hasPendingNoteOff(std::uint64_t noteId) const noexcept {
    for(std::size_t i=0;i<slots_.size();++i) {
        const auto& slot=slots_[i];
        if(!slot.deliversNoteStream)continue;
        for(std::int32_t pending=0;pending<slot.pendingCount;++pending) {
            const auto& event=slot.pending[static_cast<std::size_t>(pending)].e;
            if(event.type==EventType::NoteOff && event.noteId==noteId)return true;
        }
    }
    return false;
}
void Graph::forwardEvents(std::int32_t frames) noexcept {
    auto rejected = [&](const Event &e) {
        if (e.type == EventType::Controller)
            acknowledgeController(e, false);
        if (e.type == EventType::NoteOff)
            for (std::size_t i=0;i<slots_.size();++i)
                if (slots_[i].node)
                    slots_[i].node->noteOffRejected(e);
        if (e.type == EventType::NoteOn)
            for(std::size_t i=0;i<slots_.size();++i)
                if(slots_[i].node)slots_[i].node->noteOnRejected(e);
    };
    const std::int64_t end = now_ + frames;

    // TOPOLOGICAL ORDER, which is the whole of the correctness argument: when a
    // slot is reached, every slot feeding it has already received everything it
    // will receive this block, so what it forwards is complete.
    for (NodeId id : order_) {
        Slot& s = slots_[static_cast<std::size_t>(id)];

        // 1. Anything deferred INTO this slot on an earlier block that is due
        //    now. Compacted in place: no allocation, and order does not matter
        //    because the list is sorted by frame before anyone reads it.
        if (s.pendingCount > 0) {
            std::int32_t w = 0;
            for (std::int32_t i = 0; i < s.pendingCount; ++i) {
                const Slot::Pending& p = s.pending[static_cast<std::size_t>(i)];
                if(p.retainedSource && !p.retainedSource->retainedEventsStillValid(p.e)) {
                    rejected(p.e);continue;
                }
                if (p.due < end) {
                    Event e = p.e;
                    const std::int64_t f = p.due - now_;
                    e.frame = f > 0 ? static_cast<std::int32_t>(f) : 0;
                    const auto at=s.events.size();
                    if(s.events.push(e))s.eventOrigins[static_cast<std::size_t>(at)]=p.retainedSource;
                    else rejected(e); // overflow counted by list
                } else {
                    s.pending[static_cast<std::size_t>(w++)] = p;
                }
            }
            s.pendingCount = w;
        }

        // 2. What this slot's MAIN inputs forward to it.
        //
        // Main edges only. A sidechain is an audio KEY: a compressor keyed from
        // a kick track has no use for that track's notes, and handing them to
        // it would make every keyed plugin a second instrument. Notes from
        // another track are a note-input bus, which is a different feature and
        // is not designed yet.
        for (std::size_t k = 0; k < s.inputs.size(); ++k) {
            const Slot& u = slots_[static_cast<std::size_t>(s.inputs[k])];
            if (u.node == nullptr || u.events.empty()) continue;
            if (u.node->eventFlow() != EventFlow::Through) continue;   // Consume or Stop

            // THE SAME DELAY AS THE AUDIO BESIDE IT. The node's own latency,
            // because its output audio emerges that much later than its input;
            // plus this edge's compensation, because that is how far this input
            // is held back to meet the others. An event that skipped either
            // would reach an instrument ahead of the graph's own arithmetic --
            // ADR-0058 right for audio and wrong for everything a note drives.
            // The node's latency as last asked on the message thread, never
            // the node itself: this is the audio thread, and a CLAP plugin's
            // getter is main-thread only (linux's audit, C6; ADR-0123).
            const std::size_t ui = static_cast<std::size_t>(s.inputs[k]);
            const std::int64_t delay =
                static_cast<std::int64_t>(ui < slotLatency_.size()
                                              ? slotLatency_[ui].load(std::memory_order_relaxed)
                                              : 0) +
                (k < s.inDelays.size() ? s.inDelays[k].delay() : 0);

            for (const Event& e : u.events) {
                if (!isNoteStream(e.type)) continue;   // addressed: stays put
                const auto origin=u.eventOrigins[static_cast<std::size_t>(&e-u.events.begin())];
                const std::int64_t due = now_ + e.frame + delay;
                if (due < end) {
                    Event f = e;
                    f.frame = static_cast<std::int32_t>(due - now_);
                    const auto at=s.events.size();
                    if(s.events.push(f)) {
                        s.eventOrigins[static_cast<std::size_t>(at)]=origin;
                        ++stats_.eventsForwarded;
                    }
                    else rejected(f);
                } else if (s.pendingCount < static_cast<std::int32_t>(s.pending.size())) {
                    s.pending[static_cast<std::size_t>(s.pendingCount++)] =
                        Slot::Pending{due, e,origin};
                    ++stats_.eventsForwarded;
                    ++stats_.eventsDeferred;
                } else {
                    // Counted, never allocated for. A delay queue that grew
                    // would be the audio thread allocating on the one path a
                    // plugin latency change makes busiest.
                    ++stats_.eventsDropped;
                    rejected(e);
                }
            }
        }
    }
}

std::int32_t Graph::computeSplits(std::int32_t frames) noexcept {
    // MARK, THEN WALK.
    //
    // The loop this replaces coalesced WHILE it collected, comparing each event
    // with the last split pushed. That is only right if events arrive in time
    // order, and they arrive in SLOT order: a later slot's event at frame 100,
    // met after an earlier slot's event at frame 200, came out as -100 against
    // the floor and was dropped. A real, distinct frame with no boundary at it.
    //
    // A byte per frame makes the collection order irrelevant. It costs one
    // memset and one pass over the block -- 8192 byte tests at the largest
    // block ADR-0049 allows -- and it does not care how many slots hold the
    // same frame, which after forwarding is most of them.
    std::memset(frameMark_.data(), 0, static_cast<std::size_t>(frames));
    for (auto& s : slots_) {
        // Sorted here because `runNode` slices each segment's events as one
        // contiguous run, which is only true of a list in frame order.
        s.events.sortByFrame();
        for (const Event& e : s.events)
            if (e.frame > 0 && e.frame < frames)
                frameMark_[static_cast<std::size_t>(e.frame)] = 1;
    }

    std::int32_t nsplit = 0;
    splits_[static_cast<std::size_t>(nsplit++)] = 0;
    for (std::int32_t f = 1; f < frames; ++f) {
        if (frameMark_[static_cast<std::size_t>(f)] == 0) continue;
        const std::int32_t last = splits_[static_cast<std::size_t>(nsplit - 1)];
        if (f - last < floor_) continue;          // coalesce to the boundary
        splits_[static_cast<std::size_t>(nsplit++)] = f;
    }
    splits_[static_cast<std::size_t>(nsplit)] = frames;
    return nsplit;
}

void Graph::narrow(const std::vector<double*>& from, std::vector<float*>& to,
                   std::int32_t begin, std::int32_t frames) const noexcept {
    for (std::size_t c = 0; c < static_cast<std::size_t>(channels_); ++c) {
        const double* w = from[c] + begin;
        float* f = to[c] + begin;
        for (std::int32_t i = 0; i < frames; ++i) f[i] = static_cast<float>(w[i]);
    }
}

void Graph::runNode(RenderState& r, Slot& s, std::int32_t frames, std::int32_t nsplit,
                    const TransportInfo* transport) noexcept {
    const auto ch = static_cast<std::size_t>(channels_);
    const bool wide = precision_ == Precision::Float64;
    const bool bypass = s.hasBypass && s.node->bypassed();
    ScopeTap* const inputTap = s.node->inputTap();
    const bool advancing = transport != nullptr && transport->playing;
    const auto firstSample = transport != nullptr ? transport->timelineSample : 0;
    // With alignment off, scope input timestamps show the heard global time.
    // Physical maximum path latency remains cached for diagnostics.
    // The audio thread's copy: retapLatency rewrites s.arrival as it runs.
    const auto slotIndex = static_cast<std::size_t>(&s - slots_.data());
    const std::int32_t arrival =
        delayCompensation_ && slotIndex < slotArrival_.size() ? slotArrival_[slotIndex].load(std::memory_order_relaxed) : 0;

    bool mainSilent = true;
    for (NodeId in : s.inputs)
        if (!slots_[static_cast<std::size_t>(in)].silent) mainSilent = false;

    // A LIVE SIDECHAIN PREVENTS SUSPENSION, which ADR-0043 requires by name: a
    // compressor whose key input is playing is working, however quiet its main
    // input is, and suspending it would release the gain reduction.
    bool sideSilent = true;
    for (NodeId in : s.sidechains)
        if (!slots_[static_cast<std::size_t>(in)].silent) sideSilent = false;

    const bool inputSilent = mainSilent && sideSilent;

    // Events are NOT silence. A node with a pending event, or an instrument
    // with no audio input at all, must run -- this is the bug every
    // implementation of this feature ships once, because naive silence
    // detection suspends every synth in the project.
    const bool hasEvents = !s.events.empty();

    // A HELD NOTE IS NOT SILENCE EITHER (ADR-0158). An instrument holding a
    // chord gets no events between the note-on and the note-off, and a plugin
    // declaring no tail used to sleep on the next block: the chord stopped at
    // once, and came back for a moment when the off woke it. Held notes keep
    // it running and keep re-arming its tail, so the tail is counted from the
    // last release -- where a release tail actually starts.
    if (hasEvents) s.node->heldNotes().take(s.events, s.node->eventFlow() == EventFlow::Consume);
    const bool holding = s.node->heldNotes().any();

    // An infinite tail is a SEPARATE FLAG, not a sentinel in the counter.
    // Storing kInfiniteTail and decrementing it works -- INT64_MAX takes some
    // quadrillions of blocks to reach zero -- but it makes the never-suspend
    // rule impossible to test: planting a defect in the guard changes nothing
    // observable at any realistic duration. A decision that cannot be
    // falsified is one nobody can maintain, so it is a branch instead.
    //
    // ASKED ONCE A BLOCK. A device's answer can change between two calls:
    // during a reactivation it answers kInfiniteTail without asking its
    // plug-in (ADR-0089 d4). Asked twice, a finite answer then an infinite
    // one made `tail + reach` below INT64_MAX plus something: an overflow.
    const std::int64_t declaredTail = s.node->tailSamples();
    const std::int64_t tail = s.hasBypass
        ? (std::max)(declaredTail, static_cast<std::int64_t>(s.dryDelay.reach()))
        : declaredTail;
    const bool infinite = tail == kInfiniteTail;

    if (!inputSilent || hasEvents || holding) {
        // RE-ARMED WITH WHAT IS STILL IN FLIGHT, not only the node's own tail.
        //
        // A compensated input delays its audio on the way in (ADR-0058). When
        // every input goes silent, the rings still hold up to `reach` samples
        // that are due out over the next `reach` samples -- and a junction's
        // own tail is zero, so it used to sleep on the spot and those samples
        // were never heard. At 5120 samples of linear-phase compensation that
        // was the last 107 ms of a dry track, whenever it was the last thing
        // playing.
        std::int64_t reach = 0;
        for (const DelayLine& d : s.inDelays)
            if (d.reach() > reach) reach = d.reach();
        for (const DelayLine& d : s.sideDelays)
            if (d.reach() > reach) reach = d.reach();
        s.tailRemaining = infinite ? 0 : tail + reach;
    }

    // JUDGED AT THE START OF THE BLOCK, before any tail is spent.
    //
    // This used to decrement first and decide after, so the block in which a
    // tail's last samples belonged was itself skipped: a tail of one block or
    // less was never heard at all, and every longer one lost its final partial
    // block -- up to 85 ms at the 4096-frame blocks this engine is built for.
    // The old test accepted "3 to 6 blocks" for a tail that is exactly 4.
    bool suspend = !infinite && !s.node->alwaysProcess() &&
                   inputSilent && !hasEvents && !holding && s.tailRemaining == 0;
    if (suspend && s.hasBypass && s.dryDelay.due()) suspend = false;
    // AND NOTHING IS STILL DUE ON AN INPUT LINE (`DelayLine::due`). The tail
    // above is armed on the last loud block, with the reach of that moment.
    // A glide that reaches further, asked for during the drain, or a
    // handover in the middle of one, leaves audio in a ring that the drain
    // would not play and the next wake would, however much later. And a
    // glide asked for while the node sleeps is run now, in the silence, so
    // the wake is not where it happens.
    //
    // A node already asleep looks at its lines only when a retap has moved
    // since it last did: they cannot come due otherwise (`Slot::dozeGlides`),
    // and reading every sleeping node's lines every block was what a silent
    // project paid for this check.
    if (suspend) {
        const std::uint64_t glides = glides_.load(std::memory_order_acquire);
        if (s.dozeGlides != glides) {
            for (const DelayLine& d : s.inDelays)
                if (d.due()) suspend = false;
            for (const DelayLine& d : s.sideDelays)
                if (d.due()) suspend = false;
            if (suspend) s.dozeGlides = glides;
        }
    }
    if (suspend) {
        if (inputTap)
            inputTap->write(static_cast<const float*>(nullptr), nullptr, frames, firstSample - arrival, advancing);
        NodeIo silence;
        silence.frames = frames;
        silence.channels = channels_;
        silence.transport = transport;
        s.node->silenceTaps(silence);
        ++r.stats.nodesSuspended;
        s.silent = true;
        // ONCE, TO CAPACITY, not every block to `frames`. Consumers read this
        // buffer as zeros for as long as the node sleeps, so it has to BE zeros
        // -- all of it, because a later block may be longer than this one and
        // would read what an earlier loud block left beyond `frames`. Clearing
        // it every block was the whole cost of a sleeping node, and it scaled
        // with frames: 315 sleeping nodes cost 1.8 ms at 4096 (ADR-0102 d5).
        // Cleared once per silence and the flag does the rest; `process`
        // below clears the flag because the node has written.
        if (!s.zeroed) {
            const auto cap = static_cast<std::size_t>(maxFrames_);
            for (std::size_t c = 0; c < ch; ++c) {
                // At 64 the double buffer is what consumers read; a float
                // node's own buffer is cleared too, so it wakes onto zeros
                // exactly as it would at 32.
                if (wide) std::memset(s.widePtrs[c], 0, cap * sizeof(double));
                if (!s.doubles) std::memset(s.chanPtrs[c], 0, cap * sizeof(float));
            }
            s.zeroed = true;
            ++r.stats.suspendClears;
            // Its input lines are not run while it sleeps, so their rings
            // stop holding their edges' history -- all but the silence they
            // drained, which is still their newest past, because every input
            // stays silent while the node sleeps (`forgetAllButSilence`).
            for (DelayLine& d : s.inDelays) d.forgetAllButSilence();
            for (DelayLine& d : s.sideDelays) d.forgetAllButSilence();
            if (s.hasBypass) s.dryDelay.forgetAllButSilence();
        }
        return;
    }
    s.zeroed = false;   // the node is about to write; what it leaves is its own
    s.dozeGlides = kLinesUnchecked;

    // Only a block that actually runs on silence spends tail.
    if (inputSilent && !hasEvents && !holding && !infinite) {
        s.tailRemaining -= frames;
        if (s.tailRemaining < 0) s.tailRemaining = 0;
    }

    for (std::int32_t seg = 0; seg < nsplit; ++seg) {
        const std::int32_t begin = splits_[static_cast<std::size_t>(seg)];
        const std::int32_t end = splits_[static_cast<std::size_t>(seg + 1)];
        const std::int32_t n = end - begin;
        if (n <= 0) continue;

        // Summing here, for every node, is what makes a group (ADR-0044) a
        // node with no device rather than a special case in the scheduler.
        NodeIo nio;
        bool anyMain = false;
        if (!wide) {
            anyMain = mix(r, s.inputs, s.inDelays, r.mixPtrs_, r.mixWidePtrs_, begin, n);
            const bool anySide = mix(r, s.sidechains, s.sideDelays, r.sidePtrs_, r.sideWidePtrs_, begin, n);
            nio.in = anyMain ? r.mixPtrs_.data() : nullptr;
            nio.sidechain = anySide ? r.sidePtrs_.data() : nullptr;
            nio.out = s.chanPtrs.data();
        } else {
            // ADR-0207 d2 at 64: the sums stay double. A node that processes
            // double reads them there; any other is handed them narrowed,
            // once, and its float output is widened after it runs.
            anyMain = mixWide(r, s.inputs, s.inDelays, r.mixWidePtrs_, begin, n);
            const bool anySide = mixWide(r, s.sidechains, s.sideDelays, r.sideWidePtrs_, begin, n);
            if (s.doubles) {
                nio.in64 = anyMain ? r.mixWidePtrs_.data() : nullptr;
                nio.sidechain64 = anySide ? r.sideWidePtrs_.data() : nullptr;
                nio.out64 = s.widePtrs.data();
            } else {
                if (anyMain) narrow(r.mixWidePtrs_, r.mixPtrs_, begin, n);
                if (anySide) narrow(r.sideWidePtrs_, r.sidePtrs_, begin, n);
                nio.in = anyMain ? r.mixPtrs_.data() : nullptr;
                nio.sidechain = anySide ? r.sidePtrs_.data() : nullptr;
                nio.out = s.chanPtrs.data();
            }
        }
        nio.transport = transport;
        nio.channels = channels_;
        nio.frames = n;
        nio.blockOffset = begin;
        nio.inputSilent = mainSilent;
        nio.sidechainSilent = sideSilent;
        nio.sampleRate = sampleRate_;

        // The summed, compensated input belongs to this graph's arrival,
        // before the node's own latency. No device implementation is involved.
        if (inputTap && channels_ > 0) {
            const std::int64_t stamp = firstSample + (advancing ? begin : 0) - arrival;
            if (wide)
                inputTap->write(anyMain ? r.mixWidePtrs_[0] + begin : nullptr,
                                anyMain && channels_ > 1 ? r.mixWidePtrs_[1] + begin : nullptr,
                                n, stamp, advancing);
            else
                inputTap->write(anyMain ? r.mixPtrs_[0] + begin : nullptr,
                                anyMain && channels_ > 1 ? r.mixPtrs_[1] + begin : nullptr,
                                n, stamp, advancing);
        }

        const Event* first = nullptr;
        std::int32_t count = 0;
        for (const Event& e : s.events) {
            if (e.frame >= begin && e.frame < end) {
                if (first == nullptr) first = &e;
                ++count;
            }
        }
        nio.events = EventSpan{first, count};

        // Keep the dry line warm even while the device runs. Write at graph
        // precision: a bypassed float-only plug-in must not narrow the dry.
        if (s.hasBypass) {
            const bool immediate = s.dryDelay.delay() == 0 && !s.dryDelay.busy();
            if (immediate) s.dryDelay.forget();
            // Zero-latency active devices need neither a copy nor a ring
            // write. A later latency increase primes real history, just as
            // the graph's zero-compensation edges do (ADR-0079).
            if (!immediate || bypass) {
                const auto edge = s.dryDelay.beginEdge();
                bool drySilent = true;
                if (!anyMain) {
                    if (wide) std::fill_n(r.delayScratch64_.data(), n, 0.0);
                    else std::fill_n(r.delayScratch_.data(), n, 0.0f);
                }
                for (std::size_t c = 0; c < ch; ++c) {
                    if (wide) {
                        const double* src = anyMain ? r.mixWidePtrs_[c] + begin : r.delayScratch64_.data();
                        if (immediate) std::copy_n(src, n, s.widePtrs[c] + begin);
                        else {
                            for (std::int32_t k = 0; k < n; ++k)
                                if (src[k] != 0.0) drySilent = false;
                            s.dryDelay.process(edge, static_cast<std::int32_t>(c), src,
                                               s.widePtrs[c] + begin, n);
                        }
                    } else {
                        const float* src = anyMain ? r.mixPtrs_[c] + begin : r.delayScratch_.data();
                        if (immediate) std::copy_n(src, n, s.chanPtrs[c] + begin);
                        else {
                            for (std::int32_t k = 0; k < n; ++k)
                                if (src[k] != 0.0f) drySilent = false;
                            s.dryDelay.process(edge, static_cast<std::int32_t>(c), src,
                                               s.chanPtrs[c] + begin, n);
                        }
                    }
                }
                // Upstream nodes can be asleep while compensated input still
                // arrives. Only the actual mixed samples establish silence here.
                if (!immediate) s.dryDelay.endEdge(edge, drySilent);
            }
        }
        if (!bypass) {
            if (s.hasBypass) s.node->processActive(nio);
            else s.node->process(nio);
            ++r.stats.nodeCalls;
        }
        // A float node in a graph at 64: its segment, widened (exact).
        if (wide && !s.doubles && !bypass) {
            for (std::size_t c = 0; c < ch; ++c) {
                const float* f = s.chanPtrs[c] + begin;
                double* d = s.widePtrs[c] + begin;
                for (std::int32_t i = 0; i < n; ++i) d[i] = f[i];
            }
        }
    }

    // Is this node's output silent? Measured rather than declared: a node that
    // claims silence and writes samples is a bug the next node inherits, and
    // the check is one pass over a buffer just written and therefore in cache.
    bool allZero = true;
    for (std::size_t c = 0; c < ch && allZero; ++c) {
        if (wide) {
            for (std::int32_t i = 0; i < frames; ++i)
                if (s.widePtrs[c][i] != 0.0) { allZero = false; break; }
        } else {
            for (std::int32_t i = 0; i < frames; ++i)
                if (s.chanPtrs[c][i] != 0.0f) { allZero = false; break; }
        }
    }
    s.silent = allZero;
}

void Graph::runPartition(const Dispatch& job,std::size_t partition,RenderState& state) noexcept {
    const auto& ids=job.work->parallel;
    for(std::size_t position=partition;position<ids.size();position+=job.stride) {
        const auto id=ids[job.reverse?ids.size()-1-position:position];
        runNode(state,slots_[static_cast<std::size_t>(id)],job.frames,job.nsplit,job.transport);
        if(&state!=&render_)++state.stats.workerNodeBlocks;
    }
}
void Graph::executeWorker(void* raw,std::size_t index) noexcept {
    const auto& job=*static_cast<const Dispatch*>(raw);job.graph->runPartition(job,index+1,job.graph->workerRenders_[index]);
}
void Graph::runLevel(std::size_t index,std::int32_t frames,std::int32_t nsplit,const TransportInfo* transport) noexcept {
    const auto serial=[&]{const auto& level=levels_[index];
        if(reverseWithinLevel_)for(auto it=level.rbegin();it!=level.rend();++it)runNode(render_,slots_[static_cast<std::size_t>(*it)],frames,nsplit,transport);
        else for(const auto id:level)runNode(render_,slots_[static_cast<std::size_t>(id)],frames,nsplit,transport);
    };
    if(!workers_ || frames<parallelMinimumFrames_ || levelWork_[index].parallel.size()<parallelMinimumNodes_){serial();return;}
    const auto& work=levelWork_[index];std::size_t likelyActive=0;
    for(const auto id:work.parallel) {
        const auto& slot=slots_[static_cast<std::size_t>(id)];bool active=!slot.zeroed || !slot.events.empty() || slot.tailRemaining>0;
        for(std::size_t edge=0;!active && edge<slot.inputs.size();++edge)active=!slots_[static_cast<std::size_t>(slot.inputs[edge])].silent || slot.inDelays[edge].due() || slot.inDelays[edge].busy();
        for(std::size_t edge=0;!active && edge<slot.sidechains.size();++edge)active=!slots_[static_cast<std::size_t>(slot.sidechains[edge])].silent || slot.sideDelays[edge].due() || slot.sideDelays[edge].busy();
        if(active && ++likelyActive>=2)break;
    }
    // This only selects serial scheduling. Every node still runs its ordinary
    // suspension/tail/tap logic; no new wake/silence decision is invented.
    if(likelyActive<2){serial();return;}
    const auto count=(std::min)(workers_->size(),work.parallel.size()-1);
    Dispatch job{this,&work,frames,nsplit,transport,count+1,reverseWithinLevel_};
    ++stats_.parallelLevels;const auto posted=workers_->start(count,executeWorker,&job);
    runPartition(job,0,render_);
    if(reverseWithinLevel_)for(auto it=work.serial.rbegin();it!=work.serial.rend();++it)runNode(render_,slots_[static_cast<std::size_t>(*it)],frames,nsplit,transport);
    else for(const auto id:work.serial)runNode(render_,slots_[static_cast<std::size_t>(id)],frames,nsplit,transport);
    const auto completed=workers_->finish(posted);
    for(std::size_t worker=0;worker<count;++worker)if((completed&(std::uint32_t{1}<<worker))==0) {
        ++stats_.workerFallbacks;runPartition(job,worker+1,render_);
    }
}

void Graph::writeNodeOutput(NodeId id, const AudioIo& io) const noexcept {
    const Slot& outSlot = slots_[static_cast<std::size_t>(id)];
    const std::int32_t n = (std::min)(io.numOut, channels_);
    const bool wide = precision_ == Precision::Float64;
    const auto frames = io.frames;
    const auto bytes = static_cast<std::size_t>(frames);
    for (std::int32_t c = 0; c < n; ++c) {
        const auto k = static_cast<std::size_t>(c);
        // A device is float: a graph at 64 is narrowed once, here, at the
        // last moment (ADR-0207 d2). `out64` takes it whole.
        if (float* dst = io.out != nullptr ? io.out[c] : nullptr) {
            if (wide) {
                const double* w = outSlot.widePtrs[k];
                for (std::int32_t i = 0; i < frames; ++i) dst[i] = static_cast<float>(w[i]);
            } else {
                std::memcpy(dst, outSlot.chanPtrs[k], bytes * sizeof(float));
            }
        }
        if (double* wdst = io.out64 != nullptr ? io.out64[c] : nullptr) {
            if (wide) {
                std::memcpy(wdst, outSlot.widePtrs[k], bytes * sizeof(double));
            } else {
                const float* f = outSlot.chanPtrs[k];
                for (std::int32_t i = 0; i < frames; ++i) wdst[i] = f[i];
            }
        }
    }
}

bool Graph::copyNodeOutput(NodeId id, const AudioIo& io) const noexcept {
    for (std::int32_t c = 0; c < io.numOut; ++c) {
        for (std::int32_t i = 0; i < io.frames; ++i) {
            if (io.out && io.out[c]) io.out[c][i] = 0.0f;
            if (io.out64 && io.out64[c]) io.out64[c][i] = 0.0;
        }
    }
    if (!ok_ || !prepared_ || id < 0 || static_cast<std::size_t>(id) >= slots_.size() ||
        renderedFrames_ <= 0 || io.frames != renderedFrames_ || io.numOut < 0)
        return false;
    writeNodeOutput(id, io);
    return true;
}

std::optional<std::int64_t> Graph::outputLatencyOf(NodeId id) const noexcept {
    if (!ok_ || !prepared_ || id < 0 || static_cast<std::size_t>(id) >= slotLatency_.size())
        return std::nullopt;
    const auto i = static_cast<std::size_t>(id);
    return static_cast<std::int64_t>(slotArrival_[i].load(std::memory_order_relaxed)) +
           static_cast<std::int64_t>(slotLatency_[i].load(std::memory_order_relaxed));
}

void Graph::process(const AudioIo& io) noexcept {
    renderedFrames_ = 0;
    // Silence first and unconditionally, before any early return. A callback
    // that returns without writing hands the driver whatever was in that
    // memory, and on a first callback that is uninitialised.
    for (std::int32_t c = 0; c < io.numOut; ++c) {
        float* dst = io.out != nullptr ? io.out[c] : nullptr;
        if (dst != nullptr)
            for (std::int32_t i = 0; i < io.frames; ++i) dst[i] = 0.0f;
        double* wdst = io.out64 != nullptr ? io.out64[c] : nullptr;
        if (wdst != nullptr)
            for (std::int32_t i = 0; i < io.frames; ++i) wdst[i] = 0.0;
    }

    if (!ok_ || !prepared_) return;
    if (io.frames <= 0 || io.frames > maxFrames_) return;

    ++stats_.blocks;
    render_.stats=GraphStats{};
    for(auto& state:workerRenders_)state.stats=GraphStats{};
    const std::int32_t frames = io.frames;

    // --- ADR-0091: events travel along edges, BEFORE the splits ------------
    //
    // A note pushed at the head of a chain has to reach the instrument at the
    // tail. It is forwarded now, up front, rather than as each node runs:
    // forwarding during the run would hand a node an event at a frame the
    // splits were chosen without, and ADR-0042's promise that a value lands on
    // its own segment boundary would hold for pushed events and quietly fail
    // for forwarded ones.
    for (auto& s : slots_) {
        if (s.node == nullptr) continue;
        s.node->sourceEvents(s.events, frames);
        // ADR-0165: automation addressed to this node, beside what it sources.
        if (s.addressedSource) s.addressedSource->emit(s.events, frames);
        if(!s.events.empty())std::fill_n(s.eventOrigins.data(),s.events.size(),nullptr);
    }
    forwardEvents(frames);

    // --- segment boundaries (ADR-0042) -------------------------------------
    //
    // Split at every DISTINCT event frame, not at every event. A 500 Hz MPE+
    // frame carrying ten notes across three dimensions is one instant, not
    // thirty, so the bound is 500 splits per second rather than 15,000 --
    // which is what makes ADR-0054's requirement affordable at all.
    const std::int32_t nsplit = computeSplits(frames);

    stats_.segments += nsplit;

    // --- ADR-0043: decide suspension once per block, not per segment -------
    //
    // Per block because the tail counter is in samples and a decision that
    // changed mid-block would let a node run for part of its own tail. The
    // cost is that a node wakes at a block boundary rather than at the exact
    // sample its input returns, which is inaudible and much easier to reason
    // about.
    for(std::size_t level=0;level<levels_.size();++level)runLevel(level,frames,nsplit,io.transport);

    collectRenderStats(render_);
    for(const auto& state:workerRenders_)collectRenderStats(state);

    // --- out ---------------------------------------------------------------
    writeNodeOutput(output_, io);

    // A source off may have spent several blocks in a compensation queue.
    // Acknowledge only after it actually reaches a note consumer or output.
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& delivered = slots_[i];
        if (!delivered.node || (static_cast<NodeId>(i) != output_ &&
            delivered.node->eventFlow() != EventFlow::Consume)) continue;
        for (const auto &e : delivered.events) {
            if (e.type == EventType::Controller)
                acknowledgeController(e, true);
            if (e.type == EventType::NoteOff) {
                // Forwarding through every hop and worker processing have
                // finished. Even a short note's future on/off pair is now in
                // these queues; its first consumer cannot retire the owner.
                const bool waiting=hasPendingNoteOff(e.noteId);
                for (std::size_t source=0;source<slots_.size();++source)
                    if (slots_[source].node &&
                        (!waiting || !slots_[source].node->retainsQueuedSourceEvents()))
                        slots_[source].node->noteOffDelivered(e);
            }
            if(e.type==EventType::NoteOn)
                for(std::size_t source=0;source<slots_.size();++source)
                    if(slots_[source].node)slots_[source].node->noteOnDelivered(e);
        }
    }
    for (auto& s : slots_) {
        stats_.eventsDropped += s.events.dropped();
        s.events.resetDropped();
        s.events.clear();
    }

    // Last, after every use of this block's times. Deferred events are keyed
    // to it, so advancing it early would make everything due one block late.
    now_ += frames;
    renderedFrames_ = frames;
}

// ---------------------------------------------------------------------------
// Built-in nodes
// ---------------------------------------------------------------------------

void SumNode::process(const NodeIo& io) noexcept {
    if (io.wide()) {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            double* out = io.out64[c] + io.blockOffset;
            const auto n = static_cast<std::size_t>(io.frames);
            if (io.in64 == nullptr || io.in64[c] == nullptr) std::memset(out, 0, n * sizeof(double));
            else std::memcpy(out, io.in64[c] + io.blockOffset, n * sizeof(double));
        }
        return;
    }
    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c];
        if (io.in == nullptr || io.in[c] == nullptr) {
            std::memset(out + io.blockOffset, 0,
                        static_cast<std::size_t>(io.frames) * sizeof(float));
            continue;
        }
        std::memcpy(out + io.blockOffset, io.in[c] + io.blockOffset,
                    static_cast<std::size_t>(io.frames) * sizeof(float));
    }
}

void GainNode::process(const NodeIo& io) noexcept {
    // The value is taken at the START of the segment and held across it. That
    // is exactly what sub-block splitting buys: the segment is short because
    // the scheduler cut it at the event, so "held across the segment" is a
    // ramp at segment resolution rather than a step at block resolution.
    for (const Event& e : io.events)
        if (e.type == EventType::ParamValue && e.paramId == paramId_) {
            gain_ = static_cast<float>(e.value);
            gain64_ = e.value;
        }

    if (io.wide()) {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            double* out = io.out64[c] + io.blockOffset;
            const double* in = (io.in64 != nullptr && io.in64[c] != nullptr)
                                   ? io.in64[c] + io.blockOffset : nullptr;
            for (std::int32_t i = 0; i < io.frames; ++i) out[i] = in != nullptr ? in[i] * gain64_ : 0.0;
        }
        return;
    }

    for (std::int32_t c = 0; c < io.channels; ++c) {
        float* out = io.out[c] + io.blockOffset;
        const float* in = (io.in != nullptr && io.in[c] != nullptr)
                              ? io.in[c] + io.blockOffset : nullptr;
        if (in == nullptr) {
            std::memset(out, 0, static_cast<std::size_t>(io.frames) * sizeof(float));
            continue;
        }
        for (std::int32_t i = 0; i < io.frames; ++i) out[i] = in[i] * gain_;
    }
}

}  // namespace adi::engine
