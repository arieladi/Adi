// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_floating.hpp"
#include <chrono>
namespace adi::ui {
juce::Rectangle<int> restoreFloating(const FloatingState &s,
                                     const std::vector<MonitorArea> &screens) {
    if (screens.empty())
        return {0, 0, 800, 420};
    auto chosen = screens.begin();
    for (auto it = screens.begin(); it != screens.end(); ++it)
        if (it->id == s.monitor) {
            chosen = it;
            break;
        }
    const auto area = chosen->usable;
    const int w = std::clamp(s.width, 1, area.getWidth()),
              h = std::clamp(s.height, 1, area.getHeight());
    return {std::clamp(s.x, area.getX(), area.getRight() - w),
            std::clamp(s.y, area.getY(), area.getBottom() - h), w, h};
}
std::vector<MonitorArea> currentMonitors() {
    std::vector<MonitorArea> result;
    for (const auto &d : juce::Desktop::getInstance().getDisplays().displays)
        result.push_back({displayId(d), d.userBounds.toNearestInt()});
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
    g.fillAll(juce::Colour(0xff101820));
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
    bool closing = false, scaled = true, hidden = false, onTop = false;
    std::string title; // the window's title, where it is not the key
    std::uint64_t drains = 0;
    juce::Component::SafePointer<juce::Component> component, dock;
    juce::Rectangle<int> dockBounds, first;
    std::unique_ptr<juce::Component> owned;
    std::unique_ptr<Zoomed> zoomed; // an ADI view's, in openView's windows
    std::function<void(const SnapshotReader &)> onFrame;
    struct Window final : juce::DocumentWindow, juce::KeyListener {
        Window(FloatingHost &host, Entry &entry)
            : DocumentWindow(entry.title.empty() ? entry.key : entry.title, juce::Colours::black,
                             entry.scaled ? allButtons : (closeButton | minimiseButton), host.native_),
              h(host),
              e(entry), clock(this, [this](double) {
                  followTooltips(*this, h.commands_, tips, tipDelayMs);
                  h.frame(e.key);
              }) {
            setResizable(true, false);
            addKeyListener(this);
            // Tooltips are on in every native window (ADR-0203 d12), as the main one, and
            // follow the Settings window as it does.
            followTooltips(*this, h.commands_, tips, tipDelayMs);
        }
        using juce::DocumentWindow::keyPressed;
        using juce::DocumentWindow::keyStateChanged;
        bool keyPressed(const juce::KeyPress &k, juce::Component *) override {
            // The zoom keys zoom this window; every other key is the main window's.
            const auto id = h.commands_.manager.getKeyMappings()->findCommandForKeyPress(k);
            if (e.zoomed && (id == AppCommands::ZoomIn || id == AppCommands::ZoomOut)) {
                h.zoom(e.key, id == AppCommands::ZoomIn);
                return true;
            }
            return h.commands_.key(k, h.root_);
        }
        // A key held from here comes up here: the main window lets it go.
        bool keyStateChanged(bool isKeyDown, juce::Component *) override { return h.root_.keyStateChanged(isKeyDown); }
        void closeButtonPressed() override {
            e.closing = true;
            if (e.component.getComponent() == &h.root_.mixViewPanel)
                h.root_.mixViewPanel.stack.suspendParameters(true);
            setVisible(false);
        }
        void minimisationStateChanged(bool minimised) override {
            if (e.component.getComponent() == &h.root_.mixViewPanel) {
                h.root_.mixViewPanel.suspendAnalysis(minimised);
                h.root_.mixViewPanel.stack.suspendParameters(minimised || e.hidden || e.closing);
                if (minimised) h.root_.mixViewPanel.stack.cancelInteractions();
            }
            if (auto *analysis = dynamic_cast<AnalysisPane *>(e.component.getComponent())) analysis->suspend(minimised);
        }
        FloatingHost &h;
        Entry &e;
        std::unique_ptr<juce::TooltipWindow> tips;
        int tipDelayMs = 0;
        juce::VBlankAttachment clock;
    };
    std::unique_ptr<Window> window;
};
FloatingHost::FloatingHost(AdiRootComponent &r, AppCommands &c, ViewStateStore &s,
                           engine::ProjectView &p, std::function<bool(std::int64_t)> alive,
                           bool native)
    : root_(r), commands_(c), states_(s), project_(p), alive_(std::move(alive)), native_(native) {}
FloatingHost::~FloatingHost() { closeAll(true); }
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
        // A plug-in's own window takes the plug-in's size, which only the plug-in changes: the
        // window fits it and is not resized from outside; a kept place moves it, never sizes it.
        raw->window->setContentNonOwned(raw->component.getComponent(), true);
        raw->window->setResizable(false, false);
        if (const auto kept = states_.loadFloating(key); !kept.monitor.empty())
            raw->window->setTopLeftPosition(restoreFloating(kept, currentMonitors()).getTopLeft());
        if (native_)
            raw->window->setVisible(true);
        save(*raw, true);
        return true;
    }
    raw->window->setContentNonOwned(raw->zoomed ? static_cast<juce::Component *>(raw->zoomed.get())
                                                : raw->component.getComponent(),
                                    false);
    if (const auto kept = states_.loadFloating(key); kept.monitor.empty() && !raw->first.isEmpty()) {
        // Never placed: the content where `first` says, the title bar above it.
        raw->window->setBounds(raw->window->getContentComponentBorder().addedTo(raw->first));
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
                            std::function<void(const SnapshotReader &)> fn, bool scaled, std::string title) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->title = std::move(title);
    e->device = id;
    e->component = component.get();
    e->owned = std::move(component);
    e->onFrame = std::move(fn);
    e->scaled = scaled;
    return install(std::move(e));
}
void FloatingHost::show(const std::string &key, bool on) {
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
    e.hidden = !on;
    if (e.component.getComponent() == &root_.mixViewPanel)
        root_.mixViewPanel.stack.suspendParameters(!on || e.closing || (e.window && e.window->isMinimised()));
    if (native_ && e.window && !e.closing)
        e.window->setVisible(on);
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
    if (it->second->window)
        it->second->window->setAlwaysOnTop(onTop);
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
    const auto screens = currentMonitors();
    for (const auto &screen : screens)
        if (screen.usable.contains(b.getCentre())) {
            state.monitor = screen.id;
            break;
        }
    std::string ignored;
    states_.saveFloating(e.key, state, ignored);
}
void FloatingHost::close(const std::string &key) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
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
    // window that parents it still exists.
    e.zoomed.reset();
    e.owned.reset();
    entries_.erase(it);
}
void FloatingHost::requestClose(const std::string &key) {
    const auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    it->second->closing = true;
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
        // the window that parents it still exists.
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
        host.show(key, true); existing->suspend(false); return true;
    }
    auto pane = std::make_unique<AnalysisPane>();
    pane->setSize(800, 420);
    pane->setMode(root.mixViewPanel.analysis.mode());
    pane->setPoint(root.mixViewPanel.analysis.point());
    pane->setSource(track == root.analysisTrack() ? AnalysisPane::Source::Main : AnalysisPane::Source::Selected);
    pane->expandButton.setButtonText("Mix View");
    pane->onExpand = [&root](std::int64_t channel) {
        if (root.reader().findTrack(channel)) { root.arrangement.geometry.selection.track = channel; root.publishSelection(); }
        root.mixViewPanel.analysis.setSource(channel == root.analysisTrack() ? AnalysisPane::Source::Main : AnalysisPane::Source::Selected);
        root.chooseMixViewTab(static_cast<int>(MixViewPanel::Tab::Analysis));
        root.setMixView(true, root.state().mixViewDetached);
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
