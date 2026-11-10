// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/spectrum.hpp"
#include "adi/engine/scope.hpp"
#include "ui_shell.hpp"
#include <array>
#include <thread>
#include <vector>
namespace adi::ui {
struct MonitorArea {
    std::string id;
    juce::Rectangle<int> usable;
};
juce::Rectangle<int> restoreFloating(const FloatingState &, const std::vector<MonitorArea> &);
std::vector<MonitorArea> currentMonitors();
// Model owns the analysis worker, not a repaint clock. Views share this one result.
class AnalyserModel {
  public:
    explicit AnalyserModel(std::shared_ptr<const engine::ScopeTap>);
    ~AnalyserModel();
    void analyseOnce() noexcept;
    bool read(std::array<float, 513> &, std::uint64_t &revision) const noexcept;
    double sampleRate() const noexcept { return tap_->sampleRate(); }

  private:
    std::shared_ptr<const engine::ScopeTap> tap_;
    dsp::Spectrum spectrum_{1024};
    std::array<float, 1024> left_{}, right_{};
    std::array<float, 513> scratch_{};
    std::array<std::atomic<float>, 513> bins_{};
    std::atomic<std::uint64_t> revision_{0};
    std::jthread worker_;
};
class AnalyserView final : public ClockedView {
  public:
    explicit AnalyserView(std::shared_ptr<AnalyserModel>);
    void frame() noexcept override;
    void paint(juce::Graphics &) override;
    const std::shared_ptr<AnalyserModel> &model() const { return model_; }

  private:
    std::shared_ptr<AnalyserModel> model_;
    std::array<float, 513> bins_{};
    std::uint64_t revision_ = 0;
};
class FloatingHost {
  public:
    FloatingHost(AdiRootComponent &, AppCommands &, ViewStateStore &, engine::ProjectView &,
                 std::function<bool(std::int64_t)> alive, bool native = true);
    ~FloatingHost();
    // `first`, on the desktop: where the window's content opens the first time, before the window
    // has a place of its own kept (for the Mix View, where it stood docked).
    bool detach(const std::string &key, std::int64_t device, juce::Component &,
                juce::Component &dock, std::function<void(const SnapshotReader &)> frame = {},
                juce::Rectangle<int> first = {});
    // `scaled`: an ADI view zooms with Ctrl/Cmd + and - (N4); a plug-in's own window opens at the
    // plug-in's size and is never scaled (parity-7.5 row 10).
    bool openView(const std::string &key, std::int64_t device, std::unique_ptr<juce::Component>,
                  std::function<void(const SnapshotReader &)> frame = {}, bool scaled = true,
                  std::string title = {});
    // Closed at the next collect(), hidden at once: safe from inside the window's own frame.
    void requestClose(const std::string &key);
    bool contains(const std::string &key) const { return entries_.contains(key); }
    // Shown or hidden without closing (Live's Show/Hide Plug-In Windows, Auto-Hide): a hidden
    // window keeps its content and its place.
    void show(const std::string &key, bool shown);
    bool shown(const std::string &key) const;
    void setAlwaysOnTop(const std::string &key, bool onTop);
    bool alwaysOnTop(const std::string &key) const;
    // The keys open under `prefix`, in key order.
    std::vector<std::string> keys(const std::string &prefix) const;
    void frame(const std::string &key); // per-window attachment; public for deterministic tests
    void collect();                     // message-thread lifecycle, not a renderer clock
    void close(const std::string &key);
    void closeAll(bool remember);
    std::size_t count() const noexcept { return entries_.size(); }
    bool hasTooltips(const std::string &key) const;
    juce::Component *content(const std::string &) const;
    // The window itself, its title bar and frame included; null while it is not open.
    juce::Component *window(const std::string &) const;
    // Ctrl/Cmd + and - in an ADI view's window zoom that window alone (ADR-0129 d3), held
    // for the session (window log, N4). A plug-in's own window is never scaled by ADI.
    bool zoom(const std::string &key, bool in);
    float zoomOf(const std::string &key) const;
    std::uint64_t drains(const std::string &) const;

  private:
    struct Entry;
    bool install(std::unique_ptr<Entry>);
    void save(Entry &, bool open);
    AdiRootComponent &root_;
    AppCommands &commands_;
    ViewStateStore &states_;
    engine::ProjectView &project_;
    std::function<bool(std::int64_t)> alive_;
    bool native_;
    std::map<std::string, std::unique_ptr<Entry>> entries_;
};
// The Mix View in a window of its own, onto any display, or docked back in the centre (ADR-0209
// d11, ADR-0180 d3). The root's own panel moves into the floating window and returns: one panel,
// never two. Where it opens is this window's view state; the window's place is kept as every
// floating window's. False only when the host or the view state refuses.
bool detachMixView(FloatingHost &, AdiRootComponent &, bool detach);
// F4 while the Mix View opens in a window: opens or closes that window.
bool toggleMixViewWindow(FloatingHost &, AdiRootComponent &);
// After the host collects the windows closed by their own button: a Mix View window closed so
// closes the Mix View. With `restore`, at startup: one kept open in a window opens there again.
void syncMixView(FloatingHost &, AdiRootComponent &, bool restore = false);
} // namespace adi::ui
