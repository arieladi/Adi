// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Export page (EX-11; Live 12 manual 5.2.1, File > Export Audio/Video, Ctrl/Cmd+Shift+R): Live's
// choices in Live's order, each row a label and its control. What the renderer cannot do yet stays on the
// page and says why under it; Export is offered once nothing stands in the way. The look is the shared
// controls' until the page's sheet is agreed (ADR-0243 d1); this page is function.
#pragma once
#include "adi/ui/export_choices.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
class ExportPage final : public juce::Component {
  public:
    // `tracks`: the Rendered Track choices after Main, each a track id and its name.
    // `meters`: the project's time signatures, for Render Start and Length.
    ExportPage(ExportChoices initial, std::vector<std::pair<std::int64_t, std::string>> tracks,
               std::vector<textproj::Meter> meters);
    ~ExportPage() override;
    // The choices as the page holds them now.
    const ExportChoices &choices() const noexcept { return choices_; }
    // Export: the page's choices, once they can render; Cancel. Set by whoever shows the page.
    std::function<void(const ExportChoices &)> onExport;
    std::function<void()> onCancel;
    // While a render runs: its progress (0 to 1) and Cancel stops it; done, the page says how it ended.
    void showProgress(double fraction);
    void showDone(const std::string &message);
    void resized() override;
    void paint(juce::Graphics &) override;
    // The reasons Export is not offered now, as the page shows them; empty when it is.
    const std::vector<std::string> &refusals() const noexcept { return refusals_; }
    juce::TextButton exportButton{"Export"}, cancelButton{"Cancel"};
    juce::ComboBox renderedTrack, sampleRate, fileType, bitDepth, dither;
    juce::TextEditor start, length; // in bars.beats.sixteenths, as the Arrangement shows positions
    juce::ToggleButton includeReturnAndMain, renderAsLoop, convertToMono, normalize, createAnalysisFile, encodePcm,
        encodeMp3, video;
    static constexpr int kRow = 24, kLabel = 190, kWidth = 520;

  private:
    void read();   // the controls into `choices_`
    void judge();  // `refusals_`, and Export enabled when there are none
    ExportChoices choices_;
    std::vector<std::pair<std::int64_t, std::string>> tracks_;
    std::vector<textproj::Meter> meters_;
    bool startOk_ = true, lengthOk_ = true;
    std::vector<std::string> refusals_;
    std::string status_;
    bool rendering_ = false;
    std::vector<std::pair<juce::String, juce::Component *>> rows_;
};
} // namespace adi::ui
