// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

namespace adi::ui {

class PluginStateQuit {
  public:
    bool pending() const noexcept { return state_ != State::idle; }

    // The capture returns no value on a project error, an empty string on
    // success, or the warning for the stored states Quit would leave behind.
    template<class Capture, class Close>
    std::optional<std::string> request(Capture capture, Close close) {
        if (pending())
            return std::nullopt;
        state_ = State::capturing;
        auto warning = capture();
        if (!warning) {
            state_ = State::idle;
            return std::nullopt;
        }
        if (warning->empty()) {
            state_ = State::quitting;
            close();
            return std::nullopt;
        }
        state_ = State::waitingForAnswer;
        return warning;
    }

    bool answer(bool quit) noexcept {
        if (state_ != State::waitingForAnswer)
            return false;
        state_ = quit ? State::quitting : State::idle;
        return quit;
    }

  private:
    enum class State { idle, capturing, waitingForAnswer, quitting };
    State state_ = State::idle;
};

} // namespace adi::ui
