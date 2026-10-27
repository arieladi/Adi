// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_shell.hpp"
#include <algorithm>
#include <array>
namespace adi::ui {
namespace {
// Live's measures, in points at 100 % (ui_look.hpp): the control bar, the Status Bar,
// the AI tab, the Info View's width and the browser's preview strip.
constexpr int kTransportHeight = look::controlBarHeight, kStatusHeight = look::statusBarHeight,
              kTabHeight = 20;
constexpr int kInfoWidth = 200, kPreviewHeight = 27;
// Interface zoom steps per window. PROVISIONAL: Live's own list is to be
// measured (ADR-0203 d26); 100 % is the start and the steps are symmetric.
constexpr std::array<float, 7> kZoomSteps{.5f, .75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
bool flag(const settings::AppSettings &s, const char *key, bool fallback) {
    const auto v = s.get(key);
    return v.is_boolean() ? v.get<bool>() : fallback;
}
} // namespace
AppCommands::AppCommands(settings::AppSettings &settings) : settings_(settings) {
    manager.setFirstCommandTarget(this);
    reload();
}
void AppCommands::reload() {
    live_ = settings_.get("shortcuts.preset") == "live";
    infoView_ = flag(settings_, "lookfeel.showInfoView", true);
    tooltips_ = flag(settings_, "lookfeel.showTooltips", true);
    const auto delay = settings_.get("lookfeel.tooltipDelayMs");
    tooltipDelayMs_ = delay.is_number_integer() ? delay.get<int>() : 600;
    zoomOnSelection_ = flag(settings_, "lookfeel.zoomOnSelection", true);
    // A preset changes default keys, so the old registrations go first.
    manager.clearCommands();
    manager.registerAllCommandsForTarget(this);
    manager.getKeyMappings()->resetToDefaultMappings();
}
bool AppCommands::setInfoView(bool on, std::string &why) {
    if (!settings_.set("lookfeel.showInfoView", on, {}, why) || !settings_.save(why))
        return false;
    infoView_ = on;
    return true;
}
void AppCommands::getAllCommands(juce::Array<juce::CommandID> &ids) {
    for (int id = PlayStop; id <= LastId; ++id)
        ids.add(id);
}
void AppCommands::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo &info) {
    if (id < PlayStop || id > LastId) {
        info.setActive(false);
        return;
    }
    static const char *names[] = {"Play / stop",         "Stop",
                                  "Undo",                "Redo",
                                  "Swap side panels",    "Hide/Show Browser",
                                  "Arrangement Track Controls", "Hide/Show Device View",
                                  "Hide/Show Info View", "Zoom In Window",
                                  "Zoom Out Window",     "New Project",
                                  "Open Project...",     "Take Snapshot...",
                                  "Save Project As...",  "Settings...",
                                  "Zoom In",             "Zoom Out",
                                  "Zoom to Arrangement Time Selection",
                                  "Zoom Back",           "Optimize Arrangement Width",
                                  "Optimize Arrangement Height",
                                  "Move Insert Marker to Beginning",
                                  "Continue Play from Stop Point",
                                  "Insert Audio Track",  "Insert MIDI Track",
                                  "Delete Track",        "Rename",
                                  "Mixer"};
    info.setInfo(names[id - PlayStop], names[id - PlayStop], "ADI", 0);
    if (id == Undo || id == Redo) {
        const bool redo = id == Redo;
        info.setInfo(active_ ? juce::String(active_->undoTitle(redo))
                             : juce::String(names[id - PlayStop]),
                     names[id - PlayStop], "ADI", 0);
        info.setActive(active_ != nullptr && active_->canUndo(redo));
    }
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto alt = juce::ModifierKeys::altModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    // Live 12 manual §41.1, §41.5, §41.9 and §41.20. Esc stops nothing: in Live
    // it closes a window or a dialog, and the window lane builds Live's keys.
    switch (id) {
    case PlayStop:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
        break;
    case Undo:
        info.addDefaultKeypress('z', cmd);
        break;
    case Redo:
#if JUCE_MAC
        info.addDefaultKeypress('z', cmd | shift);
#else
        if (live_)
            info.addDefaultKeypress('y', cmd);
        else
            info.addDefaultKeypress('z', cmd | shift);
#endif
        break;
    case ToggleBrowser:
        info.addDefaultKeypress('b', cmd | alt);
        info.addDefaultKeypress('5', cmd | alt);
        break;
    case ToggleMixer:
        // The side panel of track controls; Ctrl/Cmd+Alt+M is the Mixer View's (§41.1).
        break;
    case ToggleMixerView:
        // Live 12: the View menu's Mixer and the toggle in the window's bottom-right
        // corner open the Mixer View in the Arrangement (manual 6.1, p. 162; §41.1).
        info.addDefaultKeypress('m', cmd | alt);
        info.setTicked(active_ != nullptr && active_->state().mixerView);
        break;
    case ToggleDeviceView:
        info.addDefaultKeypress('4', cmd | alt);
        break;
    case ToggleInfoView:
        // Shift+? : one platform reports the shifted character, the other the key.
        info.addDefaultKeypress('?', shift);
        info.addDefaultKeypress('/', shift);
        break;
    case ZoomIn:
        info.addDefaultKeypress('+', cmd);
        info.addDefaultKeypress('+', cmd | shift);
        info.addDefaultKeypress('=', cmd);
        break;
    case ZoomOut:
        info.addDefaultKeypress('-', cmd);
        break;
    // §41.3. Ctrl/Cmd+S takes a named snapshot rather than saving, a deviation the
    // director ruled (ADR-0203 d6, d12); Save As is Live's, a true one.
    case NewProject:
        info.addDefaultKeypress('n', cmd);
        break;
    case OpenProject:
        info.addDefaultKeypress('o', cmd);
        break;
    case TakeSnapshot:
        info.addDefaultKeypress('s', cmd);
        break;
    case SaveAs:
        info.addDefaultKeypress('s', cmd | shift);
        break;
    case Settings:
        info.addDefaultKeypress(',', cmd);
        break;
    // The arrangement: §6.2, §41.9, §41.16, §41.19, §41.20; ADR-0129's gate.
    case TimeZoomIn:
        info.addDefaultKeypress('+', 0);
        info.addDefaultKeypress('+', shift);
        info.addDefaultKeypress('=', 0);
        break;
    case TimeZoomOut:
        info.addDefaultKeypress('-', 0);
        break;
    case ZoomToSelection:
        info.addDefaultKeypress('z', 0);
        break;
    case ZoomBack:
        info.addDefaultKeypress('x', 0);
        break;
    case OptimizeWidth:
        info.addDefaultKeypress('w', 0);
        break;
    case OptimizeHeight:
        info.addDefaultKeypress('h', 0);
        break;
    case GoStart:
        info.addDefaultKeypress(juce::KeyPress::homeKey, 0);
        break;
    case ContinuePlay:
        info.addDefaultKeypress(juce::KeyPress::spaceKey, shift);
        break;
    case InsertAudioTrack:
        info.addDefaultKeypress('t', cmd);
        break;
    case InsertMidiTrack:
        info.addDefaultKeypress('t', cmd | shift);
        break;
    case DeleteTrack:
        info.setActive(active_ != nullptr && active_->arrangement.geometry.selection.track != 0);
        break;
    case RenameTrack: // §41.5
        info.addDefaultKeypress('r', cmd);
        info.setActive(active_ != nullptr && active_->arrangement.geometry.selection.track != 0);
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
    // Re-read the command infos so the Edit menu names the step it would undo.
    manager.commandStatusChanged();
}
void AppCommands::detach(AdiRootComponent &root) {
    if (active_ == &root)
        active_ = nullptr;
}
bool AppCommands::perform(const InvocationInfo &info) {
    // A command the root refuses has still been handled: the Status Bar says why.
    if (active_ == nullptr)
        return false;
    active_->command(info.commandID);
    return true;
}
StatusBar::StatusBar(AdiRootComponent &root) : root_(root) {
    // Live's corners: the Info View's toggle on the left, the Detail View's on the right.
    for (auto *b : {&infoToggle, &detailToggle}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    }
    infoToggle.setTitle("Info View");
    infoToggle.setTooltip("Shows or hides the Info View. Shift+?.");
    infoToggle.onClick = [this] { root_.command(AppCommands::ToggleInfoView); };
    detailToggle.setTitle("Detail View");
    detailToggle.setTooltip("Shows or hides the Device View. Ctrl/Cmd+Alt+4.");
    detailToggle.onClick = [this] { root_.command(AppCommands::ToggleDeviceView); };
    addAndMakeVisible(mixerToggle);
    mixerToggle.setTitle("Mixer View");
    mixerToggle.setTooltip("Shows or hides the Mixer. Ctrl/Cmd+Alt+M.");
    mixerToggle.setClickingTogglesState(false);
    mixerToggle.onClick = [this] { root_.command(AppCommands::ToggleMixerView); };
}
void StatusBar::resized() {
    // Live's Status Bar at 100 %: round corner buttons, the message field, a second
    // field, the selected track's box; 22 pt fields with the frame's 8 pt around them.
    const int w = getWidth();
    infoToggle.setBounds(8, 9, 19, 19);
    detailToggle.setBounds(w - 27, 9, 19, 19);
    mixerToggle.setBounds(w - 86, 11, 44, 16);
}
void StatusBar::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const int w = getWidth();
    const juce::Rectangle<int> message(35, 8, std::max(0, w - 419), 22);
    const juce::Rectangle<int> second(w - 376, 8, 206, 22), track(w - 162, 8, 124, 22);
    for (auto r : {message, second, track}) {
        g.setColour(look::band);
        g.fillRect(r);
        g.setColour(look::panel);
        g.fillRect(r.reduced(3));
    }
    // The corner buttons: dark and round, with Live's light triangles.
    for (auto *b : {&infoToggle, &detailToggle}) {
        const auto c = b->getBounds().toFloat();
        g.setColour(look::band);
        g.fillEllipse(c);
        look::triangle(g, c.reduced(5.f), -juce::MathConstants<float>::halfPi, look::panel);
    }
    const bool error = !root_.error().empty();
    g.setColour(look::text);
    g.setFont(look::font(12.5f));
    g.drawText(error ? juce::String(root_.error()) : root_.selectionText(), message.reduced(8, 0),
               juce::Justification::centredLeft);
    // The selected track's name, as Live shows it at the right.
    const auto &tracks = root_.reader().tracks();
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == root_.arrangement.geometry.selection.track) {
            g.setFont(look::font(11.5f));
            g.drawText(juce::String::fromUTF8(displayName(tracks[i]->name, i + 1).c_str()),
                       track.reduced(6, 0).withWidth(44), juce::Justification::centredLeft);
            break;
        }
}
AdiRootComponent::AdiRootComponent(engine::ProjectView &view, OpSubmitter &submitter,
                                   TransportMailbox &mailbox, AppCommands &commands,
                                   ViewStateStore &persistence, SessionState &session,
                                   std::string window, DesktopDefaults defaults)
    : transport(*this), status(*this), arrangement(*this), devices(*this), dockResizer(*this),
      mixer(*this), view_(view), submitter_(submitter), mailbox_(mailbox),
      commands_(commands), persistence_(persistence), session_(session),
      window_(std::move(window)),
      state_(persistence.load(window_, defaults)), infoShown_(commands.infoView()) {
    // Live's look for everything this window holds (ui_look.hpp).
    setLookAndFeel(&look::liveLook());
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(transport);
    addAndMakeVisible(status);
    addAndMakeVisible(browser);
    addAndMakeVisible(aiTab);
    addAndMakeVisible(arrangement);
    addAndMakeVisible(devices);
    addAndMakeVisible(dockResizer);
    addAndMakeVisible(mixer);
    // The Overview spans the track headers too, as Live's (ui_arrangement.hpp).
    addAndMakeVisible(arrangement.overview);
    addChildComponent(mixerView);
    addChildComponent(mixerViewResizer);
    arrangement.geometry.left = state_.timelineLeft;
    arrangement.geometry.scale = state_.pixelsPerQuarter;
    arrangement.geometry.height = state_.laneHeight;
    arrangement.geometry.heights = state_.laneHeights;
    aiTab.setColour(juce::TextButton::buttonColourId, look::columnHead);
    aiTab.setTitle("AI");
    aiTab.setTooltip("The AI chat. Opens at the foot of the browser.");
    aiTab.onClick = [this] {
        state_.aiChatOpen = !state_.aiChatOpen;
        if (!persist()) {
            state_.aiChatOpen = !state_.aiChatOpen;
            fail(error_);
            return;
        }
        resized();
    };
    reader_.emplace(view_.current());
    setSize(1200, 720);
    frame();
}
AdiRootComponent::~AdiRootComponent() {
    commands_.detach(*this);
    setLookAndFeel(nullptr);
}
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
    // Selection is the session's (ADR-0021): present it again when it changed, from
    // this window or any other.
    if (selectionGeneration_ != session_.generation()) {
        selectionGeneration_ = session_.generation();
        arrangement.geometry.selection = session_.selection();
        dirty_.mark(DirtySet::Layout);
    }
    // The Info View is app-scoped: a toggle in another window shows here too.
    if (infoShown_ != commands_.infoView()) {
        infoShown_ = commands_.infoView();
        resized();
    }
    if (infoShown_)
        hover(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse());
    lastDrain_ = dirty_.drain();
    arrangement.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    devices.frame(arrangement.geometry.selection.track);
    mixer.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    if (mixerView.isVisible())
        mixerView.frame((lastDrain_ & (DirtySet::Layout | DirtySet::Content)) != 0);
    if (lastDrain_ & DirtySet::Transport) {
        transport.refresh();
        transport.repaint();
    }
    if (lastDrain_ & DirtySet::Status)
        status.repaint();
    if (lastDrain_ & (DirtySet::Layout | DirtySet::Content))
        repaint();
}
void AdiRootComponent::hover(juce::Component *c) {
    if (c != nullptr && c != this && !isParentOf(c))
        c = nullptr;
    // The nearest element that can describe itself, as Live's Info View does.
    juce::TooltipClient *client = nullptr;
    for (auto *p = c; p != nullptr && p != this; p = p->getParentComponent())
        if ((client = dynamic_cast<juce::TooltipClient *>(p)) != nullptr) {
            c = p;
            break;
        }
    if (c == hovered_)
        return;
    hovered_ = c;
    infoText_ = client ? c->getTitle() + "\n" + client->getTooltip() : juce::String();
    dirty_.mark(DirtySet::Content);
}
std::int64_t AdiRootComponent::timelineTick() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo && rate > 0 ? tempo->secondsToTicks(static_cast<double>(position_) / rate) : 0;
}
bool AdiRootComponent::locate(std::int64_t tick) {
    // Landing reads the current cell, not this window's presented frame.
    const auto current = view_.current();
    const double rate = mailbox_.sampleRate() > 0 ? mailbox_.sampleRate() : current->sampleRate;
    if (!current->tempo || !mailbox_.hasRoom())
        return false;
    return mailbox_.post(TransportMailbox::Command::Locate,
                         static_cast<std::int64_t>(current->tempo->ticksToSeconds(tick) * rate));
}
bool AdiRootComponent::playFrom(std::int64_t tick) {
    const bool start = !mailbox_.desiredPlaying();
    if (!mailbox_.hasRoom(start ? 2 : 1))
        return fail("Transport command queue is full");
    if (start) {
        OpRequest request;
        request.opType = "transport.play";
        request.payload = Payload::object();
        if (const auto result = submitter_.submit(std::move(request)); !result)
            return fail(result.error);
    }
    if (!locate(tick))
        return fail("The project has no tempo map to locate in.");
    if (start)
        mailbox_.post(TransportMailbox::Command::Play);
    error_.clear();
    dirty_.mark(DirtySet::Transport | DirtySet::Status);
    return true;
}
double AdiRootComponent::bpm() const noexcept {
    const auto *tempo = reader_->tempo();
    const double rate = sampleRate_ > 0 ? sampleRate_ : reader_->sampleRate();
    return tempo ? tempo->bpmAt(
                       tempo->secondsToTicks(rate > 0 ? static_cast<double>(position_) / rate : 0))
                 : 120.;
}
void AdiRootComponent::resized() {
    const int w = getWidth(), h = getHeight(), gap = look::gap;
    transport.setBounds(0, 0, w, kTransportHeight);
    status.setBounds(0, h - kStatusHeight, w, kStatusHeight);
    const int top = kTransportHeight, bottom = h - kStatusHeight;
    const int body = std::max(0, bottom - top);
    // The detail row: Live's Info View and Device View. The strip never goes below its
    // floor and grows upward only (ADR-0203 d7); it leaves the transport, the ruler and
    // a row when it can.
    const int dock = state_.docked ? std::min(body, std::max(state_.deviceFloor,
                                                             std::min(state_.deviceHeight, body - 100)))
                                   : 0;
    const int row = dock > 0 ? dock : (infoShown_ ? std::min(body, state_.deviceFloor) : 0);
    const int rowY = bottom - row;
    // Live's Info View sits in the bottom-left corner beside the Device View.
    const int infoW = infoShown_ ? std::min(kInfoWidth, w / 2) : 0;
    infoArea_ = infoShown_ ? juce::Rectangle<int>(gap, rowY, infoW, row) : juce::Rectangle<int>();
    const int deviceX = infoShown_ ? infoArea_.getRight() + gap : gap;
    deviceArea_ = dock > 0 ? juce::Rectangle<int>(deviceX, rowY, w - deviceX - gap, dock)
                           : juce::Rectangle<int>();
    // Each panel has Live's dark 4 pt edge, which the root draws; the strip sits inside.
    // Widths belong to panels (ADR-0080 d2). If the window shrinks, share the
    // lost space proportionally above each panel's own minimum; the centre
    // keeps its minimum. Stored preferences survive a temporary resize.
    const auto &slots = state_.panels.slots();
    int left = state_.panels.occupied(slots[0]);
    int right = state_.panels.occupied(slots[1]);
    const int leftMin = slots[0].collapsed ? 0 : slots[0].minWidth;
    const int rightMin = slots[1].collapsed ? 0 : slots[1].minWidth;
    const int available = std::max(0, w - state_.panels.arrangementMinWidth());
    if (left + right > available) {
        const int extra = std::max(0, available - leftMin - rightMin);
        const int wanted = left + right - leftMin - rightMin;
        const int leftExtra =
            wanted > 0 ? static_cast<int>(static_cast<double>(extra) * (left - leftMin) / wanted)
                       : 0;
        left = leftMin + leftExtra;
        right = rightMin + extra - leftExtra;
    }
    const int mid = std::max(0, rowY - gap - top);
    const bool browserLeft = slots[0].panel == Panel::Browser;
    // The mixer's columns sit against the arrangement, inside its frame, as Live's
    // track headers do; the browser stands apart, a gap away.
    const int mw = browserLeft ? right : left;
    mixerArea_ = mw > 0 ? juce::Rectangle<int>(browserLeft ? w - right - gap : gap, top, mw, mid)
                        : juce::Rectangle<int>();
    mixer.setVisible(!mixerArea_.isEmpty());
    mixer.setBounds(mixerArea_);
    const int bw = browserLeft ? left : right;
    browserArea_ = bw > 0 ? juce::Rectangle<int>(browserLeft ? gap : w - right, top, bw - gap, mid)
                          : juce::Rectangle<int>();
    browser.setVisible(!browserArea_.isEmpty());
    browser.setBounds(browserArea_);
    // The AI chat is a tab at the browser's foot, collapsed by default, so the
    // column looks as Live's (ADR-0203 d6). Open, it takes the lower part.
    if (browserArea_.isEmpty()) {
        chatArea_ = {};
        aiTab.setVisible(false);
    } else {
        // ADI's addition (Master Reference 3.1): the tab sits at the foot of the
        // browser's panes, above Live's preview strip.
        const int inner = browserArea_.getHeight() - kPreviewHeight - look::border;
        const int chatH = state_.aiChatOpen ? std::max(kTabHeight, inner * 2 / 5) : kTabHeight;
        chatArea_ = juce::Rectangle<int>(browserArea_.getX() + look::border, browserArea_.getY() + inner - chatH,
                                         browserArea_.getWidth() - 2 * look::border, chatH);
        aiTab.setVisible(true);
        aiTab.setBounds(chatArea_.withHeight(kTabHeight));
    }
    leftWidth_ = left;
    rightWidth_ = right;
    // The arrangement fills what the side panels leave, between the browser's gap and
    // the mixer's columns.
    const int centreX = browserLeft ? left + gap : mixerArea_.getRight();
    const int centreW = std::max(0, (browserLeft ? mixerArea_.getX() : w - right - gap) - centreX);
    // Live's Mixer View sits at the foot of the arrangement, its full width (§18.1).
    const int mvh = state_.mixerView
                        ? std::clamp(state_.mixerViewHeight, 120,
                                     std::max(120, mid - ArrangementView::kTopHeight - 64))
                        : 0;
    mixerViewArea_ = mvh > 0 && mid > mvh
                         ? juce::Rectangle<int>(centreX, top + mid - mvh, centreW, mvh)
                         : juce::Rectangle<int>();
    mixerView.setVisible(!mixerViewArea_.isEmpty());
    mixerViewResizer.setVisible(!mixerViewArea_.isEmpty());
    mixerView.setBounds(mixerViewArea_.withTrimmedTop(1));
    mixerViewResizer.setBounds(mixerViewArea_.getX(), mixerViewArea_.getY() - 3, centreW, 6);
    status.mixerToggle.setToggleState(state_.mixerView, juce::dontSendNotification);
    arrangement.setBounds(centreX, top, centreW, mid - mixerViewArea_.getHeight());
    // The Overview runs from the lanes' left edge to Live's H and W buttons.
    const int overviewX = centreX + ArrangementView::kLeftMargin;
    const int overviewRight = browserLeft && !mixerArea_.isEmpty()
                                  ? mixerArea_.getRight() - MixerPanel::kRightStrip - 48
                                  : centreX + centreW - 4;
    arrangement.overview.setBounds(overviewX, top + 4, std::max(0, overviewRight - overviewX),
                                   ArrangementView::kOverviewHeight - 4);
    devices.setVisible(!deviceArea_.isEmpty());
    dockResizer.setVisible(!deviceArea_.isEmpty());
    devices.setBounds(deviceArea_.reduced(look::border));
    dockResizer.setBounds(deviceArea_.getX(), deviceArea_.getY() - 3, deviceArea_.getWidth(), 6);
    dockResizer.toFront(false);
    dirty_.mark(DirtySet::All);
}
void AdiRootComponent::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    // Live's panels have a dark 4 pt edge with rounded corners.
    auto panel = [&g](juce::Rectangle<int> r) {
        g.setColour(look::band);
        g.fillRoundedRectangle(r.toFloat(), 5.f);
    };
    if (!deviceArea_.isEmpty())
        panel(deviceArea_);
    // Live's Info View: a title bar with the element's name, its description below.
    if (!infoArea_.isEmpty()) {
        panel(infoArea_);
        const auto inner = infoArea_.reduced(look::border);
        g.setColour(look::panel);
        g.fillRect(inner);
        const auto titleBar = inner.withHeight(17);
        g.setColour(look::field);
        g.fillRect(titleBar);
        const int split = infoText_.indexOfChar('\n');
        const auto title = split < 0 ? infoText_ : infoText_.substring(0, split);
        const auto body = split < 0 ? juce::String() : infoText_.substring(split + 1);
        g.setColour(look::text);
        g.setFont(look::font(12.f, true));
        g.drawText(title, titleBar.reduced(5, 0), juce::Justification::centredLeft);
        // Live's body text: small, on a 15 pt line.
        juce::AttributedString words(body);
        words.setFont(look::font(11.f));
        words.setColour(look::text);
        words.setLineSpacing(2.5f);
        juce::TextLayout layout;
        layout.createLayout(words, static_cast<float>(inner.getWidth() - 10));
        layout.draw(g, inner.withTrimmedTop(20).reduced(5, 2).toFloat());
    }
    if (!chatArea_.isEmpty() && state_.aiChatOpen) {
        g.setColour(look::panel);
        g.fillRect(chatArea_);
    }
}
bool AdiRootComponent::keyPressed(const juce::KeyPress &key) { return commands_.key(key, *this); }
void AdiRootComponent::resizeMixerView(int height) {
    const int body = getHeight() - kTransportHeight - kStatusHeight - deviceArea_.getHeight();
    state_.mixerViewHeight =
        std::clamp(height, 120,
                   std::max(120, body - ArrangementView::kTopHeight - state_.laneHeight));
    resized();
}
void AdiRootComponent::resizeDevices(int height) {
    const int ceiling = getHeight() - kTransportHeight - kStatusHeight - look::gap -
                        ArrangementView::kTopHeight - state_.laneHeight;
    state_.setDeviceHeight(std::min(height, ceiling));
    resized();
}
bool AdiRootComponent::persist() { return persistence_.save(window_, state_, error_); }
void AdiRootComponent::publishSelection() {
    std::string why;
    if (!session_.select(arrangement.geometry.selection, why))
        fail(why);
    selectionGeneration_ = session_.generation();
}
bool AdiRootComponent::submitEdit(const std::string &op, Payload payload, const char *label) {
    OpRequest request;
    request.opType = op;
    request.payload = std::move(payload);
    request.label = label;
    request.selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(request));
    if (!result)
        return fail(result.error);
    if (afterEdit)
        afterEdit();
    error_.clear();
    dirty_.mark(DirtySet::All);
    return true;
}
void AdiRootComponent::say(std::string message) {
    error_ = std::move(message);
    dirty_.mark(DirtySet::Status);
}
juce::String AdiRootComponent::selectionText() const {
    // Live's Status Bar: the insert marker with its time, or the time selection.
    const auto &s = arrangement.geometry.selection;
    auto where = [](std::int64_t tick) {
        const auto sixteenths = tick / (textproj::kPPQ / 4);
        return juce::String(sixteenths / 16 + 1) + "." + juce::String(sixteenths / 4 % 4 + 1) +
               "." + juce::String(sixteenths % 4 + 1);
    };
    auto seconds = [this](std::int64_t tick) {
        const auto *tempo = reader_->tempo();
        const int t = tempo ? static_cast<int>(tempo->ticksToSeconds(tick)) : 0;
        return juce::String(t / 60) + ":" + juce::String(t % 60).paddedLeft('0', 2);
    };
    if (s.start != s.end) {
        const auto a = std::min(s.start, s.end), b = std::max(s.start, s.end);
        const auto beats = (b - a) / (textproj::kPPQ / 4);
        return "Time Selection   Start: " + where(a) + "   End: " + where(b) + "   Length: " +
               juce::String(beats / 16) + "." + juce::String(beats / 4 % 4) + "." +
               juce::String(beats % 4) + "   (Duration: " + seconds(b - a) + ")";
    }
    return "Insert Mark " + where(s.marked ? s.insert : 0) + " (Time: " +
           seconds(s.marked ? s.insert : 0) + ")";
}
std::string AdiRootComponent::undoTitle(bool redo) const {
    const auto step = redo ? submitter_.nextRedo() : submitter_.nextUndo();
    return std::string(redo ? "Redo" : "Undo") +
           (step && !step->label.empty() ? " " + step->label : "");
}
bool AdiRootComponent::fail(std::string message) {
    error_ = std::move(message);
    dirty_.mark(DirtySet::Status);
    return false;
}
bool AdiRootComponent::toggle(Panel p) {
    const auto previous = state_.panels;
    const auto *g = state_.panels.find(p);
    if (auto refusal = state_.panels.setCollapsed(p, !g->collapsed, getWidth()))
        return fail(refusal.message);
    if (!persist()) {
        state_.panels = previous;
        return fail(error_);
    }
    return true;
}
bool AdiRootComponent::insertTrack(bool midi) {
    // Live inserts after the selected track and selects the new one. The id is the
    // payload's, never SQLite's (ADR-0021), and it comes from the CURRENT cell: an
    // action lands on the project as it is now, not on this window's last frame.
    const auto current = view_.current();
    const auto &tracks = current->tracks;
    std::int64_t id = 1, index = static_cast<std::int64_t>(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        id = std::max(id, tracks[i]->id + 1);
        if (tracks[i]->id == arrangement.geometry.selection.track)
            index = static_cast<std::int64_t>(i) + 1;
    }
    OpRequest request;
    request.opType = "track.create";
    request.payload = {{"id", id},
                       {"kind", midi ? "midi" : "audio"},
                       // '#' shows the track's number and follows it (manual 18.2).
                       {"name", midi ? "# MIDI" : "# Audio"},
                       {"index", index}};
    request.label = midi ? "Insert MIDI Track" : "Insert Audio Track";
    request.selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(request));
    if (!result)
        return fail(result.error);
    arrangement.geometry.selection.track = id;
    publishSelection();
    return true;
}
bool AdiRootComponent::deleteTrack() {
    const auto id = arrangement.geometry.selection.track;
    if (id == 0)
        return fail("Select a track to delete.");
    OpRequest request;
    request.opType = "track.delete";
    request.payload = {{"id", id}};
    request.label = "Delete Track";
    request.selBefore = session_.advisory();
    const auto result = submitter_.submit(std::move(request));
    if (!result)
        return fail(result.error);
    arrangement.geometry.selection.track = 0;
    arrangement.geometry.selection.clip = 0;
    publishSelection();
    return true;
}
bool AdiRootComponent::command(int id) {
    if (id >= AppCommands::NewProject && id <= AppCommands::Settings)
        return applicationCommand && applicationCommand(id);
    if (id >= AppCommands::TimeZoomIn && id <= AppCommands::GoStart)
        return arrangement.command(id);
    // No action carries the presented frame into an edit: OpSubmitter resolves
    // current store/history state on arrival, even when this window is a frame behind.
    switch (id) {
    case AppCommands::PlayStop:
    case AppCommands::Stop:
    case AppCommands::ContinuePlay: {
        // Space plays from the insert marker, Shift+Space from where it stopped.
        const bool play = id != AppCommands::Stop && !mailbox_.desiredPlaying();
        const bool fromMarker = play && id == AppCommands::PlayStop;
        if (!mailbox_.hasRoom(fromMarker ? 2 : 1))
            return fail("Transport command queue is full");
        OpRequest request;
        request.opType = play ? "transport.play" : "transport.stop";
        request.payload = Payload::object();
        const auto result = submitter_.submit(std::move(request));
        if (!result)
            return fail(result.error);
        if (fromMarker)
            locate(arrangement.geometry.selection.insert);
        mailbox_.post(play ? TransportMailbox::Command::Play : TransportMailbox::Command::Stop);
        break;
    }
    case AppCommands::Undo:
    case AppCommands::Redo: {
        const auto result = id == AppCommands::Undo ? submitter_.undo() : submitter_.redo();
        if (!result.ok)
            return fail(result.error);
        // Undo puts back what was selected before the undone step (ADR-0021).
        if (id == AppCommands::Undo && result.selBefore)
            session_.restore(*result.selBefore);
        break;
    }
    case AppCommands::SwapPanels: {
        const auto previous = state_.panels;
        if (auto refusal = state_.panels.swapSides(getWidth()))
            return fail(refusal.message);
        if (!persist()) {
            state_.panels = previous;
            return fail(error_);
        }
        break;
    }
    case AppCommands::ToggleBrowser:
    case AppCommands::ToggleMixer:
        if (!toggle(id == AppCommands::ToggleBrowser ? Panel::Browser : Panel::Mixer))
            return false;
        break;
    case AppCommands::ToggleMixerView:
        state_.mixerView = !state_.mixerView;
        if (!persist()) {
            state_.mixerView = !state_.mixerView;
            return fail(error_);
        }
        break;
    case AppCommands::ToggleDeviceView:
        state_.docked = !state_.docked;
        if (!persist()) {
            state_.docked = !state_.docked;
            return fail(error_);
        }
        break;
    case AppCommands::ToggleInfoView: {
        std::string why;
        if (!commands_.setInfoView(!commands_.infoView(), why))
            return fail(why);
        infoShown_ = commands_.infoView();
        break;
    }
    case AppCommands::InsertAudioTrack:
    case AppCommands::InsertMidiTrack:
        if (!insertTrack(id == AppCommands::InsertMidiTrack))
            return false;
        break;
    case AppCommands::DeleteTrack:
        if (!deleteTrack())
            return false;
        break;
    case AppCommands::RenameTrack:
        // The edit itself is its own op when the name is committed.
        return arrangement.headers.beginRename(arrangement.geometry.selection.track) ||
               fail("Select a track to rename.");
    case AppCommands::ZoomIn:
    case AppCommands::ZoomOut: {
        const auto at = std::find(kZoomSteps.begin(), kZoomSteps.end(), interfaceZoom_);
        const auto i = at == kZoomSteps.end() ? 2 : at - kZoomSteps.begin();
        const auto next = id == AppCommands::ZoomIn ? i + 1 : i - 1;
        if (next < 0 || next >= static_cast<std::ptrdiff_t>(kZoomSteps.size()))
            return false;
        interfaceZoom_ = kZoomSteps[static_cast<std::size_t>(next)];
        if (auto *host = getParentComponent())
            host->resized();
        break;
    }
    default:
        return false;
    }
    if (afterEdit)
        afterEdit();
    error_.clear();
    resized();
    return true;
}
// Live's menu bar, as far as step 7 has commands for it: File, Edit, Create, View.
// Transport has no menu in Live; Space is its key.
class ShellMenu final : public juce::MenuBarModel {
  public:
    ShellMenu(AdiRootComponent &r, AppCommands &c) : root(r), commands(c) {}
    juce::StringArray getMenuBarNames() override { return {"File", "Edit", "Create", "View"}; }
    juce::PopupMenu getMenuForIndex(int index, const juce::String &) override {
        commands.activate(root);
        juce::PopupMenu menu;
        auto add = [&](int id) { menu.addCommandItem(&commands.manager, id); };
        if (index == 0) {
            add(AppCommands::NewProject);
            add(AppCommands::OpenProject);
            menu.addSeparator();
            add(AppCommands::TakeSnapshot);
            add(AppCommands::SaveAs);
#if !JUCE_MAC
            menu.addSeparator();
            add(AppCommands::Settings);
#endif
        } else if (index == 1) {
            add(AppCommands::Undo);
            add(AppCommands::Redo);
            menu.addSeparator();
            add(AppCommands::RenameTrack);
        } else if (index == 2) {
            add(AppCommands::InsertAudioTrack);
            add(AppCommands::InsertMidiTrack);
        } else if (index == 3) {
            add(AppCommands::ToggleBrowser);
            add(AppCommands::ToggleMixerView);
            add(AppCommands::ToggleMixer);
            add(AppCommands::ToggleDeviceView);
            add(AppCommands::ToggleInfoView);
            menu.addSeparator();
            add(AppCommands::ZoomIn);
            add(AppCommands::ZoomOut);
            menu.addSeparator();
            add(AppCommands::ZoomToSelection);
            add(AppCommands::ZoomBack);
            add(AppCommands::OptimizeWidth);
            add(AppCommands::OptimizeHeight);
            menu.addSeparator();
            add(AppCommands::SwapPanels);
        }
        return menu;
    }
    void menuItemSelected(int, int) override {}

  private:
    AdiRootComponent &root;
    AppCommands &commands;
};
ZoomHost::ZoomHost(std::unique_ptr<AdiRootComponent> root) : root_(std::move(root)) {
    setOpaque(true);
    addAndMakeVisible(*root_);
    setSize(root_->getWidth(), root_->getHeight());
}
void ZoomHost::resized() {
    const float z = root_->interfaceZoom();
    root_->setTransform(juce::AffineTransform::scale(z));
    root_->setBounds(0, 0, juce::roundToInt(static_cast<float>(getWidth()) / z),
                     juce::roundToInt(static_cast<float>(getHeight()) / z));
}
AdiWindow::AdiWindow(std::unique_ptr<AdiRootComponent> root, AppCommands &commands,
                     bool onDesktop)
    : DocumentWindow("ADI", juce::Colour(0xff171a20), DocumentWindow::allButtons, onDesktop),
      root_(root.get()), commands_(commands), clock_(this, [this](double) { root_->frame(); }) {
    setContentOwned(new ZoomHost(std::move(root)), true);
    setResizable(true, false);
    setResizeLimits(520, 180, 10000, 10000);
    addKeyListener(this);
    // Floating tooltips are on by default after 600 ms, a deliberate deviation
    // the director confirmed by name (ADR-0203 d12, ADR-0131); one per window.
    if (commands_.tooltips()) {
        tipDelayMs_ = commands_.tooltipDelayMs();
        tips_ = std::make_unique<juce::TooltipWindow>(this, tipDelayMs_);
    }
    commands_.activate(*root_);
    menu_ = std::make_unique<ShellMenu>(*root_, commands_);
    // On the Mac the menu is the application's, at the top of the screen, as Live's.
#if JUCE_MAC
    if (onDesktop)
        juce::MenuBarModel::setMacMainMenu(menu_.get());
#else
    setMenuBar(menu_.get());
#endif
}
AdiWindow::~AdiWindow() {
#if JUCE_MAC
    if (juce::MenuBarModel::getMacMainMenu() == menu_.get())
        juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    removeKeyListener(this);
}
juce::MenuBarModel *AdiWindow::menu() noexcept { return menu_.get(); }
bool AdiWindow::keyPressed(const juce::KeyPress &key, juce::Component *) {
    return commands_.key(key, *root_);
}
} // namespace adi::ui
