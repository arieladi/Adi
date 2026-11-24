// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "ui_look.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace adi::ui {
// Keeps the row target and allowed actions captured by the menu builder while the user filters.
class StackAddPicker final : public juce::Component, private juce::ListBoxModel, private juce::KeyListener {
  public:
    explicit StackAddPicker(const juce::PopupMenu& source) {
        for (juce::PopupMenu::MenuItemIterator it(source); it.next();) items_.push_back(it.getItem());
        search_.setTitle("Search devices and Mix View tools");
        search_.setTextToShowWhenEmpty("Search tools, devices and plug-ins", look::textDim);
        search_.onTextChange = [this] { filter(); };
        search_.onReturnKey = [this] { choose(list_.getSelectedRow()); };
        search_.onEscapeKey = [this] { close(); };
        search_.addKeyListener(this);
        addAndMakeVisible(search_);
        list_.setTitle("Available tools and devices");
        list_.setModel(this);
        list_.setRowHeight(look::fieldHeight);
        addAndMakeVisible(list_);
        filter();
        constexpr auto visibleRowCount = 12;
        setSize(look::workspaceSegmentWidth * 3, look::fieldHeight * visibleRowCount);
    }
    ~StackAddPicker() override { search_.removeKeyListener(this); list_.setModel(nullptr); }
    void focusSearch() { search_.grabKeyboardFocus(); }
    using juce::Component::keyPressed;
    void resized() override {
        auto area = getLocalBounds().reduced(look::gap);
        search_.setBounds(area.removeFromTop(look::fieldHeight));
        area.removeFromTop(look::gap);
        list_.setBounds(area);
    }
  private:
    juce::TextEditor search_;
    juce::ListBox list_;
    std::vector<juce::PopupMenu::Item> items_;
    std::vector<int> visible_;
    int getNumRows() override { return static_cast<int>(visible_.size()); }
    const juce::PopupMenu::Item* row(int i) const {
        return juce::isPositiveAndBelow(i, static_cast<int>(visible_.size()))
                   ? &items_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(i)])]
                   : nullptr;
    }
    juce::String getNameForRow(int i) override {
        const auto* item = row(i);
        if (!item) return {};
        return item->isSectionHeader || item->isEnabled ? item->text : item->text + " (unavailable)";
    }
    void filter() {
        visible_.clear();
        auto query = search_.getText().trim();
        int section = -1;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            const auto& item = items_[static_cast<std::size_t>(i)];
            if (item.isSectionHeader) { section = i; continue; }
            if (!item.text.containsIgnoreCase(query) && query.isNotEmpty()) continue;
            if (section != -1) { visible_.push_back(section); section = -1; }
            visible_.push_back(i);
        }
        list_.updateContent();
        list_.deselectAllRows();
        move(1);
        list_.repaint();
    }
    void move(int step) {
        int i = list_.getSelectedRow();
        if (i < 0 && step < 0) i = getNumRows();
        for (i += step; i >= 0 && i < getNumRows(); i += step) {
            const auto* item = row(i);
            if (item->isSectionHeader || !item->isEnabled) continue;
            list_.selectRow(i);
            list_.scrollToEnsureRowIsOnscreen(i);
            break;
        }
    }
    void paintListBoxItem(int i, juce::Graphics& g, int w, int h, bool selected) override {
        const auto* item = row(i);
        if (!item) return;
        if (selected && item->isEnabled && !item->isSectionHeader) g.fillAll(look::field);
        g.setColour(item->isSectionHeader ? look::accent : item->isEnabled ? look::text : look::textDim);
        g.setFont(look::font(item->isSectionHeader ? look::fontCaption : look::fontBody, item->isSectionHeader));
        auto bounds = juce::Rectangle<int>{w, h}.reduced(look::gap, 0);
        auto mark = bounds.removeFromRight(look::fieldHeight);
        if (!item->isSectionHeader) bounds.removeFromLeft(look::gap);
        g.drawText(item->text, bounds, juce::Justification::centredLeft);
        if (item->isTicked) {
            auto tick = getLookAndFeel().getTickShape(look::fontBody);
            mark = mark.reduced(look::gap);
            tick.applyTransform(tick.getTransformToScaleToFit(mark.toFloat(), true));
            g.fillPath(tick);
        }
    }
    void listBoxItemClicked(int i, const juce::MouseEvent&) override { choose(i); }
    void returnKeyPressed(int i) override { choose(i); }
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if (key.getKeyCode() == juce::KeyPress::upKey) { move(-1); return true; }
        if (key.getKeyCode() == juce::KeyPress::downKey) { move(1); return true; }
        return false;
    }
    void close() { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); }
    void choose(int i) {
        const auto* item = row(i);
        if (!item || !item->isEnabled || item->isSectionHeader || !item->action) return;
        auto action = item->action;
        close();
        action();
    }
};
} // namespace adi::ui
