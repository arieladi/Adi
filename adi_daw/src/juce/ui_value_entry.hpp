// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ui_look.hpp"
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>

namespace adi::ui {
inline std::optional<double> valueNumber(const juce::String &text) {
    const auto input = text.trim().toStdString();
    const auto *first = input.data(), *last = first + input.size();
    if (first != last && *first == '+')
        ++first;
    double value = 0;
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value))
        return std::nullopt;
    return value;
}

inline juce::juce_wchar valueEntryStart(const juce::KeyPress &key) {
    const auto ch = key.getTextCharacter() != 0 ? key.getTextCharacter()
                                                : static_cast<juce::juce_wchar>(key.getKeyCode());
    return (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ? ch : 0;
}

// Created when a value is edited, never during presentation of a control.
class ValueEntry final : public juce::TextEditor {
  public:
    ValueEntry(const juce::String &title, std::function<void(const juce::String &)> commit)
        : commit_(std::move(commit)) {
        setTitle(title);
        setMultiLine(false);
        setScrollbarsShown(false);
        setIndents(2, 0);
        setSelectAllWhenFocused(false);
    }

    void open(juce::Component &owner, juce::Rectangle<int> bounds, const juce::String &current, juce::juce_wchar first) {
        original_ = current;
        open_ = true;
        owner.addAndMakeVisible(*this);
        setBounds(bounds);
        setText(first == 0 ? current : juce::String::charToString(first), false);
        focusIfShowing(*this);
        if (first == 0)
            selectAll();
        else
            moveCaretToEnd();
    }

    bool keyPressed(const juce::KeyPress &key) override {
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey) {
            close(key == juce::KeyPress::returnKey, true);
            return true;
        }
        return juce::TextEditor::keyPressed(key);
    }

    void focusLost(FocusChangeType cause) override {
        juce::TextEditor::focusLost(cause);
        close(true, false);
    }

  private:
    void close(bool accept, bool returnFocus) {
        if (!open_)
            return;
        open_ = false;
        const auto text = getText();
        const bool changed = accept && text != original_;
        auto commit = commit_;
        const SafePointer<juce::Component> owner(getParentComponent());
        setVisible(false);
        if (owner && returnFocus)
            focusIfShowing(*owner);
        if (owner && changed)
            commit(text);
    }

    std::function<void(const juce::String &)> commit_;
    juce::String original_;
    bool open_ = false;
};
} // namespace adi::ui
