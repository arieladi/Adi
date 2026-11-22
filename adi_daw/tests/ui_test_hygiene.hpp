// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-02's test hygiene for the window's JUCE suites (PLAN WTS-02; ADR-0180 d8):
// - AssertionWatch: JUCE's assertions, logged (JUCE_LOG_ASSERTIONS, on the test targets only: never on
//   ADI.exe, where it would keep jassert live), are counted, so a run that trips one fails.
// - noDialogs: on Windows, the C runtime reports to stderr and never opens a dialog box (an assert, an
//   abort or a crash would otherwise wait on a click no headless run gives).
#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdio>
#if defined(_WIN32)
#include <crtdbg.h>
#include <stdlib.h>
#endif
namespace adi::uitest {
class AssertionWatch final : public juce::Logger {
  public:
    // Installed as JUCE's logger for its lifetime; the logger it replaced comes back after.
    AssertionWatch() : previous_(juce::Logger::getCurrentLogger()) { juce::Logger::setCurrentLogger(this); }
    ~AssertionWatch() override { juce::Logger::setCurrentLogger(previous_); }
    AssertionWatch(const AssertionWatch &) = delete;
    AssertionWatch &operator=(const AssertionWatch &) = delete;
    int count() const noexcept { return count_.load(); }
    juce::String last() const {
        const juce::ScopedLock held(lock_);
        return last_;
    }

  protected:
    void logMessage(const juce::String &message) override {
        if (message.startsWith("JUCE Assertion failure")) {
            ++count_;
            const juce::ScopedLock held(lock_);
            last_ = message;
        }
        std::fputs((message + "\n").toRawUTF8(), stderr);
    }

  private:
    juce::Logger *previous_ = nullptr;
    std::atomic<int> count_{0};
    juce::CriticalSection lock_;
    juce::String last_;
};
inline void noDialogs() {
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
#endif
}
} // namespace adi::uitest
