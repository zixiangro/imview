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

// Directory that holds the running executable.
std::string executableDirectory();

// Runs an external EXIF helper, if `imexif`(.exe/.bat/.cmd) exists next to the
// executable or in the working directory, and returns its stdout as lines.
// Returns false when no helper is available or it produced no output.
bool readExternalExif(const std::string& imagePath, std::vector<std::string>& lines);

} // namespace imview::shell
