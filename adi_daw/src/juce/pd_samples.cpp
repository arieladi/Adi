// SPDX-License-Identifier: GPL-3.0-or-later
//
// See pd_samples.hpp for why the handoff is SnapshotPublisher and not a new
// class, and why the hash is the media's rather than the decoded file's.

#include "juce/pd_samples.hpp"

#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"

#include <system_error>

namespace adi::device {

void PdSampleSlots::declare(const std::vector<PdSampleDecl>& decls) {
    // Rebuilt rather than merged: the declared set changes only through a
    // `device.loadState` (ADR-0177 fix 3), and that op replaces the patch. A
    // slot the new patch does not declare should go, taking its buffer with it.
    slots_.clear();
    slots_.reserve(decls.size());
    for (const auto& d : decls) {
        Slot s;
        s.id = d.id;
        s.name = d.name;
        s.pub = std::make_unique<engine::SnapshotPublisher<PdSampleBuffer>>();
        slots_.push_back(std::move(s));
    }
}

PdSampleSlots::Slot* PdSampleSlots::find(std::int32_t id) noexcept {
    for (auto& s : slots_) if (s.id == id) return &s;
    return nullptr;
}
const PdSampleSlots::Slot* PdSampleSlots::find(std::int32_t id) const noexcept {
    for (const auto& s : slots_) if (s.id == id) return &s;
    return nullptr;
}

bool PdSampleSlots::has(std::int32_t id) const noexcept { return find(id) != nullptr; }

bool PdSampleSlots::publish(std::int32_t id, std::unique_ptr<PdSampleBuffer> buffer) {
    Slot* const s = find(id);
    if (s == nullptr || buffer == nullptr) return false;
    buffer->id = id;
    s->pub->publish(std::move(buffer));
    return true;
}

std::size_t PdSampleSlots::collect() {
    std::size_t freed = 0;
    for (auto& s : slots_) freed += s.pub->collect();
    return freed;
}

const PdSampleBuffer* PdSampleSlots::forBlock(std::int32_t id) noexcept {
    Slot* const s = find(id);
    if (s == nullptr) return nullptr;
    // The AudioRead is deliberately temporary. Constructing it does the two
    // atomic operations in the order that matters -- acquire the current
    // snapshot, then announce it -- and announcing is what keeps this buffer
    // alive past the constructor. See publisher.hpp's page on why `collect`
    // frees only what is STRICTLY older than the announced sequence.
    engine::SnapshotPublisher<PdSampleBuffer>::AudioRead read(*s->pub);
    return read.get();
}

std::size_t PdSampleSlots::retained() const {
    std::size_t n = 0;
    for (const auto& s : slots_) n += s.pub->retainedCount();
    return n;
}

std::unique_ptr<PdSampleBuffer> pdDecodeSample(
    std::int32_t id, const std::filesystem::path& source,
    const std::filesystem::path& playableWav, std::string& error) {
    error.clear();
    try {
        audio::WavReader reader(playableWav);
        auto buffer = std::make_unique<PdSampleBuffer>();
        buffer->id = id;
        buffer->channels = static_cast<std::int32_t>(reader.channels());
        buffer->frames = static_cast<std::int64_t>(reader.frames());
        buffer->sampleRate = static_cast<double>(reader.sampleRate());
        if (buffer->channels <= 0 || buffer->frames < 0) {
            error = "the decoded file declares no channels";
            return nullptr;
        }
        buffer->interleaved.assign(
            static_cast<std::size_t>(buffer->frames) * static_cast<std::size_t>(buffer->channels),
            0.f);

        // In one read: this is the message thread, the file is already a WAV,
        // and a partial read would leave a buffer that plays silence after some
        // point with nothing to say where.
        std::uint64_t done = 0;
        while (done < static_cast<std::uint64_t>(buffer->frames)) {
            const std::uint64_t left = static_cast<std::uint64_t>(buffer->frames) - done;
            const std::uint32_t want = left > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                            : static_cast<std::uint32_t>(left);
            const std::uint32_t got = reader.read(
                buffer->interleaved.data() + done * static_cast<std::uint64_t>(buffer->channels),
                want);
            if (got == 0) break;
            done += got;
        }
        if (done != static_cast<std::uint64_t>(buffer->frames)) {
            error = "the decoded file ended early: " + std::to_string(done) + " of " +
                    std::to_string(buffer->frames) + " frames";
            return nullptr;
        }

        // The MEDIA's hash, from the source the project names -- not from the
        // cache's decoded copy, which is ours and disposable (ADR-0127,
        // ADR-0132 d3).
        const auto hash = media::blake3File(source);
        if (hash) buffer->blake3 = hash.hex;
        // A file that hashes badly still plays. The hash is how a project finds
        // the media again later, and losing it is worse than silent but not as
        // bad as refusing to load what the user just dropped in.
        return buffer;
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace adi::device
