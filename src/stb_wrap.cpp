// Isolated stb_image implementation with internal linkage, so the app never
// depends on (or collides with) the symbols compiled into the EUI-NEO library.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "3rd/stb_image.h"

#include "stb_wrap.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace imview {
namespace {

constexpr std::size_t kMaxDimension = 32768; // stb_image's own limit

// Hands the decoder the file contents. On Windows the file is mapped instead of
// copied, so the encode/decode peak is the decoded image plus one mapped view
// rather than twice the file plus the image.
class FileBytes {
public:
    explicit FileBytes(const std::string& path) {
#if defined(_WIN32)
        const std::filesystem::path filePath = std::filesystem::u8path(path);
        file_ = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = nullptr;
            return;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 ||
            static_cast<std::uint64_t>(size.QuadPart) >
                std::numeric_limits<std::size_t>::max()) {
            return;
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) return;
        void* view = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) return;
        data_ = static_cast<const unsigned char*>(view);
        size_ = static_cast<std::size_t>(size.QuadPart);
#else
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file) return;
        buffer_.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
        data_ = buffer_.data();
        size_ = buffer_.size();
#endif
    }

    ~FileBytes() {
#if defined(_WIN32)
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != nullptr) CloseHandle(file_);
#endif
    }

    FileBytes(const FileBytes&) = delete;
    FileBytes& operator=(const FileBytes&) = delete;

    const unsigned char* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool valid() const { return data_ != nullptr && size_ > 0; }

private:
#if defined(_WIN32)
    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;
#else
    std::vector<unsigned char> buffer_;
#endif
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace

bool decodeRgba(const std::string& path, int& width, int& height,
                std::vector<unsigned char>& pixels, std::string& reason) {
    width = 0;
    height = 0;
    pixels.clear();
    reason.clear();

    const FileBytes bytes(path);
    if (!bytes.valid()) {
        reason = "cannot open file";
        return false;
    }
    // stb_image takes an int length; anything larger is not a viewer-sized file.
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        reason = "file too large";
        return false;
    }

    int channels = 0;
    unsigned char* data = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels,
        STBI_rgb_alpha);
    if (data == nullptr) {
        reason = stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                  : "decode failed";
        return false;
    }
    if (width <= 0 || height <= 0 || width > static_cast<int>(kMaxDimension) ||
        height > static_cast<int>(kMaxDimension)) {
        stbi_image_free(data);
        width = 0;
        height = 0;
        reason = "unsupported size";
        return false;
    }

    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    pixels.assign(data, data + count);
    stbi_image_free(data);
    return true;
}

} // namespace imview
