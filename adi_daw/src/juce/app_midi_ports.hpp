// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MIDI port host (SRF-05; PLAN SRF-05, the window's `src/juce/app_midi_ports.*`): the application's one
// owner of the MIDI ports, on JUCE 9's Universal MIDI Packet API (`juce::ump`), with Windows MIDI
// Services off (the Win32 backend; SRF-16 takes the service).
//
// - It lists every port the system has, input and output, by name and identifier, and follows
//   hot-plug: a port that appears is listed, one that goes is closed (`EndpointsListener`).
// - An input whose Track or Remote switch is on (the `midi.ports` record of the Tempo & MIDI Settings)
//   is opened in the MIDI 1.0 protocol. What it sends is pushed, timestamped, into that port's own
//   lock-free ring on the port's input thread; one consumer drains all the rings (`drain`), never
//   waiting and never allocating. The engine's live MIDI input (ADR-0248 d6 item 4) is that consumer
//   once it is on main; until then the window reads the activity for the Control Bar's MIDI Indicators.
// - Track-enabled outputs connect on the message thread. The audio driver publishes
//   completed blocks to a bounded queue, and a worker sends their UMP packets.
// - `refresh` opens or closes ports on the message thread. Activity and dropped counters
//   may be read from any thread.
#pragma once
#include "adi/settings/registry.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/midi_input_feed.hpp"
#include "adi/ui/midi_output_feed.hpp"
#include "adi/ui/midi_ports.hpp"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
class MidiPortHost final : private juce::ump::EndpointsListener {
  public:
    // `ports` reads the `midi.ports` record as the Settings hold it now. `write`, when given, writes it back
    // when a listing brought it up to date (N61): each listed port's name kept with its record, and a record
    // moved to a port's new identifier; true when the Settings took it. `newInputTrack` reads the initial
    // Track switch for a new input. Without a writer the record is only read.
    using Writer = std::function<bool(const settings::Value &)>;
    using NewInputTrack = std::function<bool()>;
    explicit MidiPortHost(std::function<settings::Value()> ports, Writer write = {}, NewInputTrack newInputTrack = {});
    struct Port;
    struct OutputConnection {
        std::function<bool(std::uint32_t)> send;
        std::function<bool()> alive;
    };
    using OpenOutput = std::function<OutputConnection(const Port&)>;
    // For tests: a host over a fixed list of ports, their switches read from the record as the system's
    // are; it opens nothing and follows no hot-plug.
    MidiPortHost(std::function<settings::Value()> ports, std::vector<Port> fixed, Writer write = {},
                 NewInputTrack newInputTrack = {}, OpenOutput openOutput = {});
    ~MidiPortHost() override;
    MidiPortHost(const MidiPortHost &) = delete;
    MidiPortHost &operator=(const MidiPortHost &) = delete;
    struct Port {
        std::string id;   // the system's identifier: the key of its record in `midi.ports`
        std::string name; // as the system names it
        bool input = false, output = false;
        bool open = false; // a live input or Track-enabled output
        MidiPortSwitches switches;
    };
    // Every port the system lists, inputs first, each by name.
    const std::vector<Port> &ports() const noexcept { return ports_; }
    // The system's MIDI backend in words, as the Settings name it ("Win32" on Windows); empty when the
    // platform has no MIDI.
    std::string backend() const;
    // Open or close ports after hot-plug or a change in MIDI Settings.
    void refresh();
    // Called on the message thread after `refresh` changed what is listed or open.
    std::function<void()> onChange;
    // Called at once when the inputs the feed reads, or their switches, changed: the application
    // releases the notes held from them (`Session::resetMidiInput`), so a key held on a port closed,
    // unplugged or switched off does not sound on.
    std::function<void()> onInputsChanged;
    // The rings' one consumer (N60), owned by the application so it outlives the device callback that
    // pumps it: `refresh` publishes the open inputs to it, and the host's end takes them all back
    // before their rings go. None (tests): the rings fill and nothing drains them.
    void setFeed(MidiInputFeed *feed);
    void setOutputFeed(MidiOutputFeed *feed);
    std::uint64_t outputActivity() const noexcept { return outputActivity_.load(std::memory_order_relaxed); }
    // A count that moves each time an open input delivers a message (the MIDI Indicators).
    std::uint64_t activity() const noexcept { return activity_.load(std::memory_order_relaxed); }
    // Events lost because a ring was full (the consumer fell behind), since the host began: the input
    // callbacks count them in one atomic, so any thread may read it (DF-W36), and an input closed by
    // `refresh` keeps its count.
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    struct Input;
    struct Output;
    bool refreshOutputs(std::vector<Port>&, const OpenOutput&);
    void runOutput(std::stop_token);
    void endpointsChanged() override;
    settings::Value reconciled(const std::vector<Port> &listed);
    std::function<settings::Value()> read_;
    Writer write_;
    NewInputTrack newInputTrack_;
    bool fixed_ = false;
    std::vector<Port> fixedPorts_;
    OpenOutput fixedOutput_;
    std::optional<juce::ump::Session> session_; // none for a fixed list
    std::vector<Port> ports_;
    // Open inputs by slot; a slot's index is its events' `port`. A closed one stays null, so indexes hold.
    std::vector<std::unique_ptr<Input>> inputs_;
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<std::uint64_t> outputActivity_{0};
    std::mutex outputMutex_; // output worker and message thread only
    std::vector<std::unique_ptr<Output>> outputs_;
    MidiOutputFeed* outputFeed_ = nullptr;
    std::jthread outputWorker_;
    std::atomic<std::uint64_t> dropped_{0};
    MidiInputFeed *feed_ = nullptr;
    std::uint64_t serials_ = 0;
    void publish();
};
// Live's MIDI Ports table on the Tempo & MIDI Settings page (Live 12 manual 41.6): one row per port and
// direction, its Track, Sync, Remote and MPE switches. A switch is written to the `midi.ports` record at
// once and saved, and the host opens or closes the input. The look is the Settings' own controls until
// the Settings' sheet is agreed (ADR-0243 d1); this table adds content to the page.
class MidiPortsTable final : public juce::Component {
  public:
    MidiPortsTable(MidiPortHost &, settings::AppSettings &);
    ~MidiPortsTable() override;
    // The rows again from the host's ports: after a hot-plug or a switch.
    void rebuild();
    void resized() override;
    void paint(juce::Graphics &) override;
    static constexpr int kHeader = 20, kRow = 22, kSwitch = 64;
    static constexpr std::array<const char *, 4> kFields{"track", "sync", "remote", "mpe"};
    struct Row {
        std::string id;
        bool input = false;
        std::unique_ptr<juce::Label> name;
        std::array<std::unique_ptr<juce::ToggleButton>, 4> switches;
    };
    const std::vector<std::unique_ptr<Row>> &rows() const noexcept { return rows_; }
    // Runs a switch's write after its click has returned (it rebuilds the rows, the clicked button among
    // them): the message loop's queue, or a test's.
    std::function<void(std::function<void()>)> post = [](std::function<void()> job) {
        juce::MessageManager::callAsync(std::move(job));
    };
    // Why the last switch was not saved; empty when it was.
    const std::string &error() const noexcept { return error_; }

  private:
    void set(const std::string &id, const char *field, bool on);
    MidiPortHost &host_;
    settings::AppSettings &settings_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::string error_;
};
} // namespace adi::ui
