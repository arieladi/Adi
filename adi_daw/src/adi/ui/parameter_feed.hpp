// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/session.hpp"
#include "adi/panel.hpp"
#include <map>
#include <memory>
#include <string>
namespace adi::ui {
struct ParameterGroupMetadata {
    std::string groupId, groupName;
    bool groupEnableControl = false;
    std::int32_t bandIndex = -1;
    std::string sectionId{}, sectionName{};
    bool operator==(const ParameterGroupMetadata &) const = default;
};
struct DevicePanelData {
    std::int64_t id = 0, track = 0;
    // The track's device chain holding it (`device_chains.id`): where a paste in front of it goes.
    std::int64_t chain = 0;
    // A monotonic instance stamp, never derived from a display name or memory address.
    // Reusing a numeric device row for a new runtime invalidates retained surface targets.
    std::uint64_t instance = 0;
    engine::DeviceRuntimeToken runtime;
    bool available = false;
    std::int32_t latency = 0;
    std::string name;
    bool enabled = true;
    // Whether it plays notes as sound: a plug-in of the instrument subtype (`plugin_refs.subtype`).
    // A MIDI track holding one has an audio mixer, as Live's.
    bool instrument = false;
    // Whether it is a plug-in (a `plugin_refs` row of a plug-in format, not ADI's own built-ins),
    // which has a window of its own (manual 23.3).
    bool plugin = false;
    // Its plug-in reference's uid ("" for none): a built-in's panel draws its own parts by it.
    std::string uid;
    panel::Resolved resolved;
    std::vector<panel::Declared> declared;
    std::vector<panel::Record> records;
    // Each parameter's default, normalised, as the plug-in declares it (`ParamDescriptor::
    // defaultValue`): where Live's Return to Default (manual 41.6) takes a control.
    std::map<std::string, double> defaults;
    // Keyed by stable parameter id, copied from the live descriptor, not inferred from names.
    std::map<std::string, ParameterGroupMetadata> parameterGroups{};
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
    struct Identity {
        engine::DeviceRuntimeToken runtime;
        std::optional<std::int64_t> pluginRef;
        std::string uid;
        std::uint64_t stamp = 0;
        std::uint64_t seen = 0;
    };
    std::map<std::int64_t, Identity> identities_;
    std::uint64_t nextIdentity_ = 0;
    std::uint64_t publication_ = 0;
    std::map<std::pair<std::int64_t, std::string>, panel::Source> touched_;
    std::shared_ptr<const ParameterPublication> current_ = std::make_shared<ParameterPublication>();
};
} // namespace adi::ui
