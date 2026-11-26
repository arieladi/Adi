// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/controller_data.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace adi::engine { struct ClipNode; }
namespace adi::ui {
class ClipViewPanel;

// One MIDI clip controller envelope, read from the published clip snapshot.
// A click or drag owns an uncommitted breakpoint until release.
class ClipControllerEditor final : public juce::Component {
public:
    explicit ClipControllerEditor(ClipViewPanel&);
    void setClip(std::shared_ptr<const engine::ClipNode>);
    bool chooseLane(std::int64_t laneId);
    bool chooseController(int channel, int controller);
    [[nodiscard]] const controllers::Lane* lane() const noexcept;
    [[nodiscard]] juce::Rectangle<int> plotArea() const noexcept;
    [[nodiscard]] juce::Point<float> pointPosition(const controllers::Point&) const noexcept;
    bool clearEnvelope();
    void cancelGesture();

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
private:
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> window() const noexcept;
    [[nodiscard]] const controllers::Point* pointAt(juce::Point<float>) const;
    [[nodiscard]] controllers::Point at(controllers::Point, const juce::MouseEvent&) const;
    bool deletePoints(std::span<const std::uint64_t>);
    void openControllerMenu();
    void updateChooserSelection();
    void rebuildPoints();

    ClipViewPanel& panel_;
    juce::ComboBox chooser_;
    juce::TextButton clear_{"Clear Envelope"};
    std::shared_ptr<const engine::ClipNode> clip_;
    int channel_ = 0, controller_ = 1;
    std::uint64_t selected_ = 0;
    std::vector<const controllers::Point*> points_;
    std::optional<controllers::Point> gesture_;
    std::optional<std::int64_t> gestureHead_;
    juce::Point<float> pressed_;
};

} // namespace adi::ui
