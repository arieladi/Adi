// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_clip_controller.hpp"
#include "ui_clipview.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/curves.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace adi::ui {
namespace {
constexpr int kChooseController = std::numeric_limits<int>::max();
constexpr int kChoicesPerChannel = 129;

bool canEdit(const controllers::Lane& lane) noexcept {
    return lane.id > 0 && lane.error.empty() && lane.channel && *lane.channel >= 0 && *lane.channel < 16 &&
           lane.controller && controllers::validController(*lane.controller);
}

juce::String controllerName(int cc) {
    if (cc == controllers::kPitchBend)
        return "Pitch Bend";
    const auto name = cc == 1 ? "Mod Wheel" : cc == 7 ? "Volume" : cc == 10 ? "Pan" :
                      cc == 11 ? "Expression" : cc == 64 ? "Sustain" : "Controller";
    return juce::String(name) + " (CC " + juce::String(cc) + ")";
}

juce::String laneName(int channel, int cc) {
    return controllerName(cc) + ", Ch " + juce::String(channel + 1);
}
} // namespace

ClipControllerEditor::ClipControllerEditor(ClipViewPanel& panel) : panel_(panel) {
    setTitle("MIDI Clip Envelopes");
    setWantsKeyboardFocus(true);
    setOpaque(true);
    addAndMakeVisible(chooser_);
    addAndMakeVisible(clear_);
    chooser_.setTitle("MIDI Controller Envelope");
    chooser_.setTextWhenNothingSelected("No MIDI controller envelopes");
    chooser_.onChange = [this] {
        const auto id = chooser_.getSelectedId();
        if (id == kChooseController) {
            updateChooserSelection();
            openControllerMenu();
            return;
        }
        const auto index = id - 1;
        if (clip_ && index >= 0 && static_cast<std::size_t>(index) < clip_->controllers.size())
            chooseLane(clip_->controllers[static_cast<std::size_t>(index)].id);
    };
    clear_.onClick = [this] { clearEnvelope(); };
    chooser_.setEnabled(false);
    clear_.setEnabled(false);
}

void ClipControllerEditor::setClip(std::shared_ptr<const engine::ClipNode> clip) {
    if (clip == clip_)
        return;
    const bool changed = !clip || !clip_ || clip->id != clip_->id;
    const auto selected = changed ? 0 : selected_;
    cancelGesture();
    points_.clear();
    clip_ = std::move(clip);
    chooser_.clear(juce::dontSendNotification);
    if (changed) {
        channel_ = 0;
        controller_ = 1;
    }
    selected_ = 0;
    chooser_.setEnabled(clip_ != nullptr);
    if (clip_) {
        const controllers::Lane* first = nullptr;
        for (std::size_t index = 0; index < clip_->controllers.size(); ++index) {
            const auto& entry = clip_->controllers[index];
            if (!canEdit(entry))
                continue;
            chooser_.addItem(laneName(static_cast<int>(*entry.channel), static_cast<int>(*entry.controller)),
                             static_cast<int>(index) + 1);
            if (!first)
                first = &entry;
        }
        if (first) {
            chooser_.addSeparator();
            if (changed) {
                channel_ = static_cast<int>(*first->channel);
                controller_ = static_cast<int>(*first->controller);
            }
        }
        chooser_.addItem("Choose a MIDI Controller...", kChooseController);
        updateChooserSelection();
        rebuildPoints();
    }
    if (selected && std::any_of(points_.begin(), points_.end(),
                                [selected](const auto* p) { return p->id == selected; }))
        selected_ = selected;
    clear_.setEnabled(!points_.empty());
    repaint();
}

bool ClipControllerEditor::chooseLane(std::int64_t laneId) {
    if (!clip_)
        return false;
    const auto found = std::find_if(clip_->controllers.begin(), clip_->controllers.end(),
                                    [laneId](const auto& entry) { return entry.id == laneId && canEdit(entry); });
    if (found == clip_->controllers.end())
        return false;
    return chooseController(static_cast<int>(*found->channel), static_cast<int>(*found->controller));
}

bool ClipControllerEditor::chooseController(int channel, int controller) {
    if (!clip_ || channel < 0 || channel >= 16 || !controllers::validController(controller))
        return false;
    cancelGesture();
    channel_ = channel;
    controller_ = controller;
    selected_ = 0;
    updateChooserSelection();
    rebuildPoints();
    clear_.setEnabled(!points_.empty());
    repaint();
    return true;
}

void ClipControllerEditor::openControllerMenu() {
    if (!clip_)
        return;
    juce::PopupMenu channels;
    for (int channel = 0; channel < 16; ++channel) {
        juce::PopupMenu ranges;
        const auto firstId = 1 + kChoicesPerChannel * channel;
        ranges.addItem(firstId + 128, controllerName(controllers::kPitchBend), true,
                       channel == channel_ && controller_ == controllers::kPitchBend);
        for (int first = 0; first < 128; first += 16) {
            juce::PopupMenu ccs;
            for (int cc = first; cc < first + 16; ++cc)
                ccs.addItem(firstId + cc, controllerName(cc), true,
                            channel == channel_ && cc == controller_);
            ranges.addSubMenu("CC " + juce::String(first) + "-" + juce::String(first + 15), ccs);
        }
        channels.addSubMenu("Channel " + juce::String(channel + 1), ranges);
    }
    const auto clip = clip_->id;
    const juce::Component::SafePointer<ClipControllerEditor> safe(this);
    channels.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(chooser_), [safe, clip](int choice) {
        if (choice > 0 && safe != nullptr && safe->clip_ && safe->clip_->id == clip) {
            const auto controller = (choice - 1) % kChoicesPerChannel;
            safe->chooseController((choice - 1) / kChoicesPerChannel,
                                   controller == 128 ? controllers::kPitchBend : controller);
        }
    });
}

void ClipControllerEditor::updateChooserSelection() {
    if (const auto* selected = lane())
        chooser_.setSelectedId(static_cast<int>(selected - clip_->controllers.data()) + 1, juce::dontSendNotification);
    else if (clip_)
        chooser_.setText(laneName(channel_, controller_), juce::dontSendNotification);
}

const controllers::Lane* ClipControllerEditor::lane() const noexcept {
    if (!clip_)
        return nullptr;
    const auto found = std::find_if(clip_->controllers.begin(), clip_->controllers.end(),
        [this](const auto& entry) {
            return entry.channel == channel_ && entry.controller == controller_ && canEdit(entry);
        });
    return found == clip_->controllers.end() ? nullptr : &*found;
}

void ClipControllerEditor::rebuildPoints() {
    points_.clear();
    if (const auto* selectedLane = lane()) {
        points_.reserve(selectedLane->points.size());
        for (const auto& point : selectedLane->points)
            points_.push_back(&point);
        std::stable_sort(points_.begin(), points_.end(),
                         [](const auto* a, const auto* b) { return a->timeTicks < b->timeTicks; });
    }
}

std::pair<std::int64_t, std::int64_t> ClipControllerEditor::window() const noexcept {
    constexpr auto limit = std::int64_t{1} << 50;
    const auto* clip = panel_.details();
    if (clip == nullptr)
        return {0, textproj::kPPQ};
    const auto first = std::clamp(clip->loopEnabled ? clip->loopStartTicks : clip->startMarkerTicks, -limit, limit - 1);
    const auto last = clip->loopEnabled && clip->loopLengthTicks && *clip->loopLengthTicks > 0
                          ? first + std::min(*clip->loopLengthTicks, limit - first)
                          : clip->endMarkerTicks.value_or(first + std::clamp<std::int64_t>(
                                clip->lengthTicks.value_or(textproj::kPPQ), 1, limit - first));
    return {first, std::clamp(last, first + 1, limit)};
}

juce::Rectangle<int> ClipControllerEditor::plotArea() const noexcept {
    const int labels = controller_ == controllers::kPitchBend ? 44 : 32;
    return getLocalBounds().withTrimmedTop(34).withTrimmedLeft(labels).withTrimmedRight(9).withTrimmedBottom(12);
}

juce::Point<float> ClipControllerEditor::pointPosition(const controllers::Point& point) const noexcept {
    const auto plot = plotArea();
    const auto [first, last] = window();
    const auto amount = (static_cast<double>(point.timeTicks) - static_cast<double>(first)) /
                        static_cast<double>(last - first);
    return {static_cast<float>(plot.getX()) + static_cast<float>(amount * std::max(1, plot.getWidth() - 1)),
            static_cast<float>(plot.getY()) + static_cast<float>((1.0 - point.value) * std::max(1, plot.getHeight() - 1))};
}

const controllers::Point* ClipControllerEditor::pointAt(juce::Point<float> at) const {
    const auto [first, last] = window();
    const controllers::Point* nearest = nullptr;
    float radius = 7.f;
    for (const auto* point : points_) {
        if (point->timeTicks < first || point->timeTicks >= last)
            continue;
        const auto distance = pointPosition(*point).getDistanceFrom(at);
        if (distance <= radius) {
            radius = distance;
            nearest = point;
        }
    }
    return nearest;
}

controllers::Point ClipControllerEditor::at(controllers::Point point, const juce::MouseEvent& event) const {
    const auto plot = plotArea();
    const auto [first, last] = window();
    const auto x = std::clamp(static_cast<double>(event.position.x - static_cast<float>(plot.getX())) /
                                 std::max(1, plot.getWidth() - 1), 0., 1.);
    const auto y = std::clamp(static_cast<double>(event.position.y - static_cast<float>(plot.getY())) /
                                 std::max(1, plot.getHeight() - 1), 0., 1.);
    std::int64_t tick = first + static_cast<std::int64_t>(std::llround(x * static_cast<double>(last - first)));
    if (panel_.notes.time.grid.snap && !event.mods.isAltDown()) {
        const auto step = std::max<std::int64_t>(1, panel_.notes.gridTicks());
        tick = first + static_cast<std::int64_t>(std::llround(static_cast<double>(tick - first) / static_cast<double>(step))) * step;
    }
    point.timeTicks = std::clamp(tick, first, last - 1);
    if (controller_ == controllers::kPitchBend) {
        const auto center = static_cast<float>(plot.getY()) + 0.5f * static_cast<float>(plot.getHeight() - 1);
        point.value = std::abs(event.position.y - center) <= 1.f ? .5 : 1. - y;
    } else {
        point.value = static_cast<double>(std::lround((1. - y) * 127.)) / 127.;
    }
    return point;
}

void ClipControllerEditor::resized() {
    auto controls = getLocalBounds().reduced(8, 6).removeFromTop(21);
    clear_.setBounds(controls.removeFromRight(std::min(112, controls.getWidth() / 2)));
    controls.removeFromRight(6);
    chooser_.setBounds(controls.removeFromLeft(std::min(250, controls.getWidth())));
}

void ClipControllerEditor::paint(juce::Graphics& g) {
    g.fillAll(look::panel);
    const auto plot = plotArea();
    if (plot.isEmpty())
        return;
    g.setColour(look::lane);
    g.fillRect(plot);
    g.setFont(look::font(look::fontSmall));
    const bool pitch = controller_ == controllers::kPitchBend;
    const auto displayValue = [pitch](double value) {
        return pitch ? static_cast<int>(std::lround(controllers::pitchBendWordExact(value))) - 8192
                     : static_cast<int>(std::lround(value * 127.));
    };
    const std::array<double, 5> scale = pitch ? std::array{1., .75, .5, .25, 0.}
        : std::array{1., 96. / 127., 64. / 127., 32. / 127., 0.};
    for (const auto value : scale) {
        const auto y = static_cast<int>(std::lround(pointPosition({0, 0, value}).y));
        g.setColour(look::pianoLine);
        g.fillRect(plot.getX(), y, plot.getWidth(), 1);
        g.setColour(look::text);
        g.drawText(juce::String(displayValue(value)), 0, y - 7, plot.getX() - 5, 14, juce::Justification::centredRight);
    }
    if (!clip_) {
        g.setColour(look::text);
        g.drawText("Select a MIDI clip to edit its envelopes.", plot, juce::Justification::centred);
        return;
    }
    if (points_.empty() && !gesture_) {
        g.setColour(look::text);
        g.drawText("Click to draw the first breakpoint.", plot, juce::Justification::centred);
        return;
    }

    juce::Graphics::ScopedSaveState clipped(g);
    g.reduceClipRegion(plot);
    if (!points_.empty()) {
        g.setColour(look::accent);
        const auto [first, last] = window();
        const auto pointWithValue = [&](std::int64_t tick, double value) {
            controllers::Point point{};
            point.timeTicks = tick;
            point.value = value;
            return pointPosition(point);
        };
        const controllers::Point* previous = nullptr;
        for (const auto* point : points_) {
            if (previous && point->timeTicks >= first && previous->timeTicks < last) {
                if (previous->timeTicks == point->timeTicks) {
                    if (point->timeTicks < last)
                        g.drawLine(juce::Line<float>(pointPosition(*previous), pointPosition(*point)), 1.5f);
                } else {
                    const auto from = std::max(previous->timeTicks, first);
                    const auto to = std::min(point->timeTicks, last);
                    const auto width = static_cast<double>(point->timeTicks) - static_cast<double>(previous->timeTicks);
                    const auto shape = [&](std::int64_t tick) {
                        const auto x = (static_cast<double>(tick) - static_cast<double>(previous->timeTicks)) / width;
                        return pointWithValue(tick, engine::curveValue(previous->value, point->value, x,
                                                                        previous->curve, previous->tension));
                    };
                    auto lastPoint = shape(from);
                    if (previous->curve == static_cast<std::uint8_t>(Curve::Hold)) {
                        const auto edge = pointWithValue(to, previous->value);
                        g.drawLine(juce::Line<float>(lastPoint, edge), 1.5f);
                        if (to == point->timeTicks)
                            g.drawLine(juce::Line<float>(edge, pointPosition(*point)), 1.5f);
                    } else {
                        const auto segmentPixels = shape(to).x - lastPoint.x;
                        const auto samples = std::clamp(static_cast<int>(segmentPixels / 5.f) + 1, 1, 24);
                        for (int i = 1; i <= samples; ++i) {
                            const auto current = shape(from + (to - from) * i / samples);
                            g.drawLine(juce::Line<float>(lastPoint, current), 1.5f);
                            lastPoint = current;
                        }
                    }
                }
            }
            previous = point;
        }
        if (previous && previous->timeTicks < last) {
            const auto begin = pointWithValue(std::max(first, previous->timeTicks), previous->value);
            g.drawLine(juce::Line<float>(begin, pointWithValue(last, previous->value)), 1.5f);
        }
        for (const auto* point : points_) {
            if (point->timeTicks < first || point->timeTicks >= last || (gesture_ && gesture_->id == point->id))
                continue;
            const auto p = pointPosition(*point);
            g.setColour(point->id == selected_ ? look::textBright : look::accent);
            g.fillEllipse(p.x - 3.f, p.y - 3.f, 6.f, 6.f);
        }
    }
    if (gesture_) {
        const auto p = pointPosition(*gesture_);
        g.setColour(look::textBright);
        g.fillEllipse(p.x - 4.f, p.y - 4.f, 8.f, 8.f);
    }
    if (const auto value = gesture_ ? &*gesture_ : (selected_ == 0 ? nullptr : [&]() -> const controllers::Point* {
            const auto it = std::find_if(points_.begin(), points_.end(),
                                         [this](const auto* p) { return p->id == selected_; });
            return it == points_.end() ? nullptr : *it;
        }())) {
        g.setColour(look::textBright);
        g.drawText(juce::String(displayValue(value->value)),
                   plot.reduced(6).removeFromTop(16), juce::Justification::topRight);
    }
}

void ClipControllerEditor::mouseDown(const juce::MouseEvent& e) {
    if (!clip_ || !plotArea().contains(e.getPosition()))
        return;
    focusIfShowing(*this);
    cancelGesture();
    pressed_ = e.position;
    const auto* point = pointAt(pressed_);
    selected_ = point ? point->id : 0;
    if (e.mods.isPopupMenu()) {
        if (!selected_)
            return;
        const auto id = selected_;
        const auto clip = clip_->id;
        const auto clickedLane = lane()->id;
        juce::PopupMenu menu;
        menu.addItem(1, "Delete Breakpoint");
        const juce::Component::SafePointer<ClipControllerEditor> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [safe, id, clip, clickedLane](int choice) {
            if (choice == 1 && safe != nullptr && safe->clip_ && safe->clip_->id == clip &&
                safe->lane() && safe->lane()->id == clickedLane) {
                const std::array ids{id};
                safe->deletePoints(ids);
            }
        });
        return;
    }
    gestureHead_ = OpJournal(panel_.root.editStore()).headSeq();
    gesture_ = point ? *point : at({}, e);
    repaint();
}
void ClipControllerEditor::mouseDrag(const juce::MouseEvent& e) {
    if (gesture_) {
        gesture_ = at(*gesture_, e);
        repaint();
    }
}
void ClipControllerEditor::mouseUp(const juce::MouseEvent& e) {
    if (!gesture_ || !clip_)
        return;
    const bool moved = e.position != pressed_;
    auto point = moved ? at(*gesture_, e) : *gesture_;
    const auto head = gestureHead_;
    cancelGesture();
    if (point.id && !moved)
        return;
    if (point.id == 0) {
        const auto atTick = std::find_if(points_.rbegin(), points_.rend(),
                                        [&point](const auto* p) { return p->timeTicks == point.timeTicks; });
        if (atTick != points_.rend()) {
            const auto value = point.value;
            point = **atTick;
            if (point.value == value)
                return;
            point.value = value;
        }
    }
    const auto* selectedLane = lane();
    panel_.root.writeControllerPoint({selectedLane ? selectedLane->id : 0, clip_->id, channel_, controller_},
                                    point, head);
}
void ClipControllerEditor::cancelGesture() {
    if (gesture_) {
        gesture_.reset();
        repaint();
    }
}
bool ClipControllerEditor::deletePoints(std::span<const std::uint64_t> ids) {
    const auto* selectedLane = lane();
    if (!selectedLane || ids.empty())
        return false;
    cancelGesture();
    selected_ = 0;
    return panel_.root.deleteControllerPoints(*selectedLane, ids);
}
bool ClipControllerEditor::clearEnvelope() {
    std::vector<std::uint64_t> ids;
    ids.reserve(points_.size());
    for (const auto* point : points_)
        ids.push_back(point->id);
    return deletePoints(ids);
}
bool ClipControllerEditor::keyPressed(const juce::KeyPress& key) {
    if (key.getKeyCode() == juce::KeyPress::escapeKey && gesture_) {
        cancelGesture();
        return true;
    }
    if (key.getKeyCode() != juce::KeyPress::deleteKey && key.getKeyCode() != juce::KeyPress::backspaceKey)
        return false;
    if (selected_) {
        const std::array ids{selected_};
        deletePoints(ids);
    }
    return true;
}

} // namespace adi::ui
