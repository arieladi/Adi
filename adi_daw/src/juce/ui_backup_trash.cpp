// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_backup_trash.hpp"

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace {
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

// IFileOperation is an STA API. Balance even CoInitializeEx's S_FALSE when
// JUCE has already initialized COM on the message thread.
class ShellApartment {
public:
    ShellApartment() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ShellApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    [[nodiscard]] bool ready() const { return SUCCEEDED(result); }
private:
    HRESULT result;
};

class RecycleProgress final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IFileOperationProgressSink> {
public:
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem*) override {
        // The shell may decline recycling. Veto any ordinary delete before it starts.
        return (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) != 0 ? S_OK : E_ABORT;
    }

    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem* recycledItem) override {
        recycled = SUCCEEDED(result) && recycledItem != nullptr;
        return S_OK;
    }

    [[nodiscard]] bool movedToBin() const { return recycled; }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    bool recycled = false;
};
}  // namespace
#endif

namespace adi::backup {

bool moveToTrash(const juce::File& file) {
#if JUCE_WINDOWS
    const auto name = file.getFullPathName();
    const auto path = name.toWideCharPointer();
    const auto attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;

    // Network and removable paths need not provide a Recycle Bin at all.
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(path, volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED)
        return false;

    const ShellApartment apartment;
    if (!apartment.ready())
        return false;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(item.GetAddressOf()))))
        return false;
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(operation.GetAddressOf()))))
        return false;

    constexpr DWORD flags = FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE | FOF_SILENT | FOF_NOERRORUI;
    auto progress = Microsoft::WRL::Make<RecycleProgress>();
    if (!progress || FAILED(operation->SetOperationFlags(flags)) ||
        FAILED(operation->DeleteItem(item.Get(), progress.Get())) || FAILED(operation->PerformOperations()))
        return false;

    BOOL aborted = FALSE;
    return SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && aborted == FALSE && progress->movedToBin();
#else
    return file.moveToTrash();
#endif
}

}  // namespace adi::backup
