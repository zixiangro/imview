#include "media_paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifndef IMVIEW_ENABLE_VIDEO
#define IMVIEW_ENABLE_VIDEO 1
#endif

namespace imview::media {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

const std::vector<std::string>& imageExtensions() {
    static const std::vector<std::string> extensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif"};
    return extensions;
}

const std::vector<std::string>& videoExtensions() {
#if IMVIEW_ENABLE_VIDEO
    static const std::vector<std::string> extensions = {
        ".mp4", ".m4v", ".mov", ".3gp", ".3g2", ".mkv", ".webm", ".avi",
        ".flv", ".f4v", ".wmv", ".asf", ".ts", ".mts", ".m2ts", ".mpg",
        ".mpeg", ".mpe", ".vob", ".ogv", ".rm", ".rmvb", ".divx"};
    return extensions;
#else
    static const std::vector<std::string> extensions;
    return extensions;
#endif
}

std::string lowerExtension(const std::string& path) {
    // on Windows std::filesystem::path(std::string) interprets the input as
    // ANSI and throws for UTF-8 bytes that do not form a valid ANSI string, so
    // every path coming from a UTF-8 std::string must go through u8path.
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension;
}

bool isImagePath(const std::string& path) {
    return contains(imageExtensions(), lowerExtension(path));
}

bool isVideoPath(const std::string& path) {
    return contains(videoExtensions(), lowerExtension(path));
}

} // namespace imview::media
