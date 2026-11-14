// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlPrimitives.h"
#include "UpdatingConvolver.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace adi::mix::limiter
{
class KernelStage
{
public:
    void prepare(double rate, std::size_t capacity, std::size_t count, int shape, const std::array<float, 3>& properties)
    {
        kernel.resize(capacity);
        convolver.prepare(rate, capacity);
        const auto values = std::span<float>(kernel).first(count);
        if (!smoothingKernel(values, static_cast<KernelFamily>(shape), properties[0], properties[1], properties[2])
            || !convolver.setKernel(values))
            throw std::invalid_argument("Cannot initialize smoothing stage");
        lastCount = count;
        lastShape = shape;
        lastProperties = properties;
        served.store(0);
        generation.store(0);
    }

    void reset() noexcept { convolver.reset(); }
    float process(float input) noexcept { return convolver.process(input); }

    void request(std::size_t count, int shape, const std::array<float, 3>& properties) noexcept
    {
        if (lastCount == count && lastShape == shape && lastProperties == properties)
            return;
        if (count == 0 || count > kernel.size())
            return;
        generation.fetch_add(1);
        requestedCount.store(count);
        requestedShape.store(shape);
        for (std::size_t index = 0; index < properties.size(); ++index)
            requestedProperties[index].store(properties[index]);
        generation.fetch_add(1);
        lastCount = count;
        lastShape = shape;
        lastProperties = properties;
    }

    bool service() noexcept
    {
        const auto version = generation.load();
        if ((version & 1) != 0 || version == served.load() || convolver.hasPendingKernel()) return false;
        const auto count = requestedCount.load();
        const auto shape = requestedShape.load();
        std::array<float, 3> properties;
        for (std::size_t index = 0; index < properties.size(); ++index)
            properties[index] = requestedProperties[index].load();
        if (generation.load() != version) return false;
        if (count == 0 || count > kernel.size()) return false;
        const auto values = std::span<float>(kernel).first(count);
        if (!smoothingKernel(values, static_cast<KernelFamily>(shape), properties[0], properties[1], properties[2]))
            return false;
        if (!convolver.setKernel(values)) return false;
        served.store(version);
        return true;
    }

    bool needsService() const noexcept
    {
        const auto version = generation.load();
        return (version & 1) == 0 && version != served.load() && !convolver.hasPendingKernel();
    }

private:
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    UpdatingConvolver<> convolver;
    std::vector<float> kernel;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::size_t> requestedCount{0};
    std::atomic<int> requestedShape{0};
    std::array<std::atomic<float>, 3> requestedProperties{};
    std::atomic<std::uint64_t> served{0};
    std::size_t lastCount = 0;
    int lastShape = 0;
    std::array<float, 3> lastProperties{};
};
}
