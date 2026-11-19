// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_analysis.hpp"
#include <array>

namespace adi::ui {
class SnapshotReader;

class MixAnalysers final : public juce::Component {
  public:
    enum class Page { All, Spectrogram, Spectrum, Scope, Phase, Loudness, Analysis };
    MixAnalysers();
    ~MixAnalysers() override;
    void setPage(Page);
    Page page() const noexcept { return page_; }
    void setMode(SharedSpectrumView::Mode);
    void followSelection(bool);
    std::int64_t target(const SnapshotReader &, std::int64_t main, std::int64_t selected) const noexcept;
    void frame(const std::shared_ptr<SharedAnalysis> &, const SnapshotReader &, std::int64_t main, std::int64_t selected);
    void setMainMeterSource(const std::shared_ptr<const MainMeterSource> &);
    void suspend(bool);
    void resized() override;

    AnalysisPane detail;
    juce::ComboBox pageBox, sourceBox;

  private:
    class Table;
    struct Channel {
        std::int64_t id;
        std::string name;
        int menuId;
    };
    void refreshChannels(const SnapshotReader &, std::int64_t main);
    void refreshSourceControl();
    std::unique_ptr<Table> table_;
    std::array<SharedSpectrumView, 5> tiles_;
    std::vector<Channel> channels_;
    std::weak_ptr<SharedAnalysis> owner_;
    std::optional<std::uint64_t> channelRevision_;
    std::int64_t fixedTrack_ = 0;
    int nextMenuId_ = 3;
    Page page_ = Page::All;
    bool followsSelection_ = false;
    bool suspended_ = false;
    bool tableDirty_ = true;
};
} // namespace adi::ui
