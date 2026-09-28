// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample slot: a decoded buffer handed from the message thread to a Pd
// external -- ADR-0192 d4, ADR-0127, ADR-0132.
//
// WHAT d4 ASKS FOR, and the one word in it that decides this file: the host
// decodes the file OFF THE AUDIO THREAD, and hands the buffer to the declared
// slot "through a lock-free handoff, NEVER AS PD MESSAGES". Pd messages are
// the obvious route and they are wrong twice over: a list of a million floats
// is a million dispatches, and it would arrive on whichever thread sent it.
//
// SO THE HANDOFF IS `SnapshotPublisher`, WHICH ALREADY EXISTS. It is the
// snapshot protocol ADR-0010 and ADR-0019 settled and `GraphHost` uses to swap
// a whole graph under a running engine: publish takes ownership and RETIRES the
// previous buffer rather than freeing it, the audio thread announces which
// snapshot it is on before dereferencing, and `collect()` frees only what is
// STRICTLY older than the announced one. Its header spends a page on why that
// strictness is the safety argument, and none of that reasoning is worth
// writing a second time in a second class that would then have to be kept in
// agreement with the first.
//
// A sample is exactly the shape that protocol was built for: large, immutable
// once built, replaced rarely, and read by one audio thread that only moves
// forward.
//
// WHAT IS KEPT BESIDE THE AUDIO, and why the hash rather than the path. d4 says
// device state keeps the media's BLAKE3 hash (ADR-0127), so a project still
// plays when the file has moved: the hash identifies the media wherever it
// turns up, and a path identifies only a place it used to be.

#pragma once

#include "juce/pd_declarations.hpp"
#include "adi/engine/publisher.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace adi::device {

/// One decoded sample, immutable once published.
struct PdSampleBuffer : engine::Sequenced {
    std::int32_t id = 0;
    std::int32_t channels = 1;
    std::int64_t frames = 0;
    double sampleRate = 0.0;
    /// The MEDIA's hash, not the decoded buffer's (ADR-0127): what identifies
    /// the file a project refers to, wherever it has been moved to.
    std::string blake3;
    /// Interleaved, `frames * channels`. Interleaved rather than planar
    /// because that is what a Pd external's perform routine walks.
    std::vector<float> interleaved;

    [[nodiscard]] const float* frame(std::int64_t f) const noexcept {
        if (f < 0 || f >= frames) return nullptr;
        return interleaved.data() + f * channels;
    }
};

/// The slots a patch declared, one `SnapshotPublisher` each.
///
/// Not copyable: each slot owns published and retired buffers.
class PdSampleSlots {
public:
    PdSampleSlots() = default;
    PdSampleSlots(const PdSampleSlots&) = delete;
    PdSampleSlots& operator=(const PdSampleSlots&) = delete;

    /// Message thread, with audio stopped. One slot per `[adi.sample]`, in
    /// declaration order. Declaring again replaces the set, which is what a
    /// `device.loadState` does (ADR-0177 fix 3).
    void declare(const std::vector<PdSampleDecl>& decls);

    [[nodiscard]] std::size_t count() const noexcept { return slots_.size(); }
    /// True when the patch declared this id.
    [[nodiscard]] bool has(std::int32_t id) const noexcept;

    /// Message thread. Hands `buffer` to the slot; the previous one is retired,
    /// not freed. False when the id was never declared.
    bool publish(std::int32_t id, std::unique_ptr<PdSampleBuffer> buffer);

    /// Message thread, on a timer. Frees retired buffers the audio thread has
    /// demonstrably moved past. Never blocks audio.
    std::size_t collect();

    /// **AUDIO THREAD**, once per block per slot.
    ///
    /// The returned buffer is valid until the NEXT call for this slot from the
    /// audio thread, which for a perform routine is the next block. That is
    /// `AudioRead`'s own contract: constructing one announces the snapshot in
    /// use, and `collect` frees only what is strictly older than the announced
    /// one, so a buffer stays alive for exactly as long as the block that
    /// announced it. Holding it longer is the one way to use this wrongly.
    [[nodiscard]] const PdSampleBuffer* forBlock(std::int32_t id) noexcept;

    /// How many buffers are retired and not yet collected, per slot summed.
    /// A number that only grows means `collect` is not being called.
    [[nodiscard]] std::size_t retained() const;

private:
    struct Slot {
        std::int32_t id = 0;
        std::string name;
        std::unique_ptr<engine::SnapshotPublisher<PdSampleBuffer>> pub;
    };
    std::vector<Slot> slots_;

    [[nodiscard]] Slot* find(std::int32_t id) noexcept;
    [[nodiscard]] const Slot* find(std::int32_t id) const noexcept;
};

/// Message thread. Decodes `source` into a buffer for slot `id`.
///
/// `playableWav` is the file to read: ADR-0132's cache turns an MP3, FLAC or
/// AIFF into one, off the audio thread, and the caller owns that cache. The
/// hash is taken from `source` and not from the decoded file, because ADR-0127
/// identifies the MEDIA a project refers to.
///
/// Null with `error` set when the file cannot be read.
[[nodiscard]] std::unique_ptr<PdSampleBuffer> pdDecodeSample(
    std::int32_t id, const std::filesystem::path& source,
    const std::filesystem::path& playableWav, std::string& error);

}  // namespace adi::device
