#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace od::core {

enum class TaskStatus {
    Queued,
    Active,
    Paused,
    Completed,
    Error,
    Cancelled
};

/**
 * @brief Represents a single download task item in the application.
 * 
 * Thread-safe structure holding metrics, progress, connection counts,
 * and current transfer speeds.
 */
struct DownloadTask {
    std::wstring gid;
    std::wstring url;
    std::wstring fileName;
    std::wstring destinationPath;

    TaskStatus status{TaskStatus::Queued};
    uint64_t totalBytes{0};
    uint64_t completedBytes{0};
    uint64_t downloadSpeed{0};  // bytes per second
    uint32_t activeConnections{0};
    uint32_t totalSplits{10};

    std::wstring errorMessage;
    std::chrono::steady_clock::time_point startTime;

    [[nodiscard]] double GetProgressPercent() const {
        if (totalBytes == 0) return 0.0;
        return (static_cast<double>(completedBytes) / static_cast<double>(totalBytes)) * 100.0;
    }

    [[nodiscard]] uint64_t GetRemainingSeconds() const {
        if (downloadSpeed == 0 || completedBytes >= totalBytes) return 0;
        return (totalBytes - completedBytes) / downloadSpeed;
    }

    [[nodiscard]] std::wstring GetStatusString() const {
        switch (status) {
            case TaskStatus::Queued:    return L"Queued";
            case TaskStatus::Active:    return L"Downloading";
            case TaskStatus::Paused:    return L"Paused";
            case TaskStatus::Completed: return L"Completed";
            case TaskStatus::Error:     return L"Error: " + errorMessage;
            case TaskStatus::Cancelled: return L"Cancelled";
            default:                    return L"Unknown";
        }
    }
};

} // namespace od::core
