#include "shell_actions.h"

#include "media_paths.h"
#include "shell_menu_id.h"
#include "utf8.h"

#ifndef IMVIEW_ENABLE_VIDEO
#define IMVIEW_ENABLE_VIDEO 1
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <objbase.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif

namespace imview::shell {
namespace {

#if defined(_WIN32)

// Every write and delete is restricted to ImView's own per-user keys. The
// allowlist below is also the last line of defence against a future caller
// passing an empty or overly broad path to RegDeleteTreeW.
constexpr wchar_t kRegisteredApplicationsKey[] = L"Software\\RegisteredApplications";
constexpr wchar_t kApplicationsKey[] =
    L"Software\\Classes\\Applications\\imview.exe";
constexpr wchar_t kLegacyContextMenuKey[] =
    L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ImView";
constexpr wchar_t kImageContextMenuKey[] =
    L"Software\\Classes\\SystemFileAssociations\\image\\shellex\\ContextMenuHandlers\\ImView";
constexpr wchar_t kVideoContextMenuKey[] =
    L"Software\\Classes\\SystemFileAssociations\\video\\shellex\\ContextMenuHandlers\\ImView";
constexpr wchar_t kDirectoryContextMenuKey[] =
    L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\ImView";

bool hasRegistryPathPrefix(const std::wstring& path, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    if (path.size() < length) return false;
    if (_wcsnicmp(path.c_str(), prefix, length) != 0) return false;
    return path.size() == length || path[length] == L'\\';
}

bool isRegisteredApplicationsPath(const std::wstring& path) {
    return _wcsicmp(path.c_str(), kRegisteredApplicationsKey) == 0;
}

bool isImViewValueName(const wchar_t* valueName) {
    return valueName != nullptr && _wcsicmp(valueName, L"ImView") == 0;
}

bool isMediaExtension(const std::wstring& extension) {
    static const std::vector<std::wstring> wide = [] {
        std::vector<std::wstring> values;
        for (const auto& value : imview::media::imageExtensions()) {
            values.push_back(imview::utf8::widen(value));
        }
        for (const auto& value : imview::media::videoExtensions()) {
            values.push_back(imview::utf8::widen(value));
        }
        return values;
    }();
    for (const auto& value : wide) {
        if (_wcsicmp(extension.c_str(), value.c_str()) == 0) return true;
    }
    return false;
}

bool isMediaExtensionOpenWithPath(const std::wstring& path) {
    constexpr wchar_t kClassesPrefix[] = L"Software\\Classes\\";
    const size_t prefixLength = wcslen(kClassesPrefix);
    if (path.size() <= prefixLength ||
        _wcsnicmp(path.c_str(), kClassesPrefix, prefixLength) != 0) {
        return false;
    }
    const std::wstring rest = path.substr(prefixLength);
    const size_t slash = rest.find(L'\\');
    if (slash == std::wstring::npos) return false;
    const std::wstring extension = rest.substr(0, slash);
    const std::wstring suffix = rest.substr(slash);
    if (_wcsicmp(suffix.c_str(), L"\\OpenWithProgids") != 0) return false;
    return isMediaExtension(extension);
}

bool hasUnsafeRegistryPathText(const std::wstring& path) {
    return path.empty() || path.size() > 512 ||
           path.front() == L'\\' || path.back() == L'\\' ||
           path.find(L"\\\\") != std::wstring::npos ||
           path.find(L"..") != std::wstring::npos ||
           path.find(L'/') != std::wstring::npos;
}

bool isSafeRegistryWritePath(HKEY root, const std::wstring& path) {
    if (root != HKEY_CURRENT_USER || hasUnsafeRegistryPathText(path)) return false;
    static const wchar_t* const prefixes[] = {
        L"Software\\ImView",
        L"Software\\Classes\\ImView.",
        kApplicationsKey,
        L"Software\\Classes\\CLSID\\{A7F1E2C4-5B3D-4E6A-9C2F-1D8B7A6E5F40}",
        kImageContextMenuKey,
        kVideoContextMenuKey,
        kDirectoryContextMenuKey,
        kLegacyContextMenuKey,
    };
    for (const wchar_t* prefix : prefixes) {
        if (hasRegistryPathPrefix(path, prefix)) return true;
    }
    return isRegisteredApplicationsPath(path);
}

bool isSafeRegistryDeleteKeyPath(HKEY root, const std::wstring& path) {
    if (!isSafeRegistryWritePath(root, path)) return false;
    // Never delete the shared RegisteredApplications key itself; only the
    // ImView value inside it may be removed.
    return !isRegisteredApplicationsPath(path);
}

bool isSafeRegistryDeleteValuePath(HKEY root, const std::wstring& path) {
    if (root != HKEY_CURRENT_USER || hasUnsafeRegistryPathText(path)) return false;
    return isSafeRegistryWritePath(root, path) || isMediaExtensionOpenWithPath(path);
}

bool setRegistryString(HKEY root, const std::wstring& subkey,
                       const wchar_t* valueName, const std::wstring& value) {
    if (!isSafeRegistryWritePath(root, subkey)) return false;
    if (isRegisteredApplicationsPath(subkey) && !isImViewValueName(valueName)) {
        return false;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring data = value;
    const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool deleteRegistryKey(HKEY root, const std::wstring& subkey) {
    if (!isSafeRegistryDeleteKeyPath(root, subkey)) return false;
    const LSTATUS status = RegDeleteTreeW(root, subkey.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
           status == ERROR_PATH_NOT_FOUND;
}

bool deleteRegistryValue(HKEY root, const std::wstring& subkey,
                         const wchar_t* valueName) {
    if (valueName == nullptr || !isSafeRegistryDeleteValuePath(root, subkey)) {
        return false;
    }
    if (isRegisteredApplicationsPath(subkey) && !isImViewValueName(valueName)) {
        return false;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return true;
    }
    const LSTATUS status = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool registryKeyExists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

std::wstring registryString(HKEY root, const std::wstring& subkey,
                            const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(value.data()), &bytes) !=
        ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::wstring guidString(const CLSID& clsid) {
    wchar_t buffer[64]{};
    StringFromGUID2(clsid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

std::string executableDirectoryUncached();

constexpr int kShellDllResourceId = 101;

std::wstring localAppDataDirectory() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return std::wstring(buffer, length);
}

std::wstring shellExtensionRootW() {
    const std::wstring root = localAppDataDirectory();
    if (root.empty()) return {};
    return (std::filesystem::path(root) / L"ImView").wstring();
}

std::wstring shellExtensionDirectoryW() {
    const std::wstring root = shellExtensionRootW();
    if (root.empty()) return {};
    return (std::filesystem::path(root) / L"ShellExt").wstring();
}

bool embeddedShellDll(const void*& data, DWORD& size);

std::uint64_t hashBytes(const void* data, size_t size) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = kOffsetBasis;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kPrime;
    }
    return hash;
}

std::wstring hexHash(std::uint64_t value) {
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring text(16, L'0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<size_t>(index)] = digits[value & 0xFu];
        value >>= 4;
    }
    return text;
}

std::wstring contextMenuDllPathW() {
    const std::wstring directory = shellExtensionDirectoryW();
    if (directory.empty()) return {};
    const void* data = nullptr;
    DWORD size = 0;
    if (!embeddedShellDll(data, size)) return {};
    const std::wstring fileName =
        L"imview_shell_" + hexHash(hashBytes(data, size)) + L".dll";
    return (std::filesystem::path(directory) / fileName).wstring();
}

std::string contextMenuDllPath() {
    const std::wstring wide = contextMenuDllPathW();
    return wide.empty() ? std::string() : imview::utf8::narrow(wide.c_str());
}

bool embeddedShellDll(const void*& data, DWORD& size) {
    const HRSRC resource = FindResourceW(
        nullptr, MAKEINTRESOURCEW(kShellDllResourceId), MAKEINTRESOURCEW(10));
    if (resource == nullptr) return false;
    size = SizeofResource(nullptr, resource);
    if (size == 0) return false;
    const HGLOBAL loaded = LoadResource(nullptr, resource);
    if (loaded == nullptr) return false;
    data = LockResource(loaded);
    return data != nullptr;
}

bool hashFile(const std::wstring& path, std::uint64_t& hash, std::uint64_t& size) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint64_t value = kOffsetBasis;
    LONGLONG remaining = fileSize.QuadPart;
    bool ok = true;
    while (remaining > 0) {
        const DWORD toRead = static_cast<DWORD>(
            std::min<LONGLONG>(remaining,
                               static_cast<LONGLONG>(buffer.size())));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), toRead, &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        for (DWORD index = 0; index < read; ++index) {
            value ^= buffer[index];
            value *= kPrime;
        }
        remaining -= read;
    }
    CloseHandle(file);
    if (!ok) return false;
    hash = value;
    size = static_cast<std::uint64_t>(fileSize.QuadPart);
    return true;
}

void deleteShellFileBestEffort(const std::filesystem::path& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (DeleteFileW(path.c_str()) == FALSE) {
        // Explorer may still have the DLL mapped. Remove it on the next boot
        // instead of leaving an untracked file behind permanently.
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

void removeEmptyDirectory(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) || error) return;
    error.clear();
    std::filesystem::remove(path, error);
}

void cleanupShellDlls(const std::wstring& keep) {
    const std::wstring root = shellExtensionRootW();
    if (root.empty()) return;
    const std::filesystem::path rootPath(root);
    const std::filesystem::path directory = shellExtensionDirectoryW();
    std::error_code error;
    if (!directory.empty()) {
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            const std::filesystem::path candidate = it->path();
            std::error_code fileError;
            if (!it->is_regular_file(fileError) || fileError) continue;
            const std::wstring name = candidate.filename().wstring();
            if (name.rfind(L"imview_shell", 0) != 0) continue;
            const std::wstring extension = candidate.extension().wstring();
            if (_wcsicmp(extension.c_str(), L".dll") != 0 &&
                _wcsicmp(extension.c_str(), L".tmp") != 0) {
                continue;
            }
            if (!keep.empty() && _wcsicmp(candidate.c_str(), keep.c_str()) == 0) {
                continue;
            }
            deleteShellFileBestEffort(candidate);
        }
        error.clear();
        removeEmptyDirectory(directory);
    }
    error.clear();
    for (std::filesystem::directory_iterator it(rootPath, error), end;
         !error && it != end; it.increment(error)) {
        const std::filesystem::path candidate = it->path();
        std::error_code fileError;
        if (!it->is_regular_file(fileError) || fileError) continue;
        const std::wstring name = candidate.filename().wstring();
        if (name.rfind(L"imview_shell", 0) != 0) continue;
        const std::wstring extension = candidate.extension().wstring();
        if (_wcsicmp(extension.c_str(), L".dll") != 0 &&
            _wcsicmp(extension.c_str(), L".tmp") != 0) {
            continue;
        }
        deleteShellFileBestEffort(candidate);
    }
    error.clear();
    removeEmptyDirectory(rootPath);
}

bool extractShellDll() {
    const void* data = nullptr;
    DWORD size = 0;
    if (!embeddedShellDll(data, size)) return false;
    const std::wstring path = contextMenuDllPathW();
    if (path.empty()) return false;
    const std::filesystem::path directory = std::filesystem::path(path).parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    // The file name contains the content hash, so a matching size and hash
    // means this is exactly the embedded build. Explorer may hold it open.
    const std::uint64_t expected = hashBytes(data, size);
    std::uint64_t actual = 0;
    std::uint64_t existingSize = 0;
    if (hashFile(path, actual, existingSize) && existingSize == size &&
        actual == expected) {
        return true;
    }

    const std::wstring tempPath = path + L".tmp";
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data, size, &written, nullptr) != FALSE &&
              written == size;
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    // Replace atomically so a crash cannot leave a half-written DLL that
    // Explorer would load later.
    if (MoveFileExW(tempPath.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

std::wstring modulePathW() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

bool invokeVerb(const std::string& path, const wchar_t* verb, ULONG mask) {
    const std::wstring widePath = imview::utf8::widen(path);
    if (widePath.empty()) return false;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = mask;
    info.lpVerb = verb;
    info.lpFile = widePath.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

// Runs a command line and captures its stdout. Used for the EXIF helper.
bool runCapture(const std::wstring& commandLine, std::string& output) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine; // CreateProcessW may modify it
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return false;
    }

    // Poll the pipe so a tool that hangs cannot block this thread forever.
    constexpr DWORD kTimeoutMilliseconds = 5000;
    constexpr std::size_t kMaxOutput = 64u * 1024u;
    output.clear();
    char buffer[4096];
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMilliseconds;
    bool finished = false;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
                // The tool exited: drain whatever is left in the pipe.
                while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) &&
                       available > 0) {
                    DWORD read = 0;
                    if (!ReadFile(readEnd, buffer,
                                  static_cast<DWORD>(std::min<std::size_t>(available,
                                                                          sizeof(buffer))),
                                  &read, nullptr) || read == 0) {
                        break;
                    }
                    output.append(buffer, read);
                    if (output.size() > kMaxOutput) break;
                }
                finished = true;
                break;
            }
            continue;
        }
        DWORD read = 0;
        if (!ReadFile(readEnd, buffer,
                      static_cast<DWORD>(std::min<std::size_t>(available, sizeof(buffer))),
                      &read, nullptr) || read == 0) {
            break;
        }
        output.append(buffer, read);
        // Guard against a runaway tool.
        if (output.size() > kMaxOutput) break;
    }
    if (!finished) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }

    CloseHandle(readEnd);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

// Directory of the running executable.
std::string executableDirectoryUncached() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    const std::filesystem::path exePath(buffer);
    return exePath.parent_path().u8string();
}

// Locates imexif.exe / imexif.bat / imexif.cmd next to the executable or in the
// working directory. Returns an empty string when absent.
std::string findExifHelper() {
    std::error_code error;
    std::vector<std::filesystem::path> directories;
    const std::string exeDir = executableDirectoryUncached();
    if (!exeDir.empty()) directories.push_back(std::filesystem::u8path(exeDir));
    const std::filesystem::path cwd = std::filesystem::current_path(error);
    if (!error && !cwd.empty()) directories.push_back(cwd);

    static const char* extensions[] = {".exe", ".bat", ".cmd"};
    for (const auto& directory : directories) {
        for (const char* extension : extensions) {
            std::filesystem::path candidate =
                directory / (std::string("imexif") + extension);
            error.clear();
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate.u8string();
            }
        }
    }
    return {};
}

// IFileOpenDialog with FOS_PICKFOLDERS gives the modern folder picker. The old
// SHBrowseForFolder dialog is kept as a single-folder fallback.
std::vector<std::string> chooseFolders(const std::string& initialDirectory,
                                       bool allowMultiple) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::string> result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            if (allowMultiple) options |= FOS_ALLOWMULTISELECT;
            dialog->SetOptions(options);
        }
        dialog->SetTitle(allowMultiple ? L"Choose folders" : L"Choose a folder");

        IShellItem* initial = nullptr;
        const std::wstring wideInitial = imview::utf8::widen(initialDirectory);
        if (!wideInitial.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(wideInitial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&initial))) &&
            initial != nullptr) {
            dialog->SetFolder(initial);
            initial->Release();
        }

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItemArray* results = nullptr;
            if (SUCCEEDED(dialog->GetResults(&results)) && results != nullptr) {
                DWORD count = 0;
                if (SUCCEEDED(results->GetCount(&count))) {
                    for (DWORD index = 0; index < count; ++index) {
                        IShellItem* item = nullptr;
                        if (FAILED(results->GetItemAt(index, &item)) || item == nullptr) {
                            continue;
                        }
                        PWSTR filePath = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath))) {
                            std::string path = imview::utf8::narrow(filePath);
                            if (!path.empty()) result.push_back(std::move(path));
                            CoTaskMemFree(filePath);
                        }
                        item->Release();
                    }
                }
                results->Release();
            }
        }
        dialog->Release();
    } else {
        BROWSEINFOW browse{};
        browse.lpszTitle = allowMultiple ? L"Choose folders" : L"Choose a folder";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST idList = SHBrowseForFolderW(&browse);
        if (idList != nullptr) {
            wchar_t filePath[MAX_PATH]{};
            if (SHGetPathFromIDListW(idList, filePath)) {
                std::string path = imview::utf8::narrow(filePath);
                if (!path.empty()) result.push_back(std::move(path));
            }
            CoTaskMemFree(idList);
        }
    }

    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return result;
}

#endif

#if !defined(_WIN32)
std::vector<std::string> chooseFolders(const std::string&, bool) {
    return {};
}
#endif

} // namespace

std::string pickFolder(const std::string& initialDirectory) {
    const std::vector<std::string> folders = chooseFolders(initialDirectory, false);
    return folders.empty() ? std::string() : folders.front();
}

std::vector<std::string> pickFolders(const std::string& initialDirectory) {
    return chooseFolders(initialDirectory, true);
}

bool openUrl(const std::string& url) {
#if defined(_WIN32)
    return invokeVerb(url, L"open", SEE_MASK_FLAG_NO_UI);
#else
    (void)url;
    return false;
#endif
}

bool showInExplorer(const std::string& path) {
#if defined(_WIN32)
    const std::wstring widePath = imview::utf8::widen(path);
    if (widePath.empty()) return false;
    std::wstring arguments = L"/select,\"";
    arguments += widePath;
    arguments += L"\"";
    const HINSTANCE instance = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                            arguments.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(instance) > 32;
#else
    (void)path;
    return false;
#endif
}

bool openWith(const std::string& path) {
#if defined(_WIN32)
    // The shell's "Open with" chooser, the same entry point Explorer uses.
    return invokeVerb(path, L"openas", SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI);
#else
    (void)path;
    return false;
#endif
}

bool showProperties(const std::string& path) {
#if defined(_WIN32)
    return invokeVerb(path, L"properties", SEE_MASK_INVOKEIDLIST);
#else
    (void)path;
    return false;
#endif
}

namespace {

// Writes the complete media-association state for the requested types. This
// removes stale entries when one type is turned off, rather than leaving an
// older registration behind.
bool syncMediaAssociations(bool images, bool videos) {
#if defined(_WIN32)
    videos = videos && IMVIEW_ENABLE_VIDEO != 0;
    const std::wstring executable = modulePathW();
    if (executable.empty()) return false;
    const std::wstring quoted = L"\"" + executable + L"\"";
    const std::wstring command = quoted + L" \"%1\"";
    const std::wstring imageProgId = L"ImView.Image";
    const std::wstring videoProgId = L"ImView.Video";
    const std::wstring capabilities = L"Software\\ImView\\Capabilities";
    const std::wstring fileAssociations = capabilities + L"\\FileAssociations";

    bool ok = true;
    // Older versions also created an Applications\imview.exe Open With entry.
    // The default-app registration uses ProgIDs and capabilities instead.
    ok = deleteRegistryKey(HKEY_CURRENT_USER, kApplicationsKey) && ok;

    if (images || videos) {
        ok = setRegistryString(HKEY_CURRENT_USER, capabilities,
                               L"ApplicationName", L"ImView") && ok;
        ok = setRegistryString(HKEY_CURRENT_USER, capabilities,
                               L"ApplicationDescription",
                               L"Image and video viewer") && ok;
        ok = setRegistryString(HKEY_CURRENT_USER, capabilities,
                               L"ApplicationIcon", quoted + L",0") && ok;
        ok = setRegistryString(HKEY_CURRENT_USER,
                               kRegisteredApplicationsKey,
                               L"ImView", capabilities) && ok;
    } else {
        ok = deleteRegistryKey(HKEY_CURRENT_USER, capabilities) && ok;
        ok = deleteRegistryValue(HKEY_CURRENT_USER,
                                 kRegisteredApplicationsKey, L"ImView") && ok;
    }

    const auto registerProgId = [&](const std::wstring& progId,
                                    const std::wstring& description) {
        const std::wstring key = L"Software\\Classes\\" + progId;
        bool result = true;
        result = setRegistryString(HKEY_CURRENT_USER, key, nullptr,
                                   description) && result;
        result = setRegistryString(HKEY_CURRENT_USER, key + L"\\DefaultIcon",
                                   nullptr, quoted + L",0") && result;
        result = setRegistryString(HKEY_CURRENT_USER,
                                   key + L"\\shell\\open\\command",
                                   nullptr, command) && result;
        return result;
    };
    const auto removeProgId = [&](const std::wstring& progId) {
        return deleteRegistryKey(HKEY_CURRENT_USER,
                                 L"Software\\Classes\\" + progId);
    };

    if (images) ok = registerProgId(imageProgId, L"ImView image") && ok;
    else ok = removeProgId(imageProgId) && ok;
    if (videos) ok = registerProgId(videoProgId, L"ImView video") && ok;
    else ok = removeProgId(videoProgId) && ok;

    const auto extensionGroup = [&](const std::vector<std::string>& extensions,
                                    bool enabled, const std::wstring& progId) {
        for (const auto& extension : extensions) {
            const std::wstring wideExtension = imview::utf8::widen(extension);
            // Legacy Open With entries are no longer part of the model. Remove
            // them so an upgrade leaves only the default-app registration.
            ok = deleteRegistryValue(
                     HKEY_CURRENT_USER,
                     L"Software\\Classes\\" + wideExtension +
                         L"\\OpenWithProgids",
                     progId.c_str()) && ok;
            if (enabled) {
                ok = setRegistryString(HKEY_CURRENT_USER, fileAssociations,
                                       wideExtension.c_str(), progId) && ok;
            } else {
                ok = deleteRegistryValue(HKEY_CURRENT_USER, fileAssociations,
                                         wideExtension.c_str()) && ok;
            }
        }
    };
    extensionGroup(imview::media::imageExtensions(), images, imageProgId);
    extensionGroup(imview::media::videoExtensions(), videos, videoProgId);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
#else
    (void)images;
    (void)videos;
    return false;
#endif
}

} // namespace

bool registerImageAssociations() {
#if defined(_WIN32)
    return syncMediaAssociations(true, integrationStatus().videos);
#else
    return false;
#endif
}

bool unregisterImageAssociations() {
#if defined(_WIN32)
    return syncMediaAssociations(false, integrationStatus().videos);
#else
    return false;
#endif
}

bool registerVideoAssociations() {
#if defined(_WIN32) && IMVIEW_ENABLE_VIDEO
    return syncMediaAssociations(integrationStatus().images, true);
#else
    return false;
#endif
}

bool unregisterVideoAssociations() {
#if defined(_WIN32) && IMVIEW_ENABLE_VIDEO
    return syncMediaAssociations(integrationStatus().images, false);
#else
    return false;
#endif
}

bool registerContextMenu() {
#if defined(_WIN32)
    if (!extractShellDll()) return false;
    const std::string dllPath = contextMenuDllPath();
    const std::wstring wideDll = imview::utf8::widen(dllPath);
    const std::wstring executable = modulePathW();
    if (wideDll.empty() || executable.empty()) return false;
    const std::wstring clsid = guidString(kImViewContextMenuClsid);
    const std::wstring clsidKey = L"Software\\Classes\\CLSID\\" + clsid;
    const std::wstring contextKey = L"Software\\ImView\\ContextMenu";

    bool ok = true;
    ok = setRegistryString(HKEY_CURRENT_USER, clsidKey, nullptr,
                           L"ImView Explorer menu") && ok;
    ok = setRegistryString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32",
                           nullptr, wideDll) && ok;
    ok = setRegistryString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32",
                           L"ThreadingModel", L"Apartment") && ok;
    // The DLL lives under %LOCALAPPDATA%\ImView\ShellExt; tell it where the
    // actual exe is.
    ok = setRegistryString(HKEY_CURRENT_USER, contextKey, L"ExePath",
                           executable) && ok;
    const wchar_t* handlerKeys[] = {
        kImageContextMenuKey,
#if IMVIEW_ENABLE_VIDEO
        kVideoContextMenuKey,
#endif
        kDirectoryContextMenuKey,
    };
    for (const wchar_t* key : handlerKeys) {
        ok = setRegistryString(HKEY_CURRENT_USER, key, nullptr, clsid) && ok;
    }
#if !IMVIEW_ENABLE_VIDEO
    ok = deleteRegistryKey(HKEY_CURRENT_USER, kVideoContextMenuKey) && ok;
#endif
    // Older builds loaded the DLL for every file type. Remove that broad
    // handler so the extension only sees images, videos and directories.
    ok = deleteRegistryKey(HKEY_CURRENT_USER, kLegacyContextMenuKey) && ok;
    if (!ok) {
        unregisterContextMenu();
        return false;
    }
    cleanupShellDlls(wideDll);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

bool unregisterContextMenu() {
#if defined(_WIN32)
    const std::wstring clsid = guidString(kImViewContextMenuClsid);
    bool ok = true;
    const wchar_t* handlerKeys[] = {
        kImageContextMenuKey,
        kVideoContextMenuKey,
        kDirectoryContextMenuKey,
        kLegacyContextMenuKey,
    };
    for (const wchar_t* key : handlerKeys) {
        ok = deleteRegistryKey(HKEY_CURRENT_USER, key) && ok;
    }
    ok = deleteRegistryKey(HKEY_CURRENT_USER,
                           L"Software\\Classes\\CLSID\\" + clsid) && ok;
    ok = deleteRegistryKey(HKEY_CURRENT_USER,
                           L"Software\\ImView\\ContextMenu") && ok;
    // Keep the DLL when a registry deletion failed: it is safer to leave the
    // file behind than to leave a registered CLSID pointing at nothing.
    if (!ok) return false;
    cleanupShellDlls(std::wstring());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

bool uninstallIntegration() {
#if defined(_WIN32)
    const bool media = syncMediaAssociations(false, false);
    const bool context = unregisterContextMenu();
    return media && context;
#else
    return false;
#endif
}

IntegrationStatus integrationStatus() {
    IntegrationStatus status;
#if defined(_WIN32)
    const auto matchesCommand = [](const wchar_t* subkey) {
        const std::wstring executable = modulePathW();
        if (executable.empty()) return false;
        const std::wstring expected = L"\"" + executable + L"\" \"%1\"";
        return _wcsicmp(registryString(HKEY_CURRENT_USER, subkey, nullptr).c_str(),
                        expected.c_str()) == 0;
    };
    status.images = matchesCommand(
        L"Software\\Classes\\ImView.Image\\shell\\open\\command");
    status.videos = IMVIEW_ENABLE_VIDEO != 0 && matchesCommand(
        L"Software\\Classes\\ImView.Video\\shell\\open\\command");
    const void* embeddedData = nullptr;
    DWORD embeddedSize = 0;
    status.contextMenuAvailable = embeddedShellDll(embeddedData, embeddedSize);
    if (status.contextMenuAvailable) {
        const std::wstring clsid = guidString(kImViewContextMenuClsid);
        const std::wstring inproc =
            L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
        const std::wstring registeredDll = registryString(
            HKEY_CURRENT_USER, inproc, nullptr);
        const std::wstring wideDll = contextMenuDllPathW();
        const bool pathMatches =
            !wideDll.empty() &&
            _wcsicmp(registeredDll.c_str(), wideDll.c_str()) == 0;
        const bool threadingMatches = _wcsicmp(
            registryString(HKEY_CURRENT_USER, inproc, L"ThreadingModel").c_str(),
            L"Apartment") == 0;
        const std::wstring registeredExe = registryString(
            HKEY_CURRENT_USER, L"Software\\ImView\\ContextMenu", L"ExePath");
        const std::wstring currentExe = modulePathW();
        const bool exeMatches = !registeredExe.empty() &&
            _wcsicmp(registeredExe.c_str(), currentExe.c_str()) == 0;
        const auto handlerMatches = [&](const wchar_t* key) {
            return _wcsicmp(
                       registryString(HKEY_CURRENT_USER, key, nullptr).c_str(),
                       clsid.c_str()) == 0;
        };
        const bool imageHandler = handlerMatches(kImageContextMenuKey);
        const bool videoHandler = IMVIEW_ENABLE_VIDEO == 0
            ? !registryKeyExists(HKEY_CURRENT_USER, kVideoContextMenuKey)
            : handlerMatches(kVideoContextMenuKey);
        const bool folderHandler = handlerMatches(kDirectoryContextMenuKey);
        const bool legacyHandlerGone = !registryKeyExists(
            HKEY_CURRENT_USER, kLegacyContextMenuKey);
        status.contextMenu = pathMatches && threadingMatches && exeMatches &&
            imageHandler && videoHandler && folderHandler && legacyHandlerGone;
    }
#endif
    return status;
}

std::string executableDirectory() {
#if defined(_WIN32)
    return executableDirectoryUncached();
#else
    return {};
#endif
}

bool readExternalExif(const std::string& imagePath, std::vector<std::string>& lines) {
    lines.clear();
#if defined(_WIN32)
    const std::string helper = findExifHelper();
    if (helper.empty()) return false;

    const std::wstring wideHelper = imview::utf8::widen(helper);
    const std::wstring wideImage = imview::utf8::widen(imagePath);
    if (wideHelper.empty() || wideImage.empty()) return false;

    // Batch helpers must run through cmd.exe; executables are started directly.
    std::string extension = std::filesystem::u8path(helper).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool isScript = extension == ".bat" || extension == ".cmd";

    std::wstring commandLine;
    if (isScript) {
        commandLine = L"cmd.exe /d /c \"\"" + wideHelper + L"\" \"" + wideImage + L"\"\"";
    } else {
        commandLine = L"\"" + wideHelper + L"\" \"" + wideImage + L"\"";
    }

    std::string output;
    if (!runCapture(commandLine, output) || output.empty()) return false;
    output = imview::utf8::fromConsoleCodePage(output);

    // Split into trimmed, non-empty lines.
    std::string current;
    const auto flush = [&]() {
        while (!current.empty() &&
               (current.back() == '\r' || current.back() == ' ' || current.back() == '\t')) {
            current.pop_back();
        }
        std::size_t start = 0;
        while (start < current.size() && (current[start] == ' ' || current[start] == '\t')) {
            ++start;
        }
        if (start < current.size()) lines.push_back(current.substr(start));
        current.clear();
    };
    for (const char ch : output) {
        if (ch == '\n') {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();
    return !lines.empty();
#else
    (void)imagePath;
    return false;
#endif
}

} // namespace imview::shell
