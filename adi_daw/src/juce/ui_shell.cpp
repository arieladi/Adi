// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_shell.hpp"
#include <algorithm>
namespace adi::ui {
AppCommands::AppCommands(const settings::AppSettings &settings) {
    manager.setFirstCommandTarget(this);
    reload(settings);
}
void AppCommands::reload(const settings::AppSettings &settings) {
    live_ = settings.get("shortcuts.preset") == "live";
    manager.registerAllCommandsForTarget(this);
    manager.getKeyMappings()->resetToDefaultMappings();
}
void AppCommands::getAllCommands(juce::Array<juce::CommandID> &ids) {
    for (int id = PlayStop; id <= ContinuePlay; ++id)
        ids.add(id);
}
void AppCommands::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo &info) {
    if (id < PlayStop || id > ContinuePlay) {
        info.setActive(false);
        return;
    }
    static const char *names[] = {"Play / stop",
                                  "Stop",
                                  "Undo",
                                  "Redo",
                                  "Swap side panels",
                                  "New project",
                                  "Open project",
                                  "Save",
                                  "Audio settings",
                                  "Zoom in",
                                  "Zoom out",
                                  "Fit selection",
                                  "Previous zoom",
                                  "Fit width",
                                  "Fit height",
                                  "Scroll left",
                                  "Scroll right",
                                  "Taller tracks",
                                  "Shorter tracks",
                                  "Go to start",
                                  "Scroll up",
                                  "Scroll down",
                                  "Continue playback"};
    info.setInfo(names[id - PlayStop], names[id - PlayStop], "ADI", 0);
    if (id == Undo || id == Redo) {
        info.setInfo(active_ ? juce::String(active_->undoTitle(id == Redo))
                             : juce::String(names[id - PlayStop]),
                     names[id - PlayStop], "ADI", 0);
        info.setActive(active_ && active_->canUndo(id == Redo));
    }
    const auto cmd = juce::ModifierKeys::commandModifier;
    switch (id) {
    case ZoomIn:
        info.addDefaultKeypress('+', 0);
        break;
    case ZoomOut:
        info.addDefaultKeypress('-', 0);
        break;
    case FitSelection:
        info.addDefaultKeypress('z', 0);
        break;
    case PreviousZoom:
        info.addDefaultKeypress('x', 0);
        break;
    case FitWidth:
        info.addDefaultKeypress('w', 0);
        break;
    case FitHeight:
        info.addDefaultKeypress('h', 0);
        break;
    case ContinuePlay:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, juce::ModifierKeys::shiftModifier);
        break;
    case GoStart:
        info.addDefaultKeypress(juce::KeyPress::homeKey, 0);
        break;
    case NewProject:
        info.addDefaultKeypress('n', cmd);
        break;
    case OpenProject:
        info.addDefaultKeypress('o', cmd);
        break;
    case SaveProject:
        info.addDefaultKeypress('s', cmd);
        break;
    case PlayStop:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
        break;
    case Stop:
        info.addDefaultKeypress(juce::KeyPress::escapeKey, 0);
        break;
    case Undo:
        info.addDefaultKeypress('z', cmd);
        break;
    case Redo:
#if JUCE_MAC
        info.addDefaultKeypress('z', cmd | juce::ModifierKeys::shiftModifier);
#else
        if (live_)
            info.addDefaultKeypress('y', cmd);
        else
            info.addDefaultKeypress('z', cmd | juce::ModifierKeys::shiftModifier);
#endif
        break;
    default:
        break;
    }
}
bool AppCommands::key(const juce::KeyPress &key, AdiRootComponent &root) {
    activate(root);
    return manager.getKeyMappings()->keyPressed(key, &root);
}
void AppCommands::activate(AdiRootComponent &root) {
    active_ = &root;
    manager.registerAllCommandsForTarget(this);
}
void AppCommands::detach(AdiRootComponent &root) {
    if (active_ == &root)
        active_ = nullptr;
}
bool AppCommands::perform(const InvocationInfo &info) {
    return active_ && active_->command(info.commandID);
}
TransportBar::TransportBar(AdiRootComponent &root) : root_(root) {
    addAndMakeVisible(play);
    addAndMakeVisible(stop);
    play.onClick = [this] { root_.command(AppCommands::PlayStop); };
    stop.onClick = [this] { root_.command(AppCommands::Stop); };
}
void TransportBar::resized() {
    play.setBounds(12, 8, 108, 28);
    stop.setBounds(128, 8, 64, 28);
}
void TransportBar::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff282c34));
    g.setColour(root_.playing() ? juce::Colour(0xff65cf91) : juce::Colour(0xff6a7280));
    g.fillRect(205, 16, 12, 12);
    g.setColour(juce::Colour(0xffe0e5ed));
    g.setFont(juce::FontOptions(14.f));
    g.drawText(juce::String(root_.bpm(), 2) + " BPM", 232, 8, 120, 28,
               juce::Justification::centredLeft);
    g.drawText("ADI", getWidth() - 80, 8, 64, 28, juce::Justification::centredRight);
}
AdiRootComponent::AdiRootComponent(engine::ProjectView &view, OpSubmitter &submitter,
                                   TransportMailbox &mailbox, AppCommands &commands,
                                   ViewStateStore &persistence, std::string window,
                                   DesktopDefaults defaults)
    : transport(*this), arrangement(*this), devices(*this), dockResizer(*this), view_(view),
      submitter_(submitter), mailbox_(mailbox), commands_(commands), persistence_(persistence),
      window_(std::move(window)), state_(persistence.load(window_, defaults)), defaults_(defaults) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(transport);
    addAndMakeVisible(arrangement);
    addAndMakeVisible(devices);
    addAndMakeVisible(dockResizer);
    reader_.emplace(view_.current());
    arrangement.geometry.left = state_.timelineLeft;
    arrangement.geometry.scale = state_.pixelsPerQuarter;
    arrangement.geometry.height = state_.laneHeight;
    arrangement.geometry.heights = state_.laneHeights;
    setSize(1200, 720);
    frame();
}
AdiRootComponent::~AdiRootComponent() { commands_.detach(*this); }
void AdiRootComponent::frame() noexcept {
    // Root owns exactly one reader for the presented frame; descendants borrow it.
    // JUCE may paint after this callback, so it remains alive until the next frame.
    reader_.emplace(view_.current());
    const auto generation = view_.generation();
    if (generation_ != generation) {
        generation_ = generation;
        dirty_.mark(DirtySet::All);
    }
    const bool playing = mailbox_.playing();
    const auto position = mailbox_.position();
    const double rate = mailbox_.sampleRate();
    if (playing != playing_ || position != position_ || rate != sampleRate_) {
        sampleRate_ = rate;
        playing_ = playing;
        position_ = position;
        dirty_.mark(DirtySet::Transport);
    }
    devices.frame(arrangement.geometry.selectedTrack);
    lastDrain_ = dirty_.drain();
    arrangement.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    if (lastDrain_ & DirtySet::Transport)
        transport.repaint();
    if (lastDrain_ & (DirtySet::Layout | DirtySet::Content))
        repaint();
}
double AdiRootComponent::bpm() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo ? tempo->bpmAt(
                       tempo->secondsToTicks(rate > 0 ? static_cast<double>(position_) / rate : 0))
                 : 120.;
}
std::int64_t AdiRootComponent::timelineTick() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo && rate > 0 ? tempo->secondsToTicks(static_cast<double>(position_) / rate) : 0;
}
bool AdiRootComponent::locate(std::int64_t tick) {
    // Landing reads the current cell, not a retained drag publication.
    const auto current = view_.current();
    const double rate = mailbox_.sampleRate() > 0 ? mailbox_.sampleRate() : current->sampleRate;
    if (!current->tempo || !mailbox_.hasRoom())
        return false;
    return mailbox_.post(TransportMailbox::Command::Locate,
                         static_cast<std::int64_t>(current->tempo->ticksToSeconds(tick) * rate));
}
void AdiRootComponent::resizeDevices(int height) {
    const int ceiling = std::max(0, getHeight() - 44 - 28 - state_.laneHeight);
    const int floor = std::min(ceiling, static_cast<int>(defaults_.deviceHeight * state_.zoom));
    state_.deviceHeight = std::clamp(height, floor, ceiling);
    resized();
}
void AdiRootComponent::resized() {
    transport.setBounds(0, 0, getWidth(), 44);
    const auto &slots = state_.panels.slots();
    const int left = std::min(state_.panels.occupied(slots[0]), getWidth() / 3);
    const int right = std::min(state_.panels.occupied(slots[1]), getWidth() / 3);
    const int dock =
        state_.docked ? std::min(state_.deviceHeight, std::max(0, getHeight() - 100)) : 0;
    devices.setBounds(0, getHeight() - dock, getWidth(), dock);
    dockResizer.setBounds(0, getHeight() - dock - 3, getWidth(), 6);
    dockResizer.toFront(false);
    arrangement.setBounds(left, 44, std::max(0, getWidth() - left - right),
                          std::max(0, getHeight() - 44 - dock));
    dirty_.mark(DirtySet::All);
}
void AdiRootComponent::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff171a20));
    const auto &slots = state_.panels.slots();
    // Preserve widths by panel identity. If the window shrinks, share the
    // lost space proportionally above each panel's own minimum; the centre
    // retains its minimum. Stored preferences survive a temporary resize.
    int left = state_.panels.occupied(slots[0]);
    int right = state_.panels.occupied(slots[1]);
    const int leftMin = slots[0].collapsed ? 0 : slots[0].minWidth;
    const int rightMin = slots[1].collapsed ? 0 : slots[1].minWidth;
    const int available = std::max(0, getWidth() - state_.panels.arrangementMinWidth());
    if (left + right > available) {
        const int extra = std::max(0, available - leftMin - rightMin);
        const int wanted = left + right - leftMin - rightMin;
        const int leftExtra =
            wanted > 0 ? static_cast<int>(static_cast<double>(extra) * (left - leftMin) / wanted)
                       : 0;
        left = leftMin + leftExtra;
        right = rightMin + extra - leftExtra;
    }
    const int dock =
        state_.docked ? std::min(state_.deviceHeight, std::max(0, getHeight() - 100)) : 0;
    const int h = std::max(0, getHeight() - 44 - dock);
    g.setColour(juce::Colour(0xff21262e));
    g.fillRect(0, 44, left, h);
    g.fillRect(getWidth() - right, 44, right, h);
    g.setColour(juce::Colour(0xff20232b));
    g.fillRect(0, getHeight() - dock, getWidth(), dock);
    g.setColour(juce::Colour(0xff343b47));
    g.fillRect(left, 44, 1, h);
    g.fillRect(getWidth() - right, 44, 1, h);
    g.fillRect(0, getHeight() - dock, getWidth(), 1);
    g.setColour(juce::Colour(0xffa5b0c0));
    g.setFont(juce::FontOptions(14.f));
    auto name = [](Panel p) { return p == Panel::Browser ? "Browser" : "Mixer"; };
    g.drawText(name(slots[0].panel), 12, 56, std::max(0, left - 24), 24,
               juce::Justification::centredLeft);
    g.drawText(name(slots[1].panel), getWidth() - right + 12, 56, std::max(0, right - 24), 24,
               juce::Justification::centredLeft);
    g.drawText("Arrangement", left + 16, 56, 200, 24, juce::Justification::centredLeft);
    g.drawText("Devices", 12, getHeight() - dock + 12, 160, 24, juce::Justification::centredLeft);
}
bool AdiRootComponent::keyPressed(const juce::KeyPress &key) { return commands_.key(key, *this); }
bool AdiRootComponent::persist() { return persistence_.save(window_, state_, error_); }
bool AdiRootComponent::command(int id) {
    if (id >= AppCommands::ZoomIn && id <= AppCommands::ScrollDown)
        return arrangement.command(id);
    if (id >= AppCommands::NewProject && id <= AppCommands::AudioSettings)
        return applicationCommand && applicationCommand(id);
    // No action carries the presented frame into an edit: OpSubmitter resolves
    // current store/history state on arrival, even when this window is a frame behind.
    if (id == AppCommands::PlayStop || id == AppCommands::Stop || id == AppCommands::ContinuePlay) {
        const bool play = id != AppCommands::Stop && !mailbox_.desiredPlaying();
        if (!mailbox_.hasRoom(play && id != AppCommands::ContinuePlay ? 2 : 1)) {
            error_ = "Transport command queue is full";
            return false;
        }
        OpRequest request;
        request.opType = play ? "transport.play" : "transport.stop";
        request.payload = Payload::object();
        const auto result = submitter_.submit(std::move(request));
        if (!result) {
            error_ = result.error;
            return false;
        }
        if (play && id != AppCommands::ContinuePlay)
            locate(arrangement.geometry.insert);
        mailbox_.post(play ? TransportMailbox::Command::Play : TransportMailbox::Command::Stop);
    } else if (id == AppCommands::Undo || id == AppCommands::Redo) {
        const auto result = id == AppCommands::Undo ? submitter_.undo() : submitter_.redo();
        if (!result.ok) {
            error_ = result.error;
            return false;
        }
    } else if (id == AppCommands::SwapPanels) {
        const auto previous = state_.panels;
        if (auto refusal = state_.panels.swapSides(getWidth())) {
            error_ = refusal.message;
            return false;
        }
        if (!persist()) {
            state_.panels = previous;
            return false;
        }
    } else
        return false;
    if (id == AppCommands::SwapPanels)
        resized();
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
class ShellMenu final : public juce::MenuBarModel {
  public:
    ShellMenu(AdiRootComponent &r, AppCommands &c) : root(r), commands(c) {}
    juce::StringArray getMenuBarNames() override { return {"File", "Edit", "Transport"}; }
    juce::PopupMenu getMenuForIndex(int index, const juce::String &) override {
        commands.activate(root);
        juce::PopupMenu menu;
        auto add = [&](int id) { menu.addCommandItem(&commands.manager, id); };
        if (index == 0) {
            add(AppCommands::NewProject);
            add(AppCommands::OpenProject);
            add(AppCommands::SaveProject);
            menu.addSeparator();
            add(AppCommands::AudioSettings);
        }
        if (index == 1) {
            add(AppCommands::Undo);
            add(AppCommands::Redo);
            menu.addSeparator();
            add(AppCommands::SwapPanels);
        }
        if (index == 2) {
            add(AppCommands::PlayStop);
            add(AppCommands::Stop);
        }
        return menu;
    }
    void menuItemSelected(int, int) override {}

  private:
    AdiRootComponent &root;
    AppCommands &commands;
};
AdiWindow::AdiWindow(std::unique_ptr<AdiRootComponent> root, AppCommands &commands)
    : DocumentWindow("ADI", juce::Colour(0xff171a20), DocumentWindow::allButtons),
      root_(root.get()), commands_(commands), clock_(this, [this](double) { root_->frame(); }) {
    setContentOwned(root.release(), true);
    setResizable(true, false);
    setResizeLimits(520, 180, 10000, 10000);
    addKeyListener(this);
    commands_.activate(*root_);
    menu_ = std::make_unique<ShellMenu>(*root_, commands_);
    setMenuBar(menu_.get());
}
AdiWindow::~AdiWindow() {
    setMenuBar(nullptr);
    removeKeyListener(this);
}
bool AdiWindow::keyPressed(const juce::KeyPress &key, juce::Component *) {
    return commands_.key(key, *root_);
}
} // namespace adi::ui
