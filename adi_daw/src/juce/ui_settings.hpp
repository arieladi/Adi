// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/settings/registry.hpp"
#include "adi/settings/store.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace adi::ui {
class SettingsPanel;
// One setting on its page: its name on the left, its value on the right, as Live's
// Settings draw them, with the scope's mark when it is not the application's (R-03).
class SettingRow final : public juce::Component, public juce::SettableTooltipClient {
  public:
    SettingRow(SettingsPanel &, const settings::Setting &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseEnter(const juce::MouseEvent &) override;
    // Shows the value in force, from the store.
    void refresh();
    const settings::Setting &setting;
    std::unique_ptr<juce::Component> control;
    // Live 11's Preferences, measured: names 17 units into the page, controls at 191, a
    // chooser 197 wide and a switch or value box 97, on a 21-unit pitch.
    static constexpr int kHeight = 21, kLabelX = 17, kControlX = 191, kWide = 197, kNarrow = 97;

  private:
    void commit(const settings::Value &);
    SettingsPanel &panel_;
};
// A page's heading, as Live's: the words in bold, a rule to the page's edge.
class SettingsHeading final : public juce::Component {
  public:
    explicit SettingsHeading(const juce::String &);
    void paint(juce::Graphics &) override;
    static constexpr int kHeight = 25;
};
// The page chooser: Live's tabs down the left, a word to a line, the open one joined to
// its page (R-01).
class SettingsPageList final : public juce::Component {
  public:
    explicit SettingsPageList(SettingsPanel &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    // Each page's tab, top to bottom.
    std::vector<juce::Rectangle<int>> tabs() const;
    // A page's name as its tab shows it: one word to a line, "&" and commas dropped.
    static juce::StringArray lines(const std::string &page);

  private:
    SettingsPanel &panel_;
};
// Live's Settings window (ADR-0203 d11, Master Reference V.1): the page chooser on the
// left and one page at a time, values as switches, choosers and value boxes; REAPER's Find
// box above; the hovered setting's help in a line at the foot; every change applied and
// written at once, with no OK button; Esc closes. The pages and their settings are the
// registry's, in its order.
class SettingsPanel final : public juce::Component {
  public:
    // `audioDevices` makes the Audio page's device chooser, where the application has an
    // audio device to choose.
    SettingsPanel(settings::AppSettings &,
                  std::function<std::unique_ptr<juce::Component>()> audioDevices = {});
    ~SettingsPanel() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    void showPage(const std::string &);
    const std::string &page() const noexcept { return page_; }
    const std::vector<std::string> &pageNames() const noexcept { return pageNames_; }
    // REAPER's Find (R-02): every setting matching each word, from every page.
    void find(const juce::String &query);
    bool finding() const noexcept { return search.getText().isNotEmpty(); }
    void setHelp(const juce::String &);
    const juce::String &help() const noexcept { return help_; }
    // Writes one value and saves the file; the help line says why when it cannot.
    bool change(const settings::Setting &, const settings::Value &);
    const std::vector<std::unique_ptr<SettingRow>> &rows() const noexcept { return rows_; }
    // Shows the values in force again, after something else changed them (the audio
    // device's own chooser, the agent).
    void refreshValues();
    juce::Component *audioDevices() const noexcept { return audio_.get(); }
    settings::AppSettings &store;
    std::function<void()> onClose;
    juce::TextEditor search;
    SettingsPageList pageList{*this};
    juce::TextButton defaults{"Restore Defaults"};
    // Live's tab column and the page's frame; ADI's help line and buttons at its foot.
    static constexpr int kPageListWidth = 94, kHelpHeight = 58;

  private:
    void layoutRows();
    std::vector<std::string> pageNames_;
    std::string page_;
    juce::String help_;
    std::function<std::unique_ptr<juce::Component>()> makeAudio_;
    std::unique_ptr<juce::Component> audio_;
    juce::Viewport scroll_;
    juce::Component content_;
    std::vector<std::unique_ptr<SettingRow>> rows_;
    std::vector<std::unique_ptr<SettingsHeading>> headings_;
    std::vector<juce::Component *> order_; // the page, top to bottom
    juce::Rectangle<int> pageArea() const noexcept;
};
// The Settings window: floating, remembered where it was for the session, closed with
// Esc or its close button (R-01).
class SettingsWindow final : public juce::DocumentWindow {
  public:
    SettingsWindow(settings::AppSettings &, std::function<std::unique_ptr<juce::Component>()> audioDevices);
    void closeButtonPressed() override;
    SettingsPanel &panel() noexcept { return *panel_; }

  private:
    SettingsPanel *panel_ = nullptr;
};
} // namespace adi::ui
