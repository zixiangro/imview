#include "shell_actions.h"

#include "media_paths.h"
#include "shell_menu_id.h"
#include "utf8.h"

#ifndef IMVIEW_ENABLE_VIDEO
#define IMVIEW_ENABLE_VIDEO 1
#endif

#include <algorithm>
#include <cctype>
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

// ShellExecuteEx helper shared by the "open with" and "properties" actions.
bool setRegistryString(HKEY root, const std::wstring& subkey,
                       const wchar_t* valueName, const std::wstring& value) {
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
    const LSTATUS status = RegDeleteTreeW(root, subkey.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
           status == ERROR_PATH_NOT_FOUND;
}

bool deleteRegistryValue(HKEY root, const std::wstring& subkey,
                         const wchar_t* valueName) {
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

std::wstring contextMenuDllPathW() {
    const std::wstring root = localAppDataDirectory();
    if (root.empty()) return {};
    return (std::filesystem::path(root) / L"ImView" / L"imview_shell.dll").wstring();
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

    // Avoid rewriting a file that is already the embedded version (Explorer may
    // hold it open). A different size means this is an older/newer extraction.
    const std::filesystem::path filePath(path);
    error.clear();
    if (std::filesystem::is_regular_file(filePath, error) &&
        !error && std::filesystem::file_size(filePath, error) == size && !error) {
        return true;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) != FALSE &&
                    written == size;
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
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

    // Older versions also created an Applications\imview.exe Open With entry.
    // The default-app registration uses ProgIDs and capabilities instead.
    deleteRegistryKey(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Applications\\imview.exe");

    if (images || videos) {
        setRegistryString(HKEY_CURRENT_USER, capabilities, L"ApplicationName", L"ImView");
        setRegistryString(HKEY_CURRENT_USER, capabilities,
                          L"ApplicationDescription", L"Image and video viewer");
        setRegistryString(HKEY_CURRENT_USER, capabilities,
                          L"ApplicationIcon", quoted + L",0");
        setRegistryString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
                          L"ImView", capabilities);
    } else {
        deleteRegistryKey(HKEY_CURRENT_USER, capabilities);
        deleteRegistryValue(HKEY_CURRENT_USER,
                            L"Software\\RegisteredApplications", L"ImView");
    }

    const auto registerProgId = [&](const std::wstring& progId,
                                    const std::wstring& description) {
        const std::wstring key = L"Software\\Classes\\" + progId;
        setRegistryString(HKEY_CURRENT_USER, key, nullptr, description);
        setRegistryString(HKEY_CURRENT_USER, key + L"\\DefaultIcon",
                          nullptr, quoted + L",0");
        setRegistryString(HKEY_CURRENT_USER, key + L"\\shell\\open\\command",
                          nullptr, command);
    };
    const auto removeProgId = [&](const std::wstring& progId) {
        deleteRegistryKey(HKEY_CURRENT_USER,
                          L"Software\\Classes\\" + progId);
    };

    if (images) registerProgId(imageProgId, L"ImView image");
    else removeProgId(imageProgId);
    if (videos) registerProgId(videoProgId, L"ImView video");
    else removeProgId(videoProgId);

    const auto extensionGroup = [&](const std::vector<std::string>& extensions,
                                    bool enabled, const std::wstring& progId) {
        for (const auto& extension : extensions) {
            const std::wstring wideExtension = imview::utf8::widen(extension);
            // Legacy Open With entries are no longer part of the model. Remove
            // them so an upgrade leaves only the default-app registration.
            deleteRegistryValue(
                HKEY_CURRENT_USER,
                L"Software\\Classes\\" + wideExtension +
                    L"\\OpenWithProgids",
                progId.c_str());
            if (enabled) {
                setRegistryString(HKEY_CURRENT_USER, fileAssociations,
                                  wideExtension.c_str(), progId);
            } else {
                deleteRegistryValue(HKEY_CURRENT_USER, fileAssociations,
                                    wideExtension.c_str());
            }
        }
    };
    extensionGroup(imview::media::imageExtensions(), images, imageProgId);
    extensionGroup(imview::media::videoExtensions(), videos, videoProgId);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
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
    setRegistryString(HKEY_CURRENT_USER, clsidKey, nullptr, L"ImView Explorer menu");
    setRegistryString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32",
                      nullptr, wideDll);
    setRegistryString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32",
                      L"ThreadingModel", L"Apartment");
    // The DLL lives under %LOCALAPPDATA%; tell it where the actual exe is.
    setRegistryString(HKEY_CURRENT_USER, contextKey, L"ExePath", executable);
    const wchar_t* handlerKeys[] = {
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ImView",
        L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\ImView",
    };
    for (const wchar_t* key : handlerKeys) {
        setRegistryString(HKEY_CURRENT_USER, key, nullptr, clsid);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

void unregisterContextMenu() {
#if defined(_WIN32)
    deleteRegistryKey(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ImView");
    deleteRegistryKey(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\ImView");
    const std::wstring clsid = guidString(kImViewContextMenuClsid);
    deleteRegistryKey(HKEY_CURRENT_USER,
                      L"Software\\Classes\\CLSID\\" + clsid);
    deleteRegistryKey(HKEY_CURRENT_USER, L"Software\\ImView\\ContextMenu");
    const std::wstring dllPath = contextMenuDllPathW();
    if (!dllPath.empty()) {
        if (DeleteFileW(dllPath.c_str()) == FALSE) {
            // Explorer may still hold the DLL mapped. Schedule removal on the
            // next reboot rather than leaving an untracked file forever.
            MoveFileExW(dllPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        const std::filesystem::path directory =
            std::filesystem::path(dllPath).parent_path();
        std::error_code cleanupError;
        std::filesystem::remove_all(directory, cleanupError);
        if (cleanupError) {
            // The directory is still occupied by the DLL scheduled above.
            // Schedule the empty-directory removal after the reboot as well.
            MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

IntegrationStatus integrationStatus() {
    IntegrationStatus status;
#if defined(_WIN32)
    status.images = registryKeyExists(
        HKEY_CURRENT_USER, L"Software\\Classes\\ImView.Image\\shell\\open\\command");
    status.videos = IMVIEW_ENABLE_VIDEO != 0 && registryKeyExists(
        HKEY_CURRENT_USER, L"Software\\Classes\\ImView.Video\\shell\\open\\command");
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
            !wideDll.empty() && _wcsicmp(registeredDll.c_str(), wideDll.c_str()) == 0;
        const std::wstring registeredExe = registryString(
            HKEY_CURRENT_USER, L"Software\\ImView\\ContextMenu", L"ExePath");
        const std::wstring currentExe = modulePathW();
        const bool exeMatches = !registeredExe.empty() &&
            _wcsicmp(registeredExe.c_str(), currentExe.c_str()) == 0;
        const bool fileHandler = registryString(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ImView",
            nullptr) == clsid;
        const bool folderHandler = registryString(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\ImView",
            nullptr) == clsid;
        status.contextMenu = pathMatches && exeMatches && fileHandler && folderHandler;
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
