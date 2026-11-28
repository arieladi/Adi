// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_history.hpp"
#include "ui_look.hpp"
#include <map>
namespace adi::ui {
namespace {
juce::String historyTime(std::int64_t utcMicros) {
    return utcMicros != 0 ? juce::Time(utcMicros / 1000).formatted("%Y-%m-%d %H:%M") : juce::String{};
}
} // namespace
UndoHistoryPanel::UndoHistoryPanel() {
    setTitle("Undo History");
    list.setModel(this);
    list.setRowHeight(kRowHeight);
    list.setColour(juce::ListBox::backgroundColourId, look::panel);
    list.setColour(juce::ListBox::outlineColourId, look::outline);
    list.setOutlineThickness(1);
    addAndMakeVisible(list);
    for (auto *tab : {&stepsTab, &snapshotsTab})
        addAndMakeVisible(tab);
    for (auto *button : {&revertButton, &renameButton, &removeButton, &cancelButton})
        addChildComponent(button);
    addChildComponent(snapshotName);
    snapshotName.setTitle("Snapshot name");
    stepsTab.onClick = [this] { showSnapshots(false); };
    snapshotsTab.onClick = [this] { showSnapshots(true); };
    revertButton.onClick = [this] { revertSelectedSnapshot(); };
    renameButton.onClick = [this] { renameSelectedSnapshot(); };
    removeButton.onClick = [this] {
        if (const auto *snapshot = selectedSnapshot(); snapshot && removeSnapshot && removeSnapshot(snapshot->id))
            refresh();
    };
    cancelButton.onClick = [this] { cancelSnapshotRename(); };
    snapshotName.onTextChange = [this] {
        if (renaming_)
            renameButton.setEnabled(snapshotName.getText().trim().isNotEmpty());
    };
    snapshotName.onReturnKey = [this] { renameSelectedSnapshot(); };
    snapshotName.onEscapeKey = [this] { cancelSnapshotRename(); };
    showSnapshots(false);
}
UndoHistoryPanel::~UndoHistoryPanel() { list.setModel(nullptr); }
void UndoHistoryPanel::refresh() {
    std::optional<std::int64_t> selected;
    if (const auto *snapshot = selectedSnapshot())
        selected = snapshot->id;
    rows_.clear();
    snapshots_.clear();
    currentStep_ = -1;
    error_.clear();
    if (!read) {
        list.updateContent();
        updateSnapshotActions();
        return;
    }
    const auto timeline = read();
    if (!timeline.error.empty()) {
        error_ = timeline.error;
        list.updateContent();
        updateSnapshotActions();
        return;
    }
    std::map<std::int64_t, std::string> branchNames;
    for (const auto &b : timeline.branches)
        branchNames[b.id] = b.name;
    int currentSnapshot = -1;
    for (const auto &snapshot : timeline.snapshots) {
        if (selected == snapshot.id)
            currentSnapshot = static_cast<int>(snapshots_.size());
        snapshots_.push_back({snapshot.id, snapshot.name, historyTime(snapshot.createdUtc)});
    }
    const auto opening = openingHead ? openingHead : timeline.originSeq;
    const bool atOpening = timeline.headSeq == opening;
    for (const auto &s : timeline.steps) {
        Row row;
        row.txnId = s.step.txnId;
        row.label = s.step.label.empty() ? "Change" : s.step.label;
        row.time = historyTime(s.createdUtc);
        row.applied = s.applied;
        row.otherBranch = !s.onCurrentLine;
        row.current = !atOpening && timeline.headSeq == s.step.lastSeq;
        if (row.otherBranch && !s.branchIds.empty())
            row.detail = "on " + (branchNames[s.branchIds.front()].empty() ? std::string("another branch")
                                                                          : branchNames[s.branchIds.front()]);
        if (opening == s.step.lastSeq)
            row.detail += (row.detail.empty() ? "" : ", ") + std::string("Project opened here");
        for (const auto id : s.snapshotIds)
            for (const auto &snap : timeline.snapshots)
                if (snap.id == id)
                    row.detail += (row.detail.empty() ? "" : ", ") + std::string("◆ ") + snap.name;
        if (row.current)
            currentStep_ = static_cast<int>(rows_.size());
        rows_.push_back(std::move(row));
    }
    if (opening != timeline.originSeq) {
        Row origin;
        for (const auto &snapshot : timeline.snapshots)
            if (!snapshot.opSeq || snapshot.opSeq == timeline.originSeq)
                origin.detail += (origin.detail.empty() ? "" : ", ") + std::string("\u25c6 ") + snapshot.name;
        if (!origin.detail.empty()) {
            origin.label = "Project created";
            origin.applied = true;
            origin.current = timeline.headSeq == timeline.originSeq;
            if (origin.current)
                currentStep_ = static_cast<int>(rows_.size());
            rows_.push_back(std::move(origin));
        }
    }
    // The opening event has its own row even when earlier edits remain visible.
    {
        Row root;
        root.label = rootLabel;
        root.time = historyTime(openedUtc);
        root.applied = true;
        root.root = true;
        root.current = atOpening;
        for (const auto &snap : timeline.snapshots) {
            const auto snapshotHead = snap.opSeq ? snap.opSeq : timeline.originSeq;
            if (snapshotHead == opening)
                root.detail += (root.detail.empty() ? "" : ", ") + std::string("\u25c6 ") + snap.name;
        }
        if (root.current)
            currentStep_ = static_cast<int>(rows_.size());
        rows_.push_back(std::move(root));
    }
    list.updateContent();
    const int current = showSnapshots_ ? currentSnapshot : currentStep_;
    if (current >= 0)
        list.selectRow(current, true, true);
    else
        list.deselectAllRows();
    updateSnapshotActions();
    repaint();
}
void UndoHistoryPanel::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    if (!error_.empty()) {
        g.setColour(look::text);
        g.setFont(look::font(look::fontBody));
        g.drawFittedText(juce::String::fromUTF8(error_.c_str()), getLocalBounds().reduced(8), juce::Justification::topLeft, 3);
    }
}
void UndoHistoryPanel::resized() {
    auto area = getLocalBounds().reduced(4);
    auto tabs = area.removeFromTop(25);
    stepsTab.setBounds(tabs.removeFromLeft(76).reduced(2, 0));
    snapshotsTab.setBounds(tabs.removeFromLeft(96).reduced(2, 0));
    area.removeFromTop(4);
    if (showSnapshots_) {
        auto actions = area.removeFromBottom(25);
        if (renaming_) {
            cancelButton.setBounds(actions.removeFromRight(62).reduced(2, 0));
            renameButton.setBounds(actions.removeFromRight(84).reduced(2, 0));
            snapshotName.setBounds(actions.reduced(2, 0));
        } else {
            revertButton.setBounds(actions.removeFromLeft(65).reduced(2, 0));
            renameButton.setBounds(actions.removeFromLeft(76).reduced(2, 0));
            removeButton.setBounds(actions.removeFromLeft(76).reduced(2, 0));
        }
        area.removeFromBottom(4);
    }
    list.setBounds(area);
}
void UndoHistoryPanel::showSnapshots(bool shouldShow) {
    renaming_.reset();
    list.deselectAllRows();
    showSnapshots_ = shouldShow;
    stepsTab.setToggleState(!showSnapshots_, juce::dontSendNotification);
    snapshotsTab.setToggleState(showSnapshots_, juce::dontSendNotification);
    list.setTitle(showSnapshots_ ? "Project snapshots" : "Undo History steps");
    list.updateContent();
    if (!showSnapshots_ && currentStep_ >= 0)
        list.selectRow(currentStep_, true, true);
    updateSnapshotActions();
    list.repaint();
}
const UndoHistoryPanel::SnapshotRow *UndoHistoryPanel::selectedSnapshot() const {
    const int index = list.getSelectedRow();
    return showSnapshots_ && index >= 0 && index < static_cast<int>(snapshots_.size()) ?
               &snapshots_[static_cast<std::size_t>(index)] : nullptr;
}
void UndoHistoryPanel::updateSnapshotActions() {
    const auto *snapshot = selectedSnapshot();
    if (!snapshot || renaming_ != snapshot->id)
        renaming_.reset();
    const bool editing = renaming_.has_value();
    revertButton.setVisible(showSnapshots_ && !editing);
    removeButton.setVisible(showSnapshots_ && !editing);
    renameButton.setVisible(showSnapshots_);
    cancelButton.setVisible(editing);
    snapshotName.setVisible(editing);
    renameButton.setButtonText(editing ? "Save Name" : "Rename");
    revertButton.setEnabled(snapshot && static_cast<bool>(revertSnapshot));
    renameButton.setEnabled(snapshot && static_cast<bool>(renameSnapshot) &&
                            (!editing || snapshotName.getText().trim().isNotEmpty()));
    removeButton.setEnabled(snapshot && static_cast<bool>(removeSnapshot));
    resized();
}
void UndoHistoryPanel::revertSelectedSnapshot() {
    if (const auto *snapshot = selectedSnapshot(); snapshot && revertSnapshot && revertSnapshot(snapshot->id))
        refresh();
}
void UndoHistoryPanel::renameSelectedSnapshot() {
    const auto *snapshot = selectedSnapshot();
    if (!snapshot || !renameSnapshot)
        return;
    if (!renaming_) {
        renaming_ = snapshot->id;
        snapshotName.setText(juce::String::fromUTF8(snapshot->name.c_str()), false);
        updateSnapshotActions();
        if (isShowing())
            snapshotName.grabKeyboardFocus();
        snapshotName.selectAll();
        return;
    }
    const auto name = snapshotName.getText().trim();
    if (name.isNotEmpty() && renameSnapshot(*renaming_, name.toStdString())) {
        renaming_.reset();
        refresh();
    }
}
void UndoHistoryPanel::cancelSnapshotRename() {
    renaming_.reset();
    updateSnapshotActions();
}
int UndoHistoryPanel::getNumRows() { return static_cast<int>(showSnapshots_ ? snapshots_.size() : rows_.size()); }
void UndoHistoryPanel::paintListBoxItem(int index, juce::Graphics &g, int width, int height, bool selected) {
    if (index < 0 || index >= getNumRows())
        return;
    // Live's rows (manual 5.4.2, its figure): a rule under each, the selected one lit.
    if (selected)
        g.fillAll(look::selection);
    g.setColour(look::separator.withAlpha(0.6f));
    g.fillRect(0, height - 1, width, 1);
    if (showSnapshots_) {
        const auto &snapshot = snapshots_[static_cast<std::size_t>(index)];
        const int timeWidth = std::min(width / 2, 120);
        g.setColour(look::text);
        g.setFont(look::font(look::fontBody));
        g.drawText(juce::String::fromUTF8(snapshot.name.c_str()), 6, 0, std::max(0, width - timeWidth - 12),
                   height, juce::Justification::centredLeft, true);
        g.setColour(look::textDim);
        g.setFont(look::font(look::fontCaption));
        if (timeWidth > 6)
            g.drawText(snapshot.time, width - timeWidth, 0, timeWidth - 6, height, juce::Justification::centredRight, true);
        return;
    }
    const auto &row = rows_[static_cast<std::size_t>(index)];
    // An undone step, or one on another branch, greyed, as Live greys the steps after the current one.
    g.setColour(row.applied ? look::text : look::textDim.withAlpha(0.7f));
    g.setFont(look::font(look::fontBody, row.current));
    const int timeWidth = row.time.isEmpty() ? 0 : std::min(width / 3, 120);
    const int detailWidth = row.detail.empty() ? 0 : std::min((width - timeWidth) / 2, 160);
    g.drawText(juce::String::fromUTF8(row.label.c_str()), 6, 0, std::max(0, width - 12 - timeWidth - detailWidth), height,
               juce::Justification::centredLeft, true);
    g.setColour(look::textDim);
    g.setFont(look::font(look::fontCaption));
    if (detailWidth > 0) {
        g.drawText(juce::String::fromUTF8(row.detail.c_str()), width - timeWidth - detailWidth - 6, 0, detailWidth, height,
                   juce::Justification::centredRight, true);
    }
    if (timeWidth > 6)
        g.drawText(row.time, width - timeWidth, 0, timeWidth - 6, height, juce::Justification::centredRight, true);
}
bool UndoHistoryPanel::restoreRow(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size()))
        return false;
    const auto &row = rows_[static_cast<std::size_t>(index)];
    const bool done = row.root && restoreOpening ? restoreOpening() : restore && restore(row.txnId);
    refresh();
    return done;
}
void UndoHistoryPanel::listBoxItemClicked(int row, const juce::MouseEvent &) {
    if (!showSnapshots_)
        restoreRow(row);
}
void UndoHistoryPanel::returnKeyPressed(int lastRowSelected) {
    if (showSnapshots_ && !renaming_)
        revertSelectedSnapshot();
    else if (!showSnapshots_)
        restoreRow(lastRowSelected);
}
void UndoHistoryPanel::selectedRowsChanged(int) { updateSnapshotActions(); }
} // namespace adi::ui
