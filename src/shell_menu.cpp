// Explorer context-menu extension for ImView.
//
// This is a separate in-process DLL because the shell only hands the complete
// selection to a COM context-menu handler, not to a static registry verb. The
// handler adds one entry for the four supported selections and starts imview.exe
// with --new-window.

#include "media_paths.h"
#include "shell_menu_id.h"
#include "utf8.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <shellapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// A shell extension runs inside Explorer. No C++ exception may cross a COM
// method boundary, because an uncaught exception would take Explorer down with
// it. Keep the guard at the entry points where allocations and file-system
// work happen.
template <typename Callback>
HRESULT comGuard(Callback&& callback) noexcept {
    try {
        return callback();
    } catch (...) {
        return E_FAIL;
    }
}

std::atomic<long> gObjectCount{0};

enum class SelectionKind {
    None,
    SingleFolder,
    Images,
    Videos,
    Folders,
};

bool isDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<std::wstring> selectionPaths(IDataObject* dataObject) {
    std::vector<std::wstring> paths;
    if (dataObject == nullptr) return paths;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (dataObject->GetData(&format, &medium) != S_OK) return paths;
    HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        paths.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            if (length == 0) continue;
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            const UINT copied = DragQueryFileW(drop, index, path.data(),
                                               static_cast<UINT>(path.size()));
            if (copied != 0) {
                path.resize(copied);
                paths.push_back(std::move(path));
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return paths;
}

SelectionKind classify(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return SelectionKind::None;
    bool allDirectories = true;
    bool allImages = true;
    bool allVideos = true;
    for (const std::wstring& wide : paths) {
        if (isDirectory(wide)) {
            allImages = false;
            allVideos = false;
            continue;
        }
        allDirectories = false;
        const std::string path = imview::utf8::narrow(wide.c_str());
        if (!imview::media::isImagePath(path)) allImages = false;
        if (!imview::media::isVideoPath(path)) allVideos = false;
    }

    const int count = static_cast<int>(paths.size());
    if (allDirectories) {
        if (count == 1) return SelectionKind::SingleFolder;
        if (count == 2) return SelectionKind::Folders;
        return SelectionKind::None;
    }
    if (allImages && count >= 2 && count <= 4) return SelectionKind::Images;
    if (allVideos && count >= 2 && count <= 4) return SelectionKind::Videos;
    return SelectionKind::None;
}

std::wstring configuredExecutable() {
    wchar_t buffer[32768]{};
    DWORD bytes = sizeof(buffer);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\ImView\\ContextMenu", L"ExePath",
        RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    if (status != ERROR_SUCCESS) return {};
    std::wstring path(buffer);
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

std::wstring executablePath() {
    const std::wstring configured = configuredExecutable();
    if (!configured.empty()) return configured;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&executablePath), &module) ||
        module == nullptr) {
        return {};
    }
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return {};
    modulePath.resize(length);
    const std::filesystem::path path(modulePath);
    return (path.parent_path() / L"imview.exe").wstring();
}

bool launchNewWindow(const std::vector<std::wstring>& paths) {
    const std::wstring executable = executablePath();
    if (executable.empty() ||
        GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    std::wstring command = L"\"" + executable + L"\" --new-window";
    for (const std::wstring& path : paths) {
        command += L" \"";
        command += path;
        command += L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    const BOOL started = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

class ContextMenu final : public IShellExtInit, public IContextMenu {
public:
    ContextMenu() { ++gObjectCount; }
    ~ContextMenu() { --gObjectCount; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IShellExtInit) {
            *object = static_cast<IShellExtInit*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IContextMenu) {
            *object = static_cast<IContextMenu*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }

    // IShellExtInit
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject* dataObject,
                                         HKEY) override {
        return comGuard([&]() -> HRESULT {
            paths_ = selectionPaths(dataObject);
            kind_ = classify(paths_);
            return S_OK;
        });
    }

    // IContextMenu
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT indexMenu,
                                               UINT idCmdFirst, UINT,
                                               UINT flags) override {
        return comGuard([&]() -> HRESULT {
            if (kind_ == SelectionKind::None ||
                (flags & CMF_DEFAULTONLY) != 0) {
                return S_OK;
            }
            InsertMenuW(menu, indexMenu, MF_STRING | MF_BYPOSITION, idCmdFirst,
                        L"Open with ImView in new window");
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
        });
    }

    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        return comGuard([&]() -> HRESULT {
            if (info == nullptr) return E_INVALIDARG;
            if (!IS_INTRESOURCE(info->lpVerb) || LOWORD(info->lpVerb) != 0 ||
                paths_.empty() || kind_ == SelectionKind::None) {
                return E_INVALIDARG;
            }
            return launchNewWindow(paths_) ? S_OK : E_FAIL;
        });
    }

    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR idCmd, UINT flags,
                                               UINT*, LPSTR name,
                                               UINT nameLength) override {
        return comGuard([&]() -> HRESULT {
            if (idCmd != 0) return E_INVALIDARG;
            const wchar_t* text = (flags & GCS_HELPTEXT) != 0
                ? L"Open the selection in a new ImView window"
                : L"ImView.OpenInNewWindow";
            if ((flags & GCS_UNICODE) != 0) {
                if (name == nullptr ||
                    nameLength < (wcslen(text) + 1) * sizeof(wchar_t)) {
                    return E_POINTER;
                }
                wcscpy_s(reinterpret_cast<wchar_t*>(name),
                         nameLength / sizeof(wchar_t), text);
                return S_OK;
            }
            if (name == nullptr || nameLength == 0) return E_POINTER;
            WideCharToMultiByte(CP_ACP, 0, text, -1, name, nameLength, nullptr,
                                nullptr);
            return S_OK;
        });
    }

private:
    std::atomic<ULONG> references_{1};
    std::vector<std::wstring> paths_;
    SelectionKind kind_ = SelectionKind::None;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { ++gObjectCount; }
    ~ClassFactory() { --gObjectCount; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
                                            void** object) override {
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        if (object == nullptr) return E_POINTER;
        auto* menu = new (std::nothrow) ContextMenu();
        if (menu == nullptr) return E_OUTOFMEMORY;
        const HRESULT result = menu->QueryInterface(iid, object);
        menu->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++gObjectCount;
        else --gObjectCount;
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    try {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (!IsEqualCLSID(clsid, kImViewContextMenuClsid)) {
            return CLASS_E_CLASSNOTAVAILABLE;
        }
        auto* factory = new (std::nothrow) ClassFactory();
        if (factory == nullptr) return E_OUTOFMEMORY;
        const HRESULT result = factory->QueryInterface(iid, object);
        factory->Release();
        return result;
    } catch (...) {
        if (object != nullptr) *object = nullptr;
        return E_OUTOFMEMORY;
    }
}

STDAPI DllCanUnloadNow(void) {
    try {
        return gObjectCount.load() == 0 ? S_OK : S_FALSE;
    } catch (...) {
        return S_FALSE;
    }
}
