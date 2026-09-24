// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace adi::audio {
// Instrument the actual WAV I/O boundary, not a mock reader. The driver marks
// callbacks; tests can plant reads and assert the violation counter changes.
inline thread_local bool onAudioThread = false;
inline std::atomic<std::uint32_t> callbackFileIo{0};
inline void fileIoPoint() noexcept {
    if (onAudioThread) callbackFileIo.fetch_add(1, std::memory_order_relaxed);
}
struct CallbackScope {
    bool previous = onAudioThread;
    CallbackScope() noexcept { onAudioThread = true; }
    ~CallbackScope() { onAudioThread = previous; }
};
}
