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
//   4. **Pd loads externals from the same paths it loads abstractions from,
//      and neither of the two obvious ways to stop it works.** ADR-0188 d8
//      says ADI loads no Pd external from disk: a patch must not be able to
//      bring native code into the process. Both mechanisms that suggest
//      themselves were read in Pd 0.56's `s_loader.c` at the pinned commit and
//      both fail:
//        * **A registered loader cannot refuse.** `sys_register_loader`
//          APPENDS to the loader list, and `sys_do_load_lib` is the list's
//          static head, so anything registered runs only after the default has
//          already tried -- and succeeded or not -- on its own.
//        * **Building libpd without dynamic loading does not cover Windows.**
//          `HAVE_LIBDL` gates only the `dlopen` branch; the `#ifdef _WIN32`
//          branch above it calls `LoadLibrary` whatever `HAVE_LIBDL` says.
//      So the guarantee here is made where it can be made portably: **nothing
//      loadable is ever on a path Pd will search**, checked before
//      `libpd_openfile`, and a patch that asks for one with `[declare]` is
//      refused. Abstractions are unaffected -- Pd tries loaders first and
//      `sys_do_load_abs` only after they all fail, so refusing the file never
//      touches `adi.array.pd`.
//
// WHAT IS NOT HERE: patch discovery, parameters and published arrays. The
// parameter contract is ADR-0177's `[adi.param]`; published arrays are
// ADR-0183's `[adi.array]`. Both sit on top of this and neither changes it.

#pragma once

#include "juce/pd_device.hpp"
#include "juce/pd_declarations.hpp"
#include "adi/engine/published_array.hpp"
#include "adi/engine/mpe_output.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <memory>
#include <vector>

namespace adi::device {

/// The file extensions Pd will try to load as native code, as a superset.
///
/// Pd builds its own list at runtime from the platform and a deken specifier
/// (`sys_get_dllextensions`), and that list is neither exported nor settable.
/// So this one is deliberately WIDER than any single platform's: the cost of
/// refusing a file that this platform's Pd would not have loaded is a patch
/// that has to be tidied, and the cost of missing one is native code in the
/// process. It is also why the check is on the extension and not on the file's
/// contents -- a file Pd would attempt is a file Pd must not find, whether or
/// not it turns out to be a valid library.
[[nodiscard]] const std::vector<std::string>& pdLoadableCodeExtensions();

/// Names of files in `dir` that Pd would attempt to load as native code.
///
/// Empty is the answer for every directory the DAW writes itself. A
/// non-empty one is a directory no patch may be opened from.
[[nodiscard]] std::vector<std::string> pdLoadableCodeIn(const std::string& dir);

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
    ///
    /// **Every path given here is checked for loadable code in `open`**, and
    /// so is the patch's own directory. Pd resolves externals through exactly
    /// these paths (note 4), so a path the DAW would not vouch for is a path
    /// that must not be added.
    void addSearchPath(const std::string& dir);

    /// What Pd printed for this instance, newest last, capped.
    ///
    /// Pd reports almost everything through its console and nothing else:
    /// "couldn't create" for an object it cannot make, the filename and the
    /// loader's error for a library it tried and failed to open. Without this
    /// the engine's only report is the return value of `open`, and a patch
    /// that opened with half its objects missing looks exactly like one that
    /// opened whole. **Message thread only** -- Pd prints while opening and
    /// while sending, never from `process`.
    [[nodiscard]] std::vector<std::string> consoleLines() const;

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

    /// Message thread, after `open`. Resolves the receive symbol for every
    /// parameter the patch declares, so that `sendParameter` can reach it from
    /// the audio thread without ever calling `gensym`.
    ///
    /// **ADR-0188 d3 says modulation is sent into `[adi.param]` on the audio
    /// thread, before the block, and only to names the patch declared.** That
    /// second half is not a style rule, and `libpd_float` is why: it is
    /// `gensym(name)->s_thing` behind a `sys_lock()`, so sending by name from
    /// the audio thread takes a MUTEX and, for a name Pd has not seen,
    /// ALLOCATES -- and it allocates even in the failing case, because the
    /// symbol is created before the null `s_thing` is noticed. Both were read
    /// in `z_libpd.c` at the pinned commit.
    ///
    /// A `t_symbol*` never moves and is never freed, so resolving once here
    /// and reading `s_thing` later is safe as well as cheap.
    void bindParameters(const PdDeclarations& decls);

    /// What `[adi.transport $0]` receives -- ADR-0188 d3's field list exactly.
    ///
    /// **NO ABSOLUTE TICK COUNT, and that is the whole shape of it.** Pd's
    /// numbers are 32-bit floats, exact for integers only to 2^24 =
    /// 16,777,216, and ADI's 5,765,760 ticks per quarter note (SPEC 4.2) pass
    /// that within three quarter notes. Bar, beat and ticks-within-the-quarter
    /// are each small enough to stay exact for as long as anyone plays.
    struct Transport {
        bool playing = false;
        double bpm = 120.0;
        int timeSigNumerator = 4;
        int timeSigDenominator = 4;
        /// 1-based, as a musician counts them.
        double bar = 1.0;
        double beat = 1.0;
        /// **The QUARTER-NOTE grid, not the position within the beat**, and in
        /// 5/8 or 7/8 those are different numbers. It is the absolute tick
        /// count modulo `ADI_PPQ` (5,765,760), counted from bar 1, so it runs
        /// 0..5,765,759 and wraps once per quarter note whatever the meter
        /// calls a beat. In 7/8, where the beat is an eighth, it wraps once
        /// per TWO beats. `bar` and `beat` carry the meter; this carries a
        /// quarter-note phase, and a patch that confuses them is early or late
        /// by a factor of two with nothing to say why.
        double ticksInQuarter = 0.0;
    };

    /// **THE ENGINE'S HALF IS win_codex's, IN FLIGHT.** `NodeIo` gains
    /// `io.transport`: null outside a Session, valid only for the duration of
    /// `process`, carrying block-start values for every segment. Once it lands
    /// the Pd device node calls this from `io.transport` and nothing else
    /// changes -- everything from here to the patch is built and proved
    /// already. Until then a caller sets it directly, which is also how the
    /// test drives it.
    ///
    /// Audio thread or message thread, before `process`. Sent into the patch
    /// at the start of each segment, with the MIDI and before the audio.
    void setTransport(const Transport& t) noexcept;

    /// Message thread, before `prepare`. Which MIDI a patch's `[notein]`,
    /// `[ctlin]` and `[bendin]` receive -- ADR-0194.
    ///
    /// **THE ENGINE HAS NO MIDI TO FORWARD, AND THAT IS ADR-0054 WORKING AS
    /// INTENDED.** "No MIDI byte survives" the input parser: bytes go in and
    /// `engine::Event` comes out, with every expression value a double,
    /// because 7-bit and 14-bit are encodings of a control surface rather than
    /// properties of the music. There is no CC event and no pitch-bend event
    /// to hand to Pd.
    ///
    /// So a Pd patch is an OUTPUT EDGE, like a VST3 plugin, and it is fed by
    /// the encoder that edge already has: `MpeRouter` (ADR-0097). Nothing here
    /// re-encodes anything -- the quantisation a patch sees is the same one a
    /// JUCE-built MPE synth sees, decided once and tested once.
    ///
    /// `MpeMidi` is the default because it is the only route that delivers
    /// what a patch asks for: notes on member channels, per-note pitch bend on
    /// `[bendin]`, timbre as CC74 on `[ctlin]`, pressure as channel pressure.
    /// `Plain` puts every note on channel 1 and DROPS pitch and timbre
    /// (counted), so a `[bendin]` under it would simply never fire.
    void setExpressionRoute(engine::ExpressionRoute route) noexcept;

    /// Events the router had nowhere to put, and MIDI that overflowed the
    /// per-segment scratch. Both are counted rather than silent, as the engine
    /// counts elsewhere.
    [[nodiscard]] std::int64_t midiDropped() const noexcept { return midiDropped_; }
    /// The router itself, for the counters it keeps: dropped dimensions,
    /// shared member channels, unknown notes.
    [[nodiscard]] const engine::MpeRouter& midiRouter() const noexcept { return midiRouter_; }

    /// **AUDIO THREAD**, before `process` for the same block (ADR-0188 d3).
    /// Allocates nothing, takes no lock, and calls no `gensym`.
    ///
    /// False when the id was not declared, or when the patch has no receiver
    /// of that name -- which is the ordinary case for a patch that declares a
    /// parameter and does not connect it, and is not an error.
    bool sendParameter(std::int32_t id, float value) noexcept;

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

    /// Message thread. One line from Pd's console, capped at
    /// `kMaxConsoleLines` -- a patch in an error loop must not grow a vector
    /// without bound.
    void appendConsole(const char* line);
    /// libpd's print hook. A bare function pointer with no user data, so like
    /// the float hook it asks `libpd_this_instance()` who it is printing for.
    /// A static member rather than a free function because it is the one thing
    /// outside the class that has to reach `appendConsole`.
    static void printHookThunk(const char* line);

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

    /// Pd's console for this instance, filled by the print hook.
    mutable std::mutex consoleMutex_;
    std::vector<std::string> console_;
    std::string pending_;            ///< a line Pd has begun and not ended
    static constexpr std::size_t kMaxConsoleLines = 256;

    /// AUDIO THREAD. One routed MIDI event into this instance's Pd, through
    /// `inmidi_*` rather than `libpd_*`: the libpd entry points take
    /// `sys_lock()` on every call, which is the same reason `sendParameter`
    /// does not use `libpd_float`.
    void deliverMidi(const engine::MpeOut& m) noexcept;

    /// AUDIO THREAD. One list to `<$0>-aditr`, built on the stack.
    void deliverTransport() noexcept;

    Transport transport_{};
    bool transportSet_ = false;
    /// The receive symbol, resolved once in `open` for `sendParameter`'s
    /// reason: `gensym` on the audio thread allocates for an unseen name.
    void* transportSymbol_ = nullptr;

    /// Engine events -> MIDI, per device and stateful: a member channel is
    /// allocated at note-on and released at note-off.
    engine::MpeRouter midiRouter_;
    engine::ExpressionRoute midiRoute_ = engine::ExpressionRoute::MpeMidi;
    /// Sized in `prepare`. One segment's worth, never grown on the audio
    /// thread; overflow is counted.
    std::vector<engine::MpeOut> midiScratch_;
    std::int64_t midiDropped_ = 0;

    /// Declared parameter id -> its receive `t_symbol*`, resolved once on the
    /// message thread. `void*` because this header does not include m_pd.h.
    std::vector<std::pair<std::int32_t, void*>> paramSymbols_;

    /// Paths asked for before the instance existed, applied in `open`.
    std::vector<std::string> searchPaths_;

    /// This instance's routing table. Per engine rather than shared, because
    /// `$0` collides across instances -- see PdRuntime.
    PdReceiverTable receivers_;

    std::atomic<std::int64_t> ticks_{0}, segments_{0}, starved_{0};
};

}  // namespace adi::device
