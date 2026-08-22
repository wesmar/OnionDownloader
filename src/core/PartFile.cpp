#include "PartFile.h"

#include "StringUtils.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <winioctl.h>

namespace od::core {

namespace {

void Fail(std::string* errorOut, std::string message) {
    if (errorOut != nullptr) {
        *errorOut = std::move(message);
    }
}

std::string LastErrorText() {
    return "Windows error " + std::to_string(::GetLastError());
}

} // namespace

PartFile::Writer::~Writer() {
    if (m_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_handle);
    }
}

bool PartFile::Writer::WriteAt(uint64_t offset, const void* data, size_t length) {
    if (m_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(m_handle, position, nullptr, FILE_BEGIN)) {
        return false;
    }

    const auto* cursor = static_cast<const char*>(data);
    size_t remaining = length;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!::WriteFile(m_handle, cursor, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

PartFile::~PartFile() {
    Close();
}

std::filesystem::path PartFile::MetaPath() const {
    return m_finalPath.wstring() + L".odmeta";
}

std::wstring PartFile::SanitizeFileName(const std::wstring& candidate) {
    static constexpr std::wstring_view kIllegal = L"<>:\"/\\|?*";

    std::wstring name;
    name.reserve(candidate.size());
    for (wchar_t ch : candidate) {
        if (ch < 32 || kIllegal.find(ch) != std::wstring_view::npos) {
            name.push_back(L'_');
        } else {
            name.push_back(ch);
        }
    }

    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) {
        name.pop_back();
    }
    if (name.empty()) {
        name = L"download.bin";
    }
    if (name.size() > 180) {
        name.resize(180);
    }
    return name;
}

bool PartFile::Open(const std::filesystem::path& finalPath, uint64_t totalBytes, std::string* errorOut) {
    Close();

    m_finalPath = finalPath;
    m_partPath = finalPath.wstring() + L".odpart";

    std::error_code ec;
    std::filesystem::create_directories(m_finalPath.parent_path(), ec);

    m_primary = ::CreateFileW(
        m_partPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (m_primary == INVALID_HANDLE_VALUE) {
        Fail(errorOut, "cannot create " + m_partPath.string() + " (" + LastErrorText() + ")");
        return false;
    }

    LARGE_INTEGER currentSize{};
    ::GetFileSizeEx(m_primary, &currentSize);
    if (static_cast<uint64_t>(currentSize.QuadPart) != totalBytes) {
        // Sparse allocation: reserving the full extent is instant and the file
        // system only commits clusters that actually receive data.
        DWORD returned = 0;
        ::DeviceIoControl(m_primary, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);

        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(totalBytes);
        if (!::SetFilePointerEx(m_primary, target, nullptr, FILE_BEGIN) || !::SetEndOfFile(m_primary)) {
            Fail(errorOut, "cannot preallocate output file (" + LastErrorText() + ")");
            Close();
            return false;
        }
    }

    return true;
}

std::unique_ptr<PartFile::Writer> PartFile::CreateWriter(std::string* errorOut) const {
    HANDLE handle = ::CreateFileW(
        m_partPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        Fail(errorOut, "cannot open output file for writing (" + LastErrorText() + ")");
        return nullptr;
    }
    return std::make_unique<Writer>(handle);
}

bool PartFile::Finalize(std::string* errorOut) {
    if (m_primary != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(m_primary);
        ::CloseHandle(m_primary);
        m_primary = INVALID_HANDLE_VALUE;
    }

    std::error_code ec;
    std::filesystem::remove(MetaPath(), ec);

    std::filesystem::remove(m_finalPath, ec);
    std::filesystem::rename(m_partPath, m_finalPath, ec);
    if (ec) {
        Fail(errorOut, "cannot rename partial file to " + m_finalPath.string());
        return false;
    }
    return true;
}

void PartFile::Close() {
    if (m_primary != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_primary);
        m_primary = INVALID_HANDLE_VALUE;
    }
}

bool PartFile::LoadResume(const std::filesystem::path& finalPath, ResumeRecord& out) {
    std::ifstream file(finalPath.wstring() + L".odmeta");
    if (!file) {
        return false;
    }

    std::string line;
    if (!std::getline(file, line) || line.rfind("OD-RESUME 1", 0) != 0) {
        return false;
    }

    ResumeRecord record;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);

        if (key == "url") {
            record.url = value;
        } else if (key == "total") {
            record.totalBytes = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "segment") {
            record.segmentSize = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "bitmap") {
            record.bitmap = value;
        }
    }

    if (record.totalBytes == 0 || record.segmentSize == 0) {
        return false;
    }
    out = std::move(record);
    return true;
}

bool PartFile::SaveResume(const std::filesystem::path& finalPath, const ResumeRecord& record) {
    static std::mutex resumeWriteMutex;
    std::lock_guard<std::mutex> writeLock(resumeWriteMutex);
    const std::filesystem::path metaPath = finalPath.wstring() + L".odmeta";
    const std::filesystem::path temporaryPath = finalPath.wstring() + L".odmeta.tmp";
    std::ofstream file(temporaryPath, std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "OD-RESUME 1\n"
         << "url=" << record.url << "\n"
         << "total=" << record.totalBytes << "\n"
         << "segment=" << record.segmentSize << "\n"
         << "bitmap=" << record.bitmap << "\n";
    file.flush();
    if (!file.good()) {
        return false;
    }
    file.close();

    // A crash must leave either the old complete record or the new one, never
    // a truncated bitmap. ReplaceFile is atomic on the same Windows volume.
    if (::ReplaceFileW(metaPath.c_str(), temporaryPath.c_str(), nullptr,
                       REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return true;
    }
    if (::GetLastError() == ERROR_FILE_NOT_FOUND &&
        ::MoveFileExW(temporaryPath.c_str(), metaPath.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove(temporaryPath, ec);
    return false;
}

void PartFile::Discard(const std::filesystem::path& finalPath) {
    std::error_code ec;
    std::filesystem::remove(finalPath.wstring() + L".odpart", ec);
    std::filesystem::remove(finalPath.wstring() + L".odmeta", ec);
    std::filesystem::remove(finalPath.wstring() + L".odmeta.tmp", ec);
}

} // namespace od::core
