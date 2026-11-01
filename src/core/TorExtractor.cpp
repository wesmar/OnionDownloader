#include "TorExtractor.h"

#include "../../resources/Resource.h"

#include <windows.h>

#include <fcntl.h>
#include <fdi.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#pragma comment(lib, "cabinet.lib")

namespace od::core {

namespace {

constexpr const char* kExtractedName = "tor.exe";

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

/// Sentinel path handed to FDICopy as the cabinet name. The open callback maps
/// it to the in-memory resource instead of touching the file system.
constexpr const char* kCabSentinel = "\x01od-embedded-cab";

std::filesystem::path ExtractionDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        return std::filesystem::path(localAppData) / L"od" / L"tor" / L"bin";
    }
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    return std::filesystem::path(temp) / L"od" / L"tor" / L"bin";
}

uint32_t ReadLe32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/// A byte view of the embedded RCDATA payload (icon bytes followed by the CAB).
struct Payload {
    const unsigned char* data{nullptr};
    size_t size{0};
    [[nodiscard]] bool Valid() const { return data != nullptr && size > 0; }
};

uint64_t CabinetFingerprint(const unsigned char* data, size_t size) {
    uint64_t hash = kFnvOffsetBasis;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::filesystem::path FingerprintPath(const std::filesystem::path& target) {
    return std::filesystem::path(target.wstring() + L".cabid");
}

bool HasMatchingFingerprint(const std::filesystem::path& path, uint64_t expected) {
    std::ifstream input(path, std::ios::binary);
    uint64_t stored = 0;
    input.read(reinterpret_cast<char*>(&stored), sizeof(stored));
    return input && input.peek() == std::char_traits<char>::eof() && stored == expected;
}

void StoreFingerprint(const std::filesystem::path& path, uint64_t fingerprint) {
    const std::filesystem::path temporary(path.wstring() + L".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&fingerprint), sizeof(fingerprint));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return;
        }
    }
    ::MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

Payload LoadPayload() {
    HRSRC resource = ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_TOR_PAYLOAD), RT_RCDATA);
    if (resource == nullptr) {
        return {};
    }
    HGLOBAL handle = ::LoadResource(nullptr, resource);
    if (handle == nullptr) {
        return {};
    }
    const auto* data = static_cast<const unsigned char*>(::LockResource(handle));
    const DWORD size = ::SizeofResource(nullptr, resource);
    if (data == nullptr || size == 0) {
        return {};
    }
    return {data, static_cast<size_t>(size)};
}

/**
 * @brief Locate the cabinet inside the payload by its self-describing header.
 *
 * A CFHEADER begins with "MSCF", a 4-byte reserved field that must be zero, and
 * a 4-byte total cabinet size. Requiring that size to equal the number of bytes
 * remaining in the resource makes an accidental "MSCF" inside the icon virtually
 * impossible to mistake for the real cabinet - and frees the icon in front from
 * any fixed-offset assumption.
 */
bool FindCabinet(const Payload& payload, size_t& offsetOut, uint32_t& sizeOut) {
    if (!payload.Valid() || payload.size < 16) {
        return false;
    }
    for (size_t i = 0; i + 16 <= payload.size; ++i) {
        const unsigned char* p = payload.data + i;
        if (p[0] != 'M' || p[1] != 'S' || p[2] != 'C' || p[3] != 'F') {
            continue;
        }
        if (ReadLe32(p + 4) != 0) {
            continue;   // reserved1 must be zero in a real cabinet header
        }
        const uint32_t cabinetSize = ReadLe32(p + 8);
        if (cabinetSize == payload.size - i) {
            offsetOut = i;
            sizeOut = cabinetSize;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- FDI glue --
//
// FDI's I/O callbacks are plain C function pointers with no user-data argument,
// so the cabinet being expanded is published through this single-threaded,
// one-shot context. Extraction is guarded by a mutex and runs only at startup.

struct ExtractContext {
    const unsigned char* cab{nullptr};
    size_t cabSize{0};
    std::wstring targetPath;   ///< where the single extracted file is written
    bool wroteTarget{false};
};

ExtractContext* g_context = nullptr;

/// A file handle for FDI: either the in-memory cabinet or a real output file.
struct FdiHandle {
    bool isCabinet{false};
    size_t position{0};                 ///< cabinet read cursor
    HANDLE file{INVALID_HANDLE_VALUE};  ///< output file
};

FNALLOC(fdi_alloc) { return ::malloc(cb); }
FNFREE(fdi_free) { ::free(pv); }

FNOPEN(fdi_open) {
    (void)pmode;
    if (g_context != nullptr && std::strcmp(pszFile, kCabSentinel) == 0) {
        auto* handle = new FdiHandle{};
        handle->isCabinet = true;
        handle->position = 0;
        return reinterpret_cast<INT_PTR>(handle);
    }

    // A real output file. FDI passes _O_* flags; we only ever create the target.
    const DWORD access = (oflag & _O_WRONLY || oflag & _O_RDWR) ? GENERIC_WRITE : GENERIC_READ;
    const DWORD disposition = (oflag & _O_CREAT) ? CREATE_ALWAYS : OPEN_EXISTING;

    const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, pszFile, -1, nullptr, 0);
    std::wstring widePath(static_cast<size_t>(wideLen > 0 ? wideLen - 1 : 0), L'\0');
    if (wideLen > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, pszFile, -1, widePath.data(), wideLen);
    }

    HANDLE file = ::CreateFileW(widePath.c_str(), access, FILE_SHARE_READ, nullptr,
                                disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    auto* handle = new FdiHandle{};
    handle->file = file;
    return reinterpret_cast<INT_PTR>(handle);
}

FNREAD(fdi_read) {
    auto* handle = reinterpret_cast<FdiHandle*>(hf);
    if (handle == nullptr) {
        return 0;
    }
    if (handle->isCabinet) {
        if (g_context == nullptr) {
            return 0;
        }
        const size_t remaining = (handle->position < g_context->cabSize)
            ? g_context->cabSize - handle->position : 0;
        const size_t take = (cb < remaining) ? cb : remaining;
        std::memcpy(pv, g_context->cab + handle->position, take);
        handle->position += take;
        return static_cast<UINT>(take);
    }
    DWORD read = 0;
    if (!::ReadFile(handle->file, pv, cb, &read, nullptr)) {
        return static_cast<UINT>(-1);
    }
    return read;
}

FNWRITE(fdi_write) {
    auto* handle = reinterpret_cast<FdiHandle*>(hf);
    if (handle == nullptr || handle->isCabinet) {
        return static_cast<UINT>(-1);   // the in-memory cabinet is read only
    }
    DWORD written = 0;
    if (!::WriteFile(handle->file, pv, cb, &written, nullptr)) {
        return static_cast<UINT>(-1);
    }
    return written;
}

FNCLOSE(fdi_close) {
    auto* handle = reinterpret_cast<FdiHandle*>(hf);
    if (handle == nullptr) {
        return 0;
    }
    if (!handle->isCabinet && handle->file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle->file);
    }
    delete handle;
    return 0;
}

FNSEEK(fdi_seek) {
    auto* handle = reinterpret_cast<FdiHandle*>(hf);
    if (handle == nullptr) {
        return -1;
    }
    if (handle->isCabinet) {
        const size_t size = (g_context != nullptr) ? g_context->cabSize : 0;
        long long base = 0;
        if (seektype == SEEK_SET)      base = 0;
        else if (seektype == SEEK_CUR) base = static_cast<long long>(handle->position);
        else                           base = static_cast<long long>(size);   // SEEK_END
        long long target = base + dist;
        if (target < 0) target = 0;
        if (target > static_cast<long long>(size)) target = static_cast<long long>(size);
        handle->position = static_cast<size_t>(target);
        return static_cast<long>(handle->position);
    }
    const DWORD method = (seektype == SEEK_SET) ? FILE_BEGIN
                        : (seektype == SEEK_CUR) ? FILE_CURRENT : FILE_END;
    LARGE_INTEGER distance{};
    distance.QuadPart = dist;
    LARGE_INTEGER result{};
    if (!::SetFilePointerEx(handle->file, distance, &result, method)) {
        return -1;
    }
    return static_cast<long>(result.QuadPart);
}

FNFDINOTIFY(fdi_notify) {
    switch (fdint) {
        case fdintCOPY_FILE: {
            // One file per cabinet; route it to the fixed target path regardless
            // of the name stored inside the cabinet.
            if (g_context == nullptr) {
                return -1;
            }
            int fakeOflag = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
            const int wideLen = static_cast<int>(g_context->targetPath.size());
            (void)wideLen;

            const int narrowLen = ::WideCharToMultiByte(
                CP_UTF8, 0, g_context->targetPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string narrow(static_cast<size_t>(narrowLen > 0 ? narrowLen - 1 : 0), '\0');
            if (narrowLen > 0) {
                ::WideCharToMultiByte(CP_UTF8, 0, g_context->targetPath.c_str(), -1,
                                      narrow.data(), narrowLen, nullptr, nullptr);
            }
            g_context->wroteTarget = true;
            return fdi_open(narrow.data(), fakeOflag, _S_IREAD | _S_IWRITE);
        }
        case fdintCLOSE_FILE_INFO:
            fdi_close(pfdin->hf);
            return TRUE;   // TRUE keeps FDI going after a successful file close
        default:
            return 0;
    }
}

std::mutex g_extractMutex;

} // namespace

std::filesystem::path TorExtractor::ExtractedPath() {
    return ExtractionDirectory() / kExtractedName;
}

bool TorExtractor::HasEmbeddedPayload() {
    const Payload payload = LoadPayload();
    size_t offset = 0;
    uint32_t size = 0;
    return FindCabinet(payload, offset, size);
}

std::filesystem::path TorExtractor::EnsureExtracted(std::string* errorOut) {
    std::lock_guard<std::mutex> lock(g_extractMutex);

    const std::filesystem::path target = ExtractedPath();

    const Payload payload = LoadPayload();
    size_t cabOffset = 0;
    uint32_t cabSize = 0;
    if (!FindCabinet(payload, cabOffset, cabSize)) {
        if (errorOut != nullptr) {
            *errorOut = "no embedded Tor cabinet found in the executable";
        }
        return {};
    }

    // Reuse only the executable produced from this exact embedded cabinet.
    // The tiny sidecar prevents an updated od.exe from silently retaining an
    // older cached Tor merely because both executables are larger than the CAB.
    std::error_code ec;
    const std::filesystem::path fingerprintPath = FingerprintPath(target);
    const uint64_t fingerprint =
        CabinetFingerprint(payload.data + cabOffset, cabSize);
    if (std::filesystem::exists(target, ec)) {
        const auto existing = std::filesystem::file_size(target, ec);
        if (!ec && existing > cabSize &&
            HasMatchingFingerprint(fingerprintPath, fingerprint)) {
            return target;
        }
        std::filesystem::remove(target, ec);
    }
    std::filesystem::remove(fingerprintPath, ec);

    std::filesystem::create_directories(target.parent_path(), ec);

    ExtractContext context;
    context.cab = payload.data + cabOffset;
    context.cabSize = cabSize;
    context.targetPath = target.wstring();

    ERF erf{};
    HFDI hfdi = ::FDICreate(fdi_alloc, fdi_free, fdi_open, fdi_read, fdi_write,
                            fdi_close, fdi_seek, cpuUNKNOWN, &erf);
    if (hfdi == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = "FDICreate failed";
        }
        return {};
    }

    g_context = &context;
    char cabName[] = "\x01od-embedded-cab";   // mutable copy for the C API
    char cabPath[] = "";
    const BOOL ok = ::FDICopy(hfdi, cabName, cabPath, 0, fdi_notify, nullptr, nullptr);
    g_context = nullptr;
    ::FDIDestroy(hfdi);

    if (!ok || !context.wroteTarget || !std::filesystem::exists(target, ec)) {
        if (errorOut != nullptr) {
            *errorOut = "failed to expand the Tor cabinet (error " +
                        std::to_string(erf.erfOper) + ")";
        }
        std::filesystem::remove(target, ec);
        return {};
    }

    StoreFingerprint(fingerprintPath, fingerprint);

    return target;
}

} // namespace od::core
