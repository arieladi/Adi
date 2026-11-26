// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/blob.hpp"
#include "adi/interchange/smf.hpp"
#include "adi/ops.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace adi { class Store; }
namespace adi::clip_edit { class CopyIds; }

namespace adi::interchange {

// Owner-side mapping between SMF tracks and the native note/controller streams.
struct MidiFileTrack {
    std::int64_t length = 0;
    std::vector<NoteRecord> notes;
    std::map<std::pair<int, int>, std::vector<AutomationPoint>> controllers;
};
struct MidiFileTracks {
    std::vector<MidiFileTrack> tracks;
    std::string error;
};

[[nodiscard]] MidiFileTracks readMidiClipFile(const std::filesystem::path&);
[[nodiscard]] OpRequest createImportedMidiClip(clip_edit::CopyIds&, const MidiFileTrack&, std::int64_t track,
    std::int64_t clip, std::int64_t position, const std::string& name);
[[nodiscard]] smf::Bytes encodeMidiClipFile(const Store&, std::int64_t clip, std::string& error);

} // namespace adi::interchange
