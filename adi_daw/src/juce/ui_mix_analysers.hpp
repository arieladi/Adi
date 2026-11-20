// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_analysis.hpp"
#include "adi/ui/analysis_comparison.hpp"
#include <array>

namespace adi::engine { struct Snapshot; }

namespace adi::ui {
class SnapshotReader;

class MixAnalysers final : public juce::Component {
  public:
    enum class Page { All, Spectrogram, Spectrum, Scope, Phase, Loudness, Analysis, Average, Compare };
    MixAnalysers();
    ~MixAnalysers() override;
    void setPage(Page);
    Page page() const noexcept { return page_; }
    void setMode(SharedSpectrumView::Mode);
    void followSelection(bool);
    std::int64_t target(const SnapshotReader &, std::int64_t main, std::int64_t selected) const noexcept;
    void frame(const std::shared_ptr<SharedAnalysis> &, const SnapshotReader &, std::int64_t main, std::int64_t selected,
               std::uint64_t viewGeneration = 0);
    void setMainMeterSource(const std::shared_ptr<const MainMeterSource> &);
    AnalysisComparison::View comparisonView() const noexcept;
    void suspend(bool);
    void resized() override;

    AnalysisPane detail;
    juce::ComboBox pageBox, sourceBox, spectrumTraceBox;
    juce::ComboBox compareA, compareB, compareScale;
    juce::TextButton holdB{"Hold B"}, clearCompare{"Clear"};

  private:
    class Table;
    class Compare;
    struct Channel {
        std::int64_t id;
        std::string name;
        int menuId;
        bool isMain;
    };
    void refreshChannels(const SnapshotReader &, std::int64_t main, std::uint64_t viewGeneration);
    void refreshSourceControl();
    void updateComparison(const std::shared_ptr<SharedAnalysis> &);
    std::unique_ptr<Table> table_;
    std::unique_ptr<Compare> compare_;
    std::array<SharedSpectrumView, 5> tiles_;
    std::vector<Channel> channels_;
    std::weak_ptr<SharedAnalysis> owner_;
    const engine::Snapshot *channelSnapshot_ = nullptr; // borrowed identity, never dereferenced
    std::optional<std::uint64_t> channelGeneration_;
    std::int64_t channelMain_ = 0;
    std::int64_t fixedTrack_ = 0;
    int nextMenuId_ = 3;
    Page page_ = Page::All;
    bool followsSelection_ = false;
    bool suspended_ = false;
    bool tableDirty_ = true;
};
} // namespace adi::ui
