// SPDX-License-Identifier: AGPL-3.0-or-later
#include "app_midi_ports.hpp"
#include <algorithm>
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
            ring.push(event);
            host.activity_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    MidiPortHost &host;
    std::uint32_t slot;
    std::string id;
    ump::Input connection;
    MidiEventRing<1024> ring;
};
MidiPortHost::MidiPortHost(std::function<settings::Value()> ports)
    : read_(std::move(ports)), session_(ump::Endpoints::getInstance()->makeSession("ADI")) {
    // The ports are listed and opened through one session, the application's.
    ump::Endpoints::getInstance()->addListener(*this);
    refresh();
}
MidiPortHost::MidiPortHost(std::function<settings::Value()> ports, std::vector<Port> fixed)
    : read_(std::move(ports)), fixed_(true), fixedPorts_(std::move(fixed)) {
    refresh();
}
MidiPortHost::~MidiPortHost() {
    if (!fixed_)
        ump::Endpoints::getInstance()->removeListener(*this);
    for (auto &input : inputs_)
        if (input && input->connection.isAlive())
            input->connection.removeConsumer(*input);
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
std::uint64_t MidiPortHost::dropped() const noexcept {
    std::uint64_t n = 0;
    for (const auto &input : inputs_)
        if (input)
            n += input->ring.dropped();
    return n;
}
std::size_t MidiPortHost::drain(const std::function<void(const MidiPortEvent &)> &take) {
    std::size_t n = 0;
    MidiPortEvent e;
    for (auto &input : inputs_)
        if (input)
            while (input->ring.pop(e)) {
                if (take)
                    take(e);
                ++n;
            }
    return n;
}
void MidiPortHost::endpointsChanged() { refresh(); }
void MidiPortHost::refresh() {
    const auto record = read_ ? read_() : settings::Value::object();
    if (fixed_) {
        auto listed = fixedPorts_;
        for (auto &p : listed)
            p.switches = midiPortSwitches(record, p.id);
        sortPorts(listed);
        const bool changed = listed.size() != ports_.size() ||
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
    std::vector<ump::EndpointId> sources;
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
            p.switches = midiPortSwitches(record, p.id);
            listed.push_back(std::move(p));
            sources.push_back(id);
        }
        if (id.dst.isNotEmpty()) {
            Port p;
            p.id = "out:" + id.dst.toStdString();
            p.name = name;
            p.output = true;
            p.switches = midiPortSwitches(record, p.id);
            listed.push_back(std::move(p));
        }
    }
    sortPorts(listed);
    bool changed = listed.size() != ports_.size();
    // Close every input that is gone or switched off; open those switched on that are not open.
    for (auto &input : inputs_) {
        if (!input)
            continue;
        const auto it = std::find_if(listed.begin(), listed.end(), [&](const Port &p) { return p.input && p.id == input->id; });
        const bool wanted = it != listed.end() && (it->switches.track || it->switches.remote);
        if (!wanted || !input->connection.isAlive()) {
            if (input->connection.isAlive())
                input->connection.removeConsumer(*input);
            input.reset();
            changed = true;
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
        input->connection = std::move(connection);
        input->connection.addConsumer(*input);
        *slot = std::move(input);
        p.open = true;
        changed = true;
    }
    if (!changed)
        for (std::size_t i = 0; i < listed.size(); ++i)
            if (listed[i].id != ports_[i].id || listed[i].name != ports_[i].name || listed[i].open != ports_[i].open ||
                listed[i].switches != ports_[i].switches) {
                changed = true;
                break;
            }
    ports_ = std::move(listed);
    if (changed && onChange)
        onChange();
}
MidiPortsTable::MidiPortsTable(MidiPortHost &host, settings::AppSettings &settings) : host_(host), settings_(settings) {
    rebuild();
}
MidiPortsTable::~MidiPortsTable() = default;
void MidiPortsTable::set(const std::string &id, const char *field, bool on) {
    std::string why;
    const auto record = withMidiPortSwitch(settings_.get("midi.ports"), id, field, on);
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
