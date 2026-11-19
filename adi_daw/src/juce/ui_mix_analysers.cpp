// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_analysers.hpp"
#include "ui_look.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace adi::ui {
namespace {
using Mode = SharedSpectrumView::Mode;
constexpr std::array modes{Mode::Spectrogram, Mode::Spectrum, Mode::Scope, Mode::Phase, Mode::Meters};
constexpr int mainId = 1, followId = 2;

int channelNameWidth(int width) { return std::min(look::mixChannelMaxWidth, width / 4); }

juce::String channelName(std::int64_t id, const std::string &name) {
    return name.empty() ? "Channel " + juce::String(static_cast<juce::int64>(id)) : juce::String::fromUTF8(name.c_str());
}

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

class MixAnalysers::Compare final : public juce::Component, private juce::ComponentListener {
  public:
    explicit Compare(MixAnalysers &owner) : owner_(owner) {
        setTitle("Spectrum comparison");
        for (auto *control : controls()) addAndMakeVisible(*control);
        owner_.compareA.setTitle("Comparison channel A");
        owner_.compareA.setTextWhenNothingSelected("Choose channel A");
        owner_.compareB.setTitle("Comparison channel B");
        owner_.compareB.setTextWhenNothingSelected("Choose channel B");
        populateSpectrumScaleMenu(owner_.compareScale);
        owner_.compareScale.setTitle("Comparison display scale");
        owner_.compareScale.onChange = [this] { if (eligible()) repaint(); };
        owner_.holdB.onClick = [this] {
            const auto current = view();
            if (!current.visible || !comparison_) return;
            if (current.referenceHeld) comparison_->resumeReference();
            else comparison_->holdReference(b_.ownerToken);
            refresh();
        };
        owner_.clearCompare.onClick = [this] {
            if (comparison_ && comparison_->clear()) refresh();
        };
    }
    ~Compare() override {
        unwatch();
        owner_.holdB.onClick = {};
        owner_.clearCompare.onClick = {};
        owner_.compareScale.onChange = {};
    }
    void present(std::shared_ptr<SharedAnalysis> broker, AnalysisComparison::Endpoint a, AnalysisComparison::Endpoint b) {
        if (broker_ == broker && a_ == a && b_ == b) return;
        if (broker_ != broker) {
            comparison_.reset();
            broker_ = std::move(broker);
        }
        a_ = a; b_ = b;
        syncDemand();
        repaint();
    }
    AnalysisComparison::View view() const noexcept {
        auto current = comparison_ ? comparison_->view() : AnalysisComparison::View{};
        if (current.status == AnalysisComparison::Status::Independent &&
            (current.a.metadata->transportRevision != current.b.metadata->transportRevision ||
             current.a.metadata->advancing != current.b.metadata->advancing))
            current.status = AnalysisComparison::Status::Unavailable;
        return current;
    }
    void frame() {
        if (!eligible() || !comparison_ || !view().visible) syncDemand();
        if (comparison_) comparison_->frame();
        refresh();
    }
    void suspend(bool value) { suspended_ = value; syncDemand(); }
    void resized() override {
        auto area = getLocalBounds().reduced(look::border);
        auto row = area.removeFromTop(std::min(look::fieldHeight, area.getHeight()));
        const auto buttons = std::min(look::mixPageWidth, row.getWidth() / 5);
        owner_.clearCompare.setBounds(row.removeFromRight(buttons).reduced(look::border, 0));
        owner_.holdB.setBounds(row.removeFromRight(buttons).reduced(look::border, 0));
        owner_.compareScale.setBounds(row.removeFromRight(buttons).reduced(look::border, 0));
        owner_.compareA.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(look::border, 0));
        owner_.compareB.setBounds(row.reduced(look::border, 0));
        plot_ = area.reduced(look::gap);
        syncDemand();
    }
    void paint(juce::Graphics &g) override {
        g.fillAll(look::display);
        if (plot_.isEmpty()) return;
        auto body = plot_;
        auto caption = body.removeFromTop(look::fieldHeight);
        const auto current = view();
        g.setFont(look::font(look::fontCaption));
        g.setColour(look::value);
        g.drawFittedText("A  " + owner_.compareA.getText(), caption.removeFromLeft(caption.getWidth() / 2),
                         juce::Justification::centredLeft, 1);
        g.setColour(look::accent);
        g.drawFittedText("B  " + owner_.compareB.getText() + (current.referenceHeld ? "  (held)" : ""),
                         caption, juce::Justification::centredLeft, 1);
        g.setColour(look::text);
        if (current.status == AnalysisComparison::Status::Incompatible) {
            g.drawFittedText("The spectra use different formats", body, juce::Justification::centred, 2);
            return;
        }
        if (current.status == AnalysisComparison::Status::Unavailable) {
            g.drawFittedText(valid() ? "Waiting for both channels" : "Choose two different channels",
                             body, juce::Justification::centred, 2);
            return;
        }
        const auto footer = body.removeFromBottom(look::fieldHeight);
        g.drawFittedText("Independent spectra   |   dBFS", footer, juce::Justification::centredLeft, 1);
        const auto scale = selectedSpectrumScale(owner_.compareScale);
        const auto &axis = spectrumAxis(scale);
        const auto topHz = std::min(axis.highHz, current.a.metadata->sampleRate * .5);
        if (topHz <= axis.lowHz || body.isEmpty()) return;
        const bool wide = scale == SharedSpectrumView::SpectrumScale::Wide;
        const auto plot = wide ? body.toFloat() : paintSpectrumGrid(g, body.toFloat(), scale);
        if (plot.isEmpty()) return;
        const auto logWidth = wide ? std::log(topHz / axis.lowHz) : axis.logWidth;
        const float inset = wide ? 0.f : 1.f;
        const auto widthPerLogHz = (plot.getWidth() - inset) / logWidth;
        const auto xAt = [&](double hz) {
            return plot.getX() + static_cast<float>(widthPerLogHz * std::log(hz / axis.lowHz));
        };
        const auto yAt = [&](float db) { return plot.getY() + (plot.getHeight() - inset) * axis.y(db); };
        if (wide) {
            g.setColour(look::outline);
            for (int db = axis.topDb; db >= axis.bottomDb; db -= axis.divisionDb)
                g.drawHorizontalLine(juce::roundToInt(yAt(static_cast<float>(db))), plot.getX(), plot.getRight());
            for (const double frequency : {100., 1000., 10000.})
                if (frequency < topHz)
                    g.drawVerticalLine(juce::roundToInt(xAt(frequency)), plot.getY(), plot.getBottom());
        }
        juce::Graphics::ScopedSaveState state(g);
        if (!wide && !g.reduceClipRegion(plot.toNearestInt())) return;
        const auto stroke = [&](AnalysisComparison::Picture picture, juce::Colour colour) {
            juce::Path curve;
            bool started = false;
            const auto hzPerBin = picture.metadata->sampleRate / picture.metadata->endpoint.request.fftSize;
            for (std::size_t i = 1; i < picture.binsDbfs.size(); ++i) {
                const auto hz = hzPerBin * static_cast<double>(i);
                if (hz < axis.lowHz) continue;
                if (hz > topHz) break;
                if (!started) {
                    curve.startNewSubPath(xAt(hz), yAt(picture.binsDbfs[i]));
                    started = true;
                } else curve.lineTo(xAt(hz), yAt(picture.binsDbfs[i]));
            }
            g.setColour(colour);
            g.strokePath(curve, juce::PathStrokeType(look::line));
        };
        stroke(current.b, look::accent);
        stroke(current.a, look::value);
    }
    void visibilityChanged() override { syncDemand(); }
    void parentHierarchyChanged() override {
        unwatch();
        for (auto *parent = getParentComponent(); parent; parent = parent->getParentComponent()) {
            ancestors_.emplace_back(parent);
            parent->addComponentListener(this);
        }
        syncDemand();
    }
    void moved() override { syncDemand(); }
  private:
    struct Stamp {
        std::uint64_t generation{}, epoch{}, revision{};
        bool operator==(const Stamp &) const = default;
    };
    std::array<juce::Component *, 5> controls() {
        return {&owner_.compareA, &owner_.compareB, &owner_.compareScale, &owner_.holdB, &owner_.clearCompare};
    }
    bool valid() const noexcept {
        return a_.ownerToken && b_.ownerToken && a_.request.track != b_.request.track;
    }
    bool eligible() const noexcept {
        if (suspended_ || !isShowing() || plot_.isEmpty() || !broker_ || !broker_->active()) return false;
        if (const auto *peer = getPeer(); peer && peer->isMinimised()) return false;
        auto visible = plot_;
        for (auto *parent = getParentComponent(); parent; parent = parent->getParentComponent()) {
            visible = visible.getIntersection(getLocalArea(parent, parent->getLocalBounds()));
            if (visible.isEmpty()) return false;
        }
        return true;
    }
    void syncDemand() {
        if (!valid()) {
            // While A is unresolved there is no live comparison to feed. A
            // captured B still belongs to the same explicitly selected endpoint.
            if (comparison_ && comparison_->endpointB() == b_ && comparison_->view().referenceHeld)
                comparison_->setVisible(false);
            else
                comparison_.reset();
        } else if (eligible()) {
            try {
                if (!comparison_) comparison_ = std::make_unique<AnalysisComparison>(broker_);
                if ((comparison_->endpointA() == a_ && comparison_->endpointB() == b_) ||
                    comparison_->configure(a_, b_))
                    comparison_->setVisible(true);
                else
                    comparison_->setVisible(false);
            } catch (...) {
                comparison_.reset();
            }
        } else if (comparison_) {
            comparison_->setVisible(false);
        }
        refresh();
    }
    void refresh() {
        const auto current = view();
        const auto stamp = [](const auto *m) { return m ? Stamp{m->generation, m->sourceEpoch, m->revision} : Stamp{}; };
        const std::array observed{stamp(current.a.metadata), stamp(current.b.metadata)};
        if (current.referenceHeld != held_) owner_.holdB.setButtonText(current.referenceHeld ? "Resume B" : "Hold B");
        owner_.holdB.setEnabled(current.visible && (current.referenceHeld || current.b.metadata != nullptr));
        owner_.clearCompare.setEnabled(current.visible);
        if (observed != shown_ || current.status != status_ || current.referenceHeld != held_) {
            shown_ = observed; status_ = current.status; held_ = current.referenceHeld;
            repaint();
        }
    }
    void unwatch() noexcept {
        for (auto &parent : ancestors_) if (parent) parent->removeComponentListener(this);
        ancestors_.clear();
    }
    void componentVisibilityChanged(juce::Component &) override { syncDemand(); }
    void componentParentHierarchyChanged(juce::Component &) override { parentHierarchyChanged(); }
    void componentMovedOrResized(juce::Component &, bool, bool) override { syncDemand(); }
    void componentBeingDeleted(juce::Component &) override {
        unwatch();
        if (comparison_) comparison_->setVisible(false);
    }
    MixAnalysers &owner_;
    std::shared_ptr<SharedAnalysis> broker_;
    std::unique_ptr<AnalysisComparison> comparison_;
    AnalysisComparison::Endpoint a_, b_;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors_;
    std::array<Stamp, 2> shown_{};
    AnalysisComparison::Status status_ = AnalysisComparison::Status::Unavailable;
    juce::Rectangle<int> plot_;
    bool suspended_ = false, held_ = false;
};

MixAnalysers::MixAnalysers() {
    table_ = std::make_unique<Table>(*this);
    compare_ = std::make_unique<Compare>(*this);
    setTitle("Analysers");
    pageBox.addItemList({"All", "Spectrogram", "Spectrum", "Classic Scope", "Phase", "Loudness", "Analysis", "1 s Average", "Compare"}, 1);
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
    addChildComponent(*compare_);
    const auto comparisonChanged = [this] { updateComparison(owner_.lock()); };
    compareA.onChange = comparisonChanged; compareB.onChange = comparisonChanged;
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        tiles_[i].setMode(modes[i]);
        addChildComponent(tiles_[i]);
    }
    refreshSourceControl();
    setPage(Page::All);
}
MixAnalysers::~MixAnalysers() { compare_.reset(); table_.reset(); }

void MixAnalysers::setPage(Page page) {
    const auto index = static_cast<std::size_t>(page);
    if (index > static_cast<std::size_t>(Page::Compare)) return;
    page_ = page;
    pageBox.setSelectedId(static_cast<int>(page) + 1, juce::dontSendNotification);
    const bool all = page == Page::All;
    const bool showTable = all || page == Page::Analysis;
    const bool comparing = page == Page::Compare;
    sourceBox.setVisible(page != Page::Analysis && !comparing);
    for (auto &tile : tiles_) tile.setVisible(all);
    table_->setVisible(showTable);
    if (!showTable && !comparing) {
        if (page == Page::Scope) detail.setMode(Mode::ClassicScope);
        else if (page == Page::Average) detail.setMode(Mode::SpectrumAverage);
        else detail.setMode(modes[index - 1]);
    }
    detail.setVisible(!showTable && !comparing);
    compare_->setVisible(comparing);
    resized();
}

void MixAnalysers::setMode(Mode mode) {
    if (mode == Mode::ClassicScope) mode = Mode::Scope;
    if (mode == Mode::SpectrumAverage) setPage(Page::Average);
    else if (const auto found = std::ranges::find(modes, mode); found != modes.end())
        setPage(static_cast<Page>(found - modes.begin() + 1));
}

void MixAnalysers::followSelection(bool value) {
    followsSelection_ = value;
    fixedTrack_ = 0;
    sourceBox.setSelectedId(value ? followId : mainId, juce::dontSendNotification);
}

std::int64_t MixAnalysers::target(const SnapshotReader &reader, std::int64_t main, std::int64_t selected) const noexcept {
    if (followsSelection_) return selected;
    return fixedTrack_ > 0 && reader.findTrack(fixedTrack_) ? fixedTrack_ : main;
}

void MixAnalysers::refreshChannels(const SnapshotReader &reader, std::int64_t main, std::uint64_t viewGeneration) {
    // ProjectView snapshots carry no publisher sequence. Its generation and
    // the borrowed reader identity also cover a detached frame or preview.
    if (channelGeneration_ == viewGeneration && channelSnapshot_ == reader.get() && channelMain_ == main) return;
    channelGeneration_ = viewGeneration;
    channelSnapshot_ = reader.get();
    channelMain_ = main;
    const auto &tracks = reader.tracks();
    if (std::ranges::equal(tracks, channels_, [main](const auto &track, const Channel &channel) {
            return track->id == channel.id && track->name == channel.name && (track->id == main) == channel.isMain;
        })) return;

    std::map<std::int64_t, int> previous;
    for (const auto &channel : channels_) previous.emplace(channel.id, channel.menuId);
    channels_.clear();
    channels_.reserve(tracks.size());
    for (const auto &track : tracks) {
        const auto found = previous.find(track->id);
        const int menu = found != previous.end() ? found->second : nextMenuId_++;
        channels_.push_back({track->id, track->name, menu, track->id == main});
    }
    if (fixedTrack_ && !reader.findTrack(fixedTrack_)) fixedTrack_ = 0;
    refreshSourceControl();
    updateComparison(owner_.lock());
    tableDirty_ = true;
}

void MixAnalysers::refreshSourceControl() {
    const int primary = compareA.getSelectedId(), reference = compareB.getSelectedId();
    compareA.clear(juce::dontSendNotification);
    compareB.clear(juce::dontSendNotification);
    sourceBox.clear(juce::dontSendNotification);
    sourceBox.addItem("Main", mainId);
    sourceBox.addItem("Follow selection", followId);
    int choice = followsSelection_ ? followId : mainId;
    for (const auto &channel : channels_) {
        const auto label = channelName(channel.id, channel.name);
        compareA.addItem(label, channel.menuId); compareB.addItem(label, channel.menuId);
        if (channel.isMain) continue;
        sourceBox.addItem(label, channel.menuId);
        if (fixedTrack_ == channel.id) choice = channel.menuId;
    }
    sourceBox.setSelectedId(choice, juce::dontSendNotification);
    compareA.setSelectedId(primary, juce::dontSendNotification);
    compareB.setSelectedId(reference, juce::dontSendNotification);
    compare_->repaint();
}

AnalysisComparison::View MixAnalysers::comparisonView() const noexcept { return compare_->view(); }
void MixAnalysers::updateComparison(const std::shared_ptr<SharedAnalysis> &broker) {
    const auto selected = [this](int id) {
        AnalysisComparison::Endpoint endpoint;
        if (id <= 0) return endpoint;
        const auto found = std::ranges::find(channels_, id, &Channel::menuId);
        if (found != channels_.end()) {
            endpoint.request.track = found->id;
            endpoint.request.hopSize = endpoint.request.fftSize / 4;
            endpoint.ownerToken = static_cast<std::uint64_t>(id);
        }
        return endpoint;
    };
    compare_->present(broker, selected(compareA.getSelectedId()), selected(compareB.getSelectedId()));
}
void MixAnalysers::frame(const std::shared_ptr<SharedAnalysis> &broker, const SnapshotReader &reader,
                         std::int64_t main, std::int64_t selected, std::uint64_t viewGeneration) {
    if (!isShowing() || suspended_ || getLocalBounds().isEmpty()) return;
    if (owner_.owner_before(broker) || broker.owner_before(owner_)) {
        owner_ = broker;
        followSelection(followsSelection_);
        channelGeneration_.reset();
        channelSnapshot_ = nullptr;
        compareA.setSelectedId(0, juce::dontSendNotification);
        compareB.setSelectedId(0, juce::dontSendNotification);
        compare_->present({}, {}, {});
    }
    refreshChannels(reader, main, viewGeneration);
    if (page_ == Page::Compare) {
        updateComparison(broker);
        compare_->frame();
        return;
    }
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
    compare_->suspend(value);
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
    if (page_ == Page::Compare) { compare_->setBounds(area); return; }
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
