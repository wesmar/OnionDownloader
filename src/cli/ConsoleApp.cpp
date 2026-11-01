#include "ConsoleApp.h"

#include "ConsoleManager.h"
#include "../core/DownloadEngine.h"
#include "../core/StringUtils.h"
#include "../core/TorPool.h"

#include <windows.h>

#include <atomic>

namespace od::cli {

namespace {

constexpr DWORD kPollIntervalMs = 300;
std::atomic<bool> s_interrupted{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        s_interrupted = true;
        return TRUE;
    }
    return FALSE;
}

} // namespace

int ConsoleApp::Run(const CliOptions& options) {
    namespace core = od::core;

    ConsoleManager::InitializeConsole();
    s_interrupted = false;
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    ConsoleManager::WriteLine(L"[+] Onion Downloader - Multi-Circuit Tor Engine v1.0.0");
    ConsoleManager::WriteLine(L"[+] URL     : " + options.url);
    ConsoleManager::WriteLine(L"[+] Output  : " + options.outputDir);
    ConsoleManager::WriteLine(L"[+] Streams : " + std::to_wstring(options.workers) +
                              L" across " + std::to_wstring(options.EffectiveTorInstances()) +
                              L" Tor daemons");

    core::TorPool torPool;
    const bool torReady = torPool.Start(
        options.EffectiveTorInstances(),
        [](const std::wstring& status) { ConsoleManager::WriteLine(L"[*] " + status); });

    if (!torReady) {
        ConsoleManager::WriteLine(L"[-] Tor could not be started - aborting.");
        return kTorUnavailable;
    }

    core::DownloadEngine engine(torPool);
    engine.SetLogCallback([](const std::wstring& message) {
        ConsoleManager::WriteLine(L"[*] " + message);
    });

    if (!engine.AddDownload(options.url, options.outputDir, options.workers)) {
        ConsoleManager::WriteLine(L"[-] The URL was rejected - only http:// addresses are supported.");
        return kInvalidUrl;
    }

    const std::wstring gid = core::DownloadEngine::MakeGid(core::StringUtils::Trim(options.url));

    int exitCode = kSuccess;
    for (;;) {
        if (s_interrupted.load()) {
            ConsoleManager::WriteLine(L"\n\n[*] Interrupted by user - saving resume state...");
            exitCode = kDownloadFailed;
            break;
        }
        ::Sleep(kPollIntervalMs);

        core::DownloadTask task;
        if (!engine.TryGetTask(gid, task)) {
            continue;
        }

        if (task.status == core::TaskStatus::Active && task.totalBytes > 0) {
            ConsoleManager::RenderProgressBar(task.GetProgressPercent(),
                                              task.completedBytes,
                                              task.totalBytes,
                                              task.downloadSpeed,
                                              task.activeConnections);
        }

        if (task.status == core::TaskStatus::Completed) {
            ConsoleManager::RenderProgressBar(100.0, task.totalBytes, task.totalBytes, 0, 0);
            ConsoleManager::WriteLine(L"\n\n[+] Done: " + task.destinationPath + L"\\" + task.fileName);
            break;
        }
        if (task.status == core::TaskStatus::Error) {
            ConsoleManager::WriteLine(L"\n\n[-] Failed: " + task.errorMessage);
            exitCode = kDownloadFailed;
            break;
        }
    }

    engine.Shutdown();
    torPool.Shutdown();
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    return exitCode;
}

} // namespace od::cli
