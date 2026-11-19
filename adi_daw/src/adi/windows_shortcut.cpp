// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/windows_shortcut.hpp"
#include "adi/audio/io_audit.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <share.h>
#include <vector>

namespace adi::scan {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t maximumLinkBytes = 1024 * 1024;
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
}

std::optional<std::filesystem::path> windowsShortcutTarget(const std::filesystem::path& path) {
    if (audio::onAudioThread) return std::nullopt;
    try {
        const std::unique_ptr<std::FILE, decltype(&std::fclose)> input(
            _wfsopen(path.c_str(), L"rb", _SH_DENYNO), &std::fclose);
        if (!input) return std::nullopt;
        std::vector<unsigned char> bytes(maximumLinkBytes + 1);
        const auto size = std::fread(bytes.data(), 1, bytes.size(), input.get());
        if (size == 0 || size > maximumLinkBytes || std::ferror(input.get()) || !std::feof(input.get()))
            return std::nullopt;
        // Load from our bounded byte snapshot, not from a filename that COM
        // could reopen. The shell object never receives a Resolve call.
        ComScope com;
        if (!com.usable()) return std::nullopt;
        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(link.GetAddressOf())))) return std::nullopt;
        ComPtr<IPersistStream> persist;
        if (FAILED(link.As(&persist))) return std::nullopt;
        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) return std::nullopt;
        ULONG written = 0;
        if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(size), &written)) || written != size ||
            FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)) || FAILED(persist->Load(stream.Get())))
            return std::nullopt;
        ComPtr<IShellLinkDataList> data;
        DWORD flags = 0;
        if (FAILED(link.As(&data)) || FAILED(data->GetFlags(&flags)) || (flags & SLDF_HAS_DARWINID) != 0)
            return std::nullopt; // Advertised installer shortcuts are not filesystem discovery.
        // GetPath documents MAX_PATH as its return limit. Reject the boundary
        // where a truncated result could be confused with a complete path.
        std::array<wchar_t, MAX_PATH> raw{};
        if (link->GetPath(raw.data(), static_cast<int>(raw.size()), nullptr, SLGP_RAWPATH) != S_OK)
            return std::nullopt;
        const auto end = std::find(raw.begin(), raw.end(), L'\0');
        if (end == raw.begin() || end >= raw.end() - 1) return std::nullopt;
        const auto count = ExpandEnvironmentStringsW(raw.data(), nullptr, 0);
        if (count == 0 || count > 32768) return std::nullopt;
        std::vector<wchar_t> expanded(count);
        if (ExpandEnvironmentStringsW(raw.data(), expanded.data(), count) != count) return std::nullopt;
        std::filesystem::path target(expanded.data());
        // Relative-only links need a separate relocation policy. Do not use
        // the scanner's working directory, a link's executable arguments or
        // a shell namespace object to guess a target.
        if (!target.is_absolute()) return std::nullopt;
        return target;
    } catch (const std::exception&) { return std::nullopt; }
}
}
