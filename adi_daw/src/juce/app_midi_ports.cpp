// SPDX-License-Identifier: AGPL-3.0-or-later
#include "app_midi_ports.hpp"
#include "adi/engine/midi_input_route.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <map>
#include <utility>
namespace adi::ui {
namespace ump = juce::universal_midi_packets;
namespace {
// Inputs first, then outputs, each by name ignoring case, as Live's table lists them.
void sortPorts(std::vector<MidiPortHost::Port> &ports) {
    std::stable_sort(ports.begin(), ports.end(), [](const MidiPortHost::Port &a, const MidiPortHost::Port &b) {
        if (a.input != b.input)
            return a.input;
        return juce::String(a.name).compareIgnoreCase(juce::String(b.name)) < 0;
    });
}
} // namespace
// One open input: its connection, its ring, and the consumer JUCE calls on the port's own thread.
struct MidiPortHost::Input final : ump::Consumer {
    Input(MidiPortHost &h, std::uint32_t s, std::string i) : host(h), slot(s), id(std::move(i)) {}
    void consume(ump::Iterator b, ump::Iterator e, double time) override {
        for (auto it = b; it != e; ++it) {
            const auto view = *it;
            MidiPortEvent event;
            if (view.size() == 0 || !midiFromUmpWord(view[0], event))
                continue;
            event.timeMs = time * 1000.0; // JUCE's UMP backends pass seconds
            event.port = slot;
            if (!ring.push(event))
                host.dropped_.fetch_add(1, std::memory_order_relaxed);
            host.activity_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    MidiPortHost &host;
    std::uint32_t slot;
    std::uint64_t serial = 0; // this opening, for the feed (never 0)
    std::string id;
    ump::Input connection;
    MidiEventRing<1024> ring;
};
// Connections are opened and destroyed on the message thread. Only this
// worker and the message thread can access them, never the audio callback.
struct MidiPortHost::Output {
    struct Channel {
        std::bitset<128> notes;
        std::bitset<3> pedals;
    };
    std::string id;
    std::uint64_t key;
    OutputConnection connection;
    std::map<std::pair<std::int64_t, std::uint32_t>, Channel> channels;

    bool send(std::int64_t track, std::uint32_t word) {
        if (word == engine::kMidiHardwareRelease) {
            release(track);
            return false;
        }
        if (!connection.send(word)) return false;
        const auto status = (word >> 20) & 0x0fu;
        const auto channel = (word >> 16) & 0x0fu;
        const auto data = (word >> 8) & 0x7fu;
        const auto value = word & 0x7fu;
        if (status == 9 && value != 0) {
            channels[{track, channel}].notes.set(data);
        } else if (status == 8 || (status == 9 && value == 0)) {
            if (const auto found = channels.find({track, channel}); found != channels.end())
                found->second.notes.reset(data);
        } else if (status == 11) {
            static constexpr std::array<std::uint32_t, 3> pedals{64, 66, 69};
            if (const auto pedal = std::find(pedals.begin(), pedals.end(), data); pedal != pedals.end())
                channels[{track, channel}].pedals.set(static_cast<std::size_t>(pedal - pedals.begin()), value >= 64);
        }
        return true;
    }

    void release(std::optional<std::int64_t> track = {}) {
        for (auto it = channels.begin(); it != channels.end();) {
            if (track && it->first.first != *track) {
                ++it;
                continue;
            }
            const auto channel = it->first.second << 16;
            if (connection.send) {
                static constexpr std::array<std::uint32_t, 3> pedals{64, 66, 69};
                for (std::size_t i = 0; i < pedals.size(); ++i)
                    if (it->second.pedals.test(i))
                        connection.send(0x20b00000u | channel | (pedals[i] << 8));
                for (std::uint32_t note = 0; note < 128; ++note)
                    if (it->second.notes.test(note))
                        connection.send(0x20800000u | channel | (note << 8));
            }
            it = channels.erase(it);
        }
    }
};
MidiPortHost::MidiPortHost(std::function<settings::Value()> ports, Writer write, NewInputTrack newInputTrack)
    : read_(std::move(ports)), write_(std::move(write)), newInputTrack_(std::move(newInputTrack)),
      session_(ump::Endpoints::getInstance()->makeSession("ADI")) {
    // The ports are listed and opened through one session, the application's.
    ump::Endpoints::getInstance()->addListener(*this);
    refresh();
}
MidiPortHost::MidiPortHost(std::function<settings::Value()> ports, std::vector<Port> fixed, Writer write,
                           NewInputTrack newInputTrack, OpenOutput openOutput)
    : read_(std::move(ports)), write_(std::move(write)), newInputTrack_(std::move(newInputTrack)),
      fixed_(true), fixedPorts_(std::move(fixed)), fixedOutput_(std::move(openOutput)) {
    refresh();
}
MidiPortHost::~MidiPortHost() {
    setOutputFeed(nullptr);
    for (auto& output : outputs_) output->release();
    outputs_.clear();
    if (!fixed_)
        ump::Endpoints::getInstance()->removeListener(*this);
    for (auto &input : inputs_)
        if (input && input->connection.isAlive())
            input->connection.removeConsumer(*input);
    if (feed_)
        feed_->publish({}); // the audio thread lets go of every ring before they go
    inputs_.clear();
}
std::string MidiPortHost::backend() const {
    const auto backend = ump::Endpoints::getInstance()->getBackend();
    if (!backend)
        return {};
    switch (*backend) {
    case ump::Backend::alsa: return "ALSA";
    case ump::Backend::android: return "Android";
    case ump::Backend::coremidi: return "Core MIDI";
    case ump::Backend::winmm: return "Win32";
    case ump::Backend::winrt: return "WinRT";
    case ump::Backend::wms: return "Windows MIDI Services";
    }
    return {};
}
void MidiPortHost::publish() {
    // The open inputs, each with the switches its record holds now (N60).
    std::vector<MidiInputFeed::Source> sources;
    for (const auto &input : inputs_) {
        if (!input)
            continue;
        const auto port = std::find_if(ports_.begin(), ports_.end(), [&](const Port &p) { return p.input && p.id == input->id; });
        if (port == ports_.end())
            continue;
        sources.push_back({&input->ring, input->slot, port->switches.track, port->switches.remote, port->switches.mpe,
                           input->serial, engine::midiInputKey(port->id)});
    }
    if (feed_)
        feed_->publish(sources);
}
void MidiPortHost::setFeed(MidiInputFeed *feed) {
    if (feed_ && feed_ != feed)
        feed_->publish({});
    feed_ = feed;
    publish();
}
void MidiPortHost::setOutputFeed(MidiOutputFeed* feed) {
    if (feed == outputFeed_) return;
    if (outputWorker_.joinable()) {
        outputWorker_.request_stop();
        outputWorker_.join();
    }
    outputFeed_ = feed;
    if (feed) {
        feed->reset();
        outputWorker_ = std::jthread([this](std::stop_token stop) { runOutput(stop); });
    }
}

bool MidiPortHost::refreshOutputs(std::vector<Port>& listed, const OpenOutput& open) {
    std::lock_guard guard(outputMutex_);
    bool changed = false;
    const auto onePortWithKey = [&](std::uint64_t key) {
        return std::count_if(listed.begin(), listed.end(), [key](const Port& port) {
            return port.output && engine::midiPortKey(port.id) == key;
        }) == 1;
    };
    std::erase_if(outputs_, [&](const auto& output) {
        const auto found = std::find_if(listed.begin(), listed.end(),
            [&](const Port& port) { return port.output && port.id == output->id; });
        if (found != listed.end() && found->switches.track && onePortWithKey(output->key) &&
            (!output->connection.alive || output->connection.alive())) return false;
        output->release();
        changed = true;
        return true;
    });
    for (auto& port : listed) {
        if (!port.output || !port.switches.track || !open) continue;
        const auto key = engine::midiPortKey(port.id);
        const auto existing = std::find_if(outputs_.begin(), outputs_.end(),
            [key](const auto& output) { return output->key == key; });
        if (existing != outputs_.end()) {
            port.open = (*existing)->id == port.id;
            continue;
        }
        if (!onePortWithKey(key)) continue;
        auto connection = open(port);
        if (!connection.send || (connection.alive && !connection.alive())) continue;
        auto output = std::make_unique<Output>(Output{port.id, key, std::move(connection), {}});
        outputs_.push_back(std::move(output));
        port.open = true;
        changed = true;
    }
    if (changed && outputFeed_) outputFeed_->reset();
    return changed;
}

void MidiPortHost::runOutput(std::stop_token stop) {
    using namespace std::chrono_literals;
    std::vector<MidiOutputFeed::Packet> pending;
    pending.reserve(4096);
    auto revision = outputFeed_->revision();
    while (!stop.stop_requested()) {
        for (MidiOutputFeed::Packet packet; outputFeed_->pop(packet);) {
            if (pending.size() == 4096) {
                outputFeed_->reset();
                break;
            }
            pending.push_back(packet);
        }
        const auto current = outputFeed_->revision();
        if (current != revision) {
            revision = current;
            std::lock_guard guard(outputMutex_);
            for (auto& output : outputs_) output->release();
        }
        std::erase_if(pending, [revision](const auto& packet) { return packet.epoch != revision; });
        if (!pending.empty()) {
            std::stable_sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) {
                return a.dueMs < b.dueMs;
            });
            const auto now = juce::Time::getMillisecondCounterHiRes();
            const auto due = std::upper_bound(pending.begin(), pending.end(), now,
                [](double time, const auto& packet) { return time < packet.dueMs; });
            if (due != pending.begin()) {
                std::lock_guard guard(outputMutex_);
                for (auto packet = pending.begin(); packet != due; ++packet) {
                    if (revision != outputFeed_->revision()) break;
                    const auto port = std::find_if(outputs_.begin(), outputs_.end(),
                        [key = packet->port](const auto& output) { return output->key == key; });
                    if (port != outputs_.end() && (*port)->send(packet->track, packet->word))
                        outputActivity_.fetch_add(1, std::memory_order_relaxed);
                }
                pending.erase(pending.begin(), due);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    std::lock_guard guard(outputMutex_);
    for (auto& output : outputs_) output->release();
}
void MidiPortHost::endpointsChanged() { refresh(); }
// The record, brought up to date with the ports listed now (N61) when a writer takes it.
settings::Value MidiPortHost::reconciled(const std::vector<Port> &listed) {
    auto record = read_ ? read_() : settings::Value::object();
    std::vector<MidiPortListing> names;
    names.reserve(listed.size());
    for (const auto &p : listed)
        names.push_back({p.id, p.name});
    std::optional<bool> initialTrack;
    if (write_ && newInputTrack_)
        initialTrack = newInputTrack_();
    if (auto healed = reconcileMidiPorts(record, names, initialTrack); healed && write_ && write_(*healed))
        record = std::move(*healed);
    return record;
}
void MidiPortHost::refresh() {
    if (fixed_) {
        auto listed = fixedPorts_;
        const auto record = reconciled(listed);
        for (auto &p : listed)
            p.switches = midiPortSwitches(record, p.id);
        sortPorts(listed);
        const bool outputsChanged = refreshOutputs(listed, fixedOutput_);
        const bool changed = outputsChanged || listed.size() != ports_.size() ||
                             !std::equal(listed.begin(), listed.end(), ports_.begin(), [](const Port &a, const Port &b) {
                                 return a.id == b.id && a.switches == b.switches;
                             });
        ports_ = std::move(listed);
        if (changed && onChange)
            onChange();
        return;
    }
    auto *endpoints = ump::Endpoints::getInstance();
    std::vector<Port> listed;
    std::vector<ump::EndpointId> sources, destinations;
    for (const auto &id : endpoints->getEndpoints()) {
        const auto info = endpoints->getEndpoint(id);
        if (!info)
            continue;
        const auto name = info->getName().toStdString();
        // A port is listed once per direction, keyed as the Settings key it: its source identifier
        // for an input, its destination identifier for an output.
        if (id.src.isNotEmpty()) {
            Port p;
            // Keyed by direction: on ALSA, Android and WinRT a two-way port's source and destination share
            // one identifier, and Live's table switches each direction on its own.
            p.id = "in:" + id.src.toStdString();
            p.name = name;
            p.input = true;
            listed.push_back(std::move(p));
            sources.push_back(id);
        }
        if (id.dst.isNotEmpty()) {
            Port p;
            p.id = "out:" + id.dst.toStdString();
            p.name = name;
            p.output = true;
            listed.push_back(std::move(p));
            destinations.push_back(id);
        }
    }
    const auto record = reconciled(listed);
    for (auto &p : listed)
        p.switches = midiPortSwitches(record, p.id);
    sortPorts(listed);
    bool changed = listed.size() != ports_.size();
    bool changedInputConnections = false;
    // Close every input that is gone or switched off; open those switched on that are not open. A
    // closed input's ring stays until the feed has been told, so the audio thread never reads it freed.
    std::vector<std::unique_ptr<Input>> retired;
    for (auto &input : inputs_) {
        if (!input)
            continue;
        const auto it = std::find_if(listed.begin(), listed.end(), [&](const Port &p) { return p.input && p.id == input->id; });
        const bool wanted = it != listed.end() && (it->switches.track || it->switches.remote);
        if (!wanted || !input->connection.isAlive()) {
            if (input->connection.isAlive())
                input->connection.removeConsumer(*input);
            retired.push_back(std::move(input));
            changed = changedInputConnections = true;
        }
    }
    for (auto &p : listed) {
        if (!p.input)
            continue;
        const auto open = std::find_if(inputs_.begin(), inputs_.end(), [&](const auto &i) { return i && i->id == p.id; });
        if (open != inputs_.end()) {
            p.open = true;
            continue;
        }
        if (!p.switches.track && !p.switches.remote)
            continue;
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const ump::EndpointId &id) {
            return "in:" + id.src.toStdString() == p.id;
        });
        auto connection = session_->connectInput(*source, ump::PacketProtocol::MIDI_1_0);
        if (!connection.isAlive())
            continue;
        // A free slot keeps the open ones' indexes.
        auto slot = std::find(inputs_.begin(), inputs_.end(), nullptr);
        if (slot == inputs_.end()) {
            inputs_.push_back(nullptr);
            slot = inputs_.end() - 1;
        }
        auto input = std::make_unique<Input>(*this, static_cast<std::uint32_t>(slot - inputs_.begin()), p.id);
        input->serial = ++serials_;
        input->connection = std::move(connection);
        input->connection.addConsumer(*input);
        *slot = std::move(input);
        p.open = true;
        changed = changedInputConnections = true;
    }
    const bool outputsChanged = refreshOutputs(listed, [this, &destinations](const Port& port) {
        const auto endpoint = std::find_if(destinations.begin(), destinations.end(), [&](const auto& id) {
            return "out:" + id.dst.toStdString() == port.id;
        });
        if (endpoint == destinations.end()) return OutputConnection{};
        auto output = std::make_shared<ump::Output>(session_->connectOutput(*endpoint));
        return OutputConnection{
            [output](std::uint32_t word) {
                const ump::Iterator begin(&word, 1);
                auto end = begin;
                ++end;
                return output->send(begin, end);
            },
            [output] { return output->isAlive(); }};
    });
    changed = changed || outputsChanged;
    if (!changed)
        for (std::size_t i = 0; i < listed.size(); ++i)
            if (listed[i].id != ports_[i].id || listed[i].name != ports_[i].name || listed[i].open != ports_[i].open ||
                listed[i].switches != ports_[i].switches) {
                changed = true;
                break;
            }
    const auto inputChanged = [&](const std::vector<Port>& a, const std::vector<Port>& b) {
        return std::any_of(a.begin(), a.end(), [&](const Port& port) {
            if (!port.input) return false;
            const auto previous = std::find_if(b.begin(), b.end(),
                [&](const Port& item) { return item.input && item.id == port.id; });
            return previous == b.end() || previous->open != port.open || previous->switches != port.switches;
        });
    };
    const bool changedInputs = inputChanged(listed, ports_) || inputChanged(ports_, listed);
    ports_ = std::move(listed);
    publish();
    retired.clear();
    if ((changedInputConnections || changedInputs) && onInputsChanged)
        onInputsChanged();
    if (changed && onChange)
        onChange();
}
MidiPortsTable::MidiPortsTable(MidiPortHost &host, settings::AppSettings &settings) : host_(host), settings_(settings) {
    rebuild();
}
MidiPortsTable::~MidiPortsTable() = default;
void MidiPortsTable::set(const std::string &id, const char *field, bool on) {
    std::string why;
    const auto &ports = host_.ports();
    const auto port = std::find_if(ports.begin(), ports.end(), [&](const MidiPortHost::Port &p) { return p.id == id; });
    const auto record =
        withMidiPortSwitch(settings_.get("midi.ports"), id, field, on, port != ports.end() ? port->name : std::string());
    if (!settings_.set("midi.ports", record, {settings::Actor::User, {}, juce::Time::currentTimeMillis() * 1000}, why)) {
        error_ = why;
        rebuild(); // refused: the switches show what is kept
        return;
    }
    // In force from here, as every Settings change is; a file that could not be written says so.
    error_.clear();
    if (std::string unsaved; !settings_.save(unsaved))
        error_ = "Not saved: " + unsaved;
    host_.refresh();
    repaint();
}
void MidiPortsTable::rebuild() {
    rows_.clear();
    removeAllChildren();
    for (const auto &port : host_.ports()) {
        auto row = std::make_unique<Row>();
        row->id = port.id;
        row->input = port.input;
        row->name = std::make_unique<juce::Label>(juce::String(), juce::String(port.input ? "Input: " : "Output: ") +
                                                                     juce::String::fromUTF8(port.name.c_str()));
        row->name->setTooltip(juce::String::fromUTF8(port.id.c_str()));
        addAndMakeVisible(*row->name);
        const bool on[] = {port.switches.track, port.switches.sync, port.switches.remote, port.switches.mpe};
        for (std::size_t i = 0; i < kFields.size(); ++i) {
            auto toggle = std::make_unique<juce::ToggleButton>();
            toggle->setToggleState(on[i], juce::dontSendNotification);
            toggle->setTitle(juce::String(port.name) + " " + kFields[i]);
            toggle->onClick = [this, id = port.id, field = kFields[i], raw = toggle.get()] {
                // Deferred: `set` rebuilds the rows, this button among them.
                const bool state = raw->getToggleState();
                juce::Component::SafePointer<MidiPortsTable> self(this);
                post([self, id, field, state] {
                    if (self != nullptr)
                        self->set(id, field, state);
                });
            };
            addAndMakeVisible(*toggle);
            row->switches[i] = std::move(toggle);
        }
        rows_.push_back(std::move(row));
    }
    setSize(std::max(getWidth(), 320), kHeader + kRow * std::max<int>(1, static_cast<int>(rows_.size())) + 12);
    resized();
    repaint();
}
void MidiPortsTable::resized() {
    const int switchesLeft = std::max(160, getWidth() - kSwitch * static_cast<int>(kFields.size()) - 8);
    int y = kHeader;
    for (auto &row : rows_) {
        row->name->setBounds(8, y, switchesLeft - 12, kRow);
        for (std::size_t i = 0; i < kFields.size(); ++i)
            row->switches[i]->setBounds(switchesLeft + kSwitch * static_cast<int>(i) + kSwitch / 2 - 11, y, 22, kRow);
        y += kRow;
    }
}
void MidiPortsTable::paint(juce::Graphics &g) {
    g.setColour(findColour(juce::Label::textColourId));
    g.setFont(juce::FontOptions(12.f, juce::Font::bold));
    const int switchesLeft = std::max(160, getWidth() - kSwitch * static_cast<int>(kFields.size()) - 8);
    const auto backend = host_.backend();
    g.drawText(backend.empty() ? juce::String("MIDI Ports") : juce::String("MIDI Ports (") + backend + ")", 8, 0,
               switchesLeft - 12, kHeader, juce::Justification::centredLeft);
    static constexpr std::array<const char *, 4> kTitles{"Track", "Sync", "Remote", "MPE"};
    for (std::size_t i = 0; i < kTitles.size(); ++i)
        g.drawText(kTitles[i], switchesLeft + kSwitch * static_cast<int>(i), 0, kSwitch, kHeader,
                   juce::Justification::centred);
    if (rows_.empty())
        g.drawText("No MIDI port is connected. A port connected now appears here.", 8, kHeader,
                   getWidth() - 16, kRow, juce::Justification::centredLeft);
    if (!error_.empty())
        g.drawText(juce::String::fromUTF8(error_.c_str()), 8, getHeight() - 10, getWidth() - 16, 10,
                   juce::Justification::centredLeft, true);
}
} // namespace adi::ui
