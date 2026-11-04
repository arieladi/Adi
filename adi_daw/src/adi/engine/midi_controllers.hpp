// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/store_rows.hpp"
#include <memory>
#include <span>
namespace adi::engine {
// Owner-thread allocation of a process-unique delivery namespace.
std::uint32_t allocateControllerSourcePrefix();
// Immutable, owner-prepared control timelines. Points remain normalized double;
// callbacks only search/evaluate these arrays, and never read the Store.
class MidiControllerProgram {
  public:
    struct Data;
    explicit MidiControllerProgram(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::size_t size() const noexcept;
    std::uint16_t address(std::size_t index) const noexcept;
    bool valueAt(std::size_t index, std::int64_t sample, double &value) const noexcept;
    bool changesOwnerAt(std::size_t index, std::int64_t sample) const noexcept;
    std::int64_t nextBoundary(std::int64_t at, std::int64_t end) const noexcept;

  private:
    std::shared_ptr<const Data> data_;
};
class MidiControllerPrograms {
  public:
    MidiControllerPrograms(const rows::Model &, double rate);
    std::shared_ptr<const MidiControllerProgram> forTrack(std::int64_t) const;
    const std::vector<std::string> &problems() const noexcept {
        return problems_;
    }

  private:
    std::vector<std::pair<std::int64_t, std::shared_ptr<const MidiControllerProgram>>> tracks_;
    std::vector<std::string> problems_;
};
} // namespace adi::engine
