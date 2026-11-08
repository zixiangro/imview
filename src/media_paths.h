#pragma once

#include <string>
#include <vector>

namespace imview::media {

const std::vector<std::string>& imageExtensions();
const std::vector<std::string>& videoExtensions();

std::string lowerExtension(const std::string& path);
bool isImagePath(const std::string& path);
bool isVideoPath(const std::string& path);

} // namespace imview::media
