// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libpd-backed `PdPatchEngine` -- ADR-0035, ADR-0095, ADR-0183.
//
// `pd_device.hpp` defines the contract and leaves `PdPatchEngine` pure virtual
// so the device, its latency protocol and its route into the graph are tested
// with no libpd present. This is the other side of that seam: the one class
// that actually calls libpd.
//
// THREE THINGS ABOUT libpd DECIDE THIS FILE'S SHAPE, and none of them is
// obvious from its header.
//
//   1. **One Pd instance per device, not one per process.** Plain libpd is a
//      single instance: every open patch shares one DSP graph and one
//      `libpd_process_float` call that runs all of them together. A device in
//      track 1's chain has to be processed when the graph reaches track 1, at
//      that node's block size, with its own latency. So the build sets
//      PD_MULTI (see CMakeLists.txt), each engine owns a `t_pdinstance`, and
//      every libpd call is preceded by `libpd_set_instance`. Forgetting that
//      is not a crash; it is a patch quietly rendering into another device's
//      instance, which is the worst kind of bug to find later.
//
//   2. **Pd's block is 64 frames and the graph's is not.** `NodeIo` carries a
//      SEGMENT, not a block: an event at frame 100 splits a 512-frame block
//      into 100 and 412 (graph.hpp). So the engine practically never sees a
//      multiple of 64, and a block adapter is not an optimisation to add later
//      -- without it there is no correct way to call `libpd_process_float` at
//      all. The adapter costs exactly one Pd block of delay, which is real
//      latency and is reported as such through `adapterLatencySamples()`.
//
//   3. **libpd is interleaved; the graph is planar.** `libpd_process_float`
//      wants `ticks * 64 * channels` interleaved floats. The adapter does that
//      conversion, which is why it owns the scratch buffers rather than
//      borrowing the graph's.
//
// WHAT IS NOT HERE: patch discovery, parameters and published arrays. The
// parameter contract is ADR-0177's `[adi.param]`; published arrays are
// ADR-0183's `[adi.array]`. Both sit on top of this and neither changes it.

#pragma once

#include "juce/pd_device.hpp"
#include "juce/pd_declarations.hpp"
#include "adi/engine/published_array.hpp"

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace adi::device {

/// Process-wide libpd state: the one-time `libpd_init()`, and the map from a
/// Pd instance to the receiver table its float hook dispatches through.
///
/// **`$0` IS ONLY UNIQUE WITHIN ONE INSTANCE**, and that amends what ADR-0095
/// decision 1 assumed. `$0` comes from `canvas_getdollarzero()`, which is
/// per-instance state, so under PD_MULTI two devices in two instances both get
/// 1003 and both build the receive name `1003-report_latency`. One table keyed
/// on `$0` cannot tell them apart -- measured, not reasoned: opening the same
/// patch twice returns the same `$0` both times.
///
/// So the table is per instance, and the hook -- a bare function pointer with
/// no user data -- asks libpd which instance is current. Within an instance,
/// ADR-0095's reasoning is untouched and the name still does the routing.
class PdRuntime {
public:
    /// Idempotent, message thread. False with `error` set if libpd refuses.
    static bool initialise(std::string& error);
    /// True once `initialise` has succeeded.
    [[nodiscard]] static bool initialised() noexcept;
    /// Message thread. Routes this instance's float hook to `table`.
    static bool registerInstance(void* instance, PdReceiverTable& table) noexcept;
    /// Message thread, with that instance's audio stopped.
    static void forgetInstance(void* instance) noexcept;
    /// Pd's fixed internal block, 64. Read from libpd rather than assumed.
    [[nodiscard]] static std::int32_t blockSize() noexcept;
};

/// One patch, in its own Pd instance.
///
/// Not copyable and not movable: the instance pointer and the receiver binding
/// are owned, and a moved-from engine that still held either would close a
/// patch twice.
class LibPdEngine final : public PdPatchEngine {
public:
    /// `patchDir` and `patchFile` are passed to `libpd_openfile` unchanged.
    ///
    /// `inChannels` is how many `inlet~` the patch has and `outChannels` how
    /// many `outlet~`. They are stated rather than discovered because libpd
    /// has no way to ask a patch: `libpd_init_audio` fixes the instance's
    /// channel count before the patch is opened, and a patch with more
    /// `inlet~` than the instance has channels simply receives silence on the
    /// extras, with no error anywhere.
    LibPdEngine(std::string patchDir, std::string patchFile,
                std::int32_t inChannels, std::int32_t outChannels);
    ~LibPdEngine() override;

    LibPdEngine(const LibPdEngine&) = delete;
    LibPdEngine& operator=(const LibPdEngine&) = delete;

    /// Message thread, before `open`. Where libpd looks for abstractions the
    /// patch instantiates -- `adi.array`, `adi.param` -- when they do not sit
    /// beside the patch itself. The patch's own directory is always searched.
    void addSearchPath(const std::string& dir);

    bool open(PdLatencyReceiver& latency, std::string& error) override;
    void close() noexcept override;
    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void requestLatencyReport() override;
    void process(const engine::NodeIo& io) noexcept override;

    /// One Pd block, once prepared; 0 before that. See note 2 above.
    [[nodiscard]] std::int32_t adapterLatencySamples() const noexcept override {
        return adapterLatency_;
    }

    /// The patch's `$0`. Zero until opened.
    [[nodiscard]] int dollarZero() const noexcept { return dollarZero_; }

    /// Message thread, after `prepare`. Gives the engine the arrays the patch
    /// declares, so it can read them and publish them to the UI.
    ///
    /// The declarations come from a STATIC PARSE of the patch text, not from
    /// asking Pd (ADR-0177 fix 3) -- which is why this takes them rather than
    /// discovering them. Pd is running by now and could be asked; doing so
    /// would put the declared set outside the op log, which is the thing that
    /// contract exists to prevent.
    void bindArrays(const PdDeclarations& decls);

    /// The published array with this id, or null. The UI reads it once a
    /// frame; the audio thread writes it (ADR-0183 d2).
    [[nodiscard]] const engine::PublishedArray* publishedArray(std::int32_t id) const noexcept;

    /// Message thread. Sends a float to a receiver in THIS patch's instance,
    /// with `$0-` prefixed: `send("depth", 1.f)` reaches `[r $0-depth]`.
    /// False when the patch is not open or the receiver does not exist.
    bool sendFloat(const char* suffix, float value) noexcept;

    /// Audio-thread counters, for tests and for answering "is this patch
    /// running at all". Not atomic as a set; each member is.
    struct Counters {
        std::int64_t ticks = 0;      ///< Pd blocks rendered
        std::int64_t segments = 0;   ///< process() calls
        std::int64_t starved = 0;    ///< pops that found too little output
    };
    [[nodiscard]] Counters counters() const noexcept;

private:
    void selectInstance() const noexcept;

    std::string dir_, file_;
    std::int32_t inCh_ = 0, outCh_ = 0;
    void* instance_ = nullptr;        ///< t_pdinstance*, opaque here
    void* patch_ = nullptr;           ///< libpd_openfile handle
    int dollarZero_ = 0;
    std::int32_t block_ = 64;
    std::int32_t adapterLatency_ = 0;
    double sampleRate_ = 44100.0;
    bool dspOn_ = false;

    /// The block adapter. `in_` and `out_` are interleaved by channel, sized
    /// for one tick plus the largest segment the graph promised.
    std::vector<float> in_, out_, tickIn_, tickOut_;
    std::int32_t inFill_ = 0;         ///< frames waiting to be rendered
    std::int32_t outFill_ = 0;        ///< frames rendered and not yet handed back

    /// One published array per declaration, with the Pd table name to read it
    /// from and the frame countdown that honours its declared rate.
    struct BoundArray {
        std::int32_t id = 0;
        std::string tableName;
        std::int32_t length = 0;
        double framesPerRead = 0.0;
        double due = 0.0;
        std::vector<float> scratch;
        engine::PublishedArray buffer;
    };
    std::vector<std::unique_ptr<BoundArray>> arrays_;

    /// Paths asked for before the instance existed, applied in `open`.
    std::vector<std::string> searchPaths_;

    /// This instance's routing table. Per engine rather than shared, because
    /// `$0` collides across instances -- see PdRuntime.
    PdReceiverTable receivers_;

    std::atomic<std::int64_t> ticks_{0}, segments_{0}, starved_{0};
};

}  // namespace adi::device
