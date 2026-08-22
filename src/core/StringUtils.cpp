#include "StringUtils.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cwctype>

namespace od::core {

std::wstring StringUtils::Utf8ToWide(std::string_view utf8_str) {
    if (utf8_str.empty()) {
        return L"";
    }

    // Determine required buffer length
    const int required_size = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8_str.data(),
        static_cast<int>(utf8_str.size()),
        nullptr,
        0
    );

    if (required_size <= 0) {
        // Fallback for non-strict UTF8
        const int fallback_size = ::MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            static_cast<int>(utf8_str.size()),
            nullptr,
            0
        );
        if (fallback_size <= 0) return L"";

        std::wstring result(fallback_size, L'\0');
        ::MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8_str.data(),
            static_cast<int>(utf8_str.size()),
            result.data(),
            fallback_size
        );
        return result;
    }

    std::wstring result(required_size, L'\0');
    ::MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8_str.data(),
        static_cast<int>(utf8_str.size()),
        result.data(),
        required_size
    );

    return result;
}

std::string StringUtils::WideToUtf8(std::wstring_view wide_str) {
    if (wide_str.empty()) {
        return "";
    }

    const int required_size = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_str.data(),
        static_cast<int>(wide_str.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (required_size <= 0) {
        return "";
    }

    std::string result(required_size, '\0');
    ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_str.data(),
        static_cast<int>(wide_str.size()),
        result.data(),
        required_size,
        nullptr,
        nullptr
    );

    return result;
}

std::wstring StringUtils::FormatBytes(uint64_t bytes) {
    constexpr double KiB = 1024.0;
    constexpr double MiB = KiB * 1024.0;
    constexpr double GiB = MiB * 1024.0;
    constexpr double TiB = GiB * 1024.0;

    std::wostringstream ss;
    ss << std::fixed << std::setprecision(2);

    if (bytes >= TiB) {
        ss << (bytes / TiB) << L" TB";
    } else if (bytes >= GiB) {
        ss << (bytes / GiB) << L" GB";
    } else if (bytes >= MiB) {
        ss << (bytes / MiB) << L" MB";
    } else if (bytes >= KiB) {
        ss << (bytes / KiB) << L" KB";
    } else {
        ss << bytes << L" B";
    }

    return ss.str();
}

std::wstring StringUtils::FormatSpeed(uint64_t bytes_per_sec) {
    return FormatBytes(bytes_per_sec) + L"/s";
}

std::wstring StringUtils::FormatDuration(uint64_t total_seconds) {
    if (total_seconds == 0) {
        return L"--:--";
    }

    const uint64_t hours = total_seconds / 3600;
    const uint64_t minutes = (total_seconds % 3600) / 60;
    const uint64_t seconds = total_seconds % 60;

    std::wostringstream ss;
    if (hours > 0) {
        ss << hours << L"h " << std::setw(2) << std::setfill(L'0') << minutes << L"m";
    } else {
        ss << std::setw(2) << std::setfill(L'0') << minutes << L"m " 
           << std::setw(2) << std::setfill(L'0') << seconds << L"s";
    }

    return ss.str();
}

bool StringUtils::EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t ca, wchar_t cb) {
        return std::towlower(ca) == std::towlower(cb);
    });
}

std::wstring StringUtils::ExtractFileName(std::wstring_view url_or_path) {
    size_t last_slash = url_or_path.find_last_of(L"/\\");
    if (last_slash != std::wstring_view::npos && last_slash + 1 < url_or_path.size()) {
        std::wstring_view name = url_or_path.substr(last_slash + 1);
        // Strip URL query parameters if present
        size_t query_pos = name.find(L'?');
        if (query_pos != std::wstring_view::npos) {
            name = name.substr(0, query_pos);
        }
        return std::wstring(name);
    }
    return std::wstring(url_or_path);
}

std::wstring StringUtils::Trim(std::wstring_view str) {
    size_t first = 0;
    while (first < str.size() && std::iswspace(str[first])) {
        ++first;
    }
    size_t last = str.size();
    while (last > first && std::iswspace(str[last - 1])) {
        --last;
    }
    return std::wstring(str.substr(first, last - first));
}

} // namespace od::core
