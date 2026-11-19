// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_order.hpp"
#include "ui_analysis.hpp"
#include <functional>

namespace adi::ui {
class AdiRootComponent;

class MixChannelBrowser final : public juce::Component, private juce::ListBoxModel {
  public:
    explicit MixChannelBrowser(AdiRootComponent &);
    ~MixChannelBrowser() override;
    void frame(const SnapshotReader &);
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;
    void visibilityChanged() override { infoPressed_ = false; }
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    std::int64_t trackAt(int row) const;
    [[nodiscard]] std::optional<std::vector<std::int64_t>> filterMembership() const {
        return filterApplied_ ? membership_ : std::nullopt;
    }

    juce::TextEditor search;
    juce::ComboBox show, infoMode;
    juce::ListBox list;
    juce::TextButton refreshSounding{"Refresh"};
    juce::TextButton openInfo{"Info"};
    SharedSpectrumView preview;
    std::function<void(std::int64_t)> onChoose;
    std::function<void(std::optional<std::vector<std::int64_t>>)> onFilter;
    std::function<void()> onOpenInfo;

  private:
    struct Entry {
        std::int64_t id;
        juce::String name;
        juce::Colour colour;
        std::uint32_t depth;
        bool group;
        int level = 0;
    };
    struct Group {
        std::int64_t track;
        int item;
    };
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void selectedRowsChanged(int row) override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    void syncSelection();
    void rebuildList();
    bool rebuildGroups(const SnapshotReader &, std::int64_t reference);
    void chooseFilter();
    void updateGroupMembership();
    void captureSounding();
    void setMembership(std::optional<std::vector<std::int64_t>>);
    int filterItem() const noexcept;
    bool current() const noexcept;
    float peak(std::int64_t) const;
    int level(std::int64_t) const;

    AdiRootComponent &root_;
    const void *snapshot_ = nullptr;
    std::uint64_t generation_ = 0;
    std::int64_t selected_ = 0, group_ = 0, reference_ = 0, main_ = 0;
    int nextGroupItem_ = 3;
    bool sounding_ = false;
    bool syncing_ = false, filterApplied_ = false, infoPressed_ = false;
    std::vector<TrackRow> tree_, nextTree_;
    TrackOrder order_;
    std::vector<Entry> entries_;
    std::vector<Group> groups_;
    std::vector<std::size_t> rows_;
    std::optional<std::vector<std::int64_t>> membership_;
};
} // namespace adi::ui
