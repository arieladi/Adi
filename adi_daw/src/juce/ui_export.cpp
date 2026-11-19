// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_export.hpp"
#include "ui_look.hpp"
#include <algorithm>
namespace adi::ui {
namespace {
// A choice the renderer cannot take yet is shown, and says why under the page.
void fill(juce::ComboBox &box, const std::vector<juce::String> &items, const juce::String &current) {
    box.clear(juce::dontSendNotification);
    for (std::size_t i = 0; i < items.size(); ++i)
        box.addItem(items[i], static_cast<int>(i) + 1);
    const auto at = std::find(items.begin(), items.end(), current);
    box.setSelectedId(at == items.end() ? 1 : static_cast<int>(at - items.begin()) + 1, juce::dontSendNotification);
}
} // namespace
ExportPage::ExportPage(ExportChoices initial, std::vector<std::pair<std::int64_t, std::string>> tracks,
                       std::vector<textproj::Meter> meters)
    : choices_(std::move(initial)), tracks_(std::move(tracks)), meters_(std::move(meters)) {
    std::vector<juce::String> trackNames{"Main"};
    juce::String currentTrack = "Main";
    for (const auto &[id, name] : tracks_) {
        trackNames.push_back(juce::String::fromUTF8(name.c_str()));
        if (id == choices_.track)
            currentTrack = trackNames.back();
    }
    fill(renderedTrack, trackNames, currentTrack);
    std::vector<juce::String> rates;
    for (const int r : kExportSampleRates)
        rates.push_back(juce::String(r));
    fill(sampleRate, rates, juce::String(choices_.sampleRate));
    fill(fileType, {kExportFileTypes.begin(), kExportFileTypes.end()}, juce::String(choices_.fileType));
    fill(bitDepth, {"16", "24", "32"}, juce::String(choices_.bitDepth));
    fill(dither, {kExportDithers.begin(), kExportDithers.end()}, juce::String(choices_.dither));
    const auto pos = barPosition(choices_.startTicks, meters_);
    start.setText(juce::String(pos.bar) + "." + juce::String(pos.beat) + "." + juce::String(pos.sixteenth), false);
    length.setText(exportLengthText(choices_.startTicks, choices_.lengthTicks, meters_), false);
    includeReturnAndMain.setToggleState(choices_.includeReturnAndMain, juce::dontSendNotification);
    renderAsLoop.setToggleState(choices_.renderAsLoop, juce::dontSendNotification);
    convertToMono.setToggleState(choices_.convertToMono, juce::dontSendNotification);
    normalize.setToggleState(choices_.normalize, juce::dontSendNotification);
    createAnalysisFile.setToggleState(choices_.createAnalysisFile, juce::dontSendNotification);
    encodePcm.setToggleState(choices_.encodePcm, juce::dontSendNotification);
    encodeMp3.setToggleState(choices_.encodeMp3, juce::dontSendNotification);
    video.setToggleState(choices_.video, juce::dontSendNotification);
    // Live's order (manual 5.2.1).
    rows_ = {{"Rendered Track", &renderedTrack},
             {"Render Start", &start},
             {"Render Length", &length},
             {"Include Return and Main Effects", &includeReturnAndMain},
             {"Render as Loop", &renderAsLoop},
             {"Convert to Mono", &convertToMono},
             {"Normalize", &normalize},
             {"Create Analysis File", &createAnalysisFile},
             {"Sample Rate", &sampleRate},
             {"Encode PCM", &encodePcm},
             {"File Type", &fileType},
             {"Bit Depth", &bitDepth},
             {"Dither Options", &dither},
             {"Encode MP3", &encodeMp3},
             {"Export Video", &video}};
    const auto changed = [this] {
        read();
        judge();
        repaint();
    };
    for (auto *box : {&renderedTrack, &sampleRate, &fileType, &bitDepth, &dither})
        box->onChange = changed;
    for (auto *toggle : {&includeReturnAndMain, &renderAsLoop, &convertToMono, &normalize, &createAnalysisFile, &encodePcm,
                         &encodeMp3, &video})
        toggle->onClick = changed;
    for (auto *field : {&start, &length}) {
        field->onTextChange = changed;
        field->setFont(look::font(look::fontField));
    }
    for (auto &[label, control] : rows_) {
        control->setTitle(label);
        addAndMakeVisible(*control);
    }
    exportButton.onClick = [this] {
        read();
        judge();
        if (refusals_.empty() && onExport && !rendering_)
            onExport(choices_);
    };
    cancelButton.onClick = [this] {
        if (onCancel)
            onCancel();
    };
    addAndMakeVisible(exportButton);
    addAndMakeVisible(cancelButton);
    read();
    judge();
    setSize(kWidth, kRow * static_cast<int>(rows_.size()) + 24 + 4 * 18 + 40);
}
ExportPage::~ExportPage() = default;
void ExportPage::read() {
    const int track = renderedTrack.getSelectedId() - 1;
    choices_.track = track <= 0 || track > static_cast<int>(tracks_.size()) ? 0 : tracks_[static_cast<std::size_t>(track - 1)].first;
    if (const auto at = parsePosition(start.getText().toStdString()))
        if (const auto tick = tickOf(*at, meters_)) {
            choices_.startTicks = *tick;
            startOk_ = true;
        } else
            startOk_ = false;
    else
        startOk_ = false;
    if (const auto ticks = parseExportLength(length.getText().toStdString(), choices_.startTicks, meters_)) {
        choices_.lengthTicks = *ticks;
        lengthOk_ = true;
    } else
        lengthOk_ = false;
    choices_.includeReturnAndMain = includeReturnAndMain.getToggleState();
    choices_.renderAsLoop = renderAsLoop.getToggleState();
    choices_.convertToMono = convertToMono.getToggleState();
    choices_.normalize = normalize.getToggleState();
    choices_.createAnalysisFile = createAnalysisFile.getToggleState();
    choices_.sampleRate = sampleRate.getText().getIntValue();
    choices_.encodePcm = encodePcm.getToggleState();
    choices_.fileType = fileType.getText().toStdString();
    choices_.bitDepth = bitDepth.getText().getIntValue();
    choices_.dither = dither.getText().toStdString();
    choices_.encodeMp3 = encodeMp3.getToggleState();
    choices_.video = video.getToggleState();
}
void ExportPage::judge() {
    refusals_ = exportRefusals(choices_);
    if (!startOk_)
        refusals_.insert(refusals_.begin(), "Render Start is not a position in the song (bar.beat.sixteenth).");
    if (!lengthOk_)
        refusals_.insert(refusals_.begin(), "Render Length is not a length (bars.beats.sixteenths).");
    exportButton.setEnabled(refusals_.empty() && !rendering_);
}
void ExportPage::showProgress(double fraction) {
    rendering_ = true;
    exportButton.setEnabled(false);
    status_ = "Exporting: " + std::to_string(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100)) + " %";
    repaint();
}
void ExportPage::showDone(const std::string &message) {
    rendering_ = false;
    status_ = message;
    judge();
    repaint();
}
void ExportPage::resized() {
    int y = 12;
    for (auto &[label, control] : rows_) {
        control->setBounds(kLabel + 16, y + 2, getWidth() - kLabel - 32, kRow - 4);
        y += kRow;
    }
    cancelButton.setBounds(getWidth() - 16 - 90, getHeight() - 34, 90, 24);
    exportButton.setBounds(cancelButton.getX() - 8 - 90, getHeight() - 34, 90, 24);
}
void ExportPage::paint(juce::Graphics &g) {
    g.fillAll(look::panel);
    g.setColour(look::text);
    g.setFont(look::font(look::fontField));
    int y = 12;
    for (auto &[label, control] : rows_) {
        g.drawText(label, 16, y, kLabel, kRow, juce::Justification::centredLeft, true);
        y += kRow;
    }
    // Why Export is not offered, then the render's progress or how it ended.
    g.setColour(look::textDim);
    for (std::size_t i = 0; i < refusals_.size() && i < 4; ++i) {
        g.drawFittedText(juce::String::fromUTF8(refusals_[i].c_str()), 16, y + 4, getWidth() - 32, 18,
                         juce::Justification::centredLeft, 1);
        y += 18;
    }
    if (!status_.empty())
        g.drawText(juce::String::fromUTF8(status_.c_str()), 16, getHeight() - 34, getWidth() - 240, 24,
                   juce::Justification::centredLeft, true);
}
} // namespace adi::ui
