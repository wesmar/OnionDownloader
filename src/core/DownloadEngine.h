#pragma once

#include "DownloadTask.h"
#include "PartFile.h"
#include "SegmentMap.h"
#include "TorPool.h"
#include "../net/HttpClient.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace od::core {

/**
 * @brief Multi-circuit download orchestrator.
 *
 * One job owns one output file. Its controller thread probes the resource,
 * prepares the segment plan, then starts N worker threads. Each worker holds a
 * private HTTP connection pinned to a private Tor circuit (own daemon from the
 * pool, own SOCKS isolation credential) and pulls segments from the shared plan
 * until the file is complete. A worker that loses its circuit rotates its
 * credential, which forces Tor to build a fresh path, and returns its unfinished
 * segment to the pool - no other worker is affected.
 */
class DownloadEngine {
public:
    using UpdateCallback = std::function<void(const DownloadTask&)>;
    using LogCallback = std::function<void(const std::wstring&)>;

    explicit DownloadEngine(TorPool& torPool);
    ~DownloadEngine();

    DownloadEngine(const DownloadEngine&) = delete;
    DownloadEngine& operator=(const DownloadEngine&) = delete;

    /// Queue a new download (or restart an existing one with the same URL).
    bool AddDownload(const std::wstring& url, const std::wstring& destinationDir, uint32_t workers);

    /// Stop the workers of a running job, keeping everything already on disk.
    bool Pause(const std::wstring& gid);

    /// Restart a paused, failed or restored job from its resume metadata.
    bool Resume(const std::wstring& gid);

    /// Stop a job and forget it; optionally delete the partial file.
    bool Remove(const std::wstring& gid, bool deletePartialFile);

    void SetUpdateCallback(UpdateCallback callback);
    void SetLogCallback(LogCallback callback);

    [[nodiscard]] std::vector<DownloadTask> GetAllTasks() const;
    [[nodiscard]] bool TryGetTask(const std::wstring& gid, DownloadTask& out) const;

    void SaveSession();
    void Shutdown();

    /// Stable identifier derived from the URL, so a restored session keeps its id.
    [[nodiscard]] static std::wstring MakeGid(const std::wstring& url);

private:
    struct Job {
        DownloadTask task;
        net::Url url;
        std::filesystem::path finalPath;
        uint32_t workers{10};
        bool rangeSupported{true};

        std::unique_ptr<SegmentMap> segments;
        std::unique_ptr<PartFile> file;

        std::atomic<uint64_t> completedBytes{0};
        std::atomic<uint32_t> activeWorkers{0};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> running{false};

        std::vector<std::thread> workerThreads;
        std::thread controllerThread;

        // Non-owning pointers valid only while the associated network call is
        // running. StopJob closes them to interrupt blocking Winsock calls.
        std::mutex connectionsMutex;
        std::vector<net::HttpConnection*> activeConnections;

        uint64_t lastSampleBytes{0};
        uint64_t lastSampleTick{0};
        double smoothedSpeed{0.0};
    };

    Job* FindJob(const std::wstring& gid);
    const Job* FindJob(const std::wstring& gid) const;

    void StartJob(Job& job, bool freshStart);
    void StopJob(Job& job);
    void ControllerMain(Job& job, bool freshStart);
    void WorkerMain(Job& job, uint32_t workerIndex);

    /// Ranged HEAD-equivalent: learns total size, range support and file name.
    bool ProbeResource(Job& job, std::string& errorOut);

    void MonitorMain();
    void PublishUpdate(const Job& job);
    void SetStatus(Job& job, TaskStatus status, const std::wstring& message);
    void Log(const std::wstring& message) const;

    TorPool& m_torPool;

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Job>> m_jobs;

    UpdateCallback m_updateCallback;
    LogCallback m_logCallback;

    std::thread m_monitorThread;
    std::atomic<bool> m_monitorRunning{false};
    std::atomic<bool> m_shuttingDown{false};
};

} // namespace od::core
