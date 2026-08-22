#pragma once

#include <filesystem>
#include <string>

namespace od::core {

/**
 * @brief Materialises the bundled tor.exe carried inside the executable.
 *
 * PACKAGING RATIONALE:
 * tor.exe is ~10 MB, which dominates a two-file distribution. Instead it is
 * compressed into an LZX cabinet at build time and concatenated onto the end of
 * the application icon (`app_orig.ico` + `tor.cab` -> `app.ico`), which is then
 * embedded as a raw RCDATA resource. The single shipped file is therefore the
 * ~3 MB od.exe alone.
 *
 * At run time the cabinet is located inside that resource by scanning for the
 * "MSCF" signature and validating the cabinet's own size field - so the icon in
 * front of it can be replaced freely without touching any offset - and expanded
 * with the in-box FDI decompressor (cabinet.dll). The cabinet is fed to FDI
 * straight from the resource memory; only the finished tor.exe is written, into
 * %LOCALAPPDATA%\od\tor\bin.
 *
 * Nothing here is obfuscation: Tor is a well-known, unmodified binary. This is
 * purely a single-file convenience, and the extracted path is a plain file.
 */
class TorExtractor {
public:
    /// Absolute path where the unpacked tor.exe lives (whether or not it exists yet).
    [[nodiscard]] static std::filesystem::path ExtractedPath();

    /// True when the embedded RCDATA payload actually carries a cabinet.
    [[nodiscard]] static bool HasEmbeddedPayload();

    /**
     * @brief Ensure tor.exe exists on disk, extracting it if necessary.
     * @param errorOut  optional human-readable failure reason.
     * @return the path to a ready-to-run tor.exe, or an empty path on failure.
     *
     * Idempotent: if a correctly sized tor.exe is already present it is reused,
     * so the cabinet is expanded at most once per machine.
     */
    [[nodiscard]] static std::filesystem::path EnsureExtracted(std::string* errorOut);
};

} // namespace od::core
