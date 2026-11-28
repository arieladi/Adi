// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/transport.hpp"
#include "adi/ui/panel_layout.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
namespace adi {
class Store;
}
namespace adi::ui {
struct DesktopDefaults {
    // Live 11's widths at 100 %: the browser with the frame's margin, and the track
    // header's In/Out and mixer columns with the meter and the right-hand strip,
    // 78 + 116 + 27 (measured 2026-10-27; ui_mixer.hpp's MixerPanel::kFullWidth).
    int browser = 316, mixer = 221;
    // The device strip's fixed minimum: Live's Device View at 100 % interface zoom
    // (ADR-0203 d7). Measured on Live 11.2.7, the director's Mac, 2026-10-27: the
    // whole Device View panel is 196 points with its 4 pt edges, its device body
    // under the title bars about 170, which is the director's 169. ADI's strip is the
    // whole panel, so its floor is 196, to be read again on Live 12 (ADR-0203 d26). A desktop default
    // handed in by the shell, never a constant in a component (ADR-0200 d5).
    int deviceFloor = 196;
};
enum class InfoAnalyserMode { Spectrogram, FilledSpectrum, LineSpectrum, ClassicScope };

struct WindowState {
    explicit WindowState(DesktopDefaults d = {})
        : panels({Panel::Browser, d.browser, 120, false}, {Panel::Mixer, d.mixer, 120, false}, 240),
          deviceHeight(d.deviceFloor), deviceFloor(d.deviceFloor) {}
    PanelLayout panels;
    double zoom = 1; // the arrangement's own zoom; interface zoom is the window's
    int deviceHeight;
    int deviceFloor; // from DesktopDefaults, never persisted
    bool docked = true;
    // Live's Clip View (manual 8; STEP-7-PLAN 7.7): in the detail row above the Device View, hidden
    // until a clip is opened; and its height, which its top edge drags.
    bool clipView = false;
    int clipViewHeight = 286; // room for an octave of key tracks under the stretch markers' strip, above the Velocity lane and its controls
    // The Note Editor's own settings, kept with the window: its editing grid (manual 10.2.1),
    // Highlight Scale (10.6), what its notes are coloured by (ADR-0242 d1), and the Quantize
    // settings (10.5.11) with ADI's swing.
    bool noteGridSnap = true, noteGridFixed = true, noteGridTriplet = false;
    int noteGridNarrow = 0;
    double noteGridFixedQuarters = .25;
    bool highlightScale = true;
    int noteColours = 0;
    int noteLaneHeight = 56; // the Velocity lane, as its divider left it
    int noteSpelling = 0;    // sharps, flats, both, auto, numbers (manual 10.6)
    bool clipPanelsFolded = false; // the Clip View's panels folded to a strip (manual 8.1.1)
    int clipPanelsArrangement = 0; // by the view's height, side by side, stacked (manual 8.1.2)
    int quantizeValue = 0, quantizeAmount = 100, quantizeSwing = 0;
    bool quantizeStarts = true, quantizeEnds = false;
    bool aiChatOpen = false; // legacy state from before removal of the Browser's AI tab (SC-110)
    // The arrangement's own view: where it starts, its time zoom and its lane heights.
    // The Arrangement sheet spans 74 points per four bars. At this zoom the adaptive grid
    // opens on whole bars. Saved window states bring their own scale.
    double timelineLeft = 0, pixelsPerQuarter = 74. / 16;
    int laneHeight = 88; // Live 11's default track: five rows of its controls
    std::map<std::int64_t, int> laneHeights;
    struct AutomationChoice {
        std::string ownerKind;
        std::int64_t ownerId;
        std::string paramRef;
        bool operator==(const AutomationChoice &) const = default;
    };
    // Per-track arrangement chooser, identified by its saved envelope address.
    std::map<std::int64_t, AutomationChoice> automationChoices;
    std::set<std::int64_t> hiddenAutomationOverlays, collapsedAutomationLanes;
    std::set<std::int64_t> foldedDevices; // devices folded in the strip, per window
    bool mixerView = false;               // Live's Mixer View, hidden until asked for
    int mixerViewHeight = 220;
    // The Mix View (ADR-0209): open in place of the arrangement, and its tab, in the draft's order
    // (0 OVERVIEW, 1 STACK, 2 SCOPE & PHASE, 3 SPECTROGRAM, 4 CTZ CASCADE); STACK first (P1, d11).
    bool mixView = false;
    int mixViewTab = 1;
    // Where it opens: docked in the centre, or in a window of its own onto any display (ADR-0209
    // d11, ADR-0180 d3); the window's place is kept as every floating window's.
    bool mixViewDetached = false;
    // STACK search and resting row size belong to this window. Pins belong to the project.
    std::string mixStackSearch;
    std::optional<int> mixStackBaseHeight;
    // Optional right-side shared analyzer presentation; never audio parameters.
    bool mixAnalyzerDock = false;
    int mixAnalyzerWidth = 300, mixAnalyzerSource = 0, mixAnalyzerPoint = 0, mixAnalyzerChannel = 0;
    InfoAnalyserMode infoAnalyserMode = InfoAnalyserMode::Spectrogram;
    std::string analysisPalette = "adi";
    bool analysisPaletteReverse = false;
    std::string analysisSpectrumSlope = "flat";
    // The ★ Reference track (ADR-0209 §4, d11): hidden from the project view until its
    // [Reference Track] toggle shows it.
    bool showReference = false;
    // Live's Hide/Show Return Tracks (§18.4, the View menu's Mixer Controls, Ctrl/Cmd+Alt+R): the
    // returns left out of the lanes, the track controls and the Mixer View; shown by default.
    bool hideReturns = false;
    // Live's Follow (manual §6.2): the arrangement keeps the playhead on screen while it plays.
    static constexpr bool kDefaultFollow = false;
    bool follow = kDefaultFollow;
    // Live's View menu (§42.1): the Overview above the ruler, and the track controls' In/Out
    // (Ctrl/Cmd+Alt+I) and sends (Hide/Show Sends, Ctrl/Cmd+Alt+S): Live 12's Arrangement Track
    // Controls submenu.
    bool overview = true, inOut = true, sends = true;
    bool trackMixer = true; // Arrangement Track Controls' volume, pan and meters
    // Live 12's Mixer Controls submenu (and the Mixer View Control's options menu): the Mixer View's
    // own In/Out section, hidden until shown, and its own Sends, apart from the track controls'.
    bool mixerInOut = false, mixerSends = true;
    // Live's Track Options (manual 18.7): the Track Delay field, hidden until shown; the track
    // controls' (the D selector, the Arrangement Track Controls submenu) and the Mixer View's own
    // (the Mixer Controls submenu and the Mixer View Control's menu), apart as In/Out and Sends are.
    bool trackOptions = false, mixerTrackOptions = false;
    // Bitwig's Use Large Track Height (Master Reference 10): off, every track at half height.
    bool largeTracks = true;
    // Bitwig's project view (Master Reference 10, ADR-0112 d2): the group this window shows,
    // with everything inside it and Main; 0 for the whole project.
    std::int64_t viewGroup = 0;
    // Live's group tracks (manual 18.3): the groups folded in this window's arrangement. Folding
    // hides what a group holds from the lanes, never from the Mixer View (UI-ARCHITECTURE §4).
    std::set<std::int64_t> foldedGroups;
    std::set<std::int64_t> foldedTracks; // compact ordinary tracks, per window
    // The arrangement's editing grid (§6.10), as ArrangementGeometry::Grid holds it.
    bool gridSnap = true, gridFixed = false, gridTriplet = false;
    int gridNarrow = 0;
    double gridFixedQuarters = .25;
    // The strip expands upward only: a height below the floor becomes the floor.
    void setDeviceHeight(int h) noexcept { deviceHeight = h < deviceFloor ? deviceFloor : h; }
};
struct FloatingState {
    std::string monitor;
    int x=100,y=100,width=800,height=420;
    bool open=false;
    // WIN-02: the monitor's area and DPI when it was saved (0 for a record from before them), so the
    // rule can find the monitor again and restore in proportion.
    int areaX=0,areaY=0,areaWidth=0,areaHeight=0,dpi=0;
};
class ViewStateStore {
  public:
    explicit ViewStateStore(Store &s) : store_(s) {}
    FloatingState loadFloating(const std::string& key) const;
    bool saveFloating(const std::string& key,const FloatingState&,std::string& error);
    WindowState load(const std::string &window, DesktopDefaults defaults = {}) const;
    bool save(const std::string &window, const WindowState &, std::string &error);
    struct StackPins {
        std::uint64_t revision = 1;
        std::vector<std::int64_t> ids;
    };
    const StackPins &stackPins() const;
    bool saveStackPins(std::span<const std::int64_t> before, std::span<const std::int64_t> after, std::string &error);

  private:
    Store &store_;
    mutable std::optional<StackPins> stackPins_;
};
class DirtySet {
  public:
    enum Region : unsigned { Transport = 1, Layout = 2, Content = 4, Status = 8, All = 15 };
    void mark(unsigned bits) noexcept { bits_ |= bits; }
    unsigned drain() noexcept {
        const auto b = bits_;
        bits_ = 0;
        return b;
    }

  private:
    unsigned bits_ = All;
};
// One UI producer and one driver consumer. No UI ever reads/writes Transport.
class TransportMailbox {
  public:
    // Record (MX-09) starts playback as a recording start: the saved song start or the insert marker
    // (`sample` holds the insert marker's tick), after the saved Count-In.
    enum class Command { Play, Continue, Stop, Locate, LocateMarker, Record };
    bool hasRoom(std::size_t count = 1) const noexcept {
        return count <= queue_.size() &&
               write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) <=
                   queue_.size() - count;
    }
    bool post(Command c, std::int64_t sample = 0) noexcept {
        auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= queue_.size())
            return false;
        queue_[w % queue_.size()] = {c, sample};
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    void drain(engine::Transport &transport) noexcept {
        drain(transport, [&transport](std::int64_t) noexcept { transport.play(true); });
    }
    // `record(insertTick)` starts a recording: the driver's `RenderBlock::startPlayback(tick, true)`.
    template <class Record> void drain(engine::Transport &transport, Record &&record) noexcept {
        drain(transport, record, [](std::int64_t) noexcept { return true; });
    }
    template <class Record, class Ready>
    void drain(engine::Transport &transport, Record &&record, Ready &&ready) noexcept {
        auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        bool pending = playPending_.load(std::memory_order_relaxed);
        const auto locate = [&](std::int64_t sample) noexcept {
            transport.locate(sample);
            locateRevision_.store(transport.revision(), std::memory_order_relaxed);
        };
        while (r != w) {
            const auto item = queue_[r % queue_.size()];
            if (item.command == Command::Locate) {
                locate(item.sample);
            } else if (item.command == Command::LocateMarker) {
                if (!pending && !transport.playing() && transport.countInRemaining() == 0)
                    locate(item.sample);
            } else if (item.command == Command::Record) {
                pending = false;
                record(item.sample);
            } else if (item.command == Command::Stop) {
                if (transport.playing())
                    stoppedAt_ = transport.position();
                else if (!pending && transport.countInRemaining() == 0) {
                    // Stop while idle returns to the beginning for Continue as well as Play.
                    stoppedAt_ = 0;
                    locate(0);
                }
                pending = false;
                transport.play(false);
            } else if (!transport.playing()) {
                // A later Locate in this drain moves the pending Play target.
                transport.play(false);
                if (item.command == Command::Continue && stoppedAt_ && transport.position() != *stoppedAt_)
                    locate(*stoppedAt_);
                pending = true;
            }
            ++r;
        }
        if (pending && ready(transport.position())) {
            transport.play();
            pending = false;
        }
        publish(transport);
        playPending_.store(pending, std::memory_order_release);
        read_.store(r, std::memory_order_release);
    }
    void publish(const engine::Transport &t) noexcept {
        // A Count-In before a recording is playing for the window: Space or Stop then stops it.
        playing_.store(t.playing() || t.countInRemaining() > 0, std::memory_order_relaxed);
        position_.store(t.position(), std::memory_order_relaxed);
    }
    // Once after rendering, when the cursor has crossed all boundaries of this callback.
    void publish(const engine::Transport &t, engine::CountInPosition countIn) noexcept {
        publish(t);
        // The settings allow four bars, and the format allows 255 beats per bar.
        const auto packed = (static_cast<std::uint32_t>(-countIn.bar) << 16) |
                            (static_cast<std::uint32_t>(countIn.beat) << 8) |
                            static_cast<std::uint32_t>(countIn.sixteenth);
        countInPosition_.store(packed, std::memory_order_relaxed);
    }
    // Only the UI producer calls this. The read index acknowledges the
    // applied Locate, including its revision, before a capture can use it.
    [[nodiscard]] std::optional<std::uint64_t> settledLocateRevision() const noexcept {
        if (write_.load(std::memory_order_relaxed) != read_.load(std::memory_order_acquire))
            return std::nullopt;
        return locateRevision_.load(std::memory_order_relaxed);
    }
    // UI producer sees the last queued intent until the driver acknowledges it.
    bool desiredPlaying() const noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        for (auto i = w; i != r;) {
            const auto c = queue_[--i % queue_.size()].command;
            if (c != Command::Locate && c != Command::LocateMarker)
                return c == Command::Play || c == Command::Continue || c == Command::Record;
        }
        return playPending_.load(std::memory_order_acquire) || playing();
    }
    void setSampleRate(double rate) noexcept { sampleRate_.store(rate, std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    std::int64_t position() const noexcept { return position_.load(std::memory_order_relaxed); }
    [[nodiscard]] engine::CountInPosition countInPosition() const noexcept {
        const auto packed = countInPosition_.load(std::memory_order_relaxed);
        return {-static_cast<int>(packed >> 16), static_cast<int>((packed >> 8) & 0xff),
                static_cast<int>(packed & 0xff)};
    }

  private:
    // Driver-owned. Idle selections can move the displayed position after a real Stop.
    std::optional<std::int64_t> stoppedAt_;
    std::atomic<bool> playPending_{false};
    struct Item {
        Command command = Command::Stop;
        std::int64_t sample = 0;
    };
    std::array<Item, 16> queue_{};
    std::atomic<std::size_t> write_{0}, read_{0};
    std::atomic<bool> playing_{false};
    std::atomic<double> sampleRate_{0};
    std::atomic<std::int64_t> position_{0};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> countInPosition_{0};
    std::atomic<std::uint64_t> locateRevision_{0};
};
} // namespace adi::ui
