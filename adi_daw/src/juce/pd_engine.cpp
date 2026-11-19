// SPDX-License-Identifier: GPL-3.0-or-later
//
// See pd_engine.hpp for why this file is shaped the way it is.

#include "juce/pd_engine.hpp"

#include "juce/pd_builtins.hpp"

#include "adi/engine/graph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>   // std::lround: libc++ pulls it in transitively, libstdc++ does not
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>

extern "C" {
#include "z_libpd.h"
// The inmidi_* entry points, which are what libpd_noteon and friends call
// once they have taken sys_lock(). See deliverMidi for why we call them
// directly.
#include "s_stuff.h"
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
    /// Who prints for this instance (DEV-09). Read by the print hook on any
    /// thread without a lock; set after the slot is registered, cleared before
    /// it is forgotten.
    std::atomic<LibPdEngine*> console{nullptr};
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


/// instance -> engine for the print hook, in the same lock-free table as the
/// float hook (DEV-09). It was a mutex-guarded vector, taken on whatever thread
/// Pd printed on -- the audio thread included, for a [print] in a running patch.
void setConsoleOwner(void* instance, LibPdEngine* engine) noexcept {
    for (int i = 0; i < kMaxInstances; ++i)
        if (g_slots[i].instance.load(std::memory_order_acquire) == instance) {
            g_slots[i].console.store(engine, std::memory_order_release);
            return;
        }
}
LibPdEngine* consoleOwner(void* instance) noexcept {
    for (int i = 0; i < kMaxInstances; ++i)
        if (g_slots[i].instance.load(std::memory_order_acquire) == instance)
            return g_slots[i].console.load(std::memory_order_acquire);
    return nullptr;
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

    // ADR-0188 d8: ADI's externals are compiled in and registered as built-ins.
    // HERE, for determinism rather than for reachability -- Pd reaches every
    // instance whenever a class is registered, because `class_doaddmethod`
    // loops over all of them and `pdinstance_new` copies instance 0's list.
    // What registering here buys is that every device opens against the same,
    // complete vocabulary. See pd_builtins.hpp.
    PdBuiltins::registerAll();

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
        g_slots[i].console.store(nullptr, std::memory_order_release);
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
                         std::int32_t inChannels, std::int32_t outChannels,
                         Execution execution, BlockMode blockMode)
    : dir_(std::move(patchDir)), file_(std::move(patchFile)),
      inCh_(inChannels > 0 ? inChannels : 1),
      outCh_(outChannels > 0 ? outChannels : 1), execution_(execution), blockMode_(blockMode),
      printSlots_(std::make_unique<PrintSlot[]>(kPrintSlots)) {
    // Vyukov's bounded queue: slot i starts at sequence i.
    for (std::size_t i = 0; i < kPrintSlots; ++i)
        printSlots_[i].sequence.store(static_cast<std::uint64_t>(i), std::memory_order_relaxed);
}

LibPdEngine::InstanceLock::InstanceLock(LibPdEngine& engine) noexcept
    : instance_(engine.instance_) {
    if (instance_ == nullptr) return;
    libpd_set_instance(static_cast<t_pdinstance*>(instance_));
    sys_lock();
}
LibPdEngine::InstanceLock::~InstanceLock() {
    if (instance_ == nullptr) return;
    // pd_this is per thread: select again, in case this thread used another
    // instance while the lock was held.
    libpd_set_instance(static_cast<t_pdinstance*>(instance_));
    sys_unlock();
}

LibPdEngine::~LibPdEngine() { close(); }

void LibPdEngine::selectInstance() const noexcept {
    if (instance_ != nullptr) libpd_set_instance(static_cast<t_pdinstance*>(instance_));
}

void LibPdEngine::addSearchPath(const std::string& dir) {
    // REMEMBERED, not applied. Under PDINSTANCE the search path is per-instance
    // state, not process-wide, so a path added before this engine's instance
    // exists lands on whichever instance happened to be current -- or on none.
    // The only sign is the abstraction failing to create, which Pd reports by
    // printing the object's text and nothing else.
    searchPaths_.push_back(dir);
}

bool LibPdEngine::open(PdLatencyReceiver& latency, std::string& error) {
    if (patch_ != nullptr || native_ != nullptr) return true;

    // ADR-0188 d8, BEFORE anything is opened. Pd resolves a compiled external
    // through the same paths it resolves an abstraction through, so the only
    // portable guarantee is that no such file is on any of them (see note 4 in
    // the header). The patch's own directory is checked first because it is
    // the one the DAW does not choose -- it is wherever the project's patch
    // happens to live, and a `.pd_darwin` dropped beside it is the whole
    // attack.
    for (const std::string& dir : { dir_ }) {
        const auto found = pdLoadableCodeIn(dir);
        if (!found.empty()) {
            error = "refusing to open a patch from a directory that holds loadable "
                    "code (ADR-0188 d8): " + dir + "/" + found.front();
            return false;
        }
    }
    for (const std::string& dir : searchPaths_) {
        const auto found = pdLoadableCodeIn(dir);
        if (!found.empty()) {
            error = "refusing a search path that holds loadable code "
                    "(ADR-0188 d8): " + dir + "/" + found.front();
            return false;
        }
    }

    // `[declare -lib ...]` loads a binary by name and `[declare -path ...]`
    // adds a directory this engine never checked. Both are in the patch TEXT,
    // so both are found by the same static parse the declarations use, with Pd
    // not running -- which is the only moment at which refusing still costs
    // nothing.
    std::string patchText;
    {
        std::ifstream in(dir_ + "/" + file_, std::ios::binary);
        if (in) {
            std::ostringstream buf;
            buf << in.rdbuf();
            patchText = buf.str();
            const auto requests = pdExternalRequests(patchText);
            if (!requests.empty()) {
                error = "refusing a patch that declares " + requests.front().flag +
                        " (ADR-0188 d8): box " + std::to_string(requests.front().box);
                if (!requests.front().value.empty()) error += ", " + requests.front().value;
                return false;
            }
        }
    }

    // A filename is not an identity: the project may have edited this canvas
    // or any of its helper abstractions. Only the exact shipped program can
    // take the native path. Resolve helpers in the same path order as Pd.
    if (execution_ == Execution::Automatic) {
        auto candidate = makeNativePdDevice(patchText, inCh_, outCh_);
        bool helpersMatch = candidate != nullptr;
        for (const char* helper : {"adi.param", "adi.transport", "adi.sample"}) {
            if (!helpersMatch || patchText.find(std::string(helper) + " ") == std::string::npos)
                continue;
            const std::string filename = std::string(helper) + ".pd";
            auto matches = [&](const std::string& directory) {
                std::ifstream input(directory + "/" + filename, std::ios::binary);
                if (!input) return 0;
                std::ostringstream contents;
                contents << input.rdbuf();
                return nativePdAbstractionMatches(filename, contents.str()) ? 1 : -1;
            };
            int match = matches(dir_);
            for (const auto& path : searchPaths_) {
                if (match != 0) break;
                match = matches(path);
            }
            helpersMatch = match == 1;
        }
        if (helpersMatch) {
            native_ = std::move(candidate);
            nativeLatency_ = &latency;
            nativeLatency_->report(native_->latencySamples());
            return true;
        }
    }

    if (!PdRuntime::initialise(error)) return false;

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
    // on each one rather than once for the process. The print hook is too, and
    // it is set here rather than in PdRuntime for the same reason.
    libpd_set_floathook(&floatHook);
    setConsoleOwner(instance_, this);
    libpd_set_printhook(&LibPdEngine::printHookThunk);

    block_ = PdRuntime::blockSize();
    if (block_ <= 0) block_ = 64;

    // Audio is initialised BEFORE the patch opens. libpd fixes the instance's
    // channel count here, and a patch opened first would bind its inlet~ to
    // whatever the count was then.
    if (libpd_init_audio(inCh_, outCh_, 44100) != 0) {
        error = "libpd_init_audio() failed";
        return false;
    }

    // Search paths, now that this instance exists and is selected: its own
    // directory, then anything the caller asked for -- where the shipped
    // abstractions live.
    libpd_add_to_search_path(dir_.c_str());
    for (const auto& p : searchPaths_) libpd_add_to_search_path(p.c_str());

    patch_ = libpd_openfile(file_.c_str(), dir_.c_str());
    if (patch_ == nullptr) {
        error = "libpd_openfile() could not open " + dir_ + "/" + file_;
        return false;
    }
    dollarZero_ = libpd_getdollarzero(patch_);
    // Resolved once, here, for `sendParameter`'s reason: `gensym` on the audio
    // thread allocates for a name Pd has not seen, and `pd_list` by name would
    // do exactly that every block.
    transportSymbol_ = static_cast<void*>(
        gensym(pdTransportReceiveName(dollarZero_).c_str()));

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
    // KEPT, because it is a handle (see latencyBinding_): unbound in close().
    latencyBinding_ = libpd_bind(PdLatencyReceiver::nameFor(dollarZero_).c_str());
    return true;
}

void LibPdEngine::close() noexcept {
    native_.reset();
    nativeLatency_ = nullptr;
    nativePrepared_ = false;
    adapterLatency_.store(0, std::memory_order_release);
    paramSymbols_.clear();
    // A t_symbol* comes from the instance's own table (dogensym takes the
    // instance) and pdinstance_free frees that table: never kept past close.
    transportSymbol_ = nullptr;
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
        // Unbound while this instance is still selected and alive: the
        // receive object lives in its symbol table.
        if (latencyBinding_ != nullptr) {
            libpd_unbind(latencyBinding_);
            latencyBinding_ = nullptr;
        }
        libpd_closefile(patch_);
        patch_ = nullptr;
        dollarZero_ = 0;
    }
    // The console owner goes with the slot, before the instance is freed: a
    // hook that fired after the free would otherwise find this engine.
    PdRuntime::forgetInstance(instance_);
    sendTail_.store(sendHead_.load(std::memory_order_acquire), std::memory_order_release);
    pendingMidiCount_ = 0;
    transportPending_ = false;
    libpd_free_instance(static_cast<t_pdinstance*>(instance_));
    instance_ = nullptr;
}

void LibPdEngine::prepare(double sampleRate, std::int32_t maxFrames) {
    const int rate = std::isfinite(sampleRate) && sampleRate >= 1.0 &&
                             sampleRate <= static_cast<double>(std::numeric_limits<int>::max())
                         ? static_cast<int>(sampleRate) : 44100;
    if (native_ != nullptr) {
        sampleRate_ = static_cast<double>(rate);
        native_->prepare(sampleRate_, maxFrames);
        nativePrepared_ = true;
        adapterLatency_.store(0, std::memory_order_release);
        midiRouter_.configure(midiRoute_, engine::ExpressionCaps{}, 15);
        midiRouter_.reset();
        midiScratch_.assign(static_cast<std::size_t>(std::max<std::int32_t>(maxFrames, 64)) * 4 + 64,
                            engine::MpeOut{});
        requestLatencyReport();
        return;
    }
    if (patch_ == nullptr) return;
    selectInstance();

    sampleRate_ = static_cast<double>(rate);
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

    // The MIDI router, configured here for the same reason the adapter is
    // sized here: `prepare` is the last message-thread moment before audio.
    // `reset()` forgets every note, which a re-activated device must -- a note
    // still marked sounding would send its note-off to a member channel that
    // is no longer its own.
    //
    // The scratch is one segment's worth, generously: MpeMidi turns a single
    // note-on into a note-on plus up to three control messages, and the
    // Configuration Message is a burst of six. Overflow is counted rather than
    // dropped silently.
    midiRouter_.configure(midiRoute_, engine::ExpressionCaps{}, 15);
    midiRouter_.reset();
    midiScratch_.assign(static_cast<std::size_t>(std::max<std::int32_t>(maxFrames, 64)) * 4 + 64,
                        engine::MpeOut{});
    // DEV-09: what is routed waits here for the next locked tick. Twice the
    // scratch, because a segment shorter than a tick hands its events on.
    pendingMidi_.assign(midiScratch_.size() * 2, engine::MpeOut{});
    pendingMidiCount_ = 0;

    // Prime the output with one block of silence. This is what makes the delay
    // CONSTANT: every segment can then pop exactly as many frames as it pushed,
    // whatever its length, and the engine never has to hand back a short read.
    // The price is one Pd block of latency, reported through
    // adapterLatencySamples() so the graph compensates it.
    inFill_ = 0;
    // Automatic (DEV-13): whole ticks with no added delay when the buffer is a
    // multiple of 64; otherwise the one reported block from the start.
    wholeTicks_ = blockMode_ == BlockMode::WholeBlocks ||
                  (blockMode_ == BlockMode::Automatic && maxFrames > 0 && maxFrames % block_ == 0);
    outFill_ = wholeTicks_ ? 0 : block_;
    adapterLatency_.store(outFill_, std::memory_order_release);
}

std::int32_t LibPdEngine::segmentQuantum() const noexcept {
    if (native_ != nullptr || patch_ == nullptr) return 1;
    return blockMode_ == BlockMode::Buffered ? 1 : block_;
}

void LibPdEngine::requestLatencyReport() {
    if (native_ != nullptr) {
        nativeLatency_->report(native_->latencySamples());
        return;
    }
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
    if (native_ == nullptr ? (patch_ == nullptr || in_.empty()) : !nativePrepared_) {
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

    if (native_ == nullptr && blockMode_ == BlockMode::WholeBlocks && n % block_ != 0) {
        rejectedSegments_.fetch_add(1, std::memory_order_relaxed);
        if (io.out != nullptr)
            for (std::int32_t c = 0; c < io.channels; ++c)
                if (io.out[c] != nullptr)
                    std::fill_n(io.out[c] + io.blockOffset, n, 0.f);
        return;
    }
    if (native_ == nullptr && blockMode_ == BlockMode::Automatic && wholeTicks_ &&
        n % block_ != 0) {
        // DEV-13: a segment inside a Pd tick. From here on, one block: a block
        // of silence goes ahead of the queue (the one gap, at the switch), the
        // latency is reported and its epoch moves so the graph re-compensates.
        wholeTicks_ = false;
        std::fill_n(out_.begin(), static_cast<std::size_t>(block_) * static_cast<std::size_t>(outCh_), 0.f);
        outFill_ = block_;
        adapterLatency_.store(block_, std::memory_order_release);
        adapterEpoch_.fetch_add(1, std::memory_order_acq_rel);
        blockFallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    selectInstance();
    segments_.fetch_add(1, std::memory_order_relaxed);
    const PdSampleBlock::Read sampleRead(sampleBlock_, samples_);

    // --- transport, then MIDI, then the audio both belong to ----------------
    //
    // Transport first: a patch that reads bar and beat to decide what a note
    // means must have this block's position before the note arrives, not
    // after.
    //
    // READ HERE RATHER THAN PUSHED BY THE DEVICE NODE. `PdDevice::process`
    // already hands this engine the whole `NodeIo`, so taking `io.transport`
    // off it leaves no ordering rule for a caller to forget. `setTransport`
    // stays for callers that have no `NodeIo` -- which is what the test uses.
    //
    // COPIED, NEVER RETAINED. `graph.hpp` says `io.transport` is "valid only
    // during process"; keeping the pointer would make every later block read
    // freed memory that usually still looks right.
    if (io.transport != nullptr) {
        Transport t;
        t.playing = io.transport->playing;
        t.bpm = io.transport->bpm;
        t.timeSigNumerator = io.transport->timeSigNumerator;
        t.timeSigDenominator = io.transport->timeSigDenominator;
        t.bar = static_cast<double>(io.transport->bar);
        t.beat = static_cast<double>(io.transport->beat);
        t.ticksInQuarter = static_cast<double>(io.transport->ticksInQuarter);
        // `timelineSample` is deliberately NOT passed on: it is an absolute
        // sample count, and Pd's 32-bit floats stop being exact for integers
        // at 2^24 -- under six minutes at 48 kHz. That is the same arithmetic
        // that keeps an absolute TICK count out of the field list.
        setTransport(t);
    }
    // A NULL TRANSPORT KEEPS THE LAST VALUES, and that is deliberate rather
    // than an omission. `io.transport` is null outside a Session, so a device
    // rendered by a test harness or an offline tool would otherwise see the
    // tempo snap to 120 and the bar to 1 -- a patch synced to the host would
    // hear that as a jump. Nothing changing is the honest report of nothing
    // being known.
    // DEV-09: on the Pd path the transport and the MIDI below are QUEUED and
    // delivered inside the next locked tick (lockedTick), never into a running
    // instance without its lock. Pd acts on both at its next tick either way.
    if (transportSet_) {
        if (native_ != nullptr) deliverTransport();
        else transportPending_ = true;
    }

    // --- MIDI in, before the audio it belongs to (ADR-0194) ------------------
    //
    // AT THE SEGMENT'S START, and that is as accurate as the engine offers
    // rather than a rounding-down. The scheduler already splits a block at
    // every event frame (graph.hpp: an event at frame 100 splits 512 into 100
    // and 412), so an event that lands in this segment lands at its start.
    // Pd then quantises again to its own 64-sample block, which is Pd's limit
    // and is the same one ADR-0188 d3 records for parameters.
    if (!io.events.empty() && !midiScratch_.empty()) {
        engine::MpeOutList list(midiScratch_.data(),
                                static_cast<std::int32_t>(midiScratch_.size()));
        midiRouter_.route(io.events.first, io.events.count, io.blockOffset, list);
        if (native_ != nullptr) {
            for (std::int32_t i = 0; i < list.size(); ++i) deliverMidi(list.at(i));
        } else {
            for (std::int32_t i = 0; i < list.size(); ++i) {
                if (static_cast<std::size_t>(pendingMidiCount_) < pendingMidi_.size())
                    pendingMidi_[static_cast<std::size_t>(pendingMidiCount_++)] = list.at(i);
                else
                    ++midiDropped_;
            }
        }
        midiDropped_ += list.dropped();
    }

    if (native_ != nullptr) {
        native_->process(io);
        nativeLatency_->report(native_->latencySamples());
        return;
    }

    // Tile oversized calls into preallocated scratch. The old push clipped
    // them at capacity, silently discarding input. WholeBlocks tiles stay on
    // Pd boundaries even if prepare's maximum was not itself a multiple.
    const auto capacity = static_cast<std::int32_t>(in_.size()) / inCh_ - block_;
    const auto chunk = wholeTicks_ ? (capacity / block_) * block_ : capacity;
    for (std::int32_t offset = 0; offset < n;) {
        auto part = io;
        part.frames = std::min(chunk, n - offset);
        part.blockOffset += offset;
        processPdAudio(part);
        offset += part.frames;
    }

    // Publish only after all ticks belonging to this segment have completed.
    for (auto& a : arrays_) {
        if (a->framesPerRead <= 0.0) continue;
        a->due -= static_cast<double>(n);
        if (a->due > 0.0) continue;
        a->due += a->framesPerRead;
        if (a->due < 0.0) a->due = a->framesPerRead;
        // DEV-09: under our own trylock, never libpd_read_array (it returns
        // with the lock held when the table is shorter than asked). Busy means
        // this read waits for the next due time; a display loses one frame.
        if (sys_trylock() != 0) continue;
        pd_this->pd_islocked = 1;
        const bool read = readArrayLocked(a->tableSymbol, a->scratch.data(), a->length);
        sys_unlock();
        if (read) a->buffer.publish(a->scratch.data(), a->length);
    }
}

bool LibPdEngine::readArrayLocked(void* symbol, float* out, std::int32_t length) noexcept {
    if (symbol == nullptr || length <= 0) return false;
    auto* const array = reinterpret_cast<t_garray*>(
        pd_findbyclass(static_cast<t_symbol*>(symbol), garray_class));
    int size = 0;
    t_word* words = nullptr;
    if (array == nullptr || !garray_getfloatwords(array, &size, &words) || size < length)
        return false;
    for (std::int32_t i = 0; i < length; ++i) out[i] = static_cast<float>(words[i].w_float);
    return true;
}

bool LibPdEngine::lockedTick() noexcept {
    if (sys_trylock() != 0) {
        skippedTicks_.fetch_add(1, std::memory_order_relaxed);
        std::fill(tickOut_.begin(), tickOut_.end(), 0.f);
        return false;
    }
    // sys_trylock does not mark the instance locked, as sys_lock does; Pd's
    // own pd_globallock checks the mark.
    pd_this->pd_islocked = 1;
    deliverPendingLocked();
    // libpd_process_float's body for one tick, without its sys_lock (blocking)
    // and its sys_pollgui (sockets and GUI, never on the audio thread).
    const int inChannels = STUFF->st_inchannels, outChannels = STUFF->st_outchannels;
    t_sample* const soundIn = STUFF->st_soundin;
    t_sample* const soundOut = STUFF->st_soundout;
    for (int frame = 0; frame < DEFDACBLKSIZE; ++frame)
        for (int c = 0; c < inChannels; ++c)
            soundIn[c * DEFDACBLKSIZE + frame] =
                c < inCh_ ? tickIn_[static_cast<std::size_t>(frame * inCh_ + c)] : 0.f;
    std::memset(soundOut, 0,
                static_cast<std::size_t>(outChannels) * static_cast<std::size_t>(DEFDACBLKSIZE) * sizeof(t_sample));
    sched_tick();
    for (int frame = 0; frame < DEFDACBLKSIZE; ++frame)
        for (int c = 0; c < outCh_; ++c)
            tickOut_[static_cast<std::size_t>(frame * outCh_ + c)] =
                c < outChannels ? static_cast<float>(soundOut[c * DEFDACBLKSIZE + frame]) : 0.f;
    sys_unlock();
    return true;
}

void LibPdEngine::deliverPendingLocked() noexcept {
    // Message-thread sends first, in the order they were made.
    const auto head = sendHead_.load(std::memory_order_acquire);
    auto tail = sendTail_.load(std::memory_order_relaxed);
    for (; tail != head; ++tail) {
        const auto& send = sends_[static_cast<std::size_t>(tail % kSendSlots)];
        auto* const symbol = static_cast<t_symbol*>(send.symbol);
        if (symbol != nullptr && symbol->s_thing != nullptr) pd_float(symbol->s_thing, send.value);
    }
    sendTail_.store(tail, std::memory_order_release);
    // Then the parameters (ADR-0188 d3), the latest value of each.
    for (std::size_t i = 0; i < paramDirty_.size() && i < paramSymbols_.size(); ++i) {
        if (paramDirty_[i] == 0) continue;
        paramDirty_[i] = 0;
        auto* const symbol = static_cast<t_symbol*>(paramSymbols_[i].second);
        if (symbol != nullptr && symbol->s_thing != nullptr) pd_float(symbol->s_thing, paramValues_[i]);
    }
    // Then the transport and the MIDI that belong to the coming audio.
    if (transportPending_) {
        transportPending_ = false;
        deliverTransport();
    }
    for (std::int32_t i = 0; i < pendingMidiCount_; ++i)
        deliverMidi(pendingMidi_[static_cast<std::size_t>(i)]);
    pendingMidiCount_ = 0;
}

void LibPdEngine::processPdAudio(const engine::NodeIo& io) noexcept {
    const auto n = io.frames;

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
            else if (c >= io.channels && io.sidechain != nullptr &&
                     (c - io.channels) < io.channels)
                src = io.sidechain[c - io.channels];
            dst[c] = src != nullptr ? src[io.blockOffset + f] : 0.f;
        }
    }
    inFill_ += push;

    // --- render whole Pd blocks ---------------------------------------------
    while (inFill_ >= block_) {
        std::memcpy(tickIn_.data(), in_.data(), tickIn_.size() * sizeof(float));
        if (lockedTick()) ticks_.fetch_add(1, std::memory_order_relaxed);

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

void LibPdEngine::bindArrays(const PdDeclarations& decls) {
    arrays_.clear();
    if (patch_ == nullptr) return;
    selectInstance();

    for (const auto& d : decls.arrays) {
        auto b = std::make_unique<BoundArray>();
        b->id = d.id;
        b->tableName = pdArrayReceiveName(dollarZero_, d.id);
        b->length = d.length;

        // The table has to exist and be the declared size. A patch that
        // declares 512 and instantiates a table of 64 would otherwise have its
        // reads run past the end -- libpd_read_array does no bounds checking on
        // EITHER side, which its header says plainly.
        const int actual = libpd_arraysize(b->tableName.c_str());
        if (actual < b->length) continue;
        // Resolved once, under the lock: gensym writes this instance's symbol
        // table, which a running tick reads (DEV-09).
        sys_lock();
        b->tableSymbol = static_cast<void*>(gensym(b->tableName.c_str()));
        sys_unlock();

        b->scratch.assign(static_cast<std::size_t>(b->length), 0.f);
        b->buffer.prepare(b->length);
        b->framesPerRead = d.rate > 0.0 ? (sampleRate_ / d.rate) : 0.0;
        b->due = b->framesPerRead;
        arrays_.push_back(std::move(b));
    }
}

const engine::PublishedArray* LibPdEngine::publishedArray(std::int32_t id) const noexcept {
    for (const auto& a : arrays_) if (a->id == id) return &a->buffer;
    return nullptr;
}

bool LibPdEngine::sendFloat(const char* suffix, float value) noexcept {
    if (patch_ == nullptr || suffix == nullptr) return false;
    selectInstance();
    const std::string recv = std::to_string(dollarZero_) + "-" + suffix;
    // DEV-09: the name is resolved under the lock (gensym writes the symbol
    // table), then the value goes through a single-producer ring and is
    // delivered by the audio thread inside its next locked tick: the message
    // thread never holds the instance while a value travels, so the audio
    // thread never has to skip a tick for it.
    sys_lock();
    t_symbol* const symbol = gensym(recv.c_str());
    const bool bound = symbol->s_thing != nullptr;
    sys_unlock();
    if (!bound) return false;
    const auto head = sendHead_.load(std::memory_order_relaxed);
    if (head - sendTail_.load(std::memory_order_acquire) >= kSendSlots) {
        sendsDropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    sends_[static_cast<std::size_t>(head % kSendSlots)] = {static_cast<void*>(symbol), value};
    sendHead_.store(head + 1, std::memory_order_release);
    return true;
}

void LibPdEngine::bindSamples(const PdDeclarations& decls) {
    samples_.declare(decls.samples);
    sampleBlock_.declare(decls.samples);
}
bool LibPdEngine::publishSample(std::int32_t id, std::unique_ptr<PdSampleBuffer> sample) {
    return samples_.publish(id, std::move(sample));
}

void LibPdEngine::bindParameters(const PdDeclarations& decls) {
    paramSymbols_.clear();
    if (native_ != nullptr) {
        for (const auto& p : decls.params) paramSymbols_.emplace_back(p.id, nullptr);
        return;
    }
    if (patch_ == nullptr) return;
    selectInstance();
    paramSymbols_.reserve(decls.params.size());
    paramValues_.assign(decls.params.size(), 0.f);
    paramDirty_.assign(decls.params.size(), 0);
    // gensym HERE, on the message thread, once, and under the lock: it writes
    // the symbol table a running tick reads. Every later send reads `s_thing`
    // off the symbol this returns.
    sys_lock();
    for (const auto& p : decls.params) {
        const std::string name = pdParamReceiveName(dollarZero_, p.id);
        paramSymbols_.emplace_back(p.id, static_cast<void*>(gensym(name.c_str())));
    }
    sys_unlock();
}

bool LibPdEngine::sendParameter(std::int32_t id, float value) noexcept {
    if (!std::isfinite(value)) return false;
    for (std::size_t i = 0; i < paramSymbols_.size(); ++i) {
        const auto& entry = paramSymbols_[i];
        if (entry.first != id) continue;
        if (native_ != nullptr) {
            const bool sent = native_->sendParameter(id, value);
            nativeLatency_->report(native_->latencySamples());
            return sent;
        }
        t_symbol* const sym = static_cast<t_symbol*>(entry.second);
        if (sym == nullptr || sym->s_thing == nullptr || i >= paramDirty_.size()) return false;
        // DEV-09: kept, latest value wins, and delivered by `pd_float` inside
        // the next locked tick (lockedTick), which is still before the block's
        // first Pd tick, as ADR-0188 d3 asks. Never into a running instance
        // without its lock.
        paramValues_[i] = value;
        paramDirty_[i] = 1;
        return true;
    }
    return false;
}

void LibPdEngine::setExpressionRoute(engine::ExpressionRoute route) noexcept {
    midiRoute_ = route;
}

void LibPdEngine::setTransport(const Transport& tr) noexcept {
    transport_ = tr;
    transportSet_ = true;
}

void LibPdEngine::deliverTransport() noexcept {
    if (native_ != nullptr) {
        native_->tempo(static_cast<float>(transport_.bpm));
        return;
    }
    t_symbol* const sym = static_cast<t_symbol*>(transportSymbol_);
    // A patch with no [adi.transport] binds nothing, so this is the whole cost
    // of transport for every patch that does not ask for it.
    if (sym == nullptr || sym->s_thing == nullptr) return;

    // On the stack, and `pd_list` rather than `libpd_list`: the libpd entry
    // point would take `sys_lock()` and build the atoms with its own
    // allocating message stack. ADR-0188 d3's field list, in order.
    t_atom at[7];
    SETFLOAT(at + 0, transport_.playing ? 1.f : 0.f);
    SETFLOAT(at + 1, static_cast<t_float>(transport_.bpm));
    SETFLOAT(at + 2, static_cast<t_float>(transport_.timeSigNumerator));
    SETFLOAT(at + 3, static_cast<t_float>(transport_.timeSigDenominator));
    SETFLOAT(at + 4, static_cast<t_float>(transport_.bar));
    SETFLOAT(at + 5, static_cast<t_float>(transport_.beat));
    SETFLOAT(at + 6, static_cast<t_float>(transport_.ticksInQuarter));
    pd_list(sym->s_thing, &s_list, 7, at);
}

void LibPdEngine::deliverMidi(const engine::MpeOut& m) noexcept {
    if (native_ != nullptr) {
        native_->midi(m);
        return;
    }
    // `inmidi_*` rather than `libpd_noteon` and friends, for the reason
    // `sendParameter` avoids `libpd_float`: every libpd MIDI entry point wraps
    // its call in `sys_lock()`/`sys_unlock()` (z_libpd.c at the pinned commit),
    // and this runs on the audio thread. The functions underneath take no lock
    // and allocate nothing -- `inmidi_noteon` builds three stack atoms and
    // dispatches to `pd_this->pd_midi->m_notein_sym->s_thing`, a symbol the
    // instance already holds, so there is no `gensym` either. A patch with no
    // [notein] costs the null check on `s_thing` and nothing more.
    //
    // Port 0 always: one device is one Pd instance, and Pd adds
    // `(portno << 4)` to the channel it shows. A second port would make a
    // patch see channel 17.
    constexpr int kPort = 0;
    const int channel = static_cast<int>(m.channel);

    // `word` IS ONLY FILLED FOR A CONTROL. `MpeRouter` leaves it zero on a
    // note and a poly pressure and puts the value in `value`, a 0..1 double
    // (mpe_output.cpp) -- reading `word` there would have made every note-on a
    // note-off, which is a stuck-silent instrument rather than a crash. Read,
    // not discovered.
    const auto sevenBit = [](double unit) {
        const long v = std::lround(unit * 127.0);
        return static_cast<int>(v < 0 ? 0 : (v > 127 ? 127 : v));
    };

    switch (m.kind) {
        case engine::MpeOut::Kind::NoteOn: {
            // A note-on with velocity 0 IS a note-off in MIDI, so a quiet note
            // must not become one. The floor is 1, which is the convention
            // every sequencer uses for the same reason.
            const int velocity = std::max(1, sevenBit(m.value));
            inmidi_noteon(kPort, channel, static_cast<int>(m.key), velocity);
            break;
        }
        case engine::MpeOut::Kind::NoteOff:
            // Pd has no note-off: velocity 0 is one, which is what [notein]
            // patches test for and what every MIDI file writes.
            inmidi_noteon(kPort, channel, static_cast<int>(m.key), 0);
            break;
        case engine::MpeOut::Kind::PolyPressure:
            inmidi_polyaftertouch(kPort, channel, static_cast<int>(m.key),
                                  sevenBit(m.value));
            break;
        case engine::MpeOut::Kind::Control:
            if (m.ctrl == engine::kCtrlPitchBend) {
                // The router's `word` is 14-bit, 0..16383 with 8192 centred.
                // Pd's `inmidi_pitchbend` wants the same 0..16383 -- it is
                // `libpd_pitchbend` that takes a signed value and adds 8192,
                // which is why this does not.
                inmidi_pitchbend(kPort, channel, static_cast<int>(m.word));
            } else if (m.ctrl == engine::kCtrlAfterTouch) {
                inmidi_aftertouch(kPort, channel, static_cast<int>(m.word));
            } else if (m.ctrl < 128) {
                inmidi_controlchange(kPort, channel, static_cast<int>(m.ctrl),
                                     static_cast<int>(m.word));
            }
            break;
        case engine::MpeOut::Kind::Expression:
            // VST3's own note-expression event. It has no MIDI form at all,
            // which is exactly why the route is MpeMidi: under it the router
            // never emits one. Ignored rather than approximated.
            break;
    }
}

bool LibPdEngine::readArray(const char* suffix, std::vector<float>& out) noexcept {
    if (patch_ == nullptr || suffix == nullptr) return false;
    selectInstance();
    const std::string name = std::to_string(dollarZero_) + "-" + suffix;
    // DEV-09: not libpd_read_array, whose range error returns with the
    // instance still locked (every later call on it then waits forever).
    sys_lock();
    t_symbol* const symbol = gensym(name.c_str());
    auto* const array = reinterpret_cast<t_garray*>(pd_findbyclass(symbol, garray_class));
    int size = 0;
    t_word* words = nullptr;
    const bool found = array != nullptr && garray_getfloatwords(array, &size, &words);
    const bool fits = found && out.size() <= static_cast<std::size_t>(size);
    if (fits)
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(words[i].w_float);
    sys_unlock();
    return fits;
}

LibPdEngine::Counters LibPdEngine::counters() const noexcept {
    return {ticks_.load(std::memory_order_relaxed),
            segments_.load(std::memory_order_relaxed),
            starved_.load(std::memory_order_relaxed),
            rejectedSegments_.load(std::memory_order_relaxed),
            skippedTicks_.load(std::memory_order_relaxed),
            consoleDropped_.load(std::memory_order_relaxed),
            sendsDropped_.load(std::memory_order_relaxed),
            blockFallbacks_.load(std::memory_order_relaxed)};
}


// ---------------------------------------------------------------------------
// ADR-0188 d8 -- no Pd external is ever loaded from disk
// ---------------------------------------------------------------------------

const std::vector<std::string>& pdLoadableCodeExtensions() {
    // Pd's own list is built at runtime and neither exported nor settable, so
    // this is a superset of every platform's rather than a copy of one. The
    // deken naming convention puts the architecture BEFORE the extension
    // (`foo.darwin-amd64-32.so`), so matching the final extension catches
    // those too.
    static const std::vector<std::string> kExtensions = {
        ".pd_darwin", ".pd_linux", ".pd_freebsd", ".pd_irix5", ".pd_irix6",
        ".d_fat", ".d_ppc", ".d_i386", ".d_amd64", ".d_arm64",
        ".l_ia64", ".l_i386", ".l_arm", ".l_arm64", ".l_amd64",
        ".m_i386", ".m_amd64", ".m_arm64",
        ".w_i386", ".w_amd64", ".b_i386",
        ".dll", ".so", ".dylib", ".bundle", ".sl",
    };
    return kExtensions;
}

std::vector<std::string> pdLoadableCodeIn(const std::string& dir) {
    std::vector<std::string> found;
    if (dir.empty()) return found;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return found;   // an unreadable directory holds nothing Pd can read either
    for (const auto& entry : it) {
        std::error_code fe;
        if (!entry.is_regular_file(fe) || fe) continue;
        std::string ext = entry.path().extension().string();
        // Extensions are compared case-insensitively: Windows and macOS both
        // have case-insensitive filesystems by default, so `Evil.DLL` is the
        // same file to the loader as `evil.dll`.
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto& exts = pdLoadableCodeExtensions();
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            found.push_back(entry.path().filename().string());
        }
    }
    std::sort(found.begin(), found.end());   // a stable message, whatever the directory order
    return found;
}

void LibPdEngine::printHookThunk(const char* line) {
    // Any thread, the audio thread included: no lock, no allocation.
    LibPdEngine* const engine = consoleOwner(static_cast<void*>(libpd_this_instance()));
    if (engine != nullptr && line != nullptr) engine->enqueuePrint(line);
}

void LibPdEngine::enqueuePrint(const char* piece) noexcept {
    // A piece longer than a slot continues in the next one; the reader joins
    // pieces into lines, so a split costs nothing.
    std::size_t length = std::strlen(piece);
    do {
        const std::size_t part = std::min(length, sizeof(PrintSlot::text) - 1);
        // Vyukov's bounded multi-producer queue: claim the slot whose sequence
        // equals the position, publish by advancing its sequence.
        auto position = printHead_.load(std::memory_order_relaxed);
        PrintSlot* slot = nullptr;
        for (;;) {
            slot = &printSlots_[static_cast<std::size_t>(position % kPrintSlots)];
            const auto sequence = slot->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::int64_t>(sequence - position);
            if (difference == 0) {
                if (printHead_.compare_exchange_weak(position, position + 1,
                                                     std::memory_order_relaxed))
                    break;
            } else if (difference < 0) {
                consoleDropped_.fetch_add(1, std::memory_order_relaxed);   // full
                return;
            } else {
                position = printHead_.load(std::memory_order_relaxed);
            }
        }
        std::memcpy(slot->text, piece, part);
        slot->text[part] = '\0';
        slot->sequence.store(position + 1, std::memory_order_release);
        piece += part;
        length -= part;
    } while (length > 0);
}

void LibPdEngine::drainConsole() const {
    for (;;) {
        PrintSlot& slot = printSlots_[static_cast<std::size_t>(printTail_ % kPrintSlots)];
        if (slot.sequence.load(std::memory_order_acquire) != printTail_ + 1) return;
        appendConsole(slot.text);
        slot.sequence.store(printTail_ + kPrintSlots, std::memory_order_release);
        ++printTail_;
    }
}

void LibPdEngine::appendConsole(const char* line) const {
    if (line == nullptr) return;
    // Pd prints a line in pieces and ends it with a newline of its own, so the
    // hook is called several times for one message. They are joined here and
    // split on the newline, which is what makes a "couldn't create" line
    // searchable as one string.
    pending_ += line;
    std::size_t nl;
    while ((nl = pending_.find('\n')) != std::string::npos) {
        if (console_.size() >= kMaxConsoleLines) console_.erase(console_.begin());
        console_.push_back(pending_.substr(0, nl));
        pending_.erase(0, nl + 1);
    }
}

std::vector<std::string> LibPdEngine::consoleLines() const {
    const std::lock_guard<std::mutex> lock(consoleMutex_);
    drainConsole();
    std::vector<std::string> out = console_;
    if (!pending_.empty()) out.push_back(pending_);   // a line Pd has not ended yet
    return out;
}

}  // namespace adi::device
