#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <windows.h>

namespace od::core {

/// On-disk resume state written next to the partial file.
struct ResumeRecord {
    std::string url;
    uint64_t totalBytes{0};
    uint64_t segmentSize{0};
    std::string bitmap;
};

/**
 * @brief Owns the partial output file, its preallocation and its resume sidecar.
 *
 * CONCURRENCY MODEL:
 * Each worker receives its own Writer, i.e. its own kernel file handle on the
 * same path. Positional writes to disjoint ranges then need no user-space lock -
 * the file system already serialises them - which keeps ten circuits writing at
 * full speed without a global mutex in the hot path.
 *
 * The file is created sparse so that reserving 130 MB up front costs no time and
 * no zero-fill I/O; the extent materialises as the segments land.
 */
class PartFile {
public:
    class Writer {
    public:
        explicit Writer(HANDLE handle) noexcept : m_handle(handle) {}
        ~Writer();

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        bool WriteAt(uint64_t offset, const void* data, size_t length);

    private:
        HANDLE m_handle{INVALID_HANDLE_VALUE};
    };

    PartFile() = default;
    ~PartFile();

    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;

    /// Create (or reopen) `<final>.odpart`, sized to totalBytes.
    bool Open(const std::filesystem::path& finalPath, uint64_t totalBytes, std::string* errorOut);

    /// Open an additional handle for one worker.
    [[nodiscard]] std::unique_ptr<Writer> CreateWriter(std::string* errorOut) const;

    /// Flush, close, drop the sidecar and move the partial file into place.
    bool Finalize(std::string* errorOut);

    void Close();

    [[nodiscard]] const std::filesystem::path& PartPath() const noexcept { return m_partPath; }
    [[nodiscard]] const std::filesystem::path& FinalPath() const noexcept { return m_finalPath; }
    [[nodiscard]] std::filesystem::path MetaPath() const;

    /// Read the resume sidecar belonging to `finalPath`.
    static bool LoadResume(const std::filesystem::path& finalPath, ResumeRecord& out);

    /// Write the resume sidecar belonging to `finalPath`.
    static bool SaveResume(const std::filesystem::path& finalPath, const ResumeRecord& record);

    /// Remove both the partial file and its sidecar.
    static void Discard(const std::filesystem::path& finalPath);

    /// Turn a URL path component into a file name that is legal on Windows.
    static std::wstring SanitizeFileName(const std::wstring& candidate);

private:
    std::filesystem::path m_finalPath;
    std::filesystem::path m_partPath;
    HANDLE m_primary{INVALID_HANDLE_VALUE};
};

} // namespace od::core
