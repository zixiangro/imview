#include "exif.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace imview {
namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint16_t readU16(const std::uint8_t* p, bool little) {
    return little ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
                  : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t readU32(const std::uint8_t* p, bool little) {
    return little
        ? (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24))
        : ((static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]));
}

int typeSize(std::uint16_t type) {
    switch (type) {
        case 1: case 2: case 7: return 1; // BYTE, ASCII, UNDEFINED
        case 3: return 2;                 // SHORT
        case 4: case 9: return 4;         // LONG, SLONG
        case 5: case 10: return 8;        // RATIONAL, SRATIONAL
        default: return 0;
    }
}

std::string readAscii(const std::uint8_t* p, std::uint32_t count) {
    std::string text;
    text.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const char ch = static_cast<char>(p[i]);
        if (ch == '\0') break;
        text.push_back(ch);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

double readRational(const std::uint8_t* p, bool little) {
    const std::uint32_t numerator = readU32(p, little);
    const std::uint32_t denominator = readU32(p + 4, little);
    if (denominator == 0) return 0.0;
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

// Walks a TIFF block copied out of the file. Every read is bounds checked
// against that block, so a malformed file cannot escape the buffer.
struct TiffReader {
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;
    bool little = true;

    bool inside(std::size_t offset, std::size_t length) const {
        return base != nullptr && offset <= size && length <= size - offset;
    }

    // Walks one IFD; when isExifSubIfd is false, follows the ExifIFD tag.
    void walk(std::uint32_t ifdOffset, ExifInfo& info, bool isExifSubIfd, int depth) {
        if (depth > 4 || !inside(ifdOffset, 2)) return;
        const std::uint16_t count = readU16(base + ifdOffset, little);
        const std::size_t entriesStart = ifdOffset + 2;
        for (std::uint16_t index = 0; index < count; ++index) {
            const std::size_t entry = entriesStart + static_cast<std::size_t>(index) * 12;
            if (!inside(entry, 12)) return;
            const std::uint16_t tag = readU16(base + entry, little);
            const std::uint16_t type = readU16(base + entry + 2, little);
            const std::uint32_t valueCount = readU32(base + entry + 4, little);
            const int elementSize = typeSize(type);
            if (elementSize == 0 || valueCount == 0) continue;
            const std::size_t byteCount = static_cast<std::size_t>(elementSize) * valueCount;
            const std::uint8_t* value = nullptr;
            if (byteCount <= 4) {
                value = base + entry + 8;
            } else {
                const std::uint32_t valueOffset = readU32(base + entry + 8, little);
                if (!inside(valueOffset, byteCount)) continue;
                value = base + valueOffset;
            }

            if (!isExifSubIfd) {
                switch (tag) {
                    case 0x010F: info.make = readAscii(value, valueCount); break;
                    case 0x0110: info.model = readAscii(value, valueCount); break;
                    case 0x8769:
                        if (type == 4 || type == 3) {
                            walk(readU32(value, little), info, true, depth + 1);
                        }
                        break;
                    default: break;
                }
            } else {
                switch (tag) {
                    case 0x829A: // ExposureTime
                        if (type == 5 || type == 10) info.exposureTime = readRational(value, little);
                        break;
                    case 0x829D: // FNumber
                        if (type == 5 || type == 10) info.fNumber = readRational(value, little);
                        break;
                    case 0x8827: // ISOSpeedRatings
                        if (type == 3) info.iso = readU16(value, little);
                        else if (type == 4) info.iso = static_cast<int>(readU32(value, little));
                        break;
                    case 0x9209: // Flash
                        if (type == 3) info.flash = (readU16(value, little) & 1u) != 0 ? 1 : 0;
                        break;
                    case 0xA434: // LensModel
                        info.lens = readAscii(value, valueCount);
                        break;
                    default: break;
                }
            }
        }
    }

    void parse(ExifInfo& info) {
        if (!inside(0, 8)) return;
        if (base[0] == 'I' && base[1] == 'I') {
            little = true;
        } else if (base[0] == 'M' && base[1] == 'M') {
            little = false;
        } else {
            return;
        }
        if (readU16(base + 2, little) != 42) return;
        walk(readU32(base + 4, little), info, false, 0);
    }
};

// Reads exactly `count` bytes at `offset`; false when the file is shorter.
bool readExact(std::ifstream& file, std::uint64_t offset, void* destination, std::size_t count) {
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) return false;
    file.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(file.gcount()) == count;
}

std::uint32_t readBigEndian32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Locates the TIFF block inside a JPEG by walking the marker segments. The
// payloads are skipped with seeks, so the image data is never read.
bool findJpegExif(std::ifstream& file, std::uint64_t fileSize,
                  std::uint64_t& offset, std::size_t& length) {
    std::uint8_t header[4];
    if (fileSize < 4 || !readExact(file, 0, header, 2)) return false;
    if (header[0] != 0xFF || header[1] != 0xD8) return false;

    std::uint64_t position = 2;
    while (position + 4 <= fileSize) {
        if (!readExact(file, position, header, 4)) return false;
        if (header[0] != 0xFF) return false;
        const std::uint8_t marker = header[1];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            position += 2; // markers without a payload
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return false; // image data reached
        const std::size_t segmentLength =
            (static_cast<std::size_t>(header[2]) << 8) | header[3];
        if (segmentLength < 2) return false;
        if (marker == 0xE1 && segmentLength >= 8) {
            std::uint8_t tag[6];
            if (readExact(file, position + 4, tag, 6) &&
                std::memcmp(tag, "Exif\0\0", 6) == 0) {
                offset = position + 10;
                length = segmentLength - 8;
                return offset + length <= fileSize;
            }
        }
        position += 2 + segmentLength;
    }
    return false;
}

// Locates the "eXIf" chunk payload by walking the chunk list. Only the 8-byte
// chunk headers are read; IDAT payloads are skipped with seeks.
bool findPngExif(std::ifstream& file, std::uint64_t fileSize,
                 std::uint64_t& offset, std::size_t& length) {
    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::uint8_t header[8];
    if (fileSize < 8 || !readExact(file, 0, header, 8)) return false;
    if (std::memcmp(header, kSignature, 8) != 0) return false;

    std::uint64_t position = 8;
    while (position + 12 <= fileSize) {
        if (!readExact(file, position, header, 8)) return false;
        const std::uint32_t chunkLength = readBigEndian32(header);
        if (std::memcmp(header + 4, "eXIf", 4) == 0) {
            if (chunkLength == 0) return false;
            offset = position + 8;
            length = chunkLength;
            return offset + length + 4 <= fileSize; // payload plus CRC
        }
        if (std::memcmp(header + 4, "IEND", 4) == 0) return false;
        position += 12ull + static_cast<std::uint64_t>(chunkLength);
    }
    return false;
}

} // namespace

ExifInfo readExif(const std::string& path) {
    ExifInfo info;
    const std::filesystem::path filePath = std::filesystem::u8path(path);
    std::error_code error;
    const std::uintmax_t fileSize = std::filesystem::file_size(filePath, error);
    if (error || fileSize == 0) return info;
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return info;

    std::uint64_t offset = 0;
    std::size_t length = 0;
    bool found = findJpegExif(file, fileSize, offset, length);
    if (!found) found = findPngExif(file, fileSize, offset, length);
    if (!found) return info;

    Bytes block(length);
    if (!readExact(file, offset, block.data(), block.size())) return info;

    TiffReader reader;
    reader.base = block.data();
    reader.size = block.size();
    reader.parse(info);
    return info;
}

} // namespace imview
