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


/// instance -> engine, for the print hook. A separate registry from the slots
/// above, and deliberately so: the float hook runs on the audio thread and may
/// not touch a mutex, while the print hook runs on the message thread and a
/// mutex there costs nothing.
std::mutex g_consoleMutex;
std::vector<std::pair<void*, LibPdEngine*>> g_consoleOwners;

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

void LibPdEngine::addSearchPath(const std::string& dir) {
    // REMEMBERED, not applied. Under PDINSTANCE the search path is per-instance
    // state, not process-wide, so a path added before this engine's instance
    // exists lands on whichever instance happened to be current -- or on none.
    // The only sign is the abstraction failing to create, which Pd reports by
    // printing the object's text and nothing else.
    searchPaths_.push_back(dir);
}

bool LibPdEngine::open(PdLatencyReceiver& latency, std::string& error) {
    if (!PdRuntime::initialise(error)) return false;
    if (patch_ != nullptr) return true;

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
    {
        std::ifstream in(dir_ + "/" + file_, std::ios::binary);
        if (in) {
            std::ostringstream buf;
            buf << in.rdbuf();
            const auto requests = pdExternalRequests(buf.str());
            if (!requests.empty()) {
                error = "refusing a patch that declares " + requests.front().flag +
                        " (ADR-0188 d8): box " + std::to_string(requests.front().box);
                if (!requests.front().value.empty()) error += ", " + requests.front().value;
                return false;
            }
        }
    }

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
    {
        const std::lock_guard<std::mutex> lock(g_consoleMutex);
        g_consoleOwners.emplace_back(instance_, this);
    }
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
    // The console owner goes before the instance is freed: a hook that fired
    // after the free would look up a dangling pointer.
    {
        const std::lock_guard<std::mutex> lock(g_consoleMutex);
        g_consoleOwners.erase(
            std::remove_if(g_consoleOwners.begin(), g_consoleOwners.end(),
                           [this](const auto& o) { return o.second == this; }),
            g_consoleOwners.end());
    }
    libpd_free_instance(static_cast<t_pdinstance*>(instance_));
    instance_ = nullptr;
}

void LibPdEngine::prepare(double sampleRate, std::int32_t maxFrames) {
    if (patch_ == nullptr) return;
    selectInstance();

    const int rate = static_cast<int>(sampleRate > 0 ? sampleRate : 44100);
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

    // --- transport, then MIDI, then the audio both belong to ----------------
    //
    // Transport first: a patch that reads bar and beat to decide what a note
    // means must have this block's position before the note arrives, not
    // after.
    if (transportSet_) deliverTransport();

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
        for (std::int32_t i = 0; i < list.size(); ++i) deliverMidi(list.at(i));
        midiDropped_ += list.dropped();
    }

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

    // --- publish the declared arrays, at the rate each declared ---------------
    // On the AUDIO thread, which is where ADR-0183 d2 puts the write, and after
    // the ticks so a reader never sees a table half-written by this block.
    for (auto& a : arrays_) {
        if (a->framesPerRead <= 0.0) continue;
        a->due -= static_cast<double>(n);
        if (a->due > 0.0) continue;
        a->due += a->framesPerRead;
        if (a->due < 0.0) a->due = a->framesPerRead;   // a long segment, not a backlog
        if (libpd_read_array(a->scratch.data(), a->tableName.c_str(), 0, a->length) == 0) {
            a->buffer.publish(a->scratch.data(), a->length);
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
    return libpd_float(recv.c_str(), value) == 0;
}

void LibPdEngine::bindParameters(const PdDeclarations& decls) {
    paramSymbols_.clear();
    if (patch_ == nullptr) return;
    selectInstance();
    paramSymbols_.reserve(decls.params.size());
    for (const auto& p : decls.params) {
        // gensym HERE, on the message thread, once. Every later send reads
        // `s_thing` off the symbol this returns.
        const std::string name = pdParamReceiveName(dollarZero_, p.id);
        paramSymbols_.emplace_back(p.id, static_cast<void*>(gensym(name.c_str())));
    }
}

bool LibPdEngine::sendParameter(std::int32_t id, float value) noexcept {
    for (const auto& entry : paramSymbols_) {
        if (entry.first != id) continue;
        t_symbol* const sym = static_cast<t_symbol*>(entry.second);
        if (sym == nullptr || sym->s_thing == nullptr) return false;
        selectInstance();
        // `pd_float` is a dispatch through the receiver's class, and nothing
        // else. No `sys_lock`, deliberately: d3 puts this call on the audio
        // thread immediately before the block, where it is the only thread
        // touching this instance. A message-thread send belongs in
        // `sendFloat`, which does take the lock.
        pd_float(sym->s_thing, value);
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

LibPdEngine::Counters LibPdEngine::counters() const noexcept {
    return {ticks_.load(std::memory_order_relaxed),
            segments_.load(std::memory_order_relaxed),
            starved_.load(std::memory_order_relaxed)};
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
    void* const self = static_cast<void*>(libpd_this_instance());
    LibPdEngine* engine = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_consoleMutex);
        for (const auto& owner : g_consoleOwners) {
            if (owner.first == self) { engine = owner.second; break; }
        }
    }
    if (engine != nullptr) engine->appendConsole(line);
}

void LibPdEngine::appendConsole(const char* line) {
    if (line == nullptr) return;
    const std::lock_guard<std::mutex> lock(consoleMutex_);
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
    std::vector<std::string> out = console_;
    if (!pending_.empty()) out.push_back(pending_);   // a line Pd has not ended yet
    return out;
}

}  // namespace adi::device
