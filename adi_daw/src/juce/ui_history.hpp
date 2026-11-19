// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/history.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace adi::ui {
// Live's Undo History (Live 12 manual 5.4.2; MR V.4 §4; A5): every step of the project's history,
// newest first, the ones undone greyed; a click on one, or Enter after the arrows, puts the project
// back to just after it. ADI's history branches (ADR-0128): a step on another branch is shown with
// its branch's name and greyed, and restoring it switches to that branch. Take Snapshot's names
// stand beside the step they name. The steps come from `History::readTimeline` through the window's
// submitter; restoring is the submitter's, which refreshes the view once.
class UndoHistoryPanel final : public juce::Component, private juce::ListBoxModel {
  public:
    UndoHistoryPanel();
    ~UndoHistoryPanel() override;
    // How the panel reads the history and restores a step; the window gives them.
    std::function<History::Timeline()> read;
    std::function<bool(std::int64_t txnId)> restore;
    // The last row, as Live lists "Open Live Set" first in its history ("Creating or opening a Set is
    // treated as the first action in the Undo History and therefore cannot be undone"): the project as
    // created or opened; restoring it undoes every step on the current line. The app names the file.
    std::string rootLabel = "Open Project";
    // Reads the history again; the current step selected.
    void refresh();
    void paint(juce::Graphics &) override;
    void resized() override;
    // One row: a step, as the list shows it.
    struct Row {
        std::int64_t txnId = 0;
        std::string label, detail; // the step's label; its branch and snapshot names
        bool applied = false, current = false, otherBranch = false, root = false;
    };
    const std::vector<Row> &rows() const noexcept { return rows_; }
    // Restores the row at `index` (Enter on the selected row does it too).
    bool restoreRow(int index);
    juce::ListBox list;
    static constexpr int kRowHeight = 21;

  private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent &) override;
    void returnKeyPressed(int lastRowSelected) override;
    std::vector<Row> rows_;
    std::string error_;
};
} // namespace adi::ui
