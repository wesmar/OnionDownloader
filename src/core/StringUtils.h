#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace od::core {

/**
 * @brief High-efficiency UTF-8 <-> UTF-16 string conversion and formatting utilities.
 * 
 * WinAPI natively operates on UTF-16 (wchar_t / LPCWSTR) whereas modern network protocols
 * and Aria2/JSON APIs operate in UTF-8. These utilities guarantee zero-data-loss conversion
 * and exception-safe memory allocations.
 */
class StringUtils {
public:
    /// Convert UTF-8 string_view to Windows UTF-16 std::wstring
    [[nodiscard]] static std::wstring Utf8ToWide(std::string_view utf8_str);

    /// Convert Windows UTF-16 wstring_view to UTF-8 std::string
    [[nodiscard]] static std::string WideToUtf8(std::wstring_view wide_str);

    /// Format byte size into human-readable string (e.g. 1.25 MB, 9.45 GB)
    [[nodiscard]] static std::wstring FormatBytes(uint64_t bytes);

    /// Format transfer speed (e.g. 4.52 MB/s)
    [[nodiscard]] static std::wstring FormatSpeed(uint64_t bytes_per_sec);

    /// Format seconds into readable ETA (e.g. 02h 14m 30s)
    [[nodiscard]] static std::wstring FormatDuration(uint64_t total_seconds);

    /// Case-insensitive string comparison
    [[nodiscard]] static bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);

    /// Extract filename component from a path or URL
    [[nodiscard]] static std::wstring ExtractFileName(std::wstring_view url_or_path);

    /// Trim leading and trailing whitespace
    [[nodiscard]] static std::wstring Trim(std::wstring_view str);
};

} // namespace od::core
