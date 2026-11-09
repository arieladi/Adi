// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_history.hpp"
#include "ui_look.hpp"
#include <map>
namespace adi::ui {
UndoHistoryPanel::UndoHistoryPanel() {
    setTitle("Undo History");
    list.setTitle("Undo History steps");
    list.setModel(this);
    list.setRowHeight(kRowHeight);
    list.setColour(juce::ListBox::backgroundColourId, look::panel);
    list.setColour(juce::ListBox::outlineColourId, look::outline);
    list.setOutlineThickness(1);
    addAndMakeVisible(list);
}
UndoHistoryPanel::~UndoHistoryPanel() { list.setModel(nullptr); }
void UndoHistoryPanel::refresh() {
    rows_.clear();
    error_.clear();
    if (!read) {
        list.updateContent();
        return;
    }
    const auto timeline = read();
    if (!timeline.error.empty()) {
        error_ = timeline.error;
        list.updateContent();
        return;
    }
    std::map<std::int64_t, std::string> branchNames;
    for (const auto &b : timeline.branches)
        branchNames[b.id] = b.name;
    int current = -1;
    for (const auto &s : timeline.steps) {
        Row row;
        row.txnId = s.step.txnId;
        row.label = s.step.label.empty() ? "Change" : s.step.label;
        row.applied = s.applied;
        row.otherBranch = !s.onCurrentLine;
        row.current = timeline.headSeq && *timeline.headSeq == s.step.lastSeq;
        if (row.otherBranch && !s.branchIds.empty())
            row.detail = "on " + (branchNames[s.branchIds.front()].empty() ? std::string("another branch")
                                                                          : branchNames[s.branchIds.front()]);
        for (const auto id : s.snapshotIds)
            for (const auto &snap : timeline.snapshots)
                if (snap.id == id)
                    row.detail += (row.detail.empty() ? "" : ", ") + std::string("◆ ") + snap.name;
        if (row.current)
            current = static_cast<int>(rows_.size());
        rows_.push_back(std::move(row));
    }
    list.updateContent();
    if (current >= 0)
        list.selectRow(current, true, true);
    else
        list.deselectAllRows();
    repaint();
}
void UndoHistoryPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    if (!error_.empty()) {
        g.setColour(look::text);
        g.setFont(look::font(11.5f));
        g.drawFittedText(juce::String::fromUTF8(error_.c_str()), getLocalBounds().reduced(8), juce::Justification::topLeft, 3);
    }
}
void UndoHistoryPanel::resized() { list.setBounds(getLocalBounds().reduced(4)); }
int UndoHistoryPanel::getNumRows() { return static_cast<int>(rows_.size()); }
void UndoHistoryPanel::paintListBoxItem(int index, juce::Graphics &g, int width, int height, bool selected) {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return;
    const auto &row = rows_[static_cast<std::size_t>(index)];
    if (selected)
        g.fillAll(look::selection);
    else if (index % 2 == 1)
        g.fillAll(look::laneAlt.brighter(0.05f));
    // An undone step, or one on another branch, greyed, as Live greys the steps after the current one.
    g.setColour(row.applied ? look::text : look::textDim.withAlpha(0.7f));
    g.setFont(look::font(11.5f, row.current));
    const int detailWidth = row.detail.empty() ? 0 : std::min(width / 2, 160);
    g.drawText(juce::String::fromUTF8(row.label.c_str()), 6, 0, width - 12 - detailWidth, height,
               juce::Justification::centredLeft, true);
    if (detailWidth > 0) {
        g.setColour(look::textDim);
        g.setFont(look::font(10.5f));
        g.drawText(juce::String::fromUTF8(row.detail.c_str()), width - detailWidth - 6, 0, detailWidth, height,
                   juce::Justification::centredRight, true);
    }
}
bool UndoHistoryPanel::restoreRow(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size()) || !restore)
        return false;
    const bool done = restore(rows_[static_cast<std::size_t>(index)].txnId);
    refresh();
    return done;
}
void UndoHistoryPanel::listBoxItemClicked(int row, const juce::MouseEvent &) { restoreRow(row); }
void UndoHistoryPanel::returnKeyPressed(int lastRowSelected) { restoreRow(lastRowSelected); }
} // namespace adi::ui
