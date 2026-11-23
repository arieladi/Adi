// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WIN-06 (docs/PLAN.md; ADR-0104 d3, d4; ADR-0203 d6): ADI's command palette, Ctrl/Cmd+P. A field over
// the window and the best matches under it, ranked by fzy (adi/ui/fuzzy_match.hpp): every command with
// its key, one that cannot run now listed dimmed with why; "V/" lists ADI's devices to insert on the
// selected track instead. Enter runs the chosen row, Esc and a click elsewhere dismiss it. Live has no
// palette: this is ADI's addition, in today's look until the menus and dialogs sheet is agreed.
#pragma once
#include "adi/ui/fuzzy_match.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <string>
#include <vector>
namespace adi::ui {
class AppCommands;
class AdiRootComponent;
class CommandPalette final : public juce::Component, private juce::TextEditor::Listener {
  public:
    struct Item {
        std::string label, hint, reason; // the row's words, its key, and why it cannot run now
        int command = 0;                 // a command's id, or 0
        std::string device;              // a device's uid ("V/"), or empty
        bool enabled = true;
        fuzzy::Score score = 0;
        std::vector<std::size_t> marked; // the label's characters the query matched
    };
    CommandPalette(AppCommands &, AdiRootComponent &);
    ~CommandPalette() override;
    // Shown again with an empty query, the field taking the keys.
    void open();
    // The rows for a query, best first (an empty query lists every command in the menus' order).
    void setQuery(const juce::String &);
    const std::vector<Item> &items() const noexcept { return items_; }
    int selected() const noexcept { return selected_; }
    void select(int row);
    // Runs a row: a command through the command layer, a device onto the selected track as one undo
    // step. The palette goes either way; a row that cannot run says why in the Status Bar.
    bool run(int row);
    void dismiss();
    bool keyPressed(const juce::KeyPress &) override;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent &) override;
    // Called once it has gone (the root keeps it, hidden, for the next Ctrl/Cmd+P).
    std::function<void()> onDismiss;
    // The field hands Up, Down, Enter and Esc to the palette before it would use them itself.
    struct Field final : juce::TextEditor {
        std::function<bool(const juce::KeyPress &)> intercept;
        bool keyPressed(const juce::KeyPress &k) override { return (intercept && intercept(k)) || juce::TextEditor::keyPressed(k); }
    };
    Field field;
    static constexpr int kWidth = 560, kField = 30, kRow = 24, kRows = 12;

  private:
    void textEditorTextChanged(juce::TextEditor &) override;
    void textEditorReturnKeyPressed(juce::TextEditor &) override;
    void textEditorEscapeKeyPressed(juce::TextEditor &) override;
    void textEditorFocusLost(juce::TextEditor &) override;
    void fit();
    AppCommands &commands_;
    AdiRootComponent &root_;
    std::vector<Item> items_;
    int selected_ = 0;
    bool gone_ = false;
};
} // namespace adi::ui
