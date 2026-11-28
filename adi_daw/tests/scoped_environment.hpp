// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace adi::test {

class ScopedOneDrive {
public:
    explicit ScopedOneDrive(const std::filesystem::path &root) {
#if defined(_WIN32)
        wchar_t *value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value, &size, L"OneDrive") != 0)
            throw std::runtime_error("read test OneDrive root");
        const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, std::free);
        if (owned)
            saved_ = owned.get();
        if (_wputenv_s(L"OneDrive", root.c_str()) != 0)
            throw std::runtime_error("set test OneDrive root");
#else
        if (const auto *value = std::getenv("OneDrive"))
            saved_ = value;
        if (setenv("OneDrive", root.c_str(), 1) != 0)
            throw std::runtime_error("set test OneDrive root");
#endif
    }

    ~ScopedOneDrive() {
#if defined(_WIN32)
        (void)_wputenv_s(L"OneDrive", saved_ ? saved_->c_str() : L"");
#else
        if (saved_)
            (void)setenv("OneDrive", saved_->c_str(), 1);
        else
            (void)unsetenv("OneDrive");
#endif
    }

    ScopedOneDrive(const ScopedOneDrive &) = delete;
    ScopedOneDrive &operator=(const ScopedOneDrive &) = delete;

private:
    std::optional<std::filesystem::path::string_type> saved_;
};

} // namespace adi::test
