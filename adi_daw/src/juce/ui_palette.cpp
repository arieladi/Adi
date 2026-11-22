// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_palette.hpp"
#include "adi/builtin_catalog.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
namespace adi::ui {
CommandPalette::CommandPalette(AppCommands &commands, AdiRootComponent &root) : commands_(commands), root_(root) {
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
    addAndMakeVisible(field);
    field.setTextToShowWhenEmpty("Run a command, or V/ to insert a device", look::textFaint);
    field.setFont(look::font(look::fontBody));
    field.addListener(this);
    field.setEscapeAndReturnKeysConsumed(true);
    field.intercept = [this](const juce::KeyPress &k) { return keyPressed(k); };
    setQuery({});
}
void CommandPalette::open() {
    gone_ = false;
    field.clear();
    setQuery({});
    setVisible(true);
    toFront(false);
    if (isShowing())
        field.grabKeyboardFocus();
}
CommandPalette::~CommandPalette() { field.removeListener(this); }
void CommandPalette::setQuery(const juce::String &query) {
    items_.clear();
    const auto text = query.toStdString();
    const bool devices = text.size() >= 2 && (text[0] == 'V' || text[0] == 'v') && text[1] == '/';
    const auto needle = fuzzy::utf32(devices ? text.substr(2) : text);
    const auto add = [&](Item item) {
        const auto label = fuzzy::utf32(item.label);
        if (!needle.empty()) {
            if (!fuzzy::hasMatch(needle, label))
                return;
            item.score = fuzzy::match(needle, label, &item.marked);
        }
        items_.push_back(std::move(item));
    };
    if (devices) {
        // ADI's devices, by name; those this build cannot load are not offered.
        for (const auto &d : builtinCatalog())
            if (d.available) {
                Item item;
                item.label = std::string(d.name);
                item.hint = std::string(d.category);
                item.device = std::string(d.uid);
                add(std::move(item));
            }
    } else {
        const auto *mappings = commands_.manager.getKeyMappings();
        for (int id = AppCommands::PlayStop; id <= AppCommands::LastId; ++id) {
            if (id == AppCommands::CommandPalette)
                continue;
            juce::ApplicationCommandInfo info(id);
            commands_.getCommandInfo(id, info);
            Item item;
            item.label = info.shortName.toStdString();
            item.command = id;
            item.enabled = (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0;
            if (!item.enabled)
                item.reason = "Not available now";
            if (const auto keys = mappings->getKeyPressesAssignedToCommand(id); !keys.isEmpty())
                item.hint = keys[0].getTextDescriptionWithIcons().toStdString();
            add(std::move(item));
        }
    }
    if (!needle.empty())
        std::stable_sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    selected_ = 0;
    fit();
    repaint();
}
void CommandPalette::select(int row) {
    if (items_.empty())
        return;
    selected_ = std::clamp(row, 0, static_cast<int>(items_.size()) - 1);
    repaint();
}
bool CommandPalette::run(int row) {
    if (row < 0 || row >= static_cast<int>(items_.size()))
        return false;
    const auto item = items_[static_cast<std::size_t>(row)];
    dismiss();
    if (!item.enabled) {
        root_.say(item.label + ": " + item.reason + ".");
        return false;
    }
    if (!item.device.empty())
        return root_.insertDevice(item.device, root_.arrangement.geometry.selection.track, std::nullopt);
    return root_.command(item.command);
}
void CommandPalette::dismiss() {
    if (gone_)
        return;
    gone_ = true;
    setVisible(false);
    if (onDismiss)
        onDismiss();
}
bool CommandPalette::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::upKey) {
        select(selected_ - 1);
        return true;
    }
    if (k == juce::KeyPress::downKey) {
        select(selected_ + 1);
        return true;
    }
    if (k == juce::KeyPress::returnKey) {
        run(selected_);
        return true;
    }
    if (k == juce::KeyPress::escapeKey) {
        dismiss();
        return true;
    }
    return false;
}
void CommandPalette::fit() {
    const int rows = std::min(kRows, static_cast<int>(items_.size()));
    setSize(kWidth, kField + 8 + rows * kRow + 4);
}
void CommandPalette::resized() { field.setBounds(4, 4, getWidth() - 8, kField); }
void CommandPalette::paint(juce::Graphics &g) {
    g.setColour(look::panel);
    g.fillRect(getLocalBounds());
    g.setColour(look::outline);
    g.drawRect(getLocalBounds());
    const auto font = look::font(look::fontBody);
    const int first = std::clamp(selected_ - kRows + 1, 0, std::max(0, static_cast<int>(items_.size()) - kRows));
    for (int i = 0; i < kRows && first + i < static_cast<int>(items_.size()); ++i) {
        const auto &item = items_[static_cast<std::size_t>(first + i)];
        const juce::Rectangle<int> row(4, kField + 8 + i * kRow, getWidth() - 8, kRow);
        if (first + i == selected_) {
            g.setColour(look::field);
            g.fillRect(row);
        }
        // The label, its matched letters in the accent.
        juce::AttributedString label;
        const auto text = fuzzy::utf32(item.label);
        const auto base = item.enabled ? look::text : look::textFaint;
        for (std::size_t c = 0; c < text.size(); ++c) {
            const bool marked = std::find(item.marked.begin(), item.marked.end(), c) != item.marked.end();
            label.append(juce::String::charToString(static_cast<juce::juce_wchar>(text[c])), font,
                         marked && item.enabled ? look::accent : base);
        }
        label.setJustification(juce::Justification::centredLeft);
        label.draw(g, row.reduced(8, 0).toFloat());
        g.setColour(look::textDim);
        g.setFont(font);
        g.drawText(juce::String::fromUTF8((item.enabled ? item.hint : item.reason).c_str()), row.reduced(8, 0),
                   juce::Justification::centredRight, true);
    }
}
void CommandPalette::mouseUp(const juce::MouseEvent &e) {
    const int row = (e.y - kField - 8) / kRow;
    const int first = std::clamp(selected_ - kRows + 1, 0, std::max(0, static_cast<int>(items_.size()) - kRows));
    if (e.y > kField + 8 && row >= 0 && row < kRows)
        run(first + row);
}
void CommandPalette::textEditorTextChanged(juce::TextEditor &) { setQuery(field.getText()); }
void CommandPalette::textEditorReturnKeyPressed(juce::TextEditor &) { run(selected_); }
void CommandPalette::textEditorEscapeKeyPressed(juce::TextEditor &) { dismiss(); }
void CommandPalette::textEditorFocusLost(juce::TextEditor &) { dismiss(); }
} // namespace adi::ui
