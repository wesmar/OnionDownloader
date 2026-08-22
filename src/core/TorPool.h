#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace od::core {

/**
 * @brief A pool of independent Tor client daemons used to multiply throughput.
 *
 * WHY A POOL AND NOT A SINGLE DAEMON:
 * SOCKS stream isolation gives every worker its own circuit, but one Tor client
 * pins all of its circuits to the same entry guard by design. That guard's
 * bandwidth - not the number of circuits - becomes the ceiling. Running N client
 * daemons, each with its own DataDirectory and therefore its own guard set,
 * removes that ceiling; combined with per-worker isolation credentials the
 * result is N genuinely disjoint paths through the network. This is exactly what
 * the Docker reference stack achieves with N containers behind a load balancer,
 * implemented here as N child processes owned by a kill-on-close job object.
 *
 * Data directories live under %LOCALAPPDATA%\od\tor\<index> so the consensus
 * cache survives between runs and later startups take seconds, not minutes.
 */
class TorPool {
public:
    /// Reports human-readable startup progress (bootstrap percentages, failures).
    using StatusCallback = std::function<void(const std::wstring&)>;

    TorPool() = default;
    ~TorPool();

    TorPool(const TorPool&) = delete;
    TorPool& operator=(const TorPool&) = delete;

    /// Launch and bootstrap `instances` Tor clients. Returns true if at least one
    /// became usable. Safe to call repeatedly; subsequent calls are no-ops.
    bool Start(uint32_t instances, const StatusCallback& onStatus);

    /// Terminate every managed daemon.
    void Shutdown();

    /// Number of bootstrapped daemons available for downloads.
    [[nodiscard]] size_t ReadyCount() const;

    /// SOCKS port serving the given worker index (round-robin over the pool).
    [[nodiscard]] uint16_t SocksPortForWorker(size_t workerIndex) const;

    [[nodiscard]] bool IsReady() const { return ReadyCount() > 0; }

    /// Absolute path of the bundled tor.exe, or an empty string if not found.
    [[nodiscard]] static std::wstring LocateTorExecutable();

private:
    struct Instance {
        uint16_t socksPort{0};
        uint16_t controlPort{0};
        std::wstring dataDir;
        HANDLE process{nullptr};
        bool ready{false};
    };

    bool LaunchInstance(Instance& instance, const std::wstring& torExe);
    bool WaitForBootstrap(const Instance& instance, uint32_t timeoutMs, const StatusCallback& onStatus, bool verbose);
    static void PrimeConsensusCache(const std::wstring& sourceDir, const std::wstring& targetDir);
    static bool HasConsensusCache(const std::wstring& dataDir);

    std::vector<Instance> m_instances;
    std::vector<uint16_t> m_readyPorts;
    HANDLE m_job{nullptr};
    std::atomic<bool> m_started{false};
};

} // namespace od::core
