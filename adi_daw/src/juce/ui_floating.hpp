// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/spectrum.hpp"
#include "adi/engine/scope.hpp"
#include "ui_shell.hpp"
#include <array>
#include <thread>
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
    bool detach(const std::string &key, std::int64_t device, juce::Component &,
                juce::Component &dock, std::function<void(const SnapshotReader &)> frame = {});
    bool openView(const std::string &key, std::int64_t device, std::unique_ptr<juce::Component>,
                  std::function<void(const SnapshotReader &)> frame = {});
    void frame(const std::string &key); // per-window attachment; public for deterministic tests
    void collect();                     // message-thread lifecycle, not a renderer clock
    void close(const std::string &key);
    void closeAll(bool remember);
    std::size_t count() const noexcept { return entries_.size(); }
    juce::Component *content(const std::string &) const;
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
} // namespace adi::ui
