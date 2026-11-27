// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/settings/store.hpp"
#include <juce_core/juce_core.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace adi::ui {
// One local, opt-in text log for this application run. Only the message thread writes it.
class DiagnosticLog {
  public:
    DiagnosticLog(const std::filesystem::path &data, std::string_view version)
        : directory_(data.empty() ? data : data / "diagnostic-logs"), version_(version) {}

    // True when writing has just been enabled. After a pause, keep using this run's file.
    bool refresh(const settings::AppSettings &settings) {
        const auto setting = settings.get("privacy.diagnosticLog");
        const bool requested = setting.is_boolean() && setting.get<bool>();
        if (requested == enabled_)
            return false;
        enabled_ = requested;
        if (!enabled_ || directory_.empty())
            return false;
        if (stream_.is_open())
            return stream_.good();

        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        if (error)
            return false;
        const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S").toStdString();
        const auto name = "ADI-" + stamp + "-" + juce::Uuid{}.toString().toStdString() + ".log";
        stream_.open(directory_ / name, std::ios::out | std::ios::app);
        write("ADI version", version_);
        return stream_.good();
    }

    void write(std::string_view source, std::string_view message) {
        if (!enabled_ || !stream_.is_open() || !stream_ || message.empty())
            return;
        stream_ << juce::Time::getCurrentTime().toISO8601(true).toStdString()
                << '\t' << source << '\t';
        for (char c : message) {
            if (c == '\r')
                stream_ << "\\r";
            else if (c == '\n')
                stream_ << "\\n";
            else
                stream_ << c;
        }
        stream_ << std::endl; // Keep the last message readable if ADI exits unexpectedly.
    }

  private:
    std::filesystem::path directory_;
    std::string version_;
    std::ofstream stream_;
    bool enabled_ = false;
};
} // namespace adi::ui
