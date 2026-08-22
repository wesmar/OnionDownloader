#include "../cli/CliParser.h"
#include "../cli/ConsoleApp.h"
#include "../cli/ConsoleManager.h"
#include "../ui/OdWindow.h"

#include <windows.h>

#include <shellapi.h>

/**
 * @brief Dual-head entry point: one binary, two faces.
 *
 * The executable is linked as /SUBSYSTEM:WINDOWS so Explorer starts it without
 * a console flashing up. When it is started from a shell with arguments it
 * attaches to the parent console instead and behaves like an ordinary command
 * line tool. Everything past argument parsing lives elsewhere - this file only
 * decides which face to show.
 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    const od::cli::CliOptions options = (argv != nullptr)
        ? od::cli::CliParser::Parse(argc, argv)
        : od::cli::CliOptions{};
    if (argv != nullptr) {
        ::LocalFree(argv);
    }

    if (options.invalidArguments) {
        od::cli::ConsoleManager::InitializeConsole();
        od::cli::ConsoleManager::WriteLine(L"\nError: " + options.argumentError);
        od::cli::ConsoleManager::Write(od::cli::CliParser::GetHelpText());
        od::cli::ConsoleManager::ExitNow(2);
    }

    if (options.showHelp) {
        od::cli::ConsoleManager::InitializeConsole();
        od::cli::ConsoleManager::Write(od::cli::CliParser::GetHelpText());
        od::cli::ConsoleManager::ExitNow(0);
    }

    if (options.showVersion) {
        od::cli::ConsoleManager::InitializeConsole();
        od::cli::ConsoleManager::Write(od::cli::CliParser::GetVersionText());
        od::cli::ConsoleManager::ExitNow(0);
    }

    if (options.HasDownloadTarget() && !options.guiMode) {
        od::cli::ConsoleManager::ExitNow(od::cli::ConsoleApp::Run(options));
    }

    od::ui::OdWindow window(options);
    if (!window.Create(hInstance, nCmdShow)) {
        ::MessageBoxW(nullptr, L"Failed to create the main window.",
                      L"Onion Downloader", MB_ICONERROR | MB_OK);
        return 1;
    }
    return window.RunMessageLoop();
}
