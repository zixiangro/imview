// Logic tests for the parts that do not need a window: UTF-8 helpers, the EXIF
// reader and the image decoder. Fixtures are generated in code, so the
// repository carries no binary test data.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "exif.h"
#include "stb_wrap.h"
#include "utf8.h"

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const char* what, const char* file, int line) {
    ++gChecks;
    if (condition) return;
    ++gFailures;
    std::printf("FAIL %s:%d  %s\n", file, line, what);
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

// --- fixtures -----------------------------------------------------------------

std::filesystem::path tempPath(const char* name) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    return path;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void appendBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

// Little-endian TIFF block: IFD0 with Make + ExifIFD pointer, then the Exif IFD
// with aperture, shutter, ISO, flash and lens. Values wider than four bytes live
// after both IFDs, which exercises the offset path.
std::vector<std::uint8_t> makeTiffBlock() {
    const char make[] = "Canon";
    const char lens[] = "RF24-70mm F2.8 L IS USM";

    // Data area: the offsets are recorded while building it.
    std::vector<std::uint8_t> data;
    const std::uint32_t makeOffset = static_cast<std::uint32_t>(data.size());
    data.insert(data.end(), make, make + sizeof(make));
    while (data.size() % 4 != 0) data.push_back(0);
    const std::uint32_t fNumberOffset = static_cast<std::uint32_t>(data.size());
    appendLe32(data, 28); // f/2.8
    appendLe32(data, 10);
    const std::uint32_t exposureOffset = static_cast<std::uint32_t>(data.size());
    appendLe32(data, 1); // 1/250 s
    appendLe32(data, 250);
    const std::uint32_t lensOffset = static_cast<std::uint32_t>(data.size());
    data.insert(data.end(), lens, lens + sizeof(lens));

    constexpr std::uint32_t kIfd0Offset = 8;
    constexpr std::uint16_t kIfd0Entries = 2;
    constexpr std::uint32_t kExifIfdOffset = kIfd0Offset + 2 + kIfd0Entries * 12 + 4;
    constexpr std::uint16_t kExifEntries = 5;
    const std::uint32_t dataOffset = kExifIfdOffset + 2 + kExifEntries * 12 + 4;

    std::vector<std::uint8_t> block;
    block.push_back('I');
    block.push_back('I');
    appendLe16(block, 42);
    appendLe32(block, kIfd0Offset);

    // IFD0
    appendLe16(block, kIfd0Entries);
    appendLe16(block, 0x010F);                     // Make
    appendLe16(block, 2);                          // ASCII
    appendLe32(block, sizeof(make));               // includes the terminator
    appendLe32(block, dataOffset + makeOffset);
    appendLe16(block, 0x8769);                     // ExifIFD pointer
    appendLe16(block, 4);                          // LONG
    appendLe32(block, 1);
    appendLe32(block, kExifIfdOffset);
    appendLe32(block, 0);                          // no next IFD

    // Exif IFD
    appendLe16(block, kExifEntries);
    appendLe16(block, 0x829D);                     // FNumber
    appendLe16(block, 5);                          // RATIONAL
    appendLe32(block, 1);
    appendLe32(block, dataOffset + fNumberOffset);
    appendLe16(block, 0x829A);                     // ExposureTime
    appendLe16(block, 5);
    appendLe32(block, 1);
    appendLe32(block, dataOffset + exposureOffset);
    appendLe16(block, 0x8827);                     // ISO
    appendLe16(block, 3);                          // SHORT, inline value
    appendLe32(block, 1);
    appendLe16(block, 400);
    appendLe16(block, 0);
    appendLe16(block, 0x9209);                     // Flash
    appendLe16(block, 3);
    appendLe32(block, 1);
    appendLe16(block, 1);
    appendLe16(block, 0);
    appendLe16(block, 0xA434);                     // LensModel
    appendLe16(block, 2);
    appendLe32(block, sizeof(lens));
    appendLe32(block, dataOffset + lensOffset);
    appendLe32(block, 0);                          // no next IFD

    block.insert(block.end(), data.begin(), data.end());
    return block;
}

std::vector<std::uint8_t> makeJpegWithExif() {
    const std::vector<std::uint8_t> tiff = makeTiffBlock();
    std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE1};
    const std::size_t segmentLength = 2 + 6 + tiff.size();
    jpeg.push_back(static_cast<std::uint8_t>(segmentLength >> 8));
    jpeg.push_back(static_cast<std::uint8_t>(segmentLength & 0xFF));
    const char header[6] = {'E', 'x', 'i', 'f', '\0', '\0'};
    jpeg.insert(jpeg.end(), header, header + 6);
    jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
    // Image data marker so the walker has somewhere sane to stop.
    jpeg.insert(jpeg.end(), {0xFF, 0xDA, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xD9});
    return jpeg;
}

std::vector<std::uint8_t> makePngWithExif() {
    const std::vector<std::uint8_t> tiff = makeTiffBlock();
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    // IHDR with a 1x1 8-bit RGB image; the reader never looks inside it.
    appendBe32(png, 13);
    png.insert(png.end(), {'I', 'H', 'D', 'R'});
    std::vector<std::uint8_t> ihdr;
    appendBe32(ihdr, 1);
    appendBe32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png.insert(png.end(), ihdr.begin(), ihdr.end());
    appendBe32(png, 0); // CRC, not validated

    // IDAT before eXIf: the reader must skip the payload to find the chunk.
    appendBe32(png, 4);
    png.insert(png.end(), {'I', 'D', 'A', 'T'});
    png.insert(png.end(), {1, 2, 3, 4});
    appendBe32(png, 0);

    appendBe32(png, static_cast<std::uint32_t>(tiff.size()));
    png.insert(png.end(), {'e', 'X', 'I', 'f'});
    png.insert(png.end(), tiff.begin(), tiff.end());
    appendBe32(png, 0);

    appendBe32(png, 0);
    png.insert(png.end(), {'I', 'E', 'N', 'D'});
    appendBe32(png, 0);
    return png;
}

// 2x2 24-bit BMP, bottom-up: top row red/green, bottom row blue/white.
std::vector<std::uint8_t> makeBmp() {
    std::vector<std::uint8_t> bmp;
    const std::uint32_t pixelOffset = 54;
    const std::uint32_t rowBytes = 8; // 2 px * 3 bytes, padded to 4
    const std::uint32_t imageBytes = rowBytes * 2;
    const std::uint32_t fileSize = pixelOffset + imageBytes;
    bmp.insert(bmp.end(), {'B', 'M'});
    appendLe32(bmp, fileSize);
    appendLe16(bmp, 0);
    appendLe16(bmp, 0);
    appendLe32(bmp, pixelOffset);
    appendLe32(bmp, 40);      // DIB header size
    appendLe32(bmp, 2);       // width
    appendLe32(bmp, 2);       // height (positive: bottom-up)
    appendLe16(bmp, 1);       // planes
    appendLe16(bmp, 24);      // bits per pixel
    appendLe32(bmp, 0);       // no compression
    appendLe32(bmp, imageBytes);
    appendLe32(bmp, 2835);
    appendLe32(bmp, 2835);
    appendLe32(bmp, 0);
    appendLe32(bmp, 0);
    // Bottom row: blue, white. BMP stores BGR.
    bmp.insert(bmp.end(), {0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00});
    // Top row: red, green.
    bmp.insert(bmp.end(), {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00});
    return bmp;
}

// --- tests --------------------------------------------------------------------

void testUtf8() {
    CHECK(imview::utf8::isValid("plain ascii"));
    CHECK(imview::utf8::isValid("\xE4\xB8\xAD\xE6\x96\x87"));      // 中文
    CHECK(imview::utf8::isValid("a\xF0\x9F\x98\x80" "b"));         // emoji
    CHECK(!imview::utf8::isValid("\xE4\xB8"));                     // truncated
    CHECK(!imview::utf8::isValid("\xFF\xFE"));                     // not UTF-8
#if defined(_WIN32)
    CHECK(imview::utf8::narrow(L"\u4E2D\u6587") == "\xE4\xB8\xAD\xE6\x96\x87");
    const std::wstring wide = imview::utf8::widen("\xE4\xB8\xAD\xE6\x96\x87");
    CHECK(wide == L"\u4E2D\u6587");
    CHECK(imview::utf8::widen("") .empty());
    CHECK(imview::utf8::narrow(nullptr).empty());
#endif
}

void testExifJpeg() {
    const std::filesystem::path path = tempPath("imview_test_exif.jpg");
    writeBytes(path, makeJpegWithExif());
    const imview::ExifInfo info = imview::readExif(path.u8string());
    CHECK(info.make == "Canon");
    CHECK(info.lens == "RF24-70mm F2.8 L IS USM");
    CHECK(info.iso == 400);
    CHECK(info.flash == 1);
    CHECK(info.fNumber > 2.79 && info.fNumber < 2.81);
    CHECK(info.exposureTime > 0.0039 && info.exposureTime < 0.0041);
    CHECK(info.hasCamera());
    CHECK(info.any());
    std::filesystem::remove(path);
}

void testExifPng() {
    const std::filesystem::path path = tempPath("imview_test_exif.png");
    writeBytes(path, makePngWithExif());
    const imview::ExifInfo info = imview::readExif(path.u8string());
    CHECK(info.make == "Canon");
    CHECK(info.iso == 400);
    CHECK(info.fNumber > 2.79 && info.fNumber < 2.81);
    std::filesystem::remove(path);
}

void testExifMissing() {
    const std::filesystem::path garbage = tempPath("imview_test_garbage.bin");
    writeBytes(garbage, {1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(!imview::readExif(garbage.u8string()).any());
    std::filesystem::remove(garbage);

    // A JPEG with no APP1 payload must not report anything.
    const std::filesystem::path plain = tempPath("imview_test_plain.jpg");
    writeBytes(plain, {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xD9});
    CHECK(!imview::readExif(plain.u8string()).any());
    std::filesystem::remove(plain);

    CHECK(!imview::readExif(tempPath("imview_does_not_exist.jpg").u8string()).any());
}

void testDecode() {
    const std::filesystem::path path = tempPath("imview_test.bmp");
    writeBytes(path, makeBmp());
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
    std::string reason;
    CHECK(imview::decodeRgba(path.u8string(), width, height, pixels, reason));
    CHECK(width == 2);
    CHECK(height == 2);
    CHECK(pixels.size() == 16);
    if (pixels.size() == 16) {
        // Top-left is red, top-right green, bottom-left blue, bottom-right white.
        CHECK(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0);
        CHECK(pixels[4] == 0 && pixels[5] == 255 && pixels[6] == 0);
        CHECK(pixels[8] == 0 && pixels[9] == 0 && pixels[10] == 255);
        CHECK(pixels[12] == 255 && pixels[13] == 255 && pixels[14] == 255);
    }
    std::filesystem::remove(path);

    CHECK(!imview::decodeRgba(tempPath("imview_missing.png").u8string(), width, height,
                              pixels, reason));
    CHECK(!reason.empty());
    CHECK(pixels.empty());
    CHECK(width == 0 && height == 0);
}

} // namespace

int main() {
    testUtf8();
    testExifJpeg();
    testExifPng();
    testExifMissing();
    testDecode();

    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
