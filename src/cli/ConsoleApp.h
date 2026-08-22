#pragma once

#include "CliParser.h"

namespace od::cli {

/**
 * @brief The command line face of the application.
 *
 * Owns the whole non-interactive flow: start the Tor pool, hand the URL to the
 * engine, render the terminal progress bar until the transfer settles, and map
 * the outcome onto a process exit code.
 */
class ConsoleApp {
public:
    /// Exit codes the shell can branch on.
    enum : int {
        kSuccess = 0,
        kTorUnavailable = 1,
        kInvalidUrl = 2,
        kDownloadFailed = 3,
    };

    /// Run one download to completion. Returns a process exit code.
    [[nodiscard]] static int Run(const CliOptions& options);
};

} // namespace od::cli
