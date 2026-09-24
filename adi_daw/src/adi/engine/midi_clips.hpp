// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <memory>
namespace adi::engine {
// Message-thread registry; its per-track note state is used ONLY by the audio
// consumer. Shared by successive immutable source generations, never copied
// while the consumer is running. Graph publication decides which one executes.
struct MidiClipState;
std::shared_ptr<MidiClipState> makeMidiClipState();
class MidiClips {
public:
    MidiClips(const rows::Model&, Transport&, double rate, std::shared_ptr<MidiClipState>);
    ~MidiClips();
    std::vector<Node*> sourcesFor(std::int64_t track);
    const std::vector<std::string>& problems() const noexcept;
    std::uint32_t droppedNotes() const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
