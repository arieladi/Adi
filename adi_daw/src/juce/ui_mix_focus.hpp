// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/dsp/masking.hpp"
#include "adi/ui/focus_lag.hpp"
#include "adi/ui/focus_match.hpp"
#include "ui_look.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
namespace adi::ui {
class AdiRootComponent;
class SnapshotReader;
class MixFocus final : public juce::Component, private juce::ComponentListener {
  public:
    explicit MixFocus(AdiRootComponent &);
    ~MixFocus() override;
    static std::vector<std::int64_t> soloedChannels(const SnapshotReader &);
    void setMembers(const std::vector<std::int64_t> &);
    std::int64_t mainTrack() const noexcept { return main_; }
    size_t count() const noexcept { return members_.size(); }
    std::vector<std::int64_t> channelIds() const;
    void select(std::int64_t);
    [[nodiscard]] bool contains(std::int64_t id) const noexcept;
    void toggleMemberSolo(std::int64_t id);
    void frame(const SnapshotReader &);
    void suspend(bool);
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void resized() override;
    void moved() override;
    void paint(juce::Graphics &) override;
    juce::ComboBox available;
    juce::TextButton add{"+"}, star{"Set main"}, mute{"Mute"}, solo{"Solo"}, hearOne{"Hear track"},
        hearAll{"Hear all"}, edit{"Edit chain"}, learnMatch{"Learn Match"}, clearMatch{"Clear Match"};
    juce::ToggleButton scope{"Scope"};
    std::function<void(std::int64_t)> onEdit;
    std::function<void()> onMembersChanged;

  private:
    friend struct MixFocusTestAccess;
    look::DensityId density_ = look::densityId();
    enum class ScopeDisplay { Overlay = 1, Layers, Stack, Sum, Difference };
    enum class ScopeChannels { Stereo = 1, Mid };
    struct Member {
        std::int64_t id{};
        juce::Colour colour;
        std::unique_ptr<juce::TextButton> chip;
        SharedAnalysis::Lease lease;
        std::uint32_t outputs = 0;
        std::shared_ptr<const SharedAnalysis::Result> data;
        juce::Path path;
        std::array<juce::Path, 2> scopePaths;
        std::vector<double> energy;
        std::optional<SharedAnalysis::Lease::SourcePosition> afterLoop;
        bool spectrumChanged{true};
    };
    void setMembers(const std::vector<std::int64_t> &, bool promoteSoloed);
    void hear(bool group);
    void drop();
    void dropIfActive();
    void watchAncestors();
    void unwatchAncestors() noexcept;
    void componentVisibilityChanged(juce::Component &) override;
    void componentMovedOrResized(juce::Component &, bool, bool) override;
    void componentParentHierarchyChanged(juce::Component &) override;
    void componentBeingDeleted(juce::Component &) override;
    bool plotShowing() const noexcept;
    void rebuildPlot();
    void clearBarScopeData();
    void updateBarScope(const SnapshotReader &, const std::shared_ptr<SharedAnalysis> &);
    void clearScopeSum();
    void rebuildScope();
    bool requestsScopeMid() const noexcept;
    bool showsScopeDifference() const noexcept;
    void updateScopeDifference();
    void refreshControls();
    std::array<const Member *, 2> matchMembers() const noexcept;
    bool canLearnMatch(const std::array<const Member *, 2> &) const noexcept;
    bool matchCaptureIsCurrent(const SharedAnalysis::Result &) const noexcept;
    void updateMatchControls();
    void learnMatchPreview();
    void rebuildMatchPath();
    void clearMatchPreview();
    bool canMeasureAlign(const std::array<const Member *, 2> &) const noexcept;
    void measureAlign();
    void refreshAlign(const std::array<const Member *, 2> &);
    void pollAlign();
    void clearAlign();
    void showAlign(const juce::String &);
    AdiRootComponent &root_;
    juce::Viewport scroll_;
    juce::Component strip_;
    juce::ComboBox scopeDisplay_, scopeChannels_;
    juce::TextButton alignMeasure_{"Measure Align"};
    juce::Label alignReadout_;
    juce::Rectangle<float> plot_;
    juce::Rectangle<int> titleRow_, sumLabel_;
    std::vector<const SharedAnalysis::Result *> scopeSources_;
    std::optional<FocusScopeRange> scopeRange_;
    SharedAnalysis::BarScopeLease barScope_;
    std::shared_ptr<const engine::TempoMap> barTempo_;
    std::vector<textproj::Meter> barMeters_;
    std::optional<FocusScopeBar> requestedBar_;
    std::shared_ptr<const SharedAnalysis::BarScopeResult> shownBar_;
    std::optional<FocusSumReading> sum_;
    std::array<juce::Path, 2> scopeSumPaths_, scopeDifferencePaths_;
    std::optional<SharedAnalysis::ScopeDifference> differenceRequest_;
    std::optional<std::uint64_t> differenceLocate_, differenceRenderedRevision_;
    std::uint64_t differenceSelection_ = 0;
    std::optional<FocusMatchReading> match_;
    std::optional<std::uint64_t> matchLocateRevision_;
    std::weak_ptr<SharedAnalysis> matchBroker_;
    juce::Path matchPath_;
    juce::String matchCaption_;
    std::optional<FocusLagRequest> alignCapture_;
    SharedAnalysis::FocusLagLease alignJob_;
    double alignRate_ = 0.;
    bool alignPending_ = false;
    std::vector<Member> members_;
    std::vector<std::int64_t> choices_;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors_;
    std::weak_ptr<SharedAnalysis> broker_;
    std::unique_ptr<dsp::MaskingBands> bands_;
    std::vector<float> magnitudes_, normalizedBinX_;
    std::vector<double> maskers_;
    std::vector<juce::Range<float>> masked_;
    std::int64_t main_{}, active_{};
    double rate_{};
    size_t bins_{};
    const void *snapshot_{};
    std::uint64_t generation_{}, membershipPose_{};
    enum class Alignment { Ready, WaitForStop, WaitForFresh };
    Alignment alignment_ = Alignment::Ready;
    bool suspended_{}, redraw_{true}, scopePlaying_{}, scopeHasTrace_{};
};
} // namespace adi::ui
