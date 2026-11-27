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
ExportPage::ExportPage(ExportChoices initial, std::vector<ExportTrack> tracks,
                       std::vector<textproj::Meter> meters)
    : choices_(std::move(initial)), tracks_(std::move(tracks)), meters_(std::move(meters)) {
    renderedTrack.addItem("Main", 1);
    renderedTrack.addItem("All Individual Tracks", 2);
    renderedTrack.addItem("Selected Tracks Only", 3);
    renderedTrack.setItemEnabled(3, std::any_of(tracks_.begin(), tracks_.end(), [](const ExportTrack &track) {
        return track.selected && track.hasAudio;
    }));
    int currentTrack = choices_.tracks == ExportTracks::All ? 2 : 1;
    if (choices_.tracks == ExportTracks::Selected && renderedTrack.isItemEnabled(3))
        currentTrack = 3;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const auto &track = tracks_[i];
        if (track.id == 0)
            continue;
        const int item = static_cast<int>(i) + 4;
        renderedTrack.addItem(juce::String::fromUTF8(track.name.c_str()), item);
        if (choices_.tracks == ExportTracks::One && choices_.track == track.id)
            currentTrack = item;
    }
    renderedTrack.setSelectedId(currentTrack, juce::dontSendNotification);
    std::vector<juce::String> rates;
    for (const int r : kExportSampleRates)
        rates.push_back(juce::String(r));
    // The project's own rate is always offered, where the list does not hold it (192 kHz, say).
    if (choices_.sampleRate > 0 &&
        std::find(kExportSampleRates.begin(), kExportSampleRates.end(), choices_.sampleRate) == kExportSampleRates.end())
        rates.push_back(juce::String(choices_.sampleRate));
    fill(sampleRate, rates, juce::String(choices_.sampleRate));
    fill(fileType, {kExportFileTypes.begin(), kExportFileTypes.end()}, juce::String(choices_.fileType));
    fill(bitDepth, {"16", "24", "32"}, juce::String(choices_.bitDepth));
    fill(dither, {kExportDithers.begin(), kExportDithers.end()}, juce::String(choices_.dither));
    const auto pos = barPosition(choices_.startTicks, meters_);
    start.setText(juce::String(pos.bar) + "." + juce::String(pos.beat) + "." + juce::String(pos.sixteenth), false);
    length.setText(exportLengthText(choices_.startTicks, choices_.lengthTicks, meters_), false);
    // The fields show sixteenths; the range they were filled from is kept exact while they are unchanged,
    // so the song's last part is not cut to a sixteenth.
    shownStart_ = start.getText();
    shownLength_ = length.getText();
    exactStart_ = choices_.startTicks;
    exactLength_ = choices_.lengthTicks;
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
    setSize(kWidth, kRow * static_cast<int>(rows_.size()) + 24 + 4 * 36 + 38 + 40);
}
ExportPage::~ExportPage() = default;
void ExportPage::read() {
    const int source = renderedTrack.getSelectedId();
    const int track = source - 4;
    choices_.tracks = source == 2 ? ExportTracks::All : source == 3 ? ExportTracks::Selected : ExportTracks::One;
    choices_.track = track < 0 || track >= static_cast<int>(tracks_.size()) ? 0 : tracks_[static_cast<std::size_t>(track)].id;
    choices_.stemTracks.clear();
    if (choices_.tracks != ExportTracks::One)
        for (const auto &stem : tracks_)
            if (stem.hasAudio && (choices_.tracks == ExportTracks::All || stem.selected))
                choices_.stemTracks.push_back(stem.id);
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
    if (start.getText() == shownStart_) {
        choices_.startTicks = exactStart_;
        startOk_ = true;
        if (length.getText() == shownLength_) {
            choices_.lengthTicks = exactLength_;
            lengthOk_ = true;
        }
    }
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
    includeReturnAndMain.setEnabled(choices_.tracks != ExportTracks::One || choices_.track != 0);
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
        g.drawFittedText(juce::String::fromUTF8(refusals_[i].c_str()), 16, y + 4, getWidth() - 32, 36,
                         juce::Justification::centredLeft, 2);
        y += 36;
    }
    if (!status_.empty())
        g.drawFittedText(juce::String::fromUTF8(status_.c_str()), 16, getHeight() - 34 - 38, getWidth() - 32, 36,
                         juce::Justification::centredLeft, 2);
}
} // namespace adi::ui
