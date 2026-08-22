#pragma once

#include <windows.h>
#include <string_view>
#include <cstdint>

namespace od::cli {

/**
 * @brief Manages console attachment and terminal formatting in dual-mode (GUI/CLI) Windows applications.
 * 
 * DESIGN RATIONALE:
 * Windows GUI subsystem executables (`/SUBSYSTEM:WINDOWS`) do not automatically inherit a console
 * when launched from cmd.exe or PowerShell. ConsoleManager attaches to the parent console
 * descriptor and redirects CRT stdout/stderr streams, enabling clean CLI output without showing
 * a flashing black window when launched via desktop shortcuts or Explorer.
 */
class ConsoleManager {
public:
    /// Attach to parent console or allocate a new one if requested
    static bool InitializeConsole(bool forceAllocate = false);

    /// Detach console streams
    static void DetachConsole();

    /// Print text to console
    static void Write(std::wstring_view text);
    static void WriteLine(std::wstring_view text);

    /// Render a real-time progress bar to the CLI
    static void RenderProgressBar(double percent, uint64_t currentBytes, uint64_t totalBytes, uint64_t speedBytesPerSec, uint32_t connections);

    /// Terminate process cleanly and nudge parent shell to prompt
    [[noreturn]] static void ExitNow(int exitCode);
};

} // namespace od::cli
