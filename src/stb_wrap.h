#pragma once

#include <string>
#include <vector>

namespace imview {

// Decodes an image file (PNG/JPEG/BMP/GIF/TGA/...) into RGBA8, top-down rows.
// Returns false on failure; reason describes the decode error.
bool decodeRgba(const std::string& path, int& width, int& height,
                std::vector<unsigned char>& pixels, std::string& reason);

} // namespace imview
