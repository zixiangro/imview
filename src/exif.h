#pragma once

#include <string>

namespace imview {

// Minimal EXIF reader for JPEG (APP1) and PNG (eXIf) files.
struct ExifInfo {
    std::string make;
    std::string model;
    std::string lens;
    double fNumber = 0.0;      // aperture, e.g. 2.8
    double exposureTime = 0.0; // seconds
    int iso = 0;
    int flash = -1;            // -1 unknown, 0 off, 1 on

    bool hasCamera() const { return !make.empty() || !model.empty(); }
    bool any() const {
        return hasCamera() || !lens.empty() || fNumber > 0.0 || exposureTime > 0.0 ||
               iso > 0 || flash >= 0;
    }
};

// Returns an empty result when the file has no (or unparsable) EXIF.
ExifInfo readExif(const std::string& path);

} // namespace imview
