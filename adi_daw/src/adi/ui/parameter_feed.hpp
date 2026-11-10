// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/panel.hpp"
#include <map>
#include <memory>
#include <string>
namespace adi::ui {
struct DevicePanelData {
    std::int64_t id = 0, track = 0;
    std::string name;
    bool enabled = true;
    // Whether it plays notes as sound: a plug-in of the instrument subtype (`plugin_refs.subtype`).
    // A MIDI track holding one has an audio mixer, as Live's.
    bool instrument = false;
    // Whether it is a plug-in (a `plugin_refs` row), which has a window of its own (manual 23.3).
    bool plugin = false;
    panel::Resolved resolved;
    std::vector<panel::Declared> declared;
    std::vector<panel::Record> records;
    // Each parameter's default, normalised, as the plug-in declares it (`ParamDescriptor::
    // defaultValue`): where Live's Return to Default (manual 41.6) takes a control.
    std::map<std::string, double> defaults;
};
using ParameterPublication = std::vector<DevicePanelData>;
// One document-owned message-thread publication, shared by every window/surface.
class ParameterFeed {
  public:
    void publish(engine::Session &);
    std::shared_ptr<const ParameterPublication> current() const noexcept { return current_; }
    void touched(std::int64_t id, const std::string &param) {
        touched_[{id, param}] = panel::Source::Mouse;
    }

  private:
    std::map<std::pair<std::int64_t, std::string>, panel::Source> touched_;
    std::shared_ptr<const ParameterPublication> current_ = std::make_shared<ParameterPublication>();
};
} // namespace adi::ui
