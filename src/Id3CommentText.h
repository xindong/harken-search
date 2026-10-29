#pragma once

#include "platform/metadata_legacy_tag_content.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Id3CommentText {

// The COMM body begins with a three-byte language, then a short descriptor,
// its terminator, and finally the text the user should see.
inline std::string Decode(uint8_t encoding, std::string_view body,
                          bool recoverTruncatedAsciiTail = false) {
    if (body.size() < 3) return {};
    size_t pos = 3;
    if (encoding == 0 || encoding == 3) {
        const size_t separator = body.find('\0', pos);
        if (separator == std::string_view::npos) return {};
        const std::string_view value = body.substr(separator + 1);
        return encoding == 0
            ? Platform::Metadata::LegacyTagContent::DecodeId3v2Encoding0(value)
            : std::string(value);
    }
    if (encoding != 1 && encoding != 2) return {};

    using Order = Platform::Metadata::LegacyTagContent::Utf16ByteOrder;
    Order order = encoding == 2 ? Order::BigEndian : Order::LittleEndian;
    auto skipBom = [&]() {
        if (pos + 1 >= body.size()) return;
        const auto a = static_cast<uint8_t>(body[pos]);
        const auto b = static_cast<uint8_t>(body[pos + 1]);
        if (a == 0xFF && b == 0xFE) { order = Order::LittleEndian; pos += 2; }
        else if (a == 0xFE && b == 0xFF) { order = Order::BigEndian; pos += 2; }
    };
    skipBom();
    while (pos + 1 < body.size()) {
        if (body[pos] == '\0' && body[pos + 1] == '\0') { pos += 2; break; }
        pos += 2;
    }
    if (pos > body.size() || pos + 1 >= body.size()) return {};
    skipBom();
    std::string decoded = Platform::Metadata::LegacyTagContent::DecodeUtf16(
        reinterpret_cast<const uint8_t*>(body.data() + pos), body.size() - pos, order);
    // Some old database rows lost only the final zero high byte of a UTF-16LE
    // ASCII character. Do not apply this repair while reading intact audio tags.
    if (recoverTruncatedAsciiTail && order == Order::LittleEndian &&
        (body.size() - pos) % 2 != 0) {
        const auto tail = static_cast<uint8_t>(body.back());
        if (tail >= 0x20 && tail <= 0x7E) decoded.push_back(static_cast<char>(tail));
    }
    return decoded;
}

// A historical importer stored the raw bytes after COMM's encoding byte.
// Only recognize the exact UTF-16 BOM signature observed in those rows.
inline std::optional<std::string> RecoverStoredLegacy(std::string_view stored) {
    if (stored.size() < 9) return std::nullopt;
    for (size_t i = 0; i < 3; ++i) {
        if (stored[i] < 'a' || stored[i] > 'z') return std::nullopt;
    }
    const auto a = static_cast<uint8_t>(stored[3]);
    const auto b = static_cast<uint8_t>(stored[4]);
    if (!((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF))) return std::nullopt;
    std::string decoded = Decode(1, stored, true);
    if (decoded.empty()) return std::nullopt;
    return decoded;
}

} // namespace Id3CommentText
