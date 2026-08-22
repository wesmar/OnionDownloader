#pragma once

#include <cstdint>
#include <string>

namespace od::cli {

/// Parsed command line. Defaults match the GUI so both heads behave identically.
struct CliOptions {
    bool guiMode{false};
    bool showHelp{false};
    bool showVersion{false};
    bool invalidArguments{false};
    std::wstring argumentError;

    std::wstring url;
    // Empty means the current user's Windows Downloads known folder.
    std::wstring outputDir;

    /// Parallel HTTP workers; each one owns a private Tor circuit.
    uint32_t workers{10};

    /// Independent Tor daemons to run. 0 selects the automatic sizing below.
    uint32_t torInstances{0};

    /**
     * Automatic pool sizing: half as many daemons as workers, bounded to
     * [2, 8]. Beyond roughly eight the bottleneck moves from the entry guards
     * to the onion service itself, so extra daemons only cost memory and
     * bootstrap time.
     */
    [[nodiscard]] uint32_t EffectiveTorInstances() const noexcept {
        if (torInstances > 0) {
            return torInstances;
        }
        const uint32_t half = workers / 2;
        if (half < 2) return 2;
        if (half > 8) return 8;
        return half;
    }

    [[nodiscard]] bool HasDownloadTarget() const noexcept { return !url.empty(); }
};

class CliParser {
public:
    static CliOptions Parse(int argc, wchar_t** argv);
    static std::wstring GetHelpText();
    static std::wstring GetVersionText();
};

} // namespace od::cli
