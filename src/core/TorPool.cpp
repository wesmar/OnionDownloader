#include "TorPool.h"

#include "../net/Socks5.h"
#include "StringUtils.h"
#include "TorExtractor.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace od::core {

namespace {

constexpr uint16_t kSocksPortBase = 19050;   ///< deliberately clear of 9050/9150 (system Tor, Tor Browser)
constexpr uint32_t kMaxInstances = 32;

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, buffer, MAX_PATH) > 0) {
        return std::filesystem::path(buffer).parent_path();
    }
    return std::filesystem::current_path();
}

std::filesystem::path TorRootDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        return std::filesystem::path(localAppData) / L"od" / L"tor";
    }
    return ExecutableDirectory() / L"tor-data";
}

/// True when nothing is listening on the loopback port and it can be bound.
bool IsPortFree(uint16_t port) {
    net::EnsureWinsock();
    SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    const bool free = (::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::closesocket(probe);
    return free;
}

uint16_t FindFreePortPair(uint16_t startAt, uint16_t& controlPortOut) {
    for (uint32_t candidate = startAt; candidate < 65000; candidate += 2) {
        const auto socksPort = static_cast<uint16_t>(candidate);
        const auto controlPort = static_cast<uint16_t>(candidate + 1);
        if (IsPortFree(socksPort) && IsPortFree(controlPort)) {
            controlPortOut = controlPort;
            return socksPort;
        }
    }
    controlPortOut = 0;
    return 0;
}

std::string ToHex(const std::string& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

/**
 * @brief Minimal Tor control-port client (cookie authentication only).
 *
 * The control port is the only reliable way to learn when a daemon is actually
 * usable. Waiting for the SOCKS port to accept connections - what the previous
 * implementation did - proves nothing: Tor opens its listeners immediately and
 * only refuses traffic later, which is precisely how a broken download surfaced
 * as an opaque "Unknown error".
 */
class ControlSession {
public:
    bool Open(uint16_t controlPort, const std::wstring& dataDir) {
        if (!m_stream.ConnectLoopback(controlPort, 2000)) {
            return false;
        }
        m_stream.SetTimeouts(5000, 5000);

        const std::filesystem::path cookiePath = std::filesystem::path(dataDir) / L"control_auth_cookie";
        std::ifstream cookieFile(cookiePath, std::ios::binary);
        if (!cookieFile) {
            return false;
        }
        std::string cookie((std::istreambuf_iterator<char>(cookieFile)), std::istreambuf_iterator<char>());
        if (cookie.empty()) {
            return false;
        }

        std::string reply;
        if (!Command("AUTHENTICATE " + ToHex(cookie) + "\r\n", reply)) {
            return false;
        }
        return reply.rfind("250", 0) == 0;
    }

    /// Sends a command and reads the complete multi-line reply.
    bool Command(const std::string& command, std::string& reply) {
        if (!m_stream.SendAll(command.data(), command.size())) {
            return false;
        }
        reply.clear();
        char chunk[2048];
        for (;;) {
            const int got = m_stream.Recv(chunk, sizeof(chunk));
            if (got <= 0) {
                return false;
            }
            reply.append(chunk, static_cast<size_t>(got));

            // Per the control protocol a reply ends with a line of the form
            // "<3-digit code><space>...\r\n"; mid-reply lines use '-' or '+'.
            size_t lineStart = 0;
            while (lineStart < reply.size()) {
                const size_t eol = reply.find("\r\n", lineStart);
                if (eol == std::string::npos) {
                    break;
                }
                if (eol - lineStart >= 4 && reply[lineStart + 3] == ' ') {
                    return true;
                }
                lineStart = eol + 2;
            }
        }
    }

private:
    net::TcpStream m_stream;
};

/// Extracts PROGRESS=<n> from a status/bootstrap-phase reply.
int ParseBootstrapProgress(const std::string& reply) {
    const size_t marker = reply.find("PROGRESS=");
    if (marker == std::string::npos) {
        return -1;
    }
    const char* first = reply.data() + marker + 9;
    const char* last = reply.data() + reply.size();
    int value = 0;
    const auto result = std::from_chars(first, last, value);
    return (result.ec == std::errc{}) ? value : -1;
}

} // namespace

TorPool::~TorPool() {
    Shutdown();
}

std::wstring TorPool::LocateTorExecutable() {
    const std::filesystem::path exeDir = ExecutableDirectory();

    const std::filesystem::path candidates[] = {
        exeDir / L"tor.exe",
        exeDir / L"tor" / L"tor.exe",
        TorRootDirectory().parent_path() / L"tor.exe",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate.wstring();
        }
    }

    // No loose tor.exe next to the executable: fall back to the copy compressed
    // into our own resource, expanding it once into %LOCALAPPDATA% if needed.
    if (TorExtractor::HasEmbeddedPayload()) {
        std::string extractError;
        const std::filesystem::path extracted = TorExtractor::EnsureExtracted(&extractError);
        if (!extracted.empty()) {
            return extracted.wstring();
        }
    }

    return {};
}

bool TorPool::HasConsensusCache(const std::wstring& dataDir) {
    std::error_code ec;
    const std::filesystem::path dir(dataDir);
    return std::filesystem::exists(dir / L"cached-microdesc-consensus", ec) ||
           std::filesystem::exists(dir / L"cached-consensus", ec);
}

void TorPool::PrimeConsensusCache(const std::wstring& sourceDir, const std::wstring& targetDir) {
    // Copying the directory consensus from an already-bootstrapped daemon spares
    // every sibling a full directory fetch, cutting cold start from minutes to
    // a few seconds. Guard nodes stay per-instance, which is the whole point.
    static constexpr const wchar_t* kCacheFiles[] = {
        L"cached-certs",
        L"cached-microdesc-consensus",
        L"cached-microdescs",
        L"cached-microdescs.new",
        L"cached-consensus",
        L"cached-descriptors",
        L"cached-descriptors.new",
    };

    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);
    for (const wchar_t* name : kCacheFiles) {
        const std::filesystem::path from = std::filesystem::path(sourceDir) / name;
        if (!std::filesystem::exists(from, ec)) {
            continue;
        }
        std::filesystem::copy_file(from,
                                   std::filesystem::path(targetDir) / name,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
    }
}

bool TorPool::LaunchInstance(Instance& instance, const std::wstring& torExe) {
    std::error_code ec;
    std::filesystem::create_directories(instance.dataDir, ec);

    // A stale cookie from a previous run would be accepted by our own control
    // client before the new daemon rewrites it, so remove it up front.
    std::filesystem::remove(std::filesystem::path(instance.dataDir) / L"control_auth_cookie", ec);

    std::wostringstream command;
    command << L'"' << torExe << L'"'
            << L" --SocksPort \"" << instance.socksPort << L" IsolateSOCKSAuth KeepAliveIsolateSOCKSAuth\""
            << L" --ControlPort " << instance.controlPort
            << L" --CookieAuthentication 1"
            << L" --DataDirectory \"" << instance.dataDir << L'"'
            << L" --ClientOnly 1"
            << L" --AvoidDiskWrites 1"
            << L" --MaxClientCircuitsPending 64"
            << L" --Log \"notice file " << instance.dataDir << L"\\tor.log\"";

    std::wstring commandLine = command.str();

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    const BOOL created = ::CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created) {
        return false;
    }

    // Job assignment must happen before the child can spawn anything of its own;
    // KILL_ON_JOB_CLOSE then guarantees no orphaned tor.exe survives od.exe.
    if (m_job != nullptr) {
        ::AssignProcessToJobObject(m_job, processInfo.hProcess);
    }
    ::CloseHandle(processInfo.hThread);
    instance.process = processInfo.hProcess;
    return true;
}

bool TorPool::WaitForBootstrap(const Instance& instance,
                               uint32_t timeoutMs,
                               const StatusCallback& onStatus,
                               bool verbose) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    int lastReported = -1;

    while (::GetTickCount64() < deadline) {
        if (instance.process != nullptr &&
            ::WaitForSingleObject(instance.process, 0) == WAIT_OBJECT_0) {
            return false;   // the daemon died; its log holds the reason
        }

        ControlSession session;
        if (session.Open(instance.controlPort, instance.dataDir)) {
            for (;;) {
                std::string reply;
                if (!session.Command("GETINFO status/bootstrap-phase\r\n", reply)) {
                    break;
                }
                const int progress = ParseBootstrapProgress(reply);
                if (progress >= 100) {
                    return true;
                }
                if (verbose && onStatus && progress >= 0 && progress / 10 != lastReported / 10) {
                    lastReported = progress;
                    onStatus(L"Bootstrapping Tor network: " + std::to_wstring(progress) + L"%");
                }
                if (::GetTickCount64() >= deadline) {
                    return false;
                }
                ::Sleep(500);
            }
        }
        ::Sleep(300);
    }
    return false;
}

bool TorPool::Start(uint32_t instances, const StatusCallback& onStatus) {
    if (m_started.exchange(true)) {
        return IsReady();
    }

    const std::wstring torExe = LocateTorExecutable();
    if (torExe.empty()) {
        if (onStatus) {
            onStatus(L"tor.exe not found next to od.exe");
        }
        return false;
    }

    instances = std::clamp<uint32_t>(instances, 1, kMaxInstances);

    m_job = ::CreateJobObjectW(nullptr, nullptr);
    if (m_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    const std::filesystem::path root = TorRootDirectory();
    m_instances.resize(instances);

    uint16_t nextPort = kSocksPortBase;
    for (uint32_t i = 0; i < instances; ++i) {
        uint16_t controlPort = 0;
        const uint16_t socksPort = FindFreePortPair(nextPort, controlPort);
        if (socksPort == 0) {
            m_instances.resize(i);
            break;
        }
        m_instances[i].socksPort = socksPort;
        m_instances[i].controlPort = controlPort;
        m_instances[i].dataDir = (root / std::to_wstring(i)).wstring();
        nextPort = static_cast<uint16_t>(socksPort + 2);
    }

    if (m_instances.empty()) {
        if (onStatus) {
            onStatus(L"No free loopback ports available for Tor");
        }
        return false;
    }

    // Stage 1: bring up the first daemon on its own. On a cold profile this is
    // the only instance that pays for a full directory fetch.
    const bool coldStart = !HasConsensusCache(m_instances[0].dataDir);
    if (onStatus) {
        onStatus(coldStart ? L"Bootstrapping Tor network (first run, this takes a moment)..."
                           : L"Starting Tor circuits...");
    }

    if (!LaunchInstance(m_instances[0], torExe)) {
        if (onStatus) {
            onStatus(L"Failed to launch tor.exe");
        }
        return false;
    }
    if (!WaitForBootstrap(m_instances[0], coldStart ? 180000u : 90000u, onStatus, true)) {
        if (onStatus) {
            onStatus(L"Tor failed to bootstrap - check the log in " + m_instances[0].dataDir);
        }
        return false;
    }
    m_instances[0].ready = true;
    m_readyPorts.push_back(m_instances[0].socksPort);

    if (m_instances.size() == 1) {
        if (onStatus) {
            onStatus(L"Tor ready: 1 circuit pool");
        }
        return true;
    }

    // Stage 2: seed the remaining profiles from the live consensus and start
    // them concurrently. Each keeps its own guards, so throughput adds up.
    if (onStatus) {
        onStatus(L"Starting " + std::to_wstring(m_instances.size() - 1) + L" additional Tor circuits...");
    }

    for (size_t i = 1; i < m_instances.size(); ++i) {
        if (!HasConsensusCache(m_instances[i].dataDir)) {
            PrimeConsensusCache(m_instances[0].dataDir, m_instances[i].dataDir);
        }
        LaunchInstance(m_instances[i], torExe);
    }

    std::vector<std::thread> waiters;
    waiters.reserve(m_instances.size() - 1);
    for (size_t i = 1; i < m_instances.size(); ++i) {
        waiters.emplace_back([this, i]() {
            if (m_instances[i].process != nullptr &&
                WaitForBootstrap(m_instances[i], 120000u, nullptr, false)) {
                m_instances[i].ready = true;
            }
        });
    }
    for (auto& waiter : waiters) {
        waiter.join();
    }

    for (size_t i = 1; i < m_instances.size(); ++i) {
        if (m_instances[i].ready) {
            m_readyPorts.push_back(m_instances[i].socksPort);
        } else if (m_instances[i].process != nullptr) {
            ::TerminateProcess(m_instances[i].process, 0);
            ::CloseHandle(m_instances[i].process);
            m_instances[i].process = nullptr;
        }
    }

    if (onStatus) {
        onStatus(L"Tor ready: " + std::to_wstring(m_readyPorts.size()) + L" independent circuit pools");
    }
    return !m_readyPorts.empty();
}

void TorPool::Shutdown() {
    for (auto& instance : m_instances) {
        if (instance.process != nullptr) {
            ::TerminateProcess(instance.process, 0);
            ::CloseHandle(instance.process);
            instance.process = nullptr;
        }
        instance.ready = false;
    }
    m_instances.clear();
    m_readyPorts.clear();

    if (m_job != nullptr) {
        ::CloseHandle(m_job);
        m_job = nullptr;
    }
    m_started = false;
}

size_t TorPool::ReadyCount() const {
    return m_readyPorts.size();
}

uint16_t TorPool::SocksPortForWorker(size_t workerIndex) const {
    if (m_readyPorts.empty()) {
        return 0;
    }
    return m_readyPorts[workerIndex % m_readyPorts.size()];
}

} // namespace od::core
