// SPDX-License-Identifier: GPL-3.0-or-later
//
// See pd_engine.hpp for why this file is shaped the way it is.

#include "juce/pd_engine.hpp"

#include "adi/engine/graph.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

extern "C" {
#include "z_libpd.h"
}

namespace adi::device {
namespace {

std::mutex& initMutex() {
    static std::mutex m;
    return m;
}
bool g_initialised = false;

/// instance -> table. Fixed size and lock-free on the read path, because the
/// read happens on the audio thread inside `libpd_process_float`. Entries are
/// published with release and scanned with acquire, the same discipline
/// PdReceiverTable itself uses.
constexpr int kMaxInstances = 256;
struct InstanceSlot {
    std::atomic<void*> instance{nullptr};
    std::atomic<PdReceiverTable*> table{nullptr};
};
InstanceSlot g_slots[kMaxInstances];

/// libpd's float hook. **Audio thread** -- libpd calls it from inside
/// `libpd_process_float`. It does nothing but route, because the contract's
/// receiver is what validates (ADR-0095).
///
/// The hook takes no user data, so it asks libpd which instance is current.
/// That is the only context available, and it is the right one: `$0` is unique
/// within an instance and nowhere wider.
void floatHook(const char* recv, float value) {
    void* const self = static_cast<void*>(libpd_this_instance());
    for (int i = 0; i < kMaxInstances; ++i) {
        if (g_slots[i].instance.load(std::memory_order_acquire) != self) continue;
        PdReceiverTable* const t = g_slots[i].table.load(std::memory_order_acquire);
        if (t != nullptr) t->dispatchFloat(recv, value);
        return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PdRuntime
// ---------------------------------------------------------------------------

bool PdRuntime::initialise(std::string& error) {
    const std::lock_guard<std::mutex> lock(initMutex());
    if (g_initialised) return true;

    // libpd_init() returns -1 when it has already run. That is not a failure
    // and must not be reported as one: a second device opening is the normal
    // case, and this path is reached once per process only because of the flag
    // above -- the check is belt and braces against a caller that bypassed it.
    const int rc = libpd_init();
    if (rc != 0 && rc != -1) {
        error = "libpd_init() failed";
        return false;
    }
    g_initialised = true;
    return true;
}

bool PdRuntime::initialised() noexcept {
    const std::lock_guard<std::mutex> lock(initMutex());
    return g_initialised;
}

bool PdRuntime::registerInstance(void* instance, PdReceiverTable& t) noexcept {
    if (instance == nullptr) return false;
    for (int i = 0; i < kMaxInstances; ++i) {
        void* expected = nullptr;
        // The table is published BEFORE the instance that makes the slot
        // findable, so a hook that sees the instance always sees the table.
        if (g_slots[i].instance.load(std::memory_order_acquire) != nullptr) continue;
        g_slots[i].table.store(&t, std::memory_order_release);
        if (g_slots[i].instance.compare_exchange_strong(
                expected, instance, std::memory_order_acq_rel)) {
            return true;
        }
        g_slots[i].table.store(nullptr, std::memory_order_release);
    }
    return false;
}

void PdRuntime::forgetInstance(void* instance) noexcept {
    if (instance == nullptr) return;
    for (int i = 0; i < kMaxInstances; ++i) {
        if (g_slots[i].instance.load(std::memory_order_acquire) != instance) continue;
        // Instance first: once it is gone no hook can reach the table, so the
        // table pointer can then be cleared without racing a dispatch.
        g_slots[i].instance.store(nullptr, std::memory_order_release);
        g_slots[i].table.store(nullptr, std::memory_order_release);
        return;
    }
}

std::int32_t PdRuntime::blockSize() noexcept {
    return static_cast<std::int32_t>(libpd_blocksize());
}

// ---------------------------------------------------------------------------
// LibPdEngine
// ---------------------------------------------------------------------------

LibPdEngine::LibPdEngine(std::string patchDir, std::string patchFile,
                         std::int32_t inChannels, std::int32_t outChannels)
    : dir_(std::move(patchDir)), file_(std::move(patchFile)),
      inCh_(inChannels > 0 ? inChannels : 1),
      outCh_(outChannels > 0 ? outChannels : 1) {}

LibPdEngine::~LibPdEngine() { close(); }

void LibPdEngine::selectInstance() const noexcept {
    if (instance_ != nullptr) libpd_set_instance(static_cast<t_pdinstance*>(instance_));
}

bool LibPdEngine::open(PdLatencyReceiver& latency, std::string& error) {
    if (!PdRuntime::initialise(error)) return false;
    if (patch_ != nullptr) return true;

    // The instance is created and SELECTED before anything else. Every libpd
    // call below acts on "the current instance", so an unselected instance
    // would open this patch inside whichever device ran last.
    if (instance_ == nullptr) {
        instance_ = libpd_new_instance();
        if (instance_ == nullptr) {
            error = "libpd_new_instance() returned null (is PD_MULTI on?)";
            return false;
        }
    }
    selectInstance();
    if (!PdRuntime::registerInstance(instance_, receivers_)) {
        error = "more than 256 Pd instances";
        return false;
    }

    // The float hook is per instance under PDINSTANCE (z_hooks.h), so it is set
    // on each one rather than once for the process.
    libpd_set_floathook(&floatHook);

    block_ = PdRuntime::blockSize();
    if (block_ <= 0) block_ = 64;

    // Audio is initialised BEFORE the patch opens. libpd fixes the instance's
    // channel count here, and a patch opened first would bind its inlet~ to
    // whatever the count was then.
    if (libpd_init_audio(inCh_, outCh_, 44100) != 0) {
        error = "libpd_init_audio() failed";
        return false;
    }

    patch_ = libpd_openfile(file_.c_str(), dir_.c_str());
    if (patch_ == nullptr) {
        error = "libpd_openfile() could not open " + dir_ + "/" + file_;
        return false;
    }
    dollarZero_ = libpd_getdollarzero(patch_);

    // THE [loadbang] REPORT CANNOT BE CAUGHT, and that is structural rather
    // than an ordering mistake to fix. The receive name is built from `$0`,
    // `$0` is only knowable from the opened patch, and `[loadbang]` fires
    // INSIDE `libpd_openfile` -- so by the time a host can bind, the early
    // report has already been sent to nobody. ADR-0095 decision 2 treats the
    // query after `prepare` as a correction for a value sent at the wrong
    // sample rate; measured here, it is not a correction but the ONLY report a
    // host ever receives. Which makes the query load-bearing rather than
    // belt-and-braces: without it a patch's latency is never learned at all.
    if (!receivers_.bind(dollarZero_, latency)) {
        error = "receiver table full, or $0 " + std::to_string(dollarZero_) +
                " already bound";
        libpd_closefile(patch_);
        patch_ = nullptr;
        return false;
    }
    libpd_bind(PdLatencyReceiver::nameFor(dollarZero_).c_str());
    return true;
}

void LibPdEngine::close() noexcept {
    if (instance_ == nullptr) return;
    selectInstance();
    if (patch_ != nullptr) {
        if (dspOn_) {
            libpd_start_message(1);
            libpd_add_float(0);
            libpd_finish_message("pd", "dsp");
            dspOn_ = false;
        }
        receivers_.unbind(dollarZero_);
        libpd_closefile(patch_);
        patch_ = nullptr;
        dollarZero_ = 0;
    }
    PdRuntime::forgetInstance(instance_);
    libpd_free_instance(static_cast<t_pdinstance*>(instance_));
    instance_ = nullptr;
}

void LibPdEngine::prepare(double sampleRate, std::int32_t maxFrames) {
    if (patch_ == nullptr) return;
    selectInstance();

    const int rate = static_cast<int>(sampleRate > 0 ? sampleRate : 44100);
    libpd_init_audio(inCh_, outCh_, rate);

    // DSP on. Pd renders nothing at all without this, and a patch that opens,
    // prepares and outputs silence with no error anywhere is the first thing
    // anyone embedding libpd gets wrong.
    libpd_start_message(1);
    libpd_add_float(1);
    libpd_finish_message("pd", "dsp");
    dspOn_ = true;

    // The adapter. One tick of scratch, plus room for the largest segment the
    // graph promised, so a push never has to reject what it was handed.
    const std::int32_t cap = block_ + std::max<std::int32_t>(maxFrames, block_);
    in_.assign(static_cast<std::size_t>(cap) * static_cast<std::size_t>(inCh_), 0.f);
    out_.assign(static_cast<std::size_t>(cap) * static_cast<std::size_t>(outCh_), 0.f);
    tickIn_.assign(static_cast<std::size_t>(block_) * static_cast<std::size_t>(inCh_), 0.f);
    tickOut_.assign(static_cast<std::size_t>(block_) * static_cast<std::size_t>(outCh_), 0.f);

    // Prime the output with one block of silence. This is what makes the delay
    // CONSTANT: every segment can then pop exactly as many frames as it pushed,
    // whatever its length, and the engine never has to hand back a short read.
    // The price is one Pd block of latency, reported through
    // adapterLatencySamples() so the graph compensates it.
    inFill_ = 0;
    outFill_ = block_;
    adapterLatency_ = block_;
}

void LibPdEngine::requestLatencyReport() {
    if (patch_ == nullptr) return;
    selectInstance();
    libpd_bang(PdLatencyReceiver::queryFor(dollarZero_).c_str());
}

void LibPdEngine::process(const engine::NodeIo& io) noexcept {
    const std::int32_t n = io.frames;
    if (n <= 0) return;

    // Not open, or not prepared: pass through rather than silence. A device
    // that failed to load must not remove the signal (pd_device.hpp), and a
    // process() before prepare() is a bug elsewhere that silence would hide.
    if (patch_ == nullptr || in_.empty()) {
        if (io.in != nullptr && io.out != nullptr) {
            for (std::int32_t c = 0; c < io.channels; ++c) {
                if (io.in[c] != nullptr && io.out[c] != nullptr && io.in[c] != io.out[c]) {
                    std::memcpy(io.out[c] + io.blockOffset, io.in[c] + io.blockOffset,
                                static_cast<std::size_t>(n) * sizeof(float));
                }
            }
        }
        return;
    }

    selectInstance();
    segments_.fetch_add(1, std::memory_order_relaxed);

    // --- push: planar segment -> interleaved pending input -------------------
    const std::int32_t cap = static_cast<std::int32_t>(in_.size()) / inCh_;
    const std::int32_t push = std::min(n, cap - inFill_);
    for (std::int32_t f = 0; f < push; ++f) {
        float* dst = &in_[static_cast<std::size_t>(inFill_ + f) * static_cast<std::size_t>(inCh_)];
        for (std::int32_t c = 0; c < inCh_; ++c) {
            // The patch's first `io.channels` inlets are the main input; any
            // beyond that are the sidechain bus, which is how a patch with
            // three inlet~ gets two of signal and one of key (rmsc).
            const float* src = nullptr;
            if (c < io.channels && io.in != nullptr) src = io.in[c];
            else if (io.sidechain != nullptr && (c - io.channels) < io.channels)
                src = io.sidechain[c - io.channels];
            dst[c] = src != nullptr ? src[io.blockOffset + f] : 0.f;
        }
    }
    inFill_ += push;

    // --- render whole Pd blocks ---------------------------------------------
    while (inFill_ >= block_) {
        std::memcpy(tickIn_.data(), in_.data(), tickIn_.size() * sizeof(float));
        libpd_process_float(1, tickIn_.data(), tickOut_.data());
        ticks_.fetch_add(1, std::memory_order_relaxed);

        inFill_ -= block_;
        if (inFill_ > 0) {
            std::memmove(in_.data(),
                         in_.data() + static_cast<std::size_t>(block_) * static_cast<std::size_t>(inCh_),
                         static_cast<std::size_t>(inFill_) * static_cast<std::size_t>(inCh_) * sizeof(float));
        }
        const std::int32_t outCap = static_cast<std::int32_t>(out_.size()) / outCh_;
        const std::int32_t room = std::min(block_, outCap - outFill_);
        if (room > 0) {
            std::memcpy(&out_[static_cast<std::size_t>(outFill_) * static_cast<std::size_t>(outCh_)],
                        tickOut_.data(),
                        static_cast<std::size_t>(room) * static_cast<std::size_t>(outCh_) * sizeof(float));
            outFill_ += room;
        }
    }

    // --- pop: interleaved rendered output -> planar segment -------------------
    const std::int32_t pop = std::min(n, outFill_);
    if (pop < n) starved_.fetch_add(1, std::memory_order_relaxed);
    if (io.out != nullptr) {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* dst = io.out[c];
            if (dst == nullptr) continue;
            for (std::int32_t f = 0; f < pop; ++f) {
                dst[io.blockOffset + f] =
                    c < outCh_
                        ? out_[static_cast<std::size_t>(f) * static_cast<std::size_t>(outCh_) +
                               static_cast<std::size_t>(c)]
                        : 0.f;
            }
            // A starved pop writes silence rather than stale frames: repeating
            // the last block is audible as a stutter, silence as a gap, and a
            // gap is the one the counter above makes findable.
            for (std::int32_t f = pop; f < n; ++f) dst[io.blockOffset + f] = 0.f;
        }
    }
    outFill_ -= pop;
    if (outFill_ > 0) {
        std::memmove(out_.data(),
                     out_.data() + static_cast<std::size_t>(pop) * static_cast<std::size_t>(outCh_),
                     static_cast<std::size_t>(outFill_) * static_cast<std::size_t>(outCh_) * sizeof(float));
    }
}

bool LibPdEngine::sendFloat(const char* suffix, float value) noexcept {
    if (patch_ == nullptr || suffix == nullptr) return false;
    selectInstance();
    const std::string recv = std::to_string(dollarZero_) + "-" + suffix;
    return libpd_float(recv.c_str(), value) == 0;
}

LibPdEngine::Counters LibPdEngine::counters() const noexcept {
    return {ticks_.load(std::memory_order_relaxed),
            segments_.load(std::memory_order_relaxed),
            starved_.load(std::memory_order_relaxed)};
}

}  // namespace adi::device
