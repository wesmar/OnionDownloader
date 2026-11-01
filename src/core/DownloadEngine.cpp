#include "DownloadEngine.h"

#include "SessionManager.h"
#include "StringUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <shlobj.h>

namespace od::core {

namespace {

/// A rendezvous circuit to a busy onion service can take a full minute to build.
constexpr uint32_t kHandshakeTimeoutMs = 90000;
constexpr uint32_t kDataRecvTimeoutMs = 60000;
constexpr uint32_t kDataSendTimeoutMs = 30000;

/// Consecutive failures a single worker tolerates before it retires. Other
/// workers keep going, so retiring one bad circuit never aborts the download.
constexpr int kMaxWorkerFailures = 10;
constexpr size_t kTransferBufferBytes = 256 * 1024;
constexpr int kMaxRedirects = 5;

uint64_t NowTicks() {
    return ::GetTickCount64();
}

std::wstring DescribeHttpStatus(int code, const std::string& text) {
    std::wstring message = L"HTTP " + std::to_wstring(code);
    if (!text.empty()) {
        message += L" " + StringUtils::Utf8ToWide(text);
    }
    return message;
}

/// Derive the local file name from Content-Disposition, falling back to the
/// last path component with percent-escapes decoded.
std::wstring DeriveFileName(const net::Url& url, const net::HttpResponse& response) {
    const std::string disposition = response.Header("Content-Disposition");
    const size_t marker = disposition.find("filename=");
    if (marker != std::string::npos) {
        std::string value = disposition.substr(marker + 9);
        if (!value.empty() && value.front() == '"') {
            const size_t closing = value.find('"', 1);
            value = (closing == std::string::npos) ? value.substr(1) : value.substr(1, closing - 1);
        } else {
            const size_t semicolon = value.find(';');
            if (semicolon != std::string::npos) {
                value = value.substr(0, semicolon);
            }
        }
        if (!value.empty()) {
            return PartFile::SanitizeFileName(StringUtils::Utf8ToWide(net::PercentDecode(value)));
        }
    }

    std::string path = url.path;
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos) {
        path = path.substr(0, query);
    }
    const size_t slash = path.rfind('/');
    const std::string basename = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (basename.empty()) {
        return L"download.bin";
    }
    return PartFile::SanitizeFileName(StringUtils::Utf8ToWide(net::PercentDecode(basename)));
}

std::filesystem::path ResolveDestinationDir(const std::wstring& requested) {
    if (!requested.empty()) {
        return std::filesystem::absolute(requested);
    }

    PWSTR downloadsPath = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT,
                                         nullptr, &downloadsPath)) &&
        downloadsPath != nullptr) {
        const std::filesystem::path result(downloadsPath);
        ::CoTaskMemFree(downloadsPath);
        return result;
    }
    if (downloadsPath != nullptr) {
        ::CoTaskMemFree(downloadsPath);
    }

    // Very old or damaged user profiles may not expose the known folder.
    wchar_t exePath[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        return std::filesystem::path(exePath).parent_path() / L"downloads";
    }
    return std::filesystem::current_path() / L"downloads";
}

} // namespace

DownloadEngine::DownloadEngine(TorPool& torPool)
    : m_torPool(torPool) {
    // Restore previous sessions as paused entries so the table is populated
    // instantly; nothing touches the network until the user resumes.
    auto restored = SessionManager::LoadSession(SessionManager::GetDefaultSessionPath());
    for (auto& task : restored) {
        if (task.url.empty()) {
            continue;
        }
        auto job = std::make_unique<Job>();
        job->task = task;
        job->task.gid = MakeGid(task.url);
        job->task.downloadSpeed = 0;
        job->task.activeConnections = 0;
        if (task.status != TaskStatus::Completed) {
            job->task.status = TaskStatus::Paused;
        }
        job->workers = (task.totalSplits > 0) ? task.totalSplits : 10;
        if (!task.destinationPath.empty() && !task.fileName.empty()) {
            job->finalPath = std::filesystem::path(task.destinationPath) / task.fileName;
        }
        m_jobs.push_back(std::move(job));
    }

    m_monitorRunning = true;
    m_monitorThread = std::thread(&DownloadEngine::MonitorMain, this);
}

DownloadEngine::~DownloadEngine() {
    Shutdown();
}

std::wstring DownloadEngine::MakeGid(const std::wstring& url) {
    // FNV-1a over the UTF-8 form: stable across runs, so session.json entries
    // keep their identity and can be resumed after a restart.
    const std::string utf8 = StringUtils::WideToUtf8(url);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : utf8) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    wchar_t buffer[17] = {};
    ::swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

DownloadEngine::Job* DownloadEngine::FindJob(const std::wstring& gid) {
    for (auto& job : m_jobs) {
        if (job->task.gid == gid) {
            return job.get();
        }
    }
    return nullptr;
}

const DownloadEngine::Job* DownloadEngine::FindJob(const std::wstring& gid) const {
    for (const auto& job : m_jobs) {
        if (job->task.gid == gid) {
            return job.get();
        }
    }
    return nullptr;
}

void DownloadEngine::SetUpdateCallback(UpdateCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_updateCallback = std::move(callback);
}

void DownloadEngine::SetLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logCallback = std::move(callback);
}

void DownloadEngine::Log(const std::wstring& message) const {
    LogCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_logCallback;
    }
    if (callback) {
        callback(message);
    }
}

void DownloadEngine::PublishUpdate(const Job& job) {
    UpdateCallback callback;
    DownloadTask snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_updateCallback;
        snapshot = job.task;
    }
    if (callback) {
        callback(snapshot);
    }
}

void DownloadEngine::SetStatus(Job& job, TaskStatus status, const std::wstring& message) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.task.status = status;
        job.task.errorMessage = message;
        if (status != TaskStatus::Active) {
            job.task.downloadSpeed = 0;
            job.task.activeConnections = 0;
        }
    }
    PublishUpdate(job);
}

bool DownloadEngine::AddDownload(const std::wstring& url, const std::wstring& destinationDir, uint32_t workers) {
    const std::wstring trimmed = StringUtils::Trim(url);
    if (trimmed.empty()) {
        return false;
    }

    net::Url parsed;
    std::string parseError;
    if (!net::ParseUrl(StringUtils::WideToUtf8(trimmed), parsed, &parseError)) {
        Log(L"Rejected URL: " + StringUtils::Utf8ToWide(parseError));
        return false;
    }

    const std::wstring gid = MakeGid(trimmed);
    const std::filesystem::path destination = ResolveDestinationDir(destinationDir);

    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job = FindJob(gid);
        if (job == nullptr) {
            auto created = std::make_unique<Job>();
            created->task.gid = gid;
            created->task.url = trimmed;
            created->task.fileName = PartFile::SanitizeFileName(
                StringUtils::Utf8ToWide(net::PercentDecode(StringUtils::WideToUtf8(StringUtils::ExtractFileName(trimmed)))));
            created->task.destinationPath = destination.wstring();
            created->task.status = TaskStatus::Queued;
            created->task.totalSplits = std::clamp<uint32_t>(workers, 1, 64);
            created->task.startTime = std::chrono::steady_clock::now();
            job = created.get();
            m_jobs.push_back(std::move(created));
        } else if (job->running.load()) {
            return true;   // already downloading; nothing to do
        } else {
            job->task.destinationPath = destination.wstring();
            job->task.totalSplits = std::clamp<uint32_t>(workers, 1, 64);
            job->task.errorMessage.clear();
            if (job->task.status == TaskStatus::Completed) {
                job->task.completedBytes = 0;
            }
            job->task.status = TaskStatus::Queued;
        }
        job->url = parsed;
        job->workers = std::clamp<uint32_t>(workers, 1, 64);
    }

    StartJob(*job, false);
    SaveSession();
    return true;
}

bool DownloadEngine::Resume(const std::wstring& gid) {
    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job = FindJob(gid);
        if (job == nullptr || job->running.load()) {
            return job != nullptr;
        }
        std::string parseError;
        if (!net::ParseUrl(StringUtils::WideToUtf8(job->task.url), job->url, &parseError)) {
            return false;
        }
        job->task.errorMessage.clear();
    }
    StartJob(*job, false);
    return true;
}

bool DownloadEngine::Pause(const std::wstring& gid) {
    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job = FindJob(gid);
    }
    if (job == nullptr) {
        return false;
    }
    if (job->running.load()) {
        StopJob(*job);
        SetStatus(*job, TaskStatus::Paused, {});
    }
    SaveSession();
    return true;
}

bool DownloadEngine::Remove(const std::wstring& gid, bool deletePartialFile) {
    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job = FindJob(gid);
    }
    if (job == nullptr) {
        return false;
    }

    StopJob(*job);
    const std::filesystem::path finalPath = !job->finalPath.empty()
        ? job->finalPath
        : ((!job->task.destinationPath.empty() && !job->task.fileName.empty())
               ? (std::filesystem::path(job->task.destinationPath) / job->task.fileName)
               : std::filesystem::path{});

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase_if(m_jobs, [&](const std::unique_ptr<Job>& candidate) {
            return candidate->task.gid == gid;
        });
    }

    if (deletePartialFile && !finalPath.empty()) {
        PartFile::Discard(finalPath);
    }

    SaveSession();
    return true;
}

void DownloadEngine::StartJob(Job& job, bool freshStart) {
    StopJob(job);
    job.stopRequested = false;
    job.running = true;
    job.completedBytes = 0;
    job.activeWorkers = 0;
    job.lastSampleBytes = 0;
    job.lastSampleTick = NowTicks();
    job.smoothedSpeed = 0.0;
    job.controllerThread = std::thread(&DownloadEngine::ControllerMain, this, std::ref(job), freshStart);
}

void DownloadEngine::StopJob(Job& job) {
    job.stopRequested = true;
    {
        std::lock_guard<std::mutex> lock(job.connectionsMutex);
        for (net::HttpConnection* connection : job.activeConnections) {
            connection->Cancel();
        }
    }
    if (job.controllerThread.joinable()) {
        job.controllerThread.join();
    }
    job.running = false;
}

bool DownloadEngine::ProbeResource(Job& job, std::string& errorOut) {
    net::Url current = job.url;

    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        if (job.stopRequested.load()) {
            errorOut = "cancelled";
            return false;
        }
        net::HttpConnection probe;
        {
            std::lock_guard<std::mutex> lock(job.connectionsMutex);
            job.activeConnections.push_back(&probe);
        }
        const auto unregisterProbe = [&]() {
            std::lock_guard<std::mutex> lock(job.connectionsMutex);
            std::erase(job.activeConnections, &probe);
        };
        const std::string isolation = "probe-" + StringUtils::WideToUtf8(job.task.gid) +
                                      "-" + std::to_string(redirect);

        if (!probe.Open(m_torPool.SocksPortForWorker(0), isolation, "od", current,
                        kHandshakeTimeoutMs, &errorOut)) {
            unregisterProbe();
            return false;
        }
        probe.SetIoTimeouts(kDataRecvTimeoutMs, kDataSendTimeoutMs);

        net::HttpResponse response;
        std::string cookie;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cookie = job.sessionCookie;
        }
        if (!probe.RequestRange(current, true, 0, 0, response, &errorOut, cookie)) {
            unregisterProbe();
            return false;
        }

        if (response.statusCode == 401) {
            Log(L"Solving onion anti-DDoS PoW challenge...");
            if (!response.hasContentLength || response.WantsClose() ||
                !probe.DrainBody(response.contentLength)) {
                if (!probe.Open(m_torPool.SocksPortForWorker(0), isolation + "-pow", "od", current,
                                kHandshakeTimeoutMs, &errorOut)) {
                    unregisterProbe();
                    return false;
                }
                probe.SetIoTimeouts(kDataRecvTimeoutMs, kDataSendTimeoutMs);
            }
            std::string solvedCookie;
            if (probe.SolvePoWGate(current, solvedCookie, &errorOut)) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    job.sessionCookie = solvedCookie;
                    job.powGateRequired = true;
                }
                unregisterProbe();
                continue;
            }
        }

        if (response.statusCode >= 300 && response.statusCode < 400) {
            net::Url next;
            if (!net::ResolveRedirect(current, response.Header("Location"), next, &errorOut)) {
                unregisterProbe();
                return false;
            }
            unregisterProbe();
            current = next;
            continue;
        }

        if (response.statusCode == 206 && response.hasContentRange && response.instanceLength > 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            job.url = current;
            job.rangeSupported = true;
            job.task.url = StringUtils::Utf8ToWide(current.ToString());
            job.task.totalBytes = response.instanceLength;
            job.task.fileName = DeriveFileName(current, response);
            unregisterProbe();
            return true;
        }

        if (response.statusCode == 200) {
            // The server ignored the Range header: parallel segments are
            // impossible, so fall back to a single sequential stream.
            std::lock_guard<std::mutex> lock(m_mutex);
            job.url = current;
            job.rangeSupported = false;
            job.task.url = StringUtils::Utf8ToWide(current.ToString());
            job.task.totalBytes = response.hasContentLength ? response.contentLength : 0;
            job.task.fileName = DeriveFileName(current, response);
            unregisterProbe();
            return job.task.totalBytes > 0;
        }

        errorOut = StringUtils::WideToUtf8(DescribeHttpStatus(response.statusCode, response.statusText));
        unregisterProbe();
        return false;
    }

    errorOut = "too many redirects";
    return false;
}

void DownloadEngine::ControllerMain(Job& job, bool freshStart) {
    SetStatus(job, TaskStatus::Active, L"Connecting through Tor...");

    if (!m_torPool.IsReady()) {
        SetStatus(job, TaskStatus::Error, L"Tor is not ready yet");
        job.running = false;
        return;
    }

    std::string probeError;
    if (!ProbeResource(job, probeError)) {
        SetStatus(job, TaskStatus::Error, StringUtils::Utf8ToWide(probeError));
        job.running = false;
        SaveSession();
        return;
    }
    if (job.stopRequested.load()) {
        SetStatus(job, TaskStatus::Paused, {});
        job.running = false;
        return;
    }

    uint64_t totalBytes = 0;
    std::filesystem::path destination;
    std::wstring fileName;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        totalBytes = job.task.totalBytes;
        destination = job.task.destinationPath;
        fileName = job.task.fileName;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.finalPath = destination / fileName;
    }

    const uint32_t effectiveWorkers = job.rangeSupported
        ? std::max<uint32_t>(1, std::min<uint32_t>(job.workers, static_cast<uint32_t>(totalBytes / SegmentMap::kMinSegmentBytes) + 1))
        : 1;

    const uint64_t segmentSize = job.rangeSupported
        ? SegmentMap::ChooseSegmentSize(totalBytes, effectiveWorkers)
        : totalBytes;

    {
        // The monitor thread reads job.segments under the same lock, so the
        // handover of the plan has to be published rather than raced.
        std::lock_guard<std::mutex> lock(m_mutex);
        job.segments = std::make_unique<SegmentMap>(totalBytes, segmentSize);
    }

    // Resume: the stored plan must match exactly, otherwise the bitmap would
    // describe ranges that no longer line up with the current segmentation.
    if (!freshStart) {
        ResumeRecord record;
        if (PartFile::LoadResume(job.finalPath, record) &&
            record.totalBytes == totalBytes &&
            record.segmentSize == segmentSize &&
            record.url == job.url.ToString()) {
            job.segments->DecodeBitmap(record.bitmap);
        }
    }

    job.completedBytes = job.segments->CompletedBytes();
    job.lastSampleBytes = job.completedBytes.load();
    job.lastSampleTick = NowTicks();

    job.file = std::make_unique<PartFile>();
    std::string fileError;
    if (!job.file->Open(job.finalPath, totalBytes, &fileError)) {
        SetStatus(job, TaskStatus::Error, StringUtils::Utf8ToWide(fileError));
        job.running = false;
        return;
    }

    Log(L"Downloading " + fileName + L" (" + StringUtils::FormatBytes(totalBytes) + L") over " +
        std::to_wstring(effectiveWorkers) + L" circuits across " +
        std::to_wstring(m_torPool.ReadyCount()) + L" Tor daemons");

    SetStatus(job, TaskStatus::Active, {});

    job.workerThreads.clear();
    job.workerThreads.reserve(effectiveWorkers);
    for (uint32_t i = 0; i < effectiveWorkers; ++i) {
        job.workerThreads.emplace_back(&DownloadEngine::WorkerMain, this, std::ref(job), i);
    }
    for (auto& worker : job.workerThreads) {
        worker.join();
    }
    job.workerThreads.clear();

    const bool complete = job.segments->IsComplete();
    ResumeRecord record;
    record.url = job.url.ToString();
    record.totalBytes = totalBytes;
    record.segmentSize = segmentSize;
    record.bitmap = job.segments->EncodeBitmap();

    if (complete) {
        std::string finalizeError;
        if (job.file->Finalize(&finalizeError)) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                job.task.completedBytes = totalBytes;
            }
            SetStatus(job, TaskStatus::Completed, {});
            Log(L"Completed: " + job.finalPath.wstring());
        } else {
            SetStatus(job, TaskStatus::Error, StringUtils::Utf8ToWide(finalizeError));
        }
    } else {
        PartFile::SaveResume(job.finalPath, record);
        job.file->Close();

        if (job.stopRequested.load()) {
            SetStatus(job, TaskStatus::Paused, {});
        } else {
            std::wstring reason;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                reason = job.task.errorMessage;
            }
            SetStatus(job, TaskStatus::Error, reason.empty() ? L"all circuits failed" : reason);
        }
    }

    job.running = false;
    SaveSession();
}

void DownloadEngine::WorkerMain(Job& job, uint32_t workerIndex) {
    net::HttpConnection connection;
    std::vector<char> buffer(kTransferBufferBytes);

    std::string writerError;
    auto writer = job.file->CreateWriter(&writerError);
    if (!writer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.task.errorMessage = StringUtils::Utf8ToWide(writerError);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(job.connectionsMutex);
        job.activeConnections.push_back(&connection);
    }

    const uint16_t socksPort = m_torPool.SocksPortForWorker(workerIndex);
    uint32_t credentialNonce = 0;
    int consecutiveFailures = 0;
    std::string workerCookie;
    if (workerIndex == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        workerCookie = job.sessionCookie;
    }

    while (!job.stopRequested.load()) {
        auto claimed = job.segments->ClaimNext();
        if (!claimed) {
            break;
        }
        const Segment segment = *claimed;

        bool segmentDone = false;
        uint64_t segmentWritten = 0;
        std::wstring failureReason;

        job.activeWorkers.fetch_add(1);

        for (;;) {
            if (!connection.IsOpen()) {
                // A new credential forces Tor to build a brand new circuit, so a
                // retry never lands on the path that just failed.
                const std::string isolationUser =
                    "w" + std::to_string(workerIndex) + "-" + std::to_string(credentialNonce);
                std::string openError;
                if (!connection.Open(socksPort, isolationUser, "od", job.url,
                                     kHandshakeTimeoutMs, &openError)) {
                    failureReason = StringUtils::Utf8ToWide(openError);
                    break;
                }
                connection.SetIoTimeouts(kDataRecvTimeoutMs, kDataSendTimeoutMs);
                if (job.powGateRequired && workerCookie.empty()) {
                    std::string gateError;
                    if (!connection.SolvePoWGate(job.url, workerCookie, &gateError)) {
                        failureReason = StringUtils::Utf8ToWide(gateError);
                        break;
                    }
                }
            }

            net::HttpResponse response;
            std::string requestError;
            if (!connection.RequestRange(job.url, job.rangeSupported,
                                         segment.firstByte, segment.lastByte,
                                         response, &requestError, workerCookie)) {
                failureReason = StringUtils::Utf8ToWide(requestError);
                break;
            }

            if (response.statusCode == 401) {
                workerCookie.clear();
                job.powGateRequired = true;
                if (!response.hasContentLength || response.WantsClose() ||
                    !connection.DrainBody(response.contentLength)) {
                    connection.Close();
                    continue;
                }
                if (connection.SolvePoWGate(job.url, workerCookie, &requestError)) {
                    continue;
                }
                connection.Close();
                break;
            }

            const int expectedStatus = job.rangeSupported ? 206 : 200;
            if (response.statusCode != expectedStatus) {
                failureReason = DescribeHttpStatus(response.statusCode, response.statusText);
                if (response.statusCode >= 400 && response.statusCode < 500 &&
                    response.statusCode != 401 && response.statusCode != 429) {
                    // The resource is gone or forbidden: retrying on another
                    // circuit cannot help, so stop the whole job.
                    job.stopRequested = true;
                    std::lock_guard<std::mutex> lock(m_mutex);
                    job.task.errorMessage = failureReason;
                }
                break;
            }

            const uint64_t expectedLength = segment.Length();
            if (!response.hasContentLength || response.contentLength != expectedLength) {
                failureReason = L"server returned an unexpected content length";
                break;
            }

            uint64_t remaining = expectedLength;
            uint64_t offset = segment.firstByte;
            bool transferOk = true;

            while (remaining > 0 && !job.stopRequested.load()) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                const int got = connection.ReadBody(buffer.data(), want);
                if (got <= 0) {
                    failureReason = L"circuit stalled or closed mid-transfer";
                    transferOk = false;
                    break;
                }
                if (!writer->WriteAt(offset, buffer.data(), static_cast<size_t>(got))) {
                    failureReason = L"writing to disk failed";
                    transferOk = false;
                    job.stopRequested = true;
                    break;
                }
                offset += static_cast<uint64_t>(got);
                remaining -= static_cast<uint64_t>(got);
                segmentWritten += static_cast<uint64_t>(got);
                job.completedBytes.fetch_add(static_cast<uint64_t>(got));
            }

            if (transferOk && remaining == 0) {
                segmentDone = true;
                if (response.WantsClose()) {
                    connection.Close();
                }
            }
            break;
        }

        job.activeWorkers.fetch_sub(1);

        if (segmentDone) {
            job.segments->MarkComplete(segment.index);
            consecutiveFailures = 0;
            continue;
        }

        // Roll back this segment's contribution and hand it back to the pool.
        job.completedBytes.fetch_sub(segmentWritten);
        job.segments->Release(segment.index);
        connection.Close();
        workerCookie.clear();
        ++credentialNonce;
        ++consecutiveFailures;

        if (!failureReason.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            job.task.errorMessage = failureReason;
        }

        if (job.stopRequested.load() || consecutiveFailures > kMaxWorkerFailures) {
            break;
        }

        // Linear backoff: a congested onion service recovers faster than an
        // exponential curve would allow us to notice.
        const DWORD backoffMs = static_cast<DWORD>(std::min(consecutiveFailures, 5) * 1000);
        for (DWORD waited = 0; waited < backoffMs && !job.stopRequested.load(); waited += 50) {
            ::Sleep(std::min<DWORD>(50, backoffMs - waited));
        }
    }
    {
        std::lock_guard<std::mutex> lock(job.connectionsMutex);
        std::erase(job.activeConnections, &connection);
    }
}

void DownloadEngine::MonitorMain() {
    uint64_t lastResumeSave = NowTicks();

    while (m_monitorRunning.load()) {
        ::Sleep(400);

        bool anyActive = false;
        const uint64_t now = NowTicks();
        const bool persistResume = (now - lastResumeSave) > 5000;

        std::vector<DownloadTask> snapshots;
        UpdateCallback callback;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_updateCallback;

            for (auto& jobPtr : m_jobs) {
                Job& job = *jobPtr;
                if (!job.running.load()) {
                    continue;
                }
                anyActive = true;

                const uint64_t completed = job.completedBytes.load();
                const uint64_t elapsed = now - job.lastSampleTick;
                if (elapsed >= 400) {
                    const double instant = (completed >= job.lastSampleBytes)
                        ? static_cast<double>(completed - job.lastSampleBytes) * 1000.0 / static_cast<double>(elapsed)
                        : 0.0;
                    // Exponential smoothing keeps the readout stable while Tor
                    // circuits ramp up and down.
                    job.smoothedSpeed = (job.smoothedSpeed <= 0.0) ? instant
                                                                   : (job.smoothedSpeed * 0.7 + instant * 0.3);
                    job.lastSampleBytes = completed;
                    job.lastSampleTick = now;
                }

                job.task.completedBytes = completed;
                job.task.downloadSpeed = static_cast<uint64_t>(job.smoothedSpeed);
                job.task.activeConnections = job.activeWorkers.load();
                snapshots.push_back(job.task);

                if (persistResume && job.segments && !job.finalPath.empty()) {
                    ResumeRecord record;
                    record.url = job.url.ToString();
                    record.totalBytes = job.task.totalBytes;
                    record.segmentSize = job.segments->SegmentSize();
                    record.bitmap = job.segments->EncodeBitmap();
                    PartFile::SaveResume(job.finalPath, record);
                }
            }
        }

        if (persistResume) {
            lastResumeSave = now;
        }
        if (callback) {
            for (const auto& snapshot : snapshots) {
                callback(snapshot);
            }
        }
        if (!anyActive) {
            ::Sleep(200);
        }
    }
}

std::vector<DownloadTask> DownloadEngine::GetAllTasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DownloadTask> tasks;
    tasks.reserve(m_jobs.size());
    for (const auto& job : m_jobs) {
        tasks.push_back(job->task);
    }
    return tasks;
}

bool DownloadEngine::TryGetTask(const std::wstring& gid, DownloadTask& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Job* job = FindJob(gid);
    if (job == nullptr) {
        return false;
    }
    out = job->task;
    return true;
}

void DownloadEngine::SaveSession() {
    std::vector<DownloadTask> tasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tasks.reserve(m_jobs.size());
        for (const auto& job : m_jobs) {
            tasks.push_back(job->task);
        }
    }
    SessionManager::SaveSession(tasks, SessionManager::GetDefaultSessionPath());
}

void DownloadEngine::Shutdown() {
    if (m_shuttingDown.exchange(true)) {
        return;
    }

    m_monitorRunning = false;
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }

    std::vector<Job*> jobs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobs.reserve(m_jobs.size());
        for (auto& job : m_jobs) {
            jobs.push_back(job.get());
        }
    }
    for (Job* job : jobs) {
        StopJob(*job);
    }

    SaveSession();
}

} // namespace od::core
