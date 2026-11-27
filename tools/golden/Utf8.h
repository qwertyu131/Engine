#pragma once
// UTF-8 <-> std::filesystem::path helpers. Never use path::string() for user-visible or
// serialized paths: on Windows it converts to the ANSI codepage (e.g. Turkish cp1254) and
// throws or mangles characters outside it.

#include <filesystem>
#include <string>
#include <string_view>

namespace eng::golden {

inline std::filesystem::path fromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string toUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Forward-slash form, for JSON / HTML.
inline std::string toUtf8Generic(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

} // namespace eng::golden
