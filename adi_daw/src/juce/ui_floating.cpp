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
    for (const auto &d : juce::Desktop::getInstance().getDisplays().displays) {
        // JUCE exposes no stable EDID identifier; geometry + physical DPI is its portable identity.
        const auto a = d.logicalBounds.toNearestInt();
        result.push_back({std::to_string(a.getX()) + ":" + std::to_string(a.getY()) + ":" +
                              std::to_string(a.getWidth()) + ":" + std::to_string(a.getHeight()) +
                              ":" + std::to_string(static_cast<int>(d.dpi)),
                          d.userBounds.toNearestInt()});
    }
    return result;
}
AnalyserModel::AnalyserModel(std::shared_ptr<const engine::ScopeTap> tap) : tap_(std::move(tap)) {
    for (auto &b : bins_)
        b.store(-120, std::memory_order_relaxed);
    worker_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            analyseOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
}
AnalyserModel::~AnalyserModel() {
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
}
void AnalyserModel::analyseOnce() noexcept {
    std::int64_t stamp = 0;
    if (!tap_->read(left_, right_, stamp))
        return;
    for (std::size_t i = 0; i < left_.size(); ++i)
        left_[i] = (left_[i] + right_[i]) * .5f;
    spectrum_.analyse(left_, scratch_);
    revision_.fetch_add(1, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < bins_.size(); ++i)
        bins_[i].store(dsp::toDbfs(scratch_[i]), std::memory_order_seq_cst);
    revision_.fetch_add(1, std::memory_order_seq_cst);
}
bool AnalyserModel::read(std::array<float, 513> &out, std::uint64_t &revision) const noexcept {
    const auto before = revision_.load(std::memory_order_seq_cst);
    if (!before || (before & 1) || before == revision)
        return false;
    std::array<float, 513> next{};
    for (std::size_t i = 0; i < out.size(); ++i)
        next[i] = bins_[i].load(std::memory_order_seq_cst);
    if (revision_.load(std::memory_order_seq_cst) != before)
        return false;
    out = next;
    revision = before;
    return true;
}
AnalyserView::AnalyserView(std::shared_ptr<AnalyserModel> model) : model_(std::move(model)) {
    bins_.fill(-120);
    setName("Track spectrum");
}
void AnalyserView::frame() noexcept {
    if (model_->read(bins_, revision_))
        repaint();
}
void AnalyserView::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff101820));
    g.setColour(juce::Colour(0xff334450));
    for (int db = 0; db >= -120; db -= 24) {
        int y = static_cast<int>(-db / 120. * getHeight());
        g.drawHorizontalLine(y, 0.f, static_cast<float>(getWidth()));
    }
    g.setColour(juce::Colour(0xff72ced8));
    juce::Path line;
    for (std::size_t i = 1; i < bins_.size(); ++i) {
        const double hz = static_cast<double>(i) * model_->sampleRate() / 1024.;
        const float x =
            static_cast<float>(std::log(std::max(20., hz) / 20.) / std::log(1000.) * getWidth());
        const float y = static_cast<float>(
            std::clamp(-static_cast<double>(bins_[i]) / 120., 0., 1.) * getHeight());
        if (i == 1)
            line.startNewSubPath(x, y);
        else
            line.lineTo(x, y);
    }
    g.strokePath(line, juce::PathStrokeType(1.5f));
}
struct FloatingHost::Entry {
    std::string key;
    std::int64_t device = 0;
    bool closing = false;
    std::uint64_t drains = 0;
    juce::Component::SafePointer<juce::Component> component, dock;
    juce::Rectangle<int> dockBounds;
    std::unique_ptr<juce::Component> owned;
    std::function<void(const SnapshotReader &)> onFrame;
    struct Window final : juce::DocumentWindow, juce::KeyListener {
        Window(FloatingHost &host, Entry &entry)
            : DocumentWindow(entry.key, juce::Colours::black, allButtons, host.native_), h(host),
              e(entry), clock(this, [this](double) { h.frame(e.key); }) {
            setResizable(true, false);
            addKeyListener(this);
        }
        using juce::DocumentWindow::keyPressed;
        bool keyPressed(const juce::KeyPress &k, juce::Component *) override {
            return h.commands_.key(k, h.root_);
        }
        void closeButtonPressed() override {
            e.closing = true;
            setVisible(false);
        }
        FloatingHost &h;
        Entry &e;
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
    raw->window->setContentNonOwned(raw->component.getComponent(), false);
    raw->window->setBounds(restoreFloating(states_.loadFloating(key), currentMonitors()));
    if (native_)
        raw->window->setVisible(true);
    save(*raw, true);
    return true;
}
bool FloatingHost::detach(const std::string &key, std::int64_t id, juce::Component &component,
                          juce::Component &dock, std::function<void(const SnapshotReader &)> fn) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->device = id;
    e->component = &component;
    e->dock = &dock;
    e->dockBounds = component.getBounds();
    e->onFrame = std::move(fn);
    return install(std::move(e));
}
bool FloatingHost::openView(const std::string &key, std::int64_t id,
                            std::unique_ptr<juce::Component> component,
                            std::function<void(const SnapshotReader &)> fn) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->device = id;
    e->component = component.get();
    e->owned = std::move(component);
    e->onFrame = std::move(fn);
    return install(std::move(e));
}
void FloatingHost::frame(const std::string &key) {
    auto it = entries_.find(key);
    if (it == entries_.end())
        return;
    auto &e = *it->second;
    if (!alive_(e.device)) {
        e.closing = true;
        e.window->setVisible(false);
        return;
    }
    SnapshotReader reader(project_.current());
    ++e.drains;
    if (e.onFrame)
        e.onFrame(reader);
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
    e.window->clearContentComponent();
    if (e.dock && e.component) {
        e.dock->addAndMakeVisible(e.component);
        e.component->setBounds(e.dockBounds);
    }
    entries_.erase(it);
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
        e.window->clearContentComponent();
        if (e.dock && e.component) {
            e.dock->addAndMakeVisible(e.component);
            e.component->setBounds(e.dockBounds);
        }
        entries_.erase(entries_.begin());
    }
}
juce::Component *FloatingHost::content(const std::string &key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second->component.getComponent();
}
std::uint64_t FloatingHost::drains(const std::string &key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second->drains;
}
} // namespace adi::ui
