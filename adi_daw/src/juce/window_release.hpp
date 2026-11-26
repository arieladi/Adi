// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>

namespace adi::ui {
// Destroys a window that owns the component `root` points at, clearing `root` first (walk-A A27).
// Destroying a JUCE window can run the message loop: on Windows its accessibility teardown
// (UiaDisconnectAllProviders, with a UI Automation client attached) waits in a COM modal loop that
// dispatches timers and posted callbacks. One that reads `root` then must find it null, never the
// component being destroyed.
template <class Window, class Root>
void releaseWindow(std::unique_ptr<Window> &window, Root *&root) noexcept {
    root = nullptr;
    window.reset();
}
} // namespace adi::ui
