#pragma once

// UTF-8 <-> UTF-16 helpers. Windows APIs speak UTF-16, everything inside the
// application speaks UTF-8, so both directions appear in several places.

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace imview::utf8 {

// True when the bytes form a well-formed UTF-8 sequence.
inline bool isValid(const std::string& text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[index + k]) & 0xC0) != 0x80) return false;
        }
        index += length;
    }
    return true;
}

#if defined(_WIN32)

// UTF-8 -> UTF-16. An empty result means the input was empty or invalid.
inline std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return {};
    // The wide buffer includes the terminator, so size it for `length` and trim.
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<std::size_t>(length) - 1);
    return wide;
}

// UTF-16 -> UTF-8, ignoring the terminator.
inline std::string narrow(const wchar_t* text) {
    if (text == nullptr) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string narrowText(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrowText.data(), length, nullptr, nullptr);
    narrowText.resize(static_cast<std::size_t>(length) - 1);
    return narrowText;
}

// Re-decodes bytes from a specific Windows code page. Text that is already
// valid UTF-8 is returned unchanged.
inline std::string fromCodePage(const std::string& raw, UINT codePage) {
    if (raw.empty() || isValid(raw)) return raw;
    const int wideLength = MultiByteToWideChar(codePage, 0, raw.data(),
                                               static_cast<int>(raw.size()), nullptr, 0);
    if (wideLength <= 0) return raw;
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(codePage, 0, raw.data(), static_cast<int>(raw.size()),
                        wide.data(), wideLength);
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
                                               nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return raw;
    std::string out(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(), utf8Length,
                        nullptr, nullptr);
    return out;
}

// Re-decodes bytes that came from a console code page (GBK on Chinese Windows).
// Text that is already valid UTF-8 is returned unchanged.
inline std::string fromConsoleCodePage(const std::string& raw) {
    return fromCodePage(raw, GetOEMCP());
}

// Re-decodes legacy single-byte/multi-byte text using the ANSI code page.
inline std::string fromAnsiCodePage(const std::string& raw) {
    return fromCodePage(raw, CP_ACP);
}

#endif

} // namespace imview::utf8
