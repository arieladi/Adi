// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/history.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
// Live's Undo History (Live 12 manual 5.4.2; MR V.4 §4; A5): every step of the project's history,
// newest first, the ones undone greyed; a click on one, or Enter after the arrows, puts the project
// back to just after it. ADI's history branches (ADR-0128): a step on another branch is shown with
// its branch's name and greyed, and restoring it switches to that branch. The Snapshots page lists
// saved versions independently of the retained steps, including compacted history points.
class UndoHistoryPanel final : public juce::Component, private juce::ListBoxModel {
  public:
    UndoHistoryPanel();
    ~UndoHistoryPanel() override;
    // How the panel reads the history and restores a step; the window gives them.
    std::function<History::Timeline()> read;
    // Transaction zero selects the original project state.
    std::function<bool(std::int64_t txnId)> restore;
    std::function<bool()> restoreOpening;
    std::function<bool(std::int64_t snapshotId)> revertSnapshot, removeSnapshot;
    std::function<bool(std::int64_t snapshotId, const std::string &)> renameSnapshot;
    // The last row, as Live lists "Open Live Set" first in its history ("Creating or opening a Set is
    // treated as the first action in the Undo History and therefore cannot be undone"). ADI still
    // lists earlier edits. This row returns to the state the window installed. The app names the file.
    std::string rootLabel = "Open Project";
    std::optional<std::int64_t> openingHead;
    std::int64_t openedUtc = 0; // microseconds
    // Reads the history again; the current step selected.
    void refresh();
    void paint(juce::Graphics &) override;
    void resized() override;
    // One row: a step, as the list shows it.
    struct Row {
        std::int64_t txnId = 0;
        std::string label, detail; // the step's label; its branch and snapshot names
        bool applied = false, current = false, otherBranch = false, root = false;
        juce::String time;
    };
    const std::vector<Row> &rows() const noexcept { return rows_; }
    // Restores the row at `index` (Enter on the selected row does it too).
    bool restoreRow(int index);
    struct SnapshotRow {
        std::int64_t id;
        std::string name;
        juce::String time;
    };
    const std::vector<SnapshotRow> &snapshots() const noexcept { return snapshots_; }
    juce::ListBox list;
    juce::TextButton stepsTab{"Steps"}, snapshotsTab{"Snapshots"};
    juce::TextButton revertButton{"Revert"}, renameButton{"Rename"}, removeButton{"Remove"}, cancelButton{"Cancel"};
    juce::TextEditor snapshotName;
    static constexpr int kRowHeight = 21;

  private:
    void showSnapshots(bool);
    void updateSnapshotActions();
    const SnapshotRow *selectedSnapshot() const;
    void revertSelectedSnapshot();
    void renameSelectedSnapshot();
    void cancelSnapshotRename();
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics &, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent &) override;
    void returnKeyPressed(int lastRowSelected) override;
    void selectedRowsChanged(int) override;
    std::vector<Row> rows_;
    std::vector<SnapshotRow> snapshots_;
    std::string error_;
    int currentStep_ = -1;
    bool showSnapshots_ = false;
    std::optional<std::int64_t> renaming_;
};
} // namespace adi::ui
