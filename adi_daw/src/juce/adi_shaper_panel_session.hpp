// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "juce/adi_shaper_panel.hpp"
#include "adi/engine/session_runtime.hpp"
#include <map>
#include <memory>
#include <string>
namespace adi::device { class DeviceInstance; }
namespace adi::ui {
class ProjectDocument;

// The panel resolves the stamped instance on each write. A gesture previews
// directly on that instance, then journals the changed controls in one batch.
class AdiShaperPanelSession {
public:
    AdiShaperPanelSession(ProjectDocument&, std::int64_t device);
    ~AdiShaperPanelSession();
    AdiShaperPanelSession(const AdiShaperPanelSession&) = delete;
    AdiShaperPanelSession& operator=(const AdiShaperPanelSession&) = delete;
    AdiShaperPanel::Binding binding();
    bool applyPreset(std::span<const std::uint8_t>);
    void refresh();
    bool valid() const { return instance() != nullptr; }
    bool editing() const noexcept { return editing_; }
    const std::string& error() const noexcept { return error_; }
private:
    using Change = AdiShaperPanel::Change;
    using Phase = AdiShaperPanel::Phase;
    using Point = AdiShaperPanel::Point;
    using Key = AdiShaperPanel::Key;
    struct Touched { double before, after; };
    device::DeviceInstance* instance() const;
    bool parameters(Phase, std::span<const Change>);
    bool editWave(Key, Phase, std::span<const Point>);
    std::vector<Point> wave(Key) const;
    void cancel();
    ProjectDocument& document_;
    std::int64_t id_;
    engine::DeviceRuntimeToken token_;
    std::map<int, double> defaults_, values_, storedAtBegin_;
    std::map<int, Touched> touched_;
    std::string error_;
    std::uint64_t stamp_ = 0;
    mutable bool retired_ = false;
    bool editing_ = false;
};
} // namespace adi::ui
