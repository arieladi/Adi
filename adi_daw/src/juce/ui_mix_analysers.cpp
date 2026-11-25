// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mix_analysers.hpp"
#include "ui_look.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
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
        const auto tracePath = [&](AnalysisComparison::Picture picture) {
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
            return curve;
        };
        const auto reference = tracePath(current.b);
        const auto primary = tracePath(current.a);
        if (spectrumScaleHasFill(scale)) {
            const auto fillTrace = [&](juce::Path area, juce::Colour colour) {
                if (area.isEmpty()) return;
                area.lineTo(area.getCurrentPosition().x, plot.getBottom());
                area.lineTo(area.getBounds().getX(), plot.getBottom());
                area.closeSubPath();
                g.setColour(colour.withAlpha(spectrumFillOpacity));
                g.fillPath(area);
            };
            fillTrace(reference, look::accent);
            fillTrace(primary, look::value);
        }
        const juce::PathStrokeType stroke(look::line);
        g.setColour(look::accent);
        g.strokePath(reference, stroke);
        g.setColour(look::value);
        g.strokePath(primary, stroke);
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

class MixAnalysers::SpectrumStatistics final : public juce::Component {
    using Picture = std::shared_ptr<const SharedAnalysis::Result>;

  public:
    SpectrumStatistics() {
        setInterceptsMouseClicks(false, true);
        for (auto *label : {&balance_, &correlation_, &integratedRms_, &maxCrest_, &samplePeak_, &sampleClips_})
            label->setInterceptsMouseClicks(false, false);
        balance_.setTitle("RMS Balance");
        balance_.setTooltip("Left and right RMS balance over the most recent two seconds of the primary Spectrum source.");
        correlation_.setTitle("Stereo Correlation");
        correlation_.setTooltip("Signed left and right correlation over the primary Spectrum window.");
        balance_.setText("RMS balance --", juce::dontSendNotification);
        correlation_.setText("Correlation --", juce::dontSendNotification);
        for (auto *label : {&balance_, &correlation_}) addAndMakeVisible(*label);
        integratedRms_.setTitle("Integrated RMS");
        integratedRms_.setTooltip("Left and right integrated RMS since this level interval began.");
        maxCrest_.setTitle("Max Crest");
        maxCrest_.setTooltip("Highest left and right crest factors in this level interval.");
        samplePeak_.setTitle("Sample Peak");
        samplePeak_.setTooltip("Highest left and right sample peaks in this level interval.");
        sampleClips_.setTitle("Sample Clips");
        sampleClips_.setTooltip("Number of left and right samples with magnitude above full scale in this level interval.");
        for (auto *label : {&integratedRms_, &maxCrest_, &samplePeak_, &sampleClips_}) addChildComponent(*label);
        truePeak_.setTitle("Spectrum True Peak");
        truePeak_.setTooltip("Show held true peak for each channel. Sample clip counts remain visible.");
        truePeak_.onClick = [this] { if (onTruePeakChanged) onTruePeakChanged(); };
        addChildComponent(truePeak_);
        refreshAppearance();
    }

    void present(const Picture &window) {
        if (samePicture(window, last_)) return;
        last_ = window;
        juce::String balance("--"), correlation("--");
        if (window && window->balanceRms2s) {
            const double left = (*window->balanceRms2s)[0], right = (*window->balanceRms2s)[1];
            const bool hasLeft = std::isfinite(left) && left > 0., hasRight = std::isfinite(right) && right > 0.;
            if (hasLeft && hasRight) {
                const double difference = 20. * std::log10(left / right);
                balance = std::abs(difference) < .05 ? juce::String("Centre")
                    : juce::String(difference > 0. ? "L " : "R ") + juce::String(std::abs(difference), 1) + " dB";
            } else if (hasLeft || hasRight) {
                balance = hasLeft ? "L only" : "R only";
            }
        }
        if (window && window->rms[0] > 0.f && window->rms[1] > 0.f && std::isfinite(window->correlation)) {
            const auto value = std::abs(window->correlation) < .005 ? 0. : window->correlation;
            correlation = juce::String(value < 0. ? "" : "+") + juce::String(value, 2);
        }
        balance_.setText("RMS balance " + balance, juce::dontSendNotification);
        correlation_.setText("Correlation " + correlation, juce::dontSendNotification);
        if (levelRowsVisible_) presentLevels(window);
    }

    std::function<void()> onTruePeakChanged;
    bool truePeakRequested() const noexcept { return levelRowsVisible_ && truePeak_.getToggleState(); }
    void updateTruePeakReadout(const Picture &current) {
        const bool enabled = truePeak_.getToggleState();
        if (enabled) truePeakBeforeAdmission_ = current;
        else truePeakBeforeAdmission_.reset();
        samplePeak_.setTitle(enabled ? "True Peak" : "Sample Peak");
        samplePeak_.setTooltip(enabled ? "Highest left and right reconstructed peaks in this true peak interval."
                                      : "Highest left and right sample peaks in this level interval.");
        presentLevels(current);
    }

    void setLevelRowsVisible(bool visible, const Picture &current) {
        if (levelRowsVisible_ == visible) return;
        levelRowsVisible_ = visible;
        if (visible) {
            // The curve can remain on screen when a new level interval is admitted.
            // Its old integrated readings do not belong to the new interval.
            levelsBeforeAdmission_ = current;
            presentLevels({});
        } else {
            levelsBeforeAdmission_.reset();
        }
        for (auto *label : {&integratedRms_, &maxCrest_, &samplePeak_, &sampleClips_}) label->setVisible(visible);
        truePeak_.setVisible(visible);
    }

    void refreshAppearance() {
        const std::pair appearance{look::themeId(), look::densityId()};
        if (appearance_ == appearance) return;
        appearance_ = appearance;
        for (auto *label : {&balance_, &correlation_, &integratedRms_, &maxCrest_, &samplePeak_, &sampleClips_}) {
            label->setFont(look::font(look::fontCaption));
            label->setColour(juce::Label::textColourId, look::textDim);
        }
    }

    void resized() override {
        const auto placeRow = [](juce::Rectangle<int> row, juce::Label &left, juce::Label &right) {
            left.setBounds(row.removeFromLeft((row.getWidth() - look::gap) / 2));
            row.removeFromLeft(look::gap);
            right.setBounds(row);
        };
        auto area = getLocalBounds();
        placeRow(area.removeFromTop(look::mixChannelRowHeight), balance_, correlation_);
        if (levelRowsVisible_) {
            placeRow(area.removeFromTop(look::mixChannelRowHeight), integratedRms_, maxCrest_);
            truePeak_.setBounds(area.removeFromLeft(look::mixChannelLevelWidth + look::fieldHeight));
            area.removeFromLeft(look::gap);
            placeRow(area, samplePeak_, sampleClips_);
        }
    }
    void visibilityChanged() override { if (!isShowing()) present({}); }

  private:
    static bool samePicture(const Picture &picture, const std::weak_ptr<const SharedAnalysis::Result> &reference) noexcept {
        return !picture.owner_before(reference) && !reference.owner_before(picture);
    }

    void presentLevels(const Picture &window) {
        const bool showTruePeak = truePeak_.getToggleState();
        std::array<juce::String, 2> rms{"--", "--"}, crest{"--", "--"};
        std::array<juce::String, 2> peak{"--", "--"}, clips{"--", "--"};
        if (window && window->spectrumLevels && !samePicture(window, levelsBeforeAdmission_)) {
            const auto &levels = *window->spectrumLevels;
            const bool truePeakReady = levels.truePeak && !samePicture(window, truePeakBeforeAdmission_);
            const auto text = [](double db) { return juce::String(std::abs(db) < .05 ? 0. : db, 1); };
            for (std::size_t channel = 0; channel < rms.size(); ++channel) {
                const double peakValue = showTruePeak
                    ? (truePeakReady ? (*levels.truePeak)[channel] : 0.)
                    : static_cast<double>(levels.samplePeak[channel]);
                if (std::isfinite(peakValue) && peakValue > 0.) {
                    const double db = 20. * std::log10(peakValue);
                    peak[channel] = juce::String(db >= .05 ? "+" : "") + text(db);
                }
                clips[channel] = juce::String(static_cast<juce::uint64>(levels.sampleClips[channel]));
                const auto magnitude = levels.integratedRms[channel];
                if (!std::isfinite(magnitude) || magnitude <= 0.) continue;
                rms[channel] = text(20. * std::log10(magnitude));
                const auto crestDb = levels.maxCrestDb[channel];
                if (std::isfinite(crestDb) && crestDb >= 0.) crest[channel] = text(crestDb);
            }
        }
        integratedRms_.setText("Integrated RMS  L " + rms[0] + "  R " + rms[1] + " dBFS",
                               juce::dontSendNotification);
        maxCrest_.setText("Max crest  L " + crest[0] + "  R " + crest[1] + " dB", juce::dontSendNotification);
        samplePeak_.setText((showTruePeak ? "True peak  L " : "Sample peak  L ") + peak[0] + "  R " + peak[1] +
                            (showTruePeak ? " dBTP" : " dBFS"), juce::dontSendNotification);
        sampleClips_.setText("Sample clips  L " + clips[0] + "  R " + clips[1], juce::dontSendNotification);
    }

    juce::Label balance_, correlation_, integratedRms_, maxCrest_, samplePeak_, sampleClips_;
    juce::ToggleButton truePeak_{"TP"};
    std::optional<std::pair<ThemeId, DensityId>> appearance_;
    std::weak_ptr<const SharedAnalysis::Result> last_, levelsBeforeAdmission_, truePeakBeforeAdmission_;
    bool levelRowsVisible_ = false;
};

class MixAnalysers::SpectrogramDelta final : public juce::Component {
  public:
    SpectrogramDelta() {
        setTitle("Spectrogram post minus pre");
        cursor_.setTitle("Spectrogram comparison cursor");
        cursor_.setJustificationType(juce::Justification::centred);
        cursor_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(cursor_);
        freeze_.setTitle("Freeze Spectrogram comparison");
        freeze_.setEnabled(false);
        freeze_.onClick = [this] {
            if (!comparison_) return;
            const SafePointer<SpectrogramDelta> alive(this);
            if (comparison_->frozen()) {
                if (comparison_->thaw() && alive) clear();
            } else if (minimumRevision_) {
                comparison_->freeze(heardEnd_, *minimumRevision_);
            }
            if (alive) syncFreeze();
        };
        addAndMakeVisible(freeze_);
    }

    void stop() {
        cursorPoint_.reset();
        cursor_.setText({}, juce::dontSendNotification);
        if (!comparison_) return;
        if (!comparison_->frozen()) clear();
        comparison_->hide();
    }

    void frame(const std::shared_ptr<SharedAnalysis> &broker, std::int64_t track, SharedAnalysis::Channel channel,
               std::int64_t heardEnd, std::optional<std::uint64_t> minimumRevision) {
        const SafePointer<SpectrogramDelta> alive(this);
        if (!broker || !broker->active()) {
            auto retired = std::move(comparison_);
            owner_.reset();
            clear();
            return;
        }
        if (owner_.lock() != broker) {
            auto retired = std::move(comparison_);
            owner_ = broker;
            if (broker) comparison_ = std::make_unique<SpectrogramComparison>(broker);
            clear();
            retired.reset();
            if (!alive) return;
        }
        if (!plotIsShowing() || !comparison_ || track <= 0) {
            stop();
            return;
        }
        if (track != track_ || channel != channel_) clear();
        track_ = track;
        channel_ = channel;
        SharedAnalysis::Request request{track};
        request.channel = channel;
        const bool showing = comparison_->show(request);
        if (!alive) return;
        if (!showing) { stop(); return; }
        heardEnd_ = heardEnd;
        minimumRevision_ = minimumRevision;
        const bool held = comparison_->frozen();
        syncFreeze();
        if (held && image_.isValid() && theme_ == look::themeId()) return;
        comparison_->frame();
        if (!minimumRevision && !held) { clear(); return; }
        std::array<SpectrogramColumnPair, width> storage;
        const auto columns = comparison_->columns(heardEnd, minimumRevision.value_or(0), storage);
        const auto format = comparison_->format();
        if (columns.empty() || !format) { clear(); return; }
        freeze_.setEnabled(true);
        updateRaster(columns, *format);
    }

    void visibilityChanged() override { if (!isShowing()) stop(); }

    void mouseMove(const juce::MouseEvent &event) override {
        cursorPoint_ = event.position;
        refreshCursor();
    }

    void mouseExit(const juce::MouseEvent &) override {
        cursorPoint_.reset();
        cursor_.setText({}, juce::dontSendNotification);
    }

    void resized() override {
        auto header = getLocalBounds().reduced(look::border).removeFromTop(look::fieldHeight);
        freeze_.setBounds(header.removeFromRight(std::min(look::mixPageWidth, header.getWidth() / 3)));
        header.removeFromLeft(header.getWidth() / 3);
        cursor_.setBounds(header.removeFromLeft(header.getWidth() / 2));
        if (getLocalBounds().isEmpty()) stop();
        else refreshCursor();
    }

    void paint(juce::Graphics &graphics) override {
        auto bounds = getLocalBounds();
        graphics.fillAll(look::display);
        bounds.reduce(look::border, look::border);
        auto header = bounds.removeFromTop(look::fieldHeight);
        header = header.withRight(freeze_.getX());
        graphics.setFont(look::font());
        graphics.setColour(look::record);
        graphics.drawFittedText("-24 dB", header.removeFromLeft(header.getWidth() / 3), juce::Justification::centredLeft, 1);
        const auto centre = header.removeFromLeft(header.getWidth() / 2);
        if (cursor_.getText().isEmpty()) {
            graphics.setColour(look::textDim);
            graphics.drawFittedText("Post minus pre", centre, juce::Justification::centred, 1);
        }
        graphics.setColour(look::accent);
        graphics.drawFittedText("+24 dB", header, juce::Justification::centredRight, 1);
        bounds.removeFromTop(look::gap);
        if (image_.isValid()) graphics.drawImage(image_, bounds.toFloat());
        else {
            graphics.setColour(look::textDim);
            graphics.drawFittedText("Waiting for matched audio", bounds, juce::Justification::centred, 1);
        }
    }

  private:
    static constexpr int width = 128, height = 128;
    static constexpr float rangeDb = 24.f;
    using ColumnKey = std::array<std::uint64_t, 2>;
    struct BinChange {
        std::size_t bin = 0;
        float db = 0.f;
    };

    juce::Rectangle<int> plotBounds() const {
        return getLocalBounds().reduced(look::border).withTrimmedTop(look::fieldHeight + look::gap);
    }

    bool plotIsShowing() const {
        if (!isShowing()) return false;
        if (const auto *peer = getPeer(); peer && peer->isMinimised()) return false;
        auto visible = getLocalBounds().reduced(look::border).withTrimmedTop(look::fieldHeight + look::gap);
        for (auto *parent = getParentComponent(); parent && !visible.isEmpty(); parent = parent->getParentComponent())
            visible = visible.getIntersection(getLocalArea(parent, parent->getLocalBounds()));
        return !visible.isEmpty();
    }

    void syncFreeze() {
        const bool held = comparison_->frozen();
        freeze_.setToggleState(held, juce::dontSendNotification);
        freeze_.setButtonText(held ? "Thaw" : "Freeze");
    }

    void clear() {
        if (image_.isValid()) {
            image_ = {};
            lastHop_.reset();
            repaint();
        }
        cursor_.setText({}, juce::dontSendNotification);
        freeze_.setEnabled(comparison_ && comparison_->frozen());
    }

    void prepareImage(SpectrogramComparison::Format format, std::size_t bins) {
        format_ = format;
        theme_ = look::themeId();
        image_ = juce::Image(juce::Image::RGB, width, height, true, juce::SoftwareImageType{});
        image_.clear(image_.getBounds(), look::display);
        keys_.fill({});
        lastHop_.reset();
        const double upper = std::min(1., 40000. / format.sampleRate);
        const double lower = std::min(upper, 40. / format.sampleRate);
        const auto edge = [&](int row) {
            const double fraction = 1. - static_cast<double>(row) / height;
            return static_cast<std::size_t>(lower * std::pow(upper / lower, fraction) * static_cast<double>(bins - 1));
        };
        auto high = edge(0);
        for (int y = 0; y < height; ++y) {
            const auto low = std::min(edge(y + 1), bins - 1);
            rowBins_[static_cast<std::size_t>(y)] = {low, std::min(bins, std::max(low, high) + 1)};
            high = low;
        }
    }

    static std::optional<BinChange> strongestChange(const SpectrogramColumnPair *pair, std::array<std::size_t, 2> bins) {
        if (!pair) return {};
        float strongest = dsp::kDbFloor;
        std::optional<BinChange> result;
        for (auto bin = bins[0]; bin < bins[1]; ++bin) {
            const auto db = pair->changeDb(bin);
            if (!db) continue;
            const auto peak = std::max((*pair->pre->binsDbfs)[bin], (*pair->post->binsDbfs)[bin]);
            if (peak > strongest) { strongest = peak; result = BinChange{bin, *db}; }
        }
        return result;
    }

    void refreshCursor() {
        if (!cursorPoint_ || !plotBounds().toFloat().contains(*cursorPoint_) ||
            !comparison_ || !lastHop_ || (!minimumRevision_ && !comparison_->frozen())) {
            cursor_.setText({}, juce::dontSendNotification);
            return;
        }
        std::array<SpectrogramColumnPair, width> storage;
        updateCursor(comparison_->columns(heardEnd_, minimumRevision_.value_or(0), storage));
    }

    void updateCursor(std::span<const SpectrogramColumnPair> columns) {
        juce::String text;
        const auto plot = plotBounds().toFloat();
        if (cursorPoint_ && lastHop_ && plot.contains(*cursorPoint_)) {
            const auto x = static_cast<int>((cursorPoint_->x - plot.getX()) / plot.getWidth() * width);
            const auto y = static_cast<int>((cursorPoint_->y - plot.getY()) / plot.getHeight() * height);
            const auto age = static_cast<std::uint64_t>(width - 1 - std::clamp(x, 0, width - 1));
            const auto rows = rowBins_[static_cast<std::size_t>(std::clamp(y, 0, height - 1))];
            for (const auto &pair : columns) {
                const auto hop = pair.pre->firstStamp / format_.hop;
                if (static_cast<std::uint64_t>(*lastHop_) - static_cast<std::uint64_t>(hop) != age) continue;
                if (const auto value = strongestChange(&pair, rows)) {
                    const auto bins = pair.pre->binsDbfs->size();
                    const auto hz = format_.sampleRate * (.5 * static_cast<double>(value->bin) / static_cast<double>(bins - 1));
                    text = juce::String(hz, 1) + " Hz   " + (value->db >= 0.f ? "+" : "") + juce::String(value->db, 2) +
                        " dB   Sample " + juce::String(static_cast<juce::int64>(pair.pre->firstStamp));
                }
                break;
            }
        }
        cursor_.setText(text, juce::dontSendNotification);
    }

    void updateRaster(std::span<const SpectrogramColumnPair> columns, SpectrogramComparison::Format format) {
        const auto nextHop = columns.back().pre->firstStamp / format.hop;
        const bool reset = !image_.isValid() || format != format_ || theme_ != look::themeId() ||
            (lastHop_ && nextHop < *lastHop_);
        if (reset) prepareImage(format, columns.back().pre->binsDbfs->size());
        const auto distance = lastHop_ ? static_cast<std::uint64_t>(nextHop) - static_cast<std::uint64_t>(*lastHop_) : 0;
        const auto shift = static_cast<int>(std::min<std::uint64_t>(distance, width));
        if (shift > 0) {
            if (shift < width) {
                image_.moveImageSection(0, 0, shift, 0, width - shift, height);
                std::move(keys_.begin() + shift, keys_.end(), keys_.begin());
            }
            std::fill(keys_.end() - shift, keys_.end(), ColumnKey{});
            image_.clear({width - shift, 0, shift, height}, look::display);
        }
        lastHop_ = nextHop;
        std::array<const SpectrogramColumnPair *, width> visible{};
        for (const auto &pair : columns) {
            const auto hop = pair.pre->firstStamp / format.hop;
            const auto age = static_cast<std::uint64_t>(nextHop) - static_cast<std::uint64_t>(hop);
            if (age < visible.size()) visible[visible.size() - 1 - static_cast<std::size_t>(age)] = &pair;
        }
        const auto key = [](const SpectrogramColumnPair *pair) {
            return pair ? ColumnKey{pair->pre->serial, pair->post->serial} : ColumnKey{};
        };
        const bool changed = !std::equal(visible.begin(), visible.end(), keys_.begin(),
                                         [&](const auto *pair, const auto &prior) { return key(pair) == prior; });
        if (changed) {
            juce::Image::BitmapData pixels(image_, juce::Image::BitmapData::writeOnly);
            for (int x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(x);
                const auto *pair = visible[index];
                const auto next = key(pair);
                if (next == keys_[index]) continue;
                keys_[index] = next;
                for (int y = 0; y < height; ++y) {
                    const auto db = strongestChange(pair, rowBins_[static_cast<std::size_t>(y)]);
                    const juce::Colour colour = db ? look::frame.interpolatedWith(db->db < 0.f ? look::record : look::accent,
                        std::min(1.f, std::abs(db->db) / rangeDb)) : look::display;
                    pixels.setPixelColour(x, y, colour);
                }
            }
        }
        if (reset || shift > 0 || changed) {
            if (cursorPoint_) updateCursor(columns);
            repaint();
        }
    }

    std::weak_ptr<SharedAnalysis> owner_;
    std::unique_ptr<SpectrogramComparison> comparison_;
    std::int64_t track_ = 0, heardEnd_ = 0;
    SharedAnalysis::Channel channel_ = SharedAnalysis::Channel::Mid;
    std::optional<std::uint64_t> minimumRevision_;
    SpectrogramComparison::Format format_;
    std::optional<std::int64_t> lastHop_;
    std::optional<juce::Point<float>> cursorPoint_;
    juce::Image image_;
    std::array<ColumnKey, width> keys_{};
    std::array<std::array<std::size_t, 2>, height> rowBins_{};
    ThemeId theme_ = ThemeId::Dark;
    juce::TextButton freeze_{"Freeze"};
    juce::Label cursor_;
};

MixAnalysers::MixAnalysers() {
    table_ = std::make_unique<Table>(*this);
    compare_ = std::make_unique<Compare>(*this);
    spectrumStatistics_ = std::make_unique<SpectrumStatistics>();
    spectrogramDelta_ = std::make_unique<SpectrogramDelta>();
    spectrumStatistics_->onTruePeakChanged = [this] {
        spectrumStatistics_->updateTruePeakReadout(detail.view.snapshot());
        detail.view.setSpectrumTruePeakEnabled(spectrumStatistics_->truePeakRequested());
    };
    setTitle("Analysers");
    pageBox.addItemList({"All", "Spectrogram", "Spectrum", "Classic Scope", "Phase", "Loudness", "Analysis",
                         "1 s Average", "Compare"}, 1);
    pageBox.setTitle("Analyser page");
    sourceBox.setTitle("Analyser source");
    compareSpectrogram_.setTitle("Spectrogram comparison");
    compareSpectrogram_.setTooltip("Show the signed change between this channel's pre and post effects.");
    compareSpectrogram_.onClick = [this] {
        const SafePointer<MixAnalysers> alive(this);
        updateSpectrogramView();
        if (alive) resized();
    };
    spectrumTraceBox.addItem("Realtime", static_cast<int>(Mode::Spectrum) + 1);
    spectrumTraceBox.addItem("Realtime Average", static_cast<int>(Mode::SpectrumAverage) + 1);
    spectrumTraceBox.addItem("Cumulative Average", static_cast<int>(Mode::SpectrumCumulativeAverage) + 1);
    spectrumTraceBox.addItem("Realtime Sigma", static_cast<int>(Mode::SpectrumRealtimeSigma) + 1);
    spectrumTraceBox.addItem("Realtime Maximum", static_cast<int>(Mode::SpectrumMaximum) + 1);
    spectrumTraceBox.addItem("Cumulative Maximum", static_cast<int>(Mode::SpectrumCumulativeMaximum) + 1);
    spectrumTraceBox.setTitle("Spectrum trace");
    spectrumTraceBox.setSelectedId(static_cast<int>(Mode::Spectrum) + 1, juce::dontSendNotification);
    spectrumTraceBox.onChange = [this] {
        const auto id = spectrumTraceBox.getSelectedId();
        if (page_ != Page::Spectrum || id <= 0) return;
        const auto mode = static_cast<Mode>(id - 1);
        const bool canPin = SharedSpectrumView::supportsSpectrumOverlays(mode);
        if (!canPin) clearPinnedSpectrum();
        detail.setMode(mode);
        pinnedTracks_.setEnabled(canPin);
    };
    phaseBandsBox_.addItemList({"Correlation", "Width", "Pan"}, 1);
    phaseBandsBox_.setSelectedId(1, juce::dontSendNotification);
    phaseBandsBox_.setTitle("Phase band display");
    phaseBandsBox_.setTooltip("Show signed correlation, side energy fraction, or stereo pan by frequency band.");
    phaseBandsBox_.onChange = [this] { updatePhaseBandDisplay(); };
    pinnedTracks_.setTitle("Pinned tracks");
    pinnedTracks_.setTooltip("Overlay your Stack pins in Realtime, either average, or Realtime Sigma.");
    pinnedTracks_.onClick = [this] {
        if (!pinnedTracks_.getToggleState()) clearPinnedSpectrum();
    };
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
    addChildComponent(compareSpectrogram_);
    addChildComponent(spectrumTraceBox);
    addChildComponent(phaseBandsBox_);
    addChildComponent(pinnedTracks_);
    addChildComponent(detail);
    detail.usePageControls();
    detail.view.enablePhaseBands();
    addChildComponent(*table_);
    addChildComponent(*compare_);
    addChildComponent(*spectrumStatistics_);
    addChildComponent(*spectrogramDelta_);
    const auto comparisonChanged = [this] { updateComparison(owner_.lock()); };
    compareA.onChange = comparisonChanged; compareB.onChange = comparisonChanged;
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        if (modes[i] == Mode::Phase) tiles_[i].enablePhaseBands();
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
    spectrumTraceBox.setVisible(page == Page::Spectrum);
    pinnedTracks_.setVisible(page == Page::Spectrum);
    if (page != Page::Spectrum) clearPinnedSpectrum();
    for (auto &tile : tiles_) tile.setVisible(all);
    table_->setVisible(showTable);
    if (!showTable && !comparing) {
        if (page == Page::Scope) detail.setMode(Mode::ClassicScope);
        else if (page == Page::Average) detail.setMode(Mode::SpectrumAverage);
        else if (page == Page::Spectrum) detail.setMode(static_cast<Mode>(spectrumTraceBox.getSelectedId() - 1));
        else detail.setMode(modes[index - 1]);
    }
    const SafePointer<MixAnalysers> alive(this);
    updateSpectrogramView();
    if (!alive) return;
    const bool canPin = page == Page::Spectrum && SharedSpectrumView::supportsSpectrumOverlays(detail.mode());
    pinnedTracks_.setEnabled(canPin);
    if (!canPin) clearPinnedSpectrum();
    compare_->setVisible(comparing);
    resized();
}

void MixAnalysers::updateSpectrogramView() {
    const bool spectrogram = page_ == Page::Spectrogram;
    const bool delta = spectrogram && compareSpectrogram_.getToggleState();
    const bool detailVisible = page_ != Page::All && page_ != Page::Analysis && page_ != Page::Compare && !delta;
    compareSpectrogram_.setVisible(spectrogram);
    detail.setVisible(detailVisible);
    spectrogramDelta_->setVisible(delta);
}

void MixAnalysers::visibilityChanged() {
    if (!isShowing() && spectrogramDelta_) spectrogramDelta_->stop();
}

void MixAnalysers::setMode(Mode mode) {
    if (mode == Mode::ClassicScope) mode = Mode::Scope;
    if (mode == Mode::SpectrumAverage) setPage(Page::Average);
    else if (SharedSpectrumView::supportsSpectrumOverlays(mode) ||
             mode == Mode::SpectrumMaximum || mode == Mode::SpectrumCumulativeMaximum) {
        spectrumTraceBox.setSelectedId(static_cast<int>(mode) + 1, juce::dontSendNotification);
        setPage(Page::Spectrum);
    }
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
void MixAnalysers::updatePhaseBandDisplay() {
    using Display = SharedSpectrumView::PhaseBandDisplay;
    const auto selected = phaseBandsBox_.getSelectedId();
    const auto mode = page_ == Page::Phase && phaseBandsBox_.isVisible() && selected > 0
        ? static_cast<Display>(selected - 1) : Display::Correlation;
    detail.view.setPhaseBandDisplay(mode);
}
void MixAnalysers::updatePinnedSpectrum(const SnapshotReader &reader, std::uint64_t generation, std::int64_t primary,
                                       const ViewStateStore *viewState) {
    if (page_ != Page::Spectrum || !SharedSpectrumView::supportsSpectrumOverlays(detail.mode()) ||
        !pinnedTracks_.getToggleState() || !viewState) {
        clearPinnedSpectrum();
        return;
    }
    const auto &pins = viewState->stackPins();
    const PinProjectionStamp current{viewState, reader.get(), generation, pins.revision, primary, look::themeId()};
    if (pinnedSpectrumStamp_ == current) return;
    // The primary trace counts toward the sixteen-curve Overview limit.
    std::array<SharedSpectrumView::SpectrumOverlayTrack, SharedSpectrumView::kSpectrumOverlayTracks> selected{};
    std::size_t count = 0;
    const auto &tracks = reader.tracks();
    if (!pins.ids.empty()) {
        for (std::size_t i = 0; i < tracks.size() && count < selected.size(); ++i) {
            const auto &track = *tracks[i];
            if (track.id == primary || !std::binary_search(pins.ids.begin(), pins.ids.end(), track.id)) continue;
            const auto colour = track.color && *track.color >= 0 ? look::paletteColour(*track.color) : look::trackColour(i);
            selected[count++] = {track.id, channelName(track.id, track.name), colour};
        }
    }
    detail.view.setSpectrumOverlayTracks(std::span{selected}.first(count),
                                         SharedSpectrumView::SpectrumOverlayProfile::Overview);
    pinnedSpectrumStamp_ = current;
    const bool hasPins = count != 0;
    if (hasPinnedSpectrum_ != hasPins) {
        hasPinnedSpectrum_ = hasPins;
        resized();
    }
}
void MixAnalysers::clearPinnedSpectrum() {
    if (!pinnedSpectrumStamp_) return;
    pinnedSpectrumStamp_.reset();
    detail.view.setSpectrumOverlayTracks({});
    if (hasPinnedSpectrum_) {
        hasPinnedSpectrum_ = false;
        resized();
    }
}
void MixAnalysers::frame(const std::shared_ptr<SharedAnalysis> &broker, const SnapshotReader &reader,
                         std::int64_t main, std::int64_t selected, std::uint64_t viewGeneration,
                         const ViewStateStore *viewState, std::int64_t heardEnd, std::optional<std::uint64_t> minimumRevision) {
    if (!isShowing() || suspended_ || getLocalBounds().isEmpty()) return;
    if (owner_.owner_before(broker) || broker.owner_before(owner_)) {
        clearPinnedSpectrum();
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
    updatePinnedSpectrum(reader, viewGeneration, track, viewState);
    if (page_ == Page::All) {
        for (auto &tile : tiles_) {
            tile.setSource(detail.point());
            tile.setChannel(detail.view.channel());
            tile.setPalette(detail.view.palette(), detail.view.paletteReversed());
            tile.setSpectrumSlope(detail.view.spectrumSlope());
            if (tile.mode() == Mode::Spectrum) tile.setSpectrumScale(detail.view.spectrumScale());
            if (tile.mode() == Mode::Spectrogram) {
                tile.setSpectrogramScale(detail.view.spectrogramScale());
                tile.setSpectrogramLevels(detail.view.spectrogramLevels());
                tile.setSpectrogramGamma(detail.view.spectrogramGamma());
            }
            tile.present(broker, track);
            tile.frame();
        }
    }
    if (table_->isVisible()) {
        if (tableDirty_) { table_->refresh(); tableDirty_ = false; }
        table_->frame(broker);
    }
    if (spectrogramDelta_->isVisible()) {
        spectrogramDelta_->frame(broker, track, detail.view.channel(), heardEnd, minimumRevision);
        return;
    }
    if (detail.isVisible()) detail.frame(broker, track, track);
    if (spectrumStatistics_->isShowing()) {
        spectrumStatistics_->refreshAppearance();
        spectrumStatistics_->present(detail.view.snapshot());
    }
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
    if (value) spectrumStatistics_->present({});
    if (value) spectrogramDelta_->stop();
}

void MixAnalysers::resized() {
    auto area = getLocalBounds().reduced(look::border);
    auto header = area.removeFromTop(look::fieldHeight);
    const bool showsTrace = page_ == Page::Spectrum;
    const bool showsComparison = page_ == Page::Spectrogram;
    const auto controls = showsTrace ? 4 : showsComparison ? 3 : 2;
    pageBox.setBounds(header.removeFromLeft(std::min(look::mixPageWidth, header.getWidth() / controls)));
    header.removeFromLeft(look::gap);
    if (showsTrace) {
        sourceBox.setBounds(header.removeFromLeft(std::min(look::mixChannelMaxWidth, header.getWidth() / 3)));
        header.removeFromLeft(look::gap);
        spectrumTraceBox.setBounds(header.removeFromLeft(std::min(look::mixChannelMaxWidth, header.getWidth() / 2)));
        header.removeFromLeft(look::gap);
        pinnedTracks_.setBounds(header.removeFromLeft(look::mixPageWidth));
    } else {
        const auto width = showsComparison ? std::min(look::mixChannelMaxWidth, header.getWidth() / 2) : look::mixChannelMaxWidth;
        sourceBox.setBounds(header.removeFromLeft(width));
    }
    header.removeFromLeft(look::gap);
    if (showsComparison) compareSpectrogram_.setBounds(header.removeFromLeft(std::min(look::mixPageWidth, header.getWidth())));
    const bool showPhaseBands = page_ == Page::Phase && header.getWidth() >= look::mixChannelMinWidth;
    phaseBandsBox_.setVisible(showPhaseBands);
    if (showPhaseBands) phaseBandsBox_.setBounds(header.removeFromLeft(look::mixChannelMinWidth));
    updatePhaseBandDisplay();
    area.removeFromTop(look::gap);
    const bool showStatistics = hasPinnedSpectrum_ && showsTrace &&
        area.getWidth() >= 2 * look::mixChannelMinWidth + look::gap && area.getHeight() >= 2 * look::mixChannelRowHeight;
    const bool showLevels = showStatistics && area.getWidth() >= 2 * look::mixChannelMaxWidth + look::gap &&
        area.getHeight() >= look::mixInfoHeight + 3 * look::mixChannelRowHeight;
    spectrumStatistics_->setVisible(showStatistics);
    spectrumStatistics_->setLevelRowsVisible(showLevels, detail.view.snapshot());
    if (showLevels) detail.view.setSpectrumLevelsEnabled(true);
    detail.view.setSpectrumTruePeakEnabled(spectrumStatistics_->truePeakRequested());
    if (!showLevels) detail.view.setSpectrumLevelsEnabled(false);
    detail.view.setStereoBalanceEnabled(showStatistics);
    if (showStatistics)
        spectrumStatistics_->setBounds(area.removeFromBottom((showLevels ? 3 : 1) * look::mixChannelRowHeight));
    if (page_ == Page::Analysis) { table_->setBounds(area); return; }
    if (page_ == Page::Compare) { compare_->setBounds(area); return; }
    if (spectrogramDelta_->isVisible()) { spectrogramDelta_->setBounds(area); return; }
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
