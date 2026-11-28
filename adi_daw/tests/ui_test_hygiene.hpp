// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-02's test hygiene for the window's JUCE suites (PLAN WTS-02; ADR-0180 d8):
// - AssertionWatch: Debug builds log JUCE assertions. The suite counts them and fails on unexpected
//   assertions. The application, shell library and test targets share the JUCE assertion configuration.
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
    // What the machine, not the window's code, makes JUCE assert in a headless run: each one line of the
    // vendored JUCE, counted apart and said once, never failing the run. Every other assertion, in these
    // files too, fails it; and if a JUCE update moves a line, that assertion fails the run again.
    //  - juce_Messaging_linux.cpp:87, the message queue full: the suite posts messages and runs no loop;
    //  - juce_XWindowSystem_linux.cpp:2703, a cursor shown on no window: there is no X display;
    //  - juce_Midi_linux.cpp:539, the ALSA sequencer refused: there is no MIDI system.
    int queueFull() const noexcept { return queueFull_.load(); }
    static const char *environment(const juce::String &message) {
        if (message.endsWith("juce_Messaging_linux.cpp:87"))
            return "the headless message queue";
        if (message.endsWith("juce_XWindowSystem_linux.cpp:2703"))
            return "no X display";
        if (message.endsWith("juce_Midi_linux.cpp:539"))
            return "no ALSA MIDI";
        return nullptr;
    }
    static bool isQueueFull(const juce::String &message) { return environment(message) != nullptr; }
    juce::String last() const {
        const juce::ScopedLock held(lock_);
        return last_;
    }

  protected:
    void logMessage(const juce::String &message) override {
        if (message.startsWith("JUCE Assertion failure") && isQueueFull(message)) {
            ++queueFull_;
            const juce::ScopedLock held(lock_);
            if (seen_.addIfNotAlreadyThere(message)) { // each said once, the rest counted
                std::fputs((message + " (" + environment(message) + "; counted apart)\n").toRawUTF8(), stderr);
            }
            return;
        }
        if (message.startsWith("JUCE Assertion failure")) {
            ++count_;
            const juce::ScopedLock held(lock_);
            last_ = message;
        }
        std::fputs((message + "\n").toRawUTF8(), stderr);
    }

  private:
    juce::Logger *previous_ = nullptr;
    std::atomic<int> count_{0}, queueFull_{0};
    juce::StringArray seen_;
    juce::CriticalSection lock_;
    juce::String last_;
};
inline void noDialogs() {
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Without _DEBUG the two _CrtSetReport* macros expand to nothing, so `kind` is unused in a
    // release build, and C4189 under ADI_WERROR stopped every optimised UI test build.
    for ([[maybe_unused]] const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
#endif
}
} // namespace adi::uitest
