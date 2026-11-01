#include "shell_actions.h"

#include "utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

#endif

} // namespace

std::string pickFolder(const std::string& initialDirectory) {
#if defined(_WIN32)
    // IFileOpenDialog with FOS_PICKFOLDERS gives the modern picker; the old
    // SHBrowseForFolder dialog is kept as a fallback.
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string result;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dialog->SetTitle(L"Choose a folder");

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
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
                PWSTR filePath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath))) {
                    result = imview::utf8::narrow(filePath);
                    CoTaskMemFree(filePath);
                }
                item->Release();
            }
        }
        dialog->Release();
    } else {
        BROWSEINFOW browse{};
        browse.lpszTitle = L"Choose a folder";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST idList = SHBrowseForFolderW(&browse);
        if (idList != nullptr) {
            wchar_t filePath[MAX_PATH]{};
            if (SHGetPathFromIDListW(idList, filePath)) {
                result = imview::utf8::narrow(filePath);
            }
            CoTaskMemFree(idList);
        }
    }

    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return result;
#else
    (void)initialDirectory;
    return {};
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
