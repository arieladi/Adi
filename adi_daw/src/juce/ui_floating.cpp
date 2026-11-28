// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_floating.hpp"
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <chrono>
namespace adi::ui {
namespace {
std::vector<window_place::Monitor> asPlaces(const std::vector<MonitorArea> &screens) {
    std::vector<window_place::Monitor> out;
    for (const auto &s : screens)
        out.push_back({s.id, toPlace(s.area.isEmpty() ? s.usable : s.area), toPlace(s.usable), s.dpi, s.main});
    return out;
}
} // namespace
// WIN-02: the one placement rule (adi/ui/window_place.hpp). A record from before the monitor's area was
// kept reads as on its monitor unchanged, or, with that monitor gone, at its own coordinates.
juce::Rectangle<int> restoreFloating(const FloatingState &s, const std::vector<MonitorArea> &screens) {
    if (screens.empty())
        return {0, 0, 800, 420};
    window_place::Place p;
    p.display = s.monitor;
    p.area = {s.areaX, s.areaY, s.areaWidth, s.areaHeight};
    p.dpi = s.dpi;
    p.rect = {s.x, s.y, s.width, s.height};
    return fromPlace(window_place::restore(p, asPlaces(screens), 100, 100));
}
std::vector<MonitorArea> currentMonitors() {
    std::vector<MonitorArea> result;
    for (const auto &d : juce::Desktop::getInstance().getDisplays().displays)
        result.push_back({displayId(d), d.userBounds.toNearestInt(), d.logicalBounds.toNearestInt(),
                          static_cast<int>(d.dpi), d.isMain});
    return result;
}
AnalyserModel::AnalyserModel(std::shared_ptr<const engine::ScopeTap> tap) : tap_(std::move(tap)) {
    broker_ = std::make_shared<SharedAnalysis>([tap = tap_](std::int64_t, engine::ScopePoint) { return tap; },
                                              [](std::int64_t, engine::ScopePoint) {}, SharedAnalysis::Mode::Manual);
}
AnalyserModel::AnalyserModel(std::shared_ptr<SharedAnalysis> broker, std::int64_t track)
    : broker_(std::move(broker)), track_(track) {}
AnalyserModel::~AnalyserModel() = default;
void AnalyserModel::analyseOnce() noexcept {
    if (!broker_) return;
    try { if (!diagnostic_) diagnostic_ = broker_->acquire({track_}); broker_->pump(); } catch (...) {}
}
bool AnalyserModel::read(std::array<float, 513> &out, std::uint64_t &revision) const noexcept {
    const auto result = diagnostic_.read();
    if (!result || result->revision == revision || result->binsDbfs.size() != out.size()) return false;
    std::copy(result->binsDbfs.begin(), result->binsDbfs.end(), out.begin()); revision = result->revision;
    return true;
}
double AnalyserModel::sampleRate() const noexcept {
    if (const auto result = diagnostic_.read()) return result->sampleRate;
    return tap_ ? tap_->sampleRate() : 0;
}
AnalyserView::AnalyserView(std::shared_ptr<AnalyserModel> model) : model_(std::move(model)) {
    setName("Track spectrum");
    addAndMakeVisible(view_);
    view_.present(model_->broker(), model_->track());
}
void AnalyserView::frame() noexcept {
    view_.present(model_->broker(), model_->track()); view_.frame();
}
void AnalyserView::paint(juce::Graphics &g) {
    g.fillAll(look::editorBack);
}
namespace {
// An ADI view at its floating window's own interface zoom, as ZoomHost holds the main
// window's root: the view lays out in logical units and the transform scales it.
struct Zoomed final : juce::Component {
    explicit Zoomed(juce::Component &c) : view(c) {
        addAndMakeVisible(view);
        setSize(view.getWidth(), view.getHeight());
    }
    void resized() override {
        view.setTransform(juce::AffineTransform::scale(zoom));
        view.setBounds(0, 0, juce::roundToInt(static_cast<float>(getWidth()) / zoom),
                       juce::roundToInt(static_cast<float>(getHeight()) / zoom));
    }
    juce::Component &view;
    float zoom = 1;
};
// A floating window's name in `window.zoom` (N4): "floating:" and its key.
std::string zoomKey(const std::string &key) { return "floating:" + key; }
} // namespace
struct FloatingHost::Entry {
    std::string key;
    std::int64_t device = 0;
    bool closing = false, scaled = true, hidden = false, onTop = false, userResizable = false;
    // WIN-03: an owned window (a plug-in's editor, a device's view) goes with the main window: no task
    // bar button, minimised and back with it, above it. The Mix View's window is a normal one.
    bool ownedKind = false, byOwner = false;
    std::string title; // the window's title, where it is not the key
    std::uint64_t drains = 0;
    juce::Component::SafePointer<juce::Component> component, dock;
    juce::Rectangle<int> dockBounds, first;
    std::unique_ptr<juce::Component> owned;
    std::unique_ptr<Zoomed> zoomed; // an ADI view's, in openView's windows
    std::function<void(const SnapshotReader &)> onFrame;
    struct Window final : juce::DocumentWindow, juce::KeyListener {
        Window(FloatingHost &host, Entry &entry)
            : DocumentWindow(entry.title.empty() ? entry.key : entry.title, look::ink,
                             entry.scaled ? allButtons : (closeButton | minimiseButton), false),
              h(host),
              e(entry), clock(this, [this](double) {
                  followTooltips(*this, h.commands_, tips, tipDelayMs);
                  h.frame(e.key);
              }) {
            // On the desktop here, not in the base's constructor, so the kind's style flags are this
            // window's own.
            if (host.native_)
                addToDesktop(getDesktopWindowStyleFlags());
            setResizable(true, false);
            addKeyListener(this);
            // Tooltips are on in every native window (ADR-0203 d12), as the main one, and
            // follow the Settings window as it does.
            followTooltips(*this, h.commands_, tips, tipDelayMs);
        }
        using juce::DocumentWindow::keyPressed;
        using juce::DocumentWindow::keyStateChanged;
        int getDesktopWindowStyleFlags() const override {
            auto style = DocumentWindow::getDesktopWindowStyleFlags();
            if (e.ownedKind)
                style &= ~juce::ComponentPeer::windowAppearsOnTaskbar;
            return style;
        }
        bool keyPressed(const juce::KeyPress &k, juce::Component *) override {
            // Esc closes an owned window, when it reaches the window: a plug-in that takes the key
            // keeps it (WIN-03).
            if (e.ownedKind && k == juce::KeyPress(juce::KeyPress::escapeKey)) {
                closeButtonPressed();
                return true;
            }
            // The zoom keys zoom this window; every other key is the main window's.
            const auto id = h.commands_.manager.getKeyMappings()->findCommandForKeyPress(k);
            if (e.zoomed && (id == AppCommands::ZoomIn || id == AppCommands::ZoomOut)) {
                h.zoom(e.key, id == AppCommands::ZoomIn);
                return true;
            }
            if (e.key == "Mix View" && (id == AppCommands::FocusBrowser || id == AppCommands::SearchBrowser))
                return h.root_.mixViewPanel.focusChannelSearch();
            return h.commands_.key(k, h.root_, !e.ownedKind);
        }
        // A key held from here comes up here: the main window lets it go.
        bool keyStateChanged(bool isKeyDown, juce::Component *) override { return h.root_.keyStateChanged(isKeyDown); }
        void closeButtonPressed() override {
            e.closing = true;
            if (e.component.getComponent() == &h.root_.mixViewPanel)
                h.root_.mixViewPanel.stack.suspendParameters(true);
            setVisible(false);
        }
        // WIN-02: saved once it stands still after a move, so it keeps its place after a kill; kept on a
        // screen when the displays change.
        void moved() override {
            DocumentWindow::moved();
            place.moved();
        }
        void resized() override {
            DocumentWindow::resized();
            place.moved();
        }
        // SC-328: a plug-in window resized from its edges has an edge to take. A plug-in's own native
        // view covers the window's content to the pixel, so the edge outside it is all the mouse can
        // grab; one pixel of it, as the desk showed with Surge XT's CLAP, is none in practice.
        juce::BorderSize<int> getBorderThickness() const override {
            if (e.userResizable && !isFullScreen() && !isKioskMode() && !isUsingNativeTitleBar())
                return juce::BorderSize<int>(kResizableEdge);
            return DocumentWindow::getBorderThickness();
        }
        static constexpr int kResizableEdge = 6;
        void parentSizeChanged() override {
            DocumentWindow::parentSizeChanged();
            if (keepReachable(*this, isResizable()))
                place.moved();
        }
        void minimisationStateChanged(bool minimised) override {
            if (e.component.getComponent() == &h.root_.mixViewPanel) {
                h.root_.mixViewPanel.suspendAnalysis(minimised);
                h.root_.mixViewPanel.stack.suspendParameters(minimised || e.hidden || e.closing);
                if (minimised) h.root_.mixViewPanel.stack.cancelInteractions();
            }
            if (auto *analysis = dynamic_cast<AnalysisPane *>(e.component.getComponent())) analysis->suspend(minimised || e.hidden || e.closing);
        }
        FloatingHost &h;
        Entry &e;
        std::unique_ptr<juce::TooltipWindow> tips;
        int tipDelayMs = 0;
        PlaceWatch place;
        juce::VBlankAttachment clock;
    };
    std::unique_ptr<Window> window;
};
FloatingHost::FloatingHost(AdiRootComponent &r, AppCommands &c, ViewStateStore &s,
                           engine::ProjectView &p, std::function<bool(std::int64_t)> alive,
                           bool native)
    : root_(r), commands_(c), states_(s), project_(p), alive_(std::move(alive)), native_(native) {}
FloatingHost::~FloatingHost() {
    // The main window's activation and minimise callbacks never reach a host that is gone (WIN-03).
    if (root_.pluginHost == this)
        root_.pluginHost = nullptr;
    closeAll(true);
}
bool FloatingHost::install(std::unique_ptr<Entry> e) {
    if (entries_.contains(e->key) || !e->component || !alive_(e->device))
        return false;
    const auto key = e->key;
    auto *raw = e.get();
    entries_[key] = std::move(e);
    raw->window = std::make_unique<Entry::Window>(*this, *raw);
    if (raw->owned && raw->scaled)
        raw->zoomed = std::make_unique<Zoomed>(*raw->component);
    if (raw->owned && !raw->scaled) {
        // A plug-in's own window takes the plug-in's size: the window fits it; it is resized from
        // outside only when the plug-in can be (`userResizable`, SC-328), each size then asked of the
        // plug-in, and that is decided here, before the window is on the screen (SC-322). A kept place
        // moves it, never sizes it.
        raw->window->setContentNonOwned(raw->component.getComponent(), true);
        raw->window->setResizable(raw->userResizable, false);
        if (const auto kept = states_.loadFloating(key); !kept.monitor.empty())
            raw->window->setTopLeftPosition(restoreFloating(kept, currentMonitors()).getTopLeft());
        else if (const auto screens = currentMonitors(); !screens.empty()) {
            // Never placed: cascaded off the windows already open, from the main display's corner (WIN-03).
            auto home = screens.front();
            for (const auto &m : screens)
                if (m.main) {
                    home = m;
                    break;
                }
            std::vector<window_place::Rect> others;
            for (const auto &[k, other] : entries_)
                if (other.get() != raw && other->window)
                    others.push_back(toPlace(other->window->getBounds()));
            const auto corner = home.usable.getPosition() + juce::Point<int>(2 * window_place::kCascade, 2 * window_place::kCascade);
            const auto start = toPlace(raw->window->getBounds().withPosition(corner));
            raw->window->setTopLeftPosition(fromPlace(window_place::cascade(start, others, toPlace(home.usable))).getTopLeft());
        }
        if (native_)
            raw->window->setVisible(true);
        save(*raw, true);
        raw->window->place.save = [this, raw] { save(*raw, true); };
        return true;
    }
    raw->window->setContentNonOwned(raw->zoomed ? static_cast<juce::Component *>(raw->zoomed.get())
                                                : raw->component.getComponent(),
                                    false);
    if (const auto kept = states_.loadFloating(key); kept.monitor.empty() && !raw->first.isEmpty()) {
        // Never placed: the content where `first` says, the title bar above it.
        raw->window->setBounds(raw->window->getContentComponentBorder().addedTo(raw->first));
    } else if (kept.monitor.empty()) {
        // Never placed and nowhere asked: cascaded off the windows already open (WIN-02).
        const auto screens = currentMonitors();
        const auto start = restoreFloating(kept, screens);
        std::vector<window_place::Rect> others;
        for (const auto &[k, other] : entries_)
            if (other.get() != raw && other->window)
                others.push_back(toPlace(other->window->getBounds()));
        const auto usable = screens.empty() ? start : screens.front().usable;
        raw->window->setBounds(fromPlace(window_place::cascade(toPlace(start), others, toPlace(usable))));
    } else
        raw->window->setBounds(restoreFloating(kept, currentMonitors()));
    // An ADI view's window opens at the zoom kept for it on its display (N4).
    if (raw->zoomed)
        if (const auto kept = commands_.keptZoom(zoomKey(key), displayAt(raw->window->getBounds().getCentre()))) {
            raw->zoomed->zoom = *kept;
            raw->zoomed->resized();
        }
    if (native_)
        raw->window->setVisible(true);
    save(*raw, true);
    raw->window->place.save = [this, raw] { save(*raw, true); };
    return true;
}
bool FloatingHost::detach(const std::string &key, std::int64_t id, juce::Component &component,
                          juce::Component &dock, std::function<void(const SnapshotReader &)> fn,
                          juce::Rectangle<int> first) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->device = id;
    e->component = &component;
    e->dock = &dock;
    e->dockBounds = component.getBounds();
    e->first = first;
    e->onFrame = std::move(fn);
    return install(std::move(e));
}
bool FloatingHost::openView(const std::string &key, std::int64_t id,
                            std::unique_ptr<juce::Component> component,
                            std::function<void(const SnapshotReader &)> fn, bool scaled, std::string title,
                            bool userResizable) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->title = std::move(title);
    e->device = id;
    e->component = component.get();
    e->owned = std::move(component);
    e->onFrame = std::move(fn);
    e->scaled = scaled;
    e->userResizable = userResizable;
    e->ownedKind = true;
    return install(std::move(e));
}
void FloatingHost::show(const std::string &key, bool on) {
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
    e.hidden = !on;
    if (auto *analysis = dynamic_cast<AnalysisPane *>(e.component.getComponent()))
        analysis->suspend(!on || e.closing || (e.window && e.window->isMinimised()));
    if (e.component.getComponent() == &root_.mixViewPanel)
        root_.mixViewPanel.stack.suspendParameters(!on || e.closing || (e.window && e.window->isMinimised()));
    if (native_ && e.window && !e.closing)
        e.window->setVisible(on && !e.byOwner);
}
void FloatingHost::ownerMinimised(bool minimised) {
    for (auto &[key, e] : entries_)
        if (e->ownedKind && e->window) {
            e->byOwner = minimised;
            if (native_ && !e->closing)
                e->window->setVisible(!minimised && !e->hidden);
        }
}
void FloatingHost::ownerToFront() {
#if JUCE_LINUX || JUCE_BSD
    // JUCE's X11 toFront activates the window whatever it is asked (_NET_ACTIVE_WINDOW): it would take the
    // keys from the main window and bring back one the user minimised. On X11 the owned windows wait for
    // the transient-for hint, with the other platforms' owner windows.
    return;
#else
    // A raise that takes no focus; a window the user minimised stays down (Windows' toFront restores one).
    for (auto &[key, e] : entries_)
        if (e->ownedKind && e->window && !e->closing && e->window->isVisible() && !e->window->isMinimised())
            e->window->toFront(false);
#endif
}
bool FloatingHost::owned(const std::string &key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && it->second->ownedKind;
}
bool FloatingHost::withOwner(const std::string &key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && it->second->byOwner;
}
bool FloatingHost::taskbarButton(const std::string &key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && it->second->window &&
           (it->second->window->getDesktopWindowStyleFlags() & juce::ComponentPeer::windowAppearsOnTaskbar) != 0;
}
bool FloatingHost::shown(const std::string &key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && !it->second->hidden && !it->second->closing;
}
void FloatingHost::setAlwaysOnTop(const std::string &key, bool onTop) {
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    it->second->onTop = onTop;
    if (!it->second->window)
        return;
    // SC-313: an owned window floats above ADI's own window, not above every program.
    if (native_ && it->second->ownedKind &&
        floatAboveOwner(*it->second->window, root_.getTopLevelComponent(), onTop)) {
        it->second->window->setAlwaysOnTop(false);
        return;
    }
    it->second->window->setAlwaysOnTop(onTop);
}
bool FloatingHost::floatAboveOwner(juce::Component &window, juce::Component *owner, bool on) {
#if JUCE_WINDOWS
    auto *peer = window.getPeer();
    auto *ownerPeer = owner != nullptr ? owner->getPeer() : nullptr;
    if (peer == nullptr || (on && ownerPeer == nullptr))
        return false;
    const auto hwnd = static_cast<HWND>(peer->getNativeHandle());
    // A topmost window stays topmost whatever its owner: take that off first.
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT,
                      on ? reinterpret_cast<LONG_PTR>(static_cast<HWND>(ownerPeer->getNativeHandle())) : 0);
    return true;
#else
    juce::ignoreUnused(window, owner, on);
    return false;
#endif
}
bool FloatingHost::nativelyOwnedBy(juce::Component &window, juce::Component &owner) {
#if JUCE_WINDOWS
    auto *peer = window.getPeer();
    auto *ownerPeer = owner.getPeer();
    return peer != nullptr && ownerPeer != nullptr &&
           GetWindow(static_cast<HWND>(peer->getNativeHandle()), GW_OWNER) == static_cast<HWND>(ownerPeer->getNativeHandle());
#else
    juce::ignoreUnused(window, owner);
    return false;
#endif
}
bool FloatingHost::nativelyTopmost(juce::Component &window) {
#if JUCE_WINDOWS
    auto *peer = window.getPeer();
    return peer != nullptr &&
           (GetWindowLongPtrW(static_cast<HWND>(peer->getNativeHandle()), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
#else
    juce::ignoreUnused(window);
    return false;
#endif
}
bool FloatingHost::alwaysOnTop(const std::string &key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() && it->second->onTop;
}
std::vector<std::string> FloatingHost::keys(const std::string &prefix) const {
    std::vector<std::string> out;
    for (const auto &[key, entry] : entries_)
        if (key.rfind(prefix, 0) == 0)
            out.push_back(key);
    return out;
}
void FloatingHost::frame(const std::string &key) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
    if (!alive_(e.device)) {
        e.closing = true;
        if (e.component.getComponent() == &root_.mixViewPanel)
            root_.mixViewPanel.stack.suspendParameters(true);
        e.window->setVisible(false);
        return;
    }
    SnapshotReader reader(project_.current());
    ++e.drains;
    if (e.onFrame)
        e.onFrame(reader);
}
bool FloatingHost::zoom(const std::string &key, bool in) {
    const auto it = entries_.find(key);
    if (it == entries_.end() || !it->second->zoomed)
        return false;
    auto &zoomed = *it->second->zoomed;
    const auto next = nextZoom(zoomed.zoom, in);
    if (!next)
        return false;
    zoomed.zoom = *next;
    zoomed.resized();
    // Kept for this window on the display it stands on, for the next time it opens (N4).
    if (std::string why; !commands_.keepZoom(zoomKey(key), displayAt(it->second->window->getBounds().getCentre()),
                                             zoomed.zoom, why))
        root_.say("The zoom is not kept for the next launch: " + why);
    return true;
}
float FloatingHost::zoomOf(const std::string &key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() || !it->second->zoomed ? 1.f : it->second->zoomed->zoom;
}
void FloatingHost::save(Entry &e, bool open) {
    auto state = states_.loadFloating(e.key);
    const auto b = e.window->getBounds();
    state.x = b.getX();
    state.y = b.getY();
    state.width = b.getWidth();
    state.height = b.getHeight();
    state.open = open;
    // The monitor showing most of it, with its area and DPI then (WIN-02).
    const auto at = window_place::placeOf(toPlace(b), false, asPlaces(currentMonitors()));
    if (!at.display.empty()) {
        state.monitor = at.display;
        state.areaX = at.area.x;
        state.areaY = at.area.y;
        state.areaWidth = at.area.w;
        state.areaHeight = at.area.h;
        state.dpi = at.dpi;
    }
    std::string ignored;
    states_.saveFloating(e.key, state, ignored);
}
void FloatingHost::flushPlaces() {
    for (auto &[key, e] : entries_)
        if (e->window)
            e->window->place.flush();
}
void FloatingHost::close(const std::string &key) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
    e.window->place.save = nullptr; // closed: its last place is the one saved now
    save(e, false);
    if (e.component.getComponent() == &root_.mixViewPanel)
        root_.mixViewPanel.stack.suspendParameters(true);
    // Gone before its content leaves it: an empty window never shows (the director's R3).
    e.window->setVisible(false);
    e.window->clearContentComponent();
    if (e.dock && e.component) {
        e.dock->addAndMakeVisible(e.component);
        e.component->setBounds(e.dockBounds);
    }
    // The content before its window: a plug-in's editor tears its native view down while the
    // window that parents it still exists. Its frame callback goes first: an editor that pumps
    // messages as it closes lets the window's clock fire, which must not reach the freed editor (SC-321).
    e.onFrame = nullptr;
    e.zoomed.reset();
    e.owned.reset();
    entries_.erase(it);
}
void FloatingHost::requestClose(const std::string &key) {
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    it->second->closing = true;
    if (auto *analysis = dynamic_cast<AnalysisPane *>(it->second->component.getComponent()))
        analysis->suspend(true);
    if (it->second->component.getComponent() == &root_.mixViewPanel)
        root_.mixViewPanel.stack.suspendParameters(true);
    if (it->second->window)
        it->second->window->setVisible(false);
}
void FloatingHost::collect() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto key = it->first;
        bool closeNow = it->second->closing || !alive_(it->second->device);
        ++it;
        if (closeNow)
            close(key);
    }
}
void FloatingHost::closeAll(bool remember) {
    while (!entries_.empty()) {
        auto &e = *entries_.begin()->second;
        save(e, remember);
        if (e.component.getComponent() == &root_.mixViewPanel)
            root_.mixViewPanel.stack.suspendParameters(true);
        e.window->setVisible(false);
        e.window->clearContentComponent();
        if (e.dock && e.component) {
            e.dock->addAndMakeVisible(e.component);
            e.component->setBounds(e.dockBounds);
        }
        // The content before its window: a plug-in's editor tears its native view down while
        // the window that parents it still exists; its frame callback first (SC-321).
        e.onFrame = nullptr;
        e.zoomed.reset();
        e.owned.reset();
        entries_.erase(entries_.begin());
    }
}
namespace {
constexpr const char *kMixViewKey = "Mix View";
}
bool detachMixView(FloatingHost &host, AdiRootComponent &root, bool detach) {
    root.mixViewPanel.stack.cancelInteractions();
    root.devices.cancelFunctionInteractions();
    root.mixViewPanel.suspendAnalysis(false);
    if (!detach) {
        // Back where it docks: the host returns the panel to the root, which lays it out.
        host.close(kMixViewKey);
        return root.setMixView(root.state().mixView, false);
    }
    if (!root.setMixView(true, true))
        return false;
    if (host.content(kMixViewKey) != nullptr)
        return true;
    auto &panel = root.mixViewPanel;
    // Its first window opens where it stands docked, at its size; after that, where it was left.
    if (host.detach(kMixViewKey, 0, panel, root, [&panel](const SnapshotReader &reader) { panel.frame(reader); },
                    root.localAreaToGlobal(root.mixViewDock())))
        return true;
    root.setMixView(true, false);
    return false;
}
bool toggleMixViewWindow(FloatingHost &host, AdiRootComponent &root) {
    if (host.content(kMixViewKey) == nullptr)
        return detachMixView(host, root, true);
    host.close(kMixViewKey);
    return root.setMixView(false, true);
}
void syncMixView(FloatingHost &host, AdiRootComponent &root, bool restore) {
    const auto &state = root.state();
    if (!state.mixView || !state.mixViewDetached || host.content(kMixViewKey) != nullptr)
        return;
    if (restore)
        detachMixView(host, root, true);
    else
        root.setMixView(false, true); // its window was closed by its own button
}
bool showAnalyzerWindow(FloatingHost &host, AdiRootComponent &root, std::int64_t track) {
    constexpr const char *key = "Analysis";
    if (track <= 0) track = root.analysisTrack();
    if (track != 0 && !root.reader().findTrack(track)) return false;
    if (track != 0 && track != root.analysisTrack() &&
        track != root.arrangement.geometry.selection.track && root.reader().findTrack(track)) {
        root.arrangement.geometry.selection.track = track;
        root.publishSelection();
    }
    if (auto *existing = dynamic_cast<AnalysisPane *>(host.content(key))) {
        existing->setSource(track == root.analysisTrack() ? AnalysisPane::Source::Main : AnalysisPane::Source::Selected);
        host.show(key, true); return true;
    }
    auto pane = std::make_unique<AnalysisPane>();
    pane->setSize(800, 420);
    const auto &settings = root.mixViewPanel.analysis.detail;
    pane->view.setClassicControls(settings.view.classicControls());
    pane->setMode(settings.mode());
    pane->view.setFilledSpectrumCurve(settings.view.filledSpectrumCurve());
    pane->setPoint(settings.point());
    pane->view.setChannel(settings.view.channel());
    pane->channelBox.setSelectedId(static_cast<int>(pane->view.channel()) + 1, juce::dontSendNotification);
    pane->setPalette(settings.view.palette(), settings.view.paletteReversed());
    pane->setSpectrumSlope(settings.view.spectrumSlope());
    pane->setSpectrumScale(settings.view.spectrumScale());
    pane->setHistoryMotion(settings.view.historyMotion());
    pane->setSource(track == root.analysisTrack() ? AnalysisPane::Source::Main : AnalysisPane::Source::Selected);
    pane->expandButton.setButtonText("Mix View");
    pane->onExpand = [&host, owner = juce::Component::SafePointer<AdiRootComponent>(&root),
                     source = juce::Component::SafePointer<AnalysisPane>(pane.get())](std::int64_t channel) {
        if (!owner || !source) return;
        const auto mode = source->mode();
        if (!owner->state().mixView) {
            const bool opened = owner->state().mixViewDetached ? detachMixView(host, *owner.getComponent(), true)
                                                               : owner->setMixView(true, false);
            if (!opened || !owner) return;
        }
        if (owner->state().mixViewDetached && !host.shown(kMixViewKey)) {
            host.show(kMixViewKey, true);
            if (!owner) return;
        }
        // An ordinary open may choose Focus when several channels are soloed.
        if (owner->mixViewPanel.tab() != MixViewPanel::Tab::Analysis &&
            !owner->chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Analysis)))
            return;
        if (!owner) return;

        const SnapshotReader snapshot(owner->current());
        if (channel <= 0 || !snapshot.findTrack(channel)) {
            channel = owner->analysisTrack(snapshot, true);
            if (channel == 0) channel = owner->analysisTrack(snapshot);
        }
        if (channel > 0) {
            owner->arrangement.geometry.selection.track = channel;
            owner->publishSelection();
            if (!owner) return;
        }
        auto &analysis = owner->mixViewPanel.analysis;
        analysis.followSelection(channel != owner->analysisTrack(snapshot));
        analysis.setMode(mode);
        if (owner) owner->mixViewPanel.frame(snapshot);
    };
    auto *view = pane.get();
    return host.openView(key, 0, std::move(pane), [&root, view](const SnapshotReader &reader) {
        view->frame(root.sharedAnalysis, root.analysisTrack(reader), root.analysisTrack(reader, true));
    }, true, "Analysis");
}
juce::Component *FloatingHost::content(const std::string &key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second->component.getComponent();
}
juce::Component *FloatingHost::window(const std::string &key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second->window.get();
}
bool FloatingHost::hasTooltips(const std::string &key) const {
    auto it = entries_.find(key);
    return it != entries_.end() && it->second->window && it->second->window->tips != nullptr;
}
std::uint64_t FloatingHost::drains(const std::string &key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second->drains;
}
} // namespace adi::ui
