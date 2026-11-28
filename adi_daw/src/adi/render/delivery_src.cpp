// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/delivery_src.hpp"

#include <stdexcept>
#if defined(ADI_HAVE_SOXR)
#include <soxr.h>
#include <algorithm>
#include <utility>
#include <vector>
#endif

namespace adi::render {

#if defined(ADI_HAVE_SOXR)
namespace {
struct DeleteSoxr {
    void operator()(soxr* resampler) const noexcept { soxr_delete(resampler); }
};
constexpr std::size_t blockFrames = 4096;
}

struct DeliverySrc::Impl {
    std::unique_ptr<soxr, DeleteSoxr> resampler;
    unsigned channels;
    std::uint64_t requested, generated = 0;
    Writer write;
    std::vector<double> output;

    Impl(unsigned sourceRate, unsigned outputRate, unsigned channelsIn, std::uint64_t frames, Writer writer)
        : channels(channelsIn), requested(frames), write(std::move(writer)), output(blockFrames * channels) {
        if (outputRate == 0 || sourceRate <= outputRate || channels == 0 || frames == 0 || !write)
            throw std::invalid_argument("Invalid delivery sample rate conversion");
        const auto io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
        const auto quality = soxr_quality_spec(SOXR_VHQ, SOXR_DOUBLE_PRECISION);
        const auto runtime = soxr_runtime_spec(1);
        soxr_error_t error = nullptr;
        resampler.reset(soxr_create(sourceRate, outputRate, channels, &error, &io, &quality, &runtime));
        if (error || !resampler) throw std::runtime_error(soxr_strerror(error));
    }

    void deliver(std::size_t frames) {
        const auto remaining = requested - std::min(requested, generated);
        const auto send = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, remaining));
        if (send != 0) write(std::span<const double>(output.data(), static_cast<std::size_t>(send) * channels), send);
        generated += frames;
    }
};

bool deliverySrcAvailable() noexcept { return true; }

DeliverySrc::DeliverySrc(unsigned sourceRate, unsigned outputRate, unsigned channels,
                         std::uint64_t frames, Writer writer)
    : impl_(std::make_unique<Impl>(sourceRate, outputRate, channels, frames, std::move(writer))) {}
DeliverySrc::~DeliverySrc() = default;

void DeliverySrc::append(std::span<const double> source, std::uint32_t frames) {
    auto& s = *impl_;
    if (source.size() != static_cast<std::size_t>(frames) * s.channels)
        throw std::invalid_argument("Invalid SRC capture block");
    for (std::size_t first = 0; first < frames;) {
        std::size_t consumed = 0, produced = 0;
        const auto failure = soxr_process(s.resampler.get(), source.data() + first * s.channels,
                                          frames - first, &consumed, s.output.data(), blockFrames, &produced);
        if (failure) throw std::runtime_error(failure);
        if (consumed == 0 && produced == 0) throw std::runtime_error("Delivery SRC did not advance");
        s.deliver(produced);
        first += consumed;
    }
}

void DeliverySrc::finish(const std::function<void()>& checkpoint, std::uint64_t frames) {
    auto& s = *impl_;
    if (frames != 0) {
        if (frames > s.requested || frames < std::min(s.requested, s.generated))
            throw std::runtime_error("Delivery SRC cannot change the completed range");
        s.requested = frames;
    }
    for (;;) {
        if (checkpoint) checkpoint();
        std::size_t produced = 0;
        const auto failure = soxr_process(s.resampler.get(), nullptr, 0, nullptr,
                                          s.output.data(), blockFrames, &produced);
        if (failure) throw std::runtime_error(failure);
        if (produced == 0) break;
        s.deliver(produced);
    }
    // Rounding the input span up can produce one extra destination frame.
    if (s.generated < s.requested || s.generated - s.requested > 1)
        throw std::runtime_error("Delivery SRC produced an unexpected frame count");
}

#else

struct DeliverySrc::Impl {};
bool deliverySrcAvailable() noexcept { return false; }
DeliverySrc::DeliverySrc(unsigned, unsigned, unsigned, std::uint64_t, Writer) {
    throw std::runtime_error("Project-rate downsampling requires the pinned SoXR build");
}
DeliverySrc::~DeliverySrc() = default;
void DeliverySrc::append(std::span<const double>, std::uint32_t) {
    throw std::runtime_error("Project-rate downsampling requires the pinned SoXR build");
}
void DeliverySrc::finish(const std::function<void()>&, std::uint64_t) {
    throw std::runtime_error("Project-rate downsampling requires the pinned SoXR build");
}

#endif
} // namespace adi::render
