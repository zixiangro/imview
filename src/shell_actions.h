#pragma once

#include <string>
#include <vector>

namespace imview::shell {

// Native folder picker. Returns an empty string when the user cancels.
std::string pickFolder(const std::string& initialDirectory = {});

// Native folder picker with multi-selection. Returns an empty list when the
// user cancels.
std::vector<std::string> pickFolders(const std::string& initialDirectory = {});

// Opens a URL with the default browser.
bool openUrl(const std::string& url);

// Opens Explorer with the file selected.
bool showInExplorer(const std::string& path);

// Shows the system "Open with" chooser for the file.
bool openWith(const std::string& path);

// Shows the shell file properties dialog.
bool showProperties(const std::string& path);

// On-demand Explorer integration. Nothing is registered at startup; the About
// dialog exposes these switches. Image/video registration writes HKCU only as a
// Default Apps and Open With candidate and never changes the user's explicit
// default choice. The context-menu switch registers the small imview_shell.dll
// handler for images, videos and folders;
// it appears for a single folder, two to four images, two to four videos, or one
// to two folders. The helper is extracted under %LOCALAPPDATA%\ImView\ShellExt
// with a content-hashed file name.
struct IntegrationStatus {
    bool images = false;
    bool videos = false;
    bool contextMenu = false;
    bool contextMenuAvailable = false;
};

bool registerImageAssociations();
bool unregisterImageAssociations();
bool registerVideoAssociations();
bool unregisterVideoAssociations();
bool registerIntegration();
bool registerContextMenu();
bool unregisterContextMenu();
bool uninstallIntegration();
IntegrationStatus integrationStatus();

// Directory that holds the running executable.
std::string executableDirectory();

// Runs an external EXIF helper, if `imexif`(.exe/.bat/.cmd) exists next to the
// executable or in the working directory, and returns its stdout as lines.
// Returns false when no helper is available or it produced no output.
bool readExternalExif(const std::string& imagePath, std::vector<std::string>& lines);

} // namespace imview::shell
