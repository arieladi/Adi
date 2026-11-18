// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::mix::limiter
{
class PositiveEnergyIndex
{
public:
    void prepare(std::size_t historySize)
    {
        if (historySize == 0 || historySize > 2304000)
            throw std::invalid_argument("Invalid positive energy history capacity");
        capacity = historySize;
        leafCount = (capacity + blockSize - 1) / blockSize;
        leafBase = 1;
        while (leafBase < leafCount) leafBase *= 2;
        nodes.assign(leafBase * 2, 0.0);
        reset();
    }

    void reset() noexcept
    {
        std::fill(nodes.begin(), nodes.end(), 0.0);
        building = {};
        activeLeaf = noActiveLeaf;
    }

    void append(std::size_t historyIndex, double energy) noexcept
    {
        assert(historyIndex < capacity);
        const auto leaf = historyIndex / blockSize;
        if (historyIndex % blockSize == 0)
        {
            building = {};
            activeLeaf = leaf;
        }
        assert(activeLeaf == leaf);
        building.add(energy);
        const auto leafEnd = std::min(capacity, (leaf + 1) * blockSize);
        if (historyIndex + 1 == leafEnd)
        {
            auto node = leafBase + leaf;
            nodes[node] = building.value();
            while (node > 1)
            {
                node /= 2;
                nodes[node] = nodes[node * 2] + nodes[node * 2 + 1];
            }
            activeLeaf = noActiveLeaf;
        }
    }

    double recent(std::span<const double> history, std::size_t writeIndex,
                  std::size_t length) const noexcept
    {
        assert(history.size() == capacity && writeIndex < capacity && length <= capacity);
        PositiveSum result;
        if (length == 0) return result.value();
        const auto begin = writeIndex >= length ? writeIndex - length : capacity - (length - writeIndex);
        const auto firstLength = std::min(length, capacity - begin);
        appendRange(history, begin, begin + firstLength, result);
        if (firstLength != length) appendRange(history, 0, length - firstLength, result);
        return result.value();
    }

private:
    struct PositiveSum
    {
        void add(double value) noexcept
        {
            const auto combined = sum + value;
            if (std::abs(sum) >= std::abs(value)) correction += (sum - combined) + value;
            else correction += (value - combined) + sum;
            sum = combined;
        }

        double value() const noexcept { return sum + correction; }
        double sum = 0.0;
        double correction = 0.0;
    };

    void appendTree(std::size_t first, std::size_t last, PositiveSum& result) const noexcept
    {
        first += leafBase;
        last += leafBase;
        while (first < last)
        {
            if ((first & 1) != 0) result.add(nodes[first++]);
            if ((last & 1) != 0) result.add(nodes[--last]);
            first /= 2;
            last /= 2;
        }
    }

    void appendLeaves(std::span<const double> history, std::size_t first,
                      std::size_t last, PositiveSum& result) const noexcept
    {
        if (activeLeaf >= first && activeLeaf < last)
        {
            appendTree(first, activeLeaf, result);
            const auto end = std::min(capacity, (activeLeaf + 1) * blockSize);
            for (auto index = activeLeaf * blockSize; index < end; ++index) result.add(history[index]);
            appendTree(activeLeaf + 1, last, result);
        }
        else appendTree(first, last, result);
    }

    void appendRange(std::span<const double> history, std::size_t begin,
                     std::size_t end, PositiveSum& result) const noexcept
    {
        if (begin % blockSize != 0)
        {
            const auto boundary = std::min(end, (begin / blockSize + 1) * blockSize);
            while (begin < boundary) result.add(history[begin++]);
        }
        const auto wholeEnd = end - end % blockSize;
        if (begin < wholeEnd)
        {
            appendLeaves(history, begin / blockSize, wholeEnd / blockSize, result);
            begin = wholeEnd;
        }
        while (begin < end) result.add(history[begin++]);
    }

    static constexpr std::size_t blockSize = 64;
    static constexpr std::size_t noActiveLeaf = std::numeric_limits<std::size_t>::max();
    std::vector<double> nodes;
    std::size_t capacity = 0, leafCount = 0, leafBase = 1;
    std::size_t activeLeaf = noActiveLeaf;
    PositiveSum building;
};
}
