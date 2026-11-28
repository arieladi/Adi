// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>

namespace adi::ui {

// An action that would stop an export waits for an explicit answer.
class ExportInterruption {
  public:
    bool request(bool exporting, std::function<void()> action) {
        if (pending_)
            return false;
        if (exporting) {
            pending_ = std::move(action);
            return true;
        }
        action();
        return false;
    }

    void answer(bool discardExport) {
        auto action = std::exchange(pending_, {});
        if (discardExport && action)
            action();
    }

  private:
    std::function<void()> pending_;
};

} // namespace adi::ui
