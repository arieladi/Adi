// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_analysers.hpp"
#include "ui_look.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include <algorithm>
#include <map>

namespace adi::ui {
namespace {
using Mode = SharedSpectrumView::Mode;
constexpr std::array modes{Mode::Spectrogram, Mode::Spectrum, Mode::Scope, Mode::Phase, Mode::Meters};
constexpr int mainId = 1, followId = 2;

int channelNameWidth(int width) { return std::min(look::mixChannelMaxWidth, width / 4); }

class MeasurementRow final : public juce::Component {
  public:
    MeasurementRow() {
        view.setMode(Mode::ChannelReadout);
        view.setTooltip("Latest window. Crest is peak over RMS. Width is the percentage of energy in the side signal. "
                        "Clips counts samples at or above full scale. LUFS is the Main programme meter.");
        addAndMakeVisible(view);
        addAndMakeVisible(name);
    }
    void resized() override {
        auto area = getLocalBounds();
        name.setBounds(area.removeFromLeft(channelNameWidth(area.getWidth())).reduced(look::border, 0));
        view.setBounds(area);
    }
    juce::Label name;
    SharedSpectrumView view;
};
}

class MixAnalysers::Table final : public juce::Component, private juce::ListBoxModel {
  public:
    explicit Table(MixAnalysers &owner) : owner_(owner), rows_("Channel analysis", this) {
        setTitle("Channel analysis");
        addAndMakeVisible(rows_);
    }
    ~Table() override { rows_.setModel(nullptr); }
    void refresh() { rows_.updateContent(); rows_.repaint(); repaint(); }
    void resized() override {
        rows_.setRowHeight(look::mixChannelRowHeight);
        rows_.setBounds(getLocalBounds().withTrimmedTop(look::fieldHeight));
    }
    void paint(juce::Graphics &g) override {
        g.fillAll(look::panel);
        g.setColour(look::columnHead);
        g.fillRect(getLocalBounds().withHeight(look::fieldHeight));
        g.setColour(look::text);
        g.setFont(look::font(look::fontCaption));
        auto header = juce::Rectangle<int>(0, 0, rows_.getVisibleRowWidth(), look::fieldHeight);
        g.drawText("Channel", header.removeFromLeft(channelNameWidth(header.getWidth())).reduced(look::border, 0),
                   juce::Justification::centredLeft, true);
        constexpr std::array titles{"Peak dBFS", "Crest dB", "Side %", "Corr.", "Clips", "M LUFS"};
        for (std::size_t i = 0; i < titles.size(); ++i) {
            const int left = static_cast<int>(i) * header.getWidth() / static_cast<int>(titles.size());
            const int right = static_cast<int>(i + 1) * header.getWidth() / static_cast<int>(titles.size());
            g.drawText(titles[i], header.withX(header.getX() + left).withWidth(right - left).reduced(look::border, 0),
                       juce::Justification::centredLeft, true);
        }
    }
    void suspend(bool value) {
        visitRows([=](MeasurementRow &row, int) { row.view.suspend(value); });
    }
    void frame(const std::shared_ptr<SharedAnalysis> &broker) {
        const auto main = mainMeter.lock();
        visitRows([&](MeasurementRow &row, int i) {
            row.view.setMainMeterSource(main);
            row.view.present(broker, owner_.channels_[static_cast<std::size_t>(i)].id);
            row.view.frame();
        });
    }
    std::weak_ptr<const MainMeterSource> mainMeter;
  private:
    template <class Visitor>
    void visitRows(Visitor visitor) {
        const int first = rows_.getViewport()->getViewPositionY() / rows_.getRowHeight();
        const int end = std::min(getNumRows(), first + rows_.getNumRowsOnScreen() + 2);
        for (int i = first; i < end; ++i)
            if (auto *row = static_cast<MeasurementRow *>(rows_.getComponentForRowNumber(i)))
                visitor(*row, i);
    }
    int getNumRows() override { return static_cast<int>(owner_.channels_.size()); }
    void paintListBoxItem(int, juce::Graphics &g, int, int, bool) override { g.fillAll(look::panel); }
    juce::Component *refreshComponentForRow(int i, bool, juce::Component *existing) override {
        std::unique_ptr<MeasurementRow> row(static_cast<MeasurementRow *>(existing));
        if (i < 0 || i >= getNumRows()) return nullptr;
        if (!row) row = std::make_unique<MeasurementRow>();
        const auto &channel = owner_.channels_[static_cast<std::size_t>(i)];
        if (channel.id != row->view.track()) row->view.present({}, 0);
        row->name.setText(juce::String::fromUTF8(channel.name.c_str()), juce::dontSendNotification);
        row->name.setFont(look::font(look::fontCaption));
        row->view.suspend(owner_.suspended_);
        return row.release();
    }
    MixAnalysers &owner_;
    juce::ListBox rows_;
};

MixAnalysers::MixAnalysers() {
    table_ = std::make_unique<Table>(*this);
    setTitle("Analysers");
    pageBox.addItemList({"All", "Spectrogram", "Spectrum", "Scope", "Phase", "Loudness", "Analysis"}, 1);
    pageBox.setTitle("Analyser page");
    sourceBox.setTitle("Analyser source");
    pageBox.onChange = [this] {
        if (const int id = pageBox.getSelectedId(); id > 0) setPage(static_cast<Page>(id - 1));
    };
    sourceBox.onChange = [this] {
        const auto id = sourceBox.getSelectedId();
        if (id == mainId || id == followId) {
            followSelection(id == followId);
        } else if (const auto found = std::ranges::find(channels_, id, &Channel::menuId); found != channels_.end()) {
            followsSelection_ = false;
            fixedTrack_ = found->id;
        }
    };
    addAndMakeVisible(pageBox);
    addAndMakeVisible(sourceBox);
    addChildComponent(detail);
    detail.usePageControls();
    addChildComponent(*table_);
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        tiles_[i].setMode(modes[i]);
        addChildComponent(tiles_[i]);
    }
    refreshSourceControl();
    setPage(Page::All);
}
MixAnalysers::~MixAnalysers() { table_.reset(); }

void MixAnalysers::setPage(Page page) {
    const auto index = static_cast<std::size_t>(page);
    if (index > static_cast<std::size_t>(Page::Analysis)) return;
    page_ = page;
    pageBox.setSelectedId(static_cast<int>(page) + 1, juce::dontSendNotification);
    const bool all = page == Page::All;
    const bool showTable = all || page == Page::Analysis;
    sourceBox.setVisible(page != Page::Analysis);
    for (auto &tile : tiles_) tile.setVisible(all);
    table_->setVisible(showTable);
    if (!showTable) detail.setMode(modes[index - 1]);
    detail.setVisible(!showTable);
    resized();
}

void MixAnalysers::setMode(Mode mode) {
    if (const auto found = std::ranges::find(modes, mode); found != modes.end())
        setPage(static_cast<Page>(found - modes.begin() + 1));
}

void MixAnalysers::followSelection(bool value) {
    followsSelection_ = value;
    fixedTrack_ = 0;
    sourceBox.setSelectedId(value ? followId : mainId, juce::dontSendNotification);
}

std::int64_t MixAnalysers::target(const SnapshotReader &reader, std::int64_t main, std::int64_t selected) const noexcept {
    if (followsSelection_) return selected;
    return fixedTrack_ > 0 && (channelRevision_ == reader.seq() || reader.findTrack(fixedTrack_)) ? fixedTrack_ : main;
}

void MixAnalysers::refreshChannels(const SnapshotReader &reader, std::int64_t main) {
    if (channelRevision_ == reader.seq()) return;
    channelRevision_ = reader.seq();
    const auto &tracks = reader.tracks();
    if (std::ranges::equal(tracks, channels_, [main](const auto &track, const Channel &channel) {
            return track->id == channel.id && track->name == channel.name && (track->id == main) == (channel.menuId == 0);
        })) return;

    std::map<std::int64_t, int> previous;
    for (const auto &channel : channels_) previous.emplace(channel.id, channel.menuId);
    channels_.clear();
    channels_.reserve(tracks.size());
    for (const auto &track : tracks) {
        const auto found = previous.find(track->id);
        int menu = 0;
        if (track->id != main) menu = found != previous.end() && found->second > 0 ? found->second : nextMenuId_++;
        channels_.push_back({track->id, track->name, menu});
    }
    if (fixedTrack_ && !reader.findTrack(fixedTrack_)) fixedTrack_ = 0;
    refreshSourceControl();
    tableDirty_ = true;
}

void MixAnalysers::refreshSourceControl() {
    sourceBox.clear(juce::dontSendNotification);
    sourceBox.addItem("Main", mainId);
    sourceBox.addItem("Follow selection", followId);
    int choice = followsSelection_ ? followId : mainId;
    for (const auto &channel : channels_) {
        if (channel.menuId == 0) continue;
        const auto label = channel.name.empty() ? "Channel " + juce::String(static_cast<juce::int64>(channel.id))
                                                : juce::String::fromUTF8(channel.name.c_str());
        sourceBox.addItem(label, channel.menuId);
        if (fixedTrack_ == channel.id) choice = channel.menuId;
    }
    sourceBox.setSelectedId(choice, juce::dontSendNotification);
}

void MixAnalysers::frame(const std::shared_ptr<SharedAnalysis> &broker, const SnapshotReader &reader,
                         std::int64_t main, std::int64_t selected) {
    if (!isShowing() || suspended_ || getLocalBounds().isEmpty()) return;
    if (owner_.owner_before(broker) || broker.owner_before(owner_)) {
        owner_ = broker;
        followSelection(followsSelection_);
        channelRevision_.reset();
    }
    refreshChannels(reader, main);
    const auto track = target(reader, main, selected);
    if (page_ == Page::All) {
        for (auto &tile : tiles_) {
            tile.setSource(detail.point());
            tile.setChannel(detail.view.channel());
            tile.setPalette(detail.view.palette(), detail.view.paletteReversed());
            tile.setSpectrumSlope(detail.view.spectrumSlope());
            tile.present(broker, track);
            tile.frame();
        }
    }
    if (table_->isVisible()) {
        if (tableDirty_) { table_->refresh(); tableDirty_ = false; }
        table_->frame(broker);
    }
    if (detail.isVisible()) detail.frame(broker, track, track);
}

void MixAnalysers::setMainMeterSource(const std::shared_ptr<const MainMeterSource> &source) {
    detail.view.setMainMeterSource(source);
    table_->mainMeter = source;
    tiles_.back().setMainMeterSource(source);
}

void MixAnalysers::suspend(bool value) {
    suspended_ = value;
    detail.suspend(value);
    table_->suspend(value);
    for (auto &tile : tiles_) tile.suspend(value);
}

void MixAnalysers::resized() {
    auto area = getLocalBounds().reduced(look::border);
    auto header = area.removeFromTop(look::fieldHeight);
    pageBox.setBounds(header.removeFromLeft(std::min(look::mixPageWidth, header.getWidth() / 2)));
    header.removeFromLeft(look::gap);
    sourceBox.setBounds(header.removeFromLeft(look::mixChannelMaxWidth));
    area.removeFromTop(look::gap);
    if (page_ == Page::Analysis) { table_->setBounds(area); return; }
    if (page_ != Page::All) { detail.setBounds(area); return; }
    const int columns = area.getWidth() >= 3 * look::mixChannelMaxWidth ? 3 : 2;
    const int rows = 6 / columns;
    for (int i = 0; i < 6; ++i) {
        const int column = i % columns, row = i / columns;
        const int x = area.getWidth() * column / columns, y = area.getHeight() * row / rows;
        const int right = area.getWidth() * (column + 1) / columns, bottom = area.getHeight() * (row + 1) / rows;
        const auto cell = juce::Rectangle<int>{area.getX() + x, area.getY() + y, right - x, bottom - y}.reduced(look::border);
        if (i == 5) table_->setBounds(cell);
        else tiles_[static_cast<std::size_t>(i)].setBounds(cell);
    }
}
} // namespace adi::ui
